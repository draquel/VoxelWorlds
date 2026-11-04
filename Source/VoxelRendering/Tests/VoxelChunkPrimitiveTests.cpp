// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Testing/VoxelTestScopedCVar.h"
#include "RHI.h"             // GUsingNullRHI: no scene under -nullrhi, the test skips there
#include "RenderUtils.h"     // IsRayTracingAllowed
#include "Materials/MaterialInterface.h"
#include "VoxelMeshRenderer.h"
#include "VoxelSceneProxy.h"
#include "VoxelChunkSceneProxy.h"
#include "VoxelRegionComponent.h"
#include "VoxelWorldConfiguration.h"
#include "ChunkRenderData.h"
#include "LocalVertexFactory.h"
#include "ObjectCacheContext.h"
#include "ComponentRecreateRenderStateContext.h"
#include "Components/ComponentInterfaces.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G116 step 1: with voxel.Render.ChunkPrimitives 1 every chunk interior mesh and seam bucket is a primitive of its own,
// registered without a component. A real world component and scene: a chunk's first mesh is one primitive with its
// triangles and ray tracing geometry; a re-mesh replaces it (one removed, one added, still one); a seam bucket is a
// second; a recreated region proxy keeps them (nothing lost, nothing re-sent) and gets the water tiles back from the
// component; an unloaded chunk and a removed bucket leave none, and no mesh bytes; voxel.Render.ChunkPrimitives 0 drops
// them and asks the owner for the meshes again. Needs a render scene: runs in the editor, skips under -nullrhi.
// ---------------------------------------------------------------------------

namespace VoxelChunkPrimitiveTest
{
	/** Quads in a row at height Z (chunk-local): 2 triangles each, so meshes are told apart by their triangle counts. */
	FChunkMeshData MakeQuads(int32 NumQuads, float Z)
	{
		FChunkMeshData Mesh;
		for (int32 Q = 0; Q < NumQuads; ++Q)
		{
			const uint32 Base = Mesh.Positions.Num();
			const float X = 10.0f + Q * 60.0f;
			Mesh.Positions.Append({ FVector3f(X, 10, Z), FVector3f(X + 50, 10, Z), FVector3f(X + 50, 60, Z), FVector3f(X, 60, Z) });
			for (int32 i = 0; i < 4; ++i)
			{
				Mesh.Normals.Add(FVector3f::UpVector);
				Mesh.UVs.Add(FVector2f::ZeroVector);
			}
			Mesh.Indices.Append({ Base, Base + 2, Base + 1, Base, Base + 3, Base + 2 });
		}
		return Mesh;
	}

	/** The bytes a primitive's mesh uploads for MakeQuads(NumQuads): 4 vertices and 6 indices a quad */
	int64 QuadBytes(int32 NumQuads, uint32 VertexStride)
	{
		return static_cast<int64>(FVoxelChunkRenderData::CalcGPUMemoryUsage(4 * NumQuads, 6 * NumQuads, VertexStride));
	}

	using FScopedCVar = VoxelTesting::FScopedCVar;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelChunkPrimitiveLifecycleTest,
	"VoxelWorlds.Rendering.ChunkPrimitives.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelChunkPrimitiveLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace VoxelChunkPrimitiveTest;
	if (GUsingNullRHI) { AddInfo(TEXT("Skipped: needs a render scene (run without -nullrhi)")); return true; }

	UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr, TEXT("/VoxelWorlds/Materials/M_VoxelMaster.M_VoxelMaster"));
	if (!TestNotNull(TEXT("The terrain master loads"), Master))
	{
		return false;
	}
	const FScopedCVar Primitives(TEXT("voxel.Render.ChunkPrimitives"), TEXT("1"));
	const FScopedCVar RayTracing(TEXT("voxel.Render.RayTracing"), TEXT("1"));
	const FScopedCVar Fade(TEXT("voxel.MeshFade"), TEXT("0"));
	const bool bRayTracing = IsRayTracingAllowed();

	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
	Config->AddToRoot();
	Config->WorldMode = EWorldMode::InfinitePlane;
	Config->WorldOrigin = FVector::ZeroVector;
	Config->VoxelSize = 50.0f;
	Config->ChunkSize = 32;
	Config->MeshingMode = EMeshingMode::DualContouring;

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelChunkPrimitiveTestWorld"));
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(World);
	{
		FVoxelMeshRenderer Renderer;
		Renderer.SetMaterial(Master);
		Renderer.Initialize(World, Config);
		UVoxelRegionComponent* Component = Renderer.GetOriginRegionComponent();
		if (!TestNotNull(TEXT("The renderer has a world component"), Component))
		{
			Renderer.Shutdown();
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			Config->RemoveFromRoot();
			return false;
		}

		const FIntVector Chunk(2, 1, 0);
		auto Info = [&]()
		{
			FVoxelChunkDrawInfo Out;
			Component->GetChunkDrawInfo(Chunk, Out);
			return Out;
		};
		const bool bManualVertexFetch = FLocalVertexFactory::StaticType.SupportsManualVertexFetch(World->GetFeatureLevel());
		const uint32 Stride = bManualVertexFetch ? sizeof(FVector3f) : sizeof(FVoxelLocalVertex);
		TArray<FIntVector> Lost;

		// A first mesh: one primitive, drawn with its triangles, traced
		FVoxelChunkPrimitiveCounters::FCounts Before = FVoxelChunkPrimitiveCounters::Get();
		Renderer.UpdateChunkMeshFromCPU(Chunk, 0, MakeQuads(4, 100.0f));
		Renderer.FlushPendingOperations();
		FVoxelChunkDrawInfo I = Info();
		TestTrue(TEXT("The component uses chunk primitives"), Component->IsUsingChunkPrimitives());
		TestEqual(TEXT("First mesh: one primitive"), Component->GetChunkPrimitiveCount(), 1);
		TestEqual(TEXT("First mesh: one primitive added to the scene"), FVoxelChunkPrimitiveCounters::Get().Added - Before.Added, int64(1));
		TestTrue(*FString::Printf(TEXT("First mesh: drawn with its 8 triangles (%u) at LOD 0"), I.CurrentTriangles), I.CurrentTriangles == 8 && I.LODLevel == 0);
		TestEqual(TEXT("First mesh: the vertex stream holds the positions alone under manual vertex fetch"), I.VertexStride, Stride);
		TestEqual(TEXT("First mesh: the component counts its buffers"), I.ProxyMeshBufferBytes, QuadBytes(4, Stride));
		TestEqual(TEXT("First mesh: the renderer reports the count"), Renderer.GetGPUMemoryUsage(), QuadBytes(4, Stride));
		if (bRayTracing)
		{
			TestTrue(*FString::Printf(TEXT("First mesh: traced with its 8 triangles (%u), one chunk geometry (%d)"), I.RayTracingTriangles, I.ProxyRayTracingChunks),
				I.bRayTracing && I.RayTracingTriangles == 8 && I.ProxyRayTracingChunks == 1 && I.ProxyRayTracingBytes > 0);
		}

		// A re-mesh: a new primitive replaces it in the same flush
		Before = FVoxelChunkPrimitiveCounters::Get();
		Renderer.UpdateChunkMeshFromCPU(Chunk, 1, MakeQuads(3, 101.0f));
		Renderer.FlushPendingOperations();
		I = Info();
		const FVoxelChunkPrimitiveCounters::FCounts AfterSwap = FVoxelChunkPrimitiveCounters::Get();
		TestTrue(*FString::Printf(TEXT("Re-mesh: one removed, one added (%lld, %lld), still one"), AfterSwap.Removed - Before.Removed, AfterSwap.Added - Before.Added),
			AfterSwap.Removed - Before.Removed == 1 && AfterSwap.Added - Before.Added == 1 && Component->GetChunkPrimitiveCount() == 1);
		TestTrue(TEXT("Re-mesh: the new mesh is drawn at LOD 1"), I.CurrentTriangles == 6 && I.LODLevel == 1);
		TestEqual(TEXT("Re-mesh: the component's primitives hold the new mesh's buffers alone"), I.ProxyMeshBufferBytes, QuadBytes(3, Stride));

		// Two re-meshes before a flush: the first never reaches the scene
		Before = FVoxelChunkPrimitiveCounters::Get();
		Renderer.UpdateChunkMeshFromCPU(Chunk, 1, MakeQuads(5, 101.0f));
		Renderer.UpdateChunkMeshFromCPU(Chunk, 1, MakeQuads(2, 101.0f));
		Renderer.FlushPendingOperations();
		I = Info();
		TestTrue(TEXT("Two re-meshes in a frame: one primitive added, the last mesh drawn"),
			FVoxelChunkPrimitiveCounters::Get().Added - Before.Added == 1 && I.CurrentTriangles == 4 && I.ProxyMeshBufferBytes == QuadBytes(2, Stride));

		// A seam bucket: a primitive of its own
		Renderer.UpdateSeamMeshFromCPU(Chunk, 0, 1, MakeQuads(2, 99.0f));
		Renderer.FlushPendingOperations();
		I = Info();
		TestEqual(TEXT("Seam bucket: a second primitive"), Component->GetChunkPrimitiveCount(), 2);
		TestEqual(TEXT("Seam bucket: counted"), I.ProxyMeshBufferBytes, QuadBytes(2, Stride) + QuadBytes(2, Stride));
		if (bRayTracing)
		{
			TestEqual(TEXT("Seam bucket: one seam geometry"), I.ProxyRayTracingSeams, 1);
		}

		// A water tile, then the region proxy recreated: the primitives stay, nothing is lost, the tile comes back
		const FIntVector2 Tile(Chunk.X, Chunk.Y);
		Renderer.UpdateWaterTileMesh(Tile, MakeQuads(1, 50.0f));
		Renderer.FlushPendingOperations();
		const int64 WithWater = Info().ProxyMeshBufferBytes;
		TestEqual(TEXT("Water tile: counted by the region proxy"), WithWater, QuadBytes(2, Stride) * 2 + QuadBytes(1, Stride));
		Component->MarkRenderStateDirty();
		World->SendAllEndOfFrameUpdates();
		Renderer.FlushPendingOperations();
		I = Info();
		TestFalse(TEXT("Region proxy recreated: nothing lost"), Renderer.ConsumeLostRenderData(Lost));
		TestTrue(*FString::Printf(TEXT("Region proxy recreated: the chunk still drawn (%u tris), both primitives kept (%d)"), I.CurrentTriangles, Component->GetChunkPrimitiveCount()),
			I.CurrentTriangles == 4 && Component->GetChunkPrimitiveCount() == 2);
		TestEqual(TEXT("Region proxy recreated: the water tile re-sent from the component"), I.ProxyMeshBufferBytes, WithWater);
		FString WaterLine;
		TestTrue(TEXT("Region proxy recreated: the new proxy holds the water tile"), Component->GetWaterTileDrawInfo(Tile, WaterLine));

		// The engine's material and shader updates find the primitives as components (IPrimitiveComponent): in the object
		// cache, among the primitives that use the terrain material, and recreated by a global render state recreation
		{
			const TArray<IPrimitiveComponent*> Ours = Component->GetChunkPrimitiveInterfaces();
			int32 Listed = 0, Affected = 0;
			{
				FObjectCacheContextScope Scope;
				for (IPrimitiveComponent* Each : Scope.GetContext().GetPrimitiveComponents())
				{
					Listed += Ours.Contains(Each) ? 1 : 0;
				}
				for (IPrimitiveComponent* Each : Ours)
				{
					int32 NumUsed = 0;
					for (UMaterialInterface* Material : Scope.GetContext().GetUsedMaterials(Each))
					{
						NumUsed += Material ? 1 : 0;
					}
					AddInfo(FString::Printf(TEXT("%s: render state %d, proxy %d, object cache used materials %d"), *Each->GetName(),
						Each->IsRenderStateCreated() ? 1 : 0, Each->GetSceneProxy() ? 1 : 0, NumUsed));
				}
				UMaterialInterface* Used = Component->GetMaterial(0);
				for (IPrimitiveComponent* Each : Scope.GetContext().GetPrimitivesAffectedByMaterials(MakeArrayView(&Used, 1)))
				{
					Affected += Ours.Contains(Each) ? 1 : 0;
				}
			}
			TestTrue(*FString::Printf(TEXT("Object cache: both primitives listed (%d) and affected by the terrain material (%d)"), Listed, Affected),
				Ours.Num() == 2 && Listed == 2 && Affected == 2);
			Before = FVoxelChunkPrimitiveCounters::Get();
			{
				FGlobalComponentRecreateRenderStateContext Recreate;
			}
			const FVoxelChunkPrimitiveCounters::FCounts AfterRecreate = FVoxelChunkPrimitiveCounters::Get();
			TestTrue(*FString::Printf(TEXT("Global render state recreation: both primitives recreated (%lld removed, %lld added)"),
				AfterRecreate.Removed - Before.Removed, AfterRecreate.Added - Before.Added),
				AfterRecreate.Removed - Before.Removed == 2 && AfterRecreate.Added - Before.Added == 2 && Info().CurrentTriangles == 4);
		}

		// The bucket removed, the chunk unloaded: nothing left
		Renderer.RemoveSeamMesh(Chunk, 0);
		Renderer.RemoveChunk(Chunk);
		Renderer.RemoveWaterTile(Tile);
		Renderer.FlushPendingOperations();
		I = Info();
		TestTrue(*FString::Printf(TEXT("Unloaded: no primitive, no mesh, no geometry (%d, %u tris, %d + %d geometries)"),
			Component->GetChunkPrimitiveCount(), I.CurrentTriangles, I.ProxyRayTracingChunks, I.ProxyRayTracingSeams),
			Component->GetChunkPrimitiveCount() == 0 && I.CurrentTriangles == 0 && I.ProxyRayTracingChunks == 0 && I.ProxyRayTracingSeams == 0);
		TestEqual(TEXT("Unloaded: no mesh buffer bytes held"), I.ProxyMeshBufferBytes, int64(0));

		// Back to the region proxy: the primitives go and the owner is asked for the meshes
		Renderer.UpdateChunkMeshFromCPU(Chunk, 0, MakeQuads(4, 100.0f));
		Renderer.FlushPendingOperations();
		{
			const FScopedCVar Off(TEXT("voxel.Render.ChunkPrimitives"), TEXT("0"));
			Renderer.FlushPendingOperations();
			TestTrue(TEXT("voxel.Render.ChunkPrimitives 0: the component draws in its region proxy again"), !Component->IsUsingChunkPrimitives());
			TestEqual(TEXT("voxel.Render.ChunkPrimitives 0: no primitive left"), Component->GetChunkPrimitiveCount(), 0);
			TestTrue(TEXT("voxel.Render.ChunkPrimitives 0: the owner is asked for the chunk again"), Renderer.ConsumeLostRenderData(Lost) && Lost.Contains(Chunk));
			Renderer.UpdateChunkMeshFromCPU(Chunk, 0, MakeQuads(4, 100.0f));
			Renderer.FlushPendingOperations();
			TestEqual(TEXT("voxel.Render.ChunkPrimitives 0: the region proxy draws the re-sent mesh"), Info().CurrentTriangles, uint32(8));
		}
		Renderer.FlushPendingOperations();
		TestTrue(TEXT("voxel.Render.ChunkPrimitives 1 again: chunk primitives, the owner asked again"),
			Component->IsUsingChunkPrimitives() && Renderer.ConsumeLostRenderData(Lost) && Lost.Contains(Chunk));

		Renderer.Shutdown();
	}
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	Config->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
