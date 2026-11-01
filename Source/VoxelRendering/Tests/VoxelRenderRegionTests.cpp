// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "VoxelRenderRegions.h"
#include "VoxelMeshRenderer.h"
#include "VoxelRegionComponent.h"
#include "VoxelWorldConfiguration.h"
#include "ChunkRenderData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Spherical Phase 6, WM-BP7 (WORLD_MODES_REVIEW_PLAN.md §20.1, D23): render regions. On Review_Planet_L (R 1e8) a vertex
// stored as an absolute FVector3f moves by up to 8 uu; with the terrain split into regions of voxel.Render.RegionChunks^3
// chunks, each region's primitive at its origin in double and the vertices in float relative to it (the scene proxy adds
// the chunk's offset from the region origin to the chunk-local vertex), a vertex's world position is within 0.1 uu of the
// double reference at the pole, 45 degrees, the equator and a cube corner. The renderer keeps one world component per
// region holding content, at the region's origin, and retires it when it empties; heightmap worlds keep one at the origin.
// ---------------------------------------------------------------------------

namespace VoxelRenderRegionTest
{
	UVoxelWorldConfiguration* MakeConfig(EWorldMode Mode, double Radius)
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = Mode;
		Config->WorldOrigin = FVector(12345.0, 54321.0, 0.0);
		Config->WorldRadius = static_cast<float>(Radius);
		Config->VoxelSize = 75.0f;
		Config->ChunkSize = 64;
		Config->MeshingMode = EMeshingMode::DualContouring;
		return Config;
	}

	FVector LatLon(double Lat, double Lon)
	{
		const double La = FMath::DegreesToRadians(Lat);
		const double Lo = FMath::DegreesToRadians(Lon);
		return FVector(FMath::Cos(La) * FMath::Cos(Lo), FMath::Cos(La) * FMath::Sin(Lo), FMath::Sin(La));
	}

	FIntVector ChunkAt(const UVoxelWorldConfiguration& Config, const FVector& WorldPos)
	{
		const FVector Local = (WorldPos - Config.WorldOrigin) / Config.GetChunkWorldSize();
		return FIntVector(FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), FMath::FloorToInt32(Local.Z));
	}

	/** A small triangle mesh in chunk-local space (geometry is irrelevant; the renderer only routes it). */
	FChunkMeshData MakeMesh()
	{
		FChunkMeshData Mesh;
		Mesh.Positions = { FVector3f(10, 10, 10), FVector3f(100, 10, 10), FVector3f(10, 100, 10) };
		Mesh.Normals = { FVector3f::UpVector, FVector3f::UpVector, FVector3f::UpVector };
		Mesh.UVs = { FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector };
		Mesh.Indices = { 0, 1, 2 };
		return Mesh;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelRenderRegionPrecisionTest,
	"VoxelWorlds.Rendering.Planet.RegionPrecision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelRenderRegionPrecisionTest::RunTest(const FString& Parameters)
{
	using namespace VoxelRenderRegionTest;

	// ---- The vertex math at L's scale ----
	UVoxelWorldConfiguration* Config = MakeConfig(EWorldMode::SphericalPlanet, 1.0e8);
	const FVoxelRenderRegions Regions = FVoxelRenderRegions::FromConfig(*Config);
	TestTrue(TEXT("A planet splits into render regions"), Regions.IsSplit());
	AddInfo(FString::Printf(TEXT("Regions of %d^3 chunks (%.0f uu)"), Regions.RegionChunks, Regions.RegionChunks * Regions.ChunkWorldSize));

	const TPair<const TCHAR*, FVector> Sites[] = {
		{ TEXT("north pole"), FVector(0, 0, 1) },
		{ TEXT("latitude 45"), LatLon(45.0, 20.0) },
		{ TEXT("equator"), LatLon(0.0, 100.0) },
		{ TEXT("cube corner"), FVector(1, 1, 1).GetSafeNormal() },
	};
	FRandomStream Random(0xD23);
	const double ChunkWorldSize = Config->GetChunkWorldSize();
	for (const TPair<const TCHAR*, FVector>& Site : Sites)
	{
		const FIntVector Chunk = ChunkAt(*Config, Config->WorldOrigin + Site.Value * Config->WorldRadius);
		const FVector ChunkWorldPos = Config->WorldOrigin + FVector(Chunk) * ChunkWorldSize;
		const FIntVector Region = Regions.GetRegion(Chunk);
		const FVector RegionOrigin = Regions.GetRegionOrigin(Region);
		double MaxRegionError = 0.0;
		double MaxAbsoluteError = 0.0;
		for (int32 i = 0; i < 2000; ++i)
		{
			// A vertex anywhere in the chunk, or a seam vertex up to a chunk past it
			const FVector3f Local(Random.FRandRange(0.0, 2.0 * ChunkWorldSize), Random.FRandRange(0.0, 2.0 * ChunkWorldSize), Random.FRandRange(0.0, 2.0 * ChunkWorldSize));
			const FVector Reference = ChunkWorldPos + FVector(Local);
			// What the proxy stores now: the chunk's offset from its region's origin + the local vertex, in float
			const FVector3f Stored = Regions.ToRegionLocal(ChunkWorldPos, Region) + Local;
			MaxRegionError = FMath::Max(MaxRegionError, FVector::Dist(RegionOrigin + FVector(Stored), Reference));
			// What it stored before (one primitive at the origin): the absolute chunk offset + the local vertex, in float
			const FVector3f Absolute = FVector3f(ChunkWorldPos) + Local;
			MaxAbsoluteError = FMath::Max(MaxAbsoluteError, FVector::Dist(FVector(Absolute), Reference));
		}
		AddInfo(FString::Printf(TEXT("%s: chunk %s in region %s; a vertex's world position off by up to %.4f uu (absolute float: %.2f uu)"),
			Site.Key, *Chunk.ToString(), *Region.ToString(), MaxRegionError, MaxAbsoluteError));
		TestTrue(*FString::Printf(TEXT("%s: within 0.1 uu of the double reference (%.4f)"), Site.Key, MaxRegionError), MaxRegionError <= 0.1);
	}

	// Regions tile the grid: neighbouring chunks across a region boundary land in neighbouring regions, origins a region apart
	{
		const FIntVector A(Regions.RegionChunks - 1, 0, -1);
		const FIntVector B(Regions.RegionChunks, 0, 0);
		TestEqual(TEXT("Chunk 15 is in region 0, chunk -1 in region -1"), Regions.GetRegion(A), FIntVector(0, 0, -1));
		TestEqual(TEXT("Chunk 16 is in region 1"), Regions.GetRegion(B), FIntVector(1, 0, 0));
		TestEqual(TEXT("A region's origin is its first chunk's"), Regions.GetRegionOrigin(FIntVector(1, 0, 0)),
			Config->WorldOrigin + FVector(Regions.RegionChunks * ChunkWorldSize, 0.0, 0.0));
	}

	// ---- The renderer: one component per region with content, at its origin ----
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelRenderRegionTestWorld"));
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(World);
	{
		FVoxelMeshRenderer Renderer;
		Renderer.SetMaterial(UMaterial::GetDefaultMaterial(MD_Surface));
		Renderer.Initialize(World, Config);
		const FIntVector Pole = ChunkAt(*Config, Config->WorldOrigin + FVector(0, 0, 1) * Config->WorldRadius);
		const FIntVector PoleNeighbour = Pole + FIntVector(1, 0, 0);
		const FIntVector Equator = ChunkAt(*Config, Config->WorldOrigin + FVector(1, 0, 0) * Config->WorldRadius);
		for (const FIntVector& Chunk : { Pole, PoleNeighbour, Equator })
		{
			Renderer.UpdateChunkMeshFromCPU(Chunk, 0, MakeMesh());
		}
		FChunkMeshData Seam = MakeMesh();
		Renderer.UpdateSeamMeshFromCPU(Equator, 0, 0, MoveTemp(Seam));
		Renderer.FlushPendingOperations();

		TArray<TPair<FIntVector, UVoxelRegionComponent*>> Components;
		Renderer.GetRegionComponents(Components);
		TSet<FIntVector> ExpectedRegions = { FIntVector::ZeroValue, Regions.GetRegion(Pole), Regions.GetRegion(PoleNeighbour), Regions.GetRegion(Equator) };
		TestEqual(TEXT("A component per region with content, plus the primary one"), Components.Num(), ExpectedRegions.Num());
		int32 Misplaced = 0;
		for (const TPair<FIntVector, UVoxelRegionComponent*>& Pair : Components)
		{
			Misplaced += (Pair.Value && Pair.Value->GetComponentLocation().Equals(Regions.GetRegionOrigin(Pair.Key), 1.0e-6)) ? 0 : 1;
		}
		TestEqual(TEXT("Every component sits at its region's origin"), Misplaced, 0);
		TestEqual(TEXT("The renderer holds the three chunks"), Renderer.GetLoadedChunkCount(), 3);
		TArray<FIntVector> SeamOwners;
		Renderer.GetSeamMeshOwners(SeamOwners);
		TestTrue(TEXT("The seam lives with its owner"), SeamOwners.Num() == 1 && SeamOwners[0] == Equator);

		// Emptied regions retire; the primary component stays
		for (const FIntVector& Chunk : { Pole, PoleNeighbour, Equator })
		{
			Renderer.RemoveChunk(Chunk);
		}
		Renderer.RemoveSeamMesh(Equator, 0);
		Renderer.FlushPendingOperations();
		Renderer.GetRegionComponents(Components);
		TestTrue(TEXT("Empty regions retire, the primary component stays"), Components.Num() == 1 && Components[0].Key == FIntVector::ZeroValue
			&& Components[0].Value == Renderer.GetOriginRegionComponent());
		Renderer.Shutdown();
	}

	// Heightmap worlds split too (world modes review WM-MP5, D40): a plane 100 km out loses float precision as a big planet
	// does. The vertex math at 1e7, 1e8 and 5e7 along the plane; one component per region with content, at its origin.
	{
		UVoxelWorldConfiguration* Plane = MakeConfig(EWorldMode::InfinitePlane, 0.0);
		const FVoxelRenderRegions PlaneRegions = FVoxelRenderRegions::FromConfig(*Plane);
		TestTrue(TEXT("A heightmap world splits into render regions"), PlaneRegions.IsSplit());
		TestEqual(TEXT("... the same regions as a planet's"), PlaneRegions.RegionChunks, Regions.RegionChunks);
		const TPair<const TCHAR*, FVector> PlaneSites[] = {
			{ TEXT("100 km east"), FVector(1.0e7, 0.0, 0.0) },
			{ TEXT("1,000 km east"), FVector(1.0e8, 0.0, 0.0) },
			{ TEXT("500 km south-east"), FVector(5.0e7, -5.0e7, 0.0) },
		};
		for (const TPair<const TCHAR*, FVector>& Site : PlaneSites)
		{
			const FIntVector Chunk = ChunkAt(*Plane, Plane->WorldOrigin + Site.Value);
			const FVector ChunkWorldPos = Plane->WorldOrigin + FVector(Chunk) * ChunkWorldSize;
			const FIntVector Region = PlaneRegions.GetRegion(Chunk);
			const FVector RegionOrigin = PlaneRegions.GetRegionOrigin(Region);
			double MaxRegionError = 0.0, MaxAbsoluteError = 0.0;
			for (int32 i = 0; i < 2000; ++i)
			{
				const FVector3f Local(Random.FRandRange(0.0, 2.0 * ChunkWorldSize), Random.FRandRange(0.0, 2.0 * ChunkWorldSize), Random.FRandRange(0.0, 2.0 * ChunkWorldSize));
				const FVector Reference = ChunkWorldPos + FVector(Local);
				const FVector3f Stored = PlaneRegions.ToRegionLocal(ChunkWorldPos, Region) + Local;
				MaxRegionError = FMath::Max(MaxRegionError, FVector::Dist(RegionOrigin + FVector(Stored), Reference));
				const FVector3f Absolute = FVector3f(ChunkWorldPos) + Local;
				MaxAbsoluteError = FMath::Max(MaxAbsoluteError, FVector::Dist(FVector(Absolute), Reference));
			}
			AddInfo(FString::Printf(TEXT("plane, %s: chunk %s in region %s; a vertex's world position off by up to %.4f uu (absolute float, the rule before: %.2f uu)"),
				Site.Key, *Chunk.ToString(), *Region.ToString(), MaxRegionError, MaxAbsoluteError));
			TestTrue(*FString::Printf(TEXT("plane, %s: within 0.1 uu of the double reference (%.4f)"), Site.Key, MaxRegionError), MaxRegionError <= 0.1);
		}

		FVoxelMeshRenderer Renderer;
		Renderer.SetMaterial(UMaterial::GetDefaultMaterial(MD_Surface));
		Renderer.Initialize(World, Plane);
		const FIntVector Near(40, -20, 1);
		const FIntVector Far(-300, 7, 0);
		Renderer.UpdateChunkMeshFromCPU(Near, 0, MakeMesh());
		Renderer.UpdateChunkMeshFromCPU(Far, 0, MakeMesh());
		Renderer.FlushPendingOperations();
		TArray<TPair<FIntVector, UVoxelRegionComponent*>> Components;
		Renderer.GetRegionComponents(Components);
		TSet<FIntVector> PlaneExpected = { FIntVector::ZeroValue, PlaneRegions.GetRegion(Near), PlaneRegions.GetRegion(Far) };
		TestEqual(TEXT("Heightmap: a component per region with content, plus the primary one"), Components.Num(), PlaneExpected.Num());
		int32 Misplaced = 0;
		for (const TPair<FIntVector, UVoxelRegionComponent*>& Pair : Components)
		{
			Misplaced += (Pair.Value && Pair.Value->GetComponentLocation().Equals(PlaneRegions.GetRegionOrigin(Pair.Key), 1.0e-6)) ? 0 : 1;
		}
		TestEqual(TEXT("Heightmap: every component sits at its region's origin"), Misplaced, 0);
		Renderer.Shutdown();
		Plane->RemoveFromRoot();
	}

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	Config->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
