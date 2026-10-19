// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "Materials/Material.h"
#include "ProceduralMeshComponent.h"
#include "VoxelChunkManager.h"
#include "VoxelCollisionManager.h"
#include "VoxelPlanetOcean.h"
#include "VoxelSeaSurface.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "DistanceBandLODStrategy.h"
#include "IVoxelMeshRenderer.h"
#include "ChunkRenderData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The VoxelWorlds side of a walkable planet (WORLD_MODES_REVIEW_PLAN.md §17; SPHERICAL_PLANET_REQUIREMENTS.md SP-E2,
// SP-B11, SP-E1, SP-F5, SP-C4):
// - The ocean shell's geometry (on the sphere within its sag budget, outward top and inward underside, no cracks at the
//   cube's edges) and the chunk manager's component (built on a planet with water, its underside shown only from inside).
// - The planet spawn position over the terrain, the sea queries, KillZ under the planet.
// - The collision lead's samples: the heightmap's points exactly with up +Z, along local down and the tangent on a planet.
// ---------------------------------------------------------------------------

namespace VoxelPlanetWalkableTestUtils
{
	/** A renderer that draws nothing (the chunk manager needs one to initialise). */
	class FNullMeshRenderer : public IVoxelMeshRenderer
	{
	public:
		virtual void Initialize(UWorld* World, const UVoxelWorldConfiguration* WorldConfig) override { bInitialized = true; }
		virtual void Shutdown() override { bInitialized = false; }
		virtual bool IsInitialized() const override { return bInitialized; }
		virtual void UpdateChunkMesh(const FChunkRenderData& RenderData) override {}
		virtual void UpdateChunkMeshFromCPU(const FIntVector& ChunkCoord, int32 LODLevel, const FChunkMeshData& MeshData) override {}
		virtual void RemoveChunk(const FIntVector& ChunkCoord) override {}
		virtual void ClearAllChunks() override {}
		virtual void SetChunkVisible(const FIntVector& ChunkCoord, bool bVisible) override {}
		virtual void SetAllChunksVisible(bool bVisible) override {}
		virtual void SetMaterial(UMaterialInterface* Material) override {}
		virtual UMaterialInterface* GetMaterial() const override { return nullptr; }
		virtual void UpdateMaterialParameters() override {}
		virtual void UpdateSeamMeshFromCPU(const FIntVector& OwnerChunkCoord, uint8 SeamSlot, int32 LODLevel, FChunkMeshData&& MeshData, bool bCrossfade) override {}
		virtual void RemoveSeamMesh(const FIntVector& OwnerChunkCoord, uint8 SeamSlot) override {}
		virtual void ClearAllSeamMeshes() override {}
		virtual void GetSeamMeshOwners(TArray<FIntVector>& OutOwners) const override {}
		virtual void UpdateLODTransition(const FIntVector& ChunkCoord, float MorphFactor) override {}
		virtual bool IsChunkLoaded(const FIntVector& ChunkCoord) const override { return false; }
		virtual int32 GetLoadedChunkCount() const override { return 0; }
		virtual void GetLoadedChunks(TArray<FIntVector>& OutChunks) const override {}
		virtual int64 GetGPUMemoryUsage() const override { return 0; }
		virtual int64 GetTotalVertexCount() const override { return 0; }
		virtual int64 GetTotalTriangleCount() const override { return 0; }
		virtual bool GetChunkBounds(const FIntVector& ChunkCoord, FBox& OutBounds) const override { return false; }
		virtual FBox GetTotalBounds() const override { return FBox(ForceInit); }
		virtual FString GetDebugStats() const override { return FString(); }
		virtual FString GetRendererTypeName() const override { return TEXT("Null (test)"); }

	private:
		bool bInitialized = false;
	};

	/** A game world with a chunk manager on a small planet (or a plane), streaming nothing until ticked. */
	struct FWalkableWorld
	{
		UWorld* World = nullptr;
		UVoxelWorldConfiguration* Config = nullptr;
		AActor* Owner = nullptr;
		UVoxelChunkManager* Manager = nullptr;
		FNullMeshRenderer Renderer;

		/** Make the world and a configuration; edit Config, then call Start. */
		explicit FWalkableWorld(const TCHAR* WorldName, bool bPlanet)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, WorldName);
			FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
			WorldContext.SetCurrentWorld(World);
			Config = NewObject<UVoxelWorldConfiguration>();
			Config->AddToRoot();
			Config->WorldMode = bPlanet ? EWorldMode::SphericalPlanet : EWorldMode::InfinitePlane;
			Config->WorldOrigin = FVector::ZeroVector;
			Config->WorldRadius = 20000.0f;
			Config->PlanetHeightScale = 1600.0f;
			Config->BaseHeight = 0.0f;
			Config->PlanetMaxTerrainHeight = 1000.0f;
			Config->PlanetMaxTerrainDepth = 1000.0f;
			Config->MeshingMode = EMeshingMode::DualContouring;
			Config->ChunkSize = 32;
			Config->VoxelSize = 50.0f;
			Config->ViewDistance = 3000.0f;
			Config->bUseGPUGeneration = false;
			Config->bUseGPUMeshing = false;
			Config->bGenerateCollision = false;
			Config->bEnableScatter = false;
			Config->bEnableCaves = false;
			Config->bEnableBiomes = false;
			Config->bEnableWaterLevel = true;
			Config->WaterRadius = 19800.0f;
			Config->WaterLevel = -1500.0f;
			Config->NoiseParams.Seed = 9001;
			Config->NoiseParams.Frequency = 0.0004f;
			Config->NoiseParams.Octaves = 3;
		}

		void Start()
		{
			Owner = World->SpawnActor<AActor>();
			Manager = NewObject<UVoxelChunkManager>(Owner);
			Manager->RegisterComponent();
			Renderer.Initialize(World, Config);
			Manager->Initialize(Config, new FDistanceBandLODStrategy(), &Renderer);
		}

		~FWalkableWorld()
		{
			if (Manager)
			{
				Manager->SetBenchmarkView(false, FVector::ZeroVector);
				Manager->Shutdown();
			}
			if (Owner)
			{
				Owner->Destroy();
			}
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			Config->RemoveFromRoot();
		}
	};

	/** Direction for a latitude and longitude in degrees (the planet's frame: +Z north, longitude from +X toward +Y). */
	FVector LatLon(double Lat, double Lon)
	{
		const double La = FMath::DegreesToRadians(Lat);
		const double Lo = FMath::DegreesToRadians(Lon);
		return FVector(FMath::Cos(La) * FMath::Cos(Lo), FMath::Cos(La) * FMath::Sin(Lo), FMath::Sin(La));
	}
}

/**
 * SP-E2: the ocean shell's geometry. Every vertex on the sphere, every quad's diagonal within the sag budget, normals
 * radial, top triangles facing out and the underside's facing in, and every vertex on a face's border shared by the
 * neighbouring face (the cube's edges and corners close).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetOceanShellTest,
	"VoxelWorlds.Streaming.Planet.OceanShell",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetOceanShellTest::RunTest(const FString& Parameters)
{
	// Resolution: the review planet's sea (249,500) takes the most quads; a 1 km planet fewer; the budget holds.
	const int32 Big = FVoxelPlanetOcean::ChooseSegments(249500.0, 8.0, 32, 160);
	const int32 Small = FVoxelPlanetOcean::ChooseSegments(100000.0, 8.0, 32, 160);
	AddInfo(FString::Printf(TEXT("Segments: R 249,500 -> %d (sag %.2f), R 100,000 -> %d (sag %.2f)"),
		Big, FVoxelPlanetOcean::MeasureMaxSag(249500.0, Big), Small, FVoxelPlanetOcean::MeasureMaxSag(100000.0, Small)));
	TestTrue(TEXT("The review planet's sea fits the budget within the cap"), Big <= 160 && FVoxelPlanetOcean::MeasureMaxSag(249500.0, Big) <= 8.0);
	TestTrue(TEXT("A smaller planet needs fewer quads"), Small < Big && FVoxelPlanetOcean::MeasureMaxSag(100000.0, Small) <= 8.0);
	TestTrue(TEXT("One fewer would break the budget"), FVoxelPlanetOcean::MeasureMaxSag(100000.0, Small - 1) > 8.0);
	TestEqual(TEXT("The cap holds for a large planet"), FVoxelPlanetOcean::ChooseSegments(5.0e6, 8.0, 32, 160), 160);

	FVoxelPlanetOceanSettings Settings;
	Settings.Center = FVector(1000.0, -2000.0, 500.0); // positions stay relative to it
	Settings.Radius = 20000.0;
	Settings.VoxelSize = 50.0;
	FVoxelPlanetOceanMesh Top;
	FVoxelPlanetOceanMesh Under;
	int32 Segments = 0;
	const int32 Quads = FVoxelPlanetOcean::Build(Settings, Top, &Under, Segments);
	const int32 Row = Segments + 1;
	TestEqual(TEXT("Quads: six faces of Segments^2"), Quads, 6 * Segments * Segments);
	TestEqual(TEXT("Vertices: six faces of (Segments + 1)^2"), Top.Positions.Num(), 6 * Row * Row);
	TestEqual(TEXT("Triangles: two a quad"), Top.Triangles.Num(), Quads * 6);
	TestEqual(TEXT("The underside has the same triangles"), Under.Triangles.Num(), Top.Triangles.Num());

	double WorstRadius = 0.0, WorstNormal = 0.0, WorstTangent = 0.0;
	for (int32 i = 0; i < Top.Positions.Num(); ++i)
	{
		const FVector& P = Top.Positions[i];
		WorstRadius = FMath::Max(WorstRadius, FMath::Abs(P.Size() - Settings.Radius));
		WorstNormal = FMath::Max(WorstNormal, (Top.Normals[i] - P.GetSafeNormal()).Size());
		WorstTangent = FMath::Max(WorstTangent, FMath::Abs(FVector::DotProduct(Top.Tangents[i], Top.Normals[i])));
	}
	AddInfo(FString::Printf(TEXT("R 20,000: %d segments, worst |p| - R %.4f, normal %.6f, tangent . normal %.6f"),
		Segments, WorstRadius, WorstNormal, WorstTangent));
	TestTrue(TEXT("Every vertex on the sphere"), WorstRadius < 0.01);
	TestTrue(TEXT("Normals radial"), WorstNormal < 1.0e-6);
	TestTrue(TEXT("Tangents in the surface"), WorstTangent < 1.0e-6);

	int32 TopInward = 0, UnderOutward = 0;
	double WorstSag = 0.0;
	for (int32 t = 0; t + 2 < Top.Triangles.Num(); t += 3)
	{
		const FVector A = Top.Positions[Top.Triangles[t]];
		const FVector B = Top.Positions[Top.Triangles[t + 1]];
		const FVector C = Top.Positions[Top.Triangles[t + 2]];
		const FVector Centroid = (A + B + C) / 3.0;
		// UE's front face: (B - A) x (C - A) points against the camera for its clockwise order, so its negation is
		// the facing; the far ocean and water tiles build theirs the same way (FVoxelPlanetOcean's comment).
		const FVector Facing = -FVector::CrossProduct(B - A, C - A);
		TopInward += FVector::DotProduct(Facing, Centroid) <= 0.0 ? 1 : 0;
		const FVector UA = Under.Positions[Under.Triangles[t]];
		const FVector UB = Under.Positions[Under.Triangles[t + 1]];
		const FVector UC = Under.Positions[Under.Triangles[t + 2]];
		UnderOutward += FVector::DotProduct(-FVector::CrossProduct(UB - UA, UC - UA), (UA + UB + UC) / 3.0) >= 0.0 ? 1 : 0;
	}
	for (int32 Face = 0; Face < 6; ++Face)
	{
		for (int32 J = 0; J < Segments; ++J)
		{
			for (int32 I = 0; I < Segments; ++I)
			{
				const FVector A = FVoxelPlanetOcean::GetGridDirection(Face, I, J, Segments) * Settings.Radius;
				const FVector D = FVoxelPlanetOcean::GetGridDirection(Face, I + 1, J + 1, Segments) * Settings.Radius;
				WorstSag = FMath::Max(WorstSag, Settings.Radius - ((A + D) * 0.5).Size());
			}
		}
	}
	AddInfo(FString::Printf(TEXT("Top triangles facing in: %d; underside facing out: %d; worst sag %.3f"), TopInward, UnderOutward, WorstSag));
	TestEqual(TEXT("Every top triangle faces out"), TopInward, 0);
	TestEqual(TEXT("Every underside triangle faces in"), UnderOutward, 0);
	TestTrue(TEXT("The sag stays inside the budget on every face"), WorstSag <= Settings.MaxSag + 1.0e-6);

	// The cube's edges: every border vertex of a face is at the position of a border vertex of another face.
	TMap<FIntVector, int32> FacesAt;
	auto Key = [](const FVector& P) { return FIntVector(FMath::RoundToInt32(P.X * 100.0), FMath::RoundToInt32(P.Y * 100.0), FMath::RoundToInt32(P.Z * 100.0)); };
	for (int32 Face = 0; Face < 6; ++Face)
	{
		TSet<FIntVector> Seen;
		for (int32 J = 0; J < Row; ++J)
		{
			for (int32 I = 0; I < Row; ++I)
			{
				if (I != 0 && J != 0 && I != Segments && J != Segments)
				{
					continue;
				}
				const FIntVector K = Key(Top.Positions[Face * Row * Row + I + J * Row]);
				if (!Seen.Contains(K))
				{
					Seen.Add(K);
					++FacesAt.FindOrAdd(K);
				}
			}
		}
	}
	int32 Lonely = 0;
	for (const TPair<FIntVector, int32>& Pair : FacesAt)
	{
		Lonely += Pair.Value < 2 ? 1 : 0;
	}
	AddInfo(FString::Printf(TEXT("Border positions: %d, on one face only: %d"), FacesAt.Num(), Lonely));
	TestEqual(TEXT("Every border vertex is shared with the next face (no crack at the cube's edges)"), Lonely, 0);

	// UVs: voxels of arc from each face's centre, as the water tiles count voxels.
	const double ExpectEdgeUV = UE_DOUBLE_PI / 4.0 * Settings.Radius / Settings.VoxelSize;
	TestTrue(TEXT("UVs reach pi/4 R / VoxelSize at the face's edge"), FMath::IsNearlyEqual(Top.UV0[Row - 1].X, ExpectEdgeUV, 1.0e-6));
	TestTrue(TEXT("UV1 carries the water material id"), Top.UV1[0].X == FVoxelPlanetOcean::WaterMaterialID);
	return true;
}

/**
 * SP-E2: a planet with water and a water material gets the ocean shell, its underside shown only while the viewer is
 * inside the sea radius; a plane gets none.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetOceanComponentTest,
	"VoxelWorlds.Streaming.Planet.OceanComponent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetOceanComponentTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetWalkableTestUtils;
	{
		FWalkableWorld Planet(TEXT("PlanetOceanComponentWorld"), /*bPlanet=*/true);
		Planet.Config->WaterMeshMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		Planet.Start();
		TestTrue(TEXT("A planet with water draws its ocean"), Planet.Manager->HasPlanetOcean());
		UProceduralMeshComponent* Ocean = Planet.Manager->GetPlanetOceanComponent();
		if (Ocean && Ocean->GetNumSections() == 2)
		{
			TestTrue(TEXT("The ocean sits at the planet centre"), Ocean->GetComponentLocation().Equals(Planet.Config->WorldOrigin));
			auto Tick = [&Planet](const FVector& Viewer)
			{
				Planet.Manager->SetBenchmarkView(true, Viewer);
				Planet.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			};
			Tick(LatLon(45.0, 10.0) * (Planet.Config->WaterRadius + 300.0));
			const bool bOutside = Ocean->IsMeshSectionVisible(1);
			Tick(LatLon(45.0, 10.0) * (Planet.Config->WaterRadius - 300.0));
			const bool bInside = Ocean->IsMeshSectionVisible(1);
			const bool bInsideFlag = Planet.Manager->IsPlanetOceanUndersideShown();
			Tick(LatLon(-20.0, 200.0) * (Planet.Config->WaterRadius + 50.0));
			const bool bOutAgain = Ocean->IsMeshSectionVisible(1);
			AddInfo(FString::Printf(TEXT("Underside visible: above %d, under %d (flag %d), above again %d"), bOutside ? 1 : 0, bInside ? 1 : 0,
				bInsideFlag ? 1 : 0, bOutAgain ? 1 : 0));
			TestFalse(TEXT("Above the sea the underside is hidden"), bOutside);
			TestTrue(TEXT("Under the sea it shows"), bInside && bInsideFlag);
			TestFalse(TEXT("Back above it hides again"), bOutAgain);
			TestTrue(TEXT("The top always shows"), Ocean->IsMeshSectionVisible(0));
		}
		else
		{
			AddError(TEXT("The ocean component has two sections"));
		}
	}
	{
		FWalkableWorld Dry(TEXT("PlanetOceanDryWorld"), /*bPlanet=*/true);
		Dry.Config->WaterMeshMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		Dry.Config->bEnableWaterLevel = false;
		Dry.Start();
		TestFalse(TEXT("A planet without water has no ocean"), Dry.Manager->HasPlanetOcean());
	}
	{
		FWalkableWorld Plane(TEXT("PlaneOceanComponentWorld"), /*bPlanet=*/false);
		Plane.Config->WaterMeshMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		Plane.Start();
		TestFalse(TEXT("A plane has no ocean shell (its water tiles draw the sea)"), Plane.Manager->HasPlanetOcean());
	}
	return true;
}

/**
 * SP-B11: the planet spawn position is PlanetSpawnAltitude over the terrain along the configured direction (the
 * configuration's own is over the datum); a plane returns its origin.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetSpawnPositionTest,
	"VoxelWorlds.Streaming.Planet.SpawnPosition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetSpawnPositionTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetWalkableTestUtils;
	{
		FWalkableWorld Planet(TEXT("PlanetSpawnPositionWorld"), /*bPlanet=*/true);
		Planet.Config->PlanetSpawnAltitude = 250.0f;
		Planet.Start();
		static const FVector Directions[] = { FVector(1, 0, 0), FVector(0, 1, 0), FVector(0, 0, 1), FVector(0, 0, -1) };
		double WorstAltitude = 0.0, WorstDirection = 0.0, MostTerrain = 0.0;
		for (int32 Location = 0; Location < 4; ++Location)
		{
			Planet.Config->PlanetSpawnLocation = Location;
			const FVector Spawn = Planet.Manager->GetPlanetSpawnPosition();
			const FVoxelTerrainSurfacePoint Surface = Planet.Manager->GetSurfacePointAt(Spawn);
			WorstAltitude = FMath::Max(WorstAltitude, FMath::Abs(FVector::Dist(Spawn, Surface.Position) - 250.0));
			WorstDirection = FMath::Max(WorstDirection, (Spawn.GetSafeNormal() - Directions[Location]).Size());
			MostTerrain = FMath::Max(MostTerrain, FMath::Abs(static_cast<double>(Surface.Altitude)));
			TestTrue(FString::Printf(TEXT("Spawn %d stands above the terrain"), Location),
				Spawn.Size() > Surface.Position.Size());
		}
		AddInfo(FString::Printf(TEXT("Worst altitude error %.3f, direction error %.6f, terrain up to %.0f off the datum"),
			WorstAltitude, WorstDirection, MostTerrain));
		TestTrue(TEXT("PlanetSpawnAltitude over the terrain"), WorstAltitude < 0.5);
		TestTrue(TEXT("Along the configured direction"), WorstDirection < 1.0e-6);
		TestTrue(TEXT("The terrain is off the datum (the old helper ignored it)"), MostTerrain > 10.0);
	}
	{
		FWalkableWorld Plane(TEXT("PlaneSpawnPositionWorld"), /*bPlanet=*/false);
		Plane.Config->WorldOrigin = FVector(12345.0, 54321.0, 0.0);
		Plane.Start();
		TestTrue(TEXT("A plane returns its origin"), Plane.Manager->GetPlanetSpawnPosition().Equals(Plane.Config->WorldOrigin));
	}
	return true;
}

/**
 * SP-E1: the chunk manager's sea. On a planet the sphere of WaterRadius (the depth along the radius); on a plane the
 * WaterLevel plane, shifted by WorldOrigin.Z for the origin-relative consumers; water off disables it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetSeaQueriesTest,
	"VoxelWorlds.Streaming.Planet.SeaQueries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetSeaQueriesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetWalkableTestUtils;
	{
		FWalkableWorld Planet(TEXT("PlanetSeaQueriesWorld"), /*bPlanet=*/true);
		Planet.Start();
		const FVector Dir = LatLon(-30.0, 135.0);
		TestTrue(TEXT("Planet: 300 under the sea sphere"), FMath::IsNearlyEqual(Planet.Manager->GetSeaDepthAt(Dir * (Planet.Config->WaterRadius - 300.0)), 300.0f, 0.01f));
		TestTrue(TEXT("Planet: 120 over it"), FMath::IsNearlyEqual(Planet.Manager->GetSeaDepthAt(Dir * (Planet.Config->WaterRadius + 120.0)), -120.0f, 0.01f));
		const FVoxelSeaSurface A = Planet.Manager->GetSeaSurface(false);
		const FVoxelSeaSurface B = Planet.Manager->GetSeaSurface(true);
		TestTrue(TEXT("Planet: both conventions are the sphere"), A.bRadial && B.bRadial && A.Radius == B.Radius && A.Center.Equals(B.Center));
	}
	{
		FWalkableWorld Plane(TEXT("PlaneSeaQueriesWorld"), /*bPlanet=*/false);
		Plane.Config->WorldOrigin = FVector(100.0, 200.0, 300.0);
		Plane.Start();
		TestEqual(TEXT("Plane: generation's plane is WaterLevel"), Plane.Manager->GetSeaSurface(false).PlaneZ, -1500.0);
		TestEqual(TEXT("Plane: game code's plane adds WorldOrigin.Z"), Plane.Manager->GetSeaSurface(true).PlaneZ, -1200.0);
		TestTrue(TEXT("Plane: GetSeaDepthAt reads the game plane"), FMath::IsNearlyEqual(Plane.Manager->GetSeaDepthAt(FVector(5.0e5, -3.0e5, -1250.0)), 50.0f, 0.001f));
	}
	{
		FWalkableWorld Dry(TEXT("PlaneSeaQueriesDryWorld"), /*bPlanet=*/false);
		Dry.Config->bEnableWaterLevel = false;
		Dry.Start();
		TestFalse(TEXT("Water off: the sea is disabled"), Dry.Manager->GetSeaSurface(true).bEnabled);
		TestTrue(TEXT("Water off: never under it"), Dry.Manager->GetSeaDepthAt(FVector(0.0, 0.0, -1.0e6)) < -1.0e8f);
	}
	return true;
}

/**
 * SP-F5: KillZ under the planet. A planet the default KillZ already clears keeps it; a larger one moves it below the
 * lowest point of its world (the orbit ceiling since spherical Phase 6; the terrain's lowest point for bounds that carry
 * no ceiling); other shapes never change it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetKillZTest,
	"VoxelWorlds.Streaming.Planet.KillZ",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetKillZTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetWalkableTestUtils;
	FVoxelWorldBounds Shell;
	Shell.Shape = EVoxelWorldBoundsShape::Shell;
	Shell.Center = FVector(0.0, 0.0, 1000.0);
	Shell.OuterRadius = 256500.0f;
	TestEqual(TEXT("A 250 km planet clears the default KillZ: unchanged"), UVoxelChunkManager::ComputePlanetKillZ(-1048575.0, Shell, 4800.0), -1048575.0);
	Shell.OuterRadius = 2000000.0f;
	TestEqual(TEXT("A 2,000 km planet: below its lowest point"), UVoxelChunkManager::ComputePlanetKillZ(-1048575.0, Shell, 4800.0),
		1000.0 - 2000000.0 - 4800.0);
	FVoxelWorldBounds Circle;
	Circle.Shape = EVoxelWorldBoundsShape::Circle;
	Circle.OuterRadius = 2000000.0f;
	TestEqual(TEXT("Not a shell: unchanged"), UVoxelChunkManager::ComputePlanetKillZ(-1048575.0, Circle, 4800.0), -1048575.0);

	{
		FWalkableWorld Big(TEXT("PlanetKillZWorld"), /*bPlanet=*/true);
		Big.Config->WorldRadius = 2000000.0f;
		Big.Config->WaterRadius = 1999000.0f;
		AWorldSettings* Settings = Big.World->GetWorldSettings();
		Settings->KillZ = -1048575.0f;
		Big.Start();
		// The default orbit ceiling, 3 R above the datum (spherical Phase 6).
		const double Expected = -(2000000.0 + 3.0 * 2000000.0) - Big.Config->GetChunkWorldSize();
		AddInfo(FString::Printf(TEXT("Planet R 2,000,000: KillZ %.0f (expected %.0f)"), Settings->KillZ, Expected));
		TestTrue(TEXT("A big planet's world gets KillZ under it"), FMath::IsNearlyEqual(static_cast<double>(Settings->KillZ), Expected, 1.0));
	}
	{
		FWalkableWorld Plane(TEXT("PlaneKillZWorld"), /*bPlanet=*/false);
		AWorldSettings* Settings = Plane.World->GetWorldSettings();
		Settings->KillZ = -1048575.0f;
		Plane.Start();
		TestEqual(TEXT("A plane's KillZ is not touched"), Settings->KillZ, -1048575.0f);
	}
	return true;
}

/**
 * Spherical Phase 6, WM-BP2 (D18/D20): KillZ is safe in space. On any planet KillZ lies a chunk below the centre minus
 * the orbit radius (the datum plus the orbit ceiling, 3 R by default or PlanetOrbitCeiling), so a pawn held anywhere
 * around the planet up to the ceiling, the south pole included, stays above it; the world bounds carry the ceiling as
 * OrbitRadius (the outer out-of-world bound the character checks).
 *
 * Pure: the world mode the factory builds and ComputePlanetKillZ, up to the 1,000 km test planet. Streaming.Planet.KillZ
 * checks that the chunk manager applies it to a real world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetKillZSpaceTest,
	"VoxelWorlds.Streaming.Planet.KillZSpace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetKillZSpaceTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetWalkableTestUtils;
	struct FCase { double Radius; double Ceiling; };
	auto CheckCase = [this](const FCase& Case, const FVoxelWorldBounds& Bounds, double KillZ, double ChunkWorldSize)
	{
		const double Ceiling = Case.Ceiling > 0.0 ? Case.Ceiling : 3.0 * Case.Radius;
		AddInfo(FString::Printf(TEXT("R %.0f, ceiling %.0f: orbit radius %.0f, KillZ %.0f"), Case.Radius, Ceiling, Bounds.OrbitRadius, KillZ));
		TestTrue(TEXT("The bounds carry the orbit radius (datum + ceiling)"), FMath::IsNearlyEqual(Bounds.OrbitRadius, Case.Radius + Ceiling, 1.0));
		const double Lowest = Bounds.Center.Z - Bounds.OrbitRadius;
		TestTrue(TEXT("KillZ lies under the planet's whole world"), KillZ <= Lowest - ChunkWorldSize + 1.0);
		TestTrue(TEXT("KillZ never rises above the engine default"), KillZ <= -1048575.0);

		// Held over the south pole at 2 R (or the ceiling, if lower) and just under the ceiling: above KillZ, inside the
		// orbit radius.
		const double Altitudes[] = { FMath::Min(2.0 * Case.Radius, Ceiling - 1.0), Ceiling - 1.0 };
		for (const double Altitude : Altitudes)
		{
			const FVector South = Bounds.Center - FVector::UpVector * (Case.Radius + Altitude);
			TestTrue(*FString::Printf(TEXT("%.0f over the south pole is above KillZ"), Altitude), South.Z > KillZ);
			TestTrue(*FString::Printf(TEXT("%.0f over the south pole is inside the orbit radius"), Altitude),
				FVector::Dist(South, Bounds.Center) <= Bounds.OrbitRadius);
		}
	};

	// The demo-sized planet, the 2,000 km planet of Streaming.Planet.KillZ, the Phase 6 test planets M and L, and a set
	// ceiling: the factory's world mode and the pure KillZ.
	const FCase Pure[] = { { 250000.0, 0.0 }, { 2.0e6, 0.0 }, { 5.0e6, 0.0 }, { 1.0e8, 0.0 }, { 5.0e6, 2.0e6 } };
	for (const FCase& Case : Pure)
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->WorldMode = EWorldMode::SphericalPlanet;
		Config->WorldOrigin = FVector(0.0, 0.0, 700.0);
		Config->WorldRadius = static_cast<float>(Case.Radius);
		Config->PlanetOrbitCeiling = static_cast<float>(Case.Ceiling);
		const FVoxelWorldBounds Bounds = FVoxelWorldModeFactory::Create(*Config)->GetWorldBounds();
		CheckCase(Case, Bounds, UVoxelChunkManager::ComputePlanetKillZ(-1048575.0, Bounds, Config->GetChunkWorldSize()),
			Config->GetChunkWorldSize());
	}
	return true;
}

/**
 * SP-C4: the collision lead's samples. With up +Z they are the heightmap's points bit for bit (the old expressions are
 * repeated here); at 45 degrees on a planet the sample under the feet lies along the radius and the look-ahead across it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetCollisionLeadTest,
	"VoxelWorlds.Streaming.Planet.CollisionLead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetCollisionLeadTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetWalkableTestUtils;
	constexpr float W = 4800.0f;
	constexpr int32 LookAhead = 3;

	// The heightmap points as ForcePathCoverage computed them before (world -Z, the horizontal velocity).
	auto OldSamples = [](const FVector& Focus, const FVector& Velocity, float ChunkWorldSize, int32 Ahead)
	{
		TArray<FVoxelCollisionCoverSample> Out;
		Out.Add({ Focus, 1000.f });
		Out.Add({ Focus - FVector(0.f, 0.f, ChunkWorldSize * 0.5f), 950.f });
		FVector HeadingH(Velocity.X, Velocity.Y, 0.f);
		if (Ahead <= 0 || HeadingH.IsNearlyZero())
		{
			return Out;
		}
		HeadingH = HeadingH.GetSafeNormal();
		const bool bDescending = Velocity.Z < -1.0f;
		for (int32 i = 1; i <= Ahead; ++i)
		{
			const FVector P = Focus + HeadingH * (ChunkWorldSize * i);
			Out.Add({ P, 800.f - i });
			if (bDescending)
			{
				Out.Add({ P - FVector(0.f, 0.f, ChunkWorldSize), 780.f - i });
			}
		}
		return Out;
	};

	FRandomStream Random(1234);
	int32 Cases = 0, Mismatches = 0;
	TArray<FVector> Velocities = { FVector::ZeroVector, FVector(0.0, 0.0, -500.0), FVector(-0.0, 600.0, -0.0), FVector(300.0, -200.0, -2.0),
		FVector(300.0, -200.0, -0.5), FVector(-1500.0, 10.0, 900.0) };
	for (int32 i = 0; i < 200; ++i)
	{
		Velocities.Add(FVector(Random.FRandRange(-3000.0, 3000.0), Random.FRandRange(-3000.0, 3000.0), Random.FRandRange(-1500.0, 1500.0)));
	}
	for (const FVector& Velocity : Velocities)
	{
		const FVector Focus(Random.FRandRange(-1.0e6, 1.0e6), Random.FRandRange(-1.0e6, 1.0e6), Random.FRandRange(-5000.0, 9000.0));
		TArray<FVoxelCollisionCoverSample> New;
		UVoxelCollisionManager::ComputePathCoverageSamples(Focus, Velocity, FVector::UpVector, W, LookAhead, New);
		const TArray<FVoxelCollisionCoverSample> Old = OldSamples(Focus, Velocity, W, LookAhead);
		++Cases;
		bool bSame = New.Num() == Old.Num();
		for (int32 s = 0; bSame && s < New.Num(); ++s)
		{
			bSame = New[s].Position == Old[s].Position && New[s].Bias == Old[s].Bias;
		}
		Mismatches += bSame ? 0 : 1;
	}
	AddInfo(FString::Printf(TEXT("Heightmap: %d velocities, %d differ from the old points"), Cases, Mismatches));
	TestEqual(TEXT("With up +Z the samples are the heightmap's, bit for bit"), Mismatches, 0);

	// A pawn at 45 degrees walking north-east and downhill.
	const FVector Up = LatLon(45.0, 30.0);
	const FVector Focus = Up * 250100.0;
	const FVector East = FVector::CrossProduct(FVector::UpVector, Up).GetSafeNormal();
	const FVector North = FVector::CrossProduct(Up, East);
	const FVector Velocity = (North + East).GetSafeNormal() * 600.0 - Up * 200.0;
	TArray<FVoxelCollisionCoverSample> Planet;
	UVoxelCollisionManager::ComputePathCoverageSamples(Focus, Velocity, Up, W, LookAhead, Planet);
	TestEqual(TEXT("Two under the feet, then a point and the one below it per chunk ahead"), Planet.Num(), 2 + 2 * LookAhead);
	if (Planet.Num() == 2 + 2 * LookAhead)
	{
		TestTrue(TEXT("Under the feet: half a chunk along the radius"), Planet[1].Position.Equals(Focus - Up * (0.5 * W), 1.0e-6));
		const FVector Ahead1 = Planet[2].Position - Focus;
		TestTrue(TEXT("Ahead: one chunk across the radius"), FMath::IsNearlyEqual(Ahead1.Size(), static_cast<double>(W), 1.0e-6)
			&& FMath::Abs(FVector::DotProduct(Ahead1.GetSafeNormal(), Up)) < 1.0e-9);
		TestTrue(TEXT("Ahead: along the walk"), FVector::DotProduct(Ahead1.GetSafeNormal(), (North + East).GetSafeNormal()) > 0.999999);
		TestTrue(TEXT("Descending: the chunk below along the radius"), Planet[3].Position.Equals(Planet[2].Position - Up * W, 1.0e-6));
		// The old points here: a 'below' sample beside the pawn and a heading tilted by the walk's world-Z part.
		const TArray<FVoxelCollisionCoverSample> Old = OldSamples(Focus, Velocity, W, LookAhead);
		const double OldBelowAlongUp = FVector::DotProduct(Focus - Old[1].Position, Up) / (0.5 * W);
		AddInfo(FString::Printf(TEXT("45 degrees: the old under-the-feet sample went %.2f of its distance along local down"), OldBelowAlongUp));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
