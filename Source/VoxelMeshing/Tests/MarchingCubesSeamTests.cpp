// Copyright Daniel Raquel. All Rights Reserved.

// Seam-ownership P3 closure tests for the Marching Cubes seam meshers
// (VoxelCPUMarchingCubesSeams.cpp). Mirrors the DC SeamClosure suite's structure:
//   MCS0 — interior-domain output is an exact subset of the Full-domain output
//   MCS1 — same-LOD face seam closes the junction slab between two interior meshes
//   MCS2 — 2x2x2 assembly (8 interiors + 12 faces + 6 edges + 1 corner) closes its interior
//   MCS3 — mixed-LOD face seam (fine|coarse, ribbon + band-confined morph): junction
//          open-edge count is no worse than the legacy whole-chunk transvoxel meshes
//          (legacy carries inherent T-junctions at the ribbon's fine edge, so exact
//          watertightness is not the bar — parity with legacy is).
// MC triangle soup has no shared vertices, so closure is measured by quantized-position
// vertex welding + boundary-edge counting scoped to a spatial window.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUMarchingCubesMesher.h"
#include "VoxelMeshingTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MCSeamTest
{

static constexpr int32 CS = 32;
static constexpr float VoxSize = 100.0f;

// World-continuous height field: solid below Height(wx, wy). Slanted on both axes so every
// boundary carries a live iso-surface.
static float HeightAt(float WX, float WY)
{
	return 14.0f + WX * 10.0f / 64.0f + WY * 6.0f / 64.0f;
}

static TSharedPtr<const TArray<FVoxelData>> MakeChunkVoxels(const FIntVector& ChunkCoord)
{
	TSharedPtr<TArray<FVoxelData>> Voxels = MakeShared<TArray<FVoxelData>>();
	Voxels->SetNumZeroed(CS * CS * CS);
	for (int32 Z = 0; Z < CS; ++Z)
	{
		for (int32 Y = 0; Y < CS; ++Y)
		{
			for (int32 X = 0; X < CS; ++X)
			{
				const float WX = static_cast<float>(ChunkCoord.X * CS + X);
				const float WY = static_cast<float>(ChunkCoord.Y * CS + Y);
				const float WZ = static_cast<float>(ChunkCoord.Z * CS + Z);
				const float H = HeightAt(WX, WY);
				FVoxelData& V = (*Voxels)[X + Y * CS + Z * CS * CS];
				// Smooth density ramp around the surface for clean MC crossings.
				const float D = FMath::Clamp(0.5f + (H - WZ) * 0.25f, 0.0f, 1.0f);
				V.Density = static_cast<uint8>(FMath::RoundToInt(D * 255.0f));
				V.MaterialID = 1;
			}
		}
	}
	return Voxels;
}

static FVoxelMeshingRequest MakeInteriorRequest(const FIntVector& Coord, int32 LOD,
	const TArray<FVoxelData>& Voxels)
{
	FVoxelMeshingRequest R;
	R.ChunkCoord = Coord;
	R.LODLevel = LOD;
	R.ChunkSize = CS;
	R.VoxelSize = VoxSize;
	R.VoxelData = Voxels;
	for (int32 i = 0; i < 6; ++i) { R.NeighborLODLevels[i] = LOD; }
	R.MeshCellDomain = EVoxelMeshCellDomain::Interior;
	return R;
}

static FVoxelCPUMarchingCubesMesher& Mesher()
{
	static FVoxelCPUMarchingCubesMesher M;
	if (!M.IsInitialized())
	{
		M.Initialize();
		FVoxelMeshingConfig C;
		C.bUseSmoothMeshing = true;
		C.IsoLevel = 0.5f;
		C.bUseTransvoxel = true;
		C.bGenerateSkirts = false;
		M.SetConfig(C);
	}
	return M;
}

// Append Src translated by ChunkOffset (world units) into Accum.
static void Append(FChunkMeshData& Accum, const FChunkMeshData& Src, const FVector3f& Offset)
{
	const int32 Base = Accum.Positions.Num();
	for (int32 i = 0; i < Src.Positions.Num(); ++i)
	{
		Accum.Positions.Add(Src.Positions[i] + Offset);
	}
	for (int32 i = 0; i < Src.Indices.Num(); ++i)
	{
		Accum.Indices.Add(Base + Src.Indices[i]);
	}
}

// Count boundary edges (used by exactly one triangle after quantized-position welding) whose
// midpoint lies inside [WindowMin, WindowMax] (world units).
static int32 CountOpenEdgesInWindow(const FChunkMeshData& Mesh,
	const FVector3f& WindowMin, const FVector3f& WindowMax)
{
	TMap<FIntVector, int32> VertexIds;
	TArray<FVector3f> UniquePos;
	TArray<int32> Remap;
	Remap.Reserve(Mesh.Positions.Num());
	auto Quant = [](const FVector3f& P) {
		return FIntVector(FMath::RoundToInt(P.X * 8.0f / VoxSize),
			FMath::RoundToInt(P.Y * 8.0f / VoxSize),
			FMath::RoundToInt(P.Z * 8.0f / VoxSize));
	};
	for (const FVector3f& P : Mesh.Positions)
	{
		const FIntVector Q = Quant(P);
		if (const int32* Found = VertexIds.Find(Q))
		{
			Remap.Add(*Found);
		}
		else
		{
			const int32 Id = UniquePos.Add(P);
			VertexIds.Add(Q, Id);
			Remap.Add(Id);
		}
	}
	TMap<TPair<int32, int32>, int32> EdgeUse;
	const int32 NumTris = Mesh.Indices.Num() / 3;
	for (int32 t = 0; t < NumTris; ++t)
	{
		int32 V[3] = {
			Remap[Mesh.Indices[t * 3 + 0]],
			Remap[Mesh.Indices[t * 3 + 1]],
			Remap[Mesh.Indices[t * 3 + 2]] };
		if (V[0] == V[1] || V[1] == V[2] || V[0] == V[2])
		{
			continue; // degenerate after weld
		}
		for (int32 e = 0; e < 3; ++e)
		{
			const int32 A = V[e], B = V[(e + 1) % 3];
			EdgeUse.FindOrAdd(TPair<int32, int32>(FMath::Min(A, B), FMath::Max(A, B)))++;
		}
	}
	int32 Open = 0;
	for (const auto& Pair : EdgeUse)
	{
		if (Pair.Value != 1)
		{
			continue;
		}
		const FVector3f Mid = (UniquePos[Pair.Key.Key] + UniquePos[Pair.Key.Value]) * 0.5f;
		if (Mid.X >= WindowMin.X && Mid.X <= WindowMax.X &&
			Mid.Y >= WindowMin.Y && Mid.Y <= WindowMax.Y &&
			Mid.Z >= WindowMin.Z && Mid.Z <= WindowMax.Z)
		{
			++Open;
		}
	}
	return Open;
}

// Fill facing slices for a legacy (Full-domain) request from the neighbor's volume.
static void FillLegacyFacing(FVoxelMeshingRequest& R, int32 Axis, bool bPos, int32 Depth,
	const TArray<FVoxelData>& Other)
{
	TArray<FVoxelData>* Plane = nullptr;
	TArray<FVoxelData>* Deep = nullptr;
	switch (Axis * 2 + (bPos ? 1 : 0))
	{
	case 0: Plane = &R.NeighborXNeg; Deep = &R.NeighborXNegDeep; break;
	case 1: Plane = &R.NeighborXPos; Deep = &R.NeighborXPosDeep; break;
	case 2: Plane = &R.NeighborYNeg; Deep = &R.NeighborYNegDeep; break;
	case 3: Plane = &R.NeighborYPos; Deep = &R.NeighborYPosDeep; break;
	case 4: Plane = &R.NeighborZNeg; Deep = &R.NeighborZNegDeep; break;
	default: Plane = &R.NeighborZPos; Deep = &R.NeighborZPosDeep; break;
	}
	const int32 SliceSize = CS * CS;
	Plane->SetNumUninitialized(SliceSize);
	if (Depth > 1) { Deep->SetNumUninitialized((Depth - 1) * SliceSize); }
	R.NeighborPlaneDepth = Depth;
	for (int32 d = 0; d < Depth; ++d)
	{
		const int32 AxisCoord = bPos ? d : (CS - 1 - d);
		for (int32 b = 0; b < CS; ++b)
		for (int32 a = 0; a < CS; ++a)
		{
			int32 V[3];
			V[Axis] = AxisCoord;
			V[(Axis == 0) ? 1 : 0] = a;
			V[(Axis == 2) ? 1 : 2] = b;
			const FVoxelData& Vox = Other[V[0] + V[1] * CS + V[2] * CS * CS];
			if (d == 0) { (*Plane)[a + b * CS] = Vox; }
			else { (*Deep)[(d - 1) * SliceSize + a + b * CS] = Vox; }
		}
	}
}

} // namespace MCSeamTest

// ============================ MCS0: interior subset ============================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCSeamMCS0Test, "VoxelWorlds.Meshing.MarchingCubes.SeamClosure.MCS0_InteriorSubset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMCSeamMCS0Test::RunTest(const FString& Parameters)
{
	using namespace MCSeamTest;
	auto Voxels = MakeChunkVoxels(FIntVector::ZeroValue);

	FVoxelMeshingRequest Full = MakeInteriorRequest(FIntVector::ZeroValue, 0, *Voxels);
	Full.MeshCellDomain = EVoxelMeshCellDomain::Full;
	FVoxelMeshingRequest Interior = MakeInteriorRequest(FIntVector::ZeroValue, 0, *Voxels);

	FChunkMeshData FullMesh, InteriorMesh;
	TestTrue(TEXT("full meshes"), Mesher().GenerateMeshCPU(Full, FullMesh));
	TestTrue(TEXT("interior meshes"), Mesher().GenerateMeshCPU(Interior, InteriorMesh));
	TestTrue(TEXT("interior nonempty"), InteriorMesh.Positions.Num() > 0);
	TestTrue(TEXT("interior smaller"), InteriorMesh.Positions.Num() < FullMesh.Positions.Num());

	// Every interior triangle must appear bit-identically in the full mesh (per-cell output is
	// independent, so the interior pass is an exact subset of the full pass).
	TSet<FString> FullTris;
	auto TriKey = [](const FChunkMeshData& M, int32 T) {
		FString K;
		for (int32 i = 0; i < 3; ++i)
		{
			const FVector3f& P = M.Positions[M.Indices[T * 3 + i]];
			K += FString::Printf(TEXT("%.3f,%.3f,%.3f;"), P.X, P.Y, P.Z);
		}
		return K;
	};
	for (int32 T = 0; T < FullMesh.Indices.Num() / 3; ++T) { FullTris.Add(TriKey(FullMesh, T)); }
	int32 Missing = 0;
	for (int32 T = 0; T < InteriorMesh.Indices.Num() / 3; ++T)
	{
		if (!FullTris.Contains(TriKey(InteriorMesh, T))) { ++Missing; }
	}
	TestEqual(TEXT("interior triangles all present in full mesh"), Missing, 0);
	return true;
}

// ============================ MCS1: same-LOD face closure ============================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCSeamMCS1Test, "VoxelWorlds.Meshing.MarchingCubes.SeamClosure.MCS1_FaceClosure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMCSeamMCS1Test::RunTest(const FString& Parameters)
{
	using namespace MCSeamTest;
	const FIntVector CoordA(0, 0, 0), CoordB(1, 0, 0);
	auto VoxA = MakeChunkVoxels(CoordA);
	auto VoxB = MakeChunkVoxels(CoordB);

	FChunkMeshData MeshA, MeshB, Seam;
	Mesher().GenerateMeshCPU(MakeInteriorRequest(CoordA, 0, *VoxA), MeshA);
	Mesher().GenerateMeshCPU(MakeInteriorRequest(CoordB, 0, *VoxB), MeshB);

	FVoxelFaceSeamRequest FaceReq;
	FaceReq.OwnerChunkCoord = CoordA;
	FaceReq.Axis = 0;
	FaceReq.LODLevel = 0;
	FaceReq.ChunkSize = CS;
	FaceReq.VoxelSize = VoxSize;
	FaceReq.VoxelDataA = VoxA;
	FaceReq.VoxelDataB = VoxB;
	TestTrue(TEXT("face seam meshes"), Mesher().GenerateFaceSeamMeshCPU(FaceReq, Seam));
	TestTrue(TEXT("face seam nonempty"), Seam.Positions.Num() > 0);

	FChunkMeshData All;
	Append(All, MeshA, FVector3f::ZeroVector);
	Append(All, MeshB, FVector3f(CS * VoxSize, 0, 0));
	Append(All, Seam, FVector3f::ZeroVector); // seam is already owner(A)-local

	// Junction slab window: around the shared face, away from the pair's outer bounds and the
	// unmeshed perpendicular seam bands (those belong to absent edge/corner jobs).
	const int32 Open = CountOpenEdgesInWindow(All,
		FVector3f((CS - 3) * VoxSize, 3 * VoxSize, 0),
		FVector3f((CS + 3) * VoxSize, (CS - 3) * VoxSize, CS * VoxSize));
	TestEqual(TEXT("no open edges in the junction slab"), Open, 0);
	return true;
}

// ============================ MCS2: 2x2x2 assembly closure ============================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCSeamMCS2Test, "VoxelWorlds.Meshing.MarchingCubes.SeamClosure.MCS2_AssemblyClosure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMCSeamMCS2Test::RunTest(const FString& Parameters)
{
	using namespace MCSeamTest;
	TSharedPtr<const TArray<FVoxelData>> Vox[8];
	FIntVector Coords[8];
	for (int32 o = 0; o < 8; ++o)
	{
		Coords[o] = FIntVector(o & 1, (o >> 1) & 1, (o >> 2) & 1);
		Vox[o] = MakeChunkVoxels(Coords[o]);
	}
	auto OffsetOf = [](const FIntVector& C) {
		return FVector3f(C.X, C.Y, C.Z) * static_cast<float>(CS) * VoxSize;
	};

	FChunkMeshData All;
	for (int32 o = 0; o < 8; ++o)
	{
		FChunkMeshData M;
		Mesher().GenerateMeshCPU(MakeInteriorRequest(Coords[o], 0, *Vox[o]), M);
		Append(All, M, OffsetOf(Coords[o]));
	}

	// 12 internal faces: for each axis, owners are the 4 chunks with coordinate 0 on that axis.
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		for (int32 o = 0; o < 8; ++o)
		{
			if (Coords[o][Axis] != 0) { continue; }
			FIntVector NCoord = Coords[o]; NCoord[Axis] += 1;
			int32 nIdx = NCoord.X + NCoord.Y * 2 + NCoord.Z * 4;
			FVoxelFaceSeamRequest R;
			R.OwnerChunkCoord = Coords[o];
			R.Axis = static_cast<uint8>(Axis);
			R.LODLevel = 0;
			R.ChunkSize = CS;
			R.VoxelSize = VoxSize;
			R.VoxelDataA = Vox[o];
			R.VoxelDataB = Vox[nIdx];
			FChunkMeshData M;
			TestTrue(TEXT("face seam ok"), Mesher().GenerateFaceSeamMeshCPU(R, M));
			Append(All, M, OffsetOf(Coords[o]));
		}
	}
	// 6 internal edges: for each edge axis, owners are the 2 chunks at 0 on both perp axes.
	for (int32 EdgeAxis = 0; EdgeAxis < 3; ++EdgeAxis)
	{
		const int32 P1 = (EdgeAxis == 0) ? 1 : 0;
		const int32 P2 = (EdgeAxis == 2) ? 1 : 2;
		for (int32 along = 0; along < 2; ++along)
		{
			FIntVector Owner = FIntVector::ZeroValue;
			Owner[EdgeAxis] = along;
			FVoxelEdgeSeamRequest R;
			R.OwnerChunkCoord = Owner;
			R.EdgeAxis = static_cast<uint8>(EdgeAxis);
			R.LODLevel = 0;
			R.ChunkSize = CS;
			R.VoxelSize = VoxSize;
			for (int32 q = 0; q < 4; ++q)
			{
				FIntVector C = Owner;
				C[P1] += (q & 1);
				C[P2] += (q >> 1);
				R.VoxelData[q] = Vox[C.X + C.Y * 2 + C.Z * 4];
			}
			FChunkMeshData M;
			TestTrue(TEXT("edge seam ok"), Mesher().GenerateEdgeSeamMeshCPU(R, M));
			Append(All, M, OffsetOf(Owner));
		}
	}
	// 1 internal corner.
	{
		FVoxelCornerSeamRequest R;
		R.OwnerChunkCoord = FIntVector::ZeroValue;
		R.LODLevel = 0;
		R.ChunkSize = CS;
		R.VoxelSize = VoxSize;
		for (int32 o = 0; o < 8; ++o) { R.VoxelData[o] = Vox[o]; }
		FChunkMeshData M;
		TestTrue(TEXT("corner seam ok"), Mesher().GenerateCornerSeamMeshCPU(R, M));
		Append(All, M, FVector3f::ZeroVector);
	}

	// Interior window of the assembly (2 cells inside the outer bounds everywhere).
	const int32 Open = CountOpenEdgesInWindow(All,
		FVector3f(3 * VoxSize, 3 * VoxSize, 0),
		FVector3f((2 * CS - 3) * VoxSize, (2 * CS - 3) * VoxSize, 2 * CS * VoxSize));
	TestEqual(TEXT("no open edges inside the 2x2x2 assembly"), Open, 0);
	return true;
}

// ============================ MCS3: mixed-LOD face parity ============================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCSeamMCS3Test, "VoxelWorlds.Meshing.MarchingCubes.SeamClosure.MCS3_MixedFaceParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMCSeamMCS3Test::RunTest(const FString& Parameters)
{
	using namespace MCSeamTest;
	const FIntVector CoordA(0, 0, 0), CoordB(1, 0, 0);
	auto VoxA = MakeChunkVoxels(CoordA); // fine (LOD0)
	auto VoxB = MakeChunkVoxels(CoordB); // coarse (LOD1)

	// Seam-architecture version: interiors + mixed face seam (ribbon + confined morph).
	FChunkMeshData MeshA, MeshB, Seam, All;
	Mesher().GenerateMeshCPU(MakeInteriorRequest(CoordA, 0, *VoxA), MeshA);
	Mesher().GenerateMeshCPU(MakeInteriorRequest(CoordB, 1, *VoxB), MeshB);
	FVoxelFaceSeamRequest FaceReq;
	FaceReq.OwnerChunkCoord = CoordA;
	FaceReq.Axis = 0;
	FaceReq.LODLevel = 0;
	FaceReq.LODLevelB = 1;
	FaceReq.ChunkSize = CS;
	FaceReq.VoxelSize = VoxSize;
	FaceReq.VoxelDataA = VoxA;
	FaceReq.VoxelDataB = VoxB;
	TestTrue(TEXT("mixed face seam meshes"), Mesher().GenerateFaceSeamMeshCPU(FaceReq, Seam));
	TestTrue(TEXT("mixed face seam nonempty"), Seam.Positions.Num() > 0);
	Append(All, MeshA, FVector3f::ZeroVector);
	Append(All, MeshB, FVector3f(CS * VoxSize, 0, 0));
	Append(All, Seam, FVector3f::ZeroVector);

	// Legacy version: full-domain transvoxel meshes with proper facing slices + LODs.
	FChunkMeshData LegacyA, LegacyB, LegacyAll;
	{
		FVoxelMeshingRequest RA = MakeInteriorRequest(CoordA, 0, *VoxA);
		RA.MeshCellDomain = EVoxelMeshCellDomain::Full;
		RA.NeighborLODLevels[1] = 1; // +X coarser
		RA.TransitionFaces = FVoxelMeshingRequest::TRANSITION_XPOS;
		FillLegacyFacing(RA, 0, true, 5, *VoxB);
		Mesher().GenerateMeshCPU(RA, LegacyA);

		FVoxelMeshingRequest RB = MakeInteriorRequest(CoordB, 1, *VoxB);
		RB.MeshCellDomain = EVoxelMeshCellDomain::Full;
		RB.NeighborLODLevels[0] = 0; // -X finer
		FillLegacyFacing(RB, 0, false, 5, *VoxA);
		Mesher().GenerateMeshCPU(RB, LegacyB);

		Append(LegacyAll, LegacyA, FVector3f::ZeroVector);
		Append(LegacyAll, LegacyB, FVector3f(CS * VoxSize, 0, 0));
	}

	const FVector3f WinMin((CS - 3) * VoxSize, 3 * VoxSize, 0);
	const FVector3f WinMax((CS + 3) * VoxSize, (CS - 3) * VoxSize, CS * VoxSize);
	const int32 OpenSeam = CountOpenEdgesInWindow(All, WinMin, WinMax);
	const int32 OpenLegacy = CountOpenEdgesInWindow(LegacyAll, WinMin, WinMax);
	AddInfo(FString::Printf(TEXT("junction open edges: seam-arch=%d legacy=%d"), OpenSeam, OpenLegacy));
	// Parity bar: no worse than legacy (which carries inherent ribbon T-junctions), with a
	// small absolute allowance for windowing differences.
	TestTrue(TEXT("mixed-LOD junction no worse than legacy"), OpenSeam <= OpenLegacy + 8);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
