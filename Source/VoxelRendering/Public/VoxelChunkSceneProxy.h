// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveSceneInfoData.h"
#include "VoxelLocalVertexFactory.h"
#include "VoxelVertex.h"
#include "VoxelSceneProxy.h"
#if RHI_RAYTRACING
#include "RayTracingGeometry.h"
#endif

#include <atomic>

struct FPrimitiveSceneProxyDesc;
class UMaterialInterface;

/**
 * The chunk primitives of every region component (G116): alive now, and added and removed since the start (the streaming
 * bench's PrimN and PrimAddN columns). The game thread adds to them when it registers and removes primitives; any thread
 * reads them.
 */
struct VOXELRENDERING_API FVoxelChunkPrimitiveCounters
{
	struct FCounts
	{
		/** Primitives in the scenes now (added minus removed) */
		int64 Live = 0;
		/** Primitives added and removed since the start */
		int64 Added = 0;
		int64 Removed = 0;
	};

	/** @return The counts now */
	static FCounts Get();

	/** Game thread: primitives were handed to the scene to add (+) or remove (-). */
	static void AddAdded(int64 Count);
	static void AddRemoved(int64 Count);
};

/**
 * One uploaded chunk interior or seam bucket mesh (G116): the mesher's vertices until the render thread creates the
 * buffers, then the vertex, index and attribute buffers, the vertex factory and the ray tracing geometry. Vertices are
 * relative to Origin, the chunk's (a seam bucket's owner chunk's) world position, which the primitive drawing it sits at.
 *
 * Shared (ESPMode::ThreadSafe) between the primitive that draws it and, during a crossfade, the next primitive of the same
 * slot, which draws it dithering out; the game thread keeps a reference of its own to decide what a swap retains. The last
 * reference must drop on the render thread (the destructor releases RHI resources): the game thread hands its references
 * over with ReleaseOnRenderThread.
 *
 * The members below Origin are written once, by InitResources_RenderThread, and read on the render thread after it.
 */
class VOXELRENDERING_API FVoxelChunkMesh
{
public:
	/**
	 * Game thread: hold a mesh's vertices and indices for the render thread.
	 *
	 * @param InVertices   The mesher's vertices, relative to InOrigin (moved)
	 * @param InIndices    Triangle indices (moved)
	 * @param InLODLevel   The LOD it was meshed at
	 * @param InLocalBounds Its box relative to InOrigin
	 * @param InOrigin     The chunk's (or the seam owner chunk's) world position
	 * @param InByteCounter The region component's count of its chunk meshes' buffer bytes (outlives the mesh)
	 */
	FVoxelChunkMesh(TArray<FVoxelVertex>&& InVertices, TArray<uint32>&& InIndices, int32 InLODLevel, const FBox& InLocalBounds,
		const FVector& InOrigin, const TSharedRef<std::atomic<int64>, ESPMode::ThreadSafe>& InByteCounter);

	/** Render thread: releases the buffers, the vertex factory and the ray tracing geometry. */
	~FVoxelChunkMesh();

	FVoxelChunkMesh(const FVoxelChunkMesh&) = delete;
	FVoxelChunkMesh& operator=(const FVoxelChunkMesh&) = delete;

	/**
	 * Render thread, once: convert the vertices, create the buffers and the vertex factory, and build the ray tracing
	 * geometry over them; the CPU copies are freed.
	 *
	 * @param RHICmdList    The command list
	 * @param bPositionOnly The vertex stream holds the positions alone (manual vertex fetch, G120)
	 * @param bRadialFaceUp Face types against a planet's radial up (spherical SP-D3)
	 * @param FaceUpCenter  The planet centre (world space)
	 * @param bRayTracing   Build ray tracing geometry (visual polish F3.3)
	 * @param FeatureLevel  The vertex factory's feature level
	 */
	void InitResources_RenderThread(FRHICommandListBase& RHICmdList, bool bPositionOnly, bool bRadialFaceUp, const FVector& FaceUpCenter,
		bool bRayTracing, ERHIFeatureLevel::Type FeatureLevel);

	/** @return Whether the render thread created its buffers and it has triangles to draw */
	bool IsDrawable() const
	{
		return RenderData.HasValidBuffers() && RenderData.IndexCount >= 3 && VertexFactory.IsValid() && IndexBuffer.IsValid();
	}

	/** @return Triangles (on the game thread too: counted at construction) */
	uint32 GetNumTriangles() const { return NumTriangles; }

	/** The LOD it was meshed at, its box relative to Origin, and Origin (fixed at construction, any thread) */
	const int32 LODLevel;
	const FBox LocalBounds;
	const FVector Origin;

	/** The buffers (positions relative to Origin) */
	FVoxelChunkRenderData RenderData;
	TSharedPtr<FVoxelLocalVertexBuffer> VertexBuffer;
	TSharedPtr<FVoxelLocalIndexBuffer> IndexBuffer;
	TSharedPtr<FLocalVertexFactory> VertexFactory;

#if RHI_RAYTRACING
	/** Its acceleration structure (null when ray tracing is off for it) and the structure's bytes */
	TUniquePtr<FRayTracingGeometry> RayTracingGeometry;
	uint64 RayTracingBytes = 0;
#endif

private:
	TArray<FVoxelVertex> PendingVertices;
	TArray<uint32> PendingIndices;
	uint32 NumTriangles = 0;
	TSharedRef<std::atomic<int64>, ESPMode::ThreadSafe> ByteCounter;
};

/** A reference to a chunk mesh, shared across threads */
using FVoxelChunkMeshRef = TSharedPtr<FVoxelChunkMesh, ESPMode::ThreadSafe>;

/**
 * Game thread: drop a reference on the render thread, so a mesh's last reference never goes on the game thread.
 *
 * @param Mesh The reference (reset)
 */
VOXELRENDERING_API void ReleaseChunkMeshOnRenderThread(FVoxelChunkMeshRef&& Mesh);

/**
 * The meshes whose last primitive the scene deleted in a task of its update wait in a queue for the render thread (their
 * render resources must not be released beside the update's draw command caching). Game thread: enqueue a command that
 * releases them, when there are any (the region components' flush calls it).
 */
VOXELRENDERING_API void EnqueueDrainChunkMeshReleases();

/** Render thread: release the queued meshes (EnqueueDrainChunkMeshReleases; the module's shutdown) */
VOXELRENDERING_API void DrainChunkMeshReleases_RenderThread();

/**
 * One chunk interior's or seam bucket's primitive (G116), registered by its region component without a component of its
 * own (FPrimitiveSceneDesc, FSceneInterface::BatchAddPrimitives). It sits at its chunk's origin, uses GPUScene and draws
 * its mesh on the static path (DrawStaticElements: the terrain batch and its runtime virtual texture clones), so the
 * engine culls it alone and caches its draw commands once. Its far-cascade shadow (bCastFarShadow) follows the mesh's LOD
 * (voxel.Render.FarShadowMinLOD, read when the primitive is created).
 *
 * A new mesh is a new primitive: the region component removes the one it replaces in the same batch. The primitive owns its
 * FPrimitiveSceneInfoData (SceneData), which the scene info points at until the primitive is deleted.
 *
 * Thread safety: constructed on the game thread; everything else on the render thread. The state the commands below change
 * is guarded by StateLock.
 *
 * @see UVoxelRegionComponent
 * @see Plugins/VoxelWorlds/Documentation/Research/LUMEN_TERRAIN_SURFACE_CACHE_PLAN.md §3
 */
class VOXELRENDERING_API FVoxelChunkSceneProxy final : public FPrimitiveSceneProxy
{
public:
	/** What the region component gives a new primitive (game thread) */
	struct FInit
	{
		/** The mesh it draws (null: a seam bucket fading out to nothing, which draws Previous alone) */
		FVoxelChunkMeshRef Mesh;
		/** The mesh a crossfade dithers out (step 2: the slot's previous primitive's, or the one a held fade keeps) */
		FVoxelChunkMeshRef Previous;
		/** A fade from the first frame (crossfade, appearance or fade to nothing): drawn dynamically until it ends */
		bool bFading = false;
		FVoxelChunkFadeState Fade;
		/** The masked fade material's relevance, joined to the terrain's while fading */
		FMaterialRelevance FadeMaterialRelevance;
		/** The terrain material and its relevance (computed on the game thread) */
		UMaterialInterface* Material = nullptr;
		FMaterialRelevance MaterialRelevance;
		/** The scene's feature level, ray tracing for its mesh, the vertex stream's layout, and the planet's radial face up */
		ERHIFeatureLevel::Type FeatureLevel = ERHIFeatureLevel::SM5;
		bool bRayTracing = false;
		bool bPositionOnly = false;
		bool bRadialFaceUp = false;
		FVector FaceUpCenter = FVector::ZeroVector;
		/** A seam bucket (the counters tell chunk and seam geometry apart) */
		bool bSeam = false;
	};

	/**
	 * Game thread.
	 *
	 * @param Desc The region component's proxy description, with this primitive's id (ComponentId) and far shadow flag
	 * @param Init The mesh and the drawing state
	 */
	FVoxelChunkSceneProxy(const FPrimitiveSceneProxyDesc& Desc, FInit&& Init);
	virtual ~FVoxelChunkSceneProxy();

	/** The scene's record of this primitive (FPrimitiveSceneDesc::PrimitiveSceneData), alive as long as the primitive */
	FPrimitiveSceneInfoData SceneData;

	// ==================== FPrimitiveSceneProxy Interface ====================

	virtual SIZE_T GetTypeHash() const override;
	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override;
	virtual void DrawStaticElements(FStaticPrimitiveDrawInterface* PDI) override;
	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap, FMeshElementCollector& Collector) const override;
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;
	virtual uint32 GetMemoryFootprint() const override;
	virtual bool CanBeOccluded() const override { return true; }

#if RHI_RAYTRACING
	/** Always: whether a primitive of this type is in the ray tracing scene depends on HasRayTracingRepresentation. */
	virtual bool IsRayTracingRelevant() const override { return true; }

	/** @return Whether its mesh has ray tracing geometry (fixed for its life) */
	virtual bool HasRayTracingRepresentation() const override { return bRayTracing; }

	/**
	 * Its mesh's one instance, with the terrain material, for each view whose Lumen max trace distance (times
	 * voxel.Render.RayTracing.TraceDistanceScale) reaches the mesh's box, once its acceleration structure is built.
	 *
	 * @param Collector Receives the instance
	 */
	virtual void GetDynamicRayTracingInstances(FRayTracingInstanceCollector& Collector) override;
#endif

	// ==================== Render Thread ====================

	/**
	 * How this primitive draws its chunk now (the region component's GetChunkDrawInfo, tests and diagnostics): its mesh,
	 * and its ray tracing geometry. Render thread.
	 *
	 * @param OutInfo Receives the primitive's part (the proxy-wide totals are the component's)
	 */
	void GetDrawInfo_RenderThread(FVoxelChunkDrawInfo& OutInfo) const;

	/** @return The bytes of the buffers of the meshes it draws (render thread) */
	int64 GetMeshBufferBytes_RenderThread() const;

	/** @return Its mesh's ray tracing geometry's bytes, 0 for none (render thread) */
	uint64 GetRayTracingBytes_RenderThread() const;

	/** @return Whether it is a seam bucket */
	bool IsSeam() const { return bSeam; }

	/** Fade commands from the region component (FVoxelFadeScheduler's clock): the bookkeeping alpha (the MIDs carry the
	 *  drawn one), a released hold, and the fade's end, which drops Previous and turns the primitive back to its cached
	 *  static draws from the next frame. Render thread. */
	void SetFadeAlpha_RenderThread(float Alpha);
	void ReleaseFadeHold_RenderThread();
	void ClearFade_RenderThread();

private:
	/** The terrain batch over a mesh with a material (the static batch, the dynamic draws and the ray tracing batch share it) */
	void InitMeshBatch(FMeshBatch& MeshBatch, const FVoxelChunkMesh& InMesh, const FMaterialRenderProxy* MaterialProxy) const;

	/** The mesh it draws, and the one a crossfade dithers out (null: none) */
	FVoxelChunkMeshRef Mesh;
	FVoxelChunkMeshRef Previous;

	/**
	 * A fade runs (G116 step 2): the primitive answers dynamic relevance, so the scene skips its cached static draws and
	 * GetDynamicMeshElements draws the dithered halves, until ClearFade_RenderThread; the switch lands between two frames,
	 * so no frame draws neither
	 */
	std::atomic<bool> bFading{ false };
	FVoxelChunkFadeState Fade;
	FMaterialRelevance FadeMaterialRelevance;

	/** The terrain material and its relevance */
	UMaterialInterface* Material = nullptr;
	FMaterialRelevance MaterialRelevance;

	ERHIFeatureLevel::Type FeatureLevel;
	bool bRayTracing = false;
	bool bPositionOnly = false;
	bool bRadialFaceUp = false;
	FVector FaceUpCenter = FVector::ZeroVector;
	bool bSeam = false;

	/** Guards the state the render commands change against the frame's own tasks */
	mutable FCriticalSection StateLock;

#if RHI_RAYTRACING
	/** The batch its ray tracing instance views (its material proxy set at the gather) and the transform it views */
	FMeshBatch RayTracingMeshBatch;
	FMatrix RayTracingTransform = FMatrix::Identity;
#endif
};
