// Copyright Daniel Raquel. All Rights Reserved.

// A server has ground under every player (G217, Documentation/FINDINGS.md): the voxel world streamed its chunks and
// cooked its collision around one viewer, the first player's, so a second player beyond the first's collision radius
// had no ground on the server. Every player's pawn beside the first is now a further viewer with a ground window: the
// chunks its collision shell cooks from, loaded and kept around it.
//
// - Streaming.FurtherViewers.Plane / .Planet: a streaming viewer at A and further viewers far from it. One viewer has
//   nothing at B; a further viewer at B gets the collision a streaming viewer standing there gets (the same chunks
//   cooked, the same ground under nine probes), with the voxels those cooks read; the streaming viewer's own window and
//   shell do not change; a chunk two viewers need is loaded once and released when the last of them leaves; an edit
//   made where nobody was shows in the collision when a viewer comes, and one made under a further viewer re-cooks;
//   ground chunks are not far-compressed and the residency audit finds nothing; with no further viewer left the world
//   holds what one viewer's did. The tick's cost with 1, 2 and 4 viewers is reported.
// - Streaming.FurtherViewers.Scatter: the scatter manager measures from the nearest viewer: a further viewer's chunks
//   hold the spawn points a streaming viewer's there do (the scatter a server's player collides with and chops), and
//   they go with the viewer.
// - Streaming.FurtherViewers.Players: the further viewers are the players' pawns: a player with no pawn is none, the
//   second player's pawn far from the first gets ground, a player who dies or leaves releases it, the first player
//   leaving makes the second the streaming viewer, and voxel.Stream.GroundWindows 0 is the first player only.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Async/TaskGraphInterfaces.h"
#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"
#include "Engine/World.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "VoxelChunkManager.h"
#include "VoxelCollisionManager.h"
#include "VoxelEditManager.h"
#include "VoxelScatterManager.h"
#include "VoxelWorldConfiguration.h"
#include "Testing/VoxelScatterTestDefinitions.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "Testing/VoxelTestScopedCVar.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace VoxelFurtherViewersTest
{
	/** How far over the ground a viewer stands. */
	constexpr double ViewerHeight = 200.0;

	/** Tick until the stream and the collision pipeline have both been idle for 90 ticks; false on timeout. */
	bool WaitForStreamAndCollision(VoxelTesting::FTestChunkWorld& W, double TimeoutSeconds = 180.0)
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

	/** A plane or a small planet with three LOD bands, collision at LOD 0 within half the view distance. */
	void Configure(VoxelTesting::FTestChunkWorld& W, bool bPlanet)
	{
		if (bPlanet)
		{
			W.ConfigureSmallPlanet();
		}
		else
		{
			W.ConfigureSmallPlane();
		}
		W.ConfigureThreeLODBands();
		W.Config->bGenerateCollision = true;
		W.Config->CollisionLODLevel = 0;
	}

	/** A point on the generated ground under (or over) a position, and a viewer over it. */
	FVector GroundAt(const VoxelTesting::FTestChunkWorld& W, const FVector& Position)
	{
		return W.Manager->GetGeneratedSurfacePoint(Position);
	}

	FVector ViewerOver(const VoxelTesting::FTestChunkWorld& W, const FVector& Ground)
	{
		return Ground + W.Manager->GetUpVectorAt(Ground) * ViewerHeight;
	}

	FVector ChunkCentre(const VoxelTesting::FTestChunkWorld& W, const FIntVector& Coord)
	{
		const double ChunkWorld = W.Config->GetChunkWorldSize();
		return W.Config->WorldOrigin + FVector(Coord) * ChunkWorld + FVector(ChunkWorld * 0.5);
	}

	/** What the world has around a viewer. */
	struct FGround
	{
		/** Every loaded chunk of the world */
		TSet<FIntVector> Loaded;
		/** The loaded chunks whose centre lies within the collision shell's radius of the viewer and that have collision */
		TSet<FIntVector> Cooked;
		/** The loaded chunks whose centre lies within the shell's radius plus a chunk's diagonal (what the cooks read) */
		TSet<FIntVector> Near;
		/** Nine probes traced down onto the collision around the site: the impact point, or FLT_MAX where nothing was hit */
		TArray<FVector> Hits;

		int32 NumHits() const
		{
			int32 Count = 0;
			for (const FVector& Hit : Hits)
			{
				Count += Hit.X != FLT_MAX ? 1 : 0;
			}
			return Count;
		}
	};

	FGround ReadGround(VoxelTesting::FTestChunkWorld& W, const FVector& Site)
	{
		FGround G;
		const UVoxelCollisionManager* Collision = W.Manager->GetCollisionManager();
		const FVector Viewer = ViewerOver(W, Site);
		const double Radius = Collision->GetCollisionRadius();
		const double Diagonal = UE_DOUBLE_SQRT_3 * W.Config->GetChunkWorldSize();
		TArray<FIntVector> Loaded;
		W.Manager->GetLoadedChunks(Loaded);
		for (const FIntVector& Coord : Loaded)
		{
			G.Loaded.Add(Coord);
			const double Distance = FVector::Dist(ChunkCentre(W, Coord), Viewer);
			if (Distance <= Radius + Diagonal)
			{
				G.Near.Add(Coord);
			}
			if (Distance <= Radius && Collision->HasCollision(Coord))
			{
				G.Cooked.Add(Coord);
			}
		}
		const FVector Up = W.Manager->GetUpVectorAt(Site);
		FVector AxisA, AxisB;
		Up.FindBestAxisVectors(AxisA, AxisB);
		FCollisionQueryParams Params(FName(TEXT("FurtherViewers")), /*bTraceComplex=*/true);
		for (int32 J = -1; J <= 1; ++J)
		{
			for (int32 I = -1; I <= 1; ++I)
			{
				const FVector Ground = GroundAt(W, Site + (AxisA * I + AxisB * J) * 300.0);
				const FVector ProbeUp = W.Manager->GetUpVectorAt(Ground);
				FHitResult Hit;
				const bool bHit = W.World->LineTraceSingleByObjectType(Hit, Ground + ProbeUp * 800.0, Ground - ProbeUp * 800.0,
					FCollisionObjectQueryParams(ECC_WorldStatic), Params);
				G.Hits.Add(bHit ? FVector(Hit.ImpactPoint) : FVector(FLT_MAX));
			}
		}
		return G;
	}

	bool SameSet(const TSet<FIntVector>& A, const TSet<FIntVector>& B)
	{
		return A.Num() == B.Num() && A.Includes(B);
	}

	/** The largest distance between two readings' probes (FLT_MAX when one hit and the other did not). */
	double ProbeDifference(const FGround& A, const FGround& B)
	{
		double Worst = 0.0;
		for (int32 Index = 0; Index < FMath::Min(A.Hits.Num(), B.Hits.Num()); ++Index)
		{
			const bool bA = A.Hits[Index].X != FLT_MAX;
			const bool bB = B.Hits[Index].X != FLT_MAX;
			Worst = FMath::Max(Worst, bA != bB ? static_cast<double>(FLT_MAX) : (bA ? FVector::Dist(A.Hits[Index], B.Hits[Index]) : 0.0));
		}
		return Worst;
	}

	/** A player's sphere brush (a dig) of RadiusVoxels at Center. */
	int32 Dig(VoxelTesting::FTestChunkWorld& W, const FVector& Center, float RadiusVoxels)
	{
		UVoxelEditManager* Edits = W.Manager->GetEditManager();
		const EEditSource Previous = Edits->GetEditSource();
		Edits->SetEditSource(EEditSource::Player);
		FVoxelBrushParams Brush;
		Brush.Shape = EVoxelBrushShape::Sphere;
		Brush.Radius = RadiusVoxels * W.Config->VoxelSize;
		Brush.Strength = 1.0f;
		Brush.FalloffType = EVoxelBrushFalloff::Sharp;
		Brush.DensityDelta = 255;
		Brush.MaterialID = EVoxelMaterial::Stone;
		const int32 Count = Edits->ApplyBrushEdit(Center, Brush, EEditMode::Subtract);
		Edits->SetEditSource(Previous);
		return Count;
	}

	/** How far under a site's generated ground the collision's ground is at the site's middle probe (FLT_MAX: no ground). */
	double DepthUnder(const VoxelTesting::FTestChunkWorld& W, const FGround& G, const FVector& Site)
	{
		const FVector& Hit = G.Hits[4];
		return Hit.X == FLT_MAX ? static_cast<double>(FLT_MAX) : FVector::DotProduct(Site - Hit, W.Manager->GetUpVectorAt(Site));
	}

	/** Set the viewers and settle; false when the world does not settle. */
	bool Stream(VoxelTesting::FTestChunkWorld& W, const FVector& Viewer, const TArray<FVector>& Further)
	{
		W.SetViewer(Viewer);
		W.SetFurtherViewers(Further);
		return W.Settle() && WaitForStreamAndCollision(W);
	}

	void RunRules(FAutomationTestBase& Test, bool bPlanet)
	{
		const TCHAR* Mode = bPlanet ? TEXT("planet") : TEXT("plane");
		VoxelTesting::FTestChunkWorld W;
		W.Create(bPlanet ? TEXT("FurtherViewersPlanet") : TEXT("FurtherViewersPlane"));
		Configure(W, bPlanet);
		W.StartManager();
		UVoxelCollisionManager* Collision = W.Manager->GetCollisionManager();
		if (!Test.TestNotNull(TEXT("The chunk manager has a collision manager"), Collision))
		{
			W.Stop();
			return;
		}
		const double ChunkWorld = W.Config->GetChunkWorldSize();
		const double Radius = Collision->GetCollisionRadius();

		// Three sites far apart: many view distances on the plane, a third of the way round the planet.
		FVector SiteA, SiteB, SiteC;
		if (bPlanet)
		{
			const FVector Centre = W.Config->WorldOrigin;
			const double Far = 4.0 * W.Config->WorldRadius;
			SiteA = GroundAt(W, Centre + FVector(0.3, 0.2, 0.93).GetSafeNormal() * Far);
			SiteB = GroundAt(W, Centre + FVector(0.9, -0.35, -0.2).GetSafeNormal() * Far);
			SiteC = GroundAt(W, Centre + FVector(-0.5, 0.8, -0.3).GetSafeNormal() * Far);
		}
		else
		{
			SiteA = GroundAt(W, FVector(800.0, 700.0, 0.0));
			SiteB = GroundAt(W, FVector(800.0 + 60.0 * ChunkWorld, 700.0, 0.0));
			SiteC = GroundAt(W, FVector(800.0, 700.0 - 60.0 * ChunkWorld, 0.0));
		}
		const FVector ViewA = ViewerOver(W, SiteA);
		const FVector ViewB = ViewerOver(W, SiteB);
		const FVector ViewC = ViewerOver(W, SiteC);
		Test.TestTrue(TEXT("The sites lie beyond each other's unload distance (non-vacuous)"),
			FVector::Dist(SiteA, SiteB) > 3.0 * W.Config->ViewDistance && FVector::Dist(SiteA, SiteC) > 3.0 * W.Config->ViewDistance
			&& FVector::Dist(SiteB, SiteC) > 3.0 * W.Config->ViewDistance);

		// One viewer at A: ground at A, nothing at B.
		Test.TestTrue(TEXT("One viewer at A settles"), Stream(W, ViewA, {}));
		const FGround OnlyA = ReadGround(W, SiteA);
		const FGround NothingAtB = ReadGround(W, SiteB);
		Test.TestEqual(TEXT("One viewer: no further viewer"), W.Manager->GetFurtherViewers().Num(), 0);
		Test.TestEqual(TEXT("One viewer: no ground window"), W.Manager->GetGroundWindowKeepCount(), 0);
		Test.TestTrue(TEXT("One viewer at A: ground under A (non-vacuous)"), OnlyA.Cooked.Num() > 0 && OnlyA.NumHits() == 9);
		Test.TestTrue(TEXT("One viewer at A: no chunk, no collision and no ground at B (how a server's second player stood before)"),
			NothingAtB.Near.Num() == 0 && NothingAtB.Cooked.Num() == 0 && NothingAtB.NumHits() == 0);

		// What a streaming viewer standing at B gets: the reference for a further viewer there.
		Test.TestTrue(TEXT("One viewer at B settles"), Stream(W, ViewB, {}));
		const FGround ReferenceB = ReadGround(W, SiteB);
		Test.TestTrue(TEXT("One viewer at B: ground under B (non-vacuous)"), ReferenceB.Cooked.Num() > 0 && ReferenceB.NumHits() == 9);
		Test.TestTrue(TEXT("Back to one viewer at A settles"), Stream(W, ViewA, {}));
		const FGround BackAtA = ReadGround(W, SiteA);
		Test.TestTrue(TEXT("One viewer back at A holds what it did"), SameSet(BackAtA.Loaded, OnlyA.Loaded) && SameSet(BackAtA.Cooked, OnlyA.Cooked));

		// A further viewer at B.
		Test.TestTrue(TEXT("A further viewer at B settles"), Stream(W, ViewA, { ViewB }));
		const FGround WithB = ReadGround(W, SiteB);
		const FGround AWithB = ReadGround(W, SiteA);
		Test.TestEqual(TEXT("A further viewer"), W.Manager->GetFurtherViewers().Num(), 1);
		Test.TestTrue(TEXT("The further viewer's shell is cooked as a streaming viewer's there: the same chunks"), SameSet(WithB.Cooked, ReferenceB.Cooked));
		Test.TestTrue(TEXT("... from the voxels a streaming viewer's cooks read (every chunk within the shell and a chunk's diagonal)"), WithB.Near.Includes(ReferenceB.Near));
		Test.TestEqual(TEXT("... with ground under every probe"), WithB.NumHits(), 9);
		Test.TestTrue(TEXT("... the same ground"), ProbeDifference(WithB, ReferenceB) <= 1.0);
		Test.TestTrue(TEXT("The streaming viewer keeps its window: every chunk it had"), AWithB.Loaded.Includes(OnlyA.Loaded));
		Test.TestTrue(TEXT("... and its shell"), SameSet(AWithB.Cooked, OnlyA.Cooked) && ProbeDifference(AWithB, OnlyA) <= 0.01);
		int32 OutsideBoth = 0;
		for (const FIntVector& Coord : AWithB.Loaded)
		{
			OutsideBoth += (!OnlyA.Loaded.Contains(Coord) && !W.Manager->IsChunkInGroundWindow(Coord)) ? 1 : 0;
		}
		Test.TestEqual(TEXT("Every chunk loaded beside the streaming viewer's is in the ground window"), OutsideBoth, 0);
		const int32 GroundChunks = AWithB.Loaded.Num() - OnlyA.Loaded.Num();
		Test.TestTrue(TEXT("The ground window loaded chunks (non-vacuous)"), GroundChunks > 0);
		FString Audit;
		Test.TestEqual(TEXT("The residency audit finds nothing with a further viewer"), W.Manager->AuditResidency(Audit), 0);
		Test.AddInfo(FString::Printf(TEXT("%s: shell radius %.0f, chunks of %.0f; one viewer %d chunks loaded (%d cooked in its shell); a further viewer adds %d (%d to load, %d kept; %d cooked in its shell)"),
			Mode, Radius, ChunkWorld, OnlyA.Loaded.Num(), OnlyA.Cooked.Num(), GroundChunks, W.Manager->GetGroundWindowLoadCount(),
			W.Manager->GetGroundWindowKeepCount(), WithB.Cooked.Num()));

		// Ground chunks stay resident: far from the streaming viewer, in a band far compression takes, and near a player.
		for (int32 Tick = 0; Tick < 400; ++Tick)
		{
			W.Tick();
		}
		Test.TestTrue(TEXT("Settled again after the idle ticks"), WaitForStreamAndCollision(W));
		const FGround IdleB = ReadGround(W, SiteB);
		int32 GroundCompressed = 0;
		int32 OwnCompressed = 0;
		for (const FIntVector& Coord : IdleB.Loaded)
		{
			if (W.Manager->IsChunkFarCompressed(Coord))
			{
				++(OnlyA.Loaded.Contains(Coord) ? OwnCompressed : GroundCompressed);
			}
		}
		Test.TestEqual(TEXT("No ground chunk is far-compressed after 400 idle ticks"), GroundCompressed, 0);
		Test.TestTrue(TEXT("... while the streaming viewer's far band is (non-vacuous)"), OwnCompressed > 0);
		Test.TestTrue(TEXT("After 400 idle ticks the further viewer's ground is the same"), SameSet(IdleB.Cooked, ReferenceB.Cooked) && ProbeDifference(IdleB, ReferenceB) <= 1.0);

		// The same viewer twice: nothing twice.
		Test.TestTrue(TEXT("Two further viewers at B settle"), Stream(W, ViewA, { ViewB, ViewB }));
		const FGround TwiceB = ReadGround(W, SiteB);
		Test.TestTrue(TEXT("A second further viewer at the same place loads and cooks nothing more"),
			SameSet(TwiceB.Loaded, AWithB.Loaded) && SameSet(TwiceB.Cooked, WithB.Cooked));
		Test.TestTrue(TEXT("One of the two leaving releases nothing"), Stream(W, ViewA, { ViewB }) && SameSet(ReadGround(W, SiteB).Cooked, WithB.Cooked));

		// Two further viewers far apart; the one leaving releases its own only.
		Test.TestTrue(TEXT("Further viewers at B and C settle"), Stream(W, ViewA, { ViewB, ViewC }));
		const FGround WithC = ReadGround(W, SiteC);
		Test.TestTrue(TEXT("Both have ground"), WithC.NumHits() == 9 && WithC.Cooked.Num() > 0 && ReadGround(W, SiteB).NumHits() == 9);
		Test.TestTrue(TEXT("B's viewer gone settles"), Stream(W, ViewA, { ViewC }));
		const FGround BGone = ReadGround(W, SiteB);
		const FGround CStays = ReadGround(W, SiteC);
		Test.TestTrue(TEXT("B's chunks and collision are released"), BGone.Near.Num() == 0 && BGone.Cooked.Num() == 0 && BGone.NumHits() == 0);
		Test.TestTrue(TEXT("... and C's stay"), SameSet(CStays.Cooked, WithC.Cooked) && ProbeDifference(CStays, WithC) <= 0.01);

		// Edits: one made at B while nobody is there (the edit layer needs no loaded chunk), one under the further viewer at C.
		const double Voxel = W.Config->VoxelSize;
		const double UndugB = DepthUnder(W, WithB, SiteB);
		const double UndugC = DepthUnder(W, WithC, SiteC);
		const int32 DugAtB = Dig(W, SiteB, 4.0f);
		Test.TestTrue(TEXT("A dig at B with no chunk loaded there changes voxels"), DugAtB > 0);
		Test.TestTrue(TEXT("Further viewers at B and C settle after the dig"), Stream(W, ViewA, { ViewB, ViewC }));
		const FGround DugB = ReadGround(W, SiteB);
		Test.TestTrue(TEXT("A viewer coming to B collides with the dug ground"), DepthUnder(W, DugB, SiteB) >= UndugB + 0.5 * Voxel && DepthUnder(W, DugB, SiteB) != FLT_MAX);
		const int32 DugAtC = Dig(W, SiteC, 4.0f);
		Test.TestTrue(TEXT("A dig under the further viewer at C changes voxels"), DugAtC > 0);
		Test.TestTrue(TEXT("The dig under C settles"), W.Settle() && WaitForStreamAndCollision(W));
		const FGround DugC = ReadGround(W, SiteC);
		Test.TestTrue(TEXT("The collision under the further viewer is re-cooked for its edit"), DepthUnder(W, DugC, SiteC) >= UndugC + 0.5 * Voxel && DepthUnder(W, DugC, SiteC) != FLT_MAX);
		Test.AddInfo(FString::Printf(TEXT("%s: the collision's ground under the generated surface: at B %.0f uu before the dig and %.0f after, at C %.0f and %.0f (a brush of 4 voxels of %.0f uu)"),
			Mode, UndugB, DepthUnder(W, DugB, SiteB), UndugC, DepthUnder(W, DugC, SiteC), Voxel));

		// A further viewer walking (600 uu/s for six chunk widths): the window goes with it, the chunk under its feet has
		// collision at every tick, and what it left behind is released.
		{
			FVector AxisA, AxisB;
			W.Manager->GetUpVectorAt(SiteB).FindBestAxisVectors(AxisA, AxisB);
			const double StepLength = 600.0 / 60.0;
			const int64 RebuildsBefore = W.Manager->GetGroundWindowRebuilds();
			FVector Walker = SiteB;
			int32 Steps = 0;
			int32 Uncooked = 0;
			for (double Walked = 0.0; Walked < 6.0 * ChunkWorld; Walked += StepLength)
			{
				Walker = GroundAt(W, Walker + AxisA * StepLength);
				W.SetFurtherViewers({ ViewerOver(W, Walker), ViewC });
				W.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
				FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
				FPlatformProcess::Sleep(0.002f);
				++Steps;
				Uncooked += Collision->HasCollision(W.Manager->WorldToChunkCoord(Walker - W.Manager->GetUpVectorAt(Walker) * 25.0)) ? 0 : 1;
			}
			Test.TestEqual(TEXT("A walking further viewer has collision in the chunk under its feet at every step"), Uncooked, 0);
			Test.TestTrue(TEXT("The walk settles"), W.Settle() && WaitForStreamAndCollision(W));
			const FGround Walked = ReadGround(W, Walker);
			Test.TestTrue(TEXT("... and ground where it stops"), Walked.NumHits() == 9 && Walked.Cooked.Num() > 0);
			Test.TestFalse(TEXT("... while the chunk it started on is released"), W.Manager->IsChunkLoaded(W.Manager->WorldToChunkCoord(SiteB - W.Manager->GetUpVectorAt(SiteB) * 25.0)));
			Test.AddInfo(FString::Printf(TEXT("%s: a walk of %.0f uu in %d ticks worked the ground windows out %lld times"), Mode, 6.0 * ChunkWorld, Steps,
				W.Manager->GetGroundWindowRebuilds() - RebuildsBefore));
		}

		// What a tick costs as viewers are added: the streaming decisions (the ground windows among them) and the windows alone.
		const FVector SiteD = GroundAt(W, bPlanet ? W.Config->WorldOrigin - (SiteA - W.Config->WorldOrigin) * 4.0 : SiteA - FVector(60.0 * ChunkWorld, 60.0 * ChunkWorld, 0.0));
		const TArray<FVector> ViewerSets[] = { {}, { ViewB }, { ViewB, ViewC, ViewerOver(W, SiteD) } };
		FString Cost;
		for (const TArray<FVector>& Further : ViewerSets)
		{
			Test.TestTrue(TEXT("The cost run settles"), Stream(W, ViewA, Further));
			double StreamingMs = 0.0, GroundMs = 0.0, CollisionMs = 0.0;
			constexpr int32 Ticks = 240;
			for (int32 Tick = 0; Tick < Ticks; ++Tick)
			{
				W.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
				StreamingMs += W.Manager->GetTimingStats().StreamingMs;
				CollisionMs += W.Manager->GetTimingStats().CollisionMs;
				GroundMs += W.Manager->GetLastGroundWindowMs();
			}
			Cost += FString::Printf(TEXT("%s%d viewer(s): %d chunks loaded, streaming decisions %.3f ms a tick (the ground windows %.3f), collision %.3f"),
				Cost.IsEmpty() ? TEXT("") : TEXT("; "), 1 + Further.Num(), W.Manager->GetLoadedChunkCount(), StreamingMs / Ticks, GroundMs / Ticks, CollisionMs / Ticks);
		}
		Test.AddInfo(FString::Printf(TEXT("%s, a steady tick: %s"), Mode, *Cost));
		Test.AddInfo(FString::Printf(TEXT("%s: after 400 idle ticks %d of the streaming viewer's chunks were far-compressed and %d of the ground window's"), Mode, OwnCompressed, GroundCompressed));

		// No further viewer left: one viewer's world, with its edits' chunks out of reach unloaded.
		Test.TestTrue(TEXT("No further viewer settles"), Stream(W, ViewA, {}));
		const FGround Released = ReadGround(W, SiteA);
		Test.TestEqual(TEXT("No further viewer: no ground window"), W.Manager->GetGroundWindowKeepCount(), 0);
		Test.TestTrue(TEXT("... the world holds what one viewer's did"), SameSet(Released.Loaded, OnlyA.Loaded) && SameSet(Released.Cooked, OnlyA.Cooked));
		Test.TestTrue(TEXT("... with the same ground under A"), ProbeDifference(Released, OnlyA) <= 0.01);
		Test.TestEqual(TEXT("... and nothing at C"), ReadGround(W, SiteC).Near.Num(), 0);
		Test.TestEqual(TEXT("The residency audit finds nothing after the release"), W.Manager->AuditResidency(Audit), 0);
		W.SetFurtherViewers({});
		W.Stop();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFurtherViewersPlaneTest, "VoxelWorlds.Streaming.FurtherViewers.Plane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelFurtherViewersPlaneTest::RunTest(const FString& Parameters)
{
	VoxelFurtherViewersTest::RunRules(*this, /*bPlanet=*/false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFurtherViewersPlanetTest, "VoxelWorlds.Streaming.FurtherViewers.Planet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelFurtherViewersPlanetTest::RunTest(const FString& Parameters)
{
	VoxelFurtherViewersTest::RunRules(*this, /*bPlanet=*/true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFurtherViewersScatterTest, "VoxelWorlds.Streaming.FurtherViewers.Scatter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelFurtherViewersScatterTest::RunTest(const FString& Parameters)
{
	using namespace VoxelFurtherViewersTest;
	VoxelTesting::FTestChunkWorld W;
	W.Create(TEXT("FurtherViewersScatter"));
	W.ConfigureSmallPlane();
	W.Config->bGenerateCollision = true;
	W.Config->CollisionLODLevel = 0;
	W.Config->bEnableScatter = true;
	W.Config->ScatterRadius = 1500.0f;
	W.StartManager();
	UVoxelScatterManager* Scatter = W.Manager->GetScatterManager();
	if (!TestNotNull(TEXT("The chunk manager has a scatter manager"), Scatter) || !TestNotNull(TEXT("... and a collision manager"), W.Manager->GetCollisionManager()))
	{
		W.Stop();
		return false;
	}
	// Grass and rocks at the scatter radius; the trees within it too (their default reaches past this small world's window).
	VoxelScatterTesting::AddSmoothDefinitions(*Scatter);
	Scatter->SetSurfacePointSpacing(W.Config->VoxelSize * 2.0f);
	if (const FScatterDefinition* Trees = Scatter->GetScatterDefinition(2))
	{
		FScatterDefinition Copy = *Trees;
		Copy.SpawnDistance = 1500.0f;
		Scatter->AddScatterDefinition(Copy);
	}

	const double ChunkWorld = W.Config->GetChunkWorldSize();
	const FVector SiteA = GroundAt(W, FVector(800.0, 700.0, 0.0));
	const FVector SiteB = GroundAt(W, FVector(800.0 + 60.0 * ChunkWorld, 700.0, 0.0));
	const FVector ViewA = ViewerOver(W, SiteA);
	const FVector ViewB = ViewerOver(W, SiteB);

	// The pipeline, the collision and the scatter's extractions idle, with a few of the scatter's distance passes run.
	const auto Settled = [&W, Scatter](const FVector& Viewer, const TArray<FVector>& Further)
	{
		W.SetViewer(Viewer);
		W.SetFurtherViewers(Further);
		bool bSettled = true;
		for (int32 Pass = 0; Pass < 3 && bSettled; ++Pass)
		{
			int32 IdleTicks = 0;
			const double Deadline = FPlatformTime::Seconds() + 180.0;
			while (IdleTicks < 90 && FPlatformTime::Seconds() < Deadline)
			{
				W.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
				FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
				IdleTicks = (W.IsIdle() && Scatter->GetPendingGenerationCount() == 0) ? IdleTicks + 1 : 0;
				FPlatformProcess::Sleep(0.002f);
			}
			bSettled = IdleTicks >= 90;
		}
		return bSettled;
	};
	// The spawn points held by the chunks whose centre lies within the scatter radius of a viewer over a site.
	const auto SpawnPointsNear = [&W, Scatter](const FVector& Site)
	{
		const FVector Viewer = ViewerOver(W, Site);
		TArray<FIntVector> Loaded;
		W.Manager->GetLoadedChunks(Loaded);
		int32 Points = 0;
		for (const FIntVector& Coord : Loaded)
		{
			const FChunkScatterData* Data = Scatter->GetChunkScatterData(Coord);
			if (Data && FVector::Dist(ChunkCentre(W, Coord), Viewer) <= Scatter->GetScatterRadius())
			{
				Points += Data->SpawnPoints.Num();
			}
		}
		return Points;
	};

	TestTrue(TEXT("One viewer at A settles"), Settled(ViewA, {}));
	const int32 AtA = SpawnPointsNear(SiteA);
	TestTrue(TEXT("One viewer at A: scatter around A (non-vacuous)"), AtA > 0);
	TestEqual(TEXT("One viewer at A: none at B"), SpawnPointsNear(SiteB), 0);

	TestTrue(TEXT("One viewer at B settles"), Settled(ViewB, {}));
	const int32 ReferenceB = SpawnPointsNear(SiteB);
	TestTrue(TEXT("One viewer at B: scatter around B (non-vacuous)"), ReferenceB > 0);

	TestTrue(TEXT("A viewer at A and a further viewer at B settle"), Settled(ViewA, { ViewB }));
	TestEqual(TEXT("The further viewer's chunks hold the spawn points a streaming viewer's there do"), SpawnPointsNear(SiteB), ReferenceB);
	TestEqual(TEXT("... and the streaming viewer's hold what they did"), SpawnPointsNear(SiteA), AtA);
	AddInfo(FString::Printf(TEXT("%d spawn points within the scatter radius of A, %d of B (as a streaming viewer there: %d)"), AtA, SpawnPointsNear(SiteB), ReferenceB));

	TestTrue(TEXT("No further viewer settles"), Settled(ViewA, {}));
	TestEqual(TEXT("The further viewer gone: no scatter at B"), SpawnPointsNear(SiteB), 0);
	TestEqual(TEXT("... and A's as before"), SpawnPointsNear(SiteA), AtA);
	W.SetFurtherViewers({});
	W.Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFurtherViewersPlayersTest, "VoxelWorlds.Streaming.FurtherViewers.Players",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelFurtherViewersPlayersTest::RunTest(const FString& Parameters)
{
	using namespace VoxelFurtherViewersTest;
	VoxelTesting::FTestChunkWorld W;
	// Actors initialised for play: a player controller spawns its camera manager and joins the world's controllers.
	W.Create(TEXT("FurtherViewersPlayers"), /*bInitializeActorsForPlay=*/true);
	Configure(W, /*bPlanet=*/false);
	W.StartManager();
	UVoxelCollisionManager* Collision = W.Manager->GetCollisionManager();
	if (!TestNotNull(TEXT("The chunk manager has a collision manager"), Collision))
	{
		W.Stop();
		return false;
	}
	const double ChunkWorld = W.Config->GetChunkWorldSize();
	const FVector SiteA = GroundAt(W, FVector(800.0, 700.0, 0.0));
	const FVector SiteB = GroundAt(W, FVector(800.0 + 60.0 * ChunkWorld, 700.0, 0.0));

	FActorSpawnParameters Always;
	Always.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	// A pawn the collision cannot push: the test reads the ground under where it was put.
	const auto SpawnPawnOver = [&](const FVector& Site)
	{
		ADefaultPawn* Pawn = W.World->SpawnActor<ADefaultPawn>(ADefaultPawn::StaticClass(), ViewerOver(W, Site), FRotator::ZeroRotator, Always);
		if (Pawn)
		{
			Pawn->SetActorEnableCollision(false);
		}
		return Pawn;
	};
	// A player's camera at its pawn, as a frame's camera update leaves it (the streaming follows the first player's camera).
	const auto LookFrom = [](APlayerController* PC, APawn* Pawn)
	{
		PC->SetPawn(Pawn);
		if (PC->PlayerCameraManager)
		{
			PC->PlayerCameraManager->bUseClientSideCameraUpdates = false;
			PC->SetViewTarget(Pawn ? static_cast<AActor*>(Pawn) : PC);
			PC->PlayerCameraManager->UpdateCamera(0.0f);
		}
	};
	const auto Settled = [&W]() { return W.Settle() && WaitForStreamAndCollision(W); };

	// The first player at A: one viewer, as a standalone game's.
	APlayerController* First = W.World->SpawnActor<APlayerController>();
	if (!TestTrue(TEXT("The first player's controller is the world's first, with a camera manager"),
		First && W.World->GetFirstPlayerController() == First && First->PlayerCameraManager))
	{
		W.Stop();
		return false;
	}
	APawn* FirstPawn = SpawnPawnOver(SiteA);
	LookFrom(First, FirstPawn);
	TestTrue(TEXT("The first player at A settles"), Settled());
	const FGround OnlyA = ReadGround(W, SiteA);
	TestEqual(TEXT("One player: no further viewer"), W.Manager->GetFurtherViewers().Num(), 0);
	TestTrue(TEXT("One player: ground under it (non-vacuous)"), OnlyA.NumHits() == 9 && OnlyA.Cooked.Num() > 0);
	TestEqual(TEXT("One player: nothing at B"), ReadGround(W, SiteB).Near.Num(), 0);

	// A second player joins: no pawn, no viewer; then its pawn stands at B, far from the first.
	APlayerController* Second = W.World->SpawnActor<APlayerController>();
	W.Tick();
	TestEqual(TEXT("A second player with no pawn is no viewer"), W.Manager->GetFurtherViewers().Num(), 0);
	APawn* SecondPawn = SpawnPawnOver(SiteB);
	LookFrom(Second, SecondPawn);
	TestTrue(TEXT("The second player at B settles"), Settled());
	const FGround WithB = ReadGround(W, SiteB);
	TestTrue(TEXT("The second player's pawn is the further viewer"), W.Manager->GetFurtherViewers().Num() == 1
		&& W.Manager->GetFurtherViewers()[0].Position.Equals(SecondPawn->GetActorLocation()));
	TestTrue(TEXT("The second player has ground on this machine, far from the first"), WithB.NumHits() == 9 && WithB.Cooked.Num() > 0);
	TestTrue(TEXT("The first player keeps its window and its ground"), ReadGround(W, SiteA).Loaded.Includes(OnlyA.Loaded) && ReadGround(W, SiteA).NumHits() == 9);

	// The comparison: the first player only, as before.
	IConsoleVariable* GroundWindows = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Stream.GroundWindows"));
	if (TestNotNull(TEXT("voxel.Stream.GroundWindows exists"), GroundWindows))
	{
		GroundWindows->Set(0);
		TestTrue(TEXT("voxel.Stream.GroundWindows 0 settles"), Settled());
		const FGround Off = ReadGround(W, SiteB);
		TestTrue(TEXT("voxel.Stream.GroundWindows 0: no further viewer, and no ground under the second player"),
			W.Manager->GetFurtherViewers().Num() == 0 && Off.Near.Num() == 0 && Off.NumHits() == 0);
		GroundWindows->Set(1);
		TestTrue(TEXT("voxel.Stream.GroundWindows 1 settles"), Settled());
		TestTrue(TEXT("... and 1 brings it back"), SameSet(ReadGround(W, SiteB).Cooked, WithB.Cooked));
	}

	// The second player dies (no pawn): its ground is released, the first's stays.
	LookFrom(Second, nullptr);
	SecondPawn->Destroy();
	TestTrue(TEXT("The second player without a pawn settles"), Settled());
	TestTrue(TEXT("The second player without a pawn: no further viewer, B released"),
		W.Manager->GetFurtherViewers().Num() == 0 && ReadGround(W, SiteB).Near.Num() == 0 && W.Manager->GetGroundWindowKeepCount() == 0);
	TestTrue(TEXT("... and the world holds what one player's did"), SameSet(ReadGround(W, SiteA).Loaded, OnlyA.Loaded));

	// ... and respawns at B; then the first player leaves: the second is the world's first player, the streaming viewer.
	SecondPawn = SpawnPawnOver(SiteB);
	LookFrom(Second, SecondPawn);
	TestTrue(TEXT("The second player back at B settles"), Settled());
	TestTrue(TEXT("Respawned at B: ground again"), ReadGround(W, SiteB).NumHits() == 9);
	LookFrom(First, nullptr);
	FirstPawn->Destroy();
	First->Destroy();
	TestTrue(TEXT("The first player gone settles"), Settled());
	const FGround SecondAlone = ReadGround(W, SiteB);
	TestTrue(TEXT("The first player gone: the second is the world's first player"), W.World->GetFirstPlayerController() == Second);
	TestEqual(TEXT("... the streaming viewer, with no further viewer"), W.Manager->GetFurtherViewers().Num(), 0);
	TestTrue(TEXT("... and keeps its ground (now its own window's)"), SecondAlone.NumHits() == 9 && SameSet(SecondAlone.Cooked, WithB.Cooked));
	TestEqual(TEXT("... while A is released"), ReadGround(W, SiteA).Near.Num(), 0);

	LookFrom(Second, nullptr);
	SecondPawn->Destroy();
	Second->Destroy();
	W.Stop();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
