// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "IVoxelEditValidator.h"
#include "VoxelEditManager.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G291: where a chunk's worth of Set voxels spends its time in the edit manager. A planned dungeon stamp writes a chunk a
// tick (G280), every voxel through UVoxelEditManager::ApplyEdit, and VoxelDemo's densest chunk was a tick of 10 to 11.5 ms.
// VoxelWorlds.Edit.ChunkWrite.Cost writes every voxel of one 32^3 chunk as a Set, in an operation outside the history with
// the Local class (the stamp's), with a validator that passes System edits (the POI claims'), and logs the time of the whole
// write and of each step ApplyEdit takes per voxel, each repeated alone on containers of the same kinds: the validator's
// call, the world-to-chunk conversion, the chunk's layer lookup, the Local rule's lookup, the voxel's edit lookup and its
// insert (into an empty layer, as the stamp's, with and without the layer reserved), the 112-byte change record and the
// operation's per-voxel bounds; then the same chunk in one call (UVoxelEditManager::ApplySetVoxelsInChunk), with no change
// record (the stamp's) and with one. It checks nothing about time (a log for the measurement); it checks the counts.
// ---------------------------------------------------------------------------

namespace VoxelEditChunkWriteCostTest
{
	constexpr int32 ChunkSize = 32;
	constexpr float VoxelSize = 100.0f;
	constexpr int32 Repeats = 5;

	/** A validator that passes every edit but a player's, as the POI claims' does: its cost is the virtual call. */
	class FPassSystemValidator final : public IVoxelEditValidator
	{
	public:
		virtual bool CanApplyEdit(const FVector& WorldPos, EEditSource Source) const override
		{
			return Source != EEditSource::Player || WorldPos.Z > 1.0e9;
		}
	};

	UVoxelEditManager* MakeManager(const IVoxelEditValidator* Validator)
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->ChunkSize = ChunkSize;
		Config->VoxelSize = VoxelSize;
		Config->WorldOrigin = FVector(-1713.0, 942.0, -2611.0);
		UVoxelEditManager* Manager = NewObject<UVoxelEditManager>();
		Manager->Initialize(Config);
		Manager->SetEditValidator(Validator);
		return Manager;
	}

	/** The least of Repeats runs of Body, in microseconds. */
	template <typename FBody>
	double LeastMicros(FBody&& Body)
	{
		double Least = TNumericLimits<double>::Max();
		for (int32 Run = 0; Run < Repeats; ++Run)
		{
			const double Start = FPlatformTime::Seconds();
			Body();
			Least = FMath::Min(Least, (FPlatformTime::Seconds() - Start) * 1.0e6);
		}
		return Least;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEditChunkWriteCostTest, "VoxelWorlds.Edit.ChunkWrite.Cost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelEditChunkWriteCostTest::RunTest(const FString& Parameters)
{
	using namespace VoxelEditChunkWriteCostTest;
	const FPassSystemValidator Validator;
	const int32 Count = ChunkSize * ChunkSize * ChunkSize;
	const FIntVector Chunk(3, -2, 1);
	const FVector Origin(-1713.0, 942.0, -2611.0);

	TArray<FVector> Positions;
	TArray<FIntVector> Locals;
	Positions.Reserve(Count);
	Locals.Reserve(Count);
	for (int32 Z = 0; Z < ChunkSize; ++Z)
	{
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				Locals.Add(FIntVector(X, Y, Z));
				Positions.Add(Origin + (FVector(Chunk * ChunkSize + FIntVector(X, Y, Z)) + FVector(0.5)) * VoxelSize);
			}
		}
	}
	FVoxelData Stone;
	Stone.Density = 255;
	Stone.MaterialID = 3;

	// The whole write, as ApplyPlannedChunks makes it: one operation outside the history, Local, System.
	int32 Written = 0;
	const double Whole = LeastMicros([&]()
	{
		UVoxelEditManager* Manager = MakeManager(&Validator);
		Manager->SetEditReplication(EVoxelEditReplication::Local);
		Manager->BeginEditOperation(TEXT("Cost"), /*bAddToUndoHistory=*/false);
		Manager->SetEditSource(EEditSource::System);
		Written = 0;
		for (const FVector& Position : Positions)
		{
			Written += Manager->ApplyEdit(Position, Stone, EEditMode::Set) ? 1 : 0;
		}
		Manager->EndEditOperation();
	});
	TestEqual(TEXT("Every voxel of the chunk written"), Written, Count);

	// The same chunk in one call (G291), the stamp's way (no change record) and keeping the record.
	TArray<FVoxelChunkSetVoxel> Voxels;
	Voxels.Reserve(Count);
	for (const FIntVector& Local : Locals)
	{
		Voxels.Add({ Local, Stone });
	}
	auto BatchMicros = [&](bool bKeepRecord, int32& OutTaken)
	{
		return LeastMicros([&]()
		{
			UVoxelEditManager* Manager = MakeManager(&Validator);
			Manager->SetEditReplication(EVoxelEditReplication::Local);
			Manager->BeginEditOperation(TEXT("Cost"), /*bAddToUndoHistory=*/false);
			Manager->SetEditSource(EEditSource::System);
			OutTaken = Manager->ApplySetVoxelsInChunk(Chunk, Voxels, bKeepRecord);
			Manager->EndEditOperation();
		});
	};
	int32 BatchTaken = 0;
	int32 BatchRecordTaken = 0;
	const double Batch = BatchMicros(false, BatchTaken);
	const double BatchRecord = BatchMicros(true, BatchRecordTaken);
	TestEqual(TEXT("Every voxel of the chunk taken in one call"), BatchTaken, Count);
	TestEqual(TEXT("... and keeping the record"), BatchRecordTaken, Count);

	// Each step alone, on containers of the kinds the edit manager holds.
	volatile int32 Sink = 0;
	const double ValidatorCall = LeastMicros([&]()
	{
		const IVoxelEditValidator* V = &Validator;
		int32 Passed = 0;
		for (const FVector& Position : Positions)
		{
			Passed += V->CanApplyEdit(Position, EEditSource::System) ? 1 : 0;
		}
		Sink = Passed;
	});
	const double Conversion = LeastMicros([&]()
	{
		int32 Sum = 0;
		const float ChunkWorldSize = ChunkSize * VoxelSize;
		for (const FVector& Position : Positions)
		{
			const FVector Relative = Position - Origin;
			const FIntVector C(FMath::FloorToInt(Relative.X / ChunkWorldSize), FMath::FloorToInt(Relative.Y / ChunkWorldSize), FMath::FloorToInt(Relative.Z / ChunkWorldSize));
			const FVector Offset = Relative - FVector(C) * ChunkWorldSize;
			const FIntVector L(FMath::FloorToInt(Offset.X / VoxelSize), FMath::FloorToInt(Offset.Y / VoxelSize), FMath::FloorToInt(Offset.Z / VoxelSize));
			Sum += C.X + L.X + L.Y + L.Z;
		}
		Sink = Sum;
	});
	TMap<FIntVector, FChunkEditLayer> Layers;
	for (int32 I = 0; I < 12; ++I)
	{
		Layers.Add(Chunk + FIntVector(I, 0, 0), FChunkEditLayer(Chunk + FIntVector(I, 0, 0), ChunkSize));
	}
	const double LayerLookup = LeastMicros([&]()
	{
		int32 Found = 0;
		for (int32 I = 0; I < Count; ++I)
		{
			Found += Layers.Find(Chunk) ? 1 : 0;
		}
		Sink = Found;
	});
	TSet<int32> Marks;
	const double LocalRule = LeastMicros([&]()
	{
		int32 Found = 0;
		for (int32 I = 0; I < Count; ++I)
		{
			Found += Marks.Contains(I) ? 1 : 0;
		}
		Sink = Found;
	});
	auto MakeEdit = [&](int32 I) { return FVoxelEdit(Locals[I], Stone, EEditMode::Set); };
	const double Insert = LeastMicros([&]()
	{
		FChunkEditLayer Layer(Chunk, ChunkSize);
		for (int32 I = 0; I < Count; ++I)
		{
			Layer.ApplyEdit(MakeEdit(I));
		}
		Sink = Layer.GetEditCount();
	});
	const double InsertReserved = LeastMicros([&]()
	{
		FChunkEditLayer Layer(Chunk, ChunkSize);
		Layer.Edits.Reserve(Count);
		for (int32 I = 0; I < Count; ++I)
		{
			Layer.ApplyEdit(MakeEdit(I));
		}
		Sink = Layer.GetEditCount();
	});
	FChunkEditLayer Full(Chunk, ChunkSize);
	for (int32 I = 0; I < Count; ++I)
	{
		Full.ApplyEdit(MakeEdit(I));
	}
	const double EditLookup = LeastMicros([&]()
	{
		int32 Found = 0;
		for (int32 I = 0; I < Count; ++I)
		{
			Found += Full.GetEdit(Locals[I]) ? 1 : 0;
		}
		Sink = Found;
	});
	const double Record = LeastMicros([&]()
	{
		FVoxelEditOperation Operation(1, TEXT("Cost"));
		for (int32 I = 0; I < Count; ++I)
		{
			const FVoxelEdit Edit = MakeEdit(I);
			Operation.RecordChange(Chunk, Locals[I], nullptr, &Edit);
		}
		Sink = Operation.GetEditCount();
	});
	const double Bounds = LeastMicros([&]()
	{
		TMap<FIntVector, FVoxelEditedVoxelBounds> PerChunk;
		for (int32 I = 0; I < Count; ++I)
		{
			PerChunk.FindOrAdd(Chunk).Add(Locals[I]);
		}
		Sink = PerChunk.Num();
	});

	const double Steps = ValidatorCall + Conversion + LayerLookup + LocalRule + EditLookup + Insert + Record + Bounds;
	const FString Line = FString::Printf(
		TEXT("[ChunkWriteCost] voxels=%d whole=%.0fus (%.3fus/voxel) batch=%.0fus (%.3fus/voxel, x%.1f) batchrecord=%.0fus | ")
		TEXT("validator=%.0f conversion=%.0f layer=%.0f localrule=%.0f ")
		TEXT("editlookup=%.0f insert=%.0f insertreserved=%.0f record=%.0f bounds=%.0f steps=%.0f rest=%.0f"),
		Count, Whole, Whole / Count, Batch, Batch / Count, Whole / FMath::Max(Batch, 1.0), BatchRecord,
		ValidatorCall, Conversion, LayerLookup, LocalRule, EditLookup, Insert, InsertReserved,
		Record, Bounds, Steps, Whole - Steps);
	AddInfo(Line);
	UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
