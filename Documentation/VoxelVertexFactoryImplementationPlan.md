# Custom Vertex Factory Implementation Plan

## Detailed Step-by-Step Guide for VoxelWorlds

**Target**: UE 5.7.2  
**Approach**: Incremental implementation with verification at each step  
**Time Estimate**: 4-6 hours for complete implementation

---

## Overview: Implementation Phases

```
Phase 1: Project Setup & Shader Infrastructure     ✅ COMPLETE
    └── Module configuration, shader directory mapping

Phase 2: Minimal Vertex Factory (No Custom Shader)  ✅ COMPLETE
    └── Use FLocalVertexFactory directly, verify rendering works

Phase 3: Custom Vertex Factory Class               ✅ COMPLETE (Revised Approach)
    └── Use FLocalVertexFactory DIRECTLY (don't extend with IMPLEMENT_VERTEX_FACTORY_TYPE)
    └── See Appendix A for working implementation details
    └── Test component: ULocalVFTestComponent renders successfully

Phase 4: Custom Shader File                        ⏸️ NOT NEEDED (using FLocalVertexFactory)
    └── FLocalVertexFactory uses Epic's LocalVertexFactory.ush - no custom shader required

Phase 5: Scene Proxy & Component                   ✅ COMPLETE
    └── FLocalVFTestSceneProxy demonstrates complete rendering pipeline

Phase 6: Verification & Debugging                  ✅ COMPLETE
    └── Vertex colors working correctly (Red/Green/Blue/Yellow corners)
    └── Key findings documented in Appendix A

Phase 7: Integration with Voxel System             🔲 PENDING
    └── Apply FLocalVertexFactory pattern to FVoxelVertexFactory
```

---

# Phase 1: Project Setup & Shader Infrastructure

## Step 1.1: Verify Module Structure

**File**: `Source/VoxelRendering/VoxelRendering.Build.cs`

```csharp
using UnrealBuildTool;
using System.IO;

public class VoxelRendering : ModuleRules
{
    public VoxelRendering(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        
        // CRITICAL: These are required for custom vertex factories
        PublicDependencyModuleNames.AddRange(new string[] {
            "Core",
            "CoreUObject",
            "Engine",
            "RenderCore",
            "RHI",
            "Renderer",       // For FPrimitiveSceneProxy
            "Projects"        // For IPluginManager
        });
        
        PrivateDependencyModuleNames.AddRange(new string[] {
            "VoxelCore"
        });
        
        // Enable IWYU
        bEnforceIWYU = true;
    }
}
```

**Verification**: Module compiles without errors.

---

## Step 1.2: Update Plugin Descriptor

**File**: `VoxelWorlds.uplugin`

```json
{
    "FileVersion": 3,
    "Version": 1,
    "VersionName": "1.0",
    "FriendlyName": "VoxelWorlds",
    "Description": "GPU-driven voxel terrain system",
    "Category": "Rendering",
    "CreatedBy": "Your Name",
    "CanContainContent": true,
    "Modules": [
        {
            "Name": "VoxelCore",
            "Type": "Runtime",
            "LoadingPhase": "Default"
        },
        {
            "Name": "VoxelLOD",
            "Type": "Runtime",
            "LoadingPhase": "Default"
        },
        {
            "Name": "VoxelGeneration",
            "Type": "Runtime",
            "LoadingPhase": "PostConfigInit"
        },
        {
            "Name": "VoxelMeshing",
            "Type": "Runtime",
            "LoadingPhase": "PostConfigInit"
        },
        {
            "Name": "VoxelRendering",
            "Type": "Runtime",
            "LoadingPhase": "PostConfigInit"
        },
        {
            "Name": "VoxelStreaming",
            "Type": "Runtime",
            "LoadingPhase": "Default"
        }
    ]
}
```

**CRITICAL**: `VoxelRendering` MUST use `PostConfigInit` loading phase because it registers shader types via `IMPLEMENT_VERTEX_FACTORY_TYPE`.

**Verification**: Editor launches without "Shader type was loaded after engine init" error.

---

## Step 1.3: Create Shader Directory Structure

**Directory**: `Plugins/VoxelWorlds/Shaders/Private/`

Create the directory structure:
```
VoxelWorlds/
└── Shaders/
    └── Private/
        └── (shader files will go here)
```

---

## Step 1.4: Register Shader Directory in Module Startup

**File**: `Source/VoxelRendering/Private/VoxelRenderingModule.cpp`

```cpp
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FVoxelRenderingModule"

class FVoxelRenderingModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        // Register shader directory
        // This maps "/Plugin/VoxelWorlds" in shader includes to our Shaders folder
        FString PluginShaderDir = FPaths::Combine(
            IPluginManager::Get().FindPlugin(TEXT("VoxelWorlds"))->GetBaseDir(),
            TEXT("Shaders")
        );
        AddShaderSourceDirectoryMapping(TEXT("/Plugin/VoxelWorlds"), PluginShaderDir);
        
        UE_LOG(LogTemp, Log, TEXT("VoxelRendering: Registered shader directory: %s"), *PluginShaderDir);
    }

    virtual void ShutdownModule() override
    {
    }
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVoxelRenderingModule, VoxelRendering)
```

**File**: `Source/VoxelRendering/Public/VoxelRenderingModule.h`

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class FVoxelRenderingModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};
```

**Verification**: 
1. Compile and launch editor
2. Check Output Log for "VoxelRendering: Registered shader directory"
3. No errors about missing shader directories

---

# Phase 2: Minimal Vertex Factory (Using FLocalVertexFactory)

Before creating a custom vertex factory, let's verify we can render using Epic's built-in FLocalVertexFactory. This isolates potential issues.

## Step 2.1: Create Test Component Header

**File**: `Source/VoxelRendering/Public/SimpleVoxelTestComponent.h`

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "SimpleVoxelTestComponent.generated.h"

/**
 * Test component that renders a simple colored quad.
 * Phase 2: Uses FLocalVertexFactory (built-in)
 * Phase 3+: Will use custom vertex factory
 */
UCLASS(ClassGroup=(Rendering), meta=(BlueprintSpawnableComponent))
class VOXELRENDERING_API USimpleVoxelTestComponent : public UPrimitiveComponent
{
    GENERATED_BODY()

public:
    USimpleVoxelTestComponent();

    //~ Begin UPrimitiveComponent Interface
    virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
    virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
    virtual int32 GetNumMaterials() const override { return 1; }
    virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;
    virtual void SetMaterial(int32 ElementIndex, UMaterialInterface* Material) override;
    //~ End UPrimitiveComponent Interface

    /** Material to render with */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Test")
    TObjectPtr<UMaterialInterface> Material;

    /** Size of the test quad in cm */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Test", meta = (ClampMin = "1.0"))
    float QuadSize = 100.0f;

    /** Refresh the mesh (call after changing properties) */
    UFUNCTION(BlueprintCallable, Category = "Voxel Test")
    void RefreshMesh();
};
```

---

## Step 2.2: Create Scene Proxy with FLocalVertexFactory

**File**: `Source/VoxelRendering/Private/SimpleVoxelTestComponent.cpp`

```cpp
#include "SimpleVoxelTestComponent.h"
#include "PrimitiveSceneProxy.h"
#include "Materials/Material.h"
#include "MaterialShared.h"
#include "Engine/Engine.h"
#include "LocalVertexFactory.h"
#include "StaticMeshResources.h"
#include "RenderResource.h"
#include "RHI.h"
#include "RenderUtils.h"
#include "DynamicMeshBuilder.h"

//-----------------------------------------------------------------------------
// Vertex Buffer using FDynamicMeshVertex (compatible with FLocalVertexFactory)
//-----------------------------------------------------------------------------
class FSimpleVoxelVertexBuffer : public FVertexBuffer
{
public:
    TArray<FDynamicMeshVertex> Vertices;

    virtual void InitRHI(FRHICommandListBase& RHICmdList) override
    {
        if (Vertices.Num() == 0)
        {
            return;
        }

        const uint32 SizeInBytes = Vertices.Num() * sizeof(FDynamicMeshVertex);
        
        FRHIResourceCreateInfo CreateInfo(TEXT("SimpleVoxelVertexBuffer"));
        CreateInfo.bWithoutNativeResource = false;
        
        VertexBufferRHI = RHICmdList.CreateBuffer(
            SizeInBytes,
            BUF_Static | BUF_VertexBuffer,
            sizeof(FDynamicMeshVertex),
            ERHIAccess::VertexOrIndexBuffer,
            CreateInfo
        );

        // Copy data
        void* Data = RHICmdList.LockBuffer(VertexBufferRHI, 0, SizeInBytes, RLM_WriteOnly);
        FMemory::Memcpy(Data, Vertices.GetData(), SizeInBytes);
        RHICmdList.UnlockBuffer(VertexBufferRHI);
    }
};

//-----------------------------------------------------------------------------
// Index Buffer
//-----------------------------------------------------------------------------
class FSimpleVoxelIndexBuffer : public FIndexBuffer
{
public:
    TArray<uint32> Indices;

    virtual void InitRHI(FRHICommandListBase& RHICmdList) override
    {
        if (Indices.Num() == 0)
        {
            return;
        }

        const uint32 SizeInBytes = Indices.Num() * sizeof(uint32);
        
        FRHIResourceCreateInfo CreateInfo(TEXT("SimpleVoxelIndexBuffer"));
        
        IndexBufferRHI = RHICmdList.CreateBuffer(
            SizeInBytes,
            BUF_Static | BUF_IndexBuffer,
            sizeof(uint32),
            ERHIAccess::VertexOrIndexBuffer,
            CreateInfo
        );

        void* Data = RHICmdList.LockBuffer(IndexBufferRHI, 0, SizeInBytes, RLM_WriteOnly);
        FMemory::Memcpy(Data, Indices.GetData(), SizeInBytes);
        RHICmdList.UnlockBuffer(IndexBufferRHI);
    }
};

//-----------------------------------------------------------------------------
// Scene Proxy
//-----------------------------------------------------------------------------
class FSimpleVoxelSceneProxy : public FPrimitiveSceneProxy
{
public:
    FSimpleVoxelSceneProxy(USimpleVoxelTestComponent* Component)
        : FPrimitiveSceneProxy(Component)
        , MaterialInterface(Component->GetMaterial(0))
        , VertexFactory(GetScene().GetFeatureLevel(), "FSimpleVoxelSceneProxy")
    {
        // Get material (use default if none set)
        if (MaterialInterface == nullptr)
        {
            MaterialInterface = UMaterial::GetDefaultMaterial(MD_Surface);
        }
        
        // Cache material relevance for GetViewRelevance
        MaterialRelevance = MaterialInterface->GetRelevance(GetScene().GetFeatureLevel());

        // Build quad geometry
        const float HalfSize = Component->QuadSize * 0.5f;
        
        // Create 4 vertices for a quad
        // Using FDynamicMeshVertex which is compatible with FLocalVertexFactory
        auto AddVertex = [this](const FVector3f& Pos, const FVector3f& Normal, const FVector2f& UV, const FColor& Color)
        {
            FDynamicMeshVertex Vertex;
            Vertex.Position = Pos;
            Vertex.TangentX = FVector3f(1, 0, 0);
            Vertex.TangentZ = FVector4f(Normal.X, Normal.Y, Normal.Z, 1.0f);
            Vertex.TextureCoordinate[0] = UV;
            Vertex.Color = Color;
            VertexBuffer.Vertices.Add(Vertex);
        };

        // Quad facing +Z
        const FVector3f Normal(0, 0, 1);
        AddVertex(FVector3f(-HalfSize, -HalfSize, 0), Normal, FVector2f(0, 0), FColor::Red);
        AddVertex(FVector3f( HalfSize, -HalfSize, 0), Normal, FVector2f(1, 0), FColor::Green);
        AddVertex(FVector3f( HalfSize,  HalfSize, 0), Normal, FVector2f(1, 1), FColor::Blue);
        AddVertex(FVector3f(-HalfSize,  HalfSize, 0), Normal, FVector2f(0, 1), FColor::Yellow);

        // Two triangles
        IndexBuffer.Indices = { 0, 1, 2, 0, 2, 3 };

        // Store counts for later
        NumVertices = VertexBuffer.Vertices.Num();
        NumIndices = IndexBuffer.Indices.Num();
    }

    virtual ~FSimpleVoxelSceneProxy()
    {
        VertexBuffer.ReleaseResource();
        IndexBuffer.ReleaseResource();
        VertexFactory.ReleaseResource();
    }

    virtual SIZE_T GetTypeHash() const override
    {
        static size_t UniquePointer;
        return reinterpret_cast<size_t>(&UniquePointer);
    }

    virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override
    {
        // Initialize vertex buffer
        VertexBuffer.InitResource(RHICmdList);
        
        // Initialize index buffer
        IndexBuffer.InitResource(RHICmdList);

        // Setup vertex factory data
        // FLocalVertexFactory expects FStaticMeshDataType
        FLocalVertexFactory::FDataType Data;

        // Position stream
        Data.PositionComponent = FVertexStreamComponent(
            &VertexBuffer,
            STRUCT_OFFSET(FDynamicMeshVertex, Position),
            sizeof(FDynamicMeshVertex),
            VET_Float3
        );

        // Tangent basis streams
        Data.TangentBasisComponents[0] = FVertexStreamComponent(
            &VertexBuffer,
            STRUCT_OFFSET(FDynamicMeshVertex, TangentX),
            sizeof(FDynamicMeshVertex),
            VET_PackedNormal
        );
        Data.TangentBasisComponents[1] = FVertexStreamComponent(
            &VertexBuffer,
            STRUCT_OFFSET(FDynamicMeshVertex, TangentZ),
            sizeof(FDynamicMeshVertex),
            VET_PackedNormal
        );

        // Texture coordinate stream
        Data.TextureCoordinates.Add(FVertexStreamComponent(
            &VertexBuffer,
            STRUCT_OFFSET(FDynamicMeshVertex, TextureCoordinate),
            sizeof(FDynamicMeshVertex),
            VET_Float2
        ));

        // Color stream
        Data.ColorComponent = FVertexStreamComponent(
            &VertexBuffer,
            STRUCT_OFFSET(FDynamicMeshVertex, Color),
            sizeof(FDynamicMeshVertex),
            VET_Color
        );

        // Initialize vertex factory
        VertexFactory.SetData(RHICmdList, Data);
        VertexFactory.InitResource(RHICmdList);
    }

    virtual void GetDynamicMeshElements(
        const TArray<const FSceneView*>& Views,
        const FSceneViewFamily& ViewFamily,
        uint32 VisibilityMap,
        FMeshElementCollector& Collector) const override
    {
        QUICK_SCOPE_CYCLE_COUNTER(STAT_SimpleVoxelSceneProxy_GetDynamicMeshElements);

        // Check we have valid resources
        if (NumVertices == 0 || NumIndices == 0)
        {
            return;
        }

        // Setup wireframe material if needed
        const bool bWireframe = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;
        FMaterialRenderProxy* MaterialProxy = nullptr;
        
        if (bWireframe)
        {
            FColoredMaterialRenderProxy* WireframeMaterialInstance = new FColoredMaterialRenderProxy(
                GEngine->WireframeMaterial ? GEngine->WireframeMaterial->GetRenderProxy() : nullptr,
                FLinearColor(0, 0.5f, 1.f)
            );
            Collector.RegisterOneFrameMaterialProxy(WireframeMaterialInstance);
            MaterialProxy = WireframeMaterialInstance;
        }
        else
        {
            MaterialProxy = MaterialInterface->GetRenderProxy();
        }

        // For each view
        for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
        {
            if (VisibilityMap & (1 << ViewIndex))
            {
                // Allocate mesh batch
                FMeshBatch& Mesh = Collector.AllocateMesh();
                
                // Fill in mesh batch
                Mesh.VertexFactory = &VertexFactory;
                Mesh.MaterialRenderProxy = MaterialProxy;
                Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
                Mesh.Type = PT_TriangleList;
                Mesh.DepthPriorityGroup = SDPG_World;
                Mesh.bCanApplyViewModeOverrides = true;
                Mesh.bUseWireframeSelectionColoring = IsSelected();

                // Fill in batch element
                FMeshBatchElement& BatchElement = Mesh.Elements[0];
                BatchElement.IndexBuffer = &IndexBuffer;
                BatchElement.FirstIndex = 0;
                BatchElement.NumPrimitives = NumIndices / 3;
                BatchElement.MinVertexIndex = 0;
                BatchElement.MaxVertexIndex = NumVertices - 1;

                // Primitive uniform buffer
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
        Result.bDynamicRelevance = true;
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
    FLocalVertexFactory VertexFactory;
    
    int32 NumVertices = 0;
    int32 NumIndices = 0;
};

//-----------------------------------------------------------------------------
// Component Implementation
//-----------------------------------------------------------------------------

USimpleVoxelTestComponent::USimpleVoxelTestComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    
    // Enable rendering
    bWantsOnUpdateTransform = false;
    SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
}

FPrimitiveSceneProxy* USimpleVoxelTestComponent::CreateSceneProxy()
{
    if (QuadSize <= 0)
    {
        return nullptr;
    }
    
    return new FSimpleVoxelSceneProxy(this);
}

FBoxSphereBounds USimpleVoxelTestComponent::CalcBounds(const FTransform& LocalToWorld) const
{
    const float HalfSize = QuadSize * 0.5f;
    FBox LocalBox(
        FVector(-HalfSize, -HalfSize, -1.0f),
        FVector(HalfSize, HalfSize, 1.0f)
    );
    return FBoxSphereBounds(LocalBox).TransformBy(LocalToWorld);
}

UMaterialInterface* USimpleVoxelTestComponent::GetMaterial(int32 ElementIndex) const
{
    return Material;
}

void USimpleVoxelTestComponent::SetMaterial(int32 ElementIndex, UMaterialInterface* InMaterial)
{
    Material = InMaterial;
    MarkRenderStateDirty();
}

void USimpleVoxelTestComponent::RefreshMesh()
{
    MarkRenderStateDirty();
}
```

---

## Step 2.3: Verification Checkpoint

**To verify Phase 2 works:**

1. Compile the project
2. Open the editor
3. Create a new Actor Blueprint
4. Add `SimpleVoxelTestComponent` to it
5. Set a material (any simple material, or leave blank for default)
6. Place the actor in the level
7. **Expected Result**: A colored quad appears, lit correctly

**Troubleshooting Phase 2:**
- Black quad? Check material is valid
- No quad? Check `CalcBounds()` returns valid bounds
- Crash? Check all pointers in scene proxy constructor

---

# Phase 3: Custom Vertex Factory Class

> **⚠️ IMPORTANT UPDATE (2026-02-01)**: The approach in this section (extending FLocalVertexFactory with IMPLEMENT_VERTEX_FACTORY_TYPE) **does not work correctly** in UE5. It creates a new shader type without compiled shaders, causing "uniform buffer at slot 2 null" errors.
>
> **See Appendix A** at the end of this document for the **working approach**: use `FLocalVertexFactory` directly without extending it. The test component `ULocalVFTestComponent` demonstrates this pattern successfully.

Now we create our own vertex factory that inherits from FLocalVertexFactory.

## Step 3.1: Create Vertex Factory Header

**File**: `Source/VoxelRendering/Public/VoxelVertexFactory.h`

```cpp
#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"
#include "LocalVertexFactory.h"
#include "ShaderParameters.h"

/**
 * Custom vertex format for voxel rendering.
 * 
 * Layout (28 bytes total):
 *   Position:     float3 (12 bytes) - ATTRIBUTE0
 *   PackedNormal: uint32 (4 bytes)  - ATTRIBUTE1 (10/10/10/2 packed)
 *   UV:           float2 (8 bytes)  - ATTRIBUTE2
 *   Color:        uint32 (4 bytes)  - ATTRIBUTE3 (RGBA8)
 */
struct FVoxelVertex
{
    FVector3f Position;
    uint32 PackedNormal;  // 10/10/10/2 format
    FVector2f UV;
    uint32 Color;         // RGBA8

    FVoxelVertex()
        : Position(FVector3f::ZeroVector)
        , PackedNormal(0)
        , UV(FVector2f::ZeroVector)
        , Color(0xFFFFFFFF)
    {
    }

    FVoxelVertex(const FVector3f& InPosition, const FVector3f& InNormal, const FVector2f& InUV, const FColor& InColor)
        : Position(InPosition)
        , UV(InUV)
    {
        SetNormal(InNormal);
        SetColor(InColor);
    }

    void SetNormal(const FVector3f& Normal)
    {
        // Pack normal to 10/10/10/2 format
        // Range: -1 to 1 -> 0 to 1023
        const int32 X = FMath::Clamp(FMath::RoundToInt((Normal.X * 0.5f + 0.5f) * 1023.0f), 0, 1023);
        const int32 Y = FMath::Clamp(FMath::RoundToInt((Normal.Y * 0.5f + 0.5f) * 1023.0f), 0, 1023);
        const int32 Z = FMath::Clamp(FMath::RoundToInt((Normal.Z * 0.5f + 0.5f) * 1023.0f), 0, 1023);
        PackedNormal = (X) | (Y << 10) | (Z << 20) | (0x3 << 30); // W = 1
    }

    FVector3f GetNormal() const
    {
        const float X = ((PackedNormal) & 0x3FF) / 1023.0f * 2.0f - 1.0f;
        const float Y = ((PackedNormal >> 10) & 0x3FF) / 1023.0f * 2.0f - 1.0f;
        const float Z = ((PackedNormal >> 20) & 0x3FF) / 1023.0f * 2.0f - 1.0f;
        return FVector3f(X, Y, Z);
    }

    void SetColor(const FColor& InColor)
    {
        Color = InColor.DWColor();
    }

    FColor GetColor() const
    {
        return FColor(Color);
    }
};

/**
 * Vertex buffer for voxel vertices.
 */
class FVoxelVertexBuffer : public FVertexBuffer
{
public:
    TArray<FVoxelVertex> Vertices;

    virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
    virtual FString GetFriendlyName() const override { return TEXT("FVoxelVertexBuffer"); }
};

/**
 * Index buffer for voxel meshes.
 */
class FVoxelIndexBuffer : public FIndexBuffer
{
public:
    TArray<uint32> Indices;

    virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
    virtual FString GetFriendlyName() const override { return TEXT("FVoxelIndexBuffer"); }
};

/**
 * Custom vertex factory for voxel rendering.
 * 
 * Inherits from FLocalVertexFactory to reuse most functionality.
 * Uses custom shader file for additional features (LOD morphing, etc.)
 */
class FVoxelVertexFactory : public FLocalVertexFactory
{
    DECLARE_VERTEX_FACTORY_TYPE(FVoxelVertexFactory);

public:
    FVoxelVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, const char* InDebugName = "FVoxelVertexFactory")
        : FLocalVertexFactory(InFeatureLevel, InDebugName)
    {
    }

    /**
     * Initialize the vertex factory with a voxel vertex buffer.
     */
    void Init(FRHICommandListBase& RHICmdList, const FVoxelVertexBuffer* InVertexBuffer);

    /**
     * Should this permutation be compiled?
     */
    static bool ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters);

    /**
     * Modify shader compilation environment.
     */
    static void ModifyCompilationEnvironment(
        const FVertexFactoryShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment);
};
```

---

## Step 3.2: Implement Vertex Factory

**File**: `Source/VoxelRendering/Private/VoxelVertexFactory.cpp`

```cpp
#include "VoxelVertexFactory.h"
#include "MeshMaterialShader.h"
#include "RHICommandList.h"

//-----------------------------------------------------------------------------
// FVoxelVertexBuffer
//-----------------------------------------------------------------------------

void FVoxelVertexBuffer::InitRHI(FRHICommandListBase& RHICmdList)
{
    if (Vertices.Num() == 0)
    {
        return;
    }

    const uint32 SizeInBytes = Vertices.Num() * sizeof(FVoxelVertex);
    
    FRHIResourceCreateInfo CreateInfo(TEXT("FVoxelVertexBuffer"));
    
    VertexBufferRHI = RHICmdList.CreateBuffer(
        SizeInBytes,
        BUF_Static | BUF_VertexBuffer | BUF_ShaderResource,
        sizeof(FVoxelVertex),
        ERHIAccess::VertexOrIndexBuffer,
        CreateInfo
    );

    void* Data = RHICmdList.LockBuffer(VertexBufferRHI, 0, SizeInBytes, RLM_WriteOnly);
    FMemory::Memcpy(Data, Vertices.GetData(), SizeInBytes);
    RHICmdList.UnlockBuffer(VertexBufferRHI);
}

//-----------------------------------------------------------------------------
// FVoxelIndexBuffer
//-----------------------------------------------------------------------------

void FVoxelIndexBuffer::InitRHI(FRHICommandListBase& RHICmdList)
{
    if (Indices.Num() == 0)
    {
        return;
    }

    const uint32 SizeInBytes = Indices.Num() * sizeof(uint32);
    
    FRHIResourceCreateInfo CreateInfo(TEXT("FVoxelIndexBuffer"));
    
    IndexBufferRHI = RHICmdList.CreateBuffer(
        SizeInBytes,
        BUF_Static | BUF_IndexBuffer,
        sizeof(uint32),
        ERHIAccess::VertexOrIndexBuffer,
        CreateInfo
    );

    void* Data = RHICmdList.LockBuffer(IndexBufferRHI, 0, SizeInBytes, RLM_WriteOnly);
    FMemory::Memcpy(Data, Indices.GetData(), SizeInBytes);
    RHICmdList.UnlockBuffer(IndexBufferRHI);
}

//-----------------------------------------------------------------------------
// FVoxelVertexFactory
//-----------------------------------------------------------------------------

void FVoxelVertexFactory::Init(FRHICommandListBase& RHICmdList, const FVoxelVertexBuffer* InVertexBuffer)
{
    check(InVertexBuffer != nullptr);

    // Build data type for vertex factory
    FLocalVertexFactory::FDataType Data;

    // Position component
    Data.PositionComponent = FVertexStreamComponent(
        InVertexBuffer,
        STRUCT_OFFSET(FVoxelVertex, Position),
        sizeof(FVoxelVertex),
        VET_Float3
    );

    // Normal/Tangent - use packed normal
    // TangentX and TangentZ are expected by LocalVertexFactory
    // We'll provide the packed normal for both and handle in shader
    Data.TangentBasisComponents[0] = FVertexStreamComponent(
        InVertexBuffer,
        STRUCT_OFFSET(FVoxelVertex, PackedNormal),
        sizeof(FVoxelVertex),
        VET_UInt       // Raw uint32, we'll unpack in shader
    );
    Data.TangentBasisComponents[1] = FVertexStreamComponent(
        InVertexBuffer,
        STRUCT_OFFSET(FVoxelVertex, PackedNormal),
        sizeof(FVoxelVertex),
        VET_UInt
    );

    // Texture coordinates
    Data.TextureCoordinates.Add(FVertexStreamComponent(
        InVertexBuffer,
        STRUCT_OFFSET(FVoxelVertex, UV),
        sizeof(FVoxelVertex),
        VET_Float2
    ));

    // Vertex color
    Data.ColorComponent = FVertexStreamComponent(
        InVertexBuffer,
        STRUCT_OFFSET(FVoxelVertex, Color),
        sizeof(FVoxelVertex),
        VET_Color
    );

    // Set the data
    SetData(RHICmdList, Data);
}

bool FVoxelVertexFactory::ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters)
{
    // Compile for:
    // 1. Default materials (required fallback)
    // 2. Surface domain materials
    // 3. Special engine materials (used for debug views)
    
    const bool bIsDefault = Parameters.MaterialParameters.bIsDefaultMaterial;
    const bool bIsSurface = Parameters.MaterialParameters.MaterialDomain == MD_Surface;
    const bool bIsSpecial = Parameters.MaterialParameters.bIsSpecialEngineMaterial;
    
    return bIsDefault || bIsSurface || bIsSpecial;
}

void FVoxelVertexFactory::ModifyCompilationEnvironment(
    const FVertexFactoryShaderPermutationParameters& Parameters,
    FShaderCompilerEnvironment& OutEnvironment)
{
    // Call parent
    FLocalVertexFactory::ModifyCompilationEnvironment(Parameters, OutEnvironment);
    
    // Set our custom define
    OutEnvironment.SetDefine(TEXT("VOXEL_VERTEX_FACTORY"), 1);
}

// CRITICAL: This macro registers the vertex factory type and links it to the shader file
// The path must match where you registered the shader directory
IMPLEMENT_VERTEX_FACTORY_TYPE(
    FVoxelVertexFactory,
    "/Plugin/VoxelWorlds/Private/VoxelVertexFactory.ush",
    EVertexFactoryFlags::UsedWithMaterials |
    EVertexFactoryFlags::SupportsStaticLighting |
    EVertexFactoryFlags::SupportsDynamicLighting |
    EVertexFactoryFlags::SupportsCachingMeshDrawCommands
);
```

---

# Phase 4: Custom Shader File

## Step 4.1: Create the Vertex Factory Shader

**File**: `Shaders/Private/VoxelVertexFactory.ush`

```hlsl
// VoxelVertexFactory.ush
// Custom vertex factory shader for voxel terrain rendering

// Include common utilities
#include "/Engine/Private/VertexFactoryCommon.ush"
#include "/Engine/Private/LocalVertexFactoryCommon.ush"

// ============================================================================
// Vertex Input Structures
// ============================================================================

/**
 * Main vertex input from bound vertex buffers.
 * Must match the C++ vertex format and stream bindings.
 */
struct FVertexFactoryInput
{
    float4 Position     : ATTRIBUTE0;
    uint PackedNormal   : ATTRIBUTE1;  // 10/10/10/2 packed normal
    float2 TexCoord     : ATTRIBUTE2;
    float4 Color        : ATTRIBUTE3;
    
    // Required for instancing
    uint InstanceId     : SV_InstanceID;
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
 * Position and normal only input for certain passes.
 */
struct FPositionAndNormalOnlyVertexFactoryInput
{
    float4 Position     : ATTRIBUTE0;
    uint PackedNormal   : ATTRIBUTE1;
    uint InstanceId     : SV_InstanceID;
};

// ============================================================================
// Intermediate Structures
// ============================================================================

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
    uint PrimitiveId;
};

// ============================================================================
// VS to PS Interpolants
// ============================================================================

/**
 * Data passed from vertex shader to pixel shader.
 */
struct FVertexFactoryInterpolantsVSToPS
{
    float4 TangentToWorld0 : TEXCOORD10;
    float4 TangentToWorld2 : TEXCOORD11;
    float4 Color           : COLOR0;
    float2 TexCoord        : TEXCOORD0;
    
#if INSTANCED_STEREO
    nointerpolation uint EyeIndex : PACKED_EYE_INDEX;
#endif
};

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Unpack 10/10/10/2 normal to float3.
 */
float3 UnpackVoxelNormal(uint Packed)
{
    float3 N;
    N.x = ((Packed) & 0x3FF) / 1023.0f * 2.0f - 1.0f;
    N.y = ((Packed >> 10) & 0x3FF) / 1023.0f * 2.0f - 1.0f;
    N.z = ((Packed >> 20) & 0x3FF) / 1023.0f * 2.0f - 1.0f;
    return normalize(N);
}

/**
 * Get primitive scene data.
 */
FPrimitiveSceneData GetPrimitiveData(FVertexFactoryIntermediates Intermediates)
{
    return GetPrimitiveData(Intermediates.PrimitiveId);
}

// ============================================================================
// Main Vertex Factory Functions
// ============================================================================

/**
 * Compute intermediates from vertex input.
 */
FVertexFactoryIntermediates GetVertexFactoryIntermediates(FVertexFactoryInput Input)
{
    FVertexFactoryIntermediates Intermediates;
    
    Intermediates.PrimitiveId = 0;
    
    // Unpack normal
    float3 Normal = UnpackVoxelNormal(Input.PackedNormal);
    
    // Build simple tangent basis from normal
    // For voxel terrain, we can use world-aligned tangents
    float3 TangentX = abs(Normal.z) < 0.999f ? 
        normalize(cross(float3(0, 0, 1), Normal)) : 
        float3(1, 0, 0);
    float3 TangentY = cross(Normal, TangentX);
    
    Intermediates.TangentToLocal = half3x3(TangentX, TangentY, Normal);
    Intermediates.TangentToWorldSign = 1.0h;
    
    // Transform to world
    FLWCMatrix LocalToWorld = GetPrimitiveData(Intermediates).LocalToWorld;
    half3x3 LocalToWorld3x3 = GetLocalToWorld3x3(Intermediates.PrimitiveId);
    Intermediates.TangentToWorld = mul(Intermediates.TangentToLocal, LocalToWorld3x3);
    
    // Vertex color
    Intermediates.Color = Input.Color;
    
    // Store pre-skin position
    Intermediates.PreSkinPosition = Input.Position.xyz;
    
    return Intermediates;
}

/**
 * Get world position (main passes).
 */
float4 VertexFactoryGetWorldPosition(FVertexFactoryInput Input, FVertexFactoryIntermediates Intermediates)
{
    return TransformLocalToTranslatedWorld(Input.Position.xyz, Intermediates.PrimitiveId);
}

/**
 * Get world position (position-only passes like depth).
 */
float4 VertexFactoryGetWorldPosition(FPositionOnlyVertexFactoryInput Input)
{
    return TransformLocalToTranslatedWorld(Input.Position.xyz, 0);
}

/**
 * Get world position (position+normal passes).
 */
float4 VertexFactoryGetWorldPosition(FPositionAndNormalOnlyVertexFactoryInput Input)
{
    return TransformLocalToTranslatedWorld(Input.Position.xyz, 0);
}

/**
 * Get raster offset (used for certain effects).
 */
float4 VertexFactoryGetRasterizedWorldPosition(FVertexFactoryInput Input, FVertexFactoryIntermediates Intermediates, float4 InWorldPosition)
{
    return InWorldPosition;
}

/**
 * Get previous frame position (for motion blur / TAA).
 */
float4 VertexFactoryGetPreviousWorldPosition(FVertexFactoryInput Input, FVertexFactoryIntermediates Intermediates)
{
    FLWCMatrix PrevLocalToWorld = GetPrimitiveData(Intermediates).PreviousLocalToWorld;
    return TransformPreviousLocalPositionToTranslatedWorld(Input.Position.xyz, Intermediates.PrimitiveId);
}

/**
 * Get tangent to local matrix.
 */
half3x3 VertexFactoryGetTangentToLocal(FVertexFactoryInput Input, FVertexFactoryIntermediates Intermediates)
{
    return Intermediates.TangentToLocal;
}

/**
 * Build interpolants to pass to pixel shader.
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
    
    // Vertex color
    Interpolants.Color = Intermediates.Color;
    
    // Texture coordinate
    Interpolants.TexCoord = Input.TexCoord;
    
#if INSTANCED_STEREO
    Interpolants.EyeIndex = 0;
#endif
    
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
    
    // Vertex color
    Result.VertexColor = Interpolants.Color;
    
    // Texture coordinates
#if NUM_TEX_COORD_INTERPOLATORS
    Result.TexCoords[0] = Interpolants.TexCoord;
#endif
    
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
    
#if NUM_MATERIAL_TEXCOORDS_VERTEX
    Result.TexCoords[0] = Input.TexCoord;
#endif
    
    return Result;
}

// ============================================================================
// Required interface functions (from VertexFactoryDefaultInterface.ush)
// ============================================================================

uint VertexFactoryGetPrimitiveId(FVertexFactoryInterpolantsVSToPS Interpolants)
{
    return 0;
}

float4 VertexFactoryGetTranslatedPrimitiveVolumeBounds(FVertexFactoryInterpolantsVSToPS Interpolants)
{
    return 0;
}

#if NEEDS_VERTEX_FACTORY_INTERPOLATION
    struct FVertexFactoryRayTracingInterpolants
    {
        FVertexFactoryInterpolantsVSToPS InterpolantsVSToPS;
    };

    FVertexFactoryInterpolantsVSToPS VertexFactoryAssignInterpolants(FVertexFactoryRayTracingInterpolants Input)
    {
        return Input.InterpolantsVSToPS;
    }

    FVertexFactoryRayTracingInterpolants VertexFactoryGetRayTracingInterpolants(
        FVertexFactoryInput Input,
        FVertexFactoryIntermediates Intermediates,
        FMaterialVertexParameters VertexParameters)
    {
        FVertexFactoryRayTracingInterpolants Interpolants;
        Interpolants.InterpolantsVSToPS = VertexFactoryGetInterpolantsVSToPS(Input, Intermediates, VertexParameters);
        return Interpolants;
    }

    FVertexFactoryRayTracingInterpolants VertexFactoryInterpolate(
        FVertexFactoryRayTracingInterpolants a,
        float aInterp,
        FVertexFactoryRayTracingInterpolants b,
        float bInterp)
    {
        // Simple linear interpolation
        FVertexFactoryRayTracingInterpolants O;
        O.InterpolantsVSToPS.TangentToWorld0 = a.InterpolantsVSToPS.TangentToWorld0 * aInterp + b.InterpolantsVSToPS.TangentToWorld0 * bInterp;
        O.InterpolantsVSToPS.TangentToWorld2 = a.InterpolantsVSToPS.TangentToWorld2 * aInterp + b.InterpolantsVSToPS.TangentToWorld2 * bInterp;
        O.InterpolantsVSToPS.Color = a.InterpolantsVSToPS.Color * aInterp + b.InterpolantsVSToPS.Color * bInterp;
        O.InterpolantsVSToPS.TexCoord = a.InterpolantsVSToPS.TexCoord * aInterp + b.InterpolantsVSToPS.TexCoord * bInterp;
        return O;
    }
#endif

// Include default implementations for any functions we didn't override
// This provides stubs for compute shader related functions, etc.
#include "/Engine/Private/VertexFactoryDefaultInterface.ush"
```

---

## Step 4.2: Verification Checkpoint

**At this point:**
1. Compile the project
2. Check Output Log for shader compilation errors
3. Common errors and fixes:
   - "Undeclared identifier" → Missing include or function implementation
   - "Cannot find shader file" → Shader directory not registered properly
   - "Shader type loaded after engine init" → LoadingPhase not PostConfigInit

---

# Phase 5: Update Scene Proxy to Use Custom Vertex Factory

## Step 5.1: Create Updated Test Component

Create a new version using our custom vertex factory:

**File**: `Source/VoxelRendering/Public/VoxelTestComponent.h`

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "VoxelTestComponent.generated.h"

/**
 * Test component using our custom FVoxelVertexFactory.
 */
UCLASS(ClassGroup=(Rendering), meta=(BlueprintSpawnableComponent))
class VOXELRENDERING_API UVoxelTestComponent : public UPrimitiveComponent
{
    GENERATED_BODY()

public:
    UVoxelTestComponent();

    virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
    virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
    virtual int32 GetNumMaterials() const override { return 1; }
    virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;
    virtual void SetMaterial(int32 ElementIndex, UMaterialInterface* Material) override;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel")
    TObjectPtr<UMaterialInterface> Material;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel", meta = (ClampMin = "1.0"))
    float QuadSize = 100.0f;

    UFUNCTION(BlueprintCallable, Category = "Voxel")
    void RefreshMesh();
};
```

**File**: `Source/VoxelRendering/Private/VoxelTestComponent.cpp`

```cpp
#include "VoxelTestComponent.h"
#include "VoxelVertexFactory.h"
#include "PrimitiveSceneProxy.h"
#include "Materials/Material.h"
#include "MaterialShared.h"
#include "Engine/Engine.h"

//-----------------------------------------------------------------------------
// Scene Proxy using FVoxelVertexFactory
//-----------------------------------------------------------------------------
class FVoxelTestSceneProxy : public FPrimitiveSceneProxy
{
public:
    FVoxelTestSceneProxy(UVoxelTestComponent* Component)
        : FPrimitiveSceneProxy(Component)
        , MaterialInterface(Component->GetMaterial(0))
        , VertexFactory(GetScene().GetFeatureLevel(), "FVoxelTestSceneProxy")
    {
        // Get material
        if (MaterialInterface == nullptr)
        {
            MaterialInterface = UMaterial::GetDefaultMaterial(MD_Surface);
        }
        MaterialRelevance = MaterialInterface->GetRelevance(GetScene().GetFeatureLevel());

        // Build quad using FVoxelVertex format
        const float HalfSize = Component->QuadSize * 0.5f;
        
        auto AddVertex = [this](const FVector3f& Pos, const FVector3f& Normal, const FVector2f& UV, const FColor& Color)
        {
            VertexBuffer.Vertices.Add(FVoxelVertex(Pos, Normal, UV, Color));
        };

        // Quad facing +Z with vertex colors
        const FVector3f Normal(0, 0, 1);
        AddVertex(FVector3f(-HalfSize, -HalfSize, 0), Normal, FVector2f(0, 0), FColor::Red);
        AddVertex(FVector3f( HalfSize, -HalfSize, 0), Normal, FVector2f(1, 0), FColor::Green);
        AddVertex(FVector3f( HalfSize,  HalfSize, 0), Normal, FVector2f(1, 1), FColor::Blue);
        AddVertex(FVector3f(-HalfSize,  HalfSize, 0), Normal, FVector2f(0, 1), FColor::Yellow);

        // Two triangles
        IndexBuffer.Indices = { 0, 1, 2, 0, 2, 3 };

        NumVertices = VertexBuffer.Vertices.Num();
        NumIndices = IndexBuffer.Indices.Num();
    }

    virtual ~FVoxelTestSceneProxy()
    {
        VertexBuffer.ReleaseResource();
        IndexBuffer.ReleaseResource();
        VertexFactory.ReleaseResource();
    }

    virtual SIZE_T GetTypeHash() const override
    {
        static size_t UniquePointer;
        return reinterpret_cast<size_t>(&UniquePointer);
    }

    virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override
    {
        VertexBuffer.InitResource(RHICmdList);
        IndexBuffer.InitResource(RHICmdList);
        VertexFactory.Init(RHICmdList, &VertexBuffer);
        VertexFactory.InitResource(RHICmdList);
    }

    virtual void GetDynamicMeshElements(
        const TArray<const FSceneView*>& Views,
        const FSceneViewFamily& ViewFamily,
        uint32 VisibilityMap,
        FMeshElementCollector& Collector) const override
    {
        if (NumVertices == 0 || NumIndices == 0)
        {
            return;
        }

        const bool bWireframe = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;
        FMaterialRenderProxy* MaterialProxy = nullptr;
        
        if (bWireframe)
        {
            FColoredMaterialRenderProxy* WireframeMaterialInstance = new FColoredMaterialRenderProxy(
                GEngine->WireframeMaterial ? GEngine->WireframeMaterial->GetRenderProxy() : nullptr,
                FLinearColor(0, 0.5f, 1.f)
            );
            Collector.RegisterOneFrameMaterialProxy(WireframeMaterialInstance);
            MaterialProxy = WireframeMaterialInstance;
        }
        else
        {
            MaterialProxy = MaterialInterface->GetRenderProxy();
        }

        for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
        {
            if (VisibilityMap & (1 << ViewIndex))
            {
                FMeshBatch& Mesh = Collector.AllocateMesh();
                
                Mesh.VertexFactory = &VertexFactory;
                Mesh.MaterialRenderProxy = MaterialProxy;
                Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
                Mesh.Type = PT_TriangleList;
                Mesh.DepthPriorityGroup = SDPG_World;
                Mesh.bCanApplyViewModeOverrides = true;
                Mesh.bUseWireframeSelectionColoring = IsSelected();

                FMeshBatchElement& BatchElement = Mesh.Elements[0];
                BatchElement.IndexBuffer = &IndexBuffer;
                BatchElement.FirstIndex = 0;
                BatchElement.NumPrimitives = NumIndices / 3;
                BatchElement.MinVertexIndex = 0;
                BatchElement.MaxVertexIndex = NumVertices - 1;
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
        Result.bDynamicRelevance = true;
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
    
    FVoxelVertexBuffer VertexBuffer;
    FVoxelIndexBuffer IndexBuffer;
    FVoxelVertexFactory VertexFactory;
    
    int32 NumVertices = 0;
    int32 NumIndices = 0;
};

//-----------------------------------------------------------------------------
// Component Implementation
//-----------------------------------------------------------------------------

UVoxelTestComponent::UVoxelTestComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
}

FPrimitiveSceneProxy* UVoxelTestComponent::CreateSceneProxy()
{
    if (QuadSize <= 0)
    {
        return nullptr;
    }
    return new FVoxelTestSceneProxy(this);
}

FBoxSphereBounds UVoxelTestComponent::CalcBounds(const FTransform& LocalToWorld) const
{
    const float HalfSize = QuadSize * 0.5f;
    FBox LocalBox(
        FVector(-HalfSize, -HalfSize, -1.0f),
        FVector(HalfSize, HalfSize, 1.0f)
    );
    return FBoxSphereBounds(LocalBox).TransformBy(LocalToWorld);
}

UMaterialInterface* UVoxelTestComponent::GetMaterial(int32 ElementIndex) const
{
    return Material;
}

void UVoxelTestComponent::SetMaterial(int32 ElementIndex, UMaterialInterface* InMaterial)
{
    Material = InMaterial;
    MarkRenderStateDirty();
}

void UVoxelTestComponent::RefreshMesh()
{
    MarkRenderStateDirty();
}
```

---

# Phase 6: Testing & Debugging

## Step 6.1: Final Verification Checklist

```
□ Project compiles without errors
□ No shader compilation errors in Output Log
□ Editor launches without crash
□ Can add UVoxelTestComponent to an actor
□ Quad is visible in viewport
□ Quad is properly lit (not solid black)
□ Vertex colors appear (with vertex color material)
□ Transform (move/rotate/scale) works
□ Wireframe view mode works
□ No errors in Output Log during play
```

## Step 6.2: Creating a Vertex Color Material for Testing

In Editor:
1. Right-click in Content Browser → Material
2. Name it "M_VertexColor"
3. Open material editor
4. Add node: "Vertex Color"
5. Connect RGB to Base Color
6. Save

Use this material on your VoxelTestComponent to verify vertex colors work.

## Step 6.3: Common Issues and Debugging

### Issue: Black Mesh
**Check:**
- Material is assigned and valid
- `ShouldCompilePermutation` returns true for your material
- Normal is being passed correctly
- TangentToWorld is being computed correctly

**Debug:**
- Try with DefaultMaterial
- Try with unlit material
- Check shader compilation log

### Issue: Mesh Doesn't Appear
**Check:**
- `CalcBounds` returns valid, non-zero bounds
- `GetViewRelevance` returns `bDrawRelevance = true`
- Vertices and indices are populated
- Scene proxy is being created

**Debug:**
- Add logging in CreateSceneProxy
- Check NumVertices/NumIndices

### Issue: Crash on Startup
**Check:**
- Module LoadingPhase is PostConfigInit
- Shader file path is correct in IMPLEMENT_VERTEX_FACTORY_TYPE
- Shader directory is registered before shader types

**Debug:**
- Check crash callstack for shader-related functions
- Search log for "Shader type was loaded after engine init"

### Issue: Shader Compilation Errors
**Check:**
- All required functions are implemented
- Includes are correct
- Syntax is valid HLSL

**Debug:**
- Check ShaderCompileWorker log
- Look for specific line numbers in errors

---

# Phase 7: Integration with Voxel System

Once the minimal example works, integration involves:

## Step 7.1: Create FVoxelChunkSceneProxy

```cpp
class FVoxelChunkSceneProxy : public FPrimitiveSceneProxy
{
    // Multiple chunks
    struct FChunkSection
    {
        FVoxelVertexBuffer VertexBuffer;
        FVoxelIndexBuffer IndexBuffer;
        FVoxelVertexFactory VertexFactory;
        FIntVector ChunkCoord;
        int32 LODLevel;
    };
    
    TMap<FIntVector, TUniquePtr<FChunkSection>> Sections;
    
    // Methods to add/remove/update chunk sections
    void UpdateChunkSection(FIntVector ChunkCoord, const FChunkMeshData& MeshData);
    void RemoveChunkSection(FIntVector ChunkCoord);
};
```

## Step 7.2: Implement IVoxelMeshRenderer

```cpp
class FVoxelCustomVFRenderer : public IVoxelMeshRenderer
{
    // Single scene proxy for all chunks
    TSharedPtr<FVoxelChunkSceneProxy> SceneProxy;
    
    // Implementation of interface
    virtual void UpdateChunkMesh(const FChunkRenderData& RenderData) override;
    virtual void RemoveChunk(FIntVector ChunkCoord) override;
    virtual void UpdateLODTransition(FIntVector ChunkCoord, float MorphFactor) override;
};
```

## Step 7.3: GPU Buffer Direct Binding (Advanced)

For compute shader output → vertex factory without CPU readback:
1. Compute shader outputs to structured buffer
2. Convert RDG buffer to persistent RHI buffer
3. Bind directly to vertex factory streams
4. No FMemory::Memcpy needed

---

# Summary: File Checklist

```
VoxelWorlds/
├── VoxelWorlds.uplugin                    [Modified: LoadingPhase]
├── Shaders/
│   └── Private/
│       └── VoxelVertexFactory.ush         [Created: Phase 4]
└── Source/
    └── VoxelRendering/
        ├── VoxelRendering.Build.cs        [Modified: Dependencies]
        ├── Public/
        │   ├── VoxelRenderingModule.h     [Created: Phase 1]
        │   ├── SimpleVoxelTestComponent.h [Created: Phase 2]
        │   ├── VoxelVertexFactory.h       [Created: Phase 3]
        │   └── VoxelTestComponent.h       [Created: Phase 5]
        └── Private/
            ├── VoxelRenderingModule.cpp   [Created: Phase 1]
            ├── SimpleVoxelTestComponent.cpp [Created: Phase 2]
            ├── VoxelVertexFactory.cpp     [Created: Phase 3]
            └── VoxelTestComponent.cpp     [Created: Phase 5]
```

---

# Recommended Claude Code Commands

Use these prompts with Claude Code when implementing:

```
Phase 1:
"Update VoxelRendering.Build.cs with required dependencies for custom vertex factory"
"Create VoxelRenderingModule.cpp that registers shader directory"
"Update VoxelWorlds.uplugin to use PostConfigInit for VoxelRendering module"

Phase 2:
"Create SimpleVoxelTestComponent using FLocalVertexFactory to render a quad"
"The component should use FDynamicMeshVertex format and verify rendering works"

Phase 3:
"Create VoxelVertexFactory.h with FVoxelVertex struct (28 bytes: Position, PackedNormal, UV, Color)"
"Implement FVoxelVertexFactory inheriting from FLocalVertexFactory"

Phase 4:
"Create VoxelVertexFactory.ush shader file with all required functions"
"The shader should unpack 10/10/10/2 normals and pass vertex colors"

Phase 5:
"Create VoxelTestComponent using FVoxelVertexFactory to render a colored quad"

Phase 6:
"Debug: Why is my custom vertex factory showing a black mesh?"
"Check shader compilation errors in the output log"

Phase 7:
"Integrate FVoxelVertexFactory with the existing voxel chunk system"
"Create FVoxelChunkSceneProxy that manages multiple chunk sections"
```

---

*Implementation Plan v1.0 - Created 2026-02-01*

---

# Appendix A: Phase 3 Implementation Results (2026-02-01)

## Working Approach: Use FLocalVertexFactory Directly

After extensive debugging, we discovered that **extending FLocalVertexFactory with IMPLEMENT_VERTEX_FACTORY_TYPE creates a NEW vertex factory type that doesn't inherit FLocalVertexFactory's compiled shaders**. This causes "uniform buffer at slot 2 null (LocalVF)" errors.

### The Correct Approach

**Use `FLocalVertexFactory` directly without extending it.** Configure it via `SetData()` with proper `FDataType` setup.

### Key Files Created

1. **`Public/LocalVFTestComponent.h`** - Test component header with:
   - `FLocalVFTestVertex` struct (32 bytes: Position, TangentX, TangentZ, TexCoord, Color)
   - `FLocalVFTestVertexBuffer` with separate color buffer for SRV
   - `InitLocalVertexFactoryStreams()` helper function
   - `ULocalVFTestComponent` test component

2. **`Private/LocalVFTestComponent.cpp`** - Implementation with working scene proxy

### Critical Learnings

#### 1. Don't Extend FLocalVertexFactory with IMPLEMENT_VERTEX_FACTORY_TYPE

```cpp
// WRONG - Creates new shader type without compiled shaders
class FMyVertexFactory : public FLocalVertexFactory
{
    DECLARE_VERTEX_FACTORY_TYPE(FMyVertexFactory);
};
IMPLEMENT_VERTEX_FACTORY_TYPE(FMyVertexFactory, ...);  // DON'T DO THIS

// CORRECT - Use FLocalVertexFactory directly
FLocalVertexFactory VertexFactory(FeatureLevel, "MySceneProxy");
// Configure via SetData()
```

#### 2. Interleaved Buffers and SRVs Don't Mix

For interleaved vertex data, **SRVs cannot read correctly** because they expect contiguous data of a single type. The solution:

- **Vertex Stream Components**: Work correctly with interleaved data (specify offset and stride)
- **SRVs**: Use global null buffer SRVs for most attributes, create separate buffer for colors

```cpp
// For Position/Tangent/TexCoord SRVs - use global null buffer
NewData.PositionComponentSRV = GNullColorVertexBuffer.VertexBufferSRV;
NewData.TangentsSRV = GNullColorVertexBuffer.VertexBufferSRV;
NewData.TextureCoordinatesSRV = GNullColorVertexBuffer.VertexBufferSRV;

// For Color SRV - FLocalVertexFactory uses this for manual vertex fetch
// Must create separate contiguous color buffer
NewData.ColorComponentsSRV = SeparateColorBufferSRV;
```

#### 3. Separate Color Buffer Required

FLocalVertexFactory uses `ColorComponentsSRV` for vertex color fetching. For interleaved vertex data, create a separate buffer containing just colors:

```cpp
// In vertex buffer InitRHI:
// 1. Create interleaved vertex buffer (for vertex stream components)
VertexBufferRHI = RHICmdList.CreateBuffer(SizeInBytes, BUF_Static | BUF_VertexBuffer, ...);

// 2. Create separate color buffer (for ColorComponentsSRV)
ColorBufferRHI = RHICmdList.CreateBuffer(NumVertices * sizeof(FColor), BUF_Static | BUF_ShaderResource, ...);

// Copy just color data to separate buffer
FColor* ColorData = (FColor*)RHICmdList.LockBuffer(ColorBufferRHI, ...);
for (int32 i = 0; i < Vertices.Num(); i++)
{
    ColorData[i] = Vertices[i].Color;
}
RHICmdList.UnlockBuffer(ColorBufferRHI);

// Create SRV from separate color buffer
ColorSRV = RHICmdList.CreateShaderResourceView(ColorBufferRHI, sizeof(FColor), PF_B8G8R8A8);
```

#### 4. FColor Memory Layout is BGRA

`FColor` stores bytes in BGRA order. Use `PF_B8G8R8A8` format for color SRVs:

```cpp
// CORRECT
ColorSRV = RHICmdList.CreateShaderResourceView(ColorBufferRHI, sizeof(FColor), PF_B8G8R8A8);

// WRONG - channels will be swapped
ColorSRV = RHICmdList.CreateShaderResourceView(ColorBufferRHI, sizeof(FColor), PF_R8G8B8A8);
```

#### 5. Use FPackedNormal for Tangent Basis

`FLocalVertexFactory` expects `VET_PackedNormal` format for tangent basis:

```cpp
struct FLocalVFTestVertex
{
    FVector3f Position;       // 12 bytes
    FPackedNormal TangentX;   // 4 bytes
    FPackedNormal TangentZ;   // 4 bytes (W contains binormal sign)
    FVector2f TexCoord;       // 8 bytes
    FColor Color;             // 4 bytes
};

// Set binormal sign in constructor
TangentZ.Vector.W = 127;  // 1.0 = no flip
```

### Helper Function Pattern

```cpp
void InitLocalVertexFactoryStreams(
    FRHICommandListBase& RHICmdList,
    FLocalVertexFactory* VertexFactory,
    const FVertexBuffer* PositionBuffer,
    uint32 Stride,
    uint32 PositionOffset,
    uint32 TangentXOffset,
    uint32 TangentZOffset,
    uint32 TexCoordOffset,
    uint32 ColorOffset,
    FRHIShaderResourceView* PositionSRV,
    FRHIShaderResourceView* TangentsSRV,
    FRHIShaderResourceView* TexCoordSRV,
    FRHIShaderResourceView* ColorSRV)
{
    FLocalVertexFactory::FDataType NewData;

    // Vertex stream components (read from interleaved buffer)
    NewData.PositionComponent = FVertexStreamComponent(PositionBuffer, PositionOffset, Stride, VET_Float3);
    NewData.TangentBasisComponents[0] = FVertexStreamComponent(PositionBuffer, TangentXOffset, Stride, VET_PackedNormal);
    NewData.TangentBasisComponents[1] = FVertexStreamComponent(PositionBuffer, TangentZOffset, Stride, VET_PackedNormal);
    NewData.TextureCoordinates.Add(FVertexStreamComponent(PositionBuffer, TexCoordOffset, Stride, VET_Float2));
    NewData.ColorComponent = FVertexStreamComponent(PositionBuffer, ColorOffset, Stride, VET_Color);

    // SRVs (use global null buffers except for color)
    NewData.PositionComponentSRV = GNullColorVertexBuffer.VertexBufferSRV;
    NewData.TangentsSRV = GNullColorVertexBuffer.VertexBufferSRV;
    NewData.TextureCoordinatesSRV = GNullColorVertexBuffer.VertexBufferSRV;
    NewData.ColorComponentsSRV = ColorSRV ? ColorSRV : GNullColorVertexBuffer.VertexBufferSRV.GetReference();

    NewData.LightMapCoordinateIndex = 0;
    NewData.NumTexCoords = 1;

    VertexFactory->SetData(RHICmdList, NewData);
}
```

### Verification

The `ULocalVFTestComponent` successfully renders a quad with vertex colors (Red, Green, Blue, Yellow corners) using:
- FLocalVertexFactory directly (no custom shader needed)
- Interleaved vertex buffer for vertex stream components
- Separate color buffer for ColorComponentsSRV
- Material with VertexColor node connected to BaseColor

### Next Steps

Apply this pattern to refactor `FVoxelVertexFactory`:
1. Remove IMPLEMENT_VERTEX_FACTORY_TYPE
2. Use FLocalVertexFactory directly
3. Create separate color buffer for SRV
4. Use InitLocalVertexFactoryStreams helper function

---

*Appendix A added 2026-02-01 after Phase 3 debugging session*
