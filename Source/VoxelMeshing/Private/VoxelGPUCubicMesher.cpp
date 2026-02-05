// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelGPUCubicMesher.h"
#include "VoxelMeshing.h"
#include "VoxelCPUCubicMesher.h"
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

// ==================== Compute Shader Declarations ====================

/**
 * Main cubic mesh generation compute shader.
 */
class FGenerateCubicMeshCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGenerateCubicMeshCS);
	SHADER_USE_PARAMETER_STRUCT(FGenerateCubicMeshCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InputVoxelData)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborXPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborXNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborYPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborYNeg)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborZPos)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborZNeg)
		SHADER_PARAMETER(uint32, NeighborFlags)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVoxelVertex>, OutputVertices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutputIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, MeshCounters)
		SHADER_PARAMETER(uint32, ChunkSize)
		SHADER_PARAMETER(float, VoxelSize)
		SHADER_PARAMETER(FVector3f, ChunkWorldPosition)
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
 * Counter reset compute shader.
 */
class FResetMeshCountersCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FResetMeshCountersCS);
	SHADER_USE_PARAMETER_STRUCT(FResetMeshCountersCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, MeshCounters)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FGenerateCubicMeshCS, "/Plugin/VoxelWorlds/Private/CubicMeshGeneration.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FResetMeshCountersCS, "/Plugin/VoxelWorlds/Private/CubicMeshGeneration.usf", "ResetCountersCS", SF_Compute);

// ==================== FVoxelGPUCubicMesher Implementation ====================

FVoxelGPUCubicMesher::FVoxelGPUCubicMesher()
{
}

FVoxelGPUCubicMesher::~FVoxelGPUCubicMesher()
{
	if (bIsInitialized)
	{
		Shutdown();
	}
}

void FVoxelGPUCubicMesher::Initialize()
{
	if (bIsInitialized)
	{
		return;
	}

	bIsInitialized = true;
	UE_LOG(LogVoxelMeshing, Log, TEXT("GPU Cubic Mesher initialized"));
}

void FVoxelGPUCubicMesher::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	// Wait for any pending render commands
	FlushRenderingCommands();

	ReleaseAllHandles();
	bIsInitialized = false;

	UE_LOG(LogVoxelMeshing, Log, TEXT("GPU Cubic Mesher shutdown"));
}

bool FVoxelGPUCubicMesher::GenerateMeshCPU(
	const FVoxelMeshingRequest& Request,
	FChunkMeshData& OutMeshData)
{
	// Fallback to CPU mesher
	FVoxelCPUCubicMesher CPUMesher;
	CPUMesher.Initialize();
	bool bResult = CPUMesher.GenerateMeshCPU(Request, OutMeshData);
	CPUMesher.Shutdown();
	return bResult;
}

bool FVoxelGPUCubicMesher::GenerateMeshCPU(
	const FVoxelMeshingRequest& Request,
	FChunkMeshData& OutMeshData,
	FVoxelMeshingStats& OutStats)
{
	// Fallback to CPU mesher
	FVoxelCPUCubicMesher CPUMesher;
	CPUMesher.Initialize();
	bool bResult = CPUMesher.GenerateMeshCPU(Request, OutMeshData, OutStats);
	CPUMesher.Shutdown();
	return bResult;
}

TArray<uint32> FVoxelGPUCubicMesher::PackVoxelDataForGPU(const TArray<FVoxelData>& VoxelData)
{
	TArray<uint32> PackedData;
	PackedData.SetNum(VoxelData.Num());

	for (int32 i = 0; i < VoxelData.Num(); ++i)
	{
		PackedData[i] = VoxelData[i].Pack();
	}

	return PackedData;
}

FVoxelMeshingHandle FVoxelGPUCubicMesher::GenerateMeshAsync(
	const FVoxelMeshingRequest& Request,
	FOnVoxelMeshingComplete OnComplete)
{
	if (!bIsInitialized)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("GPU Cubic Mesher not initialized"));
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

void FVoxelGPUCubicMesher::DispatchComputeShader(
	const FVoxelMeshingRequest& Request,
	uint64 RequestId,
	TSharedPtr<FMeshingResult> Result,
	FOnVoxelMeshingComplete OnComplete)
{
	// Pack voxel data for GPU
	TArray<uint32> PackedVoxels = PackVoxelDataForGPU(Request.VoxelData);

	// Pack neighbor data
	TArray<uint32> PackedNeighborXPos, PackedNeighborXNeg;
	TArray<uint32> PackedNeighborYPos, PackedNeighborYNeg;
	TArray<uint32> PackedNeighborZPos, PackedNeighborZNeg;

	uint32 NeighborFlags = 0;
	const int32 SliceSize = Request.GetNeighborSliceSize();

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

	// Capture data for lambda
	const int32 ChunkSize = Request.ChunkSize;
	const float VoxelSize = Request.VoxelSize;
	// Use GetChunkWorldPosition() which includes WorldOrigin offset
	const FVector3f ChunkWorldPos = FVector3f(Request.GetChunkWorldPosition());
	const FVoxelMeshingConfig CapturedConfig = Config;
	const FIntVector ChunkCoord = Request.ChunkCoord;

	ENQUEUE_RENDER_COMMAND(GenerateCubicMesh)(
		[PackedVoxels = MoveTemp(PackedVoxels),
		 PackedNeighborXPos = MoveTemp(PackedNeighborXPos),
		 PackedNeighborXNeg = MoveTemp(PackedNeighborXNeg),
		 PackedNeighborYPos = MoveTemp(PackedNeighborYPos),
		 PackedNeighborYNeg = MoveTemp(PackedNeighborYNeg),
		 PackedNeighborZPos = MoveTemp(PackedNeighborZPos),
		 PackedNeighborZNeg = MoveTemp(PackedNeighborZNeg),
		 NeighborFlags,
		 ChunkSize,
		 VoxelSize,
		 ChunkWorldPos,
		 CapturedConfig,
		 ChunkCoord,
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

			// Upload voxel data
			GraphBuilder.QueueBufferUpload(VoxelBuffer, PackedVoxels.GetData(), PackedVoxels.Num() * sizeof(uint32));

			// Create neighbor buffers (even empty ones need valid buffers with data uploaded)
			// RDG requires all read buffers to have been written to
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
					// Empty neighbor - create minimal buffer with dummy data
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

			// Create atomic counter buffer (2 uints: index 0 = vertex count, index 1 = index count)
			FRDGBufferDesc CounterBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 2);
			FRDGBufferRef MeshCountersBuffer = GraphBuilder.CreateBuffer(CounterBufferDesc, TEXT("MeshCounters"));

			// Reset counters pass
			{
				TShaderMapRef<FResetMeshCountersCS> ResetShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FResetMeshCountersCS::FParameters* ResetParams = GraphBuilder.AllocParameters<FResetMeshCountersCS::FParameters>();
				ResetParams->MeshCounters = GraphBuilder.CreateUAV(MeshCountersBuffer);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("ResetMeshCounters"),
					ResetShader,
					ResetParams,
					FIntVector(1, 1, 1)
				);
			}

			// Main meshing pass
			{
				TShaderMapRef<FGenerateCubicMeshCS> MeshShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FGenerateCubicMeshCS::FParameters* MeshParams = GraphBuilder.AllocParameters<FGenerateCubicMeshCS::FParameters>();

				MeshParams->InputVoxelData = GraphBuilder.CreateSRV(VoxelBuffer);
				MeshParams->NeighborXPos = GraphBuilder.CreateSRV(NeighborXPosBuffer);
				MeshParams->NeighborXNeg = GraphBuilder.CreateSRV(NeighborXNegBuffer);
				MeshParams->NeighborYPos = GraphBuilder.CreateSRV(NeighborYPosBuffer);
				MeshParams->NeighborYNeg = GraphBuilder.CreateSRV(NeighborYNegBuffer);
				MeshParams->NeighborZPos = GraphBuilder.CreateSRV(NeighborZPosBuffer);
				MeshParams->NeighborZNeg = GraphBuilder.CreateSRV(NeighborZNegBuffer);
				MeshParams->NeighborFlags = NeighborFlags;
				MeshParams->OutputVertices = GraphBuilder.CreateUAV(VertexBuffer);
				MeshParams->OutputIndices = GraphBuilder.CreateUAV(IndexBuffer);
				MeshParams->MeshCounters = GraphBuilder.CreateUAV(MeshCountersBuffer);
				MeshParams->ChunkSize = ChunkSize;
				MeshParams->VoxelSize = VoxelSize;
				MeshParams->ChunkWorldPosition = ChunkWorldPos;

				// Calculate dispatch dimensions (8x8x4 thread groups)
				FIntVector GroupCount(
					FMath::DivideAndRoundUp(ChunkSize, 8),
					FMath::DivideAndRoundUp(ChunkSize, 8),
					FMath::DivideAndRoundUp(ChunkSize, 4)
				);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("GenerateCubicMesh"),
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

			// Calculate generation time
			const double EndTime = FPlatformTime::Seconds();
			Result->Stats.GenerationTimeMs = static_cast<float>((EndTime - StartTime) * 1000.0);
			Result->bIsComplete = true;
			Result->bWasSuccessful = true;

			// Create handle for callback
			FVoxelMeshingHandle Handle(RequestId, ChunkCoord);
			Handle.bIsComplete = true;
			Handle.bWasSuccessful = true;

			// Call completion callback on game thread
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

void FVoxelGPUCubicMesher::ReadCounters(TSharedPtr<FMeshingResult> Result)
{
	if (!Result || Result->bCountsRead || !Result->CounterBuffer.IsValid())
	{
		return;
	}

	ENQUEUE_RENDER_COMMAND(ReadMeshCounters)(
		[Result](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			// Register external buffer
			FRDGBufferRef CounterBuffer = GraphBuilder.RegisterExternalBuffer(Result->CounterBuffer, TEXT("CounterBuffer"));

			// Create staging buffer
			FRDGBufferDesc StagingDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 2);
			FRDGBufferRef StagingBuffer = GraphBuilder.CreateBuffer(StagingDesc, TEXT("StagingCounters"));

			// Copy to staging
			AddCopyBufferPass(GraphBuilder, StagingBuffer, CounterBuffer);

			// Extract staging buffer
			GraphBuilder.QueueBufferExtraction(StagingBuffer, &Result->StagingCounterBuffer);

			GraphBuilder.Execute();

			// Read from staging buffer
			if (Result->StagingCounterBuffer.IsValid())
			{
				FRHIBuffer* StagingRHI = Result->StagingCounterBuffer->GetRHI();
				void* MappedData = RHICmdList.LockBuffer(
					StagingRHI,
					0,
					2 * sizeof(uint32),
					RLM_ReadOnly
				);

				if (MappedData)
				{
					const uint32* Counts = static_cast<const uint32*>(MappedData);
					Result->VertexCount = Counts[0];
					Result->IndexCount = Counts[1];
					Result->Stats.VertexCount = Result->VertexCount;
					Result->Stats.IndexCount = Result->IndexCount;
					Result->Stats.FaceCount = Result->IndexCount / 6;
					Result->bCountsRead = true;

					RHICmdList.UnlockBuffer(StagingRHI);
				}
			}
		}
	);

	FlushRenderingCommands();
}

bool FVoxelGPUCubicMesher::IsComplete(const FVoxelMeshingHandle& Handle) const
{
	FScopeLock Lock(&ResultsLock);
	const TSharedPtr<FMeshingResult>* ResultPtr = MeshingResults.Find(Handle.RequestId);
	return ResultPtr && (*ResultPtr)->bIsComplete;
}

bool FVoxelGPUCubicMesher::WasSuccessful(const FVoxelMeshingHandle& Handle) const
{
	FScopeLock Lock(&ResultsLock);
	const TSharedPtr<FMeshingResult>* ResultPtr = MeshingResults.Find(Handle.RequestId);
	return ResultPtr && (*ResultPtr)->bWasSuccessful;
}

FRHIBuffer* FVoxelGPUCubicMesher::GetVertexBuffer(const FVoxelMeshingHandle& Handle)
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

FRHIBuffer* FVoxelGPUCubicMesher::GetIndexBuffer(const FVoxelMeshingHandle& Handle)
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

bool FVoxelGPUCubicMesher::GetBufferCounts(
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

	// Read counters if not already done
	if (!Result->bCountsRead)
	{
		const_cast<FVoxelGPUCubicMesher*>(this)->ReadCounters(Result);
	}

	if (Result->bCountsRead)
	{
		OutVertexCount = Result->VertexCount;
		OutIndexCount = Result->IndexCount;
		return true;
	}

	return false;
}

bool FVoxelGPUCubicMesher::GetRenderData(
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

	// Read counters if not already done
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

bool FVoxelGPUCubicMesher::ReadbackToCPU(
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

	// Ensure we have counts
	if (!Result->bCountsRead)
	{
		ReadCounters(Result);
	}

	if (!Result->bCountsRead || Result->VertexCount == 0)
	{
		OutMeshData.Reset();
		return true;  // Empty mesh is valid
	}

	const uint32 VertexCount = Result->VertexCount;
	const uint32 IndexCount = Result->IndexCount;

	// Prepare output
	OutMeshData.Reset();
	OutMeshData.Positions.SetNum(VertexCount);
	OutMeshData.Normals.SetNum(VertexCount);
	OutMeshData.UVs.SetNum(VertexCount);
	OutMeshData.Colors.SetNum(VertexCount);
	OutMeshData.Indices.SetNum(IndexCount);

	FChunkMeshData* OutDataPtr = &OutMeshData;

	ENQUEUE_RENDER_COMMAND(ReadbackMeshData)(
		[Result, VertexCount, IndexCount, OutDataPtr](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			// Register external buffers
			FRDGBufferRef VertexBuffer = GraphBuilder.RegisterExternalBuffer(Result->VertexBuffer, TEXT("VertexBuffer"));
			FRDGBufferRef IndexBuffer = GraphBuilder.RegisterExternalBuffer(Result->IndexBuffer, TEXT("IndexBuffer"));

			// Create staging buffers matching the actual data size
			FRDGBufferDesc VertexStagingDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVoxelVertex), VertexCount);
			FRDGBufferRef VertexStagingBuffer = GraphBuilder.CreateBuffer(VertexStagingDesc, TEXT("VertexStaging"));

			FRDGBufferDesc IndexStagingDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), IndexCount);
			FRDGBufferRef IndexStagingBuffer = GraphBuilder.CreateBuffer(IndexStagingDesc, TEXT("IndexStaging"));

			// Copy only the actual data to staging (source buffers are larger than needed)
			const uint64 VertexBytes = VertexCount * sizeof(FVoxelVertex);
			const uint64 IndexBytes = IndexCount * sizeof(uint32);
			AddCopyBufferPass(GraphBuilder, VertexStagingBuffer, 0, VertexBuffer, 0, VertexBytes);
			AddCopyBufferPass(GraphBuilder, IndexStagingBuffer, 0, IndexBuffer, 0, IndexBytes);

			// Extract staging buffers
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
						OutDataPtr->Normals[i] = Vertices[i].GetNormal();
						OutDataPtr->UVs[i] = Vertices[i].UV;
						OutDataPtr->Colors[i] = FColor(
							Vertices[i].GetMaterialID(),
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

void FVoxelGPUCubicMesher::ReleaseHandle(const FVoxelMeshingHandle& Handle)
{
	if (!Handle.IsValid())
	{
		return;
	}

	FlushRenderingCommands();

	FScopeLock Lock(&ResultsLock);
	MeshingResults.Remove(Handle.RequestId);
}

void FVoxelGPUCubicMesher::ReleaseAllHandles()
{
	FlushRenderingCommands();

	FScopeLock Lock(&ResultsLock);
	MeshingResults.Empty();
}

void FVoxelGPUCubicMesher::SetConfig(const FVoxelMeshingConfig& InConfig)
{
	Config = InConfig;
}

const FVoxelMeshingConfig& FVoxelGPUCubicMesher::GetConfig() const
{
	return Config;
}

bool FVoxelGPUCubicMesher::GetStats(
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

	// Ensure counts are read
	if (!Result->bCountsRead && Result->bIsComplete)
	{
		const_cast<FVoxelGPUCubicMesher*>(this)->ReadCounters(Result);
	}

	OutStats = Result->Stats;
	return true;
}
