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
#include "RHIGPUReadback.h"
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
 * Pass 2.5: Prepare indirect dispatch arguments for Pass 3.
 * Reads ValidEdgeCount from MeshCounters[2] and writes thread group counts.
 */
class FDCPrepareIndirectArgsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FDCPrepareIndirectArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FDCPrepareIndirectArgsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, MeshCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
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
IMPLEMENT_GLOBAL_SHADER(FDCPrepareIndirectArgsCS, "/Plugin/VoxelWorlds/Private/DualContourMeshGeneration.usf", "DCPrepareIndirectArgsCS", SF_Compute);
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

			// ===== Pass 3: Quad Generation (Fixed Dispatch) =====
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

				// Fixed dispatch: worst-case thread groups for all possible edges
				const uint32 QuadMaxEdges = static_cast<uint32>(GridDim) * GridDim * GridDim * 3;
				const FIntVector QuadGroupCount(
					FMath::DivideAndRoundUp(QuadMaxEdges, 64u),
					1,
					1
				);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("DCQuadGeneration"),
					QuadShader,
					QuadParams,
					QuadGroupCount
				);
			}

			// ===== Extract buffers for persistence =====
			GraphBuilder.QueueBufferExtraction(VertexBuffer, &Result->VertexBuffer);
			GraphBuilder.QueueBufferExtraction(IndexBuffer, &Result->IndexBuffer);
			GraphBuilder.QueueBufferExtraction(MeshCountersBuffer, &Result->CounterBuffer);

			GraphBuilder.Execute();

			const double EndTime = FPlatformTime::Seconds();
			Result->Stats.GenerationTimeMs = static_cast<float>((EndTime - StartTime) * 1000.0);

			// Enqueue async counter readback (non-blocking)
			Result->CounterReadback = new FRHIGPUBufferReadback(TEXT("DCCounterReadback"));
			Result->CounterReadback->EnqueueCopy(RHICmdList, Result->CounterBuffer->GetRHI(), 3 * sizeof(uint32));

			// Store callback for deferred firing — TickReadbacks will fire it when data is ready
			Result->PendingOnComplete = OnComplete;
			Result->PendingHandle = FVoxelMeshingHandle(RequestId, ChunkCoord);
			Result->ReadbackPhase = EReadbackPhase::WaitingForCounters;
			Result->CapturedMaxVertices = CapturedConfig.MaxVerticesPerChunk;
			Result->CapturedMaxIndices = CapturedConfig.MaxIndicesPerChunk;
		}
	);
}

void FVoxelGPUDualContourMesher::Tick(float DeltaTime)
{
	TickReadbacks();
}

void FVoxelGPUDualContourMesher::TickReadbacks()
{
	TArray<TPair<FOnVoxelMeshingComplete, FVoxelMeshingHandle>> CompletedCallbacks;

	{
		FScopeLock Lock(&ResultsLock);
		for (auto& Pair : MeshingResults)
		{
			TSharedPtr<FMeshingResult>& Result = Pair.Value;

			if (Result->ReadbackPhase == EReadbackPhase::WaitingForCounters)
			{
				if (Result->CounterReadback && Result->CounterReadback->IsReady())
				{
					// Counter readback ready — enqueue render cmd to Lock/copy/Unlock
					TSharedPtr<FMeshingResult> SharedResult = Result;
					ENQUEUE_RENDER_COMMAND(LockDCCounters)(
						[SharedResult](FRHICommandListImmediate& RHICmdList)
						{
							const void* Data = SharedResult->CounterReadback->Lock(3 * sizeof(uint32));
							if (Data)
							{
								const uint32* Counts = static_cast<const uint32*>(Data);
								SharedResult->VertexCount = FMath::Min(Counts[0], SharedResult->CapturedMaxVertices);
								SharedResult->IndexCount = FMath::Min(Counts[1], SharedResult->CapturedMaxIndices);
								SharedResult->Stats.VertexCount = SharedResult->VertexCount;
								SharedResult->Stats.IndexCount = SharedResult->IndexCount;
								SharedResult->Stats.FaceCount = SharedResult->IndexCount / 3;
							}
							SharedResult->CounterReadback->Unlock();
							delete SharedResult->CounterReadback;
							SharedResult->CounterReadback = nullptr;
							SharedResult->bCountsRead = true;
						}
					);
					Result->ReadbackPhase = EReadbackPhase::CopyingCounters;
				}
			}
			else if (Result->ReadbackPhase == EReadbackPhase::CopyingCounters)
			{
				// Poll until render cmd has finished copying counters
				if (Result->bCountsRead)
				{
					if (Result->VertexCount == 0 || Result->IndexCount == 0)
					{
						// Empty mesh — skip data readback
						Result->ReadbackMeshData.Reset();
						Result->ReadbackPhase = EReadbackPhase::Complete;
						Result->bIsComplete = true;
						Result->bWasSuccessful = true;
					}
					else
					{
						// Enqueue vertex + index readback on render thread (non-blocking)
						const uint32 VCount = Result->VertexCount;
						const uint32 ICount = Result->IndexCount;
						TSharedPtr<FMeshingResult> SharedResult = Result;
						ENQUEUE_RENDER_COMMAND(EnqueueDCDataReadback)(
							[SharedResult, VCount, ICount](FRHICommandListImmediate& RHICmdList)
							{
								SharedResult->VertexReadback = new FRHIGPUBufferReadback(TEXT("DCVertexReadback"));
								SharedResult->VertexReadback->EnqueueCopy(
									RHICmdList,
									SharedResult->VertexBuffer->GetRHI(),
									VCount * sizeof(FVoxelVertex));

								SharedResult->IndexReadback = new FRHIGPUBufferReadback(TEXT("DCIndexReadback"));
								SharedResult->IndexReadback->EnqueueCopy(
									RHICmdList,
									SharedResult->IndexBuffer->GetRHI(),
									ICount * sizeof(uint32));
							}
						);
						Result->ReadbackPhase = EReadbackPhase::WaitingForData;
					}
				}
			}
			else if (Result->ReadbackPhase == EReadbackPhase::WaitingForData)
			{
				if (Result->VertexReadback && Result->VertexReadback->IsReady()
					&& Result->IndexReadback && Result->IndexReadback->IsReady())
				{
					// Data readback ready — enqueue render cmd to Lock/copy/Unlock
					TSharedPtr<FMeshingResult> SharedResult = Result;
					ENQUEUE_RENDER_COMMAND(LockDCMeshData)(
						[SharedResult](FRHICommandListImmediate& RHICmdList)
						{
							CopyVertexReadbackData_RT(SharedResult);
							CopyIndexReadbackData_RT(SharedResult);

							delete SharedResult->VertexReadback;
							SharedResult->VertexReadback = nullptr;
							delete SharedResult->IndexReadback;
							SharedResult->IndexReadback = nullptr;

							SharedResult->CounterBuffer.SafeRelease();
							SharedResult->bIsComplete = true;
							SharedResult->bWasSuccessful = true;
						}
					);
					Result->ReadbackPhase = EReadbackPhase::CopyingData;
				}
			}
			else if (Result->ReadbackPhase == EReadbackPhase::CopyingData)
			{
				// Poll until render cmd has finished copying mesh data
				if (Result->bIsComplete)
				{
					Result->ReadbackPhase = EReadbackPhase::Complete;
				}
			}

			if (Result->ReadbackPhase == EReadbackPhase::Complete && Result->PendingOnComplete.IsBound())
			{
				CompletedCallbacks.Add(TPair<FOnVoxelMeshingComplete, FVoxelMeshingHandle>(
					Result->PendingOnComplete, Result->PendingHandle));
				Result->PendingOnComplete.Unbind();
			}
		}
	}

	// Fire callbacks outside the lock to avoid deadlocks
	for (auto& Callback : CompletedCallbacks)
	{
		Callback.Key.Execute(Callback.Value, true);
	}
}

void FVoxelGPUDualContourMesher::CopyVertexReadbackData_RT(TSharedPtr<FMeshingResult> Result)
{
	const uint32 VertexCount = Result->VertexCount;

	Result->ReadbackMeshData.Positions.SetNum(VertexCount);
	Result->ReadbackMeshData.Normals.SetNum(VertexCount);
	Result->ReadbackMeshData.UVs.SetNum(VertexCount);
	Result->ReadbackMeshData.UV1s.SetNum(VertexCount);
	Result->ReadbackMeshData.Colors.SetNum(VertexCount);

	const void* Data = Result->VertexReadback->Lock(VertexCount * sizeof(FVoxelVertex));
	if (Data)
	{
		const FVoxelVertex* Vertices = static_cast<const FVoxelVertex*>(Data);
		for (uint32 i = 0; i < VertexCount; ++i)
		{
			Result->ReadbackMeshData.Positions[i] = Vertices[i].Position;
			Result->ReadbackMeshData.Normals[i] = Vertices[i].GetNormal();
			Result->ReadbackMeshData.UVs[i] = Vertices[i].UV;

			const uint8 MaterialID = Vertices[i].GetMaterialID();
			Result->ReadbackMeshData.UV1s[i] = FVector2f(static_cast<float>(MaterialID), 0.0f);

			Result->ReadbackMeshData.Colors[i] = FColor(
				MaterialID,
				Vertices[i].GetBiomeID(),
				Vertices[i].GetAO() * 85,
				255
			);
		}
	}
	Result->VertexReadback->Unlock();
}

void FVoxelGPUDualContourMesher::CopyIndexReadbackData_RT(TSharedPtr<FMeshingResult> Result)
{
	const uint32 IndexCount = Result->IndexCount;

	Result->ReadbackMeshData.Indices.SetNum(IndexCount);

	const void* Data = Result->IndexReadback->Lock(IndexCount * sizeof(uint32));
	if (Data)
	{
		FMemory::Memcpy(Result->ReadbackMeshData.Indices.GetData(), Data, IndexCount * sizeof(uint32));
	}
	Result->IndexReadback->Unlock();
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

	FScopeLock Lock(&ResultsLock);
	const TSharedPtr<FMeshingResult>* ResultPtr = MeshingResults.Find(Handle.RequestId);
	if (!ResultPtr || !(*ResultPtr)->bIsComplete || !(*ResultPtr)->bCountsRead)
	{
		return false;
	}

	OutVertexCount = (*ResultPtr)->VertexCount;
	OutIndexCount = (*ResultPtr)->IndexCount;
	return true;
}

bool FVoxelGPUDualContourMesher::GetRenderData(
	const FVoxelMeshingHandle& Handle,
	FChunkRenderData& OutRenderData)
{
	if (!Handle.IsValid())
	{
		return false;
	}

	FScopeLock Lock(&ResultsLock);
	TSharedPtr<FMeshingResult>* ResultPtr = MeshingResults.Find(Handle.RequestId);
	if (!ResultPtr || !(*ResultPtr)->bIsComplete || !(*ResultPtr)->bCountsRead)
	{
		return false;
	}

	TSharedPtr<FMeshingResult> Result = *ResultPtr;

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

	FScopeLock Lock(&ResultsLock);
	TSharedPtr<FMeshingResult>* ResultPtr = MeshingResults.Find(Handle.RequestId);
	if (!ResultPtr || (*ResultPtr)->ReadbackPhase != EReadbackPhase::Complete)
	{
		return false;
	}

	// Data was already read from GPU by TickReadbacks — just move it
	OutMeshData = MoveTemp((*ResultPtr)->ReadbackMeshData);
	return true;
}

void FVoxelGPUDualContourMesher::ReleaseHandle(const FVoxelMeshingHandle& Handle)
{
	if (!Handle.IsValid())
	{
		return;
	}

	FScopeLock Lock(&ResultsLock);
	MeshingResults.Remove(Handle.RequestId);
	// GPU resources freed via TRefCountPtr/TSharedPtr destructors — no flush needed
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

	FScopeLock Lock(&ResultsLock);
	const TSharedPtr<FMeshingResult>* ResultPtr = MeshingResults.Find(Handle.RequestId);
	if (!ResultPtr)
	{
		return false;
	}

	OutStats = (*ResultPtr)->Stats;
	return true;
}
