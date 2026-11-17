// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelChunkNaniteSceneProxy.h"
#include "VoxelNaniteEncoder.h"
#include "VoxelRendering.h"
#include "VoxelRayTracingCounters.h"
#include "Rendering/NaniteResources.h"
#include "PrimitiveSceneProxyDesc.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialRenderProxy.h"
#include "MeshCardRepresentation.h"
#include "MeshBatch.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "HAL/IConsoleManager.h"
#include "Tasks/Task.h"
#include <atomic>
#if RHI_RAYTRACING
#include "RayTracingInstance.h"
#endif

namespace
{
	TAutoConsoleVariable<int32> CVarVoxelNanite(
		TEXT("voxel.Render.Nanite"),
		0,
		TEXT("Runtime Nanite spike (A/B): 1 = chunk meshes and seam buckets are encoded as Nanite resources and drawn through\n")
		TEXT("Nanite (FVoxelChunkNaniteSceneProxy); a fading mesh is drawn as before and swapped to Nanite when its fade ends.\n")
		TEXT("Needs voxel.Render.ChunkPrimitives 1 and a platform with Nanite. Read when a mesh is submitted: a change re-meshes\n")
		TEXT("the terrain. 0 = off."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarVoxelNaniteValidate(
		TEXT("voxel.Render.Nanite.Validate"),
		0,
		TEXT("Runtime Nanite spike (diagnostic): 1 = every Nanite encode is decoded on the CPU and compared with its input before\n")
		TEXT("it reaches the GPU; a mesh that fails draws without Nanite and counts in voxel.Render.NaniteStats. 0 = off."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarVoxelNaniteReuse(
		TEXT("voxel.Render.Nanite.Reuse"),
		1,
		TEXT("Runtime Nanite spike (tuning): 1 = a triangle's corner refers back to a vertex its cluster holds (5-bit references);\n")
		TEXT("0 = every triangle stores its three vertices. Read at each encode."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarVoxelNaniteAsync(
		TEXT("voxel.Render.Nanite.Async"),
		1,
		TEXT("Runtime Nanite spike (A/B): 1 = a chunk mesh is encoded on a task while it draws without Nanite, and swaps to its\n")
		TEXT("Nanite primitive when the encode is done; 0 = encoded on the game thread at submit. Read at each submit."),
		ECVF_Default);

	TAutoConsoleVariable<FString> CVarVoxelNaniteMaterial(
		TEXT("voxel.Render.Nanite.Material"),
		TEXT(""),
		TEXT("Runtime Nanite spike (A/B): the material Nanite primitives draw with, as an object path (an opaque copy of the\n")
		TEXT("terrain master), given the terrain material's parameters. Empty = the terrain material itself. Read when a Nanite\n")
		TEXT("primitive is created."),
		ECVF_Default);

	std::atomic<int64> GLiveResources{ 0 };
	std::atomic<int64> GLivePages{ 0 };
	std::atomic<int64> GLiveGpuBytes{ 0 };
	std::atomic<int64> GEncodes{ 0 };
	std::atomic<int64> GEncodeMicroseconds{ 0 };
	std::atomic<int64> GMaxEncodeMicroseconds{ 0 };
	std::atomic<int64> GEncodedTriangles{ 0 };
	std::atomic<int64> GFailures{ 0 };
	std::atomic<int64> GPrimitives{ 0 };
	std::atomic<int64> GFadeSwaps{ 0 };

	FAutoConsoleCommand CmdVoxelNaniteStats(
		TEXT("voxel.Render.NaniteStats"),
		TEXT("Runtime Nanite spike (diagnostic): the chunk meshes' Nanite resources live (root pages, GPU bytes), the encodes\n")
		TEXT("since the start or the last reset (count, total and longest time, triangles), failures, primitives created and\n")
		TEXT("fades swapped to Nanite. Arg: reset (clears the encode counts)."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() > 0 && Args[0].Equals(TEXT("reset"), ESearchCase::IgnoreCase))
			{
				FVoxelNaniteCounters::ResetEncodes();
			}
			const FVoxelNaniteCounters::FCounts Counts = FVoxelNaniteCounters::Get();
			UE_LOG(LogVoxelRendering, Display,
				TEXT("voxel.Render.NaniteStats: %s; live %lld resources, %lld root pages (%.1f MB of pool), %.1f MB of page data; ")
				TEXT("%lld encodes, %lld triangles, %.2f ms mean, %.2f ms longest, %.3f us a triangle; %lld failures; %lld primitives, %lld fade swaps"),
				IsVoxelNaniteWanted() ? TEXT("on") : TEXT("off"), Counts.LiveResources, Counts.LivePages,
				Counts.LivePages * double(NANITE_ROOT_PAGE_GPU_SIZE) / (1024.0 * 1024.0), Counts.LiveGpuBytes / (1024.0 * 1024.0),
				Counts.Encodes, Counts.EncodedTriangles, Counts.Encodes > 0 ? Counts.EncodeSeconds * 1000.0 / Counts.Encodes : 0.0,
				Counts.MaxEncodeSeconds * 1000.0, Counts.EncodedTriangles > 0 ? Counts.EncodeSeconds * 1e6 / Counts.EncodedTriangles : 0.0,
				Counts.Failures, Counts.Primitives, Counts.FadeSwaps);
		}));
}

// ==================== FVoxelNaniteCounters ====================

FVoxelNaniteCounters::FCounts FVoxelNaniteCounters::Get()
{
	FCounts Counts;
	Counts.LiveResources = GLiveResources.load(std::memory_order_relaxed);
	Counts.LivePages = GLivePages.load(std::memory_order_relaxed);
	Counts.LiveGpuBytes = GLiveGpuBytes.load(std::memory_order_relaxed);
	Counts.Encodes = GEncodes.load(std::memory_order_relaxed);
	Counts.EncodeSeconds = GEncodeMicroseconds.load(std::memory_order_relaxed) * 1e-6;
	Counts.MaxEncodeSeconds = GMaxEncodeMicroseconds.load(std::memory_order_relaxed) * 1e-6;
	Counts.EncodedTriangles = GEncodedTriangles.load(std::memory_order_relaxed);
	Counts.Failures = GFailures.load(std::memory_order_relaxed);
	Counts.Primitives = GPrimitives.load(std::memory_order_relaxed);
	Counts.FadeSwaps = GFadeSwaps.load(std::memory_order_relaxed);
	return Counts;
}

void FVoxelNaniteCounters::ResetEncodes()
{
	GEncodes = 0;
	GEncodeMicroseconds = 0;
	GMaxEncodeMicroseconds = 0;
	GEncodedTriangles = 0;
	GFailures = 0;
	GPrimitives = 0;
	GFadeSwaps = 0;
}

void FVoxelNaniteCounters::AddEncode(double Seconds, int64 Triangles, bool bFailed)
{
	const int64 Microseconds = int64(Seconds * 1e6);
	GEncodes.fetch_add(1, std::memory_order_relaxed);
	GEncodeMicroseconds.fetch_add(Microseconds, std::memory_order_relaxed);
	GEncodedTriangles.fetch_add(Triangles, std::memory_order_relaxed);
	int64 Max = GMaxEncodeMicroseconds.load(std::memory_order_relaxed);
	while (Microseconds > Max && !GMaxEncodeMicroseconds.compare_exchange_weak(Max, Microseconds, std::memory_order_relaxed))
	{
	}
	if (bFailed)
	{
		GFailures.fetch_add(1, std::memory_order_relaxed);
	}
}

void FVoxelNaniteCounters::AddLive(int64 Resources, int64 Pages, int64 GpuBytes)
{
	GLiveResources.fetch_add(Resources, std::memory_order_relaxed);
	GLivePages.fetch_add(Pages, std::memory_order_relaxed);
	GLiveGpuBytes.fetch_add(GpuBytes, std::memory_order_relaxed);
}

void FVoxelNaniteCounters::AddPrimitive()
{
	GPrimitives.fetch_add(1, std::memory_order_relaxed);
}

void FVoxelNaniteCounters::AddFadeSwap()
{
	GFadeSwaps.fetch_add(1, std::memory_order_relaxed);
}

bool IsVoxelNaniteWanted()
{
	return CVarVoxelNanite.GetValueOnAnyThread() != 0 && DoesPlatformSupportNanite(GMaxRHIShaderPlatform);
}

bool IsVoxelNaniteValidationWanted()
{
	return CVarVoxelNaniteValidate.GetValueOnAnyThread() != 0;
}

bool IsVoxelNaniteAsyncWanted()
{
	return CVarVoxelNaniteAsync.GetValueOnAnyThread() != 0;
}

void FVoxelNaniteEncodeJob::Run()
{
	// The vertices as InitResources_RenderThread converts them (the same face types, tangents, UVs and colours)
	TArray<FVoxelLocalVertex> Converted;
	Converted.SetNumUninitialized(Vertices.Num());
	for (int32 i = 0; i < Vertices.Num(); ++i)
	{
		const FVoxelVertex& Vertex = Vertices[i];
		Converted[i] = bRadialFaceUp
			? FVoxelLocalVertex::FromVoxelVertex(Vertex, (Vertex.Position - LocalFaceUpCenter).GetSafeNormal())
			: FVoxelLocalVertex::FromVoxelVertex(Vertex);
	}
	Result = EncodeVoxelNaniteMesh(Converted, Indices, Settings, &Stats);
	if (Result.IsValid() && bValidate)
	{
		const FVoxelNaniteValidation Validation = ValidateVoxelNaniteMesh(*Result, Converted, Indices, Settings);
		if (!Validation.bValid)
		{
			Error = Validation.Error;
			Result.Reset();
		}
	}
	Vertices.Empty();
	Indices.Empty();
	bDone.store(true, std::memory_order_release);
}

// ==================== FVoxelChunkMesh: the Nanite resource ====================

namespace
{
	TSharedPtr<FVoxelNaniteEncodeJob, ESPMode::ThreadSafe> MakeNaniteJob(const TArray<FVoxelVertex>& Vertices, const TArray<uint32>& Indices,
		const FVector& Origin, bool bRadialFaceUp, const FVector& FaceUpCenter)
	{
		TSharedPtr<FVoxelNaniteEncodeJob, ESPMode::ThreadSafe> Job = MakeShared<FVoxelNaniteEncodeJob, ESPMode::ThreadSafe>();
		Job->Vertices = Vertices;
		Job->Indices = Indices;
		Job->bRadialFaceUp = bRadialFaceUp;
		Job->LocalFaceUpCenter = FVector3f(FaceUpCenter - Origin);
		Job->Settings.bVertexReuse = CVarVoxelNaniteReuse.GetValueOnGameThread() != 0;
		Job->bValidate = IsVoxelNaniteValidationWanted();
		return Job;
	}
}

bool FVoxelChunkMesh::StartNaniteEncode_GameThread(bool bRadialFaceUp, const FVector& FaceUpCenter)
{
	check(IsInGameThread());
	if (NaniteResources.IsValid() || NaniteJob.IsValid() || PendingVertices.Num() == 0 || PendingIndices.Num() < 3)
	{
		return false;
	}
	// Copies: the render thread consumes the mesh's own arrays when its first primitive is added
	NaniteJob = MakeNaniteJob(PendingVertices, PendingIndices, Origin, bRadialFaceUp, FaceUpCenter);
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [Job = NaniteJob]() { Job->Run(); });
	return true;
}

bool FVoxelChunkMesh::IsNaniteEncodeDone() const
{
	return NaniteJob.IsValid() && NaniteJob->bDone.load(std::memory_order_acquire);
}

bool FVoxelChunkMesh::FinishNaniteEncode_GameThread(const UObject* Owner)
{
	check(IsInGameThread());
	if (!IsNaniteEncodeDone())
	{
		return NaniteResources.IsValid();
	}
	const TSharedPtr<FVoxelNaniteEncodeJob, ESPMode::ThreadSafe> Job = MoveTemp(NaniteJob);
	const bool bFailed = !Job->Result.IsValid();
	if (!Job->Error.IsEmpty())
	{
		UE_LOG(LogVoxelRendering, Error, TEXT("Nanite encode of a chunk mesh (LOD %d, %d triangles) does not decode to its input: %s"),
			LODLevel, Job->Stats.NumTriangles, *Job->Error);
	}
	FVoxelNaniteCounters::AddEncode(Job->Stats.EncodeSeconds, Job->Stats.NumTriangles, bFailed);
	if (bFailed || !Owner)
	{
		return false;
	}
	NaniteResources = MoveTemp(Job->Result);
	NanitePages = Job->Stats.NumPages;
	NaniteGpuBytes = Job->Stats.GpuBytes;
	// The streaming manager takes it on the render thread (before any primitive over it is added: the add is enqueued later)
	NaniteResources->InitResources(Owner);
	FVoxelNaniteCounters::AddLive(1, NanitePages, NaniteGpuBytes);
	return true;
}

bool FVoxelChunkMesh::BuildNanite_GameThread(const UObject* Owner, bool bRadialFaceUp, const FVector& FaceUpCenter)
{
	check(IsInGameThread());
	if (NaniteResources.IsValid() || NaniteJob.IsValid() || PendingVertices.Num() == 0 || PendingIndices.Num() < 3 || !Owner)
	{
		return NaniteResources.IsValid();
	}
	NaniteJob = MakeNaniteJob(PendingVertices, PendingIndices, Origin, bRadialFaceUp, FaceUpCenter);
	NaniteJob->Run();
	return FinishNaniteEncode_GameThread(Owner);
}

// ==================== FVoxelChunkNaniteSceneProxy ====================

FVoxelChunkNaniteSceneProxy::FVoxelChunkNaniteSceneProxy(const FPrimitiveSceneProxyDesc& Desc, FInit&& Init)
	: Nanite::FSceneProxyBase(Desc)
	, Mesh(MoveTemp(Init.Mesh))
	, Material(Init.Material)
	, FeatureLevel(Init.FeatureLevel)
	, bRayTracing(Init.bRayTracing && Mesh.IsValid())
	, bPositionOnly(Init.bPositionOnly)
	, bRadialFaceUp(Init.bRadialFaceUp)
	, FaceUpCenter(Init.FaceUpCenter)
	, bSeam(Init.bSeam)
	, bLumenCards(Init.bLumenCards && Mesh.IsValid() && Mesh->CardRepresentation.IsValid())
{
	check(Mesh.IsValid() && Mesh->HasNanite());
	SceneData.PrimitiveSceneId = Desc.ComponentId;
	SceneData.SceneProxy = this;

	// A material Nanite cannot draw (not opaque or masked, or not compiled for Nanite) is replaced by the default one
	if (!Material || !IsOpaqueOrMaskedBlendMode(*Material) || !Material->CheckMaterialUsage_Concurrent(MATUSAGE_Nanite))
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("FVoxelChunkNaniteSceneProxy: %s cannot be drawn by Nanite; the default material draws instead"),
			Material ? *Material->GetName() : TEXT("no material"));
		Material = UMaterial::GetDefaultMaterial(MD_Surface);
	}

	bVerifyUsedMaterials = false;
	bCastStaticShadow = false;
	bAffectDistanceFieldLighting = false;
	bSupportsDistanceFieldRepresentation = false;
	// Nanite always uses GPUScene: the static elements need no primitive uniform buffer
	bStaticElementsAlwaysUseProxyPrimitiveUniformBuffer = true;
	bVFRequiresPrimitiveUniformBuffer = false;
	bOpaqueOrMasked = true;
	EnableGPUSceneSupportFlags();

	MaterialSections.SetNum(1);
	FMaterialSection& Section = MaterialSections[0];
	Section.MaterialIndex = 0;
	Section.bCastShadow = true;
	Section.bHidden = false;
	Section.ShadingMaterialProxy = Material->GetRenderProxy();
	MaterialMaxIndex = 0;
	// The section's relevance, raster material (the default one unless the material is masked or programmable) and
	// displacement
	OnMaterialsUpdated();

	bAffectDynamicIndirectLighting = bLumenCards;
	UpdateVisibleInLumenScene();

	FVoxelNaniteCounters::AddPrimitive();
#if RHI_RAYTRACING
	if (bRayTracing)
	{
		FVoxelRayTracingCounters::AddProxy(1);
	}
#endif
}

FVoxelChunkNaniteSceneProxy::~FVoxelChunkNaniteSceneProxy()
{
	// As FVoxelChunkSceneProxy: the mesh's last reference goes on the render thread
	if (Mesh.IsValid())
	{
		if (IsInRenderingThread())
		{
			Mesh.Reset();
		}
		else
		{
			ReleaseChunkMeshOnRenderThread(MoveTemp(Mesh));
		}
	}
#if RHI_RAYTRACING
	if (bRayTracing)
	{
		FVoxelRayTracingCounters::AddProxy(-1);
	}
#endif
}

SIZE_T FVoxelChunkNaniteSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

uint32 FVoxelChunkNaniteSceneProxy::GetMemoryFootprint() const
{
	return sizeof(*this) + GetAllocatedSize();
}

void FVoxelChunkNaniteSceneProxy::CreateRenderThreadResources(FRHICommandListBase& RHICmdList)
{
	check(Mesh->NaniteResources->RuntimeResourceID != INDEX_NONE && Mesh->NaniteResources->HierarchyOffset != INDEX_NONE);
	// The vertex and index buffers for the ray tracing geometry (D155) and its hit shading
	Mesh->InitResources_RenderThread(RHICmdList, bPositionOnly, bRadialFaceUp, FaceUpCenter, bRayTracing, FeatureLevel);
}

FPrimitiveViewRelevance FVoxelChunkNaniteSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	// As Nanite::FSceneProxy's optimized relevance: static relevance, drawn by Nanite's passes
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View) && !!View->Family->EngineShowFlags.NaniteMeshes;
	Result.bShadowRelevance = IsShadowCast(View);
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	Result.bStaticRelevance = true;
	Result.bRenderInMainPass = true;
	CombinedMaterialRelevance.SetPrimitiveViewRelevance(Result);
	Result.bVelocityRelevance = DrawsVelocity();
	return Result;
}

const FCardRepresentationData* FVoxelChunkNaniteSceneProxy::GetMeshCardRepresentation() const
{
	return bLumenCards ? Mesh->CardRepresentation.Get() : nullptr;
}

void FVoxelChunkNaniteSceneProxy::GetDrawInfo_RenderThread(FVoxelChunkDrawInfo& OutInfo) const
{
	check(IsInRenderingThread());
	OutInfo.CurrentTriangles = Mesh->GetNumTriangles();
	OutInfo.LODLevel = Mesh->LODLevel;
	OutInfo.bVisible = OutInfo.CurrentTriangles > 0;
	OutInfo.VertexFactory = Mesh->VertexFactory.Get();
	OutInfo.VertexStride = Mesh->RenderData.VertexStride;
	OutInfo.bRayTracing = bRayTracing;
	const FCardRepresentationData* Cards = GetMeshCardRepresentation();
	OutInfo.LumenCards = Cards ? Cards->MeshCardsBuildData.CardBuildData.Num() : 0;
	OutInfo.bVisibleInLumenScene = IsVisibleInLumenScene();
#if RHI_RAYTRACING
	if (Mesh->RayTracingGeometry.IsValid())
	{
		OutInfo.RayTracingTriangles = Mesh->RayTracingGeometry->GetInitializer().TotalPrimitiveCount;
	}
#endif
}

int64 FVoxelChunkNaniteSceneProxy::GetMeshBufferBytes_RenderThread() const
{
	return Mesh->RenderData.HasValidBuffers() ? static_cast<int64>(Mesh->RenderData.GetGPUMemoryUsage()) : 0;
}

uint64 FVoxelChunkNaniteSceneProxy::GetRayTracingBytes_RenderThread() const
{
#if RHI_RAYTRACING
	return Mesh->RayTracingGeometry.IsValid() ? Mesh->RayTracingBytes : 0;
#else
	return 0;
#endif
}

Nanite::FResourceMeshInfo FVoxelChunkNaniteSceneProxy::GetResourceMeshInfo() const
{
	Nanite::FResourceMeshInfo Info;
	const Nanite::FResources& Resources = *Mesh->NaniteResources;
	Info.NumClusters = Resources.NumClusters;
	Info.NumNodes = Resources.NumHierarchyNodes;
	Info.NumVertices = Resources.NumInputVertices;
	Info.NumTriangles = Resources.NumInputTriangles;
	Info.NumMaterials = 1;
	Info.NumResidentClusters = Resources.NumResidentClusters;
	Info.DebugName = bSeam ? FName(TEXT("VoxelSeamBucket")) : FName(TEXT("VoxelChunk"));
	return Info;
}

Nanite::FResourcePrimitiveInfo FVoxelChunkNaniteSceneProxy::GetResourcePrimitiveInfo() const
{
	return Nanite::FResourcePrimitiveInfo(*Mesh->NaniteResources);
}

#if RHI_RAYTRACING
ERayTracingPrimitiveFlags FVoxelChunkNaniteSceneProxy::GetRayTracingPrimitiveFlags()
{
	// As FVoxelChunkSceneProxy: the scene caches the one instance
	ERayTracingPrimitiveFlags Flags = FPrimitiveSceneProxy::GetRayTracingPrimitiveFlags();
	if (EnumHasAnyFlags(Flags, ERayTracingPrimitiveFlags::Dynamic))
	{
		Flags = (Flags & ~ERayTracingPrimitiveFlags::Dynamic) | ERayTracingPrimitiveFlags::CacheInstances;
	}
	return Flags;
}

void FVoxelChunkNaniteSceneProxy::GetStaticRayTracingInstances(FStaticRayTracingInstances& OutRayTracingInstances)
{
	FStaticRayTracingInstancesLOD& LOD = OutRayTracingInstances.LODs.AddDefaulted_GetRef();
	const FRayTracingGeometry* Geometry = Mesh->RayTracingGeometry.Get();
	const FMaterialRenderProxy* MaterialProxy = Material ? Material->GetRenderProxy() : nullptr;
	if (!Geometry || !Geometry->IsValid() || Geometry->IsEvicted() || !MaterialProxy || !Mesh->IsDrawable())
	{
		return;
	}
	FRayTracingInstance& Instance = LOD.Instances.AddDefaulted_GetRef();
	Instance.Geometry = Geometry;
	Instance.NumTransforms = 1;
	FMeshBatch& MeshBatch = Instance.Materials.AddDefaulted_GetRef();
	MeshBatch.VertexFactory = Mesh->VertexFactory.Get();
	MeshBatch.MaterialRenderProxy = MaterialProxy;
	MeshBatch.ReverseCulling = false;
	MeshBatch.bDisableBackfaceCulling = false;
	MeshBatch.Type = PT_TriangleList;
	MeshBatch.DepthPriorityGroup = SDPG_World;
	MeshBatch.bCanApplyViewModeOverrides = false;
	MeshBatch.CastRayTracedShadow = true;
	MeshBatch.CastShadow = true;
	MeshBatch.bUseForMaterial = true;
	MeshBatch.LODIndex = 0;
	MeshBatch.SegmentIndex = 0;
	FMeshBatchElement& Element = MeshBatch.Elements[0];
	Element.IndexBuffer = Mesh->IndexBuffer.Get();
	Element.FirstIndex = 0;
	Element.NumPrimitives = Mesh->RenderData.IndexCount / 3;
	Element.MinVertexIndex = 0;
	Element.MaxVertexIndex = Mesh->RenderData.VertexCount - 1;
	Element.PrimitiveUniformBuffer = nullptr;
}

RayTracing::FGeometryGroupHandle FVoxelChunkNaniteSceneProxy::GetRayTracingGeometryGroupHandle() const
{
	return Mesh->RayTracingGroupHandle;
}
#endif
