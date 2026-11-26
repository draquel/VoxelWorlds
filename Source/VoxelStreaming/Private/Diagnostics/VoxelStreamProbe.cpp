// Copyright Daniel Raquel. All Rights Reserved.

// The streaming probe (voxel.Stream.Probe) and its synthetic worker load (voxel.Stream.ProbeLoad): G275.

#include "VoxelStreamProbe.h"
#include "VoxelChunkManager.h"
#include "VoxelStreaming.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformAffinity.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include <atomic>

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/WindowsHWrapper.h"
#include <intrin.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
	/** The cycles this thread has spent on a processor (the time stamp counter's units); 0 where the platform has no such count. */
	uint64 ThreadCycles()
	{
#if PLATFORM_WINDOWS
		ULONG64 Cycles = 0;
		::QueryThreadCycleTime(::GetCurrentThread(), &Cycles);
		return Cycles;
#else
		return 0;
#endif
	}

	uint64 TimeStampCounter()
	{
#if PLATFORM_WINDOWS
		return __rdtsc();
#else
		return 0;
#endif
	}

	int32 CurrentProcessor()
	{
#if PLATFORM_WINDOWS
		return static_cast<int32>(::GetCurrentProcessorNumber());
#else
		return -1;
#endif
	}

	/** Arithmetic on a register: no memory but the stack, so only the core's own speed shows in it. */
	FORCENOINLINE uint64 ComputeWork(int32 Iterations)
	{
		uint64 X = 0x9E3779B97F4A7C15ull;
		for (int32 Index = 0; Index < Iterations; ++Index)
		{
			X ^= X << 13;
			X ^= X >> 7;
			X ^= X << 17;
			X += Index;
		}
		return X;
	}

	/** Dependent reads scattered over a buffer far larger than the caches: each one a cache and a TLB miss. */
	FORCENOINLINE uint32 MemoryWork(const TArray<uint32>& Buffer, int32 Reads, uint32 Seed)
	{
		const uint32 Mask = static_cast<uint32>(Buffer.Num() - 1);
		uint32 Index = Seed & Mask;
		for (int32 Read = 0; Read < Reads; ++Read)
		{
			Index = (Index * 1664525u + 1013904223u + Buffer[Index]) & Mask;
		}
		return Index;
	}

	/** The allocator as a job's result arriving on the game thread meets it: one large block written to, and small ones. */
	FORCENOINLINE void AllocWork()
	{
		void* Large = FMemory::Malloc(1024 * 1024);
		FMemory::Memset(Large, 1, 1024 * 1024);
		void* Small[64];
		for (void*& Block : Small)
		{
			Block = FMemory::Malloc(512);
		}
		for (void* Block : Small)
		{
			FMemory::Free(Block);
		}
		FMemory::Free(Large);
	}

	struct FProbeRow
	{
		double Time = 0.0;
		double FrameSeconds = 0.0;
		uint64 FrameCycles = 0;
		double TickSeconds = 0.0;
		uint64 TickCycles = 0;
		double ComputeSeconds = 0.0;
		uint64 ComputeCycles = 0;
		double MemorySeconds = 0.0;
		uint64 MemoryCycles = 0;
		double AllocSeconds = 0.0;
		double ParallelSeconds = 0.0;
		int32 Processor = -1;
		int32 ProcessorAfter = -1;
		int32 JobsInFlight = 0;
		int32 LoadJobs = 0;
		UVoxelChunkManager::FVoxelTimingStats Timing;
	};

	struct FProbeState
	{
		bool bActive = false;
		FString Tag;
		TWeakObjectPtr<UVoxelChunkManager> Owner;
		TArray<FProbeRow> Rows;
		TArray<uint32> Buffer;
		double StartSeconds = 0.0;
		uint64 StartCounter = 0;
		double LastBeginSeconds = 0.0;
		uint64 LastBeginCycles = 0;
		double TickStartSeconds = 0.0;
		uint64 TickStartCycles = 0;
		FProbeRow Current;
		bool bRowOpen = false;
		volatile uint64 Sink = 0;
	};

	FProbeState GProbe;

	void StartProbe(const FString& Tag)
	{
		GProbe = FProbeState();
		GProbe.Tag = Tag;
		GProbe.Buffer.SetNumUninitialized(16 * 1024 * 1024); // 64 MB: beyond the last-level cache (20 MB on the machine G275 was found on)
		uint32 Value = 12345u;
		for (uint32& Entry : GProbe.Buffer)
		{
			Value = Value * 1664525u + 1013904223u;
			Entry = Value;
		}
		GProbe.Rows.Reserve(1 << 16);
		GProbe.StartSeconds = FPlatformTime::Seconds();
		GProbe.StartCounter = TimeStampCounter();
		GProbe.bActive = true;
		UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.Stream.Probe: recording '%s'"), *Tag);
	}

	void StopProbe()
	{
		if (!GProbe.bActive)
		{
			return;
		}
		GProbe.bActive = false;
		const double Elapsed = FPlatformTime::Seconds() - GProbe.StartSeconds;
		const double CounterHz = Elapsed > 0.0 ? static_cast<double>(TimeStampCounter() - GProbe.StartCounter) / Elapsed : 0.0;
		const double CyclesToMs = CounterHz > 0.0 ? 1000.0 / CounterHz : 0.0;
		FString Csv = TEXT("Time,FrameMs,FrameCpuMs,TickMs,TickCpuMs,ComputeUs,ComputeCpuUs,MemoryUs,MemoryCpuUs,AllocUs,ParallelUs,Cpu,CpuAfter,")
			TEXT("JobsInFlight,LoadJobs,LODUpdateMs,StreamingMs,GenLaunchMs,GenPollMs,GenApplyMs,MeshLaunchMs,MeshApplyMs,RenderMeshMs,")
			TEXT("RenderUnloadMs,RenderWaterTileMs,LODMs,SeamMs,FarCompressMs,CollisionMs,CollisionPrepMs,CollisionApplyMs,ScatterMs,")
			TEXT("RenderFlushMs,HydrologyMs,WaterBodyMeshMs,FarFieldMs,TotalMs,GenApplyCount,MeshLaunchCount,RenderSubmitCount,ScatterApplied,")
			TEXT("FarSelectMs,FarApplyMs,FarCoveredTests,FarCoveredLookups,TickBudgetMs,FarLaunchesHeld,WaterBodiesPending,CollDestroysPending,")
			TEXT("FarSkipped,FarCoveredKept,FarPatches,FarDrawn,FarComponents,FarCheckDiffs\n");
		for (const FProbeRow& Row : GProbe.Rows)
		{
			const UVoxelChunkManager::FVoxelTimingStats& T = Row.Timing;
			Csv += FString::Printf(TEXT("%.4f,%.3f,%.3f,%.3f,%.3f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%d,%d,%d,")
				TEXT("%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d,%.3f,%.3f,%d,%d,%.2f,%d,%d,%d,%d,%d,%d,%d,%d,%lld\n"),
				Row.Time, Row.FrameSeconds * 1000.0, Row.FrameCycles * CyclesToMs, Row.TickSeconds * 1000.0, Row.TickCycles * CyclesToMs,
				Row.ComputeSeconds * 1e6, Row.ComputeCycles * CyclesToMs * 1000.0, Row.MemorySeconds * 1e6, Row.MemoryCycles * CyclesToMs * 1000.0,
				Row.AllocSeconds * 1e6, Row.ParallelSeconds * 1e6, Row.Processor, Row.ProcessorAfter, Row.JobsInFlight, Row.LoadJobs,
				T.LODUpdateMs, T.StreamingMs, T.GenLaunchMs, T.GenPollMs, T.GenApplyMs, T.MeshLaunchMs, T.MeshApplyMs, T.RenderMeshMs,
				T.RenderUnloadMs, T.RenderWaterTileMs, T.LODMs, T.SeamMs, T.FarCompressMs, T.CollisionMs, T.CollisionPrepMs, T.CollisionApplyMs,
				T.ScatterMs, T.RenderFlushMs, T.HydrologyMs, T.WaterBodyMeshMs, T.FarFieldMs, T.TotalMs, T.GenApplyCount, T.MeshLaunchCount, T.RenderSubmitCount, T.ScatterApplied,
				T.FarFieldSelectMs, T.FarFieldApplyMs, T.FarFieldCoveredTests, T.FarFieldCoveredLookups,
				T.TickBudgetMs, T.FarFieldLaunchesHeld, T.WaterBodiesPending, T.CollisionDestroysPending,
				T.bFarFieldSkipped ? 1 : 0, T.FarFieldCoveredKept, T.FarFieldPatches, T.FarFieldDrawn, T.FarFieldComponents, T.FarFieldCheckDifferences);
		}
		const FString Path = FPaths::ProjectSavedDir() / TEXT("VoxelBench") / (GProbe.Tag + TEXT("_probe.csv"));
		const bool bSaved = FFileHelper::SaveStringToFile(Csv, *Path);
		UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.Stream.Probe: %d frames in %.1f s, counter %.1f MHz, %s %s"),
			GProbe.Rows.Num(), Elapsed, CounterHz / 1e6, bSaved ? TEXT("wrote") : TEXT("could not write"), *Path);
		GProbe = FProbeState();
	}

	FAutoConsoleCommand GVoxelStreamProbeCmd(
		TEXT("voxel.Stream.Probe"),
		TEXT("G275: voxel.Stream.Probe <tag> records, per chunk manager tick of the play world, the frame and the tick by the clock and by ")
		TEXT("the cycles the game thread spent on a processor, a fixed piece of arithmetic, of scattered memory reads, of allocation and a ")
		TEXT("small ParallelFor timed the same way, the streaming jobs in flight and the tick's sections; voxel.Stream.Probe 0 stops and ")
		TEXT("writes Saved/VoxelBench/<tag>_probe.csv. The fixed work costs about half a millisecond a frame while it records."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() == 0 || Args[0] == TEXT("0"))
			{
				StopProbe();
				return;
			}
			StopProbe();
			StartProbe(Args[0]);
		}));

	std::atomic<int32> GProbeLoadRunning{0};

	FAutoConsoleCommand GVoxelStreamProbeLoadCmd(
		TEXT("voxel.Stream.ProbeLoad"),
		TEXT("G275: voxel.Stream.ProbeLoad <spin|mem|alloc|churn> <jobs> <seconds> [mask=<hex>]: runs that many jobs for that long where the ")
		TEXT("streaming's background jobs run (the engine's thread pool, a background task each), with no streaming work behind them: ")
		TEXT("spin is arithmetic on a register, mem copies a 64 MB block over and over, alloc allocates, writes and frees 1 MB blocks, churn takes, zeroes and frees 48 MB and then computes for about 15 ms. ")
		TEXT("mask confines these jobs to the logical processors of a hexadecimal mask. For telling what busy workers alone do to the game thread."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 3)
			{
				UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Stream.ProbeLoad <spin|mem|alloc> <jobs> <seconds> [mask=<hex>]"));
				return;
			}
			const FString Kind = Args[0].ToLower();
			const int32 Jobs = FMath::Clamp(FCString::Atoi(*Args[1]), 1, 32);
			const double Seconds = FMath::Clamp(FCString::Atod(*Args[2]), 0.1, 600.0);
			uint64 Mask = 0;
			for (int32 Index = 3; Index < Args.Num(); ++Index)
			{
				FString Text;
				if (FParse::Value(*Args[Index], TEXT("mask="), Text))
				{
					Mask = FParse::HexNumber64(*Text);
				}
			}
			UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.Stream.ProbeLoad: %d %s jobs for %.1f s, mask %llx"), Jobs, *Kind, Seconds, Mask);
			const double Deadline = FPlatformTime::Seconds() + Seconds;
			for (int32 Job = 0; Job < Jobs; ++Job)
			{
				Async(EAsyncExecution::ThreadPool, [Kind, Deadline, Mask]()
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(VoxelWorker_ProbeLoad);
					++GProbeLoadRunning;
					if (Mask != 0)
					{
						FPlatformProcess::SetThreadAffinityMask(Mask);
					}
					TArray<uint8> Source;
					TArray<uint8> Target;
					if (Kind == TEXT("mem"))
					{
						Source.SetNumZeroed(64 * 1024 * 1024);
						Target.SetNumZeroed(64 * 1024 * 1024);
					}
					volatile uint64 Sink = 0;
					while (FPlatformTime::Seconds() < Deadline)
					{
						if (Kind == TEXT("mem"))
						{
							FMemory::Memcpy(Target.GetData(), Source.GetData(), Source.Num());
						}
						else if (Kind == TEXT("churn"))
						{
							// What a dense chunk mesh did to memory, with none of its arithmetic: 48 MB taken, zeroed and given back.
							TArray<uint8> Edges;
							Edges.SetNumZeroed(36 * 1024 * 1024);
							TArray<uint8> Cells;
							Cells.SetNumZeroed(12 * 1024 * 1024);
							Sink = Sink + Edges[Edges.Num() / 2] + Cells[Cells.Num() / 2] + ComputeWork(4000000);
						}
						else if (Kind == TEXT("alloc"))
						{
							for (int32 Round = 0; Round < 16; ++Round)
							{
								AllocWork();
							}
						}
						else
						{
							Sink = Sink + ComputeWork(1000000);
						}
					}
					if (Mask != 0)
					{
						FPlatformProcess::SetThreadAffinityMask(FPlatformAffinity::GetNoAffinityMask());
					}
					--GProbeLoadRunning;
				});
			}
		}));
}

void VoxelStreamProbe::BeginTick(UVoxelChunkManager& Manager)
{
	if (!GProbe.bActive)
	{
		return;
	}
	if (!GProbe.Owner.IsValid())
	{
		const UWorld* World = Manager.GetWorld();
		if (!World || !World->IsGameWorld())
		{
			return;
		}
		GProbe.Owner = &Manager;
	}
	if (GProbe.Owner.Get() != &Manager)
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	const uint64 Cycles = ThreadCycles();
	FProbeRow Row;
	Row.Time = Now - GProbe.StartSeconds;
	if (GProbe.LastBeginSeconds > 0.0)
	{
		Row.FrameSeconds = Now - GProbe.LastBeginSeconds;
		Row.FrameCycles = Cycles - GProbe.LastBeginCycles;
	}
	GProbe.LastBeginSeconds = Now;
	GProbe.LastBeginCycles = Cycles;
	Row.Processor = CurrentProcessor();

	double T0 = FPlatformTime::Seconds();
	uint64 C0 = ThreadCycles();
	GProbe.Sink = GProbe.Sink + ComputeWork(60000);
	double T1 = FPlatformTime::Seconds();
	uint64 C1 = ThreadCycles();
	Row.ComputeSeconds = T1 - T0;
	Row.ComputeCycles = C1 - C0;

	GProbe.Sink = GProbe.Sink + MemoryWork(GProbe.Buffer, 12000, static_cast<uint32>(GProbe.Rows.Num() * 7919u));
	double T2 = FPlatformTime::Seconds();
	uint64 C2 = ThreadCycles();
	Row.MemorySeconds = T2 - T1;
	Row.MemoryCycles = C2 - C1;

	AllocWork();
	const double T3 = FPlatformTime::Seconds();
	Row.AllocSeconds = T3 - T2;

	// Foreground tasks as the game thread's own parallel work launches them: how long 16 small bodies take to come back.
	std::atomic<uint64> Sum{0};
	ParallelFor(16, [&Sum](int32)
	{
		Sum.fetch_add(ComputeWork(2000), std::memory_order_relaxed);
	});
	Row.ParallelSeconds = FPlatformTime::Seconds() - T3;
	Row.ProcessorAfter = CurrentProcessor();

	GProbe.Current = Row;
	GProbe.bRowOpen = true;
	GProbe.TickStartSeconds = FPlatformTime::Seconds();
	GProbe.TickStartCycles = ThreadCycles();
}

void VoxelStreamProbe::EndTick(UVoxelChunkManager& Manager)
{
	if (!GProbe.bActive || !GProbe.bRowOpen || GProbe.Owner.Get() != &Manager)
	{
		return;
	}
	GProbe.bRowOpen = false;
	GProbe.Current.TickSeconds = FPlatformTime::Seconds() - GProbe.TickStartSeconds;
	GProbe.Current.TickCycles = ThreadCycles() - GProbe.TickStartCycles;
	GProbe.Current.Timing = Manager.GetTimingStats();
	GProbe.Current.JobsInFlight = Manager.GetWorkerJobs()->load();
	GProbe.Current.LoadJobs = GProbeLoadRunning.load();
	GProbe.Rows.Add(GProbe.Current);
}
