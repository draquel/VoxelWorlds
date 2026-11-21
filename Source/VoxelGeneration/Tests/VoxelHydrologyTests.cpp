// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelHydrology.h"
#include "VoxelRiverCourse.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The hydrology generator on made ground (Documentation/HYDROLOGY_PLAN.md, section 5.8; phase H7).
//
// - Generation.Hydrology.Lakes: the lake rule. A hollow on a slope fills to a margin under its rim and spills by the
//   cell beyond its saddle; two pits in one hollow make one lake, the same from either pit; a hollow that spills into
//   a lower one has a lake of its own over it; a hollow deeper than the cap holds a lake as deep as the cap, with no
//   outlet, and one wider than the cap the largest lake under it; a dip too shallow is no lake. And the rule that keeps
//   a lake's water in: no column of a footprint's edge lies under the level.
// - Generation.Hydrology.Regions: the same bodies from any region. Rolling ground with a sea: two regions that
//   overlap, a region around both, and a run with a cache give each body with the same id and the same record;
//   every river reach keeps the rules of the plan's section 5.3 (its surface never rises, it starts from nothing or in
//   a body, it ends in a body, its bed keeps a line's limits and the cut's cap); no cell has two ways on; and a
//   system one of whose reaches cannot be made has no reach.
// - Generation.Hydrology.Fingerprint: a world that names a hydrology configuration has the save fingerprint's term,
//   with the generator's version and its settings; one that names none has the fingerprint it had; a planet and a
//   block world that name one have the term too (the plan's H8; their own tests are in VoxelHydrologyPlanetTests.cpp).
// ---------------------------------------------------------------------------

namespace VoxelHydrologyTest
{
	/** What the generator keeps between pieces of work, on one thread. */
	struct FCache : IVoxelHydrologyCache
	{
		TMap<FIntPoint, TSharedRef<const FVoxelHydrologyTrace>> Traces;
		TMap<FIntPoint, TSharedRef<const FVoxelHydrologyLake>> Lakes;
		TMap<FIntPoint, TSharedRef<const FVoxelHydrologySystem>> Systems;

		virtual TSharedPtr<const FVoxelHydrologyTrace> FindTrace(const FIntPoint& Square) override
		{
			const TSharedRef<const FVoxelHydrologyTrace>* Found = Traces.Find(Square);
			return Found ? Found->ToSharedPtr() : nullptr;
		}
		virtual TSharedRef<const FVoxelHydrologyTrace> StoreTrace(const TSharedRef<const FVoxelHydrologyTrace>& Trace) override
		{
			return Traces.FindOrAdd(Trace->Square, Trace);
		}
		virtual TSharedPtr<const FVoxelHydrologyLake> FindLake(const FIntPoint& Pit) override
		{
			const TSharedRef<const FVoxelHydrologyLake>* Found = Lakes.Find(Pit);
			return Found ? Found->ToSharedPtr() : nullptr;
		}
		virtual TSharedRef<const FVoxelHydrologyLake> StoreLake(const TSharedRef<const FVoxelHydrologyLake>& Lake) override
		{
			return Lakes.FindOrAdd(Lake->Pit, Lake);
		}
		virtual TSharedPtr<const FVoxelHydrologySystem> FindSystem(const FIntPoint& Mouth) override
		{
			const TSharedRef<const FVoxelHydrologySystem>* Found = Systems.Find(Mouth);
			return Found ? Found->ToSharedPtr() : nullptr;
		}
		virtual TSharedRef<const FVoxelHydrologySystem> StoreSystem(const TSharedRef<const FVoxelHydrologySystem>& System) override
		{
			return Systems.FindOrAdd(System->Mouth, System);
		}
	};

	/** A round dip: Depth at its middle, gone at about twice Width. */
	double Dip(double X, double Y, double CentreX, double CentreY, double Depth, double Width)
	{
		const double R2 = FMath::Square(X - CentreX) + FMath::Square(Y - CentreY);
		return Depth * FMath::Exp(-R2 / (Width * Width));
	}

	FVoxelHydrologyGround MakeGround(TFunction<double(double, double)> Height, bool bSea = true)
	{
		FVoxelHydrologyGround Ground;
		Ground.Height = MoveTemp(Height);
		Ground.bSea = bSea;
		Ground.SeaLevel = 0.0;
		Ground.VoxelSize = 100.0;
		Ground.ChunkWorldSize = 3200.0;
		Ground.Seed = 4242;
		return Ground;
	}

	/** The pit a place's water runs to. */
	FIntPoint PitUnder(FVoxelHydrologyHeights& Heights, double X, double Y)
	{
		FIntPoint Cell(FMath::RoundToInt32(X / Heights.GetCellSize()), FMath::RoundToInt32(Y / Heights.GetCellSize()));
		FIntPoint Next;
		for (int32 Guard = 0; Guard < 4096 && Heights.GetDownhill(Cell, Next); ++Guard)
		{
			Cell = Next;
		}
		return Cell;
	}

	/**
	 * Whether a kept lake's water is held: no column that holds water (in the footprint, its ground under the level)
	 * has a column a voxel beside it that is outside the footprint with its ground under the level too.
	 * @return How many such columns there are (0: watertight), and through OutWet how many columns hold water
	 */
	int32 CountLeaks(const FVoxelHydrologyGround& Ground, const FVoxelHydrologyLake& Lake, int32& OutWet)
	{
		OutWet = 0;
		int32 Leaks = 0;
		const FVoxelWaterLake& Shape = *Lake.Body.Lake;
		const FBox2D Bounds = Shape.GetBounds2D();
		const double Step = Ground.VoxelSize;
		for (double Y = Bounds.Min.Y + 0.5 * Step; Y < Bounds.Max.Y; Y += Step)
		{
			for (double X = Bounds.Min.X + 0.5 * Step; X < Bounds.Max.X; X += Step)
			{
				if (!Shape.Covers(X, Y) || Ground.Height(X, Y) >= Lake.WaterLevel)
				{
					continue;
				}
				++OutWet;
				for (const FVector2D& Beside : { FVector2D(Step, 0.0), FVector2D(-Step, 0.0), FVector2D(0.0, Step), FVector2D(0.0, -Step) })
				{
					if (!Shape.Covers(X + Beside.X, Y + Beside.Y) && Ground.Height(X + Beside.X, Y + Beside.Y) < Lake.WaterLevel)
					{
						++Leaks;
					}
				}
			}
		}
		return Leaks;
	}

	bool SameReach(const FVoxelHydrologyReach& A, const FVoxelHydrologyReach& B)
	{
		const TArray<FVoxelWaterReachVertex>& VA = A.Body.Reach->GetVertices();
		const TArray<FVoxelWaterReachVertex>& VB = B.Body.Reach->GetVertices();
		if (A.Body.Id != B.Body.Id || A.Flow != B.Flow || VA.Num() != VB.Num() || A.Bed->GetId() != B.Bed->GetId())
		{
			return false;
		}
		for (int32 Index = 0; Index < VA.Num(); ++Index)
		{
			if (VA[Index].Position != VB[Index].Position || VA[Index].SurfaceLevel != VB[Index].SurfaceLevel || VA[Index].BedLevel != VB[Index].BedLevel
				|| VA[Index].HalfWidth != VB[Index].HalfWidth)
			{
				return false;
			}
		}
		return true;
	}

	bool SameLake(const FVoxelHydrologyLake& A, const FVoxelHydrologyLake& B)
	{
		return A.Body.Id == B.Body.Id && A.WaterLevel == B.WaterLevel && A.LowestCell == B.LowestCell && A.Cells.Num() == B.Cells.Num()
			&& A.Body.Lake->NumCells() == B.Body.Lake->NumCells() && A.Body.Lake->GetBounds2D() == B.Body.Lake->GetBounds2D();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHydrologyLakesTest,
	"VoxelWorlds.Generation.Hydrology.Lakes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHydrologyLakesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelHydrologyTest;
	FVoxelHydrologySettings Base;
	Base.MinLakeArea = 2.0e5f;

	// ---- One hollow on a slope that falls to the sea along +X.
	{
		const FVoxelHydrologyGround Ground = MakeGround([](double X, double Y) { return 4000.0 - 0.08 * X - Dip(X, Y, 0.0, 0.0, 700.0, 2500.0); });
		const FVoxelHydrologySettings Settings = FVoxelHydrology::Sanitize(Base, Ground);
		FVoxelHydrologyHeights Heights(Ground, Settings);
		const FIntPoint Pit = PitUnder(Heights, 0.0, 0.0);
		const TSharedRef<const FVoxelHydrologyLake> Lake = FVoxelHydrology::MakeLake(Settings, Heights, Pit);
		TestTrue(TEXT("a hollow on a slope: an open lake, kept"), Lake->IsOpen() && Lake->bKept);
		TestTrue(TEXT("... its rim is its saddle's ground, over its pit"), FMath::IsNearlyEqual(Lake->RimLevel, Heights.At(Lake->Saddle), 0.01) && Lake->RimLevel > Heights.At(Pit) + 100.0);
		TestTrue(TEXT("... the cell beyond the saddle lies under the rim, downhill of it"), Heights.At(Lake->Outlet) < Lake->RimLevel && Lake->Outlet.X > Lake->Saddle.X - 1);
		TestTrue(TEXT("... the saddle lies downhill of the pit: the water leaves toward the sea"), Lake->Saddle.X > Pit.X);
		bool bUnderRim = true;
		for (const FIntPoint& Cell : Lake->Cells)
		{
			bUnderRim &= Heights.At(Cell) <= Lake->RimLevel;
		}
		TestTrue(TEXT("... every cell of the hollow lies under the rim"), bUnderRim && Lake->Cells.Num() >= 8);
		TestTrue(TEXT("... its level is at least the margin under the rim, and over its lowest ground by its depth"),
			Lake->WaterLevel <= Lake->RimLevel - Settings.LakeLevelMarginVoxels * Ground.VoxelSize + 0.01 && Lake->Depth > 200.0
			&& Lake->Body.Level == Lake->WaterLevel && Lake->Body.Kind == EVoxelWaterBodyKind::Lake && Lake->Body.MinCaveDepth == Settings.LakeMinCaveDepth);
		TestTrue(TEXT("... from every cell of the hollow the way to the saddle is kept"), Lake->ToSaddle.Num() == Lake->Cells.Num() && Lake->Cells.Contains(Lake->PreSaddle));
		int32 Wet = 0;
		const int32 Leaks = CountLeaks(Ground, Lake.Get(), Wet);
		AddInfo(FString::Printf(TEXT("one hollow: rim %.1f, level %.1f (lowered %.1f for the edge), %d cells, %d wet columns, %.2f ha, %d leaks"),
			Lake->RimLevel, Lake->WaterLevel, Lake->LoweredForEdge, Lake->Cells.Num(), Wet, Lake->Area / 1.0e8, Leaks));
		TestTrue(TEXT("... its footprint holds water (non-vacuous)"), Wet > 200);
		TestEqual(TEXT("... no column of water stands against the footprint's edge"), Leaks, 0);
	}

	// ---- Two pits in one hollow: one lake, the same from either pit.
	{
		const FVoxelHydrologyGround Ground = MakeGround([](double X, double Y)
		{
			return 4000.0 - 0.02 * X - Dip(X, Y, 0.0, 0.0, 900.0, 5000.0) - Dip(X, Y, -1800.0, 0.0, 300.0, 1100.0) - Dip(X, Y, 1800.0, 400.0, 420.0, 1100.0);
		});
		const FVoxelHydrologySettings Settings = FVoxelHydrology::Sanitize(Base, Ground);
		FVoxelHydrologyHeights Heights(Ground, Settings);
		const FIntPoint PitA = PitUnder(Heights, -1800.0, 0.0);
		const FIntPoint PitB = PitUnder(Heights, 1800.0, 400.0);
		if (TestTrue(TEXT("nested pits: the ground has two pits (the fixture)"), PitA != PitB))
		{
			const TSharedRef<const FVoxelHydrologyLake> A = FVoxelHydrology::MakeLake(Settings, Heights, PitA);
			const TSharedRef<const FVoxelHydrologyLake> B = FVoxelHydrology::MakeLake(Settings, Heights, PitB);
			TestTrue(TEXT("nested pits: each pit's lake holds the other pit"), A->Cells.Contains(PitB) && B->Cells.Contains(PitA));
			TestTrue(TEXT("... one rim, one saddle, one outlet"), A->RimLevel == B->RimLevel && A->Saddle == B->Saddle && A->Outlet == B->Outlet && A->IsOpen() && B->IsOpen());
			TestTrue(TEXT("... the same cells and the same way to the saddle from each"), A->Cells.Num() == B->Cells.Num() && A->PreSaddle == B->PreSaddle
				&& A->ToSaddle.FindRef(PitA) == B->ToSaddle.FindRef(PitA));
			TestTrue(TEXT("... and one body: the same id, level and footprint"), A->bKept && B->bKept && SameLake(A.Get(), B.Get()));
			TestTrue(TEXT("... the rim stands over the ground between the two pits"), A->RimLevel > Heights.At(FIntPoint(0, 0)) + 50.0);
		}
	}

	// ---- A hollow that spills into a lower one: two lakes, the upper's outlet draining to the lower's pit.
	{
		const FVoxelHydrologyGround Ground = MakeGround([](double X, double Y)
		{
			return 6000.0 - 0.12 * X - Dip(X, Y, 0.0, 0.0, 800.0, 2200.0) - Dip(X, Y, 9000.0, 0.0, 800.0, 2200.0);
		});
		const FVoxelHydrologySettings Settings = FVoxelHydrology::Sanitize(Base, Ground);
		FVoxelHydrologyHeights Heights(Ground, Settings);
		const FIntPoint Upper = PitUnder(Heights, 0.0, 0.0);
		const FIntPoint Lower = PitUnder(Heights, 9000.0, 0.0);
		const TSharedRef<const FVoxelHydrologyLake> A = FVoxelHydrology::MakeLake(Settings, Heights, Upper);
		const TSharedRef<const FVoxelHydrologyLake> B = FVoxelHydrology::MakeLake(Settings, Heights, Lower);
		TestTrue(TEXT("a lower hollow: two lakes, each its own"), Upper != Lower && A->IsOpen() && B->IsOpen() && !A->Cells.Contains(Lower) && !B->Cells.Contains(Upper)
			&& A->Body.Id != B->Body.Id);
		TestTrue(TEXT("... the upper one stands higher and its outlet's water runs to the lower one's pit"),
			A->WaterLevel > B->RimLevel && PitUnder(Heights, A->Outlet.X * Heights.GetCellSize(), A->Outlet.Y * Heights.GetCellSize()) == Lower);
	}

	// ---- The caps: a hollow deeper than a lake may be, and one wider.
	{
		const FVoxelHydrologyGround Ground = MakeGround([](double X, double Y) { return 9000.0 - 0.02 * X - Dip(X, Y, 0.0, 0.0, 5000.0, 4000.0); });
		FVoxelHydrologySettings Deep = Base;
		Deep.MaxLakeDepth = 1000.0f;
		const FVoxelHydrologySettings Settings = FVoxelHydrology::Sanitize(Deep, Ground);
		FVoxelHydrologyHeights Heights(Ground, Settings);
		const FIntPoint Pit = PitUnder(Heights, 0.0, 0.0);
		const TSharedRef<const FVoxelHydrologyLake> Lake = FVoxelHydrology::MakeLake(Settings, Heights, Pit);
		TestTrue(TEXT("a hollow deeper than the cap: a lake with no outlet, closed for its depth"), !Lake->IsOpen() && Lake->Closed == 1 && Lake->bKept);
		TestTrue(TEXT("... as deep as the cap"), FMath::IsNearlyEqual(Lake->Depth, 1000.0, 60.0) && Lake->WaterLevel < Heights.At(Pit) + 1000.0 + 1.0);
		int32 Wet = 0;
		TestEqual(TEXT("... and watertight"), CountLeaks(Ground, Lake.Get(), Wet), 0);

		FVoxelHydrologySettings Wide = Base;
		Wide.MaxLakeCells = 40;
		const FVoxelHydrologySettings WideSettings = FVoxelHydrology::Sanitize(Wide, Ground);
		FVoxelHydrologyHeights WideHeights(Ground, WideSettings);
		const TSharedRef<const FVoxelHydrologyLake> Capped = FVoxelHydrology::MakeLake(WideSettings, WideHeights, Pit);
		TestTrue(TEXT("a hollow wider than the cap: the largest lake under it, with no outlet"), Capped->Closed == 2 && Capped->Cells.Num() <= 41 && Capped->Cells.Num() >= 8);
		TestTrue(TEXT("... standing under the rim it was closed at, which is under the hollow's own"), Capped->WaterLevel < Capped->RimLevel && Capped->RimLevel < Heights.At(Pit) + 4900.0);
		if (Capped->bKept)
		{
			TestEqual(TEXT("... and watertight"), CountLeaks(Ground, Capped.Get(), Wet), 0);
		}

		FVoxelHydrologySettings Narrow = Base;
		Narrow.MaxLakeSpanCells = 8;
		const FVoxelHydrologySettings NarrowSettings = FVoxelHydrology::Sanitize(Narrow, Ground);
		FVoxelHydrologyHeights NarrowHeights(Ground, NarrowSettings);
		const TSharedRef<const FVoxelHydrologyLake> Short = FVoxelHydrology::MakeLake(NarrowSettings, NarrowHeights, Pit);
		FIntPoint Min(MAX_int32, MAX_int32), Max(MIN_int32, MIN_int32);
		for (const FIntPoint& Cell : Short->Cells)
		{
			Min = FIntPoint(FMath::Min(Min.X, Cell.X), FMath::Min(Min.Y, Cell.Y));
			Max = FIntPoint(FMath::Max(Max.X, Cell.X), FMath::Max(Max.Y, Cell.Y));
		}
		TestTrue(TEXT("a hollow longer than the span: a lake no longer than it"), Short->Closed == 2 && Max.X - Min.X < 8 && Max.Y - Min.Y < 8);
	}

	// ---- A dip too shallow to be a lake.
	{
		const FVoxelHydrologyGround Ground = MakeGround([](double X, double Y) { return 4000.0 - 0.01 * X - Dip(X, Y, 0.0, 0.0, 90.0, 2500.0); });
		const FVoxelHydrologySettings Settings = FVoxelHydrology::Sanitize(Base, Ground);
		FVoxelHydrologyHeights Heights(Ground, Settings);
		const TSharedRef<const FVoxelHydrologyLake> Lake = FVoxelHydrology::MakeLake(Settings, Heights, PitUnder(Heights, 0.0, 0.0));
		TestTrue(TEXT("a dip under the least depth: a hollow with an outlet, and no lake"), Lake->IsOpen() && !Lake->bKept && !Lake->Body.Lake.IsValid());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHydrologyRegionsTest,
	"VoxelWorlds.Generation.Hydrology.Regions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHydrologyRegionsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelHydrologyTest;
	using FShape = FVoxelConditioningLineShape;
	// Rolling ground: hills of a kilometre or two over a sea, with smaller relief on them that makes hollows.
	const FVoxelHydrologyGround Ground = MakeGround([](double X, double Y)
	{
		// Three hollows on the hills: two where the test's regions overlap, one in the second region alone.
		return 900.0 + 2600.0 * FMath::Sin(X / 21000.0) * FMath::Cos(Y / 17000.0) + 1300.0 * FMath::Sin((X + Y) / 9000.0) * FMath::Cos((X - Y) / 12500.0)
			+ 260.0 * FMath::Sin(X / 2900.0 + 1.7) * FMath::Sin(Y / 3300.0 + 0.3)
			- Dip(X, Y, 12000.0, -6000.0, 900.0, 1600.0) - Dip(X, Y, 20000.0, 2000.0, 900.0, 1600.0) - Dip(X, Y, 43000.0, -12000.0, 900.0, 1600.0);
	});
	FVoxelHydrologySettings Settings;
	Settings.SpringSpacingCells = 8;
	Settings.MinSpringHeight = 600.0f;
	Settings.MinRiverLength = 12000.0f;
	Settings.MaxRiverLength = 60000.0f;
	Settings.MaxMeanGrade = 0.3f;
	Settings.MinLakeArea = 2.0e5f;
	Settings = FVoxelHydrology::Sanitize(Settings, Ground);
	const double Voxel = Ground.VoxelSize;
	const double BankWidth = Settings.BankWidthVoxels * Voxel;

	const FBox2D BoxA(FVector2D(-30000.0, -20000.0), FVector2D(30000.0, 25000.0));
	const FBox2D BoxB(FVector2D(5000.0, -45000.0), FVector2D(60000.0, 10000.0));
	const FBox2D BoxAll(FVector2D(-30000.0, -45000.0), FVector2D(60000.0, 25000.0));
	FVoxelHydrologyRegion A, B, All, Cached;
	FVoxelHydrology::BuildRegion(Settings, Ground, BoxA, nullptr, A);
	FVoxelHydrology::BuildRegion(Settings, Ground, BoxB, nullptr, B);
	FVoxelHydrology::BuildRegion(Settings, Ground, BoxAll, nullptr, All);
	FCache Cache;
	// The cache is filled from another region first, so the run reads what a different region stored.
	FVoxelHydrologyRegion Other;
	FVoxelHydrology::BuildRegion(Settings, Ground, BoxB, &Cache, Other);
	FVoxelHydrology::BuildRegion(Settings, Ground, BoxA, &Cache, Cached);

	int32 ReachesA = 0;
	for (const TSharedRef<const FVoxelHydrologySystem>& System : A.Systems)
	{
		ReachesA += System->Reaches.Num();
	}
	AddInfo(FString::Printf(TEXT("region A: %d squares, %d springs, %d rivers, %d systems (%d refused) with %d reaches, %d lakes; B: %d systems, %d lakes; both: %d systems, %d lakes"),
		A.NumSquares, A.NumSprings, A.NumRivers, A.Systems.Num(), A.NumRefused, ReachesA, A.Lakes.Num(), B.Systems.Num(), B.Lakes.Num(), All.Systems.Num(), All.Lakes.Num()));
	if (!TestTrue(TEXT("the ground has rivers and lakes in each region (the fixture)"), A.Systems.Num() >= 3 && B.Systems.Num() >= 3 && A.Lakes.Num() >= 1 && B.Lakes.Num() >= 2 && ReachesA >= 4))
	{
		return true;
	}

	// ---- The same bodies from any region: a body of one region that reaches into another is that region's too, the same.
	const auto FindReach = [](const FVoxelHydrologyRegion& Region, uint32 Id) -> const FVoxelHydrologyReach*
	{
		for (const TSharedRef<const FVoxelHydrologySystem>& System : Region.Systems)
		{
			for (const FVoxelHydrologyReach& Reach : System->Reaches)
			{
				if (Reach.Body.Id == Id)
				{
					return &Reach;
				}
			}
		}
		return nullptr;
	};
	const auto FindLake = [](const FVoxelHydrologyRegion& Region, uint32 Id) -> const FVoxelHydrologyLake*
	{
		for (const TSharedRef<const FVoxelHydrologyLake>& Lake : Region.Lakes)
		{
			if (Lake->Body.Id == Id)
			{
				return &Lake.Get();
			}
		}
		return nullptr;
	};
	const auto Compare = [&](const FVoxelHydrologyRegion& From, const FVoxelHydrologyRegion& In, const FBox2D& InBox, const TCHAR* What)
	{
		int32 Shared = 0, Missing = 0, Different = 0;
		for (const TSharedRef<const FVoxelHydrologySystem>& System : From.Systems)
		{
			for (const FVoxelHydrologyReach& Reach : System->Reaches)
			{
				if (!Reach.Body.MeetsRegion(InBox))
				{
					continue;
				}
				const FVoxelHydrologyReach* Twin = FindReach(In, Reach.Body.Id);
				Missing += Twin ? 0 : 1;
				Different += (Twin && !SameReach(Reach, *Twin)) ? 1 : 0;
				++Shared;
			}
		}
		for (const TSharedRef<const FVoxelHydrologyLake>& Lake : From.Lakes)
		{
			if (!Lake->Body.MeetsRegion(InBox))
			{
				continue;
			}
			const FVoxelHydrologyLake* Twin = FindLake(In, Lake->Body.Id);
			Missing += Twin ? 0 : 1;
			Different += (Twin && !SameLake(Lake.Get(), *Twin)) ? 1 : 0;
			++Shared;
		}
		AddInfo(FString::Printf(TEXT("%s: %d bodies shared, %d missing, %d different"), What, Shared, Missing, Different));
		TestTrue(*FString::Printf(TEXT("%s: bodies are shared (non-vacuous)"), What), Shared > 0);
		TestEqual(*FString::Printf(TEXT("%s: every body of the one that reaches the other is the other's"), What), Missing, 0);
		TestEqual(*FString::Printf(TEXT("%s: ... and the same record"), What), Different, 0);
	};
	Compare(A, B, BoxB, TEXT("A in B"));
	Compare(B, A, BoxA, TEXT("B in A"));
	Compare(A, All, BoxAll, TEXT("A in the region round both"));
	Compare(All, A, BoxA, TEXT("the region round both in A"));
	Compare(A, Cached, BoxA, TEXT("A with a cache another region filled"));
	Compare(Cached, A, BoxA, TEXT("a cache another region filled in A"));

	// ---- Every river reach keeps the rules (the plan's section 5.3), read back from the records.
	TArray<FVoxelWaterBody> Bodies;
	for (const TSharedRef<const FVoxelHydrologyLake>& Lake : All.Lakes)
	{
		Bodies.Add(Lake->Body);
	}
	for (const TSharedRef<const FVoxelHydrologySystem>& System : All.Systems)
	{
		for (const FVoxelHydrologyReach& Reach : System->Reaches)
		{
			Bodies.Add(Reach.Body);
		}
	}
	FVoxelConditioningLineLimits Limits;
	Limits.VoxelSize = static_cast<float>(Voxel);
	Limits.MaxCutFill = static_cast<float>(Settings.MaxCutVoxels * Voxel) + 1.0f;
	Limits.NaturalHeight = [&Ground](const FVector& Position) { return static_cast<float>(Ground.Height(Position.X, Position.Y)); };
	int32 NumReaches = 0, Rises = 0, OverGround = 0, BadHeads = 0, EndsOnLand = 0, Checked = 0, Warned = 0, Joins = 0, Heads = 0, Conflicts = 0, OverCut = 0;
	FString FirstWarning;
	for (const TSharedRef<const FVoxelHydrologySystem>& System : All.Systems)
	{
		Conflicts += System->NumConflicts;
		for (const FVoxelHydrologyReach& Reach : System->Reaches)
		{
			++NumReaches;
			Joins += Reach.bEndsAtJoin ? 1 : 0;
			const TArray<FVoxelWaterReachVertex>& Vertices = Reach.Body.Reach->GetVertices();
			for (int32 Index = 0; Index < Vertices.Num(); ++Index)
			{
				// Rule 1: it never rises downstream.
				Rises += (Index > 0 && Vertices[Index].SurfaceLevel > Vertices[Index - 1].SurfaceLevel + 1.0e-6) ? 1 : 0;
				// Rule 2, at the centre line: the surface lies under the ground, wherever the vertex is not in other water.
				const double Height = Ground.Height(Vertices[Index].Position.X, Vertices[Index].Position.Y);
				TArray<FVoxelWaterBody> Others;
				for (const FVoxelWaterBody& Body : Bodies)
				{
					if (Body.Id != Reach.Body.Id)
					{
						Others.Add(Body);
					}
				}
				const FVoxelWaterLevels Water = FVoxelWaterLevels::Make(FVoxelSeaSurface::MakePlane(0.0), FVoxelWaterFrame(), FVoxelWaterLevels::WholeWorld(), MoveTemp(Others));
				const double Level = FVoxelRiverCourse::GetBodyLevelAt(Water, Vertices[Index].Position, Height);
				OverGround += (FMath::IsNaN(Level) && Vertices[Index].SurfaceLevel > Height + 1.0e-3) ? 1 : 0;
				if (Index == Vertices.Num() - 1 && All.Systems.Num() > 0 && BoxAll.IsInside(FVector2D(Vertices[Index].Position)))
				{
					// Rule 3: it ends in a body.
					++Checked;
					EndsOnLand += FMath::IsNaN(Level) ? 1 : 0;
				}
			}
			// Rule 4: a head starts from nothing: no floor, the bank's width alone.
			if (!Reach.Profile.bStartsInBody)
			{
				++Heads;
				BadHeads += Vertices[0].HalfWidth > BankWidth + 1.0 ? 1 : 0;
			}
			// Rule 8: the bed keeps a line's limits, the cut's cap among them.
			OverCut += Reach.Profile.DeepestCut > Settings.MaxCutVoxels * Voxel + 1.0e-3 ? 1 : 0;
			for (const FString& Warning : Reach.Bed->GetWarnings(Limits))
			{
				if (!Reach.Profile.bStartsInBody && Warning.Contains(TEXT("the levelled core is")))
				{
					continue;
				}
				++Warned;
				if (FirstWarning.IsEmpty())
				{
					FirstWarning = Warning;
				}
			}
		}
	}
	AddInfo(FString::Printf(TEXT("the rules: %d reaches (%d from a spring, %d ending at a join), %d ends checked; the first warning: %s"),
		NumReaches, Heads, Joins, Checked, FirstWarning.IsEmpty() ? TEXT("none") : *FirstWarning));
	TestTrue(TEXT("the rules: reaches from springs, reaches that join, ends inside the region (non-vacuous)"), Heads >= 3 && Joins >= 1 && Checked >= 3);
	TestEqual(TEXT("rule 1: no surface rises downstream"), Rises, 0);
	TestEqual(TEXT("rule 2: no surface stands over the ground on its line"), OverGround, 0);
	TestEqual(TEXT("rule 3: every reach ends in a body (the sea, a lake, the reach it joins)"), EndsOnLand, 0);
	TestEqual(TEXT("rule 4: every head starts from nothing"), BadHeads, 0);
	TestEqual(TEXT("rule 8: no bed breaks a line's limits"), Warned, 0);
	TestEqual(TEXT("rule 8: no bed lies deeper than the cut's cap"), OverCut, 0);
	TestEqual(TEXT("no cell has two ways on"), Conflicts, 0);

	// ---- A river that cannot be made is no river: with a cap no bed can keep, every system is refused, whole.
	FVoxelHydrologySettings Shallow = Settings;
	Shallow.MaxCutVoxels = 1.0f;
	FVoxelHydrologyRegion Refused;
	FVoxelHydrology::BuildRegion(Shallow, Ground, BoxA, nullptr, Refused);
	TestTrue(TEXT("a cap no bed keeps: every system is refused, and has no reach"), Refused.Systems.Num() == 0 && Refused.NumRefused >= 3 && Refused.Refused.Num() == Refused.NumRefused
		&& Refused.Refused[0]->Reaches.Num() == 0 && Refused.Refused[0]->Refused.Contains(TEXT("under the ground")));
	TestTrue(TEXT("... and the lakes are as they were"), Refused.Lakes.Num() == A.Lakes.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHydrologyFingerprintTest,
	"VoxelWorlds.Generation.Hydrology.Fingerprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHydrologyFingerprintTest::RunTest(const FString& Parameters)
{
	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
	Config->WorldMode = EWorldMode::InfinitePlane;
	Config->MeshingMode = EMeshingMode::DualContouring;
	const FVoxelWorldFingerprint Without = FVoxelWorldModeFactory::MakeFingerprint(*Config);
	TestFalse(TEXT("a world that names no hydrology configuration generates none"), FVoxelHydrology::IsEnabled(*Config));
	TestFalse(TEXT("... and its fingerprint has no term for it"), Without.Shape.Contains(TEXT("Hydrology")));

	UVoxelHydrologyConfiguration* Hydrology = NewObject<UVoxelHydrologyConfiguration>();
	Config->HydrologyConfiguration = Hydrology;
	const FVoxelWorldFingerprint With = FVoxelWorldModeFactory::MakeFingerprint(*Config);
	TestTrue(TEXT("a plane that names one generates hydrology"), FVoxelHydrology::IsEnabled(*Config));
	TestTrue(TEXT("... its fingerprint ends with the term: the generator's version and its settings"),
		With.Shape == Without.Shape + FString::Printf(TEXT(";Hydrology=%d:%08X"), FVoxelHydrology::Version, Hydrology->Settings.GetGenerationHash()));
	TestFalse(TEXT("... so a save made without it is refused"), With.Matches(Without));
	TestTrue(TEXT("... and the difference names it"), Without.DescribeDifferences(With).Contains(TEXT("Hydrology")));

	Hydrology->Settings.MinRiverLength += 1000.0f;
	TestFalse(TEXT("a setting that changes the bodies changes the term"), With.Matches(FVoxelWorldModeFactory::MakeFingerprint(*Config)));
	Hydrology->Settings.MinRiverLength -= 1000.0f;
	Hydrology->Settings.TileCells = 32;
	Hydrology->Settings.WarmUpViewDistances = 3.0f;
	TestTrue(TEXT("the network's values change no body, and not the term"), With.Matches(FVoxelWorldModeFactory::MakeFingerprint(*Config)));

	Config->WorldMode = EWorldMode::IslandBowl;
	TestTrue(TEXT("an island that names one generates hydrology"), FVoxelHydrology::IsEnabled(*Config) && FVoxelWorldModeFactory::MakeFingerprint(*Config).Shape.Contains(TEXT("Hydrology=")));
	// Every world mode generates hydrology since the plan's H8: a planet and a block world carry the term too.
	Config->WorldMode = EWorldMode::SphericalPlanet;
	TestTrue(TEXT("a planet that names one generates hydrology"), FVoxelHydrology::IsEnabled(*Config) && FVoxelWorldModeFactory::MakeFingerprint(*Config).Shape.Contains(TEXT("Hydrology=")));
	Config->HydrologyConfiguration = nullptr;
	TestTrue(TEXT("... and one that names none has no term"), !FVoxelHydrology::IsEnabled(*Config) && !FVoxelWorldModeFactory::MakeFingerprint(*Config).Shape.Contains(TEXT("Hydrology")));
	Config->HydrologyConfiguration = Hydrology;
	Config->WorldMode = EWorldMode::InfinitePlane;
	Config->MeshingMode = EMeshingMode::Cubic;
	TestTrue(TEXT("a block world that names one generates hydrology"), FVoxelHydrology::IsEnabled(*Config) && FVoxelWorldModeFactory::MakeFingerprint(*Config).Shape.Contains(TEXT("Hydrology=")));
	Config->HydrologyConfiguration = nullptr;
	TestTrue(TEXT("... and one that names none has no term"), !FVoxelHydrology::IsEnabled(*Config) && !FVoxelWorldModeFactory::MakeFingerprint(*Config).Shape.Contains(TEXT("Hydrology")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
