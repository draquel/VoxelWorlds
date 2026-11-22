// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelConditioningLineShape.h"
#include "VoxelConditioningLine.h"

namespace VoxelConditioningLineShapePrivate
{
	/**
	 * Each value replaced by the mean of the values within Reach of it along the list, the window narrowing toward the
	 * two ends so that they stay where they are.
	 */
	template <typename T>
	void AverageAlong(TConstArrayView<T> In, int32 Reach, TArray<T>& Out)
	{
		const int32 Num = In.Num();
		Out.SetNum(Num);
		if (Reach <= 0)
		{
			for (int32 Index = 0; Index < Num; ++Index)
			{
				Out[Index] = In[Index];
			}
			return;
		}
		// Prefix sums: a window is two lookups.
		TArray<T> Sums;
		Sums.SetNum(Num + 1);
		Sums[0] = T(0.0);
		for (int32 Index = 0; Index < Num; ++Index)
		{
			Sums[Index + 1] = Sums[Index] + In[Index];
		}
		for (int32 Index = 0; Index < Num; ++Index)
		{
			const int32 Window = FMath::Min3(Reach, Index, Num - 1 - Index);
			Out[Index] = (Sums[Index + Window + 1] - Sums[Index - Window]) / static_cast<double>(2 * Window + 1);
		}
	}

	/**
	 * ResampleEvenly's sampling with a value carried along: each point made gets the value interpolated where it lies
	 * (FitCornerArcs' places). Its own copy, so that ResampleEvenly, which the roads use, stays as it is.
	 */
	void ResampleEvenlyWithValues(TConstArrayView<FVector> Points, TConstArrayView<double> Values, double Spacing, TArray<FVector>& Out, TArray<double>& OutValues)
	{
		Out.Reset();
		OutValues.Reset();
		if (Points.Num() == 0)
		{
			return;
		}
		const double Length = FVoxelConditioningLineShape::GetLength(Points);
		if (Points.Num() == 1 || Length <= UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			Out.Add(Points[0]);
			OutValues.Add(Values[0]);
			return;
		}
		const int32 Steps = FMath::Max(1, FMath::CeilToInt32(Length / FMath::Max(Spacing, 1.0) - 0.01));
		const double Step = Length / Steps;
		Out.Reserve(Steps + 1);
		OutValues.Reserve(Steps + 1);
		Out.Add(Points[0]);
		OutValues.Add(Values[0]);
		int32 Segment = 1;
		double SegmentStart = 0.0;
		double SegmentLength = FVector::Dist(Points[0], Points[1]);
		for (int32 Index = 1; Index < Steps; ++Index)
		{
			const double At = Index * Step;
			while (Segment < Points.Num() - 1 && At > SegmentStart + SegmentLength)
			{
				SegmentStart += SegmentLength;
				++Segment;
				SegmentLength = FVector::Dist(Points[Segment - 1], Points[Segment]);
			}
			const double T = SegmentLength > UE_DOUBLE_SMALL_NUMBER ? FMath::Clamp((At - SegmentStart) / SegmentLength, 0.0, 1.0) : 0.0;
			Out.Add(FMath::Lerp(Points[Segment - 1], Points[Segment], T));
			OutValues.Add(FMath::Lerp(Values[Segment - 1], Values[Segment], T));
		}
		Out.Add(Points.Last());
		OutValues.Add(Values.Last());
	}

	/** @return The distance from a point to the nearest segment of a polyline */
	double DistanceToPolyline(const FVector& Point, TConstArrayView<FVector> Polyline)
	{
		if (Polyline.Num() == 1)
		{
			return FVector::Dist(Point, Polyline[0]);
		}
		double Best = TNumericLimits<double>::Max();
		for (int32 Index = 1; Index < Polyline.Num(); ++Index)
		{
			Best = FMath::Min(Best, FVector::DistSquared(Point, FMath::ClosestPointOnSegment(Point, Polyline[Index - 1], Polyline[Index])));
		}
		return Best < TNumericLimits<double>::Max() ? FMath::Sqrt(Best) : 0.0;
	}

	/** One corner of FitCornerArcs: a run of vertices replaced by an arc, and the arc. */
	struct FArcCorner
	{
		/** The first and last vertex replaced; the vertices either side (First - 1 and Last + 1) stay */
		int32 First = 0;
		int32 Last = 0;

		/** The axis the corner turns about (the cross product of the directions in and out, normalized) */
		FVector Axis = FVector::ZeroVector;

		/** Left as it was (see FCornerArcs::NumLeft) */
		bool bLeft = false;

		/** The arc: from Start to End about Centre, turning about Axis */
		FVector Start = FVector::ZeroVector;
		FVector End = FVector::ZeroVector;
		FVector Centre = FVector::ZeroVector;
	};

	/** What fitting a corner's arc asked for. */
	enum class EArcFit : uint8
	{
		/** The arc fits: Start, End and Centre are set */
		Fitted,
		/** The corner must take in more vertices, on one side or both (GrowFirst, GrowLast) */
		Grow,
		/** No arc of the radius rounds it (FCornerArcs::NumLeft) */
		Left,
	};

	/**
	 * Fit the arc of a corner: tangent to the line through the vertex before it (along its segment into the corner) and
	 * to the line through the vertex after it (along its segment out of the corner), or, where the corner reaches an end
	 * of the polyline, through that end.
	 */
	EArcFit FitArc(TConstArrayView<FVector> Points, double Radius, FArcCorner& Corner, bool& bGrowFirst, bool& bGrowLast)
	{
		bGrowFirst = false;
		bGrowLast = false;
		const int32 Num = Points.Num();
		const FVector& Before = Points[Corner.First - 1];
		const FVector& After = Points[Corner.Last + 1];
		const FVector In = (Points[Corner.First] - Before).GetSafeNormal();
		const FVector Out = (After - Points[Corner.Last]).GetSafeNormal();
		const double Turn = FMath::Atan2(FVector::CrossProduct(In, Out).Size(), FVector::DotProduct(In, Out));
		if (Turn < UE_DOUBLE_KINDA_SMALL_NUMBER || Turn > FMath::DegreesToRadians(FVoxelConditioningLineShape::MaxArcTurnDegrees))
		{
			return EArcFit::Left;
		}
		const FVector Axis = FVector::CrossProduct(In, Out).GetSafeNormal();
		Corner.Axis = Axis;

		// Where the line in and the line out meet (the nearest points of the two, which a planet's sampled points on the
		// sphere leave a little apart): the corner lies Along from the vertex before it and Ahead of the vertex after it.
		const FVector Between = Before - After;
		const double Cos = FVector::DotProduct(In, Out);
		const double D = FVector::DotProduct(In, Between);
		const double E = FVector::DotProduct(Out, Between);
		const double Denominator = 1.0 - Cos * Cos;
		const double Along = (Cos * E - D) / Denominator;
		const double Ahead = -(E - Cos * D) / Denominator;
		const double Tangent = Radius * FMath::Tan(0.5 * Turn);
		const bool bShortFirst = Along < Tangent - UE_DOUBLE_KINDA_SMALL_NUMBER;
		const bool bShortLast = Ahead < Tangent - UE_DOUBLE_KINDA_SMALL_NUMBER;
		const bool bFirstAtEnd = Corner.First == 1;
		const bool bLastAtEnd = Corner.Last == Num - 2;

		// Toward the inside of the corner from the line in, and from the line out.
		const FVector InsideIn = FVector::CrossProduct(Axis, In);
		const FVector InsideOut = FVector::CrossProduct(Axis, Out);
		if (!bShortFirst && !bShortLast)
		{
			Corner.Start = Before + In * (Along - Tangent);
			Corner.End = After - Out * (Ahead - Tangent);
			Corner.Centre = Corner.Start + InsideIn * Radius;
			return EArcFit::Fitted;
		}
		if ((bShortFirst && !bFirstAtEnd) || (bShortLast && !bLastAtEnd))
		{
			bGrowFirst = bShortFirst && !bFirstAtEnd;
			bGrowLast = bShortLast && !bLastAtEnd;
			return EArcFit::Grow;
		}

		// Near both ends: an arc of the radius through the two, on the corner's side. Ends over two radii apart (a short
		// line that turns over 90 degrees) are left.
		const auto ThroughBothEnds = [&]()
		{
			const double Chord = FVector::Dist(Before, After);
			if (Chord > 2.0 * Radius)
			{
				return EArcFit::Left;
			}
			const FVector ChordDirection = (After - Before).GetSafeNormal();
			Corner.Start = Before;
			Corner.End = After;
			Corner.Centre = 0.5 * (Before + After) + FVector::CrossProduct(Axis, ChordDirection) * FMath::Sqrt(FMath::Max(Radius * Radius - 0.25 * Chord * Chord, 0.0));
			return EArcFit::Fitted;
		};
		if (bShortFirst && bShortLast)
		{
			return ThroughBothEnds();
		}
		if (bShortFirst)
		{
			// Near the start: the arc of the radius through the start, tangent to the line out, its centre at the radius
			// inside that line. Its tangent point must lie before the vertex after the corner.
			const double StartAlong = FVector::DotProduct(Before - After, Out);
			const double StartInside = FVector::DotProduct(Before - After, InsideOut);
			if (StartInside < 0.0 || StartInside > 2.0 * Radius)
			{
				return EArcFit::Left;
			}
			const double CentreAlong = StartAlong + FMath::Sqrt(FMath::Max(Radius * Radius - FMath::Square(Radius - StartInside), 0.0));
			if (CentreAlong > UE_DOUBLE_KINDA_SMALL_NUMBER)
			{
				if (bLastAtEnd)
				{
					return ThroughBothEnds();
				}
				bGrowLast = true;
				return EArcFit::Grow;
			}
			Corner.Start = Before;
			Corner.End = After + Out * CentreAlong;
			Corner.Centre = Corner.End + InsideOut * Radius;
			return EArcFit::Fitted;
		}
		// Near the end: the arc of the radius through the end, tangent to the line in.
		const double EndAlong = FVector::DotProduct(After - Before, In);
		const double EndInside = FVector::DotProduct(After - Before, InsideIn);
		if (EndInside < 0.0 || EndInside > 2.0 * Radius)
		{
			return EArcFit::Left;
		}
		const double CentreAlong = EndAlong - FMath::Sqrt(FMath::Max(Radius * Radius - FMath::Square(Radius - EndInside), 0.0));
		if (CentreAlong < -UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			if (bFirstAtEnd)
			{
				return ThroughBothEnds();
			}
			bGrowFirst = true;
			return EArcFit::Grow;
		}
		Corner.Start = Before + In * CentreAlong;
		Corner.End = After;
		Corner.Centre = Corner.Start + InsideIn * Radius;
		return EArcFit::Fitted;
	}

	/**
	 * The points of an arc from Start to End about Centre, turning about Axis, a Density or less apart. Where the two
	 * ends are not quite at the radius from the centre (a planet's corner, whose two lines do not quite meet), the
	 * difference is spread along the arc, so it starts and ends on them exactly.
	 */
	void SampleArc(const FArcCorner& Arc, double Radius, double Density, TArray<FVector>& Out)
	{
		Out.Reset();
		const FVector U = (Arc.Start - Arc.Centre).GetSafeNormal();
		const FVector V = FVector::CrossProduct(Arc.Axis, U);
		const FVector UEnd = (Arc.End - Arc.Centre).GetSafeNormal();
		double Sweep = FMath::Atan2(FVector::DotProduct(FVector::CrossProduct(U, UEnd), Arc.Axis), FVector::DotProduct(U, UEnd));
		if (Sweep < 0.0)
		{
			Sweep += 2.0 * UE_DOUBLE_PI;
		}
		const int32 Steps = FMath::Max(2, FMath::CeilToInt32(Sweep * Radius / FMath::Max(Density, 1.0)));
		const FVector StartFix = Arc.Start - (Arc.Centre + U * Radius);
		const FVector EndFix = Arc.End - (Arc.Centre + (U * FMath::Cos(Sweep) + V * FMath::Sin(Sweep)) * Radius);
		Out.Reserve(Steps + 1);
		for (int32 Index = 0; Index <= Steps; ++Index)
		{
			const double Fraction = static_cast<double>(Index) / Steps;
			const double Angle = Sweep * Fraction;
			Out.Add(Arc.Centre + (U * FMath::Cos(Angle) + V * FMath::Sin(Angle)) * Radius + StartFix * (1.0 - Fraction) + EndFix * Fraction);
		}
	}
}

double FVoxelConditioningLineShape::GetLength(TConstArrayView<FVector> Points)
{
	double Length = 0.0;
	for (int32 Index = 1; Index < Points.Num(); ++Index)
	{
		Length += FVector::Dist(Points[Index - 1], Points[Index]);
	}
	return Length;
}

void FVoxelConditioningLineShape::ResampleEvenly(TConstArrayView<FVector> Points, double Spacing, TArray<FVector>& Out)
{
	Out.Reset();
	if (Points.Num() == 0)
	{
		return;
	}
	const double Length = GetLength(Points);
	if (Points.Num() == 1 || Length <= UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		Out.Add(Points[0]);
		return;
	}
	const int32 Steps = FMath::Max(1, FMath::CeilToInt32(Length / FMath::Max(Spacing, 1.0) - 0.01));
	const double Step = Length / Steps;
	Out.Reserve(Steps + 1);
	Out.Add(Points[0]);
	int32 Segment = 1;
	double SegmentStart = 0.0;
	double SegmentLength = FVector::Dist(Points[0], Points[1]);
	for (int32 Index = 1; Index < Steps; ++Index)
	{
		const double At = Index * Step;
		while (Segment < Points.Num() - 1 && At > SegmentStart + SegmentLength)
		{
			SegmentStart += SegmentLength;
			++Segment;
			SegmentLength = FVector::Dist(Points[Segment - 1], Points[Segment]);
		}
		const double T = SegmentLength > UE_DOUBLE_SMALL_NUMBER ? FMath::Clamp((At - SegmentStart) / SegmentLength, 0.0, 1.0) : 0.0;
		Out.Add(FMath::Lerp(Points[Segment - 1], Points[Segment], T));
	}
	Out.Add(Points.Last());
}

void FVoxelConditioningLineShape::SpreadCorners(TConstArrayView<FVector> Points, double Spacing, double Distance, TArray<FVector>& Out)
{
	const int32 Reach = FMath::RoundToInt32(Distance / FMath::Max(Spacing, 1.0));
	TArray<FVector> Once;
	VoxelConditioningLineShapePrivate::AverageAlong<FVector>(Points, Reach, Once);
	VoxelConditioningLineShapePrivate::AverageAlong<FVector>(Once, Reach, Out);
}

double FVoxelConditioningLineShape::GetTurnAt(TConstArrayView<FVector> Points, int32 Index)
{
	if (Index <= 0 || Index + 1 >= Points.Num())
	{
		return 0.0;
	}
	const FVector In = (Points[Index] - Points[Index - 1]).GetSafeNormal();
	const FVector Out = (Points[Index + 1] - Points[Index]).GetSafeNormal();
	return FMath::Atan2(FVector::CrossProduct(In, Out).Size(), FVector::DotProduct(In, Out));
}

double FVoxelConditioningLineShape::GetWorstTurn(TConstArrayView<FVector> Points)
{
	double Worst = 0.0;
	for (int32 Index = 1; Index + 1 < Points.Num(); ++Index)
	{
		Worst = FMath::Max(Worst, GetTurnAt(Points, Index));
	}
	return Worst;
}

double FVoxelConditioningLineShape::GetMaxTurnForReach(double Spacing, double HeightReach)
{
	return HeightReach > UE_DOUBLE_SMALL_NUMBER ? 2.0 * FMath::Atan(Spacing / (2.0 * HeightReach)) : UE_DOUBLE_PI;
}

double FVoxelConditioningLineShape::GetMaxTurnForGrade(double Grade, float HalfWidth, float FalloffWidth, double VoxelSize, float Strength)
{
	const double Peak = static_cast<double>(Strength) * FVoxelConditioningLine::PeakOfDistanceTimesWeight(HalfWidth, FalloffWidth);
	const double Product = 2.0 * FMath::Abs(Grade) * Peak;
	// 2 x grade x tan(turn / 2) x peak <= a tenth of a voxel.
	return Product > UE_DOUBLE_SMALL_NUMBER ? FMath::Min(2.0 * FMath::Atan(0.1 * VoxelSize / Product), UE_DOUBLE_PI) : UE_DOUBLE_PI;
}

double FVoxelConditioningLineShape::GetMaxGradeForTurn(double Turn, float HalfWidth, float FalloffWidth, double VoxelSize, float Strength)
{
	const double Peak = static_cast<double>(Strength) * FVoxelConditioningLine::PeakOfDistanceTimesWeight(HalfWidth, FalloffWidth);
	const double HalfTan = FMath::Tan(FMath::Min(0.5 * FMath::Abs(Turn), 0.49 * UE_DOUBLE_PI));
	const double Product = 2.0 * HalfTan * Peak;
	return Product > UE_DOUBLE_SMALL_NUMBER ? 0.1 * VoxelSize / Product : TNumericLimits<double>::Max();
}

FVoxelConditioningLineShape::FRounding FVoxelConditioningLineShape::RoundCorners(TConstArrayView<FVector> Points, double Spacing, double MaxTurn,
	double CornerDistance, TArray<FVector>& Out, int32 MaxTries)
{
	FRounding Result;
	TArray<FVector> Dense;
	ResampleEvenly(Points, Spacing, Dense);
	Out = Dense;
	Result.WorstTurn = GetWorstTurn(Out);
	Result.bWithinBound = Result.WorstTurn <= BoundMargin * MaxTurn;
	double Distance = FMath::Max(CornerDistance, Spacing);
	TArray<FVector> Spread;
	while (!Result.bWithinBound && Result.Tries < MaxTries)
	{
		SpreadCorners(Dense, Spacing, Distance, Spread);
		Result.FarthestMove = 0.0;
		for (int32 Index = 0; Index < Dense.Num(); ++Index)
		{
			Result.FarthestMove = FMath::Max(Result.FarthestMove, FVector::Dist(Dense[Index], Spread[Index]));
		}
		ResampleEvenly(Spread, Spacing, Out);
		Result.CornerDistance = Distance;
		Result.WorstTurn = GetWorstTurn(Out);
		Result.bWithinBound = Result.WorstTurn <= BoundMargin * MaxTurn;
		++Result.Tries;
		Distance *= 1.5;
	}
	return Result;
}

double FVoxelConditioningLineShape::GetCornerRadius(double Spacing, double MaxTurn)
{
	const double Turn = BoundMargin * MaxTurn;
	return Turn > UE_DOUBLE_SMALL_NUMBER ? Spacing / Turn : TNumericLimits<double>::Max();
}

FVoxelConditioningLineShape::FCornerArcs FVoxelConditioningLineShape::FitCornerArcs(TConstArrayView<FVector> Points, double Spacing, double Radius,
	TArray<FVector>& Out, TArray<double>* OutPlaces)
{
	using namespace VoxelConditioningLineShapePrivate;
	FCornerArcs Result;
	const int32 Num = Points.Num();
	const auto Unchanged = [&]()
	{
		Out = TArray<FVector>(Points);
		if (OutPlaces)
		{
			OutPlaces->SetNumUninitialized(Num);
			for (int32 Index = 0; Index < Num; ++Index)
			{
				(*OutPlaces)[Index] = Index;
			}
		}
		Result.WorstTurn = GetWorstTurn(Points);
		return Result;
	};
	if (Num < 3 || !(Radius > UE_DOUBLE_SMALL_NUMBER) || Radius >= TNumericLimits<double>::Max())
	{
		return Unchanged();
	}

	// The corners: runs of vertices that turn the same way by more than an arc of the radius does over their segments.
	TArray<FArcCorner> Corners;
	for (int32 Index = 1; Index + 1 < Num; ++Index)
	{
		const double Chord = FMath::Min(FVector::Dist(Points[Index - 1], Points[Index]), FVector::Dist(Points[Index], Points[Index + 1]));
		const double ArcTurn = 2.0 * FMath::Asin(FMath::Min(0.5 * Chord / Radius, 1.0));
		if (GetTurnAt(Points, Index) <= ArcTurn * (1.0 + 1.0e-9))
		{
			continue;
		}
		const FVector Axis = FVector::CrossProduct(Points[Index] - Points[Index - 1], Points[Index + 1] - Points[Index]).GetSafeNormal();
		if (Corners.Num() > 0 && Corners.Last().Last == Index - 1 && FVector::DotProduct(Corners.Last().Axis, Axis) > 0.0)
		{
			Corners.Last().Last = Index;
		}
		else
		{
			FArcCorner& Corner = Corners.AddDefaulted_GetRef();
			Corner.First = Index;
			Corner.Last = Index;
			Corner.Axis = Axis;
		}
	}
	if (Corners.Num() == 0)
	{
		return Unchanged();
	}

	// Widen each corner a vertex at a time until its arc fits, merging corners that come to overlap: two that turn the
	// same way become one, two that turn opposite ways (an S too tight for two arcs) are left. Each round widens a corner
	// or ends, so it ends within the polyline's length.
	for (int32 Round = 0; Round < 4 * Num; ++Round)
	{
		bool bChanged = false;
		for (FArcCorner& Corner : Corners)
		{
			if (Corner.bLeft)
			{
				continue;
			}
			bool bGrowFirst = false, bGrowLast = false;
			const EArcFit Fit = FitArc(Points, Radius, Corner, bGrowFirst, bGrowLast);
			if (Fit == EArcFit::Left)
			{
				Corner.bLeft = true;
			}
			else if (Fit == EArcFit::Grow)
			{
				Corner.First -= bGrowFirst ? 1 : 0;
				Corner.Last += bGrowLast ? 1 : 0;
				bChanged = true;
			}
		}
		TArray<FArcCorner> Merged;
		Merged.Reserve(Corners.Num());
		for (FArcCorner& Corner : Corners)
		{
			// A corner keeps the vertex either side of it: two corners overlap when fewer than two vertices lie between.
			if (Merged.Num() > 0 && Corner.First - Merged.Last().Last < 2)
			{
				FArcCorner& Previous = Merged.Last();
				const bool bBothFitting = !Previous.bLeft && !Corner.bLeft;
				if (bBothFitting && FVector::DotProduct(Previous.Axis, Corner.Axis) > 0.0)
				{
					Previous.Last = FMath::Max(Previous.Last, Corner.Last);
					bChanged = true;
					continue;
				}
				bChanged |= !(Previous.bLeft && Corner.bLeft);
				Previous.bLeft = true;
				Previous.First = FMath::Min(Previous.First, Corner.First);
				Previous.Last = FMath::Max(Previous.Last, Corner.Last);
				continue;
			}
			Merged.Add(Corner);
		}
		Corners = MoveTemp(Merged);
		if (!bChanged)
		{
			break;
		}
	}
	// Every arc as its corner now is (the rounds end on one that changed nothing, so this is that round's fit).
	for (FArcCorner& Corner : Corners)
	{
		bool bGrowFirst = false, bGrowLast = false;
		Corner.bLeft = Corner.bLeft || FitArc(Points, Radius, Corner, bGrowFirst, bGrowLast) != EArcFit::Fitted;
	}

	// The polyline again: the points outside the corners, and each corner's arc between the vertices either side of it.
	// Each point carries its place along the polyline given, as a distance; an arc's points are spread evenly between the
	// places of its two tangent points (the vertex before the corner plus the way to the arc's start, the vertex after it
	// less the way from the arc's end).
	TArray<double> Distances;
	Distances.SetNumUninitialized(Num);
	Distances[0] = 0.0;
	for (int32 Index = 1; Index < Num; ++Index)
	{
		Distances[Index] = Distances[Index - 1] + FVector::Dist(Points[Index - 1], Points[Index]);
	}
	TArray<FVector> Made;
	TArray<double> Places;
	Made.Reserve(Num * 2);
	Places.Reserve(Num * 2);
	TArray<FVector> Arc;
	TArray<FVector> Around;
	TArray<FVector> Given;
	int32 Next = 0;
	for (const FArcCorner& Corner : Corners)
	{
		if (Corner.bLeft)
		{
			++Result.NumLeft;
			continue;
		}
		for (; Next < Corner.First; ++Next)
		{
			Made.Add(Points[Next]);
			Places.Add(Distances[Next]);
		}
		const FVector& Before = Points[Corner.First - 1];
		const FVector& After = Points[Corner.Last + 1];
		SampleArc(Corner, Radius, 0.125 * Spacing, Arc);
		double StartPlace = Distances[Corner.First - 1] + FVector::Dist(Before, Corner.Start);
		double EndPlace = Distances[Corner.Last + 1] - FVector::Dist(After, Corner.End);
		if (EndPlace < StartPlace)
		{
			StartPlace = EndPlace = 0.5 * (StartPlace + EndPlace);
		}
		const double ArcLength = GetLength(Arc);
		double AlongArc = 0.0;
		for (int32 Index = 0; Index < Arc.Num(); ++Index)
		{
			AlongArc += Index > 0 ? FVector::Dist(Arc[Index - 1], Arc[Index]) : 0.0;
			if ((Index == 0 && FVector::Dist(Arc[0], Before) <= UE_DOUBLE_KINDA_SMALL_NUMBER)
				|| (Index == Arc.Num() - 1 && FVector::Dist(Arc.Last(), After) <= UE_DOUBLE_KINDA_SMALL_NUMBER))
			{
				continue;
			}
			Made.Add(Arc[Index]);
			Places.Add(ArcLength > UE_DOUBLE_SMALL_NUMBER ? FMath::Lerp(StartPlace, EndPlace, AlongArc / ArcLength) : StartPlace);
		}
		Next = Corner.Last + 1;
		++Result.NumArcs;

		// How far the corner moved, both ways: the line made around the corner (the vertex before it, the arc, the vertex
		// after it) against the polyline given there, and the polyline given, read every eighth of a spacing, against it.
		Around.Reset();
		Around.Add(Before);
		Around.Append(Arc);
		Around.Add(After);
		Given.Reset();
		for (int32 Index = FMath::Max(Corner.First - 2, 0); Index <= FMath::Min(Corner.Last + 2, Num - 1); ++Index)
		{
			Given.Add(Points[Index]);
		}
		for (const FVector& Point : Around)
		{
			Result.FarthestMove = FMath::Max(Result.FarthestMove, DistanceToPolyline(Point, Given));
		}
		for (int32 Index = Corner.First - 1; Index <= Corner.Last; ++Index)
		{
			const int32 Pieces = FMath::Max(1, FMath::CeilToInt32(8.0 * FVector::Dist(Points[Index], Points[Index + 1]) / FMath::Max(Spacing, 1.0)));
			for (int32 Piece = 0; Piece <= Pieces; ++Piece)
			{
				Result.FarthestMove = FMath::Max(Result.FarthestMove, DistanceToPolyline(FMath::Lerp(Points[Index], Points[Index + 1], static_cast<double>(Piece) / Pieces), Around));
			}
		}
	}
	if (Result.NumArcs == 0)
	{
		// Nothing could be rounded: the polyline as it was, not sampled again.
		return Unchanged();
	}
	for (; Next < Num; ++Next)
	{
		Made.Add(Points[Next]);
		Places.Add(Distances[Next]);
	}

	// Sampled evenly, and the places from distances to points.
	TArray<double> MadePlaces;
	ResampleEvenlyWithValues(Made, Places, Spacing, Out, MadePlaces);
	if (OutPlaces)
	{
		OutPlaces->SetNumUninitialized(Out.Num());
		int32 Segment = 0;
		for (int32 Index = 0; Index < Out.Num(); ++Index)
		{
			const double Place = FMath::Clamp(MadePlaces[Index], 0.0, Distances.Last());
			while (Segment < Num - 2 && Place > Distances[Segment + 1])
			{
				++Segment;
			}
			const double SegmentLength = Distances[Segment + 1] - Distances[Segment];
			(*OutPlaces)[Index] = Segment + (SegmentLength > UE_DOUBLE_SMALL_NUMBER ? FMath::Clamp((Place - Distances[Segment]) / SegmentLength, 0.0, 1.0) : 0.0);
		}
		(*OutPlaces)[0] = 0.0;
		OutPlaces->Last() = Num - 1;
	}
	Result.WorstTurn = GetWorstTurn(Out);
	return Result;
}

FVoxelConditioningLineShape::FProfile FVoxelConditioningLineShape::MakeProfile(TConstArrayView<FVector> Points, TConstArrayView<double> Ground,
	const FProfileParams& Params, TArray<double>& OutTargets)
{
	FProfile Result;
	const int32 Num = FMath::Min(Points.Num(), Ground.Num());
	OutTargets.Reset();
	if (Num == 0)
	{
		return Result;
	}
	if (Num == 1)
	{
		OutTargets.Add(Params.StartHeight.Get(Ground[0]));
		return Result;
	}

	TArray<double> Lengths;
	Lengths.SetNumUninitialized(Num - 1);
	double Length = 0.0;
	for (int32 Index = 0; Index + 1 < Num; ++Index)
	{
		Lengths[Index] = FVector::Dist(Points[Index], Points[Index + 1]);
		Length += Lengths[Index];
	}

	// The steepest a segment may be: the grade asked for, and at a vertex that turns the grade the bend bound leaves
	// (the target steps by 2 x grade x d x tan(turn / 2) across a bend's bisector; times the weight, at its peak, that
	// stays under a tenth of a voxel).
	const double Limit = Params.MaxGrade > 0.0 ? Params.MaxGrade : TNumericLimits<double>::Max();
	const double Peak = static_cast<double>(Params.Strength) * FVoxelConditioningLine::PeakOfDistanceTimesWeight(Params.HalfWidth, Params.FalloffWidth);
	TArray<double> VertexGrade;
	VertexGrade.Init(Limit, Num);
	for (int32 Index = 1; Index + 1 < Num; ++Index)
	{
		const double HalfTan = FMath::Tan(0.5 * GetTurnAt(Points.Left(Num), Index));
		if (HalfTan > UE_DOUBLE_SMALL_NUMBER && Peak > UE_DOUBLE_SMALL_NUMBER)
		{
			VertexGrade[Index] = FMath::Min(Limit, BoundMargin * 0.1 * Params.VoxelSize / (2.0 * HalfTan * Peak));
		}
	}

	// The ground averaged along the line, then held to those grades from the start, and from the end too when the end
	// has a height to meet. With no end height the line ends where the grades bring it: every step is within its limit
	// after the pass from the start, so a pass from the end would change nothing.
	const double Step = Length / (Num - 1);
	VoxelConditioningLineShapePrivate::AverageAlong<double>(Ground.Left(Num), FMath::RoundToInt32(Params.SmoothingDistance / FMath::Max(Step, 1.0)), OutTargets);
	const double Start = Params.StartHeight.Get(OutTargets[0]);
	auto MaxStep = [&](int32 Segment)
	{
		const double Grade = FMath::Min(VertexGrade[Segment], VertexGrade[Segment + 1]);
		return Grade < TNumericLimits<double>::Max() ? Grade * Lengths[Segment] : TNumericLimits<double>::Max();
	};
	OutTargets[0] = Start;
	for (int32 Index = 1; Index < Num; ++Index)
	{
		const double Most = MaxStep(Index - 1);
		if (Most < TNumericLimits<double>::Max())
		{
			OutTargets[Index] = FMath::Clamp(OutTargets[Index], OutTargets[Index - 1] - Most, OutTargets[Index - 1] + Most);
		}
	}
	if (Params.EndHeight.IsSet())
	{
		OutTargets[Num - 1] = Params.EndHeight.GetValue();
		for (int32 Index = Num - 2; Index >= 0; --Index)
		{
			const double Most = MaxStep(Index);
			if (Most < TNumericLimits<double>::Max())
			{
				OutTargets[Index] = FMath::Clamp(OutTargets[Index], OutTargets[Index + 1] - Most, OutTargets[Index + 1] + Most);
			}
		}
		// The pass from the end moved the start off its height when the two are further apart than the line can climb.
		Result.EndMiss = FMath::Abs(OutTargets[0] - Start);
		Result.bEndsMet = Result.EndMiss <= 1.0;
	}
	for (int32 Index = 0; Index + 1 < Num; ++Index)
	{
		if (Lengths[Index] > UE_DOUBLE_SMALL_NUMBER)
		{
			Result.WorstGrade = FMath::Max(Result.WorstGrade, FMath::Abs(OutTargets[Index + 1] - OutTargets[Index]) / Lengths[Index]);
		}
	}
	return Result;
}

int32 FVoxelConditioningLineShape::HoldBedThroughBends(TConstArrayView<FVector> Points, TConstArrayView<double> Taper, const FRiverProfileParams& Params,
	TArrayView<double> Bed, double* OutMostHold)
{
	const int32 Num = FMath::Min3(Points.Num(), Taper.Num(), Bed.Num());
	if (OutMostHold)
	{
		*OutMostHold = 0.0;
	}
	if (Params.VoxelSize <= 0.0 || Num < 3)
	{
		return 0;
	}
	TArray<double> Along;
	Along.SetNumUninitialized(Num);
	Along[0] = 0.0;
	for (int32 Index = 1; Index < Num; ++Index)
	{
		Along[Index] = Along[Index - 1] + FVector::Dist(Points[Index - 1], Points[Index]);
	}
	TArray<double> VertexGrade;
	VertexGrade.Init(TNumericLimits<double>::Max(), Num);
	for (int32 Index = 1; Index + 1 < Num; ++Index)
	{
		const double Turn = GetTurnAt(Points.Left(Num), Index);
		if (Turn > UE_DOUBLE_SMALL_NUMBER)
		{
			const double Grade = GetMaxGradeForTurn(Turn, Params.HalfWidth, Params.FalloffWidth, Params.VoxelSize);
			VertexGrade[Index] = Grade < TNumericLimits<double>::Max() ? BoundMargin * Grade : Grade;
		}
	}
	const auto MaxStep = [&](int32 Segment)
	{
		const double Grade = FMath::Min(VertexGrade[Segment], VertexGrade[Segment + 1]);
		return Grade < TNumericLimits<double>::Max() ? Grade * (Along[Segment + 1] - Along[Segment]) : TNumericLimits<double>::Max();
	};
	const TArray<double> Before(Bed.GetData(), Num);
	for (int32 Index = Num - 2; Index >= 0; --Index)
	{
		const double Most = MaxStep(Index);
		if (Most < TNumericLimits<double>::Max())
		{
			Bed[Index] = FMath::Min(Bed[Index], Bed[Index + 1] + Most);
		}
	}
	for (int32 Index = 1; Index < Num; ++Index)
	{
		const double Most = MaxStep(Index - 1);
		if (Most < TNumericLimits<double>::Max())
		{
			Bed[Index] = FMath::Min(Bed[Index], Bed[Index - 1] + Most);
		}
	}
	int32 Held = 0;
	for (int32 Index = 0; Index < Num; ++Index)
	{
		Bed[Index] = FMath::Max(Bed[Index], Before[Index] - FMath::Max(Params.MaxBendDeepening, 0.0) * Params.Depth * Taper[Index]);
		if (Before[Index] - Bed[Index] > UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			++Held;
			if (OutMostHold)
			{
				*OutMostHold = FMath::Max(*OutMostHold, Before[Index] - Bed[Index]);
			}
		}
	}
	return Held;
}

FVoxelConditioningLineShape::FRiverProfile FVoxelConditioningLineShape::MakeRiverProfile(TConstArrayView<FVector> Points, TConstArrayView<double> Ground,
	TConstArrayView<double> BankGround, TConstArrayView<double> Heights, TConstArrayView<double> BodyLevels, const FRiverProfileParams& Params)
{
	FRiverProfile Result;
	int32 Num = FMath::Min3(Points.Num(), Ground.Num(), BankGround.Num());
	const bool bFollow = Heights.Num() == 0;
	if (!bFollow)
	{
		Num = FMath::Min(Num, Heights.Num());
	}
	if (BodyLevels.Num() > 0)
	{
		Num = FMath::Min(Num, BodyLevels.Num());
	}
	if (Num < 2)
	{
		return Result;
	}
	const auto InBody = [&](int32 Index) { return BodyLevels.Num() > 0 && !FMath::IsNaN(BodyLevels[Index]); };

	// How far along the line each vertex is.
	TArray<double> Along;
	Along.SetNumUninitialized(Num);
	Along[0] = 0.0;
	for (int32 Index = 1; Index < Num; ++Index)
	{
		Along[Index] = Along[Index - 1] + FVector::Dist(Points[Index - 1], Points[Index]);
	}
	const double Length = Along[Num - 1];
	const double Step = Length / (Num - 1);

	// The surface asked for: the heights given, or the ground along the line, averaged and never over the ground
	// itself, less the margin. Inside another body the average takes that body's level for the ground (its floor is
	// not the river's), so a river comes to a shore at the water's level and not under it.
	TArray<double> Asked;
	if (bFollow)
	{
		TArray<double> Effective(Ground.Left(Num));
		for (int32 Index = 0; Index < Num; ++Index)
		{
			if (InBody(Index))
			{
				Effective[Index] = BodyLevels[Index] + Params.SurfaceUnderGround;
			}
		}
		VoxelConditioningLineShapePrivate::AverageAlong<double>(Effective, FMath::RoundToInt32(Params.SmoothingDistance / FMath::Max(Step, 1.0)), Asked);
		for (int32 Index = 0; Index < Num; ++Index)
		{
			Asked[Index] = FMath::Min(Asked[Index], Ground[Index]) - Params.SurfaceUnderGround;
		}
	}
	else
	{
		Asked = TArray<double>(Heights.Left(Num));
	}

	// Rules 1, 2 and 7, upstream to downstream: under the banks, a tuck under the body the vertex lies in, and never
	// over the vertex before. Only the heights given count as rises (a river that follows the ground cuts through a
	// rise of the ground: that is its cut, not a fault of the drawing).
	Result.Surface.SetNumUninitialized(Num);
	double Lowest = TNumericLimits<double>::Max();
	double LowestAsked = TNumericLimits<double>::Max();
	for (int32 Index = 0; Index < Num; ++Index)
	{
		double Level = Asked[Index];
		if (InBody(Index))
		{
			Level = BodyLevels[Index] - Params.BodyTuck;
		}
		else
		{
			if (!bFollow && Level > LowestAsked + UE_DOUBLE_KINDA_SMALL_NUMBER)
			{
				++Result.NumRises;
				Result.MostRise = FMath::Max(Result.MostRise, Level - LowestAsked);
			}
			LowestAsked = FMath::Min(LowestAsked, Level);
			const double Bank = BankGround[Index] - Params.BankMargin;
			if (FMath::Min(Level, Lowest) > Bank + UE_DOUBLE_KINDA_SMALL_NUMBER)
			{
				++Result.NumLowBanks;
				Result.MostLowBank = FMath::Max(Result.MostLowBank, FMath::Min(Level, Lowest) - Bank);
			}
			Level = FMath::Min(Level, Bank);
		}
		Lowest = FMath::Max(FMath::Min(Lowest, Level), Params.MinSurface);
		Result.Surface[Index] = Lowest;
	}

	// Rules 3 and 4: the depth and the width start from nothing at a head and thin to nothing at an end on land.
	Result.bStartsInBody = InBody(0);
	Result.bEndsInBody = InBody(Num - 1);
	Result.Taper.SetNumUninitialized(Num);
	Result.Bed.SetNumUninitialized(Num);
	for (int32 Index = 0; Index < Num; ++Index)
	{
		double Taper = 1.0;
		if (!Result.bStartsInBody && Params.HeadLength > UE_DOUBLE_SMALL_NUMBER)
		{
			Taper = FMath::Min(Taper, Along[Index] / Params.HeadLength);
		}
		if (!Result.bEndsInBody && Params.EndLength > UE_DOUBLE_SMALL_NUMBER)
		{
			Taper = FMath::Min(Taper, (Length - Along[Index]) / Params.EndLength);
		}
		Result.Taper[Index] = FMath::Clamp(Taper, 0.0, 1.0);
		Result.Bed[Index] = Result.Surface[Index] - Params.Depth * Result.Taper[Index];
	}

	// Rule 8: the bed's grade through a bend. Across a bend's bisector the line's target steps by
	// 2 x grade x d x tan(turn / 2); held under a tenth of a voxel at the weight's peak, that is a grade per vertex that
	// turns (MakeProfile's). A segment steeper than its two vertices allow has its higher end lowered: the pass from
	// the end deepens the bed upstream of a fall, the pass from the start deepens it downstream of a rise (an end on
	// land, where the bed comes up to the surface). The surface stays: the water there is deeper, by no more than a
	// share of the depth the river has there, so a head still starts from nothing and no bend digs a pit.
	if (Params.VoxelSize > 0.0)
	{
		Result.NumBendHolds = HoldBedThroughBends(Points.Left(Num), Result.Taper, Params, Result.Bed, &Result.MostBendHold);
	}

	// The bed's grade, where it is bounded (a block world: a block a column). A segment steeper than the bound has its
	// higher end lowered, from the end up and from the start down, with no limit of its own: the surface stays, the
	// water there is deeper, and the cut is the caller's to judge.
	if (Params.MaxBedGrade > 0.0)
	{
		const TArray<double> Before = Result.Bed;
		for (int32 Index = Num - 2; Index >= 0; --Index)
		{
			Result.Bed[Index] = FMath::Min(Result.Bed[Index], Result.Bed[Index + 1] + Params.MaxBedGrade * (Along[Index + 1] - Along[Index]));
		}
		for (int32 Index = 1; Index < Num; ++Index)
		{
			Result.Bed[Index] = FMath::Min(Result.Bed[Index], Result.Bed[Index - 1] + Params.MaxBedGrade * (Along[Index] - Along[Index - 1]));
		}
		for (int32 Index = 0; Index < Num; ++Index)
		{
			if (Before[Index] - Result.Bed[Index] > UE_DOUBLE_KINDA_SMALL_NUMBER)
			{
				++Result.NumGradeHolds;
				Result.MostGradeHold = FMath::Max(Result.MostGradeHold, Before[Index] - Result.Bed[Index]);
			}
		}
	}

	// Rules 5 and 7: of the vertices inside the body the river starts in, and inside the one it ends in, the few next
	// to the land are kept: the mesh ends under that body's surface, and the bed does not run on across its floor.
	const int32 Keep = FMath::Max(Params.BodyVertices, 1);
	int32 First = 0;
	int32 Last = Num - 1;
	if (Result.bStartsInBody)
	{
		int32 Run = 0;
		while (Run < Num && InBody(Run))
		{
			++Run;
		}
		First = FMath::Max(0, FMath::Min(Run, Num - 1) - Keep);
	}
	if (Result.bEndsInBody)
	{
		int32 Run = Num - 1;
		while (Run >= 0 && InBody(Run))
		{
			--Run;
		}
		Last = FMath::Min(Num - 1, FMath::Max(Run, 0) + Keep);
	}
	if (Last - First < 1)
	{
		// A river wholly inside another body: its two ends.
		First = 0;
		Last = Num - 1;
	}
	Result.First = First;
	Result.Num = Last - First + 1;
	Result.Fall = Result.Surface[First] - Result.Surface[Last];
	for (int32 Index = First; Index <= Last; ++Index)
	{
		if (!InBody(Index))
		{
			Result.DeepestCut = FMath::Max(Result.DeepestCut, Ground[Index] - Result.Bed[Index]);
		}
	}
	return Result;
}
