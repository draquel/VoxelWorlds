// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelMesher.h"

/**
 * CPU cubic mesher (blocky terrain), the only cubic mesher.
 *
 * Emits a quad for every face of a solid voxel whose neighbour across that face is air (or where
 * either side is a non-occluding material such as leaves), optionally greedy-merging coplanar faces
 * of the same material and biome into larger quads.
 *
 * Seam ownership (SEAM_OWNERSHIP_ARCHITECTURE.md §2.3, "trivially compatible"; world modes review
 * WM-C1): in streaming the chunk manager asks for the Interior domain, which emits every face except
 * those lying on the chunk's six boundary planes, and reads nothing outside the chunk, so it never
 * waits for or re-meshes on a neighbour. The faces on a boundary plane belong to that chunk pair's
 * face seam job (GenerateFaceSeamMeshCPU), which culls them against both chunks' voxels. No cubic
 * face lies only on a chunk edge or corner, so edge and corner seams carry no geometry.
 * The Full domain (whole chunk, boundary faces culled against the request's face-neighbour slices)
 * serves collision and tests.
 *
 * Vertex AO (colour channel B) samples outside the chunk as air in the Interior domain, and outside
 * the two chunks in a face seam; no voxel material reads it today.
 *
 * Thread safety: GenerateMeshCPU and the seam entry points only read the request and Config, so they
 * may run on worker threads concurrently.
 *
 * @see IVoxelMesher
 */
class VOXELMESHING_API FVoxelCPUCubicMesher : public IVoxelMesher
{
public:
	FVoxelCPUCubicMesher();
	virtual ~FVoxelCPUCubicMesher() override;

	// ============================================================================
	// IVoxelMesher Interface
	// ============================================================================

	virtual void Initialize() override;
	virtual void Shutdown() override;
	virtual bool IsInitialized() const override;

	virtual bool GenerateMeshCPU(
		const FVoxelMeshingRequest& Request,
		FChunkMeshData& OutMeshData) override;

	virtual bool GenerateMeshCPU(
		const FVoxelMeshingRequest& Request,
		FChunkMeshData& OutMeshData,
		FVoxelMeshingStats& OutStats) override;

	// Async GPU methods - CPU mesher runs synchronously, wraps sync call
	virtual FVoxelMeshingHandle GenerateMeshAsync(
		const FVoxelMeshingRequest& Request,
		FOnVoxelMeshingComplete OnComplete) override;

	virtual bool IsComplete(const FVoxelMeshingHandle& Handle) const override;
	virtual bool WasSuccessful(const FVoxelMeshingHandle& Handle) const override;

	virtual FRHIBuffer* GetVertexBuffer(const FVoxelMeshingHandle& Handle) override;
	virtual FRHIBuffer* GetIndexBuffer(const FVoxelMeshingHandle& Handle) override;
	virtual bool GetBufferCounts(
		const FVoxelMeshingHandle& Handle,
		uint32& OutVertexCount,
		uint32& OutIndexCount) const override;
	virtual bool GetRenderData(
		const FVoxelMeshingHandle& Handle,
		FChunkRenderData& OutRenderData) override;
	virtual bool ReadbackToCPU(
		const FVoxelMeshingHandle& Handle,
		FChunkMeshData& OutMeshData) override;

	virtual void ReleaseHandle(const FVoxelMeshingHandle& Handle) override;
	virtual void ReleaseAllHandles() override;

	virtual void SetConfig(const FVoxelMeshingConfig& Config) override;
	virtual const FVoxelMeshingConfig& GetConfig() const override;

	virtual bool GetStats(
		const FVoxelMeshingHandle& Handle,
		FVoxelMeshingStats& OutStats) const override;

	virtual FString GetMesherTypeName() const override { return TEXT("CPU Cubic"); }

	// ============================================================================
	// Seam-Ownership Meshing (SEAM_OWNERSHIP_ARCHITECTURE.md §2.3)
	// ============================================================================

	/**
	 * Mesh the faces on the plane shared by the owner A and B = A + unit(Axis): A's faces toward +Axis
	 * in its last layer and B's faces toward -Axis in its first, each culled against the other side's
	 * voxel. Positions are in the owner's local frame (the plane sits at ChunkSize * VoxelSize along
	 * Axis). The LOD fields are ignored: cubic always meshes at full resolution (decision D7).
	 *
	 * @param SeamRequest Owner, axis and both chunks' edit-merged voxels
	 * @param OutMeshData Receives the seam's quads (empty when no face is visible on the plane)
	 * @return false only for an invalid request or an uninitialized mesher
	 */
	virtual bool GenerateFaceSeamMeshCPU(const FVoxelFaceSeamRequest& SeamRequest, FChunkMeshData& OutMeshData) override;

	/**
	 * Edge seams carry no cubic geometry (every face lies in a face plane): returns an empty mesh.
	 * The chunk manager does not schedule them for cubic (FVoxelSeamRegistry::SetFaceSeamsOnly).
	 */
	virtual bool GenerateEdgeSeamMeshCPU(const FVoxelEdgeSeamRequest& SeamRequest, FChunkMeshData& OutMeshData) override;

	/** Corner seams carry no cubic geometry: returns an empty mesh. See GenerateEdgeSeamMeshCPU. */
	virtual bool GenerateCornerSeamMeshCPU(const FVoxelCornerSeamRequest& SeamRequest, FChunkMeshData& OutMeshData) override;

	/**
	 * Whether a face of the voxel at (X, Y, Z) lies on one of the chunk's six boundary planes (the
	 * +X face of the last X layer, the -X face of the first, and so on): the faces the Interior domain
	 * leaves to the face seams.
	 */
	static bool IsBoundaryFace(int32 Face, int32 X, int32 Y, int32 Z, int32 ChunkSize);

private:
	/**
	 * The voxels one meshing pass reads, in the frame of the chunk whose mesh it emits (for a face
	 * seam, the owner's). Own is that chunk. A face seam adds the neighbour B = Own + unit(OtherAxis),
	 * whose voxels sit at [ChunkSize, 2 * ChunkSize) along OtherAxis. Whole-chunk (Full domain)
	 * meshing reads one plane of each face neighbour from the request's slices instead. Anything
	 * else reads as air.
	 */
	struct FCubicPass
	{
		const FVoxelData* Own = nullptr;
		const FVoxelData* Other = nullptr;
		int32 OtherAxis = -1;
		const FVoxelMeshingRequest* Slices = nullptr;
		int32 ChunkSize = 0;
		float VoxelSize = 100.0f;

		/** The voxel at (X, Y, Z) in the pass's frame. */
		FVoxelData GetVoxel(int32 X, int32 Y, int32 Z) const;
	};

	/** Build the pass for a whole-chunk request: Full reads the face slices, Interior reads none. */
	static FCubicPass MakeChunkPass(const FVoxelMeshingRequest& Request);

	/**
	 * Read a face neighbour's voxel from the request's slices (one plane per face; anything further,
	 * or diagonal, is air). Used by the Full domain.
	 */
	static FVoxelData GetSliceVoxel(const FVoxelMeshingRequest& Request, int32 X, int32 Y, int32 Z);

	/**
	 * Check if a face should be rendered: the neighbour across it is air, or either side is a
	 * non-occluding material.
	 */
	bool ShouldRenderFace(
		const FCubicPass& Pass,
		int32 X, int32 Y, int32 Z,
		int32 Face) const;

	/**
	 * Emit a quad face to the mesh data.
	 */
	void EmitQuad(
		FChunkMeshData& MeshData,
		const FCubicPass& Pass,
		int32 X, int32 Y, int32 Z,
		int32 Face,
		const FVoxelData& Voxel) const;

	// ============================================================================
	// Greedy Meshing
	// ============================================================================

	/**
	 * Generate mesh using greedy meshing algorithm.
	 * Merges adjacent coplanar faces with the same material into larger quads.
	 */
	void GenerateMeshGreedy(
		const FVoxelMeshingRequest& Request,
		FChunkMeshData& OutMeshData,
		FVoxelMeshingStats& OutStats);

	/**
	 * Generate mesh using simple per-voxel meshing algorithm.
	 * Emits one quad per visible face without merging.
	 * Useful for debugging or when per-voxel face data is needed.
	 */
	void GenerateMeshSimple(
		const FVoxelMeshingRequest& Request,
		FChunkMeshData& OutMeshData,
		FVoxelMeshingStats& OutStats);

	/**
	 * Greedy-mesh one slice of one face direction: build its face mask, emit its non-occluding faces
	 * as 1x1 quads and merge the rest.
	 * @param Face Face direction (0-5)
	 * @param SliceIndex Position along the face's primary axis, in the pass's frame
	 * @param Pass The voxels to read
	 * @param FaceMask Scratch mask (ChunkSize^2)
	 * @param Processed Scratch flags (ChunkSize^2)
	 * @param OutMeshData Output mesh data
	 * @param OutGeneratedFaces Counter for generated (merged) faces
	 */
	void ProcessSliceGreedy(
		int32 Face,
		int32 SliceIndex,
		const FCubicPass& Pass,
		TArray<uint16>& FaceMask,
		TArray<bool>& Processed,
		FChunkMeshData& OutMeshData,
		uint32& OutGeneratedFaces) const;

	/**
	 * Mesh one slice of one face direction without merging (one quad per visible face).
	 * @param Face Face direction (0-5)
	 * @param SliceIndex Position along the face's primary axis, in the pass's frame
	 * @param Pass The voxels to read
	 * @param OutMeshData Output mesh data
	 * @param OutGeneratedFaces Counter for generated faces
	 */
	void ProcessSliceSimple(
		int32 Face,
		int32 SliceIndex,
		const FCubicPass& Pass,
		FChunkMeshData& OutMeshData,
		uint32& OutGeneratedFaces) const;

	/**
	 * Build a 2D face mask for a slice perpendicular to a face direction.
	 * @param Face Face direction
	 * @param SliceIndex Position along the primary axis
	 * @param Pass The voxels to read
	 * @param OutMask Output mask (ChunkSize x ChunkSize), 0 = no face, otherwise (MaterialID + 1) | (BiomeID << 8)
	 */
	void BuildFaceMask(
		int32 Face,
		int32 SliceIndex,
		const FCubicPass& Pass,
		TArray<uint16>& OutMask) const;

	/**
	 * Emit a merged quad covering multiple voxels.
	 * @param Face Face direction
	 * @param SliceIndex Position along the primary axis
	 * @param U Start position on first secondary axis
	 * @param V Start position on second secondary axis
	 * @param Width Size along first secondary axis
	 * @param Height Size along second secondary axis
	 * @param MaterialID Material for this quad
	 * @param BiomeID Biome for this quad
	 */
	void EmitMergedQuad(
		FChunkMeshData& MeshData,
		const FCubicPass& Pass,
		int32 Face,
		int32 SliceIndex,
		int32 U, int32 V,
		int32 Width, int32 Height,
		uint8 MaterialID,
		uint8 BiomeID) const;

	/**
	 * Get the axis mapping for a face direction.
	 * @param Face Face direction (0-5)
	 * @param OutPrimaryAxis The axis perpendicular to the face (0=X, 1=Y, 2=Z)
	 * @param OutUAxis The first axis parallel to the face
	 * @param OutVAxis The second axis parallel to the face
	 * @param OutPositive Whether the face points in the positive direction
	 */
	static void GetFaceAxes(int32 Face, int32& OutPrimaryAxis, int32& OutUAxis, int32& OutVAxis, bool& OutPositive);

	/** Whether the mesher is initialized */
	bool bIsInitialized = false;

	/** Current configuration */
	FVoxelMeshingConfig Config;

	/** Counter for generating request IDs */
	std::atomic<uint64> NextRequestId{1};

	/** Cached mesh results for async pattern */
	struct FCachedResult
	{
		FChunkMeshData MeshData;
		FVoxelMeshingStats Stats;
		bool bSuccess = false;
	};
	TMap<uint64, FCachedResult> CachedResults;
	mutable FCriticalSection CacheLock;

	// ============================================================================
	// Face Data Tables
	// ============================================================================

	/** Face direction offsets (6 faces) */
	static const FIntVector FaceOffsets[6];

	/** Face normals */
	static const FVector3f FaceNormals[6];

	/** Quad vertex offsets for each face (4 vertices per face) */
	static const FVector3f QuadVertices[6][4];

	/** Per-face UV coordinates to ensure consistent texture orientation */
	static const FVector2f FaceUVs[6][4];

	/** AO neighbor offsets: [Face][Vertex][Side1, Side2, Corner] */
	static const FIntVector AONeighborOffsets[6][4][3];

	// ============================================================================
	// Ambient Occlusion
	// ============================================================================

	/**
	 * Calculate AO for a single vertex of a face.
	 * Uses the standard voxel AO algorithm checking side1, side2, and corner neighbors.
	 * @param Pass The voxels to read
	 * @param X, Y, Z Voxel position
	 * @param Face Face direction (0-5)
	 * @param VertexIndex Vertex index within the face (0-3)
	 * @return AO value 0-3 (0=unoccluded, 3=fully occluded)
	 */
	uint8 CalculateVertexAO(
		const FCubicPass& Pass,
		int32 X, int32 Y, int32 Z,
		int32 Face, int32 VertexIndex) const;

	/**
	 * Calculate AO for all 4 vertices of a face.
	 * @param Pass The voxels to read
	 * @param X, Y, Z Voxel position
	 * @param Face Face direction (0-5)
	 * @param OutAO Output array of 4 AO values (0-3 each)
	 */
	void CalculateFaceAO(
		const FCubicPass& Pass,
		int32 X, int32 Y, int32 Z,
		int32 Face, uint8 OutAO[4]) const;
};
