# Material System

**Module**: VoxelCore
**Dependencies**: Engine, RenderCore

## Overview

The material system manages 256 different material types with texture atlases for cubic meshing and Texture2DArrays with triplanar projection for smooth meshing. The system is centered around the `UVoxelMaterialAtlas` data asset which provides both runtime texture management and editor configuration.

## Implementation Status: COMPLETE

The material system has been fully implemented with:
- UVoxelMaterialAtlas data asset for material configuration
- Auto-generated Texture2DArrays from source textures
- MaterialID encoding via UV1 channel (sRGB-safe)
- Triplanar sampling with UDN normal blending
- Runtime material binding via DynamicMaterialInstance

## UVoxelMaterialAtlas (Data Asset)

The primary interface for material configuration. Create this asset in the Content Browser and assign it to your VoxelWorldComponent.

### FVoxelMaterialTextureConfig

```cpp
/**
 * Configuration for a single material in the atlas.
 * Maps MaterialID to atlas position and source textures.
 * Supports per-face texture variants (top/side/bottom).
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FVoxelMaterialTextureConfig
{
    GENERATED_BODY()

    /** Material ID this config applies to (0-255) */
    uint8 MaterialID = 0;

    /** Display name for this material */
    FString MaterialName;

    // ===== Face Variants =====

    /** Enable different textures for top/side/bottom faces */
    bool bUseFaceVariants = false;

    /** Atlas tiles for Top/Side/Bottom faces (when bUseFaceVariants = true) */
    FVoxelAtlasTile TopTile, SideTile, BottomTile;

    // ===== Default Atlas Position (when not using face variants) =====

    int32 AtlasColumn = 0;
    int32 AtlasRow = 0;

    // ===== Source Textures for Texture2DArray (Smooth Terrain) =====

    /** Albedo/BaseColor texture for this material */
    TSoftObjectPtr<UTexture2D> AlbedoTexture;

    /** Normal map texture for this material */
    TSoftObjectPtr<UTexture2D> NormalTexture;

    /** Roughness texture for this material (R channel) */
    TSoftObjectPtr<UTexture2D> RoughnessTexture;

    // ===== Per-Material Properties =====

    /** Scale for triplanar projection (smooth terrain). Higher = more tiling. */
    float TriplanarScale = 1.0f;

    /** UV scale multiplier for packed atlas sampling */
    float UVScale = 1.0f;
};
```

### UVoxelMaterialAtlas

```cpp
/**
 * Data asset defining the material atlas configuration for voxel terrain.
 *
 * Supports two rendering modes:
 * - Cubic Terrain: Uses packed texture atlases with UV-based sampling
 * - Smooth Terrain: Uses Texture2DArrays with triplanar projection
 */
UCLASS(BlueprintType)
class VOXELCORE_API UVoxelMaterialAtlas : public UDataAsset
{
    GENERATED_BODY()

public:
    // ===== Packed Atlas (Cubic Terrain) =====

    UTexture2D* PackedAlbedoAtlas;
    UTexture2D* PackedNormalAtlas;
    UTexture2D* PackedRoughnessAtlas;
    int32 AtlasColumns = 4;
    int32 AtlasRows = 4;

    // ===== Texture Arrays (Smooth Terrain) - Auto-generated =====

    /** Auto-built from MaterialConfigs source textures */
    UTexture2DArray* AlbedoArray;    // VisibleAnywhere - runtime generated
    UTexture2DArray* NormalArray;
    UTexture2DArray* RoughnessArray;
    int32 TextureArraySize = 512;    // Target size for array slices

    // ===== Per-Material Configuration =====

    TArray<FVoxelMaterialTextureConfig> MaterialConfigs;

    // ===== Key API Functions =====

    /** Build Texture2DArrays from source textures in MaterialConfigs */
    UFUNCTION(BlueprintCallable, CallInEditor)
    void BuildTextureArrays();

    /** Build Material Lookup Table for face-variant atlas positions */
    UFUNCTION(BlueprintCallable, CallInEditor)
    void BuildMaterialLUT();

    /** Check if arrays need rebuilding */
    bool AreTextureArraysDirty() const;
};
```

---

## Shader Utilities: VoxelTriplanarCommon.ush

The `Shaders/Private/VoxelTriplanarCommon.ush` file provides reusable triplanar mapping functions:

```hlsl
// Calculate triplanar blend weights from world normal
float3 GetTriplanarBlendWeights(float3 WorldNormal, float Sharpness)

// Calculate triplanar UVs from world position
void GetTriplanarUVs(float3 WorldPosition, float Scale,
                     out float2 UV_X, out float2 UV_Y, out float2 UV_Z)

// Apply sign correction for consistent texture orientation
void CorrectTriplanarUVOrientation(float3 WorldNormal,
                                   inout float2 UV_X, inout float2 UV_Y, inout float2 UV_Z)

// Extract MaterialID from UV1 (smooth terrain)
int GetSmoothMaterialID(float2 UV1)

// Sample texture using triplanar projection
float4 SampleTriplanar(Texture2D Tex, SamplerState Samp,
                       float3 WorldPosition, float3 WorldNormal,
                       float Scale, float Sharpness)

// Sample Texture2DArray using triplanar projection
float4 SampleTriplanarArray(Texture2DArray TexArray, SamplerState Samp,
                            float3 WorldPosition, float3 WorldNormal,
                            int ArrayIndex, float Scale, float Sharpness)

// Debug visualization helpers
float3 DebugTriplanarWeights(float3 WorldNormal, float Sharpness)
float3 DebugMaterialColor(int MaterialID)
```

---

## Cubic Mode: Texture Atlas

### Atlas Layout

For cubic meshing, all material textures are packed into a single atlas:

```
Atlas: 2048x2048 pixels
Tile size: 64x64 pixels
Tiles per row: 32
Max materials: 32 × 32 = 1024 tiles (256 materials × 3 faces + extras)

Layout:
┌────┬────┬────┬────┬───
│ 0T │ 0B │ 0S │ 1T │...
├────┼────┼────┼────┼───
│ 1B │ 1S │ 2T │ 2B │...
└────┴────┴────┴────┴───

Material 0: Top, Bottom, Side
Material 1: Top, Bottom, Side
...
```

### Building the Atlas

```cpp
void UVoxelMaterialRegistry::BuildAtlasTexture()
{
    // Create atlas texture
    AtlasTexture = UTexture2D::CreateTransient(AtlasSize, AtlasSize, PF_B8G8R8A8);
    
    // Get mip data
    FTexture2DMipMap& Mip = AtlasTexture->GetPlatformData()->Mips[0];
    void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
    FColor* PixelData = static_cast<FColor*>(Data);
    
    int32 TilesPerRow = AtlasSize / TileSize;
    
    // Pack each material
    for (auto& Pair : Materials)
    {
        uint8 MaterialID = Pair.Key;
        FVoxelMaterialDefinition& Mat = Pair.Value;
        
        // Calculate tile positions
        int32 TileIndex = MaterialID * 3; // 3 tiles per material
        int32 TopTileX = (TileIndex + 0) % TilesPerRow;
        int32 TopTileY = (TileIndex + 0) / TilesPerRow;
        int32 BottomTileX = (TileIndex + 1) % TilesPerRow;
        int32 BottomTileY = (TileIndex + 1) / TilesPerRow;
        int32 SideTileX = (TileIndex + 2) % TilesPerRow;
        int32 SideTileY = (TileIndex + 2) / TilesPerRow;
        
        // Copy texture data to atlas
        CopyTextureToAtlas(Mat.AlbedoTexture, PixelData, TopTileX, TopTileY);
        CopyTextureToAtlas(Mat.AlbedoTexture, PixelData, BottomTileX, BottomTileY);
        CopyTextureToAtlas(Mat.AlbedoTexture, PixelData, SideTileX, SideTileY);
        
        // Store UV coordinates
        Mat.TopUV = FVector2D(
            TopTileX * TileSize / (float)AtlasSize,
            TopTileY * TileSize / (float)AtlasSize
        );
        Mat.BottomUV = FVector2D(
            BottomTileX * TileSize / (float)AtlasSize,
            BottomTileY * TileSize / (float)AtlasSize
        );
        Mat.SideUV = FVector2D(
            SideTileX * TileSize / (float)AtlasSize,
            SideTileY * TileSize / (float)AtlasSize
        );
    }
    
    Mip.BulkData.Unlock();
    AtlasTexture->UpdateResource();
}
```

### Getting Atlas UVs in Shaders

```hlsl
// Get UV coordinates for a material face
float2 GetAtlasUV(uint MaterialID, uint Face, float2 LocalUV)
{
    // Each material has 3 tiles: Top, Bottom, Side
    uint TileIndex = MaterialID * 3 + Face;
    
    uint TilesPerRow = AtlasSize / TileSize;
    uint TileX = TileIndex % TilesPerRow;
    uint TileY = TileIndex / TilesPerRow;
    
    // Base UV of tile
    float2 TileBaseUV = float2(TileX, TileY) * TileSize / (float)AtlasSize;
    
    // Offset by local UV
    float2 TileUV = LocalUV * TileSize / (float)AtlasSize;
    
    return TileBaseUV + TileUV;
}
```

---

## Smooth Mode: Triplanar Projection

For smooth meshing (Marching Cubes), materials use triplanar projection to avoid UV seams. The implementation uses Custom HLSL nodes in the Unreal Material Editor.

### MaterialID Encoding

MaterialID is stored in the **UV1.x** channel as a float value. This avoids sRGB gamma conversion issues that would corrupt integer data in vertex colors.

**In Mesh Generator (FVoxelCPUSmoothMesher):**
```cpp
// Store MaterialID in UV1.x
Vertex.UV1.X = static_cast<float>(MaterialID);
Vertex.UV1.Y = 0.0f;  // Reserved
```

**In Shader:**
```hlsl
int MaterialID = (int)floor(MaterialUV.x + 0.5);
```

### Triplanar Albedo/Roughness Sampling (Custom Node)

This custom HLSL node samples both albedo and roughness arrays:

**Inputs:**
- `WorldPos` (float3) - Absolute World Position
- `WorldNormal` (float3) - VertexNormalWS
- `MaterialUV` (float2) - TexCoord[1]
- `Scale` (float) - Triplanar tiling scale (e.g., 0.01)
- `Sharpness` (float) - Blend sharpness (e.g., 4.0)
- `AlbedoArray` (Texture2DArray) - Material atlas parameter
- `RoughnessArray` (Texture2DArray) - Material atlas parameter

**Output:** `float4` (RGB = Albedo, A = Roughness)

```hlsl
int MaterialID = (int)floor(MaterialUV.x + 0.5);

// Calculate triplanar blend weights
float3 Weights = pow(abs(WorldNormal), Sharpness);
Weights /= max(Weights.x + Weights.y + Weights.z, 0.0001);

// Calculate triplanar UVs
float2 UV_X = WorldPos.zy * Scale;  // YZ plane
float2 UV_Y = WorldPos.xz * Scale;  // XZ plane (top/bottom)
float2 UV_Z = WorldPos.xy * Scale;  // XY plane

// Use SampleLevel for ray tracing compatibility
SamplerState Samp = View.MaterialTextureBilinearWrapedSampler;

// Sample albedo from each projection
float4 Albedo_X = AlbedoArray.SampleLevel(Samp, float3(UV_X, MaterialID), 0);
float4 Albedo_Y = AlbedoArray.SampleLevel(Samp, float3(UV_Y, MaterialID), 0);
float4 Albedo_Z = AlbedoArray.SampleLevel(Samp, float3(UV_Z, MaterialID), 0);
float3 Albedo = Albedo_X.rgb * Weights.x + Albedo_Y.rgb * Weights.y + Albedo_Z.rgb * Weights.z;

// Sample roughness
float Rough_X = RoughnessArray.SampleLevel(Samp, float3(UV_X, MaterialID), 0).r;
float Rough_Y = RoughnessArray.SampleLevel(Samp, float3(UV_Y, MaterialID), 0).r;
float Rough_Z = RoughnessArray.SampleLevel(Samp, float3(UV_Z, MaterialID), 0).r;
float Roughness = Rough_X * Weights.x + Rough_Y * Weights.y + Rough_Z * Weights.z;

return float4(Albedo, Roughness);
```

### Triplanar Normal Sampling with UDN Blend (Custom Node)

Normal maps require special blending to avoid "plaid" artifacts. This uses UDN (Unreal Developer Network) blend:

**Inputs:**
- `WorldPos` (float3) - Absolute World Position
- `WorldNormal` (float3) - VertexNormalWS (NOT PixelNormalWS)
- `MaterialUV` (float2) - TexCoord[1]
- `Scale` (float) - Triplanar tiling scale
- `Sharpness` (float) - Blend sharpness
- `NormalArray` (Texture2DArray) - Material atlas parameter

**Output:** `float3` (World-space normal)

```hlsl
int MaterialID = (int)floor(MaterialUV.x + 0.5);

float3 Weights = pow(abs(WorldNormal), Sharpness);
Weights /= max(Weights.x + Weights.y + Weights.z, 0.0001);

float2 UV_X = WorldPos.zy * Scale;
float2 UV_Y = WorldPos.xz * Scale;
float2 UV_Z = WorldPos.xy * Scale;

SamplerState Samp = View.MaterialTextureBilinearWrapedSampler;

// Decode tangent-space normals
float3 TN_X = NormalArray.SampleLevel(Samp, float3(UV_X, MaterialID), 0).rgb * 2.0 - 1.0;
float3 TN_Y = NormalArray.SampleLevel(Samp, float3(UV_Y, MaterialID), 0).rgb * 2.0 - 1.0;
float3 TN_Z = NormalArray.SampleLevel(Samp, float3(UV_Z, MaterialID), 0).rgb * 2.0 - 1.0;

// UDN blend: Project tangent-space normals to world-space derivatives
float3 N_X = float3(0, TN_X.y, -TN_X.x);        // X-axis projection
float3 N_Y = float3(TN_Y.x, 0, -TN_Y.y);        // Y-axis projection
float3 N_Z = float3(TN_Z.xy, 0);                 // Z-axis projection

// Blend and add to base normal
float3 BlendedNormal = normalize(N_X * Weights.x + N_Y * Weights.y + N_Z * Weights.z + WorldNormal);

return BlendedNormal;
```

**Important Notes:**
- Use `VertexNormalWS` as input, NOT `PixelNormalWS` (creates circular dependency)
- Disable "Tangent Space Normal" in material settings
- Use `SampleLevel()` instead of `Sample()` for ray tracing compatibility

---

## Material Assignment

### By Biome

Materials are typically assigned based on biome:

```cpp
uint8 GetMaterialForBiome(uint8 BiomeID, float Density, FVector WorldPos)
{
    const FBiomeDefinition& Biome = BiomeRegistry->GetBiome(BiomeID);
    
    // Surface material
    if (Density > 120 && Density < 135)
    {
        return Biome.SurfaceMaterial;
    }
    
    // Subsurface (by depth)
    float DepthBelowSurface = (Density - 127) * VoxelSize;
    
    if (DepthBelowSurface < 200.0f)
    {
        return Biome.ShallowMaterial; // Topsoil
    }
    else if (DepthBelowSurface < 1000.0f)
    {
        return Biome.MediumMaterial; // Stone
    }
    else
    {
        return Biome.DeepMaterial; // Bedrock
    }
}
```

### By Height

Materials can also vary by altitude:

```cpp
uint8 GetMaterialByHeight(float Elevation)
{
    if (Elevation > 2000.0f)
    {
        return MaterialID_Snow;
    }
    else if (Elevation > 1500.0f)
    {
        return MaterialID_Rock;
    }
    else if (Elevation > 500.0f)
    {
        return MaterialID_Grass;
    }
    else if (Elevation > 0.0f)
    {
        return MaterialID_Sand;
    }
    else
    {
        return MaterialID_Water;
    }
}
```

---

## Example Material Definitions

### Grass Material

```cpp
FVoxelMaterialDefinition GrassMaterial;
GrassMaterial.MaterialID = 1;
GrassMaterial.MaterialName = TEXT("Grass");
GrassMaterial.TopUV = FVector2D(0.0f, 0.0f);
GrassMaterial.BottomUV = FVector2D(0.0625f, 0.0f);
GrassMaterial.SideUV = FVector2D(0.125f, 0.0f);
GrassMaterial.Hardness = 0.5f;
GrassMaterial.bEmissive = false;
GrassMaterial.TriplanarScale = 0.1f;
```

### Stone Material

```cpp
FVoxelMaterialDefinition StoneMaterial;
StoneMaterial.MaterialID = 2;
StoneMaterial.MaterialName = TEXT("Stone");
StoneMaterial.TopUV = FVector2D(0.1875f, 0.0f);
StoneMaterial.BottomUV = FVector2D(0.1875f, 0.0f);
StoneMaterial.SideUV = FVector2D(0.1875f, 0.0f);
StoneMaterial.Hardness = 3.0f;
StoneMaterial.bEmissive = false;
StoneMaterial.TriplanarScale = 0.05f;
```

### Lava Material (Emissive)

```cpp
FVoxelMaterialDefinition LavaMaterial;
LavaMaterial.MaterialID = 10;
LavaMaterial.MaterialName = TEXT("Lava");
LavaMaterial.TopUV = FVector2D(0.25f, 0.25f);
LavaMaterial.BottomUV = FVector2D(0.25f, 0.25f);
LavaMaterial.SideUV = FVector2D(0.25f, 0.25f);
LavaMaterial.Hardness = 100.0f; // Unbreakable
LavaMaterial.bEmissive = true;
LavaMaterial.EmissiveColor = FLinearColor(1.0f, 0.3f, 0.0f, 5.0f);
LavaMaterial.TriplanarScale = 0.1f;
```

---

## Data Asset Configuration

```cpp
/**
 * Material Registry Data Asset
 * 
 * Designer-friendly configuration for materials.
 */
UCLASS()
class UVoxelMaterialRegistryAsset : public UDataAsset
{
    GENERATED_BODY()
    
public:
    /** Material definitions */
    UPROPERTY(EditAnywhere, Category = "Materials")
    TArray<FVoxelMaterialDefinition> Materials;
    
    /** Atlas configuration */
    UPROPERTY(EditAnywhere, Category = "Atlas")
    int32 AtlasSize = 2048;
    
    UPROPERTY(EditAnywhere, Category = "Atlas")
    int32 TileSize = 64;
    
    /** Apply to registry */
    void ApplyToRegistry(UVoxelMaterialRegistry* Registry);
};
```

---

## Runtime Material Binding

The material system uses Dynamic Material Instances to bind texture arrays at runtime.

### UVoxelWorldComponent::UpdateMaterialAtlasParameters

```cpp
void UVoxelWorldComponent::UpdateMaterialAtlasParameters(UMaterialInstanceDynamic* DynamicMaterial)
{
    if (!MaterialAtlas)
    {
        UE_LOG(LogVoxelRendering, Warning, TEXT("No MaterialAtlas, skipping atlas setup"));
        return;
    }

    // Build LUT if needed
    if (MaterialAtlas->IsLUTDirty() || !MaterialAtlas->GetMaterialLUT())
    {
        MaterialAtlas->BuildMaterialLUT();
    }

    // Build texture arrays if needed
    if (MaterialAtlas->AreTextureArraysDirty() || !MaterialAtlas->AlbedoArray)
    {
        MaterialAtlas->BuildTextureArrays();
    }

    // Set texture array parameters
    if (MaterialAtlas->AlbedoArray)
    {
        DynamicMaterial->SetTextureParameterValue(TEXT("AlbedoArray"), MaterialAtlas->AlbedoArray);
    }
    if (MaterialAtlas->NormalArray)
    {
        DynamicMaterial->SetTextureParameterValue(TEXT("NormalArray"), MaterialAtlas->NormalArray);
    }
    if (MaterialAtlas->RoughnessArray)
    {
        DynamicMaterial->SetTextureParameterValue(TEXT("RoughnessArray"), MaterialAtlas->RoughnessArray);
    }

    // Set atlas dimensions
    DynamicMaterial->SetScalarParameterValue(TEXT("AtlasColumns"), MaterialAtlas->AtlasColumns);
    DynamicMaterial->SetScalarParameterValue(TEXT("AtlasRows"), MaterialAtlas->AtlasRows);
}
```

### Material Setup in Material Editor

1. Create **TextureObjectParameter** nodes for:
   - `AlbedoArray` (Texture2DArray)
   - `NormalArray` (Texture2DArray)
   - `RoughnessArray` (Texture2DArray)

2. Assign a **placeholder Texture2DArray** to each parameter (required for material compilation)

3. Create Custom nodes for triplanar sampling (see shader code above)

4. Connect outputs:
   - Albedo RGB → Base Color
   - Roughness A → Roughness
   - BlendedNormal → World Space Normal (disable Tangent Space Normal)

---

## Performance Considerations

### Atlas vs Individual Textures

**Atlas (Cubic)**:
- ✅ Single texture bind
- ✅ Better batching
- ✅ Lower draw calls
- ❌ Limited resolution per material
- ❌ Atlas rebuilding overhead

**Individual Textures (Smooth)**:
- ✅ High resolution per material
- ✅ Easy to update
- ❌ More texture binds
- ❌ Worse batching

### Memory Usage

**Atlas**: 2048×2048 × 4 bytes (RGBA) = 16 MB
**Individual Textures**: 256 materials × 1024×1024 × 4 × 3 (albedo/normal/RM) = 3 GB

**Recommendation**: Use atlas for cubic, individual textures for smooth (with texture arrays).

---

## Unified Master Material (M_VoxelMaster)

The material system uses a unified master material that automatically switches between cubic and smooth rendering modes based on `UVoxelWorldConfiguration::MeshingMode`.

### Automatic Mode Selection

The material mode is synced automatically in `FVoxelCustomVFRenderer::Initialize()`:

```cpp
// Sync material mode with configuration's meshing mode
const bool bIsSmooth = (WorldConfig->MeshingMode == EMeshingMode::Smooth);
WorldComponent->SetUseSmoothMeshing(bIsSmooth);
```

This sets the `bSmoothTerrain` material parameter which controls the Lerp nodes in M_VoxelMaster.

### Material Functions

The master material uses 4 reusable material functions:

1. **MF_GetMaterialID** - Extracts MaterialID from UV1.x
2. **MF_TriplanarSampleAlbedoRoughness** - Smooth terrain color/roughness sampling
3. **MF_TriplanarSampleNormal** - Smooth terrain normals with UDN blend
4. **MF_CubicAtlasSample** - Cubic terrain sampling via UV0 + MaterialLUT

### Material Assets

| Asset | Location | Purpose |
|-------|----------|---------|
| M_VoxelMaster | Content/VoxelWorlds/Materials/ | Master material with both paths |
| MI_VoxelDefault | Content/VoxelWorlds/Materials/ | Default material instance |
| MF_* | Content/VoxelWorlds/Materials/Functions/ | Reusable material functions |

See [MASTER_MATERIAL_SETUP.md](MASTER_MATERIAL_SETUP.md) for detailed setup instructions.

---

## Implementation Complete

The material system is fully implemented with:

1. ✅ `UVoxelMaterialAtlas` data asset with per-material configuration
2. ✅ Auto-generated Texture2DArrays from source textures
3. ✅ MaterialID encoding via UV1 channel (sRGB-safe)
4. ✅ Triplanar sampling for smooth terrain (UDN normal blending)
5. ✅ UV-based atlas sampling for cubic terrain (MaterialLUT lookup)
6. ✅ Unified M_VoxelMaster material with automatic mode switching
7. ✅ 4 reusable Material Functions
8. ✅ Runtime binding via DynamicMaterialInstance
9. ✅ Automatic mode sync from UVoxelWorldConfiguration::MeshingMode
10. ✅ VoxelTriplanarCommon.ush shader utilities

### Key Files

**C++ Code:**
- `VoxelCore/Public/VoxelMaterialAtlas.h` - Data asset header
- `VoxelCore/Private/VoxelMaterialAtlas.cpp` - BuildTextureArrays(), BuildMaterialLUT()
- `VoxelRendering/Private/VoxelWorldComponent.cpp` - UpdateMaterialAtlasParameters()
- `VoxelRendering/Private/VoxelCustomVFRenderer.cpp` - Automatic mode sync

**Shaders:**
- `Shaders/Private/VoxelTriplanarCommon.ush` - Triplanar utility functions

**Material Assets (Content/VoxelWorlds/Materials/):**
- `M_VoxelMaster` - Unified master material
- `MI_VoxelDefault` - Default material instance
- `Functions/MF_GetMaterialID` - MaterialID extraction
- `Functions/MF_TriplanarSampleAlbedoRoughness` - Smooth terrain sampling
- `Functions/MF_TriplanarSampleNormal` - Smooth terrain normals
- `Functions/MF_CubicAtlasSample` - Cubic terrain sampling

See [BIOME_SYSTEM.md](BIOME_SYSTEM.md) for material assignment logic.

