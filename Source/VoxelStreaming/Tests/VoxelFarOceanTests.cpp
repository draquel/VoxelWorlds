// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "ProceduralMeshComponent.h"
#include "VoxelFarOcean.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "DistanceBandLODStrategy.h"
#include "IVoxelMeshRenderer.h"
#include "IslandBowlWorldMode.h"
#include "VoxelWorldModeFactory.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The far ocean of a bounded world (WORLD_MODES_REVIEW_PLAN.md §14.2 WM-E1, finding KI2).
//
// Geometry (pure, headless): the grid water covers every grid column except the open ones, each exactly once, and a
// window build plus a frame build of the same window cover exactly what the whole grid does; the open sea covers the
// frame from the grid out to the outer rectangle exactly once and nothing inside the grid; every water quad has its
// underside; the seabed lies outside the edge, under the seabed profile by the offset, and reaches the open sea's corners.
//
// Streaming (a real chunk manager on a small island, headless): the columns that stream are those that touch the
// footprint; the far ocean leaves open exactly the columns holding loaded chunks or a water tile, so no water tile is
// ever under the far ocean's water and every other grid column is covered, before and after the viewer moves; the far
// ocean's meshes are their own components, so a recreated terrain proxy (G15) leaves them as they are.
// ---------------------------------------------------------------------------

namespace VoxelFarOceanTest
{
	/** Up-facing triangles of a mesh as XY triangles (the winding the water mesher uses: clockwise in X and Y). */
	struct FTri2D
	{
		FVector2D A, B, C;
		FBox2D Box;
	};

	void CollectTriangles(const FChunkMeshData& Mesh, const FVector& Origin, bool bUpFacing, TArray<FTri2D>& Out)
	{
		for (int32 i = 0; i + 2 < Mesh.Indices.Num(); i += 3)
		{
			const FVector3f& P0 = Mesh.Positions[Mesh.Indices[i]];
			const FVector3f& P1 = Mesh.Positions[Mesh.Indices[i + 1]];
			const FVector3f& P2 = Mesh.Positions[Mesh.Indices[i + 2]];
			const double Cross = (double(P1.X) - P0.X) * (double(P2.Y) - P0.Y) - (double(P1.Y) - P0.Y) * (double(P2.X) - P0.X);
			if ((Cross < 0.0) != bUpFacing)
			{
				continue;
			}
			FTri2D& T = Out.AddDefaulted_GetRef();
			T.A = FVector2D(P0.X + Origin.X, P0.Y + Origin.Y);
			T.B = FVector2D(P1.X + Origin.X, P1.Y + Origin.Y);
			T.C = FVector2D(P2.X + Origin.X, P2.Y + Origin.Y);
			T.Box = FBox2D(ForceInit);
			T.Box += T.A;
			T.Box += T.B;
			T.Box += T.C;
		}
	}

	bool Inside(const FTri2D& T, const FVector2D& P)
	{
		auto Side = [](const FVector2D& A, const FVector2D& B, const FVector2D& P)
		{
			return (B.X - A.X) * (P.Y - A.Y) - (B.Y - A.Y) * (P.X - A.X);
		};
		const double D0 = Side(T.A, T.B, P), D1 = Side(T.B, T.C, P), D2 = Side(T.C, T.A, P);
		const bool bNeg = D0 < 0 || D1 < 0 || D2 < 0;
		const bool bPos = D0 > 0 || D1 > 0 || D2 > 0;
		return !(bNeg && bPos);
	}

	int32 CountCover(const TArray<FTri2D>& Tris, const FVector2D& P)
	{
		int32 Count = 0;
		for (const FTri2D& T : Tris)
		{
			if (T.Box.IsInsideOrOn(P) && Inside(T, P))
			{
				++Count;
			}
		}
		return Count;
	}

	FVoxelFarOceanSettings MakeSettings(EVoxelWorldBoundsShape Shape)
	{
		FIslandBowlParams Island(30000.0f, 10000.0f, EIslandFalloffType::Smooth);
		Island.CenterX = 12345.0f;
		Island.CenterY = -4321.0f;
		Island.EdgeHeight = -3500.0f;
		Island.SeabedDepth = 6000.0f;
		Island.SeabedSlopeWidth = 20000.0f;
		Island.Shape = Shape == EVoxelWorldBoundsShape::Rectangle ? EIslandShape::Rectangle : EIslandShape::Circular;
		Island.SizeY = 20000.0f;

		FVoxelFarOceanSettings S;
		S.Bounds.Shape = Shape;
		S.Bounds.Center = FVector(Island.CenterX, Island.CenterY, 0.0);
		S.Bounds.HalfExtent = FVector2D(Island.GetTotalExtentX(), Island.GetTotalExtentY());
		S.Bounds.OuterRadius = Shape == EVoxelWorldBoundsShape::Circle ? Island.GetTotalExtent() : 0.0f;
		S.GridOrigin = FVector2D(100.0, -250.0); // a WorldOrigin off zero: the grid follows the chunk columns
		S.ColumnSize = 4800.0;
		S.VoxelSize = 75.0;
		S.WaterZ = -1500.0;
		S.OuterDistance = 2.0e6;
		S.SeabedSlopeWidth = Island.SeabedSlopeWidth;
		S.SeabedOffset = 37.5;
		S.FloorDrop = 75.0;
		S.EdgeZ = Island.EdgeHeight;
		S.SeabedMaterialID = 7;
		S.SeabedHeight = [Island](double D) { return double(FIslandBowlWorldMode::GetSeabedHeight(float(D), Island)); };
		return S;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFarOceanGeometryTest,
	"VoxelWorlds.WorldModes.FarOcean.Geometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelFarOceanGeometryTest::RunTest(const FString& Parameters)
{
	using namespace VoxelFarOceanTest;

	for (const EVoxelWorldBoundsShape Shape : { EVoxelWorldBoundsShape::Circle, EVoxelWorldBoundsShape::Rectangle })
	{
		const FVoxelFarOceanSettings S = MakeSettings(Shape);
		const TCHAR* Name = Shape == EVoxelWorldBoundsShape::Circle ? TEXT("Circle") : TEXT("Rectangle");
		const FVector Origin = S.GetMeshOrigin();
		FIntPoint Min, Max;
		TestTrue(FString::Printf(TEXT("%s: has a grid"), Name), FVoxelFarOcean::GetGridColumns(S, Min, Max));

		// Open columns: a loaded disc near the east edge, as a viewer standing on the coast streams it.
		const FVector2D Viewer(S.Bounds.Center.X + 32000.0, S.Bounds.Center.Y + 3000.0);
		TSet<FIntPoint> Open;
		for (int32 X = Min.X; X <= Max.X; ++X)
		{
			for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
			{
				const FVector2D C(S.GridOrigin.X + (X + 0.5) * S.ColumnSize, S.GridOrigin.Y + (Y + 0.5) * S.ColumnSize);
				const FBox2D Column(C - FVector2D(S.ColumnSize * 0.5), C + FVector2D(S.ColumnSize * 0.5));
				if (FVector2D::Distance(C, Viewer) < 20000.0 && S.Bounds.IntersectsBox2D(Column))
				{
					Open.Add(FIntPoint(X, Y));
				}
			}
		}
		TestTrue(FString::Printf(TEXT("%s: some columns open"), Name), Open.Num() > 10);

		FChunkMeshData Grid, Sea, Seabed, GridUnder, SeaUnder;
		const int32 GridQuads = FVoxelFarOcean::BuildGridWater(S, Open, Grid, &GridUnder);
		const int32 SeaQuads = FVoxelFarOcean::BuildOpenSeaWater(S, Sea, &SeaUnder);
		const int32 SeabedTris = FVoxelFarOcean::BuildSeabed(S, Seabed);
		AddInfo(FString::Printf(TEXT("%s: grid %dx%d, %d open, %d grid quads, %d open-sea quads, %d seabed triangles"),
			Name, Max.X - Min.X + 1, Max.Y - Min.Y + 1, Open.Num(), GridQuads, SeaQuads, SeabedTris));

		// The water faces up; the undersides are separate meshes of the same quads facing down, with the up normal.
		TArray<FTri2D> GridUp, GridDown, SeaUp, SeaDown, Ignored;
		CollectTriangles(Grid, Origin, true, GridUp);
		CollectTriangles(GridUnder, Origin, false, GridDown);
		CollectTriangles(Sea, Origin, true, SeaUp);
		CollectTriangles(SeaUnder, Origin, false, SeaDown);
		CollectTriangles(Grid, Origin, false, Ignored);
		CollectTriangles(Sea, Origin, false, Ignored);
		CollectTriangles(GridUnder, Origin, true, Ignored);
		CollectTriangles(SeaUnder, Origin, true, Ignored);
		TestEqual(FString::Printf(TEXT("%s: the water faces up only, the undersides down only"), Name), Ignored.Num(), 0);
		TestEqual(FString::Printf(TEXT("%s: the grid's underside matches its water"), Name), GridDown.Num(), GridUp.Num());
		TestEqual(FString::Printf(TEXT("%s: the open sea's underside matches it"), Name), SeaDown.Num(), SeaUp.Num());
		bool bUpNormals = true;
		for (const FVector3f& N : GridUnder.Normals) { bUpNormals &= N.Z > 0.99f; }
		TestTrue(FString::Printf(TEXT("%s: the underside shades with the up normal, as the water tiles do from below"), Name), bUpNormals);
		TestEqual(FString::Printf(TEXT("%s: two up triangles per grid quad"), Name), GridUp.Num(), GridQuads * 2);
		bool bFlat = true;
		for (const FVector3f& P : Grid.Positions) { bFlat &= FMath::IsNearlyEqual(double(P.Z) + Origin.Z, S.WaterZ, 0.01); }
		for (const FVector3f& P : Sea.Positions) { bFlat &= FMath::IsNearlyEqual(double(P.Z) + Origin.Z, S.WaterZ, 0.01); }
		TestTrue(FString::Printf(TEXT("%s: the water lies at the water level"), Name), bFlat);

		// The grid: points inside every column, near its corners and its middle, covered once unless open, never if
		// open. The offsets are uneven so no point lies on a quad's diagonal (two triangles would both count it).
		int32 GridErrors = 0, SeaInGrid = 0;
		for (int32 X = Min.X; X <= Max.X; ++X)
		{
			for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
			{
				const double X0 = S.GridOrigin.X + X * S.ColumnSize;
				const double Y0 = S.GridOrigin.Y + Y * S.ColumnSize;
				const int32 Expected = Open.Contains(FIntPoint(X, Y)) ? 0 : 1;
				for (const FVector2D& P : { FVector2D(X0 + 0.371 * S.ColumnSize, Y0 + 0.613 * S.ColumnSize), FVector2D(X0 + 3.1, Y0 + 7.3),
					FVector2D(X0 + S.ColumnSize - 5.3, Y0 + S.ColumnSize - 11.7), FVector2D(X0 + 9.7, Y0 + S.ColumnSize - 4.1) })
				{
					GridErrors += CountCover(GridUp, P) != Expected ? 1 : 0;
					SeaInGrid += CountCover(SeaUp, P);
				}
			}
		}
		TestEqual(FString::Printf(TEXT("%s: each grid column covered once unless open, open ones not at all"), Name), GridErrors, 0);
		TestEqual(FString::Printf(TEXT("%s: the open sea stays out of the grid"), Name), SeaInGrid, 0);

		// The open sea: rays from the centre between the grid's rectangle and the outer one, covered exactly once.
		const FBox2D Outer = FVoxelFarOcean::GetOuterBox(S);
		const FBox2D GridBox(FVector2D(S.GridOrigin.X + Min.X * S.ColumnSize, S.GridOrigin.Y + Min.Y * S.ColumnSize),
			FVector2D(S.GridOrigin.X + (Max.X + 1) * S.ColumnSize, S.GridOrigin.Y + (Max.Y + 1) * S.ColumnSize));
		int32 SeaErrors = 0, SeaSamples = 0;
		for (int32 Ray = 0; Ray < 72; ++Ray)
		{
			const double Angle = UE_DOUBLE_TWO_PI * (Ray + 0.31) / 72.0;
			const FVector2D Dir(FMath::Cos(Angle), FMath::Sin(Angle));
			for (double T = 1000.0; T < 3.0e6; T *= 1.15)
			{
				const FVector2D P = FVector2D(S.Bounds.Center.X, S.Bounds.Center.Y) + Dir * T;
				if (GridBox.IsInsideOrOn(P) || !Outer.IsInside(P))
				{
					continue;
				}
				++SeaSamples;
				SeaErrors += CountCover(SeaUp, P) != 1 ? 1 : 0;
				SeaErrors += CountCover(GridUp, P) != 0 ? 1 : 0;
			}
		}
		TestTrue(FString::Printf(TEXT("%s: the open sea was sampled (%d points)"), Name, SeaSamples), SeaSamples > 1000);
		TestEqual(FString::Printf(TEXT("%s: the frame out to the horizon is covered exactly once"), Name), SeaErrors, 0);
		TestTrue(FString::Printf(TEXT("%s: the sea reaches OuterDistance past the edge"), Name),
			Outer.Max.X - S.Bounds.Center.X >= S.Bounds.HalfExtent.X + S.OuterDistance - 1.0);

		// The seabed: outside the edge, under the profile by the offset (vertices exactly, triangle centroids and edge
		// midpoints within a few uu of it), facing up, out to the open sea's corners.
		double MaxDistance = 0.0, WorstInside = 0.0, WorstVertex = 0.0, WorstInterior = 0.0;
		int32 DownFacing = 0;
		for (const FVector3f& P : Seabed.Positions)
		{
			const FVector W = FVector(P) + Origin;
			const double D = FVoxelFarOcean::GetDistancePastEdge(S.Bounds, W.X, W.Y);
			MaxDistance = FMath::Max(MaxDistance, D);
			WorstInside = FMath::Min(WorstInside, D);
			WorstVertex = FMath::Max(WorstVertex, FMath::Abs(W.Z - (S.SeabedHeight(FMath::Max(D, 0.0)) - S.SeabedOffset)));
		}
		for (int32 i = 0; i + 2 < Seabed.Indices.Num(); i += 3)
		{
			const FVector A = FVector(Seabed.Positions[Seabed.Indices[i]]) + Origin;
			const FVector B = FVector(Seabed.Positions[Seabed.Indices[i + 1]]) + Origin;
			const FVector C = FVector(Seabed.Positions[Seabed.Indices[i + 2]]) + Origin;
			const double Cross = (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
			DownFacing += Cross < 0.0 ? 0 : 1;
			for (const FVector& P : { (A + B + C) / 3.0, (A + B) * 0.5, (B + C) * 0.5, (C + A) * 0.5 })
			{
				const double D = FVoxelFarOcean::GetDistancePastEdge(S.Bounds, P.X, P.Y);
				// How far above the profile minus the offset the drawn seabed rises (it must stay under the voxel seabed).
				WorstInterior = FMath::Max(WorstInterior, P.Z - (S.SeabedHeight(FMath::Max(D, 0.0)) - S.SeabedOffset));
			}
		}
		AddInfo(FString::Printf(TEXT("%s seabed: reaches %.0f uu past the edge, innermost vertex %.1f, vertex error %.3f, interior rises %.2f uu over the offset profile"),
			Name, MaxDistance, WorstInside, WorstVertex, WorstInterior));
		TestTrue(FString::Printf(TEXT("%s: no seabed vertex inside the edge"), Name), WorstInside >= -0.01);
		TestTrue(FString::Printf(TEXT("%s: seabed vertices on the offset profile"), Name), WorstVertex < 0.5);
		TestTrue(FString::Printf(TEXT("%s: the seabed between vertices stays well under the voxel seabed (within a quarter of the offset)"), Name),
			WorstInterior < S.SeabedOffset * 0.25);
		TestEqual(FString::Printf(TEXT("%s: every seabed triangle faces up"), Name), DownFacing, 0);
		TestTrue(FString::Printf(TEXT("%s: the seabed reaches the open sea's farthest corner"), Name),
			MaxDistance >= S.OuterDistance - 1.0);
		bool bMaterial = Seabed.UV1s.Num() == Seabed.Positions.Num();
		for (const FVector2f& UV : Seabed.UV1s) { bMaterial &= FMath::IsNearlyEqual(UV.X, 7.0f); }
		TestTrue(FString::Printf(TEXT("%s: the seabed carries its material id as terrain vertices do"), Name), bMaterial);

		// The floor: under every covered column that touches the footprint (not under open ones, not wholly outside),
		// at EdgeZ inside the edge and under the seabed outside it, always below the drawn seabed.
		FChunkMeshData Floor;
		const int32 FloorTris = FVoxelFarOcean::BuildGridFloor(S, Open, Floor);
		TArray<FTri2D> FloorUp;
		CollectTriangles(Floor, Origin, true, FloorUp);
		TestEqual(FString::Printf(TEXT("%s: every floor triangle faces up"), Name), FloorUp.Num(), FloorTris);
		int32 FloorErrors = 0;
		double WorstFloorAbove = -TNumericLimits<double>::Max();
		for (int32 X = Min.X; X <= Max.X; ++X)
		{
			for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
			{
				const FVector2D Lo(S.GridOrigin.X + X * S.ColumnSize, S.GridOrigin.Y + Y * S.ColumnSize);
				const bool bTouches = S.Bounds.IntersectsBox2D(FBox2D(Lo, Lo + FVector2D(S.ColumnSize)));
				const int32 Expected = (!Open.Contains(FIntPoint(X, Y)) && bTouches) ? 1 : 0;
				const FVector2D P = Lo + FVector2D(0.371 * S.ColumnSize, 0.613 * S.ColumnSize);
				FloorErrors += CountCover(FloorUp, P) != Expected ? 1 : 0;
			}
		}
		for (const FVector3f& P : Floor.Positions)
		{
			const FVector W = FVector(P) + Origin;
			const double D = FVoxelFarOcean::GetDistancePastEdge(S.Bounds, W.X, W.Y);
			const double DrawnSeabed = (D <= 0.0 ? S.EdgeZ : S.SeabedHeight(D)) - S.SeabedOffset;
			WorstFloorAbove = FMath::Max(WorstFloorAbove, W.Z - DrawnSeabed);
		}
		AddInfo(FString::Printf(TEXT("%s floor: %d triangles; at its highest %.1f uu relative to the drawn seabed"), Name, FloorTris, WorstFloorAbove));
		TestEqual(FString::Printf(TEXT("%s: the floor lies under exactly the covered columns that touch the footprint"), Name), FloorErrors, 0);
		TestTrue(FString::Printf(TEXT("%s: the floor stays under the drawn seabed"), Name), WorstFloorAbove <= -S.FloorDrop + 0.5);

		// The chunk manager builds the grid in two parts: a window around the open columns and the frame outside it. Two
		// windows: the open columns' bounds plus a margin, and one that runs off the grid's corner.
		FIntPoint OpenMin(TNumericLimits<int32>::Max()), OpenMax(TNumericLimits<int32>::Lowest());
		for (const FIntPoint& Column : Open)
		{
			OpenMin = OpenMin.ComponentMin(Column);
			OpenMax = OpenMax.ComponentMax(Column);
		}
		for (const FIntRect& Window : { FIntRect(OpenMin - FIntPoint(3), OpenMax + FIntPoint(4)), FIntRect(Max - FIntPoint(5), Max + FIntPoint(9)) })
		{
			FChunkMeshData WindowWater, FrameWater, WindowFloor, FrameFloor;
			const int32 WindowQuads = FVoxelFarOcean::BuildGridWater(S, Open, WindowWater, nullptr, EVoxelFarOceanGridPart::Window, Window);
			const int32 FrameQuads = FVoxelFarOcean::BuildGridWater(S, Open, FrameWater, nullptr, EVoxelFarOceanGridPart::Frame, Window);
			FVoxelFarOcean::BuildGridFloor(S, Open, WindowFloor, EVoxelFarOceanGridPart::Window, Window);
			FVoxelFarOcean::BuildGridFloor(S, Open, FrameFloor, EVoxelFarOceanGridPart::Frame, Window);
			TArray<FTri2D> WindowUp, FrameUp, WindowFloorUp, FrameFloorUp;
			CollectTriangles(WindowWater, Origin, true, WindowUp);
			CollectTriangles(FrameWater, Origin, true, FrameUp);
			CollectTriangles(WindowFloor, Origin, true, WindowFloorUp);
			CollectTriangles(FrameFloor, Origin, true, FrameFloorUp);
			int32 PartErrors = 0, FloorPartErrors = 0, WindowOutside = 0;
			for (int32 X = Min.X; X <= Max.X; ++X)
			{
				for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
				{
					const FVector2D P(S.GridOrigin.X + (X + 0.371) * S.ColumnSize, S.GridOrigin.Y + (Y + 0.613) * S.ColumnSize);
					const int32 InWindow = CountCover(WindowUp, P);
					PartErrors += InWindow + CountCover(FrameUp, P) != CountCover(GridUp, P) ? 1 : 0;
					WindowOutside += (!Window.Contains(FIntPoint(X, Y)) && InWindow > 0) ? 1 : 0;
					FloorPartErrors += CountCover(WindowFloorUp, P) + CountCover(FrameFloorUp, P) != CountCover(FloorUp, P) ? 1 : 0;
				}
			}
			AddInfo(FString::Printf(TEXT("%s window (%d,%d)-(%d,%d): %d quads, frame %d quads (whole grid %d)"), Name,
				Window.Min.X, Window.Min.Y, Window.Max.X, Window.Max.Y, WindowQuads, FrameQuads, GridQuads));
			TestEqual(FString::Printf(TEXT("%s: window plus frame cover each column as the whole grid does"), Name), PartErrors, 0);
			TestEqual(FString::Printf(TEXT("%s: the window's water stays inside it"), Name), WindowOutside, 0);
			TestEqual(FString::Printf(TEXT("%s: window plus frame floors cover as the whole floor does"), Name), FloorPartErrors, 0);
			TestTrue(FString::Printf(TEXT("%s: the window holds water"), Name), WindowQuads > 0);
		}

		// Inland coverage 0: unloaded columns wholly inside the edge are left empty, the ones that reach it stay covered.
		FVoxelFarOceanSettings Coastal = S;
		Coastal.InlandCoverage = 0.0;
		FChunkMeshData CoastalGrid;
		FVoxelFarOcean::BuildGridWater(Coastal, Open, CoastalGrid);
		TArray<FTri2D> CoastalUp;
		CollectTriangles(CoastalGrid, Origin, true, CoastalUp);
		const FVector2D Centre(S.Bounds.Center.X, S.Bounds.Center.Y);
		TestEqual(FString::Printf(TEXT("%s: inland coverage 0 leaves the island's middle empty"), Name), CountCover(CoastalUp, Centre), 0);
		TestEqual(FString::Printf(TEXT("%s: ... and still covers past the edge"), Name),
			CountCover(CoastalUp, Centre - FVector2D(S.Bounds.HalfExtent.X + 2000.0, 0.0)), 1);
		FChunkMeshData CoastalFloor;
		FVoxelFarOcean::BuildGridFloor(Coastal, Open, CoastalFloor);
		TArray<FTri2D> CoastalFloorUp;
		CollectTriangles(CoastalFloor, Origin, true, CoastalFloorUp);
		TestEqual(FString::Printf(TEXT("%s: ... with no floor where it leaves no water"), Name), CountCover(CoastalFloorUp, Centre), 0);
	}

	// No far ocean for a plane.
	FVoxelFarOceanSettings Plane;
	FChunkMeshData Mesh;
	FIntPoint Min, Max;
	TestFalse(TEXT("Unbounded: no grid"), FVoxelFarOcean::GetGridColumns(Plane, Min, Max));
	TestEqual(TEXT("Unbounded: no open sea"), FVoxelFarOcean::BuildOpenSeaWater(Plane, Mesh), 0);
	TestEqual(TEXT("Unbounded: no seabed"), FVoxelFarOcean::BuildSeabed(Plane, Mesh), 0);
	return true;
}

namespace VoxelFarOceanTest
{
	/** A renderer that records chunk meshes and water tiles (what the far ocean must never cover). */
	class FWaterRecordingRenderer : public IVoxelMeshRenderer
	{
	public:
		TSet<FIntVector> Chunks;
		TSet<FIntVector2> WaterTiles;

		virtual void Initialize(UWorld* World, const UVoxelWorldConfiguration* WorldConfig) override { bInitialized = true; }
		virtual void Shutdown() override { bInitialized = false; }
		virtual bool IsInitialized() const override { return bInitialized; }
		virtual void UpdateChunkMesh(const FChunkRenderData& RenderData) override {}
		virtual void UpdateChunkMeshFromCPU(const FIntVector& ChunkCoord, int32 LODLevel, const FChunkMeshData& MeshData) override { Chunks.Add(ChunkCoord); }
		virtual void RemoveChunk(const FIntVector& ChunkCoord) override { Chunks.Remove(ChunkCoord); }
		virtual void ClearAllChunks() override { Chunks.Reset(); }
		virtual void SetChunkVisible(const FIntVector& ChunkCoord, bool bVisible) override {}
		virtual void SetAllChunksVisible(bool bVisible) override {}
		virtual void SetMaterial(UMaterialInterface* Material) override {}
		virtual UMaterialInterface* GetMaterial() const override { return nullptr; }
		virtual void UpdateMaterialParameters() override {}
		virtual void UpdateWaterTileMesh(const FIntVector2& TileCoord, const FChunkMeshData& WaterMeshData) override { WaterTiles.Add(TileCoord); }
		virtual void RemoveWaterTile(const FIntVector2& TileCoord) override { WaterTiles.Remove(TileCoord); }
		virtual void ClearAllWaterTiles() override { WaterTiles.Reset(); }
		virtual void UpdateLODTransition(const FIntVector& ChunkCoord, float MorphFactor) override {}
		virtual bool IsChunkLoaded(const FIntVector& ChunkCoord) const override { return Chunks.Contains(ChunkCoord); }
		virtual int32 GetLoadedChunkCount() const override { return Chunks.Num(); }
		virtual void GetLoadedChunks(TArray<FIntVector>& OutChunks) const override { OutChunks = Chunks.Array(); }
		virtual int64 GetGPUMemoryUsage() const override { return 0; }
		virtual int64 GetTotalVertexCount() const override { return 0; }
		virtual int64 GetTotalTriangleCount() const override { return 0; }
		virtual bool GetChunkBounds(const FIntVector& ChunkCoord, FBox& OutBounds) const override { return false; }
		virtual FBox GetTotalBounds() const override { return FBox(ForceInit); }
		virtual FString GetDebugStats() const override { return FString(); }
		virtual FString GetRendererTypeName() const override { return TEXT("Water recording (test)"); }

		/** Drop the water tiles and chunks, as a recreated terrain proxy does (G15), and report it once. */
		void SimulateProxyRecreated()
		{
			Lost = Chunks.Array();
			Chunks.Reset();
			WaterTiles.Reset();
			bLost = true;
		}
		virtual bool ConsumeLostRenderData(TArray<FIntVector>& OutChunks) override
		{
			OutChunks.Reset();
			if (!bLost) { return false; }
			bLost = false;
			OutChunks = MoveTemp(Lost);
			return true;
		}

	private:
		bool bInitialized = false;
		bool bLost = false;
		TArray<FIntVector> Lost;
	};

	/** The far ocean's grid water (frame and window) as covered columns (up-facing quads, column-aligned). */
	TSet<FIntPoint> CoveredColumns(const TArray<UProceduralMeshComponent*>& Grids, const FVoxelFarOceanSettings& S, int32& OutOverlaps)
	{
		TSet<FIntPoint> Covered;
		OutOverlaps = 0;
		const FVector Origin = S.GetMeshOrigin();
		for (UProceduralMeshComponent* Grid : Grids)
		{
			const FProcMeshSection* Section = Grid ? Grid->GetProcMeshSection(0) : nullptr;
			const int32 NumVertices = Section ? Section->ProcVertexBuffer.Num() : 0;
			// 4 vertices per quad (both sides share them); corners 0 and 2 are the minimum and the maximum.
			for (int32 Base = 0; Base + 3 < NumVertices; Base += 4)
			{
				const FVector Lo = Section->ProcVertexBuffer[Base].Position + Origin;
				const FVector Hi = Section->ProcVertexBuffer[Base + 2].Position + Origin;
				const int32 X0 = FMath::RoundToInt32((Lo.X - S.GridOrigin.X) / S.ColumnSize);
				const int32 Y0 = FMath::RoundToInt32((Lo.Y - S.GridOrigin.Y) / S.ColumnSize);
				const int32 X1 = FMath::RoundToInt32((Hi.X - S.GridOrigin.X) / S.ColumnSize);
				const int32 Y1 = FMath::RoundToInt32((Hi.Y - S.GridOrigin.Y) / S.ColumnSize);
				for (int32 X = X0; X < X1; ++X)
				{
					for (int32 Y = Y0; Y < Y1; ++Y)
					{
						bool bAlready = false;
						Covered.Add(FIntPoint(X, Y), &bAlready);
						OutOverlaps += bAlready ? 1 : 0;
					}
				}
			}
		}
		return Covered;
	}

	/** Vertices of a procedural mesh section, or 0. */
	int32 SectionVertices(UProceduralMeshComponent* Component, int32 Section)
	{
		const FProcMeshSection* S = Component ? Component->GetProcMeshSection(Section) : nullptr;
		return S ? S->ProcVertexBuffer.Num() : 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFarOceanStreamingTest,
	"VoxelWorlds.Streaming.Island.FarOcean",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelFarOceanStreamingTest::RunTest(const FString& Parameters)
{
	using namespace VoxelFarOceanTest;

	// The far ocean's grid water as it is without the far field (with it, WM-MP1 / D37, the grid water and floor are not
	// built: the far field draws the terrain and the sea inside the bounds; VoxelWorlds.FarField.Island.* cover that).
	IConsoleVariable* FarFieldVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.World.FarField"));
	const FString FarFieldBefore = FarFieldVar ? FarFieldVar->GetString() : FString();
	if (FarFieldVar)
	{
		FarFieldVar->Set(TEXT("0"), ECVF_SetByCode);
	}
	ON_SCOPE_EXIT
	{
		if (FarFieldVar)
		{
			FarFieldVar->Set(*FarFieldBefore, ECVF_SetByCode);
		}
	};

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelFarOceanTestWorld"));
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(World);

	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
	Config->AddToRoot();
	Config->WorldMode = EWorldMode::IslandBowl;
	Config->MeshingMode = EMeshingMode::Cubic; // LOD off (D7): quick to settle
	Config->bUseGreedyMeshing = true;
	Config->ChunkSize = 16;
	Config->VoxelSize = 100.0f;
	Config->ViewDistance = 4000.0f;
	Config->IslandRadius = 3000.0f;
	Config->IslandFalloffWidth = 2000.0f;
	Config->IslandEdgeHeight = -1200.0f;
	Config->IslandSeabedDepth = 1500.0f;
	Config->IslandSeabedSlopeWidth = 2000.0f;
	Config->IslandFarOceanDistance = 50000.0f;
	Config->HeightScale = 1500.0f;
	Config->bEnableWaterLevel = true;
	Config->WaterLevel = -300.0f;
	Config->WaterMeshMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
	Config->bUseGPUGeneration = false;
	Config->bUseGPUMeshing = false;
	Config->bGenerateCollision = false;
	Config->bEnableScatter = false;
	Config->bEnableCaves = false;
	Config->bWorldBoundary = false;
	Config->NoiseParams.Seed = 777;
	Config->NoiseParams.Frequency = 0.0015f;
	Config->NoiseParams.Octaves = 3;
	for (FLODBand& Band : Config->LODBands)
	{
		Band.MaxDistance = FMath::Min(Band.MaxDistance, Config->ViewDistance);
		Band.MinDistance = FMath::Min(Band.MinDistance, Config->ViewDistance);
	}
	Config->LODBands.RemoveAll([Config](const FLODBand& Band) { return Band.MinDistance >= Config->ViewDistance; });

	AActor* Owner = World->SpawnActor<AActor>();
	UVoxelChunkManager* Manager = NewObject<UVoxelChunkManager>(Owner);
	Manager->RegisterComponent();
	FWaterRecordingRenderer Renderer;
	Renderer.Initialize(World, Config);
	Manager->Initialize(Config, new FDistanceBandLODStrategy(), &Renderer);

	auto Settle = [Manager]()
	{
		int32 IdleTicks = 0;
		const double Deadline = FPlatformTime::Seconds() + 90.0;
		while (IdleTicks < 60 && FPlatformTime::Seconds() < Deadline)
		{
			Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			const bool bIdle = Manager->GetPendingGenerationCount() == 0 && Manager->GetPendingMeshingCount() == 0
				&& Manager->GetAsyncGenerationInProgressCount() == 0 && Manager->GetAsyncMeshingInProgressCount() == 0
				&& Manager->GetPendingMeshUploadCount() == 0 && Manager->GetSeamJobsInFlightCount() == 0;
			IdleTicks = bIdle ? IdleTicks + 1 : 0;
			FPlatformProcess::Sleep(0.002f);
		}
		return IdleTicks >= 60;
	};

	TestTrue(TEXT("The island has a far ocean"), Manager->HasFarOcean());
	const FVoxelFarOceanSettings& S = Manager->GetFarOceanSettings();
	FIntPoint Min, Max;
	FVoxelFarOcean::GetGridColumns(S, Min, Max);
	UProceduralMeshComponent* Static = Manager->GetFarOceanStaticComponent();
	TestTrue(TEXT("The seabed and the open sea are built"), Static && Static->GetNumSections() >= 2
		&& Static->GetProcMeshSection(0)->ProcVertexBuffer.Num() > 0 && Static->GetProcMeshSection(1)->ProcVertexBuffer.Num() > 0);

	// Two viewers: the island's middle, then its east coast, where the view takes in the edge and the sea past it.
	const float Edge = Config->IslandRadius + Config->IslandFalloffWidth;
	for (const FVector2D& ViewXY : { FVector2D(0.0, 0.0), FVector2D(Edge - 800.0, 600.0) })
	{
		const float SurfaceZ = Manager->GetGeneratedSurfaceHeight(ViewXY.X, ViewXY.Y);
		Manager->SetBenchmarkView(true, FVector(ViewXY.X, ViewXY.Y, FMath::Max(SurfaceZ, Config->WaterLevel) + 200.0));
		TestTrue(FString::Printf(TEXT("Viewer (%.0f, %.0f): the pipeline settled"), ViewXY.X, ViewXY.Y), Settle());

		TArray<FIntVector> Loaded;
		Manager->GetLoadedChunks(Loaded);
		TSet<FIntPoint> LoadedColumns;
		int32 OutsideFootprint = 0;
		const FVoxelWorldBounds Bounds = Manager->GetWorldBounds();
		for (const FIntVector& Coord : Loaded)
		{
			LoadedColumns.Add(FIntPoint(Coord.X, Coord.Y));
			const FVector2D Lo(Config->WorldOrigin.X + Coord.X * S.ColumnSize, Config->WorldOrigin.Y + Coord.Y * S.ColumnSize);
			OutsideFootprint += Bounds.IntersectsBox2D(FBox2D(Lo, Lo + FVector2D(S.ColumnSize))) ? 0 : 1;
		}
		TestEqual(TEXT("Only columns touching the footprint stream (the ring beyond R + F is gone)"), OutsideFootprint, 0);
		TestTrue(TEXT("Some water tiles were submitted"), Renderer.WaterTiles.Num() > 0);

		int32 Overlaps = 0;
		const TSet<FIntPoint> Covered = CoveredColumns({ Manager->GetFarOceanGridComponent(), Manager->GetFarOceanWindowComponent() }, S, Overlaps);
		int32 TilesUnderSea = 0, LoadedUnderSea = 0, Uncovered = 0;
		for (const FIntVector2& Tile : Renderer.WaterTiles)
		{
			TilesUnderSea += Covered.Contains(FIntPoint(Tile.X, Tile.Y)) ? 1 : 0;
		}
		for (const FIntPoint& Column : LoadedColumns)
		{
			LoadedUnderSea += Covered.Contains(Column) ? 1 : 0;
		}
		for (int32 X = Min.X; X <= Max.X; ++X)
		{
			for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
			{
				const FIntPoint Column(X, Y);
				const bool bOpen = LoadedColumns.Contains(Column) || Renderer.WaterTiles.Contains(FIntVector2(X, Y));
				Uncovered += (!bOpen && !Covered.Contains(Column)) ? 1 : 0;
			}
		}
		AddInfo(FString::Printf(TEXT("Viewer (%.0f, %.0f): %d chunks in %d columns, %d water tiles, %d grid columns covered, %d open (%s)"),
			ViewXY.X, ViewXY.Y, Loaded.Num(), LoadedColumns.Num(), Renderer.WaterTiles.Num(), Covered.Num(),
			Manager->GetFarOceanOpenColumnCount(), *Manager->GetFarOceanDebugString()));
		TestEqual(TEXT("No water tile lies under the far ocean's water"), TilesUnderSea, 0);
		TestEqual(TEXT("No loaded column lies under the far ocean's water"), LoadedUnderSea, 0);
		TestEqual(TEXT("Every other grid column is covered"), Uncovered, 0);
		TestEqual(TEXT("No grid column is covered twice"), Overlaps, 0);
		const FIntRect& Window = Manager->GetFarOceanWindowColumns();
		int32 OpenOutsideWindow = 0;
		for (const FIntPoint& Column : Manager->GetFarOceanOpenColumns())
		{
			OpenOutsideWindow += Window.Contains(Column) ? 0 : 1;
		}
		TestEqual(TEXT("The window holds every open column"), OpenOutsideWindow, 0);
	}

	// A recreated terrain proxy (G15) loses the water tiles and chunk meshes until the manager re-sends them; the far
	// ocean's meshes live on their own components and are untouched, and recreating their render state keeps them.
	UProceduralMeshComponent* Grid = Manager->GetFarOceanGridComponent();
	UProceduralMeshComponent* Window = Manager->GetFarOceanWindowComponent();
	const int32 GridVertices = SectionVertices(Grid, 0) + SectionVertices(Window, 0);
	const int32 SeabedVertices = SectionVertices(Static, 0);
	Renderer.SimulateProxyRecreated();
	for (UProceduralMeshComponent* Component : { Grid, Window, Static })
	{
		if (Component)
		{
			Component->RecreateRenderState_Concurrent();
		}
	}
	TestTrue(TEXT("After the proxy recreation the manager re-sent everything"), Settle());
	TestTrue(TEXT("The water tiles came back"), Renderer.WaterTiles.Num() > 0);
	TestEqual(TEXT("The far ocean's grid water is unchanged"), SectionVertices(Grid, 0) + SectionVertices(Window, 0), GridVertices);
	TestEqual(TEXT("The seabed is unchanged"), SectionVertices(Static, 0), SeabedVertices);
	int32 Overlaps = 0;
	const TSet<FIntPoint> Covered = CoveredColumns({ Grid, Window }, S, Overlaps);
	int32 TilesUnderSea = 0;
	for (const FIntVector2& Tile : Renderer.WaterTiles)
	{
		TilesUnderSea += Covered.Contains(FIntPoint(Tile.X, Tile.Y)) ? 1 : 0;
	}
	TestEqual(TEXT("After the recovery no water tile lies under the far ocean"), TilesUnderSea, 0);

	Manager->SetBenchmarkView(false, FVector::ZeroVector);
	Manager->Shutdown();
	TestFalse(TEXT("Shutdown removes the far ocean"), Manager->HasFarOcean());
	Owner->Destroy();
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	Config->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
