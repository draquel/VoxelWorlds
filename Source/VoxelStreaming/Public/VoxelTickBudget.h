// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"

/**
 * The chunk manager's tick budget (G282): the milliseconds a tick of the chunk manager (UVoxelChunkManager::TickComponent)
 * may take before the work that can wait a tick waits. The work the player stands on and sees comes first and is not
 * held by it (the collision cooks' applies and launches, the cooks under and ahead of the pawn, the meshes' submits to the
 * renderer, which keep their own caps); what can wait draws on what is left of the tick, in the tick's order: the far
 * field's build launches (UVoxelChunkManager::UpdateFarField), the lakes' and river reaches' meshes
 * (UpdateWaterBodyMeshes), the collision components of chunks that left the shell (UVoxelCollisionManager). Each of
 * those does at least one piece a tick, so nothing waits for ever; the rest goes on the next tick, nearest first, and the
 * world settles to the same state as without a budget.
 *
 * voxel.Stream.TickBudgetMs (UVoxelWorldsSettings::StreamTickBudgetMs): 0 = no budget (every section as before G282).
 */
struct FVoxelTickBudget
{
	/** A clock in seconds (FPlatformTime::Seconds; a test passes its own). */
	using FClock = double (*)();

	/** No budget: every piece of work goes on (the work before G282). */
	FVoxelTickBudget() = default;

	/**
	 * A budget from now.
	 *
	 * @param InBudgetMs Milliseconds from the start; <= 0 is no budget
	 * @param InClock The clock (seconds)
	 */
	explicit FVoxelTickBudget(double InBudgetMs, FClock InClock = &FPlatformTime::Seconds)
		: Clock(InClock)
		, StartSeconds(InClock())
		, BudgetSeconds(InBudgetMs > 0.0 ? InBudgetMs / 1000.0 : 0.0)
	{
	}

	/** @return Whether there is a budget (false: every piece of work goes on) */
	bool IsLimited() const { return BudgetSeconds > 0.0; }

	/** @return Milliseconds since the budget's start */
	double ElapsedMs() const { return (Clock() - StartSeconds) * 1000.0; }

	/** @return Whether time is left (always without a budget) */
	bool HasTimeLeft() const { return !IsLimited() || Clock() - StartSeconds < BudgetSeconds; }

	/**
	 * Whether a section's next piece of work may go on this tick: its first always (each section moves every tick), the
	 * others while time is left.
	 *
	 * @param DoneThisTick The pieces the section did this tick
	 * @return True to go on, false to leave the rest for the next tick
	 */
	bool MayContinue(int32 DoneThisTick) const { return DoneThisTick <= 0 || HasTimeLeft(); }

private:
	FClock Clock = &FPlatformTime::Seconds;
	double StartSeconds = 0.0;
	double BudgetSeconds = 0.0;
};
