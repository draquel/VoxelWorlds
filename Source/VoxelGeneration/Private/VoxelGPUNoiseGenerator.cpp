// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelGPUNoiseGenerator.h"
#include "VoxelGeneration.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelCaveConfiguration.h"
#include "VoxelBiomeConfiguration.h"
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
		// Cave parameters — packed into vectors (4 layers → .xyzw components)
		SHADER_PARAMETER(int32, CaveEnabled)
		SHADER_PARAMETER(int32, CaveLayerCount)
		SHADER_PARAMETER(FIntVector4, CaveLayerType)        // int4: layer types
		SHADER_PARAMETER(FIntVector4, CaveLayerSeed)        // int4: primary seeds
		SHADER_PARAMETER(FIntVector4, CaveLayerSeed2)       // int4: secondary seeds
		SHADER_PARAMETER(FVector4f, CaveLayerFrequency)     // float4: primary frequencies
		SHADER_PARAMETER(FVector4f, CaveLayerFrequency2)    // float4: secondary frequencies
		SHADER_PARAMETER(FIntVector4, CaveLayerOctaves)     // int4: octave counts
		SHADER_PARAMETER(FVector4f, CaveLayerPersistence)   // float4
		SHADER_PARAMETER(FVector4f, CaveLayerLacunarity)    // float4
		SHADER_PARAMETER(FVector4f, CaveLayerThreshold)     // float4
		SHADER_PARAMETER(FVector4f, CaveLayerCarveStrength) // float4
		SHADER_PARAMETER(FVector4f, CaveLayerCarveFalloff)  // float4
		SHADER_PARAMETER(FVector4f, CaveLayerMinDepth)      // float4
		SHADER_PARAMETER(FVector4f, CaveLayerMaxDepth)      // float4
		SHADER_PARAMETER(FVector4f, CaveLayerDepthFadeWidth)// float4
		SHADER_PARAMETER(FVector4f, CaveLayerVerticalScale) // float4
		// Continentalness parameters
		SHADER_PARAMETER(int32, ContinentalnessEnabled)
		SHADER_PARAMETER(int32, ContinentalnessSeed)
		SHADER_PARAMETER(float, ContinentalnessFrequency)
		SHADER_PARAMETER(int32, ContinentalnessCurveSamples)
		SHADER_PARAMETER_SCALAR_ARRAY(float, ContinentalnessHeightCurve, [32])
		SHADER_PARAMETER_SCALAR_ARRAY(float, ContinentalnessHeightScaleCurve, [32])
		// Water level parameters
		SHADER_PARAMETER(int32, WaterLevelEnabled)
		SHADER_PARAMETER(float, WaterLevelHeight)
		// Biome / material parameters (Phase B). BiomeData: StructuredBuffer<float4>, GPU_BIOME_FLOAT4_STRIDE(4)
		// float4s per biome; layout mirrors UVoxelBiomeConfiguration::BuildGpuData. BiomeCount==0 → shader
		// falls back to the legacy depth-based material.
		SHADER_PARAMETER(int32, BiomeCount)
		SHADER_PARAMETER(float, BiomeBlendWidth)
		SHADER_PARAMETER(int32, TemperatureSeed)
		SHADER_PARAMETER(float, TemperatureFrequency)
		SHADER_PARAMETER(int32, MoistureSeed)
		SHADER_PARAMETER(float, MoistureFrequency)
		SHADER_PARAMETER(int32, EnableUnderwaterMaterials)
		SHADER_PARAMETER(uint32, DefaultUnderwaterMaterial)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, BiomeData)
		// Height material rules (Phase B) — GPU_HEIGHTRULE_FLOAT4_STRIDE(2) float4s each, priority-sorted;
		// HeightRuleCount 0 = disabled.
		SHADER_PARAMETER(int32, HeightRuleCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, HeightRuleData)
		// Ore veins (Phase B) — GPU_OREVEIN_FLOAT4_STRIDE(3) float4s each; the dominant biome's
		// [OreStart,OreCount] range (from BiomeData) indexes into this flat buffer.
		SHADER_PARAMETER(int32, OreVeinCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, OreVeinData)
		// Cave-wall material override (Phase B)
		SHADER_PARAMETER(int32, CaveWallOverrideEnabled)
		SHADER_PARAMETER(uint32, CaveWallMaterialID)
		SHADER_PARAMETER(float, CaveWallMaterialMinDepth)
		// Terrain conditioning zones (Phase B) — 2 float4s each:
		// [CenterX,CenterY,InnerRadius,FalloffWidth][TargetHeight,Strength,0,0]
		SHADER_PARAMETER(int32, ConditioningZoneCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ConditioningZoneData)
		// IslandBowl world-mode params (Phase C) — applied when WorldMode == ISLAND_BOWL (2): edge falloff
		// on the continentalness-modulated height. IslandShape 0=circular/1=rectangle;
		// IslandFalloffType 0=Linear/1=Smooth/2=Squared/3=Exponential.
		SHADER_PARAMETER(int32, IslandShape)
		SHADER_PARAMETER(int32, IslandFalloffType)
		SHADER_PARAMETER(float, IslandRadius)
		SHADER_PARAMETER(float, IslandSizeY)
		SHADER_PARAMETER(float, IslandFalloffWidth)
		SHADER_PARAMETER(float, IslandCenterX)
		SHADER_PARAMETER(float, IslandCenterY)
		SHADER_PARAMETER(float, IslandEdgeHeight)
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

// ==================== Post-pass compute shaders ====================
// GPU ports of the CPU generation post-passes (FVoxelCPUNoiseGenerator::ApplyWaterFillPass /
// ApplyUndergroundClassificationPass). Dispatched in the same render graph as the density pass so
// the readback returns finished voxel data — the CPU-side post-passes previously ran per readback
// batch and were the traversal frame-spike source. Kernels in VoxelPostPasses.usf; semantics must
// stay bit-identical to the CPU passes (validated by the GPUvsCPU parity tests).

class FVoxelWaterColumnSeedCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelWaterColumnSeedCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelWaterColumnSeedCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, ChunkSize)
		SHADER_PARAMETER(float, VoxelSize)
		SHADER_PARAMETER(float, ChunkWorldZ)
		SHADER_PARAMETER(float, WaterLevel)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, VoxelData)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FVoxelWaterFloodSweepCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelWaterFloodSweepCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelWaterFloodSweepCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, ChunkSize)
		SHADER_PARAMETER(float, VoxelSize)
		SHADER_PARAMETER(float, ChunkWorldZ)
		SHADER_PARAMETER(float, WaterLevel)
		SHADER_PARAMETER(uint32, SweepAxis)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, VoxelData)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FVoxelUndergroundColumnCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelUndergroundColumnCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelUndergroundColumnCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, ChunkSize)
		SHADER_PARAMETER(uint32, ClearCaveFlags)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, VoxelData)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FVoxelUndergroundMarkCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelUndergroundMarkCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelUndergroundMarkCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, ChunkSize)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, VoxelData)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ScratchFlags)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FVoxelUndergroundApplyCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelUndergroundApplyCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelUndergroundApplyCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, ChunkSize)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, VoxelData)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ScratchFlags)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVoxelWaterColumnSeedCS, "/Plugin/VoxelWorlds/Private/VoxelPostPasses.usf", "WaterColumnSeedCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelWaterFloodSweepCS, "/Plugin/VoxelWorlds/Private/VoxelPostPasses.usf", "WaterFloodSweepCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelUndergroundColumnCS, "/Plugin/VoxelWorlds/Private/VoxelPostPasses.usf", "UndergroundColumnCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelUndergroundMarkCS, "/Plugin/VoxelWorlds/Private/VoxelPostPasses.usf", "UndergroundMarkCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelUndergroundApplyCS, "/Plugin/VoxelWorlds/Private/VoxelPostPasses.usf", "UndergroundApplyCS", SF_Compute);

/**
 * Append the post-pass dispatches (water fill + underground classification) to the render graph,
 * after the density pass has written VoxelBuffer. Shared by DispatchComputeShader (test/editor)
 * and BeginGenerateChunkGPU (streaming) so both paths produce bit-identical finished data —
 * the GPUvsCPU parity tests transitively validate the streaming path.
 *
 * Gating mirrors FVoxelCPUNoiseGenerator::ApplyPostReadbackPasses:
 * - Water pass: only when water is enabled and the world mode is not SphericalPlanet (radial water).
 *   Column seeding + flood only when the chunk straddles the water level (no seeds otherwise);
 *   the cave-flag clear (CPU phase 3) runs whenever the water pass ran, folded into the
 *   underground column kernel via ClearCaveFlags.
 * - Underground classification: always.
 */
static void AddVoxelPostPassDispatches(
	FRDGBuilder& GraphBuilder,
	const FVoxelNoiseGenerationRequest& Request,
	FRDGBufferRef VoxelBuffer)
{
	const int32 ChunkSize = Request.ChunkSize;
	const int32 TotalVoxels = ChunkSize * ChunkSize * ChunkSize;
	const FIntVector ColumnGroups(
		FMath::DivideAndRoundUp(ChunkSize, 8),
		FMath::DivideAndRoundUp(ChunkSize, 8),
		1);
	const FIntVector VoxelGroups(
		FMath::DivideAndRoundUp(ChunkSize, 4),
		FMath::DivideAndRoundUp(ChunkSize, 4),
		FMath::DivideAndRoundUp(ChunkSize, 4));

	// The CPU pass intentionally uses the base VoxelSize (not the LOD-effective size) — mirror it.
	const float ChunkWorldZ = static_cast<float>(Request.GetChunkWorldPosition().Z);
	const float ChunkTopZ = ChunkWorldZ + ChunkSize * Request.VoxelSize;

	const bool bWaterPass = Request.bEnableWaterLevel && Request.WorldMode != EWorldMode::SphericalPlanet;
	const bool bChunkContainsWaterLevel = bWaterPass
		&& Request.WaterLevel >= ChunkWorldZ && Request.WaterLevel < ChunkTopZ;

	const auto& ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	if (bChunkContainsWaterLevel)
	{
		// Phase 1: column seed
		{
			FVoxelWaterColumnSeedCS::FParameters* Params = GraphBuilder.AllocParameters<FVoxelWaterColumnSeedCS::FParameters>();
			Params->ChunkSize = ChunkSize;
			Params->VoxelSize = Request.VoxelSize;
			Params->ChunkWorldZ = ChunkWorldZ;
			Params->WaterLevel = Request.WaterLevel;
			Params->VoxelData = GraphBuilder.CreateUAV(VoxelBuffer);
			TShaderMapRef<FVoxelWaterColumnSeedCS> Shader(ShaderMap);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelWaterColumnSeed"), Shader, Params, ColumnGroups);
		}

		// Phase 2: flood — repeated bidirectional axis sweeps converge to the same 6-connected
		// fixpoint as the CPU BFS (monotone flag spread). Each round can carry the flood around
		// one more "corner" per axis; ocean-pocket topology converges in 1-2 rounds, 8 is margin.
		constexpr int32 FloodSweepRounds = 8;
		TShaderMapRef<FVoxelWaterFloodSweepCS> SweepShader(ShaderMap);
		for (int32 Round = 0; Round < FloodSweepRounds; ++Round)
		{
			for (const uint32 Axis : { 2u, 0u, 1u }) // Z first: seeds spread down/laterally fastest
			{
				FVoxelWaterFloodSweepCS::FParameters* Params = GraphBuilder.AllocParameters<FVoxelWaterFloodSweepCS::FParameters>();
				Params->ChunkSize = ChunkSize;
				Params->VoxelSize = Request.VoxelSize;
				Params->ChunkWorldZ = ChunkWorldZ;
				Params->WaterLevel = Request.WaterLevel;
				Params->SweepAxis = Axis;
				Params->VoxelData = GraphBuilder.CreateUAV(VoxelBuffer);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelWaterFloodSweep"), SweepShader, Params, ColumnGroups);
			}
		}
	}

	// Underground pass 1 (column scan) + the water pass's cave-flag clear (phase 3) folded in.
	{
		FVoxelUndergroundColumnCS::FParameters* Params = GraphBuilder.AllocParameters<FVoxelUndergroundColumnCS::FParameters>();
		Params->ChunkSize = ChunkSize;
		Params->ClearCaveFlags = bWaterPass ? 1 : 0;
		Params->VoxelData = GraphBuilder.CreateUAV(VoxelBuffer);
		TShaderMapRef<FVoxelUndergroundColumnCS> Shader(ShaderMap);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelUndergroundColumn"), Shader, Params, ColumnGroups);
	}

	// Underground pass 2: PropagationDepth two-phase (Jacobi) iterations — mark into scratch from
	// a consistent snapshot, then apply, matching the CPU pass exactly.
	{
		FRDGBufferRef ScratchBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), TotalVoxels), TEXT("VoxelUndergroundScratch"));
		TShaderMapRef<FVoxelUndergroundMarkCS> MarkShader(ShaderMap);
		TShaderMapRef<FVoxelUndergroundApplyCS> ApplyShader(ShaderMap);

		constexpr int32 PropagationDepth = 2;
		for (int32 Iteration = 0; Iteration < PropagationDepth; ++Iteration)
		{
			{
				FVoxelUndergroundMarkCS::FParameters* Params = GraphBuilder.AllocParameters<FVoxelUndergroundMarkCS::FParameters>();
				Params->ChunkSize = ChunkSize;
				Params->VoxelData = GraphBuilder.CreateUAV(VoxelBuffer);
				Params->ScratchFlags = GraphBuilder.CreateUAV(ScratchBuffer);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelUndergroundMark"), MarkShader, Params, VoxelGroups);
			}
			{
				FVoxelUndergroundApplyCS::FParameters* Params = GraphBuilder.AllocParameters<FVoxelUndergroundApplyCS::FParameters>();
				Params->ChunkSize = ChunkSize;
				Params->VoxelData = GraphBuilder.CreateUAV(VoxelBuffer);
				Params->ScratchFlags = GraphBuilder.CreateUAV(ScratchBuffer);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelUndergroundApply"), ApplyShader, Params, VoxelGroups);
			}
		}
	}
}

// ==================== Shared Shader Parameter Setup ====================

/**
 * Allocate and populate the density compute shader parameters from a generation request.
 * Shared by both the delegate-based DispatchComputeShader (test/editor) and the poll-based
 * BeginGenerateChunkGPU (streaming) paths so they dispatch bit-identically — this is what lets the
 * GPUvsCPU parity test transitively validate the streaming path.
 * Must be called on the render thread (allocates RDG parameters + creates the output UAV).
 */
// Create a StructuredBuffer<float4> SRV from a baked float4 array (Phase B biome/material data).
// Empty input → a 1-element dummy so RDG always has a valid resource (the shader gates on the count param).
static FRDGBufferSRVRef CreateFloat4StructuredSRV(FRDGBuilder& GraphBuilder, const TArray<FVector4f>& Data, const TCHAR* Name)
{
	static const FVector4f Dummy(0.0f, 0.0f, 0.0f, 0.0f);
	const int32 Num = FMath::Max(Data.Num(), 1);
	const void* Src = Data.Num() > 0 ? static_cast<const void*>(Data.GetData()) : static_cast<const void*>(&Dummy);
	FRDGBufferRef Buffer = CreateStructuredBuffer(GraphBuilder, Name, sizeof(FVector4f), Num, Src, static_cast<uint64>(Num) * sizeof(FVector4f));
	return GraphBuilder.CreateSRV(Buffer);
}

static FGenerateVoxelDensityCS::FParameters* BuildDensityShaderParameters(
	FRDGBuilder& GraphBuilder,
	const FVoxelNoiseGenerationRequest& Request,
	FRDGBufferRef VoxelBuffer)
{
	FGenerateVoxelDensityCS::FParameters* Parameters = GraphBuilder.AllocParameters<FGenerateVoxelDensityCS::FParameters>();
	Parameters->ChunkWorldPosition = FVector3f(Request.GetChunkWorldPosition());
	Parameters->ChunkSize = Request.ChunkSize;
	Parameters->VoxelSize = Request.VoxelSize;
	Parameters->LODLevel = Request.LODLevel;
	Parameters->NoiseType = static_cast<int32>(Request.NoiseParams.NoiseType);
	Parameters->NoiseSeed = Request.NoiseParams.Seed;
	Parameters->NoiseOctaves = Request.NoiseParams.Octaves;
	Parameters->NoiseFrequency = Request.NoiseParams.Frequency;
	Parameters->NoiseAmplitude = Request.NoiseParams.Amplitude;
	Parameters->NoiseLacunarity = Request.NoiseParams.Lacunarity;
	Parameters->NoisePersistence = Request.NoiseParams.Persistence;
	Parameters->WorldMode = static_cast<int32>(Request.WorldMode);
	Parameters->SeaLevel = Request.SeaLevel;
	Parameters->HeightScale = Request.HeightScale;
	Parameters->BaseHeight = Request.BaseHeight;
	Parameters->OutputVoxelData = GraphBuilder.CreateUAV(VoxelBuffer);

	// Continentalness parameters — zero-initialize curve arrays
	Parameters->ContinentalnessEnabled = 0;
	Parameters->ContinentalnessSeed = 0;
	Parameters->ContinentalnessFrequency = 0.00002f;
	Parameters->ContinentalnessCurveSamples = 0;
	// SHADER_PARAMETER_SCALAR_ARRAY packs floats into FVector4f (4 per element)
	for (int32 i = 0; i < 8; ++i)
	{
		Parameters->ContinentalnessHeightCurve[i] = FVector4f(0.0f);
		Parameters->ContinentalnessHeightScaleCurve[i] = FVector4f(0.0f);
	}

	if (Request.BiomeConfiguration && Request.BiomeConfiguration->bEnableContinentalness)
	{
		const UVoxelBiomeConfiguration* BiomeConfig = Request.BiomeConfiguration;
		Parameters->ContinentalnessEnabled = 1;
		Parameters->ContinentalnessSeed = Request.NoiseParams.Seed + BiomeConfig->ContinentalnessSeedOffset;
		Parameters->ContinentalnessFrequency = BiomeConfig->ContinentalnessNoiseFrequency;

		// Upload baked curve arrays to shader — pack 4 floats per FVector4f
		const int32 NumSamples = FMath::Min(BiomeConfig->BakedHeightCurve.Num(), 32);
		Parameters->ContinentalnessCurveSamples = NumSamples;
		for (int32 i = 0; i < NumSamples; ++i)
		{
			const int32 VecIdx = i / 4;
			const int32 CompIdx = i % 4;
			reinterpret_cast<float*>(&Parameters->ContinentalnessHeightCurve[VecIdx])[CompIdx] = BiomeConfig->BakedHeightCurve[i];
			reinterpret_cast<float*>(&Parameters->ContinentalnessHeightScaleCurve[VecIdx])[CompIdx] = BiomeConfig->BakedHeightScaleCurve[i];
		}
	}

	// Water level parameters
	Parameters->WaterLevelEnabled = Request.bEnableWaterLevel ? 1 : 0;
	Parameters->WaterLevelHeight = Request.WaterLevel;

	// Cave parameters — zero-initialize packed vectors
	Parameters->CaveEnabled = 0;
	Parameters->CaveLayerCount = 0;
	Parameters->CaveLayerType = FIntVector4(0, 0, 0, 0);
	Parameters->CaveLayerSeed = FIntVector4(0, 0, 0, 0);
	Parameters->CaveLayerSeed2 = FIntVector4(0, 0, 0, 0);
	Parameters->CaveLayerFrequency = FVector4f(0, 0, 0, 0);
	Parameters->CaveLayerFrequency2 = FVector4f(0, 0, 0, 0);
	Parameters->CaveLayerOctaves = FIntVector4(0, 0, 0, 0);
	Parameters->CaveLayerPersistence = FVector4f(0, 0, 0, 0);
	Parameters->CaveLayerLacunarity = FVector4f(0, 0, 0, 0);
	Parameters->CaveLayerThreshold = FVector4f(0, 0, 0, 0);
	Parameters->CaveLayerCarveStrength = FVector4f(0, 0, 0, 0);
	Parameters->CaveLayerCarveFalloff = FVector4f(0, 0, 0, 0);
	Parameters->CaveLayerMinDepth = FVector4f(0, 0, 0, 0);
	Parameters->CaveLayerMaxDepth = FVector4f(0, 0, 0, 0);
	Parameters->CaveLayerDepthFadeWidth = FVector4f(0, 0, 0, 0);
	Parameters->CaveLayerVerticalScale = FVector4f(0, 0, 0, 0);

	if (Request.bEnableCaves && Request.CaveConfiguration)
	{
		const UVoxelCaveConfiguration* CaveConfig = Request.CaveConfiguration;
		if (CaveConfig->bEnableCaves && CaveConfig->HasEnabledLayers())
		{
			Parameters->CaveEnabled = 1;
			int32 LayerIdx = 0;
			for (const FCaveLayerConfig& Layer : CaveConfig->CaveLayers)
			{
				if (!Layer.bEnabled || LayerIdx >= 4)
				{
					continue;
				}
				// Pack into vector components: [0]=X, [1]=Y, [2]=Z, [3]=W
				reinterpret_cast<int32*>(&Parameters->CaveLayerType)[LayerIdx] = static_cast<int32>(Layer.CaveType);
				reinterpret_cast<int32*>(&Parameters->CaveLayerSeed)[LayerIdx] = Request.NoiseParams.Seed + Layer.SeedOffset;
				reinterpret_cast<int32*>(&Parameters->CaveLayerSeed2)[LayerIdx] = Request.NoiseParams.Seed + Layer.SecondNoiseSeedOffset;
				reinterpret_cast<float*>(&Parameters->CaveLayerFrequency)[LayerIdx] = Layer.Frequency;
				reinterpret_cast<float*>(&Parameters->CaveLayerFrequency2)[LayerIdx] = Layer.Frequency * Layer.SecondNoiseFrequencyScale;
				reinterpret_cast<int32*>(&Parameters->CaveLayerOctaves)[LayerIdx] = Layer.Octaves;
				reinterpret_cast<float*>(&Parameters->CaveLayerPersistence)[LayerIdx] = Layer.Persistence;
				reinterpret_cast<float*>(&Parameters->CaveLayerLacunarity)[LayerIdx] = Layer.Lacunarity;
				reinterpret_cast<float*>(&Parameters->CaveLayerThreshold)[LayerIdx] = Layer.Threshold;
				reinterpret_cast<float*>(&Parameters->CaveLayerCarveStrength)[LayerIdx] = Layer.CarveStrength;
				reinterpret_cast<float*>(&Parameters->CaveLayerCarveFalloff)[LayerIdx] = Layer.CarveFalloff;
				reinterpret_cast<float*>(&Parameters->CaveLayerMinDepth)[LayerIdx] = Layer.MinDepth;
				reinterpret_cast<float*>(&Parameters->CaveLayerMaxDepth)[LayerIdx] = Layer.MaxDepth;
				reinterpret_cast<float*>(&Parameters->CaveLayerDepthFadeWidth)[LayerIdx] = Layer.DepthFadeWidth;
				reinterpret_cast<float*>(&Parameters->CaveLayerVerticalScale)[LayerIdx] = Layer.VerticalScale;
				++LayerIdx;
			}
			Parameters->CaveLayerCount = LayerIdx;
		}
	}

	// ==================== Biome / material (Phase B) ====================
	// Defaults mirror the CPU no-config fallback (VoxelCPUNoiseGenerator: seeds +1234/+5678, default freqs).
	Parameters->BiomeCount = 0;
	Parameters->BiomeBlendWidth = 0.15f;
	Parameters->TemperatureSeed = Request.NoiseParams.Seed + 1234;
	Parameters->TemperatureFrequency = 0.00005f;
	Parameters->MoistureSeed = Request.NoiseParams.Seed + 5678;
	Parameters->MoistureFrequency = 0.00007f;
	Parameters->EnableUnderwaterMaterials = 0;
	Parameters->DefaultUnderwaterMaterial = 3;

	const UVoxelBiomeConfiguration* BiomeConfig = Request.BiomeConfiguration;
	const bool bUseBiomes = Request.bEnableBiomes && BiomeConfig && BiomeConfig->BakedGpuBiomeCount > 0;
	if (bUseBiomes)
	{
		Parameters->BiomeCount = BiomeConfig->BakedGpuBiomeCount;
		Parameters->BiomeBlendWidth = FMath::Max(BiomeConfig->BiomeBlendWidth, 0.01f);
		Parameters->TemperatureSeed = Request.NoiseParams.Seed + BiomeConfig->TemperatureSeedOffset;
		Parameters->TemperatureFrequency = BiomeConfig->TemperatureNoiseFrequency;
		Parameters->MoistureSeed = Request.NoiseParams.Seed + BiomeConfig->MoistureSeedOffset;
		Parameters->MoistureFrequency = BiomeConfig->MoistureNoiseFrequency;
		Parameters->EnableUnderwaterMaterials = BiomeConfig->bEnableUnderwaterMaterials ? 1 : 0;
		Parameters->DefaultUnderwaterMaterial = BiomeConfig->DefaultUnderwaterMaterial;
	}
	Parameters->BiomeData = CreateFloat4StructuredSRV(
		GraphBuilder, bUseBiomes ? BiomeConfig->BakedGpuBiomes : TArray<FVector4f>(), TEXT("VoxelBiomeData"));

	// Height material rules + ore veins (Phase B). Baked into BakedGpu* by BuildGpuData; height rules
	// gated by bEnableHeightMaterials, ore ranges already zeroed per-biome when ore veins are disabled.
	Parameters->HeightRuleCount = (bUseBiomes && BiomeConfig->bEnableHeightMaterials) ? BiomeConfig->BakedGpuHeightRuleCount : 0;
	Parameters->OreVeinCount = bUseBiomes ? BiomeConfig->BakedGpuOreVeinCount : 0;
	Parameters->HeightRuleData = CreateFloat4StructuredSRV(
		GraphBuilder, bUseBiomes ? BiomeConfig->BakedGpuHeightRules : TArray<FVector4f>(), TEXT("VoxelHeightRuleData"));
	Parameters->OreVeinData = CreateFloat4StructuredSRV(
		GraphBuilder, bUseBiomes ? BiomeConfig->BakedGpuOreVeins : TArray<FVector4f>(), TEXT("VoxelOreVeinData"));

	// Cave-wall material override (Phase B) — from the cave configuration.
	Parameters->CaveWallOverrideEnabled = 0;
	Parameters->CaveWallMaterialID = 0;
	Parameters->CaveWallMaterialMinDepth = 0.0f;
	if (Request.bEnableCaves && Request.CaveConfiguration && Request.CaveConfiguration->bOverrideCaveWallMaterial)
	{
		Parameters->CaveWallOverrideEnabled = 1;
		Parameters->CaveWallMaterialID = Request.CaveConfiguration->CaveWallMaterialID;
		Parameters->CaveWallMaterialMinDepth = Request.CaveConfiguration->CaveWallMaterialMinDepth;
	}

	// Terrain conditioning zones (Phase B) — gathered game-side per chunk and copied onto the request;
	// flatten the terrain height toward each zone's target before the SDF (matches the CPU order).
	TArray<FVector4f> ConditioningData;
	ConditioningData.Reserve(Request.ConditioningZones.Num() * 2);
	for (const FVoxelConditioningZone& Zone : Request.ConditioningZones)
	{
		ConditioningData.Add(FVector4f(static_cast<float>(Zone.Center.X), static_cast<float>(Zone.Center.Y), Zone.InnerRadius, Zone.FalloffWidth));
		ConditioningData.Add(FVector4f(Zone.TargetHeight, Zone.Strength, 0.0f, 0.0f));
	}
	Parameters->ConditioningZoneCount = Request.ConditioningZones.Num();
	Parameters->ConditioningZoneData = CreateFloat4StructuredSRV(GraphBuilder, ConditioningData, TEXT("VoxelConditioningData"));

	// IslandBowl world-mode params (Phase C) — from the request; ignored by the shader unless WorldMode==IslandBowl.
	Parameters->IslandShape = static_cast<int32>(Request.IslandParams.Shape);
	Parameters->IslandFalloffType = static_cast<int32>(Request.IslandParams.FalloffType);
	Parameters->IslandRadius = Request.IslandParams.IslandRadius;
	Parameters->IslandSizeY = Request.IslandParams.SizeY;
	Parameters->IslandFalloffWidth = Request.IslandParams.FalloffWidth;
	Parameters->IslandCenterX = Request.IslandParams.CenterX;
	Parameters->IslandCenterY = Request.IslandParams.CenterY;
	Parameters->IslandEdgeHeight = Request.IslandParams.EdgeHeight;

	return Parameters;
}

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

			// Populate shader parameters (shared with the streaming BeginGenerateChunkGPU path via
			// BuildDensityShaderParameters — single source of truth so both dispatch bit-identically).
			FGenerateVoxelDensityCS::FParameters* Parameters = BuildDensityShaderParameters(GraphBuilder, CapturedRequest, VoxelBuffer);

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

			// Post-passes (water fill + underground classification) — finished data on the GPU
			AddVoxelPostPassDispatches(GraphBuilder, CapturedRequest, VoxelBuffer);

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

FVoxelGenerationHandle FVoxelGPUNoiseGenerator::BeginGenerateChunkGPU(const FVoxelNoiseGenerationRequest& Request)
{
	if (!bIsInitialized)
	{
		return FVoxelGenerationHandle();
	}

	const uint64 RequestId = NextRequestId++;

	TSharedPtr<FGenerationResult> Result = MakeShared<FGenerationResult>();
	Result->ChunkSize = Request.ChunkSize;
	Result->TotalVoxels = Request.ChunkSize * Request.ChunkSize * Request.ChunkSize;
	{
		FScopeLock Lock(&ResultsLock);
		GenerationResults.Add(RequestId, Result);
	}

	FVoxelNoiseGenerationRequest CapturedRequest = Request;
	const int32 TotalVoxels = Result->TotalVoxels;
	const uint32 BufferSize = static_cast<uint32>(TotalVoxels) * sizeof(uint32);

	// Dispatch the compute pass and enqueue a non-blocking readback in a single render-graph pass.
	ENQUEUE_RENDER_COMMAND(GenerateVoxelDensityAsync)(
		[CapturedRequest, Result, TotalVoxels, BufferSize](FRHICommandListImmediate& RHICmdList)
		{
			const int32 ChunkSize = CapturedRequest.ChunkSize;

			FRDGBuilder GraphBuilder(RHICmdList);

			FRDGBufferDesc VoxelBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), TotalVoxels);
			FRDGBufferRef VoxelBuffer = GraphBuilder.CreateBuffer(VoxelBufferDesc, TEXT("VoxelDensityBufferAsync"));

			FGenerateVoxelDensityCS::FParameters* Parameters = BuildDensityShaderParameters(GraphBuilder, CapturedRequest, VoxelBuffer);

			TShaderMapRef<FGenerateVoxelDensityCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

			const int32 ThreadGroupSize = 4;
			FIntVector GroupCount(
				FMath::DivideAndRoundUp(ChunkSize, ThreadGroupSize),
				FMath::DivideAndRoundUp(ChunkSize, ThreadGroupSize),
				FMath::DivideAndRoundUp(ChunkSize, ThreadGroupSize));

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GenerateVoxelDensityAsync"),
				ComputeShader,
				Parameters,
				GroupCount);

			// Post-passes (water fill + underground classification) — the readback below returns
			// finished voxel data, so the game thread / workers do no per-voxel post-processing.
			AddVoxelPostPassDispatches(GraphBuilder, CapturedRequest, VoxelBuffer);

			// Enqueue the async GPU→CPU copy into a readback within the same graph (no CPU stall).
			Result->DensityReadback = new FRHIGPUBufferReadback(TEXT("VoxelDensityReadback"));
			AddEnqueueCopyPass(GraphBuilder, Result->DensityReadback, VoxelBuffer, BufferSize);

			GraphBuilder.Execute();

			// Signal readiness AFTER the copy is recorded, so the game-thread poll never calls
			// IsReady() on a readback with no pending fence yet (would spuriously read true → empty data).
			Result->bReadbackEnqueued.store(true, std::memory_order_release);
		});

	return FVoxelGenerationHandle(RequestId);
}

EVoxelGPUReadbackStatus FVoxelGPUNoiseGenerator::PollGenerateChunkGPU(
	const FVoxelGenerationHandle& Handle,
	TArray<FVoxelData>& OutVoxelData)
{
	if (!Handle.IsValid())
	{
		return EVoxelGPUReadbackStatus::Failed;
	}

	TSharedPtr<FGenerationResult> Result;
	{
		FScopeLock Lock(&ResultsLock);
		if (TSharedPtr<FGenerationResult>* Found = GenerationResults.Find(Handle.RequestId))
		{
			Result = *Found;
		}
	}
	if (!Result.IsValid())
	{
		return EVoxelGPUReadbackStatus::Failed;
	}

	// Data already unpacked on the render thread — hand it to the caller.
	if (Result->bDataReady.load(std::memory_order_acquire))
	{
		OutVoxelData = MoveTemp(Result->CachedData);
		return EVoxelGPUReadbackStatus::Ready;
	}

	// The copy hasn't been recorded on the render thread yet — still dispatching.
	if (!Result->bReadbackEnqueued.load(std::memory_order_acquire))
	{
		return EVoxelGPUReadbackStatus::Pending;
	}

	// Readback enqueued; once the GPU fence signals, enqueue a one-shot Lock/Unpack/Unlock on the
	// render thread (mirrors the GPU-DC mesher readback pattern). bLockInFlight is game-thread-only.
	if (!Result->bLockInFlight && Result->DensityReadback && Result->DensityReadback->IsReady())
	{
		Result->bLockInFlight = true;
		const int32 TotalVoxels = Result->TotalVoxels;
		const uint32 BufferSize = static_cast<uint32>(TotalVoxels) * sizeof(uint32);
		TSharedPtr<FGenerationResult> SharedResult = Result;
		ENQUEUE_RENDER_COMMAND(LockVoxelDensityReadback)(
			[SharedResult, TotalVoxels, BufferSize](FRHICommandListImmediate& RHICmdList)
			{
				const void* MappedData = SharedResult->DensityReadback->Lock(BufferSize);
				if (MappedData)
				{
					const uint32* PackedData = static_cast<const uint32*>(MappedData);
					SharedResult->CachedData.SetNum(TotalVoxels);
					for (int32 i = 0; i < TotalVoxels; ++i)
					{
						SharedResult->CachedData[i] = FVoxelData::Unpack(PackedData[i]);
					}
				}
				else
				{
					UE_LOG(LogVoxelGeneration, Warning, TEXT("GPU generation: density readback Lock() returned null"));
				}
				SharedResult->DensityReadback->Unlock();
				delete SharedResult->DensityReadback;
				SharedResult->DensityReadback = nullptr;
				SharedResult->bDataReady.store(true, std::memory_order_release);
			});
	}

	return EVoxelGPUReadbackStatus::Pending;
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
