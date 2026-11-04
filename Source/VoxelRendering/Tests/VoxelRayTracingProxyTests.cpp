// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Testing/VoxelTestScopedCVar.h"
#include "RHI.h"             // GUsingNullRHI: no scene proxy under -nullrhi, the test skips there
#include "RenderUtils.h"     // IsRayTracingAllowed
#include "Materials/MaterialInterface.h"
#include "VoxelMeshRenderer.h"
#include "VoxelSceneProxy.h"
#include "VoxelRegionComponent.h"
#include "VoxelWorldConfiguration.h"
#include "ChunkRenderData.h"
#include "LocalVertexFactory.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Visual polish F3.3: every current chunk and seam mesh of the scene proxy has ray tracing geometry, and it goes with the
// mesh. A real world component and scene proxy: a chunk's first mesh gets geometry with its triangles; a crossfaded swap
// traces the new mesh only (the retained previous mesh has none); a seam bucket gets its own and loses it on removal; an
// unloaded chunk leaves the proxy with none; with voxel.Render.RayTracing 0 the recreated proxy builds none. Needs a render
// scene and ray tracing: runs in the editor on a ray tracing GPU, skips under -nullrhi or without ray tracing support.
// G119 and G120 on the same meshes: under manual vertex fetch the vertex stream holds the positions alone (the geometry
// above is built over it at stride 12), and the proxy's mesh buffer count, which the renderer reports (the bench's
// RendGPUMB), follows every upload, retained crossfade mesh, seam and release to 0.
// ---------------------------------------------------------------------------

namespace VoxelRayTracingProxyTest
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

	/** The bytes the proxy uploads for MakeQuads(NumQuads): 4 vertices and 6 indices a quad */
	int64 QuadBytes(int32 NumQuads, uint32 VertexStride)
	{
		return static_cast<int64>(FVoxelChunkRenderData::CalcGPUMemoryUsage(4 * NumQuads, 6 * NumQuads, VertexStride));
	}

	using FScopedCVar = VoxelTesting::FScopedCVar;

	/**
	 * FVoxelRayTracingChunkGeometryTest on one drawing path: the region proxy's per-frame gather, or the chunk primitives'
	 * cached instances (G116 step 3), whose geometry follows the mesh the same way.
	 */
	bool RunChunkGeometry(FAutomationTestBase& Test, bool bChunkPrimitives)
	{
		if (GUsingNullRHI) { Test.AddInfo(TEXT("Skipped: needs a render scene (run without -nullrhi, where components create no scene proxy)")); return true; }
		if (!IsRayTracingAllowed()) { Test.AddInfo(TEXT("Skipped: ray tracing is not allowed here (the project's support, the RHI or the GPU)")); return true; }

		UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr, TEXT("/VoxelWorlds/Materials/M_VoxelMaster.M_VoxelMaster"));
		if (!Test.TestNotNull(TEXT("The terrain master loads"), Master))
		{
			return false;
		}
		const FScopedCVar Primitives(TEXT("voxel.Render.ChunkPrimitives"), bChunkPrimitives ? TEXT("1") : TEXT("0"));
		const FScopedCVar RayTracing(TEXT("voxel.Render.RayTracing"), TEXT("1"));
		const FScopedCVar Fade(TEXT("voxel.MeshFade"), TEXT("1"));
		const FScopedCVar Duration(TEXT("voxel.MeshFade.Duration"), TEXT("0.35"));

		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::InfinitePlane;
		Config->WorldOrigin = FVector::ZeroVector;
		Config->VoxelSize = 50.0f;
		Config->ChunkSize = 32;
		Config->MeshingMode = EMeshingMode::DualContouring;

		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelRayTracingProxyTestWorld"));
		FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
		WorldContext.SetCurrentWorld(World);
		{
			FVoxelMeshRenderer Renderer;
			Renderer.SetMaterial(Master);
			Renderer.Initialize(World, Config);
			UVoxelRegionComponent* Component = Renderer.GetOriginRegionComponent();
			auto Finish = [&]()
			{
				Renderer.Shutdown();
			};
			if (!Test.TestNotNull(TEXT("The renderer has a world component"), Component))
			{
				Finish();
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
				Config->RemoveFromRoot();
				return false;
			}

			// A chunk of render region (0, 0, 0), the world component's own
			const FIntVector Chunk(2, 1, 0);
			auto Info = [&]()
			{
				FVoxelChunkDrawInfo Out;
				Component->GetChunkDrawInfo(Chunk, Out);
				return Out;
			};

			// A first mesh: traced with its triangles
			Renderer.UpdateChunkMeshFromCPU(Chunk, 0, MakeQuads(4, 100.0f));
			Renderer.FlushPendingOperations();
			FVoxelChunkDrawInfo I = Info();
			Test.TestTrue(TEXT("The proxy gives its meshes ray tracing geometry"), I.bRayTracing);
			Test.TestTrue(*FString::Printf(TEXT("First mesh: traced with its 8 triangles (%u)"), I.RayTracingTriangles), I.CurrentTriangles == 8 && I.RayTracingTriangles == 8);
			Test.TestTrue(*FString::Printf(TEXT("First mesh: one chunk geometry in the proxy, with a size (%d, %llu bytes)"), I.ProxyRayTracingChunks, I.ProxyRayTracingBytes),
				I.ProxyRayTracingChunks == 1 && I.ProxyRayTracingBytes > 0);
			const bool bManualVertexFetch = FLocalVertexFactory::StaticType.SupportsManualVertexFetch(World->GetFeatureLevel());
			const uint32 Stride = bManualVertexFetch ? sizeof(FVector3f) : sizeof(FVoxelLocalVertex);
			Test.TestEqual(TEXT("First mesh: the vertex stream holds the positions alone under manual vertex fetch"), I.VertexStride, Stride);
			Test.TestEqual(TEXT("First mesh: the proxy counts its buffers"), I.ProxyMeshBufferBytes, QuadBytes(4, Stride));
			Test.TestEqual(TEXT("First mesh: the renderer reports the proxy's count"), Renderer.GetGPUMemoryUsage(), QuadBytes(4, Stride));

			// An LOD swap crossfades: the new mesh is traced, the retained previous mesh is not
			Renderer.UpdateChunkMeshFromCPU(Chunk, 1, MakeQuads(3, 101.0f));
			Renderer.FlushPendingOperations();
			I = Info();
			Test.TestTrue(*FString::Printf(TEXT("LOD swap: crossfading from the old mesh (current %u, previous %u tris)"), I.CurrentTriangles, I.PreviousTriangles),
				I.bCrossfading && I.CurrentTriangles == 6 && I.PreviousTriangles == 8);
			Test.TestTrue(*FString::Printf(TEXT("LOD swap: the new mesh is traced, alone (%u tris, %d chunk geometries)"), I.RayTracingTriangles, I.ProxyRayTracingChunks),
				I.RayTracingTriangles == 6 && I.ProxyRayTracingChunks == 1);
			Test.TestEqual(TEXT("LOD swap: the count holds the new mesh and the retained one"), I.ProxyMeshBufferBytes, QuadBytes(3, Stride) + QuadBytes(4, Stride));

			// A seam bucket (face +X of the chunk) gets its own geometry and loses it with the bucket
			Renderer.UpdateSeamMeshFromCPU(Chunk, 0, 1, MakeQuads(2, 99.0f));
			Renderer.FlushPendingOperations();
			I = Info();
			Test.TestEqual(TEXT("Seam bucket: one seam geometry"), I.ProxyRayTracingSeams, 1);
			Test.TestEqual(TEXT("Seam bucket: counted"), I.ProxyMeshBufferBytes, QuadBytes(3, Stride) + QuadBytes(4, Stride) + QuadBytes(2, Stride));
			Renderer.RemoveSeamMesh(Chunk, 0);
			Renderer.FlushPendingOperations();
			I = Info();
			Test.TestEqual(TEXT("Seam bucket removed: no seam geometry"), I.ProxyRayTracingSeams, 0);

			// Unloaded: the chunk's geometry goes with its mesh
			Renderer.RemoveChunk(Chunk);
			Renderer.FlushPendingOperations();
			I = Info();
			Test.TestTrue(*FString::Printf(TEXT("Unloaded: no mesh and no geometry left (%u tris, %d chunk and %d seam geometries, %llu bytes)"),
				I.CurrentTriangles, I.ProxyRayTracingChunks, I.ProxyRayTracingSeams, I.ProxyRayTracingBytes),
				I.CurrentTriangles == 0 && I.RayTracingTriangles == 0 && I.ProxyRayTracingChunks == 0 && I.ProxyRayTracingSeams == 0 && I.ProxyRayTracingBytes == 0);
			Test.TestEqual(TEXT("Unloaded: no mesh buffer bytes left"), I.ProxyMeshBufferBytes, int64(0));
			// (A chunk primitive's mesh goes when the scene deletes the primitive, at its next update: the renderer's total holds it
			// until a frame renders)
			if (!bChunkPrimitives)
			{
				Test.TestEqual(TEXT("Unloaded: the renderer reports none"), Renderer.GetGPUMemoryUsage(), int64(0));
			}

			// voxel.Render.RayTracing 0 recreates the proxies; the new one builds no geometry
			{
				const FScopedCVar Off(TEXT("voxel.Render.RayTracing"), TEXT("0"));
				World->SendAllEndOfFrameUpdates();
				Renderer.UpdateChunkMeshFromCPU(Chunk, 0, MakeQuads(5, 100.0f));
				Renderer.FlushPendingOperations();
				I = Info();
				Test.TestTrue(*FString::Printf(TEXT("voxel.Render.RayTracing 0: the mesh is drawn (%u tris) and not traced (ray tracing %d, %u tris)"),
					I.CurrentTriangles, I.bRayTracing ? 1 : 0, I.RayTracingTriangles),
					I.CurrentTriangles == 10 && !I.bRayTracing && I.RayTracingTriangles == 0 && I.ProxyRayTracingChunks == 0);
			}
			World->SendAllEndOfFrameUpdates();

			Finish();
		}
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		Config->RemoveFromRoot();
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelRayTracingChunkGeometryTest,
	"VoxelWorlds.Rendering.RayTracing.ChunkGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelRayTracingChunkGeometryTest::RunTest(const FString& Parameters)
{
	return VoxelRayTracingProxyTest::RunChunkGeometry(*this, /*bChunkPrimitives*/ false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelChunkPrimitiveRayTracingTest,
	"VoxelWorlds.Rendering.ChunkPrimitives.RayTracing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelChunkPrimitiveRayTracingTest::RunTest(const FString& Parameters)
{
	return VoxelRayTracingProxyTest::RunChunkGeometry(*this, /*bChunkPrimitives*/ true);
}

#endif // WITH_DEV_AUTOMATION_TESTS
