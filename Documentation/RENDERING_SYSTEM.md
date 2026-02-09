# Rendering System Architecture

**Module**: VoxelRendering  
**Dependencies**: VoxelCore

## Table of Contents

1. [Overview](#overview)
2. [Hybrid Architecture](#hybrid-architecture)
3. [Vertex Factory Evolution](#vertex-factory-evolution)
4. [Current Implementation](#current-implementation)
5. [Renderer Interface](#renderer-interface)
6. [Custom Vertex Factory](#custom-vertex-factory)
7. [PMC Renderer](#pmc-renderer)
8. [LOD Morphing System](#lod-morphing-system)
9. [Vertex Color Encoding](#vertex-color-encoding)
10. [Collision System](#collision-system)
11. [Integration Flow](#integration-flow)
12. [Performance Comparison](#performance-comparison)
13. [Implementation Guide](#implementation-guide)

---

## Overview

The rendering system uses a **hybrid architecture** with two implementations of the `IVoxelMeshRenderer` interface:

1. **Custom Vertex Factory** (Runtime) - GPU-driven, maximum performance
2. **ProceduralMeshComponent** (Editor/Tools) - Simple fallback, full UE compatibility

This design allows shipping with optimized runtime performance while maintaining editor tool simplicity.

### Design Goals

- **Performance**: Zero CPU overhead for runtime rendering
- **Flexibility**: Swappable renderer implementations
- **Compatibility**: Full Unreal Engine feature support where needed
- **Maintainability**: Clean interface separation

---

## Hybrid Architecture

```
IVoxelMeshRenderer (Interface)
    │
    ├── FVoxelCustomVFRenderer (Runtime)
    │   ├── FVoxelVertexFactory (Custom VF)
    │   ├── FVoxelSceneProxy (Scene integration)
    │   └── GPU Buffers (Direct compute → render)
    │
    └── FVoxelPMCRenderer (Editor/Tools)
        ├── ProceduralMeshComponents
        └── CPU-side mesh generation
```

### When to Use Each

**Custom Vertex Factory**:
- ✅ Packaged game (runtime)
- ✅ Play-in-editor (PIE)
- ✅ Maximum performance needed
- ✅ Large-scale worlds (1000+ chunks)

**PMC Renderer**:
- ✅ Editor viewport (non-PIE)
- ✅ Debugging mesh generation
- ✅ Prototyping/development
- ✅ Tools that need mesh data on CPU

---

## Vertex Factory Evolution

### Original Design: Custom FVoxelVertexFactory

The initial design called for a fully custom `FVoxelVertexFactory` that would:
- Use an optimized 28-byte vertex format (`FVoxelVertex`)
- Support LOD morphing via uniform buffer parameters
- Directly bind GPU compute shader output buffers
- Provide maximum control over vertex shader behavior

### Issues Encountered

During implementation, the custom vertex factory exhibited persistent rendering problems:

1. **Terrain attached to camera**: Geometry followed camera movement instead of staying in world space
2. **Translucent appearance**: Meshes rendered as if depth testing was disabled
3. **Materials going blank**: After certain operations, all materials would stop rendering
4. **World coordinate issues**: UE5's Large World Coordinates (LWC) and Translated World Space caused positioning problems

These issues proved difficult to diagnose and resolve despite:
- Multiple architectural approaches (component-owned vs renderer-owned vertex factory)
- Various initialization patterns (direct RHI buffer binding, RDG conversion)
- Different shader permutation configurations

### Decision: FLocalVertexFactory

After extensive debugging, the project switched to using Epic's **FLocalVertexFactory**:

**Rationale:**
- Battle-tested implementation that works reliably with UE5's rendering pipeline
- Proper handling of LWC and Translated World Space
- Full material compatibility without special setup
- Maintained GPU efficiency (vertex data stays on GPU)

**Tradeoffs:**
- Slightly larger vertex format (32 bytes vs 28 bytes)
- LOD morphing requires Material Parameter Collection instead of vertex shader uniforms
- Less control over vertex shader behavior

**Conclusion:** The stability and reliability of FLocalVertexFactory outweighs the minor performance overhead of the larger vertex format.

---

## Current Implementation

### Architecture Overview

```
AVoxelWorldTestActor
    │
    ├── UVoxelChunkManager (component)
    │   └── Coordinates chunk streaming
    │
    └── IVoxelMeshRenderer (interface)
        │
        ├── FVoxelCustomVFRenderer (GPU-driven)
        │   └── UVoxelWorldComponent
        │       └── FVoxelSceneProxy
        │           └── FLocalVertexFactory (per chunk)
        │
        └── FVoxelPMCRenderer (CPU fallback)
            └── ProceduralMeshComponents
```

### Key Components

**FVoxelCustomVFRenderer** (`VoxelRendering/Private/VoxelCustomVFRenderer.cpp`)
- Implements `IVoxelMeshRenderer` interface
- Creates and manages `UVoxelWorldComponent` internally
- Forwards chunk updates and LOD configuration to the component

**UVoxelWorldComponent** (`VoxelRendering/Public/VoxelWorldComponent.h`)
- `UPrimitiveComponent` that bridges game thread and render thread
- Creates `FVoxelSceneProxy` for actual rendering
- Manages Material Parameter Collection for LOD morphing
- Exposes statistics and chunk queries

**FVoxelSceneProxy** (`VoxelRendering/Private/VoxelSceneProxy.cpp`)
- Manages per-chunk `FLocalVertexFactory` instances
- Performs frustum culling via ViewProjectionMatrix
- Issues draw calls in `GetDynamicMeshElements()`
- Handles game→render thread chunk buffer updates

**FVoxelLocalVertex** (`VoxelRendering/Public/VoxelLocalVertexFactory.h`)
- 32-byte vertex format compatible with FLocalVertexFactory
- Converts from `FVoxelVertex` (28-byte mesher output)
- Encodes MaterialID, BiomeID, and AO in vertex color

### Data Flow

```
[Mesher Output]
    FVoxelVertex (28 bytes)
    ↓
[Conversion in UpdateChunkBuffers_RenderThread]
    FVoxelLocalVertex (32 bytes)
    ↓
[GPU Buffer Creation]
    FBufferRHIRef (vertex + index)
    ↓
[FLocalVertexFactory Setup]
    Stream components bound
    ↓
[GetDynamicMeshElements]
    Frustum culling → FMeshBatch → Draw
```

---

## LOD Morphing System

### Material Parameter Collection Approach

Since FLocalVertexFactory doesn't support custom uniform buffers for per-chunk data, LOD morphing is achieved via **Material Parameter Collection (MPC)**:

1. **MPC Asset** (`MPC_VoxelLOD`) contains scalar parameters:
   - `LODStartDistance`: Distance where MorphFactor = 0
   - `LODEndDistance`: Distance where MorphFactor = 1
   - `LODInvRange`: Pre-calculated 1/(End-Start) for efficiency

2. **Component updates MPC** via `UVoxelWorldComponent::UpdateLODParameterCollection()`

3. **Material calculates per-pixel MorphFactor**:
   ```hlsl
   Distance = length(WorldPosition - CameraPosition)
   MorphFactor = saturate((Distance - LODStartDistance) * LODInvRange)
   ```

### Configuration Pipeline

```
AVoxelWorldTestActor (Details panel)
    │
    ├── LODParameterCollection (MPC asset reference)
    ├── LODStartDistance (float)
    └── LODEndDistance (float)
    │
    ↓ InitializeVoxelWorld()
    │
IVoxelMeshRenderer::SetLODParameterCollection()
    │
    ↓
FVoxelCustomVFRenderer::SetLODParameterCollection()
    │
    ↓
UVoxelWorldComponent::SetLODParameterCollection()
    │
    ↓
UMaterialParameterCollectionInstance::SetScalarParameterValue()
    │
    ↓
Material reads via CollectionParameter nodes
```

### Material Setup

In your voxel material:
1. Add **CollectionParameter** nodes referencing `MPC_VoxelLOD`
2. Calculate distance: `Length(WorldPosition - CameraPositionWS)`
3. Compute MorphFactor: `Saturate((Distance - LODStartDistance) * LODInvRange)`
4. Use MorphFactor for:
   - Dithered opacity (masked material)
   - Detail texture fading
   - Normal map intensity reduction

---

## Vertex Color Encoding

### Channel Layout

The vertex color encodes per-vertex data for material graph access:

| Channel | Data | Range | Material Access |
|---------|------|-------|-----------------|
| R | MaterialID | 0-255 | `round(VertexColor.R * 255)` |
| G | BiomeID | 0-255 | `round(VertexColor.G * 255)` |
| B | AO (top 2 bits) | 0-192 | `floor(VertexColor.B * 4)` → 0-3 |
| A | Reserved | 255 | - |

### AO Calculation

```cpp
// In material:
AO = floor(VertexColor.B * 4);  // 0-3
AOFactor = 1.0 - (AO * 0.25);   // 1.0, 0.75, 0.5, 0.25
FinalColor = BaseColor * AOFactor;
```

### Debug Visualization Modes

Console variable `voxel.VertexColorDebugMode`:
- **0** (Default): Normal encoding (R=MaterialID, G=BiomeID, B=AO)
- **1** (MaterialColors): RGB = MaterialColor * AO
- **2** (BiomeColors): RGB = BiomeHue * AO (Red=0, Green=1, Blue=2, etc.)

Note: Chunks must be re-meshed after changing debug mode.

---

## Renderer Interface

### IVoxelMeshRenderer

```cpp
/**
 * Voxel Mesh Renderer Interface
 * 
 * Abstract interface for rendering voxel chunks. Implementations handle
 * the actual rendering strategy (Custom VF vs PMC).
 * 
 * Thread Safety: All methods called from game thread only.
 */
class IVoxelMeshRenderer {
public:
    virtual ~IVoxelMeshRenderer() = default;
    
    // ==================== Lifecycle ====================
    
    /**
     * Initialize renderer with world and configuration.
     * @param World The UWorld to render in
     * @param Config World configuration
     */
    virtual void Initialize(
        UWorld* World, 
        const FVoxelWorldConfiguration& Config
    ) = 0;
    
    /**
     * Shutdown and cleanup resources.
     */
    virtual void Shutdown() = 0;
    
    // ==================== Mesh Updates ====================
    
    /**
     * Update or create mesh for a chunk.
     * @param RenderData Contains GPU buffers or vertex/index arrays
     */
    virtual void UpdateChunkMesh(const FChunkRenderData& RenderData) = 0;
    
    /**
     * Remove chunk mesh from rendering.
     * @param ChunkCoord Chunk to remove
     */
    virtual void RemoveChunk(FIntVector ChunkCoord) = 0;
    
    /**
     * Clear all chunks.
     */
    virtual void ClearAllChunks() = 0;
    
    // ==================== Material Management ====================
    
    /**
     * Set primary material for all chunks.
     * @param Material Material to use
     */
    virtual void SetMaterial(UMaterialInterface* Material) = 0;
    
    /**
     * Update material parameters (e.g., atlas texture).
     */
    virtual void UpdateMaterialParameters() = 0;
    
    // ==================== LOD Transitions ====================
    
    /**
     * Update LOD transition morph factor for a chunk.
     * @param ChunkCoord Chunk coordinate
     * @param MorphFactor Blend factor 0-1 (0=current LOD, 1=next LOD)
     */
    virtual void UpdateLODTransition(
        FIntVector ChunkCoord, 
        float MorphFactor
    ) = 0;
    
    // ==================== Queries ====================
    
    /**
     * Is this chunk currently loaded/rendered?
     */
    virtual bool IsChunkLoaded(FIntVector ChunkCoord) const = 0;
    
    /**
     * Get number of currently loaded chunks.
     */
    virtual int32 GetLoadedChunkCount() const = 0;
    
    /**
     * Get total GPU memory usage (approximate).
     */
    virtual uint64 GetGPUMemoryUsage() const = 0;
    
    // ==================== Debugging ====================
    
    /**
     * Get debug statistics string.
     */
    virtual FString GetDebugStats() const = 0;
    
    /**
     * Draw debug visualization.
     */
    virtual void DrawDebugVisualization(const FLODQueryContext& Context) const {}
};
```

---

## Custom Vertex Factory

The Custom Vertex Factory implementation provides maximum performance by keeping all data on GPU and avoiding CPU-GPU transfers.

### Architecture

```
ChunkManager
    ↓
FChunkRenderData (GPU buffers)
    ↓
FVoxelCustomVFRenderer::UpdateChunkMesh()
    ↓
Convert RDG → RHI buffers (stays on GPU!)
    ↓
FVoxelVertexFactory::UpdateBuffers()
    ↓
FVoxelSceneProxy::MarkDirty()
    ↓
Rendering (vertex shader reads GPU buffers)
```

### Optimized Vertex Format

```cpp
/**
 * Optimized vertex format for voxel rendering.
 * 
 * Total: 28 bytes per vertex (vs PMC's 48+ bytes)
 * Packed data reduces memory bandwidth and cache misses.
 */
struct FVoxelVertex {
    /** Position in local space (12 bytes) */
    FVector3f Position;
    
    /** Packed normal (10/10/10) + AO (8-bit) + padding (2-bit) (4 bytes) */
    uint32 PackedNormalAndAO;
    
    /** UV coordinates for texturing (8 bytes) */
    FVector2f UV;
    
    /** MaterialID (8) + BiomeID (8) + Metadata (16) (4 bytes) */
    uint32 PackedMaterialData;
    
    // ==================== Packing Utilities ====================
    
    /**
     * Pack normal vector and ambient occlusion into single uint32.
     * Normal uses 10 bits per component, AO uses 8 bits.
     */
    static uint32 PackNormalAndAO(FVector3f Normal, uint8 AO) {
        // Normalize to -1 to 1
        FVector3f N = Normal.GetSafeNormal();
        
        // Convert to 10-bit signed integers (-511 to 511)
        int32 X = FMath::Clamp(FMath::RoundToInt(N.X * 511.0f), -511, 511);
        int32 Y = FMath::Clamp(FMath::RoundToInt(N.Y * 511.0f), -511, 511);
        int32 Z = FMath::Clamp(FMath::RoundToInt(N.Z * 511.0f), -511, 511);
        
        // Pack: XXXXXXXXXXYYYYYYYYYYZZZZZZZZZZAA
        uint32 Packed = 0;
        Packed |= (X & 0x3FF) << 22;  // 10 bits X
        Packed |= (Y & 0x3FF) << 12;  // 10 bits Y
        Packed |= (Z & 0x3FF) << 2;   // 10 bits Z
        Packed |= (AO >> 6) & 0x3;    // 2 bits AO (high bits, rest in normal W)
        
        return Packed;
    }
    
    /**
     * Unpack normal and AO from uint32.
     */
    static void UnpackNormalAndAO(uint32 Packed, FVector3f& OutNormal, uint8& OutAO) {
        // Extract 10-bit signed values
        int32 X = (Packed >> 22) & 0x3FF;
        int32 Y = (Packed >> 12) & 0x3FF;
        int32 Z = (Packed >> 2) & 0x3FF;
        
        // Sign extend (10-bit to 32-bit signed)
        if (X & 0x200) X |= 0xFFFFFC00;
        if (Y & 0x200) Y |= 0xFFFFFC00;
        if (Z & 0x200) Z |= 0xFFFFFC00;
        
        // Convert to normalized float
        OutNormal.X = (float)X / 511.0f;
        OutNormal.Y = (float)Y / 511.0f;
        OutNormal.Z = (float)Z / 511.0f;
        
        // Extract AO
        OutAO = (Packed & 0x3) << 6;
    }
    
    /**
     * Pack material data (MaterialID, BiomeID, Metadata).
     */
    static uint32 PackMaterialData(uint8 MaterialID, uint8 BiomeID, uint16 Metadata) {
        return ((uint32)MaterialID << 24) | ((uint32)BiomeID << 16) | (uint32)Metadata;
    }
    
    /**
     * Unpack material data.
     */
    static void UnpackMaterialData(uint32 Packed, uint8& MaterialID, uint8& BiomeID, uint16& Metadata) {
        MaterialID = (Packed >> 24) & 0xFF;
        BiomeID = (Packed >> 16) & 0xFF;
        Metadata = Packed & 0xFFFF;
    }
};
```

### Vertex Factory Implementation

```cpp
/**
 * Custom Vertex Factory for Voxel Rendering
 * 
 * Handles vertex stream setup and shader bindings for voxel meshes.
 * Integrates with Unreal's rendering pipeline via FVertexFactory.
 */
class FVoxelVertexFactory : public FVertexFactory {
    DECLARE_VERTEX_FACTORY_TYPE(FVoxelVertexFactory);
    
public:
    /**
     * Data type for vertex factory.
     * Defines vertex stream components.
     */
    struct FDataType {
        /** Position stream */
        FVertexStreamComponent PositionComponent;
        
        /** Packed normal + AO stream */
        FVertexStreamComponent PackedNormalAOComponent;
        
        /** UV stream */
        FVertexStreamComponent UVComponent;
        
        /** Material data stream */
        FVertexStreamComponent MaterialDataComponent;
    };
    
    /**
     * Initialize vertex factory with data.
     */
    void Init(const FDataType& InData) {
        Data = InData;
    }
    
    /**
     * Modify shader compilation environment.
     * Defines shader macros for this vertex factory.
     */
    static void ModifyCompilationEnvironment(
        const FVertexFactoryShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FVertexFactory::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        
        OutEnvironment.SetDefine(TEXT("VOXEL_VERTEX_FACTORY"), 1);
        OutEnvironment.SetDefine(TEXT("SUPPORT_LOD_MORPHING"), 1);
        OutEnvironment.SetDefine(TEXT("USE_PACKED_NORMALS"), 1);
    }
    
    /**
     * Should cache this vertex factory for given shader type?
     */
    static bool ShouldCompilePermutation(
        const FVertexFactoryShaderPermutationParameters& Parameters)
    {
        // Only compile for voxel materials
        return Parameters.MaterialParameters.bIsUsedWithVoxelMeshes || 
               Parameters.MaterialParameters.bIsSpecialEngineMaterial;
    }
    
    /**
     * Initialize RHI resources.
     */
    virtual void InitRHI() override {
        // Create vertex declaration
        FVertexDeclarationElementList Elements;
        
        // Position (3x float)
        Elements.Add(AccessStreamComponent(Data.PositionComponent, 0));
        
        // Packed normal + AO (1x uint32)
        Elements.Add(AccessStreamComponent(Data.PackedNormalAOComponent, 1));
        
        // UV (2x float)
        Elements.Add(AccessStreamComponent(Data.UVComponent, 2));
        
        // Material data (1x uint32)
        Elements.Add(AccessStreamComponent(Data.MaterialDataComponent, 3));
        
        // Initialize declaration
        InitDeclaration(Elements);
    }
    
    /**
     * Update vertex buffers from GPU compute shader output.
     * This is the key optimization - buffers stay on GPU!
     */
    void UpdateFromComputeBuffer(
        FRHICommandListImmediate& RHICmdList,
        FRDGBuilder& GraphBuilder,
        FRDGBufferRef ComputeVertexBuffer,
        FRDGBufferRef ComputeIndexBuffer)
    {
        // Convert RDG buffers to persistent RHI buffers
        // Buffers NEVER touch CPU - direct GPU→GPU transfer
        VertexBufferRHI = GraphBuilder.ConvertToExternalBuffer(ComputeVertexBuffer);
        IndexBufferRHI = GraphBuilder.ConvertToExternalBuffer(ComputeIndexBuffer);
        
        // Update stream components to point to new buffers
        FDataType NewData;
        NewData.PositionComponent = FVertexStreamComponent(
            VertexBufferRHI,
            STRUCT_OFFSET(FVoxelVertex, Position),
            sizeof(FVoxelVertex),
            VET_Float3
        );
        NewData.PackedNormalAOComponent = FVertexStreamComponent(
            VertexBufferRHI,
            STRUCT_OFFSET(FVoxelVertex, PackedNormalAndAO),
            sizeof(FVoxelVertex),
            VET_UInt
        );
        NewData.UVComponent = FVertexStreamComponent(
            VertexBufferRHI,
            STRUCT_OFFSET(FVoxelVertex, UV),
            sizeof(FVoxelVertex),
            VET_Float2
        );
        NewData.MaterialDataComponent = FVertexStreamComponent(
            VertexBufferRHI,
            STRUCT_OFFSET(FVoxelVertex, PackedMaterialData),
            sizeof(FVoxelVertex),
            VET_UInt
        );
        
        Init(NewData);
    }
    
private:
    FDataType Data;
    FBufferRHIRef VertexBufferRHI;
    FBufferRHIRef IndexBufferRHI;
};
```

### Scene Proxy

```cpp
/**
 * Scene proxy for voxel world.
 * 
 * Manages rendering of all voxel chunks as a single primitive.
 * Integrates with Unreal's rendering thread.
 */
class FVoxelSceneProxy : public FPrimitiveSceneProxy {
public:
    FVoxelSceneProxy(UVoxelWorldComponent* InComponent)
        : FPrimitiveSceneProxy(InComponent)
        , VertexFactory(GetScene().GetFeatureLevel())
    {
        // Initialize vertex factory
        // Setup material
    }
    
    /**
     * Get dynamic mesh elements for rendering.
     * Called by render thread.
     */
    virtual void GetDynamicMeshElements(
        const TArray<const FSceneView*>& Views,
        const FSceneViewFamily& ViewFamily,
        uint32 VisibilityMap,
        FMeshElementCollector& Collector) const override
    {
        // For each visible chunk
        for (const auto& Pair : ChunkBuffers) {
            FIntVector ChunkCoord = Pair.Key;
            const FVoxelChunkGPUBuffers& Buffers = Pair.Value;
            
            // Create mesh batch
            FMeshBatch& MeshBatch = Collector.AllocateMesh();
            MeshBatch.VertexFactory = &VertexFactory;
            MeshBatch.MaterialRenderProxy = Material->GetRenderProxy();
            MeshBatch.Type = PT_TriangleList;
            MeshBatch.DepthPriorityGroup = SDPG_World;
            
            // Setup mesh element
            FMeshBatchElement& BatchElement = MeshBatch.Elements[0];
            BatchElement.IndexBuffer = Buffers.IndexBuffer;
            BatchElement.FirstIndex = 0;
            BatchElement.NumPrimitives = Buffers.IndexCount / 3;
            BatchElement.MinVertexIndex = 0;
            BatchElement.MaxVertexIndex = Buffers.VertexCount - 1;
            
            // LOD morph factor uniform
            BatchElement.UserData = &Buffers.LODMorphFactor;
            
            Collector.AddMesh(ViewId, MeshBatch);
        }
    }
    
    /**
     * Update chunk GPU buffers.
     * Called when chunk mesh is generated.
     */
    void UpdateChunkBuffers(FIntVector ChunkCoord, const FVoxelChunkGPUBuffers& Buffers) {
        // Thread-safe update (use lock or render command)
        ENQUEUE_RENDER_COMMAND(UpdateVoxelChunkBuffers)(
            [this, ChunkCoord, Buffers](FRHICommandListImmediate& RHICmdList) {
                ChunkBuffers.Add(ChunkCoord, Buffers);
            }
        );
    }
    
    /**
     * Update LOD transition morph factor.
     */
    void SetChunkLODMorphFactor(FIntVector ChunkCoord, float MorphFactor) {
        ENQUEUE_RENDER_COMMAND(SetVoxelChunkLODMorph)(
            [this, ChunkCoord, MorphFactor](FRHICommandListImmediate& RHICmdList) {
                if (FVoxelChunkGPUBuffers* Buffers = ChunkBuffers.Find(ChunkCoord)) {
                    Buffers->LODMorphFactor = MorphFactor;
                }
            }
        );
    }
    
private:
    /** Vertex factory instance */
    FVoxelVertexFactory VertexFactory;
    
    /** Per-chunk GPU buffer handles */
    TMap<FIntVector, FVoxelChunkGPUBuffers> ChunkBuffers;
    
    /** Material */
    UMaterialInterface* Material;
};
```

### Custom VF Renderer Implementation

```cpp
/**
 * GPU-driven voxel mesh renderer using Custom Vertex Factory.
 * 
 * Optimized for runtime performance with zero CPU overhead.
 */
class FVoxelCustomVFRenderer : public IVoxelMeshRenderer {
public:
    virtual void Initialize(UWorld* World, const FVoxelWorldConfiguration& Config) override {
        // Create scene component
        WorldComponent = NewObject<UVoxelWorldComponent>(World);
        WorldComponent->RegisterComponent();
        
        // Create scene proxy
        SceneProxy = MakeShared<FVoxelSceneProxy>(WorldComponent);
        
        // Set material
        SetMaterial(Config.VoxelMaterial);
    }
    
    virtual void UpdateChunkMesh(const FChunkRenderData& RenderData) override {
        // Key optimization: GPU buffers stay on GPU!
        FRDGBuilder GraphBuilder(FRHICommandListExecutor::GetImmediateCommandList());
        
        // Convert RDG → RHI (no CPU readback!)
        FBufferRHIRef VertexBuffer = 
            GraphBuilder.ConvertToExternalBuffer(RenderData.ComputeVertexBuffer);
        FBufferRHIRef IndexBuffer = 
            GraphBuilder.ConvertToExternalBuffer(RenderData.ComputeIndexBuffer);
        
        // Store buffer handles
        FVoxelChunkGPUBuffers Buffers;
        Buffers.VertexBuffer = VertexBuffer;
        Buffers.IndexBuffer = IndexBuffer;
        Buffers.VertexCount = RenderData.VertexCount;
        Buffers.IndexCount = RenderData.IndexCount;
        Buffers.LODMorphFactor = 0.0f;
        
        ChunkBuffers.Add(RenderData.ChunkCoord, Buffers);
        
        // Update scene proxy
        SceneProxy->UpdateChunkBuffers(RenderData.ChunkCoord, Buffers);
    }
    
    virtual void RemoveChunk(FIntVector ChunkCoord) override {
        ChunkBuffers.Remove(ChunkCoord);
        SceneProxy->RemoveChunk(ChunkCoord);
    }
    
    virtual void UpdateLODTransition(FIntVector ChunkCoord, float MorphFactor) override {
        if (FVoxelChunkGPUBuffers* Buffers = ChunkBuffers.Find(ChunkCoord)) {
            Buffers->LODMorphFactor = MorphFactor;
            SceneProxy->SetChunkLODMorphFactor(ChunkCoord, MorphFactor);
        }
    }
    
private:
    /** Scene proxy (rendering thread) */
    TSharedPtr<FVoxelSceneProxy> SceneProxy;
    
    /** World component (game thread) */
    UVoxelWorldComponent* WorldComponent;
    
    /** Per-chunk GPU buffers */
    TMap<FIntVector, FVoxelChunkGPUBuffers> ChunkBuffers;
};
```

### Vertex Shader (HLSL)

```hlsl
// VoxelVertexFactory.usf

#include "/Engine/Private/VertexFactoryCommon.ush"

// Uniform buffer for LOD morphing
BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FVoxelVertexFactoryUniformBuffer, )
    SHADER_PARAMETER(float, LODMorphFactor)
    SHADER_PARAMETER(int, CurrentLODLevel)
    SHADER_PARAMETER(int, NextLODLevel)
END_GLOBAL_SHADER_PARAMETER_STRUCT()

struct FVoxelVertexFactoryInput {
    float3 Position : ATTRIBUTE0;
    uint PackedNormalAO : ATTRIBUTE1;
    float2 UV : ATTRIBUTE2;
    uint PackedMaterialData : ATTRIBUTE3;
};

struct FVoxelVertexFactoryInterpolantsVSToPS {
    float4 Position : SV_POSITION;
    float3 WorldPosition : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float2 UV : TEXCOORD2;
    float AO : TEXCOORD3;
    nointerpolation uint MaterialID : TEXCOORD4;
    nointerpolation uint BiomeID : TEXCOORD5;
};

// Unpack 10/10/10 normal from uint32
float3 UnpackNormal(uint Packed) {
    int X = (int)(Packed >> 22) & 0x3FF;
    int Y = (int)(Packed >> 12) & 0x3FF;
    int Z = (int)(Packed >> 2) & 0x3FF;
    
    // Sign extend
    X = (X << 22) >> 22;
    Y = (Y << 22) >> 22;
    Z = (Z << 22) >> 22;
    
    return normalize(float3(X, Y, Z) / 511.0);
}

// Calculate next LOD position for morphing
float3 CalculateNextLODPosition(float3 CurrentPos, int CurrentLOD) {
    // Snap to next LOD grid
    int Stride = 1 << (CurrentLOD + 1);
    return floor(CurrentPos / Stride) * Stride;
}

// Morph between LOD levels
float3 MorphVertex(float3 CurrentPos, float3 NextLODPos, float MorphFactor) {
    return lerp(CurrentPos, NextLODPos, MorphFactor);
}

FVoxelVertexFactoryInterpolantsVSToPS Main(FVoxelVertexFactoryInput Input) {
    FVoxelVertexFactoryInterpolantsVSToPS Output;
    
    // Unpack normal and AO
    float3 Normal = UnpackNormal(Input.PackedNormalAO);
    float AO = (float)((Input.PackedNormalAO & 0x3) << 6) / 255.0;
    
    // Unpack material data
    uint MaterialID = (Input.PackedMaterialData >> 24) & 0xFF;
    uint BiomeID = (Input.PackedMaterialData >> 16) & 0xFF;
    
    // LOD morphing
    float3 Position = Input.Position;
    if (VoxelUniforms.LODMorphFactor > 0.01) {
        float3 NextLODPos = CalculateNextLODPosition(Position, VoxelUniforms.CurrentLODLevel);
        Position = MorphVertex(Position, NextLODPos, VoxelUniforms.LODMorphFactor);
    }
    
    // Transform to world space
    float3 WorldPosition = mul(float4(Position, 1.0), Primitive.LocalToWorld).xyz;
    float3 WorldNormal = mul(Normal, (float3x3)Primitive.LocalToWorld);
    
    // Output
    Output.Position = mul(float4(WorldPosition, 1.0), View.WorldToClip);
    Output.WorldPosition = WorldPosition;
    Output.Normal = WorldNormal;
    Output.UV = Input.UV;
    Output.AO = AO;
    Output.MaterialID = MaterialID;
    Output.BiomeID = BiomeID;
    
    return Output;
}
```

---

## PMC Renderer

The ProceduralMeshComponent renderer provides a simpler fallback implementation for editor tools and debugging.

### Implementation

```cpp
/**
 * ProceduralMeshComponent-based voxel renderer.
 * 
 * Simpler implementation for editor and tools. Requires CPU-side mesh data.
 */
class FVoxelPMCRenderer : public IVoxelMeshRenderer {
public:
    virtual void Initialize(UWorld* World, const FVoxelWorldConfiguration& Config) override {
        OwnerWorld = World;
        VoxelMaterial = Config.VoxelMaterial;
        
        // Pre-create component pool
        for (int32 i = 0; i < InitialPoolSize; ++i) {
            ComponentPool.Enqueue(CreatePMC());
        }
    }
    
    virtual void UpdateChunkMesh(const FChunkRenderData& RenderData) override {
        // Must readback from GPU (slow!)
        TArray<FVector> Vertices;
        TArray<int32> Triangles;
        TArray<FVector> Normals;
        TArray<FVector2D> UVs;
        TArray<FColor> VertexColors;
        TArray<FProcMeshTangent> Tangents;
        
        // Readback compute buffers to CPU
        ReadbackComputeBuffers(RenderData, Vertices, Triangles, Normals, UVs, VertexColors);
        
        // Get or create PMC for this chunk
        UProceduralMeshComponent* PMC = GetOrCreatePMC(RenderData.ChunkCoord);
        
        // Create mesh section
        PMC->CreateMeshSection(
            0,
            Vertices,
            Triangles,
            Normals,
            UVs,
            VertexColors,
            Tangents,
            true // Create collision
        );
        
        // Set material
        PMC->SetMaterial(0, VoxelMaterial);
        
        ChunkComponents.Add(RenderData.ChunkCoord, PMC);
    }
    
    virtual void RemoveChunk(FIntVector ChunkCoord) override {
        if (UProceduralMeshComponent** PMCPtr = ChunkComponents.Find(ChunkCoord)) {
            UProceduralMeshComponent* PMC = *PMCPtr;
            PMC->ClearAllMeshSections();
            
            // Return to pool
            ComponentPool.Enqueue(PMC);
            
            ChunkComponents.Remove(ChunkCoord);
        }
    }
    
    virtual void UpdateLODTransition(FIntVector ChunkCoord, float MorphFactor) override {
        // PMC doesn't support LOD morphing - just swap meshes at threshold
        if (MorphFactor > 0.5f && !ChunkComponents.Contains(ChunkCoord)) {
            // Regenerate at next LOD
            // (Handled by chunk manager)
        }
    }
    
private:
    /** Owner world */
    UWorld* OwnerWorld;
    
    /** Voxel material */
    UMaterialInterface* VoxelMaterial;
    
    /** Per-chunk PMC instances */
    TMap<FIntVector, UProceduralMeshComponent*> ChunkComponents;
    
    /** PMC pool for reuse */
    TQueue<UProceduralMeshComponent*> ComponentPool;
    
    static constexpr int32 InitialPoolSize = 100;
    
    /**
     * Create new PMC.
     */
    UProceduralMeshComponent* CreatePMC() {
        UProceduralMeshComponent* PMC = NewObject<UProceduralMeshComponent>(OwnerWorld);
        PMC->bUseAsyncCooking = true;
        PMC->RegisterComponent();
        return PMC;
    }
    
    /**
     * Get existing PMC or create new one.
     */
    UProceduralMeshComponent* GetOrCreatePMC(FIntVector ChunkCoord) {
        if (UProceduralMeshComponent** Existing = ChunkComponents.Find(ChunkCoord)) {
            return *Existing;
        }
        
        UProceduralMeshComponent* PMC;
        if (ComponentPool.Dequeue(PMC)) {
            return PMC;
        }
        
        return CreatePMC();
    }
    
    /**
     * Readback GPU buffers to CPU arrays.
     * THIS IS THE BOTTLENECK - avoid in runtime!
     */
    void ReadbackComputeBuffers(
        const FChunkRenderData& RenderData,
        TArray<FVector>& OutVertices,
        TArray<int32>& OutTriangles,
        TArray<FVector>& OutNormals,
        TArray<FVector2D>& OutUVs,
        TArray<FColor>& OutVertexColors)
    {
        // Read vertex buffer from GPU
        TArray<FVoxelVertex> GPUVertices;
        ReadBuffer(RenderData.ComputeVertexBuffer, GPUVertices);
        
        // Read index buffer from GPU
        TArray<uint32> GPUIndices;
        ReadBuffer(RenderData.ComputeIndexBuffer, GPUIndices);
        
        // Convert to PMC format
        OutVertices.Reserve(GPUVertices.Num());
        OutNormals.Reserve(GPUVertices.Num());
        OutUVs.Reserve(GPUVertices.Num());
        OutVertexColors.Reserve(GPUVertices.Num());
        
        for (const FVoxelVertex& Vertex : GPUVertices) {
            OutVertices.Add(FVector(Vertex.Position));
            
            FVector3f Normal;
            uint8 AO;
            FVoxelVertex::UnpackNormalAndAO(Vertex.PackedNormalAndAO, Normal, AO);
            OutNormals.Add(FVector(Normal));
            
            OutUVs.Add(FVector2D(Vertex.UV));
            
            // Pack AO into vertex color
            OutVertexColors.Add(FColor(AO, AO, AO, 255));
        }
        
        OutTriangles.Reserve(GPUIndices.Num());
        for (uint32 Index : GPUIndices) {
            OutTriangles.Add((int32)Index);
        }
    }
};
```

---

## Collision System

Collision is managed separately from visual rendering, using a fully async pipeline to eliminate game-thread stutter.

### Architecture

The collision system uses a three-stage async pipeline:

1. **Data Preparation** (game thread, lightweight): Copy voxel data, merge edits, extract neighbor slices
2. **Mesh Generation + Trimesh Construction** (thread pool, ~3-5ms): Run `GenerateMeshCPU()` at collision LOD, build `Chaos::FTriangleMeshImplicitObject`
3. **Physics Registration** (game thread, ~0.5ms): Create `UBodySetup`, register collision component

```cpp
/**
 * Voxel Collision Manager — Async Pipeline
 *
 * Manages physics collision for voxel chunks independently from visual mesh.
 * Heavy work (mesh gen + trimesh) runs on thread pool; only lightweight
 * physics registration happens on the game thread.
 */
class UVoxelCollisionManager : public UObject {
public:
    // Called every frame from ChunkManager tick
    void Update(const FVector& ViewerPosition, float DeltaTime) {
        // 1. Drain completed async results (game thread, ~0.5ms per apply)
        ProcessCompletedCollisionCooks();

        // 2. Update collision decisions (distance-based load/unload)
        if (ViewerMovedEnough || bPendingInitialUpdate) {
            UpdateCollisionDecisions(ViewerPosition);
        }

        // 3. Process dirty chunks from edits (just queues requests)
        ProcessDirtyChunks(ViewerPosition);

        // 4. Launch async tasks from priority queue
        ProcessCookingQueue();
    }

private:
    // Launches async collision cook on thread pool
    void LaunchAsyncCollisionCook(const FCollisionCookRequest& Request) {
        // Game thread: prepare meshing request (copy data, merge edits)
        FVoxelMeshingRequest MeshRequest;
        ChunkManager->PrepareCollisionMeshRequest(ChunkCoord, LODLevel, MeshRequest);
        IVoxelMesher* MesherPtr = ChunkManager->GetMesherPtr();

        // Thread pool: mesh gen + Chaos trimesh construction
        Async(EAsyncExecution::ThreadPool, [MesherPtr, MeshRequest]() {
            FChunkMeshData MeshData;
            MesherPtr->GenerateMeshCPU(MeshRequest, MeshData);  // ~2-4ms

            // Build Chaos::FTriangleMeshImplicitObject                // ~1-2ms
            TRefCountPtr<Chaos::FTriangleMeshImplicitObject> TriMesh = ...;

            // Enqueue for game thread (MPSC queue)
            CompletedCollisionQueue.Enqueue(Result);
        });
    }

    // Game thread: apply trimesh to physics
    void ApplyCollisionResult(FAsyncCollisionResult& Result) {
        // Create/reuse UBodySetup, set TriMeshGeometries
        // Create UBoxComponent, override FBodyInstance::BodySetup
        // RegisterComponent + InitBody (~0.5ms)
    }

    /** Priority queue (sorted ascending, pop from back for O(1)) */
    TArray<FCollisionCookRequest> CookingQueue;

    /** MPSC queue for thread pool → game thread results */
    TQueue<FAsyncCollisionResult, EQueueMode::Mpsc> CompletedCollisionQueue;

    /** Tracks in-flight async tasks */
    TSet<FIntVector> AsyncCollisionInProgress;

    // Config: MaxAsyncCollisionTasks (default 2), MaxAppliesPerFrame (2)
};
```

### Key Design Decisions

- **CollisionRadius**: Default 50% of ViewDistance (configurable)
- **CollisionLODLevel**: Uses coarser meshes for physics (fewer triangles, configurable)
- **Chaos Physics** (UE 5.7): `TriMeshGeometries` with `TRefCountPtr<FTriangleMeshImplicitObject>`
- **Container Actor**: `CollisionContainerActor` holds all chunk `UBoxComponent` collision holders
- **Edit Integration**: `MarkChunkDirty()` called from `EditManager->OnChunkEdited` delegate
- **Thread Safety**: Data prep reads game-thread state; mesher is stateless; trimesh is pure data construction

---

## Integration Flow

### Chunk Generation → Rendering

```
[Frame N] ChunkManager dispatches meshing compute shader
    ↓
[Frame N+1] GPU executes meshing, produces vertex/index buffers
    ↓
[Frame N+2] ChunkManager receives FChunkRenderData
    ↓
ChunkManager::OnChunkMeshingComplete()
    ↓
    ├→ MeshRenderer->UpdateChunkMesh(RenderData)
    │   ├→ [Custom VF] Direct GPU buffer binding (zero copy!)
    │   └→ [PMC] Readback + CreateMeshSection (slow)
    │
    └→ CollisionManager->QueueCollisionUpdate(RenderData)
        └→ Async collision generation (lower LOD)
```

### LOD Transition Update

```
[Every Frame] ChunkManager::UpdateLODTransitions()
    ↓
For each loaded chunk:
    ↓
    LODStrategy->GetLODMorphFactor(ChunkCoord)
    ↓
    If MorphFactor changed:
        ↓
        MeshRenderer->UpdateLODTransition(ChunkCoord, MorphFactor)
        ↓
        [Custom VF] Update vertex shader uniform
        │           └→ Vertex shader morphs positions
        │
        [PMC] No morphing support
                └→ Swap mesh at threshold
```

---

## Performance Comparison

### Memory per Chunk (LOD 0, 32³)

| Component | Custom VF | PMC |
|-----------|-----------|-----|
| Vertex Data | ~50 KB | ~50 KB |
| Index Data | ~30 KB | ~30 KB |
| CPU Copy | 0 KB | ~80 KB |
| **Total** | **~80 KB** | **~160 KB** |

Custom VF uses **50% less memory** (no CPU-side duplicate).

### Update Performance

| Operation | Custom VF | PMC |
|-----------|-----------|-----|
| Mesh Update | 0.1 ms | 2-5 ms |
| LOD Transition | 0.01 ms | N/A |
| **Per Frame (4 chunks)** | **0.4 ms** | **8-20 ms** |

Custom VF is **20-50x faster** for mesh updates.

### Feature Comparison

| Feature | Custom VF | PMC |
|---------|-----------|-----|
| GPU-driven | ✅ Yes | ❌ No |
| LOD Morphing | ✅ Yes | ❌ No |
| Zero CPU overhead | ✅ Yes | ❌ No |
| Physics collision | ⚠️ Separate | ✅ Automatic |
| Debug tools | ⚠️ Complex | ✅ Simple |
| UE compatibility | ⚠️ Limited | ✅ Full |

---

## Implementation Guide

### Step 1: Define Interface

```cpp
// Source/VoxelRendering/Public/IVoxelMeshRenderer.h
class IVoxelMeshRenderer {
    // ... (interface as shown above)
};
```

### Step 2: Implement PMC Renderer (simpler, first)

```cpp
// Source/VoxelRendering/Private/VoxelPMCRenderer.cpp
class FVoxelPMCRenderer : public IVoxelMeshRenderer {
    // ... (implementation as shown above)
};
```

### Step 3: Test with PMC

```cpp
// Verify mesh generation pipeline works end-to-end
// Debug visual output
// Profile performance baseline
```

### Step 4: Implement Custom Vertex Factory

```cpp
// Source/VoxelRendering/Public/VoxelVertexFactory.h
class FVoxelVertexFactory : public FVertexFactory {
    // ... (implementation as shown above)
};
```

### Step 5: Implement Vertex Shader

```hlsl
// Shaders/VoxelVertexFactory.usf
// ... (shader as shown above)
```

### Step 6: Implement Custom VF Renderer

```cpp
// Source/VoxelRendering/Private/VoxelCustomVFRenderer.cpp
class FVoxelCustomVFRenderer : public IVoxelMeshRenderer {
    // ... (implementation as shown above)
};
```

### Step 7: Runtime Selection

```cpp
void UVoxelChunkManager::Initialize(UVoxelWorldConfiguration* Config) {
    if (Config->bUseCustomVertexFactory && !GIsEditor) {
        MeshRenderer = MakeShared<FVoxelCustomVFRenderer>();
    } else {
        MeshRenderer = MakeShared<FVoxelPMCRenderer>();
    }
    
    MeshRenderer->Initialize(GetWorld(), *Config);
}
```

---

## Debugging Tips

### PMC Debugging
- Use "Show Collision" in viewport
- Enable "Wireframe" view mode
- Check vertex/index counts in output log

### Custom VF Debugging
- Use RenderDoc to capture frame
- Check GPU buffer contents
- Verify vertex factory bindings
- Test shader compilation

### Common Issues
- **Black meshes**: Normal packing issue, check UnpackNormal
- **Missing chunks**: Scene proxy not updating, check render commands
- **Crashes**: Buffer lifetime issue, ensure ConvertToExternalBuffer
- **Poor performance**: Check for CPU-GPU sync points

---

**Status**: Implementation Complete (Phase 3 + Per-Material Opacity)
**Current Architecture**: FLocalVertexFactory-based renderer with MPC LOD morphing
**Fallback**: PMC renderer available for editor/debugging use cases, supports masked material section splitting