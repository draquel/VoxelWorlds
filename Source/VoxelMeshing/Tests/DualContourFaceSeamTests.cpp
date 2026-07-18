// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelMeshingTypes.h"
#include "ChunkRenderData.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Seam-ownership P1 — single-owner FACE seams (SEAM_OWNERSHIP_ARCHITECTURE.md
// §2.2). Asserts the seam-mesh CLOSURE property directly (the strictly simpler
// replacement for two-sided vertex matching): interior A + interior B + the
// face seam form a closed surface across the face interior, and the seam's
// ring vertices are bit-identical to the interior meshes' boundary rings.
// Pure CPU tests (no RHI) — run headless.
// ---------------------------------------------------------------------------

namespace VoxelFaceSeamTestUtils
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

	/** Interior-domain chunk mesh over a global predicate (chunk at voxel offset; no neighbor data). */
	static bool MeshInterior(const FIntVector& ChunkOffsetVoxels, int32 LOD,
		TFunctionRef<bool(int32, int32, int32)> SolidFn, FChunkMeshData& Out)
	{
		FVoxelMeshingRequest Request;
		Request.ChunkCoord = FIntVector::ZeroValue;
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

	/** Face-seam mesh for the +X pair A=(0,0,0), B=(1,0,0) over the same global predicate. */
	static bool MeshFaceSeamX(int32 LOD, TFunctionRef<bool(int32, int32, int32)> SolidFn, FChunkMeshData& Out)
	{
		FVoxelFaceSeamRequest Seam;
		Seam.OwnerChunkCoord = FIntVector::ZeroValue;
		Seam.Axis = 0;
		Seam.LODLevel = LOD;
		Seam.ChunkSize = TestChunkSize;
		Seam.VoxelSize = TestVoxelSize;
		Seam.VoxelDataA = FillVoxels(FIntVector::ZeroValue, SolidFn);
		Seam.VoxelDataB = FillVoxels(FIntVector(TestChunkSize, 0, 0), SolidFn);

		FVoxelCPUDualContourMesher Mesher;
		Mesher.Initialize();
		Mesher.SetConfig(MakeConfig());
		const bool bOk = Mesher.GenerateFaceSeamMeshCPU(Seam, Out);
		Mesher.Shutdown();
		return bOk;
	}

	/** Exact byte key for one position. */
	static FString PosKey(const FVector3f& P)
	{
		return BytesToHex(reinterpret_cast<const uint8*>(&P), sizeof(FVector3f));
	}

	/** Append a mesh's triangles into an undirected positional edge-use counter. */
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
				// Skip degenerate (duplicate-position) edges — they carry no surface boundary.
				if (KA == KB)
				{
					continue;
				}
				const FString Key = (KA < KB) ? (KA + KB) : (KB + KA);
				EdgeUse.FindOrAdd(Key)++;
			}
		}
	}

	/** Global flat ground: solid at and below voxel Z = 12 (crosses every vertical chunk face). */
	static bool FlatGround(int32 X, int32 Y, int32 Z)
	{
		return Z <= 12;
	}

	/** Global sloped ground: height rises with global X so the two sides differ across the face. */
	static bool SlopedGround(int32 X, int32 Y, int32 Z)
	{
		return Z <= 8 + (X * 8) / (2 * TestChunkSize);
	}
}

// ===========================================================================
// SC1: interior A + interior B + face seam close the surface across the face
//      (no open edges in the face-interior probe region), flat and sloped,
//      LOD0 and same-LOD LOD1.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCFaceSeamClosureTest,
	"VoxelWorlds.Meshing.DualContour.SeamClosure.SC1_FaceClosure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCFaceSeamClosureTest::RunTest(const FString& Parameters)
{
	using namespace VoxelFaceSeamTestUtils;

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
		const float FaceX = static_cast<float>(TestChunkSize) * TestVoxelSize;

		FChunkMeshData InteriorA, InteriorB, Seam;
		TestTrue(FString::Printf(TEXT("%s: interior A meshed"), Case.Name),
			MeshInterior(FIntVector::ZeroValue, Case.LOD, Case.Fn, InteriorA));
		TestTrue(FString::Printf(TEXT("%s: interior B meshed"), Case.Name),
			MeshInterior(FIntVector(TestChunkSize, 0, 0), Case.LOD, Case.Fn, InteriorB));
		TestTrue(FString::Printf(TEXT("%s: seam meshed"), Case.Name),
			MeshFaceSeamX(Case.LOD, Case.Fn, Seam));

		TestTrue(FString::Printf(TEXT("%s: seam produced geometry"), Case.Name), Seam.GetTriangleCount() > 0);

		// Combine the three meshes in the OWNER frame (B offset by one chunk on X — the same
		// float op the seam mesher applies to ring-B vertices).
		TMap<FString, int32> EdgeUse;
		AccumulateEdges(InteriorA, FVector3f::ZeroVector, EdgeUse);
		AccumulateEdges(InteriorB, FVector3f(FaceX, 0.0f, 0.0f), EdgeUse);
		AccumulateEdges(Seam, FVector3f::ZeroVector, EdgeUse);

		// Probe region: around the face plane (covers ring junctions on both sides), well inside
		// transversely (the transverse rim is the P1-permitted opening — P2's edge/corner seams).
		const float ProbeMinX = FaceX - 2.5f * S;
		const float ProbeMaxX = FaceX + 2.5f * S;
		const float ProbeMinT = 2.0f * S;
		const float ProbeMaxT = static_cast<float>(SL - 1) * S;

		// Rebuild endpoint positions from the edge keys is awkward — instead re-walk the meshes
		// and check openness per edge: an edge is OPEN if its use-count is 1.
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
						continue; // shared — closed here
					}
					// Open edge: only permitted outside the probe region.
					const bool bInProbe =
						A.X >= ProbeMinX && A.X <= ProbeMaxX && B.X >= ProbeMinX && B.X <= ProbeMaxX &&
						A.Y >= ProbeMinT && A.Y <= ProbeMaxT && B.Y >= ProbeMinT && B.Y <= ProbeMaxT &&
						A.Z >= ProbeMinT && A.Z <= ProbeMaxT && B.Z >= ProbeMinT && B.Z <= ProbeMaxT;
					if (bInProbe)
					{
						++OpenInProbe;
						if (OpenInProbe <= 5)
						{
							AddError(FString::Printf(TEXT("%s: OPEN edge in face-interior probe: (%f,%f,%f)-(%f,%f,%f)"),
								Case.Name, A.X, A.Y, A.Z, B.X, B.Y, B.Z));
						}
					}
				}
			}
		};
		CheckMesh(InteriorA, FVector3f::ZeroVector);
		CheckMesh(InteriorB, FVector3f(FaceX, 0.0f, 0.0f));
		CheckMesh(Seam, FVector3f::ZeroVector);

		TestEqual(FString::Printf(TEXT("%s: open edges in the face-interior probe region"), Case.Name),
			OpenInProbe, 0);
	}
	return true;
}

// ===========================================================================
// SC2: the seam's ring vertices are BIT-IDENTICAL to the interior meshes'
//      boundary-ring vertices (the stitching contract of §2.2).
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCFaceSeamRingMatchTest,
	"VoxelWorlds.Meshing.DualContour.SeamClosure.SC2_RingBitMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCFaceSeamRingMatchTest::RunTest(const FString& Parameters)
{
	using namespace VoxelFaceSeamTestUtils;
	constexpr int32 LOD = 0;
	constexpr int32 Stride = 1 << LOD;
	const float S = static_cast<float>(Stride) * TestVoxelSize;
	const float FaceX = static_cast<float>(TestChunkSize) * TestVoxelSize;

	FChunkMeshData InteriorA, InteriorB, Seam;
	TestTrue(TEXT("interior A meshed"), MeshInterior(FIntVector::ZeroValue, LOD, &FlatGround, InteriorA));
	TestTrue(TEXT("interior B meshed"), MeshInterior(FIntVector(TestChunkSize, 0, 0), LOD, &FlatGround, InteriorB));
	TestTrue(TEXT("seam meshed"), MeshFaceSeamX(LOD, &FlatGround, Seam));
	TestTrue(TEXT("seam produced geometry"), Seam.GetTriangleCount() > 0);

	// Interior position sets (owner frame; B offset by one chunk — the seam's own frame op).
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

	// Classify seam vertices by X band (QEF positions are clamped to their cell bounds; the flat
	// ground puts vertices mid-cell, far from band boundaries):
	//   ring A: X in [FaceX-2S, FaceX-S]   -> must exist bitwise in interior A
	//   slab:   X in (FaceX-S, FaceX)      -> seam-only geometry
	//   ring B: X in [FaceX, FaceX+S]      -> must exist bitwise in interior B (+offset)
	int32 RingACount = 0, RingBCount = 0, SlabCount = 0;
	const float Margin = 0.5f; // uu — bands are 100 uu wide; flat-ground verts sit mid-band
	for (const FVector3f& P : Seam.Positions)
	{
		if (P.X < FaceX - S - Margin)
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
// SC3: no surface at the face -> the seam job produces an empty (valid) mesh.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCFaceSeamEmptyTest,
	"VoxelWorlds.Meshing.DualContour.SeamClosure.SC3_NoSurfaceEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCFaceSeamEmptyTest::RunTest(const FString& Parameters)
{
	using namespace VoxelFaceSeamTestUtils;

	FChunkMeshData SeamAir, SeamSolid;
	TestTrue(TEXT("all-air seam meshed"),
		MeshFaceSeamX(0, [](int32, int32, int32) { return false; }, SeamAir));
	TestEqual(TEXT("all-air seam is empty"), SeamAir.GetTriangleCount(), 0);

	TestTrue(TEXT("all-solid seam meshed"),
		MeshFaceSeamX(0, [](int32, int32, int32) { return true; }, SeamSolid));
	TestEqual(TEXT("all-solid seam is empty"), SeamSolid.GetTriangleCount(), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
