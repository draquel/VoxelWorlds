# Core Data Structures Reference

Complete reference for all voxel plugin data structures.

## Blueprint Type Compatibility

**Important**: Unreal Blueprint does not support unsigned integer types larger than uint8:
- `uint8` → Supported as "Byte" type in Blueprints
- `uint16`, `uint32`, `uint64` → **Not supported**, use signed equivalents
- For frame counters and seeds, use `int32` or `int64` instead

## Core Voxel Data

### FVoxelData (4 bytes)
```cpp
struct FVoxelData {
    uint8 MaterialID;   // 0-255 material types
    uint8 Density;      // 0-255 (127 = surface, <127 = air, >127 = solid)
    uint8 BiomeID;      // 0-255 biome types  
    uint8 Metadata;     // AO (4 bits) + flags (4 bits)
};
```

### FChunkDescriptor
```cpp
struct FChunkDescriptor {
    FIntVector ChunkCoord;      // Chunk position in chunk space
    int32 LODLevel;             // 0 = finest
    int32 ChunkSize;            // Voxels per edge (32, 64, etc.)
    TArray<FVoxelData> VoxelData;
    FBox Bounds;
    bool bIsDirty;
    bool bHasEdits;
    float MorphFactor;
    int32 GenerationSeed;       // int32 for Blueprint compatibility
    EChunkState State;          // Streaming lifecycle state
};
```

## Rendering Data

### FVoxelVertex (28 bytes - Custom VF)
```cpp
struct FVoxelVertex {
    FVector3f Position;          // 12 bytes
    uint32 PackedNormalAndAO;    // 4 bytes
    FVector2f UV;                // 8 bytes  
    uint32 PackedMaterialData;   // 4 bytes
};
```

### FChunkRenderData
```cpp
struct FChunkRenderData {
    FIntVector ChunkCoord;
    int32 LODLevel;
    FRDGBufferRef ComputeVertexBuffer;
    FRDGBufferRef ComputeIndexBuffer;
    uint32 VertexCount;
    uint32 IndexCount;
    bool bNeedsCollisionUpdate;
    FBox Bounds;
};
```

## LOD System Data

### FLODQueryContext
```cpp
struct FLODQueryContext {
    // Camera/Viewer State
    FVector ViewerPosition;
    FVector ViewerForward;
    FVector ViewerRight;
    FVector ViewerUp;
    float ViewDistance;
    float FieldOfView;
    float AspectRatio;
    TArray<FPlane> FrustumPlanes;

    // World State
    FVector WorldOrigin;
    EWorldMode WorldMode;
    float WorldRadius;

    // Performance Budgets
    int32 MaxChunksToLoadPerFrame;
    int32 MaxChunksToUnloadPerFrame;
    float TimeSliceMS;

    // Frame Information
    int64 FrameNumber;          // int64 for Blueprint compatibility
    float GameTime;
    float DeltaTime;
};
```

### FChunkLODRequest
```cpp
struct FChunkLODRequest {
    FIntVector ChunkCoord;
    int32 LODLevel;
    float Priority;
    float MorphFactor;
};
```

## Configuration Data

### FLODBand (Distance Band Strategy)
```cpp
struct FLODBand {
    float MinDistance;
    float MaxDistance;
    int32 LODLevel;
    int32 VoxelStride;
    int32 ChunkSize;
    float MorphRange;
};
```

## Biome System Data

### FBiomeDefinition
```cpp
struct FBiomeDefinition {
    FString BiomeName;
    FVector2D TemperatureRange;
    FVector2D MoistureRange;
    FVector2D ElevationRange;
    TArray<FNoiseLayer> NoiseLayers;
    float HeightMultiplier;
    TMap<EVoxelFace, int32> SurfaceMaterials;
};
```

## Material System Data

### FVoxelMaterialDefinition
```cpp
struct FVoxelMaterialDefinition {
    int32 MaterialID;
    FString MaterialName;
    FVector2D TopUV;
    FVector2D BottomUV;
    FVector2D SideUV;
    float Hardness;
    bool bEmissive;
};
```

## Edit Layer Data

### FVoxelEdit
```cpp
struct FVoxelEdit {
    FIntVector LocalPos;
    FVoxelData NewData;
    EEditMode Mode;      // Set, Add, Subtract, Paint
    float Strength;
    float Radius;
};
```

## Enums

### EWorldMode
```cpp
enum class EWorldMode : uint8 {
    InfinitePlane,
    SphericalPlanet,
    IslandBowl
};
```

### EMeshingMode
```cpp
enum class EMeshingMode : uint8 {
    Cubic,    // Block-style
    Smooth    // Marching cubes
};
```

### EEditMode
```cpp
enum class EEditMode : uint8 {
    Set,
    Add,
    Subtract,
    Paint,
    Smooth
};
```

## Memory Layouts

### Per-Chunk Memory (LOD 0, 32³)
- VoxelData: 32,768 voxels × 4 bytes = 128 KB
- Vertex Data (avg): ~50 KB (cubic) / ~100 KB (smooth)
- Index Data (avg): ~30 KB
- **Total: ~200 KB per chunk**

### Target Memory Budget
- 1000 visible chunks = 200 MB
- 2000 visible chunks (max) = 400 MB

