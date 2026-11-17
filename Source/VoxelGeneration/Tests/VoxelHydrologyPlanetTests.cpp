// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelBlockSurface.h"
#include "VoxelCubeSphere.h"
#include "VoxelHydrology.h"
#include "VoxelPolyline.h"
#include "VoxelRiverCourse.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The hydrology generator on a planet and on a block world, on made ground (Documentation/HYDROLOGY_PLAN.md, section
// 5.8's last paragraph; phase H8).
//
// - Generation.Hydrology.Planet: a made planet with hills, a sea and hollows, one of them on a cube face's edge and
//   one at a cube's corner. A descent that crosses a face's edge is the same cells from the lattice square it starts
//   in whichever face's region asks; a hollow on an edge fills as one lake with its footprint on the plane tangent at
//   its lowest cell, and no column of water stands against the footprint's edge; two caps that overlap across the
//   edge, a cap round both and a run with a cache another cap filled give each body with the same record; every river
//   reach keeps the rules of the plan's section 5.3 with its levels as distances from the centre and its bed a line on
//   the sphere; river reaches run across the edge and beside the corner (non-vacuous).
// - Generation.Hydrology.Blocks: a block world. A hollow's lake stands at a block layer's top with its body's level
//   inside that layer, its footprint is whole block columns, and no column of water blocks stands beside a lower
//   column outside the footprint; over rolling ground with a cliff no bed's segment is steeper than a block a column
//   (the engine's own check of a line on a block world), the beds that would have been were lowered (non-vacuous),
//   and the rules hold as on smooth ground.
// ---------------------------------------------------------------------------

namespace VoxelHydrologyPlanetTest
{
	/** The planet of the test: its datum radius. Small enough that a region is a few hundred lattice squares. */
	constexpr double PlanetRadius = 60000.0;

	/** A round dip about a direction: Depth at its middle, gone at about twice Width along the surface. */
	double Dip(const FVector& Direction, const FVector& Centre, double Depth, double Width)
	{
		const double Arc = PlanetRadius * FVoxelPolylineBounds::AngleBetween(Direction, Centre);
		return Depth * FMath::Exp(-(Arc * Arc) / (Width * Width));
	}

	/** Where the test's hollows are: on the edge between the +X and the +Y faces, at the corner of +X, +Y and +Z, and inside the +Y face. */
	FVector OnEdge() { return FVector(1.0, 1.0, 0.22).GetSafeNormal(); }
	FVector AtCorner() { return FVector(1.0, 1.0, 0.97).GetSafeNormal(); }
	FVector OnFace() { return FVector(0.45, 1.0, -0.2).GetSafeNormal(); }

	FVoxelHydrologyGround MakePlanet()
	{
		FVoxelHydrologyGround Ground;
		Ground.bRadial = true;
		Ground.PlanetCenter = FVector(1000.0, -2000.0, 500.0);
		Ground.PlanetRadius = PlanetRadius;
		Ground.Altitude = [](const FVector& Direction)
		{
			// Hills a few kilometres across over a sea at the datum, smaller relief on them, and three hollows, each in a
			// stretch of high ground of its own (so its floor stands over the sea whatever the hills do there).
			const FVector P = Direction * PlanetRadius;
			const double Hills = 500.0 + 1500.0 * FMath::Sin(P.X / 9000.0) * FMath::Cos(P.Y / 7300.0) + 800.0 * FMath::Sin((P.X + P.Z) / 5100.0) * FMath::Cos((P.Y - P.Z) / 6100.0)
				+ 160.0 * FMath::Sin(P.Z / 2100.0 + 1.7) * FMath::Sin(P.X / 2300.0 + 0.3);
			const double High = FMath::Max3(Dip(Direction, OnEdge(), 1.0, 7000.0), Dip(Direction, AtCorner(), 1.0, 7000.0), Dip(Direction, OnFace(), 1.0, 7000.0));
			return FMath::Lerp(Hills, 1400.0, High)
				- Dip(Direction, OnEdge(), 900.0, 2200.0) - Dip(Direction, AtCorner(), 900.0, 2200.0) - Dip(Direction, OnFace(), 900.0, 2200.0);
		};
		Ground.bSea = true;
		Ground.SeaLevel = 0.0;
		Ground.VoxelSize = 100.0;
		Ground.ChunkWorldSize = 3200.0;
		Ground.Seed = 4242;
		return Ground;
	}

	/** The pit a place's water runs to. */
	FIntPoint PitUnder(FVoxelHydrologyHeights& Heights, const FVector& Place)
	{
		FIntPoint Cell = Heights.GetGrid().CellAt(Place);
		FIntPoint Next;
		for (int32 Guard = 0; Guard < 4096 && Heights.GetDownhill(Cell, Next); ++Guard)
		{
			Cell = Next;
		}
		return Cell;
	}

	/** A planet cell's face. */
	int32 FaceOf(const FVoxelHydrologyGrid& Grid, const FIntPoint& Cell)
	{
		return Cell.X / FMath::Max(Grid.GetCellsPerEdge(), 1);
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
		FVector CapA, CapB;
		double AngleA = 0.0, AngleB = 0.0;
		A.Body.Lake->GetCap(CapA, AngleA);
		B.Body.Lake->GetCap(CapB, AngleB);
		return A.Body.Id == B.Body.Id && A.WaterLevel == B.WaterLevel && A.Body.Level == B.Body.Level && A.LowestCell == B.LowestCell && A.Cells.Num() == B.Cells.Num()
			&& A.Body.Lake->NumCells() == B.Body.Lake->NumCells() && CapA == CapB && AngleA == AngleB;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHydrologyPlanetTest,
	"VoxelWorlds.Generation.Hydrology.Planet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHydrologyPlanetTest::RunTest(const FString& Parameters)
{
	using namespace VoxelHydrologyPlanetTest;
	const FVoxelHydrologyGround Ground = MakePlanet();
	FVoxelHydrologySettings Settings;
	Settings.SpringSpacingCells = 8;
	Settings.MinSpringHeight = 300.0f;
	Settings.MinRiverLength = 6000.0f;
	Settings.MaxRiverLength = 40000.0f;
	Settings.MaxMeanGrade = 0.3f;
	Settings.MinLakeArea = 2.0e5f;
	Settings = FVoxelHydrology::Sanitize(Settings, Ground);
	const double Voxel = Ground.VoxelSize;
	const double BankWidth = Settings.BankWidthVoxels * Voxel;
	const FVoxelHydrologyGrid Grid(Ground, Settings);
	const int32 N = Grid.GetCellsPerEdge();
	AddInfo(FString::Printf(TEXT("the planet's grid: %d cells along a face's edge, %.1f uu wide at a face's middle, %d lattice squares an edge"),
		N, Grid.GetCellSize(), Grid.GetSquares().GetCellsPerEdge()));
	TestTrue(TEXT("the grid's cells are whole lattice squares, about the width asked for"),
		N == Grid.GetSquares().GetCellsPerEdge() * Settings.SpringSpacingCells && FMath::IsNearlyEqual(Grid.GetCellSize(), 800.0, 60.0));

	// ---- The lake on the edge between two faces: one lake, its footprint on the plane tangent at its lowest cell.
	{
		FVoxelHydrologyHeights Heights(Ground, Settings);
		const FIntPoint Pit = PitUnder(Heights, OnEdge());
		const TSharedRef<const FVoxelHydrologyLake> Lake = FVoxelHydrology::MakeLake(Settings, Heights, Pit);
		TSet<int32> Faces;
		for (const FIntPoint& Cell : Lake->Cells)
		{
			Faces.Add(FaceOf(Grid, Cell));
		}
		AddInfo(FString::Printf(TEXT("the hollow on the edge: pit (%d, %d) at %.1f, rim %.1f, closed %d, %d cells on %d faces, kept %d, depth %.1f, area %.0f m2"),
			Pit.X, Pit.Y, Heights.At(Pit), Lake->RimLevel, Lake->Closed, Lake->Cells.Num(), Faces.Num(), Lake->bKept ? 1 : 0, Lake->Depth, Lake->Area / 1.0e4));
		TestTrue(TEXT("a hollow on a face's edge: a lake, kept, its cells on both faces"), Lake->bKept && Faces.Num() == 2 && Lake->Cells.Num() >= 8);
		if (Lake->bKept)
		{
			const FVoxelWaterLake& Shape = *Lake->Body.Lake;
			TestTrue(TEXT("... a planet lake: its level a distance from the centre, the datum radius over its height"),
				Shape.IsRadial() && Lake->Body.Level == Lake->WaterLevel + PlanetRadius && Lake->WaterLevel <= Lake->RimLevel - Settings.LakeLevelMarginVoxels * Voxel + 0.01);
			TestTrue(TEXT("... its footprint covers its lowest cell"), Shape.CoversDirection(Heights.PlaceOf(Lake->LowestCell)));
			// No column of water against the footprint's edge: a cell that holds water has no cell beside it outside the
			// footprint whose ground lies under the level too.
			int32 Wet = 0, Leaks = 0;
			const auto GroundOf = [&](int32 X, int32 Y)
			{
				return Ground.Altitude((Shape.GetCellCentre(X, Y, Lake->Body.Level, Ground.PlanetCenter) - Ground.PlanetCenter).GetSafeNormal());
			};
			for (int32 Y = 0; Y < Shape.GetSize().Y; ++Y)
			{
				for (int32 X = 0; X < Shape.GetSize().X; ++X)
				{
					if (!Shape.HasCell(X, Y) || GroundOf(X, Y) >= Lake->WaterLevel)
					{
						continue;
					}
					++Wet;
					for (const FIntPoint& Beside : { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) })
					{
						Leaks += (!Shape.HasCell(X + Beside.X, Y + Beside.Y) && GroundOf(X + Beside.X, Y + Beside.Y) < Lake->WaterLevel) ? 1 : 0;
					}
				}
			}
			AddInfo(FString::Printf(TEXT("the lake on the edge: rim %.1f, level %.1f, %d cells on %d faces, %d wet footprint cells, %.2f ha, %d leaks"),
				Lake->RimLevel, Lake->WaterLevel, Lake->Cells.Num(), Faces.Num(), Wet, Lake->Area / 1.0e8, Leaks));
			TestTrue(TEXT("... its footprint holds water (non-vacuous)"), Wet > 100);
			TestEqual(TEXT("... no column of water stands against the footprint's edge"), Leaks, 0);
		}
		// The hollow at the cube's corner: its cells on three faces, one lake from whichever of them the flood starts.
		const FIntPoint CornerPit = PitUnder(Heights, AtCorner());
		const TSharedRef<const FVoxelHydrologyLake> Corner = FVoxelHydrology::MakeLake(Settings, Heights, CornerPit);
		TSet<int32> CornerFaces;
		for (const FIntPoint& Cell : Corner->Cells)
		{
			CornerFaces.Add(FaceOf(Grid, Cell));
		}
		AddInfo(FString::Printf(TEXT("the hollow at the corner: pit (%d, %d) at %.1f, rim %.1f, closed %d, %d cells on %d faces, kept %d, depth %.1f, area %.0f m2"),
			CornerPit.X, CornerPit.Y, Heights.At(CornerPit), Corner->RimLevel, Corner->Closed, Corner->Cells.Num(), CornerFaces.Num(), Corner->bKept ? 1 : 0, Corner->Depth, Corner->Area / 1.0e4));
		TestTrue(TEXT("a hollow at a cube's corner: a lake, kept, its cells on three faces"), Corner->bKept && CornerFaces.Num() == 3);
	}

	// ---- The same bodies from any region: two caps that overlap across the edge, one centred on each face.
	const FVoxelConditioningRegion CapA = FVoxelConditioningRegion::OfCap(FVector(1.0, 0.55, 0.3).GetSafeNormal(), 0.42);
	const FVoxelConditioningRegion CapB = FVoxelConditioningRegion::OfCap(FVector(0.55, 1.0, 0.3).GetSafeNormal(), 0.42);
	const FVoxelConditioningRegion CapAll = FVoxelConditioningRegion::OfCap(FVector(1.0, 1.0, 0.4).GetSafeNormal(), 0.75);
	FVoxelHydrologyRegion A, B, All, Cached, Other;
	FVoxelHydrology::BuildRegion(Settings, Ground, CapA, nullptr, A);
	FVoxelHydrology::BuildRegion(Settings, Ground, CapB, nullptr, B);
	FVoxelHydrology::BuildRegion(Settings, Ground, CapAll, nullptr, All);
	struct FCache : IVoxelHydrologyCache
	{
		TMap<FIntPoint, TSharedRef<const FVoxelHydrologyTrace>> Traces;
		TMap<FIntPoint, TSharedRef<const FVoxelHydrologyLake>> Lakes;
		TMap<FIntPoint, TSharedRef<const FVoxelHydrologySystem>> Systems;
		virtual TSharedPtr<const FVoxelHydrologyTrace> FindTrace(const FIntPoint& Square) override { const auto* Found = Traces.Find(Square); return Found ? Found->ToSharedPtr() : nullptr; }
		virtual TSharedRef<const FVoxelHydrologyTrace> StoreTrace(const TSharedRef<const FVoxelHydrologyTrace>& Trace) override { return Traces.FindOrAdd(Trace->Square, Trace); }
		virtual TSharedPtr<const FVoxelHydrologyLake> FindLake(const FIntPoint& Pit) override { const auto* Found = Lakes.Find(Pit); return Found ? Found->ToSharedPtr() : nullptr; }
		virtual TSharedRef<const FVoxelHydrologyLake> StoreLake(const TSharedRef<const FVoxelHydrologyLake>& Lake) override { return Lakes.FindOrAdd(Lake->Pit, Lake); }
		virtual TSharedPtr<const FVoxelHydrologySystem> FindSystem(const FIntPoint& Mouth) override { const auto* Found = Systems.Find(Mouth); return Found ? Found->ToSharedPtr() : nullptr; }
		virtual TSharedRef<const FVoxelHydrologySystem> StoreSystem(const TSharedRef<const FVoxelHydrologySystem>& System) override { return Systems.FindOrAdd(System->Mouth, System); }
	};
	FCache Cache;
	FVoxelHydrology::BuildRegion(Settings, Ground, CapB, &Cache, Other);
	FVoxelHydrology::BuildRegion(Settings, Ground, CapA, &Cache, Cached);

	// A region of the other kind finds nothing on a planet, and the planet's none on a heightmap world.
	{
		FVoxelHydrologyRegion None;
		FVoxelHydrology::BuildRegion(Settings, Ground, FBox2D(FVector2D(-30000.0, -30000.0), FVector2D(30000.0, 30000.0)), nullptr, None);
		TestTrue(TEXT("a box is no region of a planet: nothing is found"), None.NumSquares == 0 && None.Lakes.Num() == 0 && None.Systems.Num() == 0);
	}

	int32 ReachesA = 0, ReachesAll = 0;
	for (const TSharedRef<const FVoxelHydrologySystem>& System : A.Systems)
	{
		ReachesA += System->Reaches.Num();
	}
	for (const TSharedRef<const FVoxelHydrologySystem>& System : All.Systems)
	{
		ReachesAll += System->Reaches.Num();
	}
	AddInfo(FString::Printf(TEXT("cap A: %d squares, %d springs, %d rivers, %d systems (%d refused) with %d reaches, %d lakes; B: %d systems, %d lakes; both: %d systems with %d reaches, %d lakes"),
		A.NumSquares, A.NumSprings, A.NumRivers, A.Systems.Num(), A.NumRefused, ReachesA, A.Lakes.Num(), B.Systems.Num(), B.Lakes.Num(), All.Systems.Num(), ReachesAll, All.Lakes.Num()));
	if (!TestTrue(TEXT("the planet has rivers and lakes in each cap (the fixture)"), A.Systems.Num() >= 3 && B.Systems.Num() >= 3 && A.Lakes.Num() >= 1 && B.Lakes.Num() >= 1 && ReachesA >= 4))
	{
		return true;
	}

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
	const auto Compare = [&](const FVoxelHydrologyRegion& From, const FVoxelHydrologyRegion& In, const FVoxelConditioningRegion& InCap, const TCHAR* What)
	{
		int32 Shared = 0, Missing = 0, Different = 0;
		for (const TSharedRef<const FVoxelHydrologySystem>& System : From.Systems)
		{
			for (const FVoxelHydrologyReach& Reach : System->Reaches)
			{
				if (!FVoxelHydrology::MeetsRegion(Reach.Body, InCap, Ground))
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
			if (!FVoxelHydrology::MeetsRegion(Lake->Body, InCap, Ground))
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
	Compare(A, B, CapB, TEXT("the cap on +X in the cap on +Y"));
	Compare(B, A, CapA, TEXT("the cap on +Y in the cap on +X"));
	Compare(A, All, CapAll, TEXT("the cap on +X in the cap round both"));
	Compare(All, A, CapA, TEXT("the cap round both in the cap on +X"));
	Compare(A, Cached, CapA, TEXT("the cap on +X with a cache the other cap filled"));
	Compare(Cached, A, CapA, TEXT("a cache the other cap filled in the cap on +X"));

	// ---- Every river reach keeps the rules, with its levels as distances from the centre and its bed a line on the sphere.
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
	Limits.PlanetRadius = PlanetRadius;
	Limits.MaxCutFill = static_cast<float>(Settings.MaxCutVoxels * Voxel) + 1.0f;
	Limits.NaturalHeight = [&Ground](const FVector& Position) { return static_cast<float>(Ground.Altitude(Position)); };
	int32 NumReaches = 0, Rises = 0, OverGround = 0, BadHeads = 0, EndsOnLand = 0, Checked = 0, Warned = 0, Joins = 0, Heads = 0, Conflicts = 0, OverCut = 0;
	int32 AcrossEdge = 0, OffSphere = 0, NotRadial = 0, BesideCorner = 0;
	FString FirstWarning;
	for (const TSharedRef<const FVoxelHydrologySystem>& System : All.Systems)
	{
		Conflicts += System->NumConflicts;
		for (const FVoxelHydrologyReach& Reach : System->Reaches)
		{
			++NumReaches;
			Joins += Reach.bEndsAtJoin ? 1 : 0;
			NotRadial += (Reach.Body.Reach->IsRadial() && Reach.Bed->IsRadial()) ? 0 : 1;
			const TArray<FVoxelWaterReachVertex>& Vertices = Reach.Body.Reach->GetVertices();
			TSet<int32> Faces;
			bool bNearCorner = false;
			TArray<FVoxelWaterBody> Others;
			for (const FVoxelWaterBody& Body : Bodies)
			{
				if (Body.Id != Reach.Body.Id)
				{
					Others.Add(Body);
				}
			}
			const FVoxelWaterLevels Water = FVoxelWaterLevels::Make(Ground.GetSea(), Ground.GetFrame(), FVoxelWaterLevels::WholeWorld(), MoveTemp(Others));
			for (int32 Index = 0; Index < Vertices.Num(); ++Index)
			{
				const FVector& Place = Vertices[Index].Position;
				OffSphere += FMath::IsNearlyEqual(Place.Size(), 1.0, 1.0e-9) ? 0 : 1;
				Faces.Add(FVoxelCubeSphere::DirectionToSurface(Place).Face);
				bNearCorner |= PlanetRadius * FVoxelPolylineBounds::AngleBetween(Place, AtCorner()) < 9000.0;
				Rises += (Index > 0 && Vertices[Index].SurfaceLevel > Vertices[Index - 1].SurfaceLevel + 1.0e-6) ? 1 : 0;
				const double Height = Ground.Altitude(Place);
				const double Level = FVoxelRiverCourse::GetBodyLevelAt(Water, Place, Height);
				OverGround += (FMath::IsNaN(Level) && Vertices[Index].SurfaceLevel - PlanetRadius > Height + 1.0e-3) ? 1 : 0;
				if (Index == Vertices.Num() - 1 && FVoxelPolylineBounds::AngleBetween(Place, CapAll.Direction) < CapAll.AngularRadius)
				{
					++Checked;
					EndsOnLand += FMath::IsNaN(Level) ? 1 : 0;
				}
			}
			AcrossEdge += Faces.Num() >= 2 ? 1 : 0;
			BesideCorner += bNearCorner ? 1 : 0;
			if (!Reach.Profile.bStartsInBody)
			{
				++Heads;
				BadHeads += Vertices[0].HalfWidth > BankWidth + 1.0 ? 1 : 0;
			}
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
	AddInfo(FString::Printf(TEXT("the rules: %d reaches (%d from a spring, %d ending at a join, %d across a face's edge, %d within 90 m of the cube's corner), %d ends checked; the first warning: %s"),
		NumReaches, Heads, Joins, AcrossEdge, BesideCorner, Checked, FirstWarning.IsEmpty() ? TEXT("none") : *FirstWarning));
	TestTrue(TEXT("the rules: reaches from springs, reaches that join, ends inside the cap (non-vacuous)"), Heads >= 3 && Joins >= 1 && Checked >= 3);
	TestTrue(TEXT("river reaches run across a face's edge (non-vacuous)"), AcrossEdge >= 1);
	TestEqual(TEXT("every reach and every bed is a planet's: directions and distances from the centre"), NotRadial + OffSphere, 0);
	TestEqual(TEXT("rule 1: no surface rises downstream"), Rises, 0);
	TestEqual(TEXT("rule 2: no surface stands over the ground on its line"), OverGround, 0);
	TestEqual(TEXT("rule 3: every reach ends in a body (the sea, a lake, the reach it joins)"), EndsOnLand, 0);
	TestEqual(TEXT("rule 4: every head starts from nothing"), BadHeads, 0);
	TestEqual(TEXT("rule 8: no bed breaks a line's limits"), Warned, 0);
	TestEqual(TEXT("rule 8: no bed lies deeper than the cut's cap"), OverCut, 0);
	TestEqual(TEXT("no cell has two ways on"), Conflicts, 0);

	// ---- A descent that crosses a face's edge is the same cells from its lattice square, whoever asks: each cell is
	// one of the eight neighbours of the one before, by the relation that is the same from both sides.
	{
		FVoxelHydrologyHeights Heights(Ground, Settings);
		TArray<FIntPoint> Squares;
		FVoxelHydrology::GetSquaresFor(Settings, Ground, FVoxelConditioningRegion::OfCap(OnEdge(), 0.25), Squares);
		int32 Crossing = 0, Broken = 0, Traced = 0;
		for (const FIntPoint& Square : Squares)
		{
			const TSharedRef<const FVoxelHydrologyTrace> Trace = FVoxelHydrology::TraceSpring(Settings, Heights, Square, nullptr);
			if (Trace->Cells.Num() < 2)
			{
				continue;
			}
			++Traced;
			bool bCrosses = false;
			for (int32 Index = 1; Index < Trace->Cells.Num(); ++Index)
			{
				bCrosses |= FaceOf(Grid, Trace->Cells[Index]) != FaceOf(Grid, Trace->Cells[Index - 1]);
				bool bNeighbour = false;
				for (int32 D = 0; D < 8 && !bNeighbour; ++D)
				{
					FIntPoint Beside;
					bNeighbour = Grid.GetNeighbour(Trace->Cells[Index - 1], D, Beside) && Beside == Trace->Cells[Index];
				}
				Broken += bNeighbour ? 0 : 1;
			}
			Crossing += bCrosses ? 1 : 0;
		}
		AddInfo(FString::Printf(TEXT("descents round the edge: %d of %d squares traced, %d of them cross it"), Traced, Squares.Num(), Crossing));
		TestTrue(TEXT("descents cross the edge (non-vacuous)"), Crossing >= 2);
		TestEqual(TEXT("every step of a descent is to one of the cell's neighbours"), Broken, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHydrologyBlocksTest,
	"VoxelWorlds.Generation.Hydrology.Blocks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHydrologyBlocksTest::RunTest(const FString& Parameters)
{
	const auto Dip = [](double X, double Y, double CentreX, double CentreY, double Depth, double Width)
	{
		return Depth * FMath::Exp(-(FMath::Square(X - CentreX) + FMath::Square(Y - CentreY)) / (Width * Width));
	};
	const auto MakeBlocks = [](TFunction<double(double, double)> Height)
	{
		FVoxelHydrologyGround Ground;
		Ground.Height = MoveTemp(Height);
		Ground.bBlocks = true;
		Ground.BlockOrigin = FVector::ZeroVector;
		Ground.bSea = true;
		Ground.SeaLevel = 0.0;
		Ground.VoxelSize = 100.0;
		Ground.ChunkWorldSize = 3200.0;
		Ground.Seed = 4242;
		return Ground;
	};
	FVoxelHydrologySettings Base;
	Base.MinLakeArea = 2.0e5f;
	const double Voxel = 100.0;

	// ---- A hollow on a slope: the lake's water is whole blocks.
	{
		const FVoxelHydrologyGround Ground = MakeBlocks([&Dip](double X, double Y) { return 4000.0 - 0.08 * X - Dip(X, Y, 0.0, 0.0, 700.0, 2500.0); });
		const FVoxelHydrologySettings Settings = FVoxelHydrology::Sanitize(Base, Ground);
		FVoxelHydrologyHeights Heights(Ground, Settings);
		FIntPoint Pit(0, 0), Next;
		for (int32 Guard = 0; Guard < 4096 && Heights.GetDownhill(Pit, Next); ++Guard)
		{
			Pit = Next;
		}
		const TSharedRef<const FVoxelHydrologyLake> Lake = FVoxelHydrology::MakeLake(Settings, Heights, Pit);
		if (TestTrue(TEXT("a hollow on a block world: an open lake, kept"), Lake->IsOpen() && Lake->bKept))
		{
			const FVoxelWaterLake& Shape = *Lake->Body.Lake;
			TestTrue(TEXT("... the water's top is a block layer's top"), FMath::IsNearlyEqual(FMath::Fmod(Lake->WaterLevel, Voxel), 0.0, 1.0e-6) && Lake->Depth >= 2.0 * Voxel);
			TestTrue(TEXT("... and the body's level lies in that layer, a tenth of a voxel under its top"), FMath::IsNearlyEqual(Lake->Body.Level, Lake->WaterLevel - 0.1 * Voxel, 1.0e-6));
			TestTrue(TEXT("... its footprint's cells are block columns"), Shape.GetCellSize() == Voxel
				&& FMath::IsNearlyEqual(FMath::Fmod(Shape.GetBounds2D().Min.X, Voxel), 0.0, 1.0e-6) && FMath::IsNearlyEqual(FMath::Fmod(Shape.GetBounds2D().Min.Y, Voxel), 0.0, 1.0e-6));
			// In blocks: a column holds water when it is in the footprint and its topmost block's top lies under the
			// water's. No such column has a column beside it, outside the footprint, whose top lies under the water's too.
			const auto TopOf = [&Ground](double X, double Y) { return Ground.BlockTopFor(Ground.Height(X, Y)); };
			const FBox2D Bounds = Shape.GetBounds2D();
			int32 Wet = 0, Leaks = 0, OverRim = 0;
			for (double Y = Bounds.Min.Y; Y < Bounds.Max.Y - 0.5; Y += Voxel)
			{
				for (double X = Bounds.Min.X; X < Bounds.Max.X - 0.5; X += Voxel)
				{
					// A column is asked about at its middle; its ground is read at its lower corner.
					if (!Shape.Covers(X + 0.5 * Voxel, Y + 0.5 * Voxel) || TopOf(X, Y) >= Lake->WaterLevel)
					{
						continue;
					}
					++Wet;
					for (const FVector2D& Beside : { FVector2D(Voxel, 0.0), FVector2D(-Voxel, 0.0), FVector2D(0.0, Voxel), FVector2D(0.0, -Voxel) })
					{
						Leaks += (!Shape.Covers(X + Beside.X + 0.5 * Voxel, Y + Beside.Y + 0.5 * Voxel) && TopOf(X + Beside.X, Y + Beside.Y) < Lake->WaterLevel) ? 1 : 0;
					}
				}
			}
			// The water's top is no higher than the lowest block top on the way out over the saddle.
			OverRim += Lake->WaterLevel > Ground.BlockTopFor(Heights.At(Lake->Saddle)) + 1.0e-6 ? 1 : 0;
			AddInfo(FString::Printf(TEXT("the block lake: rim %.1f, water to %.1f, the body's level %.1f, %d wet columns, %.2f ha, %d leaks"),
				Lake->RimLevel, Lake->WaterLevel, Lake->Body.Level, Wet, Lake->Area / 1.0e8, Leaks));
			TestTrue(TEXT("... its footprint holds water (non-vacuous)"), Wet > 400);
			TestEqual(TEXT("... no column of water blocks stands beside a lower column outside the footprint"), Leaks, 0);
			TestEqual(TEXT("... the water stands no higher than the saddle's block"), OverRim, 0);
		}
	}

	// ---- Rivers over rolling ground with a cliff across it: no bed steps more than a block a column.
	{
		const FVoxelHydrologyGround Ground = MakeBlocks([&Dip](double X, double Y)
		{
			// The plane test's hills, and a cliff of four blocks along a line the rivers come down across.
			const double Cliff = 400.0 * FMath::SmoothStep(0.0, 250.0, X + 0.35 * Y - 9000.0);
			return 900.0 + 2600.0 * FMath::Sin(X / 21000.0) * FMath::Cos(Y / 17000.0) + 1300.0 * FMath::Sin((X + Y) / 9000.0) * FMath::Cos((X - Y) / 12500.0)
				+ 260.0 * FMath::Sin(X / 2900.0 + 1.7) * FMath::Sin(Y / 3300.0 + 0.3) + Cliff
				- Dip(X, Y, 12000.0, -6000.0, 900.0, 1600.0) - Dip(X, Y, 20000.0, 2000.0, 900.0, 1600.0);
		});
		FVoxelHydrologySettings Settings;
		Settings.SpringSpacingCells = 8;
		Settings.MinSpringHeight = 600.0f;
		Settings.MinRiverLength = 12000.0f;
		Settings.MaxRiverLength = 60000.0f;
		Settings.MaxMeanGrade = 0.3f;
		Settings.MinLakeArea = 2.0e5f;
		Settings = FVoxelHydrology::Sanitize(Settings, Ground);
		const FBox2D Box(FVector2D(-30000.0, -30000.0), FVector2D(45000.0, 25000.0));
		FVoxelHydrologyRegion Region;
		FVoxelHydrology::BuildRegion(Settings, Ground, Box, nullptr, Region);

		FVoxelConditioningLineLimits Limits;
		Limits.VoxelSize = static_cast<float>(Voxel);
		Limits.bBlockWorld = true;
		int32 NumReaches = 0, Steep = 0, Held = 0, Rises = 0, Warned = 0, BedOverSurface = 0, OffLayer = 0;
		double SteepestBed = 0.0, MostHeld = 0.0;
		FString FirstWarning;
		for (const TSharedRef<const FVoxelHydrologySystem>& System : Region.Systems)
		{
			for (const FVoxelHydrologyReach& Reach : System->Reaches)
			{
				++NumReaches;
				Held += Reach.Profile.NumGradeHolds > 0 ? 1 : 0;
				MostHeld = FMath::Max(MostHeld, Reach.Profile.MostGradeHold);
				const TArray<FVoxelWaterReachVertex>& Vertices = Reach.Body.Reach->GetVertices();
				for (int32 Index = 0; Index < Vertices.Num(); ++Index)
				{
					BedOverSurface += Vertices[Index].BedLevel > Vertices[Index].SurfaceLevel + 1.0e-6 ? 1 : 0;
					if (Index == 0)
					{
						continue;
					}
					Rises += Vertices[Index].SurfaceLevel > Vertices[Index - 1].SurfaceLevel + 1.0e-6 ? 1 : 0;
					const double Run = FVector2D::Distance(FVector2D(Vertices[Index].Position), FVector2D(Vertices[Index - 1].Position));
					const double Grade = FMath::Abs(Vertices[Index].BedLevel - Vertices[Index - 1].BedLevel) / FMath::Max(Run, 1.0);
					SteepestBed = FMath::Max(SteepestBed, Grade);
					Steep += Grade > UE_DOUBLE_INV_SQRT_2 ? 1 : 0;
				}
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
		for (const TSharedRef<const FVoxelHydrologyLake>& Lake : Region.Lakes)
		{
			OffLayer += FMath::IsNearlyEqual(FMath::Fmod(FMath::Abs(Lake->WaterLevel), Voxel), 0.0, 1.0e-6) ? 0 : 1;
		}
		AddInfo(FString::Printf(TEXT("a block world's rivers: %d systems (%d refused) with %d reaches, %d of them with a bed lowered for the grade (by up to %.0f uu); the steepest bed segment %.0f %%; %d lakes; the first warning: %s"),
			Region.Systems.Num(), Region.NumRefused, NumReaches, Held, MostHeld, 100.0 * SteepestBed, Region.Lakes.Num(), FirstWarning.IsEmpty() ? TEXT("none") : *FirstWarning));
		TestTrue(TEXT("the block world has rivers and lakes (the fixture)"), Region.Systems.Num() >= 3 && NumReaches >= 4 && Region.Lakes.Num() >= 1);
		TestTrue(TEXT("beds were lowered to hold a block a column (non-vacuous)"), Held >= 1 && MostHeld > 50.0);
		TestEqual(TEXT("no bed's segment is steeper than a block a column"), Steep, 0);
		TestEqual(TEXT("no bed breaks a line's limits on a block world (the engine's own check)"), Warned, 0);
		TestEqual(TEXT("no surface rises downstream, and no bed stands over its surface"), Rises + BedOverSurface, 0);
		TestEqual(TEXT("every lake's water stands at a block layer's top"), OffLayer, 0);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
