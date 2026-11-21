// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelChunkWaterColumns.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelData.h"
#include "VoxelNoiseTypes.h"
#include "VoxelSeaSurface.h"
#include "VoxelWaterLevels.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// A chunk's water columns and the fill that reads them (the hydrology plan's H2, section 5.4).
//
// - Generation.WaterColumns.Fill: made ground in chunks of 16 voxels, a basin 20 columns across that reaches into the
//   chunk layer below, and a lake over it whose footprint is narrower than the basin on one side and wider on the
//   other, across a chunk face. After the post-passes each chunk's water is exactly the air at or under the lake's level
//   in the columns of its footprint: a column inside holds water to the level, a column of the footprint whose ground
//   stands over the level holds none (the shore), low ground outside the footprint stays dry, the chunk the footprint
//   crosses into agrees with its neighbour, the chunk under the lake takes the basin's water from its top layer, the
//   chunk over it takes none, and a cave beside the water stays dry and loses its flag. With the sea off, with a sea
//   under the ground and with a sea a voxel under the lake, which stands in the basin's column outside the footprint
//   while the lake stands over it inside. Then a river reach
//   along a channel: each column of the channel holds water to the reach's own level there, which falls along it.
// - Generation.WaterColumns.Planet: on a planet a chunk has no columns: the list holds a level per down-line. For
//   every voxel of a chunk at latitude 45 the row of its line agrees with the water levels' sample at the voxel
//   itself (the level as an altitude and over the anchor, the voxel test) but along the footprint's edge, the reader a
//   pull or a flood asks gives the same rows as the list a generator builds, and the fill of made ground puts water
//   in the lake's footprint to its level and nowhere else.
// - Generation.WaterColumns.OneLevel: a request with no body builds no list; a chunk whose request carries a lake that
//   covers none of its columns generates, column by column, the voxels the one level gives, byte for byte on a plane.
// ---------------------------------------------------------------------------

namespace VoxelWaterColumnsTest
{
	constexpr int32 N = 16;
	constexpr float VoxelSize = 100.0f;

	int32 IndexOf(int32 X, int32 Y, int32 Z) { return X + Y * N + Z * N * N; }

	/** A heightmap request for a chunk of made ground. */
	FVoxelNoiseGenerationRequest MakeRequest(const FIntVector& Chunk, bool bSea, float SeaLevel)
	{
		FVoxelNoiseGenerationRequest Request;
		Request.ChunkCoord = Chunk;
		Request.ChunkSize = N;
		Request.VoxelSize = VoxelSize;
		Request.WorldMode = EWorldMode::InfinitePlane;
		Request.bEnableWaterLevel = bSea;
		Request.WaterLevel = SeaLevel;
		return Request;
	}

	/** Carry a sea and bodies on a request, over its chunk's box. */
	void Carry(FVoxelNoiseGenerationRequest& Request, const FVoxelSeaSurface& Sea, const FVoxelWaterFrame& Frame, TArray<FVoxelWaterBody> Bodies)
	{
		const FVector Min = Request.GetChunkWorldPosition();
		Request.WaterBodies = MakeShared<const FVoxelWaterLevels>(FVoxelWaterLevels::Make(Sea, Frame,
			FBox(Min, Min + FVector(Request.ChunkSize * Request.VoxelSize)), MoveTemp(Bodies)));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterColumnsFillTest, "VoxelWorlds.Generation.WaterColumns.Fill",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterColumnsFillTest::RunTest(const FString& Parameters)
{
	using namespace VoxelWaterColumnsTest;

	// ---- The lake. The ground in world voxel columns (gx, gy): a basin around (15.5, 7.5) whose floor is two voxels
	// under Z 0 and whose sides rise two voxels a column. The level 750: voxels up to Z index 7 are at or under it, so
	// the ground lies under it for gx 11 to 20, gy 3 to 12.
	auto GroundTop = [](int32 GX, int32 GY)
	{
		const double D = FMath::Max(FMath::Abs(GX - 15.5), FMath::Abs(GY - 7.5));
		return -2 + 2 * FMath::FloorToInt32(D);
	};
	// One cave voxel in the bank, beside the water of column (13, 7).
	const FIntVector CaveVoxel(12, 7, 2);
	const double LakeLevel = 750.0;
	// The footprint: cells of 200 uu over gx 12 to 21, gy 2 to 13. Column gx 11 is low ground outside it; column
	// gx 21 is in it with its ground over the level; it crosses the chunk face at gx 16.
	const TSharedRef<const FVoxelWaterLake> Footprint = FVoxelWaterLake::Make(FVector2D(1200.0, 200.0), 200.0, FIntPoint(5, 6), TBitArray<>(true, 30));
	auto InFootprint = [](int32 GX, int32 GY) { return GX >= 12 && GX <= 21 && GY >= 2 && GY <= 13; };
	const FVoxelWaterFrame Frame;
	const FVoxelWaterBody Lake = FVoxelWaterBody::MakeLake(7, LakeLevel, Footprint, Frame, 2000.0);

	auto BuildGround = [&](const FIntVector& Chunk, TArray<FVoxelData>& Data)
	{
		Data.SetNum(N * N * N);
		for (int32 Z = 0; Z < N; ++Z)
		for (int32 Y = 0; Y < N; ++Y)
		for (int32 X = 0; X < N; ++X)
		{
			const FIntVector G = Chunk * N + FIntVector(X, Y, Z);
			FVoxelData Voxel = G.Z < GroundTop(G.X, G.Y) ? FVoxelData::Solid(1, 0) : FVoxelData::Air();
			if (G == CaveVoxel)
			{
				Voxel = FVoxelData::Air();
				Voxel.SetCaveFlag(true);
			}
			Data[IndexOf(X, Y, Z)] = Voxel;
		}
	};

	struct FSeaCase
	{
		const TCHAR* Name;
		bool bSea;
		float SeaLevel;
		/** Wet voxels in the basin's column outside the footprint (gx 11, its ground at Z index 6) */
		int32 OutsideWet;
	};
	// The last sea stands at Z 650, a voxel under the lake: the basin's column outside the footprint holds it (one
	// voxel), and inside the footprint the lake, which stands higher, answers.
	const FSeaCase SeaCases[] = { { TEXT("the sea off"), false, 0.0f, 0 }, { TEXT("a sea under the ground"), true, -5000.0f, 0 },
		{ TEXT("a sea a voxel under the lake"), true, 650.0f, 1 } };
	const FIntVector Chunks[] = { FIntVector(0, 0, 0), FIntVector(1, 0, 0), FIntVector(0, 0, -1), FIntVector(1, 0, -1), FIntVector(0, 0, 1) };
	for (const FSeaCase& Case : SeaCases)
	{
		TMap<FIntVector, TArray<FVoxelData>> Filled;
		int32 TotalWet = 0;
		for (const FIntVector& Chunk : Chunks)
		{
			FVoxelNoiseGenerationRequest Request = MakeRequest(Chunk, Case.bSea, Case.SeaLevel);
			Carry(Request, Case.bSea ? FVoxelSeaSurface::MakePlane(Case.SeaLevel) : FVoxelSeaSurface(), Frame, { Lake });
			TArray<FVoxelData>& Data = Filled.Add(Chunk);
			BuildGround(Chunk, Data);
			FVoxelCPUNoiseGenerator::ApplyPostReadbackPasses(Request, Data);

			int32 Wrong = 0, Wet = 0, CaveFlags = 0;
			for (int32 Z = 0; Z < N; ++Z)
			for (int32 Y = 0; Y < N; ++Y)
			for (int32 X = 0; X < N; ++X)
			{
				const FIntVector G = Chunk * N + FIntVector(X, Y, Z);
				const FVoxelData& Voxel = Data[IndexOf(X, Y, Z)];
				// The level over the column: the lake's in its footprint, else the sea's (the lake stands over it).
				const bool bLakeColumn = InFootprint(G.X, G.Y);
				const bool bHasLevel = bLakeColumn || Case.bSea;
				const double Level = bLakeColumn ? LakeLevel : Case.SeaLevel;
				const bool bExpected = bHasLevel && Voxel.IsAir() && G != CaveVoxel && G.Z * static_cast<double>(VoxelSize) <= Level;
				Wrong += (Voxel.HasWaterFlag() != bExpected) ? 1 : 0;
				Wet += Voxel.HasWaterFlag() ? 1 : 0;
				CaveFlags += Voxel.HasCaveFlag() ? 1 : 0;
			}
			TotalWet += Wet;
			AddInfo(FString::Printf(TEXT("Lake, %s: chunk (%d, %d, %d) holds %d wet voxels, %d not as the lake's level and footprint say"),
				Case.Name, Chunk.X, Chunk.Y, Chunk.Z, Wet, Wrong));
			TestEqual(FString::Printf(TEXT("Lake, %s: chunk (%d, %d, %d): every voxel is wet exactly where it is air at or under the level over its column"),
				Case.Name, Chunk.X, Chunk.Y, Chunk.Z), Wrong, 0);
			TestEqual(FString::Printf(TEXT("Lake, %s: chunk (%d, %d, %d): the cave flags are cleared"), Case.Name, Chunk.X, Chunk.Y, Chunk.Z), CaveFlags, 0);
		}
		TestTrue(FString::Printf(TEXT("Lake, %s: the lake holds water (non-vacuous)"), Case.Name), TotalWet > 300);

		// The named columns, in the chunk the level lies in.
		const TArray<FVoxelData>& Level0 = Filled[FIntVector(0, 0, 0)];
		const TArray<FVoxelData>& Level1 = Filled[FIntVector(1, 0, 0)];
		auto WetInColumn = [](const TArray<FVoxelData>& Data, int32 X, int32 Y)
		{
			int32 Count = 0;
			for (int32 Z = 0; Z < N; ++Z)
			{
				Count += Data[IndexOf(X, Y, Z)].HasWaterFlag() ? 1 : 0;
			}
			return Count;
		};
		TestEqual(FString::Printf(TEXT("Lake, %s: a column inside holds water from the chunk's floor to the level (8 voxels)"), Case.Name), WetInColumn(Level0, 15, 7), 8);
		TestEqual(FString::Printf(TEXT("Lake, %s: the cave's column holds water over its ground only"), Case.Name), WetInColumn(Level0, 12, 7), 4);
		TestEqual(FString::Printf(TEXT("Lake, %s: low ground outside the footprint takes none of the lake's water (the sea's alone, where it stands there)"), Case.Name),
			WetInColumn(Level0, 11, 7), Case.OutsideWet);
		TestEqual(FString::Printf(TEXT("Lake, %s: the last column under the level holds two voxels (the shore)"), Case.Name), WetInColumn(Level1, 4, 7), 2);
		TestEqual(FString::Printf(TEXT("Lake, %s: a column of the footprint whose ground stands over the level is dry"), Case.Name), WetInColumn(Level1, 5, 7), 0);
		// Across the chunk face the two chunks agree: the same water on both sides.
		int32 FaceWet = 0, FaceDiffers = 0;
		for (int32 Z = 0; Z < N; ++Z)
		for (int32 Y = 0; Y < N; ++Y)
		{
			const bool bHere = Level0[IndexOf(N - 1, Y, Z)].HasWaterFlag();
			const bool bThere = Level1[IndexOf(0, Y, Z)].HasWaterFlag();
			FaceWet += bHere ? 1 : 0;
			// The ground is mirrored about gx 15.5, so the two layers hold the same air.
			FaceDiffers += (bHere != bThere) ? 1 : 0;
		}
		TestTrue(FString::Printf(TEXT("Lake, %s: the footprint crosses the chunk face with water on it"), Case.Name), FaceWet > 20);
		TestEqual(FString::Printf(TEXT("Lake, %s: the two chunks hold the same water across their face"), Case.Name), FaceDiffers, 0);
		// The chunk under the lake takes the basin's floor from its top layer; the chunk over it takes nothing.
		TestEqual(FString::Printf(TEXT("Lake, %s: the chunk under the lake holds the basin's floor"), Case.Name),
			WetInColumn(Filled[FIntVector(0, 0, -1)], 15, 7), 2);
	}

	// ---- The river reach. Ground six voxels high but for a channel four columns wide (gy 6 to 9) at two; the reach
	// runs along it from X 0 to 3,200, its surface falling from 560 to 360, its footprint 250 uu to each side (the
	// channel and a bank column either side, whose ground stands over the surface).
	TArray<FVoxelWaterReachVertex> Vertices;
	for (int32 Step = 0; Step <= 16; ++Step)
	{
		const double X = Step * 200.0;
		const double Surface = 560.0 - 200.0 * X / 3200.0;
		Vertices.Add(FVoxelWaterReachVertex(FVector(X, 750.0, 0.0), 200.0, Surface, 250.0));
	}
	const FVoxelWaterBody Reach = FVoxelWaterBody::MakeReach(8, FVoxelWaterReach::Make(Vertices, false, 100.0f), Frame);
	int32 ReachWet = 0;
	int32 HeadDepth = 0, FootDepth = 0;
	for (const FIntVector& Chunk : { FIntVector(0, 0, 0), FIntVector(1, 0, 0) })
	{
		FVoxelNoiseGenerationRequest Request = MakeRequest(Chunk, true, -5000.0f);
		Carry(Request, FVoxelSeaSurface::MakePlane(-5000.0), Frame, { Reach });
		TArray<FVoxelData> Data;
		Data.SetNum(N * N * N);
		for (int32 Z = 0; Z < N; ++Z)
		for (int32 Y = 0; Y < N; ++Y)
		for (int32 X = 0; X < N; ++X)
		{
			const bool bChannel = Y >= 6 && Y <= 9;
			Data[IndexOf(X, Y, Z)] = Z < (bChannel ? 2 : 6) ? FVoxelData::Solid(1, 0) : FVoxelData::Air();
		}
		FVoxelCPUNoiseGenerator::ApplyPostReadbackPasses(Request, Data);
		int32 Wrong = 0;
		for (int32 Z = 0; Z < N; ++Z)
		for (int32 Y = 0; Y < N; ++Y)
		for (int32 X = 0; X < N; ++X)
		{
			const int32 GX = Chunk.X * N + X;
			const double Surface = 560.0 - 200.0 * (GX * 100.0) / 3200.0;
			const bool bExpected = Y >= 6 && Y <= 9 && Z >= 2 && Z * 100.0 <= Surface;
			const bool bWet = Data[IndexOf(X, Y, Z)].HasWaterFlag();
			Wrong += (bWet != bExpected) ? 1 : 0;
			ReachWet += bWet ? 1 : 0;
			HeadDepth += (bWet && GX == 0 && Y == 7) ? 1 : 0;
			FootDepth += (bWet && GX == 31 && Y == 7) ? 1 : 0;
		}
		TestEqual(FString::Printf(TEXT("River reach: chunk (%d, 0, 0): each column of the channel holds water to the reach's own level there"), Chunk.X), Wrong, 0);
	}
	AddInfo(FString::Printf(TEXT("River reach: %d wet voxels; %d deep at its head, %d at its foot"), ReachWet, HeadDepth, FootDepth));
	TestTrue(TEXT("River reach: the surface falls along it (deeper water at the head than at the foot)"), HeadDepth == 4 && FootDepth == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterColumnsPlanetTest, "VoxelWorlds.Generation.WaterColumns.Planet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterColumnsPlanetTest::RunTest(const FString& Parameters)
{
	// A planet of 250,000 uu off the world's origin, chunks of 32 voxels of 75 uu; the site at latitude 45, where a
	// chunk's down-lines shear across two axes.
	const double Radius = 250000.0;
	const float VS = 75.0f;
	const int32 CS = 32;
	const FVector Center(12345.0, 54321.0, 0.0);
	const double Lat = FMath::DegreesToRadians(45.0), Lon = FMath::DegreesToRadians(3.0);
	const FVector Up(FMath::Cos(Lat) * FMath::Cos(Lon), FMath::Cos(Lat) * FMath::Sin(Lon), FMath::Sin(Lat));
	const double ChunkWorldSize = CS * VS;
	const FVector SitePos = Center + Up * Radius;
	const FIntVector Chunk(FMath::FloorToInt32((SitePos.X - Center.X) / ChunkWorldSize), FMath::FloorToInt32((SitePos.Y - Center.Y) / ChunkWorldSize),
		FMath::FloorToInt32((SitePos.Z - Center.Z) / ChunkWorldSize));

	FVoxelWaterFrame WaterFrame;
	WaterFrame.bRadial = true;
	WaterFrame.Center = Center;
	WaterFrame.DatumRadius = Radius;
	const FVoxelChunkWaterFrame Frame = FVoxelChunkWaterFrame::Make(Chunk, CS, VS, Center, true, Center, Radius);
	// The chunk's centre: the lake lies around its direction, and the made ground, the lake and the sea are placed by
	// its distance from the planet centre, so the chunk holds ground, the lake's water and air over it.
	const FVector ChunkCentre = Frame.ChunkWorldPos + FVector(0.5 * ChunkWorldSize);
	const FVector LakeUp = (ChunkCentre - Center).GetSafeNormal();
	const double CentreDist = FVector::Dist(ChunkCentre, Center);
	const double GroundRadius = CentreDist - 200.0;
	const double SeaRadius = CentreDist - 2000.0;
	const FVoxelSeaSurface Sea = FVoxelSeaSurface::MakeSphere(Center, SeaRadius);
	// A lake 8 x 8 cells of 150 uu (half the chunk's width), 300.4 over the made ground.
	const double LakeLevel = CentreDist + 100.4;
	const FVoxelWaterBody Lake = FVoxelWaterBody::MakeLake(9, LakeLevel,
		FVoxelWaterLake::MakeOnSphere(LakeUp, Radius, 150.0, FIntPoint(8, 8), TBitArray<>(true, 64)), WaterFrame, 2000.0, 6.0f);
	const FVoxelWaterLevels Water = FVoxelWaterLevels::Make(Sea, WaterFrame, FBox(Frame.ChunkWorldPos, Frame.ChunkWorldPos + FVector(ChunkWorldSize)), { Lake });

	FVoxelChunkWaterColumns Columns;
	FVoxelChunkWaterColumns::Build(Water, Frame, Columns);
	if (!TestTrue(TEXT("A planet chunk a lake reaches builds its list: the shift rows, then a row a down-line"),
		!Columns.IsEmpty() && Columns.GetFirstColumn() == CS && Columns.NumColumns() == 9 * CS * CS))
	{
		return false;
	}
	const FVoxelDownLines& Lines = Columns.GetLines();
	bool bShiftRows = true;
	bool bSheared = false;
	for (int32 K = 0; K < CS; ++K)
	{
		bShiftRows &= Columns.Data[K].X == static_cast<float>(Lines.Shifts[K].X) && Columns.Data[K].Y == static_cast<float>(Lines.Shifts[K].Y);
		bSheared |= Lines.Shifts[K].X != 0 || Lines.Shifts[K].Y != 0;
	}
	TestTrue(TEXT("The list starts with the down-lines' shifts"), bShiftRows);
	TestTrue(TEXT("The chunk's lines shear (the site is off the axes)"), bSheared);

	// Every voxel: its line's row against the water levels' sample at the voxel itself. A line takes the level over its
	// first voxel in the chunk, so along the footprint's edge a few voxels differ; everywhere else the row is the
	// sample, in both of its frames, and the voxel test is the sample's.
	const FVector AnchorRelative = (Frame.ChunkWorldPos - Center) + FVector(FVector3f(static_cast<float>(CS / 2) * VS));
	const double AnchorDist = AnchorRelative.Size();
	FVoxelChunkWaterReader Reader(Water, Frame);
	int32 EdgeVoxels = 0, WrongLevel = 0, WrongTest = 0, LakeVoxels = 0, ReaderDiffers = 0, CaveDepthWrong = 0;
	for (int32 Z = 0; Z < CS; ++Z)
	for (int32 Y = 0; Y < CS; ++Y)
	for (int32 X = 0; X < CS; ++X)
	{
		const FIntVector Local(X, Y, Z);
		const FVector Position = Frame.VoxelWorldPos(Local);
		const FVector4f Row = Columns.GetRowOf(Local);
		const FVector4f ReaderRow = Reader.GetRowOf(Local);
		ReaderDiffers += (Row != ReaderRow || Reader.IsVoxelUnder(Local) != Columns.IsVoxelUnder(Local)) ? 1 : 0;

		FVoxelWaterSample Sample;
		Water.Sample(Position, Sample);
		const double RowLevel = static_cast<double>(Row.Y) + AnchorDist;
		if (FMath::Abs(RowLevel - Sample.SurfaceLevel) > 1.0)
		{
			++EdgeVoxels; // the line's first voxel lies on the other side of the footprint's edge
			continue;
		}
		LakeVoxels += Sample.Kind == EVoxelWaterBodyKind::Lake ? 1 : 0;
		WrongLevel += (FMath::Abs(RowLevel - Sample.SurfaceLevel) > 0.01 || FMath::Abs(static_cast<double>(Row.X) + Radius - Sample.SurfaceLevel) > 0.01) ? 1 : 0;
		CaveDepthWrong += Row.Z != Sample.MinCaveDepth ? 1 : 0;
		const double Height = FVector::Dist(Position, Center);
		if (FMath::Abs(Height - Sample.SurfaceLevel) > 0.01)
		{
			WrongTest += (Columns.IsVoxelUnder(Local) != (Height <= Sample.SurfaceLevel)) ? 1 : 0;
		}
	}
	const int32 Total = CS * CS * CS;
	AddInfo(FString::Printf(TEXT("Planet: %d voxels, %d over the lake by their own sample, %d on a line whose level is the other side's (the footprint's edge), %d rows worked out by the reader of %d"),
		Total, LakeVoxels, EdgeVoxels, Reader.NumEvaluated(), Columns.NumColumns()));
	TestTrue(TEXT("The lake lies over a good part of the chunk (non-vacuous)"), LakeVoxels > Total / 10 && LakeVoxels < Total * 9 / 10);
	TestTrue(TEXT("A line's level differs from its voxels' own only along the footprint's edge (under 3 % of the voxels)"), EdgeVoxels < Total * 3 / 100);
	TestEqual(TEXT("A row holds the level over its line as an altitude and over the anchor"), WrongLevel, 0);
	TestEqual(TEXT("A row carries the lake's minimum cave depth"), CaveDepthWrong, 0);
	TestEqual(TEXT("The voxel test is the sample's: at or under the level over the voxel"), WrongTest, 0);
	TestEqual(TEXT("The reader gives the list's rows and answers"), ReaderDiffers, 0);
	TestTrue(TEXT("The reader works out only the lines that cross the chunk"), Reader.NumEvaluated() < Columns.NumColumns() / 2);

	// What a list costs to build, on a worker or the render thread, per chunk a body may reach: a planet chunk of 32
	// voxels with a lake, and a heightmap chunk of the same size with a lake and a river reach of 40 segments.
	{
		const int32 Builds = 50;
		FVoxelChunkWaterColumns Timed;
		double Start = FPlatformTime::Seconds();
		for (int32 i = 0; i < Builds; ++i)
		{
			FVoxelChunkWaterColumns::Build(Water, Frame, Timed);
		}
		const double PlanetMs = (FPlatformTime::Seconds() - Start) * 1000.0 / Builds;

		const FVoxelWaterFrame PlaneFrame;
		TArray<FVoxelWaterReachVertex> Vertices;
		for (int32 Step = 0; Step <= 40; ++Step)
		{
			Vertices.Add(FVoxelWaterReachVertex(FVector(Step * 60.0, 300.0 + Step * 45.0, 0.0), 0.0, 900.0 - Step * 5.0, 400.0));
		}
		const FVoxelChunkWaterFrame PlaneChunk = FVoxelChunkWaterFrame::Make(FIntVector::ZeroValue, CS, VS, FVector::ZeroVector, false, FVector::ZeroVector, 0.0);
		const FVoxelWaterLevels PlaneWater = FVoxelWaterLevels::Make(FVoxelSeaSurface::MakePlane(-500.0), PlaneFrame, FBox(FVector::ZeroVector, FVector(ChunkWorldSize)),
			{ FVoxelWaterBody::MakeLake(3, 700.0, FVoxelWaterLake::Make(FVector2D(900.0, 0.0), 150.0, FIntPoint(8, 8), TBitArray<>(true, 64)), PlaneFrame, 1000.0),
			  FVoxelWaterBody::MakeReach(4, FVoxelWaterReach::Make(Vertices, false, 100.0f), PlaneFrame) });
		Start = FPlatformTime::Seconds();
		for (int32 i = 0; i < Builds; ++i)
		{
			FVoxelChunkWaterColumns::Build(PlaneWater, PlaneChunk, Timed);
		}
		const double PlaneMs = (FPlatformTime::Seconds() - Start) * 1000.0 / Builds;
		AddInfo(FString::Printf(TEXT("Building a chunk's water columns: %.3f ms a planet chunk (%d lines, a lake), %.3f ms a heightmap chunk (%d columns, a lake and a river reach)"),
			PlanetMs, Columns.NumColumns(), PlaneMs, Timed.NumColumns()));
	}

	// The fill on made ground: solid under a sphere 200 uu under the chunk's centre, air over it. The lake's water is the
	// air at or under its level in its footprint; the sea, far under the chunk, holds none here.
	FVoxelNoiseGenerationRequest Request;
	Request.ChunkCoord = Chunk;
	Request.ChunkSize = CS;
	Request.VoxelSize = VS;
	Request.WorldOrigin = Center;
	Request.WorldMode = EWorldMode::SphericalPlanet;
	Request.SphericalPlanetParams.PlanetCenter = Center;
	Request.SphericalPlanetParams.PlanetRadius = Radius;
	Request.bEnableWaterLevel = true;
	Request.WaterRadius = SeaRadius;
	Request.WaterBodies = MakeShared<const FVoxelWaterLevels>(Water);
	TestTrue(TEXT("The request's frame is the frame the chunk manager makes for the chunk"),
		FVoxelChunkWaterFrame::FromRequest(Request).ChunkWorldPos.Equals(Frame.ChunkWorldPos, 0.0) && Request.GetPlanetAnchorRelative().Equals(AnchorRelative, 0.0));
	TArray<FVoxelData> Data;
	Data.SetNum(Total);
	for (int32 Z = 0; Z < CS; ++Z)
	for (int32 Y = 0; Y < CS; ++Y)
	for (int32 X = 0; X < CS; ++X)
	{
		const double Height = FVector::Dist(Frame.VoxelWorldPos(FIntVector(X, Y, Z)), Center);
		Data[X + Y * CS + Z * CS * CS] = Height < GroundRadius ? FVoxelData::Solid(1, 0) : FVoxelData::Air();
	}
	FVoxelCPUNoiseGenerator::ApplyPostReadbackPasses(Request, Data);
	int32 Wet = 0, WetWrong = 0, DryWrong = 0;
	for (int32 Z = 0; Z < CS; ++Z)
	for (int32 Y = 0; Y < CS; ++Y)
	for (int32 X = 0; X < CS; ++X)
	{
		const FIntVector Local(X, Y, Z);
		const FVoxelData& Voxel = Data[X + Y * CS + Z * CS * CS];
		const bool bExpected = Voxel.IsAir() && Columns.IsVoxelUnder(Local);
		Wet += Voxel.HasWaterFlag() ? 1 : 0;
		WetWrong += (Voxel.HasWaterFlag() && !bExpected) ? 1 : 0;
		DryWrong += (!Voxel.HasWaterFlag() && bExpected) ? 1 : 0;
	}
	AddInfo(FString::Printf(TEXT("Planet fill: %d wet voxels"), Wet));
	TestTrue(TEXT("The lake holds water over the made ground (non-vacuous)"), Wet > 200);
	TestEqual(TEXT("No voxel is wet but air at or under its line's level"), WetWrong, 0);
	TestEqual(TEXT("Every air voxel at or under its line's level is wet"), DryWrong, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterColumnsOneLevelTest, "VoxelWorlds.Generation.WaterColumns.OneLevel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterColumnsOneLevelTest::RunTest(const FString& Parameters)
{
	UVoxelBiomeConfiguration* BiomeConfig = NewObject<UVoxelBiomeConfiguration>();
	BiomeConfig->AddToRoot();
	BiomeConfig->InitializeDefaults();
	BiomeConfig->bEnableUnderwaterMaterials = true;
	for (FBiomeDefinition& Biome : BiomeConfig->Biomes)
	{
		Biome.UnderwaterSurfaceMaterial = 20;
		Biome.UnderwaterSubsurfaceMaterial = 21;
	}
	BiomeConfig->RebuildCaches();

	// The water parity test's chunk: a sea at 800 inside the chunk, most of its ground under it.
	FVoxelNoiseGenerationRequest Request;
	Request.ChunkCoord = FIntVector(3, 1, 0);
	Request.ChunkSize = 16;
	Request.VoxelSize = 100.0f;
	Request.NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	Request.NoiseParams.Seed = 4325;
	Request.NoiseParams.Frequency = 0.0025f;
	Request.NoiseParams.Octaves = 4;
	Request.NoiseParams.Lacunarity = 2.0f;
	Request.NoiseParams.Persistence = 0.5f;
	Request.WorldMode = EWorldMode::InfinitePlane;
	Request.SeaLevel = 0.0f;
	Request.HeightScale = 3000.0f;
	Request.BaseHeight = 0.0f;
	Request.bEnableBiomes = true;
	Request.BiomeConfiguration = BiomeConfig;
	Request.bEnableWaterLevel = true;
	Request.WaterLevel = 800.0f;

	FVoxelChunkWaterColumns Columns;
	FVoxelChunkWaterColumns::Build(Request, Columns);
	TestTrue(TEXT("A request with no body builds no list"), Columns.IsEmpty());
	FVoxelChunkWaterColumns::Build(FVoxelWaterLevels(FVoxelSeaSurface::MakePlane(800.0)), FVoxelChunkWaterFrame::FromRequest(Request), Columns);
	TestTrue(TEXT("The sea alone builds no list"), Columns.IsEmpty());

	FVoxelCPUNoiseGenerator Generator;
	Generator.Initialize();
	TArray<FVoxelData> OneLevel;
	Generator.GenerateChunkCPU(Request, OneLevel);
	int32 Water = 0;
	for (const FVoxelData& Voxel : OneLevel)
	{
		Water += Voxel.HasWaterFlag() ? 1 : 0;
	}
	TestTrue(TEXT("The chunk holds sea (non-vacuous)"), Water > 100);

	// The same chunk, its request carrying a lake whose footprint lies two chunks away: every column's level is the
	// sea's, read from the list. The voxels are the one level's, byte for byte.
	FVoxelNoiseGenerationRequest WithLake = Request;
	const FVector Min = Request.GetChunkWorldPosition();
	const FVoxelWaterBody Lake = FVoxelWaterBody::MakeLake(5, 1900.0,
		FVoxelWaterLake::Make(FVector2D(Min.X + 4000.0, Min.Y), 200.0, FIntPoint(4, 4), TBitArray<>(true, 16)), FVoxelWaterFrame(), 500.0);
	WithLake.WaterBodies = MakeShared<const FVoxelWaterLevels>(FVoxelWaterLevels::Make(FVoxelSeaSurface::MakePlane(800.0), FVoxelWaterFrame(),
		FBox(Min, Min + FVector(1600.0)), { Lake }));
	FVoxelChunkWaterColumns::Build(WithLake, Columns);
	bool bAllSea = !Columns.IsEmpty();
	for (int32 Column = 0; Column < Columns.NumColumns(); ++Column)
	{
		bAllSea &= Columns.GetRow(Column) == FVector4f(800.0f, 800.0f, 0.0f, 0.0f);
	}
	TestTrue(TEXT("A lake that covers no column of the chunk leaves every column the sea's level"), bAllSea);
	TArray<FVoxelData> PerColumn;
	Generator.GenerateChunkCPU(WithLake, PerColumn);
	TestTrue(TEXT("The chunk generated column by column is the chunk generated from the one level, byte for byte"),
		PerColumn.Num() == OneLevel.Num() && FMemory::Memcmp(PerColumn.GetData(), OneLevel.GetData(), OneLevel.Num() * sizeof(FVoxelData)) == 0);

	Generator.Shutdown();
	BiomeConfig->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
