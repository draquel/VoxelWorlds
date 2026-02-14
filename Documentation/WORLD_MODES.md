# World Modes

**Module**: VoxelGeneration
**Dependencies**: VoxelCore
**Last Updated**: 2026-02-06

## Overview

The world mode system supports three terrain types, each optimized for different use cases. All modes use the same voxel generation and meshing pipelines but with mode-specific density calculations and LOD culling.

## IVoxelWorldMode Interface

```cpp
class IVoxelWorldMode {
public:
    virtual ~IVoxelWorldMode() = default;

    // Core density evaluation
    virtual float GetDensityAt(const FVector& WorldPos, const FVoxelNoiseParams& NoiseParams) const = 0;

    // Material and biome selection
    virtual uint8 GetMaterialAt(const FVector& WorldPos, float Density) const = 0;
    virtual uint8 GetBiomeAt(const FVector& WorldPos) const = 0;

    // Static helper for CPU/GPU consistency
    static float ApplyWorldModeSDF(float BaseDensity, const FVector& WorldPos, const FWorldModeParams& Params);
};
```

---

## Mode 1: Infinite Plane (Default)

### Overview
Standard heightmap-style terrain extending infinitely in X and Y directions. The most common mode for traditional voxel games.

### Configuration (UVoxelWorldConfiguration)
```cpp
// World Settings
EWorldMode WorldMode = EWorldMode::InfinitePlane;

// Terrain Generation
float SeaLevel = 0.0f;           // Base elevation
float HeightScale = 5000.0f;     // Noise amplitude
float BaseHeight = 0.0f;         // Additional offset
```

### Implementation (FInfinitePlaneWorldMode)
```cpp
float FInfinitePlaneWorldMode::GetDensityAt(const FVector& WorldPos,
                                             const FVoxelNoiseParams& NoiseParams) const
{
    // Sample 2D heightmap noise
    float HeightNoise = SampleFBM2D(WorldPos.X, WorldPos.Y, NoiseParams);

    // Calculate terrain height at this XY position
    float TerrainHeight = SeaLevel + BaseHeight + (HeightNoise * HeightScale);

    // SDF: positive below surface, negative above
    return TerrainHeight - WorldPos.Z;
}
```

### LOD Culling
- **Terrain Bounds Culling**: Chunks above `TerrainMaxHeight` or below `TerrainMinHeight` are skipped
- Height range: `SeaLevel + BaseHeight ± HeightScale` with buffer for noise variation

### Use Cases
- Traditional Minecraft-style worlds
- Large-scale terrain exploration
- Any game requiring unlimited horizontal exploration

---

## Mode 2: Island/Bowl

### Overview
Bounded terrain with smooth falloff at edges, creating floating island or mountain bowl effects. Ideal for contained game worlds.

### Configuration (UVoxelWorldConfiguration)
```cpp
// World Settings
EWorldMode WorldMode = EWorldMode::IslandBowl;

// Island Shape
int32 IslandShape = 0;               // 0=Circular, 1=Rectangle

// Island Parameters
float IslandRadius = 50000.0f;       // Radius (circular) or SizeX (rectangle)
float IslandSizeY = 50000.0f;        // SizeY (rectangle only, ignored for circular)
float IslandFalloffWidth = 10000.0f; // Width of transition zone
int32 IslandFalloffType = 1;         // 0=Linear, 1=Smooth, 2=Squared, 3=Exponential
float IslandCenterX = 0.0f;          // Center offset from WorldOrigin
float IslandCenterY = 0.0f;          // Center offset from WorldOrigin
float IslandEdgeHeight = -1000.0f;   // Terrain height at island edge
bool bIslandBowlShape = false;       // True for bowl (lowered center)
```

### Island Shape Types
```cpp
enum class EIslandShape : uint8 {
    Circular = 0,    // Radial distance from center (default)
    Rectangle = 1    // Axis-aligned box with separate X/Y sizes
};
```

### Falloff Types
```cpp
enum class EIslandFalloffType : uint8 {
    Linear = 0,      // Simple linear fade
    Smooth = 1,      // Hermite interpolation (default, natural-looking)
    Squared = 2,     // Faster drop near edge
    Exponential = 3  // Gradual then sharp drop
};
```

### Implementation (FIslandBowlWorldMode)

For **Circular** islands, falloff is based on Euclidean distance from center:
```cpp
float Distance = FMath::Sqrt(DX * DX + DY * DY);
float T = (Distance - IslandRadius) / FalloffWidth;  // 0 at edge, 1 at falloff end
```

For **Rectangle** islands, falloff is based on the maximum normalized axis distance:
```cpp
// Calculate how far into falloff zone on each axis
float TX = FMath::Max(0.0f, (FMath::Abs(DX) - IslandRadius) / FalloffWidth);
float TY = FMath::Max(0.0f, (FMath::Abs(DY) - SizeY) / FalloffWidth);
float T = FMath::Max(TX, TY);  // Use the axis furthest into falloff
```

```cpp
float FIslandBowlWorldMode::CalculateFalloffFactorForPoint(
    float X, float Y,
    const FIslandBowlParams& IslandParams)
{
    const float DX = FMath::Abs(X - IslandParams.CenterX);
    const float DY = FMath::Abs(Y - IslandParams.CenterY);

    float T = 0.0f;
    if (IslandParams.Shape == EIslandShape::Rectangle)
    {
        // Rectangle: use max of normalized axis distances
        float TX = FMath::Max(0.0f, (DX - IslandParams.IslandRadius) / IslandParams.FalloffWidth);
        float TY = FMath::Max(0.0f, (DY - IslandParams.SizeY) / IslandParams.FalloffWidth);
        T = FMath::Clamp(FMath::Max(TX, TY), 0.0f, 1.0f);
    }
    else
    {
        // Circular: use Euclidean distance
        float Distance = FMath::Sqrt(DX * DX + DY * DY);
        T = FMath::Clamp((Distance - IslandParams.IslandRadius) / IslandParams.FalloffWidth, 0.0f, 1.0f);
    }

    // Apply falloff curve (Linear, Smooth, Squared, or Exponential)
    return ApplyFalloffCurve(T, IslandParams.FalloffType);
}
```

### LOD Culling
- **Terrain Bounds Culling**: Chunks above/below terrain height range are skipped (same as Infinite Plane)
- **Island Boundary Culling**:
  - Circular: Chunks beyond `IslandRadius + FalloffWidth` (Euclidean distance) are skipped
  - Rectangle: Chunks beyond `IslandRadius + FalloffWidth` in X OR `IslandSizeY + FalloffWidth` in Y are skipped
- Height range considers `IslandEdgeHeight` for bowl shapes with lowered edges
- Adds chunk buffer to prevent edge popping

### Use Cases
- Floating islands
- Contained game worlds with defined boundaries
- Arena-style environments
- Tutorial areas with natural walls

---

## Mode 3: Spherical Planet

### Overview
Radial terrain on a spherical surface, allowing seamless exploration around an entire planet. Uses cubic chunks with radial density calculation.

### Configuration (UVoxelWorldConfiguration)
```cpp
// World Settings
EWorldMode WorldMode = EWorldMode::SphericalPlanet;
float WorldRadius = 100000.0f;           // Planet base radius

// Planet Parameters
float PlanetMaxTerrainHeight = 5000.0f;  // Mountains above radius
float PlanetMaxTerrainDepth = 2000.0f;   // Valleys below radius
float PlanetHeightScale = 5000.0f;       // Terrain amplitude

// Spawn Configuration
int32 PlanetSpawnLocation = 2;           // 0=+X, 1=+Y, 2=+Z (North Pole), 3=-Z
float PlanetSpawnAltitude = 500.0f;      // Height above surface
```

### Implementation (FSphericalPlanetWorldMode)
```cpp
float FSphericalPlanetWorldMode::GetDensityAt(const FVector& WorldPos,
                                               const FVoxelNoiseParams& NoiseParams) const
{
    // Vector from planet center to sample point
    FVector ToPoint = WorldPos - WorldOrigin;
    float DistFromCenter = ToPoint.Size();

    // Base sphere SDF (positive inside planet)
    float SphereSDF = WorldRadius - DistFromCenter;

    // Sample noise using direction vector (for seamless spherical wrapping)
    FVector Direction = ToPoint.GetSafeNormal();
    float NoiseValue = SampleSphericalFBM(Direction, NoiseParams);

    // Apply terrain displacement
    float TerrainOffset = NoiseValue * PlanetHeightScale;

    return SphereSDF + TerrainOffset;
}
```

### Spawn Position
```cpp
FVector UVoxelWorldConfiguration::GetPlanetSpawnPosition() const
{
    FVector SpawnDirection;
    switch (PlanetSpawnLocation) {
        case 0: SpawnDirection = FVector(1, 0, 0); break;  // +X (Equator)
        case 1: SpawnDirection = FVector(0, 1, 0); break;  // +Y (Equator)
        case 2: SpawnDirection = FVector(0, 0, 1); break;  // +Z (North Pole)
        case 3: SpawnDirection = FVector(0, 0, -1); break; // -Z (South Pole)
    }

    return WorldOrigin + SpawnDirection * (WorldRadius + PlanetSpawnAltitude);
}
```

### LOD Culling
Three types of culling are applied:

1. **Inner Shell Culling**: Chunks closer to center than `WorldRadius - MaxTerrainDepth` (inside planet core)
2. **Outer Shell Culling**: Chunks farther than `WorldRadius + MaxTerrainHeight` (empty space)
3. **Horizon Culling**: Chunks beyond geometric horizon `√(2Rh + h²)` where R = radius, h = viewer altitude

### Use Cases
- Planetary exploration games
- Space games with landable planets
- No Man's Sky style experiences
- Any world requiring true spherical geometry

### Current Limitations
- Designed for surface exploration (terrestrial gameplay)
- Orbital-scale viewing would benefit from octree LOD (future enhancement)
- Gravity/character controller integration not yet implemented

---

## GPU Implementation

All world modes share common GPU shader functions for consistency between CPU and GPU generation:

```hlsl
// WorldModeSDF.ush

float ApplyWorldModeSDF(float3 WorldPos, float BaseDensity, WorldModeParams Params)
{
    if (Params.Mode == WORLD_MODE_INFINITE)
    {
        return BaseDensity;  // No modification needed
    }
    else if (Params.Mode == WORLD_MODE_SPHERICAL)
    {
        float3 ToCenter = WorldPos - Params.WorldOrigin;
        float DistFromCenter = length(ToCenter);
        float SphereSDF = Params.WorldRadius - DistFromCenter;

        // Noise sampled via direction for seamless wrapping
        float3 Dir = normalize(ToCenter);
        float NoiseValue = SampleSphericalNoise(Dir, Params.NoiseParams);

        return SphereSDF + NoiseValue * Params.HeightScale;
    }
    else if (Params.Mode == WORLD_MODE_ISLAND)
    {
        float2 ToCenter = WorldPos.xy - Params.IslandCenter.xy;
        float Dist2D = length(ToCenter);

        float FalloffFactor = 1.0;
        if (Dist2D > Params.IslandRadius)
        {
            float T = saturate((Dist2D - Params.IslandRadius) / Params.FalloffWidth);
            FalloffFactor = ApplyFalloffCurve(T, Params.FalloffType);
        }

        float EdgeDensity = Params.EdgeHeight - WorldPos.z;
        return lerp(EdgeDensity, BaseDensity, FalloffFactor);
    }

    return BaseDensity;
}

float ApplyFalloffCurve(float T, int FalloffType)
{
    switch (FalloffType)
    {
        case 0: return 1.0 - T;                           // Linear
        case 1: return 1.0 - T * T * (3.0 - 2.0 * T);     // Smooth (Hermite)
        case 2: return 1.0 - T * T;                       // Squared
        case 3: return exp(-T * 3.0);                     // Exponential
    }
    return 1.0 - T;
}
```

---

## Switching World Modes

World mode can only be set at initialization time. Changing modes requires reinitializing the voxel world:

```cpp
void AVoxelWorldTestActor::InitializeVoxelWorld()
{
    // Get configuration (mode is set in Configuration asset or defaults)
    UVoxelWorldConfiguration* Config = Configuration;

    // LOD strategy reads mode from config and applies appropriate culling
    LODStrategy->Initialize(Config);

    // For spherical planet, log spawn position helper
    if (Config->WorldMode == EWorldMode::SphericalPlanet)
    {
        FVector SpawnPos = Config->GetPlanetSpawnPosition();
        UE_LOG(LogVoxel, Log, TEXT("Recommended spawn: (%.0f, %.0f, %.0f)"),
            SpawnPos.X, SpawnPos.Y, SpawnPos.Z);
    }
}
```

---

## Performance Comparison

| Mode | Typical Chunk Count | Culling Effectiveness | Use Case |
|------|--------------------|-----------------------|----------|
| Infinite Plane | 200-500 | ~50% (vertical culling) | Large open worlds |
| Island/Bowl | 100-300 | ~70% (boundary culling) | Contained areas |
| Spherical Planet | 200-400 | ~60% (horizon + shell) | Planetary exploration |

All modes benefit from the streaming optimizations:
- O(1) duplicate detection in queues
- Separated load/unload decisions
- Position-based LOD update thresholds

---

## Completed Enhancements

- **Cave Systems**: Multi-layer cave generation (Cheese caverns, Spaghetti tunnels, Noodle passages) via `UVoxelCaveConfiguration` data asset. Density subtraction model works across all 3 world modes. See `Documentation/CAVE_SYSTEM.md`.

## Future Enhancements

- **Octree LOD**: For orbital-scale spherical planet viewing
- **Water/Ocean**: Sea level handling with underwater terrain
- **Biome Transitions**: Smooth material blending between biomes

---

**Status**: All three world modes implemented and tested with mode-specific LOD culling
