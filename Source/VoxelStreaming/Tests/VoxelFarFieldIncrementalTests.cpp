// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/IConsoleManager.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelEditManager.h"
#include "VoxelFarField.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "SphericalPlanetWorldMode.h"
#include "IVoxelWorldMode.h"
#include "VoxelFarFieldTestRig.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The far field's update without the full walk (G293): an update that would change nothing is skipped
// (FVoxelFarFieldView::bSkipUnchanged), and on a chunk-state change only the edge patches over the chunks that changed test
// their cover again (FVoxelFarFieldView::ChangedChunks). Each test runs a far field on the new path beside a twin on the full
// walk, the same views and windows in the same order, and compares their selections after every update: the same patches,
// drawn the same, the same covered answers, the same changes reported.
//
// - Skip: at rest the update is skipped and selects what the walk would (the drawn patches drawn at its time); a camera or
//   viewer moved, a new chunk-state version, the error, the field of view, the window's radii, a delay, an invalidated region
//   and a new query each end it; a build that finishes while it skips is applied.
// - TimeRules: a covered edge patch hides at HideDelay and a hidden patch is released at ReleaseDelay in the same update as
//   the twin's, skipped updates between.
// - ChangedChunks: on a plane and a planet, a window that moves and flickers chunk by chunk: the same selection, far fewer
//   covered tests; the checking mode finds no difference, and finds one when a changed chunk is not told.
// - ChunkManager: on a plane, an island, a block world and a planet streamed by a chunk manager with
//   voxel.World.FarFieldCheck 1, settling, walking, a cut far away, an edit and a conditioning zone: no difference.
// ---------------------------------------------------------------------------

namespace VoxelFarFieldIncrementalTestUtils
{
	using VoxelFarFieldTestRig::FScopedCVar;

	constexpr double PlaneViewDistance = 12000.0;
	constexpr double PlaneUnloadDistance = 14400.0;
	constexpr double PlanetViewDistance = 40000.0;
	constexpr double PlanetUnloadDistance = 48000.0;

	/** The heightmap suite's small plane (terrain +-800, chunks of 32 voxels of 50 uu, a sea at -200). Rooted. */
	UVoxelWorldConfiguration* MakePlaneConfig()
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::InfinitePlane;
		Config->MeshingMode = EMeshingMode::DualContouring;
		Config->WorldOrigin = FVector::ZeroVector;
		Config->ChunkSize = 32;
		Config->VoxelSize = 50.0f;
		Config->ViewDistance = 3000.0f;
		Config->HeightScale = 800.0f;
		Config->BaseHeight = 0.0f;
		Config->bEnableBiomes = false;
		Config->bEnableWaterLevel = true;
		Config->WaterLevel = -200.0f;
		Config->NoiseParams.Seed = 9001;
		Config->NoiseParams.Frequency = 0.0004f;
		Config->NoiseParams.Octaves = 3;
		return Config;
	}

	/** The planet suite's small planet (R 250,000, terrain +-6,500, chunks of 64 voxels of 75 uu). Rooted. */
	UVoxelWorldConfiguration* MakePlanetConfig()
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::SphericalPlanet;
		Config->MeshingMode = EMeshingMode::DualContouring;
		Config->VoxelSize = 75.0f;
		Config->ChunkSize = 64;
		Config->ViewDistance = PlanetViewDistance;
		Config->WorldRadius = 250000.0;
		Config->PlanetMaxTerrainHeight = 6500.0f;
		Config->PlanetMaxTerrainDepth = 6500.0f;
		Config->PlanetHeightScale = 13000.0f;
		Config->BaseHeight = 0.0f;
		Config->NoiseParams.Frequency = 2.5e-5f;
		Config->NoiseParams.Octaves = 3;
		Config->bEnableBiomes = false;
		Config->bEnableWaterLevel = true;
		Config->WaterRadius = 250000.0 - 0.25 * 6500.0;
		return Config;
	}

	/** The chunk manager's far-field settings for the plane (UVoxelChunkManager::RebuildFarField). */
	FVoxelFarFieldSettings MakePlaneSettings(const UVoxelWorldConfiguration& Config, const IVoxelWorldMode& Mode)
	{
		FVoxelFarFieldSettings Settings;
		Settings.bHeightmap = true;
		Settings.GridOrigin = Config.WorldOrigin;
		Settings.ChunkWorldSize = Config.GetChunkWorldSize();
		Settings.VoxelSize = Config.VoxelSize;
		Settings.PatchQuads = 16;
		Settings.Sink = Config.VoxelSize;
		Settings.SeaMargin = 2.0 * Config.VoxelSize;
		Settings.RootChunks = 16;
		Settings.FarDistance = 60000.0;
		Settings.Bounds = Mode.GetWorldBounds();
		Settings.MinAltitude = Settings.Bounds.MinZ;
		Settings.MaxAltitude = Settings.Bounds.MaxZ;
		Settings.bSea = true;
		Settings.SeaZ = Config.WaterLevel;
		Settings.EdgeLevel = FVoxelFarFieldSettings::ComputeHeightmapEdgeLevel(Settings.RootChunks);
		Settings.ZoneGatherMax = 64.0 * Settings.ChunkWorldSize + 1.0;
		return Settings;
	}

	/** The chunk manager's far-field settings for the planet. */
	FVoxelFarFieldSettings MakePlanetSettings(const UVoxelWorldConfiguration& Config, const FVoxelFarFieldSurface& Surface)
	{
		FVoxelFarFieldSettings Settings;
		const FSphericalPlanetWorldMode& Planet = Surface.GetPlanet();
		Settings.Center = Planet.GetPlanetParams().PlanetCenter;
		Settings.Radius = Planet.GetPlanetParams().PlanetRadius;
		Settings.GridOrigin = Config.WorldOrigin;
		Settings.ChunkWorldSize = Config.GetChunkWorldSize();
		Settings.VoxelSize = Config.VoxelSize;
		Settings.PatchQuads = 8;
		Settings.Sink = Config.VoxelSize;
		float MinAltitude = 0.0f, MaxAltitude = 0.0f;
		FSphericalPlanetWorldMode::GetDisplacementBounds(Planet.GetTerrainParams(), &Planet.GetBiomeSnapshot(), MinAltitude, MaxAltitude);
		Settings.MinAltitude = MinAltitude;
		Settings.MaxAltitude = MaxAltitude;
		Settings.bSea = true;
		Settings.SeaRadius = Config.WaterRadius;
		Settings.SeaMargin = 2.0 * Config.VoxelSize;
		Settings.EdgeLevel = FVoxelFarFieldSettings::ComputeEdgeLevel(Settings.Radius, Settings.ChunkWorldSize);
		Settings.ZoneGatherMax = 32.0 * Settings.ChunkWorldSize / Settings.Radius;
		return Settings;
	}

	/** A far field over the plane or the planet with synchronous builds (the one rig). */
	TUniquePtr<VoxelFarFieldTestRig::FRig> MakeRig(bool bPlanet)
	{
		if (bPlanet)
		{
			return MakeUnique<VoxelFarFieldTestRig::FRig>(MakePlanetConfig(),
				[](const UVoxelWorldConfiguration& Config, const IVoxelWorldMode&, const FVoxelFarFieldSurface& Surface) { return MakePlanetSettings(Config, Surface); },
				PlanetViewDistance, PlanetUnloadDistance);
		}
		return MakeUnique<VoxelFarFieldTestRig::FRig>(MakePlaneConfig(),
			[](const UVoxelWorldConfiguration& Config, const IVoxelWorldMode& Mode, const FVoxelFarFieldSurface&) { return MakePlaneSettings(Config, Mode); },
			PlaneViewDistance, PlaneUnloadDistance);
	}

	/** A viewer 300 uu over the ground at a site: a plane's (X, Y), a planet's direction (X, Y, Z). */
	FVector ViewerAt(const VoxelFarFieldTestRig::FRig& Rig, const FVector& Site)
	{
		if (Rig.Frame().IsHeightmap())
		{
			return FVector(Site.X, Site.Y, Rig.Altitude(FVoxelFarFieldSample::OfXY(FVector2D(Site.X, Site.Y))) + 300.0);
		}
		const FVector Direction = Site.GetSafeNormal();
		return Rig.Settings.Center + Direction * (Rig.Settings.Radius + Rig.Altitude(FVoxelFarFieldSample::OfDirection(Direction)) + 300.0);
	}

	/** The chunks of a loaded window around a view (every chunk whose centre lies within Radius of the ground point) in a box of chunks around it. */
	TSet<FIntVector> WindowChunks(const VoxelFarFieldTestRig::FRig& Rig, const FVoxelFarFieldView& View, double Radius)
	{
		TSet<FIntVector> Chunks;
		const FIntVector Centre = Rig.ChunkOf(View.Viewer);
		const int32 Reach = FMath::CeilToInt32(Radius / Rig.Settings.ChunkWorldSize) + 1;
		const int32 Depth = Rig.Frame().IsHeightmap() ? 2 : Reach;
		for (int32 Z = -Depth; Z <= Depth; ++Z)
		{
			for (int32 Y = -Reach; Y <= Reach; ++Y)
			{
				for (int32 X = -Reach; X <= Reach; ++X)
				{
					const FIntVector Coord = Centre + FIntVector(X, Y, Z);
					if (Rig.InWindow(View, Coord, Radius))
					{
						Chunks.Add(Coord);
					}
				}
			}
		}
		return Chunks;
	}

	/** One patch as a selection leaves it. */
	struct FPatchSnapshot
	{
		bool bDrawTerrain = false;
		bool bDrawSea = false;
		bool bResident = false;
		bool bHasChildren = false;
		bool bCovered = false;
		bool bLazy = false;
		bool bCulled = false;
		double CoveredSince = 0.0;
		double LastDrawnTime = 0.0;

		bool operator==(const FPatchSnapshot& Other) const
		{
			return bDrawTerrain == Other.bDrawTerrain && bDrawSea == Other.bDrawSea && bResident == Other.bResident
				&& bHasChildren == Other.bHasChildren && bCovered == Other.bCovered && bLazy == Other.bLazy && bCulled == Other.bCulled
				&& CoveredSince == Other.CoveredSince && LastDrawnTime == Other.LastDrawnTime;
		}
	};

	TMap<uint64, FPatchSnapshot> Snapshot(const FVoxelFarField& FarField)
	{
		TMap<uint64, FPatchSnapshot> Out;
		FarField.ForEachPatch([&Out](const FVoxelFarField::FPatchState& Patch)
		{
			FPatchSnapshot& S = Out.Add(Patch.Key.Pack());
			S.bDrawTerrain = Patch.bDrawTerrain;
			S.bDrawSea = Patch.bDrawSea;
			S.bResident = Patch.bResident;
			S.bHasChildren = Patch.bHasChildren;
			S.bCovered = Patch.bCovered;
			S.bLazy = Patch.bLazy;
			S.bCulled = Patch.bCulled;
			S.CoveredSince = Patch.CoveredSince;
			S.LastDrawnTime = Patch.LastDrawnTime;
		});
		return Out;
	}

	TSet<uint64> AsSet(const TArray<uint64>& Keys)
	{
		return TSet<uint64>(Keys);
	}

	/**
	 * A far field on the new path (New: the skip and the changed chunks, or the checking mode) beside one on the full walk
	 * (Full), fed the same views and windows. Step updates both and returns the patches and changes that differ.
	 */
	struct FTwin
	{
		TUniquePtr<VoxelFarFieldTestRig::FRig> New;
		TUniquePtr<VoxelFarFieldTestRig::FRig> Full;
		bool bCheck = false;
		bool bTellChanges = true;

		/** The window both see, its version and the chunks changed since the last step */
		TSet<FIntVector> Drawn;
		int64 Version = 0;
		TSet<FIntVector> Changed;

		FVoxelFarFieldChanges NewChanges;
		FVoxelFarFieldChanges FullChanges;
		int64 NewTests = 0;
		int64 FullTests = 0;
		int64 Kept = 0;
		int32 Steps = 0;
		int32 Skipped = 0;

		explicit FTwin(bool bPlanet) : New(MakeRig(bPlanet)), Full(MakeRig(bPlanet)) {}

		/** Set the window (bumps the version and notes the chunks that came or went). */
		void SetWindow(const TSet<FIntVector>& Window)
		{
			for (const FIntVector& Chunk : Drawn)
			{
				if (!Window.Contains(Chunk))
				{
					Changed.Add(Chunk);
				}
			}
			for (const FIntVector& Chunk : Window)
			{
				if (!Drawn.Contains(Chunk))
				{
					Changed.Add(Chunk);
				}
			}
			Drawn = Window;
			++Version;
		}

		/** Toggle one chunk of the window. */
		void Toggle(const FIntVector& Chunk, bool bTell = true)
		{
			if (Drawn.Remove(Chunk) == 0)
			{
				Drawn.Add(Chunk);
			}
			if (bTell)
			{
				Changed.Add(Chunk);
			}
			++Version;
		}

		/** Update both for a view at a time; the description of every difference in OutDifferences. */
		void Step(FVoxelFarFieldView View, TArray<FString>& OutDifferences)
		{
			View.ChunkStateVersion = Version;
			auto IsDrawn = [this](const FIntVector& Coord) { return Drawn.Contains(Coord); };
			FVoxelFarFieldView FullView = View;
			FullView.bSkipUnchanged = false;
			FullView.ChangedChunks = nullptr;
			FullView.bCheck = false;
			Full->FarField.Update(FullView, IsDrawn, FullChanges);
			FVoxelFarFieldView NewView = View;
			NewView.bSkipUnchanged = true;
			NewView.ChangedChunks = bTellChanges || bCheck ? &Changed : nullptr;
			NewView.bCheck = bCheck;
			New->FarField.Update(NewView, IsDrawn, NewChanges);
			Changed.Reset();
			++Steps;
			Skipped += New->FarField.WasLastUpdateSkipped() ? 1 : 0;
			NewTests += New->FarField.GetCoveredTests();
			FullTests += Full->FarField.GetCoveredTests();
			Kept += New->FarField.GetCoveredKept();

			if (AsSet(NewChanges.Built).Num() != AsSet(FullChanges.Built).Num() || !AsSet(NewChanges.Built).Includes(AsSet(FullChanges.Built))
				|| AsSet(NewChanges.Removed).Num() != AsSet(FullChanges.Removed).Num() || !AsSet(NewChanges.Removed).Includes(AsSet(FullChanges.Removed))
				|| AsSet(NewChanges.Visibility).Num() != AsSet(FullChanges.Visibility).Num() || !AsSet(NewChanges.Visibility).Includes(AsSet(FullChanges.Visibility)))
			{
				OutDifferences.Add(FString::Printf(TEXT("step %d: changes built %d/%d removed %d/%d visibility %d/%d (new/full)"), Steps,
					NewChanges.Built.Num(), FullChanges.Built.Num(), NewChanges.Removed.Num(), FullChanges.Removed.Num(),
					NewChanges.Visibility.Num(), FullChanges.Visibility.Num()));
			}
			const TMap<uint64, FPatchSnapshot> A = Snapshot(New->FarField);
			const TMap<uint64, FPatchSnapshot> B = Snapshot(Full->FarField);
			if (A.Num() != B.Num())
			{
				OutDifferences.Add(FString::Printf(TEXT("step %d: %d patches against %d"), Steps, A.Num(), B.Num()));
			}
			for (const TPair<uint64, FPatchSnapshot>& Pair : B)
			{
				const FPatchSnapshot* Other = A.Find(Pair.Key);
				if (!Other || !(*Other == Pair.Value))
				{
					OutDifferences.Add(FString::Printf(TEXT("step %d: patch %s differs"), Steps, *FVoxelFarFieldPatchKey::Unpack(Pair.Key).ToString()));
					if (OutDifferences.Num() > 20)
					{
						return;
					}
				}
			}
		}
	};

	/** A line for the report and the log (the headless suite's log shows no AddInfo). */
	void Note(FAutomationTestBase& Test, const FString& Text)
	{
		Test.AddInfo(Text);
		UE_LOG(LogTemp, Display, TEXT("G293 test: %s"), *Text);
	}

	/** Report a twin's differences, at most a few. */
	void ReportDifferences(FAutomationTestBase& Test, const FString& What, const TArray<FString>& Differences)
	{
		Test.TestEqual(FString::Printf(TEXT("%s: the new path selects what the full walk does"), *What), Differences.Num(), 0);
		for (int32 Index = 0; Index < FMath::Min(Differences.Num(), 5); ++Index)
		{
			Test.AddInfo(Differences[Index]);
			UE_LOG(LogTemp, Display, TEXT("G293 test: %s"), *Differences[Index]);
		}
	}
}

/**
 * G293: an update that would change nothing is skipped, and selects what the walk would; each input that can change the
 * selection ends the skip; a build that finishes while it skips is applied.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldIncrementalSkipTest,
	"VoxelWorlds.FarField.Incremental.Skip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldIncrementalSkipTest::RunTest(const FString& Parameters)
{
	using namespace VoxelFarFieldIncrementalTestUtils;

	for (const bool bPlanet : { false, true })
	{
		const FString World = bPlanet ? TEXT("Planet") : TEXT("Plane");
		FTwin Twin(bPlanet);
		const FVector Site = bPlanet ? FVector(1.0, 0.3, 0.6) : FVector(4000.0, -2500.0, 0.0);
		FVoxelFarFieldView View = Twin.New->MakeView(ViewerAt(*Twin.New, Site));
		View.HideDelay = 0.0;
		View.ReleaseDelay = 10.0;
		Twin.SetWindow(WindowChunks(*Twin.New, View, Twin.New->ViewDistance));
		TArray<FString> Differences;
		// Settle: the twin's full walk changes nothing.
		for (int32 Step = 0; Step < 300; ++Step)
		{
			Twin.Step(View, Differences);
			if (Twin.FullChanges.IsEmpty() && Twin.Full->FarField.GetBuildsPending() == 0 && Step > 2)
			{
				break;
			}
		}
		TestTrue(*FString::Printf(TEXT("%s: the window covers edge patches"), *World), Twin.New->FarField.GetHiddenEdgeCount() > 0);

		// At rest: skipped, nothing changed, the drawn patches drawn at its time.
		View.Time += 0.25;
		Twin.Step(View, Differences);
		TestTrue(*FString::Printf(TEXT("%s: at rest the update is skipped"), *World), Twin.New->FarField.WasLastUpdateSkipped());
		TestTrue(*FString::Printf(TEXT("%s: a skipped update reports no change"), *World), Twin.NewChanges.IsEmpty());
		bool bDrawnAtTime = Twin.New->FarField.GetDrawnPatchCount() > 0;
		Twin.New->FarField.ForEachPatch([&bDrawnAtTime, &View](const FVoxelFarField::FPatchState& Patch)
		{
			if ((Patch.bDrawTerrain || Patch.bDrawSea) && Patch.LastDrawnTime != View.Time)
			{
				bDrawnAtTime = false;
			}
		});
		TestTrue(*FString::Printf(TEXT("%s: a skipped update draws the drawn patches at its time"), *World), bDrawnAtTime);
		View.Time += 0.25;
		Twin.Step(View, Differences);
		TestTrue(*FString::Printf(TEXT("%s: and the next"), *World), Twin.New->FarField.WasLastUpdateSkipped());

		// Each input that may change the selection ends the skip; the update after it, at rest again, is skipped once settled.
		struct FCase
		{
			const TCHAR* Name;
			TFunction<void(FVoxelFarFieldView&, FTwin&)> Change;
		};
		const double Chunk = Twin.New->Settings.ChunkWorldSize;
		const TArray<FCase> Cases = {
			{ TEXT("the camera moved 1 uu"), [](FVoxelFarFieldView& V, FTwin&) { V.Camera.X += 1.0; } },
			{ TEXT("the viewer moved 1 uu"), [](FVoxelFarFieldView& V, FTwin&) { V.Viewer.Y += 1.0; } },
			{ TEXT("the camera cut a chunk and more away"), [Chunk](FVoxelFarFieldView& V, FTwin&) { V.Camera.Z += 1.5 * Chunk; } },
			{ TEXT("the camera back"), [Chunk](FVoxelFarFieldView& V, FTwin&) { V.Camera.Z -= 1.5 * Chunk; } },
			{ TEXT("the ground radius"), [](FVoxelFarFieldView& V, FTwin&) { V.GroundRadius += 1.0; } },
			{ TEXT("a new chunk-state version (no chunk changed)"), [](FVoxelFarFieldView&, FTwin& T) { ++T.Version; } },
			{ TEXT("the error budget (a cvar)"), [](FVoxelFarFieldView& V, FTwin&) { V.MaxScreenError *= 0.5; } },
			{ TEXT("the field of view"), [](FVoxelFarFieldView& V, FTwin&) { V.PixelsPerRadian *= 1.25; } },
			{ TEXT("the edge radius (the view distance)"), [Chunk](FVoxelFarFieldView& V, FTwin&) { V.EdgeRadius += Chunk; } },
			{ TEXT("the prebuild radius"), [Chunk](FVoxelFarFieldView& V, FTwin&) { V.PrebuildRadius -= Chunk; } },
			{ TEXT("the hide delay (the fade's cvar)"), [](FVoxelFarFieldView& V, FTwin&) { V.HideDelay += 0.1; } },
			{ TEXT("the release delay"), [](FVoxelFarFieldView& V, FTwin&) { V.ReleaseDelay += 1.0; } },
			{ TEXT("a chunk under the window left (an edit, an unload)"), [](FVoxelFarFieldView&, FTwin& T)
				{
					for (const FIntVector& C : T.Drawn) { T.Toggle(C); break; }
				} },
			{ TEXT("a region invalidated (a conditioning change)"), [](FVoxelFarFieldView& V, FTwin& T)
				{
					FVoxelFarFieldRegion Region;
					if (T.New->Frame().IsHeightmap())
					{
						Region.Box = FBox2D(FVector2D(V.Viewer) - FVector2D(2000.0), FVector2D(V.Viewer) + FVector2D(2000.0));
					}
					else
					{
						Region.bCap = true;
						Region.Direction = (V.Viewer - T.New->Settings.Center).GetSafeNormal();
						Region.AngularRadius = 2000.0 / T.New->Settings.Radius;
					}
					T.New->FarField.InvalidateRegion(Region);
					T.Full->FarField.InvalidateRegion(Region);
				} },
			{ TEXT("a water-tile query set"), [](FVoxelFarFieldView&, FTwin& T)
				{
					T.New->FarField.SetWaterTileQuery([](const FIntPoint&) { return false; });
					T.Full->FarField.SetWaterTileQuery([](const FIntPoint&) { return false; });
				} },
		};
		for (const FCase& Case : Cases)
		{
			// Settled first (a case's change may take a few updates to settle).
			for (int32 Step = 0; Step < 100 && !Twin.New->FarField.WasLastUpdateSkipped(); ++Step)
			{
				View.Time += 0.01;
				Twin.Step(View, Differences);
			}
			TestTrue(*FString::Printf(TEXT("%s, before %s: skipped at rest"), *World, Case.Name), Twin.New->FarField.WasLastUpdateSkipped());
			Case.Change(View, Twin);
			View.Time += 0.01;
			Twin.Step(View, Differences);
			TestFalse(*FString::Printf(TEXT("%s: %s ends the skip"), *World, Case.Name), Twin.New->FarField.WasLastUpdateSkipped());
		}
		TestTrue(*FString::Printf(TEXT("%s: updates skipped"), *World), Twin.Skipped > 10);
		ReportDifferences(*this, World, Differences);
	}

	// A build that finishes while the update skips is applied: builds on the thread pool, a rebuild launched by an
	// invalidated region, updates at rest until it lands.
	{
		TUniquePtr<VoxelFarFieldTestRig::FRig> Rig = MakeRig(false);
		Rig->FarField.SetSynchronousBuilds(false);
		FVoxelFarFieldView View = Rig->MakeView(ViewerAt(*Rig, FVector(4000.0, -2500.0, 0.0)));
		View.ChunkStateVersion = 1;
		View.bSkipUnchanged = true;
		FVoxelFarFieldChanges Changes;
		auto NoChunks = [](const FIntVector&) { return false; };
		const double Deadline = FPlatformTime::Seconds() + 60.0;
		bool bSettledOnce = false;
		while (FPlatformTime::Seconds() < Deadline)
		{
			Rig->FarField.Update(View, NoChunks, Changes);
			if (Rig->FarField.WasLastUpdateSkipped() && Rig->FarField.GetBuildsPending() == 0)
			{
				bSettledOnce = true;
				break;
			}
			FPlatformProcess::Sleep(0.005f);
		}
		TestTrue(TEXT("Builds on the pool: the far field settles and skips"), bSettledOnce);
		FVoxelFarFieldRegion Region;
		Region.Box = FBox2D(FVector2D(View.Viewer) - FVector2D(500.0), FVector2D(View.Viewer) + FVector2D(500.0));
		Rig->FarField.InvalidateRegion(Region);
		int32 BuiltLanded = 0;
		int32 SkippedWhileBuilding = 0;
		const double BuildDeadline = FPlatformTime::Seconds() + 30.0;
		while (FPlatformTime::Seconds() < BuildDeadline && (BuiltLanded == 0 || Rig->FarField.GetBuildsPending() > 0))
		{
			Rig->FarField.Update(View, NoChunks, Changes);
			BuiltLanded += Changes.Built.Num();
			SkippedWhileBuilding += Rig->FarField.WasLastUpdateSkipped() && Rig->FarField.GetBuildsInFlight() > 0 ? 1 : 0;
			FPlatformProcess::Sleep(0.001f);
		}
		TestTrue(TEXT("Builds on the pool: the rebuilds an invalidation launched land and are applied"), BuiltLanded > 0);
		Note(*this, FString::Printf(TEXT("Builds on the pool: %d rebuilt patches applied, %d updates skipped while they were in flight"), BuiltLanded, SkippedWhileBuilding));
		Rig->FarField.Update(View, NoChunks, Changes);
		Rig->FarField.Update(View, NoChunks, Changes);
		TestTrue(TEXT("Builds on the pool: settled again, skipped"), Rig->FarField.WasLastUpdateSkipped());
	}
	return true;
}

/**
 * G293: the time rules (a covered edge patch hides at HideDelay, a hidden patch is released at ReleaseDelay) land in the
 * same update as on the full walk, with skipped updates between.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldIncrementalTimeRulesTest,
	"VoxelWorlds.FarField.Incremental.TimeRules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldIncrementalTimeRulesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelFarFieldIncrementalTestUtils;

	for (const bool bPlanet : { false, true })
	{
		const FString World = bPlanet ? TEXT("Planet") : TEXT("Plane");
		FTwin Twin(bPlanet);
		const FVector Site = bPlanet ? FVector(-0.2, 1.0, 0.4) : FVector(-6000.0, 3500.0, 0.0);
		FVoxelFarFieldView View = Twin.New->MakeView(ViewerAt(*Twin.New, Site));
		View.HideDelay = 1.0;
		View.ReleaseDelay = 3.0;
		Twin.SetWindow(WindowChunks(*Twin.New, View, Twin.New->ViewDistance));
		TArray<FString> Differences;
		int32 HiddenAt = -1;
		int32 ReleasedAt = -1;
		int32 SkippedBeforeHide = 0;
		int32 SkippedBeforeRelease = 0;
		int32 Hidden = 0;
		// Built and selected at the start (no time passing): the covered edge patches inside the prebuild radius stand in
		// without a mesh at once; those near the window's edge are built and drawn until their chunks have been drawn a second.
		for (int32 Step = 0; Step < 300; ++Step)
		{
			Twin.Step(View, Differences);
			if (Twin.FullChanges.IsEmpty() && Twin.Full->FarField.GetBuildsPending() == 0 && Step > 2)
			{
				break;
			}
		}
		const int32 HiddenFirst = Twin.Full->FarField.GetHiddenEdgeCount();
		// Then 0.05 s steps for 5 s: the edge patches near the edge hide at 1 s; patches hidden since the start (parents under
		// drawn children) are released at 3 s.
		for (int32 Step = 0; Step < 100; ++Step)
		{
			View.Time += 0.05;
			Twin.Step(View, Differences);
			const bool bSkipped = Twin.New->FarField.WasLastUpdateSkipped();
			if (HiddenAt < 0 && Twin.Full->FarField.GetHiddenEdgeCount() > HiddenFirst)
			{
				HiddenAt = Step;
				Hidden = Twin.Full->FarField.GetHiddenEdgeCount() - HiddenFirst;
				TestTrue(*FString::Printf(TEXT("%s: the edge patches hide once their chunks have been drawn HideDelay"), *World), View.Time >= View.HideDelay);
				TestFalse(*FString::Printf(TEXT("%s: the update that hides the covered edge patches is not skipped"), *World), bSkipped);
			}
			if (HiddenAt < 0 && bSkipped)
			{
				++SkippedBeforeHide;
			}
			if (HiddenAt >= 0 && ReleasedAt < 0 && Twin.FullChanges.Removed.Num() > 0)
			{
				ReleasedAt = Step;
				TestFalse(*FString::Printf(TEXT("%s: the update that releases hidden patches is not skipped"), *World), bSkipped);
			}
			if (HiddenAt >= 0 && ReleasedAt < 0 && bSkipped)
			{
				++SkippedBeforeRelease;
			}
		}
		TestTrue(*FString::Printf(TEXT("%s: covered edge patches hide after the delay"), *World), HiddenAt > 0);
		TestTrue(*FString::Printf(TEXT("%s: hidden patches are released after the delay"), *World), ReleasedAt > HiddenAt);
		TestTrue(*FString::Printf(TEXT("%s: updates skipped before the hide"), *World), SkippedBeforeHide > 5);
		TestTrue(*FString::Printf(TEXT("%s: updates skipped before the release"), *World), SkippedBeforeRelease > 5);
		Note(*this, FString::Printf(TEXT("%s: %d edge patches hidden at step %d, a release at step %d; %d and %d updates skipped before them; %d of %d skipped in all"),
			*World, Hidden, HiddenAt, ReleasedAt, SkippedBeforeHide, SkippedBeforeRelease, Twin.Skipped, Twin.Steps));
		ReportDifferences(*this, World, Differences);
	}
	return true;
}

/**
 * G293: the covered tests by changed chunks select what a test of every edge patch does, on a plane and a planet, while the
 * window moves and flickers; the checking mode finds no difference, and finds one when a changed chunk is not told.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldIncrementalChangedChunksTest,
	"VoxelWorlds.FarField.Incremental.ChangedChunks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldIncrementalChangedChunksTest::RunTest(const FString& Parameters)
{
	using namespace VoxelFarFieldIncrementalTestUtils;
	// The checking mode's warning for the change not told below, on the plane and the planet.
	AddExpectedMessage(TEXT("voxel.World.FarFieldCheck: the covered answer kept"), EAutomationExpectedMessageFlags::Contains, 0);

	for (const bool bPlanet : { false, true })
	{
		for (const bool bCheck : { false, true })
		{
			const FString World = FString::Printf(TEXT("%s%s"), bPlanet ? TEXT("Planet") : TEXT("Plane"), bCheck ? TEXT(", checking mode") : TEXT(""));
			FTwin Twin(bPlanet);
			Twin.bCheck = bCheck;
			FRandomStream Random(293);
			const FVector Start = bPlanet ? FVector(1.0, 0.3, 0.6) : FVector(4000.0, -2500.0, 0.0);
			const FVector Step = bPlanet ? FVector(0.0, 0.0, 0.004) : FVector(0.6 * Twin.New->Settings.ChunkWorldSize, 0.0, 0.0);
			TArray<FString> Differences;
			double Time = 0.0;
			for (int32 Move = 0; Move < 12; ++Move)
			{
				FVoxelFarFieldView View = Twin.New->MakeView(ViewerAt(*Twin.New, Start + Step * Move));
				View.HideDelay = 0.2;
				TSet<FIntVector> Window = WindowChunks(*Twin.New, View, Twin.New->ViewDistance);
				// The window fills: its chunks arrive a few at a time, nearest last, then a few flicker (a re-mesh, an unload).
				TArray<FIntVector> Arriving;
				for (const FIntVector& Chunk : Window)
				{
					if (!Twin.Drawn.Contains(Chunk))
					{
						Arriving.Add(Chunk);
					}
				}
				TSet<FIntVector> Partial = Twin.Drawn.Intersect(Window);
				for (int32 Batch = 0; Batch < 6; ++Batch)
				{
					for (int32 Index = Batch; Index < Arriving.Num(); Index += 6)
					{
						Partial.Add(Arriving[Index]);
					}
					Twin.SetWindow(Partial);
					View.Time = (Time += 0.05);
					Twin.Step(View, Differences);
				}
				TArray<FIntVector> Loaded = Window.Array();
				for (int32 Flicker = 0; Flicker < 6 && Loaded.Num() > 0; ++Flicker)
				{
					Twin.Toggle(Loaded[Random.RandRange(0, Loaded.Num() - 1)]);
					View.Time = (Time += 0.05);
					Twin.Step(View, Differences);
				}
				Twin.SetWindow(Window);
				for (int32 Rest = 0; Rest < 4; ++Rest)
				{
					View.Time = (Time += 0.1);
					Twin.Step(View, Differences);
				}
			}
			TestTrue(*FString::Printf(TEXT("%s: edge patches tested"), *World), Twin.FullTests > 100);
			if (bCheck)
			{
				TestEqual(*FString::Printf(TEXT("%s: no skip would have changed the selection"), *World), Twin.New->FarField.GetSkipDifferences(), 0ll);
				TestEqual(*FString::Printf(TEXT("%s: no kept cover would have been wrong"), *World), Twin.New->FarField.GetCoveredDifferences(), 0ll);
				TestTrue(*FString::Printf(TEXT("%s: skips and kept covers checked"), *World), Twin.New->FarField.GetSkipChecks() > 0 && Twin.New->FarField.GetCoveredChecks() > 0);
				Note(*this, FString::Printf(TEXT("%s: %lld skips and %lld kept covers checked"), *World, Twin.New->FarField.GetSkipChecks(), Twin.New->FarField.GetCoveredChecks()));
			}
			else
			{
				TestTrue(*FString::Printf(TEXT("%s: a fifth of the covered tests or fewer"), *World), Twin.NewTests * 5 <= Twin.FullTests);
				TestTrue(*FString::Printf(TEXT("%s: covers kept"), *World), Twin.Kept > 0);
			}
			Note(*this, FString::Printf(TEXT("%s: %lld covered tests against %lld on the full walk, %lld kept, %d of %d updates skipped"),
				*World, Twin.NewTests, Twin.FullTests, Twin.Kept, Twin.Skipped, Twin.Steps));
			ReportDifferences(*this, World, Differences);
		}

		// The checking mode sees a change that is not told: a chunk under a covered edge patch leaves without a word.
		{
			const FString World = bPlanet ? TEXT("Planet, a change not told") : TEXT("Plane, a change not told");
			FTwin Twin(bPlanet);
			Twin.bCheck = true;
			FVoxelFarFieldView View = Twin.New->MakeView(ViewerAt(*Twin.New, bPlanet ? FVector(1.0, 0.3, 0.6) : FVector(4000.0, -2500.0, 0.0)));
			Twin.SetWindow(WindowChunks(*Twin.New, View, Twin.New->ViewDistance));
			TArray<FString> Differences;
			for (int32 Step = 0; Step < 50; ++Step)
			{
				View.Time += 0.05;
				Twin.Step(View, Differences);
			}
			// A chunk an edge patch's cover reads.
			FIntVector Under = FIntVector::ZeroValue;
			bool bFound = false;
			Twin.New->FarField.ForEachPatch([&](const FVoxelFarField::FPatchState& Patch)
			{
				if (!bFound && Patch.bCovered && Patch.SurfaceChunks.Num() > 0)
				{
					Under = Patch.SurfaceChunks[0];
					bFound = true;
				}
			});
			TestTrue(*FString::Printf(TEXT("%s: a covered edge patch"), *World), bFound);
			Twin.Toggle(Under, /*bTell*/ false);
			View.Time += 0.05;
			Twin.Step(View, Differences);
			TestTrue(*FString::Printf(TEXT("%s: the checking mode counts the kept cover a test changed"), *World), Twin.New->FarField.GetCoveredDifferences() > 0);
		}
	}
	return true;
}

/**
 * G293 in a chunk manager: a plane, an island, a block world and a planet streamed with voxel.World.FarFieldCheck 1 through
 * settling, a walk, a cut far away and back, an edit and a conditioning zone: no skip and no kept cover the walk disagrees
 * with; with the checking mode off the updates at rest are skipped.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldIncrementalChunkManagerTest,
	"VoxelWorlds.FarField.Incremental.ChunkManager",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldIncrementalChunkManagerTest::RunTest(const FString& Parameters)
{
	using namespace VoxelFarFieldIncrementalTestUtils;
	using namespace VoxelTesting;

	const FScopedCVar FarFieldVar(TEXT("voxel.World.FarField"), TEXT("1"));
	const FScopedCVar IncrementalVar(TEXT("voxel.World.FarFieldIncremental"), TEXT("1"));
	// The zone each world adds after streaming (to invalidate its patches) is registered late on purpose.
	AddExpectedMessage(TEXT("A conditioning zone was registered after this world had built"), EAutomationExpectedMessageFlags::Contains, 0);
	enum class EKind : uint8 { Plane, Island, Cubic, Planet };
	for (const EKind Kind : { EKind::Plane, EKind::Island, EKind::Cubic, EKind::Planet })
	{
		const FString World = Kind == EKind::Plane ? TEXT("Plane") : Kind == EKind::Island ? TEXT("Island") : Kind == EKind::Cubic ? TEXT("Block world") : TEXT("Planet");
		FScopedCVar CheckVar(TEXT("voxel.World.FarFieldCheck"), TEXT("1"));
		FTestChunkWorld W;
		W.Create(TEXT("FarFieldIncrementalWorld"));
		if (Kind == EKind::Planet)
		{
			W.ConfigureSmallPlanet(60000.0f);
		}
		else
		{
			W.ConfigureSmallPlane();
		}
		if (Kind == EKind::Island)
		{
			W.Config->WorldMode = EWorldMode::IslandBowl;
			W.Config->IslandRadius = 20000.0f;
			W.Config->IslandFalloffWidth = 5000.0f;
			W.Config->IslandEdgeHeight = -1200.0f;
			W.Config->IslandSeabedDepth = 1500.0f;
			W.Config->IslandSeabedSlopeWidth = 4000.0f;
			W.Config->IslandFarOceanDistance = 200000.0f;
		}
		if (Kind == EKind::Cubic)
		{
			W.Config->MeshingMode = EMeshingMode::Cubic;
			W.Config->bUseGreedyMeshing = true;
		}
		W.Config->ViewDistance = 6000.0f;
		W.Config->LODBands = { FLODBand(0.0f, 6000.0f, 0, 1) };
		W.Config->bEnableWaterLevel = true;
		W.Config->WaterLevel = -200.0f;
		W.Config->WaterRadius = 60000.0f - 300.0f;
		W.StartManager();
		if (!TestTrue(*FString::Printf(TEXT("%s: a far field"), *World), W.Manager->HasFarField()))
		{
			continue;
		}
		auto ViewerAtSite = [&W, Kind](double U, double V)
		{
			if (Kind == EKind::Planet)
			{
				const FVector Direction = FVector(1.0, U / 60000.0, V / 60000.0).GetSafeNormal();
				return W.ViewerOverSurface(Direction, 300.0f);
			}
			return W.ViewerOverGround(U, V, 300.0f);
		};
		auto TickFor = [&W](int32 Ticks)
		{
			for (int32 Tick = 0; Tick < Ticks; ++Tick)
			{
				W.Tick();
			}
		};

		// Settle at a site, walk a few chunks a tick at a time, cut far away and back, then an edit and a conditioning zone.
		FVector Viewer = ViewerAtSite(1000.0, -2000.0);
		W.SetViewer(Viewer);
		TestTrue(*FString::Printf(TEXT("%s: the window settles"), *World), W.Settle());
		TickFor(60);
		for (int32 Step = 0; Step < 60; ++Step)
		{
			Viewer = ViewerAtSite(1000.0 + 80.0 * Step, -2000.0 + 20.0 * Step);
			W.Manager->SetBenchmarkView(true, Viewer);
			W.Tick();
		}
		TestTrue(*FString::Printf(TEXT("%s: settles after the walk"), *World), W.Settle());
		TickFor(30);
		W.SetViewer(ViewerAtSite(14000.0, 9000.0));
		TestTrue(*FString::Printf(TEXT("%s: settles after a cut"), *World), W.Settle());
		TickFor(30);
		W.SetViewer(Viewer);
		TestTrue(*FString::Printf(TEXT("%s: settles after the cut back"), *World), W.Settle());
		TickFor(30);
		FVoxelBrushParams Brush;
		Brush.Shape = EVoxelBrushShape::Sphere;
		Brush.Radius = 400.0f;
		Brush.Strength = 1.0f;
		Brush.FalloffType = EVoxelBrushFalloff::Linear;
		Brush.DensityDelta = 255;
		const FVector Ground = Viewer - (Kind == EKind::Planet ? Viewer.GetSafeNormal() : FVector::UpVector) * 300.0;
		W.Manager->GetEditManager()->ApplyBrushEdit(Ground, Brush, EEditMode::Subtract);
		TestTrue(*FString::Printf(TEXT("%s: settles after an edit"), *World), W.Settle());
		TickFor(30);
		if (Kind == EKind::Planet)
		{
			W.Manager->AddConditioningZone(FVoxelConditioningZone::MakeRadial(Viewer.GetSafeNormal(), 600.0f, 400.0f, 100.0f, 1.0f));
		}
		else
		{
			W.Manager->AddConditioningZone(FVoxelConditioningZone(FVector2D(Viewer) + FVector2D(1500.0, 0.0), 600.0f, 400.0f, 100.0f, 1.0f));
		}
		TestTrue(*FString::Printf(TEXT("%s: settles after a conditioning zone"), *World), W.Settle());
		TickFor(30);

		const FVoxelFarFieldStats& Stats = W.Manager->GetFarFieldStats();
		TestEqual(*FString::Printf(TEXT("%s: no skip the walk disagrees with"), *World), Stats.CheckSkipDifferences, 0ll);
		TestEqual(*FString::Printf(TEXT("%s: no kept cover the test disagrees with"), *World), Stats.CheckCoveredDifferences, 0ll);
		TestTrue(*FString::Printf(TEXT("%s: skips checked"), *World), Stats.CheckSkips > 0);
		TestTrue(*FString::Printf(TEXT("%s: kept covers checked"), *World), Stats.CheckCovered > 0);
		Note(*this, FString::Printf(TEXT("%s: %lld skips and %lld kept covers checked, none different; %s"), *World, Stats.CheckSkips, Stats.CheckCovered,
			*W.Manager->GetFarFieldDebugString()));

		// With the checking mode off, the updates at rest are skipped.
		if (IConsoleVariable* Check = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.World.FarFieldCheck")))
		{
			Check->Set(TEXT("0"), ECVF_SetByCode); // the scoped cvar puts its old value back at the end of the case
		}
		const int64 SkippedBefore = Stats.SkippedUpdates;
		TickFor(30);
		TestTrue(*FString::Printf(TEXT("%s: at rest the chunk manager's far-field updates are skipped"), *World), W.Manager->GetFarFieldStats().SkippedUpdates > SkippedBefore + 10);
		W.Stop();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
