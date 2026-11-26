// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "IVoxelEditValidator.h"
#include "VoxelEditManager.h"
#include "VoxelWorldConfiguration.h"
#include "Testing/VoxelTestScopedCVar.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G291: UVoxelEditManager::ApplySetVoxelsInChunk, a chunk's Set voxels in one call (a planned dungeon stamp's chunk),
// against the same voxels through ApplyEdit one at a time at each voxel's centre, in one operation.
//
// - VoxelWorlds.Edit.ChunkWrite.SameAsPerVoxel: on a layer that already holds Sets and relative edits and voxels a
//   replicated operation marked, with a position given twice and a validator that refuses a box of the chunk, for the
//   stamp's operation (outside the history, Local, no change record), the same with the record, a replicated operation
//   outside the history and one in the history; and with voxel.Edit.ChunkSetWrite 0 (the A/B's per-voxel path). Each
//   pair ends with the same edits file (every chunk's edits in the same order and its marked voxels), the same count
//   returned and refused, the same OnChunkEdited broadcasts (chunk, source, centre, radius, the bounds), the same
//   replicated changes (revisions, each voxel's edit) and revisions; the undoable one undoes and redoes to the same layers;
//   the stamp's operation holds no change record and the one that keeps it holds one per write.
// - VoxelWorlds.Edit.ChunkWrite.Refusals: an empty call, a call whose voxels the validator all refuses (no layer made, no
//   broadcast, no undo step), a position outside the chunk (refused with a warning, nothing written in the neighbour), a
//   chunk no layer holds yet far from any other, an uninitialized manager, a call with no open operation (one undo step,
//   undone to nothing), and the cancel of an operation that kept no record (an error; the voxels stay) and of one that
//   did (put back).
// ---------------------------------------------------------------------------

namespace VoxelEditChunkWriteTest
{
	constexpr int32 ChunkSize = 16;
	constexpr float VoxelSize = 100.0f;
	const FVector Origin(-713.0, 342.0, -611.0);

	UVoxelEditManager* MakeManager()
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->ChunkSize = ChunkSize;
		Config->VoxelSize = VoxelSize;
		Config->WorldOrigin = Origin;
		UVoxelEditManager* Manager = NewObject<UVoxelEditManager>();
		Manager->Initialize(Config);
		return Manager;
	}

	/** A voxel's centre in the world. */
	FVector Centre(const FIntVector& Chunk, const FIntVector& Local)
	{
		return Origin + (FVector(Chunk * ChunkSize + Local) + FVector(0.5)) * VoxelSize;
	}

	/** Refuses every voxel of a box of chunk-local positions in one chunk, whatever the source. */
	class FBoxValidator final : public IVoxelEditValidator
	{
	public:
		FBox Box;
		virtual bool CanApplyEdit(const FVector& WorldPos, EEditSource Source) const override
		{
			return !Box.IsInsideOrOn(WorldPos);
		}
	};

	/** What a broadcast said. */
	struct FBroadcast
	{
		FIntVector Chunk;
		EEditSource Source;
		FVector Centre;
		float Radius;
		bool bBounds;
		FIntVector Min;
		FIntVector Max;

		FString ToString() const
		{
			return FString::Printf(TEXT("%s src=%d c=%s r=%.1f b=%d %s..%s"), *Chunk.ToString(), static_cast<int32>(Source),
				*Centre.ToString(), Radius, bBounds ? 1 : 0, *Min.ToString(), *Max.ToString());
		}
	};

	/** Every broadcast and replicated change a manager makes, as text. */
	struct FListener
	{
		TArray<FString> Broadcasts;
		TArray<FString> Changes;

		void Bind(UVoxelEditManager* Manager)
		{
			Manager->OnChunkEdited.AddLambda([this, Manager](const FIntVector& Chunk, EEditSource Source, const FVector& Centre, float Radius)
			{
				FBroadcast B{ Chunk, Source, Centre, Radius, false, FIntVector::ZeroValue, FIntVector::ZeroValue };
				B.bBounds = Manager->GetBroadcastEditBounds(B.Min, B.Max);
				Broadcasts.Add(B.ToString());
			});
			Manager->OnReplicatedChange.AddLambda([this](const FVoxelReplicatedChange& Change)
			{
				FString Text = FString::Printf(TEXT("src=%d r=%.1f"), static_cast<int32>(Change.Source), Change.EditRadius);
				for (const FVoxelReplicatedChunkChange& Chunk : Change.Chunks)
				{
					Text += FString::Printf(TEXT(" | %s %u->%u whole=%d"), *Chunk.Wire.Coord.ToString(), Chunk.PreviousRevision, Chunk.Revision,
						Chunk.bResendWhole ? 1 : 0);
					TArray<FVoxelEditWireVoxel> Voxels = Chunk.Wire.Voxels;
					Voxels.Sort([](const FVoxelEditWireVoxel& A, const FVoxelEditWireVoxel& B) { return A.Index < B.Index; });
					for (const FVoxelEditWireVoxel& V : Voxels)
					{
						Text += FString::Printf(TEXT(" %d:%d:%d:%u"), V.Index, V.bHasEdit ? 1 : 0, static_cast<int32>(V.Edit.EditMode), V.Edit.NewData.Pack());
					}
				}
				Changes.Add(Text);
			});
		}
	};

	/** The manager's edits file, every chunk's edits in their order and its marked voxels. */
	TArray<uint8> SaveBytes(UVoxelEditManager* Manager, const TCHAR* Name)
	{
		const FString Path = FPaths::Combine(FPaths::AutomationTransientDir(), FString::Printf(TEXT("ChunkWrite_%s.vedits"), Name));
		TArray<uint8> Bytes;
		if (Manager->SaveEditsToFile(Path))
		{
			FFileHelper::LoadFileToArray(Bytes, *Path);
			IFileManager::Get().Delete(*Path);
		}
		return Bytes;
	}

	/** The voxels a test writes into Chunk: a block of it in a scattered order, one position twice, a stone and an air. */
	TArray<FVoxelChunkSetVoxel> MakeVoxels(int32 Seed)
	{
		TArray<FVoxelChunkSetVoxel> Out;
		FRandomStream Random(Seed);
		for (int32 Z = 2; Z < 14; ++Z)
		{
			for (int32 Y = 0; Y < ChunkSize; ++Y)
			{
				for (int32 X = 0; X < ChunkSize; ++X)
				{
					if (Random.FRand() < 0.6f)
					{
						FVoxelChunkSetVoxel& V = Out.AddDefaulted_GetRef();
						V.LocalPosition = FIntVector(X, Y, Z);
						V.Voxel = Random.FRand() < 0.5f ? FVoxelData::Air() : FVoxelData::Solid(static_cast<uint8>(1 + Random.RandRange(0, 5)), 2);
					}
				}
			}
		}
		for (int32 I = Out.Num() - 1; I > 0; --I)
		{
			Out.Swap(I, Random.RandRange(0, I));
		}
		FVoxelChunkSetVoxel Twice = Out[7];
		Twice.Voxel = FVoxelData::Solid(9, 1);
		Out.Add(Twice);
		return Out;
	}

	/**
	 * The state a write starts on: in the chunk a Set, a dig and a fill (relative edits) on voxels the write covers, a
	 * replicated dig's marked voxels, and an edit in a neighbour chunk; the edits made outside the history.
	 */
	void Prepare(UVoxelEditManager* Manager, const FIntVector& Chunk)
	{
		Manager->SetEditSource(EEditSource::Player);
		Manager->SetEditReplication(EVoxelEditReplication::Replicated);
		Manager->BeginEditOperation(TEXT("Prepare"), /*bAddToUndoHistory=*/false);
		for (int32 X = 0; X < 6; ++X)
		{
			Manager->ApplyEdit(Centre(Chunk, FIntVector(X, 3, 4)), FVoxelData::Solid(4, 0), EEditMode::Set);
		}
		FVoxelBrushParams Dig;
		Dig.Radius = 260.0f;
		Dig.DensityDelta = 90;
		Manager->ApplyBrushEdit(Centre(Chunk, FIntVector(9, 9, 8)), Dig, EEditMode::Subtract);
		FVoxelBrushParams Fill = Dig;
		Fill.Radius = 140.0f;
		Fill.MaterialID = 5;
		Manager->ApplyBrushEdit(Centre(Chunk, FIntVector(4, 11, 11)), Fill, EEditMode::Add);
		Manager->ApplyEdit(Centre(Chunk + FIntVector(1, 0, 0), FIntVector(2, 2, 2)), FVoxelData::Solid(2, 0), EEditMode::Set);
		Manager->EndEditOperation();
	}

	/** How a write's operation is made. */
	struct FCase
	{
		const TCHAR* Name;
		EVoxelEditReplication Replication;
		bool bUndoHistory;
		bool bKeepRecord;
		bool bPerVoxelCVar;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEditChunkWriteSameTest, "VoxelWorlds.Edit.ChunkWrite.SameAsPerVoxel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelEditChunkWriteSameTest::RunTest(const FString& Parameters)
{
	using namespace VoxelEditChunkWriteTest;
	const FIntVector Chunk(2, -1, 0);
	const TArray<FVoxelChunkSetVoxel> Voxels = MakeVoxels(291);
	FBoxValidator Validator;
	Validator.Box = FBox(Centre(Chunk, FIntVector(10, 0, 2)) - FVector(10.0), Centre(Chunk, FIntVector(15, 4, 6)) + FVector(10.0));

	const FCase Cases[] = {
		{ TEXT("the stamp's (Local, outside the history, no record)"), EVoxelEditReplication::Local, false, false, false },
		{ TEXT("Local, outside the history, the record kept"), EVoxelEditReplication::Local, false, true, false },
		{ TEXT("replicated, outside the history"), EVoxelEditReplication::Replicated, false, false, false },
		{ TEXT("replicated, in the history"), EVoxelEditReplication::Replicated, true, false, false },
		{ TEXT("the stamp's with voxel.Edit.ChunkSetWrite 0"), EVoxelEditReplication::Local, false, false, true },
	};
	for (const FCase& Case : Cases)
	{
		const FString What = Case.Name;
		UVoxelEditManager* PerVoxel = MakeManager();
		UVoxelEditManager* Batch = MakeManager();
		FListener PerVoxelHeard;
		FListener BatchHeard;
		for (UVoxelEditManager* M : { PerVoxel, Batch })
		{
			Prepare(M, Chunk);
			M->SetEditValidator(&Validator);
		}
		PerVoxelHeard.Bind(PerVoxel);
		BatchHeard.Bind(Batch);

		// The same operation on both, the voxels one at a time on one and in one call on the other.
		int32 PerVoxelTaken = 0;
		int32 PerVoxelRefused = 0;
		{
			PerVoxel->SetEditSource(EEditSource::System);
			PerVoxel->SetEditReplication(Case.Replication);
			PerVoxel->BeginEditOperation(TEXT("Write"), Case.bUndoHistory);
			for (const FVoxelChunkSetVoxel& V : Voxels)
			{
				PerVoxelTaken += PerVoxel->ApplyEdit(Centre(Chunk, V.LocalPosition), V.Voxel, EEditMode::Set) ? 1 : 0;
				PerVoxelRefused += PerVoxel->GetLastRejectedEditCount();
			}
			PerVoxel->EndEditOperation();
		}
		int32 BatchTaken = 0;
		int32 BatchRefused = 0;
		{
			TOptional<VoxelTesting::FScopedCVar> PerVoxelPath;
			if (Case.bPerVoxelCVar)
			{
				PerVoxelPath.Emplace(TEXT("voxel.Edit.ChunkSetWrite"), 0);
			}
			Batch->SetEditSource(EEditSource::System);
			Batch->SetEditReplication(Case.Replication);
			Batch->BeginEditOperation(TEXT("Write"), Case.bUndoHistory);
			BatchTaken = Batch->ApplySetVoxelsInChunk(Chunk, Voxels, Case.bKeepRecord);
			BatchRefused = Batch->GetLastRejectedEditCount();
			Batch->EndEditOperation();
		}

		TestTrue(What + TEXT(": the validator refused some voxels (non-vacuous)"), PerVoxelRefused > 0);
		TestEqual(What + TEXT(": the voxels taken"), BatchTaken, PerVoxelTaken);
		TestEqual(What + TEXT(": the voxels refused"), BatchRefused, PerVoxelRefused);
		const TArray<uint8> PerVoxelBytes = SaveBytes(PerVoxel, TEXT("PerVoxel"));
		const TArray<uint8> BatchBytes = SaveBytes(Batch, TEXT("Batch"));
		TestTrue(What + TEXT(": the edits file is written (non-vacuous)"), PerVoxelBytes.Num() > 1000);
		TestTrue(What + TEXT(": the same edits file (every edit in the same order, the same marked voxels)"), PerVoxelBytes == BatchBytes);
		TestEqual(What + TEXT(": the same edits"), Batch->GetTotalEditCount(), PerVoxel->GetTotalEditCount());
		TestEqual(What + TEXT(": the same broadcasts"), FString::Join(BatchHeard.Broadcasts, TEXT("\n")), FString::Join(PerVoxelHeard.Broadcasts, TEXT("\n")));
		TestEqual(What + TEXT(": one broadcast"), BatchHeard.Broadcasts.Num(), 1);
		TestEqual(What + TEXT(": the same replicated changes"), FString::Join(BatchHeard.Changes, TEXT("\n")), FString::Join(PerVoxelHeard.Changes, TEXT("\n")));
		TestEqual(What + TEXT(": a replicated change only for a replicated operation"), BatchHeard.Changes.Num(),
			Case.Replication == EVoxelEditReplication::Replicated ? 1 : 0);
		TestEqual(What + TEXT(": the same revision"), Batch->GetReplicatedRevision(Chunk), PerVoxel->GetReplicatedRevision(Chunk));
		TestEqual(What + TEXT(": the same undo steps"), Batch->GetUndoCount(), PerVoxel->GetUndoCount());
		const bool bDropsRecord = !Case.bKeepRecord && !Case.bUndoHistory && Case.Replication == EVoxelEditReplication::Local && !Case.bPerVoxelCVar;
		AddInfo(FString::Printf(TEXT("%s: %d voxels given, %d taken, %d refused, record %s"), *What, Voxels.Num(), BatchTaken, BatchRefused,
			bDropsRecord ? TEXT("dropped") : TEXT("kept")));

		if (Case.bUndoHistory)
		{
			TestTrue(What + TEXT(": undo"), PerVoxel->Undo() && Batch->Undo());
			TestTrue(What + TEXT(": undone to the same layers"), SaveBytes(PerVoxel, TEXT("PerVoxelUndo")) == SaveBytes(Batch, TEXT("BatchUndo")));
			TestTrue(What + TEXT(": redo"), PerVoxel->Redo() && Batch->Redo());
			TestTrue(What + TEXT(": redone to the same layers"), SaveBytes(PerVoxel, TEXT("PerVoxelRedo")) == SaveBytes(Batch, TEXT("BatchRedo")));
			TestEqual(What + TEXT(": the same broadcasts through undo and redo"), FString::Join(BatchHeard.Broadcasts, TEXT("\n")),
				FString::Join(PerVoxelHeard.Broadcasts, TEXT("\n")));
			TestEqual(What + TEXT(": the same replicated changes through undo and redo"), FString::Join(BatchHeard.Changes, TEXT("\n")),
				FString::Join(PerVoxelHeard.Changes, TEXT("\n")));
		}
		PerVoxel->SetEditValidator(nullptr);
		Batch->SetEditValidator(nullptr);
	}

	// What the stamp's operation keeps: a count and no record; the same write keeping it holds one change per write.
	{
		UVoxelEditManager* Manager = MakeManager();
		Manager->SetEditReplication(EVoxelEditReplication::Local);
		Manager->SetEditSource(EEditSource::System);
		Manager->BeginEditOperation(TEXT("Stamp"), /*bAddToUndoHistory=*/false);
		const int32 Taken = Manager->ApplySetVoxelsInChunk(Chunk, Voxels, /*bKeepChangeRecord=*/false);
		TestEqual(TEXT("The stamp's operation: every voxel taken"), Taken, Voxels.Num());
		TestEqual(TEXT("The stamp's operation: no history memory, no undo"), Manager->GetHistoryMemoryUsage(), static_cast<int64>(0));
		const SIZE_T NoRecord = Manager->GetMemoryUsage();
		Manager->EndEditOperation();
		TestEqual(TEXT("... and its voxels written (one position twice)"), Manager->GetTotalEditCount(), Voxels.Num() - 1);

		UVoxelEditManager* Keeping = MakeManager();
		Keeping->SetEditReplication(EVoxelEditReplication::Local);
		Keeping->BeginEditOperation(TEXT("Stamp"), /*bAddToUndoHistory=*/false);
		Keeping->ApplySetVoxelsInChunk(Chunk, Voxels, /*bKeepChangeRecord=*/true);
		const SIZE_T WithRecord = Keeping->GetMemoryUsage();
		Keeping->EndEditOperation();
		TestTrue(TEXT("The record kept costs about 112 bytes a write the stamp's operation does not hold"),
			WithRecord >= NoRecord + static_cast<SIZE_T>(Voxels.Num()) * sizeof(FVoxelEditChange));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEditChunkWriteRefusalsTest, "VoxelWorlds.Edit.ChunkWrite.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelEditChunkWriteRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelEditChunkWriteTest;
	const FIntVector Chunk(0, 0, 0);
	const TArray<FVoxelChunkSetVoxel> Voxels = MakeVoxels(7);

	// An empty call.
	{
		UVoxelEditManager* Manager = MakeManager();
		FListener Heard;
		Heard.Bind(Manager);
		TestEqual(TEXT("Empty: nothing taken"), Manager->ApplySetVoxelsInChunk(Chunk, TConstArrayView<FVoxelChunkSetVoxel>()), 0);
		TestFalse(TEXT("Empty: no operation left open"), Manager->IsEditOperationInProgress());
		TestEqual(TEXT("Empty: no layer"), Manager->GetEditedChunkCount(), 0);
		TestEqual(TEXT("Empty: no broadcast"), Heard.Broadcasts.Num(), 0);
		TestFalse(TEXT("Empty: no undo step"), Manager->CanUndo());
	}

	// Every voxel refused by the validator.
	{
		UVoxelEditManager* Manager = MakeManager();
		FBoxValidator Validator;
		Validator.Box = FBox(Centre(Chunk, FIntVector(0)) - FVector(100.0), Centre(Chunk, FIntVector(ChunkSize - 1)) + FVector(100.0));
		Manager->SetEditValidator(&Validator);
		FListener Heard;
		Heard.Bind(Manager);
		Manager->BeginEditOperation(TEXT("Refused"));
		TestEqual(TEXT("Refused: nothing taken"), Manager->ApplySetVoxelsInChunk(Chunk, Voxels), 0);
		TestEqual(TEXT("Refused: every voxel counted"), Manager->GetLastRejectedEditCount(), Voxels.Num());
		Manager->EndEditOperation();
		TestEqual(TEXT("Refused: no layer made (the edits file counts every layer)"), Manager->GetEditedChunkCount(), 0);
		TestEqual(TEXT("Refused: no broadcast"), Heard.Broadcasts.Num(), 0);
		TestFalse(TEXT("Refused: no undo step"), Manager->CanUndo());
		Manager->SetEditValidator(nullptr);
	}

	// A position outside the chunk: refused, never written in the neighbour (on both paths).
	for (const int32 PathValue : { 1, 0 })
	{
		VoxelTesting::FScopedCVar Path(TEXT("voxel.Edit.ChunkSetWrite"), PathValue);
		UVoxelEditManager* Manager = MakeManager();
		TArray<FVoxelChunkSetVoxel> Outside;
		Outside.Add({ FIntVector(ChunkSize, 3, 3), FVoxelData::Solid(1, 0) });
		Outside.Add({ FIntVector(-1, 3, 3), FVoxelData::Solid(1, 0) });
		Outside.Add({ FIntVector(2, 3, 3), FVoxelData::Solid(1, 0) });
		AddExpectedMessage(TEXT("outside chunk"), EAutomationExpectedMessageFlags::Contains, 1);
		const int32 Taken = Manager->ApplySetVoxelsInChunk(Chunk, Outside);
		TestEqual(FString::Printf(TEXT("Outside (path %d): the one inside taken"), PathValue), Taken, 1);
		TestEqual(FString::Printf(TEXT("Outside (path %d): one edit"), PathValue), Manager->GetTotalEditCount(), 1);
		TestFalse(FString::Printf(TEXT("Outside (path %d): nothing in the +X neighbour"), PathValue), Manager->ChunkHasEdits(FIntVector(1, 0, 0)));
		TestFalse(FString::Printf(TEXT("Outside (path %d): nothing in the -X neighbour"), PathValue), Manager->ChunkHasEdits(FIntVector(-1, 0, 0)));
	}

	// A chunk no layer holds, far from any other (the edit manager writes any chunk, loaded or not: the chunk manager
	// merges the layer when the chunk generates).
	{
		UVoxelEditManager* Manager = MakeManager();
		const FIntVector Far(-4000, 2500, 90);
		Manager->BeginEditOperation(TEXT("Far"), false);
		TestEqual(TEXT("Far: every voxel taken"), Manager->ApplySetVoxelsInChunk(Far, Voxels, false), Voxels.Num());
		Manager->EndEditOperation();
		TestTrue(TEXT("Far: its layer holds them"), Manager->ChunkHasEdits(Far) && Manager->GetTotalEditCount() == Voxels.Num() - 1);
		const FChunkEditLayer* Layer = Manager->GetEditLayer(Far);
		const FVoxelChunkSetVoxel& Last = Voxels.Last();
		const FVoxelEdit* Edit = Layer ? Layer->GetEdit(Last.LocalPosition) : nullptr;
		TestTrue(TEXT("Far: a position given twice holds its last voxel"), Edit && Edit->NewData == Last.Voxel && Edit->EditMode == EEditMode::Set);
	}

	// An uninitialized manager.
	{
		UVoxelEditManager* Manager = NewObject<UVoxelEditManager>();
		TestEqual(TEXT("Uninitialized: nothing taken"), Manager->ApplySetVoxelsInChunk(Chunk, Voxels), 0);
	}

	// No open operation: one, in the history, for the call.
	{
		UVoxelEditManager* Manager = MakeManager();
		FListener Heard;
		Heard.Bind(Manager);
		Manager->ApplySetVoxelsInChunk(Chunk, Voxels, /*bKeepChangeRecord=*/false);
		TestFalse(TEXT("Alone: no operation left open"), Manager->IsEditOperationInProgress());
		TestEqual(TEXT("Alone: one undo step (the record kept for it)"), Manager->GetUndoCount(), 1);
		TestEqual(TEXT("Alone: one broadcast"), Heard.Broadcasts.Num(), 1);
		TestTrue(TEXT("Alone: undone"), Manager->Undo());
		TestEqual(TEXT("Alone: undone to nothing"), Manager->GetTotalEditCount(), 0);
	}

	// A cancel: with no record the voxels stay (an error says so); with one they go.
	{
		UVoxelEditManager* Manager = MakeManager();
		Manager->SetEditReplication(EVoxelEditReplication::Local);
		Manager->BeginEditOperation(TEXT("Stamp"), false);
		Manager->ApplySetVoxelsInChunk(Chunk, Voxels, /*bKeepChangeRecord=*/false);
		AddExpectedError(TEXT("made without a record"), EAutomationExpectedErrorFlags::Contains, 1);
		Manager->CancelEditOperation();
		TestEqual(TEXT("Cancel with no record: the voxels stay"), Manager->GetTotalEditCount(), Voxels.Num() - 1);

		UVoxelEditManager* Keeping = MakeManager();
		Keeping->SetEditReplication(EVoxelEditReplication::Local);
		Keeping->BeginEditOperation(TEXT("Stamp"), false);
		Keeping->ApplySetVoxelsInChunk(Chunk, Voxels, /*bKeepChangeRecord=*/true);
		Keeping->CancelEditOperation();
		TestEqual(TEXT("Cancel with the record: the voxels go"), Keeping->GetTotalEditCount(), 0);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
