// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "DistanceBandLODStrategy.h"
#include "IVoxelWorldMode.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The streaming window (WORLD_MODES_REVIEW_PLAN.md D8, WM-S1 and WM-S2). Pure logic, runs headless.
//
// Heightmap worlds (InfinitePlane, IslandBowl) used to stream the viewer's layer -2..+8 (island +4), so a viewer above
// layer 0 lost the ground below it (G13) and islands kept every layer of every column (no slab, KI5). Now every column
// streams its own surface slab at every distance (the world mode's surface ± 1.5 chunks, up to the water plane), plus
// the viewer's layer ± 2 in the columns near the viewer; the horizon is horizontal, so altitude never removes the
// ground. LOD distance is 3D in every mode (islands were 2D, KI13). The planet's window is its shell crossing
// (PlanetShellWindowTests.cpp, spherical SP-C1).
// ---------------------------------------------------------------------------

namespace VerticalWindowTestHelpers
{
	/** Exposes the window and the bounds for the tests. */
	class FProbeStrategy : public FDistanceBandLODStrategy
	{
	public:
		using FDistanceBandLODStrategy::GetColumnWindow;
		using FDistanceBandLODStrategy::TerrainMinHeight;
		using FDistanceBandLODStrategy::TerrainMaxHeight;
	};

	constexpr float ChunkWorldSize = 4800.0f;

	/**
	 * The demo's chunk geometry, bands and water, with real terrain noise; an island far larger than the view, centred
	 * on the viewer's column, so only the window decides.
	 */
	UVoxelWorldConfiguration* MakeConfig(EWorldMode WorldMode, bool bSlab = true)
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->WorldMode = WorldMode;
		Config->MeshingMode = EMeshingMode::DualContouring;
		Config->bEnableLOD = true;
		Config->VoxelSize = 75.0f;
		Config->ChunkSize = 64;
		Config->ViewDistance = 40000.0f;
		Config->LODBands = {
			FLODBand(0.0f, 7000.0f, 0, 1),
			FLODBand(7000.0f, 14000.0f, 1, 2),
			FLODBand(14000.0f, 40000.0f, 2, 4) };
		Config->SeaLevel = 0.0f;
		Config->BaseHeight = 0.0f;
		Config->HeightScale = 8000.0f;
		Config->NoiseParams.Seed = 1337;
		Config->NoiseParams.Frequency = 0.00005f;
		Config->bEnableWaterLevel = true;
		Config->WaterLevel = -1500.0f;
		Config->bFarBandSurfaceSlabCulling = bSlab;
		Config->bEnableFrustumCulling = false;
		Config->IslandRadius = 1.0e6f;
		Config->IslandFalloffWidth = 1000.0f;
		Config->IslandCenterX = 0.0f;
		Config->IslandCenterY = 0.0f;
		return Config;
	}

	/** Viewer above the centre of column (0, 0), in the middle of chunk layer Layer. */
	FLODQueryContext MakeContext(EWorldMode WorldMode, int32 Layer, const FVector2D& XY = FVector2D(2400.0, 2400.0))
	{
		FLODQueryContext Context;
		Context.ViewerPosition = FVector(XY.X, XY.Y, Layer * ChunkWorldSize + 2400.0f);
		Context.WorldMode = WorldMode;
		return Context;
	}

	/** Layers overlapping [Lo, Hi] (touching counts), as the strategy defines them. */
	void Layers(double Lo, double Hi, int32& OutMin, int32& OutMax)
	{
		OutMin = FMath::CeilToInt32(Lo / ChunkWorldSize) - 1;
		OutMax = FMath::FloorToInt32(Hi / ChunkWorldSize);
	}

	TSet<FIntVector> Visible(FDistanceBandLODStrategy& Strategy, const FLODQueryContext& Context)
	{
		Strategy.Update(Context, 1.0f / 60.0f);
		TSet<FIntVector> Result;
		for (const FChunkLODRequest& Request : Strategy.GetVisibleChunks(Context))
		{
			Result.Add(Request.ChunkCoord);
		}
		return Result;
	}

	double ColumnDistance(int32 X, int32 Y, const FVector& Viewer)
	{
		return FVector2D::Distance(FVector2D((X + 0.5) * ChunkWorldSize, (Y + 0.5) * ChunkWorldSize), FVector2D(Viewer.X, Viewer.Y));
	}
}

/**
 * The candidates are exactly the terrain window, at every viewer altitude: every column within the horizon gets its own
 * surface slab (computed here from the world mode, independently of the strategy), and the columns within band 0 plus
 * two chunks also get the viewer's layer ± 2; both clipped to the terrain's height bounds. A viewer 48,000 uu up still
 * streams the ground (G13).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLODVerticalWindowFromTerrainTest,
	"VoxelWorlds.LOD.VerticalWindow.FromTerrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLODVerticalWindowFromTerrainTest::RunTest(const FString& Parameters)
{
	using namespace VerticalWindowTestHelpers;

	const EWorldMode Modes[] = { EWorldMode::InfinitePlane, EWorldMode::IslandBowl };
	const int32 ViewerLayers[] = { 0, -3, -1, 1, 10 }; // layer 0 first: the baseline for the far columns
	const float NearRadius = 7000.0f + 2.0f * ChunkWorldSize;
	const double Slack = 1.5 * ChunkWorldSize;

	for (const EWorldMode Mode : Modes)
	{
		const TCHAR* ModeName = (Mode == EWorldMode::IslandBowl) ? TEXT("IslandBowl") : TEXT("InfinitePlane");
		UVoxelWorldConfiguration* Config = MakeConfig(Mode);
		FProbeStrategy Strategy;
		Strategy.Initialize(Config);
		const TSharedRef<IVoxelWorldMode> World = FVoxelWorldModeFactory::Create(*Config);

		int32 BoundsMin = 0, BoundsMax = -1;
		Layers(Strategy.TerrainMinHeight, Strategy.TerrainMaxHeight, BoundsMin, BoundsMax);
		TestTrue(*FString::Printf(TEXT("%s: the height bounds span several layers (%d..%d)"), ModeName, BoundsMin, BoundsMax), BoundsMax - BoundsMin >= 4);

		TSet<FIntVector> FarAtLayer0;
		for (const int32 Layer : ViewerLayers)
		{
			const FLODQueryContext Context = MakeContext(Mode, Layer);
			const TSet<FIntVector> Got = Visible(Strategy, Context);

			TSet<FIntVector> Expected;
			for (int32 X = -10; X <= 10; ++X)
			{
				for (int32 Y = -10; Y <= 10; ++Y)
				{
					const double Distance = ColumnDistance(X, Y, Context.ViewerPosition);
					if (Distance > 40000.0)
					{
						continue;
					}
					const FFloatInterval Surface = World->GetSurfaceRange(
						FVector2D(X * ChunkWorldSize, Y * ChunkWorldSize), FVector2D((X + 1) * ChunkWorldSize, (Y + 1) * ChunkWorldSize), Config->NoiseParams);
					int32 SlabMin = 0, SlabMax = -1;
					Layers(Surface.Min - Slack, FMath::Max(Surface.Max, Config->WaterLevel) + Slack, SlabMin, SlabMax);
					for (int32 Z = FMath::Max(SlabMin, BoundsMin); Z <= FMath::Min(SlabMax, BoundsMax); ++Z)
					{
						Expected.Add(FIntVector(X, Y, Z));
					}
					if (Distance <= NearRadius)
					{
						for (int32 Z = FMath::Max(Layer - 2, BoundsMin); Z <= FMath::Min(Layer + 2, BoundsMax); ++Z)
						{
							Expected.Add(FIntVector(X, Y, Z));
						}
					}
				}
			}

			int32 Missing = 0, Extra = 0;
			for (const FIntVector& C : Expected) { Missing += Got.Contains(C) ? 0 : 1; }
			for (const FIntVector& C : Got) { Extra += Expected.Contains(C) ? 0 : 1; }
			TestEqual(*FString::Printf(TEXT("%s, viewer on layer %d: window chunks not streamed"), ModeName, Layer), Missing, 0);
			TestEqual(*FString::Printf(TEXT("%s, viewer on layer %d: chunks streamed outside the window"), ModeName, Layer), Extra, 0);
			// Non-vacuous: the slabs of the ~550 columns within the horizon (about two layers each with MP4's noise, whose
			// per-column ranges are tighter than the table noise's were) plus the near band where it lies within the bounds.
			AddInfo(FString::Printf(TEXT("%s, viewer on layer %d: %d chunks streamed, %d expected"), ModeName, Layer, Got.Num(), Expected.Num()));
			TestTrue(*FString::Printf(TEXT("%s, viewer on layer %d: candidates (%d)"), ModeName, Layer, Got.Num()), Got.Num() > 500);

			// What the far columns stream does not depend on the viewer's altitude at all.
			TSet<FIntVector> Far;
			for (const FIntVector& C : Got)
			{
				if (ColumnDistance(C.X, C.Y, Context.ViewerPosition) > NearRadius)
				{
					Far.Add(C);
				}
			}
			if (Layer == 0)
			{
				FarAtLayer0 = Far;
			}
			else
			{
				int32 FarDiffers = 0;
				for (const FIntVector& C : Far) { FarDiffers += FarAtLayer0.Contains(C) ? 0 : 1; }
				FarDiffers += FMath::Abs(FarAtLayer0.Num() - Far.Num());
				TestEqual(*FString::Printf(TEXT("%s, viewer on layer %d: far chunks that differ from a viewer on layer 0"), ModeName, Layer), FarDiffers, 0);
			}
		}
	}
	return true;
}

/**
 * The keep window: what the near window loaded stays while the viewer changes layer (walking across a layer boundary,
 * flying up), and goes once its column leaves the near radius; slab chunks within the horizon never unload. With the slab off every column keeps its whole height range.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLODVerticalWindowKeepTest,
	"VoxelWorlds.LOD.VerticalWindow.KeepWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLODVerticalWindowKeepTest::RunTest(const FString& Parameters)
{
	using namespace VerticalWindowTestHelpers;

	UVoxelWorldConfiguration* Config = MakeConfig(EWorldMode::InfinitePlane);
	FProbeStrategy Strategy;
	Strategy.Initialize(Config);
	const float NearRadius = Strategy.GetNearWindowRadius();
	TestEqual(TEXT("Near radius: band 0 plus two chunks"), NearRadius, 7000.0f + 2.0f * ChunkWorldSize);

	const FLODQueryContext Ground = MakeContext(EWorldMode::InfinitePlane, 0);
	const TSet<FIntVector> Loaded = Visible(Strategy, Ground);

	auto UnloadCandidates = [&Strategy, &Loaded](const FLODQueryContext& Context)
	{
		Strategy.Update(Context, 1.0f / 60.0f);
		TArray<FIntVector> Candidates;
		Strategy.GetChunksToUnload(Candidates, Loaded, Context);
		return TSet<FIntVector>(Candidates);
	};

	TestEqual(TEXT("Nothing the viewer just loaded is an unload candidate"), UnloadCandidates(Ground).Num(), 0);
	TestEqual(TEXT("Viewer one layer up: nothing unloads"), UnloadCandidates(MakeContext(EWorldMode::InfinitePlane, 1)).Num(), 0);
	TestEqual(TEXT("Viewer one layer down: nothing unloads"), UnloadCandidates(MakeContext(EWorldMode::InfinitePlane, -1)).Num(), 0);
	TestEqual(TEXT("Viewer 48,000 uu up: nothing unloads"), UnloadCandidates(MakeContext(EWorldMode::InfinitePlane, 10)).Num(), 0);

	// Move 20,000 uu along X: the old near columns are now far, so their chunks outside the slab go; nothing else does.
	const FLODQueryContext Moved = MakeContext(EWorldMode::InfinitePlane, 0, FVector2D(22400.0, 2400.0));
	const TSet<FIntVector> Gone = UnloadCandidates(Moved);
	int32 WrongStays = 0, WrongGoes = 0, NearChunksGone = 0;
	for (const FIntVector& C : Loaded)
	{
		const FProbeStrategy::FColumnWindow Slab = Strategy.GetColumnWindow(C.X, C.Y, Moved, FProbeStrategy::EWindowUse::Load);
		const double Distance = ColumnDistance(C.X, C.Y, Moved.ViewerPosition);
		const bool bInSlab = C.Z >= Slab.SlabMinZ && C.Z <= Slab.SlabMaxZ;
		const bool bShouldGo = Distance > 48000.0 || (Distance > NearRadius && !bInSlab);
		WrongStays += (bShouldGo && !Gone.Contains(C)) ? 1 : 0;
		WrongGoes += (!bShouldGo && Gone.Contains(C)) ? 1 : 0;
		NearChunksGone += (bShouldGo && Distance <= 48000.0) ? 1 : 0;
	}
	TestEqual(TEXT("After moving: chunks outside the keep window that stay"), WrongStays, 0);
	TestEqual(TEXT("After moving: chunks inside the keep window that go"), WrongGoes, 0);
	TestTrue(TEXT("After moving: the near window's extra layers behind the viewer go"), NearChunksGone > 0);

	// Slab off: whole columns everywhere, whatever the viewer's layer.
	UVoxelWorldConfiguration* NoSlab = MakeConfig(EWorldMode::InfinitePlane, false);
	FProbeStrategy Whole;
	Whole.Initialize(NoSlab);
	int32 BoundsMin = 0, BoundsMax = -1;
	Layers(Whole.TerrainMinHeight, Whole.TerrainMaxHeight, BoundsMin, BoundsMax);
	const FProbeStrategy::FColumnWindow Far = Whole.GetColumnWindow(8, 0, MakeContext(EWorldMode::InfinitePlane, 5), FProbeStrategy::EWindowUse::Load);
	TestEqual(TEXT("Slab off: a far column's window starts at the bounds"), Far.SlabMinZ, BoundsMin);
	TestEqual(TEXT("Slab off: a far column's window ends at the bounds"), Far.SlabMaxZ, BoundsMax);
	return true;
}

/**
 * The horizon is horizontal in heightmap worlds and along the ground from the viewer's ground point on a planet
 * (spherical Phase 6, WM-BP8; 3D until then).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLODHorizonDistanceTest,
	"VoxelWorlds.LOD.VerticalWindow.HorizonDistance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLODHorizonDistanceTest::RunTest(const FString& Parameters)
{
	using namespace VerticalWindowTestHelpers;

	const FVector Viewer(0.0f, 0.0f, 48000.0f);
	const FVector Chunk(30000.0f, 0.0f, 2400.0f);
	for (const EWorldMode Mode : { EWorldMode::InfinitePlane, EWorldMode::IslandBowl })
	{
		FDistanceBandLODStrategy Strategy;
		Strategy.Initialize(MakeConfig(Mode));
		TestEqual(TEXT("Heightmap world: horizon distance is horizontal"), Strategy.GetHorizonDistance(Chunk, Viewer), 30000.0f, 0.5f);
	}

	UVoxelWorldConfiguration* Planet = MakeConfig(EWorldMode::SphericalPlanet);
	Planet->WorldRadius = 250000.0f;
	FProbeStrategy Strategy;
	Strategy.Initialize(Planet);
	// The arc between the two directions from the centre at the radius of the terrain under the viewer, whatever either's
	// altitude: a viewer 48,000 uu over the north pole, a chunk 30,000 uu along the ground and one straight under it.
	const FVector High(0.0, 0.0, 250000.0 + 48000.0);
	const double GroundRadius = Strategy.GetPlanetGroundRadius(High);
	const double Angle = 30000.0 / 250000.0;
	const FVector Along = FVector(FMath::Sin(Angle), 0.0, FMath::Cos(Angle)) * (250000.0 + 2400.0);
	TestTrue(TEXT("Planet: the ground point is on the terrain under the viewer"),
		GroundRadius >= 250000.0 - Planet->PlanetMaxTerrainDepth && GroundRadius <= 250000.0 + Planet->PlanetMaxTerrainHeight);
	TestEqual(TEXT("Planet: horizon distance is along the ground from the ground point"), Strategy.GetHorizonDistance(Along, High),
		static_cast<float>(GroundRadius * Angle), 0.5f);
	TestEqual(TEXT("Planet: a chunk straight under the viewer is at 0, however deep"), Strategy.GetHorizonDistance(FVector(0.0, 0.0, 240000.0), High), 0.0f, 0.5f);
	return true;
}

/**
 * WM-S2: islands measure LOD distance in 3D like InfinitePlane, so a chunk deep below a viewer is coarser than the one
 * beside it (it was LOD 0 with the island's 2D distance, KI13), and an island viewer far above its terrain streams the
 * island's ground, coarse.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLODIslandDistance3DTest,
	"VoxelWorlds.LOD.VerticalWindow.IslandLODDistance3D",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLODIslandDistance3DTest::RunTest(const FString& Parameters)
{
	using namespace VerticalWindowTestHelpers;

	UVoxelWorldConfiguration* Config = MakeConfig(EWorldMode::IslandBowl);
	Config->HeightScale = 20000.0f; // height bounds well below layer -3
	FProbeStrategy Strategy;
	Strategy.Initialize(Config);
	const FLODQueryContext Context = MakeContext(EWorldMode::IslandBowl, 0);
	Strategy.Update(Context, 1.0f / 60.0f);

	TestEqual(TEXT("Beside the viewer (4,800 uu): LOD 0"), Strategy.GetLODForChunk(FIntVector(1, 0, 0), Context), 0);
	TestEqual(TEXT("One layer down (6,788 uu in 3D): LOD 0"), Strategy.GetLODForChunk(FIntVector(1, 0, -1), Context), 0);
	TestEqual(TEXT("Two layers down (10,733 uu in 3D): LOD 1"), Strategy.GetLODForChunk(FIntVector(1, 0, -2), Context), 1);
	TestEqual(TEXT("Three layers down (15,179 uu in 3D): LOD 2"), Strategy.GetLODForChunk(FIntVector(1, 0, -3), Context), 2);

	// A small island (R + F = 40,000) and a viewer 48,000 uu above its centre: every island column streams.
	UVoxelWorldConfiguration* Small = MakeConfig(EWorldMode::IslandBowl);
	Small->IslandRadius = 30000.0f;
	Small->IslandFalloffWidth = 10000.0f;
	FProbeStrategy SmallStrategy;
	SmallStrategy.Initialize(Small);
	const FLODQueryContext High = MakeContext(EWorldMode::IslandBowl, 10);
	SmallStrategy.Update(High, 1.0f / 60.0f);
	TSet<FIntPoint> Columns;
	int32 NotCoarsest = 0;
	for (const FChunkLODRequest& Request : SmallStrategy.GetVisibleChunks(High))
	{
		Columns.Add(FIntPoint(Request.ChunkCoord.X, Request.ChunkCoord.Y));
		NotCoarsest += (Request.LODLevel == 2) ? 0 : 1;
	}
	int32 IslandColumns = 0;
	for (int32 X = -10; X <= 10; ++X)
	{
		for (int32 Y = -10; Y <= 10; ++Y)
		{
			IslandColumns += (ColumnDistance(X, Y, FVector::ZeroVector) <= 36000.0) ? 1 : 0;
		}
	}
	int32 Covered = 0;
	for (int32 X = -10; X <= 10; ++X)
	{
		for (int32 Y = -10; Y <= 10; ++Y)
		{
			Covered += (ColumnDistance(X, Y, FVector::ZeroVector) <= 36000.0 && Columns.Contains(FIntPoint(X, Y))) ? 1 : 0;
		}
	}
	TestEqual(TEXT("Viewer 48,000 uu up: island columns within 36,000 uu that stream"), Covered, IslandColumns);
	TestEqual(TEXT("Viewer 48,000 uu up: every chunk at the coarsest band"), NotCoarsest, 0);
	return true;
}

/**
 * G19 (WORLD_MODES_REVIEW_PLAN.md §14.1): a column under the sea streams up to the water plane generation floods, which
 * is WaterLevel itself wherever WorldOrigin puts the chunk grid (the terrain's heights are absolute too). Every far
 * column's slab must be its surface ± the slack, raised to the water plane, in the grid's layers. With the origin 20,000
 * uu down and the sea at 6,000 a deep column has to keep the layer the water's surface is in; with it 9,000 up and the sea
 * at 1,000, a column under it no layer above the one its slack reaches.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLODVerticalWindowWaterPlaneWithOriginTest,
	"VoxelWorlds.LOD.VerticalWindow.WaterPlaneWithOrigin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLODVerticalWindowWaterPlaneWithOriginTest::RunTest(const FString& Parameters)
{
	using namespace VerticalWindowTestHelpers;

	const double Slack = 1.5 * ChunkWorldSize;
	struct FCase { double OriginZ; float WaterLevel; };
	for (const FCase& Case : { FCase{ -20000.0, 6000.0f }, FCase{ 9000.0, 1000.0f } })
	{
		UVoxelWorldConfiguration* Config = MakeConfig(EWorldMode::InfinitePlane);
		Config->WorldOrigin = FVector(0.0, 0.0, Case.OriginZ);
		Config->WaterLevel = Case.WaterLevel;
		FProbeStrategy Strategy;
		Strategy.Initialize(Config);
		const TSharedRef<IVoxelWorldMode> World = FVoxelWorldModeFactory::Create(*Config);

		FLODQueryContext Context = MakeContext(EWorldMode::InfinitePlane, 0);
		Context.WorldOrigin = Config->WorldOrigin;
		Context.ViewerPosition.Z += Case.OriginZ;
		// Layers of the grid: layer L spans [OriginZ + L * ChunkWorldSize, OriginZ + (L + 1) * ChunkWorldSize).
		auto GridLayers = [&Case](double Lo, double Hi, int32& OutMin, int32& OutMax)
		{
			Layers(Lo - Case.OriginZ, Hi - Case.OriginZ, OutMin, OutMax);
		};
		int32 BoundsMin = 0, BoundsMax = -1;
		GridLayers(Strategy.TerrainMinHeight, Strategy.TerrainMaxHeight, BoundsMin, BoundsMax);

		int32 Columns = 0, UnderSea = 0, Raised = 0, Wrong = 0;
		for (int32 X = -8; X <= 8; ++X)
		{
			for (int32 Y = -8; Y <= 8; ++Y)
			{
				if (ColumnDistance(X, Y, Context.ViewerPosition) <= Strategy.GetNearWindowRadius())
				{
					continue; // far columns only: the slab alone decides there
				}
				const FFloatInterval Surface = World->GetSurfaceRange(FVector2D(X * ChunkWorldSize, Y * ChunkWorldSize),
					FVector2D((X + 1) * ChunkWorldSize, (Y + 1) * ChunkWorldSize), Config->NoiseParams);
				int32 GroundMin = 0, GroundMax = -1, SlabMin = 0, SlabMax = -1;
				GridLayers(Surface.Min - Slack, Surface.Max + Slack, GroundMin, GroundMax);
				GridLayers(Surface.Min - Slack, FMath::Max(Surface.Max, Config->WaterLevel) + Slack, SlabMin, SlabMax);
				SlabMin = FMath::Max(SlabMin, BoundsMin);
				SlabMax = FMath::Min(SlabMax, BoundsMax);
				++Columns;
				UnderSea += (Surface.Max < Config->WaterLevel) ? 1 : 0;
				Raised += (FMath::Min(SlabMax, BoundsMax) > FMath::Min(GroundMax, BoundsMax)) ? 1 : 0;
				const FProbeStrategy::FColumnWindow Window = Strategy.GetColumnWindow(X, Y, Context, FProbeStrategy::EWindowUse::Load);
				Wrong += (Window.SlabMinZ != SlabMin || Window.SlabMaxZ != SlabMax) ? 1 : 0;
			}
		}
		AddInfo(FString::Printf(TEXT("WorldOrigin.Z %.0f, WaterLevel %.0f: %d far columns, %d under the sea, %d raised to its plane by a layer or more"),
			Case.OriginZ, Case.WaterLevel, Columns, UnderSea, Raised));
		TestTrue(*FString::Printf(TEXT("WorldOrigin.Z %.0f: columns under the sea (non-vacuous)"), Case.OriginZ), UnderSea >= 5);
		TestEqual(*FString::Printf(TEXT("WorldOrigin.Z %.0f: every far column's slab reaches the water plane, and no further"), Case.OriginZ), Wrong, 0);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
