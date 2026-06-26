# Biome System

**Module**: VoxelGeneration  
**Dependencies**: VoxelCore

## Overview

The biome system determines terrain characteristics, materials, and vegetation based on environmental parameters (temperature, moisture, elevation).

## Biome Definition

```cpp
/**
 * Biome Definition
 * 
 * Defines all properties for a single biome type.
 */
struct FBiomeDefinition
{
    /** Biome ID (0-255) */
    uint8 BiomeID;
    
    /** Biome name */
    FString BiomeName;
    
    // ==================== Selection Parameters ====================
    
    /** Temperature range (-1 to 1) */
    FVector2D TemperatureRange;
    
    /** Moisture range (-1 to 1) */
    FVector2D MoistureRange;
    
    /** Elevation range (meters) */
    FVector2D ElevationRange;
    
    // ==================== Terrain Shaping ====================
    
    /** Base terrain height */
    float BaseHeight;
    
    /** Noise layers for terrain variation */
    TArray<FNoiseLayer> NoiseLayers;
    
    /** Overall height multiplier */
    float HeightMultiplier;
    
    // ==================== Material Assignment ====================
    
    /** Surface material (grass, sand, snow, etc.) */
    uint8 SurfaceMaterial;
    
    /** Subsurface material (dirt) */
    uint8 SubsurfaceMaterial;
    
    /** Deep material (stone) */
    uint8 DeepMaterial;
    
    /** Bedrock material */
    uint8 BedrockMaterial;
    
    // ==================== Vegetation (Phase 6) ====================
    
    /** Scatter types for this biome */
    TArray<FScatterDefinition> Vegetation;
    
    /** Tree density (0-1) */
    float TreeDensity;
    
    /** Grass density (0-1) */
    float GrassDensity;
};
```

## Biome Selection

### GPU Selection Shader

```hlsl
// Biome selection based on temperature/moisture
uint SelectBiome(float3 WorldPos)
{
    // Sample environmental parameters
    float Temperature = SampleNoise(WorldPos * 0.001, TempNoiseParams);
    float Moisture = SampleNoise(WorldPos * 0.0015, MoistureNoiseParams);
    float Elevation = WorldPos.z;
    
    // Iterate biomes (unrolled for performance)
    for (uint i = 0; i < BiomeCount; ++i)
    {
        BiomeDefinition Biome = Biomes[i];
        
        // Check if parameters match
        if (Temperature >= Biome.TemperatureMin && Temperature <= Biome.TemperatureMax &&
            Moisture >= Biome.MoistureMin && Moisture <= Biome.MoistureMax &&
            Elevation >= Biome.ElevationMin && Elevation <= Biome.ElevationMax)
        {
            return Biome.BiomeID;
        }
    }
    
    return 0; // Default biome
}
```

### Example Biomes

**Plains** (Temperature: 0.0-0.5, Moisture: 0.2-0.6)
- Surface: Grass
- Subsurface: Dirt
- Deep: Stone
- Vegetation: Scattered trees, dense grass

**Desert** (Temperature: 0.6-1.0, Moisture: -1.0-0.2)
- Surface: Sand
- Subsurface: Sandstone
- Deep: Stone
- Vegetation: Cacti (sparse)

**Tundra** (Temperature: -1.0--0.5, Moisture: Any)
- Surface: Snow
- Subsurface: Frozen dirt
- Deep: Stone
- Vegetation: None

**Forest** (Temperature: 0.2-0.7, Moisture: 0.5-1.0)
- Surface: Grass
- Subsurface: Dirt
- Deep: Stone
- Vegetation: Dense trees, bushes

## Biome Blending

For smooth transitions between biomes:

```cpp
/**
 * Multi-biome blending
 * 
 * Blend between up to 4 biomes based on distance.
 */
struct FBiomeBlend
{
    uint8 BiomeIDs[4];
    float Weights[4];  // Sum = 1.0
};

FBiomeBlend CalculateBiomeBlend(FVector WorldPos)
{
    FBiomeBlend Result;
    
    // Sample nearby biomes
    uint8 Center = SelectBiome(WorldPos);
    uint8 North = SelectBiome(WorldPos + FVector(0, 100, 0));
    uint8 East = SelectBiome(WorldPos + FVector(100, 0, 0));
    uint8 South = SelectBiome(WorldPos + FVector(0, -100, 0));
    uint8 West = SelectBiome(WorldPos + FVector(-100, 0, 0));
    
    // Calculate blend weights
    // (Simplified - actual implementation would use distance fields)
    Result.BiomeIDs[0] = Center;
    Result.Weights[0] = 1.0f;
    
    // Blend with neighbors if different
    if (North != Center)
    {
        Result.BiomeIDs[1] = North;
        Result.Weights[1] = 0.2f;
        Result.Weights[0] -= 0.2f;
    }
    
    // ... (blend other directions)
    
    return Result;
}
```

## Integration with Generation

```cpp
// In voxel generation shader
[numthreads(4,4,4)]
void GenerateVoxelData(uint3 ThreadID : SV_DispatchThreadID)
{
    float3 WorldPos = CalculateWorldPosition(ThreadID);
    
    // Select biome
    uint BiomeID = SelectBiome(WorldPos);
    BiomeDefinition Biome = Biomes[BiomeID];
    
    // Generate terrain height using biome noise layers
    float Height = Biome.BaseHeight;
    
    for (int i = 0; i < Biome.NoiseLayerCount; ++i)
    {
        NoiseLayer Layer = Biome.NoiseLayers[i];
        Height += SampleNoise(WorldPos * Layer.Frequency, Layer.Params) * Layer.Amplitude;
    }
    
    Height *= Biome.HeightMultiplier;
    
    // Calculate density
    float Density = Height - WorldPos.z;
    
    // Assign material based on biome and depth
    uint MaterialID = GetBiomeMaterial(BiomeID, Density, WorldPos);
    
    // Write voxel
    VoxelData Output;
    Output.Density = saturate((Density + 1.0) * 0.5) * 255;
    Output.MaterialID = MaterialID;
    Output.BiomeID = BiomeID;
    Output.Metadata = 0;
    
    OutputBuffer[FlatIndex(ThreadID)] = Output;
}
```

## Data Asset Configuration

```cpp
/**
 * Biome Registry Asset
 * 
 * Designer-friendly biome configuration.
 */
UCLASS()
class UVoxelBiomeRegistry : public UDataAsset
{
    GENERATED_BODY()
    
public:
    /** All biome definitions */
    UPROPERTY(EditAnywhere, Category = "Biomes")
    TArray<FBiomeDefinition> Biomes;
    
    /** Temperature noise parameters */
    UPROPERTY(EditAnywhere, Category = "Environmental")
    FNoiseParameters TemperatureNoise;
    
    /** Moisture noise parameters */
    UPROPERTY(EditAnywhere, Category = "Environmental")
    FNoiseParameters MoistureNoise;
    
    /** Get biome by ID */
    const FBiomeDefinition* GetBiome(uint8 BiomeID) const;
    
    /** Upload biomes to GPU buffer */
    void UploadToGPU(FRHICommandListImmediate& RHICmdList);
};
```

## Next Steps

1. Define 4-6 core biomes
2. Implement biome selection shader
3. Create biome data assets
4. Integrate with material system
5. Add biome blending (Phase 4)
6. Implement vegetation scatter (Phase 6)

See [MATERIAL_SYSTEM.md](MATERIAL_SYSTEM.md) for material assignment.
See [SCATTER_SYSTEM.md](SCATTER_SYSTEM.md) for vegetation placement.

