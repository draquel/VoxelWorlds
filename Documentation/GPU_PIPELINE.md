# GPU Compute Pipeline

**Modules**: VoxelGeneration, VoxelMeshing
**Dependencies**: VoxelCore

## Overview

The GPU compute pipeline handles all voxel generation and meshing on the GPU, minimizing CPU overhead and maximizing parallelism.

## Module Configuration

**IMPORTANT**: Modules that use global shaders (via `IMPLEMENT_GLOBAL_SHADER`) must be configured with `LoadingPhase: PostConfigInit` in the `.uplugin` file. This ensures the shader system is initialized before the module loads.

```json
{
    "Name": "VoxelGeneration",
    "Type": "Runtime",
    "LoadingPhase": "PostConfigInit"
}
```

Failure to set this loading phase will cause editor crashes during startup with access violations in `CreatePackage` or `ProcessNewlyLoadedUObjects`.

### Header Best Practices

To avoid static initialization issues in GPU modules:

1. **Minimize RDG includes in headers** - Forward declare types like `FRDGPooledBuffer` instead of including heavy headers
2. **Move heavy includes to .cpp files** - `RenderGraphBuilder.h`, `RenderGraphResources.h`, etc.
3. **Use `RHIFwd.h`** for forward declarations of RHI types in headers

## Pipeline Stages

### Stage 1: Voxel Generation

**Input**: Chunk coordinate, LOD level, world configuration
**Output**: VoxelData buffer (GPU memory)

```hlsl
// GenerateVoxelData.usf
[numthreads(4,4,4)]
void GenerateVoxelData(uint3 DispatchThreadID : SV_DispatchThreadID) {
    // Calculate world position
    float3 WorldPos = ChunkCoord * ChunkSize * VoxelSize + DispatchThreadID * VoxelSize;
    
    // Sample noise for base terrain
    float Density = SampleNoise(WorldPos, NoiseParams);
    
    // Apply world mode SDF
    if (WorldMode == SPHERICAL_PLANET) {
        Density = ApplySphericalSDF(WorldPos, PlanetCenter, PlanetRadius, Density);
    }
    
    // Determine biome
    float Temperature = SampleNoise(WorldPos, TempNoiseParams);
    float Moisture = SampleNoise(WorldPos, MoistureNoiseParams);
    uint BiomeID = SelectBiome(Temperature, Moisture, Density);
    
    // Determine material
    uint MaterialID = GetMaterialForBiome(BiomeID, Density, WorldPos);
    
    // Apply edit layer
    if (HasEdits) {
        ApplyEdits(DispatchThreadID, Density, MaterialID);
    }
    
    // Write output
    VoxelData Output;
    Output.Density = saturate(Density) * 255;
    Output.MaterialID = MaterialID;
    Output.BiomeID = BiomeID;
    Output.Metadata = 0;
    
    OutputBuffer[FlatIndex(DispatchThreadID)] = Output;
}
```

### Stage 2: Cubic Meshing

**Input**: VoxelData buffer
**Output**: Vertex and index buffers

```hlsl
// GenerateCubicMesh.usf
[numthreads(8,8,1)]
void GenerateCubicMesh(uint3 DispatchThreadID : SV_DispatchThreadID) {
    // For each voxel
    uint3 VoxelPos = DispatchThreadID;
    VoxelData Voxel = InputBuffer[FlatIndex(VoxelPos)];
    
    if (Voxel.Density < 127) return; // Air voxel
    
    // Check each face
    for (int Face = 0; Face < 6; Face++) {
        // Neighbor culling
        uint3 NeighborPos = VoxelPos + FaceOffset[Face];
        VoxelData Neighbor = InputBuffer[FlatIndex(NeighborPos)];
        
        if (Neighbor.Density >= 127) continue; // Face hidden
        
        // Greedy meshing (optional)
        uint2 QuadSize = CalculateGreedyQuad(VoxelPos, Face);
        
        // Calculate ambient occlusion
        float4 AO = CalculateAO(VoxelPos, Face);
        
        // Get atlas UV
        float2 AtlasUV = GetAtlasUV(Voxel.MaterialID, Face);
        
        // Emit quad
        EmitQuad(VoxelPos, Face, QuadSize, AO, AtlasUV);
    }
}
```

### Stage 3: Smooth Meshing (Marching Cubes)

**Input**: VoxelData buffer
**Output**: Vertex and index buffers

```hlsl
// GenerateSmoothMesh.usf
[numthreads(4,4,4)]
void GenerateSmoothMesh(uint3 DispatchThreadID : SV_DispatchThreadID) {
    // Sample 8 corners
    float Density[8];
    for (int i = 0; i < 8; i++) {
        uint3 CornerPos = DispatchThreadID + CubeCorner[i];
        Density[i] = InputBuffer[FlatIndex(CornerPos)].Density / 255.0;
    }
    
    // Calculate case index
    uint CaseIndex = CalculateMarchingCubesCase(Density);
    
    if (CaseIndex == 0 || CaseIndex == 255) return;
    
    // Get triangle configuration
    int TriCount = TriangleTable[CaseIndex].Count;
    
    for (int tri = 0; tri < TriCount; tri++) {
        // Interpolate vertices
        for (int vert = 0; vert < 3; vert++) {
            int EdgeIndex = TriangleTable[CaseIndex].Edges[tri * 3 + vert];
            float3 Position = InterpolateEdge(EdgeIndex, Density);
            float3 Normal = CalculateGradient(Position);
            
            EmitVertex(Position, Normal);
        }
    }
}
```

## C++ Integration

### Dispatching Compute Shaders

```cpp
void UVoxelChunkManager::GenerateChunkOnGPU(FIntVector ChunkCoord, int32 LOD) {
    FRDGBuilder GraphBuilder(RHICmdList);
    
    // Create output buffer
    FRDGBufferDesc VoxelBufferDesc = FRDGBufferDesc::CreateStructuredDesc(
        sizeof(FVoxelData),
        ChunkSize * ChunkSize * ChunkSize
    );
    FRDGBufferRef VoxelBuffer = GraphBuilder.CreateBuffer(VoxelBufferDesc, TEXT("VoxelDataBuffer"));
    
    // Dispatch generation shader
    FVoxelGenerationCS::FParameters* Params = GraphBuilder.AllocParameters<FVoxelGenerationCS::FParameters>();
    Params->ChunkCoord = ChunkCoord;
    Params->LODLevel = LOD;
    Params->OutputBuffer = GraphBuilder.CreateUAV(VoxelBuffer);
    
    TShaderMapRef<FVoxelGenerationCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    
    FIntVector GroupCount = FIntVector(
        FMath::DivideAndRoundUp(ChunkSize, 4),
        FMath::DivideAndRoundUp(ChunkSize, 4),
        FMath::DivideAndRoundUp(ChunkSize, 4)
    );
    
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("GenerateVoxelData"),
        ComputeShader,
        Params,
        GroupCount
    );
    
    GraphBuilder.Execute();
}
```

## Performance Characteristics

**Voxel Generation** (32³ chunk):
- Thread groups: 8×8×8 = 512 groups
- Threads per group: 4×4×4 = 64 threads
- Total threads: 32,768 (one per voxel)
- Estimated time: 0.3-0.5ms

**Cubic Meshing** (32³ chunk):
- Thread groups: Variable (based on solid voxels)
- Typical output: 5,000-10,000 vertices
- Estimated time: 0.5-1.0ms

**Smooth Meshing** (32³ chunk):
- Thread groups: 32×32×32 = 32,768 groups
- Typical output: 10,000-20,000 vertices
- Estimated time: 1.0-2.0ms

## Optimization Strategies

1. **Batch multiple chunks** per dispatch
2. **Use async compute** queues
3. **Cache noise samples** for overlapping chunks
4. **Implement early-out** for empty/full chunks
5. **Use GPU occlusion** for visibility

