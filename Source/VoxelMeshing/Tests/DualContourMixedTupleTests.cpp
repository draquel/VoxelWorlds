// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelMeshingTypes.h"
#include "ChunkRenderData.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Seam-ownership P2d — MIXED-LOD edge and corner seams. A 2x2x2 lattice with
// heterogeneous per-chunk LODs builds ALL 27 meshes (interiors, face seams —
// same and mixed —, edge seams — same and mixed —, and the mixed corner
// seam) and asserts closure around a mixed edge line and around the corner.
// This is the full composition across resolution changes. Pure CPU tests.
// ---------------------------------------------------------------------------

namespace VoxelMixedTupleTestUtils
{
	constexpr int32 TestChunkSize = 32;
	constexpr float TestVoxelSize = 100.0f;

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

	/** All 27 meshes of a heterogeneous-LOD 2x2x2 lattice, with world combine offsets. */
	struct FHeteroScene
	{
		TArray<FChunkMeshData> Meshes;   // interiors[0..7], faces[8..19], edges[20..25], corner[26]
		TArray<FVector3f> Offsets;
		FChunkMeshData* Corner = nullptr;
		FChunkMeshData* EdgeSeam[6] = {}; // [axis*2 + half]
	};

	/** LODOf(i,j,k) gives each lattice chunk's LOD; VoxelFn(x,y,z) the voxel at a lattice voxel coordinate. */
	static bool BuildHeteroLatticeFrom(TFunctionRef<int32(int32, int32, int32)> LODOf,
		TFunctionRef<FVoxelData(int32, int32, int32)> VoxelFn, FHeteroScene& Out, FAutomationTestBase& Test)
	{
		const float Span = static_cast<float>(TestChunkSize) * TestVoxelSize;
		auto ChunkWorld = [Span](int32 i, int32 j, int32 k) { return FVector3f(i * Span, j * Span, k * Span); };

		// The 8 chunks' voxels, filled once and shared by every request.
		TSharedPtr<const TArray<FVoxelData>> Chunks[8];
		for (int32 c = 0; c < 8; ++c)
		{
			const FIntVector Offset = FIntVector(c & 1, (c >> 1) & 1, (c >> 2) & 1) * TestChunkSize;
			TSharedPtr<TArray<FVoxelData>> Data = MakeShared<TArray<FVoxelData>>();
			Data->SetNumUninitialized(TestChunkSize * TestChunkSize * TestChunkSize);
			for (int32 Z = 0; Z < TestChunkSize; ++Z)
			{
				for (int32 Y = 0; Y < TestChunkSize; ++Y)
				{
					for (int32 X = 0; X < TestChunkSize; ++X)
					{
						(*Data)[X + Y * TestChunkSize + Z * TestChunkSize * TestChunkSize] = VoxelFn(X + Offset.X, Y + Offset.Y, Z + Offset.Z);
					}
				}
			}
			Chunks[c] = Data;
		}
		auto ChunkVoxels = [&Chunks](int32 i, int32 j, int32 k) { return Chunks[i + j * 2 + k * 4]; };

		Out.Meshes.SetNum(27);
		Out.Offsets.SetNum(27);
		int32 MeshIdx = 0;

		// 8 interiors at their own LODs.
		for (int32 k = 0; k <= 1; ++k)
		{
			for (int32 j = 0; j <= 1; ++j)
			{
				for (int32 i = 0; i <= 1; ++i)
				{
					FVoxelMeshingRequest Request;
					Request.LODLevel = LODOf(i, j, k);
					Request.ChunkSize = TestChunkSize;
					Request.VoxelSize = TestVoxelSize;
					Request.MeshCellDomain = EVoxelMeshCellDomain::Interior;
					Request.VoxelData = *ChunkVoxels(i, j, k);
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

		// 12 face seams (owner = the low chunk of each pair; LODs per side — mixed pairs take
		// the P2c path automatically).
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
					int32 NJK[3] = { IJK[0], IJK[1], IJK[2] };
					NJK[Axis] = 1;

					FVoxelFaceSeamRequest Seam;
					Seam.Axis = Axis;
					Seam.LODLevel = LODOf(IJK[0], IJK[1], IJK[2]);
					Seam.LODLevelB = LODOf(NJK[0], NJK[1], NJK[2]);
					Seam.ChunkSize = TestChunkSize;
					Seam.VoxelSize = TestVoxelSize;
					Seam.VoxelDataA = ChunkVoxels(IJK[0], IJK[1], IJK[2]);
					Seam.VoxelDataB = ChunkVoxels(NJK[0], NJK[1], NJK[2]);
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

		// 6 edge seams (per parallel axis U, halves along U; mixed tuples take the P2d path).
		for (uint8 U = 0; U < 3; ++U)
		{
			const int32 PerpA = (U == 0) ? 1 : 0;
			const int32 PerpB = (U == 2) ? 1 : 2;
			for (int32 iu = 0; iu <= 1; ++iu)
			{
				FVoxelEdgeSeamRequest Seam;
				Seam.EdgeAxis = U;
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
						const int32 Q = dv + dw * 2;
						Seam.LODLevels[Q] = LODOf(IJK[0], IJK[1], IJK[2]);
						Seam.VoxelData[Q] = ChunkVoxels(IJK[0], IJK[1], IJK[2]);
					}
				}
				Seam.LODLevel = Seam.LODLevels[0];
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
				Out.EdgeSeam[U * 2 + iu] = &Mesh;
				++MeshIdx;
			}
		}

		// The corner seam (mixed tuples take the P2d path).
		{
			FVoxelCornerSeamRequest Seam;
			Seam.ChunkSize = TestChunkSize;
			Seam.VoxelSize = TestVoxelSize;
			for (int32 k = 0; k <= 1; ++k)
			{
				for (int32 j = 0; j <= 1; ++j)
				{
					for (int32 i = 0; i <= 1; ++i)
					{
						const int32 O = i + j * 2 + k * 4;
						Seam.LODLevels[O] = LODOf(i, j, k);
						Seam.VoxelData[O] = ChunkVoxels(i, j, k);
					}
				}
			}
			Seam.LODLevel = Seam.LODLevels[0];
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

	/** BuildHeteroLatticeFrom over a binary solid. */
	static bool BuildHeteroLattice(TFunctionRef<int32(int32, int32, int32)> LODOf,
		TFunctionRef<bool(int32, int32, int32)> SolidFn, FHeteroScene& Out, FAutomationTestBase& Test)
	{
		return BuildHeteroLatticeFrom(LODOf,
			[&SolidFn](int32 X, int32 Y, int32 Z) { return SolidFn(X, Y, Z) ? FVoxelData::Solid(1, 0) : FVoxelData::Air(); },
			Out, Test);
	}

	/** Count open (use-count 1) edges with both endpoints inside a box, over all meshes. */
	static int32 CountOpenEdgesInBox(const FHeteroScene& Scene, const FVector3f& BoxMin, const FVector3f& BoxMax,
		FAutomationTestBase& Test, const TCHAR* Label, int32 MaxReported = 5)
	{
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

		int32 OpenInBox = 0;
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
					const bool bInBox =
						A.X >= BoxMin.X && A.X <= BoxMax.X && B.X >= BoxMin.X && B.X <= BoxMax.X &&
						A.Y >= BoxMin.Y && A.Y <= BoxMax.Y && B.Y >= BoxMin.Y && B.Y <= BoxMax.Y &&
						A.Z >= BoxMin.Z && A.Z <= BoxMax.Z && B.Z >= BoxMin.Z && B.Z <= BoxMax.Z;
					if (bInBox)
					{
						++OpenInBox;
						if (OpenInBox <= MaxReported)
						{
							Test.AddError(FString::Printf(TEXT("%s: OPEN edge: (%f,%f,%f)-(%f,%f,%f)"),
								Label, A.X, A.Y, A.Z, B.X, B.Y, B.Z));
						}
					}
				}
			}
		}
		return OpenInBox;
	}

	/** Low slanted ground: crosses the vertical edge lines around z ~ 2000 (away from the corner). */
	static bool SlantedLow(int32 X, int32 Y, int32 Z)
	{
		return Z <= 8 + ((X + Y) * 12) / (2 * TestChunkSize);
	}

	/** Corner-crossing slant (passes through the shared corner cell, voxels 31-32). */
	static bool SlantedCorner(int32 X, int32 Y, int32 Z)
	{
		return Z <= 16 + (X * 20 + Y * 10) / (2 * TestChunkSize);
	}

	/**
	 * Noise terrain as the generators write it (G17; the same field as MS4's): a rolling heightfield with small 3D
	 * bumps, the signed distance in voxels mapped to density like SignedDistanceToDensity. The surface sits near
	 * z = 32, the lattice's horizontal faces, edges and corner.
	 */
	static FVoxelData NoiseVoxel(const FVector& Seed, int32 X, int32 Y, int32 Z)
	{
		const FVector P = FVector(X, Y, Z) + Seed;
		const float Hills = 6.0f * FMath::PerlinNoise2D(FVector2D(P.X, P.Y) / 23.0f)
			+ 2.5f * FMath::PerlinNoise2D(FVector2D(P.X, P.Y) / 9.0f + FVector2D(17.0, 3.0));
		const float Bumps = 1.6f * FMath::PerlinNoise3D(P / 5.0f + FVector(5.0, 11.0, 2.0));
		const float Clamped = FMath::Clamp(static_cast<float>(TestChunkSize) + Hills + Bumps - static_cast<float>(Z), -1.0f, 1.0f);
		const uint8 Density = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(127.5f + 127.5f * Clamped), 0, 255));
		FVoxelData Data = (Density > 0) ? FVoxelData::Solid(1, 0) : FVoxelData::Air();
		Data.Density = Density;
		return Data;
	}

	/** Y-heavy slant: height ~31-32 along the whole X-parallel edge line at y=32 (z=32),
	 *  so the surface crosses that line across its full length (SlantedCorner only grazes
	 *  it at the far end, leaving the lower X-edge seam legitimately empty). */
	static bool SlantedYHeavy(int32 X, int32 Y, int32 Z)
	{
		return Z <= 26 + (Y * 10) / (2 * TestChunkSize) + (X * 2) / (2 * TestChunkSize);
	}
}

// ===========================================================================
// MT1: heterogeneous lattice (X-split: LOD0 | LOD1) — closure around a MIXED
//      Z-edge line (tuple LODs [0,1,0,1]) and around the MIXED corner.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCMixedTupleClosureTest,
	"VoxelWorlds.Meshing.DualContour.SeamClosure.MT1_MixedTupleClosure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCMixedTupleClosureTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMixedTupleTestUtils;

	const float Span = static_cast<float>(TestChunkSize) * TestVoxelSize;
	const float SMax = 2.0f * TestVoxelSize; // coarsest stride in the scenes (LOD1)

	auto XSplit = [](int32 i, int32 j, int32 k) { return (i == 1) ? 1 : 0; };
	auto ZSplit = [](int32 i, int32 j, int32 k) { return (k == 1) ? 1 : 0; };

	// Case 1: X-split lattice, low slant -> probe the lower MIXED Z-edge line.
	{
		FHeteroScene Scene;
		if (!BuildHeteroLattice(XSplit, &SlantedLow, Scene, *this))
		{
			return true;
		}
		TestTrue(TEXT("X-split: lower Z-edge seam (mixed [0,1,0,1]) produced geometry"),
			Scene.EdgeSeam[2 * 2 + 0]->GetTriangleCount() > 0);

		const FVector3f BoxMin(Span - 2.5f * SMax, Span - 2.5f * SMax, 2.0f * SMax);
		const FVector3f BoxMax(Span + 2.5f * SMax, Span + 2.5f * SMax, Span - 2.0f * SMax);
		const int32 Open = CountOpenEdgesInBox(Scene, BoxMin, BoxMax, *this, TEXT("X-split mixed Z-edge probe"));
		TestEqual(TEXT("X-split: open edges around the mixed Z-edge line"), Open, 0);
	}

	// Case 2: X-split lattice, corner-crossing slant -> probe the MIXED corner point.
	{
		FHeteroScene Scene;
		if (!BuildHeteroLattice(XSplit, &SlantedCorner, Scene, *this))
		{
			return true;
		}
		TestTrue(TEXT("X-split: mixed corner seam produced geometry"), Scene.Corner->GetTriangleCount() > 0);

		const FVector3f BoxMin(Span - 2.5f * SMax, Span - 2.5f * SMax, Span - 2.5f * SMax);
		const FVector3f BoxMax(Span + 2.5f * SMax, Span + 2.5f * SMax, Span + 2.5f * SMax);
		const int32 Open = CountOpenEdgesInBox(Scene, BoxMin, BoxMax, *this, TEXT("X-split mixed corner probe"));
		TestEqual(TEXT("X-split: open edges around the mixed corner"), Open, 0);
	}

	// Case 3: Z-split lattice — the X-parallel edge tuples are mixed [0,0,1,1] here; their
	// edge line runs along X at (y,z)=(Span,Span). Use the Y-heavy slant, whose surface
	// crosses that line along its whole length.
	{
		FHeteroScene Scene;
		if (!BuildHeteroLattice(ZSplit, &SlantedYHeavy, Scene, *this))
		{
			return true;
		}
		TestTrue(TEXT("Z-split: lower X-edge seam (mixed [0,0,1,1]) produced geometry"),
			Scene.EdgeSeam[0 * 2 + 0]->GetTriangleCount() > 0);

		const FVector3f BoxMin(2.0f * SMax, Span - 2.5f * SMax, Span - 2.5f * SMax);
		const FVector3f BoxMax(Span - 2.0f * SMax, Span + 2.5f * SMax, Span + 2.5f * SMax);
		const int32 Open = CountOpenEdgesInBox(Scene, BoxMin, BoxMax, *this, TEXT("Z-split mixed X-edge probe"));
		TestEqual(TEXT("Z-split: open edges around the mixed X-edge line"), Open, 0);
	}

	return true;
}

// ===========================================================================
// MT2: every mixed edge-seam and corner-seam vertex outside its own exclusive
//      cells is bit-identical to a vertex of another mesh in the lattice.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCMixedTupleRingMatchTest,
	"VoxelWorlds.Meshing.DualContour.SeamClosure.MT2_MixedTupleRingBitMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCMixedTupleRingMatchTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMixedTupleTestUtils;

	const float Span = static_cast<float>(TestChunkSize) * TestVoxelSize;
	auto XSplit = [](int32 i, int32 j, int32 k) { return (i == 1) ? 1 : 0; };

	FHeteroScene Scene;
	if (!BuildHeteroLattice(XSplit, &SlantedCorner, Scene, *this))
	{
		return true;
	}

	// Union of all mesh vertex positions except the mesh under test.
	auto BuildOthers = [&](const FChunkMeshData* Exclude) -> TSet<FString>
	{
		TSet<FString> Others;
		for (int32 m = 0; m < Scene.Meshes.Num(); ++m)
		{
			if (&Scene.Meshes[m] == Exclude)
			{
				continue;
			}
			for (const FVector3f& P : Scene.Meshes[m].Positions)
			{
				Others.Add(PosKey(P + Scene.Offsets[m]));
			}
		}
		return Others;
	};

	// Mixed lower Z-edge seam: its exclusive column is the owner's (LOD0 -> 100uu wide).
	{
		const FChunkMeshData* Edge = Scene.EdgeSeam[2 * 2 + 0];
		const TSet<FString> Others = BuildOthers(Edge);
		const float SA = 1.0f * TestVoxelSize;
		constexpr float Margin = 0.5f;
		int32 Ring = 0, Own = 0;
		for (const FVector3f& P : Edge->Positions)
		{
			const bool bInColumn =
				P.X > Span - SA - Margin && P.X < Span + Margin &&
				P.Y > Span - SA - Margin && P.Y < Span + Margin;
			if (bInColumn) { ++Own; continue; }
			++Ring;
			if (!Others.Contains(PosKey(P)))
			{
				AddError(FString::Printf(TEXT("mixed edge-seam ring vertex (%f,%f,%f) not bit-identical to any other mesh"),
					P.X, P.Y, P.Z));
				return true;
			}
		}
		TestTrue(TEXT("mixed edge seam has ring vertices"), Ring > 0);
	}

	// Mixed corner seam: exclusive cell = the owner's corner cell (LOD0 -> 100uu cube).
	{
		const TSet<FString> Others = BuildOthers(Scene.Corner);
		const float SA = 1.0f * TestVoxelSize;
		constexpr float Margin = 0.5f;
		int32 Ring = 0;
		for (const FVector3f& P : Scene.Corner->Positions)
		{
			const bool bInCornerCell =
				P.X > Span - SA - Margin && P.X < Span + Margin &&
				P.Y > Span - SA - Margin && P.Y < Span + Margin &&
				P.Z > Span - SA - Margin && P.Z < Span + Margin;
			if (bInCornerCell) { continue; }
			++Ring;
			if (!Others.Contains(PosKey(P)))
			{
				AddError(FString::Printf(TEXT("mixed corner-seam ring vertex (%f,%f,%f) not bit-identical to any other mesh"),
					P.X, P.Y, P.Z));
				return true;
			}
		}
		TestTrue(TEXT("mixed corner seam has ring vertices"), Ring > 0);
	}

	return true;
}

// ===========================================================================
// MT3 (G17): heterogeneous lattices on noise terrain close everywhere inside
//      the lattice: mixed face, edge and corner seams with LOD 0, 1 and 2.
//      All three mixed meshers used to skip a fine crossing facing a coarse
//      cell with no crossing of its own; the boundary fallback vertex they
//      share closes it, and neighbouring jobs solve the same vertex.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCMixedTupleNoiseClosureTest,
	"VoxelWorlds.Meshing.DualContour.SeamClosure.MT3_MixedTupleClosureNoise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCMixedTupleNoiseClosureTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMixedTupleTestUtils;

	const float Span = static_cast<float>(TestChunkSize) * TestVoxelSize;
	const float SMax = 4.0f * TestVoxelSize; // the coarsest stride in the lattices (LOD 2)
	struct FLattice { const TCHAR* Name; int32 (*LODOf)(int32, int32, int32); };
	const FLattice Lattices[] = {
		{ TEXT("X-split 1|2"), [](int32 i, int32 j, int32 k) { return (i == 1) ? 2 : 1; } },
		{ TEXT("Z-split 2|1"), [](int32 i, int32 j, int32 k) { return (k == 1) ? 1 : 2; } },
		{ TEXT("Y-split 0|1"), [](int32 i, int32 j, int32 k) { return (j == 1) ? 1 : 0; } },
		{ TEXT("mixed 0/1/2"), [](int32 i, int32 j, int32 k) { return (i + j + k) % 3; } },
	};
	constexpr int32 Patches = 6;
	for (const FLattice& Lattice : Lattices)
	{
		int32 Open = 0, Holed = 0, Triangles = 0;
		for (int32 Patch = 0; Patch < Patches; ++Patch)
		{
			const FVector Seed(41.0 * Patch + 5.0, 29.0 * Patch + 13.0, 17.0 * Patch);
			FHeteroScene Scene;
			if (!BuildHeteroLatticeFrom(Lattice.LODOf, [&Seed](int32 X, int32 Y, int32 Z) { return NoiseVoxel(Seed, X, Y, Z); }, Scene, *this))
			{
				return true;
			}
			for (int32 m = 8; m < Scene.Meshes.Num(); ++m)
			{
				Triangles += Scene.Meshes[m].GetTriangleCount();
			}
			// Everything inside the lattice, clear of its open outer boundary.
			const FVector3f BoxMin(2.0f * SMax);
			const FVector3f BoxMax(2.0f * Span - 2.0f * SMax);
			const int32 OpenHere = CountOpenEdgesInBox(Scene, BoxMin, BoxMax, *this,
				*FString::Printf(TEXT("%s, patch %d"), Lattice.Name, Patch), Holed < 2 ? 3 : 0);
			Open += OpenHere;
			Holed += OpenHere > 0 ? 1 : 0;
		}
		AddInfo(FString::Printf(TEXT("%s: %d open edges in %d of %d patches, %d seam triangles"), Lattice.Name, Open, Holed, Patches, Triangles));
		TestTrue(*FString::Printf(TEXT("%s: the seams have geometry"), Lattice.Name), Triangles > 0);
		TestEqual(*FString::Printf(TEXT("%s: open edges inside the lattice"), Lattice.Name), Open, 0);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
