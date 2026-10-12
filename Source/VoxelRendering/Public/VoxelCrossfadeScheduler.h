// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Game-thread timing for mesh-swap crossfades (VoxelRendering).
 *
 * Owns only the clock side of a fade: when each one started, whether it is held, and what alpha
 * it is at. UVoxelWorldComponent wraps it with the fade MIDs and the proxy commands. Chunk
 * interiors and seam buckets share one scheduler, keyed by FIntVector4 (chunk XYZ + a kind
 * slot: ChunkKind for an interior, 0..6 for a seam bucket slot), so a release can start any mix of
 * them on the same tick and they stay alpha-synchronised, which the complementary dither needs.
 *
 * Holds (R24 P2): a held fade stays at alpha 0 (the previous mesh fully visible, the new one
 * uploaded but dithered out) until Release, or until it has been held for the failsafe timeout.
 * Releasing restarts its clock, so everything released together fades together.
 *
 * Pure C++ (no UObject, no RHI); game-thread only.
 *
 * @see UVoxelWorldComponent::AdvanceCrossfades
 * @see Documentation/VOXEL_QA_FOLLOWUPS_PLAN.md (R24)
 */
class VOXELRENDERING_API FVoxelCrossfadeScheduler
{
public:
	/** Kind slot (W) of an interior fade key; seam bucket keys use the bucket slot 0..6. */
	static constexpr int32 ChunkKind = -1;

	static FIntVector4 ChunkKey(const FIntVector& Chunk) { return FIntVector4(Chunk.X, Chunk.Y, Chunk.Z, ChunkKind); }
	static FIntVector4 SeamKey(const FIntVector& Owner, uint8 Slot) { return FIntVector4(Owner.X, Owner.Y, Owner.Z, Slot); }
	static bool IsSeamKey(const FIntVector4& Key) { return Key.W != ChunkKind; }
	static FIntVector KeyCoord(const FIntVector4& Key) { return FIntVector(Key.X, Key.Y, Key.Z); }

	/** One Advance call's output. */
	struct FAdvanceResult
	{
		/** Alpha of every running or held fade (held ones report 0). */
		TArray<TPair<FIntVector4, float>> Alphas;
		/** Fades that reached alpha 1 this call (removed from the scheduler). */
		TArray<FIntVector4> Completed;
		/** Holds released by the failsafe timeout this call (they start fading now). */
		TArray<FIntVector4> TimedOutHolds;
	};

	/**
	 * Start a fade, or restart one already running (a resubmit during a fade). A restart keeps
	 * the entry and resets its clock; bHold applies to the restarted fade.
	 *
	 * @param Key        Interior or seam bucket key.
	 * @param NowSeconds Current time.
	 * @param bHold      Start held at alpha 0 until Release.
	 * @return False only when the key is new and MaxConcurrent fades are already running: the
	 *         caller should swap instantly instead (fade MIDs are pooled but not free).
	 */
	bool Start(const FIntVector4& Key, double NowSeconds, bool bHold);

	/**
	 * Release a held fade: it starts fading from alpha 0 now.
	 * @return True if the key was held.
	 */
	bool Release(const FIntVector4& Key, double NowSeconds);

	/** Drop a fade without completing it. @return True if it existed. */
	bool Cancel(const FIntVector4& Key);

	/** Drop everything, reporting the dropped keys. */
	void CancelAll(TArray<FIntVector4>& OutCancelled);

	/**
	 * Advance every fade: alpha = elapsed / Duration for running fades, 0 for held ones. Fades
	 * reaching 1 complete and leave the scheduler. Holds older than HoldTimeoutSeconds release.
	 *
	 * @param NowSeconds         Current time.
	 * @param DurationSeconds    Fade duration (clamped to >= 0.02 s).
	 * @param HoldTimeoutSeconds Failsafe cap on a hold (<= 0 = no failsafe).
	 * @param Out                Receives alphas and completions (reset first).
	 */
	void Advance(double NowSeconds, float DurationSeconds, double HoldTimeoutSeconds, FAdvanceResult& Out);

	/**
	 * Seconds until the key's running fade completes: 0 when it has none or is held (a held fade
	 * shows only its previous mesh, so replacing its never-shown current loses nothing).
	 *
	 * @param Key             Interior or seam bucket key.
	 * @param NowSeconds      Current time.
	 * @param DurationSeconds Fade duration (clamped like Advance).
	 */
	float GetRemainingSeconds(const FIntVector4& Key, double NowSeconds, float DurationSeconds) const;

	bool Contains(const FIntVector4& Key) const { return Entries.Contains(Key); }
	bool IsHeld(const FIntVector4& Key) const { const FEntry* E = Entries.Find(Key); return E && E->bHeld; }
	int32 Num() const { return Entries.Num(); }

	/** Cap on concurrent fades (new ones beyond it are refused). Default 512. */
	void SetMaxConcurrent(int32 InMax) { MaxConcurrent = FMath::Max(1, InMax); }
	int32 GetMaxConcurrent() const { return MaxConcurrent; }

private:
	struct FEntry
	{
		/** Fade clock origin (alpha 0) — reset on start, restart and release. */
		double StartSeconds = 0.0;
		/** When the current hold began (failsafe timeout reference). */
		double HeldSinceSeconds = 0.0;
		bool bHeld = false;
	};

	TMap<FIntVector4, FEntry> Entries;
	int32 MaxConcurrent = 512;
};
