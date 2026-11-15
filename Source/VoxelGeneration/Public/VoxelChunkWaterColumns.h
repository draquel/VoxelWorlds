// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelDownLines.h"
#include "VoxelWaterLevels.h"

struct FVoxelNoiseGenerationRequest;

/**
 * The chunk a list of water columns is made for: where its voxels are, and on a planet the frame its levels are
 * stored in (the planet shaders' frame, PlanetFrame.ush: the chunk's anchor, its centre). Plain data.
 */
struct VOXELGENERATION_API FVoxelChunkWaterFrame
{
	/** The chunk's origin corner in world space (WorldOrigin included): its first voxel's sample point */
	FVector ChunkWorldPos = FVector::ZeroVector;

	/** Voxels per chunk edge */
	int32 ChunkSize = 32;

	/** The base voxel size */
	float VoxelSize = 100.0f;

	/** A planet chunk: the columns are the chunk's down-lines (FVoxelDownLines) and the levels distances from the centre */
	bool bPlanet = false;

	/** A planet: the planet centre and the datum radius */
	FVector PlanetCenter = FVector::ZeroVector;
	double PlanetRadius = 0.0;

	/** The frame of a chunk's generation request. */
	static FVoxelChunkWaterFrame FromRequest(const FVoxelNoiseGenerationRequest& Request);

	/**
	 * The frame of a chunk, for a reader that holds no request (the water pull, the edit-driven flood), with the
	 * request's own arithmetic so both find the same voxels.
	 *
	 * @param ChunkCoord The chunk
	 * @param InChunkSize Voxels per chunk edge
	 * @param InVoxelSize The base voxel size
	 * @param WorldOrigin The world configuration's origin
	 * @param bInPlanet Whether the world is a planet
	 * @param InPlanetCenter The planet centre (a planet)
	 * @param InPlanetRadius The datum radius (a planet)
	 */
	static FVoxelChunkWaterFrame Make(const FIntVector& ChunkCoord, int32 InChunkSize, float InVoxelSize, const FVector& WorldOrigin,
		bool bInPlanet, const FVector& InPlanetCenter, double InPlanetRadius);

	/** @return A voxel's sample point in world space */
	FVector VoxelWorldPos(const FIntVector& Local) const
	{
		return ChunkWorldPos + FVector(Local) * VoxelSize;
	}

	/** @return The box of the chunk's voxel sample points, first to last */
	FBox GetVoxelBox() const
	{
		return FBox(ChunkWorldPos, ChunkWorldPos + FVector((ChunkSize - 1) * VoxelSize));
	}
};

/**
 * A chunk's water columns (the hydrology plan's section 5.4): the water level over each of the chunk's XY columns on a
 * heightmap world, or over each of its down-lines on a planet (FVoxelDownLines, the lines the post-passes walk: a
 * planet chunk has no columns), with a value for "no water". Made where a lake or a river reach may reach the chunk,
 * so one level no longer holds over all of it; every other chunk keeps the request's one level and builds none.
 *
 * One function makes it (Build) from the water levels gathered over the chunk, and every reader of "is this voxel
 * under the water here" reads it: the CPU generator (the fill's seed and flood, the underwater materials and the
 * caves' minimum depth per column), the GPU, which uploads the same float4s (WaterColumnData in
 * GenerateVoxelDensity.usf, WaterColumns in VoxelPostPasses.usf), and through FVoxelChunkWaterReader the water pull
 * and the edit-driven flood. So both generators and the rules after them flag the same voxels. Any change to the
 * layout or to a test is mirrored in the shaders.
 *
 * The rows, one float4 each:
 * [the level the column's terrain is compared with, the level a voxel is compared with, the minimum cave depth in
 *  voxels, 0].
 * On a heightmap world both levels are the water level over the column, world Z in float, as the request's WaterLevel
 * is. On a planet nothing of the radius's size is held in float: the first is the level as an altitude over the datum
 * (what the request's WaterRadius - PlanetRadius is to the sea), the second the level less the anchor's distance from
 * the centre (the post-passes' PlanetSeaAltitudeOverAnchor), both formed in double. NoWater in both where no body
 * covers the column: every comparison with it fails.
 *
 * On a planet the list starts with ChunkSize rows of the down-lines' shifts, [shift on B, shift on C, 0, 0] per step,
 * which the density pass needs to find a voxel's line; the line rows follow (GetFirstColumn), 9 x ChunkSize^2 of them
 * by line id. A line's level is the level over its first voxel in the chunk; a line that crosses no voxel has none.
 */
struct VOXELGENERATION_API FVoxelChunkWaterColumns
{
	/** A row's level where no body covers the column: under every terrain height and every voxel */
	static constexpr float NoWater = -1.0e30f;

	/** The rows: what the GPU uploads. Empty: the chunk has one level (the request's) */
	TArray<FVector4f> Data;

	/** @return Whether the chunk has no list: one level holds over it, the request's */
	bool IsEmpty() const { return Data.Num() == 0; }

	/** @return The index of the first column's row: 0 on a heightmap world, ChunkSize on a planet (after the shift rows) */
	int32 GetFirstColumn() const { return FirstColumn; }

	/** @return How many columns the list has: ChunkSize^2, or a planet's 9 x ChunkSize^2 lines */
	int32 NumColumns() const { return Data.Num() - FirstColumn; }

	/** @return The frame the list was made in */
	const FVoxelChunkWaterFrame& GetFrame() const { return Frame; }

	/** @return A planet's down-lines (unset on a heightmap world) */
	const FVoxelDownLines& GetLines() const { return Lines; }

	/**
	 * Build a chunk's water columns from the water levels gathered over it.
	 *
	 * @param Water The water levels over the chunk (UVoxelChunkManager::GatherWaterLevelsForChunk), a complete gather
	 * @param InFrame The chunk
	 * @param Out Reset and filled; left empty when the snapshot holds the sea alone (one level, the request's)
	 */
	static void Build(const FVoxelWaterLevels& Water, const FVoxelChunkWaterFrame& InFrame, FVoxelChunkWaterColumns& Out);

	/**
	 * Build a chunk's water columns from its request's bodies. A request without them (the chunk's range is uniform)
	 * gives an empty list at the cost of one pointer test.
	 *
	 * @param Request The chunk's generation request
	 * @param Out Reset and filled
	 */
	static void Build(const FVoxelNoiseGenerationRequest& Request, FVoxelChunkWaterColumns& Out);

	/** @return The column a voxel belongs to: X + Y x ChunkSize, or on a planet its down-line's id */
	int32 ColumnOf(const FIntVector& Local) const
	{
		return Frame.bPlanet ? Lines.LineOf(Local) : Local.X + Local.Y * Frame.ChunkSize;
	}

	/** @return A column's row */
	const FVector4f& GetRow(int32 Column) const { return Data[FirstColumn + Column]; }

	/** @return The row of the column a voxel belongs to */
	const FVector4f& GetRowOf(const FIntVector& Local) const { return Data[FirstColumn + ColumnOf(Local)]; }

	/**
	 * Whether a voxel is at or under a level of its column (a row's second value): the fill's and the flood's test, in
	 * the generators' own arithmetic. On a heightmap world the voxel's world Z in float against the level; on a planet
	 * its distance from the centre less the anchor's, in double, against the level over the anchor.
	 *
	 * @param Local The voxel's local coordinates
	 * @param VoxelLevel The level to test against (FVector4f::Y of the voxel's row)
	 * @return True at or under the level; false for NoWater
	 */
	bool IsVoxelUnderLevel(const FIntVector& Local, float VoxelLevel) const
	{
		if (!Frame.bPlanet)
		{
			const float VoxelWorldZ = Frame.ChunkWorldPos.Z + Local.Z * Frame.VoxelSize;
			return VoxelWorldZ <= VoxelLevel;
		}
		return (ChunkPlanetRelative + FVector(Local) * Frame.VoxelSize).Size() - AnchorDist <= static_cast<double>(VoxelLevel);
	}

	/** @return Whether a voxel is at or under the water level over its own column */
	bool IsVoxelUnder(const FIntVector& Local) const
	{
		return IsVoxelUnderLevel(Local, GetRowOf(Local).Y);
	}

private:
	friend class FVoxelChunkWaterReader;

	/** Set the frame and what follows from it (a planet's lines, its anchor, the shift rows); no column is evaluated. */
	void SetFrame(const FVoxelChunkWaterFrame& InFrame);

	/** The row of a column whose reference point is given: the sample there, in the row's two frames. */
	FVector4f MakeRow(const FVoxelWaterLevels& Water, const FVector& ReferenceWorldPos) const;

	/**
	 * A planet line's first voxel in the chunk, from where it enters on its upper side.
	 * @return False when the line crosses no voxel of the chunk
	 */
	bool FindLineFirstVoxel(int32 LineId, FIntVector& OutLocal) const;

	FVoxelChunkWaterFrame Frame;

	int32 FirstColumn = 0;

	/** A planet: the chunk's down-lines, the chunk origin relative to the centre and the anchor's distance from it */
	FVoxelDownLines Lines;
	FVector ChunkPlanetRelative = FVector::ZeroVector;
	double AnchorDist = 0.0;
};

/**
 * A chunk's water columns for a reader on the game thread that tests few voxels of a chunk (the water pull across a
 * chunk's faces, the edit-driven flood): the same rows as FVoxelChunkWaterColumns::Build makes, each worked out the
 * first time a voxel of its column is asked about, so a pull that finds no seed samples nothing.
 */
class VOXELGENERATION_API FVoxelChunkWaterReader
{
public:
	/**
	 * @param InWater The water levels over the chunk, a complete gather (with a lake or a river reach: with the sea
	 *                alone a reader asks the snapshot itself)
	 * @param InFrame The chunk
	 */
	FVoxelChunkWaterReader(const FVoxelWaterLevels& InWater, const FVoxelChunkWaterFrame& InFrame);

	/** @return Whether a voxel is at or under the water level over its own column, as generation's fill tests it */
	bool IsVoxelUnder(const FIntVector& Local);

	/** @return The row of the column a voxel belongs to (FVoxelChunkWaterColumns's layout) */
	const FVector4f& GetRowOf(const FIntVector& Local);

	/** @return The water levels the reader was made with */
	const FVoxelWaterLevels& GetWater() const { return Water; }

	/** @return The frame the reader was made in */
	const FVoxelChunkWaterFrame& GetFrame() const { return Columns.GetFrame(); }

	/** @return How many columns were worked out so far */
	int32 NumEvaluated() const { return Evaluated; }

private:
	FVoxelWaterLevels Water;
	FVoxelChunkWaterColumns Columns;
	TBitArray<> Known;
	int32 Evaluated = 0;
};
