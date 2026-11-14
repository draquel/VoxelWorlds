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
