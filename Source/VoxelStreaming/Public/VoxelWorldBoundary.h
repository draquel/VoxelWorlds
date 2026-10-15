// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelWorldModeTypes.h"

/**
 * One wall of a world's boundary: an oriented box in world space.
 */
struct FVoxelWorldBoundaryWall
{
	/** Box centre (world space) */
	FVector Center = FVector::ZeroVector;

	/** Box rotation: its X axis points out of the world */
	FRotator Rotation = FRotator::ZeroRotator;

	/** Half sizes: X the wall's half thickness, Y half its length along the edge, Z half its height */
	FVector HalfExtent = FVector::ZeroVector;
};

/**
 * The hard boundary of a bounded world (world modes review WM-I5, decision D9): the walls the chunk manager builds
 * as invisible collision boxes at the edge of FVoxelWorldBounds, so a pawn, an NPC or a dropped item cannot leave the
 * world (an island's R + falloff, beyond which generation writes only a flat floor and then nothing).
 *
 * A circle becomes an inscribed polygon: the walls' inner faces are its chords, so nothing passes the edge, and the
 * segment count keeps the deepest chord within MaxChordError of the circle. A rectangle gets one wall per side.
 * Unbounded worlds and planets (a shell) get none.
 *
 * Pure geometry; the chunk manager owns the collision components (UVoxelChunkManager::RebuildWorldBoundary).
 */
struct VOXELSTREAMING_API FVoxelWorldBoundary
{
	/** Fewest walls a circle is split into */
	static constexpr int32 MinCircleSegments = 16;

	/** Most walls a circle is split into (a 2,600 km radius at a 50 uu chord error) */
	static constexpr int32 MaxCircleSegments = 2048;

	/**
	 * Segment count for a circle: the inscribed polygon whose chords stay within MaxChordError of the circle.
	 *
	 * @param Radius Circle radius
	 * @param MaxChordError Largest distance between a chord's middle and the circle
	 * @return Number of segments, clamped to [MinCircleSegments, MaxCircleSegments]
	 */
	static int32 GetCircleSegmentCount(double Radius, double MaxChordError);

	/**
	 * The walls for a world's bounds.
	 *
	 * @param Bounds The world's bounds (IVoxelWorldMode::GetWorldBounds)
	 * @param Thickness Wall thickness, outward from the edge
	 * @param MaxChordError Circles only: how far inside the circle a chord may run (see GetCircleSegmentCount)
	 * @param BottomZ World Z of the walls' bottom
	 * @param TopZ World Z of the walls' top
	 * @param OutWalls Receives the walls (emptied first); none for an unbounded world or a shell
	 */
	static void BuildWalls(const FVoxelWorldBounds& Bounds, double Thickness, double MaxChordError, double BottomZ, double TopZ,
		TArray<FVoxelWorldBoundaryWall>& OutWalls);
};
