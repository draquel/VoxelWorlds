# Testing Strategy

Comprehensive testing approach for VoxelWorlds plugin.

## Testing Layers

### 1. Unit Tests
Test individual components in isolation.

### 2. Integration Tests
Test interaction between modules.

### 3. Performance Tests
Validate performance targets.

### 4. Visual Tests
Verify rendering quality.

---

## Unit Tests

### VoxelCore Module

**Test: FVoxelData Packing**
```cpp
TEST(VoxelCore, VoxelDataSize) {
    EXPECT_EQ(sizeof(FVoxelData), 4);
}

TEST(VoxelCore, VoxelDataPacking) {
    FVoxelData Data;
    Data.MaterialID = 42;
    Data.Density = 127;
    Data.BiomeID = 3;
    Data.Metadata = 0xFF;
    
    EXPECT_EQ(Data.MaterialID, 42);
    EXPECT_EQ(Data.Density, 127);
    EXPECT_EQ(Data.BiomeID, 3);
    EXPECT_EQ(Data.Metadata, 0xFF);
}
```

**Test: FChunkDescriptor**
```cpp
TEST(VoxelCore, ChunkDescriptorMemory) {
    FChunkDescriptor Chunk;
    Chunk.ChunkSize = 32;
    Chunk.VoxelData.SetNum(32 * 32 * 32);
    
    size_t ExpectedSize = 32 * 32 * 32 * sizeof(FVoxelData);
    EXPECT_EQ(Chunk.VoxelData.Num() * sizeof(FVoxelData), ExpectedSize);
}
```

### VoxelLOD Module

**Test: Distance Band LOD**
```cpp
TEST(VoxelLOD, DistanceBandBasic) {
    FDistanceBandLODStrategy Strategy;
    
    // Configure bands
    Strategy.LODBands = {
        { 0.0f, 512.0f, 0, 1, 32, 64.0f },
        { 512.0f, 1024.0f, 1, 2, 32, 64.0f }
    };
    Strategy.Initialize(Config);
    
    // Test LOD at origin
    FLODQueryContext Context;
    Context.ViewerPosition = FVector::ZeroVector;
    
    int32 LOD0 = Strategy.GetLODForChunk(FIntVector(0,0,0), Context);
    EXPECT_EQ(LOD0, 0);
    
    // Test LOD at distance
    int32 LOD1 = Strategy.GetLODForChunk(FIntVector(20,0,0), Context);
    EXPECT_EQ(LOD1, 1);
}

TEST(VoxelLOD, LODMorphFactor) {
    FDistanceBandLODStrategy Strategy;
    // ... configure
    
    FLODQueryContext Context;
    Context.ViewerPosition = FVector::ZeroVector;
    
    // Chunk at transition zone
    FIntVector ChunkInTransition(15, 0, 0);
    float MorphFactor = Strategy.GetLODMorphFactor(ChunkInTransition, Context);
    
    EXPECT_GE(MorphFactor, 0.0f);
    EXPECT_LE(MorphFactor, 1.0f);
}
```

### VoxelRendering Module

**Test: Vertex Packing**
```cpp
TEST(VoxelRendering, VertexSize) {
    EXPECT_EQ(sizeof(FVoxelVertex), 28);
}

TEST(VoxelRendering, NormalPacking) {
    FVector3f Normal(0.707f, 0.707f, 0.0f);
    uint8 AO = 128;
    
    uint32 Packed = FVoxelVertex::PackNormalAndAO(Normal, AO);
    
    FVector3f UnpackedNormal;
    uint8 UnpackedAO;
    FVoxelVertex::UnpackNormalAndAO(Packed, UnpackedNormal, UnpackedAO);
    
    EXPECT_NEAR(UnpackedNormal.X, Normal.X, 0.01f);
    EXPECT_NEAR(UnpackedNormal.Y, Normal.Y, 0.01f);
    EXPECT_NEAR(UnpackedNormal.Z, Normal.Z, 0.01f);
}
```

---

## Integration Tests

### LOD + ChunkManager

**Test: Chunk Loading**
```cpp
TEST(Integration, ChunkLoading) {
    UVoxelChunkManager* Manager = NewObject<UVoxelChunkManager>();
    Manager->Initialize(Config);
    
    // Request chunk
    FIntVector ChunkCoord(0, 0, 0);
    Manager->RequestChunkGeneration(ChunkCoord, 0);
    
    // Process queue
    Manager->ProcessGenerationQueue(10.0f);
    
    // Verify loaded
    EXPECT_TRUE(Manager->IsChunkLoaded(ChunkCoord));
}
```

**Test: LOD Transitions**
```cpp
TEST(Integration, LODTransitions) {
    UVoxelChunkManager* Manager = NewObject<UVoxelChunkManager>();
    // ... setup
    
    // Move camera
    FLODQueryContext Context;
    Context.ViewerPosition = FVector(1000, 0, 0);
    
    Manager->Update(0.016f);
    
    // Verify chunks transitioned
    // Check morph factors updated
}
```

### Generation + Meshing

**Test: Full Pipeline**
```cpp
TEST(Integration, GenerationToMesh) {
    // Generate voxel data
    FChunkDescriptor Chunk;
    GenerateChunkData(Chunk);
    
    EXPECT_GT(Chunk.VoxelData.Num(), 0);
    
    // Mesh the data
    FChunkRenderData RenderData;
    MeshChunk(Chunk, RenderData);
    
    EXPECT_GT(RenderData.VertexCount, 0);
    EXPECT_GT(RenderData.IndexCount, 0);
}
```

---

## Performance Tests

### Memory Tests

**Test: Chunk Memory Budget**
```cpp
TEST(Performance, ChunkMemoryBudget) {
    FChunkDescriptor Chunk;
    Chunk.ChunkSize = 32;
    Chunk.VoxelData.SetNum(32 * 32 * 32);
    
    size_t VoxelMemory = Chunk.VoxelData.Num() * sizeof(FVoxelData);
    
    // Should be ~128 KB
    EXPECT_LT(VoxelMemory, 150 * 1024);
}

TEST(Performance, ThousandChunksMemory) {
    TArray<FChunkDescriptor> Chunks;
    Chunks.SetNum(1000);
    
    for (auto& Chunk : Chunks) {
        Chunk.ChunkSize = 32;
        Chunk.VoxelData.SetNum(32 * 32 * 32);
    }
    
    size_t TotalMemory = Chunks.Num() * 32 * 32 * 32 * sizeof(FVoxelData);
    
    // Should be ~200 MB
    EXPECT_LT(TotalMemory, 250 * 1024 * 1024);
}
```

### Timing Tests

**Test: LOD Query Performance**
```cpp
TEST(Performance, LODQuerySpeed) {
    FDistanceBandLODStrategy Strategy;
    // ... configure
    
    FLODQueryContext Context;
    Context.ViewerPosition = FVector::ZeroVector;
    
    // Time 1000 queries
    double StartTime = FPlatformTime::Seconds();
    
    for (int32 i = 0; i < 1000; ++i) {
        FIntVector ChunkCoord(i % 10, i / 10, 0);
        Strategy.GetLODForChunk(ChunkCoord, Context);
    }
    
    double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
    
    // Should be < 1ms for 1000 queries (< 1μs per query)
    EXPECT_LT(ElapsedMs, 1.0);
}
```

**Test: Streaming Throughput**
```cpp
TEST(Performance, StreamingThroughput) {
    UVoxelChunkManager* Manager = NewObject<UVoxelChunkManager>();
    // ... setup
    
    // Request 100 chunks
    for (int32 i = 0; i < 100; ++i) {
        Manager->RequestChunkGeneration(FIntVector(i, 0, 0), 0);
    }
    
    // Process with time budget (2ms per frame)
    double StartTime = FPlatformTime::Seconds();
    
    while (Manager->GetGenerationQueueSize() > 0) {
        Manager->ProcessGenerationQueue(2.0f);
    }
    
    double ElapsedSeconds = FPlatformTime::Seconds() - StartTime;
    
    // Should process at 4 chunks/frame @ 60 FPS = 240 chunks/sec
    // 100 chunks should take < 0.5 seconds
    EXPECT_LT(ElapsedSeconds, 0.5);
}
```

---

## Visual Tests

### Rendering Quality

**Manual Test Checklist**:
- [ ] No gaps between chunks
- [ ] LOD transitions are smooth
- [ ] Materials display correctly
- [ ] Ambient occlusion looks correct
- [ ] No z-fighting or flickering
- [ ] Collision matches visual mesh

### Screenshot Comparison

Store reference screenshots:
```
Tests/ReferenceImages/
├── CubicMesh_Basic.png
├── SmoothMesh_Basic.png
├── LODTransition_0to1.png
└── BiomeBlending.png
```

Compare during testing:
```cpp
// Pseudo-code
FString ScreenshotPath = CaptureScreenshot();
float Similarity = CompareImages(ScreenshotPath, ReferenceImagePath);
EXPECT_GT(Similarity, 0.95f); // 95% match
```

---

## Automated Testing

### Continuous Integration

**Build Pipeline**:
1. Compile all modules
2. Run unit tests
3. Run integration tests
4. Run performance tests
5. Generate coverage report

**Example GitHub Actions**:
```yaml
name: VoxelWorlds Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v2
      - name: Build
        run: UE5Editor-Cmd.exe VoxelEngine.uproject -run=Build
      - name: Test
        run: UE5Editor-Cmd.exe VoxelEngine.uproject -ExecCmds="Automation RunTests VoxelWorlds" -unattended
```

---

## Test Organization

### Directory Structure
```
Plugins/VoxelWorlds/
├── Source/
│   ├── VoxelCore/
│   │   └── Tests/
│   │       └── VoxelCoreTests.cpp
│   ├── VoxelLOD/
│   │   └── Tests/
│   │       └── LODStrategyTests.cpp
│   └── VoxelRendering/
│       └── Tests/
│           └── VertexFactoryTests.cpp
└── Tests/
    ├── Integration/
    ├── Performance/
    └── Visual/
```

### Running Tests

**In Editor**:
```
Window > Test Automation
Filter: "VoxelWorlds"
Run Tests
```

**Command Line**:
```bash
UE5Editor-Cmd.exe VoxelEngine.uproject \
  -ExecCmds="Automation RunTests VoxelWorlds" \
  -unattended \
  -nopause \
  -testexit="Automation Test Queue Empty"
```

---

## Test Coverage Goals

### Phase 1 (Foundation)
- Unit test coverage: 80%+
- All core data structures tested
- LOD interface tested
- Integration tests for basic flow

### Phase 2 (Generation)
- GPU compute shader validation
- Noise generation tests
- Biome selection tests

### Phase 3 (Advanced Meshing)
- Greedy meshing correctness
- Ambient occlusion validation
- Custom VF performance benchmarks

### Final (All Phases)
- Overall coverage: 70%+
- All performance targets validated
- Full integration test suite
- Visual regression tests

---

## Debugging Failed Tests

### Common Issues

**Test: Chunk loading fails**
- Check module dependencies in .Build.cs
- Verify chunk manager initialization
- Check LOD strategy configuration

**Test: Performance test fails**
- Run on consistent hardware
- Disable background tasks
- Check for debug builds (use shipping)
- Profile with Unreal Insights

**Test: Visual test fails**
- Verify graphics settings consistent
- Check for platform-specific rendering
- Ensure deterministic generation (fixed seeds)

---

## Next Steps

1. Set up test framework in Phase 1
2. Write tests as you implement features
3. Run tests before committing
4. Add performance tests in Phase 3
5. Visual tests in Phase 4+

