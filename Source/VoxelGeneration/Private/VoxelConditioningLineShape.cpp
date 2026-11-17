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
		const TArray<double> Before = Result.Bed;
		for (int32 Index = Num - 2; Index >= 0; --Index)
		{
			const double Most = MaxStep(Index);
			if (Most < TNumericLimits<double>::Max())
			{
				Result.Bed[Index] = FMath::Min(Result.Bed[Index], Result.Bed[Index + 1] + Most);
			}
		}
		for (int32 Index = 1; Index < Num; ++Index)
		{
			const double Most = MaxStep(Index - 1);
			if (Most < TNumericLimits<double>::Max())
			{
				Result.Bed[Index] = FMath::Min(Result.Bed[Index], Result.Bed[Index - 1] + Most);
			}
		}
		for (int32 Index = 0; Index < Num; ++Index)
		{
			Result.Bed[Index] = FMath::Max(Result.Bed[Index], Before[Index] - FMath::Max(Params.MaxBendDeepening, 0.0) * Params.Depth * Result.Taper[Index]);
			if (Before[Index] - Result.Bed[Index] > UE_DOUBLE_KINDA_SMALL_NUMBER)
			{
				++Result.NumBendHolds;
				Result.MostBendHold = FMath::Max(Result.MostBendHold, Before[Index] - Result.Bed[Index]);
			}
		}
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
