# GPU Compute Pipeline

**Modules**: VoxelGeneration
**Dependencies**: VoxelCore

## Overview

The GPU compute pipeline generates each chunk's voxel data (the density, biome and material fields, then the water
and underground post-passes) in one render graph and reads the finished voxels back. Meshing is not on the GPU: the
chunk meshers run on CPU workers (`MARCHING_CUBES_MESHING.md`, `DUAL_CONTOURING.md`); the GPU meshers that once
followed generation here were never dispatched by a world and were deleted in code review CR1.

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
    
    // Apply world mode SDF: the InfinitePlane / IslandBowl heightmap, or the SphericalPlanet radial height
    // (SphericalPlanet.ush, spherical Phase 2; WORLD_MODES.md "GPU Implementation")
    Density = ApplyHeightmapWorldMode(WorldPos, Density);
    
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

### Stage 1b: Post-passes

The water fill and the underground classification run as compute passes in the same render graph
(`VoxelPostPasses.usf`, dispatched by `AddVoxelPostPassDispatches`), so the readback returns finished voxel data and
no thread does per-voxel work after it. Heightmap modes scan world-Z columns; a planet scans digital lines along each
chunk's local down (`WaterLineSeedCS`, `UndergroundLinesCS`, one thread per line, on the lines `FVoxelDownLines`
computes for the CPU passes) and floods inside the sea sphere (`WaterFloodSweepRadialCS`). The `GPUvsCPU*Parity`
tests compare the finished data with the CPU's.

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

## Optimization Strategies

1. **Batch multiple chunks** per dispatch
2. **Use async compute** queues
3. **Cache noise samples** for overlapping chunks
4. **Implement early-out** for empty/full chunks

