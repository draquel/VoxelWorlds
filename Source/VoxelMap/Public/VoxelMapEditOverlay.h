// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Templates/Function.h"
#include "VoxelData.h"

/**
 * One chunk of a map tile's vertical stack, as handed to FVoxelMapEditOverlay::Build (VoxelMap, R22).
 *
 * Gathered on the game thread from the chunk manager and the edit manager; plain data afterwards, so the
 * build runs on a worker.
 */
struct VOXELMAP_API FVoxelMapOverlayChunk
{
	/** Chunk Z coordinate. The stack's X and Y are the tile's. */
	int32 ChunkZ = 0;

	/**
	 * Edit-merged voxels of the chunk (ChunkSize^3, chunk-local index X + Y*CS + Z*CS*CS), shared with the chunk
	 * manager's snapshot cache. Null = the chunk has no voxel data loaded.
	 */
	TSharedPtr<const TArray<FVoxelData>> Voxels;

	/**
	 * Per column (PY * ChunkSize + PX): local Z of the highest edited voxel in this chunk, -1 where the column has
	 * no edit. Empty when the chunk has no edits at all.
	 */
	TArray<int16> TopEditedZ;
};

/** Everything FVoxelMapEditOverlay::Build needs for one tile, as plain data. */
struct VOXELMAP_API FVoxelMapOverlayBuildInput
{
	/** Voxels per chunk edge, which is also the tile's pixels per edge. */
	int32 ChunkSize = 0;

	/** World units per voxel. */
	float VoxelSize = 0.0f;

	/** World Z of voxel 0 of chunk Z 0 (the world origin's Z). */
	float WorldOriginZ = 0.0f;

	/** Water is enabled; columns below WaterLevel read as water in the analytic map. */
	bool bUseWater = false;
	float WaterLevel = 0.0f;

	/**
	 * The tile's chunks that carry edits (loaded or not) plus loaded unedited chunks around them, in any order.
	 * Chunks that are not listed read as the analytic terrain: solid below the analytic height, air above it.
	 */
	TArray<FVoxelMapOverlayChunk> Chunks;
};

/** The inputs of one tile's overlay build, of either kind (code review CR2, REND-15): a heightmap tile's chunk stack or a planet tile's chunks. */
struct VOXELMAP_API FVoxelMapOverlayInputs
{
	TSharedPtr<struct FVoxelMapOverlayBuildInput> Heightmap;
	TSharedPtr<struct FVoxelMapPlanetOverlayInput> Planet;

	bool IsValid() const { return Heightmap.IsValid() || Planet.IsValid(); }
	int32 NumChunks() const;
	void Reset()
	{
		Heightmap.Reset();
		Planet.Reset();
	}
};

/**
 * One chunk around a planet map tile, as handed to FVoxelMapEditOverlay::BuildPlanet (spherical Phase 4, WM-SY8).
 * Gathered on the game thread; plain data afterwards.
 */
struct VOXELMAP_API FVoxelMapPlanetOverlayChunk
{
	/** Chunk coordinate. */
	FIntVector Coord = FIntVector::ZeroValue;

	/** Edit-merged voxels (ChunkSize^3, chunk-local index X + Y*CS + Z*CS*CS). Null = no voxel data loaded. */
	TSharedPtr<const TArray<FVoxelData>> Voxels;

	/** Which voxels carry an edit (one bit per voxel index). Null when the chunk has no edits. */
	TSharedPtr<const TBitArray<>> EditedVoxels;
};

/** Everything FVoxelMapEditOverlay::BuildPlanet needs for one planet tile, as plain data. */
struct VOXELMAP_API FVoxelMapPlanetOverlayInput
{
	/** Voxels per chunk edge, which is also the tile's pixels per edge. */
	int32 ChunkSize = 0;

	/** World units per voxel. */
	float VoxelSize = 0.0f;

	/** World position of voxel 0's density sample of chunk (0, 0, 0) (the world origin). */
	FVector Origin = FVector::ZeroVector;

	/** Planet centre and datum radius. */
	FVector Center = FVector::ZeroVector;
	double PlanetRadius = 0.0;

	/** Water is enabled; terrain inside WaterRadius of the centre reads as under the sea in the analytic map. */
	bool bUseWater = false;
	double WaterRadius = 0.0;

	/**
	 * The chunks that carry edits on the tile (loaded or not) plus loaded unedited chunks around them. Voxels in chunks
	 * that are not listed read as the analytic terrain along the pixel's line: solid below its analytic altitude.
	 */
	TArray<FVoxelMapPlanetOverlayChunk> Chunks;
};

/**
 * Per-tile map edit overlay (VoxelMap, QA refinement R22).
 *
 * Map tiles are drawn from the analytic terrain (world-mode height, conditioning, biome material), which knows
 * nothing about voxel edits. The overlay records, for one tile (one chunk column, one pixel per voxel column), the
 * columns where edits changed the TOP surface, with the edited surface height, its material and whether water covers
 * it (a water-flagged voxel in the air above it). Tile composition uses those values in place of the analytic ones.
 *
 * The overlay is built from edit-merged chunk data (FVoxelMapEditOverlay::Build) while the chunks are loaded, kept
 * per tile by UVoxelMapSubsystem, and is small enough to be saved with the world, so edits show on the map before
 * their area loads again.
 *
 * Masked columns are stored sparsely in ascending column order (column = PY * Resolution + PX). Plain value type,
 * safe to copy to and read from any thread.
 */
struct VOXELMAP_API FVoxelMapEditOverlay
{
	/** Serialization format of one overlay (Serialize). Bump when the layout changes. */
	static constexpr uint8 FormatVersion = 1;

	/** The same layout with altitudes for heights: a planet's overlay (bAltitudeHeights; spherical Phase 4). */
	static constexpr uint8 PlanetFormatVersion = 2;

	/** Water covers the surface: a water-flagged voxel in the air run above it (flag bit in Flags). */
	static constexpr uint8 FlagWater = 0x01;

	/** Pixels per tile edge (the chunk size). 0 for a default-constructed overlay. */
	int32 Resolution = 0;

	/**
	 * A planet tile's overlay (BuildPlanet): a column is the pixel's line along local up and Heights are altitudes above
	 * the datum. Saved as PlanetFormatVersion.
	 */
	bool bAltitudeHeights = false;

	/** Masked column indices (PY * Resolution + PX), ascending. */
	TArray<uint16> Columns;

	/** Edited surface height (world Z) per masked column; on a planet the altitude above the datum (bAltitudeHeights). */
	TArray<float> Heights;

	/** Surface material ID per masked column. */
	TArray<uint8> Materials;

	/** Flags per masked column (FlagWater). */
	TArray<uint8> Flags;

	/**
	 * Build output only, not saved: edited columns whose surface could not be resolved because a chunk it depends
	 * on is not loaded (for example a shaft dug into an unloaded chunk). The caller keeps the previous value for
	 * these columns (MergePending) and rebuilds once the chunk loads.
	 */
	TArray<uint16> PendingColumns;

	/** No masked column. */
	bool IsEmpty() const { return Columns.Num() == 0; }

	/** Number of masked columns. */
	int32 Num() const { return Columns.Num(); }

	/**
	 * Slot of a column in the parallel arrays.
	 * @param PX Pixel X in [0, Resolution)
	 * @param PY Pixel Y in [0, Resolution)
	 * @return Index into Heights / Materials / Flags, or INDEX_NONE when the column is not masked
	 */
	int32 FindColumn(int32 PX, int32 PY) const;

	/**
	 * Whether a column is pending (Build output).
	 * @param PX Pixel X
	 * @param PY Pixel Y
	 * @return True if the column is in PendingColumns
	 */
	bool IsPending(int32 PX, int32 PY) const;

	/**
	 * Build the overlay for one tile. Pure; runs on any thread.
	 *
	 * For every column some chunk marks as edited, assembles the voxel column from the top of the highest edited
	 * chunk (or the analytic surface chunk, whichever is higher) down to one chunk below the lowest edited one, and
	 * finds its top surface with FVoxelSurfaceQuery::ExtractSurfaceFromColumn. Chunks that are not listed read as
	 * the analytic terrain. The column is masked when an edited voxel lies at or above that surface and the surface
	 * differs from the analytic one (height by more than half a voxel, material, or water). Buried edits, such as
	 * dungeon interiors, therefore produce nothing. A column whose scan needs a listed chunk with no data, or falls
	 * through the stack, is reported in PendingColumns.
	 *
	 * Cost: O(edited columns x stack voxels), e.g. ~1M voxel reads for a fully edited tile three chunks deep.
	 *
	 * @param Input            The tile's chunk stack and world constants
	 * @param AnalyticHeights  Analytic surface height per column (ChunkSize^2, row-major PY * ChunkSize + PX)
	 * @param AnalyticMaterial Analytic surface material of a column (PX, PY); called only for columns that would
	 *                         otherwise match the analytic surface
	 * @return The overlay (Resolution = ChunkSize); empty when no edit changed the top surface
	 */
	static FVoxelMapEditOverlay Build(
		const FVoxelMapOverlayBuildInput& Input,
		TConstArrayView<float> AnalyticHeights,
		TFunctionRef<uint8(int32 PX, int32 PY)> AnalyticMaterial);

	/**
	 * Build the overlay for one planet tile (spherical Phase 4, WM-SY8). Pure; runs on any thread.
	 *
	 * A column is the pixel's line along local up (its direction from the centre). For every pixel whose line passes
	 * through an edited chunk, walks the line from two voxels above the highest edited chunk (or the analytic surface)
	 * down to a chunk below the lowest, reading the nearest voxel every half voxel (listed chunks' voxels; elsewhere the
	 * analytic terrain), and takes the first solid voxel under air as the surface, refined on the line by the trilinear
	 * density as UVoxelChunkManager::QueryEditMergedSurfacePoint does. The column is masked when an edited voxel lies on
	 * the walk at or above that surface and the surface differs from the analytic one (altitude by more than half a
	 * voxel, material, or water: a water-flagged voxel in the air above). A walk that meets a listed chunk with no data
	 * before the surface, or a surface in the analytic stand-in under the analytic altitude (dug into an unloaded
	 * chunk), is pending.
	 *
	 * @param Input             The tile's chunks and world constants
	 * @param PixelDirections   Unit direction of each pixel's line (ChunkSize^2, row-major PY * ChunkSize + PX)
	 * @param AnalyticAltitudes Analytic surface altitude per pixel (same layout)
	 * @param AnalyticMaterial  Analytic surface material of a pixel (PX, PY)
	 * @return The overlay (Resolution = ChunkSize, bAltitudeHeights); empty when no edit changed the surface
	 */
	static FVoxelMapEditOverlay BuildPlanet(
		const FVoxelMapPlanetOverlayInput& Input,
		TConstArrayView<FVector> PixelDirections,
		TConstArrayView<float> AnalyticAltitudes,
		TFunctionRef<uint8(int32 PX, int32 PY)> AnalyticMaterial);

	/**
	 * Build the overlay for one tile of either kind (code review CR2, REND-15): Build over a heightmap tile's chunk stack,
	 * BuildPlanet over a planet tile's chunks.
	 *
	 * @param Inputs The tile's inputs (one kind set)
	 * @param PixelDirections Unit direction of each pixel's line (a planet; unread on a heightmap world)
	 * @param AnalyticAltitudes Analytic surface height (world Z) or altitude per pixel (ChunkSize^2, row-major PY * ChunkSize + PX)
	 * @param AnalyticMaterial Analytic surface material of a pixel (PX, PY)
	 * @return The overlay; empty when no edit changed the surface or the inputs are of no kind
	 */
	static FVoxelMapEditOverlay Build(
		const FVoxelMapOverlayInputs& Inputs,
		TConstArrayView<FVector> PixelDirections,
		TConstArrayView<float> AnalyticAltitudes,
		TFunctionRef<uint8(int32 PX, int32 PY)> AnalyticMaterial);

	/**
	 * Fill a fresh build's pending columns from the previous overlay, so a column waiting on an unloaded chunk keeps
	 * showing what it showed before.
	 * @param Built    Build output (its PendingColumns are kept in the result)
	 * @param Previous The tile's current overlay, or null
	 * @return Built's masked columns plus Previous's masked columns at Built's pending columns
	 */
	static FVoxelMapEditOverlay MergePending(const FVoxelMapEditOverlay& Built, const FVoxelMapEditOverlay* Previous);

	/**
	 * Whether two overlays agree on the tile's border columns (the ones neighbouring tiles read for their hillshade
	 * apron). Null counts as empty.
	 * @param A First overlay or null
	 * @param B Second overlay or null
	 * @return True if the border columns are masked identically with identical values
	 */
	static bool BordersEqual(const FVoxelMapEditOverlay* A, const FVoxelMapEditOverlay* B);

	/**
	 * Serialize to or from an archive: format version (FormatVersion, or PlanetFormatVersion for altitude heights),
	 * resolution, a column mask (Resolution^2 bits), then height, material and flags for each masked column in column
	 * order. PendingColumns are not serialized.
	 * @param Ar The archive
	 * @return False when loading malformed data (the overlay is then reset to empty)
	 */
	bool Serialize(FArchive& Ar);

	/** Value equality of the saved fields (pending columns ignored). */
	bool operator==(const FVoxelMapEditOverlay& Other) const;
};

/** A tile coordinate with its edit overlay: the unit UVoxelMapSubsystem saves and restores (R22 phase 3). */
struct VOXELMAP_API FVoxelMapTileOverlay
{
	/** Tile coordinate (chunk X, Y; a planet's atlas coordinate). */
	FIntPoint Tile = FIntPoint::ZeroValue;

	/** The tile's overlay. */
	FVoxelMapEditOverlay Overlay;
};
