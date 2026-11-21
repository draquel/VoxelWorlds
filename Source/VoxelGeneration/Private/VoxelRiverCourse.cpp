// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelRiverCourse.h"
#include <limits>

double FVoxelRiverCourse::GetBodyLevelAt(const FVoxelWaterLevels& Water, const FVector& Place, double GroundHeight)
{
	const FVoxelWaterFrame& Frame = Water.GetFrame();
	const double LevelBase = Frame.bRadial ? Frame.DatumRadius : 0.0;
	double Best = std::numeric_limits<double>::quiet_NaN();
	const auto Take = [&Best](double Level) { Best = FMath::IsNaN(Best) ? Level : FMath::Max(Best, Level); };
	if (Water.GetSea().bEnabled)
	{
		const FVector GroundPoint = Frame.bRadial ? Frame.Center + Place * (Frame.DatumRadius + GroundHeight) : FVector(Place.X, Place.Y, GroundHeight);
		const double Depth = Water.GetSea().GetDepth(GroundPoint);
		if (Depth > 0.0)
		{
			Take(GroundHeight + Depth);
		}
	}
	for (const FVoxelWaterBody& Body : Water.GetBodies())
	{
		if (Body.Kind == EVoxelWaterBodyKind::Lake && Body.Lake.IsValid())
		{
			if ((Frame.bRadial ? Body.Lake->CoversDirection(Place) : Body.Lake->Covers(Place.X, Place.Y)) && GroundHeight + LevelBase < Body.Level)
			{
				Take(Body.Level - LevelBase);
			}
		}
		else if (Body.Kind == EVoxelWaterBodyKind::Reach && Body.Reach.IsValid())
		{
			FVoxelWaterReachSample Sample;
			if (Frame.bRadial ? Body.Reach->SampleOnSphere(Place, Frame.DatumRadius, Sample) : Body.Reach->SampleAt(Place.X, Place.Y, Sample))
			{
				Take(Sample.SurfaceLevel - LevelBase);
			}
		}
	}
	return Best;
}

void FVoxelRiverCourse::ReadBanks(TConstArrayView<FVector> Points, TConstArrayView<FVector> Ups, TConstArrayView<double> Floors, double BankWidth, double VoxelSize,
	double BankMargin, TFunctionRef<double(const FVector& Place)> Ground, TFunctionRef<double(const FVector& Place, double GroundHeight)> BodyLevel,
	TArray<double>& OutBankGround)
{
	const int32 Num = Points.Num();
	const bool bPlanet = Ups.Num() >= Num && Num > 0;
	OutBankGround.Reset(Num);
	const int32 Steps = FMath::Max(1, FMath::CeilToInt32(BankWidth / FMath::Max(VoxelSize, 1.0)));
	for (int32 Index = 0; Index < Num; ++Index)
	{
		// The way across the river here, along the surface.
		const FVector Up = bPlanet ? Ups[Index] : FVector::UpVector;
		FVector AlongLine = Points[FMath::Min(Index + 1, Num - 1)] - Points[FMath::Max(Index - 1, 0)];
		AlongLine -= Up * FVector::DotProduct(AlongLine, Up);
		const FVector Across = FVector::CrossProduct(Up, AlongLine.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::ForwardVector));
		const double Floor = Floors.IsValidIndex(Index) ? Floors[Index] : 0.0;
		double Lowest = TNumericLimits<double>::Max();
		for (int32 Step = 0; Step <= Steps; ++Step)
		{
			const double Distance = Floor + BankWidth * Step / Steps;
			for (const double Side : { -1.0, 1.0 })
			{
				FVector Place = Points[Index] + Across * (Side * Distance);
				if (bPlanet)
				{
					Place = Place.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, Up);
				}
				double Bank = Ground(Place);
				const double Level = BodyLevel(Place, Bank);
				if (!FMath::IsNaN(Level))
				{
					Bank = FMath::Max(Bank, Level + BankMargin);
				}
				Lowest = FMath::Min(Lowest, Bank);
			}
		}
		OutBankGround.Add(Lowest);
	}
}
