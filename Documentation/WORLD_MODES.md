# World Modes

**Module**: VoxelGeneration  
**Dependencies**: VoxelCore

## Overview

The world mode system supports three different terrain types, each with specific characteristics and optimizations.

## IVoxelWorldMode Interface

```cpp
class IVoxelWorldMode {
public:
    virtual ~IVoxelWorldMode() = default;
    
    // SDF - Signed Distance Field evaluation
    virtual float GetDensityAt(FVector WorldPos, int32 LOD) const = 0;
    
    // Coordinate transformations
    virtual FVector GetWorldSpacePosition(FIntVector ChunkCoord, FIntVector LocalVoxel) const = 0;
    virtual FIntVector WorldToChunkCoord(FVector WorldPos, int32 ChunkSize) const = 0;
    
    // Visibility queries
    virtual TArray<FIntVector> GetVisibleChunks(FVector ViewOrigin, float ViewDistance) const = 0;
    
    // LOD adaptation
    virtual int32 GetMinZ(const FLODQueryContext& Context) const = 0;
    virtual int32 GetMaxZ(const FLODQueryContext& Context) const = 0;
};
```

---

## Mode 1: Infinite Plane

### Overview
Standard heightmap-style terrain extending infinitely in X and Y directions.

### Configuration
```cpp
struct FInfinitePlaneConfig {
    float SeaLevel = 0.0f;
    float MaxHeight = 1000.0f;
    float MinHeight = -1000.0f;
};
```

### Implementation
```cpp
class FInfinitePlaneWorldMode : public IVoxelWorldMode {
public:
    float GetDensityAt(FVector WorldPos, int32 LOD) const override {
        // Sample noise for height
        float Height = SampleTerrainNoise(WorldPos.X, WorldPos.Y);
        
        // Simple density: positive below surface
        return Height - WorldPos.Z;
    }
    
    FVector GetWorldSpacePosition(FIntVector ChunkCoord, FIntVector LocalVoxel) const override {
        return FVector(
            ChunkCoord.X * ChunkSize + LocalVoxel.X,
            ChunkCoord.Y * ChunkSize + LocalVoxel.Y,
            ChunkCoord.Z * ChunkSize + LocalVoxel.Z
        ) * VoxelSize;
    }
    
    int32 GetMinZ(const FLODQueryContext& Context) const override {
        return FMath::FloorToInt(Config.MinHeight / (ChunkSize * VoxelSize));
    }
    
    int32 GetMaxZ(const FLODQueryContext& Context) const override {
        return FMath::CeilToInt(Config.MaxHeight / (ChunkSize * VoxelSize));
    }
};
```

### LOD Strategy
- **Recommended**: Distance bands
- Z-range limited by min/max height
- Simple, predictable performance

---

## Mode 2: Spherical Planet

### Overview
Planet with curved surface, allowing seamless exploration around entire sphere.

### Configuration
```cpp
struct FSphericalPlanetConfig {
    float PlanetRadius = 6400000.0f;  // Earth-like
    float AtmosphereHeight = 10000.0f;
    float CoreDepth = 100000.0f;
    FVector PlanetCenter = FVector::ZeroVector;
};
```

### Implementation
```cpp
class FSphericalPlanetWorldMode : public IVoxelWorldMode {
public:
    float GetDensityAt(FVector WorldPos, int32 LOD) const override {
        FVector ToPlanetCenter = WorldPos - Config.PlanetCenter;
        float DistFromCenter = ToPlanetCenter.Size();
        
        // Base sphere SDF
        float SphereSDF = Config.PlanetRadius - DistFromCenter;
        
        // Sample noise on sphere surface
        FVector SurfaceDir = ToPlanetCenter.GetSafeNormal();
        float NoiseValue = SampleSphericalNoise(SurfaceDir);
        
        // Combine
        return SphereSDF + NoiseValue;
    }
    
    FVector GetWorldSpacePosition(FIntVector ChunkCoord, FIntVector LocalVoxel) const override {
        // Chunks positioned radially from planet center
        FVector LocalPos = FVector(LocalVoxel) * VoxelSize;
        FVector ChunkOffset = FVector(ChunkCoord) * ChunkSize * VoxelSize;
        
        // Could use spherical coordinate mapping here
        return Config.PlanetCenter + ChunkOffset + LocalPos;
    }
    
    TArray<FIntVector> GetVisibleChunks(FVector ViewOrigin, float ViewDistance) const override {
        // Only load chunks on visible hemisphere
        FVector ToViewer = (ViewOrigin - Config.PlanetCenter).GetSafeNormal();
        
        // Generate chunks on visible side
        // Implementation depends on chunk coordinate scheme
    }
};
```

### Special Considerations

**Curvature in Meshing**:
```hlsl
// Vertex shader adjusts for planetary curvature
float3 ApplyCurvature(float3 LocalPos, float3 ChunkCenter, float PlanetRadius) {
    float3 ToPlanetCenter = ChunkCenter - PlanetCenter;
    float3 RadialDir = normalize(ToPlanetCenter);
    
    // Project vertex onto sphere
    float DistFromCenter = length(ToPlanetCenter + LocalPos);
    return PlanetCenter + RadialDir * DistFromCenter;
}
```

**LOD Strategy**:
- Distance bands on surface
- Coarser LOD for interior (core)
- Hybrid with octree for deep structures

---

## Mode 3: Island/Bowl

### Overview
Bounded world with smooth falloff at edges, creating island or mountain bowl effect.

### Configuration
```cpp
struct FIslandBowlConfig {
    FVector IslandCenter = FVector::ZeroVector;
    float IslandRadius = 10000.0f;
    float FalloffDistance = 2000.0f;
    float OceanLevel = 0.0f;
    EFalloffType FalloffType = EFalloffType::Radial;
};

enum class EFalloffType : uint8 {
    Radial,     // Circular falloff
    Square,     // Square falloff
    Custom      // Custom falloff curve
};
```

### Implementation
```cpp
class FIslandBowlWorldMode : public IVoxelWorldMode {
public:
    float GetDensityAt(FVector WorldPos, int32 LOD) const override {
        // Base terrain density
        float TerrainDensity = SampleTerrainNoise(WorldPos.X, WorldPos.Y) - WorldPos.Z;
        
        // Calculate falloff
        FVector2D ToCenter(WorldPos.X - Config.IslandCenter.X, WorldPos.Y - Config.IslandCenter.Y);
        float DistFromCenter = ToCenter.Size();
        
        // Smooth falloff
        float FalloffStart = Config.IslandRadius - Config.FalloffDistance;
        float FalloffFactor = 1.0f;
        
        if (DistFromCenter > FalloffStart) {
            float FalloffT = (DistFromCenter - FalloffStart) / Config.FalloffDistance;
            FalloffFactor = 1.0f - FMath::SmoothStep(0.0f, 1.0f, FalloffT);
        }
        
        // Apply falloff (push terrain down at edges)
        float HeightFalloff = FalloffFactor * (Config.OceanLevel - WorldPos.Z);
        
        return FMath::Min(TerrainDensity, HeightFalloff);
    }
    
    TArray<FIntVector> GetVisibleChunks(FVector ViewOrigin, float ViewDistance) const override {
        // Only generate chunks within island bounds
        TArray<FIntVector> Chunks;
        
        float MaxDist = Config.IslandRadius + ViewDistance;
        // ... generate chunks within radius
        
        return Chunks;
    }
};
```

### LOD Strategy
- Distance bands centered on player
- Early culling beyond island radius
- Good for bounded, exploreable spaces

---

## GPU Implementation

```hlsl
// WorldMode.usf

float ApplyWorldModeSDF(float3 WorldPos, float BaseDensity, WorldModeParams Params) {
    if (Params.Mode == WORLD_MODE_INFINITE) {
        return BaseDensity; // No modification
    }
    else if (Params.Mode == WORLD_MODE_SPHERICAL) {
        float3 ToPlanetCenter = WorldPos - Params.PlanetCenter;
        float DistFromCenter = length(ToPlanetCenter);
        float SphereSDF = Params.PlanetRadius - DistFromCenter;
        return min(BaseDensity, SphereSDF);
    }
    else if (Params.Mode == WORLD_MODE_ISLAND) {
        float2 ToIslandCenter = WorldPos.xy - Params.IslandCenter.xy;
        float DistFromCenter = length(ToIslandCenter);
        
        float FalloffStart = Params.IslandRadius - Params.FalloffDistance;
        float FalloffFactor = 1.0;
        
        if (DistFromCenter > FalloffStart) {
            float T = (DistFromCenter - FalloffStart) / Params.FalloffDistance;
            FalloffFactor = 1.0 - smoothstep(0.0, 1.0, T);
        }
        
        float HeightFalloff = FalloffFactor * (Params.OceanLevel - WorldPos.z);
        return min(BaseDensity, HeightFalloff);
    }
    
    return BaseDensity;
}
```

---

## Switching World Modes

```cpp
void UVoxelChunkManager::SetWorldMode(TSharedPtr<IVoxelWorldMode> NewMode) {
    // Clear existing chunks
    UnloadAllChunks();
    
    // Set new mode
    WorldMode = NewMode;
    
    // Reinitialize LOD strategy with new mode
    LODStrategy->Initialize(WorldConfig);
    
    // Trigger regeneration
    InvalidateVisibleChunks();
}
```

