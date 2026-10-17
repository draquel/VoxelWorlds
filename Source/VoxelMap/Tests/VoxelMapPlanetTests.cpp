// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelMapSubsystem.h"
#include "VoxelMapEditOverlay.h"
#include "VoxelCubeSphere.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelEditManager.h"
#include "VoxelEditTypes.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelNoiseTypes.h"
#include "VoxelSeaSurface.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelWorldModeFactory.h"
#include "SphericalPlanetWorldMode.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "Algo/AllOf.h"
#include "Async/TaskGraphInterfaces.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "TimerManager.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Spherical Phase 4: the map on a planet (WORLD_MODES_REVIEW_PLAN.md §18, WM-SY8; SP-G3, SP-G9).
//
// - TileKeys: the cube-sphere grid the map's tiles are (atlas coordinates, every direction in one cell, neighbours across
//   face edges, the flood fill and the chunk span), and the planet's surface coordinate as FVoxelCubeSphere.
// - TileMatchesGeneration: on a small planet with biomes and a sea, every sampled pixel of the tiles at the poles, 45
//   degrees, the equator and a cube corner is the surface material along its direction (QueryPlanetSurfaceConditions,
//   equal to generation's surface voxel by Generation.Planet.SurfaceConditions) or the sea where the terrain is under it.
// - Exploration: the minimap's request explores the tiles within its radius along the surface, never the other
//   hemisphere; streaming explores the tiles of the chunks it generates; the explored blob and planet overlays round-trip.
// - EditOverlay: FVoxelMapEditOverlay::BuildPlanet over generated chunks: a dig, a build, a buried cavity, an edited chunk
//   without data; and through the subsystem, a brush dig on a streamed planet shows on its tile.
// ---------------------------------------------------------------------------

namespace VoxelMapPlanetTest
{
	/** Pump the map's tile tasks (and its timers) until nothing waits or draws. */
	bool DrainMap(UVoxelMapSubsystem* Map, UWorld* World, double TimeoutSeconds = 60.0)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		int32 IdleRounds = 0;
		while (FPlatformTime::Seconds() < Deadline)
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			if (World)
			{
				// One overlay gather runs per frame: advance the frame counter so the next one may start.
				++GFrameCounter;
				World->GetTimerManager().Tick(0.1f);
			}
			IdleRounds = Map->GetDebugStats().Contains(TEXT("waiting=0 drawing=0")) ? IdleRounds + 1 : 0;
			if (IdleRounds >= 12)
			{
				return true;
			}
			FPlatformProcess::Sleep(0.005f);
		}
		return false;
	}

	FVector RandomDirection(FRandomStream& Random)
	{
		return Random.GetUnitVector();
	}

	/** Whether a colour is a base colour scaled by one factor in [MinK, MaxK] (channels clamped at 255 allowed). */
	bool IsScaledColor(const FColor& Color, const FColor& Base, float MinK, float MaxK)
	{
		const FVector C(Color.R, Color.G, Color.B);
		const FVector B(Base.R, Base.G, Base.B);
		if (B.SizeSquared() < 1.0)
		{
			return C.SizeSquared() < 4.0;
		}
		const double K = FVector::DotProduct(C, B) / B.SizeSquared();
		if (K < MinK - 0.02 || K > MaxK + 0.02)
		{
			return false;
		}
		for (int32 i = 0; i < 3; ++i)
		{
			const double Expected = FMath::Min(255.0, K * B[i]);
			if (FMath::Abs(C[i] - Expected) > 3.0 && !(C[i] == 255.0 && K * B[i] >= 252.0))
			{
				return false;
			}
		}
		return true;
	}

	TArray<TPair<const TCHAR*, FVector>> Sites()
	{
		const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
		return {
			{ TEXT("north pole"), FVector(0, 0, 1) },
			{ TEXT("south pole"), FVector(0, 0, -1) },
			{ TEXT("45 degrees"), FVector(C45, 0, C45) },
			{ TEXT("equator"), FVector(0, 1, 0) },
			{ TEXT("cube corner"), FVector(1, 1, 1).GetSafeNormal() },
		};
	}

	/** A small planet (FTestChunkWorld's) with the default biomes and a sea 150 uu under the datum. */
	UVoxelBiomeConfiguration* ConfigureBiomesAndSea(VoxelTesting::FTestChunkWorld& W)
	{
		UVoxelBiomeConfiguration* Biomes = NewObject<UVoxelBiomeConfiguration>();
		Biomes->AddToRoot();
		Biomes->RebuildCaches();
		W.Config->BiomeConfiguration = Biomes;
		W.Config->bEnableBiomes = true;
		W.Config->bEnableWaterLevel = true;
		W.Config->WaterRadius = W.Config->WorldRadius - 150.0f;
		return Biomes;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapPlanetTileKeysTest,
	"VoxelWorlds.Map.Planet.TileKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapPlanetTileKeysTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapPlanetTest;

	// Review_Planet: R 250,000, chunks of 64 voxels of 75 uu.
	const int32 ReviewN = FVoxelCubeSphereGrid::CellsPerEdgeFor(250000.0, 64.0 * 75.0);
	TestEqual(TEXT("Review_Planet: 82 tiles a face edge"), ReviewN, 82);
	TestEqual(TEXT("Review_Planet: 40,344 tiles"), FVoxelCubeSphereGrid(ReviewN).GetNumCells(), static_cast<int64>(40344));

	const FVoxelCubeSphereGrid Grid(20);
	const int32 N = Grid.GetCellsPerEdge();
	FRandomStream Random(17);

	// Every direction is in one valid cell, at a fraction that leads back to it; atlas coordinates round-trip.
	int32 BadCell = 0, BadInverse = 0, BadAtlas = 0;
	double WorstInverse = 0.0;
	for (int32 i = 0; i < 20000; ++i)
	{
		const FVector Direction = RandomDirection(Random);
		FVector2D Fraction;
		const FIntPoint Cell = Grid.CellOfDirection(Direction, &Fraction);
		BadCell += Grid.IsValidCell(Cell) ? 0 : 1;
		const double Error = (Grid.DirectionAt(Cell, Fraction.X, Fraction.Y) - Direction).Size();
		WorstInverse = FMath::Max(WorstInverse, Error);
		BadInverse += Error > 1.0e-9 ? 1 : 0;
		int32 Face = 0, IU = 0, IV = 0;
		BadAtlas += (Grid.FromAtlas(Cell, Face, IU, IV) && Grid.ToAtlas(Face, IU, IV) == Cell && Face >= 0 && Face < 6) ? 0 : 1;
	}
	AddInfo(FString::Printf(TEXT("20,000 directions: worst inverse error %.2e"), WorstInverse));
	TestEqual(TEXT("Every direction is in a valid cell"), BadCell, 0);
	TestEqual(TEXT("The cell and fraction lead back to the direction"), BadInverse, 0);
	TestEqual(TEXT("Atlas coordinates round-trip"), BadAtlas, 0);
	for (const FVector& Edge : { FVector(1, 1, 0).GetSafeNormal(), FVector(1, 1, 1).GetSafeNormal(), FVector(-1, -1, -1).GetSafeNormal(), FVector(0, 0, 1), FVector(0, -1, 0) })
	{
		TestTrue(*FString::Printf(TEXT("A face edge or corner direction %s is in a cell"), *Edge.ToCompactString()), Grid.IsValidCell(Grid.CellOfDirection(Edge)));
	}
	TestFalse(TEXT("Outside the atlas: X past 6N"), Grid.IsValidCell(FIntPoint(6 * N, 0)));
	TestFalse(TEXT("Outside the atlas: Y past N"), Grid.IsValidCell(FIntPoint(0, N)));

	// Neighbours: valid, another cell, about a cell away, and back among their own neighbours.
	const double CellAngle = 0.5 * UE_DOUBLE_PI / N;
	int32 BadNeighbour = 0, NotBack = 0, AcrossEdges = 0;
	for (int32 Y = 0; Y < N; ++Y)
	{
		for (int32 X = 0; X < 6 * N; ++X)
		{
			const FIntPoint Cell(X, Y);
			const FVector CellCentre = Grid.CellCentreDirection(Cell);
			const FIntPoint Steps[4] = { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) };
			for (const FIntPoint& Step : Steps)
			{
				const FIntPoint Next = Grid.Neighbour(Cell, Step.X, Step.Y);
				const double Angle = FMath::Acos(FMath::Clamp(FVector::DotProduct(CellCentre, Grid.CellCentreDirection(Next)), -1.0, 1.0));
				if (!Grid.IsValidCell(Next) || Next == Cell || Angle < 0.4 * CellAngle || Angle > 1.6 * CellAngle)
				{
					++BadNeighbour;
				}
				AcrossEdges += (Next.X / N != X / N) ? 1 : 0;
				bool bBack = false;
				for (int32 DY = -1; DY <= 1 && !bBack; ++DY)
				{
					for (int32 DX = -1; DX <= 1 && !bBack; ++DX)
					{
						bBack = (DX != 0 || DY != 0) && Grid.Neighbour(Next, DX, DY) == Cell;
					}
				}
				NotBack += bBack ? 0 : 1;
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d cells: %d steps across a face edge"), 6 * N * N, AcrossEdges));
	TestEqual(TEXT("Every neighbour is a valid other cell about a cell away"), BadNeighbour, 0);
	TestEqual(TEXT("Every cell is among its neighbours' neighbours"), NotBack, 0);
	TestEqual(TEXT("Every face's border steps across an edge (24 N)"), AcrossEdges, 24 * N);

	// The flood fill finds exactly the cells within the angle (the start cell always).
	for (const FVector& Direction : { FVector(0, 0, 1), FVector(1, 1, 1).GetSafeNormal(), FVector(1, 1, 0).GetSafeNormal(), FVector(0.3, -0.8, 0.2).GetSafeNormal() })
	{
		const double MaxAngle = 3.2 * CellAngle;
		TArray<FIntPoint> Gathered;
		TestTrue(TEXT("The flood fill completes"), Grid.GatherWithinAngle(Direction, MaxAngle, 100000, Gathered));
		TSet<FIntPoint> Expected;
		Expected.Add(Grid.CellOfDirection(Direction));
		for (int32 Y = 0; Y < N; ++Y)
		{
			for (int32 X = 0; X < 6 * N; ++X)
			{
				if (FVector::DotProduct(Grid.CellCentreDirection(FIntPoint(X, Y)), Direction) >= FMath::Cos(MaxAngle))
				{
					Expected.Add(FIntPoint(X, Y));
				}
			}
		}
		TSet<FIntPoint> Got(Gathered);
		TestEqual(*FString::Printf(TEXT("%s: no cell twice"), *Direction.ToCompactString()), Got.Num(), Gathered.Num());
		TestTrue(*FString::Printf(TEXT("%s: the cells within the angle (%d expected, %d gathered)"), *Direction.ToCompactString(), Expected.Num(), Got.Num()),
			Got.Num() == Expected.Num() && Got.Includes(Expected));
	}

	// The chunk span: every direction through a chunk-sized box lies in one of the gathered cells.
	{
		const double Radius = 20000.0;
		const double ChunkWorld = 1600.0;
		int32 Missed = 0, Boxes = 0;
		for (int32 b = 0; b < 60; ++b)
		{
			const FVector Direction = b < 4 ? (b == 0 ? FVector(1, 1, 1).GetSafeNormal() : (b == 1 ? FVector(1, 1, 0).GetSafeNormal() : (b == 2 ? FVector(0, 0, -1) : FVector(1, 0, 0)))) : RandomDirection(Random);
			const FVector Corner = (Direction * Radius / ChunkWorld).GridSnap(1.0) * ChunkWorld - FVector(ChunkWorld * 0.5);
			TArray<FVector> Corners;
			for (int32 c = 0; c < 9; ++c)
			{
				const FVector Offset = c < 8 ? FVector(c & 1, (c >> 1) & 1, (c >> 2) & 1) : FVector(0.5);
				Corners.Add((Corner + Offset * ChunkWorld).GetSafeNormal());
			}
			TArray<FIntPoint> Cells;
			Grid.GatherSpanned(Corners, Cells);
			const TSet<FIntPoint> CellSet(Cells);
			++Boxes;
			for (int32 s = 0; s < 400; ++s)
			{
				const FVector Point = Corner + FVector(Random.FRand(), Random.FRand(), Random.FRand()) * ChunkWorld;
				Missed += CellSet.Contains(Grid.CellOfDirection(Point.GetSafeNormal())) ? 0 : 1;
			}
		}
		TestEqual(*FString::Printf(TEXT("%d chunk boxes: every point's cell is among the spanned cells"), Boxes), Missed, 0);
	}

	// The planet's surface coordinate is FVoxelCubeSphere's around its centre: SurfaceToWorld and WorldToSurface invert.
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::SphericalPlanet;
		Config->WorldOrigin = FVector(1000.0, -2000.0, 500.0);
		Config->WorldRadius = 250000.0f;
		const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
		double WorstUV = 0.0;
		int32 WrongFace = 0;
		for (int32 i = 0; i < 2000; ++i)
		{
			const FVoxelSurfaceCoord Coord(Random.RandRange(0, 5), FVector2D(Random.FRandRange(-0.999, 0.999), Random.FRandRange(-0.999, 0.999)));
			const FVoxelSurfaceCoord Back = Mode->WorldToSurface(Mode->SurfaceToWorld(Coord, Random.FRandRange(-3000.0f, 3000.0f)));
			WrongFace += Back.Face != Coord.Face ? 1 : 0;
			WorstUV = FMath::Max(WorstUV, (Back.UV - Coord.UV).GetAbsMax());
			const FVector Direction = (Mode->SurfaceToWorld(Coord, 0.0f) - Config->WorldOrigin).GetSafeNormal();
			WorstUV = FMath::Max(WorstUV, (FVoxelCubeSphere::DirectionToSurface(Direction).UV - Coord.UV).GetAbsMax());
		}
		AddInfo(FString::Printf(TEXT("Planet coordinate: worst UV round-trip error %.2e"), WorstUV));
		TestEqual(TEXT("The planet's coordinate: the same face back"), WrongFace, 0);
		TestTrue(TEXT("The planet's coordinate: the same UV back (1e-6)"), WorstUV < 1.0e-6);
		Config->RemoveFromRoot();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapPlanetTileMatchesGenerationTest,
	"VoxelWorlds.Map.Planet.TileMatchesGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapPlanetTileMatchesGenerationTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapPlanetTest;
	VoxelTesting::FTestChunkWorld W;
	W.Create(TEXT("VoxelMapPlanetTiles"));
	W.ConfigureSmallPlanet();
	UVoxelBiomeConfiguration* Biomes = ConfigureBiomesAndSea(W);
	W.StartManager();

	UVoxelMapSubsystem* Map = W.World->GetSubsystem<UVoxelMapSubsystem>();
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*W.Config);
	const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(*Mode);
	const FVoxelBiomeSnapshot Snapshot = FVoxelBiomeSnapshot::FromConfig(Biomes);
	const FVoxelSeaSurface Sea = FVoxelSeaSurface::FromConfig(*W.Config);
	const FVector Center = W.Config->WorldOrigin;
	const double Radius = W.Config->WorldRadius;
	if (TestNotNull(TEXT("The game world has a map"), Map))
	{
		Map->RequestTilesInRadius(Center + FVector(0, 0, 1) * Radius, 10.0f); // resolves the world
		TestTrue(TEXT("The map is a planet's"), Map->IsPlanetMap());
		const FVoxelCubeSphereGrid& Grid = Map->GetPlanetTileGrid();
		TestEqual(TEXT("20 tiles a face edge on a 20,000 uu planet with 1,600 uu chunks"), Grid.GetCellsPerEdge(), 20);
		const int32 Res = Map->GetTileResolution();
		FRandomStream Random(3);
		int32 Checked = 0, Matched = 0, Water = 0, WrongPixel = 0;
		for (const TPair<const TCHAR*, FVector>& Site : Sites())
		{
			const FVector Position = Mode->GetSurfacePoint(Center + Site.Value, W.Config->NoiseParams).Position;
			Map->RequestTilesInRadius(Position, 1500.0f);
		}
		TestTrue(TEXT("Every tile drew"), DrainMap(Map, nullptr));
		for (const TPair<const TCHAR*, FVector>& Site : Sites())
		{
			const FIntPoint Tile = Map->WorldToTileCoord(Center + Site.Value * Radius);
			const FVoxelMapTile* Image = Map->GetTile(Tile);
			if (!TestTrue(*FString::Printf(TEXT("%s: the tile has its detail image"), Site.Key), Image && Image->Resolution == Res))
			{
				continue;
			}
			int32 SiteMatched = 0, SiteChecked = 0;
			for (int32 i = 0; i < 200; ++i)
			{
				const int32 PX = Random.RandRange(0, Res - 1);
				const int32 PY = Random.RandRange(0, Res - 1);
				const FVector Direction = Grid.DirectionAt(Tile, (PX + 0.5) / Res, (PY + 0.5) / Res);
				FIntPoint BackTile;
				int32 BackX = 0, BackY = 0;
				Map->PlanetDirectionToTilePixel(Direction, BackTile, BackX, BackY);
				WrongPixel += (BackTile != Tile || BackX != PX || BackY != PY) ? 1 : 0;

				const float Altitude = Planet.GetTerrainAltitude(Direction, W.Config->NoiseParams);
				const FVector Surface = Center + Direction * (Radius + Altitude);
				const bool bUnderSea = Radius + Altitude < Sea.Radius;
				uint8 Material = 0, Biome = 0;
				FVoxelSurfaceQuery::QueryPlanetSurfaceConditions(Planet, Surface, W.Config->VoxelSize, W.Config->NoiseParams, Snapshot, Sea, Material, Biome);
				const FColor Pixel = Image->PixelData[PY * Res + PX];
				const bool bMatch = bUnderSea
					? IsScaledColor(Pixel, FColor(20, 80, 180), 0.3f, 1.0f)
					: IsScaledColor(Pixel, FVoxelMaterialRegistry::GetMaterialColor(Material), 0.75f * 0.55f, 1.25f);
				if (!bMatch && Checked - Matched < 4)
				{
					AddInfo(FString::Printf(TEXT("%s pixel (%d, %d): %s, material %d, colour %s"), Site.Key, PX, PY,
						bUnderSea ? TEXT("under the sea") : TEXT("land"), Material, *Pixel.ToString()));
				}
				++Checked;
				++SiteChecked;
				Matched += bMatch ? 1 : 0;
				SiteMatched += bMatch ? 1 : 0;
				Water += bUnderSea ? 1 : 0;
			}
			AddInfo(FString::Printf(TEXT("%s: tile %s, %d of %d pixels the surface's material or the sea"), Site.Key, *Tile.ToString(), SiteMatched, SiteChecked));
		}
		AddInfo(FString::Printf(TEXT("%d pixels: %d match, %d under the sea; %s"), Checked, Matched, Water, *Map->GetDebugStats()));
		TestEqual(TEXT("Every pixel's direction maps back to it"), WrongPixel, 0);
		TestTrue(TEXT("Some pixels are under the sea and some on land"), Water > 20 && Water < Checked - 20);
		TestEqual(TEXT("Every pixel is the surface material along its direction, or the sea"), Matched, Checked);
		TestTrue(TEXT("The poles are different tiles"), Map->WorldToTileCoord(Center + FVector(0, 0, Radius)) != Map->WorldToTileCoord(Center - FVector(0, 0, Radius)));
	}
	W.Stop();
	Biomes->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapPlanetExplorationTest,
	"VoxelWorlds.Map.Planet.Exploration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapPlanetExplorationTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapPlanetTest;
	VoxelTesting::FTestChunkWorld W;
	W.Create(TEXT("VoxelMapPlanetExploration"));
	W.ConfigureSmallPlanet();
	W.StartManager();
	UVoxelMapSubsystem* Map = W.World->GetSubsystem<UVoxelMapSubsystem>();
	const FVector Center = W.Config->WorldOrigin;
	const double Radius = W.Config->WorldRadius;
	if (TestNotNull(TEXT("The game world has a map"), Map))
	{
		// The minimap's request at 45 degrees.
		const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
		const FVector Site = FVector(C45, 0, C45);
		constexpr float RequestRadius = 3000.0f;
		Map->RequestTilesInRadius(Center + Site * (Radius + 200.0), RequestRadius);
		TestTrue(TEXT("The map is a planet's"), Map->IsPlanetMap());
		const FVoxelCubeSphereGrid& Grid = Map->GetPlanetTileGrid();
		auto Unpack = [](uint64 Key) { return FIntPoint(static_cast<int32>(static_cast<uint32>(Key >> 32)), static_cast<int32>(static_cast<uint32>(Key & 0xFFFFFFFF))); };
		const double TileAngle = Map->GetTileWorldSize() / Radius;
		int32 TooFar = 0, Southern = 0;
		for (const uint64 Key : Map->GetExploredTiles())
		{
			const FVector TileCentre = Grid.CellCentreDirection(Unpack(Key));
			TooFar += FMath::Acos(FMath::Clamp(FVector::DotProduct(TileCentre, Site), -1.0, 1.0)) > RequestRadius / Radius + TileAngle + 1.0e-6 ? 1 : 0;
			Southern += TileCentre.Z < 0.0 ? 1 : 0;
		}
		int32 MissedNear = 0;
		for (int32 Y = 0; Y < Grid.GetCellsPerEdge(); ++Y)
		{
			for (int32 X = 0; X < 6 * Grid.GetCellsPerEdge(); ++X)
			{
				const FIntPoint Tile(X, Y);
				if (FMath::Acos(FMath::Clamp(FVector::DotProduct(Grid.CellCentreDirection(Tile), Site), -1.0, 1.0)) <= RequestRadius / Radius)
				{
					MissedNear += Map->IsTileExplored(Tile) ? 0 : 1;
				}
			}
		}
		AddInfo(FString::Printf(TEXT("The minimap's request explored %d tiles"), Map->GetExploredTiles().Num()));
		TestTrue(TEXT("Some tiles explored"), Map->GetExploredTiles().Num() >= 4);
		TestEqual(TEXT("Every tile within the radius is explored"), MissedNear, 0);
		TestEqual(TEXT("No explored tile beyond the radius and a tile"), TooFar, 0);
		TestEqual(TEXT("Nothing explored on the other hemisphere"), Southern, 0);
		TestTrue(TEXT("Every explored tile is inside the world"), Algo::AllOf(Map->GetExploredTiles(), [&](uint64 Key) { return Map->IsTileInsideWorld(Unpack(Key)); }));

		// Save: the explored atlas keys round-trip through the blob.
		int32 Count = 0;
		const FString Blob = Map->ExportExploredTiles(Count);
		TArray<FIntPoint> Decoded;
		TestTrue(TEXT("The explored blob decodes"), UVoxelMapSubsystem::DecodeTileCoords(Blob, Count, Decoded));
		TSet<FIntPoint> DecodedSet(Decoded);
		TestTrue(TEXT("The blob holds the explored tiles"), Count == Map->GetExploredTiles().Num()
			&& Algo::AllOf(Map->GetExploredTiles(), [&](uint64 Key) { return DecodedSet.Contains(Unpack(Key)); }));

		// Streaming at the equator explores the tiles of the chunks it generates there, still none on the far side.
		TestTrue(TEXT("The first tiles drew"), DrainMap(Map, nullptr));
		const TSet<uint64> BeforeStreaming = Map->GetExploredTiles();
		const FVector Equator(0, 1, 0);
		W.SetViewer(Center + Equator * (Radius + 300.0));
		TestTrue(TEXT("Streaming settles"), W.Settle());
		TestTrue(TEXT("The streamed tiles drew"), DrainMap(Map, nullptr));
		int32 FarSide = 0;
		for (const uint64 Key : Map->GetExploredTiles())
		{
			if (!BeforeStreaming.Contains(Key))
			{
				FarSide += FVector::DotProduct(Grid.CellCentreDirection(Unpack(Key)), Equator) < 0.0 ? 1 : 0;
			}
		}
		AddInfo(FString::Printf(TEXT("Streaming explored %d more tiles"), Map->GetExploredTiles().Num() - BeforeStreaming.Num()));
		TestTrue(TEXT("Streaming explored the tiles under the viewer"), Map->IsTileExplored(Map->WorldToTileCoord(Center + Equator * Radius)));
		TestEqual(TEXT("Streaming explored nothing on the far side"), FarSide, 0);
		TestTrue(TEXT("The tile under the viewer keeps its detail image"), Map->HasTileDetail(Map->WorldToTileCoord(Center + Equator * Radius)));
	}

	// Planet overlays save as format 2; plane overlays stay format 1 (their bytes unchanged).
	{
		FVoxelMapEditOverlay PlanetOverlay;
		PlanetOverlay.Resolution = 32;
		PlanetOverlay.bAltitudeHeights = true;
		PlanetOverlay.Columns = { 3, 70, 1000 };
		PlanetOverlay.Heights = { -120.5f, 33.0f, 410.25f };
		PlanetOverlay.Materials = { 1, 2, 3 };
		PlanetOverlay.Flags = { 0, FVoxelMapEditOverlay::FlagWater, 0 };
		FVoxelMapEditOverlay PlaneOverlay = PlanetOverlay;
		PlaneOverlay.bAltitudeHeights = false;
		TArray<uint8> PlanetBytes, PlaneBytes;
		{
			FMemoryWriter Writer(PlanetBytes);
			PlanetOverlay.Serialize(Writer);
		}
		{
			FMemoryWriter Writer(PlaneBytes);
			PlaneOverlay.Serialize(Writer);
		}
		TestEqual(TEXT("A planet overlay is format 2"), static_cast<int32>(PlanetBytes[0]), static_cast<int32>(FVoxelMapEditOverlay::PlanetFormatVersion));
		TestEqual(TEXT("A plane overlay stays format 1"), static_cast<int32>(PlaneBytes[0]), 1);
		TestTrue(TEXT("Only the version byte differs"), PlanetBytes.Num() == PlaneBytes.Num()
			&& FMemory::Memcmp(PlanetBytes.GetData() + 1, PlaneBytes.GetData() + 1, PlanetBytes.Num() - 1) == 0);
		FVoxelMapEditOverlay Loaded;
		FMemoryReader Reader(PlanetBytes);
		TestTrue(TEXT("A planet overlay loads"), Loaded.Serialize(Reader) && Loaded == PlanetOverlay && Loaded.bAltitudeHeights);

		const FString SaveBlob = UVoxelMapSubsystem::EncodeEditOverlays({ FVoxelMapTileOverlay{ FIntPoint(45, 7), PlanetOverlay } });
		TArray<FVoxelMapTileOverlay> Decoded;
		TestTrue(TEXT("The save blob with a planet overlay round-trips"), UVoxelMapSubsystem::DecodeEditOverlays(SaveBlob, Decoded)
			&& Decoded.Num() == 1 && Decoded[0].Tile == FIntPoint(45, 7) && Decoded[0].Overlay == PlanetOverlay);
	}
	W.Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapPlanetEditOverlayTest,
	"VoxelWorlds.Map.Planet.EditOverlay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapPlanetEditOverlayTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapPlanetTest;

	// --- BuildPlanet over generated chunks around a site at 45 degrees on the small planet.
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::SphericalPlanet;
		Config->WorldOrigin = FVector::ZeroVector;
		Config->WorldRadius = 20000.0f;
		Config->PlanetHeightScale = 1600.0f;
		Config->PlanetMaxTerrainHeight = 1000.0f;
		Config->PlanetMaxTerrainDepth = 1000.0f;
		Config->VoxelSize = 50.0f;
		Config->ChunkSize = 32;
		Config->bEnableBiomes = false;
		Config->bEnableCaves = false;
		Config->bEnableWaterLevel = false;
		Config->NoiseParams.Seed = 9001;
		Config->NoiseParams.Frequency = 0.0004f;
		Config->NoiseParams.Octaves = 3;
		const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
		const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(*Mode);
		const FVector Center = Config->WorldOrigin;
		const double Radius = Config->WorldRadius;
		const float VS = Config->VoxelSize;
		const int32 CS = Config->ChunkSize;
		const double ChunkWorld = CS * VS;

		const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
		const FVector Site = FVector(C45, 0.1, C45).GetSafeNormal();
		const FVector SitePoint = Mode->GetSurfacePoint(Center + Site, Config->NoiseParams).Position;
		const FVoxelCubeSphereGrid Grid(FVoxelCubeSphereGrid::CellsPerEdgeFor(Radius, ChunkWorld));
		const FIntPoint Tile = Grid.CellOfDirection(Site);

		// The tile's pixel lines and analytic altitudes.
		TArray<FVector> Directions;
		TArray<float> Altitudes;
		for (int32 PY = 0; PY < CS; ++PY)
		{
			for (int32 PX = 0; PX < CS; ++PX)
			{
				const FVector Direction = Grid.DirectionAt(Tile, (PX + 0.5) / CS, (PY + 0.5) / CS);
				Directions.Add(Direction);
				Altitudes.Add(Planet.GetTerrainAltitude(Direction, Config->NoiseParams));
			}
		}
		auto AnalyticMaterial = [&](int32 PX, int32 PY) -> uint8
		{
			const int32 Index = PY * CS + PX;
			return Planet.GetMaterialAtDepth(Center + Directions[Index] * (Radius + Altitudes[Index]), static_cast<float>(Radius + Altitudes[Index]), 0.0f);
		};

		// The 3 x 3 x 3 chunks around the site, generated.
		const FIntVector SiteChunk(FMath::FloorToInt(SitePoint.X / ChunkWorld), FMath::FloorToInt(SitePoint.Y / ChunkWorld), FMath::FloorToInt(SitePoint.Z / ChunkWorld));
		TMap<FIntVector, TArray<FVoxelData>> Generated;
		FVoxelCPUNoiseGenerator Generator;
		Generator.Initialize();
		for (int32 DZ = -1; DZ <= 1; ++DZ)
		for (int32 DY = -1; DY <= 1; ++DY)
		for (int32 DX = -1; DX <= 1; ++DX)
		{
			const FIntVector Coord = SiteChunk + FIntVector(DX, DY, DZ);
			FVoxelNoiseGenerationRequest Request;
			Request.ChunkCoord = Coord;
			Request.ChunkSize = CS;
			Request.VoxelSize = VS;
			Request.NoiseParams = Config->NoiseParams;
			FVoxelWorldModeFactory::FillGenerationRequest(*Config, Request);
			Generator.GenerateChunkCPU(Request, Generated.Add(Coord));
		}
		Generator.Shutdown();

		// An edit: set the voxels whose sample lies within Radius of a point, solid or air; marks them edited.
		auto MakeInput = [&](const TFunctionRef<bool(const FVector&, FVoxelData&)>& Edit, const FIntVector* WithoutData)
		{
			FVoxelMapPlanetOverlayInput Input;
			Input.ChunkSize = CS;
			Input.VoxelSize = VS;
			Input.Origin = Center;
			Input.Center = Center;
			Input.PlanetRadius = Radius;
			for (const TPair<FIntVector, TArray<FVoxelData>>& Pair : Generated)
			{
				TSharedPtr<TArray<FVoxelData>> Voxels = MakeShared<TArray<FVoxelData>>(Pair.Value);
				TSharedPtr<TBitArray<>> Bits = MakeShared<TBitArray<>>(false, CS * CS * CS);
				bool bAny = false;
				for (int32 Index = 0; Index < CS * CS * CS; ++Index)
				{
					const FIntVector Local(Index % CS, (Index / CS) % CS, Index / (CS * CS));
					const FVector P = Center + FVector(Pair.Key * CS + Local) * VS;
					if (Edit(P, (*Voxels)[Index]))
					{
						(*Bits)[Index] = true;
						bAny = true;
					}
				}
				FVoxelMapPlanetOverlayChunk& Chunk = Input.Chunks.AddDefaulted_GetRef();
				Chunk.Coord = Pair.Key;
				if (!WithoutData || *WithoutData != Pair.Key)
				{
					Chunk.Voxels = Voxels;
				}
				if (bAny)
				{
					Chunk.EditedVoxels = Bits;
				}
			}
			return Input;
		};
		auto Sphere = [](const FVector& Centre, double SphereRadius, bool bSolid)
		{
			return [Centre, SphereRadius, bSolid](const FVector& P, FVoxelData& Voxel)
			{
				if (FVector::Dist(P, Centre) > SphereRadius)
				{
					return false;
				}
				Voxel = bSolid ? FVoxelData::Solid(2) : FVoxelData::Air();
				return true;
			};
		};
		auto NearestColumn = [&](const FVector& Direction)
		{
			int32 Best = 0;
			for (int32 i = 1; i < Directions.Num(); ++i)
			{
				Best = FVector::DotProduct(Directions[i], Direction) > FVector::DotProduct(Directions[Best], Direction) ? i : Best;
			}
			return Best;
		};
		const int32 SiteColumn = NearestColumn(Site);

		// No edit: nothing.
		{
			const FVoxelMapEditOverlay Overlay = FVoxelMapEditOverlay::BuildPlanet(MakeInput([](const FVector&, FVoxelData&) { return false; }, nullptr), Directions, Altitudes, AnalyticMaterial);
			TestTrue(TEXT("No edit: an empty overlay and nothing pending"), Overlay.IsEmpty() && Overlay.PendingColumns.Num() == 0 && Overlay.bAltitudeHeights);
		}

		// A dig of radius 250 centred on the surface.
		{
			constexpr double DigRadius = 250.0;
			const FVoxelMapEditOverlay Overlay = FVoxelMapEditOverlay::BuildPlanet(MakeInput(Sphere(SitePoint, DigRadius, false), nullptr), Directions, Altitudes, AnalyticMaterial);
			int32 OutsideFootprint = 0, InnerUnmasked = 0, Inner = 0;
			for (int32 i = 0; i < Directions.Num(); ++i)
			{
				const double Across = FVector::CrossProduct(Directions[i], SitePoint - Center).Size();
				const bool bMasked = Overlay.FindColumn(i % CS, i / CS) != INDEX_NONE;
				OutsideFootprint += (bMasked && Across > DigRadius + 2.0 * VS) ? 1 : 0;
				if (Across < DigRadius - 100.0)
				{
					++Inner;
					InnerUnmasked += bMasked ? 0 : 1;
				}
			}
			const int32 Slot = Overlay.FindColumn(SiteColumn % CS, SiteColumn / CS);
			const float Depth = Slot != INDEX_NONE ? Altitudes[SiteColumn] - Overlay.Heights[Slot] : 0.0f;
			AddInfo(FString::Printf(TEXT("Dig: %d columns masked (%d inner), the centre column %.1f uu lower"), Overlay.Num(), Inner, Depth));
			TestTrue(TEXT("Dig: columns masked"), Overlay.Num() > 0 && Inner > 0);
			TestEqual(TEXT("Dig: nothing pending"), Overlay.PendingColumns.Num(), 0);
			TestEqual(TEXT("Dig: every column well inside the hole is masked"), InnerUnmasked, 0);
			TestEqual(TEXT("Dig: no column outside the hole's footprint is masked"), OutsideFootprint, 0);
			TestTrue(TEXT("Dig: the centre column is the hole's depth lower (within a voxel)"), FMath::Abs(Depth - DigRadius) <= VS);
			TestTrue(TEXT("Dig: altitudes"), Overlay.bAltitudeHeights);

			// The same with the site's chunk not loaded: its lines are pending instead.
			const FIntVector SitePointChunk(FMath::FloorToInt(SitePoint.X / ChunkWorld), FMath::FloorToInt(SitePoint.Y / ChunkWorld), FMath::FloorToInt(SitePoint.Z / ChunkWorld));
			const FVoxelMapEditOverlay Unloaded = FVoxelMapEditOverlay::BuildPlanet(MakeInput(Sphere(SitePoint, DigRadius, false), &SitePointChunk), Directions, Altitudes, AnalyticMaterial);
			TestTrue(TEXT("Dig into a chunk without data: its lines are pending"), Unloaded.IsPending(SiteColumn % CS, SiteColumn / CS)
				&& Unloaded.FindColumn(SiteColumn % CS, SiteColumn / CS) == INDEX_NONE);
		}

		// A build: a solid ball of radius 200 standing 100 uu above the surface.
		{
			const FVector Top = SitePoint + Site * 100.0;
			const FVoxelMapEditOverlay Overlay = FVoxelMapEditOverlay::BuildPlanet(MakeInput(Sphere(Top, 200.0, true), nullptr), Directions, Altitudes, AnalyticMaterial);
			const int32 Slot = Overlay.FindColumn(SiteColumn % CS, SiteColumn / CS);
			const float Rise = Slot != INDEX_NONE ? Overlay.Heights[Slot] - Altitudes[SiteColumn] : 0.0f;
			AddInfo(FString::Printf(TEXT("Build: %d columns masked, the centre column %.1f uu higher"), Overlay.Num(), Rise));
			TestTrue(TEXT("Build: the centre column is 300 uu higher (within a voxel)"), FMath::Abs(Rise - 300.0f) <= VS);
			TestTrue(TEXT("Build: the built material"), Slot != INDEX_NONE && Overlay.Materials[Slot] == 2);
		}

		// A buried cavity (radius 150, 600 uu below the surface): the surface is untouched.
		{
			const FVoxelMapEditOverlay Overlay = FVoxelMapEditOverlay::BuildPlanet(MakeInput(Sphere(SitePoint - Site * 600.0, 150.0, false), nullptr), Directions, Altitudes, AnalyticMaterial);
			TestTrue(TEXT("Buried cavity: nothing masked, nothing pending"), Overlay.IsEmpty() && Overlay.PendingColumns.Num() == 0);
		}
		Config->RemoveFromRoot();
	}

	// --- Through the subsystem: a brush dig on a streamed planet shows on the site's tile.
	{
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("VoxelMapPlanetEditOverlay"));
		W.ConfigureSmallPlanet();
		W.StartManager();
		UVoxelMapSubsystem* Map = W.World->GetSubsystem<UVoxelMapSubsystem>();
		const FVector Center = W.Config->WorldOrigin;
		const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
		const FVector Site = FVector(C45, 0.1, C45).GetSafeNormal();
		const FVector SitePoint = W.Manager->GetSurfacePointAt(Center + Site).Position;
		W.SetViewer(SitePoint + Site * 300.0);
		if (TestNotNull(TEXT("The game world has a map"), Map) && TestTrue(TEXT("Streaming settles"), W.Settle()))
		{
			Map->RequestTilesInRadius(SitePoint, 1000.0f);
			TestTrue(TEXT("The tiles drew"), DrainMap(Map, W.World));
			const FIntPoint Tile = Map->WorldToTileCoord(SitePoint);
			TestNull(TEXT("No overlay before the dig"), Map->GetEditOverlay(Tile));

			FVoxelBrushParams Brush;
			Brush.Shape = EVoxelBrushShape::Sphere;
			Brush.Radius = 300.0f;
			Brush.Strength = 1.0f;
			Brush.FalloffType = EVoxelBrushFalloff::Linear;
			Brush.DensityDelta = 255;
			W.Manager->GetEditManager()->ApplyBrushEdit(SitePoint, Brush, EEditMode::Subtract);
			TestTrue(TEXT("The overlay rebuilt"), DrainMap(Map, W.World));

			const FVoxelMapEditOverlay* Overlay = Map->GetEditOverlay(Tile);
			FIntPoint PixelTile;
			int32 PX = 0, PY = 0;
			Map->PlanetDirectionToTilePixel(Site, PixelTile, PX, PY);
			const int32 Slot = Overlay ? Overlay->FindColumn(PX, PY) : INDEX_NONE;
			const float Analytic = W.Manager->GetAltitudeAt(SitePoint);
			AddInfo(FString::Printf(TEXT("After the dig: %s; the site's column %s"), *Map->GetDebugStats(),
				Slot != INDEX_NONE ? *FString::Printf(TEXT("at altitude %.1f (analytic %.1f)"), Overlay->Heights[Slot], Analytic) : TEXT("not masked")));
			TestTrue(TEXT("The site's tile has an overlay of altitudes"), Overlay && Overlay->bAltitudeHeights && Overlay->Num() > 0);
			TestTrue(TEXT("The site's column is more than 100 uu lower"), Slot != INDEX_NONE && Overlay->Heights[Slot] < Analytic - 100.0f);

			// The overlay round-trips through the save blob and comes back on import.
			int32 Count = 0;
			const FString Blob = Map->ExportEditOverlays(Count);
			TestTrue(TEXT("The overlays export"), Count >= 1 && !Blob.IsEmpty());
			TestTrue(TEXT("The overlays import back"), Map->ImportEditOverlays(Blob) == Count && Map->GetEditOverlay(Tile) != nullptr);
		}
		W.Stop();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
