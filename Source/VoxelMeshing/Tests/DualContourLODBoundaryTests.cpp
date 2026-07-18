// Copyright Daniel Raquel. All Rights Reserved.

// Two-chunk watertightness tests for Dual Contouring LOD-boundary meshing.
//
// Mirror of MarchingCubesLODBoundaryTests.cpp but for the Dual Contouring
// mesher. Builds two adjacent 32^3 chunks A (chunk coord 0,0,0) and B (1,0,0)
// from the same analytic world-space density field, fills neighbor
// face/edge/corner data exactly the way UVoxelChunkManager::ExtractNeighborEdgeSlices
// does, meshes both chunks, and checks the geometry on the shared face
// (world x = ChunkSize * VoxelSize) is watertight across an LOD transition.
//
// DC handles LOD boundaries by MERGING the finer chunk's boundary cells
// (Pass 3.5, FVoxelCPUDualContourMesher::MergeLODBoundaryCells) into one vertex
// per coarse-neighbour cell, instead of MC's Transvoxel transition strip. The
// merge engages on the FINER chunk whenever NeighborLODLevels[face] > LODLevel.
//
// The reused MC analytic fields (Smooth / NonLinearZ / Cliff) are mesher-
// agnostic. The seam metric is combined-mesh: A and B are fused into one mesh
// and single-use (open) edges near the shared plane that are NOT on the outer
// combined box are counted. This is robust to DC's QEF-solved vertices floating
// off the boundary plane (unlike MC, DC has no on-plane boundary vertices).
//
//   DT1  both LOD0, full slices            -> harness sanity baseline
//   DT2  both LOD1 (stride 2), full slices  -> is same-LOD strided DC watertight?
//   DT3  both LOD2 (stride 4), full slices  -> same, at stride 4
//   DT4  A fine / B coarse, merge engaged   -> the LOD boundary, per field/LOD pair
//                                              (expected GREEN on Smooth,
//                                               RED on NonLinearZ/Cliff = repro)

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelGPUDualContourMesher.h"
#include "VoxelMeshingTypes.h"
#include "VoxelData.h"
#include "ChunkRenderData.h"
#include "HAL/IConsoleManager.h"
#include "RenderingThread.h" // FlushRenderingCommands — driving the GPU mesher's async readback
#include "RHI.h"             // GUsingNullRHI — skip GPU tests under -nullrhi

namespace DualContourLODBoundaryTestHelpers
{
	constexpr int32 TestChunkSize = 32;
	constexpr float TestVoxelSize = 100.0f;
	constexpr float BoundaryPlaneWorldX = TestChunkSize * TestVoxelSize; // 3200

	/** Two vertices closer than this (world units) are treated as the same vertex. */
	constexpr float FuseTolerance = 0.5f;
	/** Face-plane membership epsilon for the outer combined box. */
	constexpr float FaceEpsilon = 0.5f;

	/**
	 * Accepted coarse-side residual at LOD-patch edges/corners.
	 *
	 * WeldStridedBoundaryCells seals the INTERIOR of every stride>1 boundary face
	 * exactly (both neighbors derive the shared vertex from the same face-plane
	 * data). The residual is at most a couple of cells where two boundary faces
	 * meet — a chunk edge crossing the LOD boundary — at which point two
	 * independently-meshed chunks of different LOD make slightly different
	 * decisions and don't emit perfectly matching geometry. This is the same class
	 * of corner residual the Marching Cubes fix accepted as a known limitation; a
	 * bit-exact result requires the deferred stride-deep neighbor-data fix (planned
	 * for the streaming-performance pass). The tests gate that the dominant seam is
	 * sealed (a gross regression would leave most/all of NumB unmatched) while
	 * tolerating this small documented edge residual.
	 *
	 * NOTE: the mesher-local weld is a partial mitigation — it cannot fully seal
	 * stride>1 boundaries because a DC boundary-plane quad needs correct data on
	 * BOTH sides, but only one neighbour plane is supplied (see DT6). Full
	 * watertightness needs the deferred stride-deep neighbour-data fix. Until then,
	 * the per-face vertex residual sits at a few cells; gate generously.
	 */
	constexpr int32 AcceptedEdgeResidual = 3;

	// ---- Analytic field (identical to the MC harness) -----------------------

	enum class ETestField { Smooth, Cliff, NonLinearZ };
	static ETestField GActiveField = ETestField::Smooth;

	/** RAII guard that selects a field for the duration of a test and restores Smooth. */
	struct FScopedTestField
	{
		explicit FScopedTestField(ETestField Field) { GActiveField = Field; }
		~FScopedTestField() { GActiveField = ETestField::Smooth; }
	};

	float SurfaceHeight(float WorldX, float WorldY)
	{
		if (GActiveField == ETestField::Cliff)
		{
			return 1600.0f + 2.5f * (WorldX - 3100.0f) + 40.0f * FMath::Sin(WorldY * 0.01f);
		}
		return 1600.0f + 0.15f * WorldX + 0.1f * WorldY + 60.0f * FMath::Sin(WorldY * 0.01f);
	}

	FVoxelData SampleField(const FVector& WorldPos)
	{
		const float H = SurfaceHeight(WorldPos.X, WorldPos.Y);
		float Normalized;
		if (GActiveField == ETestField::NonLinearZ)
		{
			// tanh profile: monotonic through 0.5 at Z=H, strongly curved near the
			// surface so coarse strides land the iso-crossing at a different height
			// than fine strides — the real-terrain LOD-mismatch condition.
			Normalized = 0.5f + 0.5f * FMath::Tanh((H - static_cast<float>(WorldPos.Z)) / 300.0f);
		}
		else
		{
			Normalized = 0.5f + (H - static_cast<float>(WorldPos.Z)) / 1600.0f;
		}
		Normalized = FMath::Clamp(Normalized, 0.0f, 1.0f);

		FVoxelData Voxel;
		Voxel.Density = static_cast<uint8>(FMath::RoundToInt(Normalized * 255.0f));
		Voxel.MaterialID = 1;
		Voxel.BiomeID = 0;
		Voxel.Metadata = 0;
		return Voxel;
	}

	FVoxelData SampleLocal(const FIntVector& ChunkCoord, int32 X, int32 Y, int32 Z)
	{
		const FVector ChunkOrigin = FVector(ChunkCoord) * TestChunkSize * TestVoxelSize;
		return SampleField(ChunkOrigin + FVector(X, Y, Z) * TestVoxelSize);
	}

	// ---- Request builders (identical layout to the MC harness) --------------

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

	void FillAllNeighborData(FVoxelMeshingRequest& Request)
	{
		const FIntVector C = Request.ChunkCoord;
		const int32 CS = TestChunkSize;
		const int32 SliceSize = CS * CS;
		// Plane 0 (the single face slice) is at depth -1 / +ChunkSize, exactly as the
		// legacy extraction supplies it. The DEEP planes (-2.. and +ChunkSize+1..) are
		// filled into the Neighbor*Deep arrays below, modelling the multi-plane
		// ExtractNeighborEdgeSlices fix (D = 2*stride faces for LOD>0).
		const int32 NStride = 1;

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
				Request.NeighborXNeg[Index] = SampleLocal(C, -NStride, A, B);
				Request.NeighborYPos[Index] = SampleLocal(C, A, CS, B); // [X + Z*CS]
				Request.NeighborYNeg[Index] = SampleLocal(C, A, -NStride, B);
				Request.NeighborZPos[Index] = SampleLocal(C, A, B, CS); // [X + Y*CS]
				Request.NeighborZNeg[Index] = SampleLocal(C, A, B, -NStride);
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
			Request.EdgeXPosYNeg[I] = SampleLocal(C, CS, -NStride, I);
			Request.EdgeXNegYPos[I] = SampleLocal(C, -NStride, CS, I);
			Request.EdgeXNegYNeg[I] = SampleLocal(C, -NStride, -NStride, I);
			Request.EdgeXPosZPos[I] = SampleLocal(C, CS, I, CS);
			Request.EdgeXPosZNeg[I] = SampleLocal(C, CS, I, -NStride);
			Request.EdgeXNegZPos[I] = SampleLocal(C, -NStride, I, CS);
			Request.EdgeXNegZNeg[I] = SampleLocal(C, -NStride, I, -NStride);
			Request.EdgeYPosZPos[I] = SampleLocal(C, I, CS, CS);
			Request.EdgeYPosZNeg[I] = SampleLocal(C, I, CS, -NStride);
			Request.EdgeYNegZPos[I] = SampleLocal(C, I, -NStride, CS);
			Request.EdgeYNegZNeg[I] = SampleLocal(C, I, -NStride, -NStride);
		}

		Request.CornerXPosYPosZPos = SampleLocal(C, CS, CS, CS);
		Request.CornerXPosYPosZNeg = SampleLocal(C, CS, CS, -NStride);
		Request.CornerXPosYNegZPos = SampleLocal(C, CS, -NStride, CS);
		Request.CornerXPosYNegZNeg = SampleLocal(C, CS, -NStride, -NStride);
		Request.CornerXNegYPosZPos = SampleLocal(C, -NStride, CS, CS);
		Request.CornerXNegYPosZNeg = SampleLocal(C, -NStride, CS, -NStride);
		Request.CornerXNegYNegZPos = SampleLocal(C, -NStride, -NStride, CS);
		Request.CornerXNegYNegZNeg = SampleLocal(C, -NStride, -NStride, -NStride);

		// Deep face planes (models ExtractNeighborEdgeSlices' multi-plane fill): D =
		// 2*stride for LOD>0 (faces only this step), so ExtraPlanes = 2*stride-1. Plane
		// k is one voxel deeper than the previous; in-plane index a + b*CS matches the
		// plane-0 layout for each face.
		const int32 DStride = 1 << FMath::Clamp(Request.LODLevel, 0, 7);
		const int32 ExtraPlanes = (DStride > 1) ? FMath::Min(2 * DStride - 1, CS - 1) : 0;
		Request.NeighborPlaneDepth = ExtraPlanes + 1;
		if (ExtraPlanes > 0)
		{
			Request.NeighborXPosDeep.SetNumUninitialized(ExtraPlanes * SliceSize);
			Request.NeighborXNegDeep.SetNumUninitialized(ExtraPlanes * SliceSize);
			Request.NeighborYPosDeep.SetNumUninitialized(ExtraPlanes * SliceSize);
			Request.NeighborYNegDeep.SetNumUninitialized(ExtraPlanes * SliceSize);
			Request.NeighborZPosDeep.SetNumUninitialized(ExtraPlanes * SliceSize);
			Request.NeighborZNegDeep.SetNumUninitialized(ExtraPlanes * SliceSize);
			for (int32 K = 0; K < ExtraPlanes; ++K)
			{
				for (int32 B = 0; B < CS; ++B)
				{
					for (int32 A = 0; A < CS; ++A)
					{
						const int32 Idx = K * SliceSize + A + B * CS;
						Request.NeighborXPosDeep[Idx] = SampleLocal(C, CS + 1 + K, A, B);
						Request.NeighborXNegDeep[Idx] = SampleLocal(C, -(K + 2), A, B);
						Request.NeighborYPosDeep[Idx] = SampleLocal(C, A, CS + 1 + K, B);
						Request.NeighborYNegDeep[Idx] = SampleLocal(C, A, -(K + 2), B);
						Request.NeighborZPosDeep[Idx] = SampleLocal(C, A, B, CS + 1 + K);
						Request.NeighborZNegDeep[Idx] = SampleLocal(C, A, B, -(K + 2));
					}
				}
			}

			// Deep edge data (D x D grid of free-axis strips) and deep corner data
			// (D x D x D box), modelling ExtractNeighborEdgeSlices' FillEdgeDeep/FillCornerDeep.
			// Coordinate per pinned axis: +axis -> CS + depth, -axis -> -1 - depth (0-based),
			// matching the base Edge*/Corner* sampling at depth 0. Layout matches
			// FVoxelMeshingRequest::EdgeDeepVoxel/CornerDeepVoxel.
			const int32 D = ExtraPlanes + 1;
			auto FillEdgeDeepT = [&](TArray<FVoxelData>& Deep, int32 AxisA, bool bPosA, int32 AxisB, bool bPosB, int32 FreeAxis)
			{
				Deep.SetNumUninitialized(D * D * CS);
				for (int32 a = 0; a < D; ++a)
				{
					const int32 CA = bPosA ? (CS + a) : (-1 - a);
					for (int32 b = 0; b < D; ++b)
					{
						const int32 CB = bPosB ? (CS + b) : (-1 - b);
						for (int32 f = 0; f < CS; ++f)
						{
							int32 W[3] = { 0, 0, 0 };
							W[AxisA] = CA; W[AxisB] = CB; W[FreeAxis] = f;
							Deep[(a * D + b) * CS + f] = SampleLocal(C, W[0], W[1], W[2]);
						}
					}
				}
			};
			FillEdgeDeepT(Request.EdgeXPosYPosDeep, 0, true, 1, true, 2);
			FillEdgeDeepT(Request.EdgeXPosYNegDeep, 0, true, 1, false, 2);
			FillEdgeDeepT(Request.EdgeXNegYPosDeep, 0, false, 1, true, 2);
			FillEdgeDeepT(Request.EdgeXNegYNegDeep, 0, false, 1, false, 2);
			FillEdgeDeepT(Request.EdgeXPosZPosDeep, 0, true, 2, true, 1);
			FillEdgeDeepT(Request.EdgeXPosZNegDeep, 0, true, 2, false, 1);
			FillEdgeDeepT(Request.EdgeXNegZPosDeep, 0, false, 2, true, 1);
			FillEdgeDeepT(Request.EdgeXNegZNegDeep, 0, false, 2, false, 1);
			FillEdgeDeepT(Request.EdgeYPosZPosDeep, 1, true, 2, true, 0);
			FillEdgeDeepT(Request.EdgeYPosZNegDeep, 1, true, 2, false, 0);
			FillEdgeDeepT(Request.EdgeYNegZPosDeep, 1, false, 2, true, 0);
			FillEdgeDeepT(Request.EdgeYNegZNegDeep, 1, false, 2, false, 0);

			auto FillCornerDeepT = [&](TArray<FVoxelData>& Deep, bool bPosX, bool bPosY, bool bPosZ)
			{
				Deep.SetNumUninitialized(D * D * D);
				for (int32 a = 0; a < D; ++a)
				{
					const int32 CX = bPosX ? (CS + a) : (-1 - a);
					for (int32 b = 0; b < D; ++b)
					{
						const int32 CY = bPosY ? (CS + b) : (-1 - b);
						for (int32 c = 0; c < D; ++c)
						{
							const int32 CZ = bPosZ ? (CS + c) : (-1 - c);
							Deep[(a * D + b) * D + c] = SampleLocal(C, CX, CY, CZ);
						}
					}
				}
			};
			FillCornerDeepT(Request.CornerXPosYPosZPosDeep, true, true, true);
			FillCornerDeepT(Request.CornerXPosYPosZNegDeep, true, true, false);
			FillCornerDeepT(Request.CornerXPosYNegZPosDeep, true, false, true);
			FillCornerDeepT(Request.CornerXPosYNegZNegDeep, true, false, false);
			FillCornerDeepT(Request.CornerXNegYPosZPosDeep, false, true, true);
			FillCornerDeepT(Request.CornerXNegYPosZNegDeep, false, true, false);
			FillCornerDeepT(Request.CornerXNegYNegZPosDeep, false, false, true);
			FillCornerDeepT(Request.CornerXNegYNegZNegDeep, false, false, false);
		}

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

	/** DC mesher config: smooth meshing, default QEF params, skirts off. */
	FVoxelMeshingConfig MakeConfig()
	{
		FVoxelMeshingConfig Config;
		Config.bUseSmoothMeshing = true;
		Config.IsoLevel = 0.5f;
		Config.bGenerateUVs = true;
		Config.bCalculateAO = false;
		// Skirts would drape extra geometry over the boundary plane and mask the
		// real watertightness of the core mesh — measure the core mesh only.
		Config.bGenerateSkirts = false;
		Config.QEFSVDThreshold = 0.1f;
		Config.QEFBiasStrength = 0.5f;
		return Config;
	}

	bool MeshChunk(const FVoxelMeshingRequest& Request, FChunkMeshData& OutMesh)
	{
		FVoxelCPUDualContourMesher Mesher;
		Mesher.Initialize();
		Mesher.SetConfig(MakeConfig());
		FVoxelMeshingStats Stats;
		const bool bOk = Mesher.GenerateMeshCPU(Request, OutMesh, Stats);
		Mesher.Shutdown();
		return bOk;
	}

	/**
	 * GPU equivalent of MeshChunk: dispatch the GPU Dual Contouring mesher and block until
	 * its async readback completes, then hand back the CPU-side mesh. Same pattern as the GPU
	 * tests in MarchingCubesMeshingTests (GenerateMeshAsync -> Flush+Tick+Sleep
	 * poll -> ReadbackToCPU). The DC weld cvar is read on the game thread inside
	 * GenerateMeshAsync, so an enclosing FScopedWeld is captured correctly. Requires a real
	 * RHI — callers gate on GUsingNullRHI before invoking this. Same signature as MeshChunk so
	 * the shared block helpers and metrics drive either mesher via a function pointer.
	 */
	bool MeshChunkGPU(const FVoxelMeshingRequest& Request, FChunkMeshData& OutMesh)
	{
		FVoxelGPUDualContourMesher Mesher;
		Mesher.Initialize();
		Mesher.SetConfig(MakeConfig());

		volatile bool bCompleted = false;
		volatile bool bSucceeded = false;
		FVoxelMeshingHandle Handle = Mesher.GenerateMeshAsync(Request,
			FOnVoxelMeshingComplete::CreateLambda([&bCompleted, &bSucceeded](FVoxelMeshingHandle, bool bSuccess)
			{
				bSucceeded = bSuccess;
				bCompleted = true;
			}));

		if (!Handle.IsValid())
		{
			Mesher.Shutdown();
			return false;
		}

		// Pump to completion. FlushRenderingCommands runs the queued compute + readback-copy
		// commands; Mesher.Tick advances the async readback state machine (WaitingForCounters
		// -> ... -> Complete) and fires the completion callback. The engine's normal tick loop
		// does NOT run during a synchronous RunTest, so we must tick the mesher ourselves —
		// the DC and MC GPU meshers' readbacks are both Tick-driven, so flushing alone never
		// completes them.
		const double StartTime = FPlatformTime::Seconds();
		// 30s, not 10s: the VERY FIRST GPU DC dispatch in a process pays a cold-start cost
		// (compute PSO creation for all DC passes, driver shader-cache warmup) that is several
		// seconds and grows whenever the .usf changed and the shader was just recompiled. The
		// first chunk meshed by the first GT* test was exceeding a 10s budget and returning empty
		// (maxDist=FLT_MAX), a harness flake unrelated to mesh correctness. Every subsequent
		// dispatch is sub-100ms, so the larger budget only affects the cold first call.
		const double TimeoutSeconds = 30.0;
		while (!bCompleted && (FPlatformTime::Seconds() - StartTime) < TimeoutSeconds)
		{
			FlushRenderingCommands();
			Mesher.Tick(0.0f);
			FPlatformProcess::Sleep(0.002f);
		}

		bool bOk = bCompleted && bSucceeded && Mesher.ReadbackToCPU(Handle, OutMesh);
		Mesher.ReleaseHandle(Handle);
		Mesher.Shutdown();
		return bOk;
	}

	/** Mesh-production function pointer: lets the shared block helpers/metrics run on either
	 *  the CPU (&MeshChunk) or GPU (&MeshChunkGPU) mesher. */
	using FDCMeshFn = bool(*)(const FVoxelMeshingRequest&, FChunkMeshData&);

	// ---- Seam watertightness metric (combined-mesh open edges) --------------

	bool OnOuterBox(const FVector3f& P)
	{
		const float Lo = 0.0f;
		const float YZHi = TestChunkSize * TestVoxelSize;       // 3200
		const float XHi = 2.0f * TestChunkSize * TestVoxelSize; // 6400 (A+B span)
		return FMath::Abs(P.X - Lo) < FaceEpsilon || FMath::Abs(P.X - XHi) < FaceEpsilon
			|| FMath::Abs(P.Y - Lo) < FaceEpsilon || FMath::Abs(P.Y - YZHi) < FaceEpsilon
			|| FMath::Abs(P.Z - Lo) < FaceEpsilon || FMath::Abs(P.Z - YZHi) < FaceEpsilon;
	}

	/**
	 * Fuse A (offset 0) and B (offset +X) into one mesh and count single-use
	 * (open) edges that are NOT on the outer combined box. The two chunks' only
	 * shared (interior-to-the-combined-box) plane is x = ChunkSize*VoxelSize, so
	 * any open edge counted here is a crack along that seam: when the boundary is
	 * watertight A's +X rim and B's -X rim fuse into manifold (2-use) edges; when
	 * cracked they stay single-use. Degenerate triangles are dropped first.
	 *
	 * @param OutNearPlane receives the subset of open edges whose midpoint X is
	 *        within BandHalfWidth of the shared plane (isolates the seam from any
	 *        unrelated internal artifact).
	 */
	int32 CountSeamOpenEdges(const FChunkMeshData& MeshA, const FChunkMeshData& MeshB,
		float BandHalfWidth, int32& OutNearPlane)
	{
		TArray<FVector3f> Verts;
		TArray<int32> Tris;

		auto AddMesh = [&](const FChunkMeshData& M, const FVector3f& Off)
		{
			TArray<int32> Remap;
			Remap.SetNum(M.Positions.Num());
			for (int32 i = 0; i < M.Positions.Num(); ++i)
			{
				const FVector3f P = M.Positions[i] + Off;
				int32 Found = INDEX_NONE;
				for (int32 v = 0; v < Verts.Num(); ++v)
				{
					if ((Verts[v] - P).SizeSquared() < FuseTolerance * FuseTolerance) { Found = v; break; }
				}
				if (Found == INDEX_NONE) { Found = Verts.Add(P); }
				Remap[i] = Found;
			}
			const int32 NumTris = M.Indices.Num() / 3;
			for (int32 t = 0; t < NumTris; ++t)
			{
				const FVector3f& P0 = M.Positions[M.Indices[t * 3 + 0]];
				const FVector3f& P1 = M.Positions[M.Indices[t * 3 + 1]];
				const FVector3f& P2 = M.Positions[M.Indices[t * 3 + 2]];
				const float Area = 0.5f * FVector3f::CrossProduct(P1 - P0, P2 - P0).Size();
				if (Area < 1.0f) { continue; } // drop degenerate
				Tris.Add(Remap[M.Indices[t * 3 + 0]]);
				Tris.Add(Remap[M.Indices[t * 3 + 1]]);
				Tris.Add(Remap[M.Indices[t * 3 + 2]]);
			}
		};
		AddMesh(MeshA, FVector3f(0, 0, 0));
		AddMesh(MeshB, FVector3f(BoundaryPlaneWorldX, 0, 0));

		auto EdgeKey = [](int32 a, int32 b) -> uint64
		{
			const uint32 lo = static_cast<uint32>(FMath::Min(a, b));
			const uint32 hi = static_cast<uint32>(FMath::Max(a, b));
			return (static_cast<uint64>(lo) << 32) | hi;
		};

		TMap<uint64, int32> EdgeCount;
		const int32 NumTris = Tris.Num() / 3;
		for (int32 t = 0; t < NumTris; ++t)
		{
			const int32 a = Tris[t * 3 + 0];
			const int32 b = Tris[t * 3 + 1];
			const int32 c = Tris[t * 3 + 2];
			EdgeCount.FindOrAdd(EdgeKey(a, b))++;
			EdgeCount.FindOrAdd(EdgeKey(b, c))++;
			EdgeCount.FindOrAdd(EdgeKey(c, a))++;
		}

		int32 OpenEdges = 0;
		OutNearPlane = 0;
		for (const TPair<uint64, int32>& KV : EdgeCount)
		{
			if (KV.Value != 1) { continue; }
			const int32 a = static_cast<int32>(KV.Key >> 32);
			const int32 b = static_cast<int32>(KV.Key & 0xffffffffu);
			if (OnOuterBox(Verts[a]) && OnOuterBox(Verts[b])) { continue; }
			++OpenEdges;
			const float MidX = 0.5f * (Verts[a].X + Verts[b].X);
			if (FMath::Abs(MidX - BoundaryPlaneWorldX) <= BandHalfWidth) { ++OutNearPlane; }
		}
		return OpenEdges;
	}

	/** Set true by a test to dump worst-unmatched-vertex diagnostics from MeasureCoarseSideMatch. */
	static bool GDumpWorstUnmatched = false;

	/**
	 * Coarse-side vertex match: every coarse-neighbour (B) boundary vertex must
	 * have a coincident vertex on A. B's boundary cells are its CX=-1 coarse
	 * layer (world X in (Shared - CoarserCellWorld, Shared]); A merges its fine
	 * boundary cells to produce one vertex per coarse cell that must land on them.
	 * Reports how many B boundary verts have no A vertex within MatchTol, and the
	 * worst distance.
	 */
	void MeasureCoarseSideMatch(const FChunkMeshData& MeshA, const FChunkMeshData& MeshB,
		float CoarserCellWorld, float MatchTol, int32& OutNumB, int32& OutUnmatchedB, float& OutMaxDist)
	{
		const float Shared = BoundaryPlaneWorldX;
		// B boundary verts: world X just below the shared plane, within one coarse cell.
		TArray<FVector3f> BBoundary;
		for (const FVector3f& Local : MeshB.Positions)
		{
			const FVector3f W = Local + FVector3f(Shared, 0, 0);
			if (W.X < Shared - CoarserCellWorld - 0.5f || W.X > Shared + 0.5f) { continue; }
			bool bDup = false;
			for (const FVector3f& E : BBoundary) { if ((E - W).SizeSquared() < 0.01f) { bDup = true; break; } }
			if (!bDup) { BBoundary.Add(W); }
		}

		OutNumB = BBoundary.Num();
		OutUnmatchedB = 0;
		OutMaxDist = 0.0f;
		FVector3f WorstB = FVector3f::ZeroVector, WorstNearestA = FVector3f::ZeroVector;
		for (const FVector3f& B : BBoundary)
		{
			float Nearest = FLT_MAX;
			FVector3f NearestA = FVector3f::ZeroVector;
			for (const FVector3f& A : MeshA.Positions)
			{
				const float D = (A - B).Size(); // A is at world offset 0
				if (D < Nearest) { Nearest = D; NearestA = A; }
			}
			if (Nearest > OutMaxDist) { OutMaxDist = Nearest; WorstB = B; WorstNearestA = NearestA; }
			if (Nearest > MatchTol) { ++OutUnmatchedB; }
		}
		if (GDumpWorstUnmatched && OutUnmatchedB > 0)
		{
			UE_LOG(LogTemp, Display, TEXT("  [DCdump] worst B=(%.1f,%.1f,%.1f) nearestA=(%.1f,%.1f,%.1f) dist=%.1f  (NumB=%d unmatched=%d)"),
				WorstB.X, WorstB.Y, WorstB.Z, WorstNearestA.X, WorstNearestA.Y, WorstNearestA.Z, OutMaxDist, OutNumB, OutUnmatchedB);
		}
	}

	/**
	 * Count interior open edges (hole rims) in a single chunk's mesh. An edge used by
	 * exactly one non-degenerate triangle is "open"; if it does NOT lie on a chunk face
	 * (the 6 planes coord==0 / coord==ChunkSize*VoxelSize) it is the rim of a HOLE
	 * (missing geometry), which vertex-matching cannot detect. Degenerate triangles are
	 * dropped first. Mesh positions are chunk-local.
	 */
	int32 CountInteriorOpenEdges(const FChunkMeshData& Mesh)
	{
		const float Lo = 0.0f;
		const float Hi = TestChunkSize * TestVoxelSize;
		const float FaceEps = 0.5f;

		TArray<FVector3f> Verts;
		TArray<int32> Remap;
		Remap.SetNum(Mesh.Positions.Num());
		for (int32 i = 0; i < Mesh.Positions.Num(); ++i)
		{
			const FVector3f& P = Mesh.Positions[i];
			int32 Found = INDEX_NONE;
			for (int32 v = 0; v < Verts.Num(); ++v)
			{
				if ((Verts[v] - P).SizeSquared() < 0.01f * 0.01f) { Found = v; break; }
			}
			if (Found == INDEX_NONE) { Found = Verts.Add(P); }
			Remap[i] = Found;
		}

		auto EdgeKey = [](int32 a, int32 b) -> uint64
		{
			const uint32 lo = static_cast<uint32>(FMath::Min(a, b));
			const uint32 hi = static_cast<uint32>(FMath::Max(a, b));
			return (static_cast<uint64>(lo) << 32) | hi;
		};

		TMap<uint64, int32> EdgeCount;
		const int32 NumTris = Mesh.Indices.Num() / 3;
		for (int32 t = 0; t < NumTris; ++t)
		{
			const int32 i0 = Mesh.Indices[t * 3 + 0];
			const int32 i1 = Mesh.Indices[t * 3 + 1];
			const int32 i2 = Mesh.Indices[t * 3 + 2];
			const float Area = 0.5f * FVector3f::CrossProduct(
				Mesh.Positions[i1] - Mesh.Positions[i0], Mesh.Positions[i2] - Mesh.Positions[i0]).Size();
			if (Area < 1.0f) { continue; }
			EdgeCount.FindOrAdd(EdgeKey(Remap[i0], Remap[i1]))++;
			EdgeCount.FindOrAdd(EdgeKey(Remap[i1], Remap[i2]))++;
			EdgeCount.FindOrAdd(EdgeKey(Remap[i2], Remap[i0]))++;
		}

		auto OnChunkFace = [&](const FVector3f& P) -> bool
		{
			return FMath::Abs(P.X - Lo) < FaceEps || FMath::Abs(P.X - Hi) < FaceEps
				|| FMath::Abs(P.Y - Lo) < FaceEps || FMath::Abs(P.Y - Hi) < FaceEps
				|| FMath::Abs(P.Z - Lo) < FaceEps || FMath::Abs(P.Z - Hi) < FaceEps;
		};

		int32 HoleEdges = 0;
		for (const TPair<uint64, int32>& KV : EdgeCount)
		{
			if (KV.Value != 1) { continue; }
			const int32 a = static_cast<int32>(KV.Key >> 32);
			const int32 b = static_cast<int32>(KV.Key & 0xffffffffu);
			if (OnChunkFace(Verts[a]) && OnChunkFace(Verts[b])) { continue; }
			++HoleEdges;
		}
		return HoleEdges;
	}

	/** Set voxel.DCBoundaryWeld for the duration of a measurement (restores on dtor). */
	struct FScopedWeld
	{
		IConsoleVariable* CVar = nullptr;
		int32 Prev = 1;
		explicit FScopedWeld(int32 Value)
		{
			CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.DCBoundaryWeld"));
			if (CVar) { Prev = CVar->GetInt(); CVar->Set(Value); }
		}
		~FScopedWeld() { if (CVar) { CVar->Set(Prev); } }
	};

	/** Build the fine|coarse LOD-boundary pair: A fine merges +X toward coarse B. */
	void BuildLODPair(int32 FineLOD, int32 CoarseLOD, FVoxelMeshingRequest& OutA, FVoxelMeshingRequest& OutB)
	{
		OutA = MakeChunkRequest(FIntVector(0, 0, 0), FineLOD);
		OutB = MakeChunkRequest(FIntVector(1, 0, 0), CoarseLOD);
		FillAllNeighborData(OutA);
		FillAllNeighborData(OutB);
		OutA.NeighborLODLevels[1] = CoarseLOD; // A's +X neighbor is coarser B -> A merges that face
		OutB.NeighborLODLevels[0] = FineLOD;   // B's -X neighbor is finer A
	}

	const TCHAR* FieldName(ETestField F)
	{
		switch (F)
		{
		case ETestField::Smooth: return TEXT("Smooth");
		case ETestField::Cliff: return TEXT("Cliff");
		default: return TEXT("NonLinearZ");
		}
	}

// NOTE: tests are defined INSIDE this namespace (no file-scope `using namespace`).
// This module is built with unity (jumbo) translation units, so a global
// `using namespace` here would collide with MarchingCubesLODBoundaryTests.cpp's
// identically-named helpers (MakeChunkRequest, FillAllNeighborData, ...). Keeping
// the tests inside the namespace resolves those names locally and unambiguously.

// ==================== DT1: both LOD0 (harness sanity baseline) ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCLODBoundaryDT1BothLOD0Test, "VoxelWorlds.Meshing.DualContour.LODBoundary.DT1_BothLOD0",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCLODBoundaryDT1BothLOD0Test::RunTest(const FString& Parameters)
{
	FVoxelMeshingRequest RequestA = MakeChunkRequest(FIntVector(0, 0, 0), 0);
	FVoxelMeshingRequest RequestB = MakeChunkRequest(FIntVector(1, 0, 0), 0);
	FillAllNeighborData(RequestA);
	FillAllNeighborData(RequestB);
	RequestA.NeighborLODLevels[1] = 0;
	RequestB.NeighborLODLevels[0] = 0;

	FChunkMeshData MeshA, MeshB;
	TestTrue(TEXT("Chunk A meshing should succeed"), MeshChunk(RequestA, MeshA));
	TestTrue(TEXT("Chunk B meshing should succeed"), MeshChunk(RequestB, MeshB));

	int32 NumB = 0, UnmatchedB = 0;
	float MaxDist = 0.0f;
	MeasureCoarseSideMatch(MeshA, MeshB, 100.0f /*cell*/, FuseTolerance, NumB, UnmatchedB, MaxDist);
	AddInfo(FString::Printf(TEXT("DT1 LOD0|LOD0: B boundary verts=%d unmatched=%d maxDist=%.2f"), NumB, UnmatchedB, MaxDist));

	TestTrue(TEXT("Surface should produce geometry on both sides"), MeshA.Positions.Num() > 0 && MeshB.Positions.Num() > 0);
	TestTrue(TEXT("B boundary should cross the shared face"), NumB > 0);
	TestEqual(TEXT("Same-LOD LOD0: every B boundary vertex coincides with an A vertex (watertight)"), UnmatchedB, 0);
	return true;
}

// ==================== DT2: both LOD1 (same-LOD stride-2 watertight?) ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCLODBoundaryDT2BothLOD1Test, "VoxelWorlds.Meshing.DualContour.LODBoundary.DT2_BothLOD1",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCLODBoundaryDT2BothLOD1Test::RunTest(const FString& Parameters)
{
	FVoxelMeshingRequest RequestA = MakeChunkRequest(FIntVector(0, 0, 0), 1);
	FVoxelMeshingRequest RequestB = MakeChunkRequest(FIntVector(1, 0, 0), 1);
	FillAllNeighborData(RequestA);
	FillAllNeighborData(RequestB);
	RequestA.NeighborLODLevels[1] = 1;
	RequestB.NeighborLODLevels[0] = 1;

	FChunkMeshData MeshA, MeshB;
	TestTrue(TEXT("Chunk A meshing should succeed"), MeshChunk(RequestA, MeshA));
	TestTrue(TEXT("Chunk B meshing should succeed"), MeshChunk(RequestB, MeshB));

	int32 NumB = 0, UnmatchedB = 0;
	float MaxDist = 0.0f;
	MeasureCoarseSideMatch(MeshA, MeshB, 200.0f /*cell*/, FuseTolerance, NumB, UnmatchedB, MaxDist);
	AddInfo(FString::Printf(TEXT("DT2 LOD1|LOD1: B boundary verts=%d unmatched=%d maxDist=%.2f"), NumB, UnmatchedB, MaxDist));

	TestTrue(TEXT("Surface should produce geometry on both sides"), MeshA.Positions.Num() > 0 && MeshB.Positions.Num() > 0);
	TestTrue(TEXT("B boundary should cross the shared face"), NumB > 0);
	TestTrue(FString::Printf(TEXT("Same-LOD LOD1 boundary sealed (B unmatched %d <= %d edge residual)"), UnmatchedB, AcceptedEdgeResidual),
		UnmatchedB <= AcceptedEdgeResidual);
	return true;
}

// ==================== DT3: both LOD2 (same-LOD stride-4 watertight?) ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCLODBoundaryDT3BothLOD2Test, "VoxelWorlds.Meshing.DualContour.LODBoundary.DT3_BothLOD2",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCLODBoundaryDT3BothLOD2Test::RunTest(const FString& Parameters)
{
	TGuardValue<bool> DumpGuard(GDumpWorstUnmatched, true);
	FVoxelMeshingRequest RequestA = MakeChunkRequest(FIntVector(0, 0, 0), 2);
	FVoxelMeshingRequest RequestB = MakeChunkRequest(FIntVector(1, 0, 0), 2);
	FillAllNeighborData(RequestA);
	FillAllNeighborData(RequestB);
	RequestA.NeighborLODLevels[1] = 2;
	RequestB.NeighborLODLevels[0] = 2;

	FChunkMeshData MeshA, MeshB;
	TestTrue(TEXT("Chunk A meshing should succeed"), MeshChunk(RequestA, MeshA));
	TestTrue(TEXT("Chunk B meshing should succeed"), MeshChunk(RequestB, MeshB));

	int32 NumB = 0, UnmatchedB = 0;
	float MaxDist = 0.0f;
	MeasureCoarseSideMatch(MeshA, MeshB, 400.0f /*cell*/, FuseTolerance, NumB, UnmatchedB, MaxDist);
	AddInfo(FString::Printf(TEXT("DT3 LOD2|LOD2: B boundary verts=%d unmatched=%d maxDist=%.2f"), NumB, UnmatchedB, MaxDist));

	TestTrue(TEXT("Surface should produce geometry on both sides"), MeshA.Positions.Num() > 0 && MeshB.Positions.Num() > 0);
	TestTrue(TEXT("B boundary should cross the shared face"), NumB > 0);
	TestTrue(FString::Printf(TEXT("Same-LOD LOD2 boundary sealed (B unmatched %d <= %d edge residual)"), UnmatchedB, AcceptedEdgeResidual),
		UnmatchedB <= AcceptedEdgeResidual);
	return true;
}

// ==================== DT4: fine|coarse LOD boundary (the merge) ====================
//
// A (fine) borders B (coarse) and merges its boundary cells toward B. This is
// the DC analogue of MC T9: expected GREEN on the Smooth/linear field (coarse
// and fine interpolate to the same height) and RED on NonLinearZ/Cliff (coarse
// and fine land the iso-surface at different heights -> the merged vertex,
// derived from fine-side data only, does not coincide with B's coarse vertex).
// Landed as the known-issue gate until Phase 2 fixes the merge.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCLODBoundaryDT4MergeTest, "VoxelWorlds.Meshing.DualContour.LODBoundary.DT4_LODBoundaryMerge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCLODBoundaryDT4MergeTest::RunTest(const FString& Parameters)
{
	struct FCase { ETestField Field; int32 FineLOD; int32 CoarseLOD; float CoarseCell; const TCHAR* Name; };
	const FCase Cases[] = {
		{ ETestField::Smooth,     0, 1, 200.0f, TEXT("Smooth LOD0|LOD1") },
		{ ETestField::NonLinearZ, 0, 1, 200.0f, TEXT("NonLinearZ LOD0|LOD1") },
		{ ETestField::Cliff,      0, 1, 200.0f, TEXT("Cliff LOD0|LOD1") },
		{ ETestField::Smooth,     1, 2, 400.0f, TEXT("Smooth LOD1|LOD2") },
		{ ETestField::NonLinearZ, 1, 2, 400.0f, TEXT("NonLinearZ LOD1|LOD2") },
		{ ETestField::Cliff,      1, 2, 400.0f, TEXT("Cliff LOD1|LOD2") },
	};

	TGuardValue<bool> DumpGuard(GDumpWorstUnmatched, true);
	bool bAnyLeak = false;
	for (const FCase& C : Cases)
	{
		FScopedTestField Scoped(C.Field);

		FVoxelMeshingRequest RequestA, RequestB;
		BuildLODPair(C.FineLOD, C.CoarseLOD, RequestA, RequestB);

		FChunkMeshData MeshA, MeshB;
		TestTrue(TEXT("Chunk A meshing should succeed"), MeshChunk(RequestA, MeshA));
		TestTrue(TEXT("Chunk B meshing should succeed"), MeshChunk(RequestB, MeshB));

		// Coarse-side coincidence is the watertightness gate: every coarse-neighbour
		// (B) boundary vertex must be reproduced on A's merged boundary. The open-edge
		// count is reported for context only (it also counts A/B's other-face exit rims).
		int32 NearPlane = 0;
		const int32 OpenEdges = CountSeamOpenEdges(MeshA, MeshB, C.CoarseCell * 1.5f, NearPlane);

		int32 NumB = 0, UnmatchedB = 0;
		float MaxDist = 0.0f;
		MeasureCoarseSideMatch(MeshA, MeshB, C.CoarseCell, FuseTolerance, NumB, UnmatchedB, MaxDist);

		AddInfo(FString::Printf(
			TEXT("DT4 %s: coarse verts B=%d unmatched=%d maxDist=%.2f | (info) seam open edges=%d near-plane=%d"),
			C.Name, NumB, UnmatchedB, MaxDist, OpenEdges, NearPlane));

		if (NumB > 0 && UnmatchedB > AcceptedEdgeResidual)
		{
			bAnyLeak = true;
			AddError(FString::Printf(
				TEXT("%s: LOD boundary NOT sealed - %d/%d coarse verts unmatched by A's merged boundary (> %d edge residual, maxDist=%.1f)"),
				C.Name, UnmatchedB, NumB, AcceptedEdgeResidual, MaxDist));
		}
	}

	TestFalse(TEXT("DC LOD boundary merge should seal the dominant seam (coarse verts matched except the documented edge residual) across all fields"), bAnyLeak);
	return true;
}

// ==================== DT5: weld must not punch holes (missing geometry) ====================
//
// The vertex-match tests (DT1-DT4) measure whether the two sides line up; they are
// BLIND to missing geometry. Live testing showed the weld punched many openings in
// stride>1 regions: a boundary cell whose surface crosses PERPENDICULAR to the face
// (no crossing on the shared face plane) was invalidated -> hole. This test meshes a
// strided chunk whose whole shell welds (the "beyond LOD0" case) and asserts the weld
// does NOT introduce interior open edges (hole rims) versus the weld-off baseline.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCLODBoundaryDT5HolesTest, "VoxelWorlds.Meshing.DualContour.LODBoundary.DT5_WeldHoles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCLODBoundaryDT5HolesTest::RunTest(const FString& Parameters)
{
	// A strided chunk meshed with same-LOD neighbours on every face — the whole
	// 1-cell boundary shell goes through the weld, exactly the live "beyond LOD0"
	// regime. Also a fine chunk with a coarser +X neighbour (the LOD transition).
	struct FCase { ETestField Field; int32 SelfLOD; int32 NbrLOD; const TCHAR* Name; };
	const FCase Cases[] = {
		{ ETestField::Smooth,     1, 1, TEXT("Smooth LOD1 shell") },
		{ ETestField::NonLinearZ, 1, 1, TEXT("NonLinearZ LOD1 shell") },
		{ ETestField::Cliff,      1, 1, TEXT("Cliff LOD1 shell") },
		{ ETestField::Smooth,     2, 2, TEXT("Smooth LOD2 shell") },
		{ ETestField::Cliff,      2, 2, TEXT("Cliff LOD2 shell") },
		{ ETestField::Cliff,      0, 1, TEXT("Cliff LOD0 +X coarse") },
	};

	bool bAnyNewHoles = false;
	for (const FCase& C : Cases)
	{
		FScopedTestField Scoped(C.Field);

		FVoxelMeshingRequest Req = MakeChunkRequest(FIntVector(0, 0, 0), C.SelfLOD);
		FillAllNeighborData(Req);
		for (int32 i = 0; i < 6; ++i) { Req.NeighborLODLevels[i] = C.NbrLOD; }

		FChunkMeshData MeshOff, MeshOn;
		{ FScopedWeld W(0); TestTrue(TEXT("mesh weld-off"), MeshChunk(Req, MeshOff)); }
		{ FScopedWeld W(1); TestTrue(TEXT("mesh weld-on"), MeshChunk(Req, MeshOn)); }

		const int32 HolesOff = CountInteriorOpenEdges(MeshOff);
		const int32 HolesOn = CountInteriorOpenEdges(MeshOn);

		AddInfo(FString::Printf(TEXT("DT5 %s: interior open edges weld-off=%d weld-on=%d (tris off=%d on=%d)"),
			C.Name, HolesOff, HolesOn, MeshOff.Indices.Num() / 3, MeshOn.Indices.Num() / 3));

		// The weld must not introduce holes the un-welded mesh did not have.
		if (HolesOn > HolesOff)
		{
			bAnyNewHoles = true;
			AddError(FString::Printf(TEXT("%s: weld introduced %d interior open edges (holes) vs %d weld-off"),
				C.Name, HolesOn, HolesOff));
		}
	}

	TestFalse(TEXT("DC boundary weld must not punch holes (missing geometry) in stride>1 regions"), bAnyNewHoles);
	return true;
}

// ==================== DT6: multi-chunk assembly watertightness (the real repro) ====================
//
// The decisive test for the user-reported openings: mesh a 3x3 block of same-LOD
// chunks (z=0 layer; the analytic surface sits mid-chunk so it only exits ±X/±Y),
// each with correct analytic neighbour data, assemble into one world-space mesh, and
// count interior open edges that are NOT on the block's outer box — i.e. holes at the
// inter-chunk seams. A watertight result is 0. This captures the multi-chunk corner/
// edge interactions a 2-chunk test cannot.

/** Count interior open edges of an assembled multi-chunk mesh, excluding the block's outer box.
 *  BlockN = chunks per side (3 for the 3x3 assembly, 2 for the focused 4-chunk-corner test);
 *  internal chunk boundaries sit at i*CW for i in 1..BlockN-1, CW = OuterBox.Max.X / BlockN. */
int32 CountAssemblyHoles(const TArray<FChunkMeshData>& Meshes, const TArray<FVector3f>& Offsets,
	const FBox3f& OuterBox, int32 BlockN = 3)
{
	// Tolerance weld: verts within FuseTol world units are the same point. Sub-tol
	// gaps are NOT counted as holes — adjacent chunks are meshed in different local
	// frames, so a perfectly-shared boundary vertex still differs by float precision;
	// that is invisible to the renderer (separate meshes). Only gaps a renderer would
	// actually show (> FuseTol) or genuinely missing geometry remain. A 27-cell
	// spatial hash makes the weld robust (no quantization-boundary splits).
	const float FuseTol = 1.0f;
	TArray<FVector3f> Verts;
	TMap<FIntVector, TArray<int32>> Grid;
	TArray<int32> Tris;
	auto CellOf = [FuseTol](const FVector3f& P) -> FIntVector
	{
		return FIntVector(FMath::FloorToInt(P.X / FuseTol), FMath::FloorToInt(P.Y / FuseTol), FMath::FloorToInt(P.Z / FuseTol));
	};
	for (int32 m = 0; m < Meshes.Num(); ++m)
	{
		const FChunkMeshData& Mesh = Meshes[m];
		const FVector3f Off = Offsets[m];
		TArray<int32> Remap; Remap.SetNum(Mesh.Positions.Num());
		for (int32 i = 0; i < Mesh.Positions.Num(); ++i)
		{
			const FVector3f W = Mesh.Positions[i] + Off;
			const FIntVector Base = CellOf(W);
			int32 Hit = INDEX_NONE;
			for (int32 dz = -1; dz <= 1 && Hit == INDEX_NONE; ++dz)
			for (int32 dy = -1; dy <= 1 && Hit == INDEX_NONE; ++dy)
			for (int32 dx = -1; dx <= 1 && Hit == INDEX_NONE; ++dx)
			{
				if (const TArray<int32>* Cell = Grid.Find(Base + FIntVector(dx, dy, dz)))
				{
					for (int32 Vi : *Cell)
					{
						if ((Verts[Vi] - W).SizeSquared() < FuseTol * FuseTol) { Hit = Vi; break; }
					}
				}
			}
			if (Hit == INDEX_NONE) { Hit = Verts.Add(W); Grid.FindOrAdd(Base).Add(Hit); }
			Remap[i] = Hit;
		}
		const int32 NumTris = Mesh.Indices.Num() / 3;
		for (int32 t = 0; t < NumTris; ++t)
		{
			const int32 i0 = Mesh.Indices[t * 3 + 0], i1 = Mesh.Indices[t * 3 + 1], i2 = Mesh.Indices[t * 3 + 2];
			const float Area = 0.5f * FVector3f::CrossProduct(
				Mesh.Positions[i1] - Mesh.Positions[i0], Mesh.Positions[i2] - Mesh.Positions[i0]).Size();
			if (Area < 1.0f) { continue; }
			Tris.Add(Remap[i0]); Tris.Add(Remap[i1]); Tris.Add(Remap[i2]);
		}
	}

	auto EdgeKey = [](int32 a, int32 b) -> uint64
	{
		const uint32 lo = static_cast<uint32>(FMath::Min(a, b));
		const uint32 hi = static_cast<uint32>(FMath::Max(a, b));
		return (static_cast<uint64>(lo) << 32) | hi;
	};
	TMap<uint64, int32> EdgeCount;
	for (int32 t = 0; t < Tris.Num() / 3; ++t)
	{
		EdgeCount.FindOrAdd(EdgeKey(Tris[t * 3 + 0], Tris[t * 3 + 1]))++;
		EdgeCount.FindOrAdd(EdgeKey(Tris[t * 3 + 1], Tris[t * 3 + 2]))++;
		EdgeCount.FindOrAdd(EdgeKey(Tris[t * 3 + 2], Tris[t * 3 + 0]))++;
	}
	// Count only open edges on the INTERNAL chunk seams (the actual inter-chunk
	// watertightness). The block's outer rim is excluded: those faces have no
	// in-block neighbor, yet a strided chunk still welds them against Air, producing
	// rim geometry that is not a real seam. An internal seam is the plane X or Y ==
	// an internal chunk boundary (CW or 2*CW); a hole-rim edge there has its midpoint
	// within ~half a chunk of it. EdgeHoles = near BOTH (the shared vertical chunk
	// edge where 4 chunks meet); FaceHoles = near exactly one (a face seam).
	const float CW = OuterBox.Max.X / static_cast<float>(BlockN);
	const float Band = 600.0f;              // ~1.5 * max test stride (LOD2 = 400)
	auto NearInternal = [&](float V) -> bool
	{
		for (int32 i = 1; i < BlockN; ++i)
		{
			if (FMath::Abs(V - i * CW) < Band) { return true; }
		}
		return false;
	};
	auto NearOuter = [&](float V) -> bool
	{
		return (FMath::Abs(V - OuterBox.Min.X) < Band) || (FMath::Abs(V - OuterBox.Max.X) < Band);
	};
	// Z top/bottom rim of the (single-layer) block. A steep field's surface can exit the
	// chunk's top/bottom Z face, leaving a legitimate open boundary there — but that is NOT
	// an LOD seam (in the live world the chunk above/below continues it). Exclude a tight
	// band around Z=0 and Z=OuterBox.Max.Z so the metric measures only the X/Y inter-chunk
	// seams. Tight (250) so genuine mid-Z seam holes near the surface are still counted.
	const float ZRimBand = 250.0f;
	auto NearZRim = [&](float Z) -> bool
	{
		return (FMath::Abs(Z - OuterBox.Min.Z) < ZRimBand) || (FMath::Abs(Z - OuterBox.Max.Z) < ZRimBand);
	};
	// An internal-seam hole-rim edge sits near an internal X or Y boundary and away
	// from the block's outer rim (which welds against Air — a test artifact, not a
	// real seam). EdgeHoles: near BOTH internal boundaries (4-chunk vertical edge);
	// FaceHoles: near exactly one (a face seam).
	int32 Holes = 0, EdgeHoles = 0, FaceHoles = 0;
	for (const TPair<uint64, int32>& KV : EdgeCount)
	{
		if (KV.Value != 1) { continue; }
		const int32 a = static_cast<int32>(KV.Key >> 32);
		const int32 b = static_cast<int32>(KV.Key & 0xffffffffu);
		const FVector3f Mid = 0.5f * (Verts[a] + Verts[b]);
		if (NearOuter(Mid.X) || NearOuter(Mid.Y)) { continue; } // block outer rim — skip
		if (NearZRim(Mid.Z)) { continue; } // chunk top/bottom — surface exit, not an LOD seam
		const bool bNX = NearInternal(Mid.X), bNY = NearInternal(Mid.Y);
		if (!bNX && !bNY) { continue; } // chunk interior — not a seam
		++Holes;
		if (bNX && bNY) { ++EdgeHoles; } else { ++FaceHoles; }
	}
	if (GDumpWorstUnmatched)
	{
		UE_LOG(LogTemp, Display, TEXT("  [DCholes] total=%d  chunk-edge-line=%d  chunk-face=%d"), Holes, EdgeHoles, FaceHoles);
		int32 Shown = 0;
		for (const TPair<uint64, int32>& KV : EdgeCount)
		{
			if (KV.Value != 1 || Shown >= 6) { continue; }
			const int32 a = static_cast<int32>(KV.Key >> 32);
			const int32 b = static_cast<int32>(KV.Key & 0xffffffffu);
			const FVector3f Mid = 0.5f * (Verts[a] + Verts[b]);
			if (NearOuter(Mid.X) || NearOuter(Mid.Y)) { continue; }
			if (NearZRim(Mid.Z)) { continue; }
			if (!NearInternal(Mid.X) && !NearInternal(Mid.Y)) { continue; }
			UE_LOG(LogTemp, Display, TEXT("    open edge: (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f) len=%.1f"),
				Verts[a].X, Verts[a].Y, Verts[a].Z, Verts[b].X, Verts[b].Y, Verts[b].Z, (Verts[a] - Verts[b]).Size());
			++Shown;
		}
	}
	return Holes;
}

/** Mesh a 3x3 (z=0) block of uniform-LOD chunks and return interior seam holes.
 *  MeshFn selects the CPU (&MeshChunk, default) or GPU (&MeshChunkGPU) mesher. */
int32 MeshBlockHoles(int32 LOD, FDCMeshFn MeshFn = &MeshChunk)
{
	const float CW = TestChunkSize * TestVoxelSize; // 3200
	TArray<FChunkMeshData> Meshes;
	TArray<FVector3f> Offsets;
	for (int32 cy = 0; cy < 3; ++cy)
	for (int32 cx = 0; cx < 3; ++cx)
	{
		FVoxelMeshingRequest Req = MakeChunkRequest(FIntVector(cx, cy, 0), LOD);
		FillAllNeighborData(Req);
		for (int32 i = 0; i < 6; ++i) { Req.NeighborLODLevels[i] = LOD; }
		// Uniform-LOD edge/corner diagonals too — see MeshCornerBlockHoles: unset (-1 -> 0)
		// diagonals against LOD>0 faces would fake a mixed sharer set at uniform corners.
		for (int32 i = 0; i < 12; ++i) { Req.EdgeLODLevels[i] = LOD; }
		for (int32 i = 0; i < 8; ++i) { Req.CornerLODLevels[i] = LOD; }
		// Isolate the block: faces with no IN-BLOCK neighbor get no neighbor data, so
		// their outward boundary cells don't weld to voxels outside the block (which
		// would be counted as spurious holes). Only the internal seams are exercised.
		auto ClearFace = [&](int32 Face, TArray<FVoxelData>& Plane, TArray<FVoxelData>& Deep)
		{
			Plane.Empty(); Deep.Empty(); Req.NeighborLODLevels[Face] = -1;
		};
		if (cx == 0) { ClearFace(0, Req.NeighborXNeg, Req.NeighborXNegDeep); }
		if (cx == 2) { ClearFace(1, Req.NeighborXPos, Req.NeighborXPosDeep); }
		if (cy == 0) { ClearFace(2, Req.NeighborYNeg, Req.NeighborYNegDeep); }
		if (cy == 2) { ClearFace(3, Req.NeighborYPos, Req.NeighborYPosDeep); }
		// Single Z layer: ±Z have no in-block neighbor (surface sits mid-chunk anyway).
		ClearFace(4, Req.NeighborZNeg, Req.NeighborZNegDeep);
		ClearFace(5, Req.NeighborZPos, Req.NeighborZPosDeep);
		FChunkMeshData Mesh;
		MeshFn(Req, Mesh);
		Meshes.Add(MoveTemp(Mesh));
		Offsets.Add(FVector3f(cx * CW, cy * CW, 0.0f));
	}
	const FBox3f Outer(FVector3f(0, 0, 0), FVector3f(3 * CW, 3 * CW, CW));
	return CountAssemblyHoles(Meshes, Offsets, Outer, 3);
}

/** Mesh a 2x2 (z=0) block of uniform-LOD chunks with FULL analytic neighbour data on every
 *  face (NO clearing) and count interior open edges around the single shared 4-chunk corner
 *  at world (CW, CW). Unlike DT3's 2-chunk test, the diagonal +X+Y neighbour IS present, so
 *  the shared corner edge is owned and generated by chunk (1,1) — this is the faithful test
 *  of whether a real 4-chunk LOD corner holes. The test field's surface sits mid-Z, so the
 *  +/-Z faces never intersect it (full +/-Z data is harmless) and there is no Z rim. The
 *  block's outer perimeter is excluded by CountAssemblyHoles' NearOuter filter.
 *  MeshFn selects the CPU (&MeshChunk, default) or GPU (&MeshChunkGPU) mesher. */
int32 MeshCornerBlockHoles(int32 LOD, FDCMeshFn MeshFn = &MeshChunk)
{
	const float CW = TestChunkSize * TestVoxelSize; // 3200
	TArray<FChunkMeshData> Meshes;
	TArray<FVector3f> Offsets;
	for (int32 cy = 0; cy < 2; ++cy)
	for (int32 cx = 0; cx < 2; ++cx)
	{
		FVoxelMeshingRequest Req = MakeChunkRequest(FIntVector(cx, cy, 0), LOD);
		FillAllNeighborData(Req);
		for (int32 i = 0; i < 6; ++i) { Req.NeighborLODLevels[i] = LOD; }
		// Declare the edge/corner diagonals at the same uniform LOD: the weld's sharer-set
		// mixedness test reads them, and leaving them -1 (clamped to 0) against LOD>0 faces
		// fakes a MIXED corner out of a uniform block (routing uniform cells into the
		// mixed-only feature fallbacks). The live extraction always fills them.
		for (int32 i = 0; i < 12; ++i) { Req.EdgeLODLevels[i] = LOD; }
		for (int32 i = 0; i < 8; ++i) { Req.CornerLODLevels[i] = LOD; }
		FChunkMeshData Mesh;
		MeshFn(Req, Mesh);
		Meshes.Add(MoveTemp(Mesh));
		Offsets.Add(FVector3f(cx * CW, cy * CW, 0.0f));
	}
	const FBox3f Outer(FVector3f(0, 0, 0), FVector3f(2 * CW, 2 * CW, CW));
	return CountAssemblyHoles(Meshes, Offsets, Outer, 2);
}

/** Mesh a Dims.X x Dims.Y x Dims.Z lattice of chunks with PER-CHUNK LODs and full analytic
 *  neighbour data, wiring every chunk's face/edge/corner neighbor-LOD fields from the lattice
 *  (out-of-lattice neighbors: the chunk's own LOD — a virtual same-LOD surrounding world, as
 *  MeshCornerBlockHoles does). Fuses the meshes and counts interior open edges near internal
 *  X/Y chunk boundaries via CountAssemblyHoles — requires Dims.X == Dims.Y (the metric's
 *  internal-boundary scan covers X and Y; Z rims are excluded, and internal Z boundaries are
 *  counted where they meet an X/Y seam, i.e. the X-face∩Z-face edge columns).
 *  This is the harness for ASYMMETRIC-LOD corners: DT7/GT7 only ever mesh uniform-LOD corner
 *  blocks, which is exactly why the live demo's mixed-LOD corner holes had no failing test.
 *  LODAt maps lattice coord -> LOD. MeshFn selects the CPU (&MeshChunk) or GPU mesher. */
int32 MeshLODLatticeHoles(const FIntVector& Dims, TFunctionRef<int32(const FIntVector&)> LODAt,
	FDCMeshFn MeshFn = &MeshChunk)
{
	const float CW = TestChunkSize * TestVoxelSize; // 3200
	// Neighbor offsets in the FVoxelMeshingRequest index orders: NeighborLODLevels
	// (-X,+X,-Y,+Y,-Z,+Z), EdgeLODLevels (EDGE_* order), CornerLODLevels (CORNER_* order).
	static const FIntVector FaceOffs[6] = {
		FIntVector(-1, 0, 0), FIntVector(1, 0, 0), FIntVector(0, -1, 0),
		FIntVector(0, 1, 0),  FIntVector(0, 0, -1), FIntVector(0, 0, 1)
	};
	static const FIntVector EdgeOffs[12] = {
		FIntVector(1, 1, 0), FIntVector(1, -1, 0), FIntVector(-1, 1, 0), FIntVector(-1, -1, 0),
		FIntVector(1, 0, 1), FIntVector(1, 0, -1), FIntVector(-1, 0, 1), FIntVector(-1, 0, -1),
		FIntVector(0, 1, 1), FIntVector(0, 1, -1), FIntVector(0, -1, 1), FIntVector(0, -1, -1)
	};
	static const FIntVector CornerOffs[8] = {
		FIntVector(1, 1, 1),  FIntVector(1, 1, -1),  FIntVector(1, -1, 1),  FIntVector(1, -1, -1),
		FIntVector(-1, 1, 1), FIntVector(-1, 1, -1), FIntVector(-1, -1, 1), FIntVector(-1, -1, -1)
	};
	auto InLattice = [&](const FIntVector& C) -> bool
	{
		return C.X >= 0 && C.X < Dims.X && C.Y >= 0 && C.Y < Dims.Y && C.Z >= 0 && C.Z < Dims.Z;
	};

	TArray<FChunkMeshData> Meshes;
	TArray<FVector3f> Offsets;
	for (int32 cz = 0; cz < Dims.Z; ++cz)
	for (int32 cy = 0; cy < Dims.Y; ++cy)
	for (int32 cx = 0; cx < Dims.X; ++cx)
	{
		const FIntVector CC(cx, cy, cz);
		const int32 SelfLOD = LODAt(CC);
		FVoxelMeshingRequest Req = MakeChunkRequest(CC, SelfLOD);
		FillAllNeighborData(Req);
		for (int32 i = 0; i < 6; ++i)
		{
			const FIntVector N = CC + FaceOffs[i];
			Req.NeighborLODLevels[i] = InLattice(N) ? LODAt(N) : SelfLOD;
		}
		for (int32 i = 0; i < 12; ++i)
		{
			const FIntVector N = CC + EdgeOffs[i];
			Req.EdgeLODLevels[i] = InLattice(N) ? LODAt(N) : SelfLOD;
		}
		for (int32 i = 0; i < 8; ++i)
		{
			const FIntVector N = CC + CornerOffs[i];
			Req.CornerLODLevels[i] = InLattice(N) ? LODAt(N) : SelfLOD;
		}
		FChunkMeshData Mesh;
		MeshFn(Req, Mesh);
		Meshes.Add(MoveTemp(Mesh));
		Offsets.Add(FVector3f(cx * CW, cy * CW, cz * CW));
	}
	const FBox3f Outer(FVector3f::ZeroVector, FVector3f(Dims.X * CW, Dims.Y * CW, Dims.Z * CW));
	return CountAssemblyHoles(Meshes, Offsets, Outer, Dims.X);
}

/** Shared DT9/GT9 body: asymmetric-LOD 2x2 corners + a coarse|fine column 2x2x2 lattice.
 *  Returns true when every case is within its documented residual. */
bool RunAsymmetricLODCornerCases(FAutomationTestBase& Test, FDCMeshFn MeshFn, const TCHAR* Tag)
{
	// The 2x2 (z=0) corner patterns the uniform-LOD DT7/GT7 never cover. [2,0/0,0] is the
	// exact configuration confirmed live in the demo (chunk corner (-16350,1650): holes
	// SURVIVED voxel.RemeshAll with full neighbor data — deterministic, not stale streaming).
	// [1,0/0,0] is the legal-balance flavor at every LOD ring corner. Layout: L[y][x].
	//
	// KnownResidual: with the feature-consistent weld (26-neighbor LOD pin rule + mixed-only
	// SURFACE-DERIVED fallbacks), the pre-fix 300-600u see-through corner TRIANGLES (11-14
	// open edges per corner) are gone. What remains is a small surface-anchored T-junction
	// crack per corner from cells whose surface crosses NO shared feature — those keep their
	// raw vertex. We deliberately do NOT snap such cells onto a fabricated feature point
	// (line-window midpoint / patch center): that sealed the metric tighter (3/3/5/5) but
	// placed vertices up to E voxels OFF the surface, and the triangles fanning to them
	// stretched into protruding FINS — live-verified to be far MORE visible than the cracks,
	// which scatter covers in practice. Closing the cracks properly needs every sharer to
	// sample the OTHER side's descent patches — cross-side on-plane data beyond one voxel,
	// i.e. neighbor-LOD-driven deep extraction — the tracked follow-up. Counts pinned exactly
	// so ANY regression (or improvement) shows up.
	struct FCase { ETestField Field; int32 L00, L10, L01, L11; int32 KnownResidual; const TCHAR* Name; };
	const FCase Cases[] = {
		{ ETestField::Smooth, 1, 0, 0, 0, 6, TEXT("Smooth [1,0/0,0]") },
		{ ETestField::Cliff,  1, 0, 0, 0, 3, TEXT("Cliff [1,0/0,0]") },
		{ ETestField::Smooth, 2, 0, 0, 0, 9, TEXT("Smooth [2,0/0,0] live-repro") },
		{ ETestField::Smooth, 2, 1, 1, 1, 5, TEXT("Smooth [2,1/1,1]") },
	};

	TGuardValue<bool> DumpGuard(GDumpWorstUnmatched, true);
	FScopedWeld WeldOn(1);
	bool bAllSealed = true;
	for (const FCase& C : Cases)
	{
		FScopedTestField Scoped(C.Field);
		const int32 LMap[2][2] = { { C.L00, C.L10 }, { C.L01, C.L11 } };
		const int32 Holes = MeshLODLatticeHoles(FIntVector(2, 2, 1),
			[&LMap](const FIntVector& CC) { return LMap[CC.Y][CC.X]; }, MeshFn);
		Test.AddInfo(FString::Printf(TEXT("%s %s: interior open edges=%d (known T-junction sliver residual=%d)"),
			Tag, C.Name, Holes, C.KnownResidual));
		if (Holes > C.KnownResidual)
		{
			bAllSealed = false;
			Test.AddError(FString::Printf(TEXT("%s %s: asymmetric-LOD corner regressed (%d open edges > known sliver residual %d)"),
				Tag, C.Name, Holes, C.KnownResidual));
		}
	}

	// Coarse|fine column split replicated across Y and Z (2x2x2): exercises the X-face∩Z-face
	// edge columns (the live "row of notches" along a coarse|fine face where the surface
	// crosses a Z chunk boundary) and the 8-chunk corner points. Cliff's slope crosses the
	// internal Z plane inside the lattice, which Smooth does not.
	{
		FScopedTestField Scoped(ETestField::Cliff);
		const int32 KnownResidual = 3; // same T-junction sliver class at the X∩Z edge columns
		const int32 Holes = MeshLODLatticeHoles(FIntVector(2, 2, 2),
			[](const FIntVector& CC) { return CC.X == 0 ? 2 : 0; }, MeshFn);
		Test.AddInfo(FString::Printf(TEXT("%s Cliff column [2|0] 2x2x2: interior open edges=%d (known T-junction sliver residual=%d)"),
			Tag, Holes, KnownResidual));
		if (Holes > KnownResidual)
		{
			bAllSealed = false;
			Test.AddError(FString::Printf(TEXT("%s Cliff column [2|0] 2x2x2: coarse|fine lattice regressed (%d open edges > known sliver residual %d)"),
				Tag, Holes, KnownResidual));
		}
	}
	return bAllSealed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCLODBoundaryDT6AssemblyTest, "VoxelWorlds.Meshing.DualContour.LODBoundary.DT6_AssemblyWatertight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCLODBoundaryDT6AssemblyTest::RunTest(const FString& Parameters)
{
	struct FCase { ETestField Field; int32 LOD; const TCHAR* Name; };
	const FCase Cases[] = {
		{ ETestField::Smooth,     1, TEXT("Smooth LOD1") },
		{ ETestField::NonLinearZ, 1, TEXT("NonLinearZ LOD1") },
		{ ETestField::Cliff,      1, TEXT("Cliff LOD1") },
		{ ETestField::Smooth,     2, TEXT("Smooth LOD2") },
		{ ETestField::Cliff,      2, TEXT("Cliff LOD2") },
	};

	TGuardValue<bool> DumpGuard(GDumpWorstUnmatched, true);
	bool bAnyHoles = false;
	for (const FCase& C : Cases)
	{
		FScopedTestField Scoped(C.Field);
		int32 HolesOff, Holes;
		{ FScopedWeld W(0); HolesOff = MeshBlockHoles(C.LOD); }
		{ FScopedWeld W(1); Holes = MeshBlockHoles(C.LOD); }
		AddInfo(FString::Printf(TEXT("DT6 %s: 3x3 assembly interior seam holes weld-off=%d weld-on=%d"), C.Name, HolesOff, Holes));
		// With stride-deep neighbour data now supplied (D = 2*stride faces + edges +
		// corners), the outward boundary cell reads the SAME densities both sides, so the
		// natural (weld-off) geometry is already near-coincident — the weld is now a
		// secondary refinement (it still helps fine->coarse transitions). This metric is
		// confounded by the 3x3 block's OUTER rim (a strided chunk welds against Air there,
		// a test artifact) and by float-diff fusing, so allow a small tolerance: the weld
		// must not materially worsen the assembly. The trustworthy per-boundary signal is
		// DT2/DT3 (clean 2-chunk coarse-side match). Residual LOD2 4-chunk-corner holes
		// come from DC cell-validity frame-dependence, not from missing data.
		const int32 WeldWorsenTol = 8;
		if (Holes > HolesOff + WeldWorsenTol)
		{
			bAnyHoles = true;
			AddError(FString::Printf(TEXT("%s: weld WORSENED assembly seams (%d holes vs %d weld-off, tol %d)"),
				C.Name, Holes, HolesOff, WeldWorsenTol));
		}
	}

	TestFalse(TEXT("DC boundary weld must not worsen multi-chunk assembly watertightness (residual holes need the deep neighbour-data fix)"), bAnyHoles);
	return true;
}

// ==================== DT7: faithful 4-chunk LOD corner watertightness ====================
//
// DT3 (2-chunk) leaves a residual at the shared corner, but that is largely an OWNERSHIP
// artifact: the corner edge is owned by the diagonal +X+Y neighbour, which is absent in a
// 2-chunk test, so neither chunk generates the closing quad. DT7 supplies the diagonal
// neighbour (a 2x2 block with full analytic data on every face) so the corner is owned and
// generated as it is in the real world. This isolates whether a 4-chunk LOD corner truly
// holes once all four chunks and their shared neighbour data are present.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCLODBoundaryDT7CornerTest, "VoxelWorlds.Meshing.DualContour.LODBoundary.DT7_FourChunkCorner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCLODBoundaryDT7CornerTest::RunTest(const FString& Parameters)
{
	// KnownResidual = extra open edges the weld is allowed to leave at this 4-chunk corner
	// beyond the un-welded baseline. We assert weld-on <= max(weld-off, KnownResidual):
	// the raw weld-off baseline is FP-sensitive, so a bare "must not exceed weld-off" check
	// is fragile. All cases are now 0: the max-min-area quad-diagonal choice in
	// FVoxelCPUDualContourMesher::GenerateQuads splits welded boundary quads along the
	// diagonal that avoids the near-zero-area sliver, eliminating the steep/shallow-seam
	// T-junction that previously left this corner cracked (Smooth LOD1 weld-on 8 -> 0).
	struct FCase { ETestField Field; int32 LOD; const TCHAR* Name; int32 KnownResidual; };
	const FCase Cases[] = {
		{ ETestField::Smooth,     1, TEXT("Smooth LOD1"),     0 },
		{ ETestField::NonLinearZ, 1, TEXT("NonLinearZ LOD1"), 0 },
		{ ETestField::Cliff,      1, TEXT("Cliff LOD1"),      0 },
		{ ETestField::Smooth,     2, TEXT("Smooth LOD2"),     0 },
		{ ETestField::Cliff,      2, TEXT("Cliff LOD2"),      0 },
	};

	TGuardValue<bool> DumpGuard(GDumpWorstUnmatched, true);
	bool bWorse = false;
	for (const FCase& C : Cases)
	{
		FScopedTestField Scoped(C.Field);
		int32 HolesOff = 0, Holes = 0;
		{ FScopedWeld W(0); HolesOff = MeshCornerBlockHoles(C.LOD); }
		{ FScopedWeld W(1); Holes = MeshCornerBlockHoles(C.LOD); }
		AddInfo(FString::Printf(TEXT("DT7 %s: 4-chunk-corner interior open edges weld-off=%d weld-on=%d"), C.Name, HolesOff, Holes));
		// The welded 4-chunk corner is now fully sealed (weld-on=0) for every field/LOD here,
		// matching the Marching Cubes path. The former steep/shallow-seam T-junction came from
		// a welded boundary quad whose fixed 0-2 diagonal degenerated into a near-zero-area
		// sliver across the near-1D seam contour; GenerateQuads now splits each quad along the
		// max-min-area diagonal, which steps through the intermediate welded vertex instead.
		const int32 Allowed = FMath::Max(HolesOff, C.KnownResidual);
		if (Holes > Allowed)
		{
			bWorse = true;
			AddError(FString::Printf(TEXT("%s: weld WORSENED 4-chunk corner (%d open edges vs %d allowed; weld-off=%d, known residual=%d)"),
				C.Name, Holes, Allowed, HolesOff, C.KnownResidual));
		}
	}
	TestFalse(TEXT("DC boundary weld must seal 4-chunk corner watertightness (weld-on must not exceed the un-welded baseline)"), bWorse);
	return true;
}

// ==================== DT9: asymmetric-LOD corners (CPU) ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCLODBoundaryDT9AsymmetricCornerTest, "VoxelWorlds.Meshing.DualContour.LODBoundary.DT9_AsymmetricLODCorner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCLODBoundaryDT9AsymmetricCornerTest::RunTest(const FString& Parameters)
{
	const bool bSealed = RunAsymmetricLODCornerCases(*this, &MeshChunk, TEXT("DT9"));
	TestTrue(TEXT("CPU DC weld must keep asymmetric-LOD chunk corners within the documented T-junction sliver residual"), bSealed);
	return true;
}

// ============================================================================
// GPU mirror: GT1-GT7 run the same watertightness checks as DT1-DT7 but feed the
// GPU Dual Contouring mesher (FVoxelGPUDualContourMesher) via MeshChunkGPU, so the
// shader path gets the same coverage as the CPU path — including the max-min-area
// quad-diagonal corner fix. They require a real RHI; under -nullrhi each test logs a
// skip and passes, so the headless CPU automation flow is unaffected. Run with e.g.
//   UnrealEditor-Cmd <proj> -ExecCmds="Automation RunTests VoxelWorlds.Meshing.DualContour.GPULODBoundary" -TestExit="Automation Test Queue Empty" -unattended -nopause -nosplash -nosound -stdout
//   (use -TestExit, not ";Quit" — Quit can fire before the automation controller registers the tests.)
// (note: NO -nullrhi).
//
// CURRENT STATUS (real RHI, verified 2026-07-17): GT0-GT8 all PASS. 49ec301's shader work
// brought the GPU base mesh to CPU watertightness for these analytic fields with full neighbor
// data: GT1 (LOD0 same-LOD seam) is bit-exact (unmatched=0, maxDist=0.00) and GT7's welded
// 4-chunk corner is fully sealed (weld-on=0) — NOT the ~37u / weld-on=31 that an earlier revision
// of this comment claimed (that "GT1/GT4 fail by design" note was stale; the shader fix landed).
// GT4's LOD0|LOD1 merge passes within the small documented AcceptedEdgeResidual.
//
// These analytic tests do NOT cover a boundary whose neighbor chunk has not streamed in yet — the
// harness always supplies every neighbor slice. That gap is exactly what produced the deterministic
// see-through boundary holes in the demo (GPU read the clamped interior at an absent neighbor instead
// of Air, so the boundary never closed). GT8 below reproduces and guards that case.
// ============================================================================

/** Shared null-RHI gate: returns true (and logs) when GPU work can't run, so the caller
 *  should early-out as a pass/skip. */
#define DC_GPU_SKIP_IF_NO_RHI(TestObj) \
	if (GUsingNullRHI) { (TestObj).AddInfo(TEXT("Skipped: GPU DC tests require a real RHI (run without -nullrhi)")); return true; }

// ==================== GT0: CPU-vs-GPU per-chunk parity diagnostic ====================
//
// Not a hard gate (always passes) — a diagnostic that meshes each chunk on BOTH the CPU
// (watertight reference) and GPU meshers and reports, per chunk, how many CPU vertices the
// GPU FAILS to reproduce within FuseTolerance (the CPU->GPU direction; the GPU emits extra
// unreferenced verts so GPU->CPU is noisy). Because the CPU pair is watertight, any GT1/GT4
// GPU seam failure must come from at least one chunk's GPU mesh diverging from its CPU mesh —
// this localizes the divergence to the fine (A) or coarse (B) side and logs the worst vertex
// position (boundary world-X≈ChunkSize*VoxelSize vs interior).

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCGPULODBoundaryGT0Test, "VoxelWorlds.Meshing.DualContour.GPULODBoundary.GT0_CPUvsGPUParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCGPULODBoundaryGT0Test::RunTest(const FString& Parameters)
{
	DC_GPU_SKIP_IF_NO_RHI(*this);

	// Per-chunk CPU-vs-GPU vertex divergence (CPU->GPU: CPU verts the GPU fails to reproduce).
	auto NearestPos = [](const FVector3f& P, const TArray<FVector3f>& S, float& OutD) -> FVector3f
	{
		float Min = FLT_MAX; FVector3f Best = FVector3f::ZeroVector;
		for (const FVector3f& Q : S) { const float D = (Q - P).Size(); if (D < Min) { Min = D; Best = Q; } }
		OutD = Min; return Best;
	};

	struct FScn { ETestField Field; int32 FineLOD; int32 CoarseLOD; float CoarseCell; const TCHAR* Name; };
	const FScn Scns[] = {
		{ ETestField::Smooth, 0, 0, 100.0f, TEXT("Smooth LOD0|LOD0 (GT1)") },
		{ ETestField::Cliff,  0, 1, 200.0f, TEXT("Cliff LOD0|LOD1 (GT4)") },
	};
	for (const FScn& S : Scns)
	{
		FScopedTestField Scoped(S.Field);
		FVoxelMeshingRequest RA, RB;
		BuildLODPair(S.FineLOD, S.CoarseLOD, RA, RB);
		FChunkMeshData CA, GA, CB, GB;
		const bool bOk = MeshChunk(RA, CA) && MeshChunk(RB, CB) && MeshChunkGPU(RA, GA) && MeshChunkGPU(RB, GB);
		if (!bOk) { AddInfo(FString::Printf(TEXT("GT0 %s: a mesh failed — skipped"), S.Name)); continue; }

		// 2x2 seam cross-match: which side (A fine / B coarse) does the GPU break? The metric
		// collects B's boundary verts and finds the nearest A vert. CC is the watertight CPU
		// reference; GG is the GT failure; CG (good A vs GPU B) high => GPU-B has bad/extra
		// boundary verts; GC (GPU A vs good B) high => GPU-A fails to provide B's matches.
		auto XM = [&](const FChunkMeshData& A, const FChunkMeshData& B, const TCHAR* Tag)
		{
			int32 N = 0, U = 0; float M = 0.0f;
			MeasureCoarseSideMatch(A, B, S.CoarseCell, FuseTolerance, N, U, M);
			AddInfo(FString::Printf(TEXT("GT0 %s  %-14s: B-band=%d unmatched=%d maxDist=%.1f"), S.Name, Tag, N, U, M));
		};
		XM(CA, CB, TEXT("CPUa-CPUb ref"));
		XM(GA, GB, TEXT("GPUa-GPUb =GT"));
		XM(CA, GB, TEXT("CPUa-GPUb"));
		XM(GA, CB, TEXT("GPUa-CPUb"));
		AddInfo(FString::Printf(TEXT("GT0 %s vertcounts: CPUa=%d GPUa=%d CPUb=%d GPUb=%d"),
			S.Name, CA.Positions.Num(), GA.Positions.Num(), CB.Positions.Num(), GB.Positions.Num()));

		// Dump the worst unmatched verts (CPU -> nearest GPU) for each side, to show whether the
		// GPU vert is "near but offset" (position divergence) or a whole cell away (missing/extra).
		auto DumpSide = [&](const FChunkMeshData& CPU, const FChunkMeshData& GPU, const TCHAR* Side)
		{
			int32 Shown = 0;
			for (const FVector3f& P : CPU.Positions)
			{
				float D = 0.0f; const FVector3f G = NearestPos(P, GPU.Positions, D);
				if (D > FuseTolerance && Shown < 6)
				{
					AddInfo(FString::Printf(TEXT("  GT0 %s %s unmatched CPU(%.0f,%.0f,%.0f)->GPU(%.0f,%.0f,%.0f) d=%.1f"),
						S.Name, Side, P.X, P.Y, P.Z, G.X, G.Y, G.Z, D));
					++Shown;
				}
			}
		};
		DumpSide(CA, GA, TEXT("A"));
		DumpSide(CB, GB, TEXT("B"));
	}
	return true; // diagnostic only — never fails the suite
}

// ==================== GT1: both LOD0 (GPU sanity baseline) ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCGPULODBoundaryGT1Test, "VoxelWorlds.Meshing.DualContour.GPULODBoundary.GT1_BothLOD0",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCGPULODBoundaryGT1Test::RunTest(const FString& Parameters)
{
	DC_GPU_SKIP_IF_NO_RHI(*this);
	// Asserts CPU-level exact match at a plain LOD0 same-LOD seam. Passes: the GPU mesher matches
	// the CPU bit-exactly here (unmatched=0, maxDist=0.00) with full neighbor data supplied.
	FVoxelMeshingRequest RequestA = MakeChunkRequest(FIntVector(0, 0, 0), 0);
	FVoxelMeshingRequest RequestB = MakeChunkRequest(FIntVector(1, 0, 0), 0);
	FillAllNeighborData(RequestA);
	FillAllNeighborData(RequestB);
	RequestA.NeighborLODLevels[1] = 0;
	RequestB.NeighborLODLevels[0] = 0;

	FChunkMeshData MeshA, MeshB;
	TestTrue(TEXT("GPU chunk A meshing should succeed"), MeshChunkGPU(RequestA, MeshA));
	TestTrue(TEXT("GPU chunk B meshing should succeed"), MeshChunkGPU(RequestB, MeshB));

	int32 NumB = 0, UnmatchedB = 0;
	float MaxDist = 0.0f;
	MeasureCoarseSideMatch(MeshA, MeshB, 100.0f, FuseTolerance, NumB, UnmatchedB, MaxDist);
	AddInfo(FString::Printf(TEXT("GT1 LOD0|LOD0 (GPU): B boundary verts=%d unmatched=%d maxDist=%.2f"), NumB, UnmatchedB, MaxDist));

	TestTrue(TEXT("Surface should produce geometry on both sides"), MeshA.Positions.Num() > 0 && MeshB.Positions.Num() > 0);
	TestTrue(TEXT("B boundary should cross the shared face"), NumB > 0);
	TestEqual(TEXT("Same-LOD LOD0 (GPU): every B boundary vertex coincides with an A vertex"), UnmatchedB, 0);
	return true;
}

// ==================== GT2: both LOD1 ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCGPULODBoundaryGT2Test, "VoxelWorlds.Meshing.DualContour.GPULODBoundary.GT2_BothLOD1",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCGPULODBoundaryGT2Test::RunTest(const FString& Parameters)
{
	DC_GPU_SKIP_IF_NO_RHI(*this);
	FVoxelMeshingRequest RequestA = MakeChunkRequest(FIntVector(0, 0, 0), 1);
	FVoxelMeshingRequest RequestB = MakeChunkRequest(FIntVector(1, 0, 0), 1);
	FillAllNeighborData(RequestA);
	FillAllNeighborData(RequestB);
	RequestA.NeighborLODLevels[1] = 1;
	RequestB.NeighborLODLevels[0] = 1;

	FChunkMeshData MeshA, MeshB;
	TestTrue(TEXT("GPU chunk A meshing should succeed"), MeshChunkGPU(RequestA, MeshA));
	TestTrue(TEXT("GPU chunk B meshing should succeed"), MeshChunkGPU(RequestB, MeshB));

	int32 NumB = 0, UnmatchedB = 0;
	float MaxDist = 0.0f;
	MeasureCoarseSideMatch(MeshA, MeshB, 200.0f, FuseTolerance, NumB, UnmatchedB, MaxDist);
	AddInfo(FString::Printf(TEXT("GT2 LOD1|LOD1 (GPU): B boundary verts=%d unmatched=%d maxDist=%.2f"), NumB, UnmatchedB, MaxDist));

	TestTrue(TEXT("Surface should produce geometry on both sides"), MeshA.Positions.Num() > 0 && MeshB.Positions.Num() > 0);
	TestTrue(TEXT("B boundary should cross the shared face"), NumB > 0);
	TestTrue(FString::Printf(TEXT("Same-LOD LOD1 (GPU) boundary sealed (B unmatched %d <= %d)"), UnmatchedB, AcceptedEdgeResidual),
		UnmatchedB <= AcceptedEdgeResidual);
	return true;
}

// ==================== GT3: both LOD2 ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCGPULODBoundaryGT3Test, "VoxelWorlds.Meshing.DualContour.GPULODBoundary.GT3_BothLOD2",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCGPULODBoundaryGT3Test::RunTest(const FString& Parameters)
{
	DC_GPU_SKIP_IF_NO_RHI(*this);
	FVoxelMeshingRequest RequestA = MakeChunkRequest(FIntVector(0, 0, 0), 2);
	FVoxelMeshingRequest RequestB = MakeChunkRequest(FIntVector(1, 0, 0), 2);
	FillAllNeighborData(RequestA);
	FillAllNeighborData(RequestB);
	RequestA.NeighborLODLevels[1] = 2;
	RequestB.NeighborLODLevels[0] = 2;

	FChunkMeshData MeshA, MeshB;
	TestTrue(TEXT("GPU chunk A meshing should succeed"), MeshChunkGPU(RequestA, MeshA));
	TestTrue(TEXT("GPU chunk B meshing should succeed"), MeshChunkGPU(RequestB, MeshB));

	int32 NumB = 0, UnmatchedB = 0;
	float MaxDist = 0.0f;
	MeasureCoarseSideMatch(MeshA, MeshB, 400.0f, FuseTolerance, NumB, UnmatchedB, MaxDist);
	AddInfo(FString::Printf(TEXT("GT3 LOD2|LOD2 (GPU): B boundary verts=%d unmatched=%d maxDist=%.2f"), NumB, UnmatchedB, MaxDist));

	TestTrue(TEXT("Surface should produce geometry on both sides"), MeshA.Positions.Num() > 0 && MeshB.Positions.Num() > 0);
	TestTrue(TEXT("B boundary should cross the shared face"), NumB > 0);
	TestTrue(FString::Printf(TEXT("Same-LOD LOD2 (GPU) boundary sealed (B unmatched %d <= %d)"), UnmatchedB, AcceptedEdgeResidual),
		UnmatchedB <= AcceptedEdgeResidual);
	return true;
}

// ==================== GT4: fine|coarse LOD boundary (the merge) ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCGPULODBoundaryGT4Test, "VoxelWorlds.Meshing.DualContour.GPULODBoundary.GT4_LODBoundaryMerge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCGPULODBoundaryGT4Test::RunTest(const FString& Parameters)
{
	DC_GPU_SKIP_IF_NO_RHI(*this);
	// Asserts CPU-level fine|coarse seal. Passes: the GPU LOD-transition merge seals the dominant
	// seam within the small documented AcceptedEdgeResidual (a few verts at LOD-patch chunk edges).
	struct FCase { ETestField Field; int32 FineLOD; int32 CoarseLOD; float CoarseCell; const TCHAR* Name; };
	const FCase Cases[] = {
		{ ETestField::Smooth,     0, 1, 200.0f, TEXT("Smooth LOD0|LOD1") },
		{ ETestField::NonLinearZ, 0, 1, 200.0f, TEXT("NonLinearZ LOD0|LOD1") },
		{ ETestField::Cliff,      0, 1, 200.0f, TEXT("Cliff LOD0|LOD1") },
		{ ETestField::Smooth,     1, 2, 400.0f, TEXT("Smooth LOD1|LOD2") },
		{ ETestField::NonLinearZ, 1, 2, 400.0f, TEXT("NonLinearZ LOD1|LOD2") },
		{ ETestField::Cliff,      1, 2, 400.0f, TEXT("Cliff LOD1|LOD2") },
	};

	bool bAnyLeak = false;
	for (const FCase& C : Cases)
	{
		FScopedTestField Scoped(C.Field);

		FVoxelMeshingRequest RequestA, RequestB;
		BuildLODPair(C.FineLOD, C.CoarseLOD, RequestA, RequestB);

		FChunkMeshData MeshA, MeshB;
		TestTrue(TEXT("GPU chunk A meshing should succeed"), MeshChunkGPU(RequestA, MeshA));
		TestTrue(TEXT("GPU chunk B meshing should succeed"), MeshChunkGPU(RequestB, MeshB));

		int32 NumB = 0, UnmatchedB = 0;
		float MaxDist = 0.0f;
		MeasureCoarseSideMatch(MeshA, MeshB, C.CoarseCell, FuseTolerance, NumB, UnmatchedB, MaxDist);
		AddInfo(FString::Printf(TEXT("GT4 %s (GPU): coarse verts B=%d unmatched=%d maxDist=%.2f"), C.Name, NumB, UnmatchedB, MaxDist));

		if (NumB > 0 && UnmatchedB > AcceptedEdgeResidual)
		{
			bAnyLeak = true;
			AddError(FString::Printf(TEXT("%s (GPU): LOD boundary NOT sealed - %d/%d coarse verts unmatched (> %d, maxDist=%.1f)"),
				C.Name, UnmatchedB, NumB, AcceptedEdgeResidual, MaxDist));
		}
	}

	TestFalse(TEXT("GPU DC LOD boundary merge should seal the dominant seam across all fields"), bAnyLeak);
	return true;
}

// ==================== GT5: weld must not punch holes ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCGPULODBoundaryGT5Test, "VoxelWorlds.Meshing.DualContour.GPULODBoundary.GT5_WeldHoles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCGPULODBoundaryGT5Test::RunTest(const FString& Parameters)
{
	DC_GPU_SKIP_IF_NO_RHI(*this);
	struct FCase { ETestField Field; int32 SelfLOD; int32 NbrLOD; const TCHAR* Name; };
	const FCase Cases[] = {
		{ ETestField::Smooth,     1, 1, TEXT("Smooth LOD1 shell") },
		{ ETestField::NonLinearZ, 1, 1, TEXT("NonLinearZ LOD1 shell") },
		{ ETestField::Cliff,      1, 1, TEXT("Cliff LOD1 shell") },
		{ ETestField::Smooth,     2, 2, TEXT("Smooth LOD2 shell") },
		{ ETestField::Cliff,      2, 2, TEXT("Cliff LOD2 shell") },
		{ ETestField::Cliff,      0, 1, TEXT("Cliff LOD0 +X coarse") },
	};

	bool bAnyNewHoles = false;
	for (const FCase& C : Cases)
	{
		FScopedTestField Scoped(C.Field);

		FVoxelMeshingRequest Req = MakeChunkRequest(FIntVector(0, 0, 0), C.SelfLOD);
		FillAllNeighborData(Req);
		for (int32 i = 0; i < 6; ++i) { Req.NeighborLODLevels[i] = C.NbrLOD; }

		FChunkMeshData MeshOff, MeshOn;
		{ FScopedWeld W(0); TestTrue(TEXT("GPU mesh weld-off"), MeshChunkGPU(Req, MeshOff)); }
		{ FScopedWeld W(1); TestTrue(TEXT("GPU mesh weld-on"), MeshChunkGPU(Req, MeshOn)); }

		const int32 HolesOff = CountInteriorOpenEdges(MeshOff);
		const int32 HolesOn = CountInteriorOpenEdges(MeshOn);
		AddInfo(FString::Printf(TEXT("GT5 %s (GPU): interior open edges weld-off=%d weld-on=%d"), C.Name, HolesOff, HolesOn));

		if (HolesOn > HolesOff)
		{
			bAnyNewHoles = true;
			AddError(FString::Printf(TEXT("%s (GPU): weld introduced %d interior open edges vs %d weld-off"), C.Name, HolesOn, HolesOff));
		}
	}

	TestFalse(TEXT("GPU DC boundary weld must not punch holes in stride>1 regions"), bAnyNewHoles);
	return true;
}

// ==================== GT6: 3x3 multi-chunk assembly watertightness ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCGPULODBoundaryGT6Test, "VoxelWorlds.Meshing.DualContour.GPULODBoundary.GT6_AssemblyWatertight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCGPULODBoundaryGT6Test::RunTest(const FString& Parameters)
{
	DC_GPU_SKIP_IF_NO_RHI(*this);
	struct FCase { ETestField Field; int32 LOD; const TCHAR* Name; };
	const FCase Cases[] = {
		{ ETestField::Smooth,     1, TEXT("Smooth LOD1") },
		{ ETestField::NonLinearZ, 1, TEXT("NonLinearZ LOD1") },
		{ ETestField::Cliff,      1, TEXT("Cliff LOD1") },
		{ ETestField::Smooth,     2, TEXT("Smooth LOD2") },
		{ ETestField::Cliff,      2, TEXT("Cliff LOD2") },
	};

	bool bAnyHoles = false;
	for (const FCase& C : Cases)
	{
		FScopedTestField Scoped(C.Field);
		int32 HolesOff, Holes;
		{ FScopedWeld W(0); HolesOff = MeshBlockHoles(C.LOD, &MeshChunkGPU); }
		{ FScopedWeld W(1); Holes = MeshBlockHoles(C.LOD, &MeshChunkGPU); }
		AddInfo(FString::Printf(TEXT("GT6 %s (GPU): 3x3 assembly interior seam holes weld-off=%d weld-on=%d"), C.Name, HolesOff, Holes));

		const int32 WeldWorsenTol = 8;
		if (Holes > HolesOff + WeldWorsenTol)
		{
			bAnyHoles = true;
			AddError(FString::Printf(TEXT("%s (GPU): weld WORSENED assembly seams (%d holes vs %d weld-off, tol %d)"),
				C.Name, Holes, HolesOff, WeldWorsenTol));
		}
	}

	TestFalse(TEXT("GPU DC boundary weld must not worsen multi-chunk assembly watertightness"), bAnyHoles);
	return true;
}

// ==================== GT7: faithful 4-chunk LOD corner watertightness ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCGPULODBoundaryGT7Test, "VoxelWorlds.Meshing.DualContour.GPULODBoundary.GT7_FourChunkCorner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCGPULODBoundaryGT7Test::RunTest(const FString& Parameters)
{
	DC_GPU_SKIP_IF_NO_RHI(*this);
	// KnownResidual mirrors the CPU DT7 expectation: the max-min-area diagonal in the GPU
	// quad shader (DCQuadGenerationCS) seals the 4-chunk corner, so weld-on must not exceed
	// the un-welded baseline (residual 0) for any field/LOD.
	struct FCase { ETestField Field; int32 LOD; const TCHAR* Name; int32 KnownResidual; };
	const FCase Cases[] = {
		{ ETestField::Smooth,     1, TEXT("Smooth LOD1"),     0 },
		{ ETestField::NonLinearZ, 1, TEXT("NonLinearZ LOD1"), 0 },
		{ ETestField::Cliff,      1, TEXT("Cliff LOD1"),      0 },
		{ ETestField::Smooth,     2, TEXT("Smooth LOD2"),     0 },
		{ ETestField::Cliff,      2, TEXT("Cliff LOD2"),      0 },
	};

	bool bWorse = false;
	for (const FCase& C : Cases)
	{
		FScopedTestField Scoped(C.Field);
		int32 HolesOff = 0, Holes = 0;
		{ FScopedWeld W(0); HolesOff = MeshCornerBlockHoles(C.LOD, &MeshChunkGPU); }
		{ FScopedWeld W(1); Holes = MeshCornerBlockHoles(C.LOD, &MeshChunkGPU); }
		AddInfo(FString::Printf(TEXT("GT7 %s (GPU): 4-chunk-corner interior open edges weld-off=%d weld-on=%d"), C.Name, HolesOff, Holes));

		const int32 Allowed = FMath::Max(HolesOff, C.KnownResidual);
		if (Holes > Allowed)
		{
			bWorse = true;
			AddError(FString::Printf(TEXT("%s (GPU): weld WORSENED 4-chunk corner (%d open edges vs %d allowed; weld-off=%d, known residual=%d)"),
				C.Name, Holes, Allowed, HolesOff, C.KnownResidual));
		}
	}
	TestFalse(TEXT("GPU DC boundary weld must seal 4-chunk corner watertightness (weld-on must not exceed the un-welded baseline)"), bWorse);
	return true;
}

// ==================== GT8: absent-neighbor boundary must seal (the demo see-through hole) ====================
//
// Reproduces the deterministic demo boundary hole: a chunk that meshed before its neighbors had
// streamed in. UVoxelChunkManager::ExtractNeighborEdgeSlices fills a neighbor slice (and sets its
// presence flag) ONLY when that neighbor is resident, so a not-yet-loaded neighbor leaves the flag
// clear. The CPU GetVoxelAt returns Air there so the boundary edge sees a solid->Air crossing and DC
// seals the seam; the GPU shader used to read the clamped interior voxel instead (solid->solid, no
// crossing) and left the boundary open — a see-through hole + trailing crack along the seam that
// persisted until the chunk happened to remesh. GT1..GT7 above never expose this because their harness
// always supplies every neighbor slice (FillAllNeighborData); the demo streams neighbors in over time.
//
// Mesh one chunk with NO neighbor data (all presence flags clear) on both meshers and require the GPU
// to be no holier than the watertight CPU reference for the identical request.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCGPULODBoundaryGT8Test, "VoxelWorlds.Meshing.DualContour.GPULODBoundary.GT8_AbsentNeighborSeal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCGPULODBoundaryGT8Test::RunTest(const FString& Parameters)
{
	DC_GPU_SKIP_IF_NO_RHI(*this);
	struct FCase { ETestField Field; const TCHAR* Name; };
	const FCase Cases[] = {
		{ ETestField::Smooth,     TEXT("Smooth") },
		{ ETestField::NonLinearZ, TEXT("NonLinearZ") },
		{ ETestField::Cliff,      TEXT("Cliff") },
	};

	bool bAnyHole = false;
	for (const FCase& C : Cases)
	{
		FScopedTestField Scoped(C.Field);
		// Deliberately DO NOT call FillAllNeighborData: every neighbor presence flag stays clear,
		// exactly as when a chunk meshes before its neighbors have streamed in.
		FVoxelMeshingRequest Req = MakeChunkRequest(FIntVector(0, 0, 0), 0);

		FChunkMeshData CpuMesh, GpuMesh;
		TestTrue(TEXT("CPU chunk meshing should succeed"), MeshChunk(Req, CpuMesh));
		TestTrue(TEXT("GPU chunk meshing should succeed"), MeshChunkGPU(Req, GpuMesh));

		const int32 CpuHoles = CountInteriorOpenEdges(CpuMesh);
		const int32 GpuHoles = CountInteriorOpenEdges(GpuMesh);
		AddInfo(FString::Printf(TEXT("GT8 %s (absent neighbors): interior open edges CPU=%d GPU=%d; tris CPU=%d GPU=%d"),
			C.Name, CpuHoles, GpuHoles, CpuMesh.Indices.Num() / 3, GpuMesh.Indices.Num() / 3));

		// The GPU must not be materially holier than the watertight CPU reference. Before the shader
		// Air fallback the GPU left the whole absent-neighbor boundary open (GpuHoles far exceeded
		// CpuHoles); the small tolerance only absorbs QEF-position differences at the boundary walls.
		if (GpuHoles > CpuHoles + AcceptedEdgeResidual)
		{
			bAnyHole = true;
			AddError(FString::Printf(TEXT("%s: GPU left %d interior open edges vs CPU %d (unsealed absent-neighbor boundary = see-through hole)"),
				C.Name, GpuHoles, CpuHoles));
		}
	}
	TestFalse(TEXT("GPU DC must seal absent-neighbor boundaries like the CPU (no see-through boundary holes)"), bAnyHole);
	return true;
}

// ==================== GT9: asymmetric-LOD corners (GPU mirror of DT9) ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCGPULODBoundaryGT9Test, "VoxelWorlds.Meshing.DualContour.GPULODBoundary.GT9_AsymmetricLODCorner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCGPULODBoundaryGT9Test::RunTest(const FString& Parameters)
{
	DC_GPU_SKIP_IF_NO_RHI(*this);
	const bool bSealed = RunAsymmetricLODCornerCases(*this, &MeshChunkGPU, TEXT("GT9"));
	TestTrue(TEXT("GPU DC weld must keep asymmetric-LOD chunk corners within the documented T-junction sliver residual"), bSealed);
	return true;
}

#undef DC_GPU_SKIP_IF_NO_RHI

} // namespace DualContourLODBoundaryTestHelpers
