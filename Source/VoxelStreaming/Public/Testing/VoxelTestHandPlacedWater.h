// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/SplineComponent.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelLakeComponent.h"
#include "VoxelRiverComponent.h"
#include "VoxelWaterLevels.h"

/**
 * Test-only helpers for a hand-placed river and lake (the hydrology plan's H4): a small world of each kind with a sea,
 * a river's course and a hollow found on its natural ground, and the two actors laid over them as a level designer
 * lays them. For the tests of any module that depends on VoxelStreaming (the components' own, the map's, the game
 * module's swimmer). Compiled only with WITH_DEV_AUTOMATION_TESTS.
 */
namespace VoxelTesting
{
	/** The kinds of world a hand-placed body is tried in. */
	enum class EHandPlacedWorld : uint8
	{
		Plane,
		Island,
		Planet,
		/** A plane meshed as blocks */
		Cubic
	};

	inline const TCHAR* GetHandPlacedWorldName(EHandPlacedWorld Kind)
	{
		switch (Kind)
		{
		case EHandPlacedWorld::Island: return TEXT("Island");
		case EHandPlacedWorld::Planet: return TEXT("Planet");
		case EHandPlacedWorld::Cubic: return TEXT("Cubic");
		default: return TEXT("Plane");
		}
	}

	/** How far under the datum the test worlds' sea lies (world units): about a third of their ground is under it. */
	constexpr double HandPlacedSeaDepth = 250.0;

	/** The small world of a kind, with the sea on HandPlacedSeaDepth under the datum. Call before StartManager. */
	inline void ConfigureHandPlacedWorld(FTestChunkWorld& W, EHandPlacedWorld Kind)
	{
		if (Kind == EHandPlacedWorld::Planet)
		{
			W.ConfigureSmallPlanet();
			W.Config->WaterRadius = W.Config->WorldRadius - static_cast<float>(HandPlacedSeaDepth);
		}
		else
		{
			W.ConfigureSmallPlane();
			W.Config->WaterLevel = -static_cast<float>(HandPlacedSeaDepth);
			if (Kind == EHandPlacedWorld::Island)
			{
				W.Config->WorldMode = EWorldMode::IslandBowl;
				// A small island: the search for a course and a hollow reaches its falloff.
				W.Config->IslandRadius = 5000.0f;
				W.Config->IslandFalloffWidth = 3000.0f;
			}
			else if (Kind == EHandPlacedWorld::Cubic)
			{
				W.Config->MeshingMode = EMeshingMode::Cubic;
			}
		}
		W.Config->bEnableWaterLevel = true;
	}

	/**
	 * A started world's natural ground, in terms that hold on a plane and on a planet: a place is a world position (its
	 * column, or its direction from the planet centre, is what counts), a height a world Z or an altitude over the datum.
	 */
	struct FHandPlacedGround
	{
		const UVoxelChunkManager* Manager = nullptr;
		FVoxelWaterFrame Frame;

		explicit FHandPlacedGround(const UVoxelChunkManager& InManager)
			: Manager(&InManager), Frame(InManager.GetWaterLevels().GetFrame())
		{
		}

		/** @return The local up at a place */
		FVector UpAt(const FVector& Place) const { return Frame.UpAt(Place); }

		/** @return The natural ground's height over a place */
		double HeightAt(const FVector& Place) const
		{
			return Frame.HeightOf(Manager->GetSurfacePointAt(Place).Position) - (Frame.bRadial ? Frame.DatumRadius : 0.0);
		}

		/** @return The world point over a place at a height */
		FVector AtHeight(const FVector& Place, double Height) const
		{
			return Frame.bRadial ? Frame.Center + UpAt(Place) * (Frame.DatumRadius + Height) : FVector(Place.X, Place.Y, Height);
		}

		/** @return The point of the natural ground over a place */
		FVector OnGround(const FVector& Place) const { return AtHeight(Place, HeightAt(Place)); }

		/** @return The place a distance east and north of another, along the ground (on a planet: on the plane tangent at the place) */
		FVector Offset(const FVector& Place, double East, double North) const
		{
			if (!Frame.bRadial)
			{
				return FVector(Place.X + East, Place.Y + North, 0.0);
			}
			const FVector Up = UpAt(Place);
			const FVector EastAxis = FVector::CrossProduct(FMath::Abs(Up.Z) < 0.99 ? FVector::UpVector : FVector::ForwardVector, Up).GetSafeNormal();
			const FVector NorthAxis = FVector::CrossProduct(Up, EastAxis);
			return Frame.Center + (Up * Frame.DatumRadius + EastAxis * East + NorthAxis * North).GetSafeNormal() * Frame.DatumRadius;
		}

		/** @return The sea's height (a very low one with the sea off) */
		double SeaHeight() const
		{
			const FVoxelSeaSurface& Sea = Manager->GetWaterLevels().GetSea();
			if (!Sea.bEnabled)
			{
				return -1.0e9;
			}
			FVoxelWaterSample Sample;
			Manager->GetWaterLevels().Sample(AtHeight(Frame.bRadial ? Frame.Center + FVector::ForwardVector : FVector::ZeroVector, 0.0), Sample);
			return Sample.SurfaceLevel - (Frame.bRadial ? Frame.DatumRadius : 0.0);
		}
	};

	/**
	 * A river's course on the natural ground: from a place on high ground near a site, downhill by steepest descent a
	 * step at a time, to a few steps past where the ground goes under the sea.
	 * @param Ground The world's ground
	 * @param Site Where to look for a start: the places within Search of it, a lattice of Search / 8
	 * @param Search How far from the site a start may lie
	 * @param MinLength The least length a course may have
	 * @param MaxLength The most
	 * @param OutCourse Receives the course's places, upstream to downstream, a step (100 uu) apart, on the ground
	 * @return Whether a start was found whose descent reaches the sea within those lengths; the longest such is taken
	 */
	inline bool FindRiverCourse(const FHandPlacedGround& Ground, const FVector& Site, double Search, double MinLength, double MaxLength, TArray<FVector>& OutCourse)
	{
		const double Sea = Ground.SeaHeight();
		const double StepLength = 100.0;
		const int32 MaxSteps = FMath::CeilToInt32(MaxLength / StepLength);
		OutCourse.Reset();
		for (int32 IY = -8; IY <= 8; ++IY)
		{
			for (int32 IX = -8; IX <= 8; ++IX)
			{
				FVector Place = Ground.Offset(Site, IX * Search / 8.0, IY * Search / 8.0);
				double Height = Ground.HeightAt(Place);
				if (Height < Sea + 300.0)
				{
					continue;
				}
				TArray<FVector> Course;
				Course.Add(Ground.OnGround(Place));
				int32 InSea = 0;
				bool bReached = false;
				for (int32 Step = 0; Step < MaxSteps; ++Step)
				{
					// The lowest of sixteen places a step away. In the sea the course runs on the way it was going.
					FVector Best = Place;
					double BestHeight = Height;
					for (int32 Turn = 0; Turn < 16; ++Turn)
					{
						const double Angle = UE_DOUBLE_TWO_PI * Turn / 16.0;
						const FVector Next = Ground.Offset(Place, StepLength * FMath::Cos(Angle), StepLength * FMath::Sin(Angle));
						const double NextHeight = Ground.HeightAt(Next);
						if (NextHeight < BestHeight)
						{
							BestHeight = NextHeight;
							Best = Next;
						}
					}
					if (BestHeight >= Height)
					{
						break; // a pit over the sea: no river from this start
					}
					Place = Best;
					Height = BestHeight;
					Course.Add(Ground.OnGround(Place));
					if (Height < Sea - 60.0 && ++InSea >= 4)
					{
						bReached = true;
						break;
					}
				}
				const double Length = StepLength * (Course.Num() - 1);
				if (bReached && Length >= MinLength && Course.Num() > OutCourse.Num())
				{
					OutCourse = MoveTemp(Course);
				}
			}
		}
		return OutCourse.Num() > 0;
	}

	/** A hollow of the natural ground a lake can stand in (FindHollow). */
	struct FHandPlacedHollow
	{
		/** The hollow's lowest place found, on the ground */
		FVector Pit = FVector::ZeroVector;

		/** The radius of the ring round the pit whose ground stands over it */
		double Radius = 0.0;

		/** The pit's height, the lowest ground along the ring, and how far the first is under the second */
		double PitHeight = 0.0;
		double RimHeight = 0.0;
		double Depth = 0.0;
	};

	/**
	 * A hollow near a site: a place lower than the places around it, with a ring round it whose lowest ground stands
	 * well over it and over the sea, so a lake drawn along the ring holds water above the sea.
	 * @param Ground The world's ground
	 * @param Site Where to look: the places within Search of it, a lattice of 250 uu
	 * @param Search How far from the site the pit may lie
	 * @param MinDepth The least the ring's lowest ground must stand over the pit
	 * @param Out Receives the deepest hollow found
	 * @return Whether one was found
	 */
	inline bool FindHollow(const FHandPlacedGround& Ground, const FVector& Site, double Search, double MinDepth, FHandPlacedHollow& Out)
	{
		const double Sea = Ground.SeaHeight();
		const double Spacing = 250.0;
		const int32 Half = FMath::FloorToInt32(Search / Spacing);
		bool bFound = false;
		for (int32 IY = -Half; IY <= Half; ++IY)
		{
			for (int32 IX = -Half; IX <= Half; ++IX)
			{
				const FVector Place = Ground.Offset(Site, IX * Spacing, IY * Spacing);
				const double Height = Ground.HeightAt(Place);
				bool bLowest = true;
				for (int32 Turn = 0; Turn < 8 && bLowest; ++Turn)
				{
					const double Angle = UE_DOUBLE_TWO_PI * Turn / 8.0;
					bLowest = Ground.HeightAt(Ground.Offset(Place, Spacing * FMath::Cos(Angle), Spacing * FMath::Sin(Angle))) > Height;
				}
				if (!bLowest)
				{
					continue;
				}
				for (const double Radius : { 500.0, 700.0, 900.0, 1200.0 })
				{
					double Rim = TNumericLimits<double>::Max();
					for (int32 Turn = 0; Turn < 32; ++Turn)
					{
						const double Angle = UE_DOUBLE_TWO_PI * Turn / 32.0;
						Rim = FMath::Min(Rim, Ground.HeightAt(Ground.Offset(Place, Radius * FMath::Cos(Angle), Radius * FMath::Sin(Angle))));
					}
					const double Depth = Rim - Height;
					if (Depth >= MinDepth && Rim > Sea + 100.0 && (!bFound || Depth > Out.Depth))
					{
						bFound = true;
						Out.Pit = Ground.OnGround(Place);
						Out.Radius = Radius;
						Out.PitHeight = Height;
						Out.RimHeight = Rim;
						Out.Depth = Depth;
					}
				}
			}
		}
		return bFound;
	}

	/**
	 * A river actor whose spline curves through every Every-th place of a course (and its last), hung Over the ground:
	 * following the terrain only where the spline runs counts.
	 * @return The actor, its spawning not finished: set its component's values, then FinishHandPlaced
	 */
	inline AVoxelRiverActor* SpawnRiverAlong(UWorld* World, const FHandPlacedGround& Ground, const TArray<FVector>& Course, int32 Every = 4, double Over = 300.0)
	{
		TArray<FVector> Points;
		for (int32 Index = 0; Index < Course.Num(); Index += FMath::Max(Every, 1))
		{
			Points.Add(Course[Index] + Ground.UpAt(Course[Index]) * Over);
		}
		if (Course.Num() > 0 && (Course.Num() - 1) % FMath::Max(Every, 1) != 0)
		{
			Points.Add(Course.Last() + Ground.UpAt(Course.Last()) * Over);
		}
		AVoxelRiverActor* Actor = World->SpawnActorDeferred<AVoxelRiverActor>(AVoxelRiverActor::StaticClass(), FTransform::Identity);
		Actor->GetSpline()->SetSplinePoints(Points, ESplineCoordinateSpace::World, true);
		return Actor;
	}

	/**
	 * A lake actor at a place whose outline is a ring of sixteen points round it, hung Over the ground.
	 * @return The actor, its spawning not finished: set its component's values, then FinishHandPlaced
	 */
	inline AVoxelLakeActor* SpawnLakeRound(UWorld* World, const FHandPlacedGround& Ground, const FVector& Centre, double Radius, double Over = 300.0)
	{
		TArray<FVector> Points;
		for (int32 Turn = 0; Turn < 16; ++Turn)
		{
			const double Angle = UE_DOUBLE_TWO_PI * Turn / 16.0;
			const FVector Place = Ground.OnGround(Ground.Offset(Centre, Radius * FMath::Cos(Angle), Radius * FMath::Sin(Angle)));
			Points.Add(Place + Ground.UpAt(Place) * Over);
		}
		AVoxelLakeActor* Actor = World->SpawnActorDeferred<AVoxelLakeActor>(AVoxelLakeActor::StaticClass(), FTransform(Ground.OnGround(Centre) + Ground.UpAt(Centre) * Over));
		Actor->GetSpline()->SetSplinePoints(Points, ESplineCoordinateSpace::World, false);
		Actor->GetSpline()->SetClosedLoop(true, true);
		return Actor;
	}

	/**
	 * Finish a river's or a lake's spawning and have it begin play, as a level's actor does when its world starts: in a
	 * world that has begun play the spawning's end does it, in one that has not it is dispatched here.
	 */
	inline void FinishHandPlaced(AActor* Actor)
	{
		if (!Actor->IsActorInitialized())
		{
			// Where it stands now: a transform given here is applied on top of a move made since the spawn.
			Actor->FinishSpawning(FTransform::Identity, /*bIsDefaultTransform=*/true);
		}
		if (!Actor->HasActorBegunPlay())
		{
			Actor->DispatchBeginPlay();
		}
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
