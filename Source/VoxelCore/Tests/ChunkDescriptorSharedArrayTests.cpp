// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "ChunkDescriptor.h"
#include "VoxelResidencyStats.h"
#include "Tasks/Task.h"
#include "HAL/PlatformProcess.h"
#include <atomic>

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G71 (WORLD_MODES_REVIEW_PLAN.md §14.1): a resident chunk's voxel array is shared copy-on-write with the readers that
// used to copy it (a seam snapshot, a cook's, the water pull's; the far-compression encode). A reader's array must never
// change under it, and the chunk must see its own writes:
//
// - Compression.SharedArray.CopyOnWrite: every write path (GetVoxelDataMutable, SetVoxel, SetVoxelByIndex) clones a shared
//   array and leaves the reader's as it was; a write to an unshared array clones nothing; installing new data, clearing,
//   collapsing and compressing drop only the chunk's reference; a copy of the descriptor shares the array the same way.
// - Compression.SharedArray.WorkerReads: a worker reading a shared array while the game thread writes the chunk reads the
//   voxels as they were when it took the array.
// ---------------------------------------------------------------------------

namespace ChunkDescriptorSharedArrayTestUtils
{
	/** A resident chunk of 16^3 voxels with a pattern (no two neighbours alike, not uniform). */
	FChunkDescriptor MakeResident(int32 ChunkSize = 16)
	{
		FChunkDescriptor D(FIntVector(2, -1, 3), ChunkSize);
		TArray<FVoxelData> Data;
		Data.SetNumUninitialized(ChunkSize * ChunkSize * ChunkSize);
		for (int32 Index = 0; Index < Data.Num(); ++Index)
		{
			Data[Index] = FVoxelData(static_cast<uint8>(Index % 7), static_cast<uint8>((Index * 37) & 0xFF), static_cast<uint8>(Index % 3), 0);
		}
		D.SetResidentVoxelData(MoveTemp(Data));
		return D;
	}

	uint32 Crc(const TArray<FVoxelData>& Data)
	{
		return FCrc::MemCrc32(Data.GetData(), Data.Num() * sizeof(FVoxelData));
	}

	int64 Clones()
	{
		int64 Count = 0;
		double Ms = 0.0;
		VoxelResidencyStats::GetSharedArrayClones(Count, Ms);
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChunkDescriptorSharedArrayCopyOnWriteTest,
	"VoxelWorlds.Compression.SharedArray.CopyOnWrite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FChunkDescriptorSharedArrayCopyOnWriteTest::RunTest(const FString& Parameters)
{
	using namespace ChunkDescriptorSharedArrayTestUtils;
	VoxelResidencyStats::ResetSharedArrayClones();

	// Sharing hands out the chunk's own array, no copy.
	{
		FChunkDescriptor D = MakeResident();
		const uint32 Before = Crc(D.GetResidentArray());
		TSharedPtr<const TArray<FVoxelData>> Reader = D.ShareVoxelData();
		TestTrue(TEXT("the reader shares the chunk's array"), Reader.IsValid() && D.SharesVoxelArray(Reader.Get()));
		TestTrue(TEXT("the same memory"), Reader.IsValid() && Reader->GetData() == D.GetResidentArray().GetData());

		// A write while shared clones: the reader's voxels stay, the chunk sees its write.
		const int64 ClonesBefore = Clones();
		TArray<FVoxelData>& Mutable = D.GetVoxelDataMutable();
		Mutable[5].SetWaterFlag(true);
		TestEqual(TEXT("GetVoxelDataMutable on a shared array clones it once"), Clones() - ClonesBefore, static_cast<int64>(1));
		TestFalse(TEXT("the chunk no longer shares the reader's array"), D.SharesVoxelArray(Reader.Get()));
		TestEqual(TEXT("the reader's voxels are as they were"), Crc(*Reader), Before);
		TestTrue(TEXT("the chunk sees its write"), D.GetVoxelByIndex(5).HasWaterFlag());
		TestFalse(TEXT("the reader does not"), (*Reader)[5].HasWaterFlag());

		// The chunk's array is its own now: a second write clones nothing.
		const TArray<FVoxelData>* Own = &D.GetResidentArray();
		D.GetVoxelDataMutable()[6].SetWaterFlag(true);
		TestEqual(TEXT("a write to an unshared array clones nothing"), Clones() - ClonesBefore, static_cast<int64>(1));
		TestTrue(TEXT("and keeps the array"), &D.GetResidentArray() == Own);
	}

	// SetVoxel and SetVoxelByIndex clone a shared array too.
	{
		FChunkDescriptor D = MakeResident();
		const uint32 Before = Crc(D.GetResidentArray());
		TSharedPtr<const TArray<FVoxelData>> Reader = D.ShareVoxelData();
		const int64 ClonesBefore = Clones();
		D.SetVoxel(FIntVector(1, 2, 3), FVoxelData::Solid(9, 1));
		TestEqual(TEXT("SetVoxel on a shared array clones it"), Clones() - ClonesBefore, static_cast<int64>(1));
		TestEqual(TEXT("SetVoxel leaves the reader's voxels"), Crc(*Reader), Before);
		TestTrue(TEXT("SetVoxel's voxel is the chunk's"), D.GetVoxel(FIntVector(1, 2, 3)) == FVoxelData::Solid(9, 1));

		TSharedPtr<const TArray<FVoxelData>> Second = D.ShareVoxelData();
		const uint32 SecondBefore = Crc(*Second);
		D.SetVoxelByIndex(11, FVoxelData::Solid(8, 2));
		TestEqual(TEXT("SetVoxelByIndex on a shared array clones it"), Clones() - ClonesBefore, static_cast<int64>(2));
		TestEqual(TEXT("SetVoxelByIndex leaves the reader's voxels"), Crc(*Second), SecondBefore);
		TestTrue(TEXT("SetVoxelByIndex's voxel is the chunk's"), D.GetVoxelByIndex(11) == FVoxelData::Solid(8, 2));
		TestEqual(TEXT("the first reader is still as it was"), Crc(*Reader), Before);
	}

	// New data, clearing, collapsing and compressing drop only the chunk's reference.
	{
		FChunkDescriptor D = MakeResident();
		const uint32 Before = Crc(D.GetResidentArray());
		TSharedPtr<const TArray<FVoxelData>> Reader = D.ShareVoxelData();
		TArray<FVoxelData> Fresh;
		Fresh.Init(FVoxelData::Solid(4, 1), D.GetTotalVoxels());
		D.SetResidentVoxelData(MoveTemp(Fresh));
		TestEqual(TEXT("new data: the reader keeps the old voxels"), Crc(*Reader), Before);
		TestTrue(TEXT("new data: the chunk holds the new"), D.GetVoxelByIndex(0) == FVoxelData::Solid(4, 1));

		TSharedPtr<const TArray<FVoxelData>> Uniform = D.ShareVoxelData();
		TestTrue(TEXT("a uniform chunk collapses while shared"), D.TryCollapseUniform());
		TestFalse(TEXT("collapsed: no resident array"), D.IsVoxelDataResident());
		TestEqual(TEXT("collapsed: the reader keeps every voxel"), Uniform->Num(), D.GetTotalVoxels());
		TestTrue(TEXT("collapsed: and their value"), (*Uniform)[123] == FVoxelData::Solid(4, 1));
		D.EnsureResident();
		TestTrue(TEXT("expanded: a new array"), !D.SharesVoxelArray(Uniform.Get()) && Crc(D.GetResidentArray()) == Crc(*Uniform));

		D.ClearVoxelData();
		TestEqual(TEXT("cleared: the reader keeps the voxels"), Crc(*Reader), Before);
		TestFalse(TEXT("cleared: sharing hands out nothing"), D.ShareVoxelData().IsValid());
	}
	{
		FChunkDescriptor D = MakeResident(32);
		const uint32 Before = Crc(D.GetResidentArray());
		TSharedPtr<const TArray<FVoxelData>> Reader = D.ShareVoxelData();
		TestTrue(TEXT("a patterned chunk compresses while shared"), D.TryCompress(EVoxelChunkCodec::Oodle));
		TestEqual(TEXT("compressed"), static_cast<int32>(D.Residency), static_cast<int32>(EVoxelDataResidency::Compressed));
		TestEqual(TEXT("compressed: the reader keeps the voxels"), Crc(*Reader), Before);
		const TArray<FVoxelData>& Expanded = D.EnsureResident();
		TestEqual(TEXT("expanded again: the same voxels in a new array"), Crc(Expanded), Before);
		TestFalse(TEXT("not the reader's array"), D.SharesVoxelArray(Reader.Get()));
	}

	// A copy of the descriptor shares the array; a write to either clones and leaves the other.
	{
		FChunkDescriptor D = MakeResident();
		const uint32 Before = Crc(D.GetResidentArray());
		FChunkDescriptor Copy = D;
		TestTrue(TEXT("a copy shares the array"), Copy.GetResidentArray().GetData() == D.GetResidentArray().GetData());
		Copy.SetVoxelByIndex(0, FVoxelData::Solid(1, 1));
		TestEqual(TEXT("writing the copy leaves the original"), Crc(D.GetResidentArray()), Before);
		TestTrue(TEXT("the copy sees its write"), Copy.GetVoxelByIndex(0) == FVoxelData::Solid(1, 1));
	}

	// The memory a chunk reports is its array either way.
	{
		FChunkDescriptor D = MakeResident();
		const SIZE_T Usage = D.GetMemoryUsage();
		TSharedPtr<const TArray<FVoxelData>> Reader = D.ShareVoxelData();
		TestEqual(TEXT("sharing does not change the chunk's memory usage"), static_cast<int64>(D.GetMemoryUsage()), static_cast<int64>(Usage));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChunkDescriptorSharedArrayWorkerReadsTest,
	"VoxelWorlds.Compression.SharedArray.WorkerReads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FChunkDescriptorSharedArrayWorkerReadsTest::RunTest(const FString& Parameters)
{
	using namespace ChunkDescriptorSharedArrayTestUtils;

	// Eight workers each take the array and read it over and over while the game thread writes the chunk 200 times: every
	// read gives the voxels as they were when that worker took them.
	FChunkDescriptor D = MakeResident(32);
	const uint32 Taken = Crc(D.GetResidentArray());
	TArray<UE::Tasks::FTask> Workers;
	TArray<TArray<uint32>> Reads;
	Reads.SetNum(8);
	std::atomic<bool> bStop{ false };
	for (int32 W = 0; W < 8; ++W)
	{
		TSharedPtr<const TArray<FVoxelData>> Reader = D.ShareVoxelData();
		Workers.Add(UE::Tasks::Launch(UE_SOURCE_LOCATION, [Reader, &Out = Reads[W], &bStop]()
		{
			while (!bStop.load() || Out.Num() < 3)
			{
				Out.Add(FCrc::MemCrc32(Reader->GetData(), Reader->Num() * sizeof(FVoxelData)));
			}
		}));
	}
	for (int32 Write = 0; Write < 200; ++Write)
	{
		D.GetVoxelDataMutable()[Write * 97 % D.GetTotalVoxels()].SetWaterFlag(Write % 2 == 0);
		D.SetVoxelByIndex((Write * 31) % D.GetTotalVoxels(), FVoxelData::Solid(static_cast<uint8>(Write % 200), 1));
		if (Write % 50 == 0)
		{
			FPlatformProcess::Sleep(0.001f);
		}
	}
	bStop.store(true);
	UE::Tasks::Wait(Workers);

	int32 ReadsDone = 0;
	int32 Different = 0;
	for (const TArray<uint32>& Out : Reads)
	{
		for (const uint32 Read : Out)
		{
			++ReadsDone;
			Different += (Read != Taken) ? 1 : 0;
		}
	}
	AddInfo(FString::Printf(TEXT("%d reads by 8 workers during 400 writes"), ReadsDone));
	TestTrue(TEXT("the workers read"), ReadsDone >= 24);
	TestEqual(TEXT("every read gives the voxels as they were taken"), Different, 0);
	TestTrue(TEXT("and the chunk holds its writes"), Crc(D.GetResidentArray()) != Taken);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
