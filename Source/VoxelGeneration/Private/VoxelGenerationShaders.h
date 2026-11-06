// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "VoxelGPUNoiseGenerator.h"
#include "VoxelGeneration.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelCaveConfiguration.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelBiomeBlendDither.h"
#include "VoxelDownLines.h"
#include "VoxelSeaSurface.h"
#include "VoxelNoise.h"
#include "SphericalPlanetWorldMode.h"
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

// The voxel generation's global compute shaders (the density pass and the post-passes), declared for the GPU generator
// (FVoxelGPUNoiseGenerator) and implemented in VoxelGenerationShaders.cpp. Private to VoxelGeneration.

// ==================== Compute Shader Declaration ====================

static_assert(VOXELGEN_MAX_GPU_CAVE_LAYERS == 4, "the cave layer parameters below are int4 / float4: one lane per layer");

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
		SHADER_PARAMETER(float, NoiseLacunarity)
		SHADER_PARAMETER(float, NoisePersistence)
		SHADER_PARAMETER(int32, NoiseRidged)
		SHADER_PARAMETER(int32, WorldMode)
		SHADER_PARAMETER(float, SeaLevel)
		SHADER_PARAMETER(float, HeightScale)
		SHADER_PARAMETER(float, BaseHeight)
		// Cave parameters — packed into vectors (4 layers → .xyzw components)
		SHADER_PARAMETER(int32, CaveEnabled)
		SHADER_PARAMETER(int32, CaveLayerCount)            // at most VOXELGEN_MAX_GPU_CAVE_LAYERS: one lane of each int4 / float4 below per layer
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
		SHADER_PARAMETER(int32, ContinentalnessEqualize)
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
		// Climate shaping (ClimateShaping.ush; FVoxelBiomeSnapshot's Shape* functions on the CPU)
		SHADER_PARAMETER(int32, ClimateEqualize)
		SHADER_PARAMETER(float, TemperatureLatitudeWeight)
		SHADER_PARAMETER(float, TemperatureLapseRate)
		SHADER_PARAMETER(int32, EnableUnderwaterMaterials)
		SHADER_PARAMETER(uint32, DefaultUnderwaterMaterial)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, BiomeData)
		// The blend zones' dither (G39): the chunk's first voxel on the world's grid.
		SHADER_PARAMETER(FIntVector, BlendDitherBase)
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
		// The seabed beyond the edge (WM-E1): FIslandBowlParams::SeabedDepth / SeabedSlopeWidth.
		SHADER_PARAMETER(float, IslandSeabedDepth)
		SHADER_PARAMETER(float, IslandSeabedSlopeWidth)
		// SphericalPlanet world-mode params (spherical Phase 2, SP-B4) — applied when WorldMode == SPHERICAL_PLANET (1),
		// see SphericalPlanet.ush. From the request's SphericalPlanetParams (FVoxelWorldModeFactory) and WaterRadius.
		// Chunk-relative (spherical Phase 6, WM-BP6): the sea as an altitude and the ore key's integer chunk base, formed in
		// double; the shell's quick reject by altitude.
		SHADER_PARAMETER(float, PlanetRadius)
		SHADER_PARAMETER(float, PlanetMaxTerrainHeight)
		SHADER_PARAMETER(float, PlanetMaxTerrainDepth)
		SHADER_PARAMETER(float, PlanetWaterAltitude)
		SHADER_PARAMETER(FIntVector, ChunkBaseInt)
		SHADER_PARAMETER(FVector3f, ChunkBaseFrac)
		// The planet noise's per-chunk origins (spherical Phase 6, WM-BP5): BuildNoiseOrigins, 2 float4s a split.
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, NoiseOrigins)
		SHADER_PARAMETER(int32, TerrainNoiseOrigin)
		SHADER_PARAMETER(int32, ContinentNoiseOrigin)
		SHADER_PARAMETER(int32, TemperatureNoiseOrigin)
		SHADER_PARAMETER(int32, MoistureNoiseOrigin)
		SHADER_PARAMETER(FIntVector4, CaveNoiseOrigin)
		SHADER_PARAMETER(int32, OreNoiseOrigin)
		SHADER_PARAMETER(FVector3f, PlanetAnchorLocal)
		SHADER_PARAMETER(FVector3f, PlanetAnchorDir)
		SHADER_PARAMETER(float, PlanetAnchorDist)
		SHADER_PARAMETER(float, PlanetAnchorAltitude)
		// The cave configuration's underwater minimum depth and biome overrides (GpuCaveCarve), 1 float4 each:
		// [BiomeID, CaveScale, MinDepthOverride, 0].
		SHADER_PARAMETER(float, CaveUnderwaterMinDepth)
		SHADER_PARAMETER(int32, CaveBiomeOverrideCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, CaveBiomeOverrideData)
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

// Flood sweep parameters, shared by the plane's sweep (below the WaterLevel plane) and the planet's (inside the sea
// sphere: PlanetChunkOffset and SeaRadiusSq).
BEGIN_SHADER_PARAMETER_STRUCT(FVoxelWaterFloodSweepParameters, )
	SHADER_PARAMETER(uint32, ChunkSize)
	SHADER_PARAMETER(float, VoxelSize)
	SHADER_PARAMETER(float, ChunkWorldZ)
	SHADER_PARAMETER(float, WaterLevel)
	SHADER_PARAMETER(uint32, SweepAxis)
	SHADER_PARAMETER(FVector3f, PlanetAnchorLocal)
	SHADER_PARAMETER(FVector3f, PlanetAnchorDir)
	SHADER_PARAMETER(float, PlanetAnchorDist)
	SHADER_PARAMETER(float, PlanetSeaAltitudeOverAnchor)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, VoxelData)
END_SHADER_PARAMETER_STRUCT()

class FVoxelWaterFloodSweepCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelWaterFloodSweepCS);
	using FParameters = FVoxelWaterFloodSweepParameters;
	SHADER_USE_PARAMETER_STRUCT(FVoxelWaterFloodSweepCS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/** The planet's flood sweep: eligibility is the sea sphere instead of the WaterLevel plane (spherical SP-B8). */
class FVoxelWaterFloodSweepRadialCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelWaterFloodSweepRadialCS);
	using FParameters = FVoxelWaterFloodSweepParameters;
	SHADER_USE_PARAMETER_STRUCT(FVoxelWaterFloodSweepRadialCS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// The planet's line passes (spherical Phase 2, SP-B7 / SP-B8 on the GPU): the digital lines along the chunk's local
// down (FVoxelDownLines, uploaded as the CPU passes compute them), the sea sphere, and the cave-flag clear.
BEGIN_SHADER_PARAMETER_STRUCT(FVoxelDownLinesParameters, )
	SHADER_PARAMETER(uint32, ChunkSize)
	SHADER_PARAMETER(float, VoxelSize)
	SHADER_PARAMETER(uint32, ClearCaveFlags)
	SHADER_PARAMETER(uint32, LineAxisA)
	SHADER_PARAMETER(uint32, LineForward)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int2>, LineShifts)
	SHADER_PARAMETER(FVector3f, PlanetAnchorLocal)
	SHADER_PARAMETER(FVector3f, PlanetAnchorDir)
	SHADER_PARAMETER(float, PlanetAnchorDist)
	SHADER_PARAMETER(float, PlanetSeaAltitudeOverAnchor)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, VoxelData)
END_SHADER_PARAMETER_STRUCT()

/** The planet's water seed: one thread per line, ApplyRadialWaterFillPass phase 1. */
class FVoxelWaterLineSeedCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelWaterLineSeedCS);
	using FParameters = FVoxelDownLinesParameters;
	SHADER_USE_PARAMETER_STRUCT(FVoxelWaterLineSeedCS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/** The planet's underground scan: one thread per line, ApplyUndergroundClassificationPass pass 1 along local down. */
class FVoxelUndergroundLinesCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelUndergroundLinesCS);
	using FParameters = FVoxelDownLinesParameters;
	SHADER_USE_PARAMETER_STRUCT(FVoxelUndergroundLinesCS, FGlobalShader);

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
