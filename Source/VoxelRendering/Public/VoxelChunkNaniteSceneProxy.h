// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NaniteSceneProxy.h"
#include "PrimitiveSceneInfoData.h"
#include "VoxelChunkSceneProxy.h"
#include "VoxelNaniteEncoder.h"
#include "VoxelCoreTypes.h"
#include <atomic>

/**
 * What the chunk meshes drawn through Nanite hold and cost now (runtime Nanite; voxel.Render.NaniteStats): any
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
		/** Of those, the triangles in a masked section (the leaves, D162) */
		int64 MaskedTriangles = 0;
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
	static void AddEncode(double Seconds, int64 Triangles, bool bFailed, int64 MaskedTriangles = 0);
	static void AddLive(int64 Resources, int64 Pages, int64 GpuBytes);
	static void AddPrimitive();
	static void AddFadeSwap();
};

/**
 * One chunk mesh's Nanite encode on a task (runtime Nanite): copies of the mesher's vertices and indices, the
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
 * Whether a world's terrain draws through Nanite (D158), from what decides it; a pure function, so a test can reach every
 * case. The override wins, then the setting (Auto follows the virtual shadow maps); either way only where Nanite can
 * draw: a platform that supports it, a world that renders (never a dedicated server), and an engine whose Nanite format the
 * encoder was checked against (D160).
 *
 * @param Setting                 The world's setting (UVoxelWorldConfiguration::NaniteTerrain)
 * @param Override                voxel.Render.Nanite: -1 = the setting, 0 = off, 1 = on
 * @param bVirtualShadows         Whether the project draws shadows with virtual shadow maps (for Auto)
 * @param bPlatformSupportsNanite Whether the shader platform supports Nanite
 * @param bRenders                Whether the world renders (false on a dedicated server or without a renderer)
 * @param bFormatVerified         Whether the encoder's format was checked against this engine version
 * @return Whether new chunk meshes of that world are encoded for Nanite
 */
VOXELRENDERING_API bool ResolveVoxelNaniteTerrain(EVoxelNaniteTerrain Setting, int32 Override, bool bVirtualShadows,
	bool bPlatformSupportsNanite, bool bRenders, bool bFormatVerified);

/** @return voxel.Render.Nanite: -1 = each world's setting, 0 = off everywhere, 1 = on everywhere */
VOXELRENDERING_API int32 GetVoxelNaniteOverride();

/** @return Whether the project draws its shadows with virtual shadow maps now (r.Shadow.Virtual.Enable) */
VOXELRENDERING_API bool IsVoxelVirtualShadowMapEnabled();

/**
 * Whether the encoder's copied Nanite layouts (VoxelNaniteFormat.h) were checked against this engine version (D160). The
 * format changed in 5.6 and 5.7; on an engine not checked the terrain draws without Nanite, and the first call logs one
 * warning, rather than risk a page the GPU misreads.
 *
 * @return Whether this engine's Nanite format is one the encoder writes
 */
VOXELRENDERING_API bool IsVoxelNaniteFormatVerified();

/** @return Whether each encode is decoded on the CPU and compared with its input before it is used (voxel.Render.Nanite.Validate) */
VOXELRENDERING_API bool IsVoxelNaniteValidationWanted();

/**
 * One chunk interior's or seam bucket's primitive drawn through Nanite (runtime Nanite; D153): a
 * Nanite::FSceneProxyBase over the mesh's Nanite resource, registered by its region component without a component of
 * its own, as FVoxelChunkSceneProxy is. One material section (the terrain material); ray tracing geometry and Lumen cards
 * from the mesh as FVoxelChunkSceneProxy has them (D155: the mesh keeps its vertex and index buffers for the BLAS). It
 * never fades: a fading mesh is drawn by FVoxelChunkSceneProxy and swapped to this primitive when its fade ends (D154).
 *
 * Thread safety: constructed on the game thread; everything else on the render thread.
 *
 * @see Documentation/RUNTIME_NANITE_OPTIN_PLAN.md, and the spike before it, RUNTIME_NANITE_SPIKE_PLAN.md
 */
class VOXELRENDERING_API FVoxelChunkNaniteSceneProxy final : public Nanite::FSceneProxyBase
{
public:
	/** What the region component gives a new primitive (game thread) */
	struct FInit
	{
		/** The mesh it draws: has a Nanite resource handed to the streaming manager */
		FVoxelChunkMeshRef Mesh;
		/** The terrain material for Nanite's section 0 (opaque: voxel.Render.Nanite.Material) */
		UMaterialInterface* Material = nullptr;
		/** The terrain's masked material for section 1, the masked triangles' (D162; the world's terrain material), if the
		 *  mesh has any */
		UMaterialInterface* MaskedMaterial = nullptr;
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

	/**
	 * How this primitive draws its chunk (as FVoxelChunkSceneProxy::GetDrawInfo_RenderThread): its mesh alone, never a fade.
	 * Render thread.
	 *
	 * @param OutInfo Receives the primitive's part
	 */
	void GetDrawInfo_RenderThread(FVoxelChunkDrawInfo& OutInfo) const;

	/** @return The bytes of its mesh's buffers (kept for ray tracing, D155) and of its ray tracing geometry (render thread) */
	int64 GetMeshBufferBytes_RenderThread() const;
	uint64 GetRayTracingBytes_RenderThread() const;

private:
	FVoxelChunkMeshRef Mesh;
	UMaterialInterface* Material = nullptr;
	/** Section 1's material (D162), or Material when Nanite cannot draw the masked one */
	UMaterialInterface* MaskedMaterial = nullptr;
	ERHIFeatureLevel::Type FeatureLevel;
	bool bRayTracing = false;
	/** Ray traced through Nanite's own geometry (r.RayTracing.Nanite.Mode other than 0, read when the proxy is made):
	 *  the engine streams the clusters out and builds the BLAS, and the mesh builds none of its own */
	bool bNaniteRayTracing = false;
	bool bPositionOnly = false;
	bool bRadialFaceUp = false;
	FVector FaceUpCenter = FVector::ZeroVector;
	bool bSeam = false;
	bool bLumenCards = false;
};
