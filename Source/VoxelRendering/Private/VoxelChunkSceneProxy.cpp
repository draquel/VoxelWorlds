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
#if RHI_RAYTRACING
#include "RayTracingInstance.h"
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
	const FVector& InOrigin, const TSharedRef<std::atomic<int64>, ESPMode::ThreadSafe>& InByteCounter)
	: LODLevel(InLODLevel)
	, LocalBounds(InLocalBounds)
	, Origin(InOrigin)
	, PendingVertices(MoveTemp(InVertices))
	, PendingIndices(MoveTemp(InIndices))
	, NumTriangles(static_cast<uint32>(PendingIndices.Num() / 3))
	, ByteCounter(InByteCounter)
{
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
		RayTracingGeometry = MakeUnique<FRayTracingGeometry>();
		RayTracingBytes = InitVoxelRayTracingGeometry(RHICmdList, RenderData, TEXT("VoxelChunkPrimitiveRayTracing"), *RayTracingGeometry);
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
		Mesh.Reset();
	}
}

void EnqueueDrainChunkMeshReleases()
{
	if (GVoxelChunkMeshReleases.IsEmpty())
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
	, Material(Init.Material)
	, MaterialRelevance(Init.MaterialRelevance)
	, FeatureLevel(Init.FeatureLevel)
	, bRayTracing(Init.bRayTracing && Mesh.IsValid())
	, bPositionOnly(Init.bPositionOnly)
	, bRadialFaceUp(Init.bRadialFaceUp)
	, FaceUpCenter(Init.FaceUpCenter)
	, bSeam(Init.bSeam)
{
	// The scene's record of this primitive carries the id the description gave it (FPrimitiveSceneInfo's ComponentId)
	SceneData.PrimitiveSceneId = Desc.ComponentId;
	SceneData.SceneProxy = this;

	if (Material == nullptr)
	{
		Material = UMaterial::GetDefaultMaterial(MD_Surface);
		MaterialRelevance = Material->GetRelevance(GetFeatureLevelShaderPlatform(FeatureLevel));
	}

	// As the region proxy: no material verification, no static shadowing, no distance fields; the runtime virtual textures
	// the description named are written by the static batch's clones
	bVerifyUsedMaterials = false;
	bCastStaticShadow = false;
	bAffectDynamicIndirectLighting = false;
	bAffectDistanceFieldLighting = false;
	bSupportsRuntimeVirtualTexture = true;

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
	}
	else if (Mesh.IsValid())
	{
		GVoxelChunkMeshReleases.Enqueue(MoveTemp(Mesh));
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

#if RHI_RAYTRACING
	if (bRayTracing && Mesh.IsValid() && Mesh->IsDrawable())
	{
		InitMeshBatch(RayTracingMeshBatch, *Mesh, nullptr);
		// Ray tracing needs no flip for a mirroring transform (and the primitive's transform is a translation)
		RayTracingMeshBatch.ReverseCulling = false;
		RayTracingMeshBatch.bCanApplyViewModeOverrides = false;
		RayTracingMeshBatch.CastRayTracedShadow = true;
	}
#endif
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
	const bool bDynamic = bWireframe || CVarVoxelChunkPrimitivesDynamic.GetValueOnRenderThread() != 0;
	Result.bStaticRelevance = !bDynamic;
	Result.bDynamicRelevance = bDynamic;
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	Result.bTranslucentSelfShadow = false;
	Result.bVelocityRelevance = false;
	MaterialRelevance.SetPrimitiveViewRelevance(Result);
	return Result;
}

void FVoxelChunkSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_VoxelChunkSceneProxy_GetDynamicMeshElements);

	FScopeLock Lock(&StateLock);
	if (!Mesh.IsValid() || !Mesh->IsDrawable())
	{
		return;
	}

	// The wireframe view mode (GetViewRelevance): the current mesh with the wireframe material
	const bool bWireframe = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;
	const FMaterialRenderProxy* DrawMaterial = Material ? Material->GetRenderProxy() : nullptr;
	if (bWireframe)
	{
		FColoredMaterialRenderProxy* WireframeMaterial = new FColoredMaterialRenderProxy(
			GEngine->WireframeMaterial ? GEngine->WireframeMaterial->GetRenderProxy() : nullptr, FLinearColor(0, 0.5f, 1.f));
		Collector.RegisterOneFrameMaterialProxy(WireframeMaterial);
		DrawMaterial = WireframeMaterial;
	}

	int32 Drawn = 0;
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		if (!(VisibilityMap & (1 << ViewIndex)))
		{
			continue;
		}
		FMeshBatch& MeshBatch = Collector.AllocateMesh();
		InitMeshBatch(MeshBatch, *Mesh, DrawMaterial);
		MeshBatch.bWireframe = bWireframe;
		Collector.AddMesh(ViewIndex, MeshBatch);
		++Drawn;
	}

	FVoxelBatchCounters::FCounts Counts;
	Counts.Drawn = Drawn;
	Counts.MaxPerCall = Drawn;
	FVoxelBatchCounters::Add(Counts);
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
	OutInfo.bRayTracing = bRayTracing;
#if RHI_RAYTRACING
	if (Mesh.IsValid() && Mesh->RayTracingGeometry.IsValid())
	{
		OutInfo.RayTracingTriangles = Mesh->RayTracingGeometry->GetInitializer().TotalPrimitiveCount;
	}
#endif
}

int64 FVoxelChunkSceneProxy::GetMeshBufferBytes_RenderThread() const
{
	FScopeLock Lock(&StateLock);
	return (Mesh.IsValid() && Mesh->RenderData.HasValidBuffers()) ? static_cast<int64>(Mesh->RenderData.GetGPUMemoryUsage()) : 0;
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
void FVoxelChunkSceneProxy::GetDynamicRayTracingInstances(FRayTracingInstanceCollector& Collector)
{
	const FMaterialRenderProxy* MaterialProxy = (bRayTracing && Material) ? Material->GetRenderProxy() : nullptr;
	FScopeLock Lock(&StateLock);
	if (!MaterialProxy || !Mesh.IsValid() || !Mesh->IsDrawable())
	{
		return;
	}
	const FRayTracingGeometry* Geometry = Mesh->RayTracingGeometry.Get();
	if (!Geometry || !Geometry->IsValid() || Geometry->IsEvicted())
	{
		return;
	}
	// The engine skips (with an ensure) an instance whose acceleration structure is still building: it joins next frame
	if (Geometry->GetRequiresBuild() || Geometry->HasPendingBuildRequest())
	{
		FVoxelRayTracingCounters::AddGathered(0, 1);
		return;
	}

	RayTracingTransform = GetLocalToWorld();
	RayTracingMeshBatch.MaterialRenderProxy = MaterialProxy;
	FRayTracingInstance Instance;
	Instance.Geometry = Geometry;
	Instance.MaterialsView = MakeArrayView(&RayTracingMeshBatch, 1);
	Instance.InstanceTransformsView = MakeArrayView(&RayTracingTransform, 1);
	Instance.NumTransforms = 1;

	// Left out of a view whose Lumen max trace distance (times the scale) does not reach the mesh's box
	const TConstArrayView<const FSceneView*> Views = Collector.GetViews();
	const uint32 VisibilityMap = Collector.GetVisibilityMap();
	const double TraceDistanceScale = GetVoxelRayTracingTraceDistanceScale();
	const FBox WorldBox = Mesh->LocalBounds.ShiftBy(Mesh->Origin);
	int64 Emitted = 0;
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		if (!(VisibilityMap & (1u << ViewIndex)))
		{
			continue;
		}
		const double Distance = TraceDistanceScale * Views[ViewIndex]->FinalPostProcessSettings.LumenMaxTraceDistance;
		if (Distance > 0.0 && WorldBox.ComputeSquaredDistanceToPoint(Views[ViewIndex]->ViewMatrices.GetViewOrigin()) > Distance * Distance)
		{
			continue;
		}
		Collector.AddRayTracingInstance(ViewIndex, Instance);
		++Emitted;
	}
	FVoxelRayTracingCounters::AddGathered(Emitted, 0);
}
#endif
