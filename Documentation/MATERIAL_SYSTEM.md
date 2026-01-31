# Material System

**Module**: VoxelGeneration  
**Dependencies**: VoxelCore

## Overview

The material system manages 256 different material types with texture atlases for cubic meshing and triplanar projection for smooth meshing.

## Material Registry

### FVoxelMaterialDefinition

```cpp
/**
 * Material Definition
 * 
 * Defines a single material type with textures and properties.
 */
struct FVoxelMaterialDefinition
{
    /** Material ID (0-255) */
    uint8 MaterialID;
    
    /** Display name */
    FString MaterialName;
    
    // ==================== Cubic Mode (Atlas UVs) ====================
    
    /** Top face UV coordinates in atlas */
    FVector2D TopUV;
    
    /** Bottom face UV coordinates in atlas */
    FVector2D BottomUV;
    
    /** Side faces UV coordinates in atlas */
    FVector2D SideUV;
    
    // ==================== Smooth Mode (Triplanar) ====================
    
    /** Albedo texture */
    UTexture2D* AlbedoTexture;
    
    /** Normal texture */
    UTexture2D* NormalTexture;
    
    /** Roughness/Metallic texture */
    UTexture2D* RoughnessMetallicTexture;
    
    // ==================== Physical Properties ====================
    
    /** Hardness (affects mining time) */
    float Hardness = 1.0f;
    
    /** Emissive (glowing) */
    bool bEmissive = false;
    
    /** Emissive color/intensity */
    FLinearColor EmissiveColor = FLinearColor::Black;
    
    /** Triplanar scale */
    float TriplanarScale = 1.0f;
};
```

### UVoxelMaterialRegistry

```cpp
/**
 * Material Registry
 * 
 * Central registry for all voxel materials.
 * Singleton accessed by generation and rendering systems.
 */
UCLASS()
class UVoxelMaterialRegistry : public UObject
{
    GENERATED_BODY()
    
public:
    /** Get singleton instance */
    static UVoxelMaterialRegistry* Get();
    
    /** Register a material definition */
    void RegisterMaterial(const FVoxelMaterialDefinition& Material);
    
    /** Get material by ID */
    const FVoxelMaterialDefinition* GetMaterial(uint8 MaterialID) const;
    
    /** Get atlas UV for material face */
    FVector2D GetAtlasUV(uint8 MaterialID, EVoxelFace Face) const;
    
    /** Get material count */
    int32 GetMaterialCount() const { return Materials.Num(); }
    
    /** Build material atlas texture */
    void BuildAtlasTexture();
    
    /** Get atlas texture (for rendering) */
    UTexture2D* GetAtlasTexture() const { return AtlasTexture; }
    
private:
    /** Material definitions indexed by MaterialID */
    UPROPERTY()
    TMap<uint8, FVoxelMaterialDefinition> Materials;
    
    /** Generated atlas texture (cubic mode) */
    UPROPERTY()
    UTexture2D* AtlasTexture;
    
    /** Atlas resolution */
    int32 AtlasSize = 2048;
    
    /** Tile size in atlas */
    int32 TileSize = 64;
};
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

For smooth meshing, materials use triplanar projection to avoid UV seams.

### Triplanar Shader

```hlsl
// Material sampling with triplanar projection
float4 SampleTriplanar(Texture2D Tex, SamplerState Sampler, float3 WorldPos, float3 Normal, float Scale)
{
    // Calculate blend weights
    float3 BlendWeights = abs(Normal);
    BlendWeights = BlendWeights / (BlendWeights.x + BlendWeights.y + BlendWeights.z);
    
    // Sample each axis
    float4 XAxis = Tex.Sample(Sampler, WorldPos.yz * Scale);
    float4 YAxis = Tex.Sample(Sampler, WorldPos.xz * Scale);
    float4 ZAxis = Tex.Sample(Sampler, WorldPos.xy * Scale);
    
    // Blend
    return XAxis * BlendWeights.x + YAxis * BlendWeights.y + ZAxis * BlendWeights.z;
}

// Pixel shader for smooth voxel mesh
float4 VoxelPixelShader(PixelInput Input) : SV_Target
{
    // Unpack material ID
    uint MaterialID = (Input.MaterialData >> 24) & 0xFF;
    
    // Get material definition (from constant buffer)
    MaterialDefinition Mat = Materials[MaterialID];
    
    // Sample albedo with triplanar
    float4 Albedo = SampleTriplanar(
        Mat.AlbedoTexture,
        Mat.AlbedoSampler,
        Input.WorldPosition,
        Input.Normal,
        Mat.TriplanarScale
    );
    
    // Sample normal map
    float3 NormalMap = SampleTriplanar(
        Mat.NormalTexture,
        Mat.NormalSampler,
        Input.WorldPosition,
        Input.Normal,
        Mat.TriplanarScale
    ).xyz;
    
    // Apply normal map
    float3 FinalNormal = ApplyNormalMap(Input.Normal, NormalMap);
    
    // Sample roughness/metallic
    float2 RoughnessMetallic = SampleTriplanar(
        Mat.RoughnessMetallicTexture,
        Mat.RMSampler,
        Input.WorldPosition,
        Input.Normal,
        Mat.TriplanarScale
    ).xy;
    
    // Lighting calculations
    float3 Lighting = CalculateLighting(Albedo.rgb, FinalNormal, RoughnessMetallic.x, RoughnessMetallic.y);
    
    // Apply emissive
    if (Mat.bEmissive)
    {
        Lighting += Mat.EmissiveColor.rgb * Mat.EmissiveColor.a;
    }
    
    return float4(Lighting, 1.0);
}
```

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

## GPU Material Buffer

For smooth meshing, material data is uploaded to GPU:

```cpp
struct FGPUMaterialData
{
    uint32 Flags;              // Emissive, etc.
    FVector4f EmissiveColor;   // RGB + Intensity
    float TriplanarScale;
    float Hardness;
    uint32 TextureIndices[3];  // Albedo, Normal, RM
};

// Upload to structured buffer
void UpdateMaterialBuffer()
{
    TArray<FGPUMaterialData> GPUMaterials;
    
    for (int32 i = 0; i < 256; ++i)
    {
        const FVoxelMaterialDefinition* Mat = Registry->GetMaterial(i);
        
        FGPUMaterialData& GPUMat = GPUMaterials.AddDefaulted_GetRef();
        GPUMat.Flags = Mat->bEmissive ? 1 : 0;
        GPUMat.EmissiveColor = Mat->EmissiveColor;
        GPUMat.TriplanarScale = Mat->TriplanarScale;
        GPUMat.Hardness = Mat->Hardness;
        // ... texture indices
    }
    
    // Upload to GPU
    MaterialBuffer->UpdateStructuredBuffer(GPUMaterials);
}
```

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

## Next Steps

1. Create `UVoxelMaterialRegistry` class
2. Define default material set
3. Build atlas texture system
4. Implement triplanar shader
5. Integrate with biome system

See [BIOME_SYSTEM.md](BIOME_SYSTEM.md) for material assignment logic.

