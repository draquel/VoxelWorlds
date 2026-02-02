# UE5 Custom Vertex Factory Research & Minimal Example Plan

## Executive Summary

Custom Vertex Factories in Unreal Engine are notoriously difficult because:
1. **Documentation is sparse** - Epic's docs provide API references but not how-to guides
2. **Template-based architecture** - .ush files define an implicit interface, not explicit entry points
3. **Many moving parts** - Requires coordination between C++ classes, shaders, scene proxies, and components
4. **Version fragility** - APIs change between UE versions (notably in 5.6 with `LooseParametersUniformBuffer`)

This document synthesizes research from multiple sources to provide a clear implementation path.

---

## Architecture Overview

### Core Components (5 pieces that must work together)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           GAME THREAD                                        │
├─────────────────────────────────────────────────────────────────────────────┤
│  UPrimitiveComponent                                                         │
│  └── Creates FPrimitiveSceneProxy in CreateSceneProxy()                     │
│  └── Provides mesh data (vertices, indices)                                 │
│  └── Calls MarkRenderStateDirty() when data changes                         │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                          RENDER THREAD                                       │
├─────────────────────────────────────────────────────────────────────────────┤
│  FPrimitiveSceneProxy                                                        │
│  ├── GetViewRelevance() - tells renderer what passes this needs             │
│  ├── GetDynamicMeshElements() - provides FMeshBatch to renderer             │
│  └── Owns: FVertexFactory, FVertexBuffer, FIndexBuffer                      │
│                                                                              │
│  FVertexFactory (our custom class)                                           │
│  ├── Inherits: FLocalVertexFactory (recommended) or FVertexFactory          │
│  ├── SetData() - receives vertex stream components                          │
│  ├── InitRHI() - creates vertex declaration from streams                    │
│  └── Links to .ush shader file via IMPLEMENT_VERTEX_FACTORY_TYPE            │
│                                                                              │
│  FVertexBuffer / FIndexBuffer                                                │
│  ├── Owns actual GPU buffer (VertexBufferRHI / IndexBufferRHI)              │
│  └── InitRHI() - creates and uploads buffer data                            │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                            GPU / SHADERS                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│  .ush Shader File (defines the vertex factory's shader interface)            │
│  ├── FVertexFactoryInput - struct of vertex attributes                      │
│  ├── FVertexFactoryIntermediates - cached calculations                      │
│  ├── FVertexFactoryInterpolantsVSToPS - VS to PS interpolants               │
│  ├── VertexFactoryGetWorldPosition() - returns world position               │
│  ├── GetMaterialVertexParameters() - provides material inputs               │
│  └── GetMaterialPixelParameters() - provides pixel shader inputs            │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Key Insight: Template-Based Shader System

**The .ush file doesn't have an entry point!** Instead, it defines:
- **Structs** that pass shaders use (FVertexFactoryInput, etc.)
- **Functions** that pass shaders call (VertexFactoryGetWorldPosition, etc.)

The actual vertex/pixel shaders are in engine files like `BasePassVertexShader.usf`. Your vertex factory just provides the mesh-specific implementation of the interface.

---

## Minimal Example: Rendering a Colored Plane

### Goal
Render a simple quad (2 triangles, 4 vertices) with vertex colors using a custom vertex factory. This proves the entire pipeline works before adding voxel complexity.

### File Structure

```
VoxelWorlds/
├── Source/
│   └── VoxelRendering/
│       ├── Public/
│       │   ├── SimpleVoxelVertexFactory.h
│       │   └── SimpleVoxelComponent.h
│       └── Private/
│           ├── SimpleVoxelVertexFactory.cpp
│           └── SimpleVoxelComponent.cpp
└── Shaders/
    └── Private/
        └── SimpleVoxelVertexFactory.ush
```

---

## Part 1: The Vertex Format

```cpp
// SimpleVoxelVertexFactory.h

#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"
#include "LocalVertexFactory.h"

/**
 * Simple vertex format for our minimal example.
 * Position (12 bytes) + Color (4 bytes) = 16 bytes per vertex
 */
struct FSimpleVoxelVertex
{
    FVector3f Position;
    FColor Color;
};

/**
 * Vertex buffer holding our vertices.
 */
class FSimpleVoxelVertexBuffer : public FVertexBuffer
{
public:
    TArray<FSimpleVoxelVertex> Vertices;

    virtual void InitRHI(FRHICommandListBase& RHICmdList) override
    {
        if (Vertices.Num() > 0)
        {
            FRHIResourceCreateInfo CreateInfo(TEXT("SimpleVoxelVertexBuffer"));
            VertexBufferRHI = RHICmdList.CreateVertexBuffer(
                Vertices.Num() * sizeof(FSimpleVoxelVertex),
                BUF_Static,
                CreateInfo
            );
            
            // Copy vertex data
            void* BufferData = RHICmdList.LockBuffer(
                VertexBufferRHI, 
                0, 
                Vertices.Num() * sizeof(FSimpleVoxelVertex), 
                RLM_WriteOnly
            );
            FMemory::Memcpy(BufferData, Vertices.GetData(), Vertices.Num() * sizeof(FSimpleVoxelVertex));
            RHICmdList.UnlockBuffer(VertexBufferRHI);
        }
    }
};

/**
 * Index buffer.
 */
class FSimpleVoxelIndexBuffer : public FIndexBuffer
{
public:
    TArray<uint32> Indices;

    virtual void InitRHI(FRHICommandListBase& RHICmdList) override
    {
        if (Indices.Num() > 0)
        {
            FRHIResourceCreateInfo CreateInfo(TEXT("SimpleVoxelIndexBuffer"));
            IndexBufferRHI = RHICmdList.CreateIndexBuffer(
                sizeof(uint32),
                Indices.Num() * sizeof(uint32),
                BUF_Static,
                CreateInfo
            );
            
            void* BufferData = RHICmdList.LockBuffer(
                IndexBufferRHI,
                0,
                Indices.Num() * sizeof(uint32),
                RLM_WriteOnly
            );
            FMemory::Memcpy(BufferData, Indices.GetData(), Indices.Num() * sizeof(uint32));
            RHICmdList.UnlockBuffer(IndexBufferRHI);
        }
    }
};
```

---

## Part 2: The Vertex Factory (C++)

```cpp
// SimpleVoxelVertexFactory.h (continued)

/**
 * Our custom vertex factory.
 * Inherits from FLocalVertexFactory to reuse most of its logic.
 */
class FSimpleVoxelVertexFactory : public FLocalVertexFactory
{
    DECLARE_VERTEX_FACTORY_TYPE(FSimpleVoxelVertexFactory);

public:
    FSimpleVoxelVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
        : FLocalVertexFactory(InFeatureLevel, "FSimpleVoxelVertexFactory")
    {
    }

    /**
     * Initialize the vertex factory with our buffer.
     */
    void Init(const FSimpleVoxelVertexBuffer* InVertexBuffer)
    {
        // Build the data type that describes our vertex layout
        FDataType NewData;
        
        // Position component - ATTRIBUTE0 in shader
        NewData.PositionComponent = FVertexStreamComponent(
            InVertexBuffer,
            STRUCT_OFFSET(FSimpleVoxelVertex, Position),
            sizeof(FSimpleVoxelVertex),
            VET_Float3
        );
        
        // For LocalVertexFactory compatibility, we need tangents
        // We'll provide dummy tangents (flat shading)
        NewData.TangentBasisComponents[0] = FVertexStreamComponent(
            InVertexBuffer,
            STRUCT_OFFSET(FSimpleVoxelVertex, Position), // Reuse position as dummy
            sizeof(FSimpleVoxelVertex),
            VET_PackedNormal
        );
        NewData.TangentBasisComponents[1] = FVertexStreamComponent(
            InVertexBuffer,
            STRUCT_OFFSET(FSimpleVoxelVertex, Position),
            sizeof(FSimpleVoxelVertex),
            VET_PackedNormal
        );
        
        // Color component - ATTRIBUTE3 typically
        NewData.ColorComponent = FVertexStreamComponent(
            InVertexBuffer,
            STRUCT_OFFSET(FSimpleVoxelVertex, Color),
            sizeof(FSimpleVoxelVertex),
            VET_Color
        );
        
        // Set the data (this stores it for InitRHI)
        SetData(NewData);
    }

    /**
     * Controls which shader permutations are compiled.
     * Return true only for permutations we actually need.
     */
    static bool ShouldCompilePermutation(
        const FVertexFactoryShaderPermutationParameters& Parameters)
    {
        // Only compile for:
        // 1. Default material (required fallback)
        // 2. Surface domain materials (not decals, post-process, etc.)
        return Parameters.MaterialParameters.bIsDefaultMaterial ||
               Parameters.MaterialParameters.MaterialDomain == MD_Surface;
    }

    /**
     * Set preprocessor defines for shader compilation.
     */
    static void ModifyCompilationEnvironment(
        const FVertexFactoryShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FLocalVertexFactory::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        
        // Define our custom macro so shader code knows we're using this VF
        OutEnvironment.SetDefine(TEXT("SIMPLE_VOXEL_VERTEX_FACTORY"), 1);
    }
};
```

```cpp
// SimpleVoxelVertexFactory.cpp

#include "SimpleVoxelVertexFactory.h"

// This macro:
// 1. Registers the vertex factory type
// 2. Links it to the shader file
// 3. Specifies which passes it supports (position, basepass, shadow, etc.)
IMPLEMENT_VERTEX_FACTORY_TYPE(
    FSimpleVoxelVertexFactory,                          // Class name
    "/Plugin/VoxelWorlds/Private/SimpleVoxelVertexFactory.ush",  // Shader file path
    EVertexFactoryFlags::UsedWithMaterials |            // Can use materials
    EVertexFactoryFlags::SupportsStaticLighting |       // Supports static lighting
    EVertexFactoryFlags::SupportsDynamicLighting |      // Supports dynamic lighting
    EVertexFactoryFlags::SupportsCachingMeshDrawCommands // Can cache draw commands
);
```

---

## Part 3: The Shader File (.ush)

```hlsl
// Shaders/Private/SimpleVoxelVertexFactory.ush

// Include common vertex factory utilities
#include "/Engine/Private/VertexFactoryCommon.ush"
#include "/Engine/Private/LocalVertexFactoryCommon.ush"

/**
 * Input from vertex buffers - matches our C++ vertex format.
 * ATTRIBUTE indices must match what we bound in C++.
 */
struct FVertexFactoryInput
{
    float4 Position : ATTRIBUTE0;
    
    // LocalVertexFactory expects tangent data for lighting
    // We'll compute flat normals if needed
    half3 TangentX : ATTRIBUTE1;
    half4 TangentZ : ATTRIBUTE2;  // W = sign of bitangent
    
    float4 Color : ATTRIBUTE3;
    
    // Instance ID for instancing support (can be 0 if not used)
    uint InstanceId : SV_InstanceID;
};

/**
 * Position-only input for depth/shadow passes.
 */
struct FPositionOnlyVertexFactoryInput
{
    float4 Position : ATTRIBUTE0;
    uint InstanceId : SV_InstanceID;
};

/**
 * Position + normal for certain passes.
 */
struct FPositionAndNormalOnlyVertexFactoryInput
{
    float4 Position : ATTRIBUTE0;
    float4 Normal : ATTRIBUTE2;
    uint InstanceId : SV_InstanceID;
};

/**
 * Cached intermediate calculations.
 */
struct FVertexFactoryIntermediates
{
    half3x3 TangentToLocal;
    half3x3 TangentToWorld;
    half TangentToWorldSign;
    half4 Color;
    float3 PreSkinPosition;
    
    // Primitive data for transforms
    uint PrimitiveId;
};

/**
 * Data interpolated from VS to PS.
 */
struct FVertexFactoryInterpolantsVSToPS
{
    float4 TangentToWorld0 : TEXCOORD0;
    float4 TangentToWorld2 : TEXCOORD1;
    float4 Color : COLOR0;
    
    #if INSTANCED_STEREO
    nointerpolation uint EyeIndex : PACKED_EYE_INDEX;
    #endif
};

/**
 * Get primitive data (transforms, etc.) for this vertex.
 */
FPrimitiveSceneData GetPrimitiveData(FVertexFactoryIntermediates Intermediates)
{
    return GetPrimitiveData(Intermediates.PrimitiveId);
}

/**
 * Compute intermediate values from input.
 */
FVertexFactoryIntermediates GetVertexFactoryIntermediates(FVertexFactoryInput Input)
{
    FVertexFactoryIntermediates Intermediates;
    
    Intermediates.PrimitiveId = 0; // Single primitive
    
    // Build tangent basis (for now, use world up as normal for flat surfaces)
    half3 TangentX = half3(1, 0, 0);
    half3 TangentZ = half3(0, 0, 1);
    half3 TangentY = cross(TangentZ, TangentX);
    
    Intermediates.TangentToLocal = half3x3(TangentX, TangentY, TangentZ);
    Intermediates.TangentToWorldSign = 1.0;
    
    // Transform tangent to world
    half3x3 LocalToWorld = (half3x3)GetPrimitiveData(Intermediates).LocalToWorld;
    Intermediates.TangentToWorld = mul(Intermediates.TangentToLocal, LocalToWorld);
    
    // Pass through vertex color
    Intermediates.Color = Input.Color FMANUALFETCH_COLOR_COMPONENT_SWIZZLE;
    
    Intermediates.PreSkinPosition = Input.Position.xyz;
    
    return Intermediates;
}

/**
 * Get world position from input (for all passes).
 */
float4 VertexFactoryGetWorldPosition(FVertexFactoryInput Input, FVertexFactoryIntermediates Intermediates)
{
    float4 LocalPosition = float4(Input.Position.xyz, 1.0);
    return TransformLocalToTranslatedWorld(LocalPosition.xyz, Intermediates.PrimitiveId);
}

/**
 * Get world position for position-only passes.
 */
float4 VertexFactoryGetWorldPosition(FPositionOnlyVertexFactoryInput Input)
{
    return TransformLocalToTranslatedWorld(Input.Position.xyz, 0);
}

/**
 * Get world position for position+normal passes.
 */
float4 VertexFactoryGetWorldPosition(FPositionAndNormalOnlyVertexFactoryInput Input)
{
    return TransformLocalToTranslatedWorld(Input.Position.xyz, 0);
}

/**
 * Get previous frame world position (for motion blur).
 */
float4 VertexFactoryGetPreviousWorldPosition(FVertexFactoryInput Input, FVertexFactoryIntermediates Intermediates)
{
    float4x4 PreviousLocalToWorld = GetPrimitiveData(Intermediates).PreviousLocalToWorld;
    return mul(float4(Input.Position.xyz, 1.0), PreviousLocalToWorld);
}

/**
 * Build VS to PS interpolants.
 */
FVertexFactoryInterpolantsVSToPS VertexFactoryGetInterpolantsVSToPS(
    FVertexFactoryInput Input,
    FVertexFactoryIntermediates Intermediates,
    FMaterialVertexParameters VertexParameters)
{
    FVertexFactoryInterpolantsVSToPS Interpolants;
    
    // Pack tangent to world
    Interpolants.TangentToWorld0 = float4(Intermediates.TangentToWorld[0], 0);
    Interpolants.TangentToWorld2 = float4(Intermediates.TangentToWorld[2], Intermediates.TangentToWorldSign);
    
    // Pass vertex color
    Interpolants.Color = Intermediates.Color;
    
    return Interpolants;
}

/**
 * Convert interpolants to material pixel parameters.
 */
FMaterialPixelParameters GetMaterialPixelParameters(
    FVertexFactoryInterpolantsVSToPS Interpolants,
    float4 SvPosition)
{
    FMaterialPixelParameters Result = MakeInitializedMaterialPixelParameters();
    
    // Unpack tangent basis
    half3 TangentToWorld0 = Interpolants.TangentToWorld0.xyz;
    half3 TangentToWorld2 = Interpolants.TangentToWorld2.xyz;
    Result.TangentToWorld = AssembleTangentToWorld(TangentToWorld0, TangentToWorld2);
    
    // Set vertex color
    Result.VertexColor = Interpolants.Color;
    
    // Set UV to 0 (we're not using textures in minimal example)
    Result.UnMirrored = 1;
    Result.TwoSidedSign = 1;
    
    return Result;
}

/**
 * Convert input to material vertex parameters.
 */
FMaterialVertexParameters GetMaterialVertexParameters(
    FVertexFactoryInput Input,
    FVertexFactoryIntermediates Intermediates,
    float3 WorldPosition,
    half3x3 TangentToLocal)
{
    FMaterialVertexParameters Result = (FMaterialVertexParameters)0;
    
    Result.SceneData = GetPrimitiveSceneData(Intermediates.PrimitiveId);
    Result.WorldPosition = WorldPosition;
    Result.VertexColor = Intermediates.Color;
    Result.PreSkinnedPosition = Intermediates.PreSkinPosition;
    Result.PreSkinnedNormal = TangentToLocal[2];
    Result.TangentToWorld = Intermediates.TangentToWorld;
    
    return Result;
}

/**
 * Tangent basis computation.
 */
half3x3 VertexFactoryGetTangentToLocal(FVertexFactoryInput Input, FVertexFactoryIntermediates Intermediates)
{
    return Intermediates.TangentToLocal;
}

/**
 * Include common vertex factory functions.
 */
#include "/Engine/Private/VertexFactoryDefaultInterface.ush"
```

---

## Part 4: The Scene Proxy and Component

```cpp
// SimpleVoxelComponent.h

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "SimpleVoxelComponent.generated.h"

UCLASS(ClassGroup=(Rendering), meta=(BlueprintSpawnableComponent))
class VOXELRENDERING_API USimpleVoxelComponent : public UPrimitiveComponent
{
    GENERATED_BODY()

public:
    USimpleVoxelComponent();

    //~ Begin UPrimitiveComponent Interface
    virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
    virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
    //~ End UPrimitiveComponent Interface

    // Material to use
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel")
    UMaterialInterface* Material;

    // Size of the plane
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel")
    float PlaneSize = 100.0f;
};
```

```cpp
// SimpleVoxelComponent.cpp

#include "SimpleVoxelComponent.h"
#include "SimpleVoxelVertexFactory.h"
#include "PrimitiveSceneProxy.h"
#include "Materials/Material.h"

/**
 * Scene proxy for our simple voxel component.
 */
class FSimpleVoxelSceneProxy : public FPrimitiveSceneProxy
{
public:
    FSimpleVoxelSceneProxy(USimpleVoxelComponent* Component)
        : FPrimitiveSceneProxy(Component)
        , MaterialInterface(Component->Material)
        , VertexFactory(GetScene().GetFeatureLevel())
        , PlaneSize(Component->PlaneSize)
    {
        // Build a simple quad
        const float HalfSize = PlaneSize * 0.5f;
        
        // 4 vertices for a quad
        VertexBuffer.Vertices.Add({FVector3f(-HalfSize, -HalfSize, 0), FColor::Red});
        VertexBuffer.Vertices.Add({FVector3f( HalfSize, -HalfSize, 0), FColor::Green});
        VertexBuffer.Vertices.Add({FVector3f( HalfSize,  HalfSize, 0), FColor::Blue});
        VertexBuffer.Vertices.Add({FVector3f(-HalfSize,  HalfSize, 0), FColor::Yellow});
        
        // 2 triangles (6 indices)
        IndexBuffer.Indices = {0, 1, 2, 0, 2, 3};
        
        // Get material
        if (MaterialInterface == nullptr)
        {
            MaterialInterface = UMaterial::GetDefaultMaterial(MD_Surface);
        }
        MaterialRelevance = MaterialInterface->GetRelevance(GetScene().GetFeatureLevel());
    }

    virtual ~FSimpleVoxelSceneProxy()
    {
        VertexBuffer.ReleaseResource();
        IndexBuffer.ReleaseResource();
        VertexFactory.ReleaseResource();
    }

    virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override
    {
        // Initialize buffers
        VertexBuffer.InitResource(RHICmdList);
        IndexBuffer.InitResource(RHICmdList);
        
        // Initialize vertex factory with our buffer
        VertexFactory.Init(&VertexBuffer);
        VertexFactory.InitResource(RHICmdList);
    }

    virtual void GetDynamicMeshElements(
        const TArray<const FSceneView*>& Views,
        const FSceneViewFamily& ViewFamily,
        uint32 VisibilityMap,
        FMeshElementCollector& Collector) const override
    {
        // For each view that can see us
        for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
        {
            if (VisibilityMap & (1 << ViewIndex))
            {
                const FSceneView* View = Views[ViewIndex];
                
                // Allocate mesh batch
                FMeshBatch& Mesh = Collector.AllocateMesh();
                
                // Setup the mesh
                Mesh.VertexFactory = &VertexFactory;
                Mesh.MaterialRenderProxy = MaterialInterface->GetRenderProxy();
                Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
                Mesh.Type = PT_TriangleList;
                Mesh.DepthPriorityGroup = SDPG_World;
                Mesh.bCanApplyViewModeOverrides = true;
                
                // Setup the element
                FMeshBatchElement& BatchElement = Mesh.Elements[0];
                BatchElement.IndexBuffer = &IndexBuffer;
                BatchElement.FirstIndex = 0;
                BatchElement.NumPrimitives = IndexBuffer.Indices.Num() / 3;
                BatchElement.MinVertexIndex = 0;
                BatchElement.MaxVertexIndex = VertexBuffer.Vertices.Num() - 1;
                
                // Provide primitive uniform buffer
                BatchElement.PrimitiveUniformBuffer = GetUniformBuffer();
                
                Collector.AddMesh(ViewIndex, Mesh);
            }
        }
    }

    virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
    {
        FPrimitiveViewRelevance Result;
        Result.bDrawRelevance = IsShown(View);
        Result.bShadowRelevance = IsShadowCast(View);
        Result.bDynamicRelevance = true;  // We use GetDynamicMeshElements
        Result.bStaticRelevance = false;
        Result.bRenderInMainPass = ShouldRenderInMainPass();
        Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
        Result.bRenderCustomDepth = ShouldRenderCustomDepth();
        MaterialRelevance.SetPrimitiveViewRelevance(Result);
        return Result;
    }

    virtual uint32 GetMemoryFootprint() const override
    {
        return sizeof(*this) + GetAllocatedSize();
    }

    SIZE_T GetAllocatedSize() const
    {
        return VertexBuffer.Vertices.GetAllocatedSize() + 
               IndexBuffer.Indices.GetAllocatedSize();
    }

private:
    UMaterialInterface* MaterialInterface;
    FMaterialRelevance MaterialRelevance;
    FSimpleVoxelVertexBuffer VertexBuffer;
    FSimpleVoxelIndexBuffer IndexBuffer;
    FSimpleVoxelVertexFactory VertexFactory;
    float PlaneSize;
};

// Component implementation

USimpleVoxelComponent::USimpleVoxelComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

FPrimitiveSceneProxy* USimpleVoxelComponent::CreateSceneProxy()
{
    return new FSimpleVoxelSceneProxy(this);
}

FBoxSphereBounds USimpleVoxelComponent::CalcBounds(const FTransform& LocalToWorld) const
{
    const float HalfSize = PlaneSize * 0.5f;
    FBox LocalBox(FVector(-HalfSize, -HalfSize, -1), FVector(HalfSize, HalfSize, 1));
    return FBoxSphereBounds(LocalBox).TransformBy(LocalToWorld);
}
```

---

## Part 5: Module Setup

### Build.cs Dependencies

```csharp
// VoxelRendering.Build.cs

PublicDependencyModuleNames.AddRange(new string[] {
    "Core",
    "CoreUObject",
    "Engine",
    "RenderCore",
    "RHI",
    "Renderer"
});

// CRITICAL: Add shader directory mapping
string ShaderPath = Path.Combine(ModuleDirectory, "..", "..", "Shaders");
PublicDefinitions.Add($"VOXELWORLDS_SHADER_PATH=\"{ShaderPath}\"");
```

### Module Startup (Shader Registration)

```cpp
// VoxelRenderingModule.cpp

#include "Interfaces/IPluginManager.h"

void FVoxelRenderingModule::StartupModule()
{
    // Register shader directory
    FString ShaderDir = FPaths::Combine(
        IPluginManager::Get().FindPlugin(TEXT("VoxelWorlds"))->GetBaseDir(),
        TEXT("Shaders")
    );
    AddShaderSourceDirectoryMapping(TEXT("/Plugin/VoxelWorlds"), ShaderDir);
}
```

### Plugin Descriptor

```json
// VoxelWorlds.uplugin - CRITICAL for modules with shaders

{
    "Modules": [
        {
            "Name": "VoxelRendering",
            "Type": "Runtime",
            "LoadingPhase": "PostConfigInit"  // REQUIRED for shader registration
        }
    ]
}
```

---

## Common Pitfalls & Solutions

### 1. Black/Missing Mesh
**Cause**: Vertex factory not compiling with material
**Solution**: Check `ShouldCompilePermutation()` returns true for your material type

### 2. Crash on Startup
**Cause**: Module loading too late for shader registration
**Solution**: Set `LoadingPhase: PostConfigInit` in .uplugin

### 3. Shader Compilation Errors
**Cause**: Missing required functions in .ush
**Solution**: Ensure all functions from `VertexFactoryDefaultInterface.ush` are either:
- Included via `#include`
- Implemented manually

### 4. Incorrect Transforms
**Cause**: Not using correct primitive data
**Solution**: Use `GetPrimitiveData(PrimitiveId)` consistently

### 5. No Lighting
**Cause**: Missing tangent basis
**Solution**: Provide TangentX and TangentZ even if using flat shading

---

## Testing Checklist

1. [ ] Module compiles without errors
2. [ ] Shaders compile (check Output Log for shader errors)
3. [ ] Component spawns in editor without crash
4. [ ] Quad is visible from front
5. [ ] Quad is lit (not solid black)
6. [ ] Vertex colors appear (if using unlit or vertex color material)
7. [ ] Transform (move/rotate/scale) works correctly
8. [ ] No errors in Output Log

---

## Next Steps After Minimal Example Works

1. **Add UVs**: Extend vertex format with FVector2f UV
2. **Add Normals**: Pack normals properly for lighting
3. **Add LOD Morphing**: Pass morph factor as shader parameter
4. **GPU Buffer Binding**: Use compute shader output directly
5. **Multiple Chunks**: Handle multiple mesh sections per proxy

---

## References

- **AyoubKhammassi/CustomMeshComponent** - Best practical example (UE4, but concepts apply)
- **ProceduralMeshComponent** - Epic's official PMC uses FLocalVertexFactory
- **Matt Hoffman's UE4 Rendering Series** - Excellent architecture explanation
- **RuntimeMeshComponent** - More complex but production-quality example
- **Epic Shader Development Docs** - Official but sparse

---

*Document generated from research conducted on 2026-02-01*
