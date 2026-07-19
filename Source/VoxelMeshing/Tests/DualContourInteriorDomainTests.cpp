// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelMeshingTypes.h"
#include "ChunkRenderData.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Seam-ownership P1 — Interior cell domain (SEAM_OWNERSHIP_ARCHITECTURE.md §2.1).
// The Interior domain must mesh ONLY cells with zero neighbor dependence
// ([0, GridSize-1) per axis): no quad may reference a -1/GridSize-1
// boundary-layer cell, and when the surface never approaches the chunk
// boundary the interior mesh must be bit-identical to the legacy Full mesh.
// Pure CPU tests (no RHI) — run headless.
// ---------------------------------------------------------------------------

namespace VoxelInteriorDomainTestUtils
{
	constexpr int32 TestChunkSize = 32;
	constexpr float TestVoxelSize = 100.0f;

	/** Build a request over a density lambda: Fn(VX,VY,VZ) -> solid? No neighbor data. */
	static FVoxelMeshingRequest MakeRequest(int32 LODLevel, EVoxelMeshCellDomain Domain,
		TFunctionRef<bool(int32, int32, int32)> SolidFn)
	{
		FVoxelMeshingRequest Request;
		Request.ChunkCoord = FIntVector::ZeroValue;
		Request.LODLevel = LODLevel;
		Request.ChunkSize = TestChunkSize;
		Request.VoxelSize = TestVoxelSize;
		Request.MeshCellDomain = Domain;
		Request.VoxelData.SetNumUninitialized(TestChunkSize * TestChunkSize * TestChunkSize);
		for (int32 Z = 0; Z < TestChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < TestChunkSize; ++Y)
			{
				for (int32 X = 0; X < TestChunkSize; ++X)
				{
					const int32 Index = X + Y * TestChunkSize + Z * TestChunkSize * TestChunkSize;
					Request.VoxelData[Index] = SolidFn(X, Y, Z) ? FVoxelData::Solid(1, 0) : FVoxelData::Air();
				}
			}
		}
		return Request;
	}

	/** Solid sphere centered in the chunk, radius in voxels (kept well clear of all boundaries). */
	static bool CenteredSphere(int32 X, int32 Y, int32 Z)
	{
		const float DX = X - 16.0f, DY = Y - 16.0f, DZ = Z - 16.0f;
		return (DX * DX + DY * DY + DZ * DZ) <= (8.0f * 8.0f);
	}

	/** Flat ground: solid at and below voxel Z = 12 — the surface crosses every side boundary. */
	static bool FlatGround(int32 X, int32 Y, int32 Z)
	{
		return Z <= 12;
	}

	/** DC mesher config matching the DT harness: smooth meshing, default QEF params, skirts off. */
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

	/** Mesh a request with a locally-initialized DC mesher. */
	static bool Mesh(const FVoxelMeshingRequest& Request, FChunkMeshData& Out)
	{
		FVoxelCPUDualContourMesher Mesher;
		Mesher.Initialize();
		Mesher.SetConfig(MakeConfig());
		const bool bOk = Mesher.GenerateMeshCPU(Request, Out);
		Mesher.Shutdown();
		return bOk;
	}

	/** Bitwise equality of two meshes (positions, normals, indices). */
	static bool MeshesIdentical(const FChunkMeshData& A, const FChunkMeshData& B)
	{
		if (A.Positions.Num() != B.Positions.Num() || A.Indices.Num() != B.Indices.Num() ||
			A.Normals.Num() != B.Normals.Num())
		{
			return false;
		}
		return (A.Positions.Num() == 0 || FMemory::Memcmp(A.Positions.GetData(), B.Positions.GetData(), A.Positions.Num() * sizeof(FVector3f)) == 0)
			&& (A.Normals.Num() == 0 || FMemory::Memcmp(A.Normals.GetData(), B.Normals.GetData(), A.Normals.Num() * sizeof(FVector3f)) == 0)
			&& (A.Indices.Num() == 0 || FMemory::Memcmp(A.Indices.GetData(), B.Indices.GetData(), A.Indices.Num() * sizeof(uint32)) == 0);
	}

	/** Canonical byte key for one triangle (exact float bits; winding preserved). */
	static FString TriangleKey(const FChunkMeshData& M, int32 TriIndex)
	{
		float Buf[9];
		for (int32 i = 0; i < 3; ++i)
		{
			const FVector3f& P = M.Positions[M.Indices[TriIndex * 3 + i]];
			Buf[i * 3 + 0] = P.X; Buf[i * 3 + 1] = P.Y; Buf[i * 3 + 2] = P.Z;
		}
		return BytesToHex(reinterpret_cast<const uint8*>(Buf), sizeof(Buf));
	}
}

// ===========================================================================
// ID1: surface far from all boundaries -> Interior output == Full output.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCInteriorDomainCenteredTest,
	"VoxelWorlds.Meshing.DualContour.InteriorDomain.ID1_CenteredSurfaceIdentical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCInteriorDomainCenteredTest::RunTest(const FString& Parameters)
{
	using namespace VoxelInteriorDomainTestUtils;

	for (int32 LOD = 0; LOD <= 1; ++LOD)
	{
		FChunkMeshData Full, Interior;
		TestTrue(TEXT("Full mesh generated"),
			Mesh(MakeRequest(LOD, EVoxelMeshCellDomain::Full, &CenteredSphere), Full));
		TestTrue(TEXT("Interior mesh generated"),
			Mesh(MakeRequest(LOD, EVoxelMeshCellDomain::Interior, &CenteredSphere), Interior));

		TestTrue(FString::Printf(TEXT("LOD%d: sphere produced geometry"), LOD), Interior.GetTriangleCount() > 0);
		TestTrue(FString::Printf(TEXT("LOD%d: interior == full for a boundary-free surface"), LOD),
			MeshesIdentical(Full, Interior));
	}
	return true;
}

// ===========================================================================
// ID2: surface crossing the chunk boundary -> Interior is a strict positional
//      subset of Full, confined to the interior cell hull.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCInteriorDomainBoundaryTest,
	"VoxelWorlds.Meshing.DualContour.InteriorDomain.ID2_BoundaryCrossingRestricted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCInteriorDomainBoundaryTest::RunTest(const FString& Parameters)
{
	using namespace VoxelInteriorDomainTestUtils;
	constexpr int32 LOD = 0;
	constexpr int32 Stride = 1 << LOD;
	constexpr int32 GridSize = TestChunkSize / Stride;

	FChunkMeshData Full, Interior;
	TestTrue(TEXT("Full mesh generated"),
		Mesh(MakeRequest(LOD, EVoxelMeshCellDomain::Full, &FlatGround), Full));
	TestTrue(TEXT("Interior mesh generated"),
		Mesh(MakeRequest(LOD, EVoxelMeshCellDomain::Interior, &FlatGround), Interior));

	TestTrue(TEXT("interior produced geometry"), Interior.GetTriangleCount() > 0);
	// Full covers the interior plus the boundary layers, so it must be strictly larger.
	TestTrue(TEXT("full has strictly more triangles than interior"),
		Full.GetTriangleCount() > Interior.GetTriangleCount());

	// Domain guarantee: every interior vertex lies inside the interior cell hull
	// [0, (GridSize-1)*Stride*VoxelSize] per axis (QEF positions are clamped to cell bounds).
	const float HullMax = static_cast<float>((GridSize - 1) * Stride) * TestVoxelSize;
	constexpr float Eps = 0.01f;
	for (const FVector3f& P : Interior.Positions)
	{
		if (P.X < -Eps || P.Y < -Eps || P.Z < -Eps ||
			P.X > HullMax + Eps || P.Y > HullMax + Eps || P.Z > HullMax + Eps)
		{
			AddError(FString::Printf(TEXT("interior vertex escapes the interior hull: (%f, %f, %f), hull max %f"),
				P.X, P.Y, P.Z, HullMax));
			return true;
		}
	}

	// Subset (deep interior only): triangles whose vertices sit >= 2 cells from every boundary
	// bit-match the Full mesh — their gradient taps never exit the chunk, so both domains
	// compute them identically. RIM-adjacent interior triangles legitimately differ: the
	// Interior domain CLAMPS out-of-chunk gradient taps ("terrain continues") while the
	// Full-without-neighbors run uses the Air fallback — the deliberate fix for the
	// per-chunk rim-shading checkerboard.
	const float DeepMin = 2.0f * Stride * TestVoxelSize;
	const float DeepMax = HullMax - 2.0f * Stride * TestVoxelSize;
	TSet<FString> FullTriangles;
	for (int32 T = 0; T < Full.GetTriangleCount(); ++T)
	{
		FullTriangles.Add(TriangleKey(Full, T));
	}
	int32 DeepChecked = 0;
	for (int32 T = 0; T < Interior.GetTriangleCount(); ++T)
	{
		bool bDeep = true;
		for (int32 i = 0; i < 3 && bDeep; ++i)
		{
			const FVector3f& P = Interior.Positions[Interior.Indices[T * 3 + i]];
			bDeep = P.X >= DeepMin && P.X <= DeepMax &&
				P.Y >= DeepMin && P.Y <= DeepMax &&
				P.Z >= DeepMin && P.Z <= DeepMax;
		}
		if (!bDeep)
		{
			continue;
		}
		++DeepChecked;
		if (!FullTriangles.Contains(TriangleKey(Interior, T)))
		{
			AddError(FString::Printf(TEXT("deep-interior triangle %d not found in the full mesh"), T));
			return true;
		}
	}
	TestTrue(TEXT("deep-interior subset check covered triangles"), DeepChecked > 0);
	return true;
}

// ===========================================================================
// ID3: the interior pass is deterministic (bitwise) across repeated runs.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCInteriorDomainDeterminismTest,
	"VoxelWorlds.Meshing.DualContour.InteriorDomain.ID3_Deterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCInteriorDomainDeterminismTest::RunTest(const FString& Parameters)
{
	using namespace VoxelInteriorDomainTestUtils;

	FChunkMeshData RunA, RunB;
	TestTrue(TEXT("run A generated"),
		Mesh(MakeRequest(/*LOD*/ 0, EVoxelMeshCellDomain::Interior, &FlatGround), RunA));
	TestTrue(TEXT("run B generated"),
		Mesh(MakeRequest(/*LOD*/ 0, EVoxelMeshCellDomain::Interior, &FlatGround), RunB));

	TestTrue(TEXT("interior pass is bitwise deterministic"), MeshesIdentical(RunA, RunB));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
