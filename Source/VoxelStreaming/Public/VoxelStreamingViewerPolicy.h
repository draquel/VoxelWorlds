// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FLODQueryContext;

/**
 * A change of a planet window's cut-over state (spherical Phase 6, WM-BP10), as the chunk manager records it for the
 * bench, the log and tests (G49, WORLD_MODES_REVIEW_PLAN.md §20.28): when the window resumed or suspended, where the
 * camera and the streaming viewer were, and for a suspension what the window had done since it last resumed.
 */
struct FVoxelCutoverTransition
{
	/** Seconds of streaming ticks since the world started, and the state entered: true = suspended, false = resumed. */
	double Time = 0.0;
	bool bSuspended = false;

	/** The camera's and the streaming viewer's altitude over the surface under them (uu), and the prefetch's lead. */
	double CameraOverSurface = 0.0;
	double ViewerOverSurface = 0.0;
	double Lead = 0.0;

	/** Suspensions: seconds since the window last resumed (-1: active since the world started, never resumed). */
	double ActiveSeconds = -1.0;

	/** Suspensions: chunks loaded when it suspended (all of them unload), and queued generations evicted then. */
	int32 ChunksHeld = 0;
	int32 GenerationsDropped = 0;

	/** Suspensions: chunks generated and meshed since the window last resumed (since the world started if it never did). */
	int64 GeneratedSinceResume = 0;
	int64 MeshedSinceResume = 0;
};

/** What the descent prefetch reads of the world each tick (the chunk manager fills it from the configuration). */
struct FVoxelDescentPrefetchInput
{
	/** Local up is the direction from Centre (a planet) rather than +Z (a heightmap world) */
	bool bPlanet = false;

	/** A planet's centre (unused on a heightmap world) */
	FVector Centre = FVector::ZeroVector;

	/** A chunk's width: a jump of more than ten in a tick is a teleport, and the viewer stops two over the surface */
	double ChunkWorldSize = 3200.0;

	/** How far ahead of the camera the viewer goes, in seconds of its descent (voxel.Stream.DescentLead; 0 = no prefetch) */
	double LeadSeconds = 0.0;

	/** Slower descents have no lead (voxel.Stream.DescentMinSpeed) */
	double MinDescentSpeed = 0.0;
};

/**
 * Where the streaming viewer is relative to the camera, and the voxel window's cut-over record (spherical Phase 6
 * WM-BP10; every world mode since WM-MP3): under a camera descending fast the streaming viewer goes ahead of it along
 * local down (the descent prefetch), so the window resumes from the cut-over and refines before the camera arrives;
 * every time the window suspends or resumes a transition is recorded for the bench, the log and tests (G49). The chunk
 * manager owns one, hands it the camera's context each tick and acts on the transitions (the queues, the log).
 */
class VOXELSTREAMING_API FVoxelStreamingViewerPolicy
{
public:
	/** How many transitions are kept (the count of all of them is GetTransitionCount) */
	static constexpr int32 MaxCutoverTransitions = 256;

	/**
	 * Pure: the descent prefetch's lead (WM-BP10; every world mode since world modes review WM-MP3): under a camera descending
	 * faster than MinDescentSpeed, the streaming viewer goes ahead of it along local down by the distance it covers in
	 * LeadSeconds, at most half its altitude and stopping Clearance over the surface, so the voxel window resumes from the
	 * cut-over and refines its LOD before the camera gets there (a descent slowing toward the ground resumes it from twice
	 * the cut-over at most).
	 *
	 * @param RadialSpeed The camera's smoothed speed along local up (uu/s, negative descending)
	 * @param AltitudeOverSurface The camera's altitude over the surface under it (the terrain, or the sea where higher)
	 * @param LeadSeconds How far ahead (0 = no prefetch)
	 * @param MinDescentSpeed Slower descents have no lead (a jump, a walk downhill)
	 * @param Clearance How close to the surface the streaming viewer may go
	 * @return The lead (uu along local down, >= 0)
	 */
	static double ComputeDescentLead(double RadialSpeed, double AltitudeOverSurface, double LeadSeconds, double MinDescentSpeed, double Clearance);

	/** Start over with a new world: no lead, no speed, no transitions. */
	void Reset();

	/** Count a streaming tick's seconds (the transitions' Time). */
	void AdvanceClock(float DeltaTime) { CutoverClock += DeltaTime; }

	/** No prefetch this tick (no world, or voxel.Stream.DescentLead 0): no lead, and the speed starts over. */
	void DisablePrefetch();

	/**
	 * Move the streaming viewer under a descending camera (ComputeDescentLead, from the camera's smoothed speed along
	 * local up; a teleport resets the speed).
	 * @param Context       The camera's context: its ViewerPosition (and ViewerLead) moved by the lead
	 * @param DeltaTime     The tick's seconds
	 * @param Input         The world's frame and the prefetch's settings
	 * @param SurfaceHeight The height along local up of the surface under the camera (the terrain, or the sea where higher)
	 */
	void ApplyDescentPrefetch(FLODQueryContext& Context, float DeltaTime, const FVoxelDescentPrefetchInput& Input,
		TFunctionRef<double(const FVector& Up, const FVector& Camera)> SurfaceHeight);

	/** Put a context's viewer where this tick's prefetch put it (under the camera by the lead). */
	void ApplyLead(FLODQueryContext& Context) const;

	/** After a camera cut: the camera is the streaming viewer, and the prefetch's speed starts over from it. */
	void RestartAt(const FVector& Camera);

	/**
	 * Note the window's cut-over state this tick.
	 * @param bSuspended         Whether the window is suspended now
	 * @param ViewerOverSurface  The streaming viewer's altitude over the surface (IVoxelLODStrategy::GetViewerSurfaceAltitude)
	 * @param TotalGenerated     Chunks generated since the world started
	 * @param TotalMeshed        Chunks meshed since the world started
	 * @param OutTransition      On a change: the transition, its ChunksHeld and GenerationsDropped left for the caller
	 * @return True when the state changed (then RecordTransition the filled record)
	 */
	bool BeginTransition(bool bSuspended, double ViewerOverSurface, int64 TotalGenerated, int64 TotalMeshed,
		FVoxelCutoverTransition& OutTransition);

	/** Keep a transition BeginTransition started (the oldest dropped past MaxCutoverTransitions). */
	void RecordTransition(const FVoxelCutoverTransition& Transition);

	/** @return How far under the camera the streaming viewer is this tick (uu along local down; 0 = at the camera) */
	double GetLead() const { return StreamingViewerLead; }

	/** @return The window's cut-over transitions since the world started, oldest first */
	const TArray<FVoxelCutoverTransition>& GetTransitions() const { return CutoverTransitions; }

	/** @return How many transitions there were in all */
	int64 GetTransitionCount() const { return CutoverTransitionCount; }

private:
	/** The camera's last position and its smoothed speed along local up (uu/s, negative descending), for the prefetch. */
	FVector LastPrefetchCamera = FVector(FLT_MAX);
	double CameraRadialSpeed = 0.0;

	/** How far under the camera the streaming viewer is this tick (uu along local down; 0 = at the camera). */
	double StreamingViewerLead = 0.0;

	/** Local down at the camera on the tick the lead was computed (toward a planet's centre; -Z on a heightmap world). */
	FVector StreamingViewerDown = -FVector::UpVector;

	/** The window's cut-over state on the last tick (IVoxelLODStrategy::IsWindowSuspended), to act when it changes. */
	bool bWasWindowSuspended = false;

	/** The cut-over transitions (GetCutoverTransitions), their count, and the last resume's time and counters (-1: none). */
	TArray<FVoxelCutoverTransition> CutoverTransitions;
	int64 CutoverTransitionCount = 0;
	double CutoverClock = 0.0; // seconds of streaming ticks since the world started (the transitions' Time)
	double LastWindowResumeTime = -1.0;
	int64 GeneratedAtWindowResume = 0;
	int64 MeshedAtWindowResume = 0;
};
