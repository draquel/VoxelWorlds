// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelResidencyStats.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

namespace VoxelResidencyStats
{
	namespace
	{
		thread_local const TCHAR* GReason = nullptr;
		thread_local int32 GWaitDepth = 0;

		struct FReasonStats
		{
			int64 Uniform = 0;
			int64 Compressed = 0;
			int64 Waits = 0;
			double Ms = 0.0;
			double MaxMs = 0.0;
			TSet<FIntVector> Chunks;
		};

		FCriticalSection GLock;
		TMap<FString, FReasonStats> GByReason; // key: "<reason> (game thread|worker|game thread wait)"
		TSet<FIntVector> GAllChunks;            // every chunk expanded or waited for since the last reset
		int64 GAllCount = 0;
		int32 GFrameCount = 0;                  // game thread, since the last ConsumeGameThread
		double GFrameMs = 0.0;

		void Record(const TCHAR* Kind, bool bCompressed, bool bWait, double Ms, const FIntVector& ChunkCoord, bool bFrame)
		{
			const FString Key = FString::Printf(TEXT("%s (%s)"), GReason ? GReason : TEXT("other"), Kind);
			FScopeLock Lock(&GLock);
			FReasonStats& Stats = GByReason.FindOrAdd(Key);
			if (bWait)
			{
				++Stats.Waits;
			}
			else
			{
				(bCompressed ? Stats.Compressed : Stats.Uniform) += 1;
			}
			Stats.Ms += Ms;
			Stats.MaxMs = FMath::Max(Stats.MaxMs, Ms);
			Stats.Chunks.Add(ChunkCoord);
			GAllChunks.Add(ChunkCoord);
			++GAllCount;
			if (bFrame)
			{
				++GFrameCount;
				GFrameMs += Ms;
			}
		}
	}

	FReadScope::FReadScope(const TCHAR* InReason)
		: Previous(GReason)
	{
		// The outermost scope wins: an API that tags its own reads (a point query) is attributed to its caller when the
		// caller tags its own.
		if (!GReason)
		{
			GReason = InReason;
		}
	}

	FReadScope::~FReadScope()
	{
		GReason = Previous;
	}

	const TCHAR* GetReadReason()
	{
		return GReason;
	}

	void RecordExpand(bool bCompressed, uint64 Cycles, const FIntVector& ChunkCoord)
	{
		const bool bGameThread = IsInGameThread();
		// An expansion a game-thread wait ran inline is in that wait's time already.
		Record(bGameThread ? TEXT("game thread") : TEXT("worker"), bCompressed, false, FPlatformTime::ToMilliseconds64(Cycles),
			ChunkCoord, bGameThread && GWaitDepth == 0);
	}

	FGameThreadWaitScope::FGameThreadWaitScope()
	{
		++GWaitDepth;
	}

	FGameThreadWaitScope::~FGameThreadWaitScope()
	{
		--GWaitDepth;
	}

	void RecordWait(uint64 Cycles, const FIntVector& ChunkCoord)
	{
		Record(TEXT("game thread wait"), false, true, FPlatformTime::ToMilliseconds64(Cycles), ChunkCoord, IsInGameThread());
	}

	void ConsumeGameThread(int32& OutCount, float& OutMs)
	{
		FScopeLock Lock(&GLock);
		OutCount = GFrameCount;
		OutMs = static_cast<float>(GFrameMs);
		GFrameCount = 0;
		GFrameMs = 0.0;
	}

	FString Describe()
	{
		FScopeLock Lock(&GLock);
		TArray<FString> Keys;
		GByReason.GetKeys(Keys);
		Keys.Sort([](const FString& A, const FString& B) { return GByReason[A].Ms > GByReason[B].Ms; });
		FString Out;
		for (const FString& Key : Keys)
		{
			const FReasonStats& S = GByReason[Key];
			const int64 N = S.Uniform + S.Compressed + S.Waits;
			if (S.Waits > 0)
			{
				Out += FString::Printf(TEXT("%s: %lld waits, %d chunks, %.1f ms, mean %.3f, max %.2f\n"),
					*Key, S.Waits, S.Chunks.Num(), S.Ms, N > 0 ? S.Ms / N : 0.0, S.MaxMs);
			}
			else
			{
				Out += FString::Printf(TEXT("%s: %lld (uniform %lld, compressed %lld), %d chunks, %.1f ms, mean %.3f, max %.2f\n"),
					*Key, N, S.Uniform, S.Compressed, S.Chunks.Num(), S.Ms, N > 0 ? S.Ms / N : 0.0, S.MaxMs);
			}
		}
		if (GAllCount > 0)
		{
			Out += FString::Printf(TEXT("total: %lld expansions and waits, %d distinct chunks\n"), GAllCount, GAllChunks.Num());
		}
		return Out.IsEmpty() ? FString(TEXT("no expansions\n")) : Out;
	}

	void Reset()
	{
		FScopeLock Lock(&GLock);
		GByReason.Reset();
		GAllChunks.Reset();
		GAllCount = 0;
	}
}
