// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelData.h"
#include "VoxelCoreTypes.h"
#include "VoxelSeaSurface.h"
#include "VoxelEditTypes.h"

class IVoxelWorldMode;
class UVoxelCaveConfiguration;
class FVoxelMaterialTable;
struct FVoxelConditioningZone;

/**
 * Global signals used to classify extracted surface points as underground / underwater — the things a
 * single chunk's voxel data cannot know on its own. A surface point can be covered by terrain that lives
 * in a NEIGHBORING chunk (a cave floor, or ground under an overhang, whose roof is in the chunk above),
 * or it can be submerged; per-chunk data can't see either. These provide the cross-chunk answers: the
 * analytic terrain-height query (solid terrain above the point → underground) and the world water level.
 */
struct FScatterExtractionContext
{
	/**
	 * World mode for the analytic terrain-height query. Null = coverage check skipped. Non-owning: a caller that runs
	 * the extraction synchronously keeps the mode alive; a context handed to a worker also sets WorldModeOwner.
	 */
	const IVoxelWorldMode* WorldMode = nullptr;
	/**
	 * Keeps WorldMode alive while a worker holds this context: the scatter manager's contexts share the chunk manager's
	 * mode, so an extraction still running after the chunk manager's Shutdown reset its pointer (a PIE stop) reads a live
	 * mode (G104). Null for a synchronous caller that owns the mode itself.
	 */
	TSharedPtr<const IVoxelWorldMode> WorldModeOwner;

	/**
	 * The world's material table (its atlas's): on the smooth terrain a surface point where the shader draws a material's
	 * slope rock counts as that material (FVoxelMaterialTable::GetDrawnMaterial), so a definition limited to grass does not
	 * grow on the rock (G106). Null: the voxels' materials.
	 */
	TSharedPtr<const FVoxelMaterialTable> Materials;
	/** Noise params for IVoxelWorldMode::GetTerrainHeightAt. */
	FVoxelNoiseParams NoiseParams;
	/**
	 * The sea (generation's: FVoxelSeaSurface::FromConfig): points below it that are not underground are underwater.
	 * The WaterLevel plane in heightmap modes, the sphere of WaterRadius on a planet (spherical SP-E1). Disabled (no
	 * underwater class from it) when water is off.
	 */
	FVoxelSeaSurface Sea;
	/**
	 * The terrain conditioning zones over the chunk (POI pads and their falloff rings), so the analytic surface the
	 * extractors compare with is the generated ground, not the natural one. Heightmap worlds: the buried-chunk test
	 * compares the chunk's top with the natural height plus these zones (FVoxelTerrainConditioning::ApplyToHeight;
	 * world modes review G23). Planets (spherical SP-B10): the voxels past a chunk's +X, +Y and +Z faces
	 * (GetVoxelOutsideChunk) are generated on the flattened ground (ApplyToAltitude). Each kind of zone applies only
	 * in its own kind of world.
	 * Null or empty: the world mode's natural surface.
	 */
	TSharedPtr<const TArray<FVoxelConditioningZone>> ConditioningZones;

	/**
	 * The world's cave configuration when its caves are on, so the voxels a chunk's extraction reads past its own data are
	 * carved as generation carves them (GetVoxelOutsideChunk: on a heightmap the voxel over the chunk's top layer, world
	 * modes review G26 / G28; on a planet the voxels past its +X, +Y and +Z faces, G72). Null: no caves (the terrain's
	 * voxel alone). Non-owning: the world configuration's asset.
	 */
	const UVoxelCaveConfiguration* CaveConfiguration = nullptr;

	/** Whether the world's biomes are on (bEnableBiomes): which biome generation gives a planet voxel, for the cave
	 *  configuration's biome overrides (GetVoxelOutsideChunk). */
	bool bEnableBiomes = false;

	/**
	 * For audits and tests (G72): when set, the voxels outside the chunk come from here (the neighbours' own voxels,
	 * LocalCoord in this chunk's frame as GetVoxelOutsideChunk takes it) instead of being made; returning false falls back
	 * to GetVoxelOutsideChunk. Unset in the streaming.
	 */
	TFunction<bool(const FIntVector& LocalCoord, FVoxelData& OutVoxel)> OutsideVoxelSource;

	/**
	 * The edits in the voxels just outside the chunk that its extraction reads (code review CR2, SCAT-16: one overlay for
	 * both modes), keyed by the voxel's coordinate in this chunk's frame: on a heightmap world the chunk above's bottom
	 * layer, the voxels over this chunk's top layer (local (X, Y, ChunkSize); world modes review G26); on a planet the
	 * layers of the seven neighbours past its +X, +Y and +Z faces that touch it (each component in [-1, ChunkSize], one of
	 * them ChunkSize; G29, G31). GetVoxelOutsideChunk applies these to the voxel the generator made there. Null: no edits
	 * there.
	 */
	TSharedPtr<const TMap<FIntVector, FVoxelEdit>> OutsideEdits;

	/**
	 * Heightmap worlds: the generated terrain height at a world X,Y, the world mode's natural height with
	 * ConditioningZones layered on as the generator does. Requires a heightmap WorldMode.
	 * @param WorldX World X
	 * @param WorldY World Y
	 * @return The generated surface's world Z
	 */
	float GetGeneratedTerrainHeight(double WorldX, double WorldY) const;

	/**
	 * A voxel just outside the chunk that its extraction reads, which the chunk's own data cannot show, as generation
	 * makes it (FVoxelSurfaceQuery::GenerateVoxelAt: the signed distance to the generated surface, the cave carve where it
	 * is solid under the ground, CaveConfiguration), with the neighbour's edit there applied (OutsideEdits). On a heightmap
	 * world the voxel over a column's top layer (the chunk above's bottom voxel; world modes review G26, G28), on a planet
	 * a voxel past the chunk's +X, +Y or +Z face (G29, G72). Only Density and MaterialID are meaningful. The one
	 * outside-voxel source (code review CR2, SCAT-16).
	 * @param LocalCoord The voxel's coordinate in this chunk's frame (ChunkSize on at least one axis)
	 * @param WorldPos The voxel's sample position (the chunk's origin + LocalCoord * VoxelSize, as generation samples it)
	 * @param VoxelSize Voxel edge in world units
	 * @param BiomeHint The biome generation gives the voxel when the caller knows it (a heightmap column's, which the
	 *        chunk's own voxels carry), else -1 (a planet derives it when a cave override needs it)
	 * @param GeneratedHeight Heightmap worlds: the generated height the caller sampled for the column
	 *        (GetGeneratedTerrainHeight), so the voxel is judged against the height the scan judges the column by
	 * @return The voxel as generated, with any edit applied
	 */
	FVoxelData GetVoxelOutsideChunk(const FIntVector& LocalCoord, const FVector& WorldPos, float VoxelSize, int32 BiomeHint = -1,
		const float* GeneratedHeight = nullptr) const;
};
