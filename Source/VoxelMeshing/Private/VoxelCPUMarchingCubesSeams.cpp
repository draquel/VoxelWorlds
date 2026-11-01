// Copyright Daniel Raquel. All Rights Reserved.

// Seam-ownership P3: single-owner Marching Cubes seam meshing (SEAM_OWNERSHIP_ARCHITECTURE.md §2.2).
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
// place from the job's snapshots (FVoxelMeshingRequest::NeighbourSnapshotViews), as the planes, strips and corner the frame used to
// copy would hold them. Copying them cost a face job two whole volumes, an edge job four and a corner job eight (with
// the planes), to mesh one cell layer; voxel.Seam.MCReadInPlace 0 copies them as before, voxel.Seam.MCStats times the
// stages.
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
// BAND-CONFINED ramp (MorphWidthOverride: zero exactly at the band's inner edge) so morphed band
// vertices stay coincident with the unmorphed interior mesh.

#include "VoxelCPUMarchingCubesMesher.h"
#include "VoxelMeshing.h"
#include "HAL/IConsoleManager.h"
#include <atomic>

static TAutoConsoleVariable<int32> CVarMCSeamReadInPlace(
	TEXT("voxel.Seam.MCReadInPlace"),
	1,
	TEXT("Marching Cubes seam jobs (world modes review G63): 1 = each participant's frame reads the participants' voxels in ")
	TEXT("place from the job's snapshots (default); 0 = copy the participant's whole volume and its neighbours' planes, ")
	TEXT("strips and corners into a request first, as before (a face job copied two volumes, an edge job four, a corner ")
	TEXT("job eight, to mesh one cell layer). The seam meshes are the same either way. Read per job."),
	ECVF_Default);

/**
 * Where a Marching Cubes seam job's worker time goes (G63; voxel.Seam.MCStats). Every job, per participant frame: the
 * frame's request (setup), its neighbours' planes, strips and corners (fill), the band cells, the transition ribbon, the
 * move into the owner's frame (translate) and the request's release (teardown).
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
		std::atomic<int64> InPlaceJobs = 0;
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
		FJobScope(int32 InType, bool bInPlace, const FChunkMeshData& InMesh) : Type(InType), Mesh(InMesh)
		{
			GCounters.Jobs[Type].fetch_add(1, std::memory_order_relaxed);
			GCounters.InPlaceJobs.fetch_add(bInPlace ? 1 : 0, std::memory_order_relaxed);
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
		GCounters.InPlaceJobs = 0;
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
		return FString::Printf(TEXT("%lld jobs (%lld read in place), worker %.1f ms (%.3f ms a job): %s; stages: %s, other %.1f ms; ")
			TEXT("band cells %lld, ribbon cells %lld, triangles %lld; voxel.Seam.MCReadInPlace %d"),
			Jobs, GCounters.InPlaceJobs.load(), Ms(Cycles), Jobs > 0 ? Ms(Cycles) / static_cast<double>(Jobs) : 0.0, *PerType,
			*Stages, Ms(Cycles > StageSum ? Cycles - StageSum : 0), GCounters.BandCells.load(), GCounters.RibbonCells.load(),
			GCounters.Triangles.load(), CVarMCSeamReadInPlace.GetValueOnAnyThread());
	}
}

// Console: voxel.Seam.MCStats [reset]
static FAutoConsoleCommand GVoxelMCSeamStatsCmd(
	TEXT("voxel.Seam.MCStats"),
	TEXT("Log the Marching Cubes seam jobs' worker time since the last reset, by job type and by stage (world modes review ")
	TEXT("G63: setup, fill, band, ribbon, translate, teardown). 'reset' clears the counters."),
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

// In-plane index layout must match VoxelNeighborSlices::Extract (VoxelStreaming) / GetVoxelAt:
// X faces -> [Y + Z*CS], Y faces -> [X + Z*CS], Z faces -> [X + Y*CS].
static FORCEINLINE int32 PlaneIndex(int32 Axis, int32 X, int32 Y, int32 Z, int32 CS)
{
	switch (Axis)
	{
	case 0:  return Y + Z * CS;
	case 1:  return X + Z * CS;
	default: return X + Y * CS;
	}
}

/** Fill a facing face's plane-0 + deep arrays from a sibling volume. bSiblingOnPos: the sibling
 *  sits on the consumer's POSITIVE side of Axis (so its LOW planes face the consumer). */
static void FillFacePlanes(
	const TArray<FVoxelData>& Src, int32 CS, int32 Axis, bool bSiblingOnPos, int32 Depth,
	TArray<FVoxelData>& OutPlane0, TArray<FVoxelData>& OutDeep)
{
	const int32 SliceSize = CS * CS;
	OutPlane0.SetNumUninitialized(SliceSize);
	const int32 ExtraPlanes = Depth - 1;
	if (ExtraPlanes > 0)
	{
		OutDeep.SetNumUninitialized(ExtraPlanes * SliceSize);
	}
	for (int32 d = 0; d < Depth; ++d)
	{
		const int32 AxisCoord = bSiblingOnPos ? d : (CS - 1 - d);
		for (int32 b = 0; b < CS; ++b)
		{
			for (int32 a = 0; a < CS; ++a)
			{
				// (a, b) map to the two non-Axis axes in ascending order — matching PlaneIndex.
				int32 V[3];
				V[Axis] = AxisCoord;
				V[(Axis == 0) ? 1 : 0] = a;
				V[(Axis == 2) ? 1 : 2] = b;
				const FVoxelData& Vox = Src[V[0] + V[1] * CS + V[2] * CS * CS];
				const int32 InPlane = a + b * CS;
				if (d == 0)
				{
					OutPlane0[InPlane] = Vox;
				}
				else
				{
					OutDeep[(d - 1) * SliceSize + InPlane] = Vox;
				}
			}
		}
	}
}

/** Fill a diagonal edge strip + deep grid from a sibling volume. AxisA < AxisB are the pinned
 *  axes; bPosA/bPosB: sibling on the consumer's positive side of that axis. FreeAxis varies. */
static void FillEdgeStrip(
	const TArray<FVoxelData>& Src, int32 CS, int32 AxisA, bool bPosA, int32 AxisB, bool bPosB,
	int32 FreeAxis, int32 Depth,
	TArray<FVoxelData>& OutStrip, TArray<FVoxelData>& OutDeep)
{
	OutStrip.SetNumUninitialized(CS);
	if (Depth > 1)
	{
		OutDeep.SetNumUninitialized(Depth * Depth * CS);
	}
	for (int32 dA = 0; dA < Depth; ++dA)
	{
		const int32 CA = bPosA ? dA : (CS - 1 - dA);
		for (int32 dB = 0; dB < Depth; ++dB)
		{
			const int32 CB = bPosB ? dB : (CS - 1 - dB);
			for (int32 f = 0; f < CS; ++f)
			{
				int32 V[3];
				V[AxisA] = CA;
				V[AxisB] = CB;
				V[FreeAxis] = f;
				const FVoxelData& Vox = Src[V[0] + V[1] * CS + V[2] * CS * CS];
				if (dA == 0 && dB == 0)
				{
					OutStrip[f] = Vox;
				}
				if (Depth > 1)
				{
					OutDeep[(dA * Depth + dB) * CS + f] = Vox;
				}
			}
		}
	}
}

/** Fill a diagonal corner voxel + deep box from a sibling volume. */
static void FillCorner(
	const TArray<FVoxelData>& Src, int32 CS, bool bPosX, bool bPosY, bool bPosZ, int32 Depth,
	FVoxelData& OutScalar, TArray<FVoxelData>& OutDeep)
{
	if (Depth > 1)
	{
		OutDeep.SetNumUninitialized(Depth * Depth * Depth);
	}
	for (int32 a = 0; a < Depth; ++a)
	{
		const int32 CX = bPosX ? a : (CS - 1 - a);
		for (int32 b = 0; b < Depth; ++b)
		{
			const int32 CY = bPosY ? b : (CS - 1 - b);
			for (int32 c = 0; c < Depth; ++c)
			{
				const int32 CZ = bPosZ ? c : (CS - 1 - c);
				const FVoxelData& Vox = Src[CX + CY * CS + CZ * CS * CS];
				if (a == 0 && b == 0 && c == 0)
				{
					OutScalar = Vox;
				}
				if (Depth > 1)
				{
					OutDeep[(a * Depth + b) * Depth + c] = Vox;
				}
			}
		}
	}
}

/** Select the request's face slice/deep arrays for (Axis, bPos). */
static void SelectFaceArrays(FVoxelMeshingRequest& R, int32 Axis, bool bPos,
	TArray<FVoxelData>*& OutPlane, TArray<FVoxelData>*& OutDeep)
{
	switch (Axis * 2 + (bPos ? 1 : 0))
	{
	case 0: OutPlane = &R.NeighborXNeg; OutDeep = &R.NeighborXNegDeep; break;
	case 1: OutPlane = &R.NeighborXPos; OutDeep = &R.NeighborXPosDeep; break;
	case 2: OutPlane = &R.NeighborYNeg; OutDeep = &R.NeighborYNegDeep; break;
	case 3: OutPlane = &R.NeighborYPos; OutDeep = &R.NeighborYPosDeep; break;
	case 4: OutPlane = &R.NeighborZNeg; OutDeep = &R.NeighborZNegDeep; break;
	default: OutPlane = &R.NeighborZPos; OutDeep = &R.NeighborZPosDeep; break;
	}
}

/** Select the request's edge strip/deep arrays + presence flag for canonical (AxisA<AxisB, signs). */
static void SelectEdgeArrays(FVoxelMeshingRequest& R, int32 AxisA, bool bPosA, int32 AxisB, bool bPosB,
	TArray<FVoxelData>*& OutStrip, TArray<FVoxelData>*& OutDeep, uint32& OutFlag)
{
	check(AxisA < AxisB);
	const int32 Pair = (AxisA == 0) ? ((AxisB == 1) ? 0 : 1) : 2; // XY, XZ, YZ
	const int32 Sel = Pair * 4 + (bPosA ? 0 : 2) + (bPosB ? 0 : 1);
	switch (Sel)
	{
	case 0:  OutStrip = &R.EdgeXPosYPos; OutDeep = &R.EdgeXPosYPosDeep; OutFlag = FVoxelMeshingRequest::EDGE_XPOS_YPOS; break;
	case 1:  OutStrip = &R.EdgeXPosYNeg; OutDeep = &R.EdgeXPosYNegDeep; OutFlag = FVoxelMeshingRequest::EDGE_XPOS_YNEG; break;
	case 2:  OutStrip = &R.EdgeXNegYPos; OutDeep = &R.EdgeXNegYPosDeep; OutFlag = FVoxelMeshingRequest::EDGE_XNEG_YPOS; break;
	case 3:  OutStrip = &R.EdgeXNegYNeg; OutDeep = &R.EdgeXNegYNegDeep; OutFlag = FVoxelMeshingRequest::EDGE_XNEG_YNEG; break;
	case 4:  OutStrip = &R.EdgeXPosZPos; OutDeep = &R.EdgeXPosZPosDeep; OutFlag = FVoxelMeshingRequest::EDGE_XPOS_ZPOS; break;
	case 5:  OutStrip = &R.EdgeXPosZNeg; OutDeep = &R.EdgeXPosZNegDeep; OutFlag = FVoxelMeshingRequest::EDGE_XPOS_ZNEG; break;
	case 6:  OutStrip = &R.EdgeXNegZPos; OutDeep = &R.EdgeXNegZPosDeep; OutFlag = FVoxelMeshingRequest::EDGE_XNEG_ZPOS; break;
	case 7:  OutStrip = &R.EdgeXNegZNeg; OutDeep = &R.EdgeXNegZNegDeep; OutFlag = FVoxelMeshingRequest::EDGE_XNEG_ZNEG; break;
	case 8:  OutStrip = &R.EdgeYPosZPos; OutDeep = &R.EdgeYPosZPosDeep; OutFlag = FVoxelMeshingRequest::EDGE_YPOS_ZPOS; break;
	case 9:  OutStrip = &R.EdgeYPosZNeg; OutDeep = &R.EdgeYPosZNegDeep; OutFlag = FVoxelMeshingRequest::EDGE_YPOS_ZNEG; break;
	case 10: OutStrip = &R.EdgeYNegZPos; OutDeep = &R.EdgeYNegZPosDeep; OutFlag = FVoxelMeshingRequest::EDGE_YNEG_ZPOS; break;
	default: OutStrip = &R.EdgeYNegZNeg; OutDeep = &R.EdgeYNegZNegDeep; OutFlag = FVoxelMeshingRequest::EDGE_YNEG_ZNEG; break;
	}
}

/** Select the request's corner scalar/deep + presence flag for the sign triple. */
static void SelectCornerArrays(FVoxelMeshingRequest& R, bool bPosX, bool bPosY, bool bPosZ,
	FVoxelData*& OutScalar, TArray<FVoxelData>*& OutDeep, uint32& OutFlag)
{
	const int32 Sel = (bPosX ? 0 : 4) + (bPosY ? 0 : 2) + (bPosZ ? 0 : 1);
	switch (Sel)
	{
	case 0:  OutScalar = &R.CornerXPosYPosZPos; OutDeep = &R.CornerXPosYPosZPosDeep; OutFlag = FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZPOS; break;
	case 1:  OutScalar = &R.CornerXPosYPosZNeg; OutDeep = &R.CornerXPosYPosZNegDeep; OutFlag = FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZNEG; break;
	case 2:  OutScalar = &R.CornerXPosYNegZPos; OutDeep = &R.CornerXPosYNegZPosDeep; OutFlag = FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZPOS; break;
	case 3:  OutScalar = &R.CornerXPosYNegZNeg; OutDeep = &R.CornerXPosYNegZNegDeep; OutFlag = FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZNEG; break;
	case 4:  OutScalar = &R.CornerXNegYPosZPos; OutDeep = &R.CornerXNegYPosZPosDeep; OutFlag = FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZPOS; break;
	case 5:  OutScalar = &R.CornerXNegYPosZNeg; OutDeep = &R.CornerXNegYPosZNegDeep; OutFlag = FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZNEG; break;
	case 6:  OutScalar = &R.CornerXNegYNegZPos; OutDeep = &R.CornerXNegYNegZPosDeep; OutFlag = FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZPOS; break;
	default: OutScalar = &R.CornerXNegYNegZNeg; OutDeep = &R.CornerXNegYNegZNegDeep; OutFlag = FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZNEG; break;
	}
}

/** Base synthetic request for one participant frame. bInPlace: read the volume from its snapshot (G63), else copy it. */
static void InitFrameRequest(FVoxelMeshingRequest& R, const FIntVector& Coord, int32 LOD,
	int32 CS, float VoxelSize, const FVector& WorldOrigin, const FVector& LocalUp, const TArray<FVoxelData>& Volume,
	bool bInPlace)
{
	R.ChunkCoord = Coord;
	R.LODLevel = LOD;
	R.ChunkSize = CS;
	R.VoxelSize = VoxelSize;
	R.WorldOrigin = WorldOrigin;
	R.LocalUp = LocalUp;
	if (bInPlace)
	{
		R.NeighbourSnapshotViews[13] = Volume.GetData();
	}
	else
	{
		R.VoxelData = Volume; // per-frame copy (before G63)
	}
	for (int32 i = 0; i < 6; ++i)
	{
		R.NeighborLODLevels[i] = LOD;
	}
	R.TransitionFaces = 0;
	R.EdgeCornerFlags = 0;
	R.MeshCellDomain = EVoxelMeshCellDomain::Full;
}

/** Read the neighbour at (DX, DY, DZ) in place from its snapshot (G63): the voxels FillFacePlanes, FillEdgeStrip and FillCorner copy. */
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

void FVoxelCPUMarchingCubesMesher::GetSeamJobStats(int64& OutJobs, int64& OutInPlaceJobs, double& OutWorkerMs)
{
	using namespace VoxelMCSeamStats;
	OutJobs = 0;
	uint64 Cycles = 0;
	for (int32 i = 0; i < 3; ++i)
	{
		OutJobs += GCounters.Jobs[i].load();
		Cycles += GCounters.JobCycles[i].load();
	}
	OutInPlaceJobs = GCounters.InPlaceJobs.load();
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

	const bool bInPlace = CVarMCSeamReadInPlace.GetValueOnAnyThread() != 0;
	VoxelMCSeamStats::FJobScope JobScope(0, bInPlace, OutMeshData);
	for (int32 SideIdx = 0; SideIdx < 2; ++SideIdx)
	{
		const FSide& P = Sides[SideIdx];
		const FSide& Other = Sides[1 - SideIdx];
		const int32 S = 1 << P.LOD;
		const int32 EOther = 1 << Other.LOD;
		const int32 Depth = FMath::Clamp(FMath::Max(S, 2 * EOther) + 1, 1, CS);

		VoxelMCSeamStats::FLap Lap;
		FVoxelMeshingRequest R;
		InitFrameRequest(R, P.Coord, P.LOD, CS, VoxelSize, SeamRequest.WorldOrigin, SeamRequest.LocalUp, *P.Vol, bInPlace);
		R.NeighborPlaneDepth = Depth;
		Lap.Take(VoxelMCSeamStats::Setup);
		if (bInPlace)
		{
			FIntVector Dir = FIntVector::ZeroValue;
			Dir[Axis] = P.bFacingPos ? 1 : -1;
			SetNeighbourView(R, Dir.X, Dir.Y, Dir.Z, *Other.Vol);
		}
		else
		{
			TArray<FVoxelData>* Plane; TArray<FVoxelData>* Deep;
			SelectFaceArrays(R, Axis, P.bFacingPos, Plane, Deep);
			FillFacePlanes(*Other.Vol, CS, Axis, P.bFacingPos, Depth, *Plane, *Deep);
		}
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
			R.MorphWidthOverride = static_cast<float>(S) / static_cast<float>(EOther);
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

	const bool bInPlace = CVarMCSeamReadInPlace.GetValueOnAnyThread() != 0;
	VoxelMCSeamStats::FJobScope JobScope(1, bInPlace, OutMeshData);
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
		InitFrameRequest(R, Coord, LOD, CS, VoxelSize, SeamRequest.WorldOrigin, SeamRequest.LocalUp, *SeamRequest.VoxelData[q], bInPlace);
		R.NeighborPlaneDepth = Depth;
		Lap.Take(VoxelMCSeamStats::Setup);

		TArray<FVoxelData>* Plane; TArray<FVoxelData>* Deep;
		TArray<FVoxelData>* Strip; uint32 Flag;
		const int32 AxisA = FMath::Min(P1, P2);
		const int32 AxisB = FMath::Max(P1, P2);
		const bool bPosA = (AxisA == P1) ? bPosP1 : bPosP2;
		const bool bPosB = (AxisB == P2) ? bPosP2 : bPosP1;
		if (bInPlace)
		{
			FIntVector Dir1 = FIntVector::ZeroValue, Dir2 = FIntVector::ZeroValue;
			Dir1[P1] = bPosP1 ? 1 : -1;
			Dir2[P2] = bPosP2 ? 1 : -1;
			const FIntVector Diagonal = Dir1 + Dir2;
			SetNeighbourView(R, Dir1.X, Dir1.Y, Dir1.Z, *SeamRequest.VoxelData[q ^ 1]);
			SetNeighbourView(R, Dir2.X, Dir2.Y, Dir2.Z, *SeamRequest.VoxelData[q ^ 2]);
			SetNeighbourView(R, Diagonal.X, Diagonal.Y, Diagonal.Z, *SeamRequest.VoxelData[q ^ 3]);
			SelectEdgeArrays(R, AxisA, bPosA, AxisB, bPosB, Strip, Deep, Flag);
		}
		else
		{
			SelectFaceArrays(R, P1, bPosP1, Plane, Deep);
			FillFacePlanes(*SeamRequest.VoxelData[q ^ 1], CS, P1, bPosP1, Depth, *Plane, *Deep);
			SelectFaceArrays(R, P2, bPosP2, Plane, Deep);
			FillFacePlanes(*SeamRequest.VoxelData[q ^ 2], CS, P2, bPosP2, Depth, *Plane, *Deep);
			SelectEdgeArrays(R, AxisA, bPosA, AxisB, bPosB, Strip, Deep, Flag);
			FillEdgeStrip(*SeamRequest.VoxelData[q ^ 3], CS, AxisA, bPosA, AxisB, bPosB, EdgeAxis, Depth, *Strip, *Deep);
		}
		R.EdgeCornerFlags |= Flag;
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
			R.MorphWidthOverride = static_cast<float>(S) / static_cast<float>(MaxCoarser);
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

	const bool bInPlace = CVarMCSeamReadInPlace.GetValueOnAnyThread() != 0;
	VoxelMCSeamStats::FJobScope JobScope(2, bInPlace, OutMeshData);
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
		InitFrameRequest(R, Coord, LOD, CS, VoxelSize, SeamRequest.WorldOrigin, SeamRequest.LocalUp, *SeamRequest.VoxelData[o], bInPlace);
		R.NeighborPlaneDepth = Depth;
		Lap.Take(VoxelMCSeamStats::Setup);

		// Toward each sibling: +1 where this participant sits at 0 on that axis.
		const int32 Dir[3] = { bPos[0] ? 1 : -1, bPos[1] ? 1 : -1, bPos[2] ? 1 : -1 };
		TArray<FVoxelData>* Plane; TArray<FVoxelData>* Deep; uint32 Flag;
		uint8 Mask = 0;
		int32 MaxCoarserStride = 0;
		for (int32 A = 0; A < 3; ++A)
		{
			if (bInPlace)
			{
				SetNeighbourView(R, A == 0 ? Dir[0] : 0, A == 1 ? Dir[1] : 0, A == 2 ? Dir[2] : 0, *SeamRequest.VoxelData[o ^ (1 << A)]);
			}
			else
			{
				SelectFaceArrays(R, A, bPos[A], Plane, Deep);
				FillFacePlanes(*SeamRequest.VoxelData[o ^ (1 << A)], CS, A, bPos[A], Depth, *Plane, *Deep);
			}
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
			TArray<FVoxelData>* Strip;
			SelectEdgeArrays(R, AxisA, bPos[AxisA], AxisB, bPos[AxisB], Strip, Deep, Flag);
			if (bInPlace)
			{
				SetNeighbourView(R, FreeAxis == 0 ? 0 : Dir[0], FreeAxis == 1 ? 0 : Dir[1], FreeAxis == 2 ? 0 : Dir[2],
					*SeamRequest.VoxelData[Sibling]);
			}
			else
			{
				FillEdgeStrip(*SeamRequest.VoxelData[Sibling], CS, AxisA, bPos[AxisA], AxisB, bPos[AxisB],
					FreeAxis, Depth, *Strip, *Deep);
			}
			R.EdgeCornerFlags |= Flag;
		}
		// The diagonal corner voxel.
		{
			FVoxelData* Scalar;
			SelectCornerArrays(R, bPos[0], bPos[1], bPos[2], Scalar, Deep, Flag);
			if (bInPlace)
			{
				SetNeighbourView(R, Dir[0], Dir[1], Dir[2], *SeamRequest.VoxelData[o ^ 7]);
			}
			else
			{
				FillCorner(*SeamRequest.VoxelData[o ^ 7], CS, bPos[0], bPos[1], bPos[2], Depth, *Scalar, *Deep);
			}
			R.EdgeCornerFlags |= Flag;
		}
		Lap.Take(VoxelMCSeamStats::Fill);
		if (Mask)
		{
			R.TransitionFaces = Mask;
			R.MorphWidthOverride = static_cast<float>(S) / static_cast<float>(MaxCoarserStride);
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
