// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "VoxelMapEditOverlay.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// QA refinement R22: the map draws voxel edits from a per-tile edit overlay. FVoxelMapEditOverlay::Build scans the
// edited columns of a tile's chunk stack for their top surface and keeps the columns where an edit changed it. These
// tests build small synthetic stacks (16^3 chunks, flat analytic terrain), so they need no world and no RHI.
// ---------------------------------------------------------------------------

namespace VoxelMapOverlayTest
{
	constexpr int32 CS = 16;
	constexpr float VS = 100.0f;
	constexpr float SurfaceHeight = 1450.0f; // inside chunk 0 (Z 0..1600): voxel 14 solid, voxel 15 air
	constexpr uint8 GrassMaterial = 1;
	constexpr uint8 DirtMaterial = 2;
	constexpr uint8 BrushMaterial = 7;

	int32 Index(int32 X, int32 Y, int32 Z) { return X + Y * CS + Z * CS * CS; }
	int32 ColumnOf(int32 X, int32 Y) { return Y * CS + X; }

	/** Procedural chunk of a flat terrain at SurfaceHeight: linear density across it, grass on top, dirt below. */
	TArray<FVoxelData> MakeTerrainChunk(int32 ChunkZ, bool bUseWater = false, float WaterLevel = 0.0f)
	{
		TArray<FVoxelData> Voxels;
		Voxels.SetNum(CS * CS * CS);
		for (int32 Z = 0; Z < CS; ++Z)
		{
			const float WorldZ = (ChunkZ * CS + Z) * VS;
			FVoxelData Voxel = FVoxelData::Air();
			Voxel.Density = static_cast<uint8>(FMath::Clamp(FMath::FloorToInt(127.0f + (SurfaceHeight - WorldZ) / VS * 128.0f), 0, 255));
			if (Voxel.IsSolid())
			{
				Voxel.MaterialID = (SurfaceHeight - WorldZ < VS) ? GrassMaterial : DirtMaterial;
			}
			else if (bUseWater && WorldZ < WaterLevel)
			{
				Voxel.SetWaterFlag(true);
			}
			for (int32 Y = 0; Y < CS; ++Y)
			{
				for (int32 X = 0; X < CS; ++X)
				{
					Voxels[Index(X, Y, Z)] = Voxel;
				}
			}
		}
		return Voxels;
	}

	/** Chunk stack entry; TopEditedZ filled from the (X, Y, Z) cells passed to MarkEdited. */
	struct FStackChunk
	{
		int32 ChunkZ = 0;
		TArray<FVoxelData> Voxels;
		bool bLoaded = true;
		TArray<int16> TopEditedZ;

		void MarkEdited(int32 X, int32 Y, int32 Z)
		{
			if (TopEditedZ.Num() == 0)
			{
				TopEditedZ.Init(-1, CS * CS);
			}
			TopEditedZ[ColumnOf(X, Y)] = FMath::Max<int16>(TopEditedZ[ColumnOf(X, Y)], static_cast<int16>(Z));
		}

		/** Apply a voxel edit to the merged data and record it. */
		void Edit(int32 X, int32 Y, int32 Z, const FVoxelData& NewVoxel)
		{
			Voxels[Index(X, Y, Z)] = NewVoxel;
			MarkEdited(X, Y, Z);
		}
	};

	FVoxelMapOverlayBuildInput MakeInput(const TArray<FStackChunk>& Stack, bool bUseWater = false, float WaterLevel = 0.0f)
	{
		FVoxelMapOverlayBuildInput Input;
		Input.ChunkSize = CS;
		Input.VoxelSize = VS;
		Input.WorldOriginZ = 0.0f;
		Input.bUseWater = bUseWater;
		Input.WaterLevel = WaterLevel;
		for (const FStackChunk& Chunk : Stack)
		{
			FVoxelMapOverlayChunk& Out = Input.Chunks.AddDefaulted_GetRef();
			Out.ChunkZ = Chunk.ChunkZ;
			if (Chunk.bLoaded)
			{
				Out.Voxels = MakeShared<const TArray<FVoxelData>>(Chunk.Voxels);
			}
			Out.TopEditedZ = Chunk.TopEditedZ;
		}
		return Input;
	}

	FVoxelMapEditOverlay BuildFlat(const FVoxelMapOverlayBuildInput& Input)
	{
		TArray<float> Heights;
		Heights.Init(SurfaceHeight, CS * CS);
		return FVoxelMapEditOverlay::Build(Input, Heights, [](int32, int32) { return GrassMaterial; });
	}

	FVoxelData SolidVoxel(uint8 Material) { return FVoxelData(Material, 255); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapEditOverlayDigTest,
	"VoxelWorlds.Map.EditOverlay.DigLowersSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapEditOverlayDigTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapOverlayTest;

	// Carve voxels 11..14 of column (5, 5): the surface drops from 1450 to between voxel 10 and 11, onto dirt.
	FStackChunk Ground;
	Ground.ChunkZ = 0;
	Ground.Voxels = MakeTerrainChunk(0);
	for (int32 Z = 11; Z <= 14; ++Z)
	{
		Ground.Edit(5, 5, Z, FVoxelData::Air());
	}
	const FVoxelMapEditOverlay Overlay = BuildFlat(MakeInput({ Ground }));

	TestEqual(TEXT("resolution is the chunk size"), Overlay.Resolution, CS);
	TestEqual(TEXT("exactly the carved column is masked"), Overlay.Num(), 1);
	const int32 Slot = Overlay.FindColumn(5, 5);
	if (!TestTrue(TEXT("the carved column is masked"), Slot != INDEX_NONE))
	{
		return false;
	}
	TestTrue(FString::Printf(TEXT("surface lowered to the top of voxel 10 (got %.1f)"), Overlay.Heights[Slot]),
		Overlay.Heights[Slot] >= 1000.0f && Overlay.Heights[Slot] <= 1100.0f);
	TestEqual(TEXT("the exposed material is dirt"), Overlay.Materials[Slot], DirtMaterial);
	TestEqual(TEXT("no water on a dry dig"), Overlay.Flags[Slot], static_cast<uint8>(0));
	TestEqual(TEXT("an untouched column is not masked"), Overlay.FindColumn(0, 0), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("nothing pending"), Overlay.PendingColumns.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapEditOverlayBuildTest,
	"VoxelWorlds.Map.EditOverlay.BuildRaisesSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapEditOverlayBuildTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapOverlayTest;

	FStackChunk Ground;
	Ground.ChunkZ = 0;
	Ground.Voxels = MakeTerrainChunk(0);
	FStackChunk Above;
	Above.ChunkZ = 1;
	Above.Voxels = MakeTerrainChunk(1);

	// Column (2, 2): a one-voxel build inside the ground chunk (voxel 15 is the first air voxel).
	Ground.Edit(2, 2, 15, SolidVoxel(BrushMaterial));
	// Column (8, 8): a pillar that crosses into the chunk above (ground voxel 15, above voxels 0..3).
	Ground.Edit(8, 8, 15, SolidVoxel(BrushMaterial));
	for (int32 Z = 0; Z <= 3; ++Z)
	{
		Above.Edit(8, 8, Z, SolidVoxel(BrushMaterial));
	}

	const FVoxelMapEditOverlay Overlay = BuildFlat(MakeInput({ Ground, Above }));
	TestEqual(TEXT("both pillars are masked"), Overlay.Num(), 2);

	const int32 Short = Overlay.FindColumn(2, 2);
	if (TestTrue(TEXT("the short pillar is masked"), Short != INDEX_NONE))
	{
		TestTrue(FString::Printf(TEXT("short pillar top is above voxel 15 (got %.1f)"), Overlay.Heights[Short]),
			Overlay.Heights[Short] >= 1500.0f && Overlay.Heights[Short] <= 1600.0f);
		TestEqual(TEXT("short pillar shows the brush material"), Overlay.Materials[Short], BrushMaterial);
	}
	const int32 Tall = Overlay.FindColumn(8, 8);
	if (TestTrue(TEXT("the pillar crossing into the chunk above is masked"), Tall != INDEX_NONE))
	{
		TestTrue(FString::Printf(TEXT("tall pillar top is the top of global voxel 19 (got %.1f)"), Overlay.Heights[Tall]),
			Overlay.Heights[Tall] >= 1900.0f && Overlay.Heights[Tall] <= 2000.0f);
		TestEqual(TEXT("tall pillar shows the brush material"), Overlay.Materials[Tall], BrushMaterial);
	}

	// The same tall pillar when the chunk above is edited but not loaded: its top is unknown, so the column waits.
	Above.bLoaded = false;
	const FVoxelMapEditOverlay Unloaded = BuildFlat(MakeInput({ Ground, Above }));
	TestTrue(TEXT("a build into an unloaded edited chunk is pending"), Unloaded.IsPending(8, 8));
	TestEqual(TEXT("and not masked"), Unloaded.FindColumn(8, 8), static_cast<int32>(INDEX_NONE));
	TestTrue(TEXT("the short pillar still resolves"), Unloaded.FindColumn(2, 2) != INDEX_NONE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapEditOverlayBuriedTest,
	"VoxelWorlds.Map.EditOverlay.BuriedEditIsIgnored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapEditOverlayBuriedTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapOverlayTest;

	// A cavity under an intact surface (voxels 3..6 of the ground chunk), a dungeon room a chunk below (loaded), and
	// edits two chunks below in a chunk that is not loaded at all.
	FStackChunk Ground;
	Ground.ChunkZ = 0;
	Ground.Voxels = MakeTerrainChunk(0);
	for (int32 Z = 3; Z <= 6; ++Z)
	{
		Ground.Edit(5, 5, Z, FVoxelData::Air());
	}
	FStackChunk Dungeon;
	Dungeon.ChunkZ = -1;
	Dungeon.Voxels = MakeTerrainChunk(-1);
	for (int32 Y = 2; Y < 12; ++Y)
	{
		for (int32 X = 2; X < 12; ++X)
		{
			for (int32 Z = 4; Z < 10; ++Z)
			{
				Dungeon.Edit(X, Y, Z, FVoxelData::Air());
			}
		}
	}
	FStackChunk Deep;
	Deep.ChunkZ = -2;
	Deep.Voxels = MakeTerrainChunk(-2);
	Deep.Edit(9, 9, 8, FVoxelData::Air());
	Deep.bLoaded = false;

	const FVoxelMapEditOverlay Overlay = BuildFlat(MakeInput({ Ground, Dungeon, Deep }));
	TestTrue(TEXT("buried edits produce no masked column"), Overlay.IsEmpty());
	TestEqual(TEXT("and nothing pending (the surface above them is readable)"), Overlay.PendingColumns.Num(), 0);

	// A paint edit on the surface voxel itself is a top-surface change: the material differs from the analytic one.
	Ground.Edit(7, 7, 14, FVoxelData(BrushMaterial, Ground.Voxels[Index(7, 7, 14)].Density));
	const FVoxelMapEditOverlay Painted = BuildFlat(MakeInput({ Ground, Dungeon, Deep }));
	const int32 Slot = Painted.FindColumn(7, 7);
	TestTrue(TEXT("a painted surface voxel is masked"), Slot != INDEX_NONE);
	if (Slot != INDEX_NONE)
	{
		TestTrue(TEXT("at the analytic height"), FMath::Abs(Painted.Heights[Slot] - SurfaceHeight) <= 0.5f * VS);
		TestEqual(TEXT("with the painted material"), Painted.Materials[Slot], BrushMaterial);
	}
	TestEqual(TEXT("only the painted column"), Painted.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapEditOverlayDugThroughTest,
	"VoxelWorlds.Map.EditOverlay.DugThroughStaysPending",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapEditOverlayDugThroughTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapOverlayTest;

	// Column (5, 5) carved through the whole ground chunk. The chunk below is not listed (read as analytic, solid):
	// real air resting on analytic ground below the analytic surface means the dig left the readable stack.
	FStackChunk Ground;
	Ground.ChunkZ = 0;
	Ground.Voxels = MakeTerrainChunk(0);
	for (int32 Z = 0; Z < CS; ++Z)
	{
		Ground.Edit(5, 5, Z, FVoxelData::Air());
	}
	const FVoxelMapEditOverlay Unlisted = BuildFlat(MakeInput({ Ground }));
	TestTrue(TEXT("dug into an unlisted chunk: pending"), Unlisted.IsPending(5, 5));
	TestEqual(TEXT("not masked"), Unlisted.FindColumn(5, 5), static_cast<int32>(INDEX_NONE));

	// The chunk below is edited too but not loaded: the scan stops above it and finds no surface.
	FStackChunk Below;
	Below.ChunkZ = -1;
	Below.Voxels = MakeTerrainChunk(-1);
	Below.Edit(5, 5, CS - 1, FVoxelData::Air());
	Below.bLoaded = false;
	const FVoxelMapEditOverlay Truncated = BuildFlat(MakeInput({ Ground, Below }));
	TestTrue(TEXT("dug into an unloaded edited chunk: pending"), Truncated.IsPending(5, 5));

	// Loaded below, the dig resolves onto its real surface.
	Below.bLoaded = true;
	const FVoxelMapEditOverlay Resolved = BuildFlat(MakeInput({ Ground, Below }));
	TestFalse(TEXT("with the chunk below loaded the column resolves"), Resolved.IsPending(5, 5));
	const int32 Slot = Resolved.FindColumn(5, 5);
	TestTrue(TEXT("and is masked"), Slot != INDEX_NONE);
	if (Slot != INDEX_NONE)
	{
		TestTrue(FString::Printf(TEXT("on top of voxel 14 of the chunk below (got %.1f)"), Resolved.Heights[Slot]),
			Resolved.Heights[Slot] >= -200.0f && Resolved.Heights[Slot] <= -100.0f);
	}

	// A pending column keeps whatever the previous overlay showed for it.
	FVoxelMapEditOverlay Previous;
	Previous.Resolution = CS;
	Previous.Columns = { static_cast<uint16>(ColumnOf(1, 1)), static_cast<uint16>(ColumnOf(5, 5)) };
	Previous.Heights = { 1200.0f, 900.0f };
	Previous.Materials = { DirtMaterial, DirtMaterial };
	Previous.Flags = { 0, 0 };
	const FVoxelMapEditOverlay Merged = FVoxelMapEditOverlay::MergePending(Unlisted, &Previous);
	const int32 Kept = Merged.FindColumn(5, 5);
	TestTrue(TEXT("merge keeps the previous value of the pending column"), Kept != INDEX_NONE && Merged.Heights[Kept] == 900.0f);
	TestEqual(TEXT("merge drops previous columns that are no longer pending or edited"), Merged.FindColumn(1, 1), static_cast<int32>(INDEX_NONE));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapEditOverlayWaterTest,
	"VoxelWorlds.Map.EditOverlay.WaterBitFromVoxels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapEditOverlayWaterTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapOverlayTest;

	// The water line (1200) lies below the analytic surface (1450): the analytic map draws land here. Two pits dug to
	// voxel 7 (surface ~800, below the water line): (5, 5) stays dry, (9, 9) flooded by water propagation.
	constexpr float WaterLevel = 1200.0f;
	FStackChunk Ground;
	Ground.ChunkZ = 0;
	Ground.Voxels = MakeTerrainChunk(0, true, WaterLevel);
	for (int32 Z = 8; Z <= 14; ++Z)
	{
		Ground.Edit(5, 5, Z, FVoxelData::Air());
		Ground.Edit(9, 9, Z, (Z * VS < WaterLevel) ? FVoxelData::Water() : FVoxelData::Air());
	}
	const FVoxelMapEditOverlay Overlay = BuildFlat(MakeInput({ Ground }, true, WaterLevel));

	const int32 Dry = Overlay.FindColumn(5, 5);
	const int32 Wet = Overlay.FindColumn(9, 9);
	if (!TestTrue(TEXT("both pits are masked"), Dry != INDEX_NONE && Wet != INDEX_NONE))
	{
		return false;
	}
	TestTrue(TEXT("both pits lie below the water line"), Overlay.Heights[Dry] < WaterLevel && Overlay.Heights[Wet] < WaterLevel);
	TestEqual(TEXT("the dry pit below the water line stays land"), Overlay.Flags[Dry] & FVoxelMapEditOverlay::FlagWater, 0);
	TestEqual(TEXT("the flooded pit is water"), Overlay.Flags[Wet] & FVoxelMapEditOverlay::FlagWater, static_cast<int32>(FVoxelMapEditOverlay::FlagWater));

	// Under the sea (water line 1700 above the 1450 seabed, so the generated air at voxel 15 is water-flagged): a pit
	// dug into the seabed opens unflagged voxels, but the sea above them is water, so the column reads as water.
	constexpr float SeaLevel = 1700.0f;
	FStackChunk Seabed;
	Seabed.ChunkZ = 0;
	Seabed.Voxels = MakeTerrainChunk(0, true, SeaLevel);
	for (int32 Z = 10; Z <= 14; ++Z)
	{
		Seabed.Edit(12, 12, Z, FVoxelData::Air());
	}
	const FVoxelMapEditOverlay Sea = BuildFlat(MakeInput({ Seabed }, true, SeaLevel));
	const int32 Pit = Sea.FindColumn(12, 12);
	if (TestTrue(TEXT("the seabed pit is masked (it is deeper)"), Pit != INDEX_NONE))
	{
		TestTrue(TEXT("lower than the seabed"), Sea.Heights[Pit] < SurfaceHeight - VS);
		TestEqual(TEXT("and still water"), Sea.Flags[Pit] & FVoxelMapEditOverlay::FlagWater, static_cast<int32>(FVoxelMapEditOverlay::FlagWater));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapEditOverlayRoundTripTest,
	"VoxelWorlds.Map.EditOverlay.EncodeDecodeRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapEditOverlayRoundTripTest::RunTest(const FString& Parameters)
{
	// A 64-wide overlay with columns at the corners and a block in the middle, negative and fractional heights.
	FVoxelMapEditOverlay Overlay;
	Overlay.Resolution = 64;
	for (int32 Col : { 0, 63, 64 * 63, 64 * 64 - 1 })
	{
		Overlay.Columns.Add(static_cast<uint16>(Col));
	}
	for (int32 Y = 20; Y < 30; ++Y)
	{
		for (int32 X = 30; X < 40; ++X)
		{
			Overlay.Columns.Add(static_cast<uint16>(Y * 64 + X));
		}
	}
	Overlay.Columns.Sort();
	for (int32 i = 0; i < Overlay.Columns.Num(); ++i)
	{
		Overlay.Heights.Add(-3125.5f + i * 17.25f);
		Overlay.Materials.Add(static_cast<uint8>(i % 256));
		Overlay.Flags.Add((i % 3 == 0) ? FVoxelMapEditOverlay::FlagWater : 0);
	}
	Overlay.PendingColumns = { 5 }; // build output only: not saved

	TArray<uint8> Bytes;
	{
		FMemoryWriter Writer(Bytes);
		TestTrue(TEXT("serializes"), Overlay.Serialize(Writer));
	}
	TestTrue(TEXT("mask + values stay small (under 8 bytes per masked column plus the mask)"),
		Bytes.Num() < Overlay.Num() * 8 + 64 * 64 / 8 + 8);

	FVoxelMapEditOverlay Decoded;
	{
		FMemoryReader Reader(Bytes);
		TestTrue(TEXT("deserializes"), Decoded.Serialize(Reader));
	}
	TestTrue(TEXT("round trip preserves every masked column and value"), Decoded == Overlay);
	TestEqual(TEXT("pending columns are not saved"), Decoded.PendingColumns.Num(), 0);

	// Truncated data and a bad version fail cleanly and leave an empty overlay.
	TArray<uint8> Truncated(Bytes.GetData(), Bytes.Num() / 2);
	FVoxelMapEditOverlay Broken;
	{
		FMemoryReader Reader(Truncated);
		TestFalse(TEXT("truncated data is rejected"), Broken.Serialize(Reader));
	}
	TestTrue(TEXT("a rejected overlay is empty"), Broken.IsEmpty() && Broken.Resolution == 0);
	TArray<uint8> BadVersion = Bytes;
	BadVersion[0] = FVoxelMapEditOverlay::FormatVersion + 1;
	{
		FMemoryReader Reader(BadVersion);
		TestFalse(TEXT("an unknown format version is rejected"), Broken.Serialize(Reader));
	}

	// An empty overlay round-trips too.
	FVoxelMapEditOverlay Empty;
	Empty.Resolution = 64;
	TArray<uint8> EmptyBytes;
	{
		FMemoryWriter Writer(EmptyBytes);
		Empty.Serialize(Writer);
	}
	FVoxelMapEditOverlay EmptyDecoded;
	{
		FMemoryReader Reader(EmptyBytes);
		TestTrue(TEXT("an empty overlay deserializes"), EmptyDecoded.Serialize(Reader));
	}
	TestTrue(TEXT("and stays empty"), EmptyDecoded == Empty);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapEditOverlayBordersTest,
	"VoxelWorlds.Map.EditOverlay.BorderChangesDetected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapEditOverlayBordersTest::RunTest(const FString& Parameters)
{
	auto Make = [](TArray<uint16> Columns, float Height)
	{
		FVoxelMapEditOverlay O;
		O.Resolution = 16;
		O.Columns = Columns;
		for (int32 i = 0; i < Columns.Num(); ++i)
		{
			O.Heights.Add(Height);
			O.Materials.Add(1);
			O.Flags.Add(0);
		}
		return O;
	};
	const FVoxelMapEditOverlay Interior = Make({ 5 * 16 + 5 }, 100.0f);
	const FVoxelMapEditOverlay InteriorMoved = Make({ 6 * 16 + 6 }, 50.0f);
	const FVoxelMapEditOverlay Edge = Make({ 5 * 16 + 0 }, 100.0f);
	const FVoxelMapEditOverlay EdgeLower = Make({ 5 * 16 + 0 }, 40.0f);

	TestTrue(TEXT("null and empty agree"), FVoxelMapEditOverlay::BordersEqual(nullptr, nullptr));
	TestTrue(TEXT("interior-only changes leave the border equal"), FVoxelMapEditOverlay::BordersEqual(&Interior, &InteriorMoved));
	TestTrue(TEXT("an interior-only overlay matches none"), FVoxelMapEditOverlay::BordersEqual(&Interior, nullptr));
	TestFalse(TEXT("a new border column is a change"), FVoxelMapEditOverlay::BordersEqual(&Interior, &Edge));
	TestFalse(TEXT("a border height change is a change"), FVoxelMapEditOverlay::BordersEqual(&Edge, &EdgeLower));
	TestTrue(TEXT("identical borders agree"), FVoxelMapEditOverlay::BordersEqual(&Edge, &Edge));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
