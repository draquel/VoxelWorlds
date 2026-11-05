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
#include "MeshCardBuild.h"
#include "MeshCardRepresentation.h"
#include "HAL/IConsoleManager.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G116 step 1: with voxel.Render.ChunkPrimitives 1 every chunk interior mesh and seam bucket is a primitive of its own,
// registered without a component. A real world component and scene: a chunk's first mesh is one primitive with its
// triangles and ray tracing geometry; a re-mesh replaces it (one removed, one added, still one); a seam bucket is a
// second; a recreated region proxy keeps them (nothing lost, nothing re-sent) and gets the water tiles back from the
// component; an unloaded chunk and a removed bucket leave none, and no mesh bytes; voxel.Render.ChunkPrimitives 0 drops
// them and asks the owner for the meshes again. Needs a render scene: runs in the editor, skips under -nullrhi.
// G116 step 4 on the same primitives: each mesh has six Lumen cards and the primitive is in Lumen's scene where Lumen
// traces the ray tracing scene; voxel.Render.LumenCards 0 re-sends the meshes without cards, out of Lumen's scene.
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

	/** Whether Lumen traces the ray tracing scene (a primitive with cards and ray tracing geometry joins Lumen's scene) */
	bool IsLumenHardwareRayTracing()
	{
		const IConsoleVariable* LumenHardwareRayTracing = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Lumen.HardwareRayTracing"));
		return IsRayTracingAllowed() && (GRHISupportsRayTracingShaders || GRHISupportsInlineRayTracing)
			&& LumenHardwareRayTracing && LumenHardwareRayTracing->GetInt() != 0;
	}
}

// ---------------------------------------------------------------------------
// G116 step 4: a chunk mesh's Lumen cards are six axis-aligned cards over its box, one per face direction, one-sided, built
// with the mesh (no render thread, no neighbour); none when the cards are off or the mesh has no triangles. Headless.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelChunkMeshCardsFromBoundsTest,
	"VoxelWorlds.Rendering.ChunkPrimitives.CardsFromBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelChunkMeshCardsFromBoundsTest::RunTest(const FString& Parameters)
{
	const TSharedRef<std::atomic<int64>, ESPMode::ThreadSafe> Bytes = MakeShared<std::atomic<int64>, ESPMode::ThreadSafe>(0);
	auto MakeMesh = [&](const FBox& LocalBox, int32 NumTriangles, bool bCards)
	{
		TArray<FVoxelVertex> Vertices;
		Vertices.SetNum(3);
		TArray<uint32> Indices;
		for (int32 T = 0; T < NumTriangles; ++T)
		{
			Indices.Append({ 0u, 1u, 2u });
		}
		return MakeUnique<FVoxelChunkMesh>(MoveTemp(Vertices), MoveTemp(Indices), 0, LocalBox, FVector(1.0e6, -2.0e6, 3.0e5), Bytes, bCards);
	};

	// A chunk's box relative to its origin, flatter than it is wide (a terrain chunk's mesh)
	const FBox Box(FVector(0.0, 0.0, 700.0), FVector(1600.0, 1600.0, 1100.0));
	const TUniquePtr<FVoxelChunkMesh> Mesh = MakeMesh(Box, 4, true);
	const FCardRepresentationData* Cards = Mesh->CardRepresentation.Get();
	if (!TestNotNull(TEXT("Cards on: the mesh has a card representation"), Cards))
	{
		return false;
	}
	const FMeshCardsBuildData& Data = Cards->MeshCardsBuildData;
	TestTrue(TEXT("Cards on: the cards' box is the mesh's"), Data.Bounds.Min.Equals(Box.Min) && Data.Bounds.Max.Equals(Box.Max));
	TestFalse(TEXT("Cards on: one-sided"), Data.bMostlyTwoSided);
	if (!TestEqual(TEXT("Cards on: six cards"), Data.CardBuildData.Num(), 6))
	{
		return false;
	}
	uint32 DirectionsSeen = 0;
	for (const FLumenCardBuildData& Card : Data.CardBuildData)
	{
		DirectionsSeen |= 1u << Card.AxisAlignedDirectionIndex;
		const FVector3f Direction = MeshCardRepresentation::GetAxisAlignedDirection(Card.AxisAlignedDirectionIndex);
		TestTrue(*FString::Printf(TEXT("Card %u faces its direction"), Card.AxisAlignedDirectionIndex), Card.OBB.AxisZ.Equals(Direction, 1.0e-4f));
		// Every corner of the box lies inside the card's box (its extent along the card's own axes)
		bool bCovers = true;
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector3f Point(Corner & 1 ? Box.Max.X : Box.Min.X, Corner & 2 ? Box.Max.Y : Box.Min.Y, Corner & 4 ? Box.Max.Z : Box.Min.Z);
			const FVector3f FromOrigin = Point - FVector3f(Card.OBB.Origin);
			bCovers &= FMath::Abs(FromOrigin.Dot(Card.OBB.AxisX)) <= Card.OBB.Extent.X + 0.01f
				&& FMath::Abs(FromOrigin.Dot(Card.OBB.AxisY)) <= Card.OBB.Extent.Y + 0.01f
				&& FMath::Abs(FromOrigin.Dot(Card.OBB.AxisZ)) <= Card.OBB.Extent.Z + 0.01f;
		}
		TestTrue(*FString::Printf(TEXT("Card %u covers the mesh's box"), Card.AxisAlignedDirectionIndex), bCovers);
	}
	TestEqual(TEXT("Cards on: one card per face direction"), DirectionsSeen, 0x3Fu);

	// Slices: two cards per slice of each axis, each over its slice's depth along the axis and the whole box across
	{
		FMeshCardsBuildData Sliced;
		BuildVoxelChunkMeshCards(Box, 3, Sliced);
		TestEqual(TEXT("3 slices: 18 cards"), Sliced.CardBuildData.Num(), 18);
		int32 PerDirection[6] = {};
		double DepthCovered[6] = {};
		for (const FLumenCardBuildData& Card : Sliced.CardBuildData)
		{
			const int32 Axis = Card.AxisAlignedDirectionIndex / 2;
			++PerDirection[Card.AxisAlignedDirectionIndex];
			// Each slice's card box spans a third of the box along its axis, plus the engine's 5 uu card offset
			DepthCovered[Card.AxisAlignedDirectionIndex] += 2.0 * Card.OBB.Extent.Z - 10.0;
			const FVector3f Center = FVector3f(Card.OBB.Origin) - 5.0f * Card.OBB.AxisZ;
			TestTrue(*FString::Printf(TEXT("3 slices: card %u's slice lies inside the box along its axis"), Card.AxisAlignedDirectionIndex),
				Center[Axis] - (Card.OBB.Extent.Z - 5.0f) >= Box.Min[Axis] - 0.01 && Center[Axis] + (Card.OBB.Extent.Z - 5.0f) <= Box.Max[Axis] + 0.01);
		}
		for (int32 Direction = 0; Direction < 6; ++Direction)
		{
			TestTrue(*FString::Printf(TEXT("3 slices: direction %d has 3 cards (%d) whose slices fill the box along it (%.1f of %.1f)"), Direction,
				PerDirection[Direction], DepthCovered[Direction], Box.GetSize()[Direction / 2]),
				PerDirection[Direction] == 3 && FMath::IsNearlyEqual(DepthCovered[Direction], Box.GetSize()[Direction / 2], 0.1));
		}
		FMeshCardsBuildData Clamped;
		BuildVoxelChunkMeshCards(Box, 9, Clamped);
		TestEqual(TEXT("9 slices: clamped to 5 (30 cards, under Lumen's 32 a mesh)"), Clamped.CardBuildData.Num(), 6 * MaxVoxelChunkMeshCardSlices);
	}

	TestNull(TEXT("Cards off: no card representation"), MakeMesh(Box, 4, false)->CardRepresentation.Get());
	TestNull(TEXT("No triangles: no card representation"), MakeMesh(Box, 0, true)->CardRepresentation.Get());
	TestNull(TEXT("No box: no card representation"), MakeMesh(FBox(ForceInit), 4, true)->CardRepresentation.Get());
	TestEqual(TEXT("Nothing uploaded: no buffer bytes counted"), Bytes->load(), int64(0));
	return true;
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
	const FScopedCVar Cards(TEXT("voxel.Render.LumenCards"), TEXT("1"));
	const bool bRayTracing = IsRayTracingAllowed();
	const bool bLumenTracesScene = IsLumenHardwareRayTracing();

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
		TestEqual(TEXT("First mesh: six Lumen cards (step 4)"), I.LumenCards, 6);
		if (bLumenTracesScene)
		{
			TestTrue(TEXT("First mesh: in Lumen's scene"), I.bVisibleInLumenScene);
		}
		else
		{
			AddInfo(TEXT("Lumen does not trace the ray tracing scene here: the scene membership is not checked"));
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

		// Lumen cards off: the primitives are recreated without cards, out of Lumen's scene, from meshes the owner sends again
		{
			const FScopedCVar CardsOff(TEXT("voxel.Render.LumenCards"), TEXT("0"));
			Renderer.FlushPendingOperations();
			TestTrue(TEXT("voxel.Render.LumenCards 0: still chunk primitives, none left, the owner asked for the chunk again"),
				Component->IsUsingChunkPrimitives() && Component->GetChunkPrimitiveCount() == 0 && Renderer.ConsumeLostRenderData(Lost) && Lost.Contains(Chunk));
			Renderer.UpdateChunkMeshFromCPU(Chunk, 1, MakeQuads(2, 101.0f));
			Renderer.UpdateSeamMeshFromCPU(Chunk, 0, 1, MakeQuads(2, 99.0f));
			Renderer.FlushPendingOperations();
			I = Info();
			TestTrue(*FString::Printf(TEXT("voxel.Render.LumenCards 0: the re-sent chunk drawn (%u tris) with no cards (%d), out of Lumen's scene"),
				I.CurrentTriangles, I.LumenCards), I.CurrentTriangles == 4 && I.LumenCards == 0 && !I.bVisibleInLumenScene);
		}
		Renderer.FlushPendingOperations();
		TestTrue(TEXT("voxel.Render.LumenCards 1 again: the owner asked for the chunk again"), Renderer.ConsumeLostRenderData(Lost) && Lost.Contains(Chunk));
		Renderer.UpdateChunkMeshFromCPU(Chunk, 1, MakeQuads(2, 101.0f));
		Renderer.UpdateSeamMeshFromCPU(Chunk, 0, 1, MakeQuads(2, 99.0f));
		Renderer.FlushPendingOperations();
		I = Info();
		TestTrue(*FString::Printf(TEXT("voxel.Render.LumenCards 1 again: six cards (%d), both primitives (%d)"), I.LumenCards, Component->GetChunkPrimitiveCount()),
			I.LumenCards == 6 && Component->GetChunkPrimitiveCount() == 2 && (!bLumenTracesScene || I.bVisibleInLumenScene));

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
