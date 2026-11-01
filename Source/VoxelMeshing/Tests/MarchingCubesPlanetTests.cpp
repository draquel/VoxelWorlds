// Copyright Daniel Raquel. All Rights Reserved.

// Marching cubes along local up (spherical Phase 4, WORLD_MODES_REVIEW_PLAN.md §18, WM-SY4; SP-D1, SP-D2).
//
// The terrain here lies across +X, as a planet's does on its equator facing +X: solid below the surface
// X = H(Y, Z), a grass layer 1.5 voxels deep over dirt. FVoxelMeshingRequest::LocalUp = +X is what the chunk manager
// passes there.
//
// - The LOD material scan walks along local up: with LocalUp +X the triangles take the surface's grass; along world Z
//   (the old scan) the walk stays inside one layer.
// - The boundary geomorph runs along the axis nearest local up: with LocalUp +X a chunk with a coarser neighbour moves
//   its seam-ramp vertices along X only; with LocalUp +Z (the old morph) the steepness gate rejects this terrain and
//   nothing moves, which is what happened at the equator.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUMarchingCubesMesher.h"
#include "VoxelMeshingTestFixtures.h"
#include "VoxelMeshingTypes.h"
#include "VoxelData.h"
#include "ChunkRenderData.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MarchingCubesPlanetTestUtils
{
	using namespace VoxelMeshingTestFixtures;

	constexpr int32 ChunkSize = 32;
	constexpr float VoxelSize = 100.0f;
	constexpr uint8 Grass = 1;
	constexpr uint8 Dirt = 2;

	/** The surface's X over (Y, Z): gently tilted with a ripple, inside the chunk. */
	float SurfaceX(float Y, float Z)
	{
		return 1600.0f + 0.12f * Y + 60.0f * FMath::Sin(Z * 0.01f);
	}

	/** A voxel of the +X-up terrain; bCurved gives the density a tanh profile (coarse and fine crossings then differ). */
	FVoxelData SampleField(const FVector& P, bool bCurved)
	{
		const float Depth = SurfaceX(P.Y, P.Z) - static_cast<float>(P.X); // below the surface along +X
		const float Normalized = FMath::Clamp(bCurved ? 0.5f + 0.5f * FMath::Tanh(Depth / 300.0f) : 0.5f + Depth / 1600.0f, 0.0f, 1.0f);
		FVoxelData Voxel;
		Voxel.Density = static_cast<uint8>(FMath::RoundToInt(Normalized * 255.0f));
		Voxel.MaterialID = Depth < 150.0f ? Grass : Dirt;
		Voxel.BiomeID = Depth < 150.0f ? 7 : 3;
		Voxel.Metadata = 0;
		return Voxel;
	}

	FVoxelData SampleLocal(int32 X, int32 Y, int32 Z, bool bCurved)
	{
		return SampleField(FVector(X, Y, Z) * VoxelSize, bCurved);
	}

	/** A request for chunk (0, 0, 0) with every face, edge and corner neighbour filled from the field. */
	FVoxelMeshingRequest MakeRequest(int32 LODLevel, const FVector& LocalUp, bool bCurved)
	{
		const int32 CS = ChunkSize;
		FVoxelMeshingRequest Request;
		Request.ChunkCoord = FIntVector::ZeroValue;
		Request.LODLevel = LODLevel;
		Request.ChunkSize = CS;
		Request.VoxelSize = VoxelSize;
		Request.LocalUp = LocalUp;
		Request.VoxelData.SetNumUninitialized(CS * CS * CS);
		for (int32 Z = 0; Z < CS; ++Z)
		for (int32 Y = 0; Y < CS; ++Y)
		for (int32 X = 0; X < CS; ++X)
		{
			Request.VoxelData[X + Y * CS + Z * CS * CS] = SampleLocal(X, Y, Z, bCurved);
		}

		const int32 Slice = CS * CS;
		for (TArray<FVoxelData>* Face : { &Request.NeighborXPos, &Request.NeighborXNeg, &Request.NeighborYPos, &Request.NeighborYNeg,
			&Request.NeighborZPos, &Request.NeighborZNeg })
		{
			Face->SetNumUninitialized(Slice);
		}
		for (int32 B = 0; B < CS; ++B)
		for (int32 A = 0; A < CS; ++A)
		{
			const int32 I = A + B * CS;
			Request.NeighborXPos[I] = SampleLocal(CS, A, B, bCurved);
			Request.NeighborXNeg[I] = SampleLocal(-1, A, B, bCurved);
			Request.NeighborYPos[I] = SampleLocal(A, CS, B, bCurved);
			Request.NeighborYNeg[I] = SampleLocal(A, -1, B, bCurved);
			Request.NeighborZPos[I] = SampleLocal(A, B, CS, bCurved);
			Request.NeighborZNeg[I] = SampleLocal(A, B, -1, bCurved);
		}
		TArray<FVoxelData>* Edges[12] = { &Request.EdgeXPosYPos, &Request.EdgeXPosYNeg, &Request.EdgeXNegYPos, &Request.EdgeXNegYNeg,
			&Request.EdgeXPosZPos, &Request.EdgeXPosZNeg, &Request.EdgeXNegZPos, &Request.EdgeXNegZNeg,
			&Request.EdgeYPosZPos, &Request.EdgeYPosZNeg, &Request.EdgeYNegZPos, &Request.EdgeYNegZNeg };
		for (TArray<FVoxelData>* Edge : Edges)
		{
			Edge->SetNumUninitialized(CS);
		}
		for (int32 I = 0; I < CS; ++I)
		{
			Request.EdgeXPosYPos[I] = SampleLocal(CS, CS, I, bCurved);
			Request.EdgeXPosYNeg[I] = SampleLocal(CS, -1, I, bCurved);
			Request.EdgeXNegYPos[I] = SampleLocal(-1, CS, I, bCurved);
			Request.EdgeXNegYNeg[I] = SampleLocal(-1, -1, I, bCurved);
			Request.EdgeXPosZPos[I] = SampleLocal(CS, I, CS, bCurved);
			Request.EdgeXPosZNeg[I] = SampleLocal(CS, I, -1, bCurved);
			Request.EdgeXNegZPos[I] = SampleLocal(-1, I, CS, bCurved);
			Request.EdgeXNegZNeg[I] = SampleLocal(-1, I, -1, bCurved);
			Request.EdgeYPosZPos[I] = SampleLocal(I, CS, CS, bCurved);
			Request.EdgeYPosZNeg[I] = SampleLocal(I, CS, -1, bCurved);
			Request.EdgeYNegZPos[I] = SampleLocal(I, -1, CS, bCurved);
			Request.EdgeYNegZNeg[I] = SampleLocal(I, -1, -1, bCurved);
		}
		Request.CornerXPosYPosZPos = SampleLocal(CS, CS, CS, bCurved);
		Request.CornerXPosYPosZNeg = SampleLocal(CS, CS, -1, bCurved);
		Request.CornerXPosYNegZPos = SampleLocal(CS, -1, CS, bCurved);
		Request.CornerXPosYNegZNeg = SampleLocal(CS, -1, -1, bCurved);
		Request.CornerXNegYPosZPos = SampleLocal(-1, CS, CS, bCurved);
		Request.CornerXNegYPosZNeg = SampleLocal(-1, CS, -1, bCurved);
		Request.CornerXNegYNegZPos = SampleLocal(-1, -1, CS, bCurved);
		Request.CornerXNegYNegZNeg = SampleLocal(-1, -1, -1, bCurved);
		Request.EdgeCornerFlags = 0xFFFFFFFFu;
		return Request;
	}

	bool Mesh(const FVoxelMeshingRequest& Request, bool bUseTransvoxel, FChunkMeshData& OutMesh)
	{
		FVoxelCPUMarchingCubesMesher Mesher;
		Mesher.Initialize();
		Mesher.SetConfig(MakeMCConfig(bUseTransvoxel));
		FVoxelMeshingStats Stats;
		const bool bOk = Mesher.GenerateMeshCPU(Request, OutMesh, Stats);
		Mesher.Shutdown();
		return bOk;
	}

	/** Share of the mesh's vertices whose material (Colors.R) is grass. */
	double GrassShare(const FChunkMeshData& Mesh)
	{
		int32 GrassCount = 0;
		for (const FColor& Color : Mesh.Colors)
		{
			GrassCount += Color.R == Grass ? 1 : 0;
		}
		return Mesh.Colors.Num() > 0 ? static_cast<double>(GrassCount) / Mesh.Colors.Num() : 0.0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMarchingCubesPlanetMaterialScanTest,
	"VoxelWorlds.Meshing.MarchingCubes.PlanetMaterialScan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMarchingCubesPlanetMaterialScanTest::RunTest(const FString& Parameters)
{
	using namespace MarchingCubesPlanetTestUtils;

	FChunkMeshData AlongUp, AlongZ;
	const bool bUp = Mesh(MakeRequest(1, FVector(1, 0, 0), false), false, AlongUp);
	const bool bZ = Mesh(MakeRequest(1, FVector::UpVector, false), false, AlongZ);
	if (!TestTrue(TEXT("Both chunks mesh"), bUp && bZ && AlongUp.Colors.Num() > 0 && AlongZ.Colors.Num() > 0))
	{
		return false;
	}
	const double ShareUp = GrassShare(AlongUp);
	const double ShareZ = GrassShare(AlongZ);
	AddInfo(FString::Printf(TEXT("LOD 1, terrain across +X: grass on %.1f%% of vertices scanning along local up (+X), %.1f%% along world Z"),
		ShareUp * 100.0, ShareZ * 100.0));
	TestTrue(TEXT("Scanning along local up, the surface takes its grass"), ShareUp >= 0.95);
	TestTrue(TEXT("Along world Z (the old scan) it does not, which is the defect fixed"), ShareZ < ShareUp - 0.05);
	TestEqual(TEXT("The scan does not change the geometry"), AlongUp.Positions.Num(), AlongZ.Positions.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMarchingCubesPlanetGeomorphTest,
	"VoxelWorlds.Meshing.MarchingCubes.PlanetGeomorph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMarchingCubesPlanetGeomorphTest::RunTest(const FString& Parameters)
{
	using namespace MarchingCubesPlanetTestUtils;

	// A LOD-0 chunk whose +Y neighbour is at LOD 1 (face 3), the transvoxel ribbon on, the curved density so the coarse
	// surface differs from the fine one. The same request is meshed with LocalUp +Z, where the steepness gate rejects
	// this terrain (the equator before Phase 4) and nothing moves, and with LocalUp +X, where the morph runs along X.
	// The morph only moves vertices, so the +Z mesh is the unmorphed reference and the two pair vertex by vertex; a
	// vertex the +Z mesh had morphed along Z would show up here as a move off the X axis.
	FVoxelMeshingRequest RequestZ = MakeRequest(0, FVector::UpVector, true);
	RequestZ.TransitionFaces = 1 << 3;
	RequestZ.NeighborLODLevels[3] = 1;
	FVoxelMeshingRequest RequestUp = RequestZ;
	RequestUp.LocalUp = FVector(1, 0, 0);

	FChunkMeshData Reference, AlongUp;
	const bool bReference = Mesh(RequestZ, true, Reference);
	const bool bUp = Mesh(RequestUp, true, AlongUp);
	const bool bSameTopology = bReference && bUp && Reference.Positions.Num() == AlongUp.Positions.Num() && Reference.Positions.Num() > 0;
	if (!TestTrue(TEXT("Both runs mesh the same topology (local up +Z and +X)"), bSameTopology))
	{
		return false;
	}

	int32 Moved = 0;
	int32 OffAxis = 0;
	float MaxShift = 0.0f;
	for (int32 i = 0; i < Reference.Positions.Num(); ++i)
	{
		const FVector3f Delta = AlongUp.Positions[i] - Reference.Positions[i];
		if (Delta.IsNearlyZero(1.0e-4f))
		{
			continue;
		}
		++Moved;
		MaxShift = FMath::Max(MaxShift, Delta.GetAbsMax());
		FVector3f OffX = Delta;
		OffX.X = 0.0f;
		OffAxis += OffX.IsNearlyZero(1.0e-4f) ? 0 : 1;
	}

	AddInfo(FString::Printf(TEXT("Terrain across +X, +Y neighbour coarser: local up +X moves %d vertices against the +Z mesh (largest shift %.1f uu, %d off the X axis)"),
		Moved, MaxShift, OffAxis));
	TestTrue(TEXT("Along local up the seam ramp is morphed"), Moved > 0);
	TestEqual(TEXT("... along X only (and along world Z the steepness gate rejects this terrain)"), OffAxis, 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
