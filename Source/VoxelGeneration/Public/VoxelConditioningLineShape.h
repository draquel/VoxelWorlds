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

	/**
	 * The radius a corner is rounded to for a vertex sampled a spacing apart along it to turn by BoundMargin times a
	 * bound (an arc turns by its length over its radius), what FitCornerArcs is given.
	 * @param Spacing The vertices' spacing
	 * @param MaxTurn The bound (GetMaxTurnForReach: the radius is then about the reach over BoundMargin)
	 * @return The radius (world units; a very large number for a bound of no turn)
	 */
	static double GetCornerRadius(double Spacing, double MaxTurn);

	/** The turn of a corner FitCornerArcs leaves as it is: over this, a corner comes back on itself (a hairpin) */
	static constexpr double MaxArcTurnDegrees = 160.0;

	/** What FitCornerArcs made of a polyline. */
	struct FCornerArcs
	{
		/** How many corners were rounded, an arc each (a run of vertices that turn the same way is one corner) */
		int32 NumArcs = 0;

		/**
		 * How many corners were left as they were: a turn of over MaxArcTurnDegrees, two corners that turn opposite ways
		 * (an S) or the same way (a U) too close for an arc each, a short line whose ends are over two radii apart
		 */
		int32 NumLeft = 0;

		/**
		 * The farthest the polyline as made runs from the one given, or the one given from it (a corner's move: the
		 * arc's middle from the corner)
		 */
		double FarthestMove = 0.0;

		/** The largest turn left at any vertex (radians) */
		double WorstTurn = 0.0;
	};

	/**
	 * Round a polyline's corners to a radius with an arc each, which turns evenly (G225; SpreadCorners averages, and an
	 * average turns most at a corner's middle). A corner is a run of vertices that turn the same way by more than an arc
	 * of the radius does; it is widened a vertex at a time until the arc tangent to the line before it and to the line
	 * after it fits between them, the arc's tangent points at the radius times tan(turn / 2) from where those two lines
	 * meet. A corner too near an end for that keeps the end where it is and lets the end's direction go: an arc of the
	 * radius through the end, tangent to the line on the corner's other side (or, near both ends, an arc of the radius
	 * through both). The polyline is then sampled evenly; a polyline with no corner comes back as it was, point for
	 * point. Where SpreadCorners averages the whole line, only the corners move here. The two ends never move.
	 * @param Points The polyline, sampled about a spacing apart (ResampleEvenly, a spline's samples)
	 * @param Spacing The spacing the polyline as made is sampled at
	 * @param Radius The radius (GetCornerRadius)
	 * @param Out Receives the vertices (emptied first)
	 * @param OutPlaces Optional: receives, for each vertex made, its place along the polyline given, in points (2.5: half
	 *                  way from the third point to the fourth; an arc's vertices are spread evenly between the places of
	 *                  its two tangent points), for what the caller reads along it (a spline's distance, widths, heights)
	 * @return The corners rounded and left, how far the line moved, and the worst turn left
	 */
	static FCornerArcs FitCornerArcs(TConstArrayView<FVector> Points, double Spacing, double Radius, TArray<FVector>& Out, TArray<double>* OutPlaces = nullptr);

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

	// ---- A river's profile (Documentation/HYDROLOGY_PLAN.md, section 5.3) ----

	/** What a river's profile is made with. Heights are world Z or altitudes, lengths world units. */
	struct FRiverProfileParams
	{
		/** The water over the bed where the river is at its full depth */
		double Depth = 150.0;

		/** A river that follows the ground: its surface is held this far under the ground along the line */
		double SurfaceUnderGround = 75.0;

		/** The surface is held this far under the lowest ground of the two banks, out to the footprint's edge (rule 2) */
		double BankMargin = 37.5;

		/** A river that follows the ground: the ground is averaged this far each way along the line (0: not at all) */
		double SmoothingDistance = 600.0;

		/** At its head the river's depth and width start from nothing and reach their full values over this length (rule 4) */
		double HeadLength = 600.0;

		/** A river that ends on land thins to nothing over this length (rule 3) */
		double EndLength = 600.0;

		/**
		 * Inside another body (the lake it leaves or runs into, the sea, the river it joins) the river's surface lies
		 * this far under that body's level, so that body answers there and the river's mesh ends under its surface
		 * (rules 5 and 7)
		 */
		double BodyTuck = 10.0;

		/** How many of the river's vertices are kept inside the body it ends in, and inside the one it starts in */
		int32 BodyVertices = 3;

		/**
		 * No surface lies under this height: the sea's level less the tuck, in a world with a sea. Ground under the sea's
		 * level holds the sea's water whatever a river says there, so a river that comes down a beach need go no lower,
		 * and its mesh stays a tuck under the sea's surface and not a margin under the beach
		 */
		double MinSurface = -TNumericLimits<double>::Max();

		/**
		 * The bed's half width, its falloff and the world's voxel size: what the bend bound is stated in. The bed's
		 * grade at a vertex that turns is held to what the bound leaves there, by lowering the bed (rule 8). A voxel
		 * size of 0 leaves the bed's grades alone
		 */
		float HalfWidth = 225.0f;
		float FalloffWidth = 300.0f;
		double VoxelSize = 0.0;

		/**
		 * The most the bend hold deepens a bed, as a share of the river's depth at that vertex (1: to twice the depth).
		 * A head and an end on land have no depth and are never deepened, so a river still starts from nothing. A bend
		 * too steep to hold within this keeps part of its step: the line made from the bed says so (its bend warning)
		 */
		double MaxBendDeepening = 1.0;

		/**
		 * The steepest a segment of the bed may be (rise over run; 0 or less: no bound). A block world's bed steps a
		 * block a column at most (FVoxelHydrology::GetBlockBedGrade): where the bed falls or rises faster, its higher
		 * end is lowered, by as much as it takes. The surface stays, so the water there is deeper; how deep the bed
		 * then lies is in DeepestCut, for the caller to judge against its cap
		 */
		double MaxBedGrade = 0.0;
	};

	/** What MakeRiverProfile made: a value per vertex of the input, and which of them the river keeps. */
	struct FRiverProfile
	{
		/** The water's surface at each vertex: it never rises from one vertex to the next */
		TArray<double> Surface;

		/** The bed's target at each vertex: the surface less the depth there */
		TArray<double> Bed;

		/** The share of the full depth and of the full width at each vertex: 0 at a head and at an end on land, 1 between */
		TArray<double> Taper;

		/** The vertices the river keeps: Num of them from First. The ones dropped lie deep inside the body it starts or ends in */
		int32 First = 0;
		int32 Num = 0;

		/** Whether the river's first vertex lies in another body (it leaves a lake: no head) and whether its last does (a mouth) */
		bool bStartsInBody = false;
		bool bEndsInBody = false;

		/** A river at given heights: how many vertices stood higher than a vertex upstream of them, and by how much at most (rule 1) */
		int32 NumRises = 0;
		double MostRise = 0.0;

		/** How many vertices were lowered for a low bank, and by how much at most (rule 2) */
		int32 NumLowBanks = 0;
		double MostLowBank = 0.0;

		/** How many vertices' beds were lowered to hold the grade through a bend, and by how much at most (rule 8) */
		int32 NumBendHolds = 0;
		double MostBendHold = 0.0;

		/** How many vertices' beds were lowered to hold the bed's grade (MaxBedGrade), and by how much at most */
		int32 NumGradeHolds = 0;
		double MostGradeHold = 0.0;

		/** The deepest the bed lies under the ground along the line, over the vertices kept outside another body */
		double DeepestCut = 0.0;

		/** How far the surface falls from the first kept vertex to the last */
		double Fall = 0.0;
	};

	/**
	 * A river's surface and bed along its line, by the rules that keep a river watertight (the hydrology plan's section
	 * 5.3), the lower-only twin of MakeProfile. The surface is what was asked for (the ground along the line less a
	 * margin, or the heights given), lowered where it must be and never raised:
	 *  1. it never rises downstream: a vertex that stands over one upstream of it is lowered to it;
	 *  2. it stays under the lowest ground of both banks by a margin, and from a low bank on;
	 *  3. a river whose last vertex is in no other body ends on land: its depth and width thin to nothing over its end;
	 *  4. at its head the depth and the width start from nothing, unless the head lies in another body;
	 *  5, 7. inside another body the surface lies a tuck under that body's level, and of the vertices inside the body the
	 *     river starts or ends in only the few next to the land are kept;
	 *  8. the bed (the surface less the depth) is lowered where its grade at a vertex that turns is over what the bend
	 *     bound leaves there, by up to a share of the depth there, so the line made from it keeps the bound's second
	 *     part through the bends of a river that is not steep and tightly curved at once.
	 * Rule 6 (a lake's level under its rim) is the lake's own. Pure and thread-safe.
	 *
	 * @param Points The line's vertices, upstream to downstream, in the frame distances are measured in (MakeProfile's)
	 * @param Ground The ground's height at each vertex
	 * @param BankGround The lowest ground across the river's footprint at each vertex: both banks, out to the footprint's edge
	 * @param Heights The surface asked for at each vertex (a spline's own heights), or empty for a river that follows the ground
	 * @param BodyLevels The level of the body a vertex lies in (the sea where the ground is under it, a lake, another
	 *                   river), or NaN where it lies in none; empty when there is no other body
	 * @param Params The depth, the margins, the head and the bed's widths
	 * @return The surface, the bed and the taper per vertex, the vertices kept, and what was lowered
	 */
	static FRiverProfile MakeRiverProfile(TConstArrayView<FVector> Points, TConstArrayView<double> Ground, TConstArrayView<double> BankGround,
		TConstArrayView<double> Heights, TConstArrayView<double> BodyLevels, const FRiverProfileParams& Params);
};
