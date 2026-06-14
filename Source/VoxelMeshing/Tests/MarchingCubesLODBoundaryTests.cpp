// Copyright Daniel Raquel. All Rights Reserved.

// Two-chunk watertightness tests for Marching Cubes LOD boundary meshing.
//
// Builds two adjacent 32^3 chunks A (chunk coord 0,0,0) and B (1,0,0) from the
// same analytic world-space density field, fills neighbor face/edge/corner data
// exactly the way UVoxelChunkManager::ExtractNeighborEdgeSlices does, meshes
// both chunks, and checks that the geometry each side produces on the shared
// face (world x = ChunkSize * VoxelSize) is watertight.
//
// Each test converts one hypothesis from the LOD seam investigation
// (Plugins/VoxelWorlds/Documentation/LOD_SEAM_INVESTIGATION.md) into pass/fail:
//   T1  both LOD0, full slices          -> harness sanity baseline
//   T2  both LOD1, full slices          -> is strided same-LOD boundary meshing sound?
//   T3  both LOD2, full slices          -> same, at stride 4
//   T4  A LOD0 / B LOD1, seams off      -> raw LOD mismatch magnitude (cracks <= 1 fine cell)
//   T5  T2 with neighbor arrays empty   -> documents the silent clamp/air fallback hazard
//   T6  A LOD0 + transition face / B LOD1 -> transvoxel watertightness acceptance

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUMarchingCubesMesher.h"
#include "VoxelMeshingTypes.h"
#include "VoxelData.h"
#include "ChunkRenderData.h"

namespace MarchingCubesLODBoundaryTestHelpers
{
	constexpr int32 TestChunkSize = 32;
	constexpr float TestVoxelSize = 100.0f;
	constexpr float BoundaryPlaneWorldX = TestChunkSize * TestVoxelSize; // 3200

	/** Vertices within this distance of the boundary plane count as boundary vertices. */
	constexpr float PlaneEpsilon = 0.5f;
	/** Two boundary vertices closer than this are considered the same vertex. */
	constexpr float MatchTolerance = 0.5f;
	/** Dedup tolerance for collapsing the triangle-soup vertices. */
	constexpr float DedupTolerance = 0.01f;

	/**
	 * Which analytic field the helpers sample. Automation tests run sequentially
	 * on one thread, so a file-scope selector is a safe, contained way to swap
	 * the field for a single test without threading a functor through every
	 * request/neighbor builder. Tests set it at entry and restore Smooth on exit.
	 */
	enum class ETestField { Smooth, Cliff };
	static ETestField GActiveField = ETestField::Smooth;

	/**
	 * Analytic surface height.
	 *
	 * Smooth: gently sloped plane with a sine ripple in Y so the iso-line on the
	 * shared face is curved (a straight line would let coarse and fine meshes
	 * coincide and hide T-junction cracks in T4).
	 *
	 * Cliff: steep ramp in X near the shared face (world x=3200). Height changes
	 * ~250 units over the 100-unit span between world x=3100 (the plane the clamp
	 * fallback duplicates) and x=3200 (the true face), so an empty neighbor slice
	 * displaces the finer chunk's face vertices by more than one coarse (LOD1)
	 * cell — the gross regime the gentle field cannot exhibit.
	 */
	float SurfaceHeight(float WorldX, float WorldY)
	{
		if (GActiveField == ETestField::Cliff)
		{
			return 1600.0f + 2.5f * (WorldX - 3100.0f) + 40.0f * FMath::Sin(WorldY * 0.01f);
		}
		return 1600.0f + 0.15f * WorldX + 0.1f * WorldY + 60.0f * FMath::Sin(WorldY * 0.01f);
	}

	/** RAII guard that selects a field for the duration of a test and restores Smooth. */
	struct FScopedTestField
	{
		explicit FScopedTestField(ETestField Field) { GActiveField = Field; }
		~FScopedTestField() { GActiveField = ETestField::Smooth; }
	};

	/**
	 * Sample the analytic density field at a world position.
	 * Density is linear in Z over a 1600-unit ramp so the iso-crossing
	 * interpolates to the same position regardless of sample stride.
	 */
	FVoxelData SampleField(const FVector& WorldPos)
	{
		const float H = SurfaceHeight(WorldPos.X, WorldPos.Y);
		const float Normalized = FMath::Clamp(0.5f + (H - WorldPos.Z) / 1600.0f, 0.0f, 1.0f);

		FVoxelData Voxel;
		Voxel.Density = static_cast<uint8>(FMath::RoundToInt(Normalized * 255.0f));
		Voxel.MaterialID = 1;
		Voxel.BiomeID = 0;
		Voxel.Metadata = 0;
		return Voxel;
	}

	/** Sample the field at a (possibly out-of-bounds) local voxel coordinate of a chunk. */
	FVoxelData SampleLocal(const FIntVector& ChunkCoord, int32 X, int32 Y, int32 Z)
	{
		const FVector ChunkOrigin = FVector(ChunkCoord) * TestChunkSize * TestVoxelSize;
		return SampleField(ChunkOrigin + FVector(X, Y, Z) * TestVoxelSize);
	}

	/** Build a meshing request for one chunk, voxel data filled from the analytic field. */
	FVoxelMeshingRequest MakeChunkRequest(const FIntVector& ChunkCoord, int32 LODLevel)
	{
		FVoxelMeshingRequest Request;
		Request.ChunkCoord = ChunkCoord;
		Request.LODLevel = LODLevel;
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
					Request.VoxelData[Index] = SampleLocal(ChunkCoord, X, Y, Z);
				}
			}
		}
		return Request;
	}

	/**
	 * Fill all 6 face slices, 12 edge strips, and 8 corners from the analytic
	 * field, using the same layout UVoxelChunkManager::ExtractNeighborEdgeSlices
	 * produces (face plane index +ChunkSize maps to neighbor plane 0, etc).
	 */
	void FillAllNeighborData(FVoxelMeshingRequest& Request)
	{
		const FIntVector C = Request.ChunkCoord;
		const int32 CS = TestChunkSize;
		const int32 SliceSize = CS * CS;

		Request.NeighborXPos.SetNumUninitialized(SliceSize);
		Request.NeighborXNeg.SetNumUninitialized(SliceSize);
		Request.NeighborYPos.SetNumUninitialized(SliceSize);
		Request.NeighborYNeg.SetNumUninitialized(SliceSize);
		Request.NeighborZPos.SetNumUninitialized(SliceSize);
		Request.NeighborZNeg.SetNumUninitialized(SliceSize);
		for (int32 B = 0; B < CS; ++B)
		{
			for (int32 A = 0; A < CS; ++A)
			{
				const int32 Index = A + B * CS;
				Request.NeighborXPos[Index] = SampleLocal(C, CS, A, B); // [Y + Z*CS]
				Request.NeighborXNeg[Index] = SampleLocal(C, -1, A, B);
				Request.NeighborYPos[Index] = SampleLocal(C, A, CS, B); // [X + Z*CS]
				Request.NeighborYNeg[Index] = SampleLocal(C, A, -1, B);
				Request.NeighborZPos[Index] = SampleLocal(C, A, B, CS); // [X + Y*CS]
				Request.NeighborZNeg[Index] = SampleLocal(C, A, B, -1);
			}
		}

		Request.EdgeXPosYPos.SetNumUninitialized(CS);
		Request.EdgeXPosYNeg.SetNumUninitialized(CS);
		Request.EdgeXNegYPos.SetNumUninitialized(CS);
		Request.EdgeXNegYNeg.SetNumUninitialized(CS);
		Request.EdgeXPosZPos.SetNumUninitialized(CS);
		Request.EdgeXPosZNeg.SetNumUninitialized(CS);
		Request.EdgeXNegZPos.SetNumUninitialized(CS);
		Request.EdgeXNegZNeg.SetNumUninitialized(CS);
		Request.EdgeYPosZPos.SetNumUninitialized(CS);
		Request.EdgeYPosZNeg.SetNumUninitialized(CS);
		Request.EdgeYNegZPos.SetNumUninitialized(CS);
		Request.EdgeYNegZNeg.SetNumUninitialized(CS);
		for (int32 I = 0; I < CS; ++I)
		{
			Request.EdgeXPosYPos[I] = SampleLocal(C, CS, CS, I);
			Request.EdgeXPosYNeg[I] = SampleLocal(C, CS, -1, I);
			Request.EdgeXNegYPos[I] = SampleLocal(C, -1, CS, I);
			Request.EdgeXNegYNeg[I] = SampleLocal(C, -1, -1, I);
			Request.EdgeXPosZPos[I] = SampleLocal(C, CS, I, CS);
			Request.EdgeXPosZNeg[I] = SampleLocal(C, CS, I, -1);
			Request.EdgeXNegZPos[I] = SampleLocal(C, -1, I, CS);
			Request.EdgeXNegZNeg[I] = SampleLocal(C, -1, I, -1);
			Request.EdgeYPosZPos[I] = SampleLocal(C, I, CS, CS);
			Request.EdgeYPosZNeg[I] = SampleLocal(C, I, CS, -1);
			Request.EdgeYNegZPos[I] = SampleLocal(C, I, -1, CS);
			Request.EdgeYNegZNeg[I] = SampleLocal(C, I, -1, -1);
		}

		Request.CornerXPosYPosZPos = SampleLocal(C, CS, CS, CS);
		Request.CornerXPosYPosZNeg = SampleLocal(C, CS, CS, -1);
		Request.CornerXPosYNegZPos = SampleLocal(C, CS, -1, CS);
		Request.CornerXPosYNegZNeg = SampleLocal(C, CS, -1, -1);
		Request.CornerXNegYPosZPos = SampleLocal(C, -1, CS, CS);
		Request.CornerXNegYPosZNeg = SampleLocal(C, -1, CS, -1);
		Request.CornerXNegYNegZPos = SampleLocal(C, -1, -1, CS);
		Request.CornerXNegYNegZNeg = SampleLocal(C, -1, -1, -1);

		Request.EdgeCornerFlags =
			FVoxelMeshingRequest::EDGE_XPOS_YPOS | FVoxelMeshingRequest::EDGE_XPOS_YNEG |
			FVoxelMeshingRequest::EDGE_XNEG_YPOS | FVoxelMeshingRequest::EDGE_XNEG_YNEG |
			FVoxelMeshingRequest::EDGE_XPOS_ZPOS | FVoxelMeshingRequest::EDGE_XPOS_ZNEG |
			FVoxelMeshingRequest::EDGE_XNEG_ZPOS | FVoxelMeshingRequest::EDGE_XNEG_ZNEG |
			FVoxelMeshingRequest::EDGE_YPOS_ZPOS | FVoxelMeshingRequest::EDGE_YPOS_ZNEG |
			FVoxelMeshingRequest::EDGE_YNEG_ZPOS | FVoxelMeshingRequest::EDGE_YNEG_ZNEG |
			FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZPOS | FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZNEG |
			FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZPOS | FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZNEG |
			FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZPOS | FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZNEG |
			FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZPOS | FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZNEG;
	}

	/** Mesher config matching production smooth meshing, with seams controllable. */
	FVoxelMeshingConfig MakeConfig(bool bUseTransvoxel)
	{
		FVoxelMeshingConfig Config;
		Config.bUseSmoothMeshing = true;
		Config.IsoLevel = 0.5f;
		Config.bGenerateUVs = true;
		Config.bCalculateAO = false;
		Config.bUseTransvoxel = bUseTransvoxel;
		// Skirts would drape extra geometry over the boundary plane and pollute
		// the watertightness measurement; production "seams off" symptoms in the
		// investigation are about the core mesh, not the skirt patches.
		Config.bGenerateSkirts = false;
		return Config;
	}

	/** Collect unique world-space vertices lying on the shared boundary plane. */
	TArray<FVector3f> CollectBoundaryVertices(const FChunkMeshData& Mesh, const FVector3f& ChunkWorldOffset)
	{
		TArray<FVector3f> Unique;
		for (const FVector3f& LocalPos : Mesh.Positions)
		{
			const FVector3f WorldPos = LocalPos + ChunkWorldOffset;
			if (FMath::Abs(WorldPos.X - BoundaryPlaneWorldX) > PlaneEpsilon)
			{
				continue;
			}

			bool bDuplicate = false;
			for (const FVector3f& Existing : Unique)
			{
				if ((Existing - WorldPos).SizeSquared() < DedupTolerance * DedupTolerance)
				{
					bDuplicate = true;
					break;
				}
			}
			if (!bDuplicate)
			{
				Unique.Add(WorldPos);
			}
		}
		return Unique;
	}

	/** Collect world-space triangle edges that lie entirely on the boundary plane. */
	TArray<TPair<FVector3f, FVector3f>> CollectBoundarySegments(const FChunkMeshData& Mesh, const FVector3f& ChunkWorldOffset)
	{
		TArray<TPair<FVector3f, FVector3f>> Segments;
		const int32 NumTris = Mesh.Indices.Num() / 3;
		for (int32 T = 0; T < NumTris; ++T)
		{
			for (int32 E = 0; E < 3; ++E)
			{
				const FVector3f P0 = Mesh.Positions[Mesh.Indices[T * 3 + E]] + ChunkWorldOffset;
				const FVector3f P1 = Mesh.Positions[Mesh.Indices[T * 3 + (E + 1) % 3]] + ChunkWorldOffset;
				if (FMath::Abs(P0.X - BoundaryPlaneWorldX) <= PlaneEpsilon &&
					FMath::Abs(P1.X - BoundaryPlaneWorldX) <= PlaneEpsilon &&
					(P1 - P0).SizeSquared() > DedupTolerance * DedupTolerance)
				{
					Segments.Emplace(P0, P1);
				}
			}
		}
		return Segments;
	}

	float DistanceToSegment(const FVector3f& P, const FVector3f& A, const FVector3f& B)
	{
		const FVector3f AB = B - A;
		const float LengthSq = AB.SizeSquared();
		if (LengthSq < KINDA_SMALL_NUMBER)
		{
			return (P - A).Size();
		}
		const float T = FMath::Clamp(FVector3f::DotProduct(P - A, AB) / LengthSq, 0.0f, 1.0f);
		return (P - (A + AB * T)).Size();
	}

	/** Watertightness measurement across the shared face. */
	struct FSeamReport
	{
		int32 NumA = 0;
		int32 NumB = 0;
		int32 UnmatchedA = 0;        // A verts with no B vert within MatchTolerance
		int32 UnmatchedB = 0;
		float MaxNearestVertex = 0;  // max over all verts of nearest opposite-side vertex distance
		float MaxCrack = 0;          // max over all verts of distance to nearest opposite-side seam segment

		bool IsWatertight() const
		{
			return NumA > 0 && NumB > 0 && UnmatchedA == 0 && UnmatchedB == 0;
		}
	};

	FSeamReport MeasureSeam(const FChunkMeshData& MeshA, const FChunkMeshData& MeshB)
	{
		const FVector3f OffsetA(0, 0, 0);
		const FVector3f OffsetB(BoundaryPlaneWorldX, 0, 0); // chunk B at (1,0,0)

		const TArray<FVector3f> VertsA = CollectBoundaryVertices(MeshA, OffsetA);
		const TArray<FVector3f> VertsB = CollectBoundaryVertices(MeshB, OffsetB);
		const TArray<TPair<FVector3f, FVector3f>> SegsA = CollectBoundarySegments(MeshA, OffsetA);
		const TArray<TPair<FVector3f, FVector3f>> SegsB = CollectBoundarySegments(MeshB, OffsetB);

		FSeamReport Report;
		Report.NumA = VertsA.Num();
		Report.NumB = VertsB.Num();

		auto Measure = [&Report](const TArray<FVector3f>& Verts, const TArray<FVector3f>& OtherVerts,
			const TArray<TPair<FVector3f, FVector3f>>& OtherSegs, int32& UnmatchedCount)
		{
			for (const FVector3f& V : Verts)
			{
				float NearestVert = FLT_MAX;
				for (const FVector3f& O : OtherVerts)
				{
					NearestVert = FMath::Min(NearestVert, (V - O).Size());
				}
				if (OtherVerts.Num() > 0)
				{
					Report.MaxNearestVertex = FMath::Max(Report.MaxNearestVertex, NearestVert);
				}
				if (NearestVert > MatchTolerance)
				{
					UnmatchedCount++;
				}

				float NearestSeg = FLT_MAX;
				for (const TPair<FVector3f, FVector3f>& S : OtherSegs)
				{
					NearestSeg = FMath::Min(NearestSeg, DistanceToSegment(V, S.Key, S.Value));
				}
				if (OtherSegs.Num() > 0)
				{
					Report.MaxCrack = FMath::Max(Report.MaxCrack, NearestSeg);
				}
			}
		};

		Measure(VertsA, VertsB, SegsB, Report.UnmatchedA);
		Measure(VertsB, VertsA, SegsA, Report.UnmatchedB);
		return Report;
	}

	FString DescribeSeam(const FSeamReport& R)
	{
		return FString::Printf(
			TEXT("boundary verts A:%d B:%d, unmatched A:%d B:%d, max nearest-vertex %.3f, max crack %.3f (units)"),
			R.NumA, R.NumB, R.UnmatchedA, R.UnmatchedB, R.MaxNearestVertex, R.MaxCrack);
	}


	/** Mesh one request with a fresh mesher; returns success. */
	bool MeshChunk(const FVoxelMeshingRequest& Request, bool bUseTransvoxel, FChunkMeshData& OutMesh)
	{
		FVoxelCPUMarchingCubesMesher Mesher;
		Mesher.Initialize();
		Mesher.SetConfig(MakeConfig(bUseTransvoxel));
		FVoxelMeshingStats Stats;
		const bool bOk = Mesher.GenerateMeshCPU(Request, OutMesh, Stats);
		Mesher.Shutdown();
		return bOk;
	}

	// ===== Generalized per-face transvoxel testing (T7) =====
	// T6 only exercised +X. To catch per-face orientation/mirror bugs in the
	// transition cell, run the same A(fine)|B(coarse) acceptance across every face.

	const TCHAR* FaceName(int32 Face)
	{
		static const TCHAR* Names[6] = { TEXT("-X"), TEXT("+X"), TEXT("-Y"), TEXT("+Y"), TEXT("-Z"), TEXT("+Z") };
		return Names[Face];
	}

	uint8 FaceTransitionFlag(int32 Face)
	{
		switch (Face)
		{
		case 0: return FVoxelMeshingRequest::TRANSITION_XNEG;
		case 1: return FVoxelMeshingRequest::TRANSITION_XPOS;
		case 2: return FVoxelMeshingRequest::TRANSITION_YNEG;
		case 3: return FVoxelMeshingRequest::TRANSITION_YPOS;
		case 4: return FVoxelMeshingRequest::TRANSITION_ZNEG;
		default: return FVoxelMeshingRequest::TRANSITION_ZPOS;
		}
	}

	FIntVector FaceOffset(int32 Face)
	{
		static const FIntVector Offs[6] = {
			FIntVector(-1,0,0), FIntVector(1,0,0),
			FIntVector(0,-1,0), FIntVector(0,1,0),
			FIntVector(0,0,-1), FIntVector(0,0,1),
		};
		return Offs[Face];
	}

	/** World-space coordinate of the shared plane along the face's axis, for chunk A at origin. */
	float FaceSharedPlaneCoord(int32 Face)
	{
		const int32 Axis = Face / 2;
		const bool bPositive = (Face % 2) == 1;
		return bPositive ? (TestChunkSize * TestVoxelSize) : 0.0f;
	}

	/** Collect unique world verts on the shared plane for an arbitrary face axis. */
	TArray<FVector3f> CollectBoundaryVertsAxis(const FChunkMeshData& Mesh, const FVector3f& WorldOffset, int32 Axis, float PlaneCoord)
	{
		TArray<FVector3f> Unique;
		for (const FVector3f& LocalPos : Mesh.Positions)
		{
			const FVector3f WorldPos = LocalPos + WorldOffset;
			if (FMath::Abs(WorldPos[Axis] - PlaneCoord) > PlaneEpsilon) { continue; }
			bool bDup = false;
			for (const FVector3f& E : Unique) { if ((E - WorldPos).SizeSquared() < DedupTolerance * DedupTolerance) { bDup = true; break; } }
			if (!bDup) { Unique.Add(WorldPos); }
		}
		return Unique;
	}

	/**
	 * Run the transvoxel acceptance for one face: A (fine, LODA) borders B (coarse,
	 * LODA+1) across Face, with A generating transition cells. Returns unmatched
	 * counts and max crack measured on the shared plane.
	 */
	FSeamReport RunFaceTransvoxelCase(int32 Face, int32 LODA)
	{
		const FIntVector Off = FaceOffset(Face);
		FVoxelMeshingRequest RequestA = MakeChunkRequest(FIntVector(0, 0, 0), LODA);
		FVoxelMeshingRequest RequestB = MakeChunkRequest(Off, LODA + 1);
		FillAllNeighborData(RequestA);
		FillAllNeighborData(RequestB);
		RequestA.NeighborLODLevels[Face] = LODA + 1;        // A's neighbor across Face is coarser B
		RequestB.NeighborLODLevels[Face ^ 1] = LODA;        // B's neighbor across opposite face is finer A
		RequestA.TransitionFaces = FaceTransitionFlag(Face); // A generates the transition strip

		FChunkMeshData MeshA, MeshB;
		MeshChunk(RequestA, true, MeshA);
		MeshChunk(RequestB, true, MeshB);

		const int32 Axis = Face / 2;
		const float Plane = FaceSharedPlaneCoord(Face);
		const FVector3f OffsetA(0, 0, 0);
		const FVector3f OffsetB = FVector3f(Off) * (TestChunkSize * TestVoxelSize);

		const TArray<FVector3f> VertsA = CollectBoundaryVertsAxis(MeshA, OffsetA, Axis, Plane);
		const TArray<FVector3f> VertsB = CollectBoundaryVertsAxis(MeshB, OffsetB, Axis, Plane);

		FSeamReport R;
		R.NumA = VertsA.Num();
		R.NumB = VertsB.Num();
		for (const FVector3f& B : VertsB)
		{
			float NV = FLT_MAX;
			for (const FVector3f& A : VertsA) { NV = FMath::Min(NV, (A - B).Size()); }
			if (VertsA.Num() > 0) { R.MaxNearestVertex = FMath::Max(R.MaxNearestVertex, NV); }
			if (NV > MatchTolerance) { R.UnmatchedB++; }
		}
		for (const FVector3f& A : VertsA)
		{
			float NV = FLT_MAX;
			for (const FVector3f& B : VertsB) { NV = FMath::Min(NV, (A - B).Size()); }
			if (NV > MatchTolerance) { R.UnmatchedA++; }
		}
		return R;
	}
} // namespace MarchingCubesLODBoundaryTestHelpers
using namespace MarchingCubesLODBoundaryTestHelpers;

// ==================== T1: both LOD0 (harness sanity baseline) ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCLODBoundaryT1BothLOD0Test, "VoxelWorlds.Meshing.MarchingCubes.LODBoundary.T1_BothLOD0",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCLODBoundaryT1BothLOD0Test::RunTest(const FString& Parameters)
{
	FVoxelMeshingRequest RequestA = MakeChunkRequest(FIntVector(0, 0, 0), 0);
	FVoxelMeshingRequest RequestB = MakeChunkRequest(FIntVector(1, 0, 0), 0);
	FillAllNeighborData(RequestA);
	FillAllNeighborData(RequestB);
	RequestA.NeighborLODLevels[1] = 0; // +X neighbor is B
	RequestB.NeighborLODLevels[0] = 0; // -X neighbor is A

	FChunkMeshData MeshA, MeshB;
	TestTrue(TEXT("Chunk A meshing should succeed"), MeshChunk(RequestA, false, MeshA));
	TestTrue(TEXT("Chunk B meshing should succeed"), MeshChunk(RequestB, false, MeshB));

	const FSeamReport Seam = MeasureSeam(MeshA, MeshB);
	AddInfo(FString::Printf(TEXT("T1 LOD0|LOD0: %s"), *DescribeSeam(Seam)));

	TestTrue(TEXT("Surface should cross the shared face on both sides"), Seam.NumA > 0 && Seam.NumB > 0);
	TestEqual(TEXT("All A boundary vertices should match B"), Seam.UnmatchedA, 0);
	TestEqual(TEXT("All B boundary vertices should match A"), Seam.UnmatchedB, 0);
	return true;
}

// ==================== T2: both LOD1, full slices, seams off ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCLODBoundaryT2BothLOD1Test, "VoxelWorlds.Meshing.MarchingCubes.LODBoundary.T2_BothLOD1",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCLODBoundaryT2BothLOD1Test::RunTest(const FString& Parameters)
{
	FVoxelMeshingRequest RequestA = MakeChunkRequest(FIntVector(0, 0, 0), 1);
	FVoxelMeshingRequest RequestB = MakeChunkRequest(FIntVector(1, 0, 0), 1);
	FillAllNeighborData(RequestA);
	FillAllNeighborData(RequestB);
	RequestA.NeighborLODLevels[1] = 1;
	RequestB.NeighborLODLevels[0] = 1;

	FChunkMeshData MeshA, MeshB;
	TestTrue(TEXT("Chunk A meshing should succeed"), MeshChunk(RequestA, false, MeshA));
	TestTrue(TEXT("Chunk B meshing should succeed"), MeshChunk(RequestB, false, MeshB));

	const FSeamReport Seam = MeasureSeam(MeshA, MeshB);
	AddInfo(FString::Printf(TEXT("T2 LOD1|LOD1: %s"), *DescribeSeam(Seam)));

	TestTrue(TEXT("Surface should cross the shared face on both sides"), Seam.NumA > 0 && Seam.NumB > 0);
	TestEqual(TEXT("All A boundary vertices should match B (stride-2 boundary meshing sound)"), Seam.UnmatchedA, 0);
	TestEqual(TEXT("All B boundary vertices should match A (stride-2 boundary meshing sound)"), Seam.UnmatchedB, 0);
	return true;
}

// ==================== T3: both LOD2, full slices, seams off ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCLODBoundaryT3BothLOD2Test, "VoxelWorlds.Meshing.MarchingCubes.LODBoundary.T3_BothLOD2",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCLODBoundaryT3BothLOD2Test::RunTest(const FString& Parameters)
{
	FVoxelMeshingRequest RequestA = MakeChunkRequest(FIntVector(0, 0, 0), 2);
	FVoxelMeshingRequest RequestB = MakeChunkRequest(FIntVector(1, 0, 0), 2);
	FillAllNeighborData(RequestA);
	FillAllNeighborData(RequestB);
	RequestA.NeighborLODLevels[1] = 2;
	RequestB.NeighborLODLevels[0] = 2;

	FChunkMeshData MeshA, MeshB;
	TestTrue(TEXT("Chunk A meshing should succeed"), MeshChunk(RequestA, false, MeshA));
	TestTrue(TEXT("Chunk B meshing should succeed"), MeshChunk(RequestB, false, MeshB));

	const FSeamReport Seam = MeasureSeam(MeshA, MeshB);
	AddInfo(FString::Printf(TEXT("T3 LOD2|LOD2: %s"), *DescribeSeam(Seam)));

	TestTrue(TEXT("Surface should cross the shared face on both sides"), Seam.NumA > 0 && Seam.NumB > 0);
	TestEqual(TEXT("All A boundary vertices should match B (stride-4 boundary meshing sound)"), Seam.UnmatchedA, 0);
	TestEqual(TEXT("All B boundary vertices should match A (stride-4 boundary meshing sound)"), Seam.UnmatchedB, 0);
	return true;
}

// ==================== T4: A LOD0, B LOD1, seams off (raw mismatch magnitude) ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCLODBoundaryT4MixedLODRawTest, "VoxelWorlds.Meshing.MarchingCubes.LODBoundary.T4_LOD0vsLOD1_SeamsOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCLODBoundaryT4MixedLODRawTest::RunTest(const FString& Parameters)
{
	FVoxelMeshingRequest RequestA = MakeChunkRequest(FIntVector(0, 0, 0), 0);
	FVoxelMeshingRequest RequestB = MakeChunkRequest(FIntVector(1, 0, 0), 1);
	FillAllNeighborData(RequestA);
	FillAllNeighborData(RequestB);
	RequestA.NeighborLODLevels[1] = 1;
	RequestB.NeighborLODLevels[0] = 0;

	FChunkMeshData MeshA, MeshB;
	TestTrue(TEXT("Chunk A meshing should succeed"), MeshChunk(RequestA, false, MeshA));
	TestTrue(TEXT("Chunk B meshing should succeed"), MeshChunk(RequestB, false, MeshB));

	const FSeamReport Seam = MeasureSeam(MeshA, MeshB);
	AddInfo(FString::Printf(TEXT("T4 LOD0|LOD1 seams off: %s"), *DescribeSeam(Seam)));

	const float FineCell = TestVoxelSize;        // 100
	const float CoarseCell = TestVoxelSize * 2;  // 200 (B's stride-2 cell)

	TestTrue(TEXT("Surface should cross the shared face on both sides"), Seam.NumA > 0 && Seam.NumB > 0);
	TestTrue(FString::Printf(TEXT("Raw LOD mismatch cracks should be <= 1 fine cell (%.0f), got %.3f"),
		FineCell, Seam.MaxCrack), Seam.MaxCrack <= FineCell);
	TestTrue(FString::Printf(TEXT("Cracks >= 1 coarse cell (%.0f) would indicate a mesher math bug, got %.3f"),
		CoarseCell, Seam.MaxCrack), Seam.MaxCrack < CoarseCell);
	return true;
}

// ==================== T5: both LOD1, neighbor arrays empty, gentle field (confirmed clamp hazard) ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCLODBoundaryT5NoNeighborDataTest, "VoxelWorlds.Meshing.MarchingCubes.LODBoundary.T5_LOD1_NoNeighborData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCLODBoundaryT5NoNeighborDataTest::RunTest(const FString& Parameters)
{
	// Same as T2 but neighbor face/edge/corner arrays are deliberately left
	// empty. This is the CONFIRMED production fallback: ExtractNeighborEdgeSlices
	// guards every slice with HasNeighborData, so a non-resident neighbor leaves
	// the slice empty (Num()==0). GetVoxelAt then clamps the out-of-bounds plane
	// to the chunk's own edge voxel (duplicate plane), NOT to Air. On this gentle
	// field the duplicated plane is nearly identical to the true plane, so the
	// displacement is small (~0.19 voxel) - it documents that the clamp hazard's
	// magnitude is terrain-dependent. See T5c for the gross (steep terrain) case.
	FVoxelMeshingRequest RequestA = MakeChunkRequest(FIntVector(0, 0, 0), 1);
	FVoxelMeshingRequest RequestB = MakeChunkRequest(FIntVector(1, 0, 0), 1);

	FChunkMeshData MeshA, MeshB;
	TestTrue(TEXT("Chunk A meshing should succeed"), MeshChunk(RequestA, false, MeshA));
	TestTrue(TEXT("Chunk B meshing should succeed"), MeshChunk(RequestB, false, MeshB));

	const FSeamReport Seam = MeasureSeam(MeshA, MeshB);
	AddInfo(FString::Printf(TEXT("T5 LOD1|LOD1 no neighbor data (gentle): %s"), *DescribeSeam(Seam)));

	// This documents the hazard: with no neighbor data, the boundary should NOT
	// be watertight (the clamp fallback displaces the iso-surface in boundary
	// cubes). If this ever starts matching, the fallback became benign and the
	// investigation notes should be updated.
	TestTrue(TEXT("Empty neighbor arrays should produce a displaced (non-watertight) boundary - documents the silent clamp fallback hazard"),
		!Seam.IsWatertight());
	AddInfo(FString::Printf(TEXT("T5 clamp displacement magnitude (gentle field): max crack %.3f units (%.2f voxels)"),
		Seam.MaxCrack, Seam.MaxCrack / TestVoxelSize));
	return true;
}

// ==================== T5c: both LOD1, neighbor arrays empty, cliff field (gross clamp hazard) ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCLODBoundaryT5cCliffClampTest, "VoxelWorlds.Meshing.MarchingCubes.LODBoundary.T5c_LOD1_NoNeighborData_Cliff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCLODBoundaryT5cCliffClampTest::RunTest(const FString& Parameters)
{
	// Same CONFIRMED empty-slice clamp path as T5, but over a steep field. The
	// clamp duplicates world x=3100 onto the x=3200 face; with ~250 units of
	// height change across that 100-unit span, the finer chunk's face vertices
	// land more than a full coarse (LOD1, 200-unit) cell away from the true
	// surface B meshes. This is the regime that produces the void-corridor /
	// exposed-cross-section tears seen in the field captures - and it shows the
	// clamp fallback alone (no Air-fill needed) is sufficient to cause them.
	FScopedTestField CliffField(ETestField::Cliff);

	FVoxelMeshingRequest RequestA = MakeChunkRequest(FIntVector(0, 0, 0), 1);
	FVoxelMeshingRequest RequestB = MakeChunkRequest(FIntVector(1, 0, 0), 1);

	FChunkMeshData MeshA, MeshB;
	TestTrue(TEXT("Chunk A meshing should succeed"), MeshChunk(RequestA, false, MeshA));
	TestTrue(TEXT("Chunk B meshing should succeed"), MeshChunk(RequestB, false, MeshB));

	const FSeamReport Seam = MeasureSeam(MeshA, MeshB);
	AddInfo(FString::Printf(TEXT("T5c LOD1|LOD1 no neighbor data (cliff): %s"), *DescribeSeam(Seam)));

	const float CoarseCell = TestVoxelSize * 2; // 200, B's LOD1 cell

	TestTrue(TEXT("Surface should cross the shared face on both sides"), Seam.NumA > 0 && Seam.NumB > 0);
	TestTrue(TEXT("Steep clamp fallback should be non-watertight"), !Seam.IsWatertight());
	TestTrue(FString::Printf(TEXT("Steep clamp fallback should produce a gross (>= 1 coarse cell, %.0f) tear, got %.1f"),
		CoarseCell, Seam.MaxCrack), Seam.MaxCrack >= CoarseCell);
	AddInfo(FString::Printf(TEXT("T5c clamp displacement magnitude (cliff field): max crack %.3f units (%.2f voxels) vs T5 gentle ~0.19 voxels"),
		Seam.MaxCrack, Seam.MaxCrack / TestVoxelSize));
	return true;
}

// ==================== T5b: both LOD1, +X slice Air-filled (resident-but-ungenerated race) ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCLODBoundaryT5bAirSliceTest, "VoxelWorlds.Meshing.MarchingCubes.LODBoundary.T5b_LOD1_AirSlice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCLODBoundaryT5bAirSliceTest::RunTest(const FString& Parameters)
{
	// PLAUSIBLE-but-unconfirmed production hazard (distinct from T5's confirmed
	// clamp path): a neighbor whose VoxelData array is allocated (so it passes
	// ExtractNeighborEdgeSlices' HasNeighborData check, Num()==VolumeSize) but
	// has not yet been populated by generation - its contents are still Air. The
	// extractor then copies a real-but-all-Air slice, and GetNeighborVoxel's
	// Air() branch (VoxelChunkManager.cpp:2779) is the same value. The mesher
	// meshes A's boundary cubes against a phantom air wall: A's surface retreats
	// from the shared face (void corridor) while B's sheet still reaches it
	// (exposed cross-section). Whether this race actually occurs at meshing time
	// is the open question for the live-repro instrumentation step (P1).
	FVoxelMeshingRequest RequestA = MakeChunkRequest(FIntVector(0, 0, 0), 1);
	FVoxelMeshingRequest RequestB = MakeChunkRequest(FIntVector(1, 0, 0), 1);
	FillAllNeighborData(RequestA);
	FillAllNeighborData(RequestB);
	RequestA.NeighborLODLevels[1] = 1;
	RequestB.NeighborLODLevels[0] = 1;

	// Overwrite only A's +X face slice with Air, as an allocated-but-ungenerated
	// (1,0,0) neighbor would produce. Edge/corner strips come from other chunks.
	for (FVoxelData& Voxel : RequestA.NeighborXPos)
	{
		Voxel = FVoxelData::Air();
	}

	FChunkMeshData MeshA, MeshB;
	TestTrue(TEXT("Chunk A meshing should succeed"), MeshChunk(RequestA, false, MeshA));
	TestTrue(TEXT("Chunk B meshing should succeed"), MeshChunk(RequestB, false, MeshB));

	const FSeamReport Seam = MeasureSeam(MeshA, MeshB);
	AddInfo(FString::Printf(TEXT("T5b LOD1|LOD1 air slice: %s"), *DescribeSeam(Seam)));

	// Count A vertices pulled into the last cell before the face: the phantom
	// air wall forces iso-crossings between plane 30 (solid below surface) and
	// plane 32 (air), creating a vertical wall of geometry inside A.
	int32 WallVerts = 0;
	for (const FVector3f& Pos : MeshA.Positions)
	{
		if (Pos.X > BoundaryPlaneWorldX - 2 * TestVoxelSize + PlaneEpsilon &&
			Pos.X < BoundaryPlaneWorldX - PlaneEpsilon)
		{
			WallVerts++;
		}
	}
	AddInfo(FString::Printf(TEXT("T5b: A vertices in the last cell before the face (wall band): %d"), WallVerts));

	// The air-wall signature: A meshes a vertical wall against the phantom air
	// (instead of continuing the terrain sheet across the face), leaving a gross
	// chunk-scale tear against B's sheet. A few stray vertices still land on the
	// plane where real edge-strip data contradicts the air'd face slice, so the
	// robust signature is the tear magnitude, not an empty plane.
	// If this ever becomes watertight, the hazard is gone and the investigation
	// notes should be updated.
	TestTrue(TEXT("Air-filled slice should produce a non-watertight boundary"), !Seam.IsWatertight() || Seam.MaxCrack > MatchTolerance);
	TestTrue(FString::Printf(TEXT("Air-filled slice should produce a gross (>= 1 coarse cell) tear, got %.1f"), Seam.MaxCrack),
		Seam.MaxCrack >= 2 * TestVoxelSize);
	TestTrue(TEXT("B should still reach the shared face (exposed cross-section)"), Seam.NumB > 0);
	return true;
}

// ==================== T6: A LOD0 + transition face, B LOD1 (transvoxel acceptance) ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCLODBoundaryT6TransvoxelTest, "VoxelWorlds.Meshing.MarchingCubes.LODBoundary.T6_LOD0vsLOD1_Transvoxel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCLODBoundaryT6TransvoxelTest::RunTest(const FString& Parameters)
{
	FVoxelMeshingRequest RequestA = MakeChunkRequest(FIntVector(0, 0, 0), 0);
	FVoxelMeshingRequest RequestB = MakeChunkRequest(FIntVector(1, 0, 0), 1);
	FillAllNeighborData(RequestA);
	FillAllNeighborData(RequestB);
	RequestA.NeighborLODLevels[1] = 1;
	RequestB.NeighborLODLevels[0] = 0;
	// The finer chunk (A) generates transition cells on the face bordering the
	// coarser neighbor, exactly as the chunk manager sets it.
	RequestA.TransitionFaces = FVoxelMeshingRequest::TRANSITION_XPOS;

	FChunkMeshData MeshA, MeshB;
	TestTrue(TEXT("Chunk A meshing should succeed"), MeshChunk(RequestA, true, MeshA));
	TestTrue(TEXT("Chunk B meshing should succeed"), MeshChunk(RequestB, true, MeshB));

	const FSeamReport Seam = MeasureSeam(MeshA, MeshB);
	AddInfo(FString::Printf(TEXT("T6 LOD0|LOD1 transvoxel: %s"), *DescribeSeam(Seam)));

	// Transvoxel acceptance (gentle/linear-Z field):
	// - Every coarse-neighbor vertex must be reproduced by A's transition strip
	//   (UnmatchedB == 0): the strip aligns with the neighbor — no gap with B.
	// - The boundary must be geometrically watertight (max crack well under one
	//   voxel): P3's Pass-2 fix cut this from ~28 to ~0.58 units.
	// KNOWN RESIDUAL: A still emits ~2x B's vertex count — zero-gap T-junctions at
	// the midpoints of B's coarse contour segments (UnmatchedA > 0). These do not
	// open a gap; eliminating them needs the coarse-2x2 boundary-face restructure
	// (see LOD_SEAM_INVESTIGATION.md, P3). NOTE: this is NOT the gross steep-terrain
	// LOD tearing seen live — that reproduces only with a non-linear field and is a
	// separate open issue.
	TestTrue(TEXT("Surface should cross the shared face on both sides"), Seam.NumA > 0 && Seam.NumB > 0);
	TestEqual(TEXT("Transvoxel: all B (coarse) boundary vertices should be matched by A's transition strip"), Seam.UnmatchedB, 0);
	TestTrue(FString::Printf(TEXT("Transvoxel boundary should be geometrically watertight (max crack <= 1.0, got %.3f)"), Seam.MaxCrack),
		Seam.MaxCrack <= 1.0f);
	AddInfo(FString::Printf(TEXT("T6 known residual T-junctions (extra fine verts on B's edges, zero-gap): %d"), Seam.UnmatchedA));
	return true;
}

// ==================== T7: per-face transvoxel acceptance (catches orientation/mirror bugs) ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCLODBoundaryT7PerFaceTest, "VoxelWorlds.Meshing.MarchingCubes.LODBoundary.T7_PerFaceTransvoxel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCLODBoundaryT7PerFaceTest::RunTest(const FString& Parameters)
{
	// T6 only tested +X. A per-face orientation/mirror bug would show as some faces
	// matching (UnmatchedB low) and others not. The smooth field (Z = H(x,y)) crosses
	// the ±X and ±Y shared planes; ±Z planes are not reliably crossed by this field,
	// so they are covered separately if needed. We focus on UnmatchedB (coarse-side
	// mismatch) — the signature of a mirrored/reversed transition strip.
	const int32 Faces[4] = { 0, 1, 2, 3 }; // -X, +X, -Y, +Y
	bool bAnyMirror = false;
	for (int32 Face : Faces)
	{
		const FSeamReport R = RunFaceTransvoxelCase(Face, 0);
		AddInfo(FString::Printf(TEXT("T7 face %s: vertsA=%d vertsB=%d unmatchedA=%d unmatchedB=%d maxNearestVert=%.2f"),
			FaceName(Face), R.NumA, R.NumB, R.UnmatchedA, R.UnmatchedB, R.MaxNearestVertex));

		if (R.NumA == 0 || R.NumB == 0)
		{
			AddInfo(FString::Printf(TEXT("  (face %s: surface does not cross this shared plane with smooth field - skipped)"), FaceName(Face)));
			continue;
		}

		// UnmatchedB > 0 means coarse-side vertices have no fine-side match within
		// tolerance — i.e. the transition strip does not align with the neighbor
		// (the mirror/reversal symptom).
		if (R.UnmatchedB > 0)
		{
			bAnyMirror = true;
			AddError(FString::Printf(TEXT("Face %s: %d/%d coarse-side verts UNMATCHED by transition strip (maxNearestVert=%.1f) - misaligned/mirrored boundary"),
				FaceName(Face), R.UnmatchedB, R.NumB, R.MaxNearestVertex));
		}
	}

	TestFalse(TEXT("No face should have a mirrored/misaligned transition boundary"), bAnyMirror);
	return true;
}
