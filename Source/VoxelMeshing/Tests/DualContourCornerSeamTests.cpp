// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelMeshingTypes.h"
#include "ChunkRenderData.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Seam-ownership P2b — single-owner CORNER seams. The full composition test:
// a 2x2x2 chunk lattice around the shared corner builds ALL 27 meshes
// (8 interiors + 12 face seams + 6 edge seams + 1 corner seam) and asserts
// complete watertightness around the corner point — the "uniform-LOD regions
// are fully closed" milestone. Pure CPU tests — run headless.
// ---------------------------------------------------------------------------

namespace VoxelCornerSeamTestUtils
{
	constexpr int32 TestChunkSize = 32;
	constexpr float TestVoxelSize = 100.0f;

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

	template <typename TFn>
	static bool RunMesher(TFn&& Fn)
	{
		FVoxelCPUDualContourMesher Mesher;
		Mesher.Initialize();
		Mesher.SetConfig(MakeConfig());
		const bool bOk = Fn(Mesher);
		Mesher.Shutdown();
		return bOk;
	}

	static FString PosKey(const FVector3f& P)
	{
		return BytesToHex(reinterpret_cast<const uint8*>(&P), sizeof(FVector3f));
	}

	/** The 27 meshes of the 2x2x2 lattice, each with its combine offset (owner frame -> world). */
	struct FLatticeScene
	{
		TArray<FChunkMeshData> Meshes;   // interiors[0..7], faces[8..19], edges[20..25], corner[26]
		TArray<FVector3f> Offsets;
		FChunkMeshData* Corner = nullptr;
	};

	static bool BuildScene(int32 LOD, TFunctionRef<bool(int32, int32, int32)> SolidFn, FLatticeScene& Out, FAutomationTestBase& Test)
	{
		const float Span = static_cast<float>(TestChunkSize) * TestVoxelSize;
		auto ChunkVoxelOffset = [](int32 i, int32 j, int32 k) { return FIntVector(i, j, k) * TestChunkSize; };
		auto ChunkWorld = [Span](int32 i, int32 j, int32 k) { return FVector3f(i * Span, j * Span, k * Span); };

		Out.Meshes.SetNum(27);
		Out.Offsets.SetNum(27);
		int32 MeshIdx = 0;

		// 8 interiors (octant order).
		for (int32 k = 0; k <= 1; ++k)
		{
			for (int32 j = 0; j <= 1; ++j)
			{
				for (int32 i = 0; i <= 1; ++i)
				{
					FVoxelMeshingRequest Request;
					Request.LODLevel = LOD;
					Request.ChunkSize = TestChunkSize;
					Request.VoxelSize = TestVoxelSize;
					Request.MeshCellDomain = EVoxelMeshCellDomain::Interior;
					Request.VoxelData = FillVoxels(ChunkVoxelOffset(i, j, k), SolidFn);
					FChunkMeshData& Mesh = Out.Meshes[MeshIdx];
					if (!RunMesher([&](FVoxelCPUDualContourMesher& M) { return M.GenerateMeshCPU(Request, Mesh); }))
					{
						Test.AddError(TEXT("interior mesh failed"));
						return false;
					}
					Out.Offsets[MeshIdx] = ChunkWorld(i, j, k);
					++MeshIdx;
				}
			}
		}

		// 12 face seams: per axis, the 4 owners with coord[axis] == 0.
		for (uint8 Axis = 0; Axis < 3; ++Axis)
		{
			for (int32 b = 0; b <= 1; ++b)
			{
				for (int32 a = 0; a <= 1; ++a)
				{
					int32 IJK[3];
					IJK[Axis] = 0;
					IJK[(Axis + 1) % 3] = a;
					IJK[(Axis + 2) % 3] = b;
					FIntVector NeighborIJK(IJK[0], IJK[1], IJK[2]);
					NeighborIJK[Axis] += 1;

					FVoxelFaceSeamRequest Seam;
					Seam.Axis = Axis;
					Seam.LODLevel = LOD;
					Seam.ChunkSize = TestChunkSize;
					Seam.VoxelSize = TestVoxelSize;
					Seam.VoxelDataA = SharedVoxels(ChunkVoxelOffset(IJK[0], IJK[1], IJK[2]), SolidFn);
					Seam.VoxelDataB = SharedVoxels(FIntVector(NeighborIJK) * TestChunkSize, SolidFn);
					FChunkMeshData& Mesh = Out.Meshes[MeshIdx];
					if (!RunMesher([&](FVoxelCPUDualContourMesher& M) { return M.GenerateFaceSeamMeshCPU(Seam, Mesh); }))
					{
						Test.AddError(TEXT("face seam failed"));
						return false;
					}
					Out.Offsets[MeshIdx] = ChunkWorld(IJK[0], IJK[1], IJK[2]);
					++MeshIdx;
				}
			}
		}

		// 6 edge seams: per parallel axis U, the two tuples at coord[U] == 0 / 1. Quadrant order
		// over the ASCENDING perpendicular axes matches FVoxelEdgeSeamRequest.
		for (uint8 U = 0; U < 3; ++U)
		{
			const int32 PerpA = (U == 0) ? 1 : 0;
			const int32 PerpB = (U == 2) ? 1 : 2;
			for (int32 iu = 0; iu <= 1; ++iu)
			{
				FVoxelEdgeSeamRequest Seam;
				Seam.EdgeAxis = U;
				Seam.LODLevel = LOD;
				Seam.ChunkSize = TestChunkSize;
				Seam.VoxelSize = TestVoxelSize;
				for (int32 dw = 0; dw <= 1; ++dw)
				{
					for (int32 dv = 0; dv <= 1; ++dv)
					{
						int32 IJK[3];
						IJK[U] = iu;
						IJK[PerpA] = dv;
						IJK[PerpB] = dw;
						Seam.VoxelData[dv + dw * 2] = SharedVoxels(ChunkVoxelOffset(IJK[0], IJK[1], IJK[2]), SolidFn);
					}
				}
				int32 OwnerIJK[3];
				OwnerIJK[U] = iu;
				OwnerIJK[PerpA] = 0;
				OwnerIJK[PerpB] = 0;
				FChunkMeshData& Mesh = Out.Meshes[MeshIdx];
				if (!RunMesher([&](FVoxelCPUDualContourMesher& M) { return M.GenerateEdgeSeamMeshCPU(Seam, Mesh); }))
				{
					Test.AddError(TEXT("edge seam failed"));
					return false;
				}
				Out.Offsets[MeshIdx] = ChunkWorld(OwnerIJK[0], OwnerIJK[1], OwnerIJK[2]);
				++MeshIdx;
			}
		}

		// 1 corner seam (octant order; owner at the origin).
		{
			FVoxelCornerSeamRequest Seam;
			Seam.LODLevel = LOD;
			Seam.ChunkSize = TestChunkSize;
			Seam.VoxelSize = TestVoxelSize;
			for (int32 k = 0; k <= 1; ++k)
			{
				for (int32 j = 0; j <= 1; ++j)
				{
					for (int32 i = 0; i <= 1; ++i)
					{
						Seam.VoxelData[i + j * 2 + k * 4] = SharedVoxels(ChunkVoxelOffset(i, j, k), SolidFn);
					}
				}
			}
			FChunkMeshData& Mesh = Out.Meshes[MeshIdx];
			if (!RunMesher([&](FVoxelCPUDualContourMesher& M) { return M.GenerateCornerSeamMeshCPU(Seam, Mesh); }))
			{
				Test.AddError(TEXT("corner seam failed"));
				return false;
			}
			Out.Offsets[MeshIdx] = FVector3f::ZeroVector;
			Out.Corner = &Mesh;
			++MeshIdx;
		}

		return true;
	}

	/** Ground slanted along X+Y so the surface passes through the shared corner point. */
	static bool SlantedGround(int32 X, int32 Y, int32 Z)
	{
		return Z <= 20 + ((X + Y) * 12) / (2 * TestChunkSize);
	}

	/** A second, asymmetric slant. Base 16 puts the height at 30-31 across the corner cell
	 *  (voxels 31-32), so the surface genuinely passes through it (base 18 skimmed above it,
	 *  leaving a legitimately empty corner seam and a vacuous closure probe). */
	static bool SteepGround(int32 X, int32 Y, int32 Z)
	{
		return Z <= 16 + (X * 20 + Y * 10) / (2 * TestChunkSize);
	}
}

// ===========================================================================
// CS1: all 27 meshes together are WATERTIGHT around the shared corner
//      (zero open edges in the corner probe), two slants, LOD0 + LOD1.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCCornerSeamClosureTest,
	"VoxelWorlds.Meshing.DualContour.SeamClosure.CS1_CornerClosure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCCornerSeamClosureTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCornerSeamTestUtils;

	struct FCase { const TCHAR* Name; bool (*Fn)(int32, int32, int32); int32 LOD; };
	const FCase Cases[] = {
		{ TEXT("SlantedGround LOD0"), &SlantedGround, 0 },
		{ TEXT("SlantedGround LOD1"), &SlantedGround, 1 },
		{ TEXT("SteepGround LOD0"),   &SteepGround,   0 },
	};

	for (const FCase& Case : Cases)
	{
		const int32 Stride = 1 << Case.LOD;
		const float S = static_cast<float>(Stride) * TestVoxelSize;
		const float CornerXYZ = static_cast<float>(TestChunkSize) * TestVoxelSize;

		FLatticeScene Scene;
		if (!BuildScene(Case.LOD, Case.Fn, Scene, *this))
		{
			return true;
		}
		TestTrue(FString::Printf(TEXT("%s: corner seam produced geometry"), Case.Name),
			Scene.Corner->GetTriangleCount() > 0);

		TMap<FString, int32> EdgeUse;
		for (int32 m = 0; m < Scene.Meshes.Num(); ++m)
		{
			const FChunkMeshData& M = Scene.Meshes[m];
			const int32 TriCount = M.Indices.Num() / 3;
			for (int32 T = 0; T < TriCount; ++T)
			{
				FVector3f P[3];
				for (int32 i = 0; i < 3; ++i)
				{
					P[i] = M.Positions[M.Indices[T * 3 + i]] + Scene.Offsets[m];
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

		// Probe: a box centered on the shared corner point — the region where interiors, all
		// three face-seam orientations, all three edge-seam orientations, and the corner seam
		// meet. Everything inside must be closed.
		const float ProbeMin = CornerXYZ - 2.5f * S;
		const float ProbeMax = CornerXYZ + 2.5f * S;

		int32 OpenInProbe = 0;
		for (int32 m = 0; m < Scene.Meshes.Num(); ++m)
		{
			const FChunkMeshData& M = Scene.Meshes[m];
			const int32 TriCount = M.Indices.Num() / 3;
			for (int32 T = 0; T < TriCount; ++T)
			{
				FVector3f P[3];
				for (int32 i = 0; i < 3; ++i)
				{
					P[i] = M.Positions[M.Indices[T * 3 + i]] + Scene.Offsets[m];
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
						A.Z >= ProbeMin && A.Z <= ProbeMax && B.Z >= ProbeMin && B.Z <= ProbeMax;
					if (bInProbe)
					{
						++OpenInProbe;
						if (OpenInProbe <= 5)
						{
							AddError(FString::Printf(TEXT("%s: OPEN edge in corner probe: (%f,%f,%f)-(%f,%f,%f)"),
								Case.Name, A.X, A.Y, A.Z, B.X, B.Y, B.Z));
						}
					}
				}
			}
		}

		TestEqual(FString::Printf(TEXT("%s: open edges in the corner probe"), Case.Name), OpenInProbe, 0);
	}
	return true;
}

// ===========================================================================
// CS2: every corner-seam RING vertex is bit-identical to a vertex of the
//      mesh whose job originally owned that cell (interior, face, or edge).
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCCornerSeamRingMatchTest,
	"VoxelWorlds.Meshing.DualContour.SeamClosure.CS2_CornerRingBitMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCCornerSeamRingMatchTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCornerSeamTestUtils;
	constexpr int32 LOD = 0;
	constexpr int32 Stride = 1 << LOD;
	const float S = static_cast<float>(Stride) * TestVoxelSize;
	const float CornerXYZ = static_cast<float>(TestChunkSize) * TestVoxelSize;

	FLatticeScene Scene;
	if (!BuildScene(LOD, &SlantedGround, Scene, *this))
	{
		return true;
	}
	TestTrue(TEXT("corner seam produced geometry"), Scene.Corner->GetTriangleCount() > 0);

	// Union of every non-corner mesh's vertex positions (world frame).
	TSet<FString> OtherPositions;
	for (int32 m = 0; m < Scene.Meshes.Num() - 1; ++m)
	{
		for (const FVector3f& P : Scene.Meshes[m].Positions)
		{
			OtherPositions.Add(PosKey(P + Scene.Offsets[m]));
		}
	}

	// The corner cell band: all three coords within [CornerXYZ - S, CornerXYZ] (+margin).
	constexpr float Margin = 0.5f;
	int32 RingCount = 0, CornerCellCount = 0;
	for (const FVector3f& P : Scene.Corner->Positions)
	{
		const bool bInCornerCell =
			P.X > CornerXYZ - S - Margin && P.X < CornerXYZ + Margin &&
			P.Y > CornerXYZ - S - Margin && P.Y < CornerXYZ + Margin &&
			P.Z > CornerXYZ - S - Margin && P.Z < CornerXYZ + Margin;
		if (bInCornerCell)
		{
			++CornerCellCount;
			continue;
		}
		++RingCount;
		if (!OtherPositions.Contains(PosKey(P)))
		{
			AddError(FString::Printf(TEXT("corner-seam ring vertex (%f,%f,%f) not bit-identical to any other mesh's vertex"),
				P.X, P.Y, P.Z));
			return true;
		}
	}
	TestTrue(TEXT("corner seam references ring vertices"), RingCount > 0);
	TestTrue(TEXT("corner seam has a corner-cell vertex of its own"), CornerCellCount > 0);
	return true;
}

// ===========================================================================
// CS3: no surface at the corner -> valid empty seam.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCCornerSeamEmptyTest,
	"VoxelWorlds.Meshing.DualContour.SeamClosure.CS3_NoSurfaceEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCCornerSeamEmptyTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCornerSeamTestUtils;

	auto MeshCorner = [](TFunctionRef<bool(int32, int32, int32)> SolidFn, FChunkMeshData& Out) -> bool
	{
		FVoxelCornerSeamRequest Seam;
		Seam.LODLevel = 0;
		Seam.ChunkSize = TestChunkSize;
		Seam.VoxelSize = TestVoxelSize;
		for (int32 k = 0; k <= 1; ++k)
		{
			for (int32 j = 0; j <= 1; ++j)
			{
				for (int32 i = 0; i <= 1; ++i)
				{
					Seam.VoxelData[i + j * 2 + k * 4] = SharedVoxels(FIntVector(i, j, k) * TestChunkSize, SolidFn);
				}
			}
		}
		return RunMesher([&](FVoxelCPUDualContourMesher& M) { return M.GenerateCornerSeamMeshCPU(Seam, Out); });
	};

	FChunkMeshData SeamAir, SeamSolid;
	TestTrue(TEXT("all-air corner seam meshed"), MeshCorner([](int32, int32, int32) { return false; }, SeamAir));
	TestEqual(TEXT("all-air corner seam is empty"), SeamAir.GetTriangleCount(), 0);

	TestTrue(TEXT("all-solid corner seam meshed"), MeshCorner([](int32, int32, int32) { return true; }, SeamSolid));
	TestEqual(TEXT("all-solid corner seam is empty"), SeamSolid.GetTriangleCount(), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
