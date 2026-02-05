// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelMesher.h"

/**
 * CPU-based cubic mesher using face culling.
 *
 * Generates mesh geometry by checking each solid voxel and
 * emitting quad faces only where adjacent voxels are air.
 * This is the fallback path when GPU compute is unavailable.
 *
 * Algorithm:
 * - For each solid voxel, check all 6 neighbors
 * - If neighbor is air, emit a quad (4 vertices, 6 indices)
 * - Supports neighbor chunk data for seamless boundaries
 *
 * Performance: ~20-50ms for 32^3 chunk on typical CPU
 *
 * @see IVoxelMesher
 * @see FVoxelGPUCubicMesher
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

private:
	/**
	 * Check if a face should be rendered.
	 * Returns true if the neighbor in the given direction is air.
	 */
	bool ShouldRenderFace(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z,
		int32 Face) const;

	/**
	 * Get voxel data at position, handling chunk boundaries.
	 */
	FVoxelData GetVoxelAt(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z) const;

	/**
	 * Emit a quad face to the mesh data.
	 */
	void EmitQuad(
		FChunkMeshData& MeshData,
		const FVoxelMeshingRequest& Request,
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
	 * Process a single face direction using greedy meshing.
	 * @param Face Face direction (0-5)
	 * @param Request The meshing request
	 * @param OutMeshData Output mesh data
	 * @param OutGeneratedFaces Counter for generated faces
	 */
	void ProcessFaceDirectionGreedy(
		int32 Face,
		const FVoxelMeshingRequest& Request,
		FChunkMeshData& OutMeshData,
		uint32& OutGeneratedFaces);

	/**
	 * Build a 2D face mask for a slice perpendicular to a face direction.
	 * @param Face Face direction
	 * @param SliceIndex Position along the primary axis
	 * @param Request The meshing request
	 * @param OutMask Output mask (ChunkSize x ChunkSize), 0 = no face, otherwise MaterialID + 1
	 */
	void BuildFaceMask(
		int32 Face,
		int32 SliceIndex,
		const FVoxelMeshingRequest& Request,
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
		const FVoxelMeshingRequest& Request,
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

	/** UV coordinates for quad vertices (legacy, use FaceUVs instead) */
	static const FVector2f QuadUVs[4];

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
	 * @param Request The meshing request with voxel data
	 * @param X, Y, Z Voxel position
	 * @param Face Face direction (0-5)
	 * @param VertexIndex Vertex index within the face (0-3)
	 * @return AO value 0-3 (0=unoccluded, 3=fully occluded)
	 */
	uint8 CalculateVertexAO(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z,
		int32 Face, int32 VertexIndex) const;

	/**
	 * Calculate AO for all 4 vertices of a face.
	 * @param Request The meshing request with voxel data
	 * @param X, Y, Z Voxel position
	 * @param Face Face direction (0-5)
	 * @param OutAO Output array of 4 AO values (0-3 each)
	 */
	void CalculateFaceAO(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z,
		int32 Face, uint8 OutAO[4]) const;
};
