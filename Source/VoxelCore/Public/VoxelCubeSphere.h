// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "VoxelWorldModeTypes.h"

/**
 * The cube-sphere surface coordinate of a planet (spherical requirement SP-A2), as pure math on unit directions from the
 * planet centre: six cube faces (0..5 = +X, -X, +Y, -Y, +Z, -Z), each with a normal N, a U axis T and a V axis B (a point
 * on the face is N + U * T + V * B, T x B = N), and the tangent warp u = (4 / pi) * atan(g) on each gnomonic coordinate g.
 * FSphericalPlanetWorldMode::WorldToSurface and SurfaceToWorld are these functions around the planet's centre and radius.
 * Thread-safe.
 */
struct VOXELCORE_API FVoxelCubeSphere
{
	/**
	 * A face's frame.
	 *
	 * @param Face Cube face (clamped to 0..5)
	 * @param OutN Face normal
	 * @param OutT U axis
	 * @param OutB V axis
	 */
	static void GetFaceAxes(int32 Face, FVector& OutN, FVector& OutT, FVector& OutB);

	/** The warp of a gnomonic coordinate: u = (4 / pi) * atan(g). */
	static double WarpFromGnomonic(double G);

	/** The gnomonic coordinate of a warped one: g = tan(pi / 4 * u). */
	static double GnomonicFromWarp(double U);

	/**
	 * The face a direction points through and its (U, V) there, in [-1, 1].
	 *
	 * @param Direction Unit direction from the planet centre
	 * @return The surface coordinate
	 */
	static FVoxelSurfaceCoord DirectionToSurface(const FVector& Direction);

	/**
	 * The unit direction through (Face, U, V). UV outside [-1, 1] continues past the face's edge (the face's plane
	 * extended), which is how a grid reaches across an edge.
	 *
	 * @param Face Cube face (clamped to 0..5)
	 * @param UV Warped coordinates on the face
	 * @return Unit direction
	 */
	static FVector SurfaceToDirection(int32 Face, const FVector2D& UV);

	/**
	 * A direction's (U, V) on a given face's extended plane, whichever face it belongs to.
	 *
	 * @param Face Cube face
	 * @param Direction Unit direction
	 * @param OutUV Warped coordinates on that face's plane (may lie outside [-1, 1])
	 * @param MinDot Directions with N . D at or below this do not reach the face's plane usefully
	 * @return False when the direction is not on the face's side
	 */
	static bool ProjectToFace(int32 Face, const FVector& Direction, FVector2D& OutUV, double MinDot = 0.05);

	/**
	 * East and north on the tangent plane at a unit up, north toward the +Z pole (the planet's axis): east =
	 * normalize(+Z x up), north = up x east (at up +X: east +Y, north +Z). Within about 0.01 degrees of a pole, where
	 * north is undefined, east is +Y projected onto the tangent plane (north is then -X at the north pole, +X at the
	 * south pole).
	 *
	 * @param Up Unit local up
	 * @param OutEast Unit east
	 * @param OutNorth Unit north
	 */
	static void GetTangentFrame(const FVector& Up, FVector& OutEast, FVector& OutNorth);
};

/**
 * A grid of N x N cells of equal warped UV on each cube face, addressed by an atlas coordinate (Face * N + IU, IV) so a
 * cell is an FIntPoint like a heightmap world's chunk column. Map tiles, navigation regions and surface-NPC population
 * cells on a planet use it (spherical Phase 4, WORLD_MODES_REVIEW_PLAN.md §18). With N = ceil((pi / 2) * R / CellSize) a
 * cell is at most CellSize across along a face's centre lines on the datum sphere, and the warp keeps cell areas within
 * about 1.4x of each other. Every direction belongs to exactly one cell. Pure and thread-safe.
 */
struct VOXELCORE_API FVoxelCubeSphereGrid
{
	FVoxelCubeSphereGrid() = default;

	/** @param InCellsPerEdge N, cells along each face edge (at least 1) */
	explicit FVoxelCubeSphereGrid(int32 InCellsPerEdge);

	/**
	 * Cells along a face edge for a cell size on a sphere.
	 *
	 * @param Radius Sphere radius (the planet's datum)
	 * @param CellSize Wanted cell size (uu)
	 * @return N = max(1, ceil((pi / 2) * Radius / CellSize))
	 */
	static int32 CellsPerEdgeFor(double Radius, double CellSize);

	/** Whether the grid has cells (a default-constructed grid has none). */
	bool IsValid() const { return N > 0; }

	/** N, cells along each face edge. */
	int32 GetCellsPerEdge() const { return N; }

	/** 6 * N * N. */
	int64 GetNumCells() const { return 6ll * N * N; }

	/** UV extent of one cell (2 / N). */
	double GetCellUV() const { return N > 0 ? 2.0 / N : 2.0; }

	/** The atlas coordinate of a cell: (Face * N + IU, IV). */
	FIntPoint ToAtlas(int32 Face, int32 IU, int32 IV) const { return FIntPoint(Face * N + IU, IV); }

	/**
	 * A cell's face and indices.
	 *
	 * @param Cell Atlas coordinate
	 * @return False when the coordinate is outside the atlas
	 */
	bool FromAtlas(const FIntPoint& Cell, int32& OutFace, int32& OutIU, int32& OutIV) const;

	/** Whether an atlas coordinate names a cell. */
	bool IsValidCell(const FIntPoint& Cell) const;

	/**
	 * The cell a direction lies in.
	 *
	 * @param Direction Unit direction from the centre
	 * @param OutFraction Optional: where in the cell, (0..1, 0..1) along its U and V
	 * @return Atlas coordinate
	 */
	FIntPoint CellOfDirection(const FVector& Direction, FVector2D* OutFraction = nullptr) const;

	/**
	 * The unit direction at a fraction of a cell ((0.5, 0.5) is its centre). Fractions outside [0, 1] continue past the
	 * cell, across a face edge too (the face's plane extended).
	 *
	 * @param Cell Atlas coordinate (must be valid)
	 * @param FU Fraction along the cell's U
	 * @param FV Fraction along the cell's V
	 * @return Unit direction
	 */
	FVector DirectionAt(const FIntPoint& Cell, double FU, double FV) const;

	/** The direction through a cell's centre. */
	FVector CellCentreDirection(const FIntPoint& Cell) const { return DirectionAt(Cell, 0.5, 0.5); }

	/**
	 * The cell a step (DX, DY) of cells away: on the face by index, across an edge the cell holding the direction that
	 * far past the cell's centre.
	 *
	 * @param Cell Atlas coordinate
	 * @param DX Cells along U
	 * @param DY Cells along V
	 * @return Atlas coordinate of the neighbour
	 */
	FIntPoint Neighbour(const FIntPoint& Cell, int32 DX, int32 DY) const;

	/**
	 * The cells whose centre lies within an angle of a direction: a flood fill through the four neighbours from the
	 * direction's cell (always included).
	 *
	 * @param Direction Unit direction
	 * @param MaxAngle Angle (radians)
	 * @param MaxCells Stop past this many cells
	 * @param OutCells The cells, nearest first in flood order
	 * @return False when stopped at MaxCells (OutCells is then incomplete)
	 */
	bool GatherWithinAngle(const FVector& Direction, double MaxAngle, int32 MaxCells, TArray<FIntPoint>& OutCells) const;

	/**
	 * The cells a small set of directions spans, e.g. a chunk's corners: on every face one of them lies on, the cells
	 * of the UV rectangle of all of them projected onto that face, clamped to the face.
	 *
	 * @param Directions Unit directions (a few degrees apart at most)
	 * @param OutCells The cells (appended, no duplicates among them)
	 */
	void GatherSpanned(TConstArrayView<FVector> Directions, TArray<FIntPoint>& OutCells) const;

private:
	int32 N = 0;
};
