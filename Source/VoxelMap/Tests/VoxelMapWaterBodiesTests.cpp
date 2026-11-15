// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "VoxelMapSubsystem.h"
#include "VoxelMapTileFrame.h"
#include "VoxelMapTypes.h"
#include "VoxelChunkManager.h"
#include "VoxelWaterLevels.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// A lake on the map (the hydrology plan's H1, section 5.5): a pixel is water when its ground lies under the level over
// that pixel, not under one level a tile.
//
// - Map.WaterBodies.Plane, .Planet: a world with the sea off and a lake at the median height of the ground under its
//   footprint, so about half of the footprint is under water and half stands out of it. The tiles around it are drawn:
//   every pixel is drawn as water exactly where the world's sample puts its ground under a level (pixels within 5 uu
//   of the shore aside), inside the footprint and nowhere outside it. Then a pond a source did not have at hand when
//   the tiles were drawn is announced (UVoxelChunkManager::NotifyWaterBodiesChanged): the tiles it reaches go stale,
//   draw again, and show it.
// ---------------------------------------------------------------------------

namespace VoxelMapWaterBodiesTest
{
	/** Pump the map's tile tasks (and its timers) until nothing waits or draws. */
	bool DrainMap(UVoxelMapSubsystem* Map, UWorld* World, double TimeoutSeconds = 60.0)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		int32 IdleRounds = 0;
		while (FPlatformTime::Seconds() < Deadline)
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			++GFrameCounter;
			World->GetTimerManager().Tick(0.1f);
			IdleRounds = Map->GetDebugStats().Contains(TEXT("waiting=0 drawing=0")) ? IdleRounds + 1 : 0;
			if (IdleRounds >= 12)
			{
				return true;
			}
			FPlatformProcess::Sleep(0.005f);
		}
		return false;
	}

	/** Whether a pixel is drawn as water: the drawer's blue (20, 80, 180) darkened with depth, which no land colour is. */
	bool IsWaterColour(const FColor& Colour)
	{
		return Colour.B >= 50 && Colour.B > 2 * Colour.G - 3 && Colour.G >= 3 * Colour.R && Colour.R <= 20;
	}

	/** A source with one pond that it has, or has not, at hand. */
	struct FLatePond : IVoxelWaterBodySource
	{
		FVoxelWaterBody Pond;
		double PlanetRadius = 0.0;
		bool bAtHand = false;

		virtual void GatherWaterBodies(const FBox2D& Bounds, EVoxelConditioningGather Need, TArray<FVoxelWaterBody>& OutBodies) const override
		{
			if (bAtHand && Pond.MeetsRegion(Bounds))
			{
				OutBodies.Add(Pond);
			}
		}

		virtual void GatherWaterBodiesOnSphere(const FVector& Direction, double AngularRadius, EVoxelConditioningGather Need, TArray<FVoxelWaterBody>& OutBodies) const override
		{
			if (bAtHand && Pond.MeetsCap(Direction, AngularRadius, PlanetRadius))
			{
				OutBodies.Add(Pond);
			}
		}
	};

	struct FPixelCounts
	{
		int32 Pixels = 0;
		int32 Water = 0;
		int32 Mismatches = 0;
		int32 Shore = 0;
	};

	void Run(FAutomationTestBase& Test, bool bPlanet)
	{
		const FString Name = bPlanet ? TEXT("Planet") : TEXT("Plane");
		VoxelTesting::FTestChunkWorld W;
		W.Create(bPlanet ? TEXT("VoxelMapWaterBodiesPlanet") : TEXT("VoxelMapWaterBodiesPlane"));
		if (bPlanet)
		{
			W.ConfigureSmallPlanet();
		}
		else
		{
			W.ConfigureSmallPlane();
		}
		W.Config->bEnableWaterLevel = false;
		W.StartManager();
		UVoxelChunkManager* Manager = W.Manager;
		UVoxelMapSubsystem* Map = W.World->GetSubsystem<UVoxelMapSubsystem>();
		const FVoxelWaterFrame Frame = Manager->GetWaterLevels().GetFrame();

		const FVector SiteDirection = FVector(0.2, 0.6, 0.75).GetSafeNormal();
		const FVector Site = bPlanet ? Manager->GetGeneratedSurfacePoint(Frame.Center + SiteDirection * Frame.DatumRadius)
			: Manager->GetGeneratedSurfacePoint(FVector(700.0, 900.0, 0.0));
		const FVector Up = Manager->GetUpVectorAt(Site);
		const FVector East = bPlanet ? FVector::CrossProduct(FVector::UpVector, Up).GetSafeNormal() : FVector::ForwardVector;
		const FVector North = FVector::CrossProduct(Up, East);
		const auto GroundAt = [&](double Along, double Across)
		{
			return bPlanet ? Manager->GetGeneratedSurfacePoint(Frame.Center + (Up * Frame.DatumRadius + East * Along + North * Across).GetSafeNormal() * Frame.DatumRadius)
				: Manager->GetGeneratedSurfacePoint(Site + East * Along + North * Across);
		};

		// The lake: 12 x 12 cells of 200 uu at the site, at the median height of the ground under it. The pond: 4 x 4
		// cells 2,400 uu away, over the highest ground under it, held back by its source.
		TArray<double> Heights;
		for (int32 I = -11; I <= 11; ++I)
		{
			for (int32 J = -11; J <= 11; ++J)
			{
				Heights.Add(Frame.HeightOf(GroundAt(100.0 * I, 100.0 * J)));
			}
		}
		Heights.Sort();
		const double LakeLevel = Heights[Heights.Num() / 2];
		const TSharedRef<const FVoxelWaterLake> LakeCells = bPlanet
			? FVoxelWaterLake::MakeOnSphere(Up, Frame.DatumRadius, 200.0, FIntPoint(12, 12), TBitArray<>(true, 144))
			: FVoxelWaterLake::Make(FVector2D(Site.X - 1200.0, Site.Y - 1200.0), 200.0, FIntPoint(12, 12), TBitArray<>(true, 144));
		Manager->AddWaterBody(FVoxelWaterBody::MakeLake(1, LakeLevel, LakeCells, Frame, 1000.0));

		FLatePond Source;
		Source.PlanetRadius = Frame.DatumRadius;
		const FVector PondCentre = GroundAt(-2400.0, 0.0);
		double PondHighest = -TNumericLimits<double>::Max();
		for (int32 I = -4; I <= 4; ++I)
		{
			for (int32 J = -4; J <= 4; ++J)
			{
				PondHighest = FMath::Max(PondHighest, Frame.HeightOf(GroundAt(-2400.0 + 100.0 * I, 100.0 * J)));
			}
		}
		const TSharedRef<const FVoxelWaterLake> PondCells = bPlanet
			? FVoxelWaterLake::MakeOnSphere((PondCentre - Frame.Center).GetSafeNormal(), Frame.DatumRadius, 200.0, FIntPoint(4, 4), TBitArray<>(true, 16))
			: FVoxelWaterLake::Make(FVector2D(PondCentre.X - 400.0, PondCentre.Y - 400.0), 200.0, FIntPoint(4, 4), TBitArray<>(true, 16));
		Source.Pond = FVoxelWaterBody::MakeLake(2, PondHighest + 150.0, PondCells, Frame, 1000.0);
		Manager->AddWaterBodySource(&Source);

		W.SetViewer(Site + Up * 300.0);
		if (!Test.TestNotNull(TEXT("The game world has a map"), Map) || !Test.TestTrue(*FString::Printf(TEXT("%s: streaming settles"), *Name), W.Settle()))
		{
			Manager->RemoveWaterBodySource(&Source);
			W.Stop();
			return;
		}
		Test.TestEqual(*FString::Printf(TEXT("%s: the bodies were registered before the first generation request"), *Name), Manager->GetNumLateConditioningSources(), 0);

		// A view of the tiles around the site (exploring draws nothing: a view asks for the images).
		Map->RequestTilesInRadius(Site, 3500.0f);
		const FVoxelMapTileFrame TileFrame = Map->GetTileFrame();
		TArray<FIntPoint> Tiles;
		TileFrame.TilesWithinRadius(Site, 3500.0, Tiles);
		{
			TArray<FVoxelMapTileId> View;
			for (const FIntPoint& Coord : Tiles)
			{
				View.Add(FVoxelMapTileId(Coord, 0));
			}
			Map->RequestView(EVoxelMapRequester::WorldMap, View);
		}
		Test.TestTrue(*FString::Printf(TEXT("%s: the tiles around the site draw"), *Name), DrainMap(Map, W.World));

		// Every drawn pixel against the world's sample at its ground. InFootprint, when set, counts only the pixels whose
		// column lies in that footprint.
		const auto Count = [&](const FVoxelWaterLake* InFootprint)
		{
			FPixelCounts Counts;
			for (const FIntPoint& Coord : Tiles)
			{
				const FVoxelMapTile* Tile = Map->GetTile(Coord);
				if (!Tile || Tile->PixelData.Num() != Tile->Resolution * Tile->Resolution)
				{
					continue;
				}
				for (int32 PY = 0; PY < Tile->Resolution; ++PY)
				{
					for (int32 PX = 0; PX < Tile->Resolution; ++PX)
					{
						const FVoxelMapTileSample Sample = TileFrame.SampleAt(FVoxelMapTileId(Coord, 0), PX + 1, PY + 1);
						const FVector Ground = bPlanet ? Manager->GetGeneratedSurfacePoint(Frame.Center + Sample.Direction * Frame.DatumRadius)
							: Manager->GetGeneratedSurfacePoint(FVector(Sample.X, Sample.Y, 0.0));
						if (InFootprint && !(bPlanet ? InFootprint->CoversDirection((Ground - Frame.Center).GetSafeNormal()) : InFootprint->Covers(Ground.X, Ground.Y)))
						{
							continue;
						}
						const double Depth = Manager->GetWaterDepth(Ground);
						if (Depth > -5.0 && Depth < 5.0)
						{
							++Counts.Shore;
							continue;
						}
						const bool bWater = IsWaterColour(Tile->PixelData[PY * Tile->Resolution + PX]);
						++Counts.Pixels;
						Counts.Water += bWater ? 1 : 0;
						Counts.Mismatches += bWater == (Depth > 0.0) ? 0 : 1;
					}
				}
			}
			return Counts;
		};

		const FPixelCounts All = Count(nullptr);
		const FPixelCounts InLake = Count(&LakeCells.Get());
		const FPixelCounts InPondBefore = Count(&PondCells.Get());
		Test.AddInfo(FString::Printf(TEXT("%s: %d pixels compared (%d on the shore skipped), %d drawn as water; in the lake's footprint %d, %d of them water; in the pond's %d, %d water before it arrives"),
			*Name, All.Pixels, All.Shore, All.Water, InLake.Pixels, InLake.Water, InPondBefore.Pixels, InPondBefore.Water));
		Test.TestTrue(*FString::Printf(TEXT("%s: tiles were drawn over the lake (non-vacuous)"), *Name), InLake.Pixels > 300);
		Test.TestTrue(*FString::Printf(TEXT("%s: part of the footprint is under water and part stands out of it"), *Name),
			InLake.Water > InLake.Pixels / 5 && InLake.Water < InLake.Pixels * 4 / 5);
		Test.TestEqual(*FString::Printf(TEXT("%s: a pixel is water exactly where its ground lies under the level over it (mismatches)"), *Name), All.Mismatches, 0);
		Test.TestEqual(*FString::Printf(TEXT("%s: no water is drawn outside the lake's footprint"), *Name), All.Water, InLake.Water);
		Test.TestTrue(*FString::Printf(TEXT("%s: the pond's ground is drawn dry while its source does not have it"), *Name), InPondBefore.Pixels > 50 && InPondBefore.Water == 0);

		// The pond arrives: its source has it now and announces where.
		Source.bAtHand = true;
		if (bPlanet)
		{
			FVector CapDirection;
			double CapAngle = 0.0;
			PondCells->GetCap(CapDirection, CapAngle);
			Manager->NotifyWaterBodiesChangedOnSphere(CapDirection, CapAngle);
		}
		else
		{
			Manager->NotifyWaterBodiesChanged(PondCells->GetBounds2D());
		}
		Test.TestTrue(*FString::Printf(TEXT("%s: the tiles the pond reaches draw again"), *Name), DrainMap(Map, W.World));
		const FPixelCounts AllAfter = Count(nullptr);
		const FPixelCounts InPondAfter = Count(&PondCells.Get());
		Test.TestEqual(*FString::Printf(TEXT("%s: with the pond the map still agrees with the world's sample (mismatches)"), *Name), AllAfter.Mismatches, 0);
		Test.TestTrue(*FString::Printf(TEXT("%s: the pond's pixels are water once it is announced"), *Name),
			InPondAfter.Pixels > 50 && InPondAfter.Water == InPondAfter.Pixels);

		Manager->RemoveWaterBodySource(&Source);
		W.Stop();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapWaterBodiesPlaneTest,
	"VoxelWorlds.Map.WaterBodies.Plane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapWaterBodiesPlaneTest::RunTest(const FString& Parameters)
{
	VoxelMapWaterBodiesTest::Run(*this, false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapWaterBodiesPlanetTest,
	"VoxelWorlds.Map.WaterBodies.Planet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapWaterBodiesPlanetTest::RunTest(const FString& Parameters)
{
	VoxelMapWaterBodiesTest::Run(*this, true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
