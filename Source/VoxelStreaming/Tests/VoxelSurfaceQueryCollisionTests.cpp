// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Async/TaskGraphInterfaces.h"
#include "CollisionQueryParams.h"
#include "Engine/World.h"
#include "VoxelChunkManager.h"
#include "VoxelCollisionManager.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelWorldConfiguration.h"
#include "Testing/VoxelTestChunkWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G165 (Documentation/FINDINGS.md): the edit-merged surface query of the heightmap modes read its column from an off-grid
// Z and (X, Y), and put the surface up to a voxel above the mesh and the collision under it (on VoxelDemo an NPC spawn
// point 85 uu over the floor with 75 uu voxels).
//
// - SurfaceQuery.MatchesCollision: a hilly plane off the grid's natural zero, streamed with collision on every loaded
//   chunk, meshed with Dual Contouring and then Marching Cubes. At random (X, Y) the query's height is compared with a
//   complex down-trace of the cooked collision there: within a fraction of a voxel on average and at worst (before the fix
//   the error was a uniform 0 to 1 voxel upward, plus the slope's share of the horizontal offset: +42 uu on average and
//   up to 266 uu with these 75 uu voxels), and the query's slope against the collision triangle's (before: 7.8 degrees
//   on average, 5 to 7 percent of the probes over 20 degrees; after, from the blended height: 2.8 to 5.0, under 2).
// ---------------------------------------------------------------------------

namespace VoxelSurfaceQueryCollisionTest
{
	/** Tick until the stream and the collision pipeline have both been idle for 90 ticks; false on timeout. */
	bool WaitForStreamAndCollision(VoxelTesting::FTestChunkWorld& W, double TimeoutSeconds)
	{
		UVoxelCollisionManager* Collision = W.Manager->GetCollisionManager();
		if (!Collision)
		{
			return false;
		}
		int32 IdleTicks = 0;
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (IdleTicks < 90 && FPlatformTime::Seconds() < Deadline)
		{
			W.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			const bool bIdle = W.IsIdle() && Collision->GetCookQueueCount() == 0 && Collision->GetCookingCount() == 0;
			IdleTicks = bIdle ? IdleTicks + 1 : 0;
			FPlatformProcess::Sleep(0.002f);
		}
		return IdleTicks >= 90;
	}

	struct FPassResult
	{
		bool bSettled = false;
		int32 Probes = 0;       // columns where the query and the trace both found the ground
		int32 QueryMisses = 0;  // the trace found ground, the query did not
		double MeanError = 0.0; // query height minus the collision's, uu (positive: the query is above)
		double MeanAbsError = 0.0;
		double MaxAbsError = 0.0;
		double MaxSlope = 0.0;  // the steepest probed ground, degrees
		double MeanSlopeError = 0.0; // |the query's slope - the collision triangle's|, degrees
		int32 SlopeOutliers = 0;     // probes whose slopes differ by more than 20 degrees
		double MicrosPerQuery = 0.0;
	};

	FPassResult RunPass(EMeshingMode MeshingMode, const TCHAR* WorldName)
	{
		FPassResult R;
		VoxelTesting::FTestChunkWorld W;
		W.Create(WorldName);
		W.ConfigureCommon(75.0f, 16, 3000.0f);
		W.Config->WorldMode = EWorldMode::InfinitePlane;
		W.Config->MeshingMode = MeshingMode;
		// Off the voxel grid's natural zero, as the demo's WorldOrigin (12345, 54321, 0) is.
		W.Config->WorldOrigin = FVector(1234.5, -543.25, 37.5);
		// Hills of a few hundred uu over a few thousand: slopes to about 40 degrees.
		W.Config->HeightScale = 900.0f;
		W.Config->BaseHeight = 0.0f;
		W.Config->NoiseParams.Seed = 9173;
		W.Config->NoiseParams.Frequency = 0.0005f;
		W.Config->bGenerateCollision = true;
		W.Config->CollisionLODLevel = 0;
		W.StartManager();
		UVoxelCollisionManager* Collision = W.Manager->GetCollisionManager();
		if (!Collision)
		{
			W.Stop();
			return R;
		}
		Collision->SetCollisionRadius(W.Config->ViewDistance * 3.0f);

		const FVector Origin = W.Config->WorldOrigin;
		W.SetViewer(W.ViewerOverGround(Origin.X, Origin.Y, 200.0f));
		R.bSettled = W.Settle() && WaitForStreamAndCollision(W, 120.0);

		const double VoxelSize = W.Config->VoxelSize;
		FCollisionQueryParams Params(FName(TEXT("SurfaceQueryCollision")), /*bTraceComplex=*/true);
		FRandomStream Rng(165);
		double ErrorSum = 0.0, AbsSum = 0.0, SlopeErrorSum = 0.0, QuerySeconds = 0.0;
		int32 Queries = 0;
		for (int32 Sample = 0; Sample < 400; ++Sample)
		{
			const double X = Origin.X + Rng.FRandRange(-1500.0f, 1500.0f);
			const double Y = Origin.Y + Rng.FRandRange(-1500.0f, 1500.0f);
			const double Generated = W.Manager->GetGeneratedSurfacePoint(FVector(X, Y, 0.0)).Z;

			FHitResult Hit;
			const FVector Start(X, Y, Generated + 4.0 * VoxelSize);
			const FVector End(X, Y, Generated - 4.0 * VoxelSize);
			if (!W.World->LineTraceSingleByObjectType(Hit, Start, End, FCollisionObjectQueryParams(ECC_WorldStatic), Params))
			{
				continue; // no cooked ground here
			}

			FVoxelSurfaceSample Surface;
			const double T0 = FPlatformTime::Seconds();
			const bool bFound = W.Manager->QueryEditMergedSurfacePoint(FVector(X, Y, 0.0), Surface);
			QuerySeconds += FPlatformTime::Seconds() - T0;
			++Queries;
			if (!bFound)
			{
				++R.QueryMisses;
				continue;
			}

			const double Error = Surface.Position.Z - Hit.ImpactPoint.Z;
			ErrorSum += Error;
			AbsSum += FMath::Abs(Error);
			R.MaxAbsError = FMath::Max(R.MaxAbsError, FMath::Abs(Error));
			R.MaxSlope = FMath::Max(R.MaxSlope, static_cast<double>(Surface.SlopeDegrees));
			const double CollisionSlope = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Hit.ImpactNormal.Z, -1.0, 1.0)));
			const double SlopeError = FMath::Abs(Surface.SlopeDegrees - CollisionSlope);
			SlopeErrorSum += SlopeError;
			R.SlopeOutliers += SlopeError > 20.0 ? 1 : 0;
			++R.Probes;
		}
		if (R.Probes > 0)
		{
			R.MeanError = ErrorSum / R.Probes;
			R.MeanAbsError = AbsSum / R.Probes;
			R.MeanSlopeError = SlopeErrorSum / R.Probes;
		}
		R.MicrosPerQuery = Queries > 0 ? QuerySeconds * 1.0e6 / Queries : 0.0;

		W.Stop();
		return R;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSurfaceQueryMatchesCollisionTest, "VoxelWorlds.Streaming.SurfaceQuery.MatchesCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelSurfaceQueryMatchesCollisionTest::RunTest(const FString& Parameters)
{
	using namespace VoxelSurfaceQueryCollisionTest;

	// Voxels of 75 uu: the bounds below are a fifth of a voxel on average and two fifths at worst, where the mesh between
	// the grid columns bends (Dual Contouring places its vertices inside the cells).
	constexpr double VoxelSize = 75.0;
	const TPair<EMeshingMode, const TCHAR*> Passes[] = {
		{ EMeshingMode::DualContouring, TEXT("Dual Contouring") },
		{ EMeshingMode::MarchingCubes, TEXT("Marching Cubes") } };
	for (const TPair<EMeshingMode, const TCHAR*>& Pass : Passes)
	{
		const FPassResult R = RunPass(Pass.Key, *FString::Printf(TEXT("SurfaceQueryCollision%d"), static_cast<int32>(Pass.Key)));
		AddInfo(FString::Printf(TEXT("%s: %d probes (%d the query missed), error mean %+.1f uu, mean |error| %.1f uu, max |error| %.1f uu, slopes to %.0f degrees (against the collision's: mean |difference| %.1f degrees, %d over 20), %.1f us a query"),
			Pass.Value, R.Probes, R.QueryMisses, R.MeanError, R.MeanAbsError, R.MaxAbsError, R.MaxSlope, R.MeanSlopeError, R.SlopeOutliers, R.MicrosPerQuery));
		TestTrue(FString::Printf(TEXT("%s: streaming and collision settle"), Pass.Value), R.bSettled);
		TestTrue(FString::Printf(TEXT("%s: enough probes (non-vacuous)"), Pass.Value), R.Probes >= 300);
		TestEqual(FString::Printf(TEXT("%s: the query finds the ground wherever the collision has it"), Pass.Value), R.QueryMisses, 0);
		TestTrue(FString::Printf(TEXT("%s: no bias against the collision"), Pass.Value), FMath::Abs(R.MeanError) <= 0.1 * VoxelSize);
		TestTrue(FString::Printf(TEXT("%s: on the collision on average"), Pass.Value), R.MeanAbsError <= 0.2 * VoxelSize);
		TestTrue(FString::Printf(TEXT("%s: on the collision at worst"), Pass.Value), R.MaxAbsError <= 0.4 * VoxelSize);
		TestTrue(FString::Printf(TEXT("%s: the slope is the collision's"), Pass.Value), R.MeanSlopeError <= 7.5);
		TestTrue(FString::Printf(TEXT("%s: few slopes off by more than 20 degrees"), Pass.Value), R.SlopeOutliers * 20 <= R.Probes);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
