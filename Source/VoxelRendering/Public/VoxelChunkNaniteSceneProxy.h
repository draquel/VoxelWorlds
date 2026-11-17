// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NaniteSceneProxy.h"
#include "PrimitiveSceneInfoData.h"
#include "VoxelChunkSceneProxy.h"
#include "VoxelNaniteEncoder.h"
#include <atomic>

/**
 * What the chunk meshes drawn through Nanite hold and cost now (runtime Nanite spike; voxel.Render.NaniteStats): any
 * thread adds, any thread reads.
 */
struct VOXELRENDERING_API FVoxelNaniteCounters
{
	struct FCounts
	{
		/** Resources handed to the streaming manager and not yet released, their root pages and their pages' GPU bytes */
		int64 LiveResources = 0;
		int64 LivePages = 0;
		int64 LiveGpuBytes = 0;
		/** Encodes since the start (or the last reset), their total and longest time, and the triangles they encoded */
		int64 Encodes = 0;
		double EncodeSeconds = 0.0;
		double MaxEncodeSeconds = 0.0;
		int64 EncodedTriangles = 0;
		/** Encodes that failed or did not decode to their input (the mesh then draws without Nanite) */
		int64 Failures = 0;
		/** Nanite primitives created, and fades that ended by swapping to one (D154) */
		int64 Primitives = 0;
		int64 FadeSwaps = 0;
	};

	/** @return The counts now */
	static FCounts Get();
	/** Clears the encode counts (the live ones stay) */
	static void ResetEncodes();
	static void AddEncode(double Seconds, int64 Triangles, bool bFailed);
	static void AddLive(int64 Resources, int64 Pages, int64 GpuBytes);
	static void AddPrimitive();
	static void AddFadeSwap();
};

/**
 * One chunk mesh's Nanite encode on a task (runtime Nanite spike, N1): copies of the mesher's vertices and indices, the
 * conversion the render thread would do, the encode and (with voxel.Render.Nanite.Validate) the CPU decode check. The
 * mesh holds it until the game thread takes the result; it owns nothing the render thread uses.
 */
struct VOXELRENDERING_API FVoxelNaniteEncodeJob
{
	TArray<FVoxelVertex> Vertices;
	TArray<uint32> Indices;
	bool bRadialFaceUp = false;
	/** The planet centre relative to the mesh's origin */
	FVector3f LocalFaceUpCenter = FVector3f::ZeroVector;
	FVoxelNaniteEncodeSettings Settings;
	bool bValidate = false;

	/** The result (null: no triangles, a failed encode or validation), its stats, the validation's error */
	TUniquePtr<Nanite::FResources> Result;
	FVoxelNaniteEncodeStats Stats;
	FString Error;
	std::atomic<bool> bDone{ false };

	/** Convert, encode and check (any thread); sets bDone last */
	void Run();
};

/** @return Whether new chunk meshes are encoded on tasks (voxel.Render.Nanite.Async, default 1) rather than at submit */
VOXELRENDERING_API bool IsVoxelNaniteAsyncWanted();

/**
 * Whether chunk primitives draw through Nanite (runtime Nanite spike, voxel.Render.Nanite): the cvar, on a platform that
 * supports Nanite. Read when a mesh is submitted; a change re-meshes the terrain.
 *
 * @return Whether new chunk meshes are encoded for Nanite
 */
VOXELRENDERING_API bool IsVoxelNaniteWanted();

/** @return Whether each encode is decoded on the CPU and compared with its input before it is used (voxel.Render.Nanite.Validate) */
VOXELRENDERING_API bool IsVoxelNaniteValidationWanted();

/**
 * One chunk interior's or seam bucket's primitive drawn through Nanite (runtime Nanite spike, N1; D153): a
 * Nanite::FSceneProxyBase over the mesh's Nanite resource, registered by its region component without a component of
 * its own, as FVoxelChunkSceneProxy is. One material section (the terrain material); ray tracing geometry and Lumen cards
 * from the mesh as FVoxelChunkSceneProxy has them (D155: the mesh keeps its vertex and index buffers for the BLAS). It
 * never fades: a fading mesh is drawn by FVoxelChunkSceneProxy and swapped to this primitive when its fade ends (D154).
 *
 * Thread safety: constructed on the game thread; everything else on the render thread.
 *
 * @see Documentation/RUNTIME_NANITE_SPIKE_PLAN.md
 */
class VOXELRENDERING_API FVoxelChunkNaniteSceneProxy final : public Nanite::FSceneProxyBase
{
public:
	/** What the region component gives a new primitive (game thread) */
	struct FInit
	{
		/** The mesh it draws: has a Nanite resource handed to the streaming manager */
		FVoxelChunkMeshRef Mesh;
		/** The terrain material for Nanite (opaque where one is set: voxel.Render.Nanite.Material) */
		UMaterialInterface* Material = nullptr;
		ERHIFeatureLevel::Type FeatureLevel = ERHIFeatureLevel::SM5;
		/** As FVoxelChunkSceneProxy::FInit */
		bool bRayTracing = false;
		bool bPositionOnly = false;
		bool bRadialFaceUp = false;
		FVector FaceUpCenter = FVector::ZeroVector;
		bool bSeam = false;
		bool bLumenCards = false;
	};

	/**
	 * Game thread.
	 *
	 * @param Desc The region component's proxy description, with this primitive's id and far shadow flag
	 * @param Init The mesh and the drawing state
	 */
	FVoxelChunkNaniteSceneProxy(const FPrimitiveSceneProxyDesc& Desc, FInit&& Init);
	virtual ~FVoxelChunkNaniteSceneProxy();

	/** The scene's record of this primitive (FPrimitiveSceneDesc::PrimitiveSceneData), alive as long as the primitive */
	FPrimitiveSceneInfoData SceneData;

	// ==================== FPrimitiveSceneProxy Interface ====================

	virtual SIZE_T GetTypeHash() const override;
	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override;
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;
	virtual uint32 GetMemoryFootprint() const override;
	virtual const FCardRepresentationData* GetMeshCardRepresentation() const override;

#if RHI_RAYTRACING
	virtual bool IsRayTracingRelevant() const override { return true; }
	virtual bool HasRayTracingRepresentation() const override { return bRayTracing; }
	virtual ERayTracingPrimitiveFlags GetRayTracingPrimitiveFlags() override;
	virtual void GetStaticRayTracingInstances(FStaticRayTracingInstances& OutRayTracingInstances) override;
	virtual RayTracing::FGeometryGroupHandle GetRayTracingGeometryGroupHandle() const override;
#endif

	// ==================== Nanite::FSceneProxyBase Interface ====================

	virtual Nanite::FResourceMeshInfo GetResourceMeshInfo() const override;
	virtual Nanite::FResourcePrimitiveInfo GetResourcePrimitiveInfo() const override;

	/** @return Whether it is a seam bucket */
	bool IsSeam() const { return bSeam; }

private:
	FVoxelChunkMeshRef Mesh;
	UMaterialInterface* Material = nullptr;
	ERHIFeatureLevel::Type FeatureLevel;
	bool bRayTracing = false;
	bool bPositionOnly = false;
	bool bRadialFaceUp = false;
	FVector FaceUpCenter = FVector::ZeroVector;
	bool bSeam = false;
	bool bLumenCards = false;
};
