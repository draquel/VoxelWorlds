// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUDualContourMesher.h"
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
		Mesher.SetConfig(MakeConfig());
		const bool bOk = Mesher.GenerateFaceSeamMeshCPU(Seam, Out);
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

	static bool FlatGround(int32 X, int32 Y, int32 Z)
	{
		return Z <= 12;
	}

	static bool SlopedGround(int32 X, int32 Y, int32 Z)
	{
		return Z <= 8 + (X * 8) / (2 * TestChunkSize) + (Y * 6) / (2 * TestChunkSize);
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

#endif // WITH_DEV_AUTOMATION_TESTS
