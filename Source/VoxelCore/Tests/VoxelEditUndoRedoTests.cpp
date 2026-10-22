// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelEditManager.h"
#include "VoxelWorldConfiguration.h"
#include "HAL/IConsoleManager.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G60 (WORLD_MODES_REVIEW_PLAN.md section 14.1, section 19.17). Undo and redo restore the edit layer exactly. An
// operation's undo record kept each voxel's edit and the previous voxel value (FVoxelEdit::OriginalData), but not the
// chunk the voxel is in nor the edit it replaced: undo took the first of the operation's chunks holding an edit at the
// voxel's local index, redo wrote every edit into the operation's first chunk, and undo set the previous value on the
// edit there (removing it when the value was air), so a voxel's previous relative edit, a previous Set to air and an edit
// removed by cancelling deltas did not come back. Each case compares every edit of every chunk (mode, delta, material,
// new voxel) with the state it should restore:
//
// - A brush across a chunk face: undo removes it from both chunks, redo puts it back in both.
// - A dig over a dig on generated voxels (two relative edits accumulate): undo of the second leaves the first.
// - A Set to air under a Set to stone: undo of the second leaves the Set to air.
// - A fill cancelling a dig (the deltas sum to 0, so the edit is removed): undo brings the dig back.
// - An operation outside the history (water propagation's, BeginEditOperation with bAddToUndoHistory false): it is
//   neither undone nor clears the redo stack; undo of an earlier operation restores that operation's voxels as they were
//   before it and leaves the rest of the unrecorded operation's voxels.
// - Cancelling an operation restores the layer as it was before it.
// ---------------------------------------------------------------------------

namespace VoxelEditUndoRedoTestUtils
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

	/** Centre of the voxel with global index (X, Y, Z) (chunk (0, 0, 0) holds 0..15 on each axis). */
	FVector VoxelCentre(int32 X, int32 Y, int32 Z)
	{
		return (FVector(X, Y, Z) + 0.5) * VoxelSize;
	}

	/** What an edit does to its voxel at merge time (not when or from what it was made). */
	struct FEditSignature
	{
		uint8 Mode = 0;
		int32 Delta = 0;
		uint8 Material = 0;
		uint32 NewData = 0;

		bool operator==(const FEditSignature& Other) const
		{
			return Mode == Other.Mode && Delta == Other.Delta && Material == Other.Material && NewData == Other.NewData;
		}
	};

	/** Every edit of every chunk with edits, keyed by chunk and voxel index. */
	using FLayerSnapshot = TMap<TPair<FIntVector, int32>, FEditSignature>;

	FLayerSnapshot Snapshot(const UVoxelEditManager& Manager)
	{
		FLayerSnapshot Out;
		TArray<FIntVector> Chunks;
		Manager.GetEditedChunkCoords(Chunks);
		for (const FIntVector& Chunk : Chunks)
		{
			const FChunkEditLayer* Layer = Manager.GetEditLayer(Chunk);
			for (const TPair<int32, FVoxelEdit>& Pair : Layer->Edits)
			{
				FEditSignature Sig;
				Sig.Mode = static_cast<uint8>(Pair.Value.EditMode);
				Sig.Delta = Pair.Value.DensityDelta;
				Sig.Material = Pair.Value.BrushMaterialID;
				Sig.NewData = Pair.Value.NewData.Pack();
				Out.Add(TPair<FIntVector, int32>(Chunk, Pair.Key), Sig);
			}
		}
		return Out;
	}

	/** Edits that differ between two snapshots: missing from one, extra in it, or different. */
	int32 Differences(const FLayerSnapshot& Expected, const FLayerSnapshot& Actual)
	{
		int32 Count = 0;
		for (const TPair<TPair<FIntVector, int32>, FEditSignature>& Pair : Expected)
		{
			const FEditSignature* Found = Actual.Find(Pair.Key);
			Count += (!Found || !(*Found == Pair.Value)) ? 1 : 0;
		}
		for (const TPair<TPair<FIntVector, int32>, FEditSignature>& Pair : Actual)
		{
			Count += Expected.Contains(Pair.Key) ? 0 : 1;
		}
		return Count;
	}

	/** Chunks holding at least one edit in a snapshot. */
	int32 ChunksIn(const FLayerSnapshot& Snap)
	{
		TSet<FIntVector> Chunks;
		for (const TPair<TPair<FIntVector, int32>, FEditSignature>& Pair : Snap)
		{
			Chunks.Add(Pair.Key.Key);
		}
		return Chunks.Num();
	}

	FVoxelBrushParams Brush(float RadiusVoxels, uint8 Delta, uint8 Material, EVoxelBrushFalloff Falloff = EVoxelBrushFalloff::Smooth)
	{
		FVoxelBrushParams B;
		B.Shape = EVoxelBrushShape::Sphere;
		B.Radius = RadiusVoxels * VoxelSize;
		B.Strength = 1.0f;
		B.FalloffType = Falloff;
		B.DensityDelta = Delta;
		B.MaterialID = Material;
		return B;
	}

	void SystemSet(UVoxelEditManager& Manager, const TArray<FIntVector>& Keys, const FVoxelData& Voxel, const TCHAR* Name)
	{
		Manager.SetEditSource(EEditSource::System);
		Manager.BeginEditOperation(Name);
		for (const FIntVector& Key : Keys)
		{
			Manager.ApplyEdit(VoxelCentre(Key.X, Key.Y, Key.Z), Voxel, EEditMode::Set);
		}
		Manager.EndEditOperation();
		Manager.SetEditSource(EEditSource::Player);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEditUndoRedoRecordsTest,
	"VoxelWorlds.Edit.UndoRedo.Records",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelEditUndoRedoRecordsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelEditUndoRedoTestUtils;

	// ---- A brush across a chunk face: undo removes it from both chunks, redo puts it back in both. ----
	{
		UVoxelEditManager* Manager = MakeManager();
		const FLayerSnapshot Before = Snapshot(*Manager);
		const int32 Modified = Manager->ApplyBrushEdit(VoxelCentre(ChunkSize, 8, 8), Brush(2.5f, 255, 0), EEditMode::Subtract);
		const FLayerSnapshot After = Snapshot(*Manager);
		TestEqual(TEXT("Across a face: the brush reaches two chunks"), ChunksIn(After), 2);
		TestTrue(TEXT("Undo the brush"), Manager->Undo());
		const FLayerSnapshot Undone = Snapshot(*Manager);
		TestTrue(TEXT("Redo the brush"), Manager->Redo());
		const FLayerSnapshot Redone = Snapshot(*Manager);
		TestEqual(TEXT("Across a face: edits that differ from before the brush after its undo"), Differences(Before, Undone), 0);
		TestEqual(TEXT("Across a face: edits that differ from after the brush after its redo"), Differences(After, Redone), 0);
		TestEqual(TEXT("Across a face: chunks with edits after the redo"), ChunksIn(Redone), 2);
		AddInfo(FString::Printf(TEXT("Across a face: %d voxels in %d chunks; after undo %d differ, after redo %d differ (%d chunks)"),
			Modified, ChunksIn(After), Differences(Before, Undone), Differences(After, Redone), ChunksIn(Redone)));
	}

	// ---- A dig over a dig on generated voxels: undo of the second leaves the first. ----
	{
		UVoxelEditManager* Manager = MakeManager();
		Manager->ApplyBrushEdit(VoxelCentre(8, 8, 8), Brush(1.5f, 80, 0), EEditMode::Subtract);
		const FLayerSnapshot First = Snapshot(*Manager);
		Manager->ApplyBrushEdit(VoxelCentre(8, 8, 8), Brush(1.5f, 80, 0), EEditMode::Subtract);
		const FLayerSnapshot Second = Snapshot(*Manager);
		TestTrue(TEXT("Dig over a dig: the second dig changed edits"), Differences(First, Second) > 0);
		TestTrue(TEXT("Undo the second dig"), Manager->Undo());
		const FLayerSnapshot Undone = Snapshot(*Manager);
		TestEqual(TEXT("Dig over a dig: edits that differ from the first dig after undoing the second"), Differences(First, Undone), 0);
		TestTrue(TEXT("Redo the second dig"), Manager->Redo());
		TestEqual(TEXT("Dig over a dig: edits that differ from the second dig after its redo"), Differences(Second, Snapshot(*Manager)), 0);
		AddInfo(FString::Printf(TEXT("Dig over a dig: %d edits after the first, undo of the second leaves %d differing"),
			First.Num(), Differences(First, Undone)));
	}

	// ---- A Set to air under a Set to stone: undo of the second leaves the Set to air. ----
	{
		UVoxelEditManager* Manager = MakeManager();
		const TArray<FIntVector> Keys = { FIntVector(3, 3, 3), FIntVector(4, 3, 3) };
		SystemSet(*Manager, Keys, FVoxelData::Air(), TEXT("Carve"));
		const FLayerSnapshot Carved = Snapshot(*Manager);
		SystemSet(*Manager, Keys, FVoxelData::Solid(2), TEXT("Wall"));
		TestTrue(TEXT("Undo the wall"), Manager->Undo());
		const FLayerSnapshot Undone = Snapshot(*Manager);
		TestEqual(TEXT("Set over a Set to air: edits that differ from the carve after undoing the wall"), Differences(Carved, Undone), 0);
		AddInfo(FString::Printf(TEXT("Set over a Set to air: %d carved, %d edits left after undoing the wall (%d differ)"),
			Carved.Num(), Undone.Num(), Differences(Carved, Undone)));
	}

	// ---- A fill cancelling a dig (its edit removed): undo brings the dig back. ----
	{
		UVoxelEditManager* Manager = MakeManager();
		Manager->ApplyBrushEdit(VoxelCentre(8, 8, 8), Brush(0.5f, 80, 0), EEditMode::Subtract);
		const FLayerSnapshot Dug = Snapshot(*Manager);
		Manager->ApplyBrushEdit(VoxelCentre(8, 8, 8), Brush(0.5f, 80, 0), EEditMode::Add);
		const FLayerSnapshot Cancelled = Snapshot(*Manager);
		TestEqual(TEXT("Cancelling fill: no edit left"), Cancelled.Num(), 0);
		TestTrue(TEXT("Undo the fill"), Manager->Undo());
		const FLayerSnapshot Undone = Snapshot(*Manager);
		TestEqual(TEXT("Cancelling fill: edits that differ from the dig after undoing the fill"), Differences(Dug, Undone), 0);
		TestTrue(TEXT("Redo the fill"), Manager->Redo());
		TestEqual(TEXT("Cancelling fill: edits left after its redo"), Snapshot(*Manager).Num(), 0);
		AddInfo(FString::Printf(TEXT("Cancelling fill: dig %d edits, %d after the fill, %d after its undo (%d differ)"),
			Dug.Num(), Cancelled.Num(), Undone.Num(), Differences(Dug, Undone)));
	}

	// ---- An operation outside the history (water propagation's): no undo step, the redo stack kept; undo of an earlier
	// operation restores that operation's voxels, the unrecorded edits there included, and leaves its other voxels. ----
	{
		UVoxelEditManager* Manager = MakeManager();
		Manager->ApplyBrushEdit(VoxelCentre(8, 8, 8), Brush(1.5f, 255, 0), EEditMode::Subtract); // A
		const FLayerSnapshot Dug = Snapshot(*Manager);
		Manager->ApplyBrushEdit(VoxelCentre(3, 3, 3), Brush(0.5f, 80, 0), EEditMode::Subtract); // B, undone: on the redo stack
		TestTrue(TEXT("Undo B"), Manager->Undo());
		const int32 UndoSteps = Manager->GetUndoCount();

		// The flood: water-flagged air at A's centre and at a voxel A did not touch.
		FVoxelData Water = FVoxelData::Air();
		Water.SetWaterFlag(true);
		Manager->SetEditSource(EEditSource::System);
		Manager->BeginEditOperation(TEXT("Flood"), /*bAddToUndoHistory=*/false);
		Manager->ApplyEdit(VoxelCentre(8, 8, 8), Water, EEditMode::Set);
		Manager->ApplyEdit(VoxelCentre(8, 8, 11), Water, EEditMode::Set);
		Manager->EndEditOperation();
		Manager->SetEditSource(EEditSource::Player);
		TestEqual(TEXT("Outside the history: undo steps unchanged"), Manager->GetUndoCount(), UndoSteps);
		TestTrue(TEXT("Outside the history: the redo stack kept"), Manager->CanRedo());
		const FVoxelEdit* Flooded = Manager->GetEditLayer(FIntVector::ZeroValue)->GetEdit(FIntVector(8, 8, 8));
		TestTrue(TEXT("Outside the history: the flood replaced the dig at its centre"), Flooded && Flooded->EditMode == EEditMode::Set && Flooded->NewData.HasWaterFlag());

		// Undo A: its voxels as before it (no edit), the flood's edit at its centre with them; the flood's other voxel stays.
		TestTrue(TEXT("Undo A"), Manager->Undo());
		const FLayerSnapshot Undone = Snapshot(*Manager);
		const FVoxelEdit* Kept = Manager->GetEditLayer(FIntVector::ZeroValue)->GetEdit(FIntVector(8, 8, 11));
		TestEqual(TEXT("Outside the history: edits left after undoing A"), Undone.Num(), 1);
		TestTrue(TEXT("Outside the history: the flood's voxel A did not touch keeps its water"), Kept && Kept->NewData.HasWaterFlag());

		// Redo A: the dig as it left it (the flood would come again from the propagation, not from the history), then B.
		TestTrue(TEXT("Redo A"), Manager->Redo());
		FLayerSnapshot Redone = Snapshot(*Manager);
		Redone.Remove(TPair<FIntVector, int32>(FIntVector::ZeroValue, 8 + 8 * ChunkSize + 11 * ChunkSize * ChunkSize));
		TestEqual(TEXT("Outside the history: edits that differ from the dig after its redo (but the flood's other voxel)"), Differences(Dug, Redone), 0);
		TestTrue(TEXT("Redo B"), Manager->Redo());
		TestFalse(TEXT("Outside the history: nothing left to redo"), Manager->CanRedo());
	}

	// ---- Cancelling an operation restores the layer as it was before it. ----
	{
		UVoxelEditManager* Manager = MakeManager();
		Manager->ApplyBrushEdit(VoxelCentre(ChunkSize, 8, 8), Brush(2.5f, 80, 0), EEditMode::Subtract);
		SystemSet(*Manager, { FIntVector(ChunkSize - 1, 2, 2) }, FVoxelData::Air(), TEXT("Carve"));
		const FLayerSnapshot Before = Snapshot(*Manager);
		Manager->BeginEditOperation(TEXT("To cancel"));
		Manager->ApplyBrushEdit(VoxelCentre(ChunkSize, 8, 8), Brush(2.5f, 80, 0), EEditMode::Subtract);
		Manager->ApplyEdit(VoxelCentre(ChunkSize - 1, 2, 2), FVoxelData::Solid(3), EEditMode::Set);
		Manager->ApplyEdit(VoxelCentre(ChunkSize + 2, 2, 2), FVoxelData::Solid(3), EEditMode::Set);
		Manager->CancelEditOperation();
		TestEqual(TEXT("Cancel: edits that differ from before the operation"), Differences(Before, Snapshot(*Manager)), 0);
	}

	return true;
}

// ---------------------------------------------------------------------------
// G68 (WORLD_MODES_REVIEW_PLAN.md section 14.1): the history's memory budget (voxel.Edit.UndoHistoryMB). Each operation
// records 112 bytes per voxel it changed, so the history was bounded only by its 100-operation count. Over the budget the
// oldest operations are dropped and the newest is always kept; what is kept undoes and redoes exactly; 0 = no budget.
// ---------------------------------------------------------------------------

namespace VoxelEditUndoRedoTestUtils
{
	/** Sets voxel.Edit.UndoHistoryMB at console priority for a scope. */
	struct FScopedUndoHistoryMB
	{
		IConsoleVariable* Var = nullptr;
		float Previous = 32.0f;

		explicit FScopedUndoHistoryMB(float Value)
		{
			Var = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Edit.UndoHistoryMB"));
			if (Var)
			{
				Previous = Var->GetFloat();
				Var->Set(Value, ECVF_SetByConsole);
			}
		}

		~FScopedUndoHistoryMB()
		{
			if (Var)
			{
				Var->Set(Previous, ECVF_SetByConsole);
			}
		}
	};

	/** Twelve digs of radius 3 voxels a few voxels apart (about 110 voxels and 13 KB of history each). */
	void DigTwelve(UVoxelEditManager& Manager, TArray<FLayerSnapshot>* OutAfterEach = nullptr)
	{
		for (int32 i = 0; i < 12; ++i)
		{
			Manager.ApplyBrushEdit(VoxelCentre(4 + 7 * (i % 4), 4 + 7 * ((i / 4) % 3), 8), Brush(3.0f, 80, 0), EEditMode::Subtract);
			if (OutAfterEach)
			{
				OutAfterEach->Add(Snapshot(Manager));
			}
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEditUndoHistoryBudgetTest,
	"VoxelWorlds.Edit.UndoRedo.MemoryBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelEditUndoHistoryBudgetTest::RunTest(const FString& Parameters)
{
	using namespace VoxelEditUndoRedoTestUtils;

	// ---- No budget: the twelve digs are all kept. ----
	int64 TwelveBytes = 0;
	{
		FScopedUndoHistoryMB Scoped(0.0f);
		UVoxelEditManager* Manager = MakeManager();
		DigTwelve(*Manager);
		TwelveBytes = Manager->GetHistoryMemoryUsage();
		TestEqual(TEXT("No budget: twelve undo steps"), Manager->GetUndoCount(), 12);
		AddInfo(FString::Printf(TEXT("Twelve digs: %d edits, %lld bytes of history"), Manager->GetTotalEditCount(), TwelveBytes));
	}

	// ---- A budget of about a third of that: the oldest are dropped, the history within it, the rest undoes exactly. ----
	{
		const float BudgetMB = static_cast<float>(TwelveBytes / 3) / (1024.0f * 1024.0f);
		FScopedUndoHistoryMB Scoped(BudgetMB);
		UVoxelEditManager* Manager = MakeManager();
		TArray<FLayerSnapshot> AfterEach;
		DigTwelve(*Manager, &AfterEach);
		const int32 Kept = Manager->GetUndoCount();
		const int64 Bytes = Manager->GetHistoryMemoryUsage();
		AddInfo(FString::Printf(TEXT("Budget %.4f MB: %d of 12 operations kept in %lld bytes"), BudgetMB, Kept, Bytes));
		TestTrue(TEXT("Budget: the oldest operations dropped, some kept"), Kept >= 2 && Kept < 12);
		TestTrue(TEXT("Budget: the history within it"), Bytes <= static_cast<int64>(BudgetMB * 1024.0 * 1024.0));
		// Undo every kept operation: the layer comes back to what it was after each earlier dig, in turn.
		int32 Mismatched = 0;
		for (int32 Step = 0; Step < Kept; ++Step)
		{
			TestTrue(TEXT("Budget: undo a kept operation"), Manager->Undo());
			Mismatched += Differences(AfterEach[12 - 2 - Step], Snapshot(*Manager)) > 0 ? 1 : 0;
		}
		TestFalse(TEXT("Budget: nothing older to undo"), Manager->CanUndo());
		TestEqual(TEXT("Budget: every undo restores the layer after the dig before it"), Mismatched, 0);
		// And redo them all: the layer as after the twelfth dig.
		while (Manager->Redo())
		{
		}
		TestEqual(TEXT("Budget: redo restores the twelfth dig's layer"), Differences(AfterEach.Last(), Snapshot(*Manager)), 0);
	}

	// ---- A budget smaller than one operation: the newest is still kept, alone. ----
	{
		FScopedUndoHistoryMB Scoped(0.001f);
		UVoxelEditManager* Manager = MakeManager();
		DigTwelve(*Manager);
		TestEqual(TEXT("Tiny budget: the newest operation kept"), Manager->GetUndoCount(), 1);
		TestTrue(TEXT("Tiny budget: it undoes"), Manager->Undo());
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
