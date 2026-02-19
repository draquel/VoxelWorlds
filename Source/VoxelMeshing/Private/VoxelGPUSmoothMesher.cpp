// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelGPUSmoothMesher.h"
#include "VoxelMeshing.h"
#include "VoxelCPUSmoothMesher.h"
#include "VoxelVertex.h"
#include "MarchingCubesTables.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "RenderingThread.h"
#include "DataDrivenShaderPlatformInfo.h"

// ==================== Compute Shader Declarations ====================

/**
 * Main smooth mesh generation compute shader.
 */
class FGenerateSmoothMeshCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGenerateSmoothMeshCS);
	SHADER_USE_PARAMETER_STRUCT(FGenerateSmoothMeshCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InputVoxelData)
		// Face neighbor data (6 faces)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborXPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborXNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborYPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborYNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborZPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborZNeg)
		// Edge neighbor data (12 edges)
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
		// Corner neighbor data (8 corners packed into buffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CornerData)
		// Flags indicating which neighbor data is present
		SHADER_PARAMETER(uint32, NeighborFlags)
		SHADER_PARAMETER(uint32, EdgeCornerFlags)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, TriangleTable)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVoxelVertex>, OutputVertices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutputIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, MeshCounters)
		SHADER_PARAMETER(uint32, ChunkSize)
		SHADER_PARAMETER(float, VoxelSize)
		SHADER_PARAMETER(FVector3f, ChunkWorldPosition)
		SHADER_PARAMETER(float, IsoLevel)
		SHADER_PARAMETER(uint32, LODStride)
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
 * Counter reset compute shader for smooth meshing.
 */
class FResetSmoothMeshCountersCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FResetSmoothMeshCountersCS);
	SHADER_USE_PARAMETER_STRUCT(FResetSmoothMeshCountersCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, MeshCounters)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FGenerateSmoothMeshCS, "/Plugin/VoxelWorlds/Private/SmoothMeshGeneration.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FResetSmoothMeshCountersCS, "/Plugin/VoxelWorlds/Private/SmoothMeshGeneration.usf", "ResetCountersCS", SF_Compute);

// ==================== FVoxelGPUSmoothMesher Implementation ====================

FVoxelGPUSmoothMesher::FVoxelGPUSmoothMesher()
{
}

FVoxelGPUSmoothMesher::~FVoxelGPUSmoothMesher()
{
	if (bIsInitialized)
	{
		Shutdown();
	}
}

void FVoxelGPUSmoothMesher::Initialize()
{
	if (bIsInitialized)
	{
		return;
	}

	bIsInitialized = true;
	UE_LOG(LogVoxelMeshing, Log, TEXT("GPU Smooth Mesher initialized"));
}

void FVoxelGPUSmoothMesher::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	// Wait for any pending render commands
	FlushRenderingCommands();

	ReleaseAllHandles();
	bIsInitialized = false;

	UE_LOG(LogVoxelMeshing, Log, TEXT("GPU Smooth Mesher shutdown"));
}

bool FVoxelGPUSmoothMesher::GenerateMeshCPU(
	const FVoxelMeshingRequest& Request,
	FChunkMeshData& OutMeshData)
{
	// Fallback to CPU mesher
	FVoxelCPUSmoothMesher CPUMesher;
	CPUMesher.Initialize();
	CPUMesher.SetConfig(Config);
	bool bResult = CPUMesher.GenerateMeshCPU(Request, OutMeshData);
	CPUMesher.Shutdown();
	return bResult;
}

bool FVoxelGPUSmoothMesher::GenerateMeshCPU(
	const FVoxelMeshingRequest& Request,
	FChunkMeshData& OutMeshData,
	FVoxelMeshingStats& OutStats)
{
	// Fallback to CPU mesher
	FVoxelCPUSmoothMesher CPUMesher;
	CPUMesher.Initialize();
	CPUMesher.SetConfig(Config);
	bool bResult = CPUMesher.GenerateMeshCPU(Request, OutMeshData, OutStats);
	CPUMesher.Shutdown();
	return bResult;
}

TArray<uint32> FVoxelGPUSmoothMesher::PackVoxelDataForGPU(const TArray<FVoxelData>& VoxelData)
{
	TArray<uint32> PackedData;
	PackedData.SetNum(VoxelData.Num());

	for (int32 i = 0; i < VoxelData.Num(); ++i)
	{
		PackedData[i] = VoxelData[i].Pack();
	}

	return PackedData;
}

TArray<int32> FVoxelGPUSmoothMesher::CreateTriangleTableData()
{
	TArray<int32> TableData;
	TableData.SetNum(256 * 16);

	for (int32 CubeIndex = 0; CubeIndex < 256; ++CubeIndex)
	{
		for (int32 TriIndex = 0; TriIndex < 16; ++TriIndex)
		{
			TableData[CubeIndex * 16 + TriIndex] = MarchingCubesTables::TriTable[CubeIndex][TriIndex];
		}
	}

	return TableData;
}

FVoxelMeshingHandle FVoxelGPUSmoothMesher::GenerateMeshAsync(
	const FVoxelMeshingRequest& Request,
	FOnVoxelMeshingComplete OnComplete)
{
	if (!bIsInitialized)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("GPU Smooth Mesher not initialized"));
		return FVoxelMeshingHandle();
	}

	if (!Request.IsValid())
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("Invalid meshing request"));
		return FVoxelMeshingHandle();
	}

	uint64 RequestId = NextRequestId++;
	FVoxelMeshingHandle Handle(RequestId, Request.ChunkCoord);

	// Create result entry
	TSharedPtr<FMeshingResult> Result = MakeShared<FMeshingResult>();
	Result->ChunkCoord = Request.ChunkCoord;
	Result->ChunkSize = Request.ChunkSize;
	Result->Stats.GenerationTimeMs = 0.0f;

	{
		FScopeLock Lock(&ResultsLock);
		MeshingResults.Add(RequestId, Result);
	}

	// Dispatch compute shader on render thread
	DispatchComputeShader(Request, RequestId, Result, OnComplete);

	return Handle;
}

void FVoxelGPUSmoothMesher::DispatchComputeShader(
	const FVoxelMeshingRequest& Request,
	uint64 RequestId,
	TSharedPtr<FMeshingResult> Result,
	FOnVoxelMeshingComplete OnComplete)
{
	// Pack voxel data for GPU
	TArray<uint32> PackedVoxels = PackVoxelDataForGPU(Request.VoxelData);

	// Create triangle table data
	TArray<int32> TriTableData = CreateTriangleTableData();

	// Pack face neighbor data
	TArray<uint32> PackedNeighborXPos, PackedNeighborXNeg;
	TArray<uint32> PackedNeighborYPos, PackedNeighborYNeg;
	TArray<uint32> PackedNeighborZPos, PackedNeighborZNeg;

	uint32 NeighborFlags = 0;
	const int32 SliceSize = Request.GetNeighborSliceSize();
	const int32 EdgeSize = Request.GetEdgeStripSize();

	if (Request.NeighborXPos.Num() == SliceSize)
	{
		PackedNeighborXPos = PackVoxelDataForGPU(Request.NeighborXPos);
		NeighborFlags |= (1 << 0);
	}
	if (Request.NeighborXNeg.Num() == SliceSize)
	{
		PackedNeighborXNeg = PackVoxelDataForGPU(Request.NeighborXNeg);
		NeighborFlags |= (1 << 1);
	}
	if (Request.NeighborYPos.Num() == SliceSize)
	{
		PackedNeighborYPos = PackVoxelDataForGPU(Request.NeighborYPos);
		NeighborFlags |= (1 << 2);
	}
	if (Request.NeighborYNeg.Num() == SliceSize)
	{
		PackedNeighborYNeg = PackVoxelDataForGPU(Request.NeighborYNeg);
		NeighborFlags |= (1 << 3);
	}
	if (Request.NeighborZPos.Num() == SliceSize)
	{
		PackedNeighborZPos = PackVoxelDataForGPU(Request.NeighborZPos);
		NeighborFlags |= (1 << 4);
	}
	if (Request.NeighborZNeg.Num() == SliceSize)
	{
		PackedNeighborZNeg = PackVoxelDataForGPU(Request.NeighborZNeg);
		NeighborFlags |= (1 << 5);
	}

	// Pack edge neighbor data (12 edges)
	TArray<uint32> PackedEdgeXPosYPos, PackedEdgeXPosYNeg, PackedEdgeXNegYPos, PackedEdgeXNegYNeg;
	TArray<uint32> PackedEdgeXPosZPos, PackedEdgeXPosZNeg, PackedEdgeXNegZPos, PackedEdgeXNegZNeg;
	TArray<uint32> PackedEdgeYPosZPos, PackedEdgeYPosZNeg, PackedEdgeYNegZPos, PackedEdgeYNegZNeg;

	if (Request.EdgeXPosYPos.Num() == EdgeSize)
		PackedEdgeXPosYPos = PackVoxelDataForGPU(Request.EdgeXPosYPos);
	if (Request.EdgeXPosYNeg.Num() == EdgeSize)
		PackedEdgeXPosYNeg = PackVoxelDataForGPU(Request.EdgeXPosYNeg);
	if (Request.EdgeXNegYPos.Num() == EdgeSize)
		PackedEdgeXNegYPos = PackVoxelDataForGPU(Request.EdgeXNegYPos);
	if (Request.EdgeXNegYNeg.Num() == EdgeSize)
		PackedEdgeXNegYNeg = PackVoxelDataForGPU(Request.EdgeXNegYNeg);
	if (Request.EdgeXPosZPos.Num() == EdgeSize)
		PackedEdgeXPosZPos = PackVoxelDataForGPU(Request.EdgeXPosZPos);
	if (Request.EdgeXPosZNeg.Num() == EdgeSize)
		PackedEdgeXPosZNeg = PackVoxelDataForGPU(Request.EdgeXPosZNeg);
	if (Request.EdgeXNegZPos.Num() == EdgeSize)
		PackedEdgeXNegZPos = PackVoxelDataForGPU(Request.EdgeXNegZPos);
	if (Request.EdgeXNegZNeg.Num() == EdgeSize)
		PackedEdgeXNegZNeg = PackVoxelDataForGPU(Request.EdgeXNegZNeg);
	if (Request.EdgeYPosZPos.Num() == EdgeSize)
		PackedEdgeYPosZPos = PackVoxelDataForGPU(Request.EdgeYPosZPos);
	if (Request.EdgeYPosZNeg.Num() == EdgeSize)
		PackedEdgeYPosZNeg = PackVoxelDataForGPU(Request.EdgeYPosZNeg);
	if (Request.EdgeYNegZPos.Num() == EdgeSize)
		PackedEdgeYNegZPos = PackVoxelDataForGPU(Request.EdgeYNegZPos);
	if (Request.EdgeYNegZNeg.Num() == EdgeSize)
		PackedEdgeYNegZNeg = PackVoxelDataForGPU(Request.EdgeYNegZNeg);

	// Pack corner data (8 corners into a single array)
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

	// Copy edge/corner flags
	uint32 EdgeCornerFlags = Request.EdgeCornerFlags;

	// Capture data for lambda
	const int32 ChunkSize = Request.ChunkSize;
	const float VoxelSize = Request.VoxelSize;
	// Use GetChunkWorldPosition() which includes WorldOrigin offset
	const FVector3f ChunkWorldPos = FVector3f(Request.GetChunkWorldPosition());
	const FVoxelMeshingConfig CapturedConfig = Config;
	const FIntVector ChunkCoord = Request.ChunkCoord;

	// Calculate LOD stride: 2^LODLevel
	const int32 LODLevel = FMath::Clamp(Request.LODLevel, 0, 7);
	const uint32 LODStride = 1u << LODLevel;

	ENQUEUE_RENDER_COMMAND(GenerateSmoothMesh)(
		[PackedVoxels = MoveTemp(PackedVoxels),
		 TriTableData = MoveTemp(TriTableData),
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
		 NeighborFlags,
		 EdgeCornerFlags,
		 ChunkSize,
		 VoxelSize,
		 ChunkWorldPos,
		 CapturedConfig,
		 ChunkCoord,
		 LODStride,
		 RequestId,
		 Result,
		 OnComplete](FRHICommandListImmediate& RHICmdList)
		{
			const double StartTime = FPlatformTime::Seconds();
			const int32 TotalVoxels = ChunkSize * ChunkSize * ChunkSize;
			const int32 SliceSize = ChunkSize * ChunkSize;

			FRDGBuilder GraphBuilder(RHICmdList);

			// Create input voxel buffer
			FRDGBufferDesc VoxelBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), TotalVoxels);
			FRDGBufferRef VoxelBuffer = GraphBuilder.CreateBuffer(VoxelBufferDesc, TEXT("InputVoxelData"));
			GraphBuilder.QueueBufferUpload(VoxelBuffer, PackedVoxels.GetData(), PackedVoxels.Num() * sizeof(uint32));

			// Create triangle table buffer
			FRDGBufferDesc TriTableBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), 256 * 16);
			FRDGBufferRef TriTableBuffer = GraphBuilder.CreateBuffer(TriTableBufferDesc, TEXT("TriangleTable"));
			GraphBuilder.QueueBufferUpload(TriTableBuffer, TriTableData.GetData(), TriTableData.Num() * sizeof(int32));

			// Create neighbor buffers
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

			// Create edge neighbor buffers
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

			// Create corner data buffer (always 8 elements)
			FRDGBufferDesc CornerBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 8);
			FRDGBufferRef CornerDataBuffer = GraphBuilder.CreateBuffer(CornerBufferDesc, TEXT("CornerData"));
			GraphBuilder.QueueBufferUpload(CornerDataBuffer, PackedCornerData.GetData(), PackedCornerData.Num() * sizeof(uint32));

			// Create output buffers with max capacity
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

			// Create atomic counter buffer
			FRDGBufferDesc CounterBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 2);
			FRDGBufferRef MeshCountersBuffer = GraphBuilder.CreateBuffer(CounterBufferDesc, TEXT("MeshCounters"));

			// Reset counters pass
			{
				TShaderMapRef<FResetSmoothMeshCountersCS> ResetShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FResetSmoothMeshCountersCS::FParameters* ResetParams = GraphBuilder.AllocParameters<FResetSmoothMeshCountersCS::FParameters>();
				ResetParams->MeshCounters = GraphBuilder.CreateUAV(MeshCountersBuffer);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("ResetSmoothMeshCounters"),
					ResetShader,
					ResetParams,
					FIntVector(1, 1, 1)
				);
			}

			// Main meshing pass
			{
				TShaderMapRef<FGenerateSmoothMeshCS> MeshShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FGenerateSmoothMeshCS::FParameters* MeshParams = GraphBuilder.AllocParameters<FGenerateSmoothMeshCS::FParameters>();

				MeshParams->InputVoxelData = GraphBuilder.CreateSRV(VoxelBuffer);
				// Face neighbors
				MeshParams->NeighborXPos = GraphBuilder.CreateSRV(NeighborXPosBuffer);
				MeshParams->NeighborXNeg = GraphBuilder.CreateSRV(NeighborXNegBuffer);
				MeshParams->NeighborYPos = GraphBuilder.CreateSRV(NeighborYPosBuffer);
				MeshParams->NeighborYNeg = GraphBuilder.CreateSRV(NeighborYNegBuffer);
				MeshParams->NeighborZPos = GraphBuilder.CreateSRV(NeighborZPosBuffer);
				MeshParams->NeighborZNeg = GraphBuilder.CreateSRV(NeighborZNegBuffer);
				// Edge neighbors
				MeshParams->EdgeXPosYPos = GraphBuilder.CreateSRV(EdgeXPosYPosBuffer);
				MeshParams->EdgeXPosYNeg = GraphBuilder.CreateSRV(EdgeXPosYNegBuffer);
				MeshParams->EdgeXNegYPos = GraphBuilder.CreateSRV(EdgeXNegYPosBuffer);
				MeshParams->EdgeXNegYNeg = GraphBuilder.CreateSRV(EdgeXNegYNegBuffer);
				MeshParams->EdgeXPosZPos = GraphBuilder.CreateSRV(EdgeXPosZPosBuffer);
				MeshParams->EdgeXPosZNeg = GraphBuilder.CreateSRV(EdgeXPosZNegBuffer);
				MeshParams->EdgeXNegZPos = GraphBuilder.CreateSRV(EdgeXNegZPosBuffer);
				MeshParams->EdgeXNegZNeg = GraphBuilder.CreateSRV(EdgeXNegZNegBuffer);
				MeshParams->EdgeYPosZPos = GraphBuilder.CreateSRV(EdgeYPosZPosBuffer);
				MeshParams->EdgeYPosZNeg = GraphBuilder.CreateSRV(EdgeYPosZNegBuffer);
				MeshParams->EdgeYNegZPos = GraphBuilder.CreateSRV(EdgeYNegZPosBuffer);
				MeshParams->EdgeYNegZNeg = GraphBuilder.CreateSRV(EdgeYNegZNegBuffer);
				// Corner data
				MeshParams->CornerData = GraphBuilder.CreateSRV(CornerDataBuffer);
				// Flags
				MeshParams->NeighborFlags = NeighborFlags;
				MeshParams->EdgeCornerFlags = EdgeCornerFlags;
				MeshParams->TriangleTable = GraphBuilder.CreateSRV(TriTableBuffer);
				MeshParams->OutputVertices = GraphBuilder.CreateUAV(VertexBuffer);
				MeshParams->OutputIndices = GraphBuilder.CreateUAV(IndexBuffer);
				MeshParams->MeshCounters = GraphBuilder.CreateUAV(MeshCountersBuffer);
				MeshParams->ChunkSize = ChunkSize;
				MeshParams->VoxelSize = VoxelSize;
				MeshParams->ChunkWorldPosition = ChunkWorldPos;
				MeshParams->IsoLevel = CapturedConfig.IsoLevel;
				MeshParams->LODStride = LODStride;

				// Calculate dispatch dimensions (8x8x4 thread groups)
				// With LOD stride, we need fewer threads (ChunkSize/Stride cubes per axis)
				const int32 LODChunkSize = ChunkSize / (int32)LODStride;
				FIntVector GroupCount(
					FMath::DivideAndRoundUp(LODChunkSize, 8),
					FMath::DivideAndRoundUp(LODChunkSize, 8),
					FMath::DivideAndRoundUp(LODChunkSize, 4)
				);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("GenerateSmoothMesh"),
					MeshShader,
					MeshParams,
					GroupCount
				);
			}

			// Extract buffers for persistence
			GraphBuilder.QueueBufferExtraction(VertexBuffer, &Result->VertexBuffer);
			GraphBuilder.QueueBufferExtraction(IndexBuffer, &Result->IndexBuffer);
			GraphBuilder.QueueBufferExtraction(MeshCountersBuffer, &Result->CounterBuffer);

			// Execute the graph
			GraphBuilder.Execute();

			const double EndTime = FPlatformTime::Seconds();
			Result->Stats.GenerationTimeMs = static_cast<float>((EndTime - StartTime) * 1000.0);

			// Enqueue async counter readback (non-blocking)
			Result->CounterReadback = new FRHIGPUBufferReadback(TEXT("SmoothCounterReadback"));
			Result->CounterReadback->EnqueueCopy(RHICmdList, Result->CounterBuffer->GetRHI(), 2 * sizeof(uint32));

			// Store callback for deferred firing — TickReadbacks will fire it when data is ready
			Result->PendingOnComplete = OnComplete;
			Result->PendingHandle = FVoxelMeshingHandle(RequestId, ChunkCoord);
			Result->ReadbackPhase = EReadbackPhase::WaitingForCounters;
			Result->CapturedMaxVertices = CapturedConfig.MaxVerticesPerChunk;
			Result->CapturedMaxIndices = CapturedConfig.MaxIndicesPerChunk;
		}
	);
}

void FVoxelGPUSmoothMesher::Tick(float DeltaTime)
{
	TickReadbacks();
}

void FVoxelGPUSmoothMesher::TickReadbacks()
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
					ENQUEUE_RENDER_COMMAND(LockSmoothCounters)(
						[SharedResult](FRHICommandListImmediate& RHICmdList)
						{
							const void* Data = SharedResult->CounterReadback->Lock(2 * sizeof(uint32));
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
						ENQUEUE_RENDER_COMMAND(EnqueueSmoothDataReadback)(
							[SharedResult, VCount, ICount](FRHICommandListImmediate& RHICmdList)
							{
								SharedResult->VertexReadback = new FRHIGPUBufferReadback(TEXT("SmoothVertexReadback"));
								SharedResult->VertexReadback->EnqueueCopy(
									RHICmdList,
									SharedResult->VertexBuffer->GetRHI(),
									VCount * sizeof(FVoxelVertex));

								SharedResult->IndexReadback = new FRHIGPUBufferReadback(TEXT("SmoothIndexReadback"));
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
					ENQUEUE_RENDER_COMMAND(LockSmoothMeshData)(
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

void FVoxelGPUSmoothMesher::CopyVertexReadbackData_RT(TSharedPtr<FMeshingResult> Result)
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

			// UV1: MaterialID only (smooth meshing uses triplanar, no FaceType needed)
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

void FVoxelGPUSmoothMesher::CopyIndexReadbackData_RT(TSharedPtr<FMeshingResult> Result)
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

bool FVoxelGPUSmoothMesher::IsComplete(const FVoxelMeshingHandle& Handle) const
{
	FScopeLock Lock(&ResultsLock);
	const TSharedPtr<FMeshingResult>* ResultPtr = MeshingResults.Find(Handle.RequestId);
	return ResultPtr && (*ResultPtr)->bIsComplete;
}

bool FVoxelGPUSmoothMesher::WasSuccessful(const FVoxelMeshingHandle& Handle) const
{
	FScopeLock Lock(&ResultsLock);
	const TSharedPtr<FMeshingResult>* ResultPtr = MeshingResults.Find(Handle.RequestId);
	return ResultPtr && (*ResultPtr)->bWasSuccessful;
}

FRHIBuffer* FVoxelGPUSmoothMesher::GetVertexBuffer(const FVoxelMeshingHandle& Handle)
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

FRHIBuffer* FVoxelGPUSmoothMesher::GetIndexBuffer(const FVoxelMeshingHandle& Handle)
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

bool FVoxelGPUSmoothMesher::GetBufferCounts(
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
	if (!ResultPtr || !(*ResultPtr)->bCountsRead)
	{
		return false;
	}

	OutVertexCount = (*ResultPtr)->VertexCount;
	OutIndexCount = (*ResultPtr)->IndexCount;
	return true;
}

bool FVoxelGPUSmoothMesher::GetRenderData(
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

	TSharedPtr<FMeshingResult>& Result = *ResultPtr;

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

bool FVoxelGPUSmoothMesher::ReadbackToCPU(
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

void FVoxelGPUSmoothMesher::ReleaseHandle(const FVoxelMeshingHandle& Handle)
{
	if (!Handle.IsValid())
	{
		return;
	}

	FScopeLock Lock(&ResultsLock);
	MeshingResults.Remove(Handle.RequestId);
	// GPU resources freed via TRefCountPtr/TSharedPtr destructors — no flush needed
}

void FVoxelGPUSmoothMesher::ReleaseAllHandles()
{
	FlushRenderingCommands();

	FScopeLock Lock(&ResultsLock);
	MeshingResults.Empty();
}

void FVoxelGPUSmoothMesher::SetConfig(const FVoxelMeshingConfig& InConfig)
{
	Config = InConfig;
}

const FVoxelMeshingConfig& FVoxelGPUSmoothMesher::GetConfig() const
{
	return Config;
}

bool FVoxelGPUSmoothMesher::GetStats(
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
