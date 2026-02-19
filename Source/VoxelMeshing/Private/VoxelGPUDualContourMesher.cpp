// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelGPUDualContourMesher.h"
#include "VoxelMeshing.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelVertex.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "RenderingThread.h"
#include "DataDrivenShaderPlatformInfo.h"

// ==================== GPU DC Intermediate Structures ====================
// Must match HLSL struct layout in DualContourMeshGeneration.usf

struct FDCEdgeCrossingGPU
{
	FVector3f Position;     // 12 bytes
	FVector3f Normal;       // 12 bytes
	uint32 PackedMaterial;  // 4 bytes
	uint32 Flags;           // 4 bytes
	// Total: 32 bytes
};

struct FDCCellVertexGPU
{
	FVector3f Position;     // 12 bytes
	uint32 PackedNormal;    // 4 bytes
	uint32 PackedMaterial;  // 4 bytes
	uint32 Flags;           // 4 bytes
	uint32 VertexIndex;     // 4 bytes
	uint32 _Pad;            // 4 bytes
	// Total: 32 bytes
};

// ==================== Compute Shader Declarations ====================

/**
 * Pass 0: Reset atomic counters.
 */
class FDCResetCountersCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FDCResetCountersCS);
	SHADER_USE_PARAMETER_STRUCT(FDCResetCountersCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, MeshCounters)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/**
 * Pass 1: Edge crossing detection.
 * 3D dispatch over grid cells, checks 3 edges per cell for density sign changes.
 */
class FDCEdgeCrossingCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FDCEdgeCrossingCS);
	SHADER_USE_PARAMETER_STRUCT(FDCEdgeCrossingCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InputVoxelData)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborXPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborXNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborYPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborYNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborZPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborZNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXPosYPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXPosYNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXNegYPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXNegYNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXPosZPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXPosZNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXNegZPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXNegZNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeYPosZPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeYPosZNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeYNegZPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeYNegZNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CornerData)
		SHADER_PARAMETER(uint32, NeighborFlags)
		SHADER_PARAMETER(uint32, EdgeCornerFlags)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FDCEdgeCrossingGPU>, DCEdgeCrossings)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, DCValidEdgeIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, MeshCounters)
		SHADER_PARAMETER(uint32, ChunkSize)
		SHADER_PARAMETER(float, VoxelSize)
		SHADER_PARAMETER(FVector3f, ChunkWorldPosition)
		SHADER_PARAMETER(float, IsoLevel)
		SHADER_PARAMETER(uint32, LODStride)
		SHADER_PARAMETER(uint32, GridDim)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), 8);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), 8);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Z"), 4);
	}
};

/**
 * Pass 2: QEF vertex solve per cell.
 * 3D dispatch, collects hermite data from 12 edges per cell, solves 3x3 SVD.
 */
class FDCQEFSolveCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FDCQEFSolveCS);
	SHADER_USE_PARAMETER_STRUCT(FDCQEFSolveCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FDCEdgeCrossingGPU>, DCEdgeCrossings)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FDCCellVertexGPU>, DCCellVertices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVoxelVertex>, OutputVertices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, MeshCounters)
		SHADER_PARAMETER(uint32, ChunkSize)
		SHADER_PARAMETER(float, VoxelSize)
		SHADER_PARAMETER(float, IsoLevel)
		SHADER_PARAMETER(uint32, LODStride)
		SHADER_PARAMETER(uint32, GridDim)
		SHADER_PARAMETER(float, QEFThreshold)
		SHADER_PARAMETER(float, QEFBias)
		SHADER_PARAMETER(uint32, MaxVertexCount)
		// Voxel access for material/biome lookup
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InputVoxelData)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborXPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborXNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborYPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborYNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborZPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborZNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXPosYPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXPosYNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXNegYPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXNegYNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXPosZPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXPosZNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXNegZPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXNegZNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeYPosZPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeYPosZNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeYNegZPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeYNegZNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CornerData)
		SHADER_PARAMETER(uint32, NeighborFlags)
		SHADER_PARAMETER(uint32, EdgeCornerFlags)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), 8);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), 8);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Z"), 4);
	}
};

/**
 * Pass 3: Quad generation from edge crossings.
 * 1D dispatch over valid edges, emits quads from 4 cells sharing each edge.
 */
class FDCQuadGenerationCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FDCQuadGenerationCS);
	SHADER_USE_PARAMETER_STRUCT(FDCQuadGenerationCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, DCValidEdgeIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FDCCellVertexGPU>, DCCellVertices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutputIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, MeshCounters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, LODMergeMap)
		SHADER_PARAMETER(uint32, LODMergeMapCount)
		SHADER_PARAMETER(uint32, ChunkSize)
		SHADER_PARAMETER(float, VoxelSize)
		SHADER_PARAMETER(float, IsoLevel)
		SHADER_PARAMETER(uint32, LODStride)
		SHADER_PARAMETER(uint32, GridDim)
		SHADER_PARAMETER(uint32, MaxIndexCount)
		// Voxel access for winding determination
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InputVoxelData)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborXPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborXNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborYPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborYNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborZPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborZNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXPosYPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXPosYNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXNegYPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXNegYNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXPosZPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXPosZNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXNegZPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeXNegZNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeYPosZPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeYPosZNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeYNegZPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EdgeYNegZNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CornerData)
		SHADER_PARAMETER(uint32, NeighborFlags)
		SHADER_PARAMETER(uint32, EdgeCornerFlags)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDCResetCountersCS, "/Plugin/VoxelWorlds/Private/DualContourMeshGeneration.usf", "DCResetCountersCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FDCEdgeCrossingCS, "/Plugin/VoxelWorlds/Private/DualContourMeshGeneration.usf", "DCEdgeCrossingCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FDCQEFSolveCS, "/Plugin/VoxelWorlds/Private/DualContourMeshGeneration.usf", "DCQEFSolveCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FDCQuadGenerationCS, "/Plugin/VoxelWorlds/Private/DualContourMeshGeneration.usf", "DCQuadGenerationCS", SF_Compute);

// ==================== FVoxelGPUDualContourMesher Implementation ====================

FVoxelGPUDualContourMesher::FVoxelGPUDualContourMesher()
{
}

FVoxelGPUDualContourMesher::~FVoxelGPUDualContourMesher()
{
	if (bIsInitialized)
	{
		Shutdown();
	}
}

void FVoxelGPUDualContourMesher::Initialize()
{
	if (bIsInitialized)
	{
		return;
	}

	bIsInitialized = true;
	UE_LOG(LogVoxelMeshing, Log, TEXT("GPU Dual Contouring Mesher initialized"));
}

void FVoxelGPUDualContourMesher::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	FlushRenderingCommands();
	ReleaseAllHandles();
	bIsInitialized = false;

	UE_LOG(LogVoxelMeshing, Log, TEXT("GPU Dual Contouring Mesher shutdown"));
}

bool FVoxelGPUDualContourMesher::GenerateMeshCPU(
	const FVoxelMeshingRequest& Request,
	FChunkMeshData& OutMeshData)
{
	// Fallback to CPU DC mesher
	FVoxelCPUDualContourMesher CPUMesher;
	CPUMesher.Initialize();
	CPUMesher.SetConfig(Config);
	bool bResult = CPUMesher.GenerateMeshCPU(Request, OutMeshData);
	CPUMesher.Shutdown();
	return bResult;
}

bool FVoxelGPUDualContourMesher::GenerateMeshCPU(
	const FVoxelMeshingRequest& Request,
	FChunkMeshData& OutMeshData,
	FVoxelMeshingStats& OutStats)
{
	FVoxelCPUDualContourMesher CPUMesher;
	CPUMesher.Initialize();
	CPUMesher.SetConfig(Config);
	bool bResult = CPUMesher.GenerateMeshCPU(Request, OutMeshData, OutStats);
	CPUMesher.Shutdown();
	return bResult;
}

TArray<uint32> FVoxelGPUDualContourMesher::PackVoxelDataForGPU(const TArray<FVoxelData>& VoxelData)
{
	TArray<uint32> PackedData;
	PackedData.SetNum(VoxelData.Num());

	for (int32 i = 0; i < VoxelData.Num(); ++i)
	{
		PackedData[i] = VoxelData[i].Pack();
	}

	return PackedData;
}

TArray<uint32> FVoxelGPUDualContourMesher::BuildLODMergeMap(
	const FVoxelMeshingRequest& Request,
	int32 GridDim,
	int32 Stride)
{
	TArray<uint32> MergeMap;
	const int32 ChunkSize = Request.ChunkSize;
	const int32 GridSize = ChunkSize / Stride;

	auto CellIdx = [GridDim](int32 CX, int32 CY, int32 CZ) -> uint32
	{
		return static_cast<uint32>((CX + 1) + (CY + 1) * GridDim + (CZ + 1) * GridDim * GridDim);
	};

	for (int32 Face = 0; Face < 6; Face++)
	{
		const int32 NeighborLOD = Request.NeighborLODLevels[Face];
		if (NeighborLOD <= Request.LODLevel)
		{
			continue;
		}

		const int32 CoarserStride = 1 << NeighborLOD;
		const int32 MergeRatio = CoarserStride / Stride;
		if (MergeRatio <= 1)
		{
			continue;
		}

		const int32 DepthAxis = Face / 2;
		const bool bPositiveFace = (Face % 2 == 1);
		const int32 BoundaryCell = bPositiveFace ? (GridSize - 1) : 0;

		for (int32 A2 = 0; A2 < GridSize; A2 += MergeRatio)
		{
			for (int32 A1 = 0; A1 < GridSize; A1 += MergeRatio)
			{
				// Compute base cell
				int32 BaseCX, BaseCY, BaseCZ;
				switch (DepthAxis)
				{
				case 0: BaseCX = BoundaryCell; BaseCY = A1; BaseCZ = A2; break;
				case 1: BaseCX = A1; BaseCY = BoundaryCell; BaseCZ = A2; break;
				default: BaseCX = A1; BaseCY = A2; BaseCZ = BoundaryCell; break;
				}

				const uint32 BaseIdx = CellIdx(BaseCX, BaseCY, BaseCZ);

				// Map all fine cells in this group to the base cell
				for (int32 DA2 = 0; DA2 < MergeRatio && (A2 + DA2) < GridSize; DA2++)
				{
					for (int32 DA1 = 0; DA1 < MergeRatio && (A1 + DA1) < GridSize; DA1++)
					{
						if (DA1 == 0 && DA2 == 0)
						{
							continue; // Skip base cell itself
						}

						int32 AliasCX, AliasCY, AliasCZ;
						switch (DepthAxis)
						{
						case 0: AliasCX = BoundaryCell; AliasCY = A1 + DA1; AliasCZ = A2 + DA2; break;
						case 1: AliasCX = A1 + DA1; AliasCY = BoundaryCell; AliasCZ = A2 + DA2; break;
						default: AliasCX = A1 + DA1; AliasCY = A2 + DA2; AliasCZ = BoundaryCell; break;
						}

						MergeMap.Add(CellIdx(AliasCX, AliasCY, AliasCZ));
						MergeMap.Add(BaseIdx);
					}
				}
			}
		}
	}

	return MergeMap;
}

FVoxelMeshingHandle FVoxelGPUDualContourMesher::GenerateMeshAsync(
	const FVoxelMeshingRequest& Request,
	FOnVoxelMeshingComplete OnComplete)
{
	if (!bIsInitialized)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("GPU DC Mesher not initialized"));
		return FVoxelMeshingHandle();
	}

	if (!Request.IsValid())
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("Invalid meshing request"));
		return FVoxelMeshingHandle();
	}

	uint64 RequestId = NextRequestId++;
	FVoxelMeshingHandle Handle(RequestId, Request.ChunkCoord);

	TSharedPtr<FMeshingResult> Result = MakeShared<FMeshingResult>();
	Result->ChunkCoord = Request.ChunkCoord;
	Result->ChunkSize = Request.ChunkSize;
	Result->Stats.GenerationTimeMs = 0.0f;

	{
		FScopeLock Lock(&ResultsLock);
		MeshingResults.Add(RequestId, Result);
	}

	DispatchComputeShader(Request, RequestId, Result, OnComplete);

	return Handle;
}

void FVoxelGPUDualContourMesher::DispatchComputeShader(
	const FVoxelMeshingRequest& Request,
	uint64 RequestId,
	TSharedPtr<FMeshingResult> Result,
	FOnVoxelMeshingComplete OnComplete)
{
	// Pack voxel data
	TArray<uint32> PackedVoxels = PackVoxelDataForGPU(Request.VoxelData);

	// Pack neighbor data
	TArray<uint32> PackedNeighborXPos, PackedNeighborXNeg;
	TArray<uint32> PackedNeighborYPos, PackedNeighborYNeg;
	TArray<uint32> PackedNeighborZPos, PackedNeighborZNeg;

	uint32 NeighborFlags = 0;
	const int32 SliceSize = Request.GetNeighborSliceSize();
	const int32 EdgeSize = Request.GetEdgeStripSize();

	if (Request.NeighborXPos.Num() == SliceSize) { PackedNeighborXPos = PackVoxelDataForGPU(Request.NeighborXPos); NeighborFlags |= (1 << 0); }
	if (Request.NeighborXNeg.Num() == SliceSize) { PackedNeighborXNeg = PackVoxelDataForGPU(Request.NeighborXNeg); NeighborFlags |= (1 << 1); }
	if (Request.NeighborYPos.Num() == SliceSize) { PackedNeighborYPos = PackVoxelDataForGPU(Request.NeighborYPos); NeighborFlags |= (1 << 2); }
	if (Request.NeighborYNeg.Num() == SliceSize) { PackedNeighborYNeg = PackVoxelDataForGPU(Request.NeighborYNeg); NeighborFlags |= (1 << 3); }
	if (Request.NeighborZPos.Num() == SliceSize) { PackedNeighborZPos = PackVoxelDataForGPU(Request.NeighborZPos); NeighborFlags |= (1 << 4); }
	if (Request.NeighborZNeg.Num() == SliceSize) { PackedNeighborZNeg = PackVoxelDataForGPU(Request.NeighborZNeg); NeighborFlags |= (1 << 5); }

	// Pack edge neighbor data
	TArray<uint32> PackedEdgeXPosYPos, PackedEdgeXPosYNeg, PackedEdgeXNegYPos, PackedEdgeXNegYNeg;
	TArray<uint32> PackedEdgeXPosZPos, PackedEdgeXPosZNeg, PackedEdgeXNegZPos, PackedEdgeXNegZNeg;
	TArray<uint32> PackedEdgeYPosZPos, PackedEdgeYPosZNeg, PackedEdgeYNegZPos, PackedEdgeYNegZNeg;

	if (Request.EdgeXPosYPos.Num() == EdgeSize) PackedEdgeXPosYPos = PackVoxelDataForGPU(Request.EdgeXPosYPos);
	if (Request.EdgeXPosYNeg.Num() == EdgeSize) PackedEdgeXPosYNeg = PackVoxelDataForGPU(Request.EdgeXPosYNeg);
	if (Request.EdgeXNegYPos.Num() == EdgeSize) PackedEdgeXNegYPos = PackVoxelDataForGPU(Request.EdgeXNegYPos);
	if (Request.EdgeXNegYNeg.Num() == EdgeSize) PackedEdgeXNegYNeg = PackVoxelDataForGPU(Request.EdgeXNegYNeg);
	if (Request.EdgeXPosZPos.Num() == EdgeSize) PackedEdgeXPosZPos = PackVoxelDataForGPU(Request.EdgeXPosZPos);
	if (Request.EdgeXPosZNeg.Num() == EdgeSize) PackedEdgeXPosZNeg = PackVoxelDataForGPU(Request.EdgeXPosZNeg);
	if (Request.EdgeXNegZPos.Num() == EdgeSize) PackedEdgeXNegZPos = PackVoxelDataForGPU(Request.EdgeXNegZPos);
	if (Request.EdgeXNegZNeg.Num() == EdgeSize) PackedEdgeXNegZNeg = PackVoxelDataForGPU(Request.EdgeXNegZNeg);
	if (Request.EdgeYPosZPos.Num() == EdgeSize) PackedEdgeYPosZPos = PackVoxelDataForGPU(Request.EdgeYPosZPos);
	if (Request.EdgeYPosZNeg.Num() == EdgeSize) PackedEdgeYPosZNeg = PackVoxelDataForGPU(Request.EdgeYPosZNeg);
	if (Request.EdgeYNegZPos.Num() == EdgeSize) PackedEdgeYNegZPos = PackVoxelDataForGPU(Request.EdgeYNegZPos);
	if (Request.EdgeYNegZNeg.Num() == EdgeSize) PackedEdgeYNegZNeg = PackVoxelDataForGPU(Request.EdgeYNegZNeg);

	// Pack corner data
	TArray<uint32> PackedCornerData;
	PackedCornerData.SetNum(8);
	PackedCornerData[0] = Request.CornerXPosYPosZPos.Pack();
	PackedCornerData[1] = Request.CornerXPosYPosZNeg.Pack();
	PackedCornerData[2] = Request.CornerXPosYNegZPos.Pack();
	PackedCornerData[3] = Request.CornerXPosYNegZNeg.Pack();
	PackedCornerData[4] = Request.CornerXNegYPosZPos.Pack();
	PackedCornerData[5] = Request.CornerXNegYPosZNeg.Pack();
	PackedCornerData[6] = Request.CornerXNegYNegZPos.Pack();
	PackedCornerData[7] = Request.CornerXNegYNegZNeg.Pack();

	uint32 EdgeCornerFlags = Request.EdgeCornerFlags;

	// LOD parameters
	const int32 ChunkSize = Request.ChunkSize;
	const float VoxelSize = Request.VoxelSize;
	const FVector3f ChunkWorldPos = FVector3f(Request.GetChunkWorldPosition());
	const FVoxelMeshingConfig CapturedConfig = Config;
	const FIntVector ChunkCoord = Request.ChunkCoord;
	const int32 LODLevel = FMath::Clamp(Request.LODLevel, 0, 7);
	const uint32 LODStride = 1u << LODLevel;
	const int32 LODChunkSize = ChunkSize / static_cast<int32>(LODStride);
	const int32 GridDim = LODChunkSize + 3;
	const int32 TotalCells = GridDim * GridDim * GridDim;

	// Build LOD merge map (CPU pre-pass)
	TArray<uint32> MergeMap = BuildLODMergeMap(Request, GridDim, static_cast<int32>(LODStride));
	const uint32 MergeMapPairCount = MergeMap.Num() / 2;

	ENQUEUE_RENDER_COMMAND(GenerateDCMesh)(
		[PackedVoxels = MoveTemp(PackedVoxels),
		 PackedNeighborXPos = MoveTemp(PackedNeighborXPos),
		 PackedNeighborXNeg = MoveTemp(PackedNeighborXNeg),
		 PackedNeighborYPos = MoveTemp(PackedNeighborYPos),
		 PackedNeighborYNeg = MoveTemp(PackedNeighborYNeg),
		 PackedNeighborZPos = MoveTemp(PackedNeighborZPos),
		 PackedNeighborZNeg = MoveTemp(PackedNeighborZNeg),
		 PackedEdgeXPosYPos = MoveTemp(PackedEdgeXPosYPos),
		 PackedEdgeXPosYNeg = MoveTemp(PackedEdgeXPosYNeg),
		 PackedEdgeXNegYPos = MoveTemp(PackedEdgeXNegYPos),
		 PackedEdgeXNegYNeg = MoveTemp(PackedEdgeXNegYNeg),
		 PackedEdgeXPosZPos = MoveTemp(PackedEdgeXPosZPos),
		 PackedEdgeXPosZNeg = MoveTemp(PackedEdgeXPosZNeg),
		 PackedEdgeXNegZPos = MoveTemp(PackedEdgeXNegZPos),
		 PackedEdgeXNegZNeg = MoveTemp(PackedEdgeXNegZNeg),
		 PackedEdgeYPosZPos = MoveTemp(PackedEdgeYPosZPos),
		 PackedEdgeYPosZNeg = MoveTemp(PackedEdgeYPosZNeg),
		 PackedEdgeYNegZPos = MoveTemp(PackedEdgeYNegZPos),
		 PackedEdgeYNegZNeg = MoveTemp(PackedEdgeYNegZNeg),
		 PackedCornerData = MoveTemp(PackedCornerData),
		 MergeMap = MoveTemp(MergeMap),
		 NeighborFlags,
		 EdgeCornerFlags,
		 ChunkSize,
		 VoxelSize,
		 ChunkWorldPos,
		 CapturedConfig,
		 ChunkCoord,
		 LODStride,
		 GridDim,
		 TotalCells,
		 MergeMapPairCount,
		 RequestId,
		 Result,
		 OnComplete](FRHICommandListImmediate& RHICmdList)
		{
			const double StartTime = FPlatformTime::Seconds();
			const int32 TotalVoxels = ChunkSize * ChunkSize * ChunkSize;
			const int32 MaxEdges = TotalCells * 3;

			FRDGBuilder GraphBuilder(RHICmdList);

			// ===== Create input voxel buffer =====
			FRDGBufferDesc VoxelBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), TotalVoxels);
			FRDGBufferRef VoxelBuffer = GraphBuilder.CreateBuffer(VoxelBufferDesc, TEXT("InputVoxelData"));
			GraphBuilder.QueueBufferUpload(VoxelBuffer, PackedVoxels.GetData(), PackedVoxels.Num() * sizeof(uint32));

			// ===== Create neighbor buffers =====
			static const uint32 DummyData = 0;
			auto CreateNeighborBuffer = [&](const TArray<uint32>& Data, const TCHAR* Name) -> FRDGBufferRef
			{
				if (Data.Num() > 0)
				{
					FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Data.Num());
					FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(Desc, Name);
					GraphBuilder.QueueBufferUpload(Buffer, Data.GetData(), Data.Num() * sizeof(uint32));
					return Buffer;
				}
				else
				{
					FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
					FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(Desc, Name);
					GraphBuilder.QueueBufferUpload(Buffer, &DummyData, sizeof(uint32));
					return Buffer;
				}
			};

			FRDGBufferRef NeighborXPosBuffer = CreateNeighborBuffer(PackedNeighborXPos, TEXT("NeighborXPos"));
			FRDGBufferRef NeighborXNegBuffer = CreateNeighborBuffer(PackedNeighborXNeg, TEXT("NeighborXNeg"));
			FRDGBufferRef NeighborYPosBuffer = CreateNeighborBuffer(PackedNeighborYPos, TEXT("NeighborYPos"));
			FRDGBufferRef NeighborYNegBuffer = CreateNeighborBuffer(PackedNeighborYNeg, TEXT("NeighborYNeg"));
			FRDGBufferRef NeighborZPosBuffer = CreateNeighborBuffer(PackedNeighborZPos, TEXT("NeighborZPos"));
			FRDGBufferRef NeighborZNegBuffer = CreateNeighborBuffer(PackedNeighborZNeg, TEXT("NeighborZNeg"));

			FRDGBufferRef EdgeXPosYPosBuffer = CreateNeighborBuffer(PackedEdgeXPosYPos, TEXT("EdgeXPosYPos"));
			FRDGBufferRef EdgeXPosYNegBuffer = CreateNeighborBuffer(PackedEdgeXPosYNeg, TEXT("EdgeXPosYNeg"));
			FRDGBufferRef EdgeXNegYPosBuffer = CreateNeighborBuffer(PackedEdgeXNegYPos, TEXT("EdgeXNegYPos"));
			FRDGBufferRef EdgeXNegYNegBuffer = CreateNeighborBuffer(PackedEdgeXNegYNeg, TEXT("EdgeXNegYNeg"));
			FRDGBufferRef EdgeXPosZPosBuffer = CreateNeighborBuffer(PackedEdgeXPosZPos, TEXT("EdgeXPosZPos"));
			FRDGBufferRef EdgeXPosZNegBuffer = CreateNeighborBuffer(PackedEdgeXPosZNeg, TEXT("EdgeXPosZNeg"));
			FRDGBufferRef EdgeXNegZPosBuffer = CreateNeighborBuffer(PackedEdgeXNegZPos, TEXT("EdgeXNegZPos"));
			FRDGBufferRef EdgeXNegZNegBuffer = CreateNeighborBuffer(PackedEdgeXNegZNeg, TEXT("EdgeXNegZNeg"));
			FRDGBufferRef EdgeYPosZPosBuffer = CreateNeighborBuffer(PackedEdgeYPosZPos, TEXT("EdgeYPosZPos"));
			FRDGBufferRef EdgeYPosZNegBuffer = CreateNeighborBuffer(PackedEdgeYPosZNeg, TEXT("EdgeYPosZNeg"));
			FRDGBufferRef EdgeYNegZPosBuffer = CreateNeighborBuffer(PackedEdgeYNegZPos, TEXT("EdgeYNegZPos"));
			FRDGBufferRef EdgeYNegZNegBuffer = CreateNeighborBuffer(PackedEdgeYNegZNeg, TEXT("EdgeYNegZNeg"));

			// Corner data
			FRDGBufferDesc CornerBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 8);
			FRDGBufferRef CornerDataBuffer = GraphBuilder.CreateBuffer(CornerBufferDesc, TEXT("CornerData"));
			GraphBuilder.QueueBufferUpload(CornerDataBuffer, PackedCornerData.GetData(), PackedCornerData.Num() * sizeof(uint32));

			// ===== Create intermediate DC buffers =====

			// Edge crossings: TotalCells * 3 entries
			FRDGBufferDesc EdgeCrossingDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FDCEdgeCrossingGPU), MaxEdges);
			FRDGBufferRef EdgeCrossingBuffer = GraphBuilder.CreateBuffer(EdgeCrossingDesc, TEXT("DCEdgeCrossings"));

			// Valid edge indices (worst case = all edges)
			FRDGBufferDesc ValidEdgeDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), MaxEdges);
			FRDGBufferRef ValidEdgeBuffer = GraphBuilder.CreateBuffer(ValidEdgeDesc, TEXT("DCValidEdgeIndices"));

			// Cell vertices: TotalCells entries
			FRDGBufferDesc CellVertexDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FDCCellVertexGPU), TotalCells);
			FRDGBufferRef CellVertexBuffer = GraphBuilder.CreateBuffer(CellVertexDesc, TEXT("DCCellVertices"));

			// LOD merge map
			const int32 MergeMapElements = FMath::Max(MergeMap.Num(), 2); // At least 2 to avoid empty buffer
			FRDGBufferDesc MergeMapDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), MergeMapElements);
			FRDGBufferRef MergeMapBuffer = GraphBuilder.CreateBuffer(MergeMapDesc, TEXT("LODMergeMap"));
			if (MergeMap.Num() > 0)
			{
				GraphBuilder.QueueBufferUpload(MergeMapBuffer, MergeMap.GetData(), MergeMap.Num() * sizeof(uint32));
			}
			else
			{
				static const uint32 DummyMerge[2] = {0, 0};
				GraphBuilder.QueueBufferUpload(MergeMapBuffer, DummyMerge, sizeof(DummyMerge));
			}

			// ===== Create output buffers =====
			FRDGBufferDesc VertexBufferDesc = FRDGBufferDesc::CreateStructuredDesc(
				sizeof(FVoxelVertex),
				CapturedConfig.MaxVerticesPerChunk
			);
			FRDGBufferRef VertexBuffer = GraphBuilder.CreateBuffer(VertexBufferDesc, TEXT("OutputVertices"));

			FRDGBufferDesc IndexBufferDesc = FRDGBufferDesc::CreateStructuredDesc(
				sizeof(uint32),
				CapturedConfig.MaxIndicesPerChunk
			);
			FRDGBufferRef IndexBuffer = GraphBuilder.CreateBuffer(IndexBufferDesc, TEXT("OutputIndices"));

			// Counters: [0]=VertexCount, [1]=IndexCount, [2]=ValidEdgeCount
			FRDGBufferDesc CounterBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 3);
			FRDGBufferRef MeshCountersBuffer = GraphBuilder.CreateBuffer(CounterBufferDesc, TEXT("MeshCounters"));

			// ===== Zero-initialize intermediate buffers =====
			// Matches CPU mesher behavior where TArray::SetNum zero-fills all entries.
			// Ensures unwritten edges/cells have Flags=0 (invalid), preventing Pass 2/3
			// from treating uninitialized data as valid crossings/vertices.
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(EdgeCrossingBuffer), 0u);
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(CellVertexBuffer), 0u);

			// Create SRVs/UAVs that are shared across passes
			auto VoxelSRV = GraphBuilder.CreateSRV(VoxelBuffer);
			auto NeighborXPosSRV = GraphBuilder.CreateSRV(NeighborXPosBuffer);
			auto NeighborXNegSRV = GraphBuilder.CreateSRV(NeighborXNegBuffer);
			auto NeighborYPosSRV = GraphBuilder.CreateSRV(NeighborYPosBuffer);
			auto NeighborYNegSRV = GraphBuilder.CreateSRV(NeighborYNegBuffer);
			auto NeighborZPosSRV = GraphBuilder.CreateSRV(NeighborZPosBuffer);
			auto NeighborZNegSRV = GraphBuilder.CreateSRV(NeighborZNegBuffer);
			auto EdgeXPosYPosSRV = GraphBuilder.CreateSRV(EdgeXPosYPosBuffer);
			auto EdgeXPosYNegSRV = GraphBuilder.CreateSRV(EdgeXPosYNegBuffer);
			auto EdgeXNegYPosSRV = GraphBuilder.CreateSRV(EdgeXNegYPosBuffer);
			auto EdgeXNegYNegSRV = GraphBuilder.CreateSRV(EdgeXNegYNegBuffer);
			auto EdgeXPosZPosSRV = GraphBuilder.CreateSRV(EdgeXPosZPosBuffer);
			auto EdgeXPosZNegSRV = GraphBuilder.CreateSRV(EdgeXPosZNegBuffer);
			auto EdgeXNegZPosSRV = GraphBuilder.CreateSRV(EdgeXNegZPosBuffer);
			auto EdgeXNegZNegSRV = GraphBuilder.CreateSRV(EdgeXNegZNegBuffer);
			auto EdgeYPosZPosSRV = GraphBuilder.CreateSRV(EdgeYPosZPosBuffer);
			auto EdgeYPosZNegSRV = GraphBuilder.CreateSRV(EdgeYPosZNegBuffer);
			auto EdgeYNegZPosSRV = GraphBuilder.CreateSRV(EdgeYNegZPosBuffer);
			auto EdgeYNegZNegSRV = GraphBuilder.CreateSRV(EdgeYNegZNegBuffer);
			auto CornerSRV = GraphBuilder.CreateSRV(CornerDataBuffer);
			auto MeshCountersUAV = GraphBuilder.CreateUAV(MeshCountersBuffer);

			// Helper lambda to bind voxel access parameters
			auto BindVoxelAccess = [&](auto* Params)
			{
				Params->InputVoxelData = VoxelSRV;
				Params->NeighborXPos = NeighborXPosSRV;
				Params->NeighborXNeg = NeighborXNegSRV;
				Params->NeighborYPos = NeighborYPosSRV;
				Params->NeighborYNeg = NeighborYNegSRV;
				Params->NeighborZPos = NeighborZPosSRV;
				Params->NeighborZNeg = NeighborZNegSRV;
				Params->EdgeXPosYPos = EdgeXPosYPosSRV;
				Params->EdgeXPosYNeg = EdgeXPosYNegSRV;
				Params->EdgeXNegYPos = EdgeXNegYPosSRV;
				Params->EdgeXNegYNeg = EdgeXNegYNegSRV;
				Params->EdgeXPosZPos = EdgeXPosZPosSRV;
				Params->EdgeXPosZNeg = EdgeXPosZNegSRV;
				Params->EdgeXNegZPos = EdgeXNegZPosSRV;
				Params->EdgeXNegZNeg = EdgeXNegZNegSRV;
				Params->EdgeYPosZPos = EdgeYPosZPosSRV;
				Params->EdgeYPosZNeg = EdgeYPosZNegSRV;
				Params->EdgeYNegZPos = EdgeYNegZPosSRV;
				Params->EdgeYNegZNeg = EdgeYNegZNegSRV;
				Params->CornerData = CornerSRV;
				Params->NeighborFlags = NeighborFlags;
				Params->EdgeCornerFlags = EdgeCornerFlags;
			};

			// ===== Pass 0: Reset Counters =====
			{
				TShaderMapRef<FDCResetCountersCS> ResetShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FDCResetCountersCS::FParameters* ResetParams = GraphBuilder.AllocParameters<FDCResetCountersCS::FParameters>();
				ResetParams->MeshCounters = MeshCountersUAV;

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("DCResetCounters"),
					ResetShader,
					ResetParams,
					FIntVector(1, 1, 1)
				);
			}

			// ===== Pass 1: Edge Crossing Detection =====
			{
				TShaderMapRef<FDCEdgeCrossingCS> EdgeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FDCEdgeCrossingCS::FParameters* EdgeParams = GraphBuilder.AllocParameters<FDCEdgeCrossingCS::FParameters>();

				BindVoxelAccess(EdgeParams);
				EdgeParams->DCEdgeCrossings = GraphBuilder.CreateUAV(EdgeCrossingBuffer);
				EdgeParams->DCValidEdgeIndices = GraphBuilder.CreateUAV(ValidEdgeBuffer);
				EdgeParams->MeshCounters = MeshCountersUAV;
				EdgeParams->ChunkSize = ChunkSize;
				EdgeParams->VoxelSize = VoxelSize;
				EdgeParams->ChunkWorldPosition = ChunkWorldPos;
				EdgeParams->IsoLevel = CapturedConfig.IsoLevel;
				EdgeParams->LODStride = LODStride;
				EdgeParams->GridDim = GridDim;

				// Dispatch covers [-1, GridSize] in each axis → GridDim threads per axis
				// Thread groups: [8,8,4]
				FIntVector GroupCount(
					FMath::DivideAndRoundUp(GridDim, 8),
					FMath::DivideAndRoundUp(GridDim, 8),
					FMath::DivideAndRoundUp(GridDim, 4)
				);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("DCEdgeCrossing"),
					EdgeShader,
					EdgeParams,
					GroupCount
				);
			}

			// ===== Pass 2: QEF Vertex Solve =====
			{
				TShaderMapRef<FDCQEFSolveCS> QEFShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FDCQEFSolveCS::FParameters* QEFParams = GraphBuilder.AllocParameters<FDCQEFSolveCS::FParameters>();

				QEFParams->DCEdgeCrossings = GraphBuilder.CreateUAV(EdgeCrossingBuffer);
				QEFParams->DCCellVertices = GraphBuilder.CreateUAV(CellVertexBuffer);
				QEFParams->OutputVertices = GraphBuilder.CreateUAV(VertexBuffer);
				QEFParams->MeshCounters = MeshCountersUAV;
				QEFParams->ChunkSize = ChunkSize;
				QEFParams->VoxelSize = VoxelSize;
				QEFParams->IsoLevel = CapturedConfig.IsoLevel;
				QEFParams->LODStride = LODStride;
				QEFParams->GridDim = GridDim;
				QEFParams->QEFThreshold = CapturedConfig.QEFSVDThreshold;
				QEFParams->QEFBias = CapturedConfig.QEFBiasStrength;
				QEFParams->MaxVertexCount = CapturedConfig.MaxVerticesPerChunk;
				BindVoxelAccess(QEFParams);

				FIntVector GroupCount(
					FMath::DivideAndRoundUp(GridDim, 8),
					FMath::DivideAndRoundUp(GridDim, 8),
					FMath::DivideAndRoundUp(GridDim, 4)
				);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("DCQEFSolve"),
					QEFShader,
					QEFParams,
					GroupCount
				);
			}

			// ===== Pass 3: Quad Generation =====
			{
				TShaderMapRef<FDCQuadGenerationCS> QuadShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FDCQuadGenerationCS::FParameters* QuadParams = GraphBuilder.AllocParameters<FDCQuadGenerationCS::FParameters>();

				QuadParams->DCValidEdgeIndices = GraphBuilder.CreateUAV(ValidEdgeBuffer);
				QuadParams->DCCellVertices = GraphBuilder.CreateUAV(CellVertexBuffer);
				QuadParams->OutputIndices = GraphBuilder.CreateUAV(IndexBuffer);
				QuadParams->MeshCounters = MeshCountersUAV;
				QuadParams->LODMergeMap = GraphBuilder.CreateSRV(MergeMapBuffer);
				QuadParams->LODMergeMapCount = MergeMapPairCount;
				QuadParams->ChunkSize = ChunkSize;
				QuadParams->VoxelSize = VoxelSize;
				QuadParams->IsoLevel = CapturedConfig.IsoLevel;
				QuadParams->LODStride = LODStride;
				QuadParams->GridDim = GridDim;
				QuadParams->MaxIndexCount = CapturedConfig.MaxIndicesPerChunk;
				BindVoxelAccess(QuadParams);

				// 1D dispatch: one thread per possible valid edge
				// MaxEdges is the worst case; actual count is in MeshCounters[2]
				const int32 QuadGroupCount = FMath::DivideAndRoundUp(MaxEdges, 64);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("DCQuadGeneration"),
					QuadShader,
					QuadParams,
					FIntVector(QuadGroupCount, 1, 1)
				);
			}

			// ===== Extract buffers for persistence =====
			GraphBuilder.QueueBufferExtraction(VertexBuffer, &Result->VertexBuffer);
			GraphBuilder.QueueBufferExtraction(IndexBuffer, &Result->IndexBuffer);
			GraphBuilder.QueueBufferExtraction(MeshCountersBuffer, &Result->CounterBuffer);

			GraphBuilder.Execute();

			const double EndTime = FPlatformTime::Seconds();
			Result->Stats.GenerationTimeMs = static_cast<float>((EndTime - StartTime) * 1000.0);
			Result->bIsComplete = true;
			Result->bWasSuccessful = true;

			FVoxelMeshingHandle Handle(RequestId, ChunkCoord);
			Handle.bIsComplete = true;
			Handle.bWasSuccessful = true;

			if (OnComplete.IsBound())
			{
				AsyncTask(ENamedThreads::GameThread, [OnComplete, Handle]()
				{
					OnComplete.Execute(Handle, true);
				});
			}
		}
	);
}

void FVoxelGPUDualContourMesher::ReadCounters(TSharedPtr<FMeshingResult> Result)
{
	if (!Result || Result->bCountsRead || !Result->CounterBuffer.IsValid())
	{
		return;
	}

	// Capture max buffer capacities — GPU atomic counters can over-count because
	// InterlockedAdd increments before the bounds check, so readback values may
	// exceed the actual buffer sizes. We must clamp to prevent copy overrun.
	const uint32 MaxVerts = Config.MaxVerticesPerChunk;
	const uint32 MaxIdxs = Config.MaxIndicesPerChunk;

	ENQUEUE_RENDER_COMMAND(ReadDCMeshCounters)(
		[Result, MaxVerts, MaxIdxs](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			FRDGBufferRef CounterBuffer = GraphBuilder.RegisterExternalBuffer(Result->CounterBuffer, TEXT("CounterBuffer"));

			FRDGBufferDesc StagingDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 3);
			FRDGBufferRef StagingBuffer = GraphBuilder.CreateBuffer(StagingDesc, TEXT("StagingCounters"));

			AddCopyBufferPass(GraphBuilder, StagingBuffer, CounterBuffer);
			GraphBuilder.QueueBufferExtraction(StagingBuffer, &Result->StagingCounterBuffer);

			GraphBuilder.Execute();

			if (Result->StagingCounterBuffer.IsValid())
			{
				FRHIBuffer* StagingRHI = Result->StagingCounterBuffer->GetRHI();
				void* MappedData = RHICmdList.LockBuffer(
					StagingRHI,
					0,
					3 * sizeof(uint32),
					RLM_ReadOnly
				);

				if (MappedData)
				{
					const uint32* Counts = static_cast<const uint32*>(MappedData);
					// Clamp to buffer capacity — atomic counters may exceed max due to
					// InterlockedAdd happening before the bounds check in the shader
					Result->VertexCount = FMath::Min(Counts[0], MaxVerts);
					Result->IndexCount = FMath::Min(Counts[1], MaxIdxs);
					Result->Stats.VertexCount = Result->VertexCount;
					Result->Stats.IndexCount = Result->IndexCount;
					Result->Stats.FaceCount = Result->IndexCount / 3;
					Result->bCountsRead = true;

					RHICmdList.UnlockBuffer(StagingRHI);
				}
			}
		}
	);

	FlushRenderingCommands();
}

bool FVoxelGPUDualContourMesher::IsComplete(const FVoxelMeshingHandle& Handle) const
{
	FScopeLock Lock(&ResultsLock);
	const TSharedPtr<FMeshingResult>* ResultPtr = MeshingResults.Find(Handle.RequestId);
	return ResultPtr && (*ResultPtr)->bIsComplete;
}

bool FVoxelGPUDualContourMesher::WasSuccessful(const FVoxelMeshingHandle& Handle) const
{
	FScopeLock Lock(&ResultsLock);
	const TSharedPtr<FMeshingResult>* ResultPtr = MeshingResults.Find(Handle.RequestId);
	return ResultPtr && (*ResultPtr)->bWasSuccessful;
}

FRHIBuffer* FVoxelGPUDualContourMesher::GetVertexBuffer(const FVoxelMeshingHandle& Handle)
{
	if (!Handle.IsValid())
	{
		return nullptr;
	}

	FScopeLock Lock(&ResultsLock);
	TSharedPtr<FMeshingResult>* ResultPtr = MeshingResults.Find(Handle.RequestId);
	if (ResultPtr && (*ResultPtr)->VertexBuffer.IsValid())
	{
		return (*ResultPtr)->VertexBuffer->GetRHI();
	}

	return nullptr;
}

FRHIBuffer* FVoxelGPUDualContourMesher::GetIndexBuffer(const FVoxelMeshingHandle& Handle)
{
	if (!Handle.IsValid())
	{
		return nullptr;
	}

	FScopeLock Lock(&ResultsLock);
	TSharedPtr<FMeshingResult>* ResultPtr = MeshingResults.Find(Handle.RequestId);
	if (ResultPtr && (*ResultPtr)->IndexBuffer.IsValid())
	{
		return (*ResultPtr)->IndexBuffer->GetRHI();
	}

	return nullptr;
}

bool FVoxelGPUDualContourMesher::GetBufferCounts(
	const FVoxelMeshingHandle& Handle,
	uint32& OutVertexCount,
	uint32& OutIndexCount) const
{
	if (!Handle.IsValid())
	{
		return false;
	}

	TSharedPtr<FMeshingResult> Result;
	{
		FScopeLock Lock(&ResultsLock);
		const TSharedPtr<FMeshingResult>* ResultPtr = MeshingResults.Find(Handle.RequestId);
		if (!ResultPtr || !(*ResultPtr)->bIsComplete)
		{
			return false;
		}
		Result = *ResultPtr;
	}

	if (!Result->bCountsRead)
	{
		const_cast<FVoxelGPUDualContourMesher*>(this)->ReadCounters(Result);
	}

	if (Result->bCountsRead)
	{
		OutVertexCount = Result->VertexCount;
		OutIndexCount = Result->IndexCount;
		return true;
	}

	return false;
}

bool FVoxelGPUDualContourMesher::GetRenderData(
	const FVoxelMeshingHandle& Handle,
	FChunkRenderData& OutRenderData)
{
	if (!Handle.IsValid())
	{
		return false;
	}

	TSharedPtr<FMeshingResult> Result;
	{
		FScopeLock Lock(&ResultsLock);
		TSharedPtr<FMeshingResult>* ResultPtr = MeshingResults.Find(Handle.RequestId);
		if (!ResultPtr || !(*ResultPtr)->bIsComplete)
		{
			return false;
		}
		Result = *ResultPtr;
	}

	if (!Result->bCountsRead)
	{
		ReadCounters(Result);
	}

	if (!Result->bCountsRead)
	{
		return false;
	}

	OutRenderData.ChunkCoord = Result->ChunkCoord;
	OutRenderData.VertexCount = Result->VertexCount;
	OutRenderData.IndexCount = Result->IndexCount;

	if (Result->VertexBuffer.IsValid())
	{
		OutRenderData.VertexBufferRHI = Result->VertexBuffer->GetRHI();
	}
	if (Result->IndexBuffer.IsValid())
	{
		OutRenderData.IndexBufferRHI = Result->IndexBuffer->GetRHI();
	}

	return true;
}

bool FVoxelGPUDualContourMesher::ReadbackToCPU(
	const FVoxelMeshingHandle& Handle,
	FChunkMeshData& OutMeshData)
{
	if (!Handle.IsValid())
	{
		return false;
	}

	TSharedPtr<FMeshingResult> Result;
	{
		FScopeLock Lock(&ResultsLock);
		TSharedPtr<FMeshingResult>* ResultPtr = MeshingResults.Find(Handle.RequestId);
		if (!ResultPtr || !(*ResultPtr)->bIsComplete)
		{
			return false;
		}
		Result = *ResultPtr;
	}

	if (!Result->bCountsRead)
	{
		ReadCounters(Result);
	}

	if (!Result->bCountsRead || Result->VertexCount == 0 || Result->IndexCount == 0)
	{
		OutMeshData.Reset();
		return true; // Empty mesh is valid
	}

	const uint32 VertexCount = Result->VertexCount;
	const uint32 IndexCount = Result->IndexCount;

	OutMeshData.Reset();
	OutMeshData.Positions.SetNum(VertexCount);
	OutMeshData.Normals.SetNum(VertexCount);
	OutMeshData.UVs.SetNum(VertexCount);
	OutMeshData.UV1s.SetNum(VertexCount);
	OutMeshData.Colors.SetNum(VertexCount);
	OutMeshData.Indices.SetNum(IndexCount);

	FChunkMeshData* OutDataPtr = &OutMeshData;

	ENQUEUE_RENDER_COMMAND(ReadbackDCMeshData)(
		[Result, VertexCount, IndexCount, OutDataPtr](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			FRDGBufferRef VertexBuffer = GraphBuilder.RegisterExternalBuffer(Result->VertexBuffer, TEXT("VertexBuffer"));
			FRDGBufferRef IndexBuffer = GraphBuilder.RegisterExternalBuffer(Result->IndexBuffer, TEXT("IndexBuffer"));

			FRDGBufferDesc VertexStagingDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVoxelVertex), VertexCount);
			FRDGBufferRef VertexStagingBuffer = GraphBuilder.CreateBuffer(VertexStagingDesc, TEXT("VertexStaging"));

			FRDGBufferDesc IndexStagingDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), IndexCount);
			FRDGBufferRef IndexStagingBuffer = GraphBuilder.CreateBuffer(IndexStagingDesc, TEXT("IndexStaging"));

			const uint64 VertexBytes = VertexCount * sizeof(FVoxelVertex);
			const uint64 IndexBytes = IndexCount * sizeof(uint32);
			AddCopyBufferPass(GraphBuilder, VertexStagingBuffer, 0, VertexBuffer, 0, VertexBytes);
			AddCopyBufferPass(GraphBuilder, IndexStagingBuffer, 0, IndexBuffer, 0, IndexBytes);

			GraphBuilder.QueueBufferExtraction(VertexStagingBuffer, &Result->StagingVertexBuffer);
			GraphBuilder.QueueBufferExtraction(IndexStagingBuffer, &Result->StagingIndexBuffer);

			GraphBuilder.Execute();

			// Read vertex data
			if (Result->StagingVertexBuffer.IsValid())
			{
				FRHIBuffer* StagingRHI = Result->StagingVertexBuffer->GetRHI();
				void* MappedData = RHICmdList.LockBuffer(
					StagingRHI,
					0,
					VertexCount * sizeof(FVoxelVertex),
					RLM_ReadOnly
				);

				if (MappedData)
				{
					const FVoxelVertex* Vertices = static_cast<const FVoxelVertex*>(MappedData);
					for (uint32 i = 0; i < VertexCount; ++i)
					{
						OutDataPtr->Positions[i] = Vertices[i].Position;
						const FVector3f Normal = Vertices[i].GetNormal();
						OutDataPtr->Normals[i] = Normal;
						OutDataPtr->UVs[i] = Vertices[i].UV;

						const uint8 MaterialID = Vertices[i].GetMaterialID();
						OutDataPtr->UV1s[i] = FVector2f(static_cast<float>(MaterialID), 0.0f);

						OutDataPtr->Colors[i] = FColor(
							MaterialID,
							Vertices[i].GetBiomeID(),
							Vertices[i].GetAO() * 85,
							255
						);
					}
					RHICmdList.UnlockBuffer(StagingRHI);
				}
			}

			// Read index data
			if (Result->StagingIndexBuffer.IsValid())
			{
				FRHIBuffer* StagingRHI = Result->StagingIndexBuffer->GetRHI();
				void* MappedData = RHICmdList.LockBuffer(
					StagingRHI,
					0,
					IndexCount * sizeof(uint32),
					RLM_ReadOnly
				);

				if (MappedData)
				{
					FMemory::Memcpy(OutDataPtr->Indices.GetData(), MappedData, IndexCount * sizeof(uint32));
					RHICmdList.UnlockBuffer(StagingRHI);
				}
			}
		}
	);

	FlushRenderingCommands();
	return true;
}

void FVoxelGPUDualContourMesher::ReleaseHandle(const FVoxelMeshingHandle& Handle)
{
	if (!Handle.IsValid())
	{
		return;
	}

	FlushRenderingCommands();

	FScopeLock Lock(&ResultsLock);
	MeshingResults.Remove(Handle.RequestId);
}

void FVoxelGPUDualContourMesher::ReleaseAllHandles()
{
	FlushRenderingCommands();

	FScopeLock Lock(&ResultsLock);
	MeshingResults.Empty();
}

void FVoxelGPUDualContourMesher::SetConfig(const FVoxelMeshingConfig& InConfig)
{
	Config = InConfig;
}

const FVoxelMeshingConfig& FVoxelGPUDualContourMesher::GetConfig() const
{
	return Config;
}

bool FVoxelGPUDualContourMesher::GetStats(
	const FVoxelMeshingHandle& Handle,
	FVoxelMeshingStats& OutStats) const
{
	if (!Handle.IsValid())
	{
		return false;
	}

	TSharedPtr<FMeshingResult> Result;
	{
		FScopeLock Lock(&ResultsLock);
		const TSharedPtr<FMeshingResult>* ResultPtr = MeshingResults.Find(Handle.RequestId);
		if (!ResultPtr)
		{
			return false;
		}
		Result = *ResultPtr;
	}

	if (!Result->bCountsRead && Result->bIsComplete)
	{
		const_cast<FVoxelGPUDualContourMesher*>(this)->ReadCounters(Result);
	}

	OutStats = Result->Stats;
	return true;
}
