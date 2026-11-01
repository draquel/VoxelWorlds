// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FSphericalPlanetWorldMode;

/** One cell of a planet's PCG sampling grid: a cube face, the cell's indices on it and the direction of its centre. */
struct VOXELPCG_API FPCGVoxelPlanetSurfaceCell
{
	/** Cube face (FSphericalPlanetWorldMode::WorldToSurface's) */
	int32 Face = 0;

	/** Cell indices along the face's U and V, in [0, CellsPerFaceEdge) */
	int32 IU = 0;
	int32 IV = 0;

	/** Unit direction from the planet centre through the cell's centre */
	FVector Direction = FVector::UpVector;
};

/**
 * The Voxel Surface Sampler's grid on a planet (spherical Phase 4, SP-G2; WORLD_MODES_REVIEW_PLAN.md §18, WM-SY7): the
 * shared cube-sphere grid (FVoxelCubeSphereGrid, code review CR2 REND-17: the cells, their directions and their neighbours
 * across face edges are its) at N = ceil((pi / 2) * R / Spacing) cells a face edge, so a cell is at most Spacing across
 * along the face's centre lines (the tangent warp keeps cell areas within 1.38x). The cells a box can hold are gathered by
 * a flood fill from the cells of the box's centre and corners, through the grid's neighbours, keeping each cell whose ray
 * from the centre crosses the box between the shell's inner and outer radii. Every direction belongs to one face, so no
 * point is sampled twice. Pure and thread-safe.
 */
struct VOXELPCG_API FPCGVoxelPlanetSurfaceGrid
{
	/**
	 * Cells along a face edge for a spacing (FVoxelCubeSphereGrid::CellsPerEdgeFor).
	 *
	 * @param PlanetRadius Datum radius
	 * @param Spacing Point spacing (uu)
	 * @return N
	 */
	static int32 CellsPerFaceEdge(double PlanetRadius, double Spacing);

	/**
	 * The cells whose rays cross a box inside the planet's terrain shell.
	 *
	 * @param Planet The planet world mode
	 * @param Bounds World-space box
	 * @param Spacing Point spacing (uu)
	 * @param MaxCells Give up past this many cells
	 * @param OutCells The cells, in flood order
	 * @return False when the box holds more than MaxCells cells (OutCells is then incomplete)
	 */
	static bool GatherCells(const FSphericalPlanetWorldMode& Planet, const FBox& Bounds, double Spacing, int64 MaxCells,
		TArray<FPCGVoxelPlanetSurfaceCell>& OutCells);
};
