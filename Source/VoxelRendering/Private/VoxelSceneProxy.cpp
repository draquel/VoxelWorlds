// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelSceneProxy.h"
#include "VoxelWorldComponent.h"
#include "VoxelRendering.h"
#include "VoxelLocalVertexFactory.h"
#include "LocalVFTestComponent.h"  // For InitLocalVertexFactoryStreams
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"  // For FColoredMaterialRenderProxy
#include "Engine/Engine.h"
#include "SceneManagement.h"
#include "PrimitiveSceneInfo.h"
#include "MeshBatch.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalRenderResources.h"

// ==================== Console Variables ====================

// Global debug mode variable (declared extern in VoxelLocalVertexFactory.h)
int32 GVoxelVertexColorDebugMode = EVoxelVertexColorDebugMode::Disabled;

static TAutoConsoleVariable<int32> CVarVoxelVertexColorDebugMode(
	TEXT("voxel.VertexColorDebugMode"),
	0,
	TEXT("Debug mode for voxel vertex colors:\n")
	TEXT("  0 = Disabled (R=AO, G=MaterialID, B=BiomeID) - for material graph use\n")
	TEXT("  1 = MaterialColors (RGB=MaterialColor*AO) - visual debugging\n")
	TEXT("  2 = BiomeColors (RGB=BiomeHue*AO) - visual debugging\n")
	TEXT("Note: Chunks must be re-meshed to see changes (reload level or move far away and back)."),
	ECVF_Default
);

// Sync the console variable to the global debug mode on access
static void SyncVertexColorDebugMode()
{
	GVoxelVertexColorDebugMode = CVarVoxelVertexColorDebugMode.GetValueOnAnyThread();
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

FVoxelSceneProxy::FVoxelSceneProxy(UVoxelWorldComponent* InComponent, UMaterialInterface* InMaterial)
	: FPrimitiveSceneProxy(InComponent)
	, Material(InMaterial)
	, FeatureLevel(InComponent->GetWorld()->GetFeatureLevel())
	, VoxelSize(100.0f)
{
	// Get voxel size from component if available
	if (InComponent)
	{
		VoxelSize = InComponent->GetVoxelSize();
	}

	// Get material (use default if none set)
	if (Material == nullptr)
	{
		Material = UMaterial::GetDefaultMaterial(MD_Surface);
	}

	// Cache material relevance
	MaterialRelevance = Material->GetRelevance(FeatureLevel);

	// Note: Per-chunk vertex factories are created in UpdateChunkBuffers_RenderThread

	// Set proxy properties
	bVerifyUsedMaterials = false;
	bCastDynamicShadow = true;
	bCastStaticShadow = false;
	bAffectDynamicIndirectLighting = false;
	bAffectDistanceFieldLighting = false;

	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelSceneProxy: Created with FLocalVertexFactory"));
}

FVoxelSceneProxy::~FVoxelSceneProxy()
{
	// Release all chunk resources
	FScopeLock Lock(&ChunkDataLock);

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
}

SIZE_T FVoxelSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

void FVoxelSceneProxy::CreateRenderThreadResources(FRHICommandListBase& RHICmdList)
{
	// Per-chunk vertex factories are created in UpdateChunkBuffers_RenderThread
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

	if (ChunkRenderData.Num() == 0)
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

	// Safety limit for mesh batches - should rarely be hit with proper frustum culling
	// The Non-Nanite job queue overflow was caused by Virtual Shadow Maps, not mesh count
	constexpr int32 MaxMeshBatchesPerFrame = 500;

	// Process each view
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		if (!(VisibilityMap & (1 << ViewIndex)))
		{
			continue;
		}

		const FSceneView* View = Views[ViewIndex];

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

			// Skip invisible or empty chunks
			if (!RenderData.bIsVisible || !RenderData.HasValidBuffers())
			{
				SkippedInvisible++;
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

					// Use UE's built-in frustum intersection test which handles all edge cases:
					// - Corners outside but surface visible
					// - Camera inside bounds
					// - Bounds spanning frustum planes
					const FConvexVolume& ViewFrustum = View->ViewFrustum;

					// IntersectBox returns true if the box intersects or is inside the frustum
					// This is the proper conservative test for frustum culling
					bool bIntersects = ViewFrustum.IntersectBox(WorldBounds.GetCenter(), WorldBounds.GetExtent());

					if (!bIntersects)
					{
						SkippedFrustum++;
						continue;
					}
				}
			}

			// Allocate mesh batch
			FMeshBatch& MeshBatch = Collector.AllocateMesh();
			MeshBatch.VertexFactory = VertexFactoryPtr->Get();
			MeshBatch.MaterialRenderProxy = MaterialProxy;
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

			// Add mesh to collector
			Collector.AddMesh(ViewIndex, MeshBatch);
			TotalMeshesAdded++;
		}
	}

	// Debug logging (every 60 frames)
	static int32 LogCounter = 0;
	LogCounter++;
	if (LogCounter >= 60)
	{
		LogCounter = 0;
		UE_LOG(LogVoxelRendering, Log, TEXT("GetDynamicMeshElements: Added %d meshes (limit=%d), Skipped: %d invisible, %d frustum, %d over-limit"),
			TotalMeshesAdded, MaxMeshBatchesPerFrame, SkippedInvisible, SkippedFrustum, SkippedOverLimit);
	}
}

FPrimitiveViewRelevance FVoxelSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View);
	Result.bShadowRelevance = IsShadowCast(View);
	Result.bDynamicRelevance = true;
	Result.bStaticRelevance = false;
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	Result.bTranslucentSelfShadow = false;
	Result.bVelocityRelevance = false;
	MaterialRelevance.SetPrimitiveViewRelevance(Result);
	return Result;
}

uint32 FVoxelSceneProxy::GetMemoryFootprint() const
{
	return sizeof(*this) + GetGPUMemoryUsage();
}

// ==================== Chunk Management ====================

void FVoxelSceneProxy::UpdateChunkBuffers_RenderThread(FRHICommandListBase& RHICmdList, const FIntVector& ChunkCoord, const FVoxelChunkGPUData& GPUData)
{
	check(IsInRenderingThread());

	// Sync debug mode from console variable
	SyncVertexColorDebugMode();

	// Log debug mode for verification
	static int32 LastLoggedMode = -1;
	if (GVoxelVertexColorDebugMode != LastLoggedMode)
	{
		UE_LOG(LogVoxelRendering, Log, TEXT("Vertex Color Debug Mode: %d"), GVoxelVertexColorDebugMode);
		LastLoggedMode = GVoxelVertexColorDebugMode;
	}

	if (!GPUData.HasValidBuffers() || GPUData.VertexCount == 0)
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("UpdateChunkBuffers_RenderThread: Invalid GPU data for chunk %s"), *ChunkCoord.ToString());
		return;
	}

	FScopeLock Lock(&ChunkDataLock);

	// Remove existing data if any
	if (FVoxelChunkRenderData* ExistingData = ChunkRenderData.Find(ChunkCoord))
	{
		ExistingData->ReleaseResources();
	}
	if (TSharedPtr<FVoxelLocalVertexBuffer>* ExistingVB = ChunkVertexBuffers.Find(ChunkCoord))
	{
		if (ExistingVB->IsValid())
		{
			(*ExistingVB)->ReleaseResource();
		}
	}
	if (TSharedPtr<FVoxelLocalIndexBuffer>* ExistingIB = ChunkIndexBuffers.Find(ChunkCoord))
	{
		if (ExistingIB->IsValid())
		{
			(*ExistingIB)->ReleaseResource();
		}
	}
	if (TSharedPtr<FLocalVertexFactory>* ExistingVF = ChunkVertexFactories.Find(ChunkCoord))
	{
		if (ExistingVF->IsValid())
		{
			(*ExistingVF)->ReleaseResource();
		}
	}

	// Read source vertices from the GPU buffer
	const uint32 SourceVertexCount = GPUData.VertexCount;
	const uint32 SourceVertexSize = SourceVertexCount * sizeof(FVoxelVertex);

	// Create temporary array to read the source data
	TArray<FVoxelVertex> SourceVertices;
	SourceVertices.SetNumUninitialized(SourceVertexCount);

	// Read source vertex data
	void* MappedData = RHICmdList.LockBuffer(GPUData.VertexBufferRHI, 0, SourceVertexSize, RLM_ReadOnly);
	FMemory::Memcpy(SourceVertices.GetData(), MappedData, SourceVertexSize);
	RHICmdList.UnlockBuffer(GPUData.VertexBufferRHI);

	// Convert to FVoxelLocalVertex format
	// Vertices are in local chunk space - we need to offset them to world space
	TArray<FVoxelLocalVertex> ConvertedVertices;
	ConvertedVertices.SetNumUninitialized(SourceVertexCount);

	TArray<FColor> ColorData;
	ColorData.SetNumUninitialized(SourceVertexCount);

	// Tangent data for SRV: interleaved TangentX + TangentZ (2 x FPackedNormal = 8 bytes per vertex)
	struct FPackedTangentPair
	{
		FPackedNormal TangentX;
		FPackedNormal TangentZ;
	};
	TArray<FPackedTangentPair> TangentData;
	TangentData.SetNumUninitialized(SourceVertexCount);

	// TexCoord data for SRV (GPUScene manual vertex fetch)
	// With 2 UV channels, store as float4 per vertex: (UV0.x, UV0.y, UV1.x, UV1.y)
	TArray<FVector4f> TexCoordData;
	TexCoordData.SetNumUninitialized(SourceVertexCount);

	// Get chunk world position offset
	const FVector3f ChunkOffset = FVector3f(GPUData.ChunkWorldPosition);

	// Debug: Track MaterialID and BiomeID distribution for first chunk
	static bool bLoggedMaterialIDs = false;
	TMap<uint8, int32> MaterialIDCounts;
	TMap<uint8, int32> BiomeIDCounts;

	for (uint32 i = 0; i < SourceVertexCount; i++)
	{
		// Debug: Count MaterialIDs and BiomeIDs
		if (!bLoggedMaterialIDs)
		{
			uint8 MatID = SourceVertices[i].GetMaterialID();
			uint8 BiomeID = SourceVertices[i].GetBiomeID();
			MaterialIDCounts.FindOrAdd(MatID)++;
			BiomeIDCounts.FindOrAdd(BiomeID)++;
		}

		ConvertedVertices[i] = FVoxelLocalVertex::FromVoxelVertex(SourceVertices[i]);
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
	}

	// Debug: Log MaterialID distribution once
	if (!bLoggedMaterialIDs && SourceVertexCount > 0)
	{
		bLoggedMaterialIDs = true;

		// Log to Output Log
		UE_LOG(LogVoxelRendering, Error, TEXT("=== MaterialID Distribution for chunk %s ==="), *ChunkCoord.ToString());
		for (const auto& Pair : MaterialIDCounts)
		{
			UE_LOG(LogVoxelRendering, Error, TEXT("  MaterialID %d: %d vertices"), Pair.Key, Pair.Value);
		}
		UE_LOG(LogVoxelRendering, Error, TEXT("=== BiomeID Distribution ==="));
		for (const auto& Pair : BiomeIDCounts)
		{
			UE_LOG(LogVoxelRendering, Error, TEXT("  BiomeID %d: %d vertices"), Pair.Key, Pair.Value);
		}
		UE_LOG(LogVoxelRendering, Error, TEXT("  Total: %d vertices"), SourceVertexCount);

		// Also print to screen for easy visibility
		if (GEngine)
		{
			FString ScreenMsg = FString::Printf(TEXT("MaterialID Distribution (chunk %s):"), *ChunkCoord.ToString());
			for (const auto& Pair : MaterialIDCounts)
			{
				ScreenMsg += FString::Printf(TEXT("\n  Mat %d: %d"), Pair.Key, Pair.Value);
			}
			ScreenMsg += TEXT("\nBiomeID Distribution:");
			for (const auto& Pair : BiomeIDCounts)
			{
				ScreenMsg += FString::Printf(TEXT("\n  Biome %d: %d"), Pair.Key, Pair.Value);
			}
			GEngine->AddOnScreenDebugMessage(-1, 15.0f, FColor::Yellow, ScreenMsg);
		}

		// Also log PackedMaterialData from first vertex
		if (SourceVertexCount > 0)
		{
			const FVoxelVertex& FirstVert = SourceVertices[0];
			UE_LOG(LogVoxelRendering, Warning, TEXT("  First vertex PackedMaterialData raw: 0x%08X"),
				*reinterpret_cast<const uint32*>(&FirstVert.PackedMaterialData));
			UE_LOG(LogVoxelRendering, Warning, TEXT("  First vertex GetMaterialID(): %d, GetBiomeID(): %d, GetAO(): %d"),
				FirstVert.GetMaterialID(), FirstVert.GetBiomeID(), FirstVert.GetAO());
		}
	}

	// Create new render data
	FVoxelChunkRenderData NewRenderData;
	NewRenderData.ChunkCoord = ChunkCoord;
	NewRenderData.LODLevel = GPUData.LODLevel;
	NewRenderData.VertexCount = SourceVertexCount;
	NewRenderData.IndexCount = GPUData.IndexCount;
	// Offset local bounds to world space
	NewRenderData.WorldBounds = GPUData.LocalBounds.ShiftBy(GPUData.ChunkWorldPosition);
	NewRenderData.ChunkWorldPosition = GPUData.ChunkWorldPosition;
	NewRenderData.MorphFactor = GPUData.MorphFactor;
	NewRenderData.bIsVisible = GPUData.bIsVisible;

	// Create vertex buffer
	const uint32 ConvertedVertexSize = SourceVertexCount * sizeof(FVoxelLocalVertex);
	FRHIResourceCreateInfo VertexCreateInfo(TEXT("VoxelLocalVertexBuffer"));
	NewRenderData.VertexBufferRHI = RHICmdList.CreateBuffer(
		ConvertedVertexSize,
		BUF_Static | BUF_VertexBuffer,
		sizeof(FVoxelLocalVertex),
		ERHIAccess::VertexOrIndexBuffer,
		VertexCreateInfo
	);

	void* VertexData = RHICmdList.LockBuffer(NewRenderData.VertexBufferRHI, 0, ConvertedVertexSize, RLM_WriteOnly);
	FMemory::Memcpy(VertexData, ConvertedVertices.GetData(), ConvertedVertexSize);
	RHICmdList.UnlockBuffer(NewRenderData.VertexBufferRHI);

	// Create separate color buffer for SRV
	const uint32 ColorBufferSize = SourceVertexCount * sizeof(FColor);
	FRHIResourceCreateInfo ColorCreateInfo(TEXT("VoxelColorBuffer"));
	NewRenderData.ColorBufferRHI = RHICmdList.CreateBuffer(
		ColorBufferSize,
		BUF_Static | BUF_ShaderResource,
		sizeof(FColor),
		ERHIAccess::SRVMask,
		ColorCreateInfo
	);

	void* ColorBufferData = RHICmdList.LockBuffer(NewRenderData.ColorBufferRHI, 0, ColorBufferSize, RLM_WriteOnly);
	FMemory::Memcpy(ColorBufferData, ColorData.GetData(), ColorBufferSize);
	RHICmdList.UnlockBuffer(NewRenderData.ColorBufferRHI);

	// Create color SRV (PF_B8G8R8A8 matches FColor's BGRA layout)
	NewRenderData.ColorSRV = RHICmdList.CreateShaderResourceView(NewRenderData.ColorBufferRHI, sizeof(FColor), PF_B8G8R8A8);

	// Create tangent buffer for SRV (interleaved TangentX + TangentZ, 8 bytes per vertex)
	const uint32 TangentBufferSize = SourceVertexCount * sizeof(FPackedTangentPair);
	FRHIResourceCreateInfo TangentCreateInfo(TEXT("VoxelTangentBuffer"));
	NewRenderData.TangentBufferRHI = RHICmdList.CreateBuffer(
		TangentBufferSize,
		BUF_Static | BUF_ShaderResource,
		sizeof(FPackedTangentPair),
		ERHIAccess::SRVMask,
		TangentCreateInfo
	);

	void* TangentBufferData = RHICmdList.LockBuffer(NewRenderData.TangentBufferRHI, 0, TangentBufferSize, RLM_WriteOnly);
	FMemory::Memcpy(TangentBufferData, TangentData.GetData(), TangentBufferSize);
	RHICmdList.UnlockBuffer(NewRenderData.TangentBufferRHI);

	// Create tangent SRV
	NewRenderData.TangentsSRV = RHICmdList.CreateShaderResourceView(NewRenderData.TangentBufferRHI, sizeof(FPackedNormal), PF_R8G8B8A8_SNORM);

	// Create TexCoord buffer for SRV (GPUScene manual vertex fetch)
	// With 2 UV channels, each vertex has 4 floats (UV0.xy, UV1.xy)
	const uint32 TexCoordBufferSize = SourceVertexCount * sizeof(FVector4f);
	FRHIResourceCreateInfo TexCoordCreateInfo(TEXT("VoxelTexCoordBuffer"));
	NewRenderData.TexCoordBufferRHI = RHICmdList.CreateBuffer(
		TexCoordBufferSize,
		BUF_Static | BUF_ShaderResource,
		sizeof(FVector4f),
		ERHIAccess::SRVMask,
		TexCoordCreateInfo
	);

	void* TexCoordBufferData = RHICmdList.LockBuffer(NewRenderData.TexCoordBufferRHI, 0, TexCoordBufferSize, RLM_WriteOnly);
	FMemory::Memcpy(TexCoordBufferData, TexCoordData.GetData(), TexCoordBufferSize);
	RHICmdList.UnlockBuffer(NewRenderData.TexCoordBufferRHI);

	// Create TexCoord SRV - FVector4f is 16 bytes (4x float for 2 UV channels)
	NewRenderData.TexCoordSRV = RHICmdList.CreateShaderResourceView(NewRenderData.TexCoordBufferRHI, sizeof(FVector2f), PF_G32R32F);

	// Copy index buffer reference
	NewRenderData.IndexBufferRHI = GPUData.IndexBufferRHI;

	// Store render data
	ChunkRenderData.Add(ChunkCoord, NewRenderData);

	// Create and initialize vertex buffer wrapper
	TSharedPtr<FVoxelLocalVertexBuffer> VertexBufferWrapper = MakeShared<FVoxelLocalVertexBuffer>();
	VertexBufferWrapper->InitWithRHIBuffer(NewRenderData.VertexBufferRHI);
	VertexBufferWrapper->InitResource(RHICmdList);
	ChunkVertexBuffers.Add(ChunkCoord, VertexBufferWrapper);

	// Create and initialize index buffer wrapper
	TSharedPtr<FVoxelLocalIndexBuffer> IndexBufferWrapper = MakeShared<FVoxelLocalIndexBuffer>();
	IndexBufferWrapper->InitWithRHIBuffer(NewRenderData.IndexBufferRHI, NewRenderData.IndexCount);
	IndexBufferWrapper->InitResource(RHICmdList);
	ChunkIndexBuffers.Add(ChunkCoord, IndexBufferWrapper);

	// Create and initialize per-chunk vertex factory
	TSharedPtr<FLocalVertexFactory> ChunkVertexFactory = MakeShared<FLocalVertexFactory>(FeatureLevel, "FVoxelChunkVertexFactory");
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

	UE_LOG(LogVoxelRendering, Verbose, TEXT("FVoxelSceneProxy: Updated chunk %s with %d vertices, %d indices (converted to FLocalVertexFactory format)"),
		*ChunkCoord.ToString(), SourceVertexCount, GPUData.IndexCount);
}

void FVoxelSceneProxy::UpdateChunkFromCPUData_RenderThread(
	FRHICommandListBase& RHICmdList,
	const FIntVector& ChunkCoord,
	TArray<FVoxelVertex>&& Vertices,
	TArray<uint32>&& Indices,
	int32 LODLevel,
	const FBox& ChunkLocalBounds,
	const FVector& ChunkWorldPosition)
{
	check(IsInRenderingThread());

	// Sync debug mode from console variable
	SyncVertexColorDebugMode();

	const uint32 VertexCount = Vertices.Num();
	const uint32 IndexCount = Indices.Num();

	if (VertexCount == 0 || IndexCount == 0)
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("UpdateChunkFromCPUData_RenderThread: Empty data for chunk %s"), *ChunkCoord.ToString());
		return;
	}

	FScopeLock Lock(&ChunkDataLock);

	// Remove existing data if any
	if (FVoxelChunkRenderData* ExistingData = ChunkRenderData.Find(ChunkCoord))
	{
		ExistingData->ReleaseResources();
	}
	if (TSharedPtr<FVoxelLocalVertexBuffer>* ExistingVB = ChunkVertexBuffers.Find(ChunkCoord))
	{
		if (ExistingVB->IsValid())
		{
			(*ExistingVB)->ReleaseResource();
		}
	}
	if (TSharedPtr<FVoxelLocalIndexBuffer>* ExistingIB = ChunkIndexBuffers.Find(ChunkCoord))
	{
		if (ExistingIB->IsValid())
		{
			(*ExistingIB)->ReleaseResource();
		}
	}
	if (TSharedPtr<FLocalVertexFactory>* ExistingVF = ChunkVertexFactories.Find(ChunkCoord))
	{
		if (ExistingVF->IsValid())
		{
			(*ExistingVF)->ReleaseResource();
		}
	}

	// Convert FVoxelVertex to FVoxelLocalVertex format directly from CPU data (NO GPU READBACK!)
	const FVector3f ChunkOffset = FVector3f(ChunkWorldPosition);

	TArray<FVoxelLocalVertex> ConvertedVertices;
	ConvertedVertices.SetNumUninitialized(VertexCount);

	TArray<FColor> ColorData;
	ColorData.SetNumUninitialized(VertexCount);

	// Tangent data for SRV: interleaved TangentX + TangentZ (2 x FPackedNormal = 8 bytes per vertex)
	struct FPackedTangentPair
	{
		FPackedNormal TangentX;
		FPackedNormal TangentZ;
	};
	TArray<FPackedTangentPair> TangentData;
	TangentData.SetNumUninitialized(VertexCount);

	// TexCoord data for SRV (GPUScene manual vertex fetch)
	// With 2 UV channels, store as float4 per vertex: (UV0.x, UV0.y, UV1.x, UV1.y)
	TArray<FVector4f> TexCoordData;
	TexCoordData.SetNumUninitialized(VertexCount);

	for (uint32 i = 0; i < VertexCount; i++)
	{
		ConvertedVertices[i] = FVoxelLocalVertex::FromVoxelVertex(Vertices[i]);
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
	}

	// Create new render data
	FVoxelChunkRenderData NewRenderData;
	NewRenderData.ChunkCoord = ChunkCoord;
	NewRenderData.LODLevel = LODLevel;
	NewRenderData.VertexCount = VertexCount;
	NewRenderData.IndexCount = IndexCount;
	// Offset local bounds to world space
	NewRenderData.WorldBounds = ChunkLocalBounds.ShiftBy(ChunkWorldPosition);
	NewRenderData.ChunkWorldPosition = ChunkWorldPosition;
	NewRenderData.MorphFactor = 0.0f;
	NewRenderData.bIsVisible = true;

	// Create vertex buffer
	const uint32 ConvertedVertexSize = VertexCount * sizeof(FVoxelLocalVertex);
	FRHIResourceCreateInfo VertexCreateInfo(TEXT("VoxelLocalVertexBuffer_CPU"));
	NewRenderData.VertexBufferRHI = RHICmdList.CreateBuffer(
		ConvertedVertexSize,
		BUF_Static | BUF_VertexBuffer,
		sizeof(FVoxelLocalVertex),
		ERHIAccess::VertexOrIndexBuffer,
		VertexCreateInfo
	);

	void* VertexData = RHICmdList.LockBuffer(NewRenderData.VertexBufferRHI, 0, ConvertedVertexSize, RLM_WriteOnly);
	FMemory::Memcpy(VertexData, ConvertedVertices.GetData(), ConvertedVertexSize);
	RHICmdList.UnlockBuffer(NewRenderData.VertexBufferRHI);

	// Create separate color buffer for SRV
	const uint32 ColorBufferSize = VertexCount * sizeof(FColor);
	FRHIResourceCreateInfo ColorCreateInfo(TEXT("VoxelColorBuffer_CPU"));
	NewRenderData.ColorBufferRHI = RHICmdList.CreateBuffer(
		ColorBufferSize,
		BUF_Static | BUF_ShaderResource,
		sizeof(FColor),
		ERHIAccess::SRVMask,
		ColorCreateInfo
	);

	void* ColorBufferData = RHICmdList.LockBuffer(NewRenderData.ColorBufferRHI, 0, ColorBufferSize, RLM_WriteOnly);
	FMemory::Memcpy(ColorBufferData, ColorData.GetData(), ColorBufferSize);
	RHICmdList.UnlockBuffer(NewRenderData.ColorBufferRHI);

	// Create color SRV
	NewRenderData.ColorSRV = RHICmdList.CreateShaderResourceView(NewRenderData.ColorBufferRHI, sizeof(FColor), PF_B8G8R8A8);

	// Create tangent buffer for SRV (interleaved TangentX + TangentZ, 8 bytes per vertex)
	const uint32 TangentBufferSize = VertexCount * sizeof(FPackedTangentPair);
	FRHIResourceCreateInfo TangentCreateInfo(TEXT("VoxelTangentBuffer_CPU"));
	NewRenderData.TangentBufferRHI = RHICmdList.CreateBuffer(
		TangentBufferSize,
		BUF_Static | BUF_ShaderResource,
		sizeof(FPackedTangentPair),
		ERHIAccess::SRVMask,
		TangentCreateInfo
	);

	void* TangentBufferData = RHICmdList.LockBuffer(NewRenderData.TangentBufferRHI, 0, TangentBufferSize, RLM_WriteOnly);
	FMemory::Memcpy(TangentBufferData, TangentData.GetData(), TangentBufferSize);
	RHICmdList.UnlockBuffer(NewRenderData.TangentBufferRHI);

	// Create tangent SRV
	NewRenderData.TangentsSRV = RHICmdList.CreateShaderResourceView(NewRenderData.TangentBufferRHI, sizeof(FPackedNormal), PF_R8G8B8A8_SNORM);

	// Create TexCoord buffer for SRV (GPUScene manual vertex fetch)
	// With 2 UV channels, each vertex has 4 floats (UV0.xy, UV1.xy)
	const uint32 TexCoordBufferSize = VertexCount * sizeof(FVector4f);
	FRHIResourceCreateInfo TexCoordCreateInfo(TEXT("VoxelTexCoordBuffer_CPU"));
	NewRenderData.TexCoordBufferRHI = RHICmdList.CreateBuffer(
		TexCoordBufferSize,
		BUF_Static | BUF_ShaderResource,
		sizeof(FVector4f),
		ERHIAccess::SRVMask,
		TexCoordCreateInfo
	);

	void* TexCoordBufferData = RHICmdList.LockBuffer(NewRenderData.TexCoordBufferRHI, 0, TexCoordBufferSize, RLM_WriteOnly);
	FMemory::Memcpy(TexCoordBufferData, TexCoordData.GetData(), TexCoordBufferSize);
	RHICmdList.UnlockBuffer(NewRenderData.TexCoordBufferRHI);

	// Create TexCoord SRV - FVector4f is 16 bytes (4x float for 2 UV channels)
	NewRenderData.TexCoordSRV = RHICmdList.CreateShaderResourceView(NewRenderData.TexCoordBufferRHI, sizeof(FVector2f), PF_G32R32F);

	// Create index buffer directly from CPU data
	const uint32 IndexBufferSize = IndexCount * sizeof(uint32);
	FRHIResourceCreateInfo IndexCreateInfo(TEXT("VoxelIndexBuffer_CPU"));
	NewRenderData.IndexBufferRHI = RHICmdList.CreateBuffer(
		IndexBufferSize,
		BUF_Static | BUF_IndexBuffer,
		sizeof(uint32),
		ERHIAccess::VertexOrIndexBuffer,
		IndexCreateInfo
	);

	void* IndexData = RHICmdList.LockBuffer(NewRenderData.IndexBufferRHI, 0, IndexBufferSize, RLM_WriteOnly);
	FMemory::Memcpy(IndexData, Indices.GetData(), IndexBufferSize);
	RHICmdList.UnlockBuffer(NewRenderData.IndexBufferRHI);

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
	TSharedPtr<FLocalVertexFactory> ChunkVertexFactory = MakeShared<FLocalVertexFactory>(FeatureLevel, "FVoxelChunkVertexFactory_CPU");
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

	UE_LOG(LogVoxelRendering, Verbose, TEXT("FVoxelSceneProxy: Updated chunk %s from CPU data - %d vertices, %d indices (DIRECT PATH)"),
		*ChunkCoord.ToString(), VertexCount, IndexCount);
}

void FVoxelSceneProxy::RemoveChunk_RenderThread(const FIntVector& ChunkCoord)
{
	check(IsInRenderingThread());

	FScopeLock Lock(&ChunkDataLock);

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

void FVoxelSceneProxy::SetChunkVisible_RenderThread(const FIntVector& ChunkCoord, bool bVisible)
{
	check(IsInRenderingThread());

	FScopeLock Lock(&ChunkDataLock);

	if (FVoxelChunkRenderData* RenderData = ChunkRenderData.Find(ChunkCoord))
	{
		RenderData->bIsVisible = bVisible;
	}
}

void FVoxelSceneProxy::UpdateChunkMorphFactor_RenderThread(const FIntVector& ChunkCoord, float MorphFactor)
{
	check(IsInRenderingThread());

	FScopeLock Lock(&ChunkDataLock);

	if (FVoxelChunkRenderData* RenderData = ChunkRenderData.Find(ChunkCoord))
	{
		RenderData->MorphFactor = FMath::Clamp(MorphFactor, 0.0f, 1.0f);
	}
}

void FVoxelSceneProxy::SetMaterial_RenderThread(UMaterialInterface* InMaterial, const FMaterialRelevance& InMaterialRelevance)
{
	check(IsInRenderingThread());
	Material = InMaterial;
	MaterialRelevance = InMaterialRelevance;
}

void FVoxelSceneProxy::ProcessBatchUpdate_RenderThread(
	FRHICommandListBase& RHICmdList,
	TArray<FBatchChunkAdd>&& Adds,
	TArray<FIntVector>&& Removals)
{
	check(IsInRenderingThread());

	// Sync debug mode from console variable
	SyncVertexColorDebugMode();

	// Process removals first to free up resources
	for (const FIntVector& ChunkCoord : Removals)
	{
		FScopeLock Lock(&ChunkDataLock);

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

		// Remove existing data if any
		if (FVoxelChunkRenderData* ExistingData = ChunkRenderData.Find(ChunkCoord))
		{
			ExistingData->ReleaseResources();
		}
		if (TSharedPtr<FVoxelLocalVertexBuffer>* ExistingVB = ChunkVertexBuffers.Find(ChunkCoord))
		{
			if (ExistingVB->IsValid())
			{
				(*ExistingVB)->ReleaseResource();
			}
		}
		if (TSharedPtr<FVoxelLocalIndexBuffer>* ExistingIB = ChunkIndexBuffers.Find(ChunkCoord))
		{
			if (ExistingIB->IsValid())
			{
				(*ExistingIB)->ReleaseResource();
			}
		}
		if (TSharedPtr<FLocalVertexFactory>* ExistingVF = ChunkVertexFactories.Find(ChunkCoord))
		{
			if (ExistingVF->IsValid())
			{
				(*ExistingVF)->ReleaseResource();
			}
		}

		// Convert FVoxelVertex to FVoxelLocalVertex format directly from CPU data
		const FVector3f ChunkOffset = FVector3f(Add.ChunkWorldPosition);

		TArray<FVoxelLocalVertex> ConvertedVertices;
		ConvertedVertices.SetNumUninitialized(VertexCount);

		TArray<FColor> ColorData;
		ColorData.SetNumUninitialized(VertexCount);

		// Tangent data for SRV: interleaved TangentX + TangentZ (2 x FPackedNormal = 8 bytes per vertex)
		struct FPackedTangentPair
		{
			FPackedNormal TangentX;
			FPackedNormal TangentZ;
		};
		TArray<FPackedTangentPair> TangentData;
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

			ConvertedVertices[i] = FVoxelLocalVertex::FromVoxelVertex(Add.Vertices[i]);
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
		NewRenderData.MorphFactor = 0.0f;
		NewRenderData.bIsVisible = true;

		// Create vertex buffer
		const uint32 ConvertedVertexSize = VertexCount * sizeof(FVoxelLocalVertex);
		FRHIResourceCreateInfo VertexCreateInfo(TEXT("VoxelLocalVertexBuffer_Batch"));
		NewRenderData.VertexBufferRHI = RHICmdList.CreateBuffer(
			ConvertedVertexSize,
			BUF_Static | BUF_VertexBuffer,
			sizeof(FVoxelLocalVertex),
			ERHIAccess::VertexOrIndexBuffer,
			VertexCreateInfo
		);

		void* VertexData = RHICmdList.LockBuffer(NewRenderData.VertexBufferRHI, 0, ConvertedVertexSize, RLM_WriteOnly);
		FMemory::Memcpy(VertexData, ConvertedVertices.GetData(), ConvertedVertexSize);
		RHICmdList.UnlockBuffer(NewRenderData.VertexBufferRHI);

		// Create separate color buffer for SRV
		const uint32 ColorBufferSize = VertexCount * sizeof(FColor);
		FRHIResourceCreateInfo ColorCreateInfo(TEXT("VoxelColorBuffer_Batch"));
		NewRenderData.ColorBufferRHI = RHICmdList.CreateBuffer(
			ColorBufferSize,
			BUF_Static | BUF_ShaderResource,
			sizeof(FColor),
			ERHIAccess::SRVMask,
			ColorCreateInfo
		);

		void* ColorBufferData = RHICmdList.LockBuffer(NewRenderData.ColorBufferRHI, 0, ColorBufferSize, RLM_WriteOnly);
		FMemory::Memcpy(ColorBufferData, ColorData.GetData(), ColorBufferSize);
		RHICmdList.UnlockBuffer(NewRenderData.ColorBufferRHI);

		// Create color SRV
		NewRenderData.ColorSRV = RHICmdList.CreateShaderResourceView(NewRenderData.ColorBufferRHI, sizeof(FColor), PF_B8G8R8A8);

		// Create tangent buffer for SRV (interleaved TangentX + TangentZ, 8 bytes per vertex)
		const uint32 TangentBufferSize = VertexCount * sizeof(FPackedTangentPair);
		FRHIResourceCreateInfo TangentCreateInfo(TEXT("VoxelTangentBuffer_Batch"));
		NewRenderData.TangentBufferRHI = RHICmdList.CreateBuffer(
			TangentBufferSize,
			BUF_Static | BUF_ShaderResource,
			sizeof(FPackedTangentPair),
			ERHIAccess::SRVMask,
			TangentCreateInfo
		);

		void* TangentBufferData = RHICmdList.LockBuffer(NewRenderData.TangentBufferRHI, 0, TangentBufferSize, RLM_WriteOnly);
		FMemory::Memcpy(TangentBufferData, TangentData.GetData(), TangentBufferSize);
		RHICmdList.UnlockBuffer(NewRenderData.TangentBufferRHI);

		// Create tangent SRV - use 2x4 bytes format (RGBA8 x2 for TangentX and TangentZ)
		// FLocalVertexFactory expects tangents as 2x FPackedNormal per vertex
		NewRenderData.TangentsSRV = RHICmdList.CreateShaderResourceView(NewRenderData.TangentBufferRHI, sizeof(FPackedNormal), PF_R8G8B8A8_SNORM);

		// Create TexCoord buffer for SRV
		// With 2 UV channels, each vertex has 4 floats (UV0.xy, UV1.xy)
		const uint32 TexCoordBufferSize = VertexCount * sizeof(FVector4f);
		FRHIResourceCreateInfo TexCoordCreateInfo(TEXT("VoxelTexCoordBuffer_Batch"));
		NewRenderData.TexCoordBufferRHI = RHICmdList.CreateBuffer(
			TexCoordBufferSize,
			BUF_Static | BUF_ShaderResource,
			sizeof(FVector4f),
			ERHIAccess::SRVMask,
			TexCoordCreateInfo
		);

		void* TexCoordBufferData = RHICmdList.LockBuffer(NewRenderData.TexCoordBufferRHI, 0, TexCoordBufferSize, RLM_WriteOnly);
		FMemory::Memcpy(TexCoordBufferData, TexCoordData.GetData(), TexCoordBufferSize);
		RHICmdList.UnlockBuffer(NewRenderData.TexCoordBufferRHI);

		// Create TexCoord SRV - FVector4f is 16 bytes (4x float for 2 UV channels)
		NewRenderData.TexCoordSRV = RHICmdList.CreateShaderResourceView(NewRenderData.TexCoordBufferRHI, sizeof(FVector2f), PF_G32R32F);

		// Create index buffer directly from CPU data
		const uint32 IndexBufferSize = IndexCount * sizeof(uint32);
		FRHIResourceCreateInfo IndexCreateInfo(TEXT("VoxelIndexBuffer_Batch"));
		NewRenderData.IndexBufferRHI = RHICmdList.CreateBuffer(
			IndexBufferSize,
			BUF_Static | BUF_IndexBuffer,
			sizeof(uint32),
			ERHIAccess::VertexOrIndexBuffer,
			IndexCreateInfo
		);

		void* IndexData = RHICmdList.LockBuffer(NewRenderData.IndexBufferRHI, 0, IndexBufferSize, RLM_WriteOnly);
		FMemory::Memcpy(IndexData, Add.Indices.GetData(), IndexBufferSize);
		RHICmdList.UnlockBuffer(NewRenderData.IndexBufferRHI);

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
	return Total;
}
