// Copyright Daniel Raquel. All Rights Reserved.

// Single-owner Marching Cubes seam meshing (SEAM_OWNERSHIP_ARCHITECTURE.md §2.2).
//
// Design: MC vertices and normals are pure functions of local density samples (edge-lerp crossings,
// central-difference gradients) — no QEF, no accumulation-order sensitivity. So each participant's
// boundary band (its first/last cell layer toward the seam) is meshed with a per-participant
// SYNTHETIC FVoxelMeshingRequest — the participant's own volume plus REAL neighbor slices extracted
// from the other participants' snapshots — driven through the exact same ProcessCubeLOD /
// ProcessTransitionCell / geomorph code as legacy whole-chunk meshing. Junctions with the
// interior-domain chunk meshes are bit-identical by construction (same functions, same values).
//
// Reading the participants (world modes review G63): each frame's request reads its own volume and its neighbours' in
// place from the job's snapshots (FVoxelMeshingRequest::NeighbourSnapshotViews), as planes, strips and corners copied
// into the request would hold them (copying cost a face job two whole volumes, an edge job four and a corner job eight,
// to mesh one cell layer); voxel.Seam.MCStats times the stages.
//
// Coverage partition per chunk (cells, per axis, N = ChunkSize/Stride):
//   interior mesh: [1, N-2]   face seam: exactly one axis at {0, N-1}, others [1, N-2]
//   edge seam: exactly two axes at {0, N-1}   corner seam: all three
// Every cell is meshed exactly once across the world (MC cells never extend outside their chunk).
//
// Mixed LOD: the transvoxel ribbon (zero-thickness, on the boundary plane) is emitted by the FACE
// job in the finer participant's frame over the full face extent; ribbon cells that would need
// data beyond the two face participants (the positive-perpendicular perimeter rows) self-skip via
// HasRequiredNeighborData — a known v1 limitation (small boundary-plane gaps along those face
// edges at mixed LOD; the boundary geomorph reduces their visibility). The geomorph runs with a
// BAND-CONFINED ramp (FVoxelMeshingRequest::MorphWidth: zero exactly at the band's inner edge) so morphed band
// vertices stay coincident with the unmorphed interior mesh.

#include "VoxelCPUMarchingCubesMesher.h"
#include "VoxelMeshing.h"
#include "HAL/IConsoleManager.h"
#include <atomic>

/**
 * Where a Marching Cubes seam job's worker time goes (G63; voxel.Seam.MCStats). Every job, per participant frame: the
 * frame's request (setup), its neighbours' views (fill), the band cells, the transition ribbon, the move into the owner's
 * frame (translate) and the request's release (teardown).
 */
namespace VoxelMCSeamStats
{
	enum EStage : int32 { Setup, Fill, Band, Ribbon, Translate, Teardown, NumStages };
	static const TCHAR* StageNames[NumStages] = { TEXT("setup"), TEXT("fill"), TEXT("band"), TEXT("ribbon"), TEXT("translate"), TEXT("teardown") };
	static const TCHAR* JobNames[3] = { TEXT("face"), TEXT("edge"), TEXT("corner") };

	struct FCounters
	{
		std::atomic<int64> Jobs[3] = {};
		std::atomic<int64> Frames[3] = {};
		std::atomic<uint64> JobCycles[3] = {};
		std::atomic<uint64> StageCycles[NumStages] = {};
		std::atomic<int64> BandCells = 0;
		std::atomic<int64> RibbonCells = 0;
		std::atomic<int64> Triangles = 0;
	};
	static FCounters GCounters;

	static void AddStage(EStage Stage, uint64 Cycles)
	{
		GCounters.StageCycles[Stage].fetch_add(Cycles, std::memory_order_relaxed);
	}

	/** Lap timer: each Take adds the cycles since the last to a stage and restarts. */
	struct FLap
	{
		uint64 Mark = FPlatformTime::Cycles64();
		void Take(EStage Stage)
		{
			const uint64 Now = FPlatformTime::Cycles64();
			AddStage(Stage, Now - Mark);
			Mark = Now;
		}
	};

	/** One job: counts it, and its total cycles and triangles when it ends. */
	struct FJobScope
	{
		int32 Type;
		const FChunkMeshData& Mesh;
		uint64 Start = FPlatformTime::Cycles64();
		FJobScope(int32 InType, const FChunkMeshData& InMesh) : Type(InType), Mesh(InMesh)
		{
			GCounters.Jobs[Type].fetch_add(1, std::memory_order_relaxed);
		}
		~FJobScope()
		{
			GCounters.JobCycles[Type].fetch_add(FPlatformTime::Cycles64() - Start, std::memory_order_relaxed);
			GCounters.Triangles.fetch_add(Mesh.Indices.Num() / 3, std::memory_order_relaxed);
		}
	};

	/** Counts one participant frame and its band and ribbon cells. */
	static void AddFrame(int32 Type, const FIntVector& BandMin, const FIntVector& BandMaxEx, int32 Stride, int64 RibbonCells)
	{
		GCounters.Frames[Type].fetch_add(1, std::memory_order_relaxed);
		int64 Cells = 1;
		for (int32 A = 0; A < 3; ++A)
		{
			Cells *= FMath::Max(0, (BandMaxEx[A] - BandMin[A] + Stride - 1) / Stride);
		}
		GCounters.BandCells.fetch_add(Cells, std::memory_order_relaxed);
		GCounters.RibbonCells.fetch_add(RibbonCells, std::memory_order_relaxed);
	}

	static double Ms(uint64 Cycles)
	{
		return static_cast<double>(Cycles) * FPlatformTime::GetSecondsPerCycle64() * 1000.0;
	}

	static void Reset()
	{
		for (int32 i = 0; i < 3; ++i)
		{
			GCounters.Jobs[i] = 0;
			GCounters.Frames[i] = 0;
			GCounters.JobCycles[i] = 0;
		}
		for (int32 i = 0; i < NumStages; ++i)
		{
			GCounters.StageCycles[i] = 0;
		}
		GCounters.BandCells = 0;
		GCounters.RibbonCells = 0;
		GCounters.Triangles = 0;
	}

	static FString Describe()
	{
		int64 Jobs = 0;
		uint64 Cycles = 0;
		FString PerType;
		for (int32 i = 0; i < 3; ++i)
		{
			const int64 N = GCounters.Jobs[i].load();
			const uint64 C = GCounters.JobCycles[i].load();
			Jobs += N;
			Cycles += C;
			PerType += FString::Printf(TEXT("%s%s %lld (%lld frames, %.1f ms, %.3f ms a job)"), i ? TEXT(", ") : TEXT(""),
				JobNames[i], N, GCounters.Frames[i].load(), Ms(C), N > 0 ? Ms(C) / static_cast<double>(N) : 0.0);
		}
		FString Stages;
		uint64 StageSum = 0;
		for (int32 i = 0; i < NumStages; ++i)
		{
			const uint64 C = GCounters.StageCycles[i].load();
			StageSum += C;
			Stages += FString::Printf(TEXT("%s%s %.1f ms (%.1f%%)"), i ? TEXT(", ") : TEXT(""), StageNames[i], Ms(C),
				Cycles > 0 ? 100.0 * static_cast<double>(C) / static_cast<double>(Cycles) : 0.0);
		}
		return FString::Printf(TEXT("%lld jobs, worker %.1f ms (%.3f ms a job): %s; stages: %s, other %.1f ms; ")
			TEXT("band cells %lld, ribbon cells %lld, triangles %lld"),
			Jobs, Ms(Cycles), Jobs > 0 ? Ms(Cycles) / static_cast<double>(Jobs) : 0.0, *PerType,
			*Stages, Ms(Cycles > StageSum ? Cycles - StageSum : 0), GCounters.BandCells.load(), GCounters.RibbonCells.load(),
			GCounters.Triangles.load());
	}
}

// Console: voxel.Seam.MCStats [reset]
static FAutoConsoleCommand GVoxelMCSeamStatsCmd(
	TEXT("voxel.Seam.MCStats"),
	TEXT("Log the Marching Cubes seam jobs' worker time since the last reset, by job type and by stage ")
	TEXT("(setup, fill, band, ribbon, translate, teardown). 'reset' clears the counters."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() > 0 && Args[0].Equals(TEXT("reset"), ESearchCase::IgnoreCase))
		{
			VoxelMCSeamStats::Reset();
			UE_LOG(LogVoxelMeshing, Warning, TEXT("voxel.Seam.MCStats: reset"));
			return;
		}
		UE_LOG(LogVoxelMeshing, Warning, TEXT("voxel.Seam.MCStats: %s"), *VoxelMCSeamStats::Describe());
	}));

namespace VoxelMCSeamDetail
{

/** The request's presence flag of the diagonal edge across (AxisA < AxisB, signs): the sibling there is held. */
static uint32 EdgeFlag(int32 AxisA, bool bPosA, int32 AxisB, bool bPosB)
{
	check(AxisA < AxisB);
	static constexpr uint32 Flags[12] = {
		FVoxelMeshingRequest::EDGE_XPOS_YPOS, FVoxelMeshingRequest::EDGE_XPOS_YNEG, FVoxelMeshingRequest::EDGE_XNEG_YPOS, FVoxelMeshingRequest::EDGE_XNEG_YNEG,
		FVoxelMeshingRequest::EDGE_XPOS_ZPOS, FVoxelMeshingRequest::EDGE_XPOS_ZNEG, FVoxelMeshingRequest::EDGE_XNEG_ZPOS, FVoxelMeshingRequest::EDGE_XNEG_ZNEG,
		FVoxelMeshingRequest::EDGE_YPOS_ZPOS, FVoxelMeshingRequest::EDGE_YPOS_ZNEG, FVoxelMeshingRequest::EDGE_YNEG_ZPOS, FVoxelMeshingRequest::EDGE_YNEG_ZNEG,
	};
	const int32 Pair = (AxisA == 0) ? ((AxisB == 1) ? 0 : 1) : 2; // XY, XZ, YZ
	return Flags[Pair * 4 + (bPosA ? 0 : 2) + (bPosB ? 0 : 1)];
}

/** The request's presence flag of the diagonal corner for the sign triple: the sibling there is held. */
static uint32 CornerFlag(bool bPosX, bool bPosY, bool bPosZ)
{
	static constexpr uint32 Flags[8] = {
		FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZPOS, FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZNEG,
		FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZPOS, FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZNEG,
		FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZPOS, FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZNEG,
		FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZPOS, FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZNEG,
	};
	return Flags[(bPosX ? 0 : 4) + (bPosY ? 0 : 2) + (bPosZ ? 0 : 1)];
}

/** Base synthetic request for one participant frame, reading its volume in place from its snapshot (G63). */
static void InitFrameRequest(FVoxelMeshingRequest& R, const FIntVector& Coord, int32 LOD,
	int32 CS, float VoxelSize, const FVector& WorldOrigin, const FVector& LocalUp, const TArray<FVoxelData>& Volume)
{
	R.ChunkCoord = Coord;
	R.LODLevel = LOD;
	R.ChunkSize = CS;
	R.VoxelSize = VoxelSize;
	R.WorldOrigin = WorldOrigin;
	R.LocalUp = LocalUp;
	R.NeighbourSnapshotViews[13] = Volume.GetData();
	for (int32 i = 0; i < 6; ++i)
	{
		R.NeighborLODLevels[i] = LOD;
	}
	R.TransitionFaces = 0;
	R.EdgeCornerFlags = 0;
	R.MeshCellDomain = EVoxelMeshCellDomain::WholeChunk;
}

/** Read the neighbour at (DX, DY, DZ) in place from its snapshot (G63): its face planes, edge strip or corner voxel. */
static void SetNeighbourView(FVoxelMeshingRequest& R, int32 DX, int32 DY, int32 DZ, const TArray<FVoxelData>& Src)
{
	R.NeighbourSnapshotViews[FVoxelMeshingRequest::VolumeViewIndex(DX, DY, DZ)] = Src.GetData();
}

/** Translate a vertex range into the owner's local frame. */
static void TranslateRange(FChunkMeshData& Mesh, int32 FirstVertex, const FVector3f& Offset)
{
	if (Offset.IsNearlyZero())
	{
		return;
	}
	for (int32 i = FirstVertex; i < Mesh.Positions.Num(); ++i)
	{
		Mesh.Positions[i] += Offset;
	}
}

} // namespace VoxelMCSeamDetail

using namespace VoxelMCSeamDetail;

void FVoxelCPUMarchingCubesMesher::GetSeamJobStats(int64& OutJobs, double& OutWorkerMs)
{
	using namespace VoxelMCSeamStats;
	OutJobs = 0;
	uint64 Cycles = 0;
	for (int32 i = 0; i < 3; ++i)
	{
		OutJobs += GCounters.Jobs[i].load();
		Cycles += GCounters.JobCycles[i].load();
	}
	OutWorkerMs = Ms(Cycles);
}

void FVoxelCPUMarchingCubesMesher::ResetSeamJobStats()
{
	VoxelMCSeamStats::Reset();
}

void FVoxelCPUMarchingCubesMesher::MeshSeamBandCells(
	const FVoxelMeshingRequest& SyntheticRequest,
	const FIntVector& BandMin,
	const FIntVector& BandMaxEx,
	uint8 TransitionMask,
	FChunkMeshData& OutMeshData,
	uint32& TriangleCount)
{
	const int32 Stride = 1 << FMath::Clamp(SyntheticRequest.LODLevel, 0, 7);
	for (int32 Z = BandMin.Z; Z < BandMaxEx.Z; Z += Stride)
	{
		for (int32 Y = BandMin.Y; Y < BandMaxEx.Y; Y += Stride)
		{
			for (int32 X = BandMin.X; X < BandMaxEx.X; X += Stride)
			{
				ProcessCubeLOD(SyntheticRequest, X, Y, Z, Stride, OutMeshData, TriangleCount, TransitionMask);
			}
		}
	}
}

bool FVoxelCPUMarchingCubesMesher::GenerateFaceSeamMeshCPU(
	const FVoxelFaceSeamRequest& SeamRequest, FChunkMeshData& OutMeshData)
{
	if (!bIsInitialized || !SeamRequest.IsValid())
	{
		return false;
	}
	OutMeshData.Reset();
	uint32 TriangleCount = 0;

	const int32 CS = SeamRequest.ChunkSize;
	const float VoxelSize = SeamRequest.VoxelSize;
	const int32 Axis = SeamRequest.Axis;
	const int32 LODA = SeamRequest.LODLevel;
	const int32 LODB = SeamRequest.GetLODLevelB();

	struct FSide { FIntVector Coord; int32 LOD; const TArray<FVoxelData>* Vol; bool bFacingPos; };
	FIntVector BCoord = SeamRequest.OwnerChunkCoord;
	BCoord[Axis] += 1;
	const FSide Sides[2] = {
		{ SeamRequest.OwnerChunkCoord, LODA, SeamRequest.VoxelDataA.Get(), true  },
		{ BCoord,                      LODB, SeamRequest.VoxelDataB.Get(), false },
	};

	VoxelMCSeamStats::FJobScope JobScope(0, OutMeshData);
	for (int32 SideIdx = 0; SideIdx < 2; ++SideIdx)
	{
		const FSide& P = Sides[SideIdx];
		const FSide& Other = Sides[1 - SideIdx];
		const int32 S = 1 << P.LOD;
		const int32 EOther = 1 << Other.LOD;
		const int32 Depth = FMath::Clamp(FMath::Max(S, 2 * EOther) + 1, 1, CS);

		VoxelMCSeamStats::FLap Lap;
		FVoxelMeshingRequest R;
		InitFrameRequest(R, P.Coord, P.LOD, CS, VoxelSize, SeamRequest.WorldOrigin, SeamRequest.LocalUp, *P.Vol);
		R.NeighborPlaneDepth = Depth;
		Lap.Take(VoxelMCSeamStats::Setup);
		FIntVector Dir = FIntVector::ZeroValue;
		Dir[Axis] = P.bFacingPos ? 1 : -1;
		SetNeighbourView(R, Dir.X, Dir.Y, Dir.Z, *Other.Vol);
		Lap.Take(VoxelMCSeamStats::Fill);
		const int32 FacingFaceIdx = Axis * 2 + (P.bFacingPos ? 1 : 0);
		R.NeighborLODLevels[FacingFaceIdx] = Other.LOD;
		uint8 Mask = 0;
		if (Other.LOD > P.LOD)
		{
			Mask = 1 << FacingFaceIdx;
			R.TransitionFaces = Mask;
			// Band-confined geomorph ramp: zero at the band's inner edge (depth S voxels),
			// expressed in the coarse neighbor's cells.
			R.MorphWidth = static_cast<float>(S) / static_cast<float>(EOther);
		}

		FIntVector BandMin(S, S, S), BandMaxEx(CS - S, CS - S, CS - S);
		BandMin[Axis] = P.bFacingPos ? (CS - S) : 0;
		BandMaxEx[Axis] = BandMin[Axis] + S;

		const int32 FirstVertex = OutMeshData.Positions.Num();
		MeshSeamBandCells(R, BandMin, BandMaxEx, Mask, OutMeshData, TriangleCount);
		Lap.Take(VoxelMCSeamStats::Band);
		VoxelMCSeamStats::AddFrame(0, BandMin, BandMaxEx, S,
			(Other.LOD > P.LOD) ? static_cast<int64>(CS / EOther) * static_cast<int64>(CS / EOther) : 0);

		// Mixed LOD: the finer side emits the transvoxel ribbon (full face; cells needing data
		// beyond the two participants self-skip via HasRequiredNeighborData).
		if (Other.LOD > P.LOD)
		{
			const int32 CoarserStride = EOther;
			const int32 BoundaryPos = P.bFacingPos ? (CS - S) : 0;
			for (int32 FP2 = 0; FP2 < CS; FP2 += CoarserStride)
			{
				for (int32 FP1 = 0; FP1 < CS; FP1 += CoarserStride)
				{
					int32 CellX, CellY, CellZ;
					switch (Axis)
					{
					case 0:  CellX = BoundaryPos; CellY = FP1; CellZ = FP2; break;
					case 1:  CellX = FP1; CellY = BoundaryPos; CellZ = FP2; break;
					default: CellX = FP1; CellY = FP2; CellZ = BoundaryPos; break;
					}
					ProcessTransitionCell(R, CellX, CellY, CellZ, CoarserStride, FacingFaceIdx,
						OutMeshData, TriangleCount);
				}
			}
		}
		Lap.Take(VoxelMCSeamStats::Ribbon);

		const FIntVector DeltaP = P.Coord - SeamRequest.OwnerChunkCoord;
		TranslateRange(OutMeshData, FirstVertex,
			FVector3f(DeltaP.X, DeltaP.Y, DeltaP.Z) * static_cast<float>(CS) * VoxelSize);
		Lap.Take(VoxelMCSeamStats::Translate);
		R = FVoxelMeshingRequest(); // release the frame's arrays here, so their release is timed
		Lap.Take(VoxelMCSeamStats::Teardown);
	}
	return true;
}

bool FVoxelCPUMarchingCubesMesher::GenerateEdgeSeamMeshCPU(
	const FVoxelEdgeSeamRequest& SeamRequest, FChunkMeshData& OutMeshData)
{
	if (!bIsInitialized || !SeamRequest.IsValid())
	{
		return false;
	}
	OutMeshData.Reset();
	uint32 TriangleCount = 0;

	const int32 CS = SeamRequest.ChunkSize;
	const float VoxelSize = SeamRequest.VoxelSize;
	const int32 EdgeAxis = SeamRequest.EdgeAxis;
	const int32 P1 = (EdgeAxis == 0) ? 1 : 0;
	const int32 P2 = (EdgeAxis == 2) ? 1 : 2;

	VoxelMCSeamStats::FJobScope JobScope(1, OutMeshData);
	for (int32 q = 0; q < 4; ++q)
	{
		const int32 qa = q & 1;
		const int32 qb = q >> 1;
		FIntVector Coord = SeamRequest.OwnerChunkCoord;
		Coord[P1] += qa;
		Coord[P2] += qb;
		const int32 LOD = SeamRequest.GetLODLevelOf(q);
		const int32 S = 1 << LOD;
		const int32 LodP1 = SeamRequest.GetLODLevelOf(q ^ 1);
		const int32 LodP2 = SeamRequest.GetLODLevelOf(q ^ 2);
		const int32 MaxE = FMath::Max3(S, 2 * (1 << LodP1), 2 * (1 << LodP2));
		const int32 Depth = FMath::Clamp(MaxE + 1, 1, CS);
		const bool bPosP1 = (qa == 0);
		const bool bPosP2 = (qb == 0);

		VoxelMCSeamStats::FLap Lap;
		FVoxelMeshingRequest R;
		InitFrameRequest(R, Coord, LOD, CS, VoxelSize, SeamRequest.WorldOrigin, SeamRequest.LocalUp, *SeamRequest.VoxelData[q]);
		R.NeighborPlaneDepth = Depth;
		Lap.Take(VoxelMCSeamStats::Setup);

		const int32 AxisA = FMath::Min(P1, P2);
		const int32 AxisB = FMath::Max(P1, P2);
		const bool bPosA = (AxisA == P1) ? bPosP1 : bPosP2;
		const bool bPosB = (AxisB == P2) ? bPosP2 : bPosP1;
		FIntVector Dir1 = FIntVector::ZeroValue, Dir2 = FIntVector::ZeroValue;
		Dir1[P1] = bPosP1 ? 1 : -1;
		Dir2[P2] = bPosP2 ? 1 : -1;
		const FIntVector Diagonal = Dir1 + Dir2;
		SetNeighbourView(R, Dir1.X, Dir1.Y, Dir1.Z, *SeamRequest.VoxelData[q ^ 1]);
		SetNeighbourView(R, Dir2.X, Dir2.Y, Dir2.Z, *SeamRequest.VoxelData[q ^ 2]);
		SetNeighbourView(R, Diagonal.X, Diagonal.Y, Diagonal.Z, *SeamRequest.VoxelData[q ^ 3]);
		R.EdgeCornerFlags |= EdgeFlag(AxisA, bPosA, AxisB, bPosB);
		Lap.Take(VoxelMCSeamStats::Fill);

		const int32 FaceP1 = P1 * 2 + (bPosP1 ? 1 : 0);
		const int32 FaceP2 = P2 * 2 + (bPosP2 ? 1 : 0);
		R.NeighborLODLevels[FaceP1] = LodP1;
		R.NeighborLODLevels[FaceP2] = LodP2;
		uint8 Mask = 0;
		int32 MaxCoarser = 0;
		if (LodP1 > LOD) { Mask |= (1 << FaceP1); MaxCoarser = FMath::Max(MaxCoarser, 1 << LodP1); }
		if (LodP2 > LOD) { Mask |= (1 << FaceP2); MaxCoarser = FMath::Max(MaxCoarser, 1 << LodP2); }
		if (Mask)
		{
			R.TransitionFaces = Mask;
			R.MorphWidth = static_cast<float>(S) / static_cast<float>(MaxCoarser);
		}

		FIntVector BandMin(S, S, S), BandMaxEx(CS - S, CS - S, CS - S);
		BandMin[P1] = bPosP1 ? (CS - S) : 0;
		BandMaxEx[P1] = BandMin[P1] + S;
		BandMin[P2] = bPosP2 ? (CS - S) : 0;
		BandMaxEx[P2] = BandMin[P2] + S;

		const int32 FirstVertex = OutMeshData.Positions.Num();
		MeshSeamBandCells(R, BandMin, BandMaxEx, Mask, OutMeshData, TriangleCount);
		Lap.Take(VoxelMCSeamStats::Band);
		VoxelMCSeamStats::AddFrame(1, BandMin, BandMaxEx, S, 0);
		const FIntVector Delta = Coord - SeamRequest.OwnerChunkCoord;
		TranslateRange(OutMeshData, FirstVertex,
			FVector3f(Delta.X, Delta.Y, Delta.Z) * static_cast<float>(CS) * VoxelSize);
		Lap.Take(VoxelMCSeamStats::Translate);
		R = FVoxelMeshingRequest(); // release the frame's arrays here, so their release is timed
		Lap.Take(VoxelMCSeamStats::Teardown);
	}
	return true;
}

bool FVoxelCPUMarchingCubesMesher::GenerateCornerSeamMeshCPU(
	const FVoxelCornerSeamRequest& SeamRequest, FChunkMeshData& OutMeshData)
{
	if (!bIsInitialized || !SeamRequest.IsValid())
	{
		return false;
	}
	OutMeshData.Reset();
	uint32 TriangleCount = 0;

	const int32 CS = SeamRequest.ChunkSize;
	const float VoxelSize = SeamRequest.VoxelSize;

	VoxelMCSeamStats::FJobScope JobScope(2, OutMeshData);
	for (int32 o = 0; o < 8; ++o)
	{
		const int32 dx = o & 1;
		const int32 dy = (o >> 1) & 1;
		const int32 dz = (o >> 2) & 1;
		const FIntVector Coord = SeamRequest.OwnerChunkCoord + FIntVector(dx, dy, dz);
		const int32 LOD = SeamRequest.GetLODLevelOf(o);
		const int32 S = 1 << LOD;
		const bool bPos[3] = { dx == 0, dy == 0, dz == 0 };

		int32 MaxCoarser = S;
		int32 FaceLods[3];
		for (int32 A = 0; A < 3; ++A)
		{
			FaceLods[A] = SeamRequest.GetLODLevelOf(o ^ (1 << A));
			MaxCoarser = FMath::Max(MaxCoarser, 2 * (1 << FaceLods[A]));
		}
		const int32 Depth = FMath::Clamp(MaxCoarser + 1, 1, CS);

		VoxelMCSeamStats::FLap Lap;
		FVoxelMeshingRequest R;
		InitFrameRequest(R, Coord, LOD, CS, VoxelSize, SeamRequest.WorldOrigin, SeamRequest.LocalUp, *SeamRequest.VoxelData[o]);
		R.NeighborPlaneDepth = Depth;
		Lap.Take(VoxelMCSeamStats::Setup);

		// Toward each sibling: +1 where this participant sits at 0 on that axis.
		const int32 Dir[3] = { bPos[0] ? 1 : -1, bPos[1] ? 1 : -1, bPos[2] ? 1 : -1 };
		uint8 Mask = 0;
		int32 MaxCoarserStride = 0;
		for (int32 A = 0; A < 3; ++A)
		{
			SetNeighbourView(R, A == 0 ? Dir[0] : 0, A == 1 ? Dir[1] : 0, A == 2 ? Dir[2] : 0, *SeamRequest.VoxelData[o ^ (1 << A)]);
			const int32 FaceIdx = A * 2 + (bPos[A] ? 1 : 0);
			R.NeighborLODLevels[FaceIdx] = FaceLods[A];
			if (FaceLods[A] > LOD)
			{
				Mask |= (1 << FaceIdx);
				MaxCoarserStride = FMath::Max(MaxCoarserStride, 1 << FaceLods[A]);
			}
		}
		// The three diagonal edge strips (sibling across both axes of each pair).
		const int32 Pairs[3][2] = { {0, 1}, {0, 2}, {1, 2} };
		for (int32 pi = 0; pi < 3; ++pi)
		{
			const int32 AxisA = Pairs[pi][0];
			const int32 AxisB = Pairs[pi][1];
			const int32 FreeAxis = 3 - AxisA - AxisB;
			const int32 Sibling = o ^ (1 << AxisA) ^ (1 << AxisB);
			SetNeighbourView(R, FreeAxis == 0 ? 0 : Dir[0], FreeAxis == 1 ? 0 : Dir[1], FreeAxis == 2 ? 0 : Dir[2],
				*SeamRequest.VoxelData[Sibling]);
			R.EdgeCornerFlags |= EdgeFlag(AxisA, bPos[AxisA], AxisB, bPos[AxisB]);
		}
		// The diagonal corner voxel.
		SetNeighbourView(R, Dir[0], Dir[1], Dir[2], *SeamRequest.VoxelData[o ^ 7]);
		R.EdgeCornerFlags |= CornerFlag(bPos[0], bPos[1], bPos[2]);
		Lap.Take(VoxelMCSeamStats::Fill);
		if (Mask)
		{
			R.TransitionFaces = Mask;
			R.MorphWidth = static_cast<float>(S) / static_cast<float>(MaxCoarserStride);
		}

		FIntVector BandMin, BandMaxEx;
		for (int32 A = 0; A < 3; ++A)
		{
			BandMin[A] = bPos[A] ? (CS - S) : 0;
			BandMaxEx[A] = BandMin[A] + S;
		}

		const int32 FirstVertex = OutMeshData.Positions.Num();
		MeshSeamBandCells(R, BandMin, BandMaxEx, Mask, OutMeshData, TriangleCount);
		Lap.Take(VoxelMCSeamStats::Band);
		VoxelMCSeamStats::AddFrame(2, BandMin, BandMaxEx, S, 0);
		const FIntVector Delta = Coord - SeamRequest.OwnerChunkCoord;
		TranslateRange(OutMeshData, FirstVertex,
			FVector3f(Delta.X, Delta.Y, Delta.Z) * static_cast<float>(CS) * VoxelSize);
		Lap.Take(VoxelMCSeamStats::Translate);
		R = FVoxelMeshingRequest(); // release the frame's arrays here, so their release is timed
		Lap.Take(VoxelMCSeamStats::Teardown);
	}
	return true;
}
