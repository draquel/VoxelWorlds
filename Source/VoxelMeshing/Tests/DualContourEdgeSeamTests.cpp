// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelMeshingTypes.h"
#include "ChunkRenderData.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Seam-ownership P2a — single-owner EDGE seams (SEAM_OWNERSHIP_ARCHITECTURE.md
// "P2 plan"). A 2x2 chunk lattice around the shared Z-parallel edge: the four
// Interior meshes + the four face-seam meshes + the edge-seam mesh must close
// the surface across the edge region, and every edge-seam ring vertex must be
// bit-identical to a vertex of the mesh whose job originally owned that cell
// (interior pass or face seam). Pure CPU tests — run headless.
// ---------------------------------------------------------------------------

namespace VoxelEdgeSeamTestUtils
{
	constexpr int32 TestChunkSize = 32;
	constexpr float TestVoxelSize = 100.0f;

	/** Fill a ChunkSize^3 array from a GLOBAL-voxel-coordinate solid predicate, at chunk offset. */
	static TArray<FVoxelData> FillVoxels(const FIntVector& ChunkOffsetVoxels,
		TFunctionRef<bool(int32, int32, int32)> SolidFn)
	{
		TArray<FVoxelData> Data;
		Data.SetNumUninitialized(TestChunkSize * TestChunkSize * TestChunkSize);
		for (int32 Z = 0; Z < TestChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < TestChunkSize; ++Y)
			{
				for (int32 X = 0; X < TestChunkSize; ++X)
				{
					const int32 Index = X + Y * TestChunkSize + Z * TestChunkSize * TestChunkSize;
					const bool bSolid = SolidFn(X + ChunkOffsetVoxels.X, Y + ChunkOffsetVoxels.Y, Z + ChunkOffsetVoxels.Z);
					Data[Index] = bSolid ? FVoxelData::Solid(1, 0) : FVoxelData::Air();
				}
			}
		}
		return Data;
	}

	/** FillVoxels wrapped as the shared immutable snapshot the seam requests carry. */
	static TSharedPtr<const TArray<FVoxelData>> SharedVoxels(const FIntVector& ChunkOffsetVoxels,
		TFunctionRef<bool(int32, int32, int32)> SolidFn)
	{
		return MakeShared<TArray<FVoxelData>>(FillVoxels(ChunkOffsetVoxels, SolidFn));
	}

	static FVoxelMeshingConfig MakeConfig()
	{
		FVoxelMeshingConfig Config;
		Config.bUseSmoothMeshing = true;
		Config.IsoLevel = 0.5f;
		Config.bGenerateUVs = true;
		Config.bCalculateAO = false;
		Config.bGenerateSkirts = false;
		Config.QEFSVDThreshold = 0.1f;
		return Config;
	}

	static bool MeshInterior(const FIntVector& ChunkOffsetVoxels, int32 LOD,
		TFunctionRef<bool(int32, int32, int32)> SolidFn, FChunkMeshData& Out)
	{
		FVoxelMeshingRequest Request;
		Request.LODLevel = LOD;
		Request.ChunkSize = TestChunkSize;
		Request.VoxelSize = TestVoxelSize;
		Request.MeshCellDomain = EVoxelMeshCellDomain::Interior;
		Request.VoxelData = FillVoxels(ChunkOffsetVoxels, SolidFn);

		FVoxelCPUDualContourMesher Mesher;
		Mesher.Initialize();
		Mesher.SetConfig(MakeConfig());
		const bool bOk = Mesher.GenerateMeshCPU(Request, Out);
		Mesher.Shutdown();
		return bOk;
	}

	/** Face seam between OwnerOffset and OwnerOffset + ChunkSize along Axis. */
	static bool MeshFaceSeam(const FIntVector& OwnerOffsetVoxels, uint8 Axis, int32 LOD,
		TFunctionRef<bool(int32, int32, int32)> SolidFn, FChunkMeshData& Out)
	{
		FIntVector NeighborOffset = OwnerOffsetVoxels;
		NeighborOffset[Axis] += TestChunkSize;

		FVoxelFaceSeamRequest Seam;
		Seam.Axis = Axis;
		Seam.LODLevel = LOD;
		Seam.ChunkSize = TestChunkSize;
		Seam.VoxelSize = TestVoxelSize;
		Seam.VoxelDataA = SharedVoxels(OwnerOffsetVoxels, SolidFn);
		Seam.VoxelDataB = SharedVoxels(NeighborOffset, SolidFn);

		FVoxelCPUDualContourMesher Mesher;
		Mesher.Initialize();
		Mesher.SetConfig(MakeConfig());
		const bool bOk = Mesher.GenerateFaceSeamMeshCPU(Seam, Out);
		Mesher.Shutdown();
		return bOk;
	}

	/** Edge seam of the Z-parallel edge shared by chunks at (0,0,0),(1,0,0),(0,1,0),(1,1,0). */
	static bool MeshEdgeSeamZ(int32 LOD, TFunctionRef<bool(int32, int32, int32)> SolidFn, FChunkMeshData& Out)
	{
		FVoxelEdgeSeamRequest Seam;
		Seam.EdgeAxis = 2; // parallel to Z; PerpA = X, PerpB = Y (ascending)
		Seam.LODLevel = LOD;
		Seam.ChunkSize = TestChunkSize;
		Seam.VoxelSize = TestVoxelSize;
		Seam.VoxelData[0] = SharedVoxels(FIntVector(0, 0, 0), SolidFn);                            // (0,0) owner
		Seam.VoxelData[1] = SharedVoxels(FIntVector(TestChunkSize, 0, 0), SolidFn);                // (1,0) +X
		Seam.VoxelData[2] = SharedVoxels(FIntVector(0, TestChunkSize, 0), SolidFn);                // (0,1) +Y
		Seam.VoxelData[3] = SharedVoxels(FIntVector(TestChunkSize, TestChunkSize, 0), SolidFn);    // (1,1)

		FVoxelCPUDualContourMesher Mesher;
		Mesher.Initialize();
		Mesher.SetConfig(MakeConfig());
		const bool bOk = Mesher.GenerateEdgeSeamMeshCPU(Seam, Out);
		Mesher.Shutdown();
		return bOk;
	}

	static FString PosKey(const FVector3f& P)
	{
		return BytesToHex(reinterpret_cast<const uint8*>(&P), sizeof(FVector3f));
	}

	static void AccumulateEdges(const FChunkMeshData& M, const FVector3f& Offset, TMap<FString, int32>& EdgeUse)
	{
		const int32 TriCount = M.Indices.Num() / 3;
		for (int32 T = 0; T < TriCount; ++T)
		{
			FVector3f P[3];
			for (int32 i = 0; i < 3; ++i)
			{
				P[i] = M.Positions[M.Indices[T * 3 + i]] + Offset;
			}
			for (int32 i = 0; i < 3; ++i)
			{
				const FString KA = PosKey(P[i]);
				const FString KB = PosKey(P[(i + 1) % 3]);
				if (KA == KB)
				{
					continue;
				}
				const FString Key = (KA < KB) ? (KA + KB) : (KB + KA);
				EdgeUse.FindOrAdd(Key)++;
			}
		}
	}

	/** Flat ground: crosses the vertical (Z-parallel) chunk edge everywhere. */
	static bool FlatGround(int32 X, int32 Y, int32 Z)
	{
		return Z <= 12;
	}

	/** Sloped ground varying along BOTH perpendicular axes so the four quadrants differ. */
	static bool SlopedGround(int32 X, int32 Y, int32 Z)
	{
		return Z <= 8 + (X * 4) / (2 * TestChunkSize) + (Y * 6) / (2 * TestChunkSize);
	}

	/** The nine meshes of the 2x2 lattice scene, with their combine offsets (owner frames -> world). */
	struct FLatticeScene
	{
		FChunkMeshData Interior[4];   // A, B(+X), C(+Y), D(+X+Y)
		FChunkMeshData FaceSeam[4];   // A|B (+X of A), C|D (+X of C), A|C (+Y of A), B|D (+Y of B)
		FChunkMeshData EdgeSeam;
		FVector3f InteriorOffset[4];
		FVector3f FaceOffset[4];
	};

	static bool BuildScene(int32 LOD, TFunctionRef<bool(int32, int32, int32)> SolidFn, FLatticeScene& Out, FAutomationTestBase& Test)
	{
		const float Span = static_cast<float>(TestChunkSize) * TestVoxelSize;
		const FIntVector ChunkOffsets[4] = {
			FIntVector(0, 0, 0), FIntVector(TestChunkSize, 0, 0),
			FIntVector(0, TestChunkSize, 0), FIntVector(TestChunkSize, TestChunkSize, 0)
		};
		const FVector3f WorldOffsets[4] = {
			FVector3f(0, 0, 0), FVector3f(Span, 0, 0), FVector3f(0, Span, 0), FVector3f(Span, Span, 0)
		};

		for (int32 i = 0; i < 4; ++i)
		{
			if (!MeshInterior(ChunkOffsets[i], LOD, SolidFn, Out.Interior[i]))
			{
				Test.AddError(TEXT("interior mesh failed"));
				return false;
			}
			Out.InteriorOffset[i] = WorldOffsets[i];
		}

		// Face seams: A|B (owner A, +X), C|D (owner C, +X), A|C (owner A, +Y), B|D (owner B, +Y).
		const FIntVector FaceOwners[4] = { ChunkOffsets[0], ChunkOffsets[2], ChunkOffsets[0], ChunkOffsets[1] };
		const uint8 FaceAxes[4] = { 0, 0, 1, 1 };
		const FVector3f FaceWorld[4] = { WorldOffsets[0], WorldOffsets[2], WorldOffsets[0], WorldOffsets[1] };
		for (int32 i = 0; i < 4; ++i)
		{
			if (!MeshFaceSeam(FaceOwners[i], FaceAxes[i], LOD, SolidFn, Out.FaceSeam[i]))
			{
				Test.AddError(TEXT("face seam failed"));
				return false;
			}
			Out.FaceOffset[i] = FaceWorld[i];
		}

		if (!MeshEdgeSeamZ(LOD, SolidFn, Out.EdgeSeam))
		{
			Test.AddError(TEXT("edge seam failed"));
			return false;
		}
		return true;
	}
}

// ===========================================================================
// ES1: the nine meshes close the surface across the edge region (no open
//      edges in the probe around the shared edge line), flat + sloped,
//      LOD0 and same-LOD LOD1.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCEdgeSeamClosureTest,
	"VoxelWorlds.Meshing.DualContour.SeamClosure.ES1_EdgeClosure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCEdgeSeamClosureTest::RunTest(const FString& Parameters)
{
	using namespace VoxelEdgeSeamTestUtils;

	struct FCase { const TCHAR* Name; bool (*Fn)(int32, int32, int32); int32 LOD; };
	const FCase Cases[] = {
		{ TEXT("FlatGround LOD0"),   &FlatGround,   0 },
		{ TEXT("FlatGround LOD1"),   &FlatGround,   1 },
		{ TEXT("SlopedGround LOD0"), &SlopedGround, 0 },
		{ TEXT("SlopedGround LOD1"), &SlopedGround, 1 },
	};

	for (const FCase& Case : Cases)
	{
		const int32 Stride = 1 << Case.LOD;
		const int32 GridSize = TestChunkSize / Stride;
		const int32 SL = GridSize - 1;
		const float S = static_cast<float>(Stride) * TestVoxelSize;
		const float EdgeXY = static_cast<float>(TestChunkSize) * TestVoxelSize;

		FLatticeScene Scene;
		if (!BuildScene(Case.LOD, Case.Fn, Scene, *this))
		{
			return true;
		}
		TestTrue(FString::Printf(TEXT("%s: edge seam produced geometry"), Case.Name),
			Scene.EdgeSeam.GetTriangleCount() > 0);

		TMap<FString, int32> EdgeUse;
		for (int32 i = 0; i < 4; ++i)
		{
			AccumulateEdges(Scene.Interior[i], Scene.InteriorOffset[i], EdgeUse);
			AccumulateEdges(Scene.FaceSeam[i], Scene.FaceOffset[i], EdgeUse);
		}
		AccumulateEdges(Scene.EdgeSeam, FVector3f::ZeroVector, EdgeUse);

		// Probe: a box around the shared edge line, well inside along Z (the column's Z-rim is
		// P2b corner-seam territory; the face seams' own transverse rims lie along OTHER chunk
		// edges, outside this probe).
		const float ProbeMin = EdgeXY - 2.5f * S;
		const float ProbeMax = EdgeXY + 2.5f * S;
		const float ProbeMinZ = 2.0f * S;
		const float ProbeMaxZ = static_cast<float>(SL - 1) * S;

		int32 OpenInProbe = 0;
		auto CheckMesh = [&](const FChunkMeshData& M, const FVector3f& Offset)
		{
			const int32 TriCount = M.Indices.Num() / 3;
			for (int32 T = 0; T < TriCount; ++T)
			{
				FVector3f P[3];
				for (int32 i = 0; i < 3; ++i)
				{
					P[i] = M.Positions[M.Indices[T * 3 + i]] + Offset;
				}
				for (int32 i = 0; i < 3; ++i)
				{
					const FVector3f& A = P[i];
					const FVector3f& B = P[(i + 1) % 3];
					const FString KA = PosKey(A);
					const FString KB = PosKey(B);
					if (KA == KB)
					{
						continue;
					}
					const FString Key = (KA < KB) ? (KA + KB) : (KB + KA);
					if (EdgeUse.FindChecked(Key) != 1)
					{
						continue;
					}
					const bool bInProbe =
						A.X >= ProbeMin && A.X <= ProbeMax && B.X >= ProbeMin && B.X <= ProbeMax &&
						A.Y >= ProbeMin && A.Y <= ProbeMax && B.Y >= ProbeMin && B.Y <= ProbeMax &&
						A.Z >= ProbeMinZ && A.Z <= ProbeMaxZ && B.Z >= ProbeMinZ && B.Z <= ProbeMaxZ;
					if (bInProbe)
					{
						++OpenInProbe;
						if (OpenInProbe <= 5)
						{
							AddError(FString::Printf(TEXT("%s: OPEN edge in edge-region probe: (%f,%f,%f)-(%f,%f,%f)"),
								Case.Name, A.X, A.Y, A.Z, B.X, B.Y, B.Z));
						}
					}
				}
			}
		};
		for (int32 i = 0; i < 4; ++i)
		{
			CheckMesh(Scene.Interior[i], Scene.InteriorOffset[i]);
			CheckMesh(Scene.FaceSeam[i], Scene.FaceOffset[i]);
		}
		CheckMesh(Scene.EdgeSeam, FVector3f::ZeroVector);

		TestEqual(FString::Printf(TEXT("%s: open edges in the edge-region probe"), Case.Name), OpenInProbe, 0);
	}
	return true;
}

// ===========================================================================
// ES2: every edge-seam RING vertex is bit-identical to a vertex of the mesh
//      whose job originally owned that cell (an interior or a face seam).
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCEdgeSeamRingMatchTest,
	"VoxelWorlds.Meshing.DualContour.SeamClosure.ES2_EdgeRingBitMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCEdgeSeamRingMatchTest::RunTest(const FString& Parameters)
{
	using namespace VoxelEdgeSeamTestUtils;
	constexpr int32 LOD = 0;
	constexpr int32 Stride = 1 << LOD;
	const float S = static_cast<float>(Stride) * TestVoxelSize;
	const float EdgeXY = static_cast<float>(TestChunkSize) * TestVoxelSize;

	FLatticeScene Scene;
	if (!BuildScene(LOD, &FlatGround, Scene, *this))
	{
		return true;
	}
	TestTrue(TEXT("edge seam produced geometry"), Scene.EdgeSeam.GetTriangleCount() > 0);

	// Union of every other mesh's vertex positions (world frame).
	TSet<FString> OtherPositions;
	for (int32 i = 0; i < 4; ++i)
	{
		for (const FVector3f& P : Scene.Interior[i].Positions)
		{
			OtherPositions.Add(PosKey(P + Scene.InteriorOffset[i]));
		}
		for (const FVector3f& P : Scene.FaceSeam[i].Positions)
		{
			OtherPositions.Add(PosKey(P + Scene.FaceOffset[i]));
		}
	}

	// Edge-column band: X and Y both within the shared cell [EdgeXY-S, EdgeXY] (QEF clamps to
	// cell bounds; flat ground puts vertices mid-cell, far from band boundaries).
	constexpr float Margin = 0.5f;
	int32 RingCount = 0, ColumnCount = 0;
	for (const FVector3f& P : Scene.EdgeSeam.Positions)
	{
		const bool bInColumn =
			P.X > EdgeXY - S - Margin && P.X < EdgeXY + Margin &&
			P.Y > EdgeXY - S - Margin && P.Y < EdgeXY + Margin;
		if (bInColumn)
		{
			++ColumnCount;
			continue; // the edge column itself is the seam's own geometry
		}
		++RingCount;
		if (!OtherPositions.Contains(PosKey(P)))
		{
			AddError(FString::Printf(TEXT("edge-seam ring vertex (%f,%f,%f) not bit-identical to any interior/face-seam vertex"),
				P.X, P.Y, P.Z));
			return true;
		}
	}
	TestTrue(TEXT("edge seam references ring vertices"), RingCount > 0);
	TestTrue(TEXT("edge seam has column vertices of its own"), ColumnCount > 0);
	return true;
}

// ===========================================================================
// ES3: no surface at the edge -> valid empty seam.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCEdgeSeamEmptyTest,
	"VoxelWorlds.Meshing.DualContour.SeamClosure.ES3_NoSurfaceEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCEdgeSeamEmptyTest::RunTest(const FString& Parameters)
{
	using namespace VoxelEdgeSeamTestUtils;

	FChunkMeshData SeamAir, SeamSolid;
	TestTrue(TEXT("all-air edge seam meshed"),
		MeshEdgeSeamZ(0, [](int32, int32, int32) { return false; }, SeamAir));
	TestEqual(TEXT("all-air edge seam is empty"), SeamAir.GetTriangleCount(), 0);

	TestTrue(TEXT("all-solid edge seam meshed"),
		MeshEdgeSeamZ(0, [](int32, int32, int32) { return true; }, SeamSolid));
	TestEqual(TEXT("all-solid edge seam is empty"), SeamSolid.GetTriangleCount(), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
