// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCollisionManager.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelMeshingTypes.h"
#include "ChunkRenderData.h"
#include "VoxelData.h"
#include "Chaos/TriangleMeshImplicitObject.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Collision trimesh winding (QA refinement R1).
// The voxel collision body is built directly as a Chaos implicit object from the render mesh. Physics
// winds triangles opposite to rendering, so the render indices must be converted the way the engine
// cooker converts every render-derived mesh. With the wrong winding every terrain face points into the
// ground: queries still hit (double-sided body) but report downward normals, and rigid bodies such as
// the death ragdoll fall through because Chaos contacts treat mesh triangles as one-sided.
// Pure CPU tests (no RHI, no world) -- run headless.
// ---------------------------------------------------------------------------

namespace VoxelCollisionWindingTestUtils
{
	constexpr int32 TestChunkSize = 32;
	constexpr float TestVoxelSize = 100.0f;
	constexpr int32 GroundTopVoxel = 12;

	/** Flat ground: solid at and below voxel Z = GroundTopVoxel. */
	static FVoxelMeshingRequest MakeFlatGroundRequest()
	{
		FVoxelMeshingRequest Request;
		Request.ChunkCoord = FIntVector::ZeroValue;
		Request.LODLevel = 0;
		Request.ChunkSize = TestChunkSize;
		Request.VoxelSize = TestVoxelSize;
		Request.VoxelData.SetNumUninitialized(TestChunkSize * TestChunkSize * TestChunkSize);
		for (int32 Z = 0; Z < TestChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < TestChunkSize; ++Y)
			{
				for (int32 X = 0; X < TestChunkSize; ++X)
				{
					const int32 Index = X + Y * TestChunkSize + Z * TestChunkSize * TestChunkSize;
					Request.VoxelData[Index] = (Z <= GroundTopVoxel) ? FVoxelData::Solid(1, 0) : FVoxelData::Air();
				}
			}
		}
		return Request;
	}

	/** Mesh with the smooth dual-contouring mesher the streaming pipeline uses. */
	static bool MeshFlatGround(FChunkMeshData& Out)
	{
		FVoxelMeshingConfig Config;
		Config.bUseSmoothMeshing = true;
		Config.IsoLevel = 0.5f;
		Config.bGenerateUVs = false;
		Config.bCalculateAO = false;
		Config.bGenerateSkirts = false;
		Config.QEFSVDThreshold = 0.1f;

		FVoxelCPUDualContourMesher Mesher;
		Mesher.Initialize();
		Mesher.SetConfig(Config);
		const bool bOk = Mesher.GenerateMeshCPU(MakeFlatGroundRequest(), Out);
		Mesher.Shutdown();
		return bOk;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCollisionWindingSwapTest,
	"VoxelWorlds.Streaming.Collision.Winding.SwapsFirstTwoIndices",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelCollisionWindingSwapTest::RunTest(const FString& Parameters)
{
	const TArray<uint32> RenderIndices = { 0, 1, 2, 3, 4, 5, 6 }; // two triangles + a stray index
	TArray<Chaos::TVector<int32, 3>> Triangles;
	Triangles.Add(Chaos::TVector<int32, 3>(9, 9, 9)); // must be replaced, not appended to
	UVoxelCollisionManager::BuildPhysicsTriangles(RenderIndices, Triangles);

	TestEqual(TEXT("one physics triangle per complete render triangle"), Triangles.Num(), 2);
	if (Triangles.Num() == 2)
	{
		TestTrue(TEXT("triangle 0 = (1, 0, 2)"), Triangles[0][0] == 1 && Triangles[0][1] == 0 && Triangles[0][2] == 2);
		TestTrue(TEXT("triangle 1 = (4, 3, 5)"), Triangles[1][0] == 4 && Triangles[1][1] == 3 && Triangles[1][2] == 5);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCollisionWindingFacesUpTest,
	"VoxelWorlds.Streaming.Collision.Winding.FlatGroundFacesUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelCollisionWindingFacesUpTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCollisionWindingTestUtils;

	FChunkMeshData Mesh;
	if (!TestTrue(TEXT("flat ground meshes"), MeshFlatGround(Mesh) && Mesh.IsValid()))
	{
		return false;
	}

	// Build the body exactly as the collision manager does.
	TArray<Chaos::TVec3<Chaos::FRealSingle>> Vertices;
	Vertices.Reserve(Mesh.Positions.Num());
	for (const FVector3f& P : Mesh.Positions)
	{
		Vertices.Add(Chaos::TVec3<Chaos::FRealSingle>(P.X, P.Y, P.Z));
	}
	TArray<Chaos::TVector<int32, 3>> Triangles;
	UVoxelCollisionManager::BuildPhysicsTriangles(Mesh.Indices, Triangles);
	const TArray<Chaos::TVector<int32, 3>> TrianglesCopy = Triangles; // the implicit object consumes its input
	TRefCountPtr<Chaos::FTriangleMeshImplicitObject> TriMesh = new Chaos::FTriangleMeshImplicitObject(
		MoveTemp(Vertices), MoveTemp(Triangles), TArray<uint16>());
	if (!TestTrue(TEXT("trimesh built"), TriMesh.IsValid()))
	{
		return false;
	}

	// Only judge the top surface away from the chunk borders: without neighbour data the mesher may close
	// the solid at the borders, and those faces legitimately face sideways or down.
	const float Lo = 3.0f * TestVoxelSize;
	const float Hi = (TestChunkSize - 3) * TestVoxelSize;
	const float TopZ = (GroundTopVoxel + 0.5f) * TestVoxelSize;
	auto IsTopFace = [&](const FVector3f& Centroid)
	{
		return Centroid.X > Lo && Centroid.X < Hi && Centroid.Y > Lo && Centroid.Y < Hi
			&& FMath::Abs(Centroid.Z - TopZ) < TestVoxelSize;
	};

	// 1) Our physics triangle order, judged with Chaos's face-normal formula (B - A) x (C - A).
	int32 TopFaces = 0;
	int32 UpFacing = 0;
	for (const Chaos::TVector<int32, 3>& T : TrianglesCopy)
	{
		const FVector3f A = Mesh.Positions[T[0]], B = Mesh.Positions[T[1]], C = Mesh.Positions[T[2]];
		if (!IsTopFace((A + B + C) / 3.0f))
		{
			continue;
		}
		++TopFaces;
		if (FVector3f::CrossProduct(B - A, C - A).GetSafeNormal().Z > 0.5f)
		{
			++UpFacing;
		}
	}

	// 2) The built body itself. Chaos stores triangles in its own internal order, so read its index buffer
	// and particles rather than assuming input order, and ask it for each face normal.
	int32 BodyTopFaces = 0;
	int32 BodyUpFacing = 0;
	auto JudgeBody = [&](const auto& Elements)
	{
		for (int32 FaceIdx = 0; FaceIdx < Elements.Num(); ++FaceIdx)
		{
			const auto& Tri = Elements[FaceIdx];
			const auto A = TriMesh->Particles().GetX(static_cast<int32>(Tri[0]));
			const auto B = TriMesh->Particles().GetX(static_cast<int32>(Tri[1]));
			const auto C = TriMesh->Particles().GetX(static_cast<int32>(Tri[2]));
			const FVector3f Centroid((A.X + B.X + C.X) / 3.0f, (A.Y + B.Y + C.Y) / 3.0f, (A.Z + B.Z + C.Z) / 3.0f);
			if (!IsTopFace(Centroid))
			{
				continue;
			}
			++BodyTopFaces;
			if (TriMesh->GetFaceNormal(FaceIdx).Z > 0.5f)
			{
				++BodyUpFacing;
			}
		}
	};
	if (TriMesh->Elements().RequiresLargeIndices())
	{
		JudgeBody(TriMesh->Elements().GetLargeIndexBuffer());
	}
	else
	{
		JudgeBody(TriMesh->Elements().GetSmallIndexBuffer());
	}

	TestEqual(TEXT("the body holds every top-surface face"), BodyTopFaces, TopFaces);
	TestEqual(TEXT("every top-surface body face normal points up (away from the solid)"), BodyUpFacing, BodyTopFaces);
	TestTrue(TEXT("the flat ground has top-surface faces to judge"), TopFaces > 100);
	TestEqual(TEXT("every top-surface face in physics order winds up (away from the solid)"), UpFacing, TopFaces);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
