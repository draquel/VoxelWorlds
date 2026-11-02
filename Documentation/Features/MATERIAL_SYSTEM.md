# Material System

> State as of 2026-11-02 (code review CR5).

**Module**: VoxelCore
**Dependencies**: Engine, RenderCore

## Overview

The material system manages 256 different material types with a packed texture atlas for cubic meshing and
Texture2DArrays with triplanar projection for smooth meshing (Marching Cubes and Dual Contouring). It is centred on the
`UVoxelMaterialAtlas` data asset, which holds the per-material configuration, the saved texture arrays and the material
LUT; `M_VoxelMaster` draws both paths; MaterialIDs travel in UV1 (sRGB-safe); the arrays are bound at runtime through a
dynamic material instance.

## UVoxelMaterialAtlas (Data Asset)

The primary interface for material configuration. Create this asset in the Content Browser and assign it to the world
configuration; the renderer binds it on every render region's component.

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
with it. A packaged game has no source textures to build from, so it binds what was saved (F2.11; before, a package drew
the smooth terrain black from empty arrays). They are Git LFS files in the parent repo (`.gitattributes`), one slice per distinct texture (F2.12), about
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
slices, 20-24 s of the first PIE and 800 MB on the GPU; F2.10, tests `VoxelWorlds.MaterialAtlas.TextureArrays.*`):

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

### Shipping on DX11 (SM5)

A Windows package cooks SM6 and SM5. The arrays are saved, so the cook builds nothing (a missing array fails it, see
"Cook check"). The voxel compute shaders are written so FXC, the SM5 compiler, compiles them in seconds: branchy helpers
under `[branch]`, voxel and noise loops under `[loop]`, one cellular search for Cellular and Voronoi, and asin by its
series below 0.1 (FXC's asin polynomial put DX11 planet pads off). Before, the SM5 cook's global shader step ran
1 h 51 min of FXC and failed (F2.13; [GPU_PIPELINE.md](GPU_PIPELINE.md)). A cold-cache package's local DDC path must
stay under 119 characters (`-LocalDataCachePath` moves Zen's store too).

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

The cubic path samples `PackedAlbedoAtlas` (and the normal and roughness atlases), a grid of `AtlasColumns` x
`AtlasRows` tiles. Each MaterialID's tile per face comes from the material LUT's face rows (`BuildMaterialLUT`; rows
0-2 for the top, side and bottom faces): R the atlas column, G the row, B the UV scale x 25.5, A the flags ("LUT
A-Channel Encoding" below). A config with `bUseFaceVariants` gives each face its own tile (`TopTile`, `SideTile`,
`BottomTile`), else every face uses `AtlasColumn` / `AtlasRow`. `MF_CubicAtlasSample` reads the LUT by texel with the
vertex's MaterialID (UV1.x) and face type (UV1.y) and samples the tile with UV0.

### Face Type on a Planet (spherical Phase 4, SP-D3)

The face type (top, side, bottom) that runs is the renderer's: `FVoxelSceneProxy::ConvertVertex` classifies
every vertex's normal against 0.7, along world +Z on heightmap worlds. On a planet the renderer turns on
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

For smooth meshing (Marching Cubes and Dual Contouring), materials use triplanar projection to avoid UV seams. The implementation uses Custom HLSL nodes in the Unreal Material Editor.

### MaterialID Encoding

MaterialID is stored in the **UV1.x** channel as a float value. This avoids sRGB gamma conversion issues that would corrupt integer data in vertex colors.

**In the renderer's vertex conversion (`FVoxelLocalVertex::FromVoxelVertex`):**
```cpp
// MaterialID and the face type in UV1 as exact floats
Result.TexCoord1 = FVector2f(static_cast<float>(MaterialID), static_cast<float>(FaceType));
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
- `GradientSampling`, `TileVariation` (float) - a constant 1 (Footprint Sampling and Texture Variation below)
- `MacroVariation` (float) - `MPC_VoxelMaterial` collection parameter (Texture Variation below; the node
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

// UV derivatives from the continuous translated position, scaled by GradientSampling (1)
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
- `GradientSampling`, `TileVariation` (float) - a constant 1 (Texture Variation below)

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

The texture arrays hold one slice per distinct texture, not one per MaterialID (F2.12: on `DemoMaterialAtlas` 14-19 of 22
slices had been copies; test `VoxelWorlds.MaterialAtlas.TextureArrays.SliceTables`), so the triplanar functions cannot sample at the MaterialID. `UVoxelMaterialAtlas::BuildMaterialLUT` writes a fourth row below
the three face rows: texel (MaterialID, 3) holds the material's slice in the albedo array (R), the normal array (G) and
the roughness array (B), from the atlas's slice tables; A is 0. Both triplanar functions take the `MaterialLUT` texture
as an input (wired in `M_VoxelMaster` and `M_VoxelMaster_Fade` from the same `MaterialLUT` parameter the cubic path
reads) and `Load` that texel. The face rows are read by texel too (`MF_CubicAtlasSample`), so the extra row does not
move them. An atlas whose arrays were built with a slice per MaterialID has empty slice tables, and its LUT gives every
MaterialID the slice of its own index. `BuildTextureArrays()` rewrites an existing LUT in place, so materials already
bound to it read the new slices with the new arrays (the editor rebuilds stale arrays when a world binds the atlas, after
the LUT was built). The far field's `FarFieldMaterialPick` produces a MaterialID like every voxel vertex and needs
nothing of its own. `Scripts/setup_texture_array_slices.py` (parent repository) made the material edits, headless.

### Footprint Sampling (visual polish VP2-O1)

At one 512 px tile per metre a pixel of ground 10 m away covers about 5 texels and 50 m away about 26, so a sampler
reading mip 0 picks one texel out of each pixel's footprint, a different one with every sub-pixel camera move and TSR
jitter: aliasing TSR cannot resolve, a noisy-looking terrain. Each projection samples with `SampleGrad` and the UV derivatives of the translated world position, with the
array's own sampler (`AlbedoArraySampler` etc., anisotropic: the arrays are `TEXTUREGROUP_World` with `TF_Default`).
Both functions take `GradientSampling` as a constant 1 (0 gave mip 0, the rule before: `MPC_VoxelMaterial`'s scalar and
its console variable were retired in VoxelWorlds 8). On `PlanetTest`
at four fixed views, a quarter-pixel camera turn changes the ground's pixels by 43.8-45.0 luma (of 255) at mip 0 and
0.6-1.6 with gradients; the ground's high-frequency energy under TSR falls from 55-59 to 3.5-7.3; GPU time per frame
is not higher (P50 5.11-5.22 ms against 4.94-5.07, three bench runs each).

### Terrain Roughness (visual polish VP2-O6)

`M_VoxelMaster` and `M_VoxelMaster_Fade` used to output the sampled roughness times `RoughnessMultiplier`, which is 0 in
the master and in `MI_VoxelDefault`: roughness 0, a mirror. No terrain material has a roughness map (empty slots of the
roughness array hold a placeholder: the first valid texture, the coal ore's packed ORD map), so the multiplier could not
fix it either. The terrain reflected the sky at grazing angles: a pale sheen and hard-edged patches on far, flat ground,
and before VP2-O1 sparkle. Both masters now output `lerp(sampled x RoughnessMultiplier, TerrainRoughness,
ConstantRoughness)`: `TerrainRoughness` is a scalar parameter (0.9 by default), `ConstantRoughness` a constant 1 (0 was
the rule before; `MPC_VoxelMaterial`'s scalar was retired in VoxelWorlds 8). On `PlanetTest` the
roughness buffer reads 0 → 0.9 on the ground, and the far ground at a grazing view loses its sky sheen (blue channel
112 → 86, saturation 41.6 → 66.0). When real roughness maps exist, a `ConstantRoughness` of 0 and a
`RoughnessMultiplier` of 1 use them.

### Texture Variation (visual polish VP2-O2)

With mips in use (VP2-O1) the one-metre tiles repeat visibly: rows of the same marks close up, a plaid grid on flat
ground at mid distance (F2.8). Both triplanar functions include `Shaders/Private/VoxelTextureVariation.ush`:

- **Tile variation** (`TileVariation`, a constant 1): the ground becomes a mosaic of
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
collection scalars; `VoxelWorlds.Rendering.TextureVariation.Assets` checks the wiring. The tile-lattice autocorrelation
of the ground from above fell from 0.281 to 0.049 (the plaid on far ground gone) at no GPU cost (VP2-O2; history:
`VISUAL_POLISH_PLAN.md` "VP2-O2 texture variation").

---

## Material Assignment

Which material a voxel gets (the biome's surface and depth layers, height rules, ores) is decided at generation; see
[BIOME_SYSTEM.md](BIOME_SYSTEM.md). The material definitions are `FVoxelMaterialDefinition` (`VoxelMaterialDefinition.h`)
in `FVoxelMaterialRegistry`, which an atlas extends (below).

---

## Hardness (decision D31)

`FVoxelMaterialDefinition::Hardness` is how hard a block of the material is to break: the seconds it takes bare-handed.
It is a property of the material, set by the registry's defaults (`BuildDefaultMaterials`, `VoxelMaterialRegistry.cpp`), read with
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
is held (`UVCBlockInteractionComponent::GetBreakSeconds`; bare hands 1, the stone pickaxe 1.5, the iron pickaxe 2),
decided on the server; smooth worlds dig at once (D32).

---

## Runtime Material Binding

The material system uses Dynamic Material Instances to bind texture arrays at runtime.

### UVoxelRegionComponent::UpdateMaterialAtlasParameters

Abridged:

```cpp
void UVoxelRegionComponent::UpdateMaterialAtlasParameters()
{
    DynamicMaterialInstance->SetScalarParameterValue(FName("bSmoothTerrain"), bUseSmoothMeshing ? 1.0f : 0.0f);

    // The material LUT (face rows and the slice row), built if dirty or missing
    if (MaterialAtlas->IsLUTDirty() || !MaterialAtlas->GetMaterialLUT())
    {
        MaterialAtlas->BuildMaterialLUT();
    }
    DynamicMaterialInstance->SetTextureParameterValue(FName("MaterialLUT"), MaterialAtlas->GetMaterialLUT());

    // Cubic: the packed atlases and their grid
    DynamicMaterialInstance->SetTextureParameterValue(FName("PackedAlbedoAtlas"), MaterialAtlas->PackedAlbedoAtlas);
    DynamicMaterialInstance->SetScalarParameterValue(FName("AtlasColumns"), MaterialAtlas->AtlasColumns);
    DynamicMaterialInstance->SetScalarParameterValue(FName("AtlasRows"), MaterialAtlas->AtlasRows);

    // Smooth: the saved arrays; in the editor missing or stale ones are rebuilt first (left unsaved, with a warning)
    MaterialAtlas->EnsureTextureArrays();
    DynamicMaterialInstance->SetTextureParameterValue(FName("AlbedoArray"), MaterialAtlas->AlbedoArray);
    DynamicMaterialInstance->SetTextureParameterValue(FName("NormalArray"), MaterialAtlas->NormalArray);
    DynamicMaterialInstance->SetTextureParameterValue(FName("RoughnessArray"), MaterialAtlas->RoughnessArray);
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

## Memory

The packed atlas is one texture per kind. The arrays hold one 512 x 512 BC-compressed slice per distinct texture:
4.8 MB on the GPU for `DemoMaterialAtlas`, 7.2 MB for `VoxelMaterialAtlas` (800 MB at 4096 RGBA32F before F2.10).

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

The far field ([LOD_SYSTEM.md](LOD_SYSTEM.md) "The Far Field": a planet's, a plane's and an island's) draws with the
same material, and since G45 (decision D28) its quads blend two materials: UV1.x the quad's first material as on
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
gradient across the quad: no texture sample is added. Before, a far-field quad drew one material, the most common of
its corners' picks, so every material border was a staircase of quads (a few km a step at a planet's horizon) (tests
`FarField.Planet.MaterialMix`, `.MaterialBlend`, `FarField.Plane.MaterialMix`). Every voxel chunk vertex has alpha 255 (weight 0) and keeps
`MaterialID`. Vertex colour reaches the shader as UNORM (`VET_Color`, no sRGB decode), so R carries an ID exactly; IDs stay
out of interpolation because both are flat per triangle. The node is added by `Scripts/setup_farfield_material_blend.py`
(parent repository; idempotent, run headless). The VertexColor node's default output is RGB only, so R and A go in as their own
inputs: a `.a` on that output fails the material's compile and every mesh using it draws the default material.

### Material Assets

| Asset | Location | Purpose |
|-------|----------|---------|
| M_VoxelMaster | `/VoxelWorlds/Materials/` | Master material with both paths |
| MI_VoxelDefault | `/VoxelWorlds/Materials/` | Default material instance |
| MF_* | `/VoxelWorlds/Materials/Functions/` | Reusable material functions |

See [MASTER_MATERIAL_SETUP.md](../Guides/MASTER_MATERIAL_SETUP.md) for detailed setup instructions.

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


### Non-Occluding Face Generation (Cubic Mesher)

For non-occluding materials, `ShouldRenderFace()` returns true if **either** the current voxel or the neighbor voxel has `bNonOccluding` set. This generates all boundary faces for visual density.

Non-occluding faces are emitted as individual 1x1 quads in a **pre-pass** before greedy meshing. This prevents them from being merged into larger quads (which would lose per-voxel visual variation).

### The Registry's Table and the Atlas (G100)

`FVoxelMaterialRegistry` answers every query from one immutable table. Until an atlas is bound it is the defaults (IDs
0-21, `BuildDefaultMaterials`), built once by the first query on any thread (a function-local static, which C++ builds
on one thread while the others wait). `SetAtlasPositions` publishes an atlas's table: a new table from the defaults,
grown with placeholders (`Material<ID>`, the default hardness) up to the atlas's highest MaterialID, so an atlas can
define materials without a change to C++, and each configured material's atlas position, scales, name and masked and
non-occluding flags from its config. Each atlas sets the whole table; it does not build on the last atlas's.

The table is published with one atomic pointer: the meshers' workers (the cubic mesher's `IsNonOccluding`, per face) and
the map's read it with no lock and see the old table or the new one, whole. No published table is changed or freed, so
the pointer `GetMaterial` returns and the array `GetAllMaterials` returns stay valid; the registry keeps one table per
distinct content, and an atlas whose table it already holds (`UVoxelRegionComponent::SetMaterialAtlas` calls
`SetAtlasPositions` for every region component the renderer creates, during streaming) republishes that table and logs
nothing. Before G100 the defaults were built lazily with no lock, and every `SetAtlasPositions` grew and rewrote, in
place, the array those workers were reading. Tests `VoxelWorlds.MaterialRegistry.AtlasTable` and
`VoxelWorlds.MaterialRegistry.WorkerReads` (eight workers read the table while the game thread publishes two atlases
800 times: every read is one atlas's table whole).

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

See [MASTER_MATERIAL_SETUP.md](../Guides/MASTER_MATERIAL_SETUP.md) for detailed setup instructions.

---

## Key Files

**C++ Code:**
- `VoxelCore/Public/VoxelMaterialAtlas.h` - Data asset header
- `VoxelCore/Private/VoxelMaterialAtlas.cpp` - BuildTextureArrays(), BuildMaterialLUT()
- `VoxelCore/Private/VoxelMaterialRegistry.cpp` - The registry's defaults and the published table (`SetAtlasPositions`)
- `VoxelRendering/Private/VoxelRegionComponent.cpp` - UpdateMaterialAtlasParameters(), the `voxel.Material.*` switches
- `VoxelRendering/Private/VoxelMeshRenderer.cpp` - Automatic mode sync

**Shaders:**
- `Shaders/Private/VoxelTriplanarCommon.ush` - Triplanar utility functions
- `Shaders/Private/VoxelTextureVariation.ush` - Tile and macro variation (VP2-O2)

**Material Assets (`/VoxelWorlds/Materials/`):**
- `M_VoxelMaster` - Unified master material
- `M_VoxelMaster_Fade` - The crossfade twin (Masked)
- `MI_VoxelDefault` - Default material instance
- `MPC_VoxelMaterial` - `MacroVariation`, set by `voxel.Material.MacroVariation`
- `Functions/MF_GetMaterialID` - MaterialID extraction
- `Functions/MF_TriplanarSampleAlbedoRoughness` - Smooth terrain sampling
- `Functions/MF_TriplanarSampleNormal` - Smooth terrain normals
- `Functions/MF_CubicAtlasSample` - Cubic terrain sampling

See [BIOME_SYSTEM.md](BIOME_SYSTEM.md) for material assignment logic.

