// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelLakeComponent.h"
#include "VoxelStreaming.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelBlockSurface.h"
#include "Components/SplineComponent.h"

bool UVoxelLakeComponent::BuildLake(const UVoxelChunkManager& Manager, FVoxelLakeBuild& Out) const
{
	Out = FVoxelLakeBuild();
	const USplineComponent* Spline = GetSpline();
	const UVoxelWorldConfiguration* Configuration = Manager.GetConfiguration();
	if (!Spline || !Configuration || !Manager.IsInitialized())
	{
		return false;
	}
	const double Length = Spline->GetSplineLength();
	if (Length <= UE_KINDA_SMALL_NUMBER)
	{
		return false;
	}
	const FVoxelWaterFrame Frame = Manager.GetWaterLevels().GetFrame();
	const bool bPlanet = Frame.bRadial;
	const double VoxelSize = Configuration->VoxelSize;
	// A line's height is a world Z or an altitude; a water level is a world Z or a distance from the planet centre.
	const double LevelBase = bPlanet ? Frame.DatumRadius : 0.0;
	// A world position's place: its column (X, Y, 0), or its direction from the planet centre.
	const auto PlaceOf = [&](const FVector& World)
	{
		return bPlanet ? (World - Frame.Center).GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector) : FVector(World.X, World.Y, 0.0);
	};

	// The outline: a place every voxel along the spline, and, when the spline is not a closed loop, along the straight
	// way back from its last point to its first.
	Out.bClosedLoop = Spline->IsClosedLoop();
	TArray<FVector> Places;
	const int32 Steps = FMath::Max(3, FMath::CeilToInt32(Length / FMath::Max(VoxelSize, 1.0)));
	Places.Reserve(Steps + 1);
	for (int32 Index = 0; Index < Steps + (Out.bClosedLoop ? 0 : 1); ++Index)
	{
		Places.Add(PlaceOf(Spline->GetLocationAtDistanceAlongSpline(static_cast<float>(Length * Index / Steps), ESplineCoordinateSpace::World)));
	}
	if (!Out.bClosedLoop)
	{
		const FVector Last = Spline->GetLocationAtDistanceAlongSpline(static_cast<float>(Length), ESplineCoordinateSpace::World);
		const FVector First = Spline->GetLocationAtDistanceAlongSpline(0.0f, ESplineCoordinateSpace::World);
		const int32 Back = FMath::FloorToInt32(FVector::Dist(Last, First) / FMath::Max(VoxelSize, 1.0));
		for (int32 Index = 1; Index < Back; ++Index)
		{
			Places.Add(PlaceOf(FMath::Lerp(Last, First, static_cast<double>(Index) / Back)));
		}
	}
	if (Places.Num() < 3)
	{
		return false;
	}

	// The natural ground along it.
	TArray<double> OutlineGround;
	OutlineGround.Reserve(Places.Num());
	Out.NumOutlinePlaces = Places.Num();
	Out.LowestOutlineGround = TNumericLimits<double>::Max();
	Out.HighestOutlineGround = TNumericLimits<double>::Lowest();
	for (const FVector& Place : Places)
	{
		const double Height = GetNaturalHeight(Manager, Place);
		OutlineGround.Add(Height);
		Out.LowestOutlineGround = FMath::Min(Out.LowestOutlineGround, Height);
		Out.HighestOutlineGround = FMath::Max(Out.HighestOutlineGround, Height);
	}

	// The level, in a line's terms and then as a water level.
	double LevelHeight = Out.LowestOutlineGround - LevelMargin;
	if (LevelMode == EVoxelLakeLevel::ActorHeight)
	{
		const FVector Location = GetOwner() ? GetOwner()->GetActorLocation() : Spline->GetComponentLocation();
		LevelHeight = Frame.HeightOf(Location) - LevelBase;
	}
	Out.Level = LevelHeight + LevelBase;
	for (const double Height : OutlineGround)
	{
		if (Height < LevelHeight)
		{
			++Out.NumOutlinePlacesUnder;
			Out.MostOutlineUnder = FMath::Max(Out.MostOutlineUnder, LevelHeight - Height);
		}
	}

	// The footprint: the cells inside the outline whose ground lies under the level, grown by one cell.
	const double Cell = FMath::Max(CellSizeVoxels, 1.0f) * VoxelSize;
	const auto IsUnder = [&](const FVector& Place)
	{
		const double Height = GetNaturalHeight(Manager, Place);
		if (Height < LevelHeight)
		{
			++Out.NumWetCells;
			Out.Deepest = FMath::Max(Out.Deepest, LevelHeight - Height);
			return true;
		}
		return false;
	};
	if (bPlanet)
	{
		Out.Footprint = FVoxelWaterLake::MakeFromOutlineOnSphere(Places, Frame.DatumRadius, Cell, IsUnder);
	}
	else
	{
		TArray<FVector2D> Outline;
		Outline.Reserve(Places.Num());
		for (const FVector& Place : Places)
		{
			Outline.Add(FVector2D(Place.X, Place.Y));
		}
		Out.Footprint = FVoxelWaterLake::MakeFromOutline(Outline, Cell, IsUnder);
	}
	// The ground at the footprint's cells' corners and centres, for the drawn sheet to follow the shore and not the
	// cells' steps (G229). A block world's water and shore are block columns, which the cells' squares already follow.
	if (Out.Footprint->NumCells() > 0 && !FVoxelBlockSurface::UsesBlockSurface(*Configuration))
	{
		Out.Footprint = FVoxelWaterLake::WithGround(*Out.Footprint, [&](const FVector& Place) { return GetNaturalHeight(Manager, Place) + LevelBase; });
	}

	// The cells of the footprint that lie under the level: the ones inside the outline, counted above, and any of the
	// grown edge outside it, where water would stand past the drawn rim.
	int32 UnderInFootprint = 0;
	const FIntPoint Size = Out.Footprint->GetSize();
	for (int32 Y = 0; Y < Size.Y; ++Y)
	{
		for (int32 X = 0; X < Size.X; ++X)
		{
			if (Out.Footprint->HasCell(X, Y) && GetNaturalHeight(Manager, PlaceOf(Out.Footprint->GetCellCentre(X, Y, Out.Level, Frame.Center))) < LevelHeight)
			{
				++UnderInFootprint;
			}
		}
	}
	Out.NumEdgeCellsUnder = FMath::Max(0, UnderInFootprint - Out.NumWetCells);
	return true;
}

bool UVoxelLakeComponent::AddBody(UVoxelChunkManager& Manager, FVoxelWaterBody& OutBody, TArray<FString>& OutWarnings)
{
	const FString Name = GetBodyName();
	FVoxelLakeBuild Build;
	if (!BuildLake(Manager, Build))
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("Lake %s: no lake was made (%s)."), *Name,
			GetSpline() ? TEXT("its spline has no length") : TEXT("its actor has no spline component, or none with the name asked for"));
		return false;
	}
	const FVoxelWaterFrame Frame = Manager.GetWaterLevels().GetFrame();
	const double LevelBase = Frame.bRadial ? Frame.DatumRadius : 0.0;
	const double Cell = Build.Footprint->GetCellSize();

	if (!Build.bClosedLoop)
	{
		OutWarnings.Add(TEXT("the spline is not a closed loop: the outline is closed straight from its last point to its first"));
	}
	if (Build.NumWetCells == 0)
	{
		OutWarnings.Add(FString::Printf(TEXT("nothing inside the outline lies under the lake's level (%.0f; the ground along the outline is %.0f to %.0f): the lake holds no water. Draw the outline round a hollow, along the ground that is to hold the water"),
			Build.Level - LevelBase, Build.LowestOutlineGround, Build.HighestOutlineGround));
	}
	if (Build.NumOutlinePlacesUnder > 0)
	{
		OutWarnings.Add(FString::Printf(TEXT("the ground along the outline lies under the lake's level at %d of %d places, by up to %.0f uu: the water ends in a wall there. Lower the lake, or draw the outline over ground that stands over the level"),
			Build.NumOutlinePlacesUnder, Build.NumOutlinePlaces, Build.MostOutlineUnder));
	}
	if (Build.NumEdgeCellsUnder > 0)
	{
		OutWarnings.Add(FString::Printf(TEXT("the ground just outside the outline lies under the lake's level at %d cells of the footprint's edge: water may stand past a rim narrower than a cell (%.0f uu). Use a smaller cell, or lower the level"),
			Build.NumEdgeCellsUnder, Cell));
	}
	UE_LOG(LogVoxelStreaming, Log, TEXT("Lake %s: level %.0f (%s; the ground along the outline is %.0f to %.0f over %d places), %d cells of %.0f uu inside the outline under it, the deepest %.0f under; body %u."),
		*Name, Build.Level - LevelBase,
		LevelMode == EVoxelLakeLevel::BelowOutline ? TEXT("the lowest ground along the outline less the margin") : TEXT("the actor's height"),
		Build.LowestOutlineGround, Build.HighestOutlineGround, Build.NumOutlinePlaces, Build.NumWetCells, Cell, Build.Deepest, GetWaterBodyId());

	LakeBuild = Build;
	if (Build.Footprint->NumCells() == 0)
	{
		// A lake with no water has no footprint: there is nothing to add (the warning above says why).
		return true;
	}
	// The water lies no deeper than the deepest cell read, and a chunk for what a cell's centre did not see.
	OutBody = FVoxelWaterBody::MakeLake(GetWaterBodyId(), Build.Level, Build.Footprint.ToSharedRef(), Frame, Build.Deepest + Manager.GetConfiguration()->GetChunkWorldSize(), MinCaveDepth);
	Manager.AddWaterBody(OutBody);
	return true;
}

AVoxelLakeActor::AVoxelLakeActor()
{
	PrimaryActorTick.bCanEverTick = false;

	Spline = CreateDefaultSubobject<USplineComponent>(TEXT("Spline"));
	SetRootComponent(Spline);
	// A ring big enough to see, round the actor: the default spline is 100 uu and open.
	TArray<FVector> Ring;
	for (int32 Index = 0; Index < 8; ++Index)
	{
		const double Angle = UE_DOUBLE_TWO_PI * Index / 8.0;
		Ring.Add(FVector(2000.0 * FMath::Cos(Angle), 2000.0 * FMath::Sin(Angle), 0.0));
	}
	Spline->SetSplinePoints(Ring, ESplineCoordinateSpace::Local, false);
	Spline->SetClosedLoop(true);

	Lake = CreateDefaultSubobject<UVoxelLakeComponent>(TEXT("Lake"));
}
