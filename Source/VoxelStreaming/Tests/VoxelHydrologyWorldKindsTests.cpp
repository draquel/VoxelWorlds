// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelBlockSurface.h"
#include "VoxelChunkManager.h"
#include "VoxelHydrologyNetwork.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// A streamed planet and a streamed block world that generate hydrology (Documentation/HYDROLOGY_PLAN.md, phase H8).
//
// - Streaming.Hydrology.Planet: a small planet whose configuration names a hydrology configuration. The network is
//   there when the world's sources register, the fingerprint has the term, no source registers late; a generated
//   lake's voxels hold flagged water under its level and the world's sample there is the lake's, at its level as a
//   distance from the centre; a river reach's voxels hold water over its bed, with the reach's flow, and the generated
//   ground on its line lies under the natural ground; a gather over a cap gives what the survey of that cap holds; and
//   no tile is worked out inside a gather on the game thread while the viewer walks on round the planet.
// - Streaming.Hydrology.Cubic: the same on a block world. The lake's water is whole blocks: the voxel layer under its
//   top is flagged and the layer over it is not, at the lake's lowest cell.
// ---------------------------------------------------------------------------

namespace VoxelHydrologyWorldKindsTest
{
	void RunWorld(FAutomationTestBase& Test, bool bPlanet)
	{
		const TCHAR* Kind = bPlanet ? TEXT("planet") : TEXT("block world");
		VoxelTesting::FTestChunkWorld W;
		W.Create(bPlanet ? TEXT("VoxelHydrologyPlanetWorld") : TEXT("VoxelHydrologyCubicWorld"));
		if (bPlanet)
		{
			W.ConfigureSmallPlanet();
			W.Config->WaterRadius = W.Config->WorldRadius - 300.0f;
		}
		else
		{
			W.ConfigureSmallPlane();
			W.Config->MeshingMode = EMeshingMode::Cubic;
			W.Config->WaterLevel = -300.0f;
		}
		W.Config->bEnableWaterLevel = true;
		UVoxelHydrologyConfiguration* Hydrology = NewObject<UVoxelHydrologyConfiguration>();
		Hydrology->AddToRoot();
		// The test worlds' hills are a few hundred units high and a kilometre or two across: springs close together,
		// short rivers, small lakes (the plane test's values).
		Hydrology->Settings.SpringSpacingCells = 6;
		Hydrology->Settings.MinSpringHeight = 150.0f;
		Hydrology->Settings.MinRiverLength = 2500.0f;
		Hydrology->Settings.MaxRiverLength = 20000.0f;
		Hydrology->Settings.MaxMeanGrade = 0.6f;
		Hydrology->Settings.MinLakeDepthVoxels = 1.5f;
		Hydrology->Settings.MinLakeArea = 2.0e5f;
		Hydrology->Settings.HeadLength = 400.0f;
		W.Config->HydrologyConfiguration = Hydrology;

		bool bNetworkAtRegistration = false;
		const FDelegateHandle Handle = UVoxelChunkManager::OnInitialized.AddLambda([&](UVoxelChunkManager& Manager)
		{
			bNetworkAtRegistration = Manager.GetHydrologyNetwork().IsValid() && Manager.GetThreadSafeWaterBodySource().IsValid();
		});
		W.StartManager();
		UVoxelChunkManager::OnInitialized.Remove(Handle);
		UVoxelChunkManager* Manager = W.Manager;
		const TSharedPtr<FVoxelHydrologyNetwork, ESPMode::ThreadSafe> Network = Manager->GetHydrologyNetwork();
		const auto Finish = [&]()
		{
			W.Stop();
			Hydrology->RemoveFromRoot();
		};
		if (!Test.TestTrue(*FString::Printf(TEXT("a %s that names a hydrology configuration has a network"), Kind), Network.IsValid()))
		{
			Finish();
			return;
		}
		const FVoxelHydrologyGround& Ground = Network->GetGround();
		const FVoxelHydrologyGrid Grid(Ground, Network->GetSettings());
		const double LevelBase = Ground.GetLevelBase();
		// A place at a height, as a world position.
		const auto WorldAt = [&Ground](const FVector& Place, double Height)
		{
			return Ground.bRadial ? Ground.PlanetCenter + Place * (Ground.PlanetRadius + Height) : FVector(Place.X, Place.Y, Height);
		};
		const auto HeightOf = [&Ground](const FVector& World)
		{
			return Ground.bRadial ? FVector::Dist(World, Ground.PlanetCenter) - Ground.PlanetRadius : World.Z;
		};
		Test.TestTrue(TEXT("... of the world's kind"), Ground.bRadial == bPlanet && Ground.bBlocks == !bPlanet && Ground.bSea);
		Test.TestTrue(TEXT("... there when the world's sources register"), bNetworkAtRegistration);
		Test.TestTrue(TEXT("... registered as a source of bodies and of beds"), Manager->HasWaterBodies() && Manager->HasTerrainConditioner() && Manager->GetNumWaterBodies() == 0);
		Test.TestTrue(TEXT("the world's fingerprint has the hydrology's term"), Manager->GetWorldFingerprint().Shape.Contains(FString::Printf(TEXT("Hydrology=%d:"), FVoxelHydrology::Version)));

		// The bodies of the whole planet, or of the plane round its origin: a lake and a river reach to stream to.
		const FVoxelConditioningRegion SurveyRegion = bPlanet
			? FVoxelConditioningRegion::OfCap(FVector::ForwardVector, UE_DOUBLE_PI)
			: FVoxelConditioningRegion::OfBox(FBox2D(FVector2D(-60000.0, -60000.0), FVector2D(60000.0, 60000.0)));
		FVoxelHydrologyRegion Region;
		Network->Survey(SurveyRegion, Region);
		const FVoxelHydrologyLake* Lake = nullptr;
		for (const TSharedRef<const FVoxelHydrologyLake>& Candidate : Region.Lakes)
		{
			if (!Lake || Candidate->Depth > Lake->Depth)
			{
				Lake = &Candidate.Get();
			}
		}
		const FVoxelHydrologyReach* Reach = nullptr;
		int32 NumReaches = 0;
		for (const TSharedRef<const FVoxelHydrologySystem>& System : Region.Systems)
		{
			for (const FVoxelHydrologyReach& Candidate : System->Reaches)
			{
				++NumReaches;
				if (!Reach || Candidate.Length > Reach->Length)
				{
					Reach = &Candidate;
				}
			}
		}
		Test.AddInfo(FString::Printf(TEXT("the test %s: %d cells of %.0f uu%s, %d lakes, %d river systems with %d reaches (%d refused); the longest reach %.0f uu; the deepest lake %.0f uu, %.2f ha"),
			Kind, bPlanet ? 6 * Grid.GetCellsPerEdge() * Grid.GetCellsPerEdge() : 0, Grid.GetCellSize(), bPlanet ? TEXT(" on the sphere") : TEXT(""), Region.Lakes.Num(),
			Region.Systems.Num(), NumReaches, Region.NumRefused, Reach ? Reach->Length : 0.0, Lake ? Lake->Depth : 0.0, Lake ? Lake->Area / 1.0e8 : 0.0));
		if (!Test.TestTrue(*FString::Printf(TEXT("the test %s has a generated lake and a river reach (the fixture)"), Kind), Lake != nullptr && Reach != nullptr))
		{
			Finish();
			return;
		}

		// ---- The lake: its voxels hold flagged water, and the world's sample is the lake's.
		const double Voxel = W.Config->VoxelSize;
		const FVector PitPlace = Grid.PlaceOf(Lake->LowestCell);
		const double Under = bPlanet ? FMath::Min(0.5 * Lake->Depth, 60.0) : 0.5 * Voxel;
		const FVector InLake = WorldAt(PitPlace, Lake->WaterLevel - Under);
		W.SetViewer(WorldAt(PitPlace, Lake->WaterLevel + 300.0));
		if (Test.TestTrue(TEXT("the world settles round the lake"), W.Settle()))
		{
			FVoxelWaterSample Sample;
			Test.TestTrue(TEXT("a voxel of the generated lake holds flagged water"), Manager->GetVoxelAt(InLake).HasWaterFlag());
			Test.TestTrue(TEXT("the world's sample in it is the lake's, at the body's level"), Manager->SampleWater(InLake, Sample) && Sample.bWater && Sample.Kind == EVoxelWaterBodyKind::Lake
				&& Sample.BodyId == Lake->Body.Id && Sample.Depth > 0.0 && FMath::IsNearlyEqual(Sample.SurfaceLevel, Lake->Body.Level, 0.01));
			Test.TestFalse(TEXT("a voxel over it holds none"), Manager->GetVoxelAt(WorldAt(PitPlace, Lake->WaterLevel + (bPlanet ? 150.0 : 0.5 * Voxel))).HasWaterFlag());
			if (bPlanet)
			{
				Test.TestTrue(TEXT("the lake's level is a distance from the planet's centre: the datum radius over its height"),
					FMath::IsNearlyEqual(Lake->Body.Level, Lake->WaterLevel + LevelBase, 1.0e-6) && Lake->Body.Lake->IsRadial());
			}
			else
			{
				// Whole blocks: the water's top is a layer's top, the layer under it wet and the one over it dry (above).
				Test.TestTrue(TEXT("the lake's water is whole blocks: its top a block layer's, its level in that layer"),
					FMath::IsNearlyEqual(FMath::Fmod(FMath::Abs(Lake->WaterLevel), Voxel), 0.0, 1.0e-6) && FMath::IsNearlyEqual(Lake->Body.Level, Lake->WaterLevel - 0.1 * Voxel, 1.0e-6));
			}
			Test.TestEqual(TEXT("no source registered late"), Manager->GetNumLateConditioningSources(), 0);
		}

		// ---- The river reach: its bed is cut and holds the reach's water, which runs.
		const TArray<FVoxelWaterReachVertex>& Vertices = Reach->Body.Reach->GetVertices();
		const double SeaHeight = Ground.SeaLevel;
		int32 At = INDEX_NONE;
		for (int32 Index = 4; Index < Vertices.Num() - 4 && At == INDEX_NONE; ++Index)
		{
			if (Vertices[Index].SurfaceLevel - Vertices[Index].BedLevel > 0.9 * Network->GetSettings().DepthVoxels * Voxel
				&& Vertices[Index].SurfaceLevel - LevelBase > SeaHeight + 30.0)
			{
				At = Index;
			}
		}
		if (Test.TestTrue(TEXT("the river reach has a vertex at its full depth on land (the fixture)"), At != INDEX_NONE))
		{
			const FVoxelWaterReachVertex& Vertex = Vertices[At];
			const FVector InRiver = WorldAt(Vertex.Position, 0.5 * (Vertex.SurfaceLevel + Vertex.BedLevel) - LevelBase);
			W.SetViewer(WorldAt(Vertex.Position, Vertex.SurfaceLevel - LevelBase + 300.0));
			if (Test.TestTrue(TEXT("the world settles round the river reach"), W.Settle()))
			{
				FVoxelWaterSample Sample;
				Test.TestTrue(TEXT("a voxel of the generated river reach holds flagged water: the bed is cut and the reach fills it"), Manager->GetVoxelAt(InRiver).HasWaterFlag());
				const double Generated = HeightOf(Manager->GetGeneratedSurfacePoint(InRiver));
				const double Natural = Ground.HeightAt(Vertex.Position);
				// A block world's ground stands up to a block over the natural height it is made from.
				Test.TestTrue(TEXT("the generated ground on the reach's line lies under the natural ground: the bed"), Generated < Natural - 20.0);
				const bool bSampled = Manager->SampleWater(InRiver, Sample);
				Test.TestTrue(TEXT("the world's sample there is the river reach's, with its flow"), bSampled && Sample.Kind == EVoxelWaterBodyKind::Reach && Sample.Depth > 0.0
					&& FMath::IsNearlyEqual(Sample.Flow.Size(), static_cast<double>(Network->GetSettings().FlowSpeed), 0.01));
			}
		}

		// ---- A gather over a region round the lake gives the lake, by either need once it is kept.
		{
			TArray<FVoxelWaterBody> Complete, Ready;
			if (bPlanet)
			{
				const double Angle = 3000.0 / Ground.PlanetRadius;
				Network->GatherWaterBodiesOnSphere(PitPlace, Angle, EVoxelConditioningGather::Complete, Complete);
				Network->GatherWaterBodiesOnSphere(PitPlace, Angle, EVoxelConditioningGather::Ready, Ready);
				Test.TestTrue(TEXT("the network holds the cap after a complete gather over it"), Network->HoldsWaterBodiesOnSphere(PitPlace, Angle));
				TArray<FVoxelWaterBody> Boxed;
				Network->GatherWaterBodies(FBox2D(FVector2D(-5000.0, -5000.0), FVector2D(5000.0, 5000.0)), EVoxelConditioningGather::Complete, Boxed);
				Test.TestEqual(TEXT("a box is no region of a planet: a gather over one gives nothing"), Boxed.Num(), 0);
			}
			else
			{
				const FBox2D Box(FVector2D(PitPlace.X - 3000.0, PitPlace.Y - 3000.0), FVector2D(PitPlace.X + 3000.0, PitPlace.Y + 3000.0));
				Network->GatherWaterBodies(Box, EVoxelConditioningGather::Complete, Complete);
				Network->GatherWaterBodies(Box, EVoxelConditioningGather::Ready, Ready);
			}
			const auto Has = [Lake](const TArray<FVoxelWaterBody>& Bodies) { return Bodies.ContainsByPredicate([Lake](const FVoxelWaterBody& Body) { return Body.Id == Lake->Body.Id; }); };
			Test.TestTrue(TEXT("a complete gather round the lake gives it, and a ready one after it the same bodies"), Has(Complete) && Has(Ready) && Complete.Num() == Ready.Num());
		}

		// ---- No tile worked out inside a gather while the viewer walks on: the worker is there first.
		Network->WaitForWorker();
		Network->ResetStats();
		FVector Place = PitPlace;
		W.SetViewer(WorldAt(Place, Lake->WaterLevel + 300.0));
		W.Settle();
		Network->WaitForWorker();
		Network->ResetStats();
		bool bSettled = true;
		const double Stride = 2.0 * W.Config->GetChunkWorldSize();
		for (int32 Step = 0; Step < 6 && bSettled; ++Step)
		{
			if (bPlanet)
			{
				// Along a great circle through the lake.
				FVector East, North;
				FVoxelCubeSphere::GetTangentFrame(Place, East, North);
				Place = (Place + (East + 0.5 * North).GetSafeNormal() * FMath::Tan(Stride / Ground.PlanetRadius)).GetSafeNormal();
			}
			else
			{
				Place += FVector(Stride, 0.5 * Stride, 0.0);
			}
			W.SetViewer(WorldAt(Place, Lake->WaterLevel + 300.0));
			bSettled = W.Settle();
			// A walker gives the worker the time a settle takes; the test waits for it outright.
			Network->WaitForWorker();
		}
		const FVoxelHydrologyStats Stats = Network->GetStats();
		Test.AddInfo(FString::Printf(TEXT("the walk: %lld complete gathers, %lld tiles by the worker, %lld inside a gather on the game thread, %lld on another thread; %d cell(s) with two ways on in %d systems"),
			Stats.CompleteGathers, Stats.WorkerTiles, Stats.SyncTiles, Stats.OffThreadTiles, Stats.Conflicts, Stats.Systems));
		Test.TestTrue(TEXT("the world settles at each step of a walk of twelve chunks"), bSettled);
		Test.TestTrue(TEXT("the chunks along the walk asked for their bodies (non-vacuous)"), Stats.CompleteGathers > 100);
		Test.TestEqual(TEXT("no tile is worked out inside a gather on the game thread along the walk"), Stats.SyncTiles, static_cast<int64>(0));
		Test.TestEqual(TEXT("no cell of the network has two ways on (G231)"), Stats.Conflicts, 0);
		Finish();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHydrologyPlanetWorldTest,
	"VoxelWorlds.Streaming.Hydrology.Planet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHydrologyPlanetWorldTest::RunTest(const FString& Parameters)
{
	VoxelHydrologyWorldKindsTest::RunWorld(*this, /*bPlanet=*/true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHydrologyCubicWorldTest,
	"VoxelWorlds.Streaming.Hydrology.Cubic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHydrologyCubicWorldTest::RunTest(const FString& Parameters)
{
	VoxelHydrologyWorldKindsTest::RunWorld(*this, /*bPlanet=*/false);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
