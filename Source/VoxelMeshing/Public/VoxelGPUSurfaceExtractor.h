// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "VoxelScatterTypes.h"

/**
 * Request for GPU surface extraction.
 * Contains all data needed to dispatch a compute shader for surface point extraction.
 */
struct VOXELMESHING_API FGPUExtractionRequest
{
	FIntVector ChunkCoord;
	FVector ChunkWorldOrigin;
	float CellSize = 100.0f;

	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector2f> UV1s;
	TArray<FColor> Colors;
};

/**
 * Result from GPU surface extraction.
 * Contains extracted surface points ready for scatter placement.
 */
struct VOXELMESHING_API FGPUExtractionResult
{
	FIntVector ChunkCoord;
	TArray<FVoxelSurfacePoint> SurfacePoints;
	bool bSuccess = false;
};

/**
 * GPU-accelerated surface point extraction using compute shaders.
 *
 * Extracts surface points from mesh vertex data on the GPU.
 * Uses spatial hashing via occupancy grid for deduplication,
 * same algorithm as the CPU path but massively parallelized.
 *
 * Workflow:
 *   1. DispatchExtraction() uploads vertex data, dispatches compute shader
 *   2. Readback completes asynchronously
 *   3. Results enqueued to MPSC queue for game thread consumption
 *
 * Thread Safety:
 *   - DispatchExtraction() must be called from game thread
 *   - Results are enqueued from render thread, consumed on game thread
 *
 * Note: Lives in VoxelMeshing module (not VoxelScatter) because it requires
 * GPU shader infrastructure (IMPLEMENT_GLOBAL_SHADER, RDG) that VoxelMeshing
 * already provides.
 *
 * @see FVoxelSurfaceExtractor (CPU equivalent, in VoxelScatter)
 * @see ScatterSurfaceExtraction.usf (GPU shader)
 */
class VOXELMESHING_API FVoxelGPUSurfaceExtractor
{
public:
	/**
	 * Check if GPU surface extraction is supported on the current platform.
	 * Requires SM5 feature level.
	 */
	static bool IsGPUExtractionSupported();

	/**
	 * Dispatch a GPU surface extraction.
	 *
	 * Uploads vertex data, dispatches compute shader, and enqueues
	 * the result to the provided MPSC queue when readback completes.
	 *
	 * @param Request Extraction request with vertex data
	 * @param ResultQueue MPSC queue to enqueue result when complete
	 */
	static void DispatchExtraction(
		FGPUExtractionRequest Request,
		TQueue<FGPUExtractionResult, EQueueMode::Mpsc>* ResultQueue);

private:
	/** Max output points per dispatch (limits GPU memory allocation) */
	static constexpr int32 MaxOutputPoints = 4096;

	/** GPU surface point struct - must match FSurfacePointGPU in .usf (48 bytes) */
	struct FSurfacePointGPU
	{
		FVector3f Position;   // 12
		FVector3f Normal;     // 12
		uint32 MaterialID;    // 4
		uint32 BiomeID;       // 4
		uint32 FaceType;      // 4
		uint32 AO;            // 4
		float SlopeAngle;     // 4
		uint32 _Pad;          // 4
	};
	static_assert(sizeof(FSurfacePointGPU) == 48, "FSurfacePointGPU must be 48 bytes to match GPU struct");

	/** Convert GPU surface points to CPU format */
	static void ConvertGPUToCPU(
		const FSurfacePointGPU* GPUPoints,
		uint32 Count,
		TArray<FVoxelSurfacePoint>& OutPoints);
};
