// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "DistanceBandLODStrategy.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ==================== Helpers ====================

namespace LODHysteresisTestHelpers
{
	/**
	 * Exposes the protected hysteresis entry point and the band/config state it reads, so the
	 * decision function can be exercised directly instead of through a fully streamed world.
	 */
	struct FTestableLODStrategy : public FDistanceBandLODStrategy
	{
		using FDistanceBandLODStrategy::ApplyLODHysteresis;
		using FDistanceBandLODStrategy::LODBands;
		using FDistanceBandLODStrategy::RefineHysteresisFraction;
		using FDistanceBandLODStrategy::CoarsenHysteresisFraction;
		using FDistanceBandLODStrategy::BaseChunkSize;
		using FDistanceBandLODStrategy::VoxelSize;
	};

	/**
	 * Two adjacent bands sharing an edge at 7000uu, with the shipped demo geometry:
	 * 64-voxel chunks at 75uu => 4800uu chunk width.
	 */
	void SetupDemoBands(FTestableLODStrategy& Strategy)
	{
		Strategy.BaseChunkSize = 64;
		Strategy.VoxelSize = 75.0f;

		FLODBand Lod0;
		Lod0.LODLevel = 0;
		Lod0.MinDistance = 0.0f;
		Lod0.MaxDistance = 7000.0f;
		Lod0.VoxelStride = 1;

		FLODBand Lod1;
		Lod1.LODLevel = 1;
		Lod1.MinDistance = 7000.0f;
		Lod1.MaxDistance = 14000.0f;
		Lod1.VoxelStride = 2;

		Strategy.LODBands = { Lod0, Lod1 };
		Strategy.RefineHysteresisFraction = 0.25f;
		Strategy.CoarsenHysteresisFraction = 0.5f;
	}

	constexpr float ChunkWidth = 64.0f * 75.0f; // 4800uu
	constexpr float BandEdge = 7000.0f;
}

// ==================== Tests ====================

/**
 * A chunk whose raw LOD already equals its committed LOD must never be disturbed, at any
 * distance. This is the common case every frame and must not depend on the margins.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLODHysteresisNoChangeIsStableTest,
	"VoxelWorlds.LOD.Hysteresis.NoChangeIsStable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLODHysteresisNoChangeIsStableTest::RunTest(const FString& Parameters)
{
	using namespace LODHysteresisTestHelpers;
	FTestableLODStrategy Strategy;
	SetupDemoBands(Strategy);

	for (float Distance = 0.0f; Distance <= 20000.0f; Distance += 500.0f)
	{
		TestEqual(FString::Printf(TEXT("LOD0 committed, raw LOD0 at %.0f"), Distance),
			Strategy.ApplyLODHysteresis(0, 0, Distance), 0);
		TestEqual(FString::Printf(TEXT("LOD1 committed, raw LOD1 at %.0f"), Distance),
			Strategy.ApplyLODHysteresis(1, 1, Distance), 1);
	}

	return true;
}

/**
 * Refining must ANTICIPATE the band edge: a chunk committed at LOD1 becomes LOD0 slightly
 * before the viewer crosses the edge, not after. Guards the sign of the refine margin, which
 * previously subtracted and so delayed detail until 1200uu past where it was due.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLODHysteresisRefineAnticipatesEdgeTest,
	"VoxelWorlds.LOD.Hysteresis.RefineAnticipatesEdge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLODHysteresisRefineAnticipatesEdgeTest::RunTest(const FString& Parameters)
{
	using namespace LODHysteresisTestHelpers;
	FTestableLODStrategy Strategy;
	SetupDemoBands(Strategy);

	const float RefineThreshold = BandEdge + Strategy.RefineHysteresisFraction * ChunkWidth; // 8200

	// Just outside the threshold: keep the coarse LOD.
	TestEqual(TEXT("Still LOD1 just beyond the refine threshold"),
		Strategy.ApplyLODHysteresis(1, 0, RefineThreshold + 50.0f), 1);

	// Just inside: refine, even though we have not yet reached the nominal band edge.
	TestEqual(TEXT("Refines to LOD0 just inside the refine threshold"),
		Strategy.ApplyLODHysteresis(1, 0, RefineThreshold - 50.0f), 0);

	// The whole point of the fix: detail is present BEFORE the band edge, not after it.
	TestEqual(TEXT("Already LOD0 at the nominal band edge"),
		Strategy.ApplyLODHysteresis(1, 0, BandEdge), 0);
	TestEqual(TEXT("Already LOD0 well inside the fine band"),
		Strategy.ApplyLODHysteresis(1, 0, BandEdge - 1200.0f), 0);

	return true;
}

/**
 * Coarsening must TRAIL the band edge, so terrain just left behind does not flicker back to
 * full detail.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLODHysteresisCoarsenTrailsEdgeTest,
	"VoxelWorlds.LOD.Hysteresis.CoarsenTrailsEdge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLODHysteresisCoarsenTrailsEdgeTest::RunTest(const FString& Parameters)
{
	using namespace LODHysteresisTestHelpers;
	FTestableLODStrategy Strategy;
	SetupDemoBands(Strategy);

	const float CoarsenThreshold = BandEdge + Strategy.CoarsenHysteresisFraction * ChunkWidth; // 9400

	TestEqual(TEXT("Holds LOD0 at the nominal band edge"),
		Strategy.ApplyLODHysteresis(0, 1, BandEdge), 0);
	TestEqual(TEXT("Holds LOD0 just inside the coarsen threshold"),
		Strategy.ApplyLODHysteresis(0, 1, CoarsenThreshold - 50.0f), 0);
	TestEqual(TEXT("Coarsens to LOD1 past the coarsen threshold"),
		Strategy.ApplyLODHysteresis(0, 1, CoarsenThreshold + 50.0f), 1);

	return true;
}

/**
 * The stability invariant: there must be no distance at which a chunk both wants to refine and
 * wants to coarsen, or it flips every frame. Sweeping the whole range, a chunk committed either
 * way must settle rather than disagree with itself.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLODHysteresisNoFlipZoneTest,
	"VoxelWorlds.LOD.Hysteresis.NoFlipZone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLODHysteresisNoFlipZoneTest::RunTest(const FString& Parameters)
{
	using namespace LODHysteresisTestHelpers;
	FTestableLODStrategy Strategy;
	SetupDemoBands(Strategy);

	for (float Distance = 0.0f; Distance <= 20000.0f; Distance += 25.0f)
	{
		const int32 FromCoarse = Strategy.ApplyLODHysteresis(1, 0, Distance); // wants to refine
		const int32 FromFine = Strategy.ApplyLODHysteresis(0, 1, Distance);   // wants to coarsen

		// A flip zone is a distance where the chunk refines when coarse AND coarsens when fine:
		// committing either value immediately demands the other one, so it oscillates forever.
		const bool bFlips = (FromCoarse == 0) && (FromFine == 1);
		TestFalse(FString::Printf(TEXT("No oscillation at distance %.0f"), Distance), bFlips);
	}

	return true;
}

/**
 * The deadband is the gap between the two thresholds and must equal
 * (Coarsen - Refine) * ChunkWidth. With the shipped fractions that is 1200uu, down from the
 * 3600uu the subtracting refine margin produced.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLODHysteresisDeadbandWidthTest,
	"VoxelWorlds.LOD.Hysteresis.DeadbandWidth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLODHysteresisDeadbandWidthTest::RunTest(const FString& Parameters)
{
	using namespace LODHysteresisTestHelpers;
	FTestableLODStrategy Strategy;
	SetupDemoBands(Strategy);

	// Walk inward and find where a coarse-committed chunk first refines.
	float RefineAt = -1.0f;
	for (float Distance = 20000.0f; Distance >= 0.0f; Distance -= 5.0f)
	{
		if (Strategy.ApplyLODHysteresis(1, 0, Distance) == 0) { RefineAt = Distance; break; }
	}

	// Walk outward and find where a fine-committed chunk first coarsens.
	float CoarsenAt = -1.0f;
	for (float Distance = 0.0f; Distance <= 20000.0f; Distance += 5.0f)
	{
		if (Strategy.ApplyLODHysteresis(0, 1, Distance) == 1) { CoarsenAt = Distance; break; }
	}

	TestTrue(TEXT("Found a refine threshold"), RefineAt > 0.0f);
	TestTrue(TEXT("Found a coarsen threshold"), CoarsenAt > 0.0f);
	TestTrue(TEXT("Refine threshold is nearer than coarsen threshold"), RefineAt < CoarsenAt);

	const float ExpectedDeadband =
		(Strategy.CoarsenHysteresisFraction - Strategy.RefineHysteresisFraction) * ChunkWidth;
	TestTrue(
		FString::Printf(TEXT("Deadband %.0f matches expected %.0f"), CoarsenAt - RefineAt, ExpectedDeadband),
		FMath::Abs((CoarsenAt - RefineAt) - ExpectedDeadband) <= 15.0f);

	return true;
}

/**
 * Multi-level changes (a teleport) bypass the margins entirely and are taken immediately —
 * waiting out a deadband after a jump would leave the viewer inside coarse terrain.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLODHysteresisMultiLevelIsImmediateTest,
	"VoxelWorlds.LOD.Hysteresis.MultiLevelIsImmediate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLODHysteresisMultiLevelIsImmediateTest::RunTest(const FString& Parameters)
{
	using namespace LODHysteresisTestHelpers;
	FTestableLODStrategy Strategy;
	SetupDemoBands(Strategy);

	TestEqual(TEXT("LOD2 -> LOD0 accepted immediately regardless of distance"),
		Strategy.ApplyLODHysteresis(2, 0, 12000.0f), 0);
	TestEqual(TEXT("LOD0 -> LOD2 accepted immediately regardless of distance"),
		Strategy.ApplyLODHysteresis(0, 2, 1000.0f), 2);

	return true;
}

/**
 * Initialize() must clamp Refine to Coarsen. Without it, a config with refine > coarsen orders
 * the thresholds backwards and produces a permanent flip zone.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLODHysteresisInvariantClampTest,
	"VoxelWorlds.LOD.Hysteresis.InvariantClamp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLODHysteresisInvariantClampTest::RunTest(const FString& Parameters)
{
	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
	Config->LODRefineHysteresis = 2.0f;   // deliberately inverted
	Config->LODCoarsenHysteresis = 0.5f;

	LODHysteresisTestHelpers::FTestableLODStrategy Strategy;
	AddExpectedError(TEXT("exceeds LODCoarsenHysteresis"), EAutomationExpectedErrorFlags::Contains, 0);
	Strategy.Initialize(Config);

	TestTrue(TEXT("Refine clamped to be no greater than coarsen"),
		Strategy.RefineHysteresisFraction <= Strategy.CoarsenHysteresisFraction);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
