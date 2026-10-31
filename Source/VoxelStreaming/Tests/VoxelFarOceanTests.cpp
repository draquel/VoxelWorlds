// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
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
// Geometry (pure, headless): the open sea covers the frame from the grid's rectangle out to the outer rectangle exactly
// once and nothing inside the grid; every water quad has its underside; the seabed lies outside the edge, under the
// seabed profile by the offset, and reaches the open sea's corners. (The grid water and floor inside the box, which
// stood in for unloaded columns while the far field was off, went in code review CR1: D-CR13.)
//
// Streaming (a real chunk manager on a small island, headless): the columns that stream are those that touch the
// footprint; the far ocean's mesh is its own component, so a recreated terrain proxy (G15) leaves it as it is.
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

		FChunkMeshData Sea, Seabed, SeaUnder;
		const int32 SeaQuads = FVoxelFarOcean::BuildOpenSeaWater(S, Sea, &SeaUnder);
		const int32 SeabedTris = FVoxelFarOcean::BuildSeabed(S, Seabed);
		AddInfo(FString::Printf(TEXT("%s: box %dx%d columns, %d open-sea quads, %d seabed triangles"),
			Name, Max.X - Min.X + 1, Max.Y - Min.Y + 1, SeaQuads, SeabedTris));

		// The water faces up; the underside is a separate mesh of the same quads facing down, with the up normal.
		TArray<FTri2D> SeaUp, SeaDown, Ignored;
		CollectTriangles(Sea, Origin, true, SeaUp);
		CollectTriangles(SeaUnder, Origin, false, SeaDown);
		CollectTriangles(Sea, Origin, false, Ignored);
		CollectTriangles(SeaUnder, Origin, true, Ignored);
		TestEqual(FString::Printf(TEXT("%s: the water faces up only, the underside down only"), Name), Ignored.Num(), 0);
		TestEqual(FString::Printf(TEXT("%s: the open sea's underside matches it"), Name), SeaDown.Num(), SeaUp.Num());
		bool bUpNormals = true;
		for (const FVector3f& N : SeaUnder.Normals) { bUpNormals &= N.Z > 0.99f; }
		TestTrue(FString::Printf(TEXT("%s: the underside shades with the up normal, as the water tiles do from below"), Name), bUpNormals);
		TestEqual(FString::Printf(TEXT("%s: two up triangles per open-sea quad"), Name), SeaUp.Num(), SeaQuads * 2);
		bool bFlat = true;
		for (const FVector3f& P : Sea.Positions) { bFlat &= FMath::IsNearlyEqual(double(P.Z) + Origin.Z, S.WaterZ, 0.01); }
		TestTrue(FString::Printf(TEXT("%s: the water lies at the water level"), Name), bFlat);

		// Inside the box (the far field's) the open sea draws nothing: points inside every column, near its corners and its
		// middle. The offsets are uneven so no point lies on a quad's diagonal (two triangles would both count it).
		int32 SeaInGrid = 0;
		for (int32 X = Min.X; X <= Max.X; ++X)
		{
			for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
			{
				const double X0 = S.GridOrigin.X + X * S.ColumnSize;
				const double Y0 = S.GridOrigin.Y + Y * S.ColumnSize;
				for (const FVector2D& P : { FVector2D(X0 + 0.371 * S.ColumnSize, Y0 + 0.613 * S.ColumnSize), FVector2D(X0 + 3.1, Y0 + 7.3),
					FVector2D(X0 + S.ColumnSize - 5.3, Y0 + S.ColumnSize - 11.7), FVector2D(X0 + 9.7, Y0 + S.ColumnSize - 4.1) })
				{
					SeaInGrid += CountCover(SeaUp, P);
				}
			}
		}
		TestEqual(FString::Printf(TEXT("%s: the open sea stays out of the box"), Name), SeaInGrid, 0);

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
		virtual void UpdateChunkMeshFromCPU(const FIntVector& ChunkCoord, int32 LODLevel, const FChunkMeshData& MeshData) override { Chunks.Add(ChunkCoord); }
		virtual void RemoveChunk(const FIntVector& ChunkCoord) override { Chunks.Remove(ChunkCoord); }
		virtual void ClearAllChunks() override { Chunks.Reset(); }
		virtual void SetMaterial(UMaterialInterface* Material) override {}
		virtual UMaterialInterface* GetMaterial() const override { return nullptr; }
		virtual void UpdateMaterialParameters() override {}
		virtual void UpdateWaterTileMesh(const FIntVector2& TileCoord, const FChunkMeshData& WaterMeshData) override { WaterTiles.Add(TileCoord); }
		virtual void RemoveWaterTile(const FIntVector2& TileCoord) override { WaterTiles.Remove(TileCoord); }
		virtual void ClearAllWaterTiles() override { WaterTiles.Reset(); }
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

	}

	// A recreated terrain proxy (G15) loses the water tiles and chunk meshes until the manager re-sends them; the far
	// ocean's mesh lives on its own component and is untouched, and recreating its render state keeps it.
	const int32 SeabedVertices = SectionVertices(Static, 0);
	const int32 SeaVertices = SectionVertices(Static, 1);
	Renderer.SimulateProxyRecreated();
	if (Static)
	{
		Static->RecreateRenderState_Concurrent();
	}
	TestTrue(TEXT("After the proxy recreation the manager re-sent everything"), Settle());
	TestTrue(TEXT("The water tiles came back"), Renderer.WaterTiles.Num() > 0);
	TestEqual(TEXT("The seabed is unchanged"), SectionVertices(Static, 0), SeabedVertices);
	TestEqual(TEXT("The open sea is unchanged"), SectionVertices(Static, 1), SeaVertices);

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
