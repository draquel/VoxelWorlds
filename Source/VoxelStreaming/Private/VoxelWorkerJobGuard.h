// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include <atomic>

/**
 * Counts a worker job that holds a raw pointer to something the chunk manager frees in Shutdown (its meshers, its noise
 * generator) from the job's dispatch to its end, so Shutdown waits for them before those go: a chunk mesh, a seam job, a
 * chunk generation and its tree injection, a collision cook (world modes review G76, G104). Copies count too: a lambda
 * moved into its task keeps one. The counter is UVoxelChunkManager::GetWorkerJobs.
 */
struct FVoxelWorkerJobGuard
{
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Count;
	explicit FVoxelWorkerJobGuard(const TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe>& InCount) : Count(InCount) { ++(*Count); }
	FVoxelWorkerJobGuard(const FVoxelWorkerJobGuard& Other) : Count(Other.Count) { ++(*Count); }
	FVoxelWorkerJobGuard& operator=(const FVoxelWorkerJobGuard&) = delete;
	~FVoxelWorkerJobGuard() { --(*Count); }
};
