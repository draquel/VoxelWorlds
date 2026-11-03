// Copyright Daniel Raquel. All Rights Reserved.
//
// SPIKE (D-VP4, 2026-11-03), not production code: giving Lumen, distance field shadows and AO a representation of the
// voxel terrain off screen. Two prototypes behind cvars, both off by default:
//   voxel.Render.Spike.RayTracing     ray tracing geometry per chunk and seam mesh in FVoxelSceneProxy
//   voxel.Render.Spike.DistanceField  an engine mesh distance field per chunk, built from the chunk's voxels on the mesh
//                                     worker and registered by a distance-field-only primitive per chunk
// Findings: Plugins/VoxelWorlds/Documentation/Research/LUMEN_OFFSCREEN_TERRAIN_SPIKE.md.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include <atomic>
#include "VoxelTerrainRepresentationSpike.generated.h"

class FDistanceFieldVolumeData;
struct FVoxelData;

namespace VoxelTerrainSpike
{
	/** @return Whether new chunk and seam uploads build ray tracing geometry (voxel.Render.Spike.RayTracing). */
	VOXELRENDERING_API bool IsRayTracingEnabled();

	/** @return Whether mesh jobs build a distance field beside the mesh (voxel.Render.Spike.DistanceField). */
	VOXELRENDERING_API bool IsDistanceFieldEnabled();

	/** @return Distance field voxels per terrain voxel (voxel.Render.Spike.DistanceFieldResolution). */
	VOXELRENDERING_API float GetDistanceFieldResolution();

	/** Counters the spike's console command prints (voxel.Render.Spike.Stats); every member is updated atomically. */
	struct FCounters
	{
		std::atomic<int64> RTGeometriesLive{ 0 };
		std::atomic<int64> RTBytesLive{ 0 };
		std::atomic<int64> RTBuilds{ 0 };
		std::atomic<int64> RTTrianglesBuilt{ 0 };
		std::atomic<int64> RTInstancesLastFrame{ 0 };

		std::atomic<int64> DFObjectsLive{ 0 };
		std::atomic<int64> DFBytesLive{ 0 };
		std::atomic<int64> DFBuilds{ 0 };
		std::atomic<int64> DFSkippedNoSurface{ 0 };
		std::atomic<int64> DFBricksBuilt{ 0 };
		std::atomic<int64> DFBuildMicrosTotal{ 0 };
		std::atomic<int64> DFBuildMicrosMax{ 0 };
		std::atomic<int64> DFRegisterMicrosTotal{ 0 };
		std::atomic<int64> DFRegisters{ 0 };
	};

	/** @return The process-wide counters. */
	VOXELRENDERING_API FCounters& Counters();

	/**
	 * Build an engine mesh distance field (the sparse brick format of MeshDistanceFieldUtilities.cpp, one mip, kept in
	 * AlwaysLoadedMip so it needs no bulk data on disk) from a chunk's voxels. The density saturates one voxel either side
	 * of the surface, so the voxels near the surface give the distance exactly and a Euclidean distance transform gives it
	 * further out. Reads the chunk's own voxels only (seam ownership): each chunk is its own distance field object and the
	 * engine composes objects by their minimum. Any thread.
	 *
	 * @param Voxels     The chunk's edit-merged voxels, ChunkSize^3, X fastest
	 * @param ChunkSize  Voxels per axis
	 * @param VoxelSize  World size of a voxel (uu)
	 * @param Resolution Distance field voxels per terrain voxel
	 * @param OutBricks  Receives the number of bricks stored
	 * @return The distance field in chunk-local space (voxel i at i * VoxelSize), or null when the chunk holds no surface
	 */
	VOXELRENDERING_API TSharedPtr<FDistanceFieldVolumeData, ESPMode::ThreadSafe> BuildChunkDistanceField(
		const TArray<FVoxelData>& Voxels, int32 ChunkSize, float VoxelSize, float Resolution, int32& OutBricks);
}

/**
 * SPIKE (D-VP4): a primitive that draws nothing and gives the scene one chunk's mesh distance field, for Lumen's software
 * tracing, the global distance field, distance field shadows and distance field AO. One per chunk with a surface, at the
 * chunk's origin; owned by the renderer's container actor.
 */
UCLASS(Transient, ClassGroup = Rendering)
class VOXELRENDERING_API UVoxelChunkDistanceFieldComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UVoxelChunkDistanceFieldComponent();

	/**
	 * Set the distance field (game thread); recreates the render state.
	 *
	 * @param InDistanceField The chunk's distance field, in the component's local space
	 */
	void SetDistanceField(TSharedPtr<FDistanceFieldVolumeData, ESPMode::ThreadSafe> InDistanceField);

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

private:
	TSharedPtr<FDistanceFieldVolumeData, ESPMode::ThreadSafe> DistanceField;
};
