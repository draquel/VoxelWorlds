// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelMeshingTestFixtures.h"
#include "VoxelMeshingTypes.h"
#include "ChunkRenderData.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Seam-ownership P2c — MIXED-LOD face seams (the §2.3 cross-resolution
// mesher). Interior A (stride SA) + interior B (stride SB) + the mixed seam
// must close the face across the resolution change: T-junction fans stitch
// fine crossings onto coarse ring vertices, and each ring is bit-identical
// to its side's interior mesh at that side's own stride. Pure CPU tests.
// ---------------------------------------------------------------------------

namespace VoxelMixedSeamTestUtils
{
	using namespace VoxelMeshingTestFixtures;

	/** Mixed-LOD +X face seam between A at (0,0,0) [LODA] and B at (1,0,0) [LODB]. */
	static bool MeshMixedFaceSeamX(int32 LODA, int32 LODB,
		TFunctionRef<bool(int32, int32, int32)> SolidFn, FChunkMeshData& Out)
	{
		FVoxelFaceSeamRequest Seam;
		Seam.Axis = 0;
		Seam.LODLevel = LODA;
		Seam.LODLevelB = LODB;
		Seam.ChunkSize = TestChunkSize;
		Seam.VoxelSize = TestVoxelSize;
		Seam.VoxelDataA = SharedVoxels(FIntVector::ZeroValue, SolidFn);
		Seam.VoxelDataB = SharedVoxels(FIntVector(TestChunkSize, 0, 0), SolidFn);

		FVoxelCPUDualContourMesher Mesher;
		Mesher.Initialize();
		Mesher.SetConfig(MakeDCConfig());
		const bool bOk = Mesher.GenerateFaceSeamMeshCPU(Seam, Out);
		Mesher.Shutdown();
		return bOk;
	}

	static bool SlopedGround(int32 X, int32 Y, int32 Z)
	{
		return Z <= 8 + (X * 8) / (2 * TestChunkSize) + (Y * 6) / (2 * TestChunkSize);
	}

	/**
	 * Noise terrain as the generators write it (G17): a rolling heightfield with small 3D bumps, the signed
	 * distance (in voxels, positive inside) mapped to density like SignedDistanceToDensity (127.5 at the surface,
	 * saturating one voxel away). Base is the height in voxels; Seed shifts the noise so each case is a new patch.
	 */
	struct FNoiseTerrain
	{
		float Base = 16.0f;
		FVector Seed = FVector::ZeroVector;

		float SignedDistance(int32 X, int32 Y, int32 Z) const
		{
			const FVector P = FVector(X, Y, Z) + Seed;
			const float Hills = 6.0f * FMath::PerlinNoise2D(FVector2D(P.X, P.Y) / 23.0f)
				+ 2.5f * FMath::PerlinNoise2D(FVector2D(P.X, P.Y) / 9.0f + FVector2D(17.0, 3.0));
			const float Bumps = 1.6f * FMath::PerlinNoise3D(P / 5.0f + FVector(5.0, 11.0, 2.0));
			return Base + Hills + Bumps - static_cast<float>(Z);
		}

		FVoxelData Voxel(int32 X, int32 Y, int32 Z) const
		{
			const float Clamped = FMath::Clamp(SignedDistance(X, Y, Z), -1.0f, 1.0f);
			const uint8 Density = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(127.5f + 127.5f * Clamped), 0, 255));
			FVoxelData Data = (Density > 0) ? FVoxelData::Solid(1, 0) : FVoxelData::Air();
			Data.Density = Density;
			return Data;
		}
	};

	static TSharedPtr<const TArray<FVoxelData>> NoiseVoxels(const FIntVector& ChunkOffsetVoxels, const FNoiseTerrain& Terrain)
	{
		TSharedPtr<TArray<FVoxelData>> Data = MakeShared<TArray<FVoxelData>>();
		Data->SetNumUninitialized(TestChunkSize * TestChunkSize * TestChunkSize);
		for (int32 Z = 0; Z < TestChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < TestChunkSize; ++Y)
			{
				for (int32 X = 0; X < TestChunkSize; ++X)
				{
					(*Data)[X + Y * TestChunkSize + Z * TestChunkSize * TestChunkSize] =
						Terrain.Voxel(X + ChunkOffsetVoxels.X, Y + ChunkOffsetVoxels.Y, Z + ChunkOffsetVoxels.Z);
				}
			}
		}
		return Data;
	}

	/** Interior mesh of one chunk from voxel data. */
	static bool MeshInteriorFrom(const TArray<FVoxelData>& Voxels, int32 LOD, FChunkMeshData& Out)
	{
		FVoxelMeshingRequest Request;
		Request.LODLevel = LOD;
		Request.ChunkSize = TestChunkSize;
		Request.VoxelSize = TestVoxelSize;
		Request.MeshCellDomain = EVoxelMeshCellDomain::Interior;
		Request.VoxelData = Voxels;

		FVoxelCPUDualContourMesher Mesher;
		Mesher.Initialize();
		Mesher.SetConfig(MakeDCConfig());
		const bool bOk = Mesher.GenerateMeshCPU(Request, Out);
		Mesher.Shutdown();
		return bOk;
	}

	/** Face seam between A (owner) and B, one chunk along Axis, from voxel data. */
	static bool MeshFaceSeamFrom(int32 Axis, int32 LODA, int32 LODB, const TSharedPtr<const TArray<FVoxelData>>& A,
		const TSharedPtr<const TArray<FVoxelData>>& B, FChunkMeshData& Out)
	{
		FVoxelFaceSeamRequest Seam;
		Seam.Axis = Axis;
		Seam.LODLevel = LODA;
		Seam.LODLevelB = LODB;
		Seam.ChunkSize = TestChunkSize;
		Seam.VoxelSize = TestVoxelSize;
		Seam.VoxelDataA = A;
		Seam.VoxelDataB = B;

		FVoxelCPUDualContourMesher Mesher;
		Mesher.Initialize();
		Mesher.SetConfig(MakeDCConfig());
		const bool bOk = Mesher.GenerateFaceSeamMeshCPU(Seam, Out);
		Mesher.Shutdown();
		return bOk;
	}

	/**
	 * Interior A + interior B + their face seam: the open edges inside the face probe (around
	 * the face plane, transversely clear of the rim, which is edge-seam territory). Margins use the coarser
	 * stride. OutFirst gets the first few open edges for the log.
	 */
	static int32 CountOpenEdgesAtFace(int32 Axis, int32 LODA, int32 LODB, const TSharedPtr<const TArray<FVoxelData>>& A,
		const TSharedPtr<const TArray<FVoxelData>>& B, int32& OutSeamTriangles, TArray<FString>& OutFirst)
	{
		FChunkMeshData InteriorA, InteriorB, Seam;
		if (!MeshInteriorFrom(*A, LODA, InteriorA) || !MeshInteriorFrom(*B, LODB, InteriorB)
			|| !MeshFaceSeamFrom(Axis, LODA, LODB, A, B, Seam))
		{
			OutFirst.Add(TEXT("meshing failed"));
			return -1;
		}
		OutSeamTriangles = Seam.GetTriangleCount();

		const float Face = static_cast<float>(TestChunkSize) * TestVoxelSize;
		FVector3f ShiftB = FVector3f::ZeroVector;
		ShiftB[Axis] = Face;
		TMap<FString, int32> EdgeUse;
		AccumulateEdges(InteriorA, FVector3f::ZeroVector, EdgeUse);
		AccumulateEdges(InteriorB, ShiftB, EdgeUse);
		AccumulateEdges(Seam, FVector3f::ZeroVector, EdgeUse);

		// The open box: a vertex stays in its own cell (G81), on its faces included, so one on a margin's plane may be a
		// rim cell's.
		const float S = static_cast<float>(1 << FMath::Max(LODA, LODB)) * TestVoxelSize;
		auto InProbe = [&](const FVector3f& P)
		{
			for (int32 i = 0; i < 3; ++i)
			{
				const float Lo = (i == Axis) ? Face - 2.5f * S : 2.0f * S;
				const float Hi = (i == Axis) ? Face + 2.5f * S : Face - 2.0f * S;
				if (P[i] <= Lo || P[i] >= Hi)
				{
					return false;
				}
			}
			return true;
		};

		int32 Open = 0;
		auto CheckMesh = [&](const FChunkMeshData& M, const FVector3f& Offset)
		{
			for (int32 T = 0; T + 2 < M.Indices.Num(); T += 3)
			{
				for (int32 i = 0; i < 3; ++i)
				{
					const FVector3f P0 = M.Positions[M.Indices[T + i]] + Offset;
					const FVector3f P1 = M.Positions[M.Indices[T + (i + 1) % 3]] + Offset;
					const FString K0 = PosKey(P0);
					const FString K1 = PosKey(P1);
					if (K0 == K1 || EdgeUse.FindChecked(K0 < K1 ? K0 + K1 : K1 + K0) != 1 || !InProbe(P0) || !InProbe(P1))
					{
						continue;
					}
					++Open;
					if (OutFirst.Num() < 4)
					{
						OutFirst.Add(FString::Printf(TEXT("(%.1f, %.1f, %.1f)-(%.1f, %.1f, %.1f)"), P0.X, P0.Y, P0.Z, P1.X, P1.Y, P1.Z));
					}
				}
			}
		};
		CheckMesh(InteriorA, FVector3f::ZeroVector);
		CheckMesh(InteriorB, ShiftB);
		CheckMesh(Seam, FVector3f::ZeroVector);
		return Open;
	}
}

// ===========================================================================
// MS1: interior A + interior B + mixed seam close the face across the
//      resolution change — owner-finer, owner-coarser, and a 4x ratio.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCMixedSeamClosureTest,
	"VoxelWorlds.Meshing.DualContour.SeamClosure.MS1_MixedFaceClosure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCMixedSeamClosureTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMixedSeamTestUtils;

	struct FCase { const TCHAR* Name; bool (*Fn)(int32, int32, int32); int32 LODA; int32 LODB; };
	const FCase Cases[] = {
		{ TEXT("Flat owner-fine 0|1"),    &FlatGround,   0, 1 },
		{ TEXT("Flat owner-coarse 1|0"),  &FlatGround,   1, 0 },
		{ TEXT("Sloped owner-fine 0|1"),  &SlopedGround, 0, 1 },
		{ TEXT("Sloped ratio-4 0|2"),     &SlopedGround, 0, 2 },
	};

	for (const FCase& Case : Cases)
	{
		const int32 SMax = 1 << FMath::Max(Case.LODA, Case.LODB);
		const float S = static_cast<float>(SMax) * TestVoxelSize;
		const float FaceX = static_cast<float>(TestChunkSize) * TestVoxelSize;

		FChunkMeshData InteriorA, InteriorB, Seam;
		TestTrue(FString::Printf(TEXT("%s: interior A meshed"), Case.Name),
			MeshInterior(FIntVector::ZeroValue, Case.LODA, Case.Fn, InteriorA));
		TestTrue(FString::Printf(TEXT("%s: interior B meshed"), Case.Name),
			MeshInterior(FIntVector(TestChunkSize, 0, 0), Case.LODB, Case.Fn, InteriorB));
		TestTrue(FString::Printf(TEXT("%s: mixed seam meshed"), Case.Name),
			MeshMixedFaceSeamX(Case.LODA, Case.LODB, Case.Fn, Seam));
		TestTrue(FString::Printf(TEXT("%s: seam produced geometry"), Case.Name), Seam.GetTriangleCount() > 0);

		TMap<FString, int32> EdgeUse;
		AccumulateEdges(InteriorA, FVector3f::ZeroVector, EdgeUse);
		AccumulateEdges(InteriorB, FVector3f(FaceX, 0.0f, 0.0f), EdgeUse);
		AccumulateEdges(Seam, FVector3f::ZeroVector, EdgeUse);

		// Probe around the face plane, transversely well inside (the transverse rim is the
		// P2d mixed-LOD edge-seam territory). Margins use the COARSER stride.
		const float ProbeMinX = FaceX - 2.5f * S;
		const float ProbeMaxX = FaceX + 2.5f * S;
		const float ProbeMinT = 2.0f * S;
		const float ProbeMaxT = FaceX - 2.0f * S;

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
						A.X >= ProbeMinX && A.X <= ProbeMaxX && B.X >= ProbeMinX && B.X <= ProbeMaxX &&
						A.Y >= ProbeMinT && A.Y <= ProbeMaxT && B.Y >= ProbeMinT && B.Y <= ProbeMaxT &&
						A.Z >= ProbeMinT && A.Z <= ProbeMaxT && B.Z >= ProbeMinT && B.Z <= ProbeMaxT;
					if (bInProbe)
					{
						++OpenInProbe;
						if (OpenInProbe <= 5)
						{
							AddError(FString::Printf(TEXT("%s: OPEN edge in mixed-face probe: (%f,%f,%f)-(%f,%f,%f)"),
								Case.Name, A.X, A.Y, A.Z, B.X, B.Y, B.Z));
						}
					}
				}
			}
		};
		CheckMesh(InteriorA, FVector3f::ZeroVector);
		CheckMesh(InteriorB, FVector3f(FaceX, 0.0f, 0.0f));
		CheckMesh(Seam, FVector3f::ZeroVector);

		TestEqual(FString::Printf(TEXT("%s: open edges in the mixed-face probe"), Case.Name), OpenInProbe, 0);
	}
	return true;
}

// ===========================================================================
// MS2: mixed-seam ring vertices are bit-identical to each side's interior
//      mesh at that side's OWN stride.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCMixedSeamRingMatchTest,
	"VoxelWorlds.Meshing.DualContour.SeamClosure.MS2_MixedRingBitMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCMixedSeamRingMatchTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMixedSeamTestUtils;
	constexpr int32 LODA = 0;
	constexpr int32 LODB = 1;
	const float SA = static_cast<float>(1 << LODA) * TestVoxelSize;
	const float SB = static_cast<float>(1 << LODB) * TestVoxelSize;
	const float FaceX = static_cast<float>(TestChunkSize) * TestVoxelSize;

	FChunkMeshData InteriorA, InteriorB, Seam;
	TestTrue(TEXT("interior A meshed"), MeshInterior(FIntVector::ZeroValue, LODA, &FlatGround, InteriorA));
	TestTrue(TEXT("interior B meshed"), MeshInterior(FIntVector(TestChunkSize, 0, 0), LODB, &FlatGround, InteriorB));
	TestTrue(TEXT("mixed seam meshed"), MeshMixedFaceSeamX(LODA, LODB, &FlatGround, Seam));
	TestTrue(TEXT("seam produced geometry"), Seam.GetTriangleCount() > 0);

	TSet<FString> InteriorAPositions;
	for (const FVector3f& P : InteriorA.Positions)
	{
		InteriorAPositions.Add(PosKey(P));
	}
	TSet<FString> InteriorBPositions;
	for (const FVector3f& P : InteriorB.Positions)
	{
		InteriorBPositions.Add(PosKey(P + FVector3f(FaceX, 0.0f, 0.0f)));
	}

	// X bands: ring-A [FaceX-2*SA, FaceX-SA], slab (FaceX-SA, FaceX), ring-B [FaceX, FaceX+SB].
	constexpr float Margin = 0.5f;
	int32 RingACount = 0, RingBCount = 0, SlabCount = 0;
	for (const FVector3f& P : Seam.Positions)
	{
		if (P.X < FaceX - SA - Margin)
		{
			++RingACount;
			if (!InteriorAPositions.Contains(PosKey(P)))
			{
				AddError(FString::Printf(TEXT("ring-A seam vertex (%f,%f,%f) not bit-identical to any interior-A vertex"),
					P.X, P.Y, P.Z));
				return true;
			}
		}
		else if (P.X > FaceX + Margin)
		{
			++RingBCount;
			if (!InteriorBPositions.Contains(PosKey(P)))
			{
				AddError(FString::Printf(TEXT("ring-B seam vertex (%f,%f,%f) not bit-identical to any interior-B vertex"),
					P.X, P.Y, P.Z));
				return true;
			}
		}
		else
		{
			++SlabCount;
		}
	}

	TestTrue(TEXT("seam references ring-A vertices"), RingACount > 0);
	TestTrue(TEXT("seam references ring-B vertices"), RingBCount > 0);
	TestTrue(TEXT("seam has slab vertices of its own"), SlabCount > 0);
	return true;
}

// ===========================================================================
// MS3: no surface at the face -> valid empty mixed seam.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCMixedSeamEmptyTest,
	"VoxelWorlds.Meshing.DualContour.SeamClosure.MS3_NoSurfaceEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCMixedSeamEmptyTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMixedSeamTestUtils;

	FChunkMeshData SeamAir, SeamSolid;
	TestTrue(TEXT("all-air mixed seam meshed"),
		MeshMixedFaceSeamX(0, 1, [](int32, int32, int32) { return false; }, SeamAir));
	TestEqual(TEXT("all-air mixed seam is empty"), SeamAir.GetTriangleCount(), 0);

	TestTrue(TEXT("all-solid mixed seam meshed"),
		MeshMixedFaceSeamX(0, 1, [](int32, int32, int32) { return true; }, SeamSolid));
	TestEqual(TEXT("all-solid mixed seam is empty"), SeamSolid.GetTriangleCount(), 0);

	return true;
}

// ===========================================================================
// MS4 (G17): interior A + interior B + the mixed seam close the face on noise
//      terrain, every LOD pair up to 2, on all three axes. Before the fix a
//      fine crossing facing a coarse cell with no crossing of its own was
//      skipped, leaving small holes (the 5-6 edge gaps a streamed world shows
//      at 1|2 and 0|1 face seams).
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCMixedSeamNoiseClosureTest,
	"VoxelWorlds.Meshing.DualContour.SeamClosure.MS4_MixedFaceClosureNoise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCMixedSeamNoiseClosureTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMixedSeamTestUtils;

	const FIntPoint Pairs[] = { { 0, 1 }, { 1, 0 }, { 1, 2 }, { 2, 1 }, { 0, 2 }, { 2, 0 }, { 1, 1 } }; // 1|1: the same-LOD control
	constexpr int32 Patches = 24;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		int32 Open[UE_ARRAY_COUNT(Pairs)] = {};
		int32 Holed[UE_ARRAY_COUNT(Pairs)] = {};
		int32 SeamTriangles[UE_ARRAY_COUNT(Pairs)] = {};
		for (int32 Patch = 0; Patch < Patches; ++Patch)
		{
			FNoiseTerrain Terrain;
			Terrain.Base = (Axis == 2) ? static_cast<float>(TestChunkSize) : 16.0f; // the surface meets the face plane
			Terrain.Seed = FVector(37.0 * Patch + 3.0, 53.0 * Patch + 7.0, 11.0 * Patch);
			FIntVector OffsetB = FIntVector::ZeroValue;
			OffsetB[Axis] = TestChunkSize;
			const TSharedPtr<const TArray<FVoxelData>> A = NoiseVoxels(FIntVector::ZeroValue, Terrain);
			const TSharedPtr<const TArray<FVoxelData>> B = NoiseVoxels(OffsetB, Terrain);

			for (int32 p = 0; p < UE_ARRAY_COUNT(Pairs); ++p)
			{
				int32 Triangles = 0;
				TArray<FString> First;
				const int32 OpenHere = CountOpenEdgesAtFace(Axis, Pairs[p].X, Pairs[p].Y, A, B, Triangles, First);
				SeamTriangles[p] += Triangles;
				if (OpenHere != 0)
				{
					Open[p] += OpenHere;
					++Holed[p];
					if (Holed[p] <= 2)
					{
						AddInfo(FString::Printf(TEXT("axis %d, LOD %d|%d, patch %d: %d open edges, first %s"),
							Axis, Pairs[p].X, Pairs[p].Y, Patch, OpenHere, *FString::Join(First, TEXT(", "))));
					}
				}
			}
		}
		for (int32 p = 0; p < UE_ARRAY_COUNT(Pairs); ++p)
		{
			const FString Name = FString::Printf(TEXT("axis %d, LOD %d|%d"), Axis, Pairs[p].X, Pairs[p].Y);
			AddInfo(FString::Printf(TEXT("%s: %d open edges in %d of %d patches, %d seam triangles"),
				*Name, Open[p], Holed[p], Patches, SeamTriangles[p]));
			TestTrue(*FString::Printf(TEXT("%s: the seams have geometry"), *Name), SeamTriangles[p] > 0);
			TestEqual(*FString::Printf(TEXT("%s: open edges in the face probe"), *Name), Open[p], 0);
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
