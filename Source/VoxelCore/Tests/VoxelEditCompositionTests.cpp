// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "VoxelEditManager.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// A relative edit (Add, Subtract, Paint) on a voxel that holds a Set edit composes with it
// (WORLD_MODES_REVIEW_PLAN.md G2, WM-H3). POI pads and dungeon walls are written with Set; the player digs with
// Subtract. The Subtract used to replace the Set, so the voxel fell back to procedural data minus the delta: one
// dig under a pad standing over air removed the aimed voxel and its four face neighbours, which had only received
// the brush falloff. Pure edit-layer logic plus the edits file round trip, runs headless.
// ---------------------------------------------------------------------------

namespace VoxelEditCompositionTestUtils
{
	constexpr int32 ChunkSize = 16;
	constexpr float VoxelSize = 100.0f;

	UVoxelEditManager* MakeManager()
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->ChunkSize = ChunkSize;
		Config->VoxelSize = VoxelSize;
		Config->WorldOrigin = FVector::ZeroVector;
		UVoxelEditManager* Manager = NewObject<UVoxelEditManager>();
		Manager->Initialize(Config);
		Manager->SetEditSource(EEditSource::Player);
		return Manager;
	}

	/** Centre of voxel (X, Y, Z) of chunk (0, 0, 0). */
	FVector VoxelCentre(int32 X, int32 Y, int32 Z)
	{
		return (FVector(X, Y, Z) + 0.5) * VoxelSize;
	}

	int32 Index(int32 X, int32 Y, int32 Z)
	{
		return X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
	}

	/** Chunk (0, 0, 0) as the meshers see it: all-air procedural data (a pad over a pit) with the edits merged. */
	TArray<FVoxelData> Merged(const UVoxelEditManager& Manager)
	{
		TArray<FVoxelData> Data;
		Data.Init(FVoxelData::Air(), ChunkSize * ChunkSize * ChunkSize);
		Manager.ApplyEditsToVoxelData(FIntVector::ZeroValue, Data);
		return Data;
	}

	/** The player's dig (AVCPlayerController): a smooth sphere of 1.5 voxels taking 80 density at its centre. */
	FVoxelBrushParams DigBrush()
	{
		FVoxelBrushParams Brush;
		Brush.Shape = EVoxelBrushShape::Sphere;
		Brush.Radius = VoxelSize * 1.5f;
		Brush.Strength = 1.0f;
		Brush.FalloffType = EVoxelBrushFalloff::Smooth;
		Brush.DensityDelta = 80;
		return Brush;
	}

	/** A 12 x 12 x 3 stone slab written with Set, as POI pads are. */
	void WriteSlab(UVoxelEditManager& Manager)
	{
		Manager.SetEditSource(EEditSource::System);
		Manager.BeginEditOperation(TEXT("Pad"));
		for (int32 Z = 4; Z <= 6; ++Z)
		{
			for (int32 Y = 2; Y <= 13; ++Y)
			{
				for (int32 X = 2; X <= 13; ++X)
				{
					Manager.ApplyEdit(VoxelCentre(X, Y, Z), FVoxelData(2, 255), EEditMode::Set);
				}
			}
		}
		Manager.EndEditOperation();
		Manager.SetEditSource(EEditSource::Player);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEditSubtractComposesWithSetTest,
	"VoxelWorlds.Edit.Composition.SubtractComposesWithSet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelEditSubtractComposesWithSetTest::RunTest(const FString& Parameters)
{
	using namespace VoxelEditCompositionTestUtils;

	UVoxelEditManager* Manager = MakeManager();
	WriteSlab(*Manager);

	const FIntVector Target(8, 8, 6);
	const FIntVector Faces[5] = { {7, 8, 6}, {9, 8, 6}, {8, 7, 6}, {8, 9, 6}, {8, 8, 5} };

	// First dig: the aimed voxel loses 80, its face neighbours the falloff's 21. All of them stay solid, as a
	// procedural voxel at full density would.
	Manager->ApplyBrushEdit(VoxelCentre(Target.X, Target.Y, Target.Z), DigBrush(), EEditMode::Subtract);
	TArray<FVoxelData> Data = Merged(*Manager);
	TestEqual(TEXT("First dig: the aimed voxel keeps 255 - 80"), static_cast<int32>(Data[Index(Target.X, Target.Y, Target.Z)].Density), 175);
	TestEqual(TEXT("First dig: the aimed voxel keeps the slab's material"), static_cast<int32>(Data[Index(Target.X, Target.Y, Target.Z)].MaterialID), 2);
	for (const FIntVector& Face : Faces)
	{
		TestEqual(FString::Printf(TEXT("First dig: face neighbour (%d,%d,%d) keeps 255 - 21"), Face.X, Face.Y, Face.Z),
			static_cast<int32>(Data[Index(Face.X, Face.Y, Face.Z)].Density), 234);
	}

	// Second dig empties the aimed voxel and nothing else.
	Manager->ApplyBrushEdit(VoxelCentre(Target.X, Target.Y, Target.Z), DigBrush(), EEditMode::Subtract);
	Data = Merged(*Manager);
	TestTrue(TEXT("Second dig: the aimed voxel is air"), Data[Index(Target.X, Target.Y, Target.Z)].IsAir());
	int32 SlabAir = 0;
	for (int32 Z = 4; Z <= 6; ++Z)
	{
		for (int32 Y = 2; Y <= 13; ++Y)
		{
			for (int32 X = 2; X <= 13; ++X)
			{
				SlabAir += Data[Index(X, Y, Z)].IsAir() ? 1 : 0;
			}
		}
	}
	TestEqual(TEXT("Two digs remove one slab voxel"), SlabAir, 1);

	// Undo puts the slab voxel back as it was after the first dig.
	TestTrue(TEXT("Undo the second dig"), Manager->Undo());
	Data = Merged(*Manager);
	TestEqual(TEXT("Undone: the aimed voxel is back at 175"), static_cast<int32>(Data[Index(Target.X, Target.Y, Target.Z)].Density), 175);

	// Add and Paint compose too: a placed voxel on the slab stays the slab plus the brush.
	FVoxelBrushParams Paint;
	Paint.Shape = EVoxelBrushShape::Sphere;
	Paint.Radius = VoxelSize * 0.5f;
	Paint.MaterialID = 5;
	Manager->ApplyBrushEdit(VoxelCentre(3, 3, 6), Paint, EEditMode::Paint);
	Data = Merged(*Manager);
	TestEqual(TEXT("Paint on the slab: new material"), static_cast<int32>(Data[Index(3, 3, 6)].MaterialID), 5);
	TestEqual(TEXT("Paint on the slab: density kept"), static_cast<int32>(Data[Index(3, 3, 6)].Density), 255);

	// A Set over a dug Set voxel replaces it (absolute edits stay absolute).
	Manager->ApplyEdit(VoxelCentre(Target.X, Target.Y, Target.Z), FVoxelData(3, 255), EEditMode::Set);
	Data = Merged(*Manager);
	TestEqual(TEXT("Set over a dug voxel: replaced"), static_cast<int32>(Data[Index(Target.X, Target.Y, Target.Z)].MaterialID), 3);
	TestEqual(TEXT("Set over a dug voxel: full density"), static_cast<int32>(Data[Index(Target.X, Target.Y, Target.Z)].Density), 255);

	// Relative edits on procedural voxels still accumulate as before: two Subtracts of 30 make one of 60.
	Manager->ApplyBrushEdit(VoxelCentre(1, 1, 1), FVoxelBrushParams(), EEditMode::Subtract);
	const FVoxelEdit* Relative = Manager->GetEditLayer(FIntVector::ZeroValue)->GetEdit(FIntVector(1, 1, 1));
	TestTrue(TEXT("A Subtract on procedural data stays relative"), Relative && Relative->EditMode == EEditMode::Subtract);
	return true;
}

/**
 * The composed edits survive the edits file (UVoxelEditManager::SaveEditsToFile / LoadEditsFromFile, the format
 * VoxelWorldSave writes as <save>.edits.bin): a reloaded world shows the same dug pad.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEditCompositionSaveRoundTripTest,
	"VoxelWorlds.Edit.Composition.SaveRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelEditCompositionSaveRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace VoxelEditCompositionTestUtils;

	UVoxelEditManager* Manager = MakeManager();
	WriteSlab(*Manager);
	Manager->ApplyBrushEdit(VoxelCentre(8, 8, 6), DigBrush(), EEditMode::Subtract);
	Manager->ApplyBrushEdit(VoxelCentre(8, 8, 6), DigBrush(), EEditMode::Subtract);
	Manager->ApplyBrushEdit(VoxelCentre(4, 10, 5), DigBrush(), EEditMode::Subtract);
	const TArray<FVoxelData> Before = Merged(*Manager);

	const FString Path = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("VoxelEditComposition.edits.bin"));
	TestTrue(TEXT("Saved"), Manager->SaveEditsToFile(Path));

	UVoxelEditManager* Reloaded = MakeManager();
	TestTrue(TEXT("Loaded"), Reloaded->LoadEditsFromFile(Path));
	const TArray<FVoxelData> After = Merged(*Reloaded);
	IFileManager::Get().Delete(*Path);

	int32 Differences = 0;
	for (int32 i = 0; i < Before.Num(); ++i)
	{
		Differences += (Before[i].Density != After[i].Density || Before[i].MaterialID != After[i].MaterialID) ? 1 : 0;
	}
	TestEqual(TEXT("Voxels that differ after the round trip"), Differences, 0);
	TestTrue(TEXT("The dug voxel is still air"), After[Index(8, 8, 6)].IsAir());
	TestTrue(TEXT("Its face neighbour is still solid"), After[Index(9, 8, 6)].IsSolid());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
