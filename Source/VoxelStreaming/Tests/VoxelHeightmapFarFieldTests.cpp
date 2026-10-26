// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Camera/CameraActor.h"
#include "Camera/PlayerCameraManager.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "ProceduralMeshComponent.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelPlanetFarField.h"
#include "VoxelBlockSurface.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "IVoxelWorldMode.h"
#include "DistanceBandLODStrategy.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The far field on heightmap worlds (WORLD_MODES_REVIEW_PLAN.md §23, milestone MP3, WM-MP1, decisions D36 and D37): the
// planet's analytic far surface (FVoxelPlanetFarField) over a plane or an island as a quadtree of root tiles on the chunk
// grid, the generated height its surface, the WaterLevel plane its sea.
//
// - Plane.Quadtree: nothing of the voxel window loaded, from 300 uu over the ground at four sites: the selection settles,
//   every drawn patch outside the band meets the screen-space error bound, drawn neighbours differ by at most SkirtLevels
//   levels, and along every shared edge the gap between the two patches' edges is within the higher one's skirt.
// - Plane.Hole: with the window loaded (every column within the view distance drawn), no drawn terrain patch lies wholly
//   inside it, every place out to three view distances is drawn by a patch or a chunk, the hole follows a move with no
//   rebuild, chunks missing inside the window are drawn by patches, and a band patch's sea hides with its terrain (with no
//   water-tile query; a chunk manager's hides it where the column's water tile is drawn, Plane.ChunkManager).
// - Plane.MatchesSurface: every surface vertex at the generated height less the sink, normals up, one material per
//   triangle, skirts within their depth, the sea on the WaterLevel plane, band patches knowing their chunks.
// - Cubic.BlockTop: on a cubic world the surface is the block tops (FVoxelBlockSurface): every vertex a whole number of
//   voxels over the origin, within a block over the generated height.
// - Plane.MaterialMix: in biome blend zones a vertex's mix is what the voxels around it take (as the planet's, G45).
// - Island.Bounds: roots cover the island's bounds box only (its corners past the circle hold the sea, where the far
//   ocean's open sea starts), a far distance culls patches past it, and a camera far outside leaves no root.
// - Plane.ChunkManager: a game world on a plane with a sea draws its far field with the sea, has a component per built
//   patch showing what is drawn, hides band patches under the window; voxel.World.FarField 0 drops it. An island's far
//   ocean keeps its open sea and seabed and builds no grid water or floor while the far field is on (D37).
// ---------------------------------------------------------------------------

namespace VoxelHeightmapFarFieldTestUtils
{
	constexpr double ChunkWorld = 1600.0;   // 32 voxels of 50 uu
	// The far field's view: a window of 7.5 chunks (the hole rule needs a window wider than a band patch's reach: a band
	// patch's surface chunks include the next column's along its edges).
	constexpr double ViewDistance = 12000.0;
	constexpr double UnloadDistance = 14400.0;

	/**
	 * Sets a console variable for a scope at the priority it already holds: a set below that priority is ignored (a test
	 * that set it by console earlier in a suite run), and a set above it would outrank the other tests' code-priority sets.
	 */
	struct FScopedCVar
	{
		IConsoleVariable* Var = nullptr;
		FString Previous;
		EConsoleVariableFlags Priority = ECVF_SetByCode;
		FScopedCVar(const TCHAR* Name, const TCHAR* Value)
		{
			Var = IConsoleManager::Get().FindConsoleVariable(Name);
			if (Var)
			{
				Previous = Var->GetString();
				Priority = static_cast<EConsoleVariableFlags>(FMath::Max(static_cast<uint32>(Var->GetFlags() & ECVF_SetByMask), static_cast<uint32>(ECVF_SetByCode)));
				Var->Set(Value, Priority);
			}
		}
		~FScopedCVar()
		{
			if (Var)
			{
				Var->Set(*Previous, Priority);
			}
		}
	};

	/** The small plane's configuration (terrain +-800, chunks of 32 voxels of 50 uu, a sea at -200). Rooted. */
	UVoxelWorldConfiguration* MakePlaneConfig(EMeshingMode Meshing = EMeshingMode::DualContouring)
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::InfinitePlane;
		Config->MeshingMode = Meshing;
		Config->bUseGreedyMeshing = Meshing == EMeshingMode::Cubic;
		Config->WorldOrigin = FVector(0.0, 0.0, 0.0);
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

	/** An island (R 20,000, falloff 5,000, a sea at -300) with the plane's chunks. Rooted. */
	UVoxelWorldConfiguration* MakeIslandConfig()
	{
		UVoxelWorldConfiguration* Config = MakePlaneConfig();
		Config->WorldMode = EWorldMode::IslandBowl;
		Config->IslandRadius = 20000.0f;
		Config->IslandFalloffWidth = 5000.0f;
		Config->IslandEdgeHeight = -1200.0f;
		Config->IslandSeabedDepth = 1500.0f;
		Config->IslandSeabedSlopeWidth = 4000.0f;
		Config->IslandFarOceanDistance = 200000.0f;
		Config->WaterLevel = -300.0f;
		return Config;
	}

	/** The chunk manager's far-field settings for a heightmap configuration (UVoxelChunkManager::RebuildFarField). */
	FVoxelFarFieldSettings MakeSettings(const UVoxelWorldConfiguration& Config, const IVoxelWorldMode& Mode, int32 RootChunks, double FarDistance, bool bSea)
	{
		FVoxelFarFieldSettings Settings;
		Settings.bHeightmap = true;
		Settings.GridOrigin = Config.WorldOrigin;
		Settings.ChunkWorldSize = Config.GetChunkWorldSize();
		Settings.VoxelSize = Config.VoxelSize;
		Settings.PatchQuads = 16;
		Settings.Sink = Config.VoxelSize;
		Settings.SeaMargin = 2.0 * Config.VoxelSize;
		Settings.RootChunks = RootChunks;
		Settings.FarDistance = FarDistance;
		Settings.Bounds = Mode.GetWorldBounds();
		float MinHeight = 0.0f, MaxHeight = 0.0f;
		Mode.GetTerrainHeightBounds(MinHeight, MaxHeight);
		Settings.MinAltitude = MinHeight;
		Settings.MaxAltitude = MaxHeight;
		Settings.bSea = bSea;
		Settings.SeaZ = Config.WaterLevel;
		Settings.BandLevel = FVoxelFarFieldSettings::ComputeHeightmapBandLevel(RootChunks);
		Settings.ZoneGatherMaxSize = 64.0 * Settings.ChunkWorldSize + 1.0;
		return Settings;
	}

	/** A far field over a heightmap configuration with synchronous builds. */
	struct FRig
	{
		UVoxelWorldConfiguration* Config = nullptr;
		TSharedPtr<IVoxelWorldMode> Mode;
		TSharedPtr<const FVoxelFarFieldSurface> Surface;
		FVoxelFarFieldSettings Settings;
		FVoxelPlanetFarField FarField;
		const TArray<FVoxelConditioningZone> NoZones;

		FRig(UVoxelWorldConfiguration* InConfig, int32 RootChunks, double FarDistance, bool bSea)
		{
			Config = InConfig;
			Mode = FVoxelWorldModeFactory::Create(*Config);
			Surface = FVoxelFarFieldSurface::Make(*Config, Mode);
			Settings = MakeSettings(*Config, *Mode, RootChunks, FarDistance, bSea);
			FarField.Initialize(Settings, Surface);
			FarField.SetSynchronousBuilds(true);
			FarField.SetMaxBuildsInFlight(256);
		}

		~FRig()
		{
			FarField.Reset();
			Config->RemoveFromRoot();
		}

		double Height(double X, double Y) const { return Surface->SampleHeight(X, Y, NoZones); }
		FVector SurfacePoint(const FVector2D& XY) const { return FVector(XY.X, XY.Y, Height(XY.X, XY.Y)); }

		FIntVector ChunkOf(const FVector& Point) const
		{
			const FVector Local = (Point - Settings.GridOrigin) / Settings.ChunkWorldSize;
			return FIntVector(FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), FMath::FloorToInt32(Local.Z));
		}

		FVector2D ColumnCentre(const FIntVector& Coord) const
		{
			return FVector2D(Settings.GridOrigin.X + (Coord.X + 0.5) * Settings.ChunkWorldSize, Settings.GridOrigin.Y + (Coord.Y + 0.5) * Settings.ChunkWorldSize);
		}

		FVoxelFarFieldView MakeView(const FVector& Viewer) const
		{
			FVoxelFarFieldView View;
			View.Camera = Viewer;
			View.Viewer = Viewer;
			View.PixelsPerRadian = 300.0;
			View.MaxScreenError = 4.0;
			View.GroundRadius = 0.0;
			View.BandRadius = UnloadDistance + 0.5 * UE_DOUBLE_SQRT_3 * Settings.ChunkWorldSize;
			View.PrebuildRadius = 0.75 * ViewDistance;
			View.Time = 0.0;
			View.HideDelay = 0.0;
			View.ChunkStateVersion = -1;
			return View;
		}

		/** Update until nothing is left to build and an update changes nothing. Returns the updates it took (-1: never settled). */
		int32 Converge(const FVoxelFarFieldView& View, TFunctionRef<bool(const FIntVector&)> IsDrawn, int32 MaxUpdates = 300)
		{
			FVoxelFarFieldChanges Changes;
			for (int32 Update = 1; Update <= MaxUpdates; ++Update)
			{
				FarField.Update(View, IsDrawn, Changes);
				if (FarField.GetBuildsPending() == 0 && Changes.Built.Num() == 0 && Changes.Removed.Num() == 0 && Changes.Visibility.Num() == 0)
				{
					return Update;
				}
			}
			return -1;
		}

		/** A loaded voxel window: a chunk is drawn when its column's centre lies within Radius of the viewer's XY. */
		bool InWindow(const FVoxelFarFieldView& View, const FIntVector& Coord, double Radius) const
		{
			return FVector2D::Distance(ColumnCentre(Coord), FVector2D(View.Viewer.X, View.Viewer.Y)) <= Radius;
		}

		/** The XY distance from a point to a patch's footprint (0 inside). */
		static double DistanceToBox(const FVector2D& Point, const FBox2D& Box)
		{
			const double DX = FMath::Max(FMath::Max(Box.Min.X - Point.X, Point.X - Box.Max.X), 0.0);
			const double DY = FMath::Max(FMath::Max(Box.Min.Y - Point.Y, Point.Y - Box.Max.Y), 0.0);
			return FMath::Sqrt(DX * DX + DY * DY);
		}

		/** The XY of a patch's edge at a fraction along it (edge 0: J 0, 1: I Q, 2: J Q, 3: I 0), and the outward normal. */
		static FVector2D EdgePointXY(const FBox2D& Box, int32 Edge, double Along, FVector2D* OutOutward = nullptr)
		{
			const FVector2D Size = Box.GetSize();
			FVector2D Outward;
			FVector2D XY;
			switch (Edge)
			{
			case 0: XY = FVector2D(Box.Min.X + Along * Size.X, Box.Min.Y); Outward = FVector2D(0.0, -1.0); break;
			case 1: XY = FVector2D(Box.Max.X, Box.Min.Y + Along * Size.Y); Outward = FVector2D(1.0, 0.0); break;
			case 2: XY = FVector2D(Box.Min.X + Along * Size.X, Box.Max.Y); Outward = FVector2D(0.0, 1.0); break;
			default: XY = FVector2D(Box.Min.X, Box.Min.Y + Along * Size.Y); Outward = FVector2D(-1.0, 0.0); break;
			}
			if (OutOutward)
			{
				*OutOutward = Outward;
			}
			return XY;
		}

		/** The drawn height of a patch's edge polyline (chords between its edge vertices) at an XY on that edge. */
		double EdgeHeight(const FBox2D& Box, int32 Edge, const FVector2D& XY) const
		{
			const int32 Q = Settings.PatchQuads;
			const FVector2D Size = Box.GetSize();
			const double Along = (Edge == 0 || Edge == 2) ? (XY.X - Box.Min.X) / Size.X : (XY.Y - Box.Min.Y) / Size.Y;
			const double S = FMath::Clamp(Along, 0.0, 1.0) * Q;
			const int32 K = FMath::Clamp(FMath::FloorToInt32(S), 0, Q - 1);
			const double T = S - K;
			const FVector2D A = EdgePointXY(Box, Edge, static_cast<double>(K) / Q);
			const FVector2D B = EdgePointXY(Box, Edge, static_cast<double>(K + 1) / Q);
			return FMath::Lerp(Height(A.X, A.Y) - Settings.Sink, Height(B.X, B.Y) - Settings.Sink, T);
		}

		/** The edge of a patch an XY on its boundary lies on (false when it is not on one). */
		static bool FindEdge(const FBox2D& Box, const FVector2D& XY, int32& OutEdge)
		{
			const double Tolerance = 1.0e-3 * Box.GetSize().X;
			const double D[4] = { FMath::Abs(XY.Y - Box.Min.Y), FMath::Abs(XY.X - Box.Max.X), FMath::Abs(XY.Y - Box.Max.Y), FMath::Abs(XY.X - Box.Min.X) };
			int32 Best = 0;
			for (int32 Edge = 1; Edge < 4; ++Edge)
			{
				Best = D[Edge] < D[Best] ? Edge : Best;
			}
			OutEdge = Best;
			return D[Best] <= Tolerance && XY.X >= Box.Min.X - Tolerance && XY.X <= Box.Max.X + Tolerance
				&& XY.Y >= Box.Min.Y - Tolerance && XY.Y <= Box.Max.Y + Tolerance;
		}
	};

	TArray<FVector2D> GetSites()
	{
		return { FVector2D(0.0, 0.0), FVector2D(4000.0, -2500.0), FVector2D(-37000.0, 51000.0), FVector2D(120000.0, 80000.0) };
	}
}

/**
 * WM-MP1: the quadtree on a plane. Nothing of the voxel window is loaded (every patch draws), from 300 uu over the ground at
 * four sites (one on a root tile's corner): the selection settles; every drawn patch outside the band meets the error
 * bound; drawn neighbours differ by at most SkirtLevels levels; along every shared edge the gap between the two patches'
 * edges is within the higher one's skirt; the roots are the tiles within the far distance of the camera.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldPlaneQuadtreeTest,
	"VoxelWorlds.FarField.Plane.Quadtree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldPlaneQuadtreeTest::RunTest(const FString& Parameters)
{
	using namespace VoxelHeightmapFarFieldTestUtils;

	constexpr double FarDistance = 150000.0;
	for (const FVector2D& Site : GetSites())
	{
		FRig Rig(MakePlaneConfig(), 64, FarDistance, /*bSea=*/true);
		const FVector Viewer = Rig.SurfacePoint(Site) + FVector(0.0, 0.0, 300.0);
		const FVoxelFarFieldView View = Rig.MakeView(Viewer);
		const double Start = FPlatformTime::Seconds();
		const int32 Updates = Rig.Converge(View, [](const FIntVector&) { return false; });
		const double Seconds = FPlatformTime::Seconds() - Start;
		const FString Name = FString::Printf(TEXT("site (%.0f, %.0f)"), Site.X, Site.Y);
		TestTrue(*FString::Printf(TEXT("%s: the selection settles"), *Name), Updates > 0);

		// The roots: every tile of the root size overlapping the square of the far distance about the camera.
		const double RootTile = Rig.Settings.GetRootTileSize();
		const int32 X0 = FMath::FloorToInt32((Site.X - FarDistance) / RootTile), X1 = FMath::FloorToInt32((Site.X + FarDistance) / RootTile);
		const int32 Y0 = FMath::FloorToInt32((Site.Y - FarDistance) / RootTile), Y1 = FMath::FloorToInt32((Site.Y + FarDistance) / RootTile);
		const int32 ExpectedRoots = (X1 - X0 + 1) * (Y1 - Y0 + 1);
		int32 RootsBuilt = 0;
		for (const uint64 Key : Rig.FarField.GetHeightmapRoots())
		{
			const FVoxelPlanetFarField::FPatchState* Root = Rig.FarField.FindPatch(Key);
			RootsBuilt += Root && Root->bResident ? 1 : 0;
		}

		// The error bound, outside the band (inside it patches split to the band level whatever their error).
		const FVector2D ViewerXY(Viewer.X, Viewer.Y);
		int32 Drawn = 0, OverBound = 0, InBand = 0, MinLevel = 64, MaxLevel = 0, Culled = 0;
		double WorstPixels = 0.0;
		TArray<const FVoxelPlanetFarField::FPatchState*> DrawnPatches;
		Rig.FarField.ForEachPatch([&](const FVoxelPlanetFarField::FPatchState& Patch)
		{
			Culled += Patch.bCulled ? 1 : 0;
			if (!Patch.bDrawTerrain && !Patch.bDrawSea)
			{
				return;
			}
			++Drawn;
			DrawnPatches.Add(&Patch);
			MinLevel = FMath::Min(MinLevel, Patch.Key.Level);
			MaxLevel = FMath::Max(MaxLevel, Patch.Key.Level);
			if (FRig::DistanceToBox(ViewerXY, Patch.Box) < View.BandRadius || Patch.Key.Level >= Rig.Settings.BandLevel)
			{
				++InBand;
				return;
			}
			const double Distance = FMath::Max(FVector::Dist(View.Camera, Patch.BoundsCenter) - Patch.BoundsRadius, 1.0);
			const double Pixels = Patch.Error * View.PixelsPerRadian / Distance;
			WorstPixels = FMath::Max(WorstPixels, Pixels);
			OverBound += Pixels > View.MaxScreenError + 1.0e-6 ? 1 : 0;
		});

		// Cracks: walk every drawn terrain patch's edges and compare with the drawn neighbour just across.
		int32 EdgeSamples = 0, Cracks = 0, LevelJumps = 0, MaxLevelDifference = 0;
		double WorstGap = 0.0, WorstUncovered = 0.0;
		const int32 Q = Rig.Settings.PatchQuads;
		for (const FVoxelPlanetFarField::FPatchState* A : DrawnPatches)
		{
			if (!A->bDrawTerrain)
			{
				continue;
			}
			for (int32 Edge = 0; Edge < 4; ++Edge)
			{
				for (int32 S = 1; S < 4 * Q; ++S)
				{
					const double Along = static_cast<double>(S) / (4 * Q);
					FVector2D Outward;
					const FVector2D XY = FRig::EdgePointXY(A->Box, Edge, Along, &Outward);
					const FVector2D Outside = XY + Outward * 1.0e-4 * A->Box.GetSize().X;
					const FVoxelPlanetFarField::FPatchState* B = Rig.FarField.FindDrawnPatchAt(FVector(Outside.X, Outside.Y, 0.0));
					if (!B || B == A)
					{
						continue;
					}
					int32 EdgeB = 0;
					if (!FRig::FindEdge(B->Box, XY, EdgeB))
					{
						continue;
					}
					++EdgeSamples;
					const int32 Difference = FMath::Abs(A->Key.Level - B->Key.Level);
					MaxLevelDifference = FMath::Max(MaxLevelDifference, Difference);
					LevelJumps += Difference > Rig.Settings.SkirtLevels ? 1 : 0;
					const double ZA = Rig.EdgeHeight(A->Box, Edge, XY);
					const double ZB = Rig.EdgeHeight(B->Box, EdgeB, XY);
					const double Gap = FMath::Abs(ZA - ZB);
					const double Skirt = ZA > ZB ? A->SkirtDepth : B->SkirtDepth;
					WorstGap = FMath::Max(WorstGap, Gap);
					if (Gap > Skirt + 1.0e-3)
					{
						++Cracks;
						WorstUncovered = FMath::Max(WorstUncovered, Gap - Skirt);
					}
				}
			}
		}

		AddInfo(FString::Printf(TEXT("%s: %d roots (%d expected, %d built), %d patches (%d culled), %d drawn (levels %d-%d, %d in the band), settled in %d updates (%.2f s, %lld built, %.2f ms each); worst %.2f px (bound %.1f), %d over; %d edge samples, worst gap %.1f uu, %d cracks past the skirt (worst %.2f uu), level difference up to %d"),
			*Name, Rig.FarField.GetHeightmapRoots().Num(), ExpectedRoots, RootsBuilt, Rig.FarField.GetPatchCount(), Culled, Drawn, MinLevel, MaxLevel, InBand, Updates, Seconds,
			Rig.FarField.GetTotalBuilt(), Rig.FarField.GetTotalBuilt() > 0 ? Rig.FarField.GetTotalBuildMs() / Rig.FarField.GetTotalBuilt() : 0.0,
			WorstPixels, View.MaxScreenError, OverBound, EdgeSamples, WorstGap, Cracks, WorstUncovered, MaxLevelDifference));
		TestEqual(*FString::Printf(TEXT("%s: the roots are the tiles within the far distance"), *Name), Rig.FarField.GetHeightmapRoots().Num(), ExpectedRoots);
		TestEqual(*FString::Printf(TEXT("%s: every root is built"), *Name), RootsBuilt, ExpectedRoots);
		TestTrue(*FString::Printf(TEXT("%s: patches drawn"), *Name), Drawn > 6);
		TestTrue(*FString::Printf(TEXT("%s: the band level was reached near the viewer"), *Name), MaxLevel == Rig.Settings.BandLevel);
		TestEqual(*FString::Printf(TEXT("%s: every patch outside the band meets the error bound"), *Name), OverBound, 0);
		TestTrue(*FString::Printf(TEXT("%s: shared edges were found"), *Name), EdgeSamples > 100);
		TestEqual(*FString::Printf(TEXT("%s: no crack past a skirt"), *Name), Cracks, 0);
		TestEqual(*FString::Printf(TEXT("%s: neighbours within the skirts' levels"), *Name), LevelJumps, 0);
	}
	return true;
}

/**
 * WM-MP1: the hole on a plane. With the voxel window loaded around a viewer 300 uu over the ground (every chunk of every
 * column whose centre lies within the view distance is drawn): no drawn terrain patch lies wholly inside the window (each
 * reaches past the view distance less a chunk's diagonal); every place out to three view distances is drawn by a patch or
 * a voxel chunk; after the viewer moves three chunks the same holds and no patch that stayed was rebuilt; with chunks
 * missing inside the window (catch-up) their places are drawn by patches; and no band patch draws its sea without its
 * terrain (the water tiles draw the sea over streamed columns).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldPlaneHoleTest,
	"VoxelWorlds.FarField.Plane.Hole",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldPlaneHoleTest::RunTest(const FString& Parameters)
{
	using namespace VoxelHeightmapFarFieldTestUtils;

	const double ChunkDiagonal = UE_DOUBLE_SQRT_3 * ChunkWorld;
	const TArray<FVector2D> Sites = GetSites();
	for (int32 SiteIndex : { 1, 2 })
	{
		const FVector2D& Site = Sites[SiteIndex];
		FRig Rig(MakePlaneConfig(), 64, 150000.0, /*bSea=*/true);
		const FString Name = FString::Printf(TEXT("site (%.0f, %.0f)"), Site.X, Site.Y);

		// Checks a settled selection against a drawn-chunk rule: no drawn patch wholly inside the window, no gap.
		auto Check = [&](const TCHAR* Stage, const FVoxelFarFieldView& View, TFunctionRef<bool(const FIntVector&)> IsDrawn)
		{
			const FVector2D ViewerXY(View.Viewer.X, View.Viewer.Y);
			int32 DrawnTerrain = 0, Inside = 0, HiddenBand = 0, SeaWithoutTerrain = 0;
			Rig.FarField.ForEachPatch([&](const FVoxelPlanetFarField::FPatchState& Patch)
			{
				if (Patch.Key.Level >= Rig.Settings.BandLevel && !Patch.bDrawTerrain && Patch.bCovered)
				{
					++HiddenBand;
				}
				SeaWithoutTerrain += (Patch.bDrawSea && !Patch.bDrawTerrain) ? 1 : 0;
				if (!Patch.bDrawTerrain)
				{
					return;
				}
				++DrawnTerrain;
				// The farthest corner from the viewer's XY.
				double Far = 0.0;
				for (int32 Corner = 0; Corner < 4; ++Corner)
				{
					const FVector2D C((Corner & 1) ? Patch.Box.Max.X : Patch.Box.Min.X, (Corner & 2) ? Patch.Box.Max.Y : Patch.Box.Min.Y);
					Far = FMath::Max(Far, FVector2D::Distance(C, ViewerXY));
				}
				Inside += Far < ViewDistance - ChunkDiagonal ? 1 : 0;
			});

			// Every place on a polar grid out to three view distances: a drawn patch's or a drawn chunk's.
			int32 Samples = 0, Gaps = 0, ByVoxels = 0;
			for (double Distance = 0.0; Distance <= 3.0 * ViewDistance; Distance += ChunkWorld / 4.0)
			{
				const int32 Around = FMath::Max(8, FMath::CeilToInt32(UE_DOUBLE_TWO_PI * Distance / (ChunkWorld / 2.0)));
				for (int32 Step = 0; Step < Around; ++Step)
				{
					const double Theta = UE_DOUBLE_TWO_PI * Step / Around;
					const FVector2D XY = ViewerXY + FVector2D(FMath::Cos(Theta), FMath::Sin(Theta)) * Distance;
					++Samples;
					if (IsDrawn(Rig.ChunkOf(Rig.SurfacePoint(XY))))
					{
						++ByVoxels;
						continue;
					}
					Gaps += Rig.FarField.FindDrawnPatchAt(FVector(XY.X, XY.Y, 0.0)) ? 0 : 1;
				}
			}
			AddInfo(FString::Printf(TEXT("%s, %s: %d terrain patches drawn, %d band patches hidden by the window, %d wholly inside it, %d drawing the sea without their terrain; %d places, %d drawn by voxels, %d gaps"),
				*Name, Stage, DrawnTerrain, HiddenBand, Inside, SeaWithoutTerrain, Samples, ByVoxels, Gaps));
			TestEqual(*FString::Printf(TEXT("%s, %s: no terrain patch drawn wholly inside the window"), *Name, Stage), Inside, 0);
			TestEqual(*FString::Printf(TEXT("%s, %s: no gap"), *Name, Stage), Gaps, 0);
			TestTrue(*FString::Printf(TEXT("%s, %s: the window hides band patches"), *Name, Stage), HiddenBand > 0);
			TestTrue(*FString::Printf(TEXT("%s, %s: the voxels draw the middle"), *Name, Stage), ByVoxels > 0);
			TestEqual(*FString::Printf(TEXT("%s, %s: a patch's sea hides with its terrain"), *Name, Stage), SeaWithoutTerrain, 0);
		};

		// The window loaded.
		const FVector Viewer = Rig.SurfacePoint(Site) + FVector(0.0, 0.0, 300.0);
		const FVoxelFarFieldView View = Rig.MakeView(Viewer);
		auto Loaded = [&Rig, &View](const FIntVector& Coord) { return Rig.InWindow(View, Coord, ViewDistance); };
		TestTrue(*FString::Printf(TEXT("%s: settles"), *Name), Rig.Converge(View, Loaded) > 0);
		Check(TEXT("loaded"), View, Loaded);

		// The viewer moves three chunks east: the hole follows, nothing that stayed is rebuilt.
		TMap<uint64, uint32> Serials;
		Rig.FarField.ForEachPatch([&Serials](const FVoxelPlanetFarField::FPatchState& Patch)
		{
			if (Patch.bBuilt)
			{
				Serials.Add(Patch.Key.Pack(), Patch.BuildSerial);
			}
		});
		const FVector2D MovedSite = Site + FVector2D(3.0 * ChunkWorld, 0.0);
		const FVector MovedViewer = Rig.SurfacePoint(MovedSite) + FVector(0.0, 0.0, 300.0);
		const FVoxelFarFieldView MovedView = Rig.MakeView(MovedViewer);
		auto MovedLoaded = [&Rig, &MovedView](const FIntVector& Coord) { return Rig.InWindow(MovedView, Coord, ViewDistance); };
		TestTrue(*FString::Printf(TEXT("%s: settles after the move"), *Name), Rig.Converge(MovedView, MovedLoaded) > 0);
		Check(TEXT("moved three chunks"), MovedView, MovedLoaded);
		int32 Stayed = 0, Rebuilt = 0;
		Rig.FarField.ForEachPatch([&](const FVoxelPlanetFarField::FPatchState& Patch)
		{
			if (const uint32* Serial = Serials.Find(Patch.Key.Pack()))
			{
				++Stayed;
				Rebuilt += Patch.BuildSerial != *Serial ? 1 : 0;
			}
		});
		AddInfo(FString::Printf(TEXT("%s: after the move %d built patches stayed, %d rebuilt"), *Name, Stayed, Rebuilt));
		TestTrue(*FString::Printf(TEXT("%s: patches stayed"), *Name), Stayed > 0);
		TestEqual(*FString::Printf(TEXT("%s: the hole moved without a rebuild"), *Name), Rebuilt, 0);

		// Catch-up: the chunks within two chunks of the viewer's ground point are missing.
		const FVector GroundPoint = Rig.SurfacePoint(MovedSite);
		auto CatchUp = [&Rig, &MovedView, GroundPoint](const FIntVector& Coord)
		{
			return Rig.InWindow(MovedView, Coord, ViewDistance)
				&& FVector2D::Distance(Rig.ColumnCentre(Coord), FVector2D(GroundPoint.X, GroundPoint.Y)) > 2.0 * ChunkWorld;
		};
		TestTrue(*FString::Printf(TEXT("%s: settles with chunks missing"), *Name), Rig.Converge(MovedView, CatchUp) > 0);
		int32 Missing = 0, MissingDrawn = 0;
		for (int32 I = -6; I <= 6; ++I)
		{
			for (int32 J = -6; J <= 6; ++J)
			{
				const FVector2D XY = MovedSite + FVector2D(I, J) * 0.25 * ChunkWorld;
				if (CatchUp(Rig.ChunkOf(Rig.SurfacePoint(XY))))
				{
					continue;
				}
				++Missing;
				MissingDrawn += Rig.FarField.FindDrawnPatchAt(FVector(XY.X, XY.Y, 0.0)) ? 1 : 0;
			}
		}
		AddInfo(FString::Printf(TEXT("%s: with chunks missing near the viewer, %d of %d places over them drawn by patches"), *Name, MissingDrawn, Missing));
		TestTrue(*FString::Printf(TEXT("%s: chunks are missing"), *Name), Missing > 0);
		TestEqual(*FString::Printf(TEXT("%s: patches draw where chunks are missing"), *Name), MissingDrawn, Missing);
	}
	return true;
}

namespace VoxelHeightmapFarFieldTestUtils
{
	/** The key of the patch at a level whose footprint holds an XY. */
	FVoxelFarFieldPatchKey KeyAt(const FVoxelFarFieldSettings& Settings, int32 Level, const FVector2D& XY)
	{
		const double Cell = Settings.GetRootTileSize() / static_cast<double>(1ll << Level);
		return FVoxelFarFieldPatchKey(FVoxelFarFieldPatchKey::HeightmapFace, Level,
			FMath::FloorToInt32((XY.X - Settings.GridOrigin.X) / Cell), FMath::FloorToInt32((XY.Y - Settings.GridOrigin.Y) / Cell));
	}

	/** Checks one built patch's mesh against the analytic surface; returns the worst surface offset. */
	struct FSurfaceCheck
	{
		double WorstSurface = 0.0, WorstOffset = 0.0, WorstNormal = 1.0, WorstSkirt = 0.0, WorstSea = 0.0, WorstBlock = 0.0, WorstOverHeight = 0.0;
		int32 MixedTriangles = 0, NonBlockVertices = 0, OffBlockVertices = 0;
		double Tolerance = 0.0;
	};

	FSurfaceCheck CheckPatch(const FRig& Rig, const FVoxelFarFieldPatchKey& Key, const FVoxelFarFieldPatchMesh& Mesh)
	{
		const FVoxelFarFieldSettings& Settings = Rig.Settings;
		const FBox2D Box = Key.GetBox2D(Settings.GridOrigin, Settings.GetRootTileSize());
		const double Size = Box.GetSize().X;
		const int32 Q = Settings.PatchQuads;
		FSurfaceCheck Out;
		// Float offsets of the patch's size: the tolerance scales with it.
		Out.Tolerance = 8.0 * FMath::Pow(2.0, -24.0) * Size + 0.05;
		// A vertex's own grid point: its XY on the patch, rounded to the nearest grid point (the float position is a
		// rounding off it, and the height's slope would magnify a raw lookup).
		auto GridXY = [&](const FVector& World)
		{
			const int32 I = FMath::Clamp(FMath::RoundToInt32((World.X - Box.Min.X) / Size * Q), 0, Q);
			const int32 J = FMath::Clamp(FMath::RoundToInt32((World.Y - Box.Min.Y) / Size * Q), 0, Q);
			return Key.GetPoint2D(Settings.GridOrigin, Settings.GetRootTileSize(), static_cast<double>(I) / Q, static_cast<double>(J) / Q);
		};
		const FVoxelFarFieldSection& Terrain = Mesh.Terrain;
		for (int32 V = 0; V < Mesh.TerrainSurfaceVertices; ++V)
		{
			const FVector World = Mesh.Origin + FVector(Terrain.Positions[V]);
			const FVector2D XY = GridXY(World);
			const double Expected = Rig.Height(XY.X, XY.Y) - Settings.Sink;
			Out.WorstSurface = FMath::Max(Out.WorstSurface, FMath::Abs(World.Z - Expected));
			Out.WorstOffset = FMath::Max(Out.WorstOffset, static_cast<double>(Terrain.Positions[V].Size()));
			Out.WorstNormal = FMath::Min(Out.WorstNormal, static_cast<double>(Terrain.Normals[V].Z));
			if (Rig.Surface->bBlockSurface)
			{
				// A block top: a whole number of voxels over the origin, within a block over the generated height.
				const double Top = World.Z + Settings.Sink;
				const double Layers = (Top - Settings.GridOrigin.Z) / Settings.VoxelSize;
				const double Off = FMath::Abs(Layers - FMath::RoundToDouble(Layers)) * Settings.VoxelSize;
				Out.WorstBlock = FMath::Max(Out.WorstBlock, Off);
				Out.NonBlockVertices += Off > Out.Tolerance ? 1 : 0;
				const double CornerX = FVoxelBlockSurface::ColumnCorner(XY.X, Settings.GridOrigin.X, Settings.VoxelSize);
				const double CornerY = FVoxelBlockSurface::ColumnCorner(XY.Y, Settings.GridOrigin.Y, Settings.VoxelSize);
				const double Raw = Rig.Mode->GetTerrainHeightAt(static_cast<float>(CornerX), static_cast<float>(CornerY), Rig.Config->NoiseParams);
				// Between the height and a block over it, plus the density's rounding (0.004 voxel: a height just under a
				// cell's bottom face still makes that cell solid, FVoxelBlockSurface::TopSolidLayerForHeight).
				const double Over = Top - Raw;
				Out.WorstOverHeight = FMath::Max(Out.WorstOverHeight, Over);
				Out.OffBlockVertices += (Over < -Out.Tolerance || Over > 1.01 * Settings.VoxelSize + Out.Tolerance) ? 1 : 0;
			}
		}
		for (int32 T = 0; T + 2 < Terrain.Triangles.Num(); T += 3)
		{
			const float M0 = Terrain.UV1[Terrain.Triangles[T]].X;
			Out.MixedTriangles += (Terrain.UV1[Terrain.Triangles[T + 1]].X != M0 || Terrain.UV1[Terrain.Triangles[T + 2]].X != M0) ? 1 : 0;
		}
		for (int32 V = Mesh.TerrainSurfaceVertices; V < Terrain.Positions.Num(); ++V)
		{
			const FVector World = Mesh.Origin + FVector(Terrain.Positions[V]);
			const FVector2D XY = GridXY(World);
			const double Below = (Rig.Height(XY.X, XY.Y) - Settings.Sink) - World.Z;
			Out.WorstSkirt = FMath::Max(Out.WorstSkirt, Below - Mesh.SkirtDepth);
		}
		for (const FVector3f& Position : Mesh.Sea.Positions)
		{
			Out.WorstSea = FMath::Max(Out.WorstSea, FMath::Abs((Mesh.Origin.Z + Position.Z) - Settings.SeaZ));
		}
		return Out;
	}
}

/**
 * WM-MP1: a patch matches the surface. For patches holding two sites at levels from a root tile to the band: every surface
 * vertex at the generated height less the sink; offsets of the patch's size; normals up; one material per triangle; skirts
 * under their edges within their depth; the sea on the WaterLevel plane, present only where the terrain dips near or under
 * it; band patches know their chunks (the chunk of each sample's surface point among them); a patch's footprint is on the
 * chunk grid (band patches one chunk column).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldPlaneMatchesSurfaceTest,
	"VoxelWorlds.FarField.Plane.MatchesSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldPlaneMatchesSurfaceTest::RunTest(const FString& Parameters)
{
	using namespace VoxelHeightmapFarFieldTestUtils;

	FRig Rig(MakePlaneConfig(), 64, 150000.0, /*bSea=*/true);
	const FVoxelFarFieldSettings& Settings = Rig.Settings;
	const int32 Band = Settings.BandLevel;
	TestEqual(TEXT("64 chunks a root tile: band level 6"), Band, 6);
	TestEqual(TEXT("The automatic root tile: 256 chunks of 1,600 uu for a 2,000,000 uu reach"), FVoxelFarFieldSettings::ComputeHeightmapRootChunks(2.0e6, 1600.0), 256);
	TestEqual(TEXT("... 64 chunks of 6,400 uu"), FVoxelFarFieldSettings::ComputeHeightmapRootChunks(2.0e6, 6400.0), 64);
	TestEqual(TEXT("... 4 at least"), FVoxelFarFieldSettings::ComputeHeightmapRootChunks(1000.0, 6400.0), 4);
	const TArray<FVector2D> Sites = GetSites();
	int32 PatchesWithSea = 0, PatchesWithoutSea = 0;
	for (int32 SiteIndex : { 1, 2 })
	{
		const FVector2D& Site = Sites[SiteIndex];
		for (const int32 Level : { 0, 2, Band - 2, Band })
		{
			const FVoxelFarFieldPatchKey Key = KeyAt(Settings, Level, Site);
			FVoxelFarFieldPatchMesh Mesh;
			FVoxelPlanetFarField::BuildPatch(Settings, *Rig.Surface, Rig.NoZones, Key, Mesh);
			const FSurfaceCheck C = CheckPatch(Rig, Key, Mesh);
			const FBox2D Box = Key.GetBox2D(Settings.GridOrigin, Settings.GetRootTileSize());
			const double Size = Box.GetSize().X;

			// The footprint on the chunk grid; a band patch one chunk column.
			const double CellsX = (Box.Min.X - Settings.GridOrigin.X) / Settings.ChunkWorldSize;
			const bool bOnGrid = Level > Band || FMath::IsNearlyEqual(CellsX, FMath::RoundToDouble(CellsX), 1.0e-6);
			// Band patches: every sample's surface chunk is among the patch's.
			int32 SamplesOutside = 0;
			double Lowest = TNumericLimits<double>::Max(), Highest = TNumericLimits<double>::Lowest();
			for (int32 J = 0; J <= 8; ++J)
			{
				for (int32 I = 0; I <= 8; ++I)
				{
					const FVector2D XY = Key.GetPoint2D(Settings.GridOrigin, Settings.GetRootTileSize(), I / 8.0, J / 8.0);
					const FVector Point = Rig.SurfacePoint(XY);
					Lowest = FMath::Min(Lowest, Point.Z);
					Highest = FMath::Max(Highest, Point.Z);
					if (Level >= Band)
					{
						SamplesOutside += Mesh.SurfaceChunks.Contains(Rig.ChunkOf(Point)) ? 0 : 1;
					}
				}
			}
			PatchesWithSea += Mesh.bHasSea ? 1 : 0;
			PatchesWithoutSea += Mesh.bHasSea ? 0 : 1;

			AddInfo(FString::Printf(TEXT("site (%.0f, %.0f), level %d (%.0f uu across, %s): %d surface vertices within %.4f uu (tolerance %.3f), offsets up to %.0f, normals' Z >= %.3f, %d mixed triangles, skirt %.1f uu (worst past it %.3f), sea %d vertices within %.4f uu of Z %.0f (%s; terrain %.0f to %.0f), error %.2f uu, %d surface chunks (%d samples outside), %.1f ms"),
				Site.X, Site.Y, Level, Size, *Key.ToString(), Mesh.TerrainSurfaceVertices, C.WorstSurface, C.Tolerance, C.WorstOffset, C.WorstNormal,
				C.MixedTriangles, Mesh.SkirtDepth, C.WorstSkirt, Mesh.Sea.Positions.Num(), C.WorstSea, Settings.SeaZ, Mesh.bHasSea ? TEXT("has sea") : TEXT("no sea"),
				Lowest, Highest, Mesh.Error, Mesh.SurfaceChunks.Num(), SamplesOutside, Mesh.BuildMs));
			const FString Name = FString::Printf(TEXT("site (%.0f, %.0f), level %d"), Site.X, Site.Y, Level);
			TestTrue(*FString::Printf(TEXT("%s: the footprint is on the chunk grid"), *Name), bOnGrid);
			TestTrue(*FString::Printf(TEXT("%s: surface vertices on the generated surface less the sink"), *Name), C.WorstSurface <= C.Tolerance);
			TestTrue(*FString::Printf(TEXT("%s: offsets of the patch's size"), *Name), C.WorstOffset <= 1.2 * Size + 2.0 * 800.0);
			TestTrue(*FString::Printf(TEXT("%s: normals up"), *Name), C.WorstNormal > 0.0);
			TestEqual(*FString::Printf(TEXT("%s: one material per triangle"), *Name), C.MixedTriangles, 0);
			TestTrue(*FString::Printf(TEXT("%s: skirts within their depth"), *Name), C.WorstSkirt <= C.Tolerance);
			TestTrue(*FString::Printf(TEXT("%s: sea vertices on the sea plane"), *Name), C.WorstSea <= C.Tolerance);
			TestEqual(*FString::Printf(TEXT("%s: the sea where the terrain dips near or under it"), *Name), Mesh.bHasSea, Lowest - Settings.Sink < Settings.SeaZ + Settings.SeaMargin);
			if (Level >= Band)
			{
				TestTrue(*FString::Printf(TEXT("%s: a band patch is one chunk column"), *Name), FMath::IsNearlyEqual(Size, Settings.ChunkWorldSize, 1.0e-6));
				TestTrue(*FString::Printf(TEXT("%s: band patches know their chunks"), *Name), Mesh.SurfaceChunks.Num() > 0);
				TestEqual(*FString::Printf(TEXT("%s: every sample's chunk is among them"), *Name), SamplesOutside, 0);
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d patches with the sea, %d without (terrain +-800 about a sea at -200: every wide patch dips under it)"), PatchesWithSea, PatchesWithoutSea));
	TestTrue(TEXT("Patches with the sea were built"), PatchesWithSea > 0);
	return true;
}

/**
 * WM-MP1 on a cubic world (WM-C5's block surface): with cubic meshing the far field's surface is the block tops. For
 * patches at the band and two levels up: every surface vertex's height plus the sink lies a whole number of voxels over
 * the origin and within a block over the generated height of its column's corner; the sea and the skirts as on a smooth
 * world. With voxel.Cubic.BlockSurface 0 the vertices follow the isosurface again.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldCubicBlockTopTest,
	"VoxelWorlds.FarField.Cubic.BlockTop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldCubicBlockTopTest::RunTest(const FString& Parameters)
{
	using namespace VoxelHeightmapFarFieldTestUtils;

	for (const bool bBlockSurface : { true, false })
	{
		const FScopedCVar BlockVar(TEXT("voxel.Cubic.BlockSurface"), bBlockSurface ? TEXT("1") : TEXT("0"));
		FRig Rig(MakePlaneConfig(EMeshingMode::Cubic), 64, 150000.0, /*bSea=*/true);
		const TCHAR* Rule = bBlockSurface ? TEXT("block tops") : TEXT("voxel.Cubic.BlockSurface 0");
		TestEqual(*FString::Printf(TEXT("%s: the surface knows whether it is block tops"), Rule), Rig.Surface->bBlockSurface, bBlockSurface);
		const FVoxelFarFieldSettings& Settings = Rig.Settings;
		const int32 Band = Settings.BandLevel;
		int32 Vertices = 0, NonBlock = 0, OffBlock = 0;
		double WorstBlock = 0.0, WorstOver = 0.0, WorstSurface = 0.0, WorstSkirt = 0.0, WorstSea = 0.0;
		for (const FVector2D& Site : { FVector2D(4000.0, -2500.0), FVector2D(-37000.0, 51000.0) })
		{
			for (const int32 Level : { Band - 2, Band })
			{
				const FVoxelFarFieldPatchKey Key = KeyAt(Settings, Level, Site);
				FVoxelFarFieldPatchMesh Mesh;
				FVoxelPlanetFarField::BuildPatch(Settings, *Rig.Surface, Rig.NoZones, Key, Mesh);
				const FSurfaceCheck C = CheckPatch(Rig, Key, Mesh);
				Vertices += Mesh.TerrainSurfaceVertices;
				NonBlock += C.NonBlockVertices;
				OffBlock += C.OffBlockVertices;
				WorstBlock = FMath::Max(WorstBlock, C.WorstBlock);
				WorstOver = FMath::Max(WorstOver, C.WorstOverHeight);
				WorstSurface = FMath::Max(WorstSurface, C.WorstSurface);
				WorstSkirt = FMath::Max(WorstSkirt, C.WorstSkirt);
				WorstSea = FMath::Max(WorstSea, C.WorstSea);
			}
		}
		// Without the block surface: the isosurface's heights are not on the voxel layers (a smooth terrain).
		int32 OnLayers = 0;
		if (!bBlockSurface)
		{
			for (int32 I = 0; I < 64; ++I)
			{
				const double H = Rig.Height(4000.0 + I * 37.0, -2500.0 + I * 53.0);
				const double Layers = (H - Settings.GridOrigin.Z) / Settings.VoxelSize;
				OnLayers += FMath::Abs(Layers - FMath::RoundToDouble(Layers)) * Settings.VoxelSize < 0.05 ? 1 : 0;
			}
		}
		AddInfo(FString::Printf(TEXT("%s: %d surface vertices, %d off a block layer (worst %.3f uu), %d outside the block over the height (highest %.2f over it); surface within %.4f, skirts past their depth %.3f, sea off its plane %.4f; %d of 64 heights on a layer"),
			Rule, Vertices, NonBlock, WorstBlock, OffBlock, WorstOver, WorstSurface, WorstSkirt, WorstSea, OnLayers));
		TestTrue(*FString::Printf(TEXT("%s: vertices"), Rule), Vertices > 0);
		TestTrue(*FString::Printf(TEXT("%s: surface vertices on the surface less the sink"), Rule), WorstSurface <= 0.1);
		TestTrue(*FString::Printf(TEXT("%s: skirts within their depth"), Rule), WorstSkirt <= 0.1);
		TestTrue(*FString::Printf(TEXT("%s: the sea on its plane"), Rule), WorstSea <= 0.1);
		if (bBlockSurface)
		{
			TestEqual(*FString::Printf(TEXT("%s: every vertex on a block layer"), Rule), NonBlock, 0);
			TestEqual(*FString::Printf(TEXT("%s: every vertex within a block over the generated height"), Rule), OffBlock, 0);
		}
		else
		{
			TestTrue(*FString::Printf(TEXT("%s: the isosurface is not on the layers"), Rule), OnLayers < 8);
		}
	}
	return true;
}

/**
 * WM-MP1 (G45 on a plane): in biome blend zones a vertex's material mix is what the voxels around it take: the share of
 * the per-voxel picks (FVoxelFarFieldSurface::SampleConditionsXY, generation's) over 48 x 48 voxels taking the second
 * material, of the two, is the mix's weight, and the two materials hold nearly all the picks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldPlaneMaterialMixTest,
	"VoxelWorlds.FarField.Plane.MaterialMix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldPlaneMaterialMixTest::RunTest(const FString& Parameters)
{
	using namespace VoxelHeightmapFarFieldTestUtils;

	UVoxelBiomeConfiguration* Biomes = NewObject<UVoxelBiomeConfiguration>();
	Biomes->AddToRoot();
	Biomes->InitializeDefaults();
	Biomes->BiomeBlendWidth = 0.5f;
	UVoxelWorldConfiguration* Config = MakePlaneConfig();
	Config->bEnableBiomes = true;
	Config->BiomeConfiguration = Biomes;
	FRig Rig(Config, 64, 150000.0, /*bSea=*/true);
	TestTrue(TEXT("The surface has biomes"), Rig.Surface->Biomes.bIsValid);

	auto Mix = [&Rig](const FVector2D& XY)
	{
		return Rig.Surface->SampleMaterialMixXY(XY.X, XY.Y, static_cast<float>(Rig.Height(XY.X, XY.Y)));
	};
	auto Pick = [&Rig](const FVector2D& XY)
	{
		uint8 Material = 0, Biome = 0;
		Rig.Surface->SampleConditionsXY(XY.X, XY.Y, static_cast<float>(Rig.Height(XY.X, XY.Y)), Material, Biome);
		return Material;
	};

	// Blend-zone sites (a mix weight in [0.15, 0.5]) from a fixed random sweep of 400 km about the origin.
	TArray<FVector2D> Sites;
	FRandomStream Random(4501);
	for (int32 i = 0; i < 20000 && Sites.Num() < 24; ++i)
	{
		const FVector2D XY(Random.FRandRange(-200000.0f, 200000.0f), Random.FRandRange(-200000.0f, 200000.0f));
		const FVoxelSurfaceMaterialMix M = Mix(XY);
		if (M.Materials[0] != M.Materials[1] && M.Weight >= 0.15f)
		{
			Sites.Add(XY);
		}
	}
	AddInfo(FString::Printf(TEXT("%d blend-zone sites (mix weight 0.15 to 0.5) found"), Sites.Num()));
	if (!TestTrue(TEXT("Blend zones on the test plane (non-vacuous)"), Sites.Num() >= 12))
	{
		Biomes->RemoveFromRoot();
		return true;
	}

	// Around each site, a 48 x 48 grid of points a voxel apart: the voxels' picks there against the mixes there. The
	// plane's climate turns faster across the grid than the planet's, so the expectation is the mean of the grid's own
	// mixes (what the far field interpolates across its quads), not the centre's alone: the share of the centre pair's
	// second material in each point's mix, summed, against the picks taking it.
	auto ShareOf = [](const FVoxelSurfaceMaterialMix& M, uint8 Material)
	{
		const float Second = M.Materials[1] != M.Materials[0] ? M.Weight : 0.0f;
		return (M.Materials[0] == Material ? 1.0 - Second : 0.0) + (M.Materials[1] == Material && M.Materials[1] != M.Materials[0] ? Second : 0.0);
	};
	constexpr int32 Grid = 48;
	const double Step = Config->VoxelSize;
	double SumError = 0.0, WorstError = 0.0, WorstOutside = 0.0, SumCentreError = 0.0;
	for (const FVector2D& Site : Sites)
	{
		const FVoxelSurfaceMaterialMix M = Mix(Site);
		int32 First = 0, Second = 0, Outside = 0;
		double ExpectedFirst = 0.0, ExpectedSecond = 0.0;
		for (int32 J = 0; J < Grid; ++J)
		{
			for (int32 I = 0; I < Grid; ++I)
			{
				const FVector2D XY = Site + FVector2D((I - Grid / 2) * Step, (J - Grid / 2) * Step);
				const uint8 Material = Pick(XY);
				First += Material == M.Materials[0] ? 1 : 0;
				Second += Material == M.Materials[1] ? 1 : 0;
				Outside += (Material != M.Materials[0] && Material != M.Materials[1]) ? 1 : 0;
				const FVoxelSurfaceMaterialMix Here = Mix(XY);
				ExpectedFirst += ShareOf(Here, M.Materials[0]);
				ExpectedSecond += ShareOf(Here, M.Materials[1]);
			}
		}
		const double Share = (First + Second) > 0 ? static_cast<double>(Second) / (First + Second) : 0.0;
		const double Expected = (ExpectedFirst + ExpectedSecond) > 0.0 ? ExpectedSecond / (ExpectedFirst + ExpectedSecond) : 0.0;
		const double ExpectedOutside = 1.0 - (ExpectedFirst + ExpectedSecond) / (Grid * Grid);
		const double Error = FMath::Abs(Share - Expected);
		SumError += Error;
		SumCentreError += FMath::Abs(Share - M.Weight);
		WorstError = FMath::Max(WorstError, Error);
		WorstOutside = FMath::Max(WorstOutside, static_cast<double>(Outside) / (Grid * Grid) - ExpectedOutside);
	}
	const double MeanError = SumError / Sites.Num();
	AddInfo(FString::Printf(TEXT("The second material's share of the voxels' picks against the grid's mixes: off by %.3f on average, %.3f at worst (against the centre's mix alone %.3f on average); picks of neither material over the mixes' expectation by up to %.3f of a site"),
		MeanError, WorstError, SumCentreError / Sites.Num(), WorstOutside));
	TestTrue(TEXT("The mixes' weights are the share of the voxels taking the second material (mean error under 0.05)"), MeanError < 0.05);
	TestTrue(TEXT("... at every site (error under 0.15)"), WorstError < 0.15);
	TestTrue(TEXT("The two materials hold the picks the mixes give them (other materials within 10% of the expectation)"), WorstOutside < 0.1);
	Biomes->RemoveFromRoot();
	return true;
}

/**
 * WM-MP1, D37 on an island: with no far distance the roots are the tiles overlapping the island's bounds box, every
 * patch's footprint meets the box, and the drawn patches in the box's corners past the island's circle draw the sea
 * (the far ocean's open sea starts at the box); with a far distance shorter than the island, no drawn patch lies farther
 * from the camera than it; a camera far outside the island leaves no root.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldIslandBoundsTest,
	"VoxelWorlds.FarField.Island.Bounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldIslandBoundsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelHeightmapFarFieldTestUtils;

	// Root tiles of 8 chunks (12,800 uu): the 50 km box holds a few of them.
	{
		FRig Rig(MakeIslandConfig(), 8, 0.0, /*bSea=*/true);
		const FVoxelWorldBounds& Bounds = Rig.Settings.Bounds;
		TestTrue(TEXT("The island is bounded"), Bounds.IsBounded());
		const FBox2D Box(FVector2D(Bounds.Center.X, Bounds.Center.Y) - Bounds.HalfExtent, FVector2D(Bounds.Center.X, Bounds.Center.Y) + Bounds.HalfExtent);
		const FVector Viewer = Rig.SurfacePoint(FVector2D(0.0, 0.0)) + FVector(0.0, 0.0, 300.0);
		const FVoxelFarFieldView View = Rig.MakeView(Viewer);
		TestTrue(TEXT("Island: settles"), Rig.Converge(View, [](const FIntVector&) { return false; }) > 0);

		const double RootTile = Rig.Settings.GetRootTileSize();
		const int32 X0 = FMath::FloorToInt32(Box.Min.X / RootTile), X1 = FMath::FloorToInt32(Box.Max.X / RootTile);
		const int32 Y0 = FMath::FloorToInt32(Box.Min.Y / RootTile), Y1 = FMath::FloorToInt32(Box.Max.Y / RootTile);
		const int32 ExpectedRoots = (X1 - X0 + 1) * (Y1 - Y0 + 1);
		int32 OutsideBox = 0, Drawn = 0, CornerSea = 0, CornerDrawn = 0, OutsideCircleDrawn = 0;
		Rig.FarField.ForEachPatch([&](const FVoxelPlanetFarField::FPatchState& Patch)
		{
			OutsideBox += Box.Intersect(Patch.Box) ? 0 : 1;
			if (!Patch.bDrawTerrain && !Patch.bDrawSea)
			{
				return;
			}
			++Drawn;
			// Wholly past the circle (its nearest point to the centre farther than the outer radius) yet in the box: a corner.
			const double Nearest = FRig::DistanceToBox(FVector2D(Bounds.Center.X, Bounds.Center.Y), Patch.Box);
			if (Nearest > Bounds.OuterRadius)
			{
				++CornerDrawn;
				CornerSea += Patch.bDrawSea ? 1 : 0;
			}
			const FVector2D Centre = Patch.Box.GetCenter();
			OutsideCircleDrawn += Bounds.ContainsXY(Centre.X, Centre.Y) ? 0 : 1;
		});
		AddInfo(FString::Printf(TEXT("Island, no far distance: %d roots (%d expected over the %.0f x %.0f box), %d patches (%d outside the box), %d drawn, %d drawn wholly past the circle in the box's corners (%d with the sea), %d drawn centred outside the circle"),
			Rig.FarField.GetHeightmapRoots().Num(), ExpectedRoots, Box.GetSize().X, Box.GetSize().Y, Rig.FarField.GetPatchCount(), OutsideBox, Drawn, CornerDrawn, CornerSea, OutsideCircleDrawn));
		TestEqual(TEXT("Island: the roots are the tiles over the bounds box"), Rig.FarField.GetHeightmapRoots().Num(), ExpectedRoots);
		TestEqual(TEXT("Island: no patch outside the bounds box"), OutsideBox, 0);
		TestTrue(TEXT("Island: the box's corners past the circle are drawn"), CornerDrawn > 0);
		TestEqual(TEXT("Island: ... with the sea (the far ocean's open sea starts at the box)"), CornerSea, CornerDrawn);
	}

	// A far distance of 10,000 from a camera at the centre: nothing drawn farther; a camera 100 km out: no root.
	{
		FRig Rig(MakeIslandConfig(), 8, 10000.0, /*bSea=*/true);
		const FVector Viewer = Rig.SurfacePoint(FVector2D(0.0, 0.0)) + FVector(0.0, 0.0, 300.0);
		const FVoxelFarFieldView View = Rig.MakeView(Viewer);
		TestTrue(TEXT("Island, far distance 10,000: settles"), Rig.Converge(View, [](const FIntVector&) { return false; }) > 0);
		int32 Drawn = 0, Beyond = 0;
		double Farthest = 0.0;
		Rig.FarField.ForEachPatch([&](const FVoxelPlanetFarField::FPatchState& Patch)
		{
			if (!Patch.bDrawTerrain && !Patch.bDrawSea)
			{
				return;
			}
			++Drawn;
			const double Distance = FRig::DistanceToBox(FVector2D(Viewer.X, Viewer.Y), Patch.Box);
			Farthest = FMath::Max(Farthest, Distance);
			Beyond += Distance > 10000.0 ? 1 : 0;
		});
		AddInfo(FString::Printf(TEXT("Island, far distance 10,000: %d roots, %d drawn, the farthest %.0f from the camera, %d beyond the far distance"),
			Rig.FarField.GetHeightmapRoots().Num(), Drawn, Farthest, Beyond));
		TestTrue(TEXT("Island, far distance 10,000: patches drawn"), Drawn > 0);
		TestEqual(TEXT("Island, far distance 10,000: none drawn past it"), Beyond, 0);

		const FVector FarViewer(100000.0, 100000.0, 300.0);
		const FVoxelFarFieldView FarView = Rig.MakeView(FarViewer);
		TestTrue(TEXT("Island, camera 100 km out: settles"), Rig.Converge(FarView, [](const FIntVector&) { return false; }) > 0);
		TestEqual(TEXT("Island, camera 100 km out: no root"), Rig.FarField.GetHeightmapRoots().Num(), 0);
		TestEqual(TEXT("Island, camera 100 km out: nothing drawn"), Rig.FarField.GetDrawnPatchCount(), 0);
		TestEqual(TEXT("Island, camera 100 km out: no patch left"), Rig.FarField.GetPatchCount(), 0);
	}
	return true;
}

/**
 * WM-MP1 in a chunk manager: a game world on a plane with a sea draws its far field with the sea in it, has a component
 * per built patch with the drawn sections shown, hides band patches under the streamed window and their sea with them;
 * voxel.World.FarField 0 drops it and back. An island's far ocean keeps its open sea and seabed and builds no grid water
 * or floor while the far field is on, and builds them again without it (D37).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldPlaneChunkManagerTest,
	"VoxelWorlds.FarField.Plane.ChunkManager",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldPlaneChunkManagerTest::RunTest(const FString& Parameters)
{
	using namespace VoxelHeightmapFarFieldTestUtils;
	using namespace VoxelTesting;

	const FScopedCVar FarFieldVar(TEXT("voxel.World.FarField"), TEXT("1"));

	// The plane.
	{
		FTestChunkWorld W;
		W.Create(TEXT("FarFieldPlaneManagerWorld"));
		W.ConfigureSmallPlane();
		// A window of about four chunks: the hole rule hides a band patch once its column and the edges of its neighbours
		// are drawn.
		W.Config->ViewDistance = 6000.0f;
		W.Config->LODBands = { FLODBand(0.0f, 6000.0f, 0, 1) };
		W.Config->bEnableWaterLevel = true;
		W.Config->WaterLevel = -200.0f;
		W.Config->WaterMeshMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		W.StartManager();
		TestTrue(TEXT("Plane: a plane in a game world has a far field"), W.Manager->HasFarField());
		TestTrue(TEXT("Plane: the far field draws the sea"), W.Manager->DoesFarFieldDrawSea());
		TestFalse(TEXT("Plane: no ocean shell"), W.Manager->HasPlanetOcean());
		if (FVoxelPlanetFarField* FarField = W.Manager->GetFarFieldMutable())
		{
			TestTrue(TEXT("Plane: a heightmap far field"), FarField->GetSettings().bHeightmap);
			TestEqual(TEXT("Plane: the automatic root tile for 1,600 uu chunks and the 2,000,000 uu reach"), FarField->GetSettings().RootChunks, 256);
			FarField->SetSynchronousBuilds(true);
			FarField->SetMaxBuildsInFlight(256);
			const FVector2D Site(4000.0, -2500.0);
			const FVector Viewer(Site.X, Site.Y, W.Manager->GetGeneratedSurfaceHeight(Site.X, Site.Y) + 300.0);
			W.SetViewer(Viewer);
			TestTrue(TEXT("Plane: the window settles"), W.Settle());
			for (int32 Update = 0; Update < 200; ++Update)
			{
				W.Manager->UpdateFarFieldForViewer(Viewer);
				if (FarField->GetBuildsPending() == 0 && Update > 2)
				{
					break;
				}
			}
			// A covered band patch hides once its chunks have been drawn for the fade's length (HideDelay, about 0.45 s).
			FPlatformProcess::Sleep(0.7f);
			W.Manager->UpdateFarFieldForViewer(Viewer);
			W.Manager->UpdateFarFieldForViewer(Viewer);
			int32 Built = 0, Drawn = 0, Shown = 0, Hidden = 0, Missing = 0, SeaShown = 0, HiddenBand = 0, SeaWithoutTerrain = 0, SeaOverTile = 0, SeaWithoutTile = 0;
			FarField->ForEachPatch([&](const FVoxelPlanetFarField::FPatchState& Patch)
			{
				const bool bBand = Patch.Key.Level >= FarField->GetSettings().BandLevel;
				HiddenBand += (bBand && !Patch.bDrawTerrain && Patch.bCovered) ? 1 : 0;
				SeaWithoutTerrain += (Patch.bDrawSea && !Patch.bDrawTerrain) ? 1 : 0;
				if (bBand && Patch.bHasSea)
				{
					// A band patch's sea hides where its column's water tile is drawn and draws where none is.
					const FVector2D Centre = Patch.Box.GetCenter();
					const FIntPoint Column(FMath::FloorToInt32(Centre.X / W.Config->GetChunkWorldSize()), FMath::FloorToInt32(Centre.Y / W.Config->GetChunkWorldSize()));
					const bool bTile = W.Manager->HasWaterTile(Column);
					SeaOverTile += (Patch.bDrawSea && bTile) ? 1 : 0;
					// A selected band patch (its terrain drawn, or hidden by the window) draws its sea where no tile is.
					SeaWithoutTile += (!Patch.bDrawSea && !bTile && (Patch.bDrawTerrain || Patch.bCovered)) ? 1 : 0;
				}
				if (!Patch.bResident)
				{
					return;
				}
				++Built;
				UProceduralMeshComponent* Component = W.Manager->GetFarFieldComponent(Patch.Key.Pack());
				if (!Component)
				{
					++Missing;
					return;
				}
				Drawn += Patch.bDrawTerrain ? 1 : 0;
				const bool bShown = Component->GetNumSections() > 0 && Component->IsMeshSectionVisible(0);
				Shown += (Patch.bDrawTerrain && bShown) ? 1 : 0;
				Hidden += (!Patch.bDrawTerrain && !bShown) ? 1 : 0;
				SeaShown += (Patch.bDrawSea && Component->GetNumSections() > 1 && Component->IsMeshSectionVisible(1)) ? 1 : 0;
			});
			const FVoxelFarFieldStats Stats = W.Manager->GetFarFieldStats();
			AddInfo(FString::Printf(TEXT("Plane: %d chunks loaded; %d built patches, %d components (%d missing), %d drawn and shown of %d, %d hidden, %d band patches hidden by the window, %d seas shown, %d seas without terrain, %d band seas drawn over a water tile, %d band seas hidden without one; stats: %s"),
				W.Manager->GetLoadedChunkCount(), Built, W.Manager->GetFarFieldComponentCount(), Missing, Shown, Drawn, Hidden, HiddenBand, SeaShown, SeaWithoutTerrain, SeaOverTile, SeaWithoutTile, *W.Manager->GetFarFieldDebugString()));
			// The band patches about the viewer: which of their chunks are not drawn.
			{
				const int32 BandLevel = FarField->GetSettings().BandLevel;
				int32 BandPatches = 0, BandBuilt = 0, BandCovered = 0, BandLazy = 0, BandDrawn = 0;
				FString Nearest;
				FarField->ForEachPatch([&](const FVoxelPlanetFarField::FPatchState& Patch)
				{
					if (Patch.Key.Level < BandLevel)
					{
						return;
					}
					++BandPatches;
					BandBuilt += Patch.bBuilt ? 1 : 0;
					BandCovered += Patch.bCovered ? 1 : 0;
					BandLazy += Patch.bLazy ? 1 : 0;
					BandDrawn += Patch.bDrawTerrain ? 1 : 0;
					if (Patch.Box.IsInside(FVector2D(Viewer.X, Viewer.Y)) || FVector2D::Distance(Patch.Box.GetCenter(), FVector2D(Viewer.X, Viewer.Y)) < 1700.0)
					{
						FString Undrawn;
						int32 UndrawnCount = 0;
						const TArray<FIntVector>& Chunks = Patch.bBuilt ? Patch.SurfaceChunks : Patch.SampleChunks;
						for (const FIntVector& Chunk : Chunks)
						{
							if (!W.Manager->IsChunkLoaded(Chunk))
							{
								++UndrawnCount;
								if (UndrawnCount <= 6)
								{
									Undrawn += FString::Printf(TEXT(" %s"), *Chunk.ToString());
								}
							}
						}
						Nearest += FString::Printf(TEXT(" [%s built %d covered %d lazy %d drawn %d resident %d since %.2f: %d chunks, %d not loaded:%s]"),
							*Patch.Key.ToString(), Patch.bBuilt ? 1 : 0, Patch.bCovered ? 1 : 0, Patch.bLazy ? 1 : 0, Patch.bDrawTerrain ? 1 : 0,
							Patch.bResident ? 1 : 0, Patch.CoveredSince, Chunks.Num(), UndrawnCount, *Undrawn);
					}
				});
				FIntVector UndrawnChunk;
				const bool bUndrawn = FarField->GetUndrawnChunk(UndrawnChunk);
				FString Sample;
				int32 Listed = 0;
				FarField->ForEachPatch([&](const FVoxelPlanetFarField::FPatchState& Patch)
				{
					if (Patch.Key.Level >= BandLevel && Listed < 6)
					{
						++Listed;
						Sample += FString::Printf(TEXT(" %s box (%.0f,%.0f)-(%.0f,%.0f) culled %d"), *Patch.Key.ToString(), Patch.Box.Min.X, Patch.Box.Min.Y, Patch.Box.Max.X, Patch.Box.Max.Y, Patch.bCulled ? 1 : 0);
					}
				});
				// The chain of patches holding the viewer, level by level.
				FString Chain;
				for (int32 Level = 0; Level <= BandLevel; ++Level)
				{
					const FVoxelFarFieldPatchKey Key = KeyAt(FarField->GetSettings(), Level, FVector2D(Viewer.X, Viewer.Y));
					const FVoxelPlanetFarField::FPatchState* P = FarField->FindPatch(Key.Pack());
					if (!P)
					{
						Chain += FString::Printf(TEXT(" L%d %s: none"), Level, *Key.ToString());
						break;
					}
					Chain += FString::Printf(TEXT(" L%d %s: built %d res %d drawn %d sea %d kids %d culled %d lazy %d cov %d err %.1f box (%.0f,%.0f)-(%.0f,%.0f)"),
						Level, *Key.ToString(), P->bBuilt ? 1 : 0, P->bResident ? 1 : 0, P->bDrawTerrain ? 1 : 0, P->bDrawSea ? 1 : 0, P->bHasChildren ? 1 : 0,
						P->bCulled ? 1 : 0, P->bLazy ? 1 : 0, P->bCovered ? 1 : 0, P->Error, P->Box.Min.X, P->Box.Min.Y, P->Box.Max.X, P->Box.Max.Y);
				}
				AddInfo(FString::Printf(TEXT("Plane: the viewer's chain:%s"), *Chain));
				AddInfo(FString::Printf(TEXT("Plane: band level %d: %d band patches, %d built, %d covered, %d lazy, %d drawn; unready covered %d, uncovered %d, an undrawn chunk %s; viewer %s camera %s streaming viewer %s; band samples:%s; near the viewer:%s"),
					BandLevel, BandPatches, BandBuilt, BandCovered, BandLazy, BandDrawn, FarField->GetUnreadyBandCount(true), FarField->GetUnreadyBandCount(false),
					bUndrawn ? *UndrawnChunk.ToString() : TEXT("none"), *Viewer.ToCompactString(), *W.Manager->GetCameraPosition().ToCompactString(),
					*W.Manager->GetStreamingViewerPosition().ToCompactString(), *Sample, *Nearest));
			}
			TestTrue(TEXT("Plane: patches built"), Built > 6);
			TestTrue(TEXT("Plane: a few rows of roots, not a grid of them"), FarField->GetHeightmapRoots().Num() <= 144);
			// The memory is a budget question for the live checks (this plane's 8 m relief at a 25 m wavelength out to 20 km
			// splits far more patches than real content; the cost is recorded above).
			TestEqual(TEXT("Plane: a component per built patch"), Missing, 0);
			TestEqual(TEXT("Plane: components for resident patches only"), W.Manager->GetFarFieldComponentCount(), Built);
			TestEqual(TEXT("Plane: drawn patches shown"), Shown, Drawn);
			TestTrue(TEXT("Plane: the window hides band patches"), HiddenBand > 0);
			TestEqual(TEXT("Plane: no band patch draws its sea over a water tile"), SeaOverTile, 0);
			TestEqual(TEXT("Plane: no band patch hides its sea where no water tile is drawn"), SeaWithoutTile, 0);
			TestTrue(TEXT("Plane: sea sections shown"), SeaShown > 0);
			TestTrue(TEXT("Plane: the stats report it"), Stats.bActive && Stats.DrawnPatches > 0 && Stats.MemoryMB > 0.0f && Stats.bDrawsSea);
		}
		W.Manager->SetFarFieldEnabled(false);
		TestFalse(TEXT("Plane: off, no far field"), W.Manager->HasFarField());
		TestEqual(TEXT("Plane: off, its components are gone"), W.Manager->GetFarFieldComponentCount(), 0);
		W.Manager->SetFarFieldEnabled(true);
		TestTrue(TEXT("Plane: on again"), W.Manager->HasFarField());
		TestTrue(TEXT("Plane: settled before the world goes"), W.Settle());
		W.Stop();
	}

	// The island: the far ocean's grid water and floor retired while the far field is on.
	{
		FTestChunkWorld W;
		W.Create(TEXT("FarFieldIslandManagerWorld"));
		W.ConfigureSmallPlane();
		W.Config->WorldMode = EWorldMode::IslandBowl;
		W.Config->IslandRadius = 6000.0f;
		W.Config->IslandFalloffWidth = 2000.0f;
		W.Config->IslandEdgeHeight = -1200.0f;
		W.Config->IslandSeabedDepth = 1500.0f;
		W.Config->IslandSeabedSlopeWidth = 2000.0f;
		W.Config->IslandFarOceanDistance = 50000.0f;
		W.Config->bEnableWaterLevel = true;
		W.Config->WaterLevel = -300.0f;
		W.Config->WaterMeshMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		W.StartManager();
		TestTrue(TEXT("Island: a far field"), W.Manager->HasFarField());
		TestTrue(TEXT("Island: a far ocean"), W.Manager->HasFarOcean());
		UProceduralMeshComponent* Static = W.Manager->GetFarOceanStaticComponent();
		TestTrue(TEXT("Island: the far ocean's seabed and open sea are built"), Static && Static->GetNumSections() >= 2
			&& Static->GetProcMeshSection(0)->ProcVertexBuffer.Num() > 0 && Static->GetProcMeshSection(1)->ProcVertexBuffer.Num() > 0);
		TestNull(TEXT("Island: no grid water with the far field (D37)"), W.Manager->GetFarOceanGridComponent());
		TestNull(TEXT("Island: no grid window with the far field"), W.Manager->GetFarOceanWindowComponent());
		AddInfo(FString::Printf(TEXT("Island with the far field: far ocean %s; far field %s"), *W.Manager->GetFarOceanDebugString(), *W.Manager->GetFarFieldDebugString()));

		W.Manager->SetFarFieldEnabled(false);
		TestFalse(TEXT("Island: off, no far field"), W.Manager->HasFarField());
		TestTrue(TEXT("Island: off, the far ocean stays"), W.Manager->HasFarOcean());
		TestNotNull(TEXT("Island: off, the grid water is back"), W.Manager->GetFarOceanGridComponent());
		AddInfo(FString::Printf(TEXT("Island without the far field: far ocean %s"), *W.Manager->GetFarOceanDebugString()));
		W.Manager->SetFarFieldEnabled(true);
		TestTrue(TEXT("Island: on again"), W.Manager->HasFarField());
		TestNull(TEXT("Island: on again, no grid water"), W.Manager->GetFarOceanGridComponent());
		W.Stop();
	}
	return true;
}

/**
 * WM-MP1 (VP4's test on a plane): no coarse stand-in over the voxel window while the viewer walks. With the window loaded
 * around a viewer 170 uu over the ground, settled past the release delay (ancestors hidden under their children are
 * released), the viewer walks 7,500 uu east at 1,500 uu/s, the window following, one update a frame at 30 fps: with
 * View.bAvoidStandIns no update draws a stand-in over the window and no covered band patch waits for a build; every place
 * out to three view distances stays drawn by a chunk or a patch; the window shrunk to 0.85 of the view distance leaves no
 * gap in the very next update; and a teleport 40,000 uu north (nothing loaded there) has no gap in its first update (a
 * root tile stands in, as a face root does on a planet). The rule before is run too and its stand-ins reported.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldPlaneNoStandInTest,
	"VoxelWorlds.FarField.Plane.NoStandInOverWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldPlaneNoStandInTest::RunTest(const FString& Parameters)
{
	using namespace VoxelHeightmapFarFieldTestUtils;

	const FVector2D Site = GetSites()[1];
	for (const bool bAvoidStandIns : { false, true })
	{
		const TCHAR* Rule = bAvoidStandIns ? TEXT("avoiding stand-ins") : TEXT("the rule before");
		FRig Rig(MakePlaneConfig(), 64, 150000.0, /*bSea=*/false);
		auto MakeView = [&Rig, bAvoidStandIns](const FVector& Viewer, double Time)
		{
			FVoxelFarFieldView View = Rig.MakeView(Viewer);
			View.Time = Time;
			View.HideDelay = 0.45;
			View.bAvoidStandIns = bAvoidStandIns;
			return View;
		};
		auto Viewer = [&Rig](const FVector2D& XY) { return Rig.SurfacePoint(XY) + FVector(0.0, 0.0, 170.0); };
		// Every place on a polar grid out to three view distances: a drawn patch's or a drawn chunk's.
		auto CountGaps = [&Rig](const FVoxelFarFieldView& View, TFunctionRef<bool(const FIntVector&)> IsDrawn)
		{
			const FVector2D ViewerXY(View.Viewer.X, View.Viewer.Y);
			int32 Gaps = 0;
			for (double Distance = 0.0; Distance <= 3.0 * ViewDistance; Distance += ChunkWorld / 4.0)
			{
				const int32 Around = FMath::Max(8, FMath::CeilToInt32(UE_DOUBLE_TWO_PI * Distance / (ChunkWorld / 2.0)));
				for (int32 Step = 0; Step < Around; ++Step)
				{
					const double Theta = UE_DOUBLE_TWO_PI * Step / Around;
					const FVector2D XY = ViewerXY + FVector2D(FMath::Cos(Theta), FMath::Sin(Theta)) * Distance;
					if (IsDrawn(Rig.ChunkOf(Rig.SurfacePoint(XY))))
					{
						continue;
					}
					Gaps += Rig.FarField.FindDrawnPatchAt(FVector(XY.X, XY.Y, 0.0)) ? 0 : 1;
				}
			}
			return Gaps;
		};

		// Settled with the window loaded, then again past the release delay (hidden ancestors released).
		double Time = 0.0;
		FVoxelFarFieldView View = MakeView(Viewer(Site), Time);
		auto Loaded = [&Rig, &View](const FIntVector& Coord) { return Rig.InWindow(View, Coord, ViewDistance); };
		TestTrue(*FString::Printf(TEXT("%s: settles"), Rule), Rig.Converge(View, Loaded) > 0);
		Time = 20.0;
		View = MakeView(Viewer(Site), Time);
		TestTrue(*FString::Printf(TEXT("%s: settles past the release delay"), Rule), Rig.Converge(View, Loaded) > 0);
		const int64 ReleasedBefore = Rig.FarField.GetTotalReleased();

		// The walk: 1,500 uu/s east, an update a frame at 30 fps, the window following.
		const int32 Frames = 150;
		int32 FramesWithStandIns = 0, StandIns = 0, CoarsestLevel = 64, UnreadyCovered = 0, WalkGaps = 0;
		FVoxelFarFieldChanges Changes;
		for (int32 Frame = 1; Frame <= Frames; ++Frame)
		{
			const FVector2D XY = Site + FVector2D(50.0 * Frame, 0.0);
			Time += 1.0 / 30.0;
			View = MakeView(Viewer(XY), Time);
			Rig.FarField.Update(View, Loaded, Changes);
			const int32 Count = Rig.FarField.GetStandInCount();
			StandIns += Count;
			FramesWithStandIns += Count > 0 ? 1 : 0;
			if (Count > 0)
			{
				CoarsestLevel = FMath::Min(CoarsestLevel, Rig.FarField.GetStandInMinLevel());
				FIntVector Undrawn;
				const bool bUndrawn = Rig.FarField.GetUndrawnChunk(Undrawn);
				AddInfo(FString::Printf(TEXT("%s: frame %d: %d stand-in(s), level %d; unready band patches %d covered, %d uncovered%s"),
					Rule, Frame, Count, Rig.FarField.GetStandInMinLevel(), Rig.FarField.GetUnreadyBandCount(true), Rig.FarField.GetUnreadyBandCount(false),
					bUndrawn ? *FString::Printf(TEXT(" (chunk %s not drawn)"), *Undrawn.ToString()) : TEXT("")));
			}
			UnreadyCovered += Rig.FarField.GetUnreadyBandCount(true);
			if (Frame % 30 == 0)
			{
				WalkGaps += CountGaps(View, Loaded);
			}
		}
		AddInfo(FString::Printf(TEXT("%s: walking 7,500 uu, %d of %d frames drew a stand-in over the window (%d in all, coarsest level %d, band %d); covered band patches not ready %d; %lld patches released while settling; %d gaps at five checks"),
			Rule, FramesWithStandIns, Frames, StandIns, FramesWithStandIns > 0 ? CoarsestLevel : -1, Rig.Settings.BandLevel, UnreadyCovered,
			ReleasedBefore, WalkGaps));
		TestTrue(*FString::Printf(TEXT("%s: hidden patches were released while settling"), Rule), ReleasedBefore > 0);
		TestEqual(*FString::Printf(TEXT("%s: no gap while walking"), Rule), WalkGaps, 0);
		if (bAvoidStandIns)
		{
			TestEqual(*FString::Printf(TEXT("%s: no stand-in over the window while walking"), Rule), FramesWithStandIns, 0);
			TestEqual(*FString::Printf(TEXT("%s: no covered band patch waits for a build"), Rule), UnreadyCovered, 0);

			// The window's edge unloads: the band patches there were built meanwhile and draw in the very next update.
			TestTrue(*FString::Printf(TEXT("%s: settles after the walk"), Rule), Rig.Converge(View, Loaded) > 0);
			auto Shrunk = [&Rig, &View](const FIntVector& Coord) { return Rig.InWindow(View, Coord, 0.85 * ViewDistance); };
			Rig.FarField.Update(View, Shrunk, Changes);
			const int32 EdgeGaps = CountGaps(View, Shrunk);
			AddInfo(FString::Printf(TEXT("%s: the window shrunk to 0.85 of the view distance: %d gaps in the next update, %d stand-ins"),
				Rule, EdgeGaps, Rig.FarField.GetStandInCount()));
			TestEqual(*FString::Printf(TEXT("%s: the edge's patches draw at once when its chunks unload"), Rule), EdgeGaps, 0);
		}

		// A teleport 40,000 uu north, settled past the release delay first: nothing is loaded there, and the root tile
		// there is built (roots within the far distance are kept), so the first update after the cut has no gap.
		Time += 20.0;
		TestTrue(*FString::Printf(TEXT("%s: settles before the teleport"), Rule), Rig.Converge(MakeView(View.Viewer, Time), Loaded) > 0);
		const FVector2D FarXY = Site + FVector2D(7500.0, 40000.0);
		Time += 1.0 / 30.0;
		View = MakeView(Viewer(FarXY), Time);
		auto Nothing = [](const FIntVector&) { return false; };
		Rig.FarField.Update(View, Nothing, Changes);
		const int32 TeleportGaps = CountGaps(View, Nothing);
		AddInfo(FString::Printf(TEXT("%s: a teleport 40,000 uu north: %d gaps in the first update, %d stand-ins over the window, coarsest level %d"),
			Rule, TeleportGaps, Rig.FarField.GetStandInCount(), Rig.FarField.GetStandInMinLevel()));
		TestEqual(*FString::Printf(TEXT("%s: no gap after a teleport"), Rule), TeleportGaps, 0);
	}
	return true;
}

/**
 * WM-MP1 (F4.3's test on a plane): the far field for the camera a frame renders with. A chunk manager on the small plane
 * in a game world with a player controller whose view target is a camera actor, frames stepped in UWorld::Tick's order
 * (the chunk manager's tick in TG_PrePhysics reading the camera manager's camera, the camera managers, the far field's
 * camera tick in TG_PostUpdateWork). The window settles around the camera 1,200 over the ground; the camera then jumps
 * 600,000 uu east, where only the root tile of the old camera's selection lies (within the far distance, built up front).
 * With voxel.World.FarFieldCameraUpdate that very frame runs one far-field update for the new camera; under the rule before
 * none. A plane has no horizon, so the built root draws under the camera in the frame of the cut under either rule (the
 * planet's gap, a face culled under the horizon, cannot occur here) and the refinement under the new camera takes its
 * updates either way. Moving under a chunk a frame runs no second update.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldPlaneCameraAfterCutTest,
	"VoxelWorlds.FarField.Plane.CameraAfterCut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldPlaneCameraAfterCutTest::RunTest(const FString& Parameters)
{
	using namespace VoxelHeightmapFarFieldTestUtils;
	using namespace VoxelTesting;

	constexpr double Altitude = 1200.0;
	constexpr float FrameSeconds = 1.0f / 60.0f;
	const FVector2D SiteXY(4000.0, -2500.0);
	// Far enough that the old camera's selection there is coarser than the band (this plane's 800 uu relief at a 2,500 uu
	// wavelength splits to the band's first level within about 200,000 uu of the camera at 4 px).
	const FVector2D FarXY = SiteXY + FVector2D(600000.0, 0.0);
	const FScopedCVar FarFieldVar(TEXT("voxel.World.FarField"), TEXT("1"));
	for (const bool bCameraUpdate : { false, true })
	{
		const TCHAR* Rule = bCameraUpdate ? TEXT("updating for the frame's camera") : TEXT("the rule before");
		const FScopedCVar CameraUpdate(TEXT("voxel.World.FarFieldCameraUpdate"), bCameraUpdate ? TEXT("1") : TEXT("0"));

		FTestChunkWorld W;
		W.Create(TEXT("FarFieldPlaneCameraAfterCutWorld"));
		// Actors initialised, so a player controller spawns its camera manager and joins the world's controllers.
		W.World->InitializeActorsForPlay(FURL());
		W.ConfigureSmallPlane();
		W.Config->bEnableWaterLevel = true;
		W.Config->WaterLevel = -200.0f;
		W.Config->WaterMeshMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		W.StartManager();
		UVoxelChunkManager* Manager = W.Manager;

		// The camera: a camera actor, the controller's view target (as vox.Orbit's camera is).
		auto CameraAt = [Manager](const FVector2D& XY) { return FVector(XY.X, XY.Y, Manager->GetGeneratedSurfaceHeight(XY.X, XY.Y) + Altitude); };
		APlayerController* PC = W.World->SpawnActor<APlayerController>();
		ACameraActor* Camera = W.World->SpawnActor<ACameraActor>(CameraAt(SiteXY), FRotator::ZeroRotator);
		FVoxelPlanetFarField* FarField = Manager->GetFarFieldMutable();
		const bool bReady = FarField && PC && PC->PlayerCameraManager && Camera && W.World->GetFirstPlayerController() == PC;
		TestTrue(*FString::Printf(TEXT("%s: a far field, and a controller with a camera manager, the world's first"), Rule), bReady);
		if (bReady)
		{
			FarField->SetSynchronousBuilds(true);
			FarField->SetMaxBuildsInFlight(256);
			PC->PlayerCameraManager->bUseClientSideCameraUpdates = false;
			PC->SetViewTarget(Camera);

			Manager->RegisterAllComponentTickFunctions(true);
			FVoxelFarFieldCameraTickFunction& CameraTick = Manager->GetFarFieldCameraTickFunction();
			TestTrue(*FString::Printf(TEXT("%s: the camera tick is registered in TG_PostUpdateWork"), Rule),
				CameraTick.IsTickFunctionRegistered() && CameraTick.TickGroup == TG_PostUpdateWork);

			// A frame as UWorld::Tick runs it.
			auto Frame = [Manager, PC, &CameraTick]()
			{
				Manager->TickComponent(FrameSeconds, LEVELTICK_All, nullptr);
				PC->UpdateCameraManager(FrameSeconds);
				CameraTick.ExecuteTick(FrameSeconds, LEVELTICK_All, ENamedThreads::GameThread, FGraphEventRef());
			};
			// The drawn patch under a ground point and its level (-1: none, -2: one without a visible component).
			auto DrawnLevelAt = [Manager, FarField](const FVector2D& XY)
			{
				const FVoxelPlanetFarField::FPatchState* Patch = FarField->FindDrawnPatchAt(FVector(XY.X, XY.Y, 0.0));
				if (!Patch)
				{
					return -1;
				}
				const UProceduralMeshComponent* Component = Manager->GetFarFieldComponent(Patch->Key.Pack());
				const bool bShown = Component && Component->GetNumSections() > 0 && Component->IsMeshSectionVisible(0);
				return bShown ? Patch->Key.Level : -2;
			};

			// Settle: the window loaded and the far field built around the camera.
			int32 IdleFrames = 0;
			const double Deadline = FPlatformTime::Seconds() + 120.0;
			while (IdleFrames < 30 && FPlatformTime::Seconds() < Deadline)
			{
				Frame();
				FPlatformProcess::Sleep(0.002f);
				const bool bIdle = Manager->GetPendingGenerationCount() == 0 && Manager->GetPendingMeshingCount() == 0
					&& Manager->GetAsyncGenerationInProgressCount() == 0 && Manager->GetAsyncMeshingInProgressCount() == 0
					&& Manager->GetPendingMeshUploadCount() == 0 && Manager->GetSeamJobsInFlightCount() == 0 && FarField->GetBuildsPending() == 0;
				IdleFrames = bIdle ? IdleFrames + 1 : 0;
			}
			TestTrue(*FString::Printf(TEXT("%s: settles"), Rule), IdleFrames >= 30);
			TestTrue(*FString::Printf(TEXT("%s: the camera manager's camera is the camera actor"), Rule),
				PC->PlayerCameraManager->GetCameraLocation().Equals(Camera->GetActorLocation(), 1.0));
			const int32 BandLevel = FarField->GetSettings().BandLevel;
			const int32 FarLevelBefore = DrawnLevelAt(FarXY);
			TestTrue(*FString::Printf(TEXT("%s: the destination lies under a coarse patch of the old camera's selection"), Rule),
				FarLevelBefore >= 0 && FarLevelBefore < BandLevel);

			// Moving under a chunk a frame: the tick's selection holds, no second update.
			const int64 UpdatesBefore = Manager->GetFarFieldStats().CameraUpdates;
			for (int32 Step = 1; Step <= 5; ++Step)
			{
				Camera->SetActorLocation(CameraAt(SiteXY + FVector2D(400.0 * Step, 0.0)));
				Frame();
			}
			TestEqual(*FString::Printf(TEXT("%s: no second update while moving 400 uu a frame"), Rule),
				Manager->GetFarFieldStats().CameraUpdates, UpdatesBefore);

			// The cut: the camera 60,000 uu east.
			Camera->SetActorLocation(CameraAt(FarXY));
			Frame();
			const int32 CutLevel = DrawnLevelAt(FarXY);
			const int64 CutUpdates = Manager->GetFarFieldStats().CameraUpdates - UpdatesBefore;
			Frame();
			const int32 NextLevel = DrawnLevelAt(FarXY);
			AddInfo(FString::Printf(TEXT("%s: before the cut the destination's patch is at level %d (band %d); the frame of the cut draws level %d there with %lld update(s) for the frame's camera; the next frame level %d"),
				Rule, FarLevelBefore, BandLevel, CutLevel, CutUpdates, NextLevel));
			// A plane has no horizon: the root tile under the destination is built and drawn before the cut, so the frame of the
			// cut draws something there under either rule (the planet's gap, a face culled under the horizon, cannot occur) and
			// the refinement under the new camera takes its updates either way; what tells the rules apart in the frame of the
			// cut is the update for the frame's camera.
			TestTrue(*FString::Printf(TEXT("%s: the frame of the cut draws a patch under the camera (a built root at least)"), Rule), CutLevel >= 0);
			TestTrue(*FString::Printf(TEXT("%s: the frame after the cut draws a patch under the camera"), Rule), NextLevel >= 0);
			TestEqual(*FString::Printf(TEXT("%s: %s for the frame's camera"), Rule, bCameraUpdate ? TEXT("one update") : TEXT("no update")),
				CutUpdates, int64(bCameraUpdate ? 1 : 0));
		}

		if (PC)
		{
			PC->Destroy();
		}
		if (Camera)
		{
			Camera->Destroy();
		}
		W.Stop();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
