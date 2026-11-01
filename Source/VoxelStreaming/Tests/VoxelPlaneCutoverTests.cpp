// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelPlanetFarField.h"
#include "VoxelStreamingBenchmark.h"
#include "VoxelCubeSphere.h"
#include "VoxelWorldConfiguration.h"
#include "DistanceBandLODStrategy.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The voxel cut-over on a heightmap world (WORLD_MODES_REVIEW_PLAN.md §23, milestone MP3, WM-MP2, decision D38) and the
// bench's altitude routes there (WM-MP11).
//
// - LOD.Plane.Cutover: a chunk manager on a plane with a far field: the window streams on the ground; above the cut-over
//   (three view distances over the terrain or the sea under the viewer by default, here the view distance) every chunk
//   unloads and the far field alone draws; between the resume altitude and the cut-over it stays empty; under the resume
//   altitude it streams back. Without a far field (voxel.World.FarField 0) a plane's window is never suspended, whatever
//   the altitude (the ground stays loaded under a high viewer, D8).
// - Streaming.Plane.CutoverDescentStop: G49's guard on a plane: a fast descent that stops above the cut-over never resumes
//   the window; one that stops under it keeps it.
// - Bench.Plane.ClimbRoute: climb=X:Y[:Alt] and descend=X:Y:Alt: straight paths along +Z from the ground at (X, Y), the
//   report recording the site's X and Y.
// ---------------------------------------------------------------------------

namespace VoxelPlaneCutoverTestUtils
{
	/** Set by code: a console-priority set would outrank the planet tests' own sets of the same variables. */
	struct FScopedCVar
	{
		IConsoleVariable* Var = nullptr;
		FString Previous;
		EConsoleVariableFlags Priority = ECVF_SetByCode;
		// At the priority the variable already holds: a set below it is ignored (a test that set it by console earlier in a
		// suite run), one above it would outrank the other tests' code-priority sets.
		FScopedCVar(const TCHAR* Name, const TCHAR* Value)
		{
			Var = IConsoleManager::Get().FindConsoleVariable(Name);
			if (Var)
			{
				Previous = Var->GetString();
				Priority = static_cast<EConsoleVariableFlags>(FMath::Max(static_cast<uint32>(Var->GetFlags() & ECVF_SetByMask), static_cast<uint32>(ECVF_SetByCode)));
				Var->Set(Value, Priority);
			}
		}
		~FScopedCVar()
		{
			if (Var)
			{
				Var->Set(*Previous, Priority);
			}
		}
	};

	constexpr float ViewDistance = 6000.0f;

	/** A plane with three LOD bands, a sea at -200 and the cut-over at the view distance (the default is three). */
	void ConfigurePlane(VoxelTesting::FTestChunkWorld& W)
	{
		W.ConfigureSmallPlane();
		W.Config->bEnableLOD = true;
		W.Config->ViewDistance = ViewDistance;
		W.Config->LODBands = {
			FLODBand(0.0f, 2000.0f, 0, 1),
			FLODBand(2000.0f, 4000.0f, 1, 2),
			FLODBand(4000.0f, ViewDistance, 2, 4) };
		W.Config->bEnableWaterLevel = true;
		W.Config->WaterLevel = -200.0f;
		W.Config->WaterMeshMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		W.Config->PlanetVoxelCutoverAltitude = ViewDistance;
	}

	/** Synchronous far-field builds (the tests read its state right after an update). */
	void MakeFarFieldSynchronous(VoxelTesting::FTestChunkWorld& W)
	{
		if (FVoxelPlanetFarField* FarField = W.Manager->GetFarFieldMutable())
		{
			FarField->SetSynchronousBuilds(true);
			FarField->SetMaxBuildsInFlight(64);
		}
	}
}

/**
 * WM-MP2: the cut-over in a chunk manager on a plane. The cut-over is the view distance (6,000 over the terrain or the
 * sea), resuming below 0.85 of it (5,100).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlaneCutoverTest,
	"VoxelWorlds.LOD.Plane.Cutover",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlaneCutoverTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlaneCutoverTestUtils;
	using namespace VoxelTesting;

	const FScopedCVar LeadVar(TEXT("voxel.Stream.DescentLead"), TEXT("0")); // no prefetch: the altitudes below are the viewer's own
	for (const bool bFarField : { true, false })
	{
		const TCHAR* Rule = bFarField ? TEXT("with the far field") : TEXT("voxel.World.FarField 0");
		const FScopedCVar FarFieldVar(TEXT("voxel.World.FarField"), bFarField ? TEXT("1") : TEXT("0"));
		FTestChunkWorld W;
		W.Create(TEXT("PlaneCutoverWorld"));
		ConfigurePlane(W);
		W.StartManager();
		MakeFarFieldSynchronous(W);
		TestEqual(*FString::Printf(TEXT("%s: a far field as asked"), Rule), W.Manager->HasFarField(), bFarField);

		// The site over land; altitudes over the terrain or the sea, whichever is higher.
		const FVector2D Site(4000.0, -2500.0);
		const double Ground = W.Manager->GetGeneratedSurfacePoint(FVector(Site.X, Site.Y, 0.0)).Z;
		const double SurfaceZ = FMath::Max(Ground, static_cast<double>(W.Config->WaterLevel));
		auto At = [&Site, SurfaceZ](double OverSurface) { return FVector(Site.X, Site.Y, SurfaceZ + OverSurface); };

		// On the ground: the window streams; the strategy reports the cut-over only with a far field.
		W.SetViewer(At(300.0));
		TestTrue(*FString::Printf(TEXT("%s: settled on the ground"), Rule), W.Settle());
		const int32 OnTheGround = W.Manager->GetLoadedChunkCount();
		const double Cutover = W.Manager->GetLODStrategy()->GetCutoverAltitude();
		const double Resume = Cutover * FDistanceBandLODStrategy::CutoverResumeFraction;
		AddInfo(FString::Printf(TEXT("%s: on the ground %d chunks; cut-over %.0f (resumes below %.0f); viewer %.0f over the surface"),
			Rule, OnTheGround, Cutover, Resume, W.Manager->GetLODStrategy()->GetViewerSurfaceAltitude()));
		TestTrue(*FString::Printf(TEXT("%s: the window holds chunks on the ground"), Rule), OnTheGround > 20);
		TestFalse(*FString::Printf(TEXT("%s: active on the ground"), Rule), W.Manager->IsVoxelWindowSuspended());
		TestTrue(*FString::Printf(TEXT("%s: the viewer's altitude over the surface is measured"), Rule),
			FMath::IsNearlyEqual(W.Manager->GetLODStrategy()->GetViewerSurfaceAltitude(), 300.0, 1.0));
		if (bFarField)
		{
			TestTrue(*FString::Printf(TEXT("%s: the cut-over is the view distance"), Rule), FMath::IsNearlyEqual(Cutover, ViewDistance, 1.0));
		}
		else
		{
			TestEqual(*FString::Printf(TEXT("%s: no cut-over without a far field"), Rule), Cutover, 0.0);
		}

		// Three cut-overs up: suspended with the far field (every chunk unloads, the far field draws); never without one.
		W.SetViewer(At(3.0 * ViewDistance));
		TestTrue(*FString::Printf(TEXT("%s: settled high above"), Rule), W.Settle());
		const FVoxelFarFieldStats High = W.Manager->GetFarFieldStats();
		AddInfo(FString::Printf(TEXT("%s: at %.0f over the surface: window %s, %d chunks, %d pending generations; far field %d patches drawn (%d hidden by the window)"),
			Rule, 3.0 * ViewDistance, W.Manager->IsVoxelWindowSuspended() ? TEXT("suspended") : TEXT("active"), W.Manager->GetLoadedChunkCount(),
			W.Manager->GetPendingGenerationCount(), High.DrawnPatches, High.HiddenBand));
		if (bFarField)
		{
			TestTrue(*FString::Printf(TEXT("%s: suspended above the cut-over"), Rule), W.Manager->IsVoxelWindowSuspended());
			TestEqual(*FString::Printf(TEXT("%s: every chunk unloaded"), Rule), W.Manager->GetLoadedChunkCount(), 0);
			TestEqual(*FString::Printf(TEXT("%s: nothing queued"), Rule), W.Manager->GetPendingGenerationCount(), 0);
			TestTrue(*FString::Printf(TEXT("%s: the far field draws"), Rule), High.DrawnPatches > 0);
			TestEqual(*FString::Printf(TEXT("%s: the far field cuts no hole"), Rule), High.HiddenBand, 0);
			TestEqual(*FString::Printf(TEXT("%s: one suspension recorded"), Rule), W.Manager->GetCutoverTransitions().Num(), 1);

			// Between the resume altitude and the cut-over: still suspended (hysteresis).
			W.SetViewer(At(0.5 * (Resume + Cutover)));
			TestTrue(*FString::Printf(TEXT("%s: settled between resume and cut-over"), Rule), W.Settle());
			TestTrue(*FString::Printf(TEXT("%s: still suspended under the cut-over, over the resume altitude"), Rule), W.Manager->IsVoxelWindowSuspended());
			TestEqual(*FString::Printf(TEXT("%s: still empty"), Rule), W.Manager->GetLoadedChunkCount(), 0);

			// Under the resume altitude: the window streams back.
			W.SetViewer(At(0.8 * Resume));
			TestTrue(*FString::Printf(TEXT("%s: settled under the resume altitude"), Rule), W.Settle());
			const int32 Resumed = W.Manager->GetLoadedChunkCount();
			AddInfo(FString::Printf(TEXT("%s: at %.0f over the surface: window %s, %d chunks"), Rule, 0.8 * Resume,
				W.Manager->IsVoxelWindowSuspended() ? TEXT("suspended") : TEXT("active"), Resumed));
			TestFalse(*FString::Printf(TEXT("%s: resumed under the resume altitude"), Rule), W.Manager->IsVoxelWindowSuspended());
			TestTrue(*FString::Printf(TEXT("%s: the window streamed back"), Rule), Resumed > 20);
			TestEqual(*FString::Printf(TEXT("%s: a resume recorded"), Rule), W.Manager->GetCutoverTransitions().Num(), 2);
		}
		else
		{
			TestFalse(*FString::Printf(TEXT("%s: never suspended without a far field"), Rule), W.Manager->IsVoxelWindowSuspended());
			TestTrue(*FString::Printf(TEXT("%s: the ground stays loaded under a high viewer (D8)"), Rule), W.Manager->GetLoadedChunkCount() > 0);
			TestEqual(*FString::Printf(TEXT("%s: no transition"), Rule), W.Manager->GetCutoverTransitions().Num(), 0);
		}
		W.Stop();
	}
	return true;
}

/**
 * WM-MP2, G49 on a plane: with the resume guard a fast descent (18,000 uu/s, the prefetch's lead 3 s) that stops at 1.4
 * cut-overs never resumes the window, and one that stops at 0.92 cut-overs resumes it once, during the descent, and keeps
 * it; under the rule before (voxel.World.CutoverResumeGuard 0) the first stop resumed and suspended again.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlaneCutoverDescentStopTest,
	"VoxelWorlds.Streaming.Plane.CutoverDescentStop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlaneCutoverDescentStopTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlaneCutoverTestUtils;
	using namespace VoxelTesting;

	const FScopedCVar FarFieldVar(TEXT("voxel.World.FarField"), TEXT("1"));
	const FScopedCVar LeadVar(TEXT("voxel.Stream.DescentLead"), TEXT("3"));
	IConsoleVariable* GuardVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.World.CutoverResumeGuard"));
	if (!TestNotNull(TEXT("voxel.World.CutoverResumeGuard exists"), GuardVar))
	{
		return false;
	}
	const FString SavedGuard = GuardVar->GetString();

	FTestChunkWorld W;
	W.Create(TEXT("PlaneCutoverStopWorld"));
	ConfigurePlane(W);
	W.StartManager();
	MakeFarFieldSynchronous(W);
	const FVector2D Site(4000.0, -2500.0);
	const double SurfaceZ = FMath::Max(W.Manager->GetGeneratedSurfacePoint(FVector(Site.X, Site.Y, 0.0)).Z, static_cast<double>(W.Config->WaterLevel));
	auto At = [&Site, SurfaceZ](double OverSurface) { return FVector(Site.X, Site.Y, SurfaceZ + OverSurface); };
	// The strategy learns of the far field from its first update's context: settle on the ground before reading the cut-over.
	W.SetViewer(At(300.0));
	TestTrue(TEXT("Settled on the ground"), W.Settle());
	const double Cutover = W.Manager->GetLODStrategy()->GetCutoverAltitude();
	const double Resume = Cutover * FDistanceBandLODStrategy::CutoverResumeFraction;
	TestTrue(TEXT("The cut-over is in force"), Cutover > 0.0);

	struct FDescent
	{
		double ResumedAt = -1.0;
		int32 Resumes = 0, Suspensions = 0, MaxLoaded = 0;
		bool bSuspendedAtEnd = false;
	};
	auto Descend = [&](double From, double To, double SpeedPerTick, int32 HoldTicks)
	{
		FDescent Out;
		const int64 Start = W.Manager->GetCutoverTransitionCount();
		double CameraOver = From;
		int32 Hold = 0;
		while (Hold < HoldTicks)
		{
			if (CameraOver > To)
			{
				CameraOver = FMath::Max(CameraOver - SpeedPerTick, To);
			}
			else
			{
				++Hold;
			}
			W.Manager->SetBenchmarkView(true, At(CameraOver));
			W.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			FPlatformProcess::Sleep(0.002f);
			Out.MaxLoaded = FMath::Max(Out.MaxLoaded, W.Manager->GetLoadedChunkCount());
			if (Out.ResumedAt < 0.0 && !W.Manager->IsVoxelWindowSuspended())
			{
				Out.ResumedAt = CameraOver;
			}
		}
		const TArray<FVoxelCutoverTransition>& Transitions = W.Manager->GetCutoverTransitions();
		const int64 First = W.Manager->GetCutoverTransitionCount() - Transitions.Num();
		for (int32 I = 0; I < Transitions.Num(); ++I)
		{
			if (First + I >= Start)
			{
				(Transitions[I].bSuspended ? Out.Suspensions : Out.Resumes) += 1;
			}
		}
		Out.bSuspendedAtEnd = W.Manager->IsVoxelWindowSuspended();
		return Out;
	};
	auto Describe = [](const FDescent& D)
	{
		return FString::Printf(TEXT("resumed with the camera %.0f over the surface, %d resume(s), %d suspension(s), at most %d chunks loaded, suspended at the end %d"),
			D.ResumedAt, D.Resumes, D.Suspensions, D.MaxLoaded, D.bSuspendedAtEnd ? 1 : 0);
	};
	constexpr double Fast = 300.0; // 18,000 uu/s at 60 Hz

	FDescent StopHigh[2], StopLow[2];
	for (int32 Guard = 0; Guard < 2; ++Guard)
	{
		// By console, as the planet's tests set it (LOD.Planet.CutoverResumeGuard, Streaming.Planet.CutoverDescentStop): a
		// code-priority set after theirs is ignored in a suite run.
		GuardVar->Set(Guard, ECVF_SetByConsole);
		W.SetViewer(At(3.0 * Cutover));
		TestTrue(TEXT("Settled over the cut-over"), W.Settle());
		TestTrue(TEXT("Suspended over the cut-over"), W.Manager->IsVoxelWindowSuspended());
		StopHigh[Guard] = Descend(3.0 * Cutover, 1.4 * Cutover, Fast, 180);
		AddInfo(FString::Printf(TEXT("Guard %d, stop at 1.4 C (%.0f): %s"), Guard, 1.4 * Cutover, *Describe(StopHigh[Guard])));

		W.SetViewer(At(3.0 * Cutover));
		TestTrue(TEXT("Settled over the cut-over again"), W.Settle());
		StopLow[Guard] = Descend(3.0 * Cutover, 0.92 * Cutover, Fast, 180);
		AddInfo(FString::Printf(TEXT("Guard %d, stop at 0.92 C (%.0f): %s"), Guard, 0.92 * Cutover, *Describe(StopLow[Guard])));
		TestTrue(*FString::Printf(TEXT("Guard %d: resumed during the descent"), Guard), StopLow[Guard].Resumes == 1 && StopLow[Guard].ResumedAt > Resume);
		TestTrue(*FString::Printf(TEXT("Guard %d: stays resumed under the cut-over"), Guard), StopLow[Guard].Suspensions == 0 && !StopLow[Guard].bSuspendedAtEnd);
		TestTrue(*FString::Printf(TEXT("Guard %d: settles with its ground disk"), Guard), W.Settle() && !W.Manager->IsVoxelWindowSuspended() && W.Manager->GetLoadedChunkCount() > 20);
	}
	TestTrue(TEXT("Rule before: the lead resumed the window over the cut-over"), StopHigh[0].Resumes >= 1 && StopHigh[0].ResumedAt > Cutover);
	TestTrue(TEXT("Rule before: ... and it suspended again once the camera stopped"), StopHigh[0].Suspensions >= 1 && StopHigh[0].bSuspendedAtEnd);
	TestEqual(TEXT("Guard: no resume"), StopHigh[1].Resumes, 0);
	TestEqual(TEXT("Guard: no suspension"), StopHigh[1].Suspensions, 0);
	TestEqual(TEXT("Guard: no chunk loaded"), StopHigh[1].MaxLoaded, 0);
	TestTrue(TEXT("Guard: suspended throughout"), StopHigh[1].bSuspendedAtEnd);
	TestTrue(TEXT("Rule before: the low stop's resume came with the camera over the cut-over"), StopLow[0].ResumedAt > Cutover);
	TestTrue(TEXT("Guard: the low stop's resume came with the camera at the cut-over (within a tick)"), StopLow[1].ResumedAt <= Cutover && StopLow[1].ResumedAt >= Cutover - Fast);

	GuardVar->Set(*SavedGuard, ECVF_SetByConsole);
	W.Stop();
	return true;
}

/**
 * WM-MP11: the bench's climb and descent on a heightmap world. MakeClimbAlong with +Z: a straight path up from the start
 * altitude over the ground point, the altitude growing by the distance flown, the report's site the point's X and Y;
 * MakeDescentAlong the same path the other way. The planet's MakeClimb records a latitude and longitude as before.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBenchPlaneClimbRouteTest,
	"VoxelWorlds.Bench.Plane.ClimbRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelBenchPlaneClimbRouteTest::RunTest(const FString& Parameters)
{
	const FVector2D Sites[] = { { 0.0, 0.0 }, { 4000.0, -2500.0 }, { -37000.0, 51000.0 } };
	double WorstAltitude = 0.0;
	for (const FVector2D& Site : Sites)
	{
		const FVector Ground(Site.X, Site.Y, 137.5);
		FVoxelBenchConfig Climb;
		FVoxelStreamingBenchmark::MakeClimbAlong(Climb, FVector::UpVector, Ground, 500.0f, Site);
		TestTrue(TEXT("A climb is a straight path up"), Climb.PathType == EVoxelBenchPath::Straight && Climb.bClimb && !Climb.bDescend
			&& Climb.Direction.Equals(FVector::UpVector));
		TestTrue(TEXT("The climb starts 500 over the ground"), Climb.StartPosition.Equals(Ground + FVector(0.0, 0.0, 500.0)));
		TestTrue(TEXT("The report's site is the point's X and Y"), Climb.bClimbSiteXY && Climb.ClimbLatLonAltitude.Equals(FVector(Site.X, Site.Y, 500.0)));
		for (const double S : { 0.0, 1000.0, 250000.0, 2.0e6 })
		{
			const double Altitude = (Climb.StartPosition + Climb.Direction * S).Z - Ground.Z;
			WorstAltitude = FMath::Max(WorstAltitude, FMath::Abs(Altitude - (500.0 + S)));
		}

		FVoxelBenchConfig Descent;
		FVoxelStreamingBenchmark::MakeDescentAlong(Descent, FVector::UpVector, Ground, 40000.0f, Site);
		TestTrue(TEXT("A descent is a straight path down"), Descent.PathType == EVoxelBenchPath::Straight && Descent.bClimb && Descent.bDescend
			&& Descent.Direction.Equals(-FVector::UpVector));
		TestTrue(TEXT("The descent starts 40,000 over the ground"), Descent.StartPosition.Equals(Ground + FVector(0.0, 0.0, 40000.0)));
		TestTrue(TEXT("The descent's report site"), Descent.bClimbSiteXY && Descent.ClimbLatLonAltitude.Equals(FVector(Site.X, Site.Y, 40000.0)));
	}
	AddInfo(FString::Printf(TEXT("Climbs at 3 sites: altitude %.2e uu off start + distance"), WorstAltitude));
	TestTrue(TEXT("The altitude grows by the distance flown"), WorstAltitude < 1.0e-3);

	// The planet's climb keeps its latitude and longitude.
	FVoxelBenchConfig Planet;
	const FVector Center(1000.0, -2000.0, 500.0);
	const FVector Direction = FVoxelCubeSphere::LatLonToDirection(45.0, 20.0);
	FVoxelStreamingBenchmark::MakeClimb(Planet, Center, Center + Direction * 250600.0, 500.0f, FVector2D(45.0, 20.0));
	TestTrue(TEXT("A planet's climb reports a latitude and longitude"), !Planet.bClimbSiteXY && Planet.ClimbLatLonAltitude.Equals(FVector(45.0, 20.0, 500.0))
		&& Planet.Direction.Equals(Direction, 1.0e-9));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
