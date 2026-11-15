// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * A polyline's bounds, and the search for its nearest segment: what a conditioning line (FVoxelConditioningLine) and a
 * river reach (FVoxelWaterReach) share, so the bed's target and the water's level over it are read at the same place
 * along the same segment (the hydrology plan's section 5.1).
 *
 * Two kinds, as the line's: a heightmap polyline (vertices at world X, Y; the distance is in the plane) and a planet
 * polyline (vertices as unit directions from the planet centre; a segment is the chord between its two vertices' points
 * on the datum sphere, and the distance is that chord's, which a caller turns into an arc with ChordToArc).
 *
 * The segments are grouped in blocks of BlockSegments with bounds, so a point far from a long polyline costs a test per
 * block. The vertices stay with their owner: every function that reads them takes PositionOf, a callable from a vertex
 * index to its position (a const FVector&), so the owner's own vertex array is the one copy.
 */
struct VOXELCORE_API FVoxelPolylineBounds
{
	/** Segments in a block of the bounds hierarchy */
	static constexpr int32 BlockSegments = 16;

	/** A planet polyline: the vertices are unit directions */
	bool bRadial = false;

	/** How many segments the polyline has (the vertices less one) */
	int32 Segments = 0;

	/** A heightmap polyline: the vertices' XY box (not grown) and one per block of BlockSegments segments */
	FBox2D Bounds = FBox2D(ForceInit);
	TArray<FBox2D> BlockBounds;

	/**
	 * A planet polyline: a ball around the vertices on the unit sphere (a centre direction and the largest chord from it
	 * to a vertex), and one per block. Chords, not angles: a point's chord to the centre is at most the ball's radius
	 * plus its chord to the nearest segment, which lies in the vertices' hull.
	 */
	FVector CapCentre = FVector::UpVector;
	double CapChordRadius = 0.0;
	TArray<FVector4d> BlockBalls;

	/**
	 * Build the bounds and the blocks from the vertices.
	 * @param NumVertices How many vertices there are (fewer than two: no segment, empty bounds)
	 * @param bInRadial Whether the positions are unit directions (a planet polyline)
	 * @param PositionOf A vertex's position by index
	 */
	void Build(int32 NumVertices, bool bInRadial, TFunctionRef<const FVector&(int32)> PositionOf);

	/** @return A heightmap polyline's XY bounds grown by Reach (invalid for a planet polyline or an empty one) */
	FBox2D GetBounds2D(double Reach) const;

	/**
	 * A planet polyline's bounds as a cap on the sphere, grown by Reach.
	 * @param PlanetRadius The datum radius
	 * @param Reach How far from its segments the polyline matters (world units along the surface)
	 * @param OutDirection The cap's centre direction
	 * @param OutAngle The cap's half-angle (radians)
	 * @return False for a heightmap polyline or an empty one
	 */
	bool GetCap(double PlanetRadius, double Reach, FVector& OutDirection, double& OutAngle) const;

	/**
	 * Whether a heightmap polyline, grown by Reach, meets a world XY region: some block of its segments overlaps it.
	 * Conservative (a block's box, not each segment).
	 * @return False for a planet polyline
	 */
	bool MeetsRegion(const FBox2D& Region, double Reach) const;

	/**
	 * Whether a planet polyline, grown by Reach, meets a cap on the sphere. Conservative, as MeetsRegion.
	 * @param CapDirection Unit direction of the cap's centre
	 * @param CapAngle The cap's half-angle (radians; pi or more covers the sphere)
	 * @param PlanetRadius The datum radius
	 * @param Reach How far from its segments the polyline matters
	 * @return False for a heightmap polyline
	 */
	bool MeetsCap(const FVector& CapDirection, double CapAngle, double PlanetRadius, double Reach) const;

	/**
	 * The nearest place on the segment AB to P.
	 * @param OutT Where along AB it is, 0 at A to 1 at B (0 for a segment of no length)
	 * @return The squared distance from P to it
	 */
	template <typename VectorType>
	static double NearestOnSegment(const VectorType& P, const VectorType& A, const VectorType& B, double& OutT)
	{
		const VectorType AB = B - A;
		const VectorType AP = P - A;
		const double LengthSq = AB | AB;
		OutT = LengthSq > UE_DOUBLE_SMALL_NUMBER ? FMath::Clamp((AP | AB) / LengthSq, 0.0, 1.0) : 0.0;
		const VectorType D = AP - AB * OutT;
		return D | D;
	}

	/** The arc on a sphere of radius R under a chord: 2 R asin(chord / 2R), as a planet zone measures its distance. */
	static double ChordToArc(double Chord, double Radius)
	{
		return 2.0 * Radius * FMath::Asin(FMath::Min(1.0, Chord / (2.0 * Radius)));
	}

	/** The angle between two unit directions, well conditioned at small angles. */
	static double AngleBetween(const FVector& A, const FVector& B)
	{
		return FMath::Atan2(FVector::CrossProduct(A, B).Size(), FVector::DotProduct(A, B));
	}

	/**
	 * The nearest segment of a heightmap polyline to (X, Y), among the blocks within SearchReach of it. Of two segments
	 * at the same distance the earlier one along the polyline answers, as in the generators' loop.
	 *
	 * @param X World X
	 * @param Y World Y
	 * @param SearchReach Only a segment nearer than this is found
	 * @param PositionOf A vertex's position by index
	 * @param OutSegment The segment's index (its first vertex's)
	 * @param OutT Where along the segment the nearest place is, 0 to 1
	 * @param OutDistSq The squared distance to it
	 * @return False when no segment is within SearchReach (or for a planet polyline)
	 */
	template <typename PositionOfType>
	bool FindNearest(double X, double Y, double SearchReach, PositionOfType&& PositionOf, int32& OutSegment, double& OutT, double& OutDistSq) const
	{
		if (bRadial || Segments == 0
			|| X < Bounds.Min.X - SearchReach || X > Bounds.Max.X + SearchReach || Y < Bounds.Min.Y - SearchReach || Y > Bounds.Max.Y + SearchReach)
		{
			return false;
		}
		// Only a segment nearer than the reach can answer, so the search starts there: a block farther than the reach
		// cannot hold the nearest segment of a point the polyline matters at.
		const FVector2D P(X, Y);
		double Best = SearchReach * SearchReach;
		bool bFound = false;
		for (int32 Block = 0; Block < BlockBounds.Num(); ++Block)
		{
			const FBox2D& Box = BlockBounds[Block];
			if (X < Box.Min.X - SearchReach || X > Box.Max.X + SearchReach || Y < Box.Min.Y - SearchReach || Y > Box.Max.Y + SearchReach)
			{
				continue;
			}
			const int32 First = Block * BlockSegments;
			const int32 Last = FMath::Min(First + BlockSegments, Segments);
			for (int32 Segment = First; Segment < Last; ++Segment)
			{
				const FVector& A = PositionOf(Segment);
				const FVector& B = PositionOf(Segment + 1);
				double T = 0.0;
				const double DistSq = NearestOnSegment(P, FVector2D(A.X, A.Y), FVector2D(B.X, B.Y), T);
				// Strictly nearer: of two segments at the same distance the earlier one along the polyline answers.
				if (DistSq < Best)
				{
					Best = DistSq;
					OutSegment = Segment;
					OutT = T;
					bFound = true;
				}
			}
		}
		OutDistSq = Best;
		return bFound;
	}

	/**
	 * The nearest segment of a planet polyline to a unit direction, as FindNearest: the distance is the chord between
	 * the direction's point and the segment's on the datum sphere (world units, squared).
	 *
	 * @param Point Unit direction from the planet centre
	 * @param PlanetRadius The datum radius
	 * @param SearchReach Only a segment nearer than this (world units) is found
	 * @param PositionOf A vertex's unit direction by index
	 * @return False when no segment is within SearchReach (or for a heightmap polyline)
	 */
	template <typename PositionOfType>
	bool FindNearestOnSphere(const FVector& Point, double PlanetRadius, double SearchReach, PositionOfType&& PositionOf, int32& OutSegment,
		double& OutT, double& OutDistSq) const
	{
		if (!bRadial || Segments == 0 || PlanetRadius <= 0.0)
		{
			return false;
		}
		// On the unit sphere: a point's chord to a ball's centre is at most the ball's radius plus its chord to the nearest
		// segment, and a chord is no longer than its arc, so a ball farther than its radius plus the reach holds no segment
		// within reach.
		const double ReachChord = SearchReach / PlanetRadius;
		if ((Point - CapCentre).SizeSquared() > FMath::Square(CapChordRadius + ReachChord))
		{
			return false;
		}
		double Best = ReachChord * ReachChord;
		bool bFound = false;
		for (int32 Block = 0; Block < BlockBalls.Num(); ++Block)
		{
			const FVector4d& Ball = BlockBalls[Block];
			if ((Point - FVector(Ball.X, Ball.Y, Ball.Z)).SizeSquared() > FMath::Square(Ball.W + ReachChord))
			{
				continue;
			}
			const int32 First = Block * BlockSegments;
			const int32 Last = FMath::Min(First + BlockSegments, Segments);
			for (int32 Segment = First; Segment < Last; ++Segment)
			{
				double T = 0.0;
				const double DistSq = NearestOnSegment(Point, PositionOf(Segment), PositionOf(Segment + 1), T);
				if (DistSq < Best)
				{
					Best = DistSq;
					OutSegment = Segment;
					OutT = T;
					bFound = true;
				}
			}
		}
		// A chord on the datum sphere, in world units.
		OutDistSq = Best * PlanetRadius * PlanetRadius;
		return bFound;
	}
};
