// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelChunkSceneProxy.h"
#include "VoxelRendering.h"
#include "VoxelRayTracingCounters.h"
#include "PrimitiveSceneProxyDesc.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialRenderProxy.h"
#include "Engine/Engine.h"
#include "SceneManagement.h"
#include "MeshBatch.h"
#include "RenderingThread.h"
#include "Containers/Queue.h"
#include "MeshCardBuild.h"
#include "MeshCardRepresentation.h"
#include "VoxelChunkMeshCardsPrivate.h"
#if RHI_RAYTRACING
#include "RayTracingInstance.h"
#include "RayTracingGeometryManagerInterface.h"
#endif

namespace
{
	TAutoConsoleVariable<int32> CVarVoxelChunkPrimitivesDynamic(
		TEXT("voxel.Render.ChunkPrimitives.Dynamic"),
		0,
		TEXT("Diagnostic (G116): the chunk primitives draw their meshes on the dynamic path (GetDynamicMeshElements every frame) ")
		TEXT("instead of their cached static batches. 0 = static."),
		ECVF_RenderThreadSafe);

	/**
	 * Meshes whose last primitive the scene deleted off the render thread: the scene deletes removed primitives in a task of
	 * its update, beside the tasks that cache the new primitives' draw commands; their vertex factories, buffers and ray
	 * tracing geometry are released on the render thread instead (DrainChunkMeshReleases_RenderThread).
	 */
	TQueue<FVoxelChunkMeshRef, EQueueMode::Mpsc> GVoxelChunkMeshReleases;
	/** The meshes in GVoxelChunkMeshReleases: the producers' and the game thread's view of it (IsEmpty is the consumer's) */
	std::atomic<int32> GVoxelChunkMeshReleasesQueued{ 0 };

	void QueueChunkMeshRelease(FVoxelChunkMeshRef&& Mesh)
	{
		GVoxelChunkMeshReleases.Enqueue(MoveTemp(Mesh));
		GVoxelChunkMeshReleasesQueued.fetch_add(1, std::memory_order_release);
	}

	std::atomic<int64> GVoxelChunkPrimitivesAdded{ 0 };
	std::atomic<int64> GVoxelChunkPrimitivesRemoved{ 0 };
}

// ==================== FVoxelChunkPrimitiveCounters ====================

FVoxelChunkPrimitiveCounters::FCounts FVoxelChunkPrimitiveCounters::Get()
{
	FCounts Counts;
	Counts.Added = GVoxelChunkPrimitivesAdded.load(std::memory_order_relaxed);
	Counts.Removed = GVoxelChunkPrimitivesRemoved.load(std::memory_order_relaxed);
	Counts.Live = Counts.Added - Counts.Removed;
	return Counts;
}

void FVoxelChunkPrimitiveCounters::AddAdded(int64 Count)
{
	GVoxelChunkPrimitivesAdded.fetch_add(Count, std::memory_order_relaxed);
}

void FVoxelChunkPrimitiveCounters::AddRemoved(int64 Count)
{
	GVoxelChunkPrimitivesRemoved.fetch_add(Count, std::memory_order_relaxed);
}

// ==================== FVoxelChunkMesh ====================

FVoxelChunkMesh::FVoxelChunkMesh(TArray<FVoxelVertex>&& InVertices, TArray<uint32>&& InIndices, int32 InLODLevel, const FBox& InLocalBounds,
	const FVector& InOrigin, const TSharedRef<std::atomic<int64>, ESPMode::ThreadSafe>& InByteCounter, const FVoxelChunkMeshCardSettings& InCards,
	const TSharedPtr<const FVoxelPrebuiltChunkMeshCards, ESPMode::ThreadSafe>& InPrebuiltCards)
	: LODLevel(InLODLevel)
	, LocalBounds(InLocalBounds)
	, Origin(InOrigin)
	, PendingVertices(MoveTemp(InVertices))
	, PendingIndices(MoveTemp(InIndices))
	, NumTriangles(static_cast<uint32>(PendingIndices.Num() / 3))
	, ByteCounter(InByteCounter)
{
	if (InCards.HasCards() && NumTriangles > 0 && LocalBounds.IsValid)
	{
		const double StartTime = FPlatformTime::Seconds();
		CardRepresentation = MakeUnique<FCardRepresentationData>();
		FMeshCardsBuildData& CardData = CardRepresentation->MeshCardsBuildData;
		CardData.Bounds = LocalBounds;
		// One-sided (the terrain is closed from the outside) and undilated: a two-sided mesh's card bias of 50 uu would let
		// a cave's hit take the lit ground's card above it
		CardData.bMostlyTwoSided = false;
		FVoxelChunkMeshCardBuildStats Stats;
		const bool bFitted = InCards.Mode == EVoxelChunkMeshCards::Fitted;
		// The mesh worker's cards, when it fitted them as this mesh wants in the box the mesh has (a settings change between
		// the mesher and the upload, or another box, has them fitted again here)
		const bool bPrebuilt = bFitted && InPrebuiltCards.IsValid() && InPrebuiltCards->Settings == InCards
			&& InPrebuiltCards->LocalBounds.Equals(LocalBounds, 0.01);
		if (bPrebuilt)
		{
			CardData.CardBuildData = InPrebuiltCards->Cards;
			Stats = InPrebuiltCards->Stats;
		}
		else if (bFitted)
		{
			BuildVoxelChunkMeshFittedCards(PendingVertices, PendingIndices, LocalBounds, InCards, CardData, &Stats);
		}
		else
		{
			BuildVoxelChunkMeshCards(LocalBounds, InCards.Slices, CardData);
		}
		const int32 NumCards = CardData.CardBuildData.Num();
		if (NumCards == 0)
		{
			// Nothing a card captures (a sliver below the cluster minimum): no cards, out of Lumen's scene as without them
			CardRepresentation.Reset();
		}
		FVoxelChunkMeshCardCounters::AddBuild(NumCards, FPlatformTime::Seconds() - StartTime, bFitted ? &Stats : nullptr, InCards.MaxCards, bPrebuilt);
	}
}

void BuildVoxelChunkMeshCards(const FBox& LocalBounds, int32 Slices, FMeshCardsBuildData& OutCardData)
{
	// Axis-aligned cards, as the engine gives a mesh without built cards (dynamic and spline meshes): the box is all they
	// need, so nothing reads a neighbour. A card captures the first surface facing it inside its own box, so one card per
	// direction over the whole box sees one layer: the ground over a cave hides the cave's floor from the box's +Z card,
	// and a cave's ceiling below an overhang hides the overhang's underside from the -Z card. Slices along each axis peel
	// the layers: slice i's two cards on that axis cover the box's i-th part along it, and all of it across.
	Slices = FMath::Clamp(Slices, 1, MaxVoxelChunkMeshCardSlices);
	OutCardData.CardBuildData.Reset(6 * Slices);
	const FVector Size = LocalBounds.GetSize();
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		for (int32 Slice = 0; Slice < Slices; ++Slice)
		{
			FMeshCardsBuildData SliceData;
			SliceData.Bounds = LocalBounds;
			SliceData.Bounds.Min[Axis] = LocalBounds.Min[Axis] + Size[Axis] * Slice / Slices;
			SliceData.Bounds.Max[Axis] = LocalBounds.Min[Axis] + Size[Axis] * (Slice + 1) / Slices;
			MeshCardRepresentation::SetCardsFromBounds(SliceData);
			// The slice's two cards facing along this axis (directions 2 * Axis and 2 * Axis + 1: -X +X, -Y +Y, -Z +Z)
			for (const FLumenCardBuildData& Card : SliceData.CardBuildData)
			{
				if (Card.AxisAlignedDirectionIndex / 2 == Axis)
				{
					OutCardData.CardBuildData.Add(Card);
				}
			}
		}
	}
}

FVoxelChunkMesh::~FVoxelChunkMesh()
{
#if RHI_RAYTRACING
	if (RayTracingGeometry.IsValid())
	{
		RayTracingGeometry->ReleaseResource();
		RayTracingGeometry.Reset();
		FVoxelRayTracingCounters::AddRelease(static_cast<int64>(RayTracingBytes));
	}
	if (!RayTracingGroupHandle.IsNull())
	{
		// The manager keeps the group until the primitives that cached it are gone too
		GRayTracingGeometryManager->ReleaseRayTracingGeometryGroup(RayTracingGroupHandle);
		RayTracingGroupHandle = {};
	}
#endif
	if (VertexFactory.IsValid())
	{
		VertexFactory->ReleaseResource();
	}
	if (IndexBuffer.IsValid())
	{
		IndexBuffer->ReleaseResource();
	}
	if (VertexBuffer.IsValid())
	{
		VertexBuffer->ReleaseResource();
	}
	// Takes the buffers' bytes off the region component's count
	RenderData.ReleaseResources();
}

void FVoxelChunkMesh::InitResources_RenderThread(FRHICommandListBase& RHICmdList, bool bPositionOnly, bool bRadialFaceUp, const FVector& FaceUpCenter,
	bool bRayTracing, ERHIFeatureLevel::Type FeatureLevel)
{
	check(IsInRenderingThread());

	const uint32 VertexCount = PendingVertices.Num();
	const uint32 IndexCount = PendingIndices.Num();
	if (VertexBuffer.IsValid() || VertexCount == 0 || IndexCount == 0)
	{
		return;
	}

	// The vertices stay relative to the chunk's origin, where the primitive sits. On a planet the face type is measured
	// against the radial up at the vertex (SP-D3): the centre relative to the origin, in float, is a direction's worth of
	// precision (a centre 10^8 uu away is off by 8 uu).
	const FVector3f LocalFaceUpCenter(FaceUpCenter - Origin);
	TArray<FVoxelLocalVertex> ConvertedVertices;
	ConvertedVertices.SetNumUninitialized(VertexCount);
	TArray<FColor> ColorData;
	ColorData.SetNumUninitialized(VertexCount);
	TArray<FVoxelPackedTangentPair> TangentData;
	TangentData.SetNumUninitialized(VertexCount);
	TArray<FVector4f> TexCoordData;
	TexCoordData.SetNumUninitialized(VertexCount);
	for (uint32 i = 0; i < VertexCount; ++i)
	{
		const FVoxelVertex& Vertex = PendingVertices[i];
		FVoxelLocalVertex& Converted = ConvertedVertices[i];
		Converted = bRadialFaceUp
			? FVoxelLocalVertex::FromVoxelVertex(Vertex, (Vertex.Position - LocalFaceUpCenter).GetSafeNormal())
			: FVoxelLocalVertex::FromVoxelVertex(Vertex);
		ColorData[i] = Converted.Color;
		TangentData[i].TangentX = Converted.TangentX;
		TangentData[i].TangentZ = Converted.TangentZ;
		TexCoordData[i] = FVector4f(Converted.TexCoord.X, Converted.TexCoord.Y, Converted.TexCoord1.X, Converted.TexCoord1.Y);
	}

	RenderData.LODLevel = LODLevel;
	RenderData.VertexCount = VertexCount;
	RenderData.IndexCount = IndexCount;
	RenderData.WorldBounds = LocalBounds.ShiftBy(Origin);
	RenderData.ChunkWorldPosition = Origin;
	CreateVoxelMeshBuffers_RenderThread(RHICmdList,
		FVoxelMeshBufferNames{ TEXT("VoxelChunkPrimitiveVertexBuffer"), TEXT("VoxelChunkPrimitiveColorBuffer"), TEXT("VoxelChunkPrimitiveTangentBuffer"),
			TEXT("VoxelChunkPrimitiveTexCoordBuffer"), TEXT("VoxelChunkPrimitiveIndexBuffer") },
		bPositionOnly, ConvertedVertices, ColorData, TangentData, TexCoordData, PendingIndices, &ByteCounter.Get(), RenderData);

	VertexBuffer = MakeShared<FVoxelLocalVertexBuffer>();
	VertexBuffer->InitWithRHIBuffer(RenderData.VertexBufferRHI);
	VertexBuffer->InitResource(RHICmdList);

	IndexBuffer = MakeShared<FVoxelLocalIndexBuffer>();
	IndexBuffer->InitWithRHIBuffer(RenderData.IndexBufferRHI, IndexCount);
	IndexBuffer->InitResource(RHICmdList);

	VertexFactory = MakeShared<FLocalVertexFactory>(FeatureLevel, "FVoxelChunkPrimitiveVertexFactory");
	InitVoxelLocalVertexFactory(RHICmdList, VertexFactory.Get(), VertexBuffer.Get(), bPositionOnly,
		RenderData.ColorSRV, RenderData.TangentsSRV, RenderData.TexCoordSRV);
	VertexFactory->InitResource(RHICmdList);

#if RHI_RAYTRACING
	if (bRayTracing)
	{
		// A group of its own (no LODs, the geometry an additional one): the primitive caching its instance follows the
		// geometry's build and eviction (step 3)
		RayTracingGroupHandle = GRayTracingGeometryManager->RegisterRayTracingGeometryGroup(/*NumLODs*/ 0, /*CurrentFirstLODIdx*/ 0);
		RayTracingGeometry = MakeUnique<FRayTracingGeometry>();
		RayTracingBytes = InitVoxelRayTracingGeometry(RHICmdList, RenderData, TEXT("VoxelChunkPrimitiveRayTracing"), *RayTracingGeometry, RayTracingGroupHandle);
		FVoxelRayTracingCounters::AddBuild(static_cast<int64>(RayTracingBytes));
	}
#endif

	PendingVertices.Empty();
	PendingIndices.Empty();
}

void DrainChunkMeshReleases_RenderThread()
{
	check(IsInRenderingThread());
	FVoxelChunkMeshRef Mesh;
	while (GVoxelChunkMeshReleases.Dequeue(Mesh))
	{
		GVoxelChunkMeshReleasesQueued.fetch_sub(1, std::memory_order_relaxed);
		Mesh.Reset();
	}
}

void EnqueueDrainChunkMeshReleases()
{
	if (GVoxelChunkMeshReleasesQueued.load(std::memory_order_acquire) <= 0)
	{
		return;
	}
	ENQUEUE_RENDER_COMMAND(DrainVoxelChunkMeshReleases)(
		[](FRHICommandListImmediate&)
		{
			DrainChunkMeshReleases_RenderThread();
		});
}

void ReleaseChunkMeshOnRenderThread(FVoxelChunkMeshRef&& Mesh)
{
	if (!Mesh.IsValid())
	{
		return;
	}
	ENQUEUE_RENDER_COMMAND(ReleaseVoxelChunkMesh)(
		[Released = MoveTemp(Mesh)](FRHICommandListImmediate&) mutable
		{
			Released.Reset();
		});
	Mesh.Reset();
}

// ==================== FVoxelChunkSceneProxy ====================

FVoxelChunkSceneProxy::FVoxelChunkSceneProxy(const FPrimitiveSceneProxyDesc& Desc, FInit&& Init)
	: FPrimitiveSceneProxy(Desc)
	, Mesh(MoveTemp(Init.Mesh))
	, Previous(MoveTemp(Init.Previous))
	, Fade(Init.Fade)
	, FadeMaterialRelevance(Init.FadeMaterialRelevance)
	, Material(Init.Material)
	, MaterialRelevance(Init.MaterialRelevance)
	, FeatureLevel(Init.FeatureLevel)
	, bRayTracing(Init.bRayTracing && Mesh.IsValid())
	, bPositionOnly(Init.bPositionOnly)
	, bRadialFaceUp(Init.bRadialFaceUp)
	, FaceUpCenter(Init.FaceUpCenter)
	, bSeam(Init.bSeam)
	, bLumenCards(Init.bLumenCards && Mesh.IsValid() && Mesh->CardRepresentation.IsValid())
{
	// The scene's record of this primitive carries the id the description gave it (FPrimitiveSceneInfo's ComponentId)
	SceneData.PrimitiveSceneId = Desc.ComponentId;
	SceneData.SceneProxy = this;
	bFading.store(Init.bFading, std::memory_order_relaxed);

	if (Material == nullptr)
	{
		Material = UMaterial::GetDefaultMaterial(MD_Surface);
		MaterialRelevance = Material->GetRelevance(GetFeatureLevelShaderPlatform(FeatureLevel));
	}

	// As the region proxy: no material verification, no static shadowing, no distance fields; the runtime virtual textures
	// the description named are written by the static batch's clones
	bVerifyUsedMaterials = false;
	bCastStaticShadow = false;
	bAffectDistanceFieldLighting = false;
	bSupportsRuntimeVirtualTexture = true;

	// Lumen's scene takes it with its mesh's cards (step 4): the flag has Lumen cache a card capture command for the static
	// batch and capture the cards; without cards it stays out (a hit on it then reads black, as before step 4). The base
	// constructor decided the scene membership before this one set the flag and the ray tracing representation.
	bAffectDynamicIndirectLighting = bLumenCards;
	UpdateVisibleInLumenScene();

	// GPUScene: the static batches take the primitive's data from it (no primitive uniform buffer), and the cached ray
	// tracing instances their transform
	bStaticElementsAlwaysUseProxyPrimitiveUniformBuffer = true;
	EnableGPUSceneSupportFlags();

#if RHI_RAYTRACING
	if (bRayTracing)
	{
		FVoxelRayTracingCounters::AddProxy(1);
	}
#endif
}

FVoxelChunkSceneProxy::~FVoxelChunkSceneProxy()
{
	// The mesh goes with the primitive, once the scene has deleted it (its cached draw commands are gone by then). The scene
	// deletes primitives in a task of its update: the references go to the render thread's queue from there (a primitive
	// never added is deleted on the render thread itself)
	if (IsInRenderingThread())
	{
		Mesh.Reset();
		Previous.Reset();
	}
	else
	{
		if (Mesh.IsValid())
		{
			QueueChunkMeshRelease(MoveTemp(Mesh));
		}
		if (Previous.IsValid())
		{
			QueueChunkMeshRelease(MoveTemp(Previous));
		}
	}
#if RHI_RAYTRACING
	if (bRayTracing)
	{
		FVoxelRayTracingCounters::AddProxy(-1);
	}
#endif
}

SIZE_T FVoxelChunkSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

uint32 FVoxelChunkSceneProxy::GetMemoryFootprint() const
{
	return sizeof(*this) + GetAllocatedSize();
}

void FVoxelChunkSceneProxy::CreateRenderThreadResources(FRHICommandListBase& RHICmdList)
{
	if (Mesh.IsValid())
	{
		Mesh->InitResources_RenderThread(RHICmdList, bPositionOnly, bRadialFaceUp, FaceUpCenter, bRayTracing, FeatureLevel);
	}
	// A fade's outgoing mesh normally has its buffers from the primitive that drew it; one replaced twice before a flush
	// came from a primitive the scene never added, and uploads here (once: a mesh with buffers returns at once)
	if (Previous.IsValid())
	{
		Previous->InitResources_RenderThread(RHICmdList, bPositionOnly, bRadialFaceUp, FaceUpCenter, bRayTracing, FeatureLevel);
	}
}

void FVoxelChunkSceneProxy::InitMeshBatch(FMeshBatch& MeshBatch, const FVoxelChunkMesh& InMesh, const FMaterialRenderProxy* MaterialProxy) const
{
	MeshBatch.VertexFactory = InMesh.VertexFactory.Get();
	MeshBatch.MaterialRenderProxy = MaterialProxy;
	MeshBatch.ReverseCulling = IsLocalToWorldDeterminantNegative();
	MeshBatch.bDisableBackfaceCulling = false;
	MeshBatch.Type = PT_TriangleList;
	MeshBatch.DepthPriorityGroup = SDPG_World;
	MeshBatch.bCanApplyViewModeOverrides = true;
	MeshBatch.bUseWireframeSelectionColoring = false;
	MeshBatch.bUseAsOccluder = true;
	MeshBatch.bWireframe = false;
	MeshBatch.CastShadow = true;
	MeshBatch.bUseForMaterial = true;
	MeshBatch.bUseForDepthPass = true;
	// One LOD per primitive: the scene's LOD selection over its static batches always picks it
	MeshBatch.LODIndex = 0;
	MeshBatch.SegmentIndex = 0;

	FMeshBatchElement& Element = MeshBatch.Elements[0];
	Element.IndexBuffer = InMesh.IndexBuffer.Get();
	Element.FirstIndex = 0;
	Element.NumPrimitives = InMesh.RenderData.IndexCount / 3;
	Element.MinVertexIndex = 0;
	Element.MaxVertexIndex = InMesh.RenderData.VertexCount - 1;
}

void FVoxelChunkSceneProxy::DrawStaticElements(FStaticPrimitiveDrawInterface* PDI)
{
	if (!Mesh.IsValid() || !Mesh->IsDrawable() || !Material)
	{
		return;
	}
	const FMaterialRenderProxy* MaterialProxy = Material->GetRenderProxy();
	if (!MaterialProxy)
	{
		return;
	}

	FMeshBatch MeshBatch;
	InitMeshBatch(MeshBatch, *Mesh, MaterialProxy);

	// The runtime virtual texture clones (the base proxy filled RuntimeVirtualTextureMaterialTypes from the region
	// component's textures): the terrain material, written to the virtual texture only
	PDI->ReserveMemoryForMeshes(1 + RuntimeVirtualTextureMaterialTypes.Num());
	for (ERuntimeVirtualTextureMaterialType RVTMaterialType : RuntimeVirtualTextureMaterialTypes)
	{
		FMeshBatch VirtualMeshBatch = MeshBatch;
		VirtualMeshBatch.CastShadow = false;
		VirtualMeshBatch.bUseAsOccluder = false;
		VirtualMeshBatch.bUseForDepthPass = false;
		VirtualMeshBatch.bUseForMaterial = false;
		VirtualMeshBatch.bDitheredLODTransition = false;
		VirtualMeshBatch.bRenderToVirtualTexture = true;
		VirtualMeshBatch.RuntimeVirtualTextureMaterialType = (uint32)RVTMaterialType;
		PDI->DrawMesh(VirtualMeshBatch, FLT_MAX);
	}
	PDI->DrawMesh(MeshBatch, FLT_MAX);
}

FPrimitiveViewRelevance FVoxelChunkSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View);
	Result.bShadowRelevance = IsShadowCast(View);
	// The wireframe view mode draws the mesh dynamically with the wireframe material
	const bool bWireframe = AllowDebugViewmodes() && View->Family->EngineShowFlags.Wireframe;
	// A fade draws dynamically too (its dithered halves): the scene skips the cached static batches while it runs
	const bool bDynamic = bWireframe || bFading.load(std::memory_order_relaxed) || CVarVoxelChunkPrimitivesDynamic.GetValueOnRenderThread() != 0;
	Result.bStaticRelevance = !bDynamic;
	Result.bDynamicRelevance = bDynamic;
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	Result.bTranslucentSelfShadow = false;
	Result.bVelocityRelevance = false;
	// SetPrimitiveViewRelevance overwrites the bits: the fade material's relevance (masked) joins the terrain's first
	FMaterialRelevance Combined = MaterialRelevance;
	if (bFading.load(std::memory_order_relaxed))
	{
		Combined |= FadeMaterialRelevance;
	}
	Combined.SetPrimitiveViewRelevance(Result);
	return Result;
}

void FVoxelChunkSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_VoxelChunkSceneProxy_GetDynamicMeshElements);

	FScopeLock Lock(&StateLock);
	const bool bCurrent = Mesh.IsValid() && Mesh->IsDrawable();
	const bool bPrevious = Previous.IsValid() && Previous->IsDrawable();
	const FMaterialRenderProxy* TerrainMaterial = Material ? Material->GetRenderProxy() : nullptr;
	if ((!bCurrent && !bPrevious) || !TerrainMaterial)
	{
		return;
	}

	// The wireframe view mode: the current mesh with the wireframe material
	const bool bWireframe = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;
	FColoredMaterialRenderProxy* WireframeMaterial = nullptr;
	if (bWireframe)
	{
		WireframeMaterial = new FColoredMaterialRenderProxy(
			GEngine->WireframeMaterial ? GEngine->WireframeMaterial->GetRenderProxy() : nullptr, FLinearColor(0, 0.5f, 1.f));
		Collector.RegisterOneFrameMaterialProxy(WireframeMaterial);
	}

	// What the fade draws (as the region proxy decided it per mesh): a crossfade, the previous mesh dithering out under the
	// current one dithering in, both pooled MIDs on the same alpha with complementary masks; an appearance, the current
	// one alone dithering in from nothing; a seam bucket fading out to nothing, the previous one alone dithering out
	const bool bFadeNow = !bWireframe && bFading.load(std::memory_order_relaxed);
	const bool bCrossfade = bFadeNow && bCurrent && bPrevious && Fade.FadeInProxy && Fade.FadeOutProxy;
	const bool bAppear = bFadeNow && !bCrossfade && bCurrent && Fade.bAppear && Fade.FadeInProxy;
	const bool bFadeOutOnly = bFadeNow && !bCurrent && bPrevious && Fade.FadeOutProxy;

	FVoxelBatchCounters::FCounts Counts;
	const bool bShadowGather = Views.Num() > 0 && Views[0]->GetDynamicMeshElementsShadowCullFrustum() != nullptr;
	const bool bFarShadowGather = bShadowGather && (Views[0]->DrawDynamicFlags & EDrawDynamicFlags::FarShadowCascade) != 0;
	auto Emit = [&](int32 ViewIndex, const FVoxelChunkMesh& InMesh, const FMaterialRenderProxy* DrawMaterial)
	{
		FMeshBatch& MeshBatch = Collector.AllocateMesh();
		InitMeshBatch(MeshBatch, InMesh, DrawMaterial);
		MeshBatch.bWireframe = bWireframe;
		Collector.AddMesh(ViewIndex, MeshBatch);
		++Counts.Drawn;
	};

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		if (!(VisibilityMap & (1 << ViewIndex)))
		{
			continue;
		}
		if (bWireframe)
		{
			if (bCurrent)
			{
				Emit(ViewIndex, *Mesh, WireframeMaterial);
			}
			continue;
		}

		// A far cascade draws the current mesh alone with the opaque material: its texels are metres across, where a
		// dither mask is noise and the outgoing half one more batch (the region proxy's rule, D-VP3)
		if (bFarShadowGather)
		{
			if (bCurrent)
			{
				Emit(ViewIndex, *Mesh, TerrainMaterial);
			}
			continue;
		}

		if (bCrossfade || bFadeOutOnly)
		{
			Emit(ViewIndex, *Previous, Fade.FadeOutProxy);
		}
		if (bCurrent)
		{
			Emit(ViewIndex, *Mesh, (bCrossfade || bAppear) ? Fade.FadeInProxy : TerrainMaterial);

			// The runtime virtual texture always takes the current mesh with the opaque material (a dither mask would punch
			// transient holes into the cached pages); a shadow pass draws none
			if (!bShadowGather)
			{
				for (ERuntimeVirtualTextureMaterialType RVTMaterialType : RuntimeVirtualTextureMaterialTypes)
				{
					FMeshBatch& VirtualMeshBatch = Collector.AllocateMesh();
					InitMeshBatch(VirtualMeshBatch, *Mesh, TerrainMaterial);
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
		}
	}

	Counts.ShadowDrawn = bShadowGather ? Counts.Drawn : 0;
	Counts.FarShadowDrawn = bFarShadowGather ? Counts.Drawn : 0;
	Counts.MaxPerCall = Counts.Drawn;
	FVoxelBatchCounters::Add(Counts);
}

void FVoxelChunkSceneProxy::SetFadeAlpha_RenderThread(float Alpha)
{
	FScopeLock Lock(&StateLock);
	Fade.FadeAlpha = FMath::Clamp(Alpha, 0.0f, 1.0f);
}

void FVoxelChunkSceneProxy::ReleaseFadeHold_RenderThread()
{
	FScopeLock Lock(&StateLock);
	Fade.bHeld = false;
}

void FVoxelChunkSceneProxy::ClearFade_RenderThread()
{
	FScopeLock Lock(&StateLock);
	// From the next frame the scene draws the cached static batches (cached when the primitive was added)
	bFading.store(false, std::memory_order_relaxed);
	Fade = FVoxelChunkFadeState();
	Previous.Reset();
}

void FVoxelChunkSceneProxy::GetDrawInfo_RenderThread(FVoxelChunkDrawInfo& OutInfo) const
{
	check(IsInRenderingThread());

	FScopeLock Lock(&StateLock);
	if (Mesh.IsValid() && Mesh->IsDrawable())
	{
		OutInfo.CurrentTriangles = Mesh->RenderData.IndexCount / 3;
		OutInfo.LODLevel = Mesh->LODLevel;
		OutInfo.bVisible = true;
		OutInfo.VertexFactory = Mesh->VertexFactory.Get();
		OutInfo.VertexStride = Mesh->RenderData.VertexStride;
	}
	OutInfo.PreviousTriangles = Previous.IsValid() ? Previous->RenderData.IndexCount / 3 : 0;
	if (bFading.load(std::memory_order_relaxed))
	{
		OutInfo.bHasFadeState = true;
		OutInfo.FadeAlpha = Fade.FadeAlpha;
		OutInfo.bHeld = Fade.bHeld;
		OutInfo.bAppear = Fade.bAppear;
		// As GetDynamicMeshElements decides it (outside wireframe)
		const bool bCurrent = Mesh.IsValid() && Mesh->IsDrawable();
		OutInfo.bCrossfading = bCurrent && Previous.IsValid() && Previous->IsDrawable() && Fade.FadeInProxy && Fade.FadeOutProxy;
		OutInfo.bAppearing = !OutInfo.bCrossfading && bCurrent && Fade.bAppear && Fade.FadeInProxy;
	}
	OutInfo.bRayTracing = bRayTracing;
	const FCardRepresentationData* Cards = GetMeshCardRepresentation();
	OutInfo.LumenCards = Cards ? Cards->MeshCardsBuildData.CardBuildData.Num() : 0;
	OutInfo.bVisibleInLumenScene = IsVisibleInLumenScene();
#if RHI_RAYTRACING
	if (Mesh.IsValid() && Mesh->RayTracingGeometry.IsValid())
	{
		OutInfo.RayTracingTriangles = Mesh->RayTracingGeometry->GetInitializer().TotalPrimitiveCount;
	}
#endif
}

const FCardRepresentationData* FVoxelChunkSceneProxy::GetMeshCardRepresentation() const
{
	return bLumenCards ? Mesh->CardRepresentation.Get() : nullptr;
}

int64 FVoxelChunkSceneProxy::GetMeshBufferBytes_RenderThread() const
{
	FScopeLock Lock(&StateLock);
	int64 Bytes = (Mesh.IsValid() && Mesh->RenderData.HasValidBuffers()) ? static_cast<int64>(Mesh->RenderData.GetGPUMemoryUsage()) : 0;
	Bytes += (Previous.IsValid() && Previous->RenderData.HasValidBuffers()) ? static_cast<int64>(Previous->RenderData.GetGPUMemoryUsage()) : 0;
	return Bytes;
}

uint64 FVoxelChunkSceneProxy::GetRayTracingBytes_RenderThread() const
{
#if RHI_RAYTRACING
	FScopeLock Lock(&StateLock);
	return (Mesh.IsValid() && Mesh->RayTracingGeometry.IsValid()) ? Mesh->RayTracingBytes : 0;
#else
	return 0;
#endif
}

#if RHI_RAYTRACING
ERayTracingPrimitiveFlags FVoxelChunkSceneProxy::GetRayTracingPrimitiveFlags()
{
	ERayTracingPrimitiveFlags Flags = FPrimitiveSceneProxy::GetRayTracingPrimitiveFlags();
	if (EnumHasAnyFlags(Flags, ERayTracingPrimitiveFlags::Dynamic))
	{
		Flags = (Flags & ~ERayTracingPrimitiveFlags::Dynamic) | ERayTracingPrimitiveFlags::CacheInstances;
	}
	return Flags;
}

void FVoxelChunkSceneProxy::GetStaticRayTracingInstances(FStaticRayTracingInstances& OutRayTracingInstances)
{
	FStaticRayTracingInstancesLOD& LOD = OutRayTracingInstances.LODs.AddDefaulted_GetRef();
	const FRayTracingGeometry* Geometry = Mesh.IsValid() ? Mesh->RayTracingGeometry.Get() : nullptr;
	const FMaterialRenderProxy* MaterialProxy = Material ? Material->GetRenderProxy() : nullptr;
	if (!Geometry || !Geometry->IsValid() || Geometry->IsEvicted() || !MaterialProxy || !Mesh->IsDrawable())
	{
		return;
	}

	FRayTracingInstance& Instance = LOD.Instances.AddDefaulted_GetRef();
	Instance.Geometry = Geometry;
	Instance.NumTransforms = 1;
	FMeshBatch& MeshBatch = Instance.Materials.AddDefaulted_GetRef();
	InitMeshBatch(MeshBatch, *Mesh, MaterialProxy);
	// Ray tracing needs no flip for a mirroring transform (and the primitive's transform is a translation)
	MeshBatch.ReverseCulling = false;
	MeshBatch.bCanApplyViewModeOverrides = false;
	MeshBatch.CastRayTracedShadow = true;
}

RayTracing::FGeometryGroupHandle FVoxelChunkSceneProxy::GetRayTracingGeometryGroupHandle() const
{
	return Mesh.IsValid() ? Mesh->RayTracingGroupHandle : RayTracing::FGeometryGroupHandle();
}
#endif
