// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Where far-compressed chunk data is expanded back to a raw array, on which thread, and what it costs (world modes review
 * G65, §20.13).
 *
 * Far-chunk compression keeps a settled far chunk's voxels in a compact form (one uniform value, or an Oodle buffer). A read
 * of the raw array expands the chunk in place (FChunkDescriptor::EnsureResident, game thread); since G65 most readers read
 * a snapshot that a worker task decodes from the compact form, and the chunk stays compressed. Readers tag themselves
 * with an FReadScope so the expansions can be told apart by what asked for them: `voxel.FarCompression.Decodes` lists them,
 * and the streaming bench records the game thread's share per sample.
 */
namespace VoxelResidencyStats
{
	/**
	 * Tags the expansions on this thread while it lives with Reason (a string literal); the outermost scope wins, so a
	 * caller's tag covers the APIs it calls. Untagged expansions count as "other".
	 */
	struct VOXELCORE_API FReadScope
	{
		explicit FReadScope(const TCHAR* InReason);
		~FReadScope();

		FReadScope(const FReadScope&) = delete;
		FReadScope& operator=(const FReadScope&) = delete;

	private:
		const TCHAR* Previous;
	};

	/** The reason the current thread's reads are tagged with (nullptr when untagged), to hand to a worker's FReadScope. */
	VOXELCORE_API const TCHAR* GetReadReason();

	/**
	 * Records one expansion of a compact form.
	 *
	 * @param bCompressed True for a codec buffer, false for a uniform value
	 * @param Cycles      How long it took (FPlatformTime::Cycles64 units)
	 * @param ChunkCoord  The chunk expanded (distinct chunks are counted)
	 */
	VOXELCORE_API void RecordExpand(bool bCompressed, uint64 Cycles, const FIntVector& ChunkCoord);

	/**
	 * While it lives, the game thread is waiting for a worker's expansion (an unfinished snapshot decode). An expansion the
	 * wait runs inline is listed but not added to the game thread's per-sample time a second time; RecordWait adds the wait.
	 */
	struct VOXELCORE_API FGameThreadWaitScope
	{
		FGameThreadWaitScope();
		~FGameThreadWaitScope();

		FGameThreadWaitScope(const FGameThreadWaitScope&) = delete;
		FGameThreadWaitScope& operator=(const FGameThreadWaitScope&) = delete;
	};

	/**
	 * Records a game-thread wait for a worker's expansion: listed per reason, and added to the game thread's per-sample
	 * time (what the bench's DecodeMs reports).
	 *
	 * @param Cycles     How long the game thread waited (FPlatformTime::Cycles64 units)
	 * @param ChunkCoord The chunk waited for
	 */
	VOXELCORE_API void RecordWait(uint64 Cycles, const FIntVector& ChunkCoord);

	/**
	 * The game thread's expansion and wait time since the last call: the streaming bench's per-sample columns.
	 *
	 * @param OutCount Expansions and waits
	 * @param OutMs    Their total time (ms)
	 */
	VOXELCORE_API void ConsumeGameThread(int32& OutCount, float& OutMs);

	/**
	 * One line per reason since the last reset: count (uniform / compressed, or waits), distinct chunks, total, mean and
	 * largest time, per thread kind; and a total line with the distinct chunks over every reason.
	 */
	VOXELCORE_API FString Describe();

	/** Clears the per-reason statistics (not the bench's running counters). */
	VOXELCORE_API void Reset();
}
