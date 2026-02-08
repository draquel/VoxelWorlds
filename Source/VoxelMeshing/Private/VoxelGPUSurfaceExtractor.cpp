// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelGPUSurfaceExtractor.h"
#include "VoxelMeshing.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"

// ==================== Shader Declarations ====================

class FResetSurfaceCounterCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FResetSurfaceCounterCS);
	SHADER_USE_PARAMETER_STRUCT(FResetSurfaceCounterCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SurfacePointCounter)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FScatterSurfaceExtractionCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FScatterSurfaceExtractionCS);
	SHADER_USE_PARAMETER_STRUCT(FScatterSurfaceExtractionCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, InputPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, InputNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float2>, InputUV1s)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>,   InputColors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>,  OccupancyGrid)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FSurfacePointGPU>, OutputSurfacePoints)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>,  SurfacePointCounter)
		SHADER_PARAMETER(uint32, VertexCount)
		SHADER_PARAMETER(uint32, bHasUV1)
		SHADER_PARAMETER(uint32, bHasColors)
		SHADER_PARAMETER(FVector3f, ChunkWorldOrigin)
		SHADER_PARAMETER(float, CellSize)
		SHADER_PARAMETER(uint32, GridDimX)
		SHADER_PARAMETER(uint32, GridDimY)
		SHADER_PARAMETER(uint32, GridDimZ)
		SHADER_PARAMETER(FVector3f, GridOrigin)
		SHADER_PARAMETER(uint32, MaxOutputPoints)
	END_SHADER_PARAMETER_STRUCT()

	// GPU struct must match HLSL (48 bytes)
	struct FSurfacePointGPU
	{
		FVector3f Position;
		FVector3f Normal;
		uint32 MaterialID;
		uint32 BiomeID;
		uint32 FaceType;
		uint32 AO;
		float SlopeAngle;
		uint32 _Pad;
	};

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FResetSurfaceCounterCS,
	"/Plugin/VoxelWorlds/Private/ScatterSurfaceExtraction.usf",
	"ResetCounterCS",
	SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FScatterSurfaceExtractionCS,
	"/Plugin/VoxelWorlds/Private/ScatterSurfaceExtraction.usf",
	"MainCS",
	SF_Compute);

// ==================== Implementation ====================

bool FVoxelGPUSurfaceExtractor::IsGPUExtractionSupported()
{
	return GMaxRHIFeatureLevel >= ERHIFeatureLevel::SM5;
}

void FVoxelGPUSurfaceExtractor::DispatchExtraction(
	FGPUExtractionRequest Request,
	TQueue<FGPUExtractionResult, EQueueMode::Mpsc>* ResultQueue)
{
	if (!ResultQueue)
	{
		return;
	}

	const int32 VertexCount = Request.Positions.Num();
	if (VertexCount == 0)
	{
		FGPUExtractionResult FailResult;
		FailResult.ChunkCoord = Request.ChunkCoord;
		FailResult.bSuccess = false;
		ResultQueue->Enqueue(MoveTemp(FailResult));
		return;
	}

	// Calculate occupancy grid dimensions
	// Find bounds from vertex positions to size the grid tightly
	FVector3f MinPos(FLT_MAX);
	FVector3f MaxPos(-FLT_MAX);
	for (const FVector3f& Pos : Request.Positions)
	{
		MinPos = FVector3f::Min(MinPos, Pos);
		MaxPos = FVector3f::Max(MaxPos, Pos);
	}

	const FVector3f WorldMin = FVector3f(Request.ChunkWorldOrigin) + MinPos;
	const FVector3f WorldMax = FVector3f(Request.ChunkWorldOrigin) + MaxPos;
	const float CellSize = Request.CellSize;

	// Grid origin snapped to cell boundaries
	const FVector3f GridOrigin(
		FMath::FloorToFloat(WorldMin.X / CellSize) * CellSize,
		FMath::FloorToFloat(WorldMin.Y / CellSize) * CellSize,
		FMath::FloorToFloat(WorldMin.Z / CellSize) * CellSize
	);

	const uint32 GridDimX = FMath::Max(1u, static_cast<uint32>(FMath::CeilToInt((WorldMax.X - GridOrigin.X) / CellSize)) + 1);
	const uint32 GridDimY = FMath::Max(1u, static_cast<uint32>(FMath::CeilToInt((WorldMax.Y - GridOrigin.Y) / CellSize)) + 1);
	const uint32 GridDimZ = FMath::Max(1u, static_cast<uint32>(FMath::CeilToInt((WorldMax.Z - GridOrigin.Z) / CellSize)) + 1);
	const uint32 GridTotalCells = GridDimX * GridDimY * GridDimZ;

	// Clamp grid size to prevent excessive GPU memory
	if (GridTotalCells > 256 * 256 * 256)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("GPU scatter extraction: Grid too large (%u cells), falling back to CPU"),
			GridTotalCells);
		FGPUExtractionResult FailResult;
		FailResult.ChunkCoord = Request.ChunkCoord;
		FailResult.bSuccess = false;
		ResultQueue->Enqueue(MoveTemp(FailResult));
		return;
	}

	const bool bHasUV1 = Request.UV1s.Num() == VertexCount;
	const bool bHasColors = Request.Colors.Num() == VertexCount;
	const FVector3f ChunkWorldOriginF(Request.ChunkWorldOrigin);
	const FIntVector ChunkCoord = Request.ChunkCoord;
	const int32 CapturedMaxOutputPoints = MaxOutputPoints;

	// Shared result holder for render thread readback
	struct FGPUExtractionState
	{
		TRefCountPtr<FRDGPooledBuffer> SurfacePointBuffer;
		TRefCountPtr<FRDGPooledBuffer> CounterBuffer;
		FIntVector ChunkCoord;
		int32 MaxOutputPoints = 0;
	};
	TSharedPtr<FGPUExtractionState> State = MakeShared<FGPUExtractionState>();
	State->ChunkCoord = ChunkCoord;
	State->MaxOutputPoints = CapturedMaxOutputPoints;

	ENQUEUE_RENDER_COMMAND(ScatterSurfaceExtraction)(
		[Positions = MoveTemp(Request.Positions),
		 Normals = MoveTemp(Request.Normals),
		 UV1s = MoveTemp(Request.UV1s),
		 Colors = MoveTemp(Request.Colors),
		 VertexCount, bHasUV1, bHasColors,
		 ChunkWorldOriginF, CellSize, GridOrigin,
		 GridDimX, GridDimY, GridDimZ, GridTotalCells,
		 CapturedMaxOutputPoints, ChunkCoord,
		 State, ResultQueue](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			// ---- Upload input buffers ----
			FRDGBufferDesc PosDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), VertexCount);
			FRDGBufferRef PosBuffer = GraphBuilder.CreateBuffer(PosDesc, TEXT("ScatterInputPositions"));
			GraphBuilder.QueueBufferUpload(PosBuffer, Positions.GetData(), Positions.Num() * sizeof(FVector3f));

			FRDGBufferDesc NormDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), VertexCount);
			FRDGBufferRef NormBuffer = GraphBuilder.CreateBuffer(NormDesc, TEXT("ScatterInputNormals"));
			GraphBuilder.QueueBufferUpload(NormBuffer, Normals.GetData(), Normals.Num() * sizeof(FVector3f));

			// UV1 buffer (create minimal dummy if not available)
			static const FVector2f DummyUV1(0.0f, 0.0f);
			FRDGBufferRef UV1Buffer;
			if (bHasUV1)
			{
				FRDGBufferDesc UV1Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector2f), VertexCount);
				UV1Buffer = GraphBuilder.CreateBuffer(UV1Desc, TEXT("ScatterInputUV1s"));
				GraphBuilder.QueueBufferUpload(UV1Buffer, UV1s.GetData(), UV1s.Num() * sizeof(FVector2f));
			}
			else
			{
				FRDGBufferDesc UV1Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector2f), 1);
				UV1Buffer = GraphBuilder.CreateBuffer(UV1Desc, TEXT("ScatterInputUV1s"));
				GraphBuilder.QueueBufferUpload(UV1Buffer, &DummyUV1, sizeof(FVector2f));
			}

			// Color buffer (pack FColor as uint32)
			static const uint32 DummyColor = 0;
			FRDGBufferRef ColorBuffer;
			if (bHasColors)
			{
				FRDGBufferDesc ColorDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), VertexCount);
				ColorBuffer = GraphBuilder.CreateBuffer(ColorDesc, TEXT("ScatterInputColors"));
				GraphBuilder.QueueBufferUpload(ColorBuffer, Colors.GetData(), Colors.Num() * sizeof(uint32));
			}
			else
			{
				FRDGBufferDesc ColorDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
				ColorBuffer = GraphBuilder.CreateBuffer(ColorDesc, TEXT("ScatterInputColors"));
				GraphBuilder.QueueBufferUpload(ColorBuffer, &DummyColor, sizeof(uint32));
			}

			// ---- Create occupancy grid (zero-initialized) ----
			FRDGBufferDesc GridDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), GridTotalCells);
			FRDGBufferRef GridBuffer = GraphBuilder.CreateBuffer(GridDesc, TEXT("ScatterOccupancyGrid"));
			// Zero-init: upload zeros
			TArray<uint32> ZeroGrid;
			ZeroGrid.SetNumZeroed(GridTotalCells);
			GraphBuilder.QueueBufferUpload(GridBuffer, ZeroGrid.GetData(), GridTotalCells * sizeof(uint32));

			// ---- Create output buffers ----
			FRDGBufferDesc OutputDesc = FRDGBufferDesc::CreateStructuredDesc(48, CapturedMaxOutputPoints);
			FRDGBufferRef OutputBuffer = GraphBuilder.CreateBuffer(OutputDesc, TEXT("ScatterOutputPoints"));

			FRDGBufferDesc CounterDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
			FRDGBufferRef CounterBuffer = GraphBuilder.CreateBuffer(CounterDesc, TEXT("ScatterPointCounter"));

			// ---- Pass 1: Reset counter ----
			{
				TShaderMapRef<FResetSurfaceCounterCS> ResetShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FResetSurfaceCounterCS::FParameters* ResetParams =
					GraphBuilder.AllocParameters<FResetSurfaceCounterCS::FParameters>();
				ResetParams->SurfacePointCounter = GraphBuilder.CreateUAV(CounterBuffer);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("ResetSurfaceCounter"),
					ResetShader,
					ResetParams,
					FIntVector(1, 1, 1)
				);
			}

			// ---- Pass 2: Main extraction ----
			{
				TShaderMapRef<FScatterSurfaceExtractionCS> ExtractShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FScatterSurfaceExtractionCS::FParameters* Params =
					GraphBuilder.AllocParameters<FScatterSurfaceExtractionCS::FParameters>();

				Params->InputPositions = GraphBuilder.CreateSRV(PosBuffer);
				Params->InputNormals = GraphBuilder.CreateSRV(NormBuffer);
				Params->InputUV1s = GraphBuilder.CreateSRV(UV1Buffer);
				Params->InputColors = GraphBuilder.CreateSRV(ColorBuffer);
				Params->OccupancyGrid = GraphBuilder.CreateUAV(GridBuffer);
				Params->OutputSurfacePoints = GraphBuilder.CreateUAV(OutputBuffer);
				Params->SurfacePointCounter = GraphBuilder.CreateUAV(CounterBuffer);
				Params->VertexCount = static_cast<uint32>(VertexCount);
				Params->bHasUV1 = bHasUV1 ? 1u : 0u;
				Params->bHasColors = bHasColors ? 1u : 0u;
				Params->ChunkWorldOrigin = ChunkWorldOriginF;
				Params->CellSize = CellSize;
				Params->GridDimX = GridDimX;
				Params->GridDimY = GridDimY;
				Params->GridDimZ = GridDimZ;
				Params->GridOrigin = GridOrigin;
				Params->MaxOutputPoints = static_cast<uint32>(CapturedMaxOutputPoints);

				// 64 threads per group, one thread per vertex
				const int32 GroupCount = FMath::DivideAndRoundUp(VertexCount, 64);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("ScatterSurfaceExtraction"),
					ExtractShader,
					Params,
					FIntVector(GroupCount, 1, 1)
				);
			}

			// ---- Extract buffers for readback ----
			GraphBuilder.QueueBufferExtraction(OutputBuffer, &State->SurfacePointBuffer);
			GraphBuilder.QueueBufferExtraction(CounterBuffer, &State->CounterBuffer);

			GraphBuilder.Execute();

			// ---- Readback: counter then surface points ----
			// Read counter first to know how many points to read
			uint32 PointCount = 0;
			if (State->CounterBuffer.IsValid())
			{
				FRDGBuilder ReadbackBuilder(RHICmdList);

				FRDGBufferRef CounterRef = ReadbackBuilder.RegisterExternalBuffer(State->CounterBuffer, TEXT("CounterReadback"));
				FRDGBufferDesc StagingCounterDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
				FRDGBufferRef StagingCounter = ReadbackBuilder.CreateBuffer(StagingCounterDesc, TEXT("StagingCounter"));
				AddCopyBufferPass(ReadbackBuilder, StagingCounter, CounterRef);

				TRefCountPtr<FRDGPooledBuffer> StagingCounterPool;
				ReadbackBuilder.QueueBufferExtraction(StagingCounter, &StagingCounterPool);
				ReadbackBuilder.Execute();

				if (StagingCounterPool.IsValid())
				{
					FRHIBuffer* StagingRHI = StagingCounterPool->GetRHI();
					void* MappedData = RHICmdList.LockBuffer(StagingRHI, 0, sizeof(uint32), RLM_ReadOnly);
					if (MappedData)
					{
						PointCount = *static_cast<const uint32*>(MappedData);
						PointCount = FMath::Min(PointCount, static_cast<uint32>(CapturedMaxOutputPoints));
						RHICmdList.UnlockBuffer(StagingRHI);
					}
				}
			}

			// Read surface points
			FGPUExtractionResult ExtractionResult;
			ExtractionResult.ChunkCoord = ChunkCoord;

			if (PointCount > 0 && State->SurfacePointBuffer.IsValid())
			{
				FRDGBuilder PointReadbackBuilder(RHICmdList);

				FRDGBufferRef PointRef = PointReadbackBuilder.RegisterExternalBuffer(State->SurfacePointBuffer, TEXT("PointReadback"));
				// Staging buffer must match source size (AddCopyBufferPass copies entire source)
				FRDGBufferDesc StagingPointDesc = FRDGBufferDesc::CreateStructuredDesc(48, CapturedMaxOutputPoints);
				FRDGBufferRef StagingPoints = PointReadbackBuilder.CreateBuffer(StagingPointDesc, TEXT("StagingPoints"));
				AddCopyBufferPass(PointReadbackBuilder, StagingPoints, PointRef);

				TRefCountPtr<FRDGPooledBuffer> StagingPointsPool;
				PointReadbackBuilder.QueueBufferExtraction(StagingPoints, &StagingPointsPool);
				PointReadbackBuilder.Execute();

				if (StagingPointsPool.IsValid())
				{
					FRHIBuffer* StagingRHI = StagingPointsPool->GetRHI();
					void* MappedData = RHICmdList.LockBuffer(StagingRHI, 0, PointCount * 48, RLM_ReadOnly);
					if (MappedData)
					{
						const FSurfacePointGPU* GPUPoints = static_cast<const FSurfacePointGPU*>(MappedData);
						ConvertGPUToCPU(GPUPoints, PointCount, ExtractionResult.SurfacePoints);
						ExtractionResult.bSuccess = true;
						RHICmdList.UnlockBuffer(StagingRHI);
					}
				}
			}
			else
			{
				// Zero points is still a success (empty chunk surface)
				ExtractionResult.bSuccess = true;
			}

			ResultQueue->Enqueue(MoveTemp(ExtractionResult));
		}
	);
}

void FVoxelGPUSurfaceExtractor::ConvertGPUToCPU(
	const FSurfacePointGPU* GPUPoints,
	uint32 Count,
	TArray<FVoxelSurfacePoint>& OutPoints)
{
	OutPoints.Reserve(Count);

	for (uint32 i = 0; i < Count; ++i)
	{
		const FSurfacePointGPU& GPU = GPUPoints[i];

		FVoxelSurfacePoint Point;
		Point.Position = FVector(GPU.Position);
		Point.Normal = FVector(GPU.Normal).GetSafeNormal();
		Point.MaterialID = static_cast<uint8>(GPU.MaterialID);
		Point.BiomeID = static_cast<uint8>(GPU.BiomeID);
		Point.FaceType = (GPU.FaceType == 1) ? EVoxelFaceType::Side :
		                 (GPU.FaceType == 2) ? EVoxelFaceType::Bottom : EVoxelFaceType::Top;
		Point.AmbientOcclusion = static_cast<uint8>(GPU.AO);
		Point.SlopeAngle = GPU.SlopeAngle;

		OutPoints.Add(Point);
	}
}
