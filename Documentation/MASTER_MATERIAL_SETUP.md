# Master Material Setup Guide

This guide walks through creating the unified voxel master material system.

## Overview

The master material system consists of:
1. **4 Material Functions** - Reusable sampling logic for both terrain modes
   - MF_GetMaterialID - Extract MaterialID from UV1
   - MF_TriplanarSampleAlbedoRoughness - Smooth terrain albedo/roughness
   - MF_TriplanarSampleNormal - Smooth terrain normals with UDN blend
   - MF_CubicAtlasSample - Cubic terrain atlas sampling with MaterialLUT
2. **1 Master Material** - M_VoxelMaster with automatic cubic/smooth switching
3. **1 Default Material Instance** - MI_VoxelDefault for easy customization

## Automatic Mode Selection

The material mode is automatically determined by `UVoxelWorldConfiguration::MeshingMode`:
- `EMeshingMode::Smooth` → Triplanar sampling with Texture2DArrays
- `EMeshingMode::Cubic` → UV-based sampling with PackedAtlas + MaterialLUT

This is synced in `FVoxelCustomVFRenderer::Initialize()` which sets `bUseSmoothMeshing` on the WorldComponent based on the configuration.

## Prerequisites

- Placeholder Texture2DArray assets (for parameter defaults)
- UVoxelMaterialAtlas configured with:
  - **For Smooth**: Source textures in MaterialConfigs (auto-builds Texture2DArrays)
  - **For Cubic**: PackedAlbedoAtlas, PackedNormalAtlas, PackedRoughnessAtlas assigned

---

## Step 1: Create Material Functions

### Location
Create in: `Content/VoxelWorlds/Materials/Functions/`

---

### MF_GetMaterialID

**Purpose**: Extract MaterialID from UV1 channel

**Inputs**:
| Name | Type | Description |
|------|------|-------------|
| MaterialUV | Vector2 | TexCoord[1] from mesh |

**Outputs**:
| Name | Type | Description |
|------|------|-------------|
| MaterialID | Scalar | Integer material index (0-255) |

**Implementation**:
1. Create new Material Function: `MF_GetMaterialID`
2. Check "Expose to Library" in details
3. Add **Function Input** node:
   - Input Name: `MaterialUV`
   - Input Type: Function Input Vector 2
   - Preview Value: (0, 0)
4. Add **Custom** node with code:
```hlsl
return floor(MaterialUV.x + 0.5);
```
   - Input: MaterialUV (Vector2)
   - Output Type: CMOT Float 1
5. Add **Function Output** node:
   - Output Name: `MaterialID`
6. Connect: FunctionInput → Custom → FunctionOutput

---

### MF_TriplanarSampleAlbedoRoughness

**Purpose**: Sample albedo and roughness arrays with triplanar projection

**Inputs**:
| Name | Type | Description |
|------|------|-------------|
| WorldPosition | Vector3 | Absolute World Position |
| WorldNormal | Vector3 | VertexNormalWS |
| MaterialID | Scalar | From MF_GetMaterialID |
| Scale | Scalar | Triplanar tiling (default: 0.01) |
| Sharpness | Scalar | Blend sharpness (default: 4.0) |
| AlbedoArray | Texture2DArray | Albedo texture array |
| RoughnessArray | Texture2DArray | Roughness texture array |

**Outputs**:
| Name | Type | Description |
|------|------|-------------|
| Albedo | Vector3 | RGB color |
| Roughness | Scalar | Roughness value |

**Implementation**:
1. Create new Material Function: `MF_TriplanarSampleAlbedoRoughness`
2. Check "Expose to Library"
3. Add **Function Inputs**:
   - `WorldPosition` (Vector3)
   - `WorldNormal` (Vector3)
   - `MaterialID` (Scalar)
   - `Scale` (Scalar, Preview: 0.01)
   - `Sharpness` (Scalar, Preview: 4.0)
   - `AlbedoArray` (Texture2DArray)
   - `RoughnessArray` (Texture2DArray)

4. Add **Custom** node with this code:

```hlsl
int MatID = (int)MaterialID;

// Calculate triplanar blend weights
float3 Weights = pow(abs(WorldNormal), Sharpness);
Weights /= max(Weights.x + Weights.y + Weights.z, 0.0001);

// Calculate triplanar UVs
float2 UV_X = WorldPosition.zy * Scale;
float2 UV_Y = WorldPosition.xz * Scale;
float2 UV_Z = WorldPosition.xy * Scale;

// Get sampler
SamplerState Samp = View.MaterialTextureBilinearWrapedSampler;

// Sample albedo from each projection
float4 Albedo_X = AlbedoArray.SampleLevel(Samp, float3(UV_X, MatID), 0);
float4 Albedo_Y = AlbedoArray.SampleLevel(Samp, float3(UV_Y, MatID), 0);
float4 Albedo_Z = AlbedoArray.SampleLevel(Samp, float3(UV_Z, MatID), 0);
float3 Albedo = Albedo_X.rgb * Weights.x + Albedo_Y.rgb * Weights.y + Albedo_Z.rgb * Weights.z;

// Sample roughness from each projection
float Rough_X = RoughnessArray.SampleLevel(Samp, float3(UV_X, MatID), 0).r;
float Rough_Y = RoughnessArray.SampleLevel(Samp, float3(UV_Y, MatID), 0).r;
float Rough_Z = RoughnessArray.SampleLevel(Samp, float3(UV_Z, MatID), 0).r;
float Roughness = Rough_X * Weights.x + Rough_Y * Weights.y + Rough_Z * Weights.z;

return float4(Albedo, Roughness);
```

   **Custom Node Settings**:
   - Inputs (add in order, names must match EXACTLY):
     - `WorldPosition` (Vector3)
     - `WorldNormal` (Vector3)
     - `MaterialID` (Scalar)
     - `Scale` (Scalar)
     - `Sharpness` (Scalar)
     - `AlbedoArray` (Texture2DArray)
     - `RoughnessArray` (Texture2DArray)
   - Output Type: CMOT Float 4

5. Add **Component Mask** nodes to split the output:
   - RGB mask → Albedo output
   - A mask → Roughness output

6. Add **Function Outputs**:
   - `Albedo` (Vector3)
   - `Roughness` (Scalar)

7. Connect all inputs and outputs

---

### MF_TriplanarSampleNormal

**Purpose**: Sample normal array with triplanar projection and UDN blend

**Inputs**:
| Name | Type | Description |
|------|------|-------------|
| WorldPosition | Vector3 | Absolute World Position |
| WorldNormal | Vector3 | VertexNormalWS |
| MaterialID | Scalar | From MF_GetMaterialID |
| Scale | Scalar | Triplanar tiling (default: 0.01) |
| Sharpness | Scalar | Blend sharpness (default: 4.0) |
| NormalArray | Texture2DArray | Normal map texture array |

**Outputs**:
| Name | Type | Description |
|------|------|-------------|
| Normal | Vector3 | World-space blended normal |

**Implementation**:
1. Create new Material Function: `MF_TriplanarSampleNormal`
2. Check "Expose to Library"
3. Add **Function Inputs**:
   - `WorldPosition` (Vector3)
   - `WorldNormal` (Vector3)
   - `MaterialID` (Scalar)
   - `Scale` (Scalar, Preview: 0.01)
   - `Sharpness` (Scalar, Preview: 4.0)
   - `NormalArray` (Texture2DArray)

4. Add **Custom** node with this code:

```hlsl
int MatID = (int)MaterialID;

// Calculate triplanar blend weights
float3 Weights = pow(abs(WorldNormal), Sharpness);
Weights /= max(Weights.x + Weights.y + Weights.z, 0.0001);

// Calculate triplanar UVs
float2 UV_X = WorldPosition.zy * Scale;
float2 UV_Y = WorldPosition.xz * Scale;
float2 UV_Z = WorldPosition.xy * Scale;

// Get sampler
SamplerState Samp = View.MaterialTextureBilinearWrapedSampler;

// Sample and decode tangent-space normals
float3 TN_X = NormalArray.SampleLevel(Samp, float3(UV_X, MatID), 0).rgb * 2.0 - 1.0;
float3 TN_Y = NormalArray.SampleLevel(Samp, float3(UV_Y, MatID), 0).rgb * 2.0 - 1.0;
float3 TN_Z = NormalArray.SampleLevel(Samp, float3(UV_Z, MatID), 0).rgb * 2.0 - 1.0;

// UDN blend: Project tangent-space normals to world-space derivatives
float3 N_X = float3(0, TN_X.y, -TN_X.x);
float3 N_Y = float3(TN_Y.x, 0, -TN_Y.y);
float3 N_Z = float3(TN_Z.xy, 0);

// Blend weighted normals and add to base geometry normal
float3 BlendedNormal = normalize(N_X * Weights.x + N_Y * Weights.y + N_Z * Weights.z + WorldNormal);

return BlendedNormal;
```

   **Custom Node Settings**:
   - Inputs (add in order, names must match EXACTLY):
     - `WorldPosition` (Vector3)
     - `WorldNormal` (Vector3)
     - `MaterialID` (Scalar)
     - `Scale` (Scalar)
     - `Sharpness` (Scalar)
     - `NormalArray` (Texture2DArray)
   - Output Type: CMOT Float 3

5. Add **Function Output**:
   - `Normal` (Vector3)

6. Connect: Inputs → Custom → Output

---

### MF_CubicAtlasSample

**Purpose**: Sample packed atlas textures using UV0 + MaterialLUT for cubic terrain

**Inputs**:
| Name | Type | Description |
|------|------|-------------|
| FaceUV | Vector2 | TexCoord[0] - local face UVs |
| MaterialID | Scalar | From UV1.x (component masked) |
| FaceType | Scalar | From UV1.y (0=Top, 1=Side, 2=Bottom) |
| AtlasColumns | Scalar | Number of columns in atlas |
| AtlasRows | Scalar | Number of rows in atlas |
| MaterialLUT | Texture2D | Material lookup table (256x3) |
| AlbedoAtlas | Texture2D | Packed albedo atlas |
| NormalAtlas | Texture2D | Packed normal atlas |
| RoughnessAtlas | Texture2D | Packed roughness atlas |

**Outputs**:
| Name | Type | Description |
|------|------|-------------|
| Albedo | Vector3 | RGB color |
| Roughness | Scalar | Roughness value |
| Normal | Vector3 | Tangent-space normal |

**Implementation**:
1. Create new Material Function: `MF_CubicAtlasSample`
2. Check "Expose to Library"
3. Add all **Function Inputs** as listed above

4. Add **Custom** node for Albedo/Roughness:

```hlsl
// Sample MaterialLUT to get atlas tile position
float2 LutUV = float2((MaterialID + 0.5) / 256.0, (FaceType + 0.5) / 3.0);
float4 LutSample = MaterialLUT.SampleLevel(View.MaterialTextureBilinearClampedSampler, LutUV, 0);

float Column = LutSample.r * 255.0;
float Row = LutSample.g * 255.0;

float TileU = 1.0 / AtlasColumns;
float TileV = 1.0 / AtlasRows;

float2 AtlasUV = float2(
    (Column + frac(FaceUV.x)) * TileU,
    (Row + frac(FaceUV.y)) * TileV
);

SamplerState Samp = View.MaterialTextureBilinearWrapedSampler;
float3 Albedo = AlbedoAtlas.SampleLevel(Samp, AtlasUV, 0).rgb;

// Roughness with fallback for missing texture
float RoughnessSample = RoughnessAtlas.SampleLevel(Samp, AtlasUV, 0).r;
float Roughness = RoughnessSample < 0.01 ? 0.5 : RoughnessSample;

return float4(Albedo, Roughness);
```

5. Add second **Custom** node for Normal:

```hlsl
float2 LutUV = float2((MaterialID + 0.5) / 256.0, (FaceType + 0.5) / 3.0);
float4 LutSample = MaterialLUT.SampleLevel(View.MaterialTextureBilinearClampedSampler, LutUV, 0);

float Column = LutSample.r * 255.0;
float Row = LutSample.g * 255.0;

float TileU = 1.0 / AtlasColumns;
float TileV = 1.0 / AtlasRows;

float2 AtlasUV = float2(
    (Column + frac(FaceUV.x)) * TileU,
    (Row + frac(FaceUV.y)) * TileV
);

float3 NormalSample = NormalAtlas.SampleLevel(View.MaterialTextureBilinearWrapedSampler, AtlasUV, 0).rgb;

// Fallback for missing normal map
float3 TangentNormal;
if (dot(NormalSample, NormalSample) < 0.01)
{
    TangentNormal = float3(0, 0, 1);
}
else
{
    TangentNormal = NormalSample * 2.0 - 1.0;
}

return TangentNormal;
```

6. Add **Component Mask** to split first custom node (RGB → Albedo, A → Roughness)

7. Add **Function Outputs**: `Albedo`, `Roughness`, `Normal`

**Note**: The Normal output is in tangent-space. In M_VoxelMaster, use a **Transform** node (Tangent→World) before the Lerp.

---

## Step 2: Create Master Material (M_VoxelMaster)

### Location
Create in: `Content/VoxelWorlds/Materials/M_VoxelMaster`

### Material Settings
- **Blend Mode**: Opaque
- **Shading Model**: Default Lit
- **Two Sided**: false
- **Use Material Attributes**: false
- **Tangent Space Normal**: OFF (we output world-space normals)

### Parameters to Create

**Scalar Parameters**:
| Name | Default | Group | Description |
|------|---------|-------|-------------|
| bSmoothTerrain | 1 | Mode | 1=Smooth (triplanar), 0=Cubic (UV atlas) |
| bDebugMode | 0 | Debug | 1=Show MaterialID colors |
| TriplanarScale | 0.01 | Triplanar | UV tiling for smooth terrain |
| TriplanarSharpness | 4.0 | Triplanar | Blend sharpness (1-8) |
| AtlasColumns | 4 | CubicAtlas | Packed atlas grid columns |
| AtlasRows | 4 | CubicAtlas | Packed atlas grid rows |
| RoughnessMultiplier | 1.0 | Surface | Roughness adjustment |
| MetallicValue | 0.0 | Surface | Base metallic value |

**Texture2DArray Parameters** (for Smooth terrain):
| Name | Group | Sampler Type |
|------|-------|--------------|
| AlbedoArray | TextureArrays | Use "Texture Sample Parameter 2D Array" node |
| NormalArray | TextureArrays | Use "Texture Sample Parameter 2D Array" node |
| RoughnessArray | TextureArrays | Use "Texture Sample Parameter 2D Array" node |

**Texture2D Parameters** (for Cubic terrain):
| Name | Group | Description |
|------|-------|-------------|
| MaterialLUT | CubicAtlas | Material lookup table (auto-built) |
| PackedAlbedoAtlas | CubicAtlas | Packed albedo atlas texture |
| PackedNormalAtlas | CubicAtlas | Packed normal atlas texture |
| PackedRoughnessAtlas | CubicAtlas | Packed roughness atlas texture |

### Material Graph Structure

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              M_VoxelMaster                                  │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  INPUTS:                                                                    │
│  ┌──────────────┐                                                           │
│  │TexCoord[0]   │─────────────────────────────────► FaceUV (Cubic)          │
│  └──────────────┘                                                           │
│  ┌──────────────┐     ┌─────────────────┐                                   │
│  │TexCoord[1]   │──┬─►│ MF_GetMaterialID│──► MaterialID (Smooth)            │
│  │              │  │  └─────────────────┘                                   │
│  │              │  ├─► Mask R ──► MaterialID (Cubic)                        │
│  │              │  └─► Mask G ──► FaceType (Cubic)                          │
│  └──────────────┘                                                           │
│  ┌──────────────┐                                                           │
│  │AbsWorldPos   │──────────────────────────────────► WorldPosition          │
│  └──────────────┘                                                           │
│  ┌──────────────┐                                                           │
│  │VertexNormalWS│──────────────────────────────────► WorldNormal            │
│  └──────────────┘                                                           │
│                                                                             │
│  SMOOTH PATH (bSmoothTerrain = 1):                                          │
│  ┌───────────────────────────────────┐                                      │
│  │ MF_TriplanarSampleAlbedoRoughness │──► Smooth_Albedo, Smooth_Roughness   │
│  │   (uses Texture2DArrays)          │                                      │
│  └───────────────────────────────────┘                                      │
│  ┌───────────────────────────────────┐                                      │
│  │ MF_TriplanarSampleNormal          │──► Smooth_Normal (World Space)       │
│  │   (UDN blend)                     │                                      │
│  └───────────────────────────────────┘                                      │
│                                                                             │
│  CUBIC PATH (bSmoothTerrain = 0):                                           │
│  ┌───────────────────────────────────┐                                      │
│  │ MF_CubicAtlasSample               │──► Cubic_Albedo, Cubic_Roughness     │
│  │   (uses PackedAtlas + MaterialLUT)│──► Cubic_Normal (Tangent Space)      │
│  └───────────────────────────────────┘                                      │
│                      │                                                      │
│                      ▼                                                      │
│  ┌───────────────────────────────────┐                                      │
│  │ Transform (Tangent → World)       │──► Cubic_Normal (World Space)        │
│  └───────────────────────────────────┘                                      │
│                                                                             │
│  MODE SELECTION (Lerp with bSmoothTerrain as Alpha):                        │
│  ┌─────────────────────────────────────────────────────────────┐            │
│  │ Lerp_Albedo:    A=Cubic_Albedo    B=Smooth_Albedo           │            │
│  │ Lerp_Roughness: A=Cubic_Roughness B=Smooth_Roughness        │            │
│  │ Lerp_Normal:    A=Cubic_Normal    B=Smooth_Normal           │            │
│  └─────────────────────────────────────────────────────────────┘            │
│                      │                                                      │
│                      ▼                                                      │
│  ┌─────────────────────────────────────────────────────────────┐            │
│  │ Debug Lerp (optional): Shows MaterialID colors when enabled │            │
│  └─────────────────────────────────────────────────────────────┘            │
│                      │                                                      │
│                      ▼                                                      │
│  ┌─────────────────────────────────────────────────────────────┐            │
│  │ MATERIAL OUTPUTS:                                           │            │
│  │   Base Color ◄── Lerp_Albedo (or Debug color)               │            │
│  │   Roughness  ◄── Lerp_Roughness × RoughnessMultiplier       │            │
│  │   Metallic   ◄── MetallicValue parameter                    │            │
│  │   Normal     ◄── Lerp_Normal (World Space)                  │            │
│  └─────────────────────────────────────────────────────────────┘            │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Detailed Node Setup

#### 1. Input Nodes
- **TextureCoordinate** (Index 1) - For MaterialID
- **Absolute World Position** - For triplanar UVs
- **VertexNormalWS** - For triplanar blending (NOT PixelNormalWS)

#### 2. Get MaterialID
- Add **MF_GetMaterialID** function call
- Connect TexCoord[1] → MaterialUV input

#### 3. Smooth Terrain Path
- Add **MF_TriplanarSampleAlbedoRoughness**:
  - WorldPosition ← Absolute World Position
  - WorldNormal ← VertexNormalWS
  - MaterialID ← MF_GetMaterialID output
  - Scale ← TriplanarScale parameter
  - Sharpness ← TriplanarSharpness parameter
  - AlbedoArray ← AlbedoArray parameter
  - RoughnessArray ← RoughnessArray parameter

- Add **MF_TriplanarSampleNormal**:
  - Same inputs as above (except texture arrays)
  - NormalArray ← NormalArray parameter

- Multiply Roughness output by RoughnessMultiplier parameter

#### 4. Cubic Terrain Path
- Add **MF_CubicAtlasSample**:
  - FaceUV ← TexCoord[0]
  - MaterialID ← TexCoord[1].x (Component Mask R)
  - FaceType ← TexCoord[1].y (Component Mask G)
  - AtlasColumns ← AtlasColumns parameter
  - AtlasRows ← AtlasRows parameter
  - MaterialLUT ← MaterialLUT parameter
  - AlbedoAtlas ← PackedAlbedoAtlas parameter
  - NormalAtlas ← PackedNormalAtlas parameter
  - RoughnessAtlas ← PackedRoughnessAtlas parameter

- Add **Transform** node after MF_CubicAtlasSample Normal output:
  - Source: Tangent Space
  - Destination: World Space
  - This converts cubic tangent-space normals to world-space

#### 5. Mode Selection with Lerp Nodes
Since bSmoothTerrain is a scalar (not static switch), use **Lerp** nodes:

- **Lerp_Albedo**: A=Cubic_Albedo, B=Smooth_Albedo, Alpha=bSmoothTerrain
- **Lerp_Roughness**: A=Cubic_Roughness, B=Smooth_Roughness, Alpha=bSmoothTerrain
- **Lerp_Normal**: A=Cubic_Normal (transformed), B=Smooth_Normal, Alpha=bSmoothTerrain

#### 6. Debug Mode
- **Lerp_Debug**: A=Lerp_Albedo, B=DebugColor, Alpha=bDebugMode

#### 6. Debug Visualization
Add a Custom node for debug colors:
```hlsl
// Generate distinct color from MaterialID
float3 Color;
Color.r = frac(MaterialID * 0.381966);
Color.g = frac(MaterialID * 0.618034);
Color.b = frac(MaterialID * 0.127);
return Color;
```

#### 7. Final Connections
Connect to Material Outputs:
- Base Color ← bDebugMode Switch (Debug color OR Albedo)
- Roughness ← Roughness * RoughnessMultiplier (or 0.5 in debug)
- Metallic ← MetallicValue parameter
- Normal ← bSmoothTerrain Switch output (World Space)

---

## Step 3: Create Default Material Instance

### Location
Create in: `Content/VoxelWorlds/Materials/MI_VoxelDefault`

### Setup
1. Right-click M_VoxelMaster → Create Material Instance
2. Name it `MI_VoxelDefault`
3. Set default parameter values:
   - bSmoothTerrain: true
   - bDebugMode: false
   - TriplanarScale: 0.01
   - TriplanarSharpness: 4.0
   - RoughnessMultiplier: 1.0
   - MetallicValue: 0.0

4. Assign placeholder texture arrays (will be overridden at runtime)

---

## Step 4: Update VoxelWorldComponent

The component should use M_VoxelMaster as the base material and create a Dynamic Material Instance from it.

### Code Changes
See `VoxelWorldComponent.cpp` - CreateVoxelMaterialInstance() should:
1. Load M_VoxelMaster (or accept it as a parameter)
2. Create Dynamic Material Instance
3. Call UpdateMaterialAtlasParameters() to bind texture arrays

---

## Testing Checklist

- [ ] MF_GetMaterialID compiles and extracts correct ID
- [ ] MF_TriplanarSampleAlbedoRoughness samples textures correctly
- [ ] MF_TriplanarSampleNormal produces correct normals (no plaid)
- [ ] MF_CubicAtlasSample samples atlas correctly using MaterialLUT
- [ ] M_VoxelMaster compiles without errors
- [ ] Smooth terrain displays correctly (set MeshingMode=Smooth in config)
- [ ] Cubic terrain displays correctly (set MeshingMode=Cubic in config)
- [ ] Mode automatically switches based on configuration
- [ ] Debug mode shows distinct colors per MaterialID
- [ ] Texture arrays bind correctly at runtime
- [ ] Material Instance overrides work

---

## Troubleshooting

### "Undeclared identifier" errors in Custom nodes
- Verify input names match EXACTLY (case-sensitive)
- Ensure all inputs are connected before compiling

### "Cannot cast from TextureExternal to Texture2DArray"
- Use "Texture Sample Parameter 2D Array" nodes for array textures
- Don't set Sampler Type to "External" for Texture2DArrays

### Grey terrain
- Check texture arrays are built (BuildTextureArrays())
- Verify Dynamic Material Instance is created
- Check placeholder textures are assigned to parameters
- For cubic: Ensure PackedAlbedoAtlas is assigned in MaterialAtlas

### Plaid pattern on normals (Smooth terrain)
- Ensure using UDN blend code (not simple average)
- Verify using VertexNormalWS, not PixelNormalWS

### Bright/glowing terrain (Cubic mode)
- Missing normal/roughness atlas textures cause extreme specular
- Assign PackedNormalAtlas (flat normal map: #8080FF)
- Assign PackedRoughnessAtlas (mid-grey: #808080)
- Or use fallback code in MF_CubicAtlasSample that defaults to 0.5 roughness

### Ray tracing errors
- Use SampleLevel() instead of Sample()
- Explicit mip level 0 for all samples

### Wrong terrain mode displayed
- Verify MeshingMode is set correctly in UVoxelWorldConfiguration
- Check logs for "MeshingMode=Smooth/Cubic, bUseSmoothMeshing=true/false"
- The mode is synced in FVoxelCustomVFRenderer::Initialize()
