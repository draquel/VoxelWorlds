// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelSceneProxy.h"
#include "VoxelRegionComponent.h"
#include "VoxelRendering.h"
#include "VoxelLocalVertexFactory.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"  // For FColoredMaterialRenderProxy
#include "Engine/Engine.h"
#include "SceneManagement.h"
#include "PrimitiveSceneInfo.h"
#include "MeshBatch.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalRenderResources.h"
#include "RenderUtils.h"
#include "UObject/UObjectIterator.h"
#include "VoxelRayTracingCounters.h"
#if RHI_RAYTRACING
#include "RayTracingInstance.h"
#endif

#include <atomic>

namespace
{
	/** Recreate every region component's scene proxy (game thread): ray tracing is fixed per proxy, and a new proxy gets its
	 *  geometry back from its owner (UVoxelChunkManager::RecoverLostRenderData, G15). */
	void RecreateVoxelRegionProxies(IConsoleVariable* Var)
	{
		static int32 Applied = 1;
		const int32 Value = Var ? Var->GetInt() : Applied;
		if (Value == Applied)
		{
			return;
		}
		Applied = Value;
		int32 Recreated = 0;
		for (TObjectIterator<UVoxelRegionComponent> It; It; ++It)
		{
			if (It->IsRegistered() && It->IsRenderStateCreated())
			{
				It->MarkRenderStateDirty();
				++Recreated;
			}
		}
		UE_LOG(LogVoxelRendering, Log, TEXT("voxel.Render.RayTracing %d: recreated %d region proxies"), Value, Recreated);
	}

	TAutoConsoleVariable<int32> CVarVoxelRayTracing(
		TEXT("voxel.Render.RayTracing"),
		1,
		TEXT("Ray tracing geometry for the terrain's chunk and seam meshes (visual polish F3.3), so hardware Lumen and ray traced ")
		TEXT("shadows and reflections see the terrain off screen. Needs the project's ray tracing support. A change recreates the ")
		TEXT("terrain's region proxies (their chunks re-mesh). 0 = off: Lumen sees the terrain in screen space only."),
		FConsoleVariableDelegate::CreateStatic(&RecreateVoxelRegionProxies),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarVoxelRayTracingTraceDistanceScale(
		TEXT("voxel.Render.RayTracing.TraceDistanceScale"),
		1.0f,
		TEXT("The terrain's ray tracing instances (voxel.Render.RayTracing) are gathered each frame for the chunk and seam meshes ")
		TEXT("within this multiple of the view's Lumen max trace distance (the post process setting, 20,000 uu by default), measured ")
		TEXT("from the camera to the mesh's box: rays from the pixels near the camera end before the meshes left out, which cost ")
		TEXT("the gather every frame. 0 = every mesh."),
		ECVF_RenderThreadSafe);

	std::atomic<int64> GVoxelRTGeometriesLive{ 0 };
	std::atomic<int64> GVoxelRTBytesLive{ 0 };
	std::atomic<int64> GVoxelRTBuilds{ 0 };
	std::atomic<int64> GVoxelRTReleases{ 0 };
	std::atomic<int64> GVoxelRTInstancesLastFrame{ 0 };
	std::atomic<uint32> GVoxelRTInstancesFrame{ 0 };
	std::atomic<int64> GVoxelRTSkippedPending{ 0 };
	std::atomic<int64> GVoxelRTProxiesLive{ 0 };

	FAutoConsoleCommand CmdVoxelRayTracingStats(
		TEXT("voxel.Render.RayTracingStats"),
		TEXT("The terrain's ray tracing geometry (visual polish F3.3), over every voxel scene proxy: proxies with it, geometries ")
		TEXT("alive and their BLAS megabytes, the instances the last frame's gather emitted, and since the start or the last reset ")
		TEXT("the geometries built and released and the gathers that left a geometry out while its BLAS was building. 'reset' clears ")
		TEXT("the cumulative counts."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const FVoxelRayTracingCounters::FCounts C = FVoxelRayTracingCounters::Get();
			UE_LOG(LogVoxelRendering, Display,
				TEXT("voxel.Render.RayTracing %d (ray tracing %s): %lld proxies, %lld geometries live, %.1f MB BLAS, %lld instances last frame; %lld built, %lld released, %lld left out while building"),
				CVarVoxelRayTracing.GetValueOnGameThread(), IsRayTracingAllowed() ? TEXT("allowed") : TEXT("not allowed"),
				C.ProxiesLive, C.GeometriesLive, C.BytesLive / (1024.0 * 1024.0), C.ProxiesLive > 0 ? C.InstancesLastFrame : 0, C.Builds, C.Releases, C.SkippedPending);
			if (Args.Num() > 0 && Args[0].Equals(TEXT("reset"), ESearchCase::IgnoreCase))
			{
				FVoxelRayTracingCounters::Reset();
			}
		}));

	TAutoConsoleVariable<int32> CVarVoxelFarShadowMinLOD(
		TEXT("voxel.Render.FarShadowMinLOD"),
		1,
		TEXT("The finest chunk LOD drawn into the sun's far shadow cascades (visual polish D-VP3, F3.2): chunks and seams meshed ")
		TEXT("finer are left out, since the far cascades' receivers lie past the near cascades, in the coarse bands. ")
		TEXT("0 = every chunk; a large value = none (the far cascades without the terrain)."),
		ECVF_RenderThreadSafe);

	/**
	 * How GetDynamicMeshElements culls one view's batches (visual polish VP3-O1). A shadow depth pass calls with a snapshot
	 * of the camera's view, whose ViewFrustum is still the camera's, and passes the shadow's caster volume as
	 * GetDynamicMeshElementsShadowCullFrustum, in the space translated by GetPreShadowTranslation (zero for the sun's
	 * whole-scene cascades). Culling with the camera's frustum there dropped every chunk out of view from the shadow
	 * map, so a hill behind or beside the camera cast no shadow until it was turned into view.
	 */
	struct FVoxelViewCull
	{
		const FConvexVolume* Frustum = nullptr;
		FVector Translation = FVector::ZeroVector;
		/** A shadow gather culled by its caster volume: batches that cast no shadow (RVT clones, water) are left out. */
		bool bShadowCasters = false;
		/**
		 * A gather for one of the sun's far cascades (EDrawDynamicFlags::FarShadowCascade, D-VP3): the coarse terrain only
		 * (chunks and seams of LOD MinLOD and coarser), each batch once with the opaque material (no crossfade halves).
		 */
		bool bFarShadowCascade = false;
		int32 MinLOD = 0;

		explicit FVoxelViewCull(const FSceneView& View)
			: Frustum(&View.ViewFrustum)
		{
			const FConvexVolume* ShadowFrustum = View.GetDynamicMeshElementsShadowCullFrustum();
			if (ShadowFrustum)
			{
				Frustum = ShadowFrustum;
				Translation = View.GetPreShadowTranslation();
				bShadowCasters = true;
				bFarShadowCascade = (View.DrawDynamicFlags & EDrawDynamicFlags::FarShadowCascade) != 0;
				MinLOD = bFarShadowCascade ? CVarVoxelFarShadowMinLOD.GetValueOnRenderThread() : 0;
			}
		}

		/** @return Whether the world-space box can be seen by the view (or cast into the shadow) */
		bool Intersects(const FBox& WorldBox) const
		{
			return Frustum->IntersectBox(WorldBox.GetCenter() + Translation, WorldBox.GetExtent());
		}

		/** @return Whether a mesh at this LOD is too fine for the view (a far cascade's detail) */
		bool IsTooFine(int32 LODLevel) const
		{
			return bFarShadowCascade && LODLevel < MinLOD;
		}
	};

	/**
	 * A chunk with this fade state and retained Previous mesh crossfades: both meshes draw, under complementary dither
	 * masks. Any other combination draws the current mesh alone (an appearance fades it in from nothing).
	 */
	bool IsChunkFadeDrawable(const FVoxelChunkFadeState* FadeState, const FVoxelChunkPreviousMesh* PreviousMesh)
	{
		return FadeState && PreviousMesh
			&& FadeState->FadeInProxy && FadeState->FadeOutProxy
			&& PreviousMesh->RenderData.HasValidBuffers()
			&& PreviousMesh->RenderData.IndexCount >= 3
			&& PreviousMesh->VertexFactory.IsValid()
			&& PreviousMesh->IndexBuffer.IsValid();
	}
}

namespace VoxelFadeDebug
{
	/** voxel.MeshFade.Stats' focus chunk: GetDynamicMeshElements records what the main view drew of it. */
	std::atomic<bool> bFocusActive{ false };
	FCriticalSection FocusLock;
	FIntVector FocusChunk = FIntVector::ZeroValue;
	FString FocusDrawn;
}

// ==================== Helper Function Implementation ====================

void InitVoxelLocalVertexFactory(
	FRHICommandListBase& RHICmdList,
	FLocalVertexFactory* VertexFactory,
	const FVertexBuffer* VertexBuffer,
	FRHIShaderResourceView* ColorSRV,
	FRHIShaderResourceView* TangentsSRV,
	FRHIShaderResourceView* TexCoordSRV)
{
	check(IsInRenderingThread());
	check(VertexFactory != nullptr);
	check(VertexBuffer != nullptr);

	const uint32 Stride = sizeof(FVoxelLocalVertex);

	// Build FDataType for FLocalVertexFactory
	FLocalVertexFactory::FDataType NewData;

	// Position stream component - reads from interleaved buffer via vertex input
	NewData.PositionComponent = FVertexStreamComponent(
		VertexBuffer,
		STRUCT_OFFSET(FVoxelLocalVertex, Position),
		Stride,
		VET_Float3
	);

	// Use global null buffer SRVs to satisfy uniform buffer requirements
	// The actual vertex data comes from the vertex stream components
	NewData.PositionComponentSRV = GNullColorVertexBuffer.VertexBufferSRV;

	// Tangent basis stream components - for traditional vertex input
	NewData.TangentBasisComponents[0] = FVertexStreamComponent(
		VertexBuffer,
		STRUCT_OFFSET(FVoxelLocalVertex, TangentX),
		Stride,
		VET_PackedNormal
	);
	NewData.TangentBasisComponents[1] = FVertexStreamComponent(
		VertexBuffer,
		STRUCT_OFFSET(FVoxelLocalVertex, TangentZ),
		Stride,
		VET_PackedNormal
	);

	// Tangents SRV - for manual vertex fetch (GPUScene path)
	NewData.TangentsSRV = TangentsSRV ? TangentsSRV : GNullColorVertexBuffer.VertexBufferSRV.GetReference();

	// Texture coordinates - for traditional vertex input
	// UV0: Face UVs for texture tiling within atlas tiles
	// UV1: MaterialID (X) and FaceType (Y) as floats to avoid sRGB issues
	NewData.TextureCoordinates.Empty();
	NewData.TextureCoordinates.Add(FVertexStreamComponent(
		VertexBuffer,
		STRUCT_OFFSET(FVoxelLocalVertex, TexCoord),
		Stride,
		VET_Float2
	));
	NewData.TextureCoordinates.Add(FVertexStreamComponent(
		VertexBuffer,
		STRUCT_OFFSET(FVoxelLocalVertex, TexCoord1),
		Stride,
		VET_Float2
	));
	// TexCoord SRV - for manual vertex fetch (GPUScene path)
	NewData.TextureCoordinatesSRV = TexCoordSRV ? TexCoordSRV : GNullColorVertexBuffer.VertexBufferSRV.GetReference();

	// Vertex color - for traditional vertex input
	// Note: VET_Color applies sRGB conversion. Use gamma correction in shader to recover linear values.
	// MaterialID and FaceType are now in UV1 to avoid sRGB issues.
	NewData.ColorComponent = FVertexStreamComponent(
		VertexBuffer,
		STRUCT_OFFSET(FVoxelLocalVertex, Color),
		Stride,
		VET_Color
	);
	// Color SRV - for manual vertex fetch (GPUScene path)
	NewData.ColorComponentsSRV = ColorSRV ? ColorSRV : GNullColorVertexBuffer.VertexBufferSRV.GetReference();

	// Light map coordinate index
	NewData.LightMapCoordinateIndex = 0;
	NewData.NumTexCoords = 2;

	// Set the data on the vertex factory
	VertexFactory->SetData(RHICmdList, NewData);
}

// ==================== FVoxelSceneProxy ====================

FVoxelSceneProxy::FVoxelSceneProxy(UVoxelRegionComponent* InComponent, UMaterialInterface* InMaterial)
	: FPrimitiveSceneProxy(InComponent)
	, Material(InMaterial)
	, FeatureLevel(InComponent->GetWorld()->GetFeatureLevel())
	, VoxelSize(100.0f)
{
	// Get voxel size from component if available
	if (InComponent)
	{
		VoxelSize = InComponent->GetVoxelSize();
		RenderOrigin = InComponent->GetComponentLocation();
		bRadialFaceUp = InComponent->IsRadialFaceUp();
		FaceUpCenter = ToRenderOffset(InComponent->GetFaceUpCenter());
	}

	// Get material (use default if none set)
	if (Material == nullptr)
	{
		Material = UMaterial::GetDefaultMaterial(MD_Surface);
	}

	// Cache material relevance
	MaterialRelevance = Material->GetRelevance(GetFeatureLevelShaderPlatform(FeatureLevel));

	// Note: Per-chunk vertex factories are created in ProcessBatchUpdate_RenderThread

	// Set proxy properties
	bVerifyUsedMaterials = false;
	bCastDynamicShadow = true;
	bCastStaticShadow = false;
	bAffectDynamicIndirectLighting = false;
	bAffectDistanceFieldLighting = false;

	// Allow terrain chunks to write into Runtime Virtual Textures when the owning
	// component has RVTs assigned. The base FPrimitiveSceneProxy constructor reads
	// UPrimitiveComponent::RuntimeVirtualTextures and populates
	// RuntimeVirtualTextureMaterialTypes; we just opt in here and emit the RVT mesh
	// batches in GetDynamicMeshElements. See Documentation/Research/Archive/ENGINE_INTEGRATION.md.
	bSupportsRuntimeVirtualTexture = true;

#if RHI_RAYTRACING
	// Ray tracing geometry per chunk and seam mesh (visual polish F3.3), fixed for the proxy's life
	bRayTracing = IsRayTracingAllowed() && CVarVoxelRayTracing.GetValueOnAnyThread() != 0;
	if (bRayTracing)
	{
		FVoxelRayTracingCounters::AddProxy(1);
	}
#endif

	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelSceneProxy: Created with FLocalVertexFactory (RVT material types=%d, ray tracing %s)"),
		RuntimeVirtualTextureMaterialTypes.Num(), bRayTracing ? TEXT("on") : TEXT("off"));
}

FVoxelSceneProxy::~FVoxelSceneProxy()
{
	// Release all chunk resources
	FScopeLock Lock(&ChunkDataLock);

#if RHI_RAYTRACING
	ReleaseAllChunkRayTracing_AssumesLocked();
	ReleaseAllSeamRayTracing_AssumesLocked();
	if (bRayTracing)
	{
		FVoxelRayTracingCounters::AddProxy(-1);
	}
#endif

	ReleaseAllChunkFadeStates_AssumesLocked();

	for (auto& Pair : ChunkRenderData)
	{
		Pair.Value.ReleaseResources();
	}
	ChunkRenderData.Empty();

	for (auto& Pair : ChunkVertexBuffers)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->ReleaseResource();
		}
	}
	ChunkVertexBuffers.Empty();

	for (auto& Pair : ChunkIndexBuffers)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->ReleaseResource();
		}
	}
	ChunkIndexBuffers.Empty();

	for (auto& Pair : ChunkVertexFactories)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->ReleaseResource();
		}
	}
	ChunkVertexFactories.Empty();

	// Release all water tile resources
	for (auto& Pair : WaterTileRenderData)
	{
		Pair.Value.ReleaseResources();
	}
	WaterTileRenderData.Empty();

	for (auto& Pair : WaterTileVertexBuffers)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->ReleaseResource();
		}
	}
	WaterTileVertexBuffers.Empty();

	for (auto& Pair : WaterTileIndexBuffers)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->ReleaseResource();
		}
	}
	WaterTileIndexBuffers.Empty();

	for (auto& Pair : WaterTileVertexFactories)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->ReleaseResource();
		}
	}
	WaterTileVertexFactories.Empty();

	// Release all seam mesh resources, including buckets retained for fades
	ReleaseAllSeamFadeStates_AssumesLocked();
	for (auto& Pair : SeamRenderData)
	{
		Pair.Value.ReleaseResources();
	}
	SeamRenderData.Empty();

	for (auto& Pair : SeamVertexBuffers)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->ReleaseResource();
		}
	}
	SeamVertexBuffers.Empty();

	for (auto& Pair : SeamIndexBuffers)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->ReleaseResource();
		}
	}
	SeamIndexBuffers.Empty();

	for (auto& Pair : SeamVertexFactories)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->ReleaseResource();
		}
	}
	SeamVertexFactories.Empty();
}

void FVoxelSceneProxy::CreateMeshBuffers_RenderThread(FRHICommandListBase& RHICmdList, const FVoxelMeshBufferNames& Names,
	const TArray<FVoxelLocalVertex>& ConvertedVertices, const TArray<FColor>& ColorData, const TArray<FVoxelPackedTangentPair>& TangentData,
	const TArray<FVector4f>& TexCoordData, const TArray<uint32>& Indices, FVoxelChunkRenderData& NewRenderData)
{
	const uint32 VertexCount = ConvertedVertices.Num();
	const uint32 IndexCount = Indices.Num();

	// Create vertex buffer
	const uint32 ConvertedVertexSize = VertexCount * sizeof(FVoxelLocalVertex);
	NewRenderData.VertexBufferRHI = RHICmdList.CreateBuffer(
		FRHIBufferCreateDesc::Create(Names.Vertex, ConvertedVertexSize, sizeof(FVoxelLocalVertex), BUF_Static | BUF_VertexBuffer)
			.SetInitialState(ERHIAccess::VertexOrIndexBuffer));

	void* VertexData = RHICmdList.LockBuffer(NewRenderData.VertexBufferRHI, 0, ConvertedVertexSize, RLM_WriteOnly);
	FMemory::Memcpy(VertexData, ConvertedVertices.GetData(), ConvertedVertexSize);
	RHICmdList.UnlockBuffer(NewRenderData.VertexBufferRHI);

	// Create color buffer for SRV
	const uint32 ColorBufferSize = VertexCount * sizeof(FColor);
	NewRenderData.ColorBufferRHI = RHICmdList.CreateBuffer(
		FRHIBufferCreateDesc::Create(Names.Color, ColorBufferSize, sizeof(FColor), BUF_Static | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::SRVMask));

	void* ColorBufferData = RHICmdList.LockBuffer(NewRenderData.ColorBufferRHI, 0, ColorBufferSize, RLM_WriteOnly);
	FMemory::Memcpy(ColorBufferData, ColorData.GetData(), ColorBufferSize);
	RHICmdList.UnlockBuffer(NewRenderData.ColorBufferRHI);

	NewRenderData.ColorSRV = RHICmdList.CreateShaderResourceView(NewRenderData.ColorBufferRHI, FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_B8G8R8A8));

	// Create tangent buffer for SRV
	const uint32 TangentBufferSize = VertexCount * sizeof(FVoxelPackedTangentPair);
	NewRenderData.TangentBufferRHI = RHICmdList.CreateBuffer(
		FRHIBufferCreateDesc::Create(Names.Tangent, TangentBufferSize, sizeof(FVoxelPackedTangentPair), BUF_Static | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::SRVMask));

	void* TangentBufferData = RHICmdList.LockBuffer(NewRenderData.TangentBufferRHI, 0, TangentBufferSize, RLM_WriteOnly);
	FMemory::Memcpy(TangentBufferData, TangentData.GetData(), TangentBufferSize);
	RHICmdList.UnlockBuffer(NewRenderData.TangentBufferRHI);

	NewRenderData.TangentsSRV = RHICmdList.CreateShaderResourceView(NewRenderData.TangentBufferRHI, FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_R8G8B8A8_SNORM));

	// Create TexCoord buffer for SRV
	const uint32 TexCoordBufferSize = VertexCount * sizeof(FVector4f);
	NewRenderData.TexCoordBufferRHI = RHICmdList.CreateBuffer(
		FRHIBufferCreateDesc::Create(Names.TexCoord, TexCoordBufferSize, sizeof(FVector4f), BUF_Static | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::SRVMask));

	void* TexCoordBufferData = RHICmdList.LockBuffer(NewRenderData.TexCoordBufferRHI, 0, TexCoordBufferSize, RLM_WriteOnly);
	FMemory::Memcpy(TexCoordBufferData, TexCoordData.GetData(), TexCoordBufferSize);
	RHICmdList.UnlockBuffer(NewRenderData.TexCoordBufferRHI);

	NewRenderData.TexCoordSRV = RHICmdList.CreateShaderResourceView(NewRenderData.TexCoordBufferRHI, FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_G32R32F));

	// Create index buffer
	const uint32 IndexBufferSize = IndexCount * sizeof(uint32);
	NewRenderData.IndexBufferRHI = RHICmdList.CreateBuffer(
		FRHIBufferCreateDesc::Create(Names.Index, IndexBufferSize, sizeof(uint32), BUF_Static | BUF_IndexBuffer)
			.SetInitialState(ERHIAccess::VertexOrIndexBuffer));

	void* IndexData = RHICmdList.LockBuffer(NewRenderData.IndexBufferRHI, 0, IndexBufferSize, RLM_WriteOnly);
	FMemory::Memcpy(IndexData, Indices.GetData(), IndexBufferSize);
	RHICmdList.UnlockBuffer(NewRenderData.IndexBufferRHI);
}

SIZE_T FVoxelSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

void FVoxelSceneProxy::CreateRenderThreadResources(FRHICommandListBase& RHICmdList)
{
	// Per-chunk vertex factories are created in ProcessBatchUpdate_RenderThread
}

void FVoxelSceneProxy::GetDynamicMeshElements(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector) const
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_VoxelSceneProxy_GetDynamicMeshElements);

	if (!Material)
	{
		return;
	}

	FScopeLock Lock(&ChunkDataLock);

	// Nothing at all to draw. A proxy with water tiles and no chunk mesh draws them (world modes review G77): since
	// render regions (MP2) a water tile goes to the region of its column's Z-0 chunk, which over a lake is air with no
	// mesh, and a region component holding only water tiles returned here, so a lake under such a region drew its bed
	// and no water (a block of chunk columns at the region's corner, seen from altitude).
	if (ChunkRenderData.Num() == 0 && WaterTileRenderData.Num() == 0)
	{
		return;
	}

	// Get material render proxy
	const bool bWireframe = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;
	FMaterialRenderProxy* MaterialProxy = nullptr;

	if (bWireframe)
	{
		FColoredMaterialRenderProxy* WireframeMaterialInstance = new FColoredMaterialRenderProxy(
			GEngine->WireframeMaterial ? GEngine->WireframeMaterial->GetRenderProxy() : nullptr,
			FLinearColor(0, 0.5f, 1.f)
		);
		Collector.RegisterOneFrameMaterialProxy(WireframeMaterialInstance);
		MaterialProxy = WireframeMaterialInstance;
	}
	else
	{
		MaterialProxy = Material->GetRenderProxy();
	}

	if (!MaterialProxy)
	{
		return;
	}

	int32 TotalMeshesAdded = 0;
	int32 SkippedInvisible = 0;
	int32 SkippedFrustum = 0;
	int32 SkippedOverLimit = 0;
	int32 SkippedTooFine = 0;

	// Safety limit for mesh batches - should rarely be hit with proper frustum culling
	// The Non-Nanite job queue overflow was caused by Virtual Shadow Maps, not mesh count.
	// Raised 500 -> 2000: the original value was tuned for a chunks-only world; with RVT
	// clones (x2 per chunk) plus the seam-ownership seam buckets (one per owner) drawn AFTER
	// chunks, 500 was routinely exhausted by near geometry and far seams never rendered.
	constexpr int32 MaxMeshBatchesPerFrame = 2000;

	// Crossfade lookups only happen while at least one chunk is actually transitioning (a swap with a
	// retained Previous mesh, or an appearance, which has none)
	const bool bAnyFades = ChunkFadeStates.Num() > 0;

	// A shadow gather passes one view carrying the shadow's caster volume (FVoxelViewCull)
	const bool bShadowGather = Views.Num() > 0 && Views[0]->GetDynamicMeshElementsShadowCullFrustum() != nullptr;
	const bool bFarShadowGather = bShadowGather && (Views[0]->DrawDynamicFlags & EDrawDynamicFlags::FarShadowCascade) != 0;
	if (!bShadowGather)
	{
		LastMainViewFrame = GFrameNumberRenderThread;
		++MainViewCalls;
	}
	else
	{
		++ShadowGatherCalls;
	}

	// Process each view
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		if (!(VisibilityMap & (1 << ViewIndex)))
		{
			continue;
		}

		const FVoxelViewCull Cull(*Views[ViewIndex]);

		// Iterate over all chunks
		for (const auto& Pair : ChunkRenderData)
		{
			// Check mesh batch limit to avoid job queue overflow
			if (TotalMeshesAdded >= MaxMeshBatchesPerFrame)
			{
				SkippedOverLimit++;
				continue;
			}

			const FIntVector& ChunkCoord = Pair.Key;
			const FVoxelChunkRenderData& RenderData = Pair.Value;

			// Skip empty chunks (IndexCount < 3 means zero triangles)
			if (!RenderData.HasValidBuffers() || RenderData.IndexCount < 3)
			{
				SkippedInvisible++;
				continue;
			}

			// A far cascade takes the coarse chunks only (voxel.Render.FarShadowMinLOD)
			if (Cull.IsTooFine(RenderData.LODLevel))
			{
				SkippedTooFine++;
				continue;
			}

			// Get vertex factory for this chunk
			const TSharedPtr<FLocalVertexFactory>* VertexFactoryPtr = ChunkVertexFactories.Find(ChunkCoord);
			if (!VertexFactoryPtr || !VertexFactoryPtr->IsValid())
			{
				SkippedInvisible++;
				continue;
			}

			// Get index buffer wrapper
			const TSharedPtr<FVoxelLocalIndexBuffer>* IndexBufferPtr = ChunkIndexBuffers.Find(ChunkCoord);
			if (!IndexBufferPtr || !IndexBufferPtr->IsValid())
			{
				SkippedInvisible++;
				continue;
			}

			// Frustum culling - use proper box-frustum intersection test
			// The old corner-only test failed for nearby chunks when looking down
			{
				FBox WorldBounds = RenderData.WorldBounds;

				if (WorldBounds.IsValid)
				{
					// Expand bounds for safety margin (accounts for vertex displacement, LOD morphing)
					WorldBounds = WorldBounds.ExpandBy(FVector(VoxelSize * 2.0f));

					// UE's box-frustum test (conservative: corners outside but surface visible, camera inside
					// the bounds, bounds spanning planes), against the camera's frustum or, in a shadow pass,
					// the shadow's caster volume
					if (!Cull.Intersects(WorldBounds))
					{
						SkippedFrustum++;
						continue;
					}
				}
			}

			// ==================== Mesh-Swap Crossfade ====================
			// While a swap fade is active this chunk draws twice: the retained previous mesh
			// dithering out and the current mesh dithering in. Both use pooled MIDs of the
			// masked fade material whose FadeAlpha is the same t with complementary masks
			// (FadeInvert), so every pixel is covered by exactly one of the two surfaces.
			// Fading requires BOTH the previous mesh set and the attached fade state — any
			// other combination renders the current mesh with the shared material as before.
			// The exception is an appearance (R24 P3): no previous mesh, the current one alone
			// dithers in from nothing.
			// See Documentation/Research/Archive/SEAM_OWNERSHIP_ARCHITECTURE.md §5.
			// A far cascade draws the current mesh alone with the opaque material: its texels are metres across, where a
			// dither mask is noise and the outgoing half one more batch.
			const FVoxelChunkFadeState* FadeState = nullptr;
			const FVoxelChunkPreviousMesh* PreviousMesh = nullptr;
			if (bAnyFades && !bWireframe && !Cull.bFarShadowCascade)
			{
				FadeState = ChunkFadeStates.Find(ChunkCoord);
				if (FadeState)
				{
					PreviousMesh = ChunkPreviousMeshes.Find(ChunkCoord);
				}
			}
			const bool bFading = IsChunkFadeDrawable(FadeState, PreviousMesh);
			const bool bAppearing = !bFading && FadeState && FadeState->bAppear && FadeState->FadeInProxy;

			// voxel.MeshFade.Stats with a focus chunk: what the main view draws of it (G74)
			if (VoxelFadeDebug::bFocusActive.load(std::memory_order_relaxed) && !Cull.bShadowCasters)
			{
				FScopeLock FocusScope(&VoxelFadeDebug::FocusLock);
				if (ChunkCoord == VoxelFadeDebug::FocusChunk)
				{
					VoxelFadeDebug::FocusDrawn = FString::Printf(TEXT("frame %u view %d: current factory %p, %u tris, %s%s"),
						ViewFamily.FrameNumber, ViewIndex, VertexFactoryPtr->Get(), RenderData.IndexCount / 3,
						(bFading || bAppearing) ? TEXT("fade-in material") : TEXT("terrain material"),
						bFading ? *FString::Printf(TEXT("; previous factory %p, %u tris, fade-out material"), PreviousMesh->VertexFactory.Get(),
							PreviousMesh->RenderData.IndexCount / 3) : TEXT(""));
				}
			}

			if (bFading)
			{
				// Previous mesh — dithers out via the FadeInvert=1 MID. Culled with the current
				// mesh's bounds above (the two differ by at most one LOD's worth of surface
				// motion, inside the ExpandBy margin).
				FMeshBatch& PrevBatch = Collector.AllocateMesh();
				PrevBatch.VertexFactory = PreviousMesh->VertexFactory.Get();
				PrevBatch.MaterialRenderProxy = FadeState->FadeOutProxy;
				PrevBatch.ReverseCulling = IsLocalToWorldDeterminantNegative();
				PrevBatch.bDisableBackfaceCulling = false;
				PrevBatch.Type = PT_TriangleList;
				PrevBatch.DepthPriorityGroup = SDPG_World;
				PrevBatch.bCanApplyViewModeOverrides = true;
				PrevBatch.bUseWireframeSelectionColoring = IsSelected();
				PrevBatch.bUseAsOccluder = true;
				PrevBatch.bWireframe = false;
				PrevBatch.CastShadow = true;
				PrevBatch.bUseForMaterial = true;
				PrevBatch.bUseForDepthPass = true;
				PrevBatch.LODIndex = PreviousMesh->RenderData.LODLevel;
				PrevBatch.SegmentIndex = 0;

				FMeshBatchElement& PrevElement = PrevBatch.Elements[0];
				PrevElement.IndexBuffer = PreviousMesh->IndexBuffer.Get();
				PrevElement.FirstIndex = 0;
				PrevElement.NumPrimitives = PreviousMesh->RenderData.IndexCount / 3;
				PrevElement.MinVertexIndex = 0;
				PrevElement.MaxVertexIndex = PreviousMesh->RenderData.VertexCount - 1;
				PrevElement.PrimitiveUniformBuffer = GetUniformBuffer();

				Collector.AddMesh(ViewIndex, PrevBatch);
				TotalMeshesAdded++;
			}

			// Allocate mesh batch
			FMeshBatch& MeshBatch = Collector.AllocateMesh();
			MeshBatch.VertexFactory = VertexFactoryPtr->Get();
			MeshBatch.MaterialRenderProxy = (bFading || bAppearing) ? FadeState->FadeInProxy : MaterialProxy;
			MeshBatch.ReverseCulling = IsLocalToWorldDeterminantNegative();
			MeshBatch.bDisableBackfaceCulling = false;
			MeshBatch.Type = PT_TriangleList;
			MeshBatch.DepthPriorityGroup = SDPG_World;
			MeshBatch.bCanApplyViewModeOverrides = true;
			MeshBatch.bUseWireframeSelectionColoring = IsSelected();
			MeshBatch.bUseAsOccluder = true;
			MeshBatch.bWireframe = bWireframe;
			MeshBatch.CastShadow = true;
			MeshBatch.bUseForMaterial = true;
			MeshBatch.bUseForDepthPass = true;
			MeshBatch.LODIndex = RenderData.LODLevel;
			MeshBatch.SegmentIndex = 0;

			// Setup mesh batch element
			FMeshBatchElement& BatchElement = MeshBatch.Elements[0];
			BatchElement.IndexBuffer = IndexBufferPtr->Get();
			BatchElement.FirstIndex = 0;
			BatchElement.NumPrimitives = RenderData.IndexCount / 3;
			BatchElement.MinVertexIndex = 0;
			BatchElement.MaxVertexIndex = RenderData.VertexCount - 1;
			BatchElement.PrimitiveUniformBuffer = GetUniformBuffer();

			// ==================== Runtime Virtual Texture Pass ====================
			// For each RVT material type this primitive writes to, emit a clone of the
			// terrain batch flagged for the virtual texture pass. RuntimeVirtualTextureMaterialTypes
			// is populated by the base FPrimitiveSceneProxy from the component's assigned RVTs,
			// so this loop is a no-op when no RVT is assigned (zero overhead in that case).
			// Pattern mirrors FBaseDynamicMeshSceneProxy. Only terrain writes to RVT; water does not.
			// A shadow pass draws none of them.
			if (!Cull.bShadowCasters)
			{
				for (ERuntimeVirtualTextureMaterialType RVTMaterialType : RuntimeVirtualTextureMaterialTypes)
				{
					FMeshBatch& VirtualMeshBatch = Collector.AllocateMesh();
					VirtualMeshBatch = MeshBatch;
					// RVT always samples the shared (opaque) material — a dithered fade mask would
					// punch transient holes into the cached virtual texture pages.
					VirtualMeshBatch.MaterialRenderProxy = MaterialProxy;
					VirtualMeshBatch.CastShadow = false;
					VirtualMeshBatch.bUseAsOccluder = false;
					VirtualMeshBatch.bUseForDepthPass = false;
					VirtualMeshBatch.bUseForMaterial = false;
					VirtualMeshBatch.bDitheredLODTransition = false;
					VirtualMeshBatch.bRenderToVirtualTexture = true;
					VirtualMeshBatch.RuntimeVirtualTextureMaterialType = (uint32)RVTMaterialType;
					Collector.AddMesh(ViewIndex, VirtualMeshBatch);
				}
			}

			// Add mesh to collector
			Collector.AddMesh(ViewIndex, MeshBatch);
			TotalMeshesAdded++;
		}
	}

	// ==================== Seam Mesh Pass ====================
	// Seam buckets: terrain geometry, so they draw with the terrain material and emit RVT clones like
	// chunk batches (a seam missing from RVT would crack the cached ground). Since R24 they crossfade
	// like chunks: a retained Previous bucket dithers out under the FadeOut MID while the current one
	// dithers in; a bucket with a fade state but no Previous fades in from nothing; a Previous with no
	// current bucket fades out to nothing. During a coordinated LOD transition these fades run on the
	// same alpha as the chunk interiors they meet, so old interior + old seams and new interior + new
	// seams each cover every pixel the dither gives them. RVT always takes the current bucket with the
	// shared opaque material (a dither mask would punch transient holes into cached pages).

	const bool bAnySeamFades = !bWireframe && SeamFadeStates.Num() > 0;
	if (SeamRenderData.Num() > 0 || (bAnySeamFades && SeamPreviousMeshes.Num() > 0))
	{
		auto IsFrustumCulled = [this](const FVoxelViewCull& Cull, const FBox& SeamWorldBounds)
		{
			if (!SeamWorldBounds.IsValid)
			{
				return false;
			}
			return !Cull.Intersects(SeamWorldBounds.ExpandBy(FVector(VoxelSize * 2.0f)));
		};

		auto EmitSeamBatch = [&](int32 ViewIndex, const FLocalVertexFactory* VertexFactory, const FIndexBuffer* IndexBuffer,
			const FVoxelChunkRenderData& RenderData, const FMaterialRenderProxy* DrawMaterial, bool bWithRVT)
		{
			FMeshBatch& MeshBatch = Collector.AllocateMesh();
			MeshBatch.VertexFactory = VertexFactory;
			MeshBatch.MaterialRenderProxy = DrawMaterial;
			MeshBatch.ReverseCulling = IsLocalToWorldDeterminantNegative();
			MeshBatch.bDisableBackfaceCulling = false;
			MeshBatch.Type = PT_TriangleList;
			MeshBatch.DepthPriorityGroup = SDPG_World;
			MeshBatch.bCanApplyViewModeOverrides = true;
			MeshBatch.bUseWireframeSelectionColoring = IsSelected();
			MeshBatch.bUseAsOccluder = true;
			MeshBatch.bWireframe = bWireframe;
			MeshBatch.CastShadow = true;
			MeshBatch.bUseForMaterial = true;
			MeshBatch.bUseForDepthPass = true;
			MeshBatch.LODIndex = RenderData.LODLevel;
			MeshBatch.SegmentIndex = 0;

			FMeshBatchElement& BatchElement = MeshBatch.Elements[0];
			BatchElement.IndexBuffer = IndexBuffer;
			BatchElement.FirstIndex = 0;
			BatchElement.NumPrimitives = RenderData.IndexCount / 3;
			BatchElement.MinVertexIndex = 0;
			BatchElement.MaxVertexIndex = RenderData.VertexCount - 1;
			BatchElement.PrimitiveUniformBuffer = GetUniformBuffer();

			// RVT clone pass — mirrors the chunk batch loop (terrain writes RVT; a seam that
			// skipped it would leave cracks in RVT-derived ground rendering).
			if (bWithRVT)
			{
				for (ERuntimeVirtualTextureMaterialType RVTMaterialType : RuntimeVirtualTextureMaterialTypes)
				{
					FMeshBatch& VirtualMeshBatch = Collector.AllocateMesh();
					VirtualMeshBatch = MeshBatch;
					VirtualMeshBatch.MaterialRenderProxy = MaterialProxy;
					VirtualMeshBatch.CastShadow = false;
					VirtualMeshBatch.bUseAsOccluder = false;
					VirtualMeshBatch.bUseForDepthPass = false;
					VirtualMeshBatch.bUseForMaterial = false;
					VirtualMeshBatch.bDitheredLODTransition = false;
					VirtualMeshBatch.bRenderToVirtualTexture = true;
					VirtualMeshBatch.RuntimeVirtualTextureMaterialType = (uint32)RVTMaterialType;
					Collector.AddMesh(ViewIndex, VirtualMeshBatch);
				}
			}

			Collector.AddMesh(ViewIndex, MeshBatch);
			TotalMeshesAdded++;
		};

		auto IsDrawablePrevious = [](const FVoxelChunkPreviousMesh* Previous)
		{
			return Previous && Previous->RenderData.HasValidBuffers() && Previous->RenderData.IndexCount >= 3
				&& Previous->VertexFactory.IsValid() && Previous->IndexBuffer.IsValid();
		};

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			if (!(VisibilityMap & (1 << ViewIndex)))
			{
				continue;
			}

			const FVoxelViewCull Cull(*Views[ViewIndex]);
			const bool bWithRVT = !Cull.bShadowCasters;

			for (const auto& Pair : SeamRenderData)
			{
				if (TotalMeshesAdded >= MaxMeshBatchesPerFrame)
				{
					SkippedOverLimit++;
					continue;
				}

				const FVoxelSeamMeshKey& SeamKey = Pair.Key;
				const FVoxelChunkRenderData& RenderData = Pair.Value;

				if (!RenderData.HasValidBuffers() || RenderData.IndexCount < 3)
				{
					SkippedInvisible++;
					continue;
				}

				if (Cull.IsTooFine(RenderData.LODLevel))
				{
					SkippedTooFine++;
					continue;
				}

				const TSharedPtr<FLocalVertexFactory>* VertexFactoryPtr = SeamVertexFactories.Find(SeamKey);
				if (!VertexFactoryPtr || !VertexFactoryPtr->IsValid())
				{
					SkippedInvisible++;
					continue;
				}

				const TSharedPtr<FVoxelLocalIndexBuffer>* IndexBufferPtr = SeamIndexBuffers.Find(SeamKey);
				if (!IndexBufferPtr || !IndexBufferPtr->IsValid())
				{
					SkippedInvisible++;
					continue;
				}

				// Frustum culling (same conservative box test as chunks)
				if (IsFrustumCulled(Cull, RenderData.WorldBounds))
				{
					SkippedFrustum++;
					continue;
				}

				// A crossfade has both MIDs; an appearance (R24 P3) has only the fade-in one. A far cascade draws the
				// current bucket alone with the opaque material, as it does chunks.
				const FVoxelChunkFadeState* FadeState = (bAnySeamFades && !Cull.bFarShadowCascade) ? SeamFadeStates.Find(SeamKey) : nullptr;
				const bool bFading = FadeState && FadeState->FadeInProxy && (FadeState->FadeOutProxy || FadeState->bAppear);
				if (bFading && FadeState->FadeOutProxy)
				{
					const FVoxelChunkPreviousMesh* Previous = SeamPreviousMeshes.Find(SeamKey);
					if (IsDrawablePrevious(Previous))
					{
						EmitSeamBatch(ViewIndex, Previous->VertexFactory.Get(), Previous->IndexBuffer.Get(),
							Previous->RenderData, FadeState->FadeOutProxy, /*bWithRVT*/ false);
					}
				}

				EmitSeamBatch(ViewIndex, VertexFactoryPtr->Get(), IndexBufferPtr->Get(), RenderData,
					bFading ? FadeState->FadeInProxy : MaterialProxy, bWithRVT);
			}

			// Buckets fading out to nothing (removed during a fade): only their Previous set remains.
			if (bAnySeamFades && !Cull.bFarShadowCascade)
			{
				for (const auto& Pair : SeamPreviousMeshes)
				{
					if (SeamRenderData.Contains(Pair.Key))
					{
						continue; // drawn above as the outgoing half of a crossfade
					}
					const FVoxelChunkFadeState* FadeState = SeamFadeStates.Find(Pair.Key);
					if (!FadeState || !FadeState->FadeOutProxy || !IsDrawablePrevious(&Pair.Value))
					{
						continue;
					}
					if (TotalMeshesAdded >= MaxMeshBatchesPerFrame)
					{
						SkippedOverLimit++;
						continue;
					}
					if (IsFrustumCulled(Cull, Pair.Value.RenderData.WorldBounds))
					{
						SkippedFrustum++;
						continue;
					}
					EmitSeamBatch(ViewIndex, Pair.Value.VertexFactory.Get(), Pair.Value.IndexBuffer.Get(),
						Pair.Value.RenderData, FadeState->FadeOutProxy, /*bWithRVT*/ false);
				}
			}
		}
	}

	// ==================== Water Tile Pass ====================
	// Render water tiles with separate water material

	if (WaterMaterial && WaterTileRenderData.Num() > 0)
	{
		FMaterialRenderProxy* WaterMaterialProxy = nullptr;

		if (bWireframe)
		{
			FColoredMaterialRenderProxy* WireframeWaterProxy = new FColoredMaterialRenderProxy(
				GEngine->WireframeMaterial ? GEngine->WireframeMaterial->GetRenderProxy() : nullptr,
				FLinearColor(0, 0.8f, 1.f)
			);
			Collector.RegisterOneFrameMaterialProxy(WireframeWaterProxy);
			WaterMaterialProxy = WireframeWaterProxy;
		}
		else
		{
			WaterMaterialProxy = WaterMaterial->GetRenderProxy();
		}

		if (WaterMaterialProxy)
		{
			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
			{
				if (!(VisibilityMap & (1 << ViewIndex)))
				{
					continue;
				}

				const FVoxelViewCull Cull(*Views[ViewIndex]);
				if (Cull.bShadowCasters)
				{
					continue; // water casts no shadow
				}

				for (const auto& Pair : WaterTileRenderData)
				{
					if (TotalMeshesAdded >= MaxMeshBatchesPerFrame)
					{
						SkippedOverLimit++;
						continue;
					}

					const FIntVector2& TileCoord = Pair.Key;
					const FVoxelChunkRenderData& RenderData = Pair.Value;

					if (!RenderData.HasValidBuffers() || RenderData.IndexCount < 3)
					{
						SkippedInvisible++;
						continue;
					}

					const TSharedPtr<FLocalVertexFactory>* VertexFactoryPtr = WaterTileVertexFactories.Find(TileCoord);
					if (!VertexFactoryPtr || !VertexFactoryPtr->IsValid())
					{
						SkippedInvisible++;
						continue;
					}

					const TSharedPtr<FVoxelLocalIndexBuffer>* IndexBufferPtr = WaterTileIndexBuffers.Find(TileCoord);
					if (!IndexBufferPtr || !IndexBufferPtr->IsValid())
					{
						SkippedInvisible++;
						continue;
					}

					// Frustum culling
					{
						FBox WorldBounds = RenderData.WorldBounds;
						if (WorldBounds.IsValid)
						{
							WorldBounds = WorldBounds.ExpandBy(FVector(VoxelSize * 2.0f));
							if (!Cull.Intersects(WorldBounds))
							{
								SkippedFrustum++;
								continue;
							}
						}
					}
					if (!bShadowGather)
					{
						WaterTileDrawnFrame.FindOrAdd(TileCoord) = GFrameNumberRenderThread;
					}

					FMeshBatch& MeshBatch = Collector.AllocateMesh();
					MeshBatch.VertexFactory = VertexFactoryPtr->Get();
					MeshBatch.MaterialRenderProxy = WaterMaterialProxy;
					MeshBatch.ReverseCulling = IsLocalToWorldDeterminantNegative();
					// The water mesher emits up-facing quads only: with back faces culled the surface vanished from
					// below, so a diver could not see where the air is (QA R11). Draw both sides.
					MeshBatch.bDisableBackfaceCulling = true;
					MeshBatch.Type = PT_TriangleList;
					MeshBatch.DepthPriorityGroup = SDPG_World;
					MeshBatch.bCanApplyViewModeOverrides = true;
					MeshBatch.bUseWireframeSelectionColoring = IsSelected();
					MeshBatch.bUseAsOccluder = false;  // Water is translucent
					MeshBatch.bWireframe = bWireframe;
					MeshBatch.CastShadow = false;       // Water doesn't cast shadows
					MeshBatch.bUseForMaterial = true;
					MeshBatch.bUseForDepthPass = true;
					MeshBatch.LODIndex = 0;
					MeshBatch.SegmentIndex = 0;

					FMeshBatchElement& BatchElement = MeshBatch.Elements[0];
					BatchElement.IndexBuffer = IndexBufferPtr->Get();
					BatchElement.FirstIndex = 0;
					BatchElement.NumPrimitives = RenderData.IndexCount / 3;
					BatchElement.MinVertexIndex = 0;
					BatchElement.MaxVertexIndex = RenderData.VertexCount - 1;
					BatchElement.PrimitiveUniformBuffer = GetUniformBuffer();

					Collector.AddMesh(ViewIndex, MeshBatch);
					TotalMeshesAdded++;
				}
			}
		}
	}

	// The bench's batch columns (spherical Phase 6, WM-BP3).
	{
		FVoxelBatchCounters::FCounts Counts;
		Counts.Drawn = TotalMeshesAdded;
		Counts.SkippedInvisible = SkippedInvisible;
		Counts.SkippedFrustum = SkippedFrustum;
		Counts.SkippedOverLimit = SkippedOverLimit;
		Counts.SkippedTooFine = SkippedTooFine;
		Counts.ShadowDrawn = bShadowGather ? TotalMeshesAdded : 0;
		Counts.FarShadowDrawn = bFarShadowGather ? TotalMeshesAdded : 0;
		Counts.MaxPerCall = TotalMeshesAdded;
		FVoxelBatchCounters::Add(Counts);
	}

	// Debug logging (every 60 frames)
	static int32 LogCounter = 0;
	LogCounter++;
	if (LogCounter >= 60)
	{
		LogCounter = 0;
		UE_LOG(LogVoxelRendering, Log, TEXT("GetDynamicMeshElements: Added %d meshes (limit=%d, water=%d), Skipped: %d invisible, %d frustum, %d over-limit, %d too fine for a far cascade"),
			TotalMeshesAdded, MaxMeshBatchesPerFrame, WaterTileRenderData.Num(), SkippedInvisible, SkippedFrustum, SkippedOverLimit, SkippedTooFine);
	}
}

namespace
{
	std::atomic<int32> GVoxelBatchesDrawn{0};
	std::atomic<int32> GVoxelBatchesSkippedInvisible{0};
	std::atomic<int32> GVoxelBatchesSkippedFrustum{0};
	std::atomic<int32> GVoxelBatchesSkippedOverLimit{0};
	std::atomic<int32> GVoxelBatchesShadowDrawn{0};
	std::atomic<int32> GVoxelBatchesSkippedTooFine{0};
	std::atomic<int32> GVoxelBatchesFarShadowDrawn{0};
	std::atomic<int32> GVoxelBatchesMaxPerCall{0};
}

void FVoxelBatchCounters::Add(const FCounts& Counts)
{
	GVoxelBatchesDrawn.fetch_add(Counts.Drawn, std::memory_order_relaxed);
	GVoxelBatchesSkippedInvisible.fetch_add(Counts.SkippedInvisible, std::memory_order_relaxed);
	GVoxelBatchesSkippedFrustum.fetch_add(Counts.SkippedFrustum, std::memory_order_relaxed);
	GVoxelBatchesSkippedOverLimit.fetch_add(Counts.SkippedOverLimit, std::memory_order_relaxed);
	GVoxelBatchesShadowDrawn.fetch_add(Counts.ShadowDrawn, std::memory_order_relaxed);
	GVoxelBatchesSkippedTooFine.fetch_add(Counts.SkippedTooFine, std::memory_order_relaxed);
	GVoxelBatchesFarShadowDrawn.fetch_add(Counts.FarShadowDrawn, std::memory_order_relaxed);
	int32 Max = GVoxelBatchesMaxPerCall.load(std::memory_order_relaxed);
	while (Counts.MaxPerCall > Max && !GVoxelBatchesMaxPerCall.compare_exchange_weak(Max, Counts.MaxPerCall, std::memory_order_relaxed))
	{
	}
}

FVoxelBatchCounters::FCounts FVoxelBatchCounters::Consume()
{
	FCounts Counts;
	Counts.Drawn = GVoxelBatchesDrawn.exchange(0, std::memory_order_relaxed);
	Counts.SkippedInvisible = GVoxelBatchesSkippedInvisible.exchange(0, std::memory_order_relaxed);
	Counts.SkippedFrustum = GVoxelBatchesSkippedFrustum.exchange(0, std::memory_order_relaxed);
	Counts.SkippedOverLimit = GVoxelBatchesSkippedOverLimit.exchange(0, std::memory_order_relaxed);
	Counts.ShadowDrawn = GVoxelBatchesShadowDrawn.exchange(0, std::memory_order_relaxed);
	Counts.SkippedTooFine = GVoxelBatchesSkippedTooFine.exchange(0, std::memory_order_relaxed);
	Counts.FarShadowDrawn = GVoxelBatchesFarShadowDrawn.exchange(0, std::memory_order_relaxed);
	Counts.MaxPerCall = GVoxelBatchesMaxPerCall.exchange(0, std::memory_order_relaxed);
	return Counts;
}

FPrimitiveViewRelevance FVoxelSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View);
	++RelevanceCalls;
	LastRelevanceFrame = GFrameNumberRenderThread;
	bLastShown = Result.bDrawRelevance;
	if (View && View->GetDynamicMeshElementsShadowCullFrustum() == nullptr)
	{
		++MainRelevanceCalls;
		LastMainRelevanceFrame = GFrameNumberRenderThread;
		LastRelevanceViewOrigin = View->ViewMatrices.GetViewOrigin();
		bLastRelevanceFrustumHit = View->ViewFrustum.IntersectBox(GetBounds().Origin, GetBounds().BoxExtent);
	}
	Result.bShadowRelevance = IsShadowCast(View);
	Result.bDynamicRelevance = true;
	Result.bStaticRelevance = false;
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	Result.bTranslucentSelfShadow = false;
	Result.bVelocityRelevance = false;
	// SetPrimitiveViewRelevance OVERWRITES the relevance bits (Raw = Raw), so the
	// per-material relevances must be OR-combined first. Calling it per material let
	// whichever material came last win — the always-present masked fade material
	// clobbered the water material's translucent relevance, so the translucent pass
	// never collected this proxy's water tile batches (invisible water).
	FMaterialRelevance CombinedRelevance = MaterialRelevance;
	if (WaterMaterial)
	{
		CombinedRelevance |= WaterMaterialRelevance;
	}
	if (FadeMaterial)
	{
		CombinedRelevance |= FadeMaterialRelevance;
	}
	CombinedRelevance.SetPrimitiveViewRelevance(Result);
	return Result;
}

uint32 FVoxelSceneProxy::GetMemoryFootprint() const
{
	return sizeof(*this) + GetGPUMemoryUsage();
}

// ==================== Chunk Management ====================

void FVoxelSceneProxy::ReleaseChunkFadeState_AssumesLocked(const FIntVector& ChunkCoord)
{
	if (FVoxelChunkPreviousMesh* Previous = ChunkPreviousMeshes.Find(ChunkCoord))
	{
		Previous->ReleaseResources();
		ChunkPreviousMeshes.Remove(ChunkCoord);
	}
	ChunkFadeStates.Remove(ChunkCoord);
}

void FVoxelSceneProxy::ReleaseAllChunkFadeStates_AssumesLocked()
{
	for (auto& Pair : ChunkPreviousMeshes)
	{
		Pair.Value.ReleaseResources();
	}
	ChunkPreviousMeshes.Empty();
	ChunkFadeStates.Empty();
}

void FVoxelSceneProxy::RetireOrReleaseChunk_AssumesLocked(const FIntVector& ChunkCoord, bool bMoveToPrevious)
{
#if RHI_RAYTRACING
	// The current mesh leaves its slot (released, or kept as a crossfade's Previous, which is not traced)
	ReleaseChunkRayTracing_AssumesLocked(ChunkCoord);
#endif

	// Held at alpha 0 by a coordinated LOD transition: the surface on screen is the retained
	// Previous mesh and the current one was uploaded but never shown. Replace only the current —
	// releasing Previous here would change the visible surface in the middle of the hold.
	const FVoxelChunkFadeState* HeldFade = bMoveToPrevious ? ChunkFadeStates.Find(ChunkCoord) : nullptr;
	const bool bKeepPrevious = HeldFade && HeldFade->bHeld && ChunkPreviousMeshes.Contains(ChunkCoord);

	// An appearance has nothing to fade from: the replaced mesh goes, the new one continues the fade-in.
	if (HeldFade && HeldFade->bAppear)
	{
		bMoveToPrevious = false;
	}

	// Otherwise cap at one retained generation: any older Previous goes now, whatever happens next.
	if (!bKeepPrevious)
	{
		if (FVoxelChunkPreviousMesh* OldPrevious = ChunkPreviousMeshes.Find(ChunkCoord))
		{
			OldPrevious->ReleaseResources();
			ChunkPreviousMeshes.Remove(ChunkCoord);
		}
	}

	FVoxelChunkRenderData ExistingData;
	const bool bHadData = ChunkRenderData.RemoveAndCopyValue(ChunkCoord, ExistingData);

	TSharedPtr<FVoxelLocalVertexBuffer> ExistingVB;
	ChunkVertexBuffers.RemoveAndCopyValue(ChunkCoord, ExistingVB);

	TSharedPtr<FVoxelLocalIndexBuffer> ExistingIB;
	ChunkIndexBuffers.RemoveAndCopyValue(ChunkCoord, ExistingIB);

	TSharedPtr<FLocalVertexFactory> ExistingVF;
	ChunkVertexFactories.RemoveAndCopyValue(ChunkCoord, ExistingVF);

	if (bMoveToPrevious && !bKeepPrevious && bHadData && ExistingData.HasValidBuffers()
		&& ExistingVB.IsValid() && ExistingIB.IsValid() && ExistingVF.IsValid())
	{
		// Retain the outgoing mesh alive (buffers stay initialized) for the crossfade.
		FVoxelChunkPreviousMesh Previous;
		Previous.RenderData = MoveTemp(ExistingData);
		Previous.VertexBuffer = ExistingVB;
		Previous.IndexBuffer = ExistingIB;
		Previous.VertexFactory = ExistingVF;
		ChunkPreviousMeshes.Add(ChunkCoord, MoveTemp(Previous));
		return;
	}

	if (bHadData)
	{
		ExistingData.ReleaseResources();
	}
	if (ExistingVF.IsValid())
	{
		ExistingVF->ReleaseResource();
	}
	if (ExistingIB.IsValid())
	{
		ExistingIB->ReleaseResource();
	}
	if (ExistingVB.IsValid())
	{
		ExistingVB->ReleaseResource();
	}
}

void FVoxelSceneProxy::RemoveChunk_RenderThread(const FIntVector& ChunkCoord)
{
	check(IsInRenderingThread());

	FScopeLock Lock(&ChunkDataLock);

	// Drop any crossfade state along with the chunk
	ReleaseChunkFadeState_AssumesLocked(ChunkCoord);

#if RHI_RAYTRACING
	ReleaseChunkRayTracing_AssumesLocked(ChunkCoord);
#endif

	if (FVoxelChunkRenderData* RenderData = ChunkRenderData.Find(ChunkCoord))
	{
		RenderData->ReleaseResources();
		ChunkRenderData.Remove(ChunkCoord);
	}

	if (TSharedPtr<FVoxelLocalVertexBuffer>* VB = ChunkVertexBuffers.Find(ChunkCoord))
	{
		if (VB->IsValid())
		{
			(*VB)->ReleaseResource();
		}
		ChunkVertexBuffers.Remove(ChunkCoord);
	}

	if (TSharedPtr<FVoxelLocalIndexBuffer>* IB = ChunkIndexBuffers.Find(ChunkCoord))
	{
		if (IB->IsValid())
		{
			(*IB)->ReleaseResource();
		}
		ChunkIndexBuffers.Remove(ChunkCoord);
	}

	if (TSharedPtr<FLocalVertexFactory>* VF = ChunkVertexFactories.Find(ChunkCoord))
	{
		if (VF->IsValid())
		{
			(*VF)->ReleaseResource();
		}
		ChunkVertexFactories.Remove(ChunkCoord);
	}

	UE_LOG(LogVoxelRendering, Verbose, TEXT("FVoxelSceneProxy: Removed chunk %s"), *ChunkCoord.ToString());
}

void FVoxelSceneProxy::ClearAllChunks_RenderThread()
{
	check(IsInRenderingThread());

	FScopeLock Lock(&ChunkDataLock);

	ReleaseAllChunkFadeStates_AssumesLocked();

#if RHI_RAYTRACING
	ReleaseAllChunkRayTracing_AssumesLocked();
#endif

	for (auto& Pair : ChunkRenderData)
	{
		Pair.Value.ReleaseResources();
	}
	ChunkRenderData.Empty();

	for (auto& Pair : ChunkVertexBuffers)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->ReleaseResource();
		}
	}
	ChunkVertexBuffers.Empty();

	for (auto& Pair : ChunkIndexBuffers)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->ReleaseResource();
		}
	}
	ChunkIndexBuffers.Empty();

	for (auto& Pair : ChunkVertexFactories)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->ReleaseResource();
		}
	}
	ChunkVertexFactories.Empty();

	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelSceneProxy: Cleared all chunks"));
}

void FVoxelSceneProxy::SetMaterial_RenderThread(UMaterialInterface* InMaterial, const FMaterialRelevance& InMaterialRelevance)
{
	check(IsInRenderingThread());
	Material = InMaterial;
	MaterialRelevance = InMaterialRelevance;
}

// ==================== Mesh-Swap Crossfade ====================

void FVoxelSceneProxy::SetFadeMaterial_RenderThread(UMaterialInterface* InMaterial, const FMaterialRelevance& InRelevance)
{
	check(IsInRenderingThread());
	FadeMaterial = InMaterial;
	FadeMaterialRelevance = InRelevance;
}

void FVoxelSceneProxy::SetChunkFadeState_RenderThread(const FIntVector& ChunkCoord, const FMaterialRenderProxy* InFadeInProxy, const FMaterialRenderProxy* InFadeOutProxy, bool bHeld, bool bAppear)
{
	check(IsInRenderingThread());

	FScopeLock Lock(&ChunkDataLock);

	FVoxelChunkFadeState& FadeState = ChunkFadeStates.FindOrAdd(ChunkCoord);
	FadeState.FadeAlpha = 0.0f;
	FadeState.FadeInProxy = InFadeInProxy;
	FadeState.FadeOutProxy = InFadeOutProxy;
	FadeState.bHeld = bHeld;
	FadeState.bAppear = bAppear;
}

void FVoxelSceneProxy::ReleaseFadeHolds_RenderThread(const TArray<FIntVector4>& Keys)
{
	check(IsInRenderingThread());

	FScopeLock Lock(&ChunkDataLock);

	for (const FIntVector4& Key : Keys)
	{
		// W = -1: chunk interior; 0..6: seam bucket slot (FVoxelFadeScheduler layout).
		FVoxelChunkFadeState* FadeState = (Key.W < 0)
			? ChunkFadeStates.Find(FIntVector(Key.X, Key.Y, Key.Z))
			: SeamFadeStates.Find(FVoxelSeamMeshKey{ FIntVector(Key.X, Key.Y, Key.Z), static_cast<uint8>(Key.W) });
		if (FadeState)
		{
			FadeState->bHeld = false;
		}
	}
}

void FVoxelSceneProxy::UpdateChunkFadeAlphasBatch_RenderThread(const TArray<TPair<FIntVector, float>>& Alphas)
{
	check(IsInRenderingThread());

	FScopeLock Lock(&ChunkDataLock);

	for (const TPair<FIntVector, float>& Pair : Alphas)
	{
		const float Clamped = FMath::Clamp(Pair.Value, 0.0f, 1.0f);

		if (FVoxelChunkFadeState* FadeState = ChunkFadeStates.Find(Pair.Key))
		{
			FadeState->FadeAlpha = Clamped;
		}
	}
}

void FVoxelSceneProxy::ClearChunkFadeState_RenderThread(const FIntVector& ChunkCoord)
{
	check(IsInRenderingThread());

	FScopeLock Lock(&ChunkDataLock);

	ReleaseChunkFadeState_AssumesLocked(ChunkCoord);
}

void FVoxelSceneProxy::ClearAllChunkFadeStates_RenderThread()
{
	check(IsInRenderingThread());

	FScopeLock Lock(&ChunkDataLock);

	ReleaseAllChunkFadeStates_AssumesLocked();
	ReleaseAllSeamFadeStates_AssumesLocked();
}

// ==================== Water Tile Management ====================

void FVoxelSceneProxy::SetWaterMaterial_RenderThread(UMaterialInterface* InMaterial, const FMaterialRelevance& InRelevance)
{
	check(IsInRenderingThread());
	WaterMaterial = InMaterial;
	WaterMaterialRelevance = InRelevance;
}

void FVoxelSceneProxy::UpdateWaterTileFromCPUData_RenderThread(
	FRHICommandListBase& RHICmdList,
	const FIntVector2& TileCoord,
	TArray<FVoxelVertex>&& Vertices,
	TArray<uint32>&& Indices,
	const FVector& TileWorldPosition)
{
	check(IsInRenderingThread());

	const uint32 VertexCount = Vertices.Num();
	const uint32 IndexCount = Indices.Num();

	if (VertexCount == 0 || IndexCount == 0)
	{
		return;
	}

	FScopeLock Lock(&ChunkDataLock);

	// Remove existing data if any
	if (FVoxelChunkRenderData* ExistingData = WaterTileRenderData.Find(TileCoord))
	{
		ExistingData->ReleaseResources();
	}
	if (TSharedPtr<FVoxelLocalVertexBuffer>* ExistingVB = WaterTileVertexBuffers.Find(TileCoord))
	{
		if (ExistingVB->IsValid())
		{
			(*ExistingVB)->ReleaseResource();
		}
	}
	if (TSharedPtr<FVoxelLocalIndexBuffer>* ExistingIB = WaterTileIndexBuffers.Find(TileCoord))
	{
		if (ExistingIB->IsValid())
		{
			(*ExistingIB)->ReleaseResource();
		}
	}
	if (TSharedPtr<FLocalVertexFactory>* ExistingVF = WaterTileVertexFactories.Find(TileCoord))
	{
		if (ExistingVF->IsValid())
		{
			(*ExistingVF)->ReleaseResource();
		}
	}

	// Convert FVoxelVertex to FVoxelLocalVertex format
	const FVector3f TileOffset = ToRenderOffset(TileWorldPosition);

	TArray<FVoxelLocalVertex> ConvertedVertices;
	ConvertedVertices.SetNumUninitialized(VertexCount);

	TArray<FColor> ColorData;
	ColorData.SetNumUninitialized(VertexCount);

	TArray<FVoxelPackedTangentPair> TangentData;
	TangentData.SetNumUninitialized(VertexCount);

	TArray<FVector4f> TexCoordData;
	TexCoordData.SetNumUninitialized(VertexCount);

	FBox TileBounds(ForceInit);

	for (uint32 i = 0; i < VertexCount; i++)
	{
		ConvertedVertices[i] = ConvertVertex(Vertices[i], TileOffset);
		ConvertedVertices[i].Position += TileOffset;
		ColorData[i] = ConvertedVertices[i].Color;

		TangentData[i].TangentX = ConvertedVertices[i].TangentX;
		TangentData[i].TangentZ = ConvertedVertices[i].TangentZ;

		TexCoordData[i] = FVector4f(
			ConvertedVertices[i].TexCoord.X,
			ConvertedVertices[i].TexCoord.Y,
			ConvertedVertices[i].TexCoord1.X,
			ConvertedVertices[i].TexCoord1.Y
		);

		TileBounds += FVector(Vertices[i].Position);
	}

	// Create new render data
	FVoxelChunkRenderData NewRenderData;
	NewRenderData.ChunkCoord = FIntVector(TileCoord.X, TileCoord.Y, 0);
	NewRenderData.LODLevel = 0;
	NewRenderData.VertexCount = VertexCount;
	NewRenderData.IndexCount = IndexCount;
	NewRenderData.WorldBounds = TileBounds.ShiftBy(TileWorldPosition);
	NewRenderData.ChunkWorldPosition = TileWorldPosition;

	// The vertex, colour, tangent, texcoord and index buffers and their SRVs
	CreateMeshBuffers_RenderThread(RHICmdList, FVoxelMeshBufferNames{ TEXT("VoxelWaterVertexBuffer"), TEXT("VoxelWaterColorBuffer"), TEXT("VoxelWaterTangentBuffer"), TEXT("VoxelWaterTexCoordBuffer"), TEXT("VoxelWaterIndexBuffer") },
		ConvertedVertices, ColorData, TangentData, TexCoordData, Indices, NewRenderData);

	// Store render data
	WaterTileRenderData.Add(TileCoord, NewRenderData);

	// Create and initialize vertex buffer wrapper
	TSharedPtr<FVoxelLocalVertexBuffer> VertexBufferWrapper = MakeShared<FVoxelLocalVertexBuffer>();
	VertexBufferWrapper->InitWithRHIBuffer(NewRenderData.VertexBufferRHI);
	VertexBufferWrapper->InitResource(RHICmdList);
	WaterTileVertexBuffers.Add(TileCoord, VertexBufferWrapper);

	// Create and initialize index buffer wrapper
	TSharedPtr<FVoxelLocalIndexBuffer> IndexBufferWrapper = MakeShared<FVoxelLocalIndexBuffer>();
	IndexBufferWrapper->InitWithRHIBuffer(NewRenderData.IndexBufferRHI, IndexCount);
	IndexBufferWrapper->InitResource(RHICmdList);
	WaterTileIndexBuffers.Add(TileCoord, IndexBufferWrapper);

	// Create and initialize per-tile vertex factory
	TSharedPtr<FLocalVertexFactory> TileVertexFactory = MakeShared<FLocalVertexFactory>(FeatureLevel, "FVoxelWaterTileVertexFactory");
	InitVoxelLocalVertexFactory(
		RHICmdList,
		TileVertexFactory.Get(),
		VertexBufferWrapper.Get(),
		NewRenderData.ColorSRV,
		NewRenderData.TangentsSRV,
		NewRenderData.TexCoordSRV
	);
	TileVertexFactory->InitResource(RHICmdList);
	WaterTileVertexFactories.Add(TileCoord, TileVertexFactory);

	UE_LOG(LogVoxelRendering, Verbose, TEXT("FVoxelSceneProxy: Updated water tile (%d,%d) - %d vertices, %d indices"),
		TileCoord.X, TileCoord.Y, VertexCount, IndexCount);
}

void FVoxelSceneProxy::RemoveWaterTile_RenderThread(const FIntVector2& TileCoord)
{
	check(IsInRenderingThread());

	FScopeLock Lock(&ChunkDataLock);

	if (FVoxelChunkRenderData* RenderData = WaterTileRenderData.Find(TileCoord))
	{
		RenderData->ReleaseResources();
		WaterTileRenderData.Remove(TileCoord);
	}

	if (TSharedPtr<FVoxelLocalVertexBuffer>* VB = WaterTileVertexBuffers.Find(TileCoord))
	{
		if (VB->IsValid())
		{
			(*VB)->ReleaseResource();
		}
		WaterTileVertexBuffers.Remove(TileCoord);
	}

	if (TSharedPtr<FVoxelLocalIndexBuffer>* IB = WaterTileIndexBuffers.Find(TileCoord))
	{
		if (IB->IsValid())
		{
			(*IB)->ReleaseResource();
		}
		WaterTileIndexBuffers.Remove(TileCoord);
	}

	if (TSharedPtr<FLocalVertexFactory>* VF = WaterTileVertexFactories.Find(TileCoord))
	{
		if (VF->IsValid())
		{
			(*VF)->ReleaseResource();
		}
		WaterTileVertexFactories.Remove(TileCoord);
	}

	UE_LOG(LogVoxelRendering, Verbose, TEXT("FVoxelSceneProxy: Removed water tile (%d,%d)"), TileCoord.X, TileCoord.Y);
}

void FVoxelSceneProxy::ClearAllWaterTiles_RenderThread()
{
	check(IsInRenderingThread());

	FScopeLock Lock(&ChunkDataLock);

	for (auto& Pair : WaterTileRenderData)
	{
		Pair.Value.ReleaseResources();
	}
	WaterTileRenderData.Empty();

	for (auto& Pair : WaterTileVertexBuffers)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->ReleaseResource();
		}
	}
	WaterTileVertexBuffers.Empty();

	for (auto& Pair : WaterTileIndexBuffers)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->ReleaseResource();
		}
	}
	WaterTileIndexBuffers.Empty();

	for (auto& Pair : WaterTileVertexFactories)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->ReleaseResource();
		}
	}
	WaterTileVertexFactories.Empty();

	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelSceneProxy: Cleared all water tiles"));
}

// ==================== Seam Meshes ====================

void FVoxelSceneProxy::UpdateSeamMeshFromCPUData_RenderThread(
	FRHICommandListBase& RHICmdList,
	const FIntVector& OwnerChunkCoord,
	uint8 Axis,
	int32 LODLevel,
	TArray<FVoxelVertex>&& Vertices,
	TArray<uint32>&& Indices,
	const FVector& OwnerWorldPosition,
	bool bCrossfade)
{
	check(IsInRenderingThread());

	const uint32 VertexCount = Vertices.Num();
	const uint32 IndexCount = Indices.Num();

	if (VertexCount == 0 || IndexCount == 0)
	{
		RemoveSeamMesh_RenderThread(OwnerChunkCoord, Axis, /*bFadeOut*/ bCrossfade);
		return;
	}

	const FVoxelSeamMeshKey Key{ OwnerChunkCoord, Axis };

	FScopeLock Lock(&ChunkDataLock);

	// Retire (crossfade) or release the existing bucket — a seam rebuild swaps only this bucket (§2.4)
	RetireOrReleaseSeam_AssumesLocked(Key, bCrossfade);

	// Convert FVoxelVertex to FVoxelLocalVertex format (same pipeline as terrain chunks)
	const FVector3f SeamOffset = ToRenderOffset(OwnerWorldPosition);

	TArray<FVoxelLocalVertex> ConvertedVertices;
	ConvertedVertices.SetNumUninitialized(VertexCount);

	TArray<FColor> ColorData;
	ColorData.SetNumUninitialized(VertexCount);

	TArray<FVoxelPackedTangentPair> TangentData;
	TangentData.SetNumUninitialized(VertexCount);

	TArray<FVector4f> TexCoordData;
	TexCoordData.SetNumUninitialized(VertexCount);

	// (Named SeamBounds — FPrimitiveSceneProxy already has a LocalBounds member.)
	FBox SeamBounds(ForceInit);

	for (uint32 i = 0; i < VertexCount; i++)
	{
		ConvertedVertices[i] = ConvertVertex(Vertices[i], SeamOffset);
		ConvertedVertices[i].Position += SeamOffset;
		ColorData[i] = ConvertedVertices[i].Color;

		TangentData[i].TangentX = ConvertedVertices[i].TangentX;
		TangentData[i].TangentZ = ConvertedVertices[i].TangentZ;

		TexCoordData[i] = FVector4f(
			ConvertedVertices[i].TexCoord.X,
			ConvertedVertices[i].TexCoord.Y,
			ConvertedVertices[i].TexCoord1.X,
			ConvertedVertices[i].TexCoord1.Y
		);

		SeamBounds += FVector(Vertices[i].Position);
	}

	// Create new render data
	FVoxelChunkRenderData NewRenderData;
	NewRenderData.ChunkCoord = OwnerChunkCoord;
	NewRenderData.LODLevel = LODLevel;
	NewRenderData.VertexCount = VertexCount;
	NewRenderData.IndexCount = IndexCount;
	NewRenderData.WorldBounds = SeamBounds.ShiftBy(OwnerWorldPosition);
	NewRenderData.ChunkWorldPosition = OwnerWorldPosition;

	// The vertex, colour, tangent, texcoord and index buffers and their SRVs
	CreateMeshBuffers_RenderThread(RHICmdList, FVoxelMeshBufferNames{ TEXT("VoxelSeamVertexBuffer"), TEXT("VoxelSeamColorBuffer"), TEXT("VoxelSeamTangentBuffer"), TEXT("VoxelSeamTexCoordBuffer"), TEXT("VoxelSeamIndexBuffer") },
		ConvertedVertices, ColorData, TangentData, TexCoordData, Indices, NewRenderData);

	// Store render data
	SeamRenderData.Add(Key, NewRenderData);

	// Create and initialize vertex buffer wrapper
	TSharedPtr<FVoxelLocalVertexBuffer> VertexBufferWrapper = MakeShared<FVoxelLocalVertexBuffer>();
	VertexBufferWrapper->InitWithRHIBuffer(NewRenderData.VertexBufferRHI);
	VertexBufferWrapper->InitResource(RHICmdList);
	SeamVertexBuffers.Add(Key, VertexBufferWrapper);

	// Create and initialize index buffer wrapper
	TSharedPtr<FVoxelLocalIndexBuffer> IndexBufferWrapper = MakeShared<FVoxelLocalIndexBuffer>();
	IndexBufferWrapper->InitWithRHIBuffer(NewRenderData.IndexBufferRHI, IndexCount);
	IndexBufferWrapper->InitResource(RHICmdList);
	SeamIndexBuffers.Add(Key, IndexBufferWrapper);

	// Create and initialize per-seam vertex factory
	TSharedPtr<FLocalVertexFactory> SeamVertexFactory = MakeShared<FLocalVertexFactory>(FeatureLevel, "FVoxelSeamMeshVertexFactory");
	InitVoxelLocalVertexFactory(
		RHICmdList,
		SeamVertexFactory.Get(),
		VertexBufferWrapper.Get(),
		NewRenderData.ColorSRV,
		NewRenderData.TangentsSRV,
		NewRenderData.TexCoordSRV
	);
	SeamVertexFactory->InitResource(RHICmdList);
	SeamVertexFactories.Add(Key, SeamVertexFactory);

#if RHI_RAYTRACING
	BuildRayTracing_AssumesLocked(RHICmdList, NewRenderData, SeamVertexFactory.Get(), IndexBufferWrapper.Get(), TEXT("VoxelSeamRayTracing"),
		SeamRayTracing.FindOrAdd(Key));
#endif

	UE_LOG(LogVoxelRendering, Verbose, TEXT("FVoxelSceneProxy: Updated seam mesh %s axis=%d - %d vertices, %d indices"),
		*OwnerChunkCoord.ToString(), Axis, VertexCount, IndexCount);
}

void FVoxelSceneProxy::ReleaseSeamFadeState_AssumesLocked(const FVoxelSeamMeshKey& Key)
{
	if (FVoxelChunkPreviousMesh* Previous = SeamPreviousMeshes.Find(Key))
	{
		Previous->ReleaseResources();
		SeamPreviousMeshes.Remove(Key);
	}
	SeamFadeStates.Remove(Key);
}

void FVoxelSceneProxy::ReleaseAllSeamFadeStates_AssumesLocked()
{
	for (auto& Pair : SeamPreviousMeshes)
	{
		Pair.Value.ReleaseResources();
	}
	SeamPreviousMeshes.Empty();
	SeamFadeStates.Empty();
}

void FVoxelSceneProxy::RetireOrReleaseSeam_AssumesLocked(const FVoxelSeamMeshKey& Key, bool bMoveToPrevious)
{
	// Same rules as RetireOrReleaseChunk_AssumesLocked: a held fade keeps its (visible) Previous and
	// only the never-shown current bucket is replaced; otherwise one retained generation at most.
	// Seams add one case: a bucket that is fading out to nothing has a Previous and no current, and
	// a new bucket arriving for it keeps that remnant as the outgoing half of the restarted fade.
#if RHI_RAYTRACING
	// The current bucket leaves its slot (released, or kept as a fade's Previous, which is not traced)
	ReleaseSeamRayTracing_AssumesLocked(Key);
#endif
	const FVoxelChunkFadeState* HeldFade = bMoveToPrevious ? SeamFadeStates.Find(Key) : nullptr;
	// An appearance has nothing to fade from: the replaced bucket goes, the new one continues the fade-in.
	if (HeldFade && HeldFade->bAppear)
	{
		bMoveToPrevious = false;
		HeldFade = nullptr;
	}
	const bool bHasPrevious = SeamPreviousMeshes.Contains(Key);
	const bool bKeepPrevious = bHasPrevious && bMoveToPrevious
		&& ((HeldFade && HeldFade->bHeld) || !SeamRenderData.Contains(Key));

	if (!bKeepPrevious)
	{
		if (FVoxelChunkPreviousMesh* OldPrevious = SeamPreviousMeshes.Find(Key))
		{
			OldPrevious->ReleaseResources();
			SeamPreviousMeshes.Remove(Key);
		}
	}

	FVoxelChunkRenderData ExistingData;
	const bool bHadData = SeamRenderData.RemoveAndCopyValue(Key, ExistingData);

	TSharedPtr<FVoxelLocalVertexBuffer> ExistingVB;
	SeamVertexBuffers.RemoveAndCopyValue(Key, ExistingVB);

	TSharedPtr<FVoxelLocalIndexBuffer> ExistingIB;
	SeamIndexBuffers.RemoveAndCopyValue(Key, ExistingIB);

	TSharedPtr<FLocalVertexFactory> ExistingVF;
	SeamVertexFactories.RemoveAndCopyValue(Key, ExistingVF);

	if (bMoveToPrevious && !bKeepPrevious && bHadData && ExistingData.HasValidBuffers()
		&& ExistingVB.IsValid() && ExistingIB.IsValid() && ExistingVF.IsValid())
	{
		FVoxelChunkPreviousMesh Previous;
		Previous.RenderData = MoveTemp(ExistingData);
		Previous.VertexBuffer = ExistingVB;
		Previous.IndexBuffer = ExistingIB;
		Previous.VertexFactory = ExistingVF;
		SeamPreviousMeshes.Add(Key, MoveTemp(Previous));
		return;
	}

	if (bHadData)
	{
		ExistingData.ReleaseResources();
	}
	if (ExistingVF.IsValid())
	{
		ExistingVF->ReleaseResource();
	}
	if (ExistingIB.IsValid())
	{
		ExistingIB->ReleaseResource();
	}
	if (ExistingVB.IsValid())
	{
		ExistingVB->ReleaseResource();
	}
}

void FVoxelSceneProxy::RemoveSeamMesh_RenderThread(const FIntVector& OwnerChunkCoord, uint8 Axis, bool bFadeOut)
{
	check(IsInRenderingThread());

	const FVoxelSeamMeshKey Key{ OwnerChunkCoord, Axis };

	FScopeLock Lock(&ChunkDataLock);

	if (bFadeOut)
	{
		// Fade to nothing: the bucket becomes a Previous set that dithers out under its fade state
		// (attached by the game thread just before this command) and is released on completion.
		RetireOrReleaseSeam_AssumesLocked(Key, /*bMoveToPrevious*/ true);
	}
	else
	{
		ReleaseSeamFadeState_AssumesLocked(Key);
		RetireOrReleaseSeam_AssumesLocked(Key, /*bMoveToPrevious*/ false);
	}
}

void FVoxelSceneProxy::ClearAllSeamMeshes_RenderThread()
{
	check(IsInRenderingThread());

	FScopeLock Lock(&ChunkDataLock);

	ReleaseAllSeamFadeStates_AssumesLocked();

#if RHI_RAYTRACING
	ReleaseAllSeamRayTracing_AssumesLocked();
#endif

	for (auto& Pair : SeamRenderData)
	{
		Pair.Value.ReleaseResources();
	}
	SeamRenderData.Empty();

	for (auto& Pair : SeamVertexBuffers)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->ReleaseResource();
		}
	}
	SeamVertexBuffers.Empty();

	for (auto& Pair : SeamIndexBuffers)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->ReleaseResource();
		}
	}
	SeamIndexBuffers.Empty();

	for (auto& Pair : SeamVertexFactories)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->ReleaseResource();
		}
	}
	SeamVertexFactories.Empty();

	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelSceneProxy: Cleared all seam meshes"));
}

void FVoxelSceneProxy::SetSeamFadeState_RenderThread(const FIntVector& OwnerChunkCoord, uint8 Axis,
	const FMaterialRenderProxy* InFadeInProxy, const FMaterialRenderProxy* InFadeOutProxy, bool bHeld, bool bAppear)
{
	check(IsInRenderingThread());

	FScopeLock Lock(&ChunkDataLock);

	FVoxelChunkFadeState& FadeState = SeamFadeStates.FindOrAdd(FVoxelSeamMeshKey{ OwnerChunkCoord, Axis });
	FadeState.FadeAlpha = 0.0f;
	FadeState.FadeInProxy = InFadeInProxy;
	FadeState.FadeOutProxy = InFadeOutProxy;
	FadeState.bHeld = bHeld;
	FadeState.bAppear = bAppear;
}

void FVoxelSceneProxy::UpdateSeamFadeAlphasBatch_RenderThread(const TArray<TPair<FIntVector4, float>>& Alphas)
{
	check(IsInRenderingThread());

	FScopeLock Lock(&ChunkDataLock);

	for (const TPair<FIntVector4, float>& Pair : Alphas)
	{
		const FVoxelSeamMeshKey Key{ FIntVector(Pair.Key.X, Pair.Key.Y, Pair.Key.Z), static_cast<uint8>(Pair.Key.W) };
		if (FVoxelChunkFadeState* FadeState = SeamFadeStates.Find(Key))
		{
			FadeState->FadeAlpha = FMath::Clamp(Pair.Value, 0.0f, 1.0f);
		}
	}
}

void FVoxelSceneProxy::ClearSeamFadeState_RenderThread(const FIntVector& OwnerChunkCoord, uint8 Axis)
{
	check(IsInRenderingThread());

	FScopeLock Lock(&ChunkDataLock);

	ReleaseSeamFadeState_AssumesLocked(FVoxelSeamMeshKey{ OwnerChunkCoord, Axis });
}

void FVoxelSceneProxy::ProcessBatchUpdate_RenderThread(
	FRHICommandListBase& RHICmdList,
	TArray<FBatchChunkAdd>&& Adds,
	TArray<FIntVector>&& Removals)
{
	check(IsInRenderingThread());

	// Process removals first to free up resources
	for (const FIntVector& ChunkCoord : Removals)
	{
		FScopeLock Lock(&ChunkDataLock);

		// Drop any crossfade state along with the chunk
		ReleaseChunkFadeState_AssumesLocked(ChunkCoord);

#if RHI_RAYTRACING
		ReleaseChunkRayTracing_AssumesLocked(ChunkCoord);
#endif

		if (FVoxelChunkRenderData* RenderData = ChunkRenderData.Find(ChunkCoord))
		{
			RenderData->ReleaseResources();
			ChunkRenderData.Remove(ChunkCoord);
		}

		if (TSharedPtr<FVoxelLocalVertexBuffer>* VB = ChunkVertexBuffers.Find(ChunkCoord))
		{
			if (VB->IsValid())
			{
				(*VB)->ReleaseResource();
			}
			ChunkVertexBuffers.Remove(ChunkCoord);
		}

		if (TSharedPtr<FVoxelLocalIndexBuffer>* IB = ChunkIndexBuffers.Find(ChunkCoord))
		{
			if (IB->IsValid())
			{
				(*IB)->ReleaseResource();
			}
			ChunkIndexBuffers.Remove(ChunkCoord);
		}

		if (TSharedPtr<FLocalVertexFactory>* VF = ChunkVertexFactories.Find(ChunkCoord))
		{
			if (VF->IsValid())
			{
				(*VF)->ReleaseResource();
			}
			ChunkVertexFactories.Remove(ChunkCoord);
		}
	}

	// Process adds
	for (FBatchChunkAdd& Add : Adds)
	{
		const FIntVector& ChunkCoord = Add.ChunkCoord;
		const uint32 VertexCount = Add.Vertices.Num();
		const uint32 IndexCount = Add.Indices.Num();

		if (VertexCount == 0 || IndexCount == 0)
		{
			continue;
		}

		FScopeLock Lock(&ChunkDataLock);

		// Retire (crossfade) or release any existing mesh for this chunk
		RetireOrReleaseChunk_AssumesLocked(ChunkCoord, Add.bCrossfade);

		// Convert FVoxelVertex to FVoxelLocalVertex format directly from CPU data, the chunk origin relative to the
		// primitive's (RenderOrigin, WM-BP7)
		const FVector3f ChunkOffset = ToRenderOffset(Add.ChunkWorldPosition);

		TArray<FVoxelLocalVertex> ConvertedVertices;
		ConvertedVertices.SetNumUninitialized(VertexCount);

		TArray<FColor> ColorData;
		ColorData.SetNumUninitialized(VertexCount);

		// Tangent data for SRV: interleaved TangentX + TangentZ (2 x FPackedNormal = 8 bytes per vertex)
		TArray<FVoxelPackedTangentPair> TangentData;
		TangentData.SetNumUninitialized(VertexCount);

		// TexCoord data for SRV
		// With 2 UV channels, store as float4 per vertex: (UV0.x, UV0.y, UV1.x, UV1.y)
		TArray<FVector4f> TexCoordData;
		TexCoordData.SetNumUninitialized(VertexCount);

		// Debug: Track normal statistics
		int32 ZeroNormals = 0;
		int32 UpNormals = 0;
		int32 ValidNormals = 0;

		for (uint32 i = 0; i < VertexCount; i++)
		{
			// Debug: Check input normal before conversion
			FVector3f InputNormal = Add.Vertices[i].GetNormal();

			ConvertedVertices[i] = ConvertVertex(Add.Vertices[i], ChunkOffset);
			// Offset vertex position from chunk-local to world space
			ConvertedVertices[i].Position += ChunkOffset;
			ColorData[i] = ConvertedVertices[i].Color;

			// Extract tangent data for SRV
			TangentData[i].TangentX = ConvertedVertices[i].TangentX;
			TangentData[i].TangentZ = ConvertedVertices[i].TangentZ;

			// Extract TexCoord data for SRV (both UV channels as float4)
			TexCoordData[i] = FVector4f(
				ConvertedVertices[i].TexCoord.X,
				ConvertedVertices[i].TexCoord.Y,
				ConvertedVertices[i].TexCoord1.X,
				ConvertedVertices[i].TexCoord1.Y
			);

			// Debug: Categorize normals
			if (InputNormal.IsNearlyZero(0.01f))
			{
				ZeroNormals++;
			}
			else if (FMath::Abs(InputNormal.Z - 1.0f) < 0.01f && FMath::Abs(InputNormal.X) < 0.01f && FMath::Abs(InputNormal.Y) < 0.01f)
			{
				UpNormals++;
			}
			else
			{
				ValidNormals++;
			}
		}

		// Log normal statistics for first few chunks
		static int32 DebugChunkCount = 0;
		if (DebugChunkCount < 5)
		{
			DebugChunkCount++;
			UE_LOG(LogVoxelRendering, Warning, TEXT("Chunk %s normals: %d zero, %d up-only, %d varied (total %d)"),
				*ChunkCoord.ToString(), ZeroNormals, UpNormals, ValidNormals, VertexCount);
		}

		// Create new render data
		FVoxelChunkRenderData NewRenderData;
		NewRenderData.ChunkCoord = ChunkCoord;
		NewRenderData.LODLevel = Add.LODLevel;
		NewRenderData.VertexCount = VertexCount;
		NewRenderData.IndexCount = IndexCount;
		// Offset local bounds to world space
		NewRenderData.WorldBounds = Add.LocalBounds.ShiftBy(Add.ChunkWorldPosition);
		NewRenderData.ChunkWorldPosition = Add.ChunkWorldPosition;

		// The vertex, colour, tangent, texcoord and index buffers and their SRVs
		CreateMeshBuffers_RenderThread(RHICmdList, FVoxelMeshBufferNames{ TEXT("VoxelLocalVertexBuffer_Batch"), TEXT("VoxelColorBuffer_Batch"), TEXT("VoxelTangentBuffer_Batch"), TEXT("VoxelTexCoordBuffer_Batch"), TEXT("VoxelIndexBuffer_Batch") },
			ConvertedVertices, ColorData, TangentData, TexCoordData, Add.Indices, NewRenderData);

		// Store render data
		ChunkRenderData.Add(ChunkCoord, NewRenderData);

		// Create and initialize vertex buffer wrapper
		TSharedPtr<FVoxelLocalVertexBuffer> VertexBufferWrapper = MakeShared<FVoxelLocalVertexBuffer>();
		VertexBufferWrapper->InitWithRHIBuffer(NewRenderData.VertexBufferRHI);
		VertexBufferWrapper->InitResource(RHICmdList);
		ChunkVertexBuffers.Add(ChunkCoord, VertexBufferWrapper);

		// Create and initialize index buffer wrapper
		TSharedPtr<FVoxelLocalIndexBuffer> IndexBufferWrapper = MakeShared<FVoxelLocalIndexBuffer>();
		IndexBufferWrapper->InitWithRHIBuffer(NewRenderData.IndexBufferRHI, IndexCount);
		IndexBufferWrapper->InitResource(RHICmdList);
		ChunkIndexBuffers.Add(ChunkCoord, IndexBufferWrapper);

		// Create and initialize per-chunk vertex factory
		TSharedPtr<FLocalVertexFactory> ChunkVertexFactory = MakeShared<FLocalVertexFactory>(FeatureLevel, "FVoxelChunkVertexFactory_Batch");
		InitVoxelLocalVertexFactory(
			RHICmdList,
			ChunkVertexFactory.Get(),
			VertexBufferWrapper.Get(),
			NewRenderData.ColorSRV,
			NewRenderData.TangentsSRV,
			NewRenderData.TexCoordSRV
		);
		ChunkVertexFactory->InitResource(RHICmdList);
		ChunkVertexFactories.Add(ChunkCoord, ChunkVertexFactory);

#if RHI_RAYTRACING
		BuildRayTracing_AssumesLocked(RHICmdList, NewRenderData, ChunkVertexFactory.Get(), IndexBufferWrapper.Get(), TEXT("VoxelChunkRayTracing"),
			ChunkRayTracing.FindOrAdd(ChunkCoord));
#endif
	}

	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelSceneProxy: Batch update - %d adds, %d removals processed"),
		Adds.Num(), Removals.Num());
}

// ==================== Statistics ====================

int64 FVoxelSceneProxy::GetTotalVertexCount() const
{
	FScopeLock Lock(&ChunkDataLock);

	int64 Total = 0;
	for (const auto& Pair : ChunkRenderData)
	{
		Total += Pair.Value.VertexCount;
	}
	return Total;
}

int64 FVoxelSceneProxy::GetTotalTriangleCount() const
{
	FScopeLock Lock(&ChunkDataLock);

	int64 Total = 0;
	for (const auto& Pair : ChunkRenderData)
	{
		Total += Pair.Value.IndexCount / 3;
	}
	return Total;
}

SIZE_T FVoxelSceneProxy::GetGPUMemoryUsage() const
{
	FScopeLock Lock(&ChunkDataLock);

	SIZE_T Total = 0;
	for (const auto& Pair : ChunkRenderData)
	{
		Total += Pair.Value.GetGPUMemoryUsage();
	}
	// Meshes retained for active crossfades (transient, released when each fade ends)
	for (const auto& Pair : ChunkPreviousMeshes)
	{
		Total += Pair.Value.RenderData.GetGPUMemoryUsage();
	}
	for (const auto& Pair : SeamPreviousMeshes)
	{
		Total += Pair.Value.RenderData.GetGPUMemoryUsage();
	}
#if RHI_RAYTRACING
	// The acceleration structures of the current meshes (F3.3)
	for (const auto& Pair : ChunkRayTracing)
	{
		Total += Pair.Value.Bytes;
	}
	for (const auto& Pair : SeamRayTracing)
	{
		Total += Pair.Value.Bytes;
	}
#endif
	return Total;
}

bool FVoxelSceneProxy::GetChunkDrawInfo_RenderThread(const FIntVector& ChunkCoord, FVoxelChunkDrawInfo& OutInfo) const
{
	check(IsInRenderingThread());

	FScopeLock Lock(&ChunkDataLock);

	OutInfo = FVoxelChunkDrawInfo();
	const FVoxelChunkRenderData* Current = ChunkRenderData.Find(ChunkCoord);
	const TSharedPtr<FLocalVertexFactory>* VertexFactoryPtr = ChunkVertexFactories.Find(ChunkCoord);
	const FVoxelChunkPreviousMesh* PreviousMesh = ChunkPreviousMeshes.Find(ChunkCoord);
	const FVoxelChunkFadeState* FadeState = ChunkFadeStates.Find(ChunkCoord);
	if (Current && Current->HasValidBuffers() && Current->IndexCount >= 3 && VertexFactoryPtr && VertexFactoryPtr->IsValid())
	{
		OutInfo.CurrentTriangles = Current->IndexCount / 3;
		OutInfo.LODLevel = Current->LODLevel;
		OutInfo.bVisible = true;
		OutInfo.VertexFactory = VertexFactoryPtr->Get();
	}
	OutInfo.PreviousTriangles = PreviousMesh ? PreviousMesh->RenderData.IndexCount / 3 : 0;
	if (FadeState)
	{
		OutInfo.bHasFadeState = true;
		OutInfo.FadeAlpha = FadeState->FadeAlpha;
		OutInfo.bHeld = FadeState->bHeld;
		OutInfo.bAppear = FadeState->bAppear;
	}
	// As GetDynamicMeshElements decides it (outside wireframe)
	OutInfo.bCrossfading = IsChunkFadeDrawable(FadeState, PreviousMesh);
	OutInfo.bAppearing = !OutInfo.bCrossfading && FadeState && FadeState->bAppear && FadeState->FadeInProxy;
	OutInfo.bRayTracing = bRayTracing;
#if RHI_RAYTRACING
	if (const FVoxelMeshRayTracing* CurrentRayTracing = ChunkRayTracing.Find(ChunkCoord); CurrentRayTracing && CurrentRayTracing->Geometry.IsValid())
	{
		OutInfo.RayTracingTriangles = CurrentRayTracing->Geometry->GetInitializer().TotalPrimitiveCount;
	}
	for (const auto& Pair : ChunkRayTracing)
	{
		OutInfo.ProxyRayTracingChunks += Pair.Value.Geometry.IsValid() ? 1 : 0;
		OutInfo.ProxyRayTracingBytes += Pair.Value.Bytes;
	}
	for (const auto& Pair : SeamRayTracing)
	{
		OutInfo.ProxyRayTracingSeams += Pair.Value.Geometry.IsValid() ? 1 : 0;
		OutInfo.ProxyRayTracingBytes += Pair.Value.Bytes;
	}
#endif
	return OutInfo.CurrentTriangles > 0;
}

bool FVoxelSceneProxy::GetWaterTileDrawInfo_RenderThread(const FIntVector2& TileCoord, FString& Out) const
{
	check(IsInRenderingThread());

	FScopeLock Lock(&ChunkDataLock);
	const FVoxelChunkRenderData* Data = WaterTileRenderData.Find(TileCoord);
	const TSharedPtr<FLocalVertexFactory>* VertexFactoryPtr = WaterTileVertexFactories.Find(TileCoord);
	const uint32* Drawn = WaterTileDrawnFrame.Find(TileCoord);
	const FString ProxyState = FString::Printf(TEXT("proxy (origin %s, scene bounds %s radius %.0f, draw distance %.0f to %.0f, main pass %s; relevance calls %u the last %u frames ago shown %s, of them main-view %u the last %u frames ago from %s frustum hit %s; main-view draws %u, the last %u frames ago, shadow gathers %u, water material %s)"),
		*RenderOrigin.ToString(), *GetBounds().GetBox().ToString(), GetBounds().SphereRadius, GetMinDrawDistance(), GetMaxDrawDistance(),
		ShouldRenderInMainPass() ? TEXT("yes") : TEXT("NO"),
		RelevanceCalls, GFrameNumberRenderThread - LastRelevanceFrame, bLastShown ? TEXT("yes") : TEXT("NO"),
		MainRelevanceCalls, GFrameNumberRenderThread - LastMainRelevanceFrame, *LastRelevanceViewOrigin.ToString(), bLastRelevanceFrustumHit ? TEXT("yes") : TEXT("NO"),
		MainViewCalls, GFrameNumberRenderThread - LastMainViewFrame, ShadowGatherCalls, WaterMaterial ? TEXT("set") : TEXT("NONE"));
	if (!Data)
	{
		Out = FString::Printf(TEXT("%s: no mesh for the tile%s"), *ProxyState,
			Drawn ? *FString::Printf(TEXT(", last drawn %u frames ago"), GFrameNumberRenderThread - *Drawn) : TEXT(""));
		return false;
	}
	Out = FString::Printf(TEXT("%s: %u vertices, %u indices, buffers %s, vertex factory %s, world bounds %s; drawn %s"),
		*ProxyState, Data->VertexCount, Data->IndexCount, Data->HasValidBuffers() ? TEXT("valid") : TEXT("INVALID"),
		(VertexFactoryPtr && VertexFactoryPtr->IsValid()) ? TEXT("valid") : TEXT("MISSING"),
		*Data->WorldBounds.ToString(),
		Drawn ? *FString::Printf(TEXT("%u frames ago"), GFrameNumberRenderThread - *Drawn) : TEXT("NEVER by a main view"));
	return Data->HasValidBuffers();
}

FString FVoxelSceneProxy::GetFadeDebugString_RenderThread(int32 MaxEntries, const FIntVector* FocusChunk) const
{
	check(IsInRenderingThread());

	FScopeLock Lock(&ChunkDataLock);

	FString Out = FString::Printf(TEXT("proxy: chunks %d, chunk fade states %d, chunk previous meshes %d, seam fade states %d, seam previous meshes %d"),
		ChunkRenderData.Num(), ChunkFadeStates.Num(), ChunkPreviousMeshes.Num(), SeamFadeStates.Num(), SeamPreviousMeshes.Num());
	int32 Listed = 0;
	for (const TPair<FIntVector, FVoxelChunkFadeState>& Pair : ChunkFadeStates)
	{
		if (Listed++ >= MaxEntries)
		{
			break;
		}
		const FVoxelChunkPreviousMesh* Previous = ChunkPreviousMeshes.Find(Pair.Key);
		const FVoxelChunkRenderData* Current = ChunkRenderData.Find(Pair.Key);
		Out += FString::Printf(TEXT("\n  chunk %s: alpha %.2f held %d appear %d in %d out %d, previous %s, current %s"),
			*Pair.Key.ToString(), Pair.Value.FadeAlpha, Pair.Value.bHeld ? 1 : 0, Pair.Value.bAppear ? 1 : 0,
			Pair.Value.FadeInProxy ? 1 : 0, Pair.Value.FadeOutProxy ? 1 : 0,
			Previous ? *FString::Printf(TEXT("%u tris"), Previous->RenderData.IndexCount / 3) : TEXT("none"),
			Current ? *FString::Printf(TEXT("%u tris"), Current->IndexCount / 3) : TEXT("none"));
	}
	{
		FScopeLock FocusScope(&VoxelFadeDebug::FocusLock);
		if (VoxelFadeDebug::bFocusActive.load() && (!FocusChunk || *FocusChunk == VoxelFadeDebug::FocusChunk))
		{
			Out += FString::Printf(TEXT("\n  last drawn for %s: %s"), *VoxelFadeDebug::FocusChunk.ToString(),
				VoxelFadeDebug::FocusDrawn.IsEmpty() ? TEXT("nothing since the focus was set") : *VoxelFadeDebug::FocusDrawn);
		}
		if (FocusChunk && (!VoxelFadeDebug::bFocusActive.load() || *FocusChunk != VoxelFadeDebug::FocusChunk))
		{
			VoxelFadeDebug::FocusChunk = *FocusChunk;
			VoxelFadeDebug::FocusDrawn.Reset();
		}
		VoxelFadeDebug::bFocusActive.store(FocusChunk != nullptr);
	}
	if (FocusChunk)
	{
		FVoxelChunkDrawInfo Info;
		GetChunkDrawInfo_RenderThread(*FocusChunk, Info);
		Out += FString::Printf(TEXT("\n  focus chunk %s: current %s (LOD %d, visible %d, factory %p), previous %s, fade state %s -> a frame draws %s"),
			*FocusChunk->ToString(),
			Info.CurrentTriangles > 0 ? *FString::Printf(TEXT("%u tris"), Info.CurrentTriangles) : TEXT("none"),
			Info.LODLevel, Info.bVisible ? 1 : 0, Info.VertexFactory,
			Info.PreviousTriangles > 0 ? *FString::Printf(TEXT("%u tris"), Info.PreviousTriangles) : TEXT("none"),
			Info.bHasFadeState ? *FString::Printf(TEXT("alpha %.2f held %d appear %d"), Info.FadeAlpha, Info.bHeld ? 1 : 0, Info.bAppear ? 1 : 0) : TEXT("none"),
			Info.bCrossfading ? TEXT("a crossfade") : Info.bAppearing ? TEXT("an appearance") : Info.DrawsCurrentOnly() ? TEXT("the current mesh") : TEXT("nothing"));
	}
	return Out;
}

// ==================== Ray Tracing (visual polish F3.3) ====================

FVoxelRayTracingCounters::FCounts FVoxelRayTracingCounters::Get()
{
	FCounts Counts;
	Counts.GeometriesLive = GVoxelRTGeometriesLive.load(std::memory_order_relaxed);
	Counts.BytesLive = GVoxelRTBytesLive.load(std::memory_order_relaxed);
	Counts.Builds = GVoxelRTBuilds.load(std::memory_order_relaxed);
	Counts.Releases = GVoxelRTReleases.load(std::memory_order_relaxed);
	Counts.InstancesLastFrame = GVoxelRTInstancesLastFrame.load(std::memory_order_relaxed);
	Counts.SkippedPending = GVoxelRTSkippedPending.load(std::memory_order_relaxed);
	Counts.ProxiesLive = GVoxelRTProxiesLive.load(std::memory_order_relaxed);
	return Counts;
}

void FVoxelRayTracingCounters::Reset()
{
	GVoxelRTBuilds.store(0, std::memory_order_relaxed);
	GVoxelRTReleases.store(0, std::memory_order_relaxed);
	GVoxelRTSkippedPending.store(0, std::memory_order_relaxed);
}

void FVoxelRayTracingCounters::AddBuild(int64 Bytes)
{
	GVoxelRTGeometriesLive.fetch_add(1, std::memory_order_relaxed);
	GVoxelRTBytesLive.fetch_add(Bytes, std::memory_order_relaxed);
	GVoxelRTBuilds.fetch_add(1, std::memory_order_relaxed);
}

void FVoxelRayTracingCounters::AddRelease(int64 Bytes)
{
	GVoxelRTGeometriesLive.fetch_sub(1, std::memory_order_relaxed);
	GVoxelRTBytesLive.fetch_sub(Bytes, std::memory_order_relaxed);
	GVoxelRTReleases.fetch_add(1, std::memory_order_relaxed);
}

void FVoxelRayTracingCounters::AddGathered(int64 Instances, int64 SkippedPending)
{
	// Summed over the proxies of one render frame: the first gather of a new frame starts the sum again
	const uint32 Frame = GFrameNumberRenderThread;
	if (GVoxelRTInstancesFrame.exchange(Frame, std::memory_order_relaxed) != Frame)
	{
		GVoxelRTInstancesLastFrame.store(Instances, std::memory_order_relaxed);
	}
	else
	{
		GVoxelRTInstancesLastFrame.fetch_add(Instances, std::memory_order_relaxed);
	}
	GVoxelRTSkippedPending.fetch_add(SkippedPending, std::memory_order_relaxed);
}

void FVoxelRayTracingCounters::AddProxy(int32 Delta)
{
	GVoxelRTProxiesLive.fetch_add(Delta, std::memory_order_relaxed);
}

#if RHI_RAYTRACING
void FVoxelSceneProxy::GetDynamicRayTracingInstances(FRayTracingInstanceCollector& Collector)
{
	const FMaterialRenderProxy* MaterialProxy = (bRayTracing && Material) ? Material->GetRenderProxy() : nullptr;
	if (!MaterialProxy)
	{
		return;
	}

	FScopeLock Lock(&ChunkDataLock);

	const TConstArrayView<const FSceneView*> Views = Collector.GetViews();
	const uint32 VisibilityMap = Collector.GetVisibilityMap();
	RayTracingTransform = GetLocalToWorld();
	int64 Emitted = 0;
	int64 SkippedPending = 0;

	// Per view: its origin and the squared distance beyond which a mesh is left out (0 = none)
	const double TraceDistanceScale = CVarVoxelRayTracingTraceDistanceScale.GetValueOnRenderThread();
	TArray<TPair<FVector, double>, TInlineAllocator<4>> ViewCulls;
	for (const FSceneView* View : Views)
	{
		const double Distance = TraceDistanceScale * View->FinalPostProcessSettings.LumenMaxTraceDistance;
		ViewCulls.Emplace(View->ViewMatrices.GetViewOrigin(), Distance > 0.0 ? Distance * Distance : 0.0);
	}

	auto Emit = [&](FVoxelMeshRayTracing& Entry)
	{
		const FRayTracingGeometry* Geometry = Entry.Geometry.Get();
		if (!Geometry || !Geometry->IsValid() || Geometry->IsEvicted())
		{
			return;
		}
		// The engine skips (with an ensure) an instance whose acceleration structure is still building: it joins next frame
		if (Geometry->GetRequiresBuild() || Geometry->HasPendingBuildRequest())
		{
			++SkippedPending;
			return;
		}
		// The material is the proxy's current one (SetMaterial_RenderThread may have replaced it since the build)
		Entry.MeshBatch.MaterialRenderProxy = MaterialProxy;

		FRayTracingInstance Instance;
		Instance.Geometry = Geometry;
		Instance.MaterialsView = MakeArrayView(&Entry.MeshBatch, 1);
		Instance.InstanceTransformsView = MakeArrayView(&RayTracingTransform, 1);
		Instance.NumTransforms = 1;
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			const TPair<FVector, double>& ViewCull = ViewCulls[ViewIndex];
			if ((VisibilityMap & (1u << ViewIndex))
				&& (ViewCull.Value <= 0.0 || Entry.WorldBounds.ComputeSquaredDistanceToPoint(ViewCull.Key) <= ViewCull.Value))
			{
				Collector.AddRayTracingInstance(ViewIndex, Instance);
				++Emitted;
			}
		}
	};

	for (auto& Pair : ChunkRayTracing)
	{
		Emit(Pair.Value);
	}
	for (auto& Pair : SeamRayTracing)
	{
		Emit(Pair.Value);
	}
	FVoxelRayTracingCounters::AddGathered(Emitted, SkippedPending);
}

void FVoxelSceneProxy::BuildRayTracing_AssumesLocked(FRHICommandListBase& RHICmdList, const FVoxelChunkRenderData& RenderData,
	const FLocalVertexFactory* VertexFactory, const FIndexBuffer* IndexBuffer, const TCHAR* DebugName, FVoxelMeshRayTracing& Entry)
{
	ReleaseRayTracing_AssumesLocked(Entry);
	if (!bRayTracing || !RenderData.HasValidBuffers() || RenderData.IndexCount < 3 || !VertexFactory || !IndexBuffer)
	{
		return;
	}

	// Over the buffers just uploaded: the positions are the interleaved FVoxelLocalVertex's first member, relative to
	// RenderOrigin. Built once on the GPU (fast build, no updates: a re-mesh uploads new buffers and builds anew).
	FRayTracingGeometryInitializer Initializer;
	Initializer.DebugName = FName(DebugName);
	Initializer.IndexBuffer = RenderData.IndexBufferRHI;
	Initializer.TotalPrimitiveCount = RenderData.IndexCount / 3;
	Initializer.GeometryType = RTGT_Triangles;
	Initializer.bFastBuild = true;
	Initializer.bAllowUpdate = false;

	FRayTracingGeometrySegment Segment;
	Segment.VertexBuffer = RenderData.VertexBufferRHI;
	Segment.VertexBufferElementType = VET_Float3;
	Segment.VertexBufferOffset = STRUCT_OFFSET(FVoxelLocalVertex, Position);
	Segment.VertexBufferStride = sizeof(FVoxelLocalVertex);
	Segment.MaxVertices = RenderData.VertexCount;
	Segment.NumPrimitives = Initializer.TotalPrimitiveCount;
	Initializer.Segments.Add(Segment);

	Entry.Bytes = RHICalcRayTracingGeometrySize(Initializer).ResultSize;
	Entry.Geometry = MakeUnique<FRayTracingGeometry>();
	Entry.Geometry->SetInitializer(MoveTemp(Initializer));
	Entry.Geometry->InitResource(RHICmdList);
	Entry.WorldBounds = RenderData.WorldBounds;

	// The one segment's material: the batch the instances view every frame (its material proxy is set at the gather)
	FMeshBatch& MeshBatch = Entry.MeshBatch;
	MeshBatch = FMeshBatch();
	MeshBatch.VertexFactory = VertexFactory;
	// Ray tracing needs no flip for a mirroring transform (and the primitive's transform is a translation)
	MeshBatch.ReverseCulling = false;
	MeshBatch.Type = PT_TriangleList;
	MeshBatch.DepthPriorityGroup = SDPG_World;
	MeshBatch.bCanApplyViewModeOverrides = false;
	MeshBatch.CastRayTracedShadow = true;
	MeshBatch.SegmentIndex = 0;
	FMeshBatchElement& Element = MeshBatch.Elements[0];
	Element.IndexBuffer = IndexBuffer;
	Element.FirstIndex = 0;
	Element.NumPrimitives = RenderData.IndexCount / 3;
	Element.MinVertexIndex = 0;
	Element.MaxVertexIndex = RenderData.VertexCount - 1;
	Element.PrimitiveUniformBuffer = GetUniformBuffer();

	FVoxelRayTracingCounters::AddBuild(static_cast<int64>(Entry.Bytes));
}

void FVoxelSceneProxy::ReleaseRayTracing_AssumesLocked(FVoxelMeshRayTracing& Entry)
{
	if (Entry.Geometry.IsValid())
	{
		Entry.Geometry->ReleaseResource();
		Entry.Geometry.Reset();
		FVoxelRayTracingCounters::AddRelease(static_cast<int64>(Entry.Bytes));
	}
	Entry.Bytes = 0;
}

void FVoxelSceneProxy::ReleaseChunkRayTracing_AssumesLocked(const FIntVector& ChunkCoord)
{
	if (FVoxelMeshRayTracing* Entry = ChunkRayTracing.Find(ChunkCoord))
	{
		ReleaseRayTracing_AssumesLocked(*Entry);
		ChunkRayTracing.Remove(ChunkCoord);
	}
}

void FVoxelSceneProxy::ReleaseSeamRayTracing_AssumesLocked(const FVoxelSeamMeshKey& Key)
{
	if (FVoxelMeshRayTracing* Entry = SeamRayTracing.Find(Key))
	{
		ReleaseRayTracing_AssumesLocked(*Entry);
		SeamRayTracing.Remove(Key);
	}
}

void FVoxelSceneProxy::ReleaseAllChunkRayTracing_AssumesLocked()
{
	for (auto& Pair : ChunkRayTracing)
	{
		ReleaseRayTracing_AssumesLocked(Pair.Value);
	}
	ChunkRayTracing.Empty();
}

void FVoxelSceneProxy::ReleaseAllSeamRayTracing_AssumesLocked()
{
	for (auto& Pair : SeamRayTracing)
	{
		ReleaseRayTracing_AssumesLocked(Pair.Value);
	}
	SeamRayTracing.Empty();
}
#endif
