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

The primary interface for material configuration. Create this asset in the Content Browser and assign it to your VoxelRegionComponent.

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

    // ===== Opacity Flags =====

    /** Use masked (alpha cutout) blending for this material.
     *  Requires albedo texture to have alpha channel with cutout pattern. */
    bool bIsMasked = false;

    /** Non-occluding material — render faces between this and any different material.
     *  Same-material adjacency still culls. Creates visual density for leaves, etc. */
    bool bNonOccluding = false;
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

    // ===== Texture Arrays (Smooth Terrain) - Built from MaterialConfigs =====

    /** Saved assets next to the atlas (<Atlas>_AlbedoArray, ...), built by BuildTextureArrays() */
    UTexture2DArray* AlbedoArray;    // VisibleAnywhere
    UTexture2DArray* NormalArray;
    UTexture2DArray* RoughnessArray;
    int32 TextureArraySize = 512;    // Size of every array slice (rounded up to a power of two)
    FString TextureArrayBuildKey;    // Editor-only: hash of the inputs the arrays were built from
    TArray<uint8> AlbedoArraySlices; // Each MaterialID's slice (256 entries), written with the arrays; saved
    TArray<uint8> NormalArraySlices;
    TArray<uint8> RoughnessArraySlices;

    // ===== Per-Material Configuration =====

    TArray<FVoxelMaterialTextureConfig> MaterialConfigs;

    // ===== Key API Functions =====

    /** Build Texture2DArrays from source textures in MaterialConfigs */
    UFUNCTION(BlueprintCallable, CallInEditor)
    void BuildTextureArrays();

    /** Build Material Lookup Table: face-variant atlas positions (rows 0-2) and texture array slices (row 3) */
    UFUNCTION(BlueprintCallable, CallInEditor)
    void BuildMaterialLUT();

    /** Check if arrays need rebuilding (editor: missing, or built from other inputs) */
    bool AreTextureArraysDirty() const;

    /** What a world calls before binding: the editor rebuilds missing or stale arrays first */
    bool EnsureTextureArrays();
};
```

### Building the Texture Arrays

The arrays are **saved assets** next to the atlas: `<Atlas>_AlbedoArray`, `<Atlas>_NormalArray` and
`<Atlas>_RoughnessArray` in the atlas's folder (`DemoMaterialAtlas_AlbedoArray`, ...), referenced by the atlas and cooked
with it. A packaged game has no source textures to build from, so it binds what was saved (`VISUAL_POLISH_PLAN.md`,
F2.11). They are Git LFS files in the parent repo (`.gitattributes`), one slice per distinct texture (F2.12), about
half of 512 x 512 x 4 bytes a slice on disk (the source, delta-coded and Kraken-compressed): `DemoMaterialAtlas`'s three
7.9 MB (8, 8 and 3 slices), `VoxelMaterialAtlas`'s 14.3 MB (10 each); 38.8 and 39.2 MB with a slice per MaterialID.

- **Building.** `BuildTextureArrays()` (the atlas's *Build Texture Arrays* button) creates the three assets or updates
  them in place (`PreEditChange` / `PostEditChange`, as a reimport does, so materials drawing with them pick up the new
  data), stores the inputs' hash in `TextureArrayBuildKey`, bakes the map colours, and leaves the atlas and its arrays
  unsaved. `Scripts/rebuild_atlas_arrays.py` rebuilds and saves every atlas under `/Game` (headless:
  `UnrealEditor-Cmd.exe VoxelEngine.uproject -run=pythonscript -script="Scripts/rebuild_atlas_arrays.py" -nullrhi
  -unattended`; `--only-stale` skips up-to-date ones). An atlas outside a package (a transient one, as in the tests)
  gets transient arrays. Outside the editor `BuildTextureArrays()` does nothing (no source data); the saved arrays stay.
- **Staleness.** The key hashes `TextureArraySize`, a build version, and for each config its MaterialID and each
  texture's path with what identifies its content: the saved package's hash from the asset registry when the texture's
  package is saved and unmodified (so checking loads nothing), otherwise the loaded texture's source id. The arrays
  are stale when the key differs or an array the configs give textures for is missing (`AreTextureArraysDirty()`,
  `NeedsTextureArrayRebuild()`). Editing the atlas's configs or `TextureArraySize`, or reimporting a source texture,
  makes them stale; renaming a material does not. An atlas with no source texture and no array (a packed atlas for
  cubic terrain only) is never stale.
- **Binding.** `UVoxelRegionComponent::UpdateMaterialAtlasParameters`
  call `EnsureTextureArrays()` and bind the arrays. In the editor, missing or stale arrays are rebuilt there first, with
  a warning naming the atlas (save the atlas and its arrays to keep them; a packaged game uses the saved ones); up to
  date ones are only bound, so an editor session's first PIE builds nothing (0.28 s on `PlanetTest`; a rebuild of the
  demo atlas's arrays there takes about 3 s). A cook never rebuilds. Outside the editor an atlas with source textures
  and no saved arrays logs an error once (smooth terrain then draws untextured: black in a package).
- **Cook check.** Cooking an atlas (`PreSave` with a target platform) logs an error when an array is missing, which
  fails the cook, and a warning when they are stale (the package ships the saved ones). Data validation (`IsDataValid`)
  reports both as errors, so a cook with `-RunAssetValidation -ValidationErrorsAreFatal` fails on stale arrays too.

For each array (albedo, normal, roughness), `BuildSingleTextureArray` writes the array's source itself rather than through
`UTexture2DArray::UpdateSourceFromSourceTextures()`, which would size every slice to the largest source texture and
convert them all to RGBA32F when their gamma spaces or formats differ (on the demo atlas that was 4096 x 4096 x 22 float
slices, 20-24 s of the first PIE and 800 MB on the GPU; see `VISUAL_POLISH_PLAN.md`, F2.10):

- Slices are `TextureArraySize` square (rounded up to a power of two), 8-bit BGRA. One slice per distinct texture with
  readable source data, in MaterialID order (F2.12): materials sharing a texture share its slice.
- Each slice is converted from its texture's own gamma into the array's: sRGB for albedo, linear for normals and
  roughness, so the shader samples the same values whatever the source textures' formats. The resize is ImageCore's
  default filter in linear light, wrapping at the edges (terrain textures tile).
- Each material's slice per array goes in the atlas's slice tables (`AlbedoArraySlices`, `NormalArraySlices`,
  `RoughnessArraySlices`: 256 entries, index = MaterialID, saved with the atlas) and from there into the material LUT's
  slice row, which the triplanar functions read (see "The Slice Row" below). The kinds are independent: a material may
  have an albedo but no normal map. A material without a texture (or with no readable source), and a MaterialID with no
  config below the highest configured, reads the first slice; a MaterialID above the highest configured reads the
  highest's: what a slice per MaterialID gave them (a copy of the first slice, and the sampler clamping the index), so
  the terrain draws the same. With no readable texture at all, the array is one slice of the placeholder colour.
- The settings are fixed per array: albedo `TC_Default` (BC1, or BC3 with alpha for masked materials), sRGB; normals
  `TC_Normalmap` (BC5) in `TEXTUREGROUP_WorldNormalMap`; roughness `TC_Masks` (BC1/BC3, linear). Mips from the texture
  group, `TF_Default` filtering, never streamed.
- The platform data builds asynchronously and is cached in the DDC by the source's hash.

On `DemoMaterialAtlas` (8, 8 and 3 slices at 512 for its 22 materials) the build takes about 3.3 s and the arrays
4.8 MB on the GPU (15.8 MB with a slice per MaterialID).

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

### Face Type on a Planet (spherical Phase 4, SP-D3)

The face type (top, side, bottom) that runs is the custom vertex factory's: `FVoxelSceneProxy::ConvertVertex` classifies
every vertex's normal against 0.7, along world +Z on heightmap worlds. On a planet the custom VF renderer turns on
`UVoxelRegionComponent::SetRadialFaceUp`, and the normal is measured against the vertex's radial up instead
(`Rendering.Planet.FaceTypeByLocalUp`). `VoxelMaterialCommon.ush` gets an up-aware `GetVoxelFaceTypeUp` for shaders
that need one (no shader includes it today). Smooth materials are triplanar on world axes, which works in any
orientation, and read the face type only in the cubic atlas path, which planets do not use (SD3).

The triplanar position is large-world safe (spherical Phase 6, WM-BP7): the triplanar custom nodes
(`MF_TriplanarSampleAlbedoRoughness`, `MF_TriplanarSampleNormal`) take a float3, and Absolute World Position demoted to
float is 8 uu coarse at 10^8 (streaked ground on a 1,000 km planet). `M_VoxelMaster` and `M_VoxelMaster_Fade` feed
them a tiled position computed with LWC math nodes: Period = max(1, round(65,536 x `TriplanarScale`)) / `TriplanarScale`,
a whole number of texture tiles, and tiled = frac(absolute world position / Period) x Period. The UV differs from the
untiled one by whole tiles, so the same texels are read with no seam at a period boundary. The nodes take their UV
derivatives from the translated (camera-relative) world position, not from the tiled one, which jumps by a period at
its boundaries (see "Footprint Sampling" below). A new smooth master material must tile its position the same way
before it reaches a custom node.

---

## Smooth Mode: Triplanar Projection

For smooth meshing (Marching Cubes), materials use triplanar projection to avoid UV seams. The implementation uses Custom HLSL nodes in the Unreal Material Editor.

### MaterialID Encoding

MaterialID is stored in the **UV1.x** channel as a float value. This avoids sRGB gamma conversion issues that would corrupt integer data in vertex colors.

**In Mesh Generator (FVoxelCPUMarchingCubesMesher):**
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
- `MaterialLUT` (Texture2D) - The atlas's material LUT (its slice row gives the material's slice in each array)
- `GradientSampling` (float) - `MPC_VoxelMaterial` collection parameter (Footprint Sampling below)
- `TileVariation`, `MacroVariation` (float) - `MPC_VoxelMaterial` collection parameters (Texture Variation below; the node
  includes `/Plugin/VoxelWorlds/Private/VoxelTextureVariation.ush`)

**Output:** `float4` (RGB = Albedo, A = Roughness)

The code below is the node with both variation switches at 0; the full code, with the variation path, is in
`Scripts/setup_texture_variation.py` (parent repo), which writes it into the asset; `Scripts/setup_texture_array_slices.py`
(F2.12) then reads each array at the material's slice from the LUT. Run both, in that order.

```hlsl
int MaterialID = (int)floor(MaterialUV.x + 0.5);

// The material's slice in each array, from the material LUT's slice row (row 3: R albedo, G normal, B roughness)
const float4 ArraySlices = MaterialLUT.Load(int3(clamp(MaterialID, 0, 255), 3, 0));
const float AlbedoSlice = floor(ArraySlices.r * 255.0 + 0.5);
const float RoughnessSlice = floor(ArraySlices.b * 255.0 + 0.5);

// Calculate triplanar blend weights
float3 Weights = pow(abs(WorldNormal), Sharpness);
Weights /= max(Weights.x + Weights.y + Weights.z, 0.0001);

// Calculate triplanar UVs
float2 UV_X = WorldPos.zy * Scale;  // YZ plane
float2 UV_Y = WorldPos.xz * Scale;  // XZ plane (top/bottom)
float2 UV_Z = WorldPos.xy * Scale;  // XY plane

// UV derivatives from the continuous translated position, scaled by GradientSampling (MPC_VoxelMaterial)
float3 DPdx = 0;
float3 DPdy = 0;
#if PIXELSHADER
const float3 TranslatedPosition = GetTranslatedWorldPosition(Parameters);
DPdx = ddx(TranslatedPosition) * (Scale * GradientSampling);
DPdy = ddy(TranslatedPosition) * (Scale * GradientSampling);
#endif

// Sample albedo from each projection at the pixel's footprint, with the array's own (anisotropic) sampler
float4 Albedo_X = AlbedoArray.SampleGrad(AlbedoArraySampler, float3(UV_X, AlbedoSlice), DPdx.zy, DPdy.zy);
float4 Albedo_Y = AlbedoArray.SampleGrad(AlbedoArraySampler, float3(UV_Y, AlbedoSlice), DPdx.xz, DPdy.xz);
float4 Albedo_Z = AlbedoArray.SampleGrad(AlbedoArraySampler, float3(UV_Z, AlbedoSlice), DPdx.xy, DPdy.xy);
float3 Albedo = Albedo_X.rgb * Weights.x + Albedo_Y.rgb * Weights.y + Albedo_Z.rgb * Weights.z;

// Sample roughness
float Rough_X = RoughnessArray.SampleGrad(RoughnessArraySampler, float3(UV_X, RoughnessSlice), DPdx.zy, DPdy.zy).r;
float Rough_Y = RoughnessArray.SampleGrad(RoughnessArraySampler, float3(UV_Y, RoughnessSlice), DPdx.xz, DPdy.xz).r;
float Rough_Z = RoughnessArray.SampleGrad(RoughnessArraySampler, float3(UV_Z, RoughnessSlice), DPdx.xy, DPdy.xy).r;
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
- `MaterialLUT` (Texture2D) - The atlas's material LUT
- `GradientSampling` (float) - `MPC_VoxelMaterial` collection parameter
- `TileVariation` (float) - `MPC_VoxelMaterial` collection parameter (Texture Variation below)

**Output:** `float3` (World-space normal)

```hlsl
int MaterialID = (int)floor(MaterialUV.x + 0.5);

// The material's slice in each array, from the material LUT's slice row (row 3: R albedo, G normal, B roughness)
const float4 ArraySlices = MaterialLUT.Load(int3(clamp(MaterialID, 0, 255), 3, 0));
const float NormalSlice = floor(ArraySlices.g * 255.0 + 0.5);

float3 Weights = pow(abs(WorldNormal), Sharpness);
Weights /= max(Weights.x + Weights.y + Weights.z, 0.0001);

float2 UV_X = WorldPos.zy * Scale;
float2 UV_Y = WorldPos.xz * Scale;
float2 UV_Z = WorldPos.xy * Scale;

// DPdx / DPdy as in the albedo node
float3 DPdx = 0;
float3 DPdy = 0;
#if PIXELSHADER
const float3 TranslatedPosition = GetTranslatedWorldPosition(Parameters);
DPdx = ddx(TranslatedPosition) * (Scale * GradientSampling);
DPdy = ddy(TranslatedPosition) * (Scale * GradientSampling);
#endif

// Decode tangent-space normals
float3 TN_X = NormalArray.SampleGrad(NormalArraySampler, float3(UV_X, NormalSlice), DPdx.zy, DPdy.zy).rgb * 2.0 - 1.0;
float3 TN_Y = NormalArray.SampleGrad(NormalArraySampler, float3(UV_Y, NormalSlice), DPdx.xz, DPdy.xz).rgb * 2.0 - 1.0;
float3 TN_Z = NormalArray.SampleGrad(NormalArraySampler, float3(UV_Z, NormalSlice), DPdx.xy, DPdy.xy).rgb * 2.0 - 1.0;

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
- Sample with `SampleGrad()` and explicit derivatives, not `Sample()`: the derivatives must come from the continuous
  translated position (the tiled position jumps at its period), and outside the pixel shader (ray tracing hit shaders)
  there are none, so the gradients stay zero there (mip 0)

### The Slice Row (F2.12)

The texture arrays hold one slice per distinct texture, not one per MaterialID (`VISUAL_POLISH_PLAN.md`, F2.12), so the
triplanar functions cannot sample at the MaterialID. `UVoxelMaterialAtlas::BuildMaterialLUT` writes a fourth row below
the three face rows: texel (MaterialID, 3) holds the material's slice in the albedo array (R), the normal array (G) and
the roughness array (B), from the atlas's slice tables; A is 0. Both triplanar functions take the `MaterialLUT` texture
as an input (wired in `M_VoxelMaster` and `M_VoxelMaster_Fade` from the same `MaterialLUT` parameter the cubic path
reads) and `Load` that texel. The face rows are read by texel too (`MF_CubicAtlasSample`), so the extra row does not
move them. An atlas whose arrays were built with a slice per MaterialID has empty slice tables, and its LUT gives every
MaterialID the slice of its own index. `BuildTextureArrays()` rewrites an existing LUT in place, so materials already
bound to it read the new slices with the new arrays (the editor rebuilds stale arrays when a world binds the atlas, after
the LUT was built). The planet far field's `FarFieldMaterialPick` produces a MaterialID like every voxel vertex and needs
nothing of its own. `Scripts/setup_texture_array_slices.py` (UE5_PluginDev) made the material edits, headless.

### Footprint Sampling (visual polish VP2-O1)

Until 2026-10-24 both nodes sampled with `SampleLevel(..., 0)`: mip 0 at every distance. At one 512 px tile per metre a
pixel of ground 10 m away covers about 5 texels and 50 m away about 26, so every pixel beyond a few metres picked one
texel out of its footprint, a different one with every sub-pixel camera move and TSR jitter: aliasing TSR cannot
resolve, the terrain's "extremely noisy" look (`Documentation/VISUAL_POLISH_PLAN.md`). The mip chain restored in
VoxelWorlds#25 was never read.

Now each projection samples with `SampleGrad` and the UV derivatives of the translated world position, with the
array's own sampler (`AlbedoArraySampler` etc., anisotropic: the arrays are `TEXTUREGROUP_World` with `TF_Default`).
The switch is the scalar `GradientSampling` of `MPC_VoxelMaterial` (`/VoxelWorlds/Materials/`), read inside both
functions; `voxel.Material.GradientSampling` (1 by default, 0 = mip 0 as before) sets it in every game and PIE world
(a console variable sink, and each `UVoxelRegionComponent` applies it when its scene proxy is created). On `PlanetTest`
at four fixed views, a quarter-pixel camera turn changes the ground's pixels by 43.8-45.0 luma (of 255) at mip 0 and
0.6-1.6 with gradients; the ground's high-frequency energy under TSR falls from 55-59 to 3.5-7.3; GPU time per frame
is not higher (P50 5.11-5.22 ms against 4.94-5.07, three bench runs each).

### Terrain Roughness (visual polish VP2-O6)

`M_VoxelMaster` and `M_VoxelMaster_Fade` used to output the sampled roughness times `RoughnessMultiplier`, which is 0 in
the master and in `MI_VoxelDefault`: roughness 0, a mirror. No terrain material has a roughness map (empty slots of the
roughness array hold a placeholder: the first valid texture, the coal ore's packed ORD map), so the multiplier could not
fix it either. The terrain reflected the sky at grazing angles: a pale sheen and hard-edged patches on far, flat ground,
and before VP2-O1 sparkle. Both masters now output `lerp(sampled x RoughnessMultiplier, TerrainRoughness,
ConstantRoughness)`: `TerrainRoughness` is a scalar parameter (0.9 by default), `ConstantRoughness` is
`MPC_VoxelMaterial`'s, set by `voxel.Material.ConstantRoughness` (1 by default; 0 = as before). On `PlanetTest` the
roughness buffer reads 0 → 0.9 on the ground, and the far ground at a grazing view loses its sky sheen (blue channel
112 → 86, saturation 41.6 → 66.0). When real roughness maps exist, `ConstantRoughness` 0 and a `RoughnessMultiplier` of 1
use them.

### Texture Variation (visual polish VP2-O2)

With mips in use (VP2-O1) the one-metre tiles repeat visibly: rows of the same marks close up, a plaid grid on flat
ground at mid distance (F2.8). Both triplanar functions include `Shaders/Private/VoxelTextureVariation.ush`:

- **Tile variation** (`TileVariation`, `voxel.Material.TileVariation`, 1 by default): the ground becomes a mosaic of
  patches under a tile across, each reading the texture at its own offset (Inigo Quilez, "Texture repetition",
  technique 3). A smooth value noise about a tile across, times eight, picks one of eight offsets; each patch's tail
  blends into the next patch's offset, which the next patch starts with, so there is no seam. Offsets keep the
  texture's orientation, so the normal map needs no rotation, and both functions compute the same offsets. Where a patch
  is not blending, one read suffices; the second read happens only in the blend bands. Triplanar projections under 2%
  are dropped (the rest renormalised), so at the terrain's sharpness most pixels read one projection.
- **Macro variation** (`MacroVariation`, `voxel.Material.MacroVariation`, 0.3 by default, albedo only): the albedo's
  brightness is scaled by up to 1 ± that by two octaves of value noise about 13 and 41 tiles across.

The triplanar position is tiled for large-world precision (spherical Phase 6, P6.2): it wraps every
max(1, round(65,536 x `TriplanarScale`)) tiles. Every noise is built on a lattice that fits that period a whole number
of times, with its hash wrapped to it, so nothing jumps where the position wraps. Both switches at 0 give the material
as before. The tunables (cell size, number of offsets, where the blend starts) are `#define`s at the top of the include;
the include's hash is part of the material's shader key, so editing it recompiles the masters (in a running editor:
`recompileshaders changed`, then recompile the materials). `Scripts/setup_texture_variation.py` writes the nodes and the
collection scalars; `VoxelWorlds.Rendering.TextureVariation.Assets` checks the wiring. Results: `VISUAL_POLISH_PLAN.md`,
"VP2-O2 texture variation".

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

## Hardness (world modes review §22, decision D31)

`FVoxelMaterialDefinition::Hardness` is how hard a block of the material is to break: the seconds it takes bare-handed.
It is a property of the material, set by the registry's defaults (`FVoxelMaterialRegistry::EnsureInitialized`), read with
`FVoxelMaterialRegistry::GetHardness` (`DefaultHardness`, 1 s, for an ID the registry does not know). The material atlas
does not override it: the atlas is rendering data, which a dedicated server need not load, and the server decides a break.

| Material | Hardness (s) | Material | Hardness (s) |
|----------|--------------|----------|--------------|
| Leaves | 0.2 | Wood | 1.2 |
| Snow | 0.3 | Stone | 1.5 |
| Dirt, Sand | 0.5 | Coal | 2.0 |
| Grass | 0.6 | Copper | 2.2 |
| Frozen Dirt | 0.8 | Iron, Gold | 2.5 |
| Sandstone | 1.0 | Diamond | 4.0 |

The reserved slots, and materials an atlas adds past the registry's list, keep the default. Gameplay decides what the
number means: in VoxelCharacterPlugin a cubic world's block breaks after `Hardness / MiningSpeed` while the primary action
is held (`UVCBlockInteractionComponent::GetBreakSeconds`; bare hands 1, the stone pickaxe 1.5, the iron pickaxe 2;
`vc.Dig.BreakTimes`), decided on the server; smooth worlds dig at once (D32).

---

## Example Material Definitions

*(A design sketch from before the registry; its fields do not all exist. The real definition is `VoxelMaterialDefinition.h`,
and its `Hardness` is the section above.)*

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

### UVoxelRegionComponent::UpdateMaterialAtlasParameters

```cpp
void UVoxelRegionComponent::UpdateMaterialAtlasParameters(UMaterialInstanceDynamic* DynamicMaterial)
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

    // The saved arrays; in the editor missing or stale ones are rebuilt first (left unsaved, with a warning)
    MaterialAtlas->EnsureTextureArrays();

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

The material mode is synced automatically in `FVoxelMeshRenderer::Initialize()`:

```cpp
// Sync material mode with configuration's meshing mode
const bool bIsSmooth = (WorldConfig->MeshingMode != EMeshingMode::Cubic);
OriginRegionComponent->SetUseSmoothMeshing(bIsSmooth);
```

This sets the `bSmoothTerrain` material parameter which controls the Lerp nodes in M_VoxelMaster.

### Material Functions

The master material uses 4 reusable material functions:

1. **MF_GetMaterialID** - Extracts MaterialID from UV1.x
2. **MF_TriplanarSampleAlbedoRoughness** - Smooth terrain color/roughness sampling
3. **MF_TriplanarSampleNormal** - Smooth terrain normals with UDN blend
4. **MF_CubicAtlasSample** - Cubic terrain sampling via UV0 + MaterialLUT

### Far-Field Material Blend (FarFieldMaterialPick)

A planet's far field ([LOD_SYSTEM.md](LOD_SYSTEM.md) "The Far Field") draws with the same material, and since G45
(WORLD_MODES_REVIEW_PLAN.md §20.27, decision D28) its quads blend two materials: UV1.x the quad's first material as on
every voxel vertex, vertex colour R its second, and vertex colour A 255 less the second's weight at the vertex. In
`M_VoxelMaster` and `M_VoxelMaster_Fade` a Custom node, **FarFieldMaterialPick**, sits between `MF_GetMaterialID` and the
`MaterialID` named reroute, so everything that reads the reroute sees the picked material:

```hlsl
// Inputs: MaterialID (MF_GetMaterialID), SecondaryR (VertexColor R), VertexAlpha (VertexColor A)
#if PIXELSHADER
float W = saturate(1.0 - VertexAlpha);
if (W > 0.002)
{
    float2 P = Parameters.SvPosition.xy + 5.588238 * (float)View.StateFrameIndexMod8;
    float Noise = frac(52.9829189 * frac(dot(P, float2(0.06711056, 0.00583715))));
    if (Noise < W) { return floor(SecondaryR * 255.0 + 0.5); }
}
#endif
return MaterialID;
```

Each pixel takes one of the two materials against interleaved gradient noise moved every frame, which TSR averages into a
gradient across the quad: no texture sample is added. Every voxel chunk vertex has alpha 255 (weight 0) and keeps
`MaterialID`. Vertex colour reaches the shader as UNORM (`VET_Color`, no sRGB decode), so R carries an ID exactly; IDs stay
out of interpolation because both are flat per triangle. The node is added by `Scripts/setup_farfield_material_blend.py`
(UE5_PluginDev; idempotent, run headless). The VertexColor node's default output is RGB only, so R and A go in as their own
inputs: a `.a` on that output fails the material's compile and every mesh using it draws the default material.

### Material Assets

| Asset | Location | Purpose |
|-------|----------|---------|
| M_VoxelMaster | Content/VoxelWorlds/Materials/ | Master material with both paths |
| MI_VoxelDefault | Content/VoxelWorlds/Materials/ | Default material instance |
| MF_* | Content/VoxelWorlds/Materials/Functions/ | Reusable material functions |

See [MASTER_MATERIAL_SETUP.md](MASTER_MATERIAL_SETUP.md) for detailed setup instructions.

---

## Per-Material Opacity / Non-Occluding Materials

### Overview

Materials can be flagged as **masked** (alpha cutout) and/or **non-occluding** (render all boundary faces). These flags are set per-material in `FVoxelMaterialTextureConfig` on the `UVoxelMaterialAtlas` data asset. The atlas configuration is **authoritative** — it overrides any hardcoded registry defaults.

### Flags

| Flag | Effect |
|------|--------|
| `bIsMasked` | Triangles rendered with `BLEND_Masked` using albedo alpha as opacity mask |
| `bNonOccluding` | Face culling disabled between this material and any other material. Same-material adjacency still culls. |

### LUT A-Channel Encoding

The Material LUT texture (256 x 4: three face rows and the slice row, see "The Slice Row") uses the **A channel** of its
face rows to encode per-material flags:
- **Bit 0**: `bIsMasked` (1 = masked blend, 0 = opaque)
- **Bits 1-7**: Reserved for future flags

Previously the A channel was unused (always 255). Now it encodes flags as a bitfield.

### Non-Occluding Face Generation (Cubic Mesher)

For non-occluding materials, `ShouldRenderFace()` returns true if **either** the current voxel or the neighbor voxel has `bNonOccluding` set. This generates all boundary faces for visual density.

Non-occluding faces are emitted as individual 1x1 quads in a **pre-pass** before greedy meshing. This prevents them from being merged into larger quads (which would lose per-voxel visual variation).

### Material Registry Dynamic Growth

`SetAtlasPositions()` grows the `Materials` array if the atlas defines MaterialIDs beyond the hardcoded set (IDs 0-21). This allows users to define arbitrary materials in the atlas without modifying C++ code.

### Texture Requirements

For masked materials to work:
- The albedo texture in the packed atlas (or Texture2DArray slice) must have an **alpha channel** with the cutout pattern
- The atlas texture format must preserve alpha (RGBA, `TC_Default` or `TC_BC7`)
- All other material tiles should have alpha = 1.0 (fully opaque)

### Master Material Setup

The master material (M_VoxelMaster) must:
1. Use `BLEND_Masked` as its blend mode
2. Connect the atlas albedo **alpha channel** to the **Opacity Mask** pin
3. Set `Opacity Mask Clip Value` to 0.333 (or desired threshold)

When the opaque MID overrides blend mode back to `BLEND_Opaque`, UE ignores the OpacityMask pin entirely — no performance cost for opaque chunks.

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
11. ✅ Per-material opacity flags (`bIsMasked`, `bNonOccluding`)
13. ✅ LUT A-channel flag encoding
14. ✅ Non-occluding face generation in cubic mesher

### Key Files

**C++ Code:**
- `VoxelCore/Public/VoxelMaterialAtlas.h` - Data asset header
- `VoxelCore/Private/VoxelMaterialAtlas.cpp` - BuildTextureArrays(), BuildMaterialLUT()
- `VoxelRendering/Private/VoxelRegionComponent.cpp` - UpdateMaterialAtlasParameters()
- `VoxelRendering/Private/VoxelMeshRenderer.cpp` - Automatic mode sync

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

