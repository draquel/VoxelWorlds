// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelStreamingViewerPolicy.h"
#include "LODTypes.h"

double FVoxelStreamingViewerPolicy::ComputeDescentLead(double RadialSpeed, double AltitudeOverSurface, double LeadSeconds, double MinDescentSpeed,
	double Clearance)
{
	const double Descent = -RadialSpeed;
	if (LeadSeconds <= 0.0 || Descent <= FMath::Max(MinDescentSpeed, 0.0))
	{
		return 0.0;
	}
	// At most half the altitude: a descent that slows toward the ground (vox.FlyTo's falls by the same factor every
	// second) would otherwise put the viewer at the ground from any height and resume the window far above the cut-over.
	const double MaxLead = FMath::Min(0.5 * AltitudeOverSurface, AltitudeOverSurface - Clearance);
	return FMath::Clamp(Descent * LeadSeconds, 0.0, FMath::Max(MaxLead, 0.0));
}

void FVoxelStreamingViewerPolicy::Reset()
{
	bWasWindowSuspended = false;
	StreamingViewerLead = 0.0;
	CameraRadialSpeed = 0.0;
	LastPrefetchCamera = FVector(FLT_MAX);
	CutoverTransitions.Reset();
	CutoverTransitionCount = 0;
	CutoverClock = 0.0;
	LastWindowResumeTime = -1.0;
	GeneratedAtWindowResume = 0;
	MeshedAtWindowResume = 0;
}

void FVoxelStreamingViewerPolicy::DisablePrefetch()
{
	StreamingViewerLead = 0.0;
	CameraRadialSpeed = 0.0;
	LastPrefetchCamera = FVector(FLT_MAX);
}

void FVoxelStreamingViewerPolicy::ApplyDescentPrefetch(FLODQueryContext& Context, float DeltaTime, const FVoxelDescentPrefetchInput& Input,
	TFunctionRef<double(const FVector& Up, const FVector& Camera)> SurfaceHeightAt)
{
	// Local up at the camera and its height along it: on a planet the direction from the centre and the distance to it;
	// on a heightmap world +Z and Z (world modes review WM-MP3: the prefetch in every world mode).
	const bool bPlanet = Input.bPlanet;
	const FVector Centre = Input.Centre;
	const FVector Camera = Context.ViewerPosition;
	const FVector Up = bPlanet ? (Camera - Centre).GetSafeNormal() : FVector::UpVector;
	if (Up.IsNearlyZero())
	{
		StreamingViewerLead = 0.0;
		return;
	}
	auto HeightOf = [bPlanet, &Centre](const FVector& Position) { return bPlanet ? FVector::Dist(Position, Centre) : Position.Z; };

	// The camera's speed along local up, smoothed. A jump of more than ten chunks in a frame is a teleport (or a switch of
	// view target), not motion: the speed starts over.
	const double ChunkWorldSize = Input.ChunkWorldSize;
	const double CameraHeight = HeightOf(Camera);
	if (DeltaTime > SMALL_NUMBER && LastPrefetchCamera.X != FLT_MAX && FVector::DistSquared(Camera, LastPrefetchCamera) < FMath::Square(10.0 * ChunkWorldSize))
	{
		const double Instant = (CameraHeight - HeightOf(LastPrefetchCamera)) / DeltaTime;
		CameraRadialSpeed = FMath::FInterpTo(CameraRadialSpeed, Instant, static_cast<double>(DeltaTime), 4.0);
	}
	else
	{
		CameraRadialSpeed = 0.0;
	}
	LastPrefetchCamera = Camera;

	// Ahead of the camera along local down by what it covers in LeadSeconds, stopping two chunks over the surface under it.
	const double SurfaceHeight = SurfaceHeightAt(Up, Camera);
	StreamingViewerLead = ComputeDescentLead(CameraRadialSpeed, CameraHeight - SurfaceHeight, Input.LeadSeconds,
		Input.MinDescentSpeed, 2.0 * ChunkWorldSize);
	StreamingViewerDown = -Up;
	if (StreamingViewerLead > 0.0)
	{
		Context.ViewerPosition = Camera + StreamingViewerDown * StreamingViewerLead;
		// The cut-over reads the camera's altitude from it (G49: a suspended window resumes only with the camera under it).
		Context.ViewerLead = StreamingViewerLead;
	}
}

void FVoxelStreamingViewerPolicy::ApplyLead(FLODQueryContext& Context) const
{
	if (StreamingViewerLead > 0.0)
	{
		Context.ViewerPosition += StreamingViewerDown * StreamingViewerLead;
		Context.ViewerLead = StreamingViewerLead;
	}
}

void FVoxelStreamingViewerPolicy::RestartAt(const FVector& Camera)
{
	StreamingViewerLead = 0.0;
	CameraRadialSpeed = 0.0;
	LastPrefetchCamera = Camera;
}

bool FVoxelStreamingViewerPolicy::BeginTransition(bool bSuspended, double ViewerOverSurface, int64 TotalGenerated, int64 TotalMeshed,
	FVoxelCutoverTransition& OutTransition)
{
	if (bSuspended == bWasWindowSuspended)
	{
		return false;
	}
	bWasWindowSuspended = bSuspended;
	OutTransition = FVoxelCutoverTransition();
	OutTransition.Time = CutoverClock;
	OutTransition.bSuspended = bSuspended;
	OutTransition.ViewerOverSurface = ViewerOverSurface;
	OutTransition.Lead = StreamingViewerLead;
	OutTransition.CameraOverSurface = OutTransition.ViewerOverSurface + StreamingViewerLead;
	if (bSuspended)
	{
		OutTransition.ActiveSeconds = LastWindowResumeTime >= 0.0 ? OutTransition.Time - LastWindowResumeTime : -1.0;
		OutTransition.GeneratedSinceResume = TotalGenerated - GeneratedAtWindowResume;
		OutTransition.MeshedSinceResume = TotalMeshed - MeshedAtWindowResume;
	}
	else
	{
		LastWindowResumeTime = OutTransition.Time;
		GeneratedAtWindowResume = TotalGenerated;
		MeshedAtWindowResume = TotalMeshed;
	}
	return true;
}

void FVoxelStreamingViewerPolicy::RecordTransition(const FVoxelCutoverTransition& Transition)
{
	if (CutoverTransitions.Num() >= MaxCutoverTransitions)
	{
		CutoverTransitions.RemoveAt(0);
	}
	CutoverTransitions.Add(Transition);
	++CutoverTransitionCount;
}
