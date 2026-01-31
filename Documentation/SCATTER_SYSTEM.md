# Scatter System

**Module**: VoxelScatter  
**Dependencies**: VoxelCore, VoxelGeneration

## Overview

The scatter system places vegetation (trees, grass, rocks) on voxel terrain using GPU-driven placement and Hierarchical Instanced Static Mesh (HISM) rendering.

## Scatter Definition

```cpp
/**
 * Scatter Definition
 * 
 * Defines rules for placing a type of scatter (trees, grass, etc.)
 */
struct FScatterDefinition
{
    /** Scatter ID */
    int32 ScatterID;
    
    /** Display name */
    FString ScatterName;
    
    /** Static mesh to instance */
    UStaticMesh* Mesh;
    
    // ==================== Placement Rules ====================
    
    /** Density (instances per square meter) */
    float Density;
    
    /** Minimum slope (degrees) */
    float MinSlope;
    
    /** Maximum slope (degrees) */
    float MaxSlope;
    
    /** Allowed materials (surface must be one of these) */
    TArray<uint8> AllowedMaterials;
    
    /** Allowed biomes */
    TArray<uint8> AllowedBiomes;
    
    /** Minimum elevation */
    float MinElevation;
    
    /** Maximum elevation */
    float MaxElevation;
    
    // ==================== Instance Variation ====================
    
    /** Scale range */
    FVector2D ScaleRange;
    
    /** Random rotation? */
    bool bRandomRotation;
    
    /** Align to surface normal? */
    bool bAlignToNormal;
    
    /** Offset from surface (cm) */
    float SurfaceOffset;
    
    // ==================== Culling ====================
    
    /** LOD distances */
    TArray<float> LODDistances;
    
    /** Cull distance */
    float CullDistance;
};
```

## GPU Scatter Generation

```cpp
/**
 * Generate scatter for a chunk (GPU compute shader)
 */
void UVoxelScatterManager::GenerateScatterForChunk(FIntVector ChunkCoord)
{
    FRDGBuilder GraphBuilder(RHICmdList);
    
    // Get surface voxels for chunk
    FRDGBufferRef SurfaceBuffer = GetChunkSurfaceVoxels(ChunkCoord);
    
    // Create output buffer for instances
    FRDGBufferDesc InstanceBufferDesc = FRDGBufferDesc::CreateStructuredDesc(
        sizeof(FScatterInstance),
        MaxInstancesPerChunk
    );
    FRDGBufferRef InstanceBuffer = GraphBuilder.CreateBuffer(InstanceBufferDesc, TEXT("ScatterInstances"));
    
    // Dispatch scatter generation shader
    FScatterGenerationCS::FParameters* Params = GraphBuilder.AllocParameters<FScatterGenerationCS::FParameters>();
    Params->SurfaceVoxels = GraphBuilder.CreateSRV(SurfaceBuffer);
    Params->OutInstances = GraphBuilder.CreateUAV(InstanceBuffer);
    Params->ScatterDefinitions = ScatterDefinitionsBuffer;
    Params->ChunkCoord = ChunkCoord;
    Params->RandomSeed = GetRandomSeed(ChunkCoord);
    
    TShaderMapRef<FScatterGenerationCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("GenerateScatter"),
        ComputeShader,
        Params,
        FIntVector(ChunkSize, ChunkSize, 1)
    );
    
    GraphBuilder.Execute();
    
    // Extract instances to HISM
    UpdateHISMInstances(ChunkCoord, InstanceBuffer);
}
```

### Scatter Shader

```hlsl
// ScatterGeneration.usf

struct ScatterDefinition
{
    float Density;
    float MinSlope;
    float MaxSlope;
    uint MaterialMask;  // Bitmask of allowed materials
    float MinElevation;
    float MaxElevation;
    float2 ScaleRange;
    // ...
};

struct SurfaceVoxel
{
    float3 WorldPosition;
    float3 Normal;
    uint MaterialID;
    uint BiomeID;
};

struct ScatterInstance
{
    float3 Position;
    float4 Rotation;  // Quaternion
    float Scale;
    uint ScatterID;
};

StructuredBuffer<SurfaceVoxel> SurfaceVoxels;
StructuredBuffer<ScatterDefinition> ScatterDefinitions;
RWStructuredBuffer<ScatterInstance> OutInstances;

[numthreads(8,8,1)]
void GenerateScatter(uint3 ThreadID : SV_DispatchThreadID)
{
    uint VoxelIndex = ThreadID.x + ThreadID.y * ChunkSize;
    if (VoxelIndex >= SurfaceVoxelCount) return;
    
    SurfaceVoxel Voxel = SurfaceVoxels[VoxelIndex];
    
    // Calculate slope
    float Slope = acos(dot(Voxel.Normal, float3(0,0,1))) * 57.2958; // Radians to degrees
    
    // Iterate scatter types
    for (uint i = 0; i < ScatterTypeCount; ++i)
    {
        ScatterDefinition Scatter = ScatterDefinitions[i];
        
        // Check placement rules
        if (Slope < Scatter.MinSlope || Slope > Scatter.MaxSlope) continue;
        if (Voxel.WorldPosition.z < Scatter.MinElevation || Voxel.WorldPosition.z > Scatter.MaxElevation) continue;
        if (!IsMaterialAllowed(Voxel.MaterialID, Scatter.MaterialMask)) continue;
        
        // Poisson disk sampling for placement
        uint Seed = Hash(VoxelIndex + RandomSeed + i);
        float Random = Random01(Seed);
        
        if (Random < Scatter.Density)
        {
            // Create instance
            ScatterInstance Instance;
            Instance.Position = Voxel.WorldPosition + Voxel.Normal * Scatter.SurfaceOffset;
            Instance.Scale = lerp(Scatter.ScaleRange.x, Scatter.ScaleRange.y, Random01(Seed + 1));
            
            if (Scatter.bRandomRotation)
            {
                float Angle = Random01(Seed + 2) * 6.28318; // 0 to 2π
                Instance.Rotation = QuaternionFromAxisAngle(float3(0,0,1), Angle);
            }
            else if (Scatter.bAlignToNormal)
            {
                Instance.Rotation = QuaternionLookRotation(Voxel.Normal, float3(0,0,1));
            }
            else
            {
                Instance.Rotation = float4(0,0,0,1); // Identity
            }
            
            Instance.ScatterID = i;
            
            // Append to output (atomic)
            uint OutputIndex;
            InterlockedAdd(InstanceCount, 1, OutputIndex);
            
            if (OutputIndex < MaxInstancesPerChunk)
            {
                OutInstances[OutputIndex] = Instance;
            }
        }
    }
}
```

## HISM Management

```cpp
/**
 * Scatter Component Manager
 * 
 * Manages HISMs for scatter rendering.
 */
class UVoxelScatterComponent : public USceneComponent
{
public:
    /** HISM components per scatter type */
    TMap<int32, UHierarchicalInstancedStaticMeshComponent*> HISMComponents;
    
    /**
     * Create HISM for scatter type
     */
    UHierarchicalInstancedStaticMeshComponent* GetOrCreateHISM(int32 ScatterID)
    {
        if (UHierarchicalInstancedStaticMeshComponent** Existing = HISMComponents.Find(ScatterID))
        {
            return *Existing;
        }
        
        const FScatterDefinition& Scatter = ScatterRegistry->GetScatter(ScatterID);
        
        UHierarchicalInstancedStaticMeshComponent* HISM = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
        HISM->SetStaticMesh(Scatter.Mesh);
        HISM->SetCullDistances(0, Scatter.CullDistance);
        HISM->SetupAttachment(this);
        HISM->RegisterComponent();
        
        HISMComponents.Add(ScatterID, HISM);
        return HISM;
    }
    
    /**
     * Update instances for a chunk
     */
    void UpdateChunkInstances(FIntVector ChunkCoord, const TArray<FScatterInstance>& Instances)
    {
        // Group instances by scatter type
        TMap<int32, TArray<FTransform>> InstancesByType;
        
        for (const FScatterInstance& Instance : Instances)
        {
            FTransform Transform;
            Transform.SetLocation(Instance.Position);
            Transform.SetRotation(Instance.Rotation);
            Transform.SetScale3D(FVector(Instance.Scale));
            
            InstancesByType.FindOrAdd(Instance.ScatterID).Add(Transform);
        }
        
        // Update HISMs
        for (auto& Pair : InstancesByType)
        {
            int32 ScatterID = Pair.Key;
            TArray<FTransform>& Transforms = Pair.Value;
            
            UHierarchicalInstancedStaticMeshComponent* HISM = GetOrCreateHISM(ScatterID);
            
            // Clear old instances for this chunk
            ClearChunkInstances(HISM, ChunkCoord);
            
            // Add new instances
            for (const FTransform& Transform : Transforms)
            {
                HISM->AddInstance(Transform);
            }
            
            // Build spatial tree
            HISM->BuildTreeIfOutdated(true, false);
        }
    }
};
```

## Biome Integration

```cpp
/**
 * Get scatter types for biome
 */
TArray<FScatterDefinition> GetScatterForBiome(uint8 BiomeID)
{
    const FBiomeDefinition& Biome = BiomeRegistry->GetBiome(BiomeID);
    
    TArray<FScatterDefinition> Result;
    
    // Trees
    if (Biome.TreeDensity > 0.0f)
    {
        FScatterDefinition TreeScatter;
        TreeScatter.ScatterID = 0;
        TreeScatter.Mesh = TreeMeshes[Biome.BiomeID];
        TreeScatter.Density = Biome.TreeDensity;
        TreeScatter.MinSlope = 0.0f;
        TreeScatter.MaxSlope = 30.0f;
        TreeScatter.AllowedMaterials = { Biome.SurfaceMaterial };
        TreeScatter.ScaleRange = FVector2D(0.8f, 1.2f);
        TreeScatter.bRandomRotation = true;
        TreeScatter.bAlignToNormal = true;
        
        Result.Add(TreeScatter);
    }
    
    // Grass
    if (Biome.GrassDensity > 0.0f)
    {
        FScatterDefinition GrassScatter;
        GrassScatter.ScatterID = 1;
        GrassScatter.Mesh = GrassMesh;
        GrassScatter.Density = Biome.GrassDensity;
        GrassScatter.MinSlope = 0.0f;
        GrassScatter.MaxSlope = 45.0f;
        GrassScatter.AllowedMaterials = { Biome.SurfaceMaterial };
        GrassScatter.ScaleRange = FVector2D(0.9f, 1.1f);
        GrassScatter.bRandomRotation = true;
        GrassScatter.bAlignToNormal = true;
        GrassScatter.CullDistance = 5000.0f;  // Grass culls closer
        
        Result.Add(GrassScatter);
    }
    
    return Result;
}
```

## Performance Optimization

### Instance Pooling

```cpp
// Reuse instances when chunks load/unload
TMap<FIntVector, TArray<FScatterInstance>> InstanceCache;

void UVoxelScatterManager::UnloadChunk(FIntVector ChunkCoord)
{
    // Cache instances before unloading
    TArray<FScatterInstance> Instances = GetChunkInstances(ChunkCoord);
    InstanceCache.Add(ChunkCoord, Instances);
    
    // Clear from HISM
    ClearChunkFromHISM(ChunkCoord);
}

void UVoxelScatterManager::LoadChunk(FIntVector ChunkCoord)
{
    // Check cache first
    if (TArray<FScatterInstance>* Cached = InstanceCache.Find(ChunkCoord))
    {
        UpdateChunkInstances(ChunkCoord, *Cached);
        InstanceCache.Remove(ChunkCoord);
    }
    else
    {
        // Generate new
        GenerateScatterForChunk(ChunkCoord);
    }
}
```

### LOD System

```cpp
// Update scatter LOD based on distance
void UpdateScatterLOD()
{
    FVector ViewerPos = GetViewerPosition();
    
    for (auto& Pair : HISMComponents)
    {
        UHierarchicalInstancedStaticMeshComponent* HISM = Pair.Value;
        const FScatterDefinition& Scatter = GetScatterDefinition(Pair.Key);
        
        // Set LOD distances
        HISM->OverrideMaterials = GetLODMaterials(Scatter.ScatterID);
        
        // Cull based on distance
        float DistanceToViewer = FVector::Dist(HISM->GetComponentLocation(), ViewerPos);
        HISM->SetVisibility(DistanceToViewer < Scatter.CullDistance);
    }
}
```

## Next Steps

1. Implement scatter generation shader
2. Create HISM management system
3. Define scatter types (trees, grass, rocks)
4. Integrate with biome system
5. Add LOD and culling
6. Optimize instance pooling

See [BIOME_SYSTEM.md](BIOME_SYSTEM.md) for biome-scatter integration.

