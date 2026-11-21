// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "VoxelMapSubsystem.h"
#include "VoxelMapTileFrame.h"
#include "VoxelMapTypes.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "IVoxelWorldMode.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelConditioningLine.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// A conditioning line's band on the map (Documentation/TERRAIN_LINES_PLAN.md, L3): the map and the voxels agree on
// where the band is, and a band narrower than a coarse tile's pixels still draws unbroken.
//
// - Level 0: a world with the biome pipeline, a line that only paints and a levelled road crossing it; the chunks
//   around the site are streamed, and at every pixel of the level-0 tiles there the material the map's drawer names
//   (the tile's gathered conditioning, FVoxelSurfaceQuery::QueryMapSurfaceMaterial) is a band's exactly where the
//   streamed surface voxel's is, and the same band's.
// - Levels 1 to 6: a trail three voxels wide and a road six voxels wide, long enough to cross many tiles; the pixels a
//   tile names the band on (each a point sample 2^Level voxels apart, the line taken as at least 0.71 of that wide) are
//   one connected run from the line's start to its end, where the same pixels with the line as it is break into pieces.
// - Level 7 and above: a tile gathers no line and names no band.
// ---------------------------------------------------------------------------

namespace VoxelMapLineTest
{
	constexpr uint8 PaintMaterial = 30;
	constexpr uint8 RoadMaterial = 31;

	/** A straight heightmap line with a vertex every two voxels. */
	FVoxelConditioningLineParams Straight(const FVector2D& A, const FVector2D& B, float Target, float HalfWidth, float Falloff, float VoxelSize, int64 Id)
	{
		FVoxelConditioningLineParams Params;
		Params.FalloffWidth = Falloff;
		Params.Id = Id;
		const int32 Steps = FMath::Max(1, FMath::CeilToInt32((B - A).Size() / (2.0 * VoxelSize)));
		for (int32 Step = 0; Step <= Steps; ++Step)
		{
			const FVector2D P = FMath::Lerp(A, B, static_cast<double>(Step) / Steps);
			Params.Vertices.Add(FVoxelConditioningLineVertex(FVector(P.X, P.Y, 0.0), Target, HalfWidth));
		}
		return Params;
	}

	/** Whether every pixel of a set can be reached from one of them through neighbours (the eight around a pixel). */
	bool IsConnected(const TSet<FIntPoint>& Pixels, const FIntPoint& From, const FIntPoint& To)
	{
		if (!Pixels.Contains(From) || !Pixels.Contains(To))
		{
			return false;
		}
		TSet<FIntPoint> Seen;
		TArray<FIntPoint> Open;
		Open.Add(From);
		Seen.Add(From);
		while (Open.Num() > 0)
		{
			const FIntPoint At = Open.Pop(EAllowShrinking::No);
			if (At == To)
			{
				return true;
			}
			for (int32 DY = -1; DY <= 1; ++DY)
			{
				for (int32 DX = -1; DX <= 1; ++DX)
				{
					const FIntPoint Next(At.X + DX, At.Y + DY);
					if (Pixels.Contains(Next) && !Seen.Contains(Next))
					{
						Seen.Add(Next);
						Open.Add(Next);
					}
				}
			}
		}
		return false;
	}

	/** The pixel of a set nearest a world point, pixels Size apart with their sample at their corner. */
	FIntPoint NearestPixel(const TSet<FIntPoint>& Pixels, const FVector2D& Point, double Size)
	{
		FIntPoint Best(0, 0);
		double BestDistance = TNumericLimits<double>::Max();
		for (const FIntPoint& Pixel : Pixels)
		{
			const double Distance = FVector2D::DistSquared(FVector2D(Pixel.X * Size, Pixel.Y * Size), Point);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Best = Pixel;
			}
		}
		return Best;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapConditioningLineBandTest,
	"VoxelWorlds.Map.ConditioningLine.Band",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapConditioningLineBandTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapLineTest;
	VoxelTesting::FTestChunkWorld W;
	W.Create(TEXT("VoxelMapConditioningLine"));
	W.ConfigureSmallPlane();
	UVoxelBiomeConfiguration* Biomes = NewObject<UVoxelBiomeConfiguration>();
	Biomes->AddToRoot();
	Biomes->InitializeDefaults();
	Biomes->RebuildCaches();
	W.Config->BiomeConfiguration = Biomes;
	W.Config->bEnableBiomes = true;
	W.StartManager();
	const float VoxelSize = W.Config->VoxelSize;
	const int32 Res = W.Config->ChunkSize;
	const FVoxelNoiseParams NoiseParams = W.Config->GetNoiseParams();
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*W.Config);
	const FVoxelBiomeSnapshot Snapshot = FVoxelBiomeSnapshot::FromConfig(Biomes);

	// ---- The lines, added before anything streams. Near the site: a line that only paints (three voxels wide) and a
	// levelled road (six voxels wide, 150 uu over the ground at the site) crossing it. Far from the site: a long trail
	// and a long road for the coarse levels.
	const FVector2D Site(900.0, -1300.0);
	const float SiteHeight = static_cast<float>(W.Manager->GetSurfacePointAt(FVector(Site.X, Site.Y, 0.0)).Position.Z);
	{
		FVoxelConditioningLineParams Paint = Straight(Site + FVector2D(-1500.0, -1100.0), Site + FVector2D(1500.0, 1100.0), 0.0f, 1.5f * VoxelSize, 2.0f * VoxelSize, VoxelSize, 1);
		Paint.Strength = 0.0f;
		Paint.Surface.BandMaterialID = PaintMaterial;
		W.Manager->AddConditioningLine(Paint);
		FVoxelConditioningLineParams Road = Straight(Site + FVector2D(-900.0, 1300.0), Site + FVector2D(900.0, -1300.0), SiteHeight + 150.0f, 3.0f * VoxelSize, 4.0f * VoxelSize, VoxelSize, 2);
		Road.Surface.BandMaterialID = RoadMaterial;
		W.Manager->AddConditioningLine(Road);
	}
	const FVector2D TrailStart(40000.0, 21000.0);
	const FVector2D TrailEnd(88000.0, 53000.0);
	const FVector2D RoadStart(40000.0, -21000.0);
	const FVector2D RoadEnd(91000.0, -47000.0);
	{
		FVoxelConditioningLineParams Trail = Straight(TrailStart, TrailEnd, 0.0f, 1.5f * VoxelSize, 2.0f * VoxelSize, VoxelSize, 3);
		Trail.Strength = 0.0f;
		Trail.Surface.BandMaterialID = PaintMaterial;
		W.Manager->AddConditioningLine(Trail);
		FVoxelConditioningLineParams Road = Straight(RoadStart, RoadEnd, 0.0f, 3.0f * VoxelSize, 4.0f * VoxelSize, VoxelSize, 4);
		Road.Strength = 0.0f;
		Road.Surface.BandMaterialID = RoadMaterial;
		W.Manager->AddConditioningLine(Road);
	}

	UVoxelMapSubsystem* Map = W.World->GetSubsystem<UVoxelMapSubsystem>();
	W.SetViewer(W.ViewerOverGround(Site.X, Site.Y, 300.0f));
	if (!TestNotNull(TEXT("The game world has a map"), Map) || !TestTrue(TEXT("Streaming settles"), W.Settle()))
	{
		W.Stop();
		Biomes->RemoveFromRoot();
		return true;
	}
	// The map resolves its world on its first request; its frame is the drawer's.
	Map->RequestTilesInRadius(FVector(Site.X, Site.Y, SiteHeight), 100.0f);
	const FVoxelMapTileFrame Frame = Map->GetTileFrame();
	// The material the drawer names at a tile's pixel, with the tile's own gathered conditioning.
	const auto MapMaterial = [&](const FVoxelMapTileId& Id, const FVoxelConditioning& Conditioning, int32 PX, int32 PY, FVoxelMapTileSample& OutSample)
	{
		OutSample = Frame.SampleAt(Id, PX + 1, PY + 1);
		const float Altitude = Frame.Altitude(*Mode, OutSample, NoiseParams, Conditioning);
		return FVoxelSurfaceQuery::QueryMapSurfaceMaterial(*Mode, Frame.PositionAt(OutSample, Altitude), OutSample.Direction, Altitude, Frame.VoxelSize,
			NoiseParams, Snapshot, true, Frame.Water, &Conditioning, FVoxelNoiseParams(), FVoxelNoiseParams());
	};

	// ---- Level 0: every pixel is a voxel column. The map's material against the streamed surface voxel's.
	{
		int32 Compared = 0;
		int32 MapBand = 0;
		int32 BandDisagree = 0;
		int32 SameMaterial = 0;
		int32 NearEdge = 0;
		TArray<FIntPoint> Tiles;
		Frame.TilesWithinRadius(FVector(Site.X, Site.Y, SiteHeight), 900.0, Tiles);
		FVoxelConditioningLineSample At;
		for (const FIntPoint& Tile : Tiles)
		{
			const FVoxelMapTileId Id = Frame.LevelTileOf(Tile, 0);
			FVoxelConditioning Conditioning;
			Frame.GatherConditioning(*W.Manager, Id, Conditioning);
			TestEqual(TEXT("A level-0 tile takes the lines as they are"), Conditioning.LeastHalfWidth, 0.0f);
			for (int32 PY = 0; PY < Res; ++PY)
			{
				for (int32 PX = 0; PX < Res; ++PX)
				{
					FVoxelMapTileSample Sample;
					const uint8 OnMap = MapMaterial(Id, Conditioning, PX, PY, Sample);
					FVoxelSurfaceSample Voxel;
					if (!W.Manager->QueryEditMergedSurfacePoint(FVector(Sample.X, Sample.Y, 0.0), Voxel))
					{
						continue;
					}
					// A column within a hundredth of a voxel of a band's edge may fall either way (float against double).
					bool bEdge = false;
					for (const FVoxelConditioningLineRef& Line : Conditioning.Lines)
					{
						bEdge |= Line->Sample(Sample.X, Sample.Y, 0.0, At) && FMath::Abs(At.Distance - At.HalfWidth) < 0.01f * VoxelSize;
					}
					if (bEdge)
					{
						++NearEdge;
						continue;
					}
					++Compared;
					const bool bMapBand = OnMap == PaintMaterial || OnMap == RoadMaterial;
					const bool bVoxelBand = Voxel.MaterialID == PaintMaterial || Voxel.MaterialID == RoadMaterial;
					MapBand += bMapBand ? 1 : 0;
					BandDisagree += (bMapBand != bVoxelBand || (bMapBand && OnMap != Voxel.MaterialID)) ? 1 : 0;
					SameMaterial += OnMap == Voxel.MaterialID ? 1 : 0;
				}
			}
		}
		AddInfo(FString::Printf(TEXT("Level 0: %d pixels of %d tiles against the streamed voxels (%d more left out at a band's edge); %d on a band, %d where the map and the voxel disagree about the band; the same material at %d"),
			Compared, Tiles.Num(), NearEdge, MapBand, BandDisagree, SameMaterial));
		TestTrue(TEXT("Level 0: pixels over streamed voxels, some on the bands (non-vacuous)"), Compared > 3000 && MapBand > 300);
		TestEqual(TEXT("Level 0: the map's pixel and the generated voxel name the same band"), BandDisagree, 0);
		TestTrue(TEXT("Level 0: off the bands too the map names the voxel's material nearly everywhere"), SameMaterial > Compared * 95 / 100);
	}

	// ---- Levels 1 to 7: a pixel is a point sample 2^Level voxels apart. The band's pixels along each long line.
	bool bSomePointSampleBroke = false;
	for (int32 Level = 1; Level <= 7; ++Level)
	{
		const double PixelSize = static_cast<double>(1 << Level) * VoxelSize;
		const double TileSize = PixelSize * Res;
		struct FLineCase
		{
			const TCHAR* Name;
			FVector2D Start;
			FVector2D End;
			uint8 Material;
		};
		for (const FLineCase& Case : { FLineCase{ TEXT("the trail"), TrailStart, TrailEnd, PaintMaterial }, FLineCase{ TEXT("the road"), RoadStart, RoadEnd, RoadMaterial } })
		{
			// The tiles the line passes through or beside, their pixels as global pixel coordinates at this level.
			TSet<FIntPoint> TileCoords;
			const int32 Steps = FMath::CeilToInt32((Case.End - Case.Start).Size() / (0.25 * TileSize)) + 1;
			for (int32 Step = 0; Step <= Steps; ++Step)
			{
				const FVector2D P = FMath::Lerp(Case.Start, Case.End, static_cast<double>(Step) / Steps);
				for (int32 DY = -1; DY <= 1; ++DY)
				{
					for (int32 DX = -1; DX <= 1; ++DX)
					{
						TileCoords.Add(FIntPoint(FMath::FloorToInt32(P.X / TileSize) + DX, FMath::FloorToInt32(P.Y / TileSize) + DY));
					}
				}
			}
			TSet<FIntPoint> Widened;
			TSet<FIntPoint> AsItIs;
			bool bGathered = false;
			float Least = 0.0f;
			for (const FIntPoint& Coord : TileCoords)
			{
				const FVoxelMapTileId Id(Coord, Level);
				FVoxelConditioning Conditioning;
				Frame.GatherConditioning(*W.Manager, Id, Conditioning);
				bGathered |= Conditioning.Lines.Num() > 0;
				Least = FMath::Max(Least, Conditioning.LeastHalfWidth);
				// The same tile with the lines as they are: what a point sample alone would draw.
				FVoxelConditioning Unwidened = Conditioning;
				Unwidened.LeastHalfWidth = 0.0f;
				for (int32 PY = 0; PY < Res; ++PY)
				{
					for (int32 PX = 0; PX < Res; ++PX)
					{
						FVoxelMapTileSample Sample;
						const FIntPoint Pixel(Coord.X * Res + PX, Coord.Y * Res + PY);
						if (MapMaterial(Id, Conditioning, PX, PY, Sample) == Case.Material)
						{
							Widened.Add(Pixel);
						}
						uint8 Band = 0;
						if (Unwidened.GetBandAt(Sample.X, Sample.Y, Band) && Band == Case.Material)
						{
							AsItIs.Add(Pixel);
						}
					}
				}
			}
			const bool bTakes = FVoxelMapTileFrame::TakesLines(Level);
			TestEqual(FString::Printf(TEXT("Level %d: a tile gathers lines up to the gather limit"), Level), bGathered, bTakes);
			if (!bTakes)
			{
				TestEqual(FString::Printf(TEXT("Level %d, %s: above the gather limit the map draws no band"), Level, Case.Name), Widened.Num(), 0);
				continue;
			}
			TestEqual(FString::Printf(TEXT("Level %d: a tile takes a line as at least 0.71 of a pixel wide"), Level), Least, static_cast<float>(0.71 * PixelSize), 0.01f);
			const bool bUnbroken = Widened.Num() > 0 && IsConnected(Widened, NearestPixel(Widened, Case.Start, PixelSize), NearestPixel(Widened, Case.End, PixelSize));
			const bool bPointUnbroken = AsItIs.Num() > 0 && IsConnected(AsItIs, NearestPixel(AsItIs, Case.Start, PixelSize), NearestPixel(AsItIs, Case.End, PixelSize));
			bSomePointSampleBroke |= !bPointUnbroken;
			int32 Lost = 0;
			for (const FIntPoint& Pixel : AsItIs)
			{
				Lost += Widened.Contains(Pixel) ? 0 : 1;
			}
			AddInfo(FString::Printf(TEXT("Level %d (a pixel %.0f uu), %s: %d band pixels in %d tiles, %s from end to end; with the line as it is %d pixels, %s"),
				Level, PixelSize, Case.Name, Widened.Num(), TileCoords.Num(), bUnbroken ? TEXT("unbroken") : TEXT("BROKEN"), AsItIs.Num(),
				bPointUnbroken ? TEXT("unbroken") : TEXT("broken")));
			TestTrue(FString::Printf(TEXT("Level %d, %s: the band is unbroken from the line's start to its end"), Level, Case.Name), bUnbroken);
			TestEqual(FString::Printf(TEXT("Level %d, %s: every pixel on the line as it is stays on the band"), Level, Case.Name), Lost, 0);
			// The band stays a line: no wider than three pixels across (the widening is 1.42 pixels).
			TestTrue(FString::Printf(TEXT("Level %d, %s: the band stays a line, not an area"), Level, Case.Name),
				Widened.Num() < 4 * FMath::CeilToInt32((Case.End - Case.Start).Size() / PixelSize) + 16);
		}
	}
	TestTrue(TEXT("A point sample alone breaks a band at some level (non-vacuous)"), bSomePointSampleBroke);

	W.Stop();
	Biomes->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
