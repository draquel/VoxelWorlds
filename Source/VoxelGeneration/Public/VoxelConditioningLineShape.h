// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * From a drawn or routed polyline to a conditioning line's vertices and targets (Documentation/TERRAIN_LINES_PLAN.md,
 * sections 4.1 and 4.6; D134): the corners rounded so no vertex turns by more than the bend bound allows, and a
 * levelled line's profile, the ground averaged along the line and held to a grade, flatter through what is left of a
 * bend. What makes a line (a road between POIs, a spline in a level) calls these and then FVoxelConditioningLine::Make.
 *
 * Plain positions and heights, nothing of who makes the line: a point is a position in any frame the caller measures
 * distances in (world X and Y with Z 0 on a heightmap world; a planet's sampled points before they become
 * directions), a height a world Z or an altitude. Pure and thread-safe.
 *
 * The bend bound (FVoxelConditioningLine::GetWarnings) has two parts at each vertex. The turn itself: the bisectors of
 * neighbouring vertices meet outside the reach, tan(turn / 2) <= spacing / (2 x reach) (GetMaxTurnForReach). And the
 * step: across a bend's bisector the target changes by 2 x grade x d x tan(turn / 2), which times the weight, at its
 * peak, stays under a tenth of a voxel (GetMaxTurnForGrade, GetMaxGradeForTurn). RoundCorners brings the turns under a
 * bound by spreading the corners along the line; MakeProfile keeps the second part by holding the grade through a
 * bend, whatever turn is left.
 */
struct VOXELGENERATION_API FVoxelConditioningLineShape
{
	/** How far under the bound the functions aim (a line made with them passes the engine's own check with room) */
	static constexpr double BoundMargin = 0.9;

	/** @return A polyline's length */
	static double GetLength(TConstArrayView<FVector> Points);

	/**
	 * A polyline sampled evenly along its length: its two ends kept, the points between them a spacing or a little
	 * less apart (the length is cut into whole steps).
	 * @param Points The polyline
	 * @param Spacing The step asked for (a line that bends wants a vertex every 2 voxels)
	 * @param Out Receives the points (emptied first)
	 */
	static void ResampleEvenly(TConstArrayView<FVector> Points, double Spacing, TArray<FVector>& Out);

	/**
	 * Spread a polyline's corners along it: each point becomes the mean of the points within a distance of it along the
	 * line, twice over (the first pass spreads a corner evenly over the window, the second rounds the window's own
	 * ends). The window narrows toward the two ends, which stay where they are. A point moves by no more than twice the
	 * distance, and by far less on a gentle curve; a straight run stays straight.
	 * @param Points The polyline, evenly sampled (ResampleEvenly): the window is counted in points
	 * @param Spacing The points' spacing
	 * @param Distance How far each way along the line a point is averaged over (0: the points as they are)
	 * @param Out Receives the points, one for one (emptied first)
	 */
	static void SpreadCorners(TConstArrayView<FVector> Points, double Spacing, double Distance, TArray<FVector>& Out);

	/** @return The angle a polyline turns by at an inner point (radians; 0 on a straight run and at the two ends) */
	static double GetTurnAt(TConstArrayView<FVector> Points, int32 Index);

	/** @return The largest turn at any point of a polyline (radians) */
	static double GetWorstTurn(TConstArrayView<FVector> Points);

	/**
	 * The bend bound's first part: the largest turn a vertex may have for the bisectors of neighbouring vertices to
	 * meet outside the reach, 2 x atan(spacing / (2 x reach)).
	 * @param Spacing The vertices' spacing
	 * @param HeightReach The line's half width plus its falloff (FVoxelConditioningLine::GetHeightReach)
	 * @return The turn (radians)
	 */
	static double GetMaxTurnForReach(double Spacing, double HeightReach);

	/**
	 * The bend bound's second part, as a turn: the largest turn a vertex may have on a grade for the target's step
	 * across the bisector, times the weight at its peak, to stay under a tenth of a voxel. What a line that keeps its
	 * own profile (a spline's height) must round its corners to; a profile made by MakeProfile needs only the reach's.
	 * @param Grade The profile's grade there (rise over run)
	 * @param HalfWidth The core's half width
	 * @param FalloffWidth The falloff's width
	 * @param VoxelSize The world's voxel size
	 * @param Strength The line's strength (0: no height, any turn)
	 * @return The turn (radians; pi when nothing bounds it)
	 */
	static double GetMaxTurnForGrade(double Grade, float HalfWidth, float FalloffWidth, double VoxelSize, float Strength = 1.0f);

	/**
	 * The bend bound's second part, as a grade: the steepest a segment at a vertex that turns may be.
	 * @param Turn The vertex's turn (radians)
	 * @param HalfWidth The core's half width
	 * @param FalloffWidth The falloff's width
	 * @param VoxelSize The world's voxel size
	 * @param Strength The line's strength
	 * @return The grade (rise over run; a very large number on a straight run)
	 */
	static double GetMaxGradeForTurn(double Turn, float HalfWidth, float FalloffWidth, double VoxelSize, float Strength = 1.0f);

	/** What RoundCorners made of a polyline. */
	struct FRounding
	{
		/** The largest turn left at any vertex (radians) */
		double WorstTurn = 0.0;

		/** Whether that is within the bound asked for (with BoundMargin). False for a line that comes back on itself: no spreading rounds a hairpin */
		bool bWithinBound = false;

		/** The distance the corners were spread over in the end (the one asked for, widened by half as much again per try) */
		double CornerDistance = 0.0;

		/** The farthest a point was moved from where the polyline had it (before the last resampling) */
		double FarthestMove = 0.0;

		/** How many times the corners were spread (0: the polyline was within the bound as sampled) */
		int32 Tries = 0;
	};

	/**
	 * Round a polyline's corners to a bound on the turn per vertex: sample it evenly, and while a vertex turns by more
	 * than the bound, spread the corners (SpreadCorners) over a distance, wider each try, and sample again. The whole
	 * line is averaged, not only the corners that break the bound; the two ends stay. A polyline already within the
	 * bound comes back sampled evenly and otherwise as it was.
	 * @param Points The polyline (a route's waypoints, a spline's samples)
	 * @param Spacing The vertices' spacing (2 voxels)
	 * @param MaxTurn The bound (GetMaxTurnForReach, or the lesser of it and GetMaxTurnForGrade)
	 * @param CornerDistance The distance the first try spreads over (0: one spacing)
	 * @param Out Receives the vertices (emptied first)
	 * @param MaxTries How many times to widen
	 * @return What was done
	 */
	static FRounding RoundCorners(TConstArrayView<FVector> Points, double Spacing, double MaxTurn, double CornerDistance, TArray<FVector>& Out, int32 MaxTries = 3);

	/** What a levelled line's profile is made with. */
	struct FProfileParams
	{
		/** The ground is averaged over this distance each way along the line (the window narrows toward the ends) */
		double SmoothingDistance = 1200.0;

		/** The steepest a segment may be (rise over run; 0 or less: only the bend bounds the grade) */
		double MaxGrade = 0.15;

		/** The line's half width, falloff and strength, and the world's voxel size: what the bend bound is stated in */
		float HalfWidth = 225.0f;
		float FalloffWidth = 300.0f;
		float Strength = 1.0f;
		double VoxelSize = 100.0;

		/** The height the line starts at (a pad's altitude). Unset: the ground's own there */
		TOptional<double> StartHeight;

		/**
		 * The height the line ends at. Unset: the line ends where the grades bring it from its start, on the ground when
		 * they can follow it and off it when the ground is steeper for longer than the grade allows, as the hand-placed
		 * line's FollowTerrain does
		 */
		TOptional<double> EndHeight;
	};

	/** What MakeProfile made. */
	struct FProfile
	{
		/**
		 * Whether the profile reaches both end heights: false when an end height is given and the two are further apart
		 * than the grades let the line climb (the end is then at its height and the start is not)
		 */
		bool bEndsMet = true;

		/** By how much the start is missed when it is */
		double EndMiss = 0.0;

		/** The steepest segment of the profile (rise over run) */
		double WorstGrade = 0.0;
	};

	/**
	 * A levelled line's target heights: the ground averaged along the line, then held from the start (and from the end,
	 * when it has a height to meet) to the grade and, at a vertex that turns, to the grade the bend bound leaves there
	 * (GetMaxGradeForTurn, with BoundMargin). With no end heights and no turn it is the hand-placed line's FollowTerrain
	 * profile. A line made from the points and these targets steps by less than a tenth of a voxel across
	 * every bend, whatever its turns; a tight bend on a slope is levelled instead, with more cut and fill, which the
	 * caller judges against its own cap.
	 * @param Points The line's vertices (RoundCorners, or any evenly sampled polyline)
	 * @param Ground The ground's height at each vertex
	 * @param Params The smoothing, the grade, the line's widths and the end heights
	 * @param OutTargets Receives a target per vertex (emptied first)
	 * @return Whether both ends were met, and the steepest segment
	 */
	static FProfile MakeProfile(TConstArrayView<FVector> Points, TConstArrayView<double> Ground, const FProfileParams& Params, TArray<double>& OutTargets);
};
