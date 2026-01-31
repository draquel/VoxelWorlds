// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelGPUNoiseGenerator.h"
#include "VoxelGeneration.h"
#include "VoxelCPUNoiseGenerator.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "RenderingThread.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "RHIGPUReadback.h"

// ==================== Compute Shader Declaration ====================

class FGenerateVoxelDensityCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGenerateVoxelDensityCS);
	SHADER_USE_PARAMETER_STRUCT(FGenerateVoxelDensityCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FVector3f, ChunkWorldPosition)
		SHADER_PARAMETER(uint32, ChunkSize)
		SHADER_PARAMETER(float, VoxelSize)
		SHADER_PARAMETER(uint32, LODLevel)
		SHADER_PARAMETER(int32, NoiseType)
		SHADER_PARAMETER(int32, NoiseSeed)
		SHADER_PARAMETER(int32, NoiseOctaves)
		SHADER_PARAMETER(float, NoiseFrequency)
		SHADER_PARAMETER(float, NoiseAmplitude)
		SHADER_PARAMETER(float, NoiseLacunarity)
		SHADER_PARAMETER(float, NoisePersistence)
		SHADER_PARAMETER(int32, WorldMode)
		SHADER_PARAMETER(float, SeaLevel)
		SHADER_PARAMETER(float, HeightScale)
		SHADER_PARAMETER(float, BaseHeight)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutputVoxelData)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 4);
	}
};

IMPLEMENT_GLOBAL_SHADER(FGenerateVoxelDensityCS, "/Plugin/VoxelWorlds/Private/GenerateVoxelDensity.usf", "MainCS", SF_Compute);

// ==================== FVoxelGPUNoiseGenerator Implementation ====================

FVoxelGPUNoiseGenerator::FVoxelGPUNoiseGenerator()
{
}

FVoxelGPUNoiseGenerator::~FVoxelGPUNoiseGenerator()
{
	if (bIsInitialized)
	{
		Shutdown();
	}
}

void FVoxelGPUNoiseGenerator::Initialize()
{
	if (bIsInitialized)
	{
		return;
	}

	bIsInitialized = true;
	UE_LOG(LogVoxelGeneration, Log, TEXT("GPU Noise Generator initialized"));
}

void FVoxelGPUNoiseGenerator::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	// Wait for any pending render commands
	FlushRenderingCommands();

	FScopeLock Lock(&ResultsLock);
	GenerationResults.Empty();
	bIsInitialized = false;

	UE_LOG(LogVoxelGeneration, Log, TEXT("GPU Noise Generator shutdown"));
}

FVoxelGenerationHandle FVoxelGPUNoiseGenerator::GenerateChunkAsync(
	const FVoxelNoiseGenerationRequest& Request,
	FOnVoxelGenerationComplete OnComplete)
{
	uint64 RequestId = NextRequestId++;
	FVoxelGenerationHandle Handle(RequestId);

	// Create result entry
	TSharedPtr<FGenerationResult> Result = MakeShared<FGenerationResult>();
	Result->ChunkSize = Request.ChunkSize;
	{
		FScopeLock Lock(&ResultsLock);
		GenerationResults.Add(RequestId, Result);
	}

	// Dispatch compute shader on render thread
	DispatchComputeShader(Request, RequestId, Result, OnComplete);

	return Handle;
}

void FVoxelGPUNoiseGenerator::DispatchComputeShader(
	const FVoxelNoiseGenerationRequest& Request,
	uint64 RequestId,
	TSharedPtr<FGenerationResult> Result,
	FOnVoxelGenerationComplete OnComplete)
{
	// Capture request data for lambda
	FVoxelNoiseGenerationRequest CapturedRequest = Request;

	ENQUEUE_RENDER_COMMAND(GenerateVoxelDensity)(
		[CapturedRequest, RequestId, Result, OnComplete](FRHICommandListImmediate& RHICmdList)
		{
			const int32 ChunkSize = CapturedRequest.ChunkSize;
			const int32 TotalVoxels = ChunkSize * ChunkSize * ChunkSize;

			// Create RDG builder following GPU_PIPELINE.md pattern
			FRDGBuilder GraphBuilder(RHICmdList);

			// Create output buffer using RDG pattern from GPU_PIPELINE.md
			FRDGBufferDesc VoxelBufferDesc = FRDGBufferDesc::CreateStructuredDesc(
				sizeof(uint32),
				TotalVoxels
			);
			FRDGBufferRef VoxelBuffer = GraphBuilder.CreateBuffer(VoxelBufferDesc, TEXT("VoxelDensityBuffer"));

			// Allocate and set up shader parameters
			FGenerateVoxelDensityCS::FParameters* Parameters = GraphBuilder.AllocParameters<FGenerateVoxelDensityCS::FParameters>();
			Parameters->ChunkWorldPosition = FVector3f(CapturedRequest.GetChunkWorldPosition());
			Parameters->ChunkSize = ChunkSize;
			Parameters->VoxelSize = CapturedRequest.VoxelSize;
			Parameters->LODLevel = CapturedRequest.LODLevel;
			Parameters->NoiseType = static_cast<int32>(CapturedRequest.NoiseParams.NoiseType);
			Parameters->NoiseSeed = CapturedRequest.NoiseParams.Seed;
			Parameters->NoiseOctaves = CapturedRequest.NoiseParams.Octaves;
			Parameters->NoiseFrequency = CapturedRequest.NoiseParams.Frequency;
			Parameters->NoiseAmplitude = CapturedRequest.NoiseParams.Amplitude;
			Parameters->NoiseLacunarity = CapturedRequest.NoiseParams.Lacunarity;
			Parameters->NoisePersistence = CapturedRequest.NoiseParams.Persistence;
			Parameters->WorldMode = static_cast<int32>(CapturedRequest.WorldMode);
			Parameters->SeaLevel = CapturedRequest.SeaLevel;
			Parameters->HeightScale = CapturedRequest.HeightScale;
			Parameters->BaseHeight = CapturedRequest.BaseHeight;
			Parameters->OutputVoxelData = GraphBuilder.CreateUAV(VoxelBuffer);

			// Get shader
			TShaderMapRef<FGenerateVoxelDensityCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

			// Calculate dispatch dimensions (4x4x4 thread groups)
			const int32 ThreadGroupSize = 4;
			FIntVector GroupCount(
				FMath::DivideAndRoundUp(ChunkSize, ThreadGroupSize),
				FMath::DivideAndRoundUp(ChunkSize, ThreadGroupSize),
				FMath::DivideAndRoundUp(ChunkSize, ThreadGroupSize)
			);

			// Add compute pass using RDG pattern from GPU_PIPELINE.md
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GenerateVoxelDensity"),
				ComputeShader,
				Parameters,
				GroupCount
			);

			// Extract the buffer to persist it beyond graph execution
			GraphBuilder.QueueBufferExtraction(VoxelBuffer, &Result->OutputBuffer);

			// Execute the graph
			GraphBuilder.Execute();

			// Create handle for callback
			FVoxelGenerationHandle Handle(RequestId);
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

bool FVoxelGPUNoiseGenerator::GenerateChunkCPU(
	const FVoxelNoiseGenerationRequest& Request,
	TArray<FVoxelData>& OutVoxelData)
{
	// Use the CPU generator for blocking generation
	FVoxelCPUNoiseGenerator CPUGenerator;
	CPUGenerator.Initialize();
	bool bResult = CPUGenerator.GenerateChunkCPU(Request, OutVoxelData);
	CPUGenerator.Shutdown();
	return bResult;
}

float FVoxelGPUNoiseGenerator::SampleNoiseAt(
	const FVector& WorldPosition,
	const FVoxelNoiseParams& Params)
{
	// Use CPU for single-point sampling
	return FVoxelCPUNoiseGenerator::FBM3D(WorldPosition, Params);
}

FRHIBuffer* FVoxelGPUNoiseGenerator::GetGeneratedBuffer(const FVoxelGenerationHandle& Handle)
{
	if (!Handle.IsValid())
	{
		return nullptr;
	}

	FScopeLock Lock(&ResultsLock);
	if (TSharedPtr<FGenerationResult>* ResultPtr = GenerationResults.Find(Handle.RequestId))
	{
		if ((*ResultPtr)->OutputBuffer.IsValid())
		{
			return (*ResultPtr)->OutputBuffer->GetRHI();
		}
	}

	return nullptr;
}

bool FVoxelGPUNoiseGenerator::ReadbackToCPU(
	const FVoxelGenerationHandle& Handle,
	TArray<FVoxelData>& OutVoxelData)
{
	if (!Handle.IsValid())
	{
		return false;
	}

	TSharedPtr<FGenerationResult> Result;
	{
		FScopeLock Lock(&ResultsLock);
		TSharedPtr<FGenerationResult>* FoundResult = GenerationResults.Find(Handle.RequestId);
		if (!FoundResult || !(*FoundResult)->OutputBuffer)
		{
			return false;
		}
		Result = *FoundResult;
	}

	// Check if we already have cached data
	if (Result->bReadbackComplete)
	{
		OutVoxelData = Result->CachedData;
		return true;
	}

	const int32 ChunkSize = Result->ChunkSize;
	const int32 TotalVoxels = ChunkSize * ChunkSize * ChunkSize;
	const uint32 BufferSize = TotalVoxels * sizeof(uint32);

	// Prepare output array
	OutVoxelData.SetNum(TotalVoxels);
	TArray<FVoxelData>* OutDataPtr = &OutVoxelData;

	// Perform GPU readback on render thread using RDG
	ENQUEUE_RENDER_COMMAND(ReadbackVoxelData)(
		[Result, TotalVoxels, BufferSize, OutDataPtr](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			// Register the external buffer with RDG
			FRDGBufferRef SourceBuffer = GraphBuilder.RegisterExternalBuffer(Result->OutputBuffer, TEXT("VoxelSourceBuffer"));

			// Create staging buffer for readback
			FRDGBufferDesc StagingBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), TotalVoxels);
			FRDGBufferRef StagingBuffer = GraphBuilder.CreateBuffer(StagingBufferDesc, TEXT("VoxelStagingBuffer"));

			// Add copy pass
			AddCopyBufferPass(GraphBuilder, StagingBuffer, SourceBuffer);

			// Extract staging buffer
			GraphBuilder.QueueBufferExtraction(StagingBuffer, &Result->StagingBuffer);

			GraphBuilder.Execute();

			// Now read from the extracted staging buffer's RHI resource
			FRHIBuffer* StagingRHIBuffer = Result->StagingBuffer->GetRHI();
			void* MappedData = RHICmdList.LockBuffer(
				StagingRHIBuffer,
				0,
				BufferSize,
				RLM_ReadOnly
			);

			if (MappedData)
			{
				const uint32* PackedData = static_cast<const uint32*>(MappedData);
				for (int32 i = 0; i < TotalVoxels; ++i)
				{
					(*OutDataPtr)[i] = FVoxelData::Unpack(PackedData[i]);
				}

				RHICmdList.UnlockBuffer(StagingRHIBuffer);
			}
		}
	);

	// Wait for readback to complete
	FlushRenderingCommands();

	// Cache the result
	Result->CachedData = OutVoxelData;
	Result->bReadbackComplete = true;

	return true;
}

void FVoxelGPUNoiseGenerator::ReleaseHandle(const FVoxelGenerationHandle& Handle)
{
	if (!Handle.IsValid())
	{
		return;
	}

	// Wait for any pending render commands before releasing
	FlushRenderingCommands();

	FScopeLock Lock(&ResultsLock);
	GenerationResults.Remove(Handle.RequestId);
}

FBufferRHIRef FVoxelGPUNoiseGenerator::CreateOutputBuffer(int32 ChunkSize)
{
	// This function is no longer used - buffer creation happens via RDG
	// Kept for interface compatibility
	return nullptr;
}

FBufferRHIRef FVoxelGPUNoiseGenerator::CreateStagingBuffer(int32 ChunkSize)
{
	// This function is no longer used - buffer creation happens via RDG
	// Kept for interface compatibility
	return nullptr;
}
