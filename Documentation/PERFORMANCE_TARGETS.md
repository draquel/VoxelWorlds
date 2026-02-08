# Performance Targets

Performance budgets and optimization guidelines for VoxelWorlds.

## Target Specifications

### Hardware Baseline
- **GPU**: GTX 1060 / RX 580 equivalent (6GB VRAM)
- **CPU**: Intel i5-8400 / Ryzen 5 2600
- **RAM**: 16GB
- **Target Resolution**: 1920×1080

### Frame Rate Target
**60 FPS minimum** (16.6ms frame budget)

---

## Memory Budgets

### Per-Chunk Memory (LOD 0, 32³)

| Component | Size | Notes |
|-----------|------|-------|
| Voxel Data | 128 KB | 32,768 voxels × 4 bytes |
| Vertex Buffer | ~50 KB | Cubic meshing, average |
| Index Buffer | ~30 KB | Typical quad count |
| **Total** | **~210 KB** | Per chunk baseline |

### Total Memory Targets

| Scenario | Chunks | Memory | Status |
|----------|--------|--------|--------|
| Minimum | 100 | 21 MB | Phase 2 target |
| Target | 1000 | 210 MB | Phase 6 target |
| Maximum | 2000 | 420 MB | Stretch goal |

### Memory Breakdown (1000 chunks)

```
Voxel Data:     128 MB  (128 KB × 1000)
Vertex Buffers:  50 MB  (50 KB × 1000)
Index Buffers:   30 MB  (30 KB × 1000)
System/Other:    20 MB  (Metadata, managers, etc.)
────────────────────────
Total:          228 MB
```

**Budget Remaining**: 22 MB for textures, materials, scatter

---

## Compute Time Budgets (Per Frame)

### GPU Compute Budget: 5ms

| Operation | Budget | Target | Notes |
|-----------|--------|--------|-------|
| Chunk Generation | 2.0 ms | 4 chunks | 0.5ms per chunk |
| Meshing | 2.0 ms | 4 chunks | 0.5ms per chunk |
| LOD Updates | 0.5 ms | 1000 chunks | Vertex shader morphing |
| Collision | 0.5 ms | Async | Lower priority |
| **Total** | **5.0 ms** | | 30% of frame |

### CPU Time Budget: 2ms

| Operation | Budget | Notes |
|-----------|--------|-------|
| LOD Queries | 0.1 ms | Cached - only on chunk boundary crossing |
| Queue Management | 0.3 ms | O(1) dedup, O(log n) insert |
| State Updates | 0.5 ms | Chunk states, transitions (throttled) |
| Processing | 0.6 ms | Generation/meshing dispatch |
| Other | 0.5 ms | Buffer |
| **Total** | **2.0 ms** | 12% of frame |

**Note**: LOD queries reduced from 0.5ms to 0.1ms via Phase 2 caching. Queue management reduced via Phase 1 O(1) operations.

---

## Streaming Throughput Targets

### Load/Unload Rates

**Per Frame @ 60 FPS**:
- **Load**: 4 chunks/frame = 240 chunks/sec
- **Unload**: 8 chunks/frame = 480 chunks/sec

### Player Movement Response

At player speed 1000 cm/s (10 m/s):
- Crosses ~0.3 chunks/second (32m chunks)
- Load rate: 240 chunks/sec
- **Safety margin**: 800× faster than needed

### Streaming Latency

| Operation | Frames | Time @ 60 FPS |
|-----------|--------|---------------|
| Queue → Generation | 1 | 16ms |
| Generation complete | 1 | 16ms |
| Queue → Meshing | 1 | 16ms |
| Meshing complete | 1 | 16ms |
| **Total (cold start)** | **4** | **~67ms** |

**Hot path** (chunk in cache): 2 frames (~33ms)

---

## Phase-Specific Targets

### Phase 2: Generation & Basic Rendering

**Memory**:
- 100 visible chunks
- < 100 MB total
- ✅ Goal: Prove pipeline works

**Performance**:
- 60 FPS with 100 chunks
- < 16ms frame time
- Cubic meshing only
- PMC renderer acceptable

**Success Criteria**:
- ✅ No visible gaps
- ✅ Smooth camera movement
- ✅ Materials display correctly

### Phase 3: Advanced Meshing

**Memory**:
- 500 visible chunks
- < 150 MB total
- ✅ Goal: Optimize rendering

**Performance**:
- 60 FPS with 500 chunks
- Greedy meshing reduces triangles 40%
- Custom VF faster than PMC
- LOD transitions smooth

**Success Criteria**:
- ✅ 1000 chunks at 60 FPS
- ✅ < 5ms GPU compute per frame
- ✅ No LOD popping

### Phase 6: Final Polish

**Memory**:
- 1000+ visible chunks
- < 250 MB total
- ✅ Goal: Production-ready

**Performance**:
- 60 FPS sustained
- 4 chunks gen/meshed per frame
- < 5ms total GPU compute
- < 2ms CPU overhead

**Success Criteria**:
- ✅ All targets met
- ✅ Profiled and optimized
- ✅ Memory within budget
- ✅ Stable under stress

---

## GPU Performance Breakdown

### Compute Shader Timings (32³ chunk)

| Shader | Threads | Time | Notes |
|--------|---------|------|-------|
| GenerateVoxelData | 32,768 | 0.3-0.5ms | Noise sampling |
| CubicMesh | Variable | 0.5-1.0ms | Face culling |
| SmoothMesh | 32,768 | 1.0-2.0ms | Marching Cubes |
| GreedyMesh | Variable | 0.3-0.7ms | Quad merging |
| AO Calculation | Variable | 0.1-0.3ms | 3×3×3 sampling |

### Rendering Timings

| Operation | Time | Notes |
|-----------|------|-------|
| Vertex Transform | ~1ms | For 1000 chunks |
| Rasterization | ~3ms | Depends on res/settings |
| Material Shading | ~2ms | Simple atlas material |
| Post-processing | ~2ms | Engine default |

**Total Rendering**: ~8ms (48% of frame)

---

## Optimization Strategies

### Memory Optimization

1. **Chunk Pooling**: Reuse allocations
2. **Sparse Storage**: Don't store air-only chunks
3. **LOD Decimation**: Coarser LODs use fewer voxels
4. **Lazy Generation**: Generate only when visible
5. **Atlas Compression**: Compress material textures

### Compute Optimization

1. **Batch Dispatch**: Multiple chunks per shader call
2. **Async Compute**: Overlap with rendering
3. **Early Exit**: Skip empty/full chunks
4. **Cache Noise**: Reuse overlapping samples
5. **GPU Occlusion**: Don't generate invisible chunks

### CPU Optimization

1. **Spatial Hashing**: Fast chunk lookups
2. **Cache LOD Queries**: Don't recompute every frame
3. **Parallel Queue Processing**: Multi-thread where safe
4. **Temporal Coherence**: Skip updates if camera static
5. **Chunk Pooling**: Avoid allocations

### Streaming Optimizations (Implemented)

**Phase 1: Queue Management**
- O(1) duplicate detection with TSet tracking
- Sorted insertion with `Algo::LowerBound()` (O(log n) vs O(n²))
- Queue growth capped at 2× processing rate per frame

**Phase 2: Decision Caching**
- Streaming decisions cached until viewer crosses chunk boundary
- LOD updates skip when viewer moved < 100 units
- Reduces LOD queries from 60,000/sec to ~190/sec (99.7% reduction)

**Phase 3: LOD Hysteresis** (Future, if needed)
- Buffer zones around LOD boundaries (~50-100 units)
- Prevents rapid back-and-forth remeshing at band edges
- Asymmetric thresholds for LOD upgrades vs downgrades

---

## Profiling Guidelines

### What to Profile

**Phase 2**:
- Chunk generation time
- Meshing time
- Memory allocation patterns
- Frame time breakdown

**Phase 3**:
- Greedy meshing gains
- Custom VF vs PMC performance
- LOD transition cost
- GPU memory bandwidth

**Phase 6**:
- Full system under load
- Worst-case scenarios
- Memory fragmentation
- Streaming latency spikes

### Profiling Tools

**Unreal Insights**:
- GPU timing (compute shaders)
- CPU timing (game thread)
- Memory allocations
- Asset loading

**RenderDoc**:
- GPU buffer inspection
- Shader performance
- Overdraw analysis
- Texture usage

**Visual Studio Profiler**:
- CPU hotspots
- Memory leaks
- Call graph analysis

---

## Performance Monitoring

### Runtime Stats

Display in development builds:
```
Chunks Visible: 847 / 1000
Chunks Loading: 3
FPS: 62.1
Frame Time: 16.1ms
  - CPU: 1.8ms
  - GPU Compute: 4.2ms
  - GPU Render: 8.9ms
Memory: 198 MB / 250 MB
```

### Warning Thresholds

Trigger warnings when:
- FPS < 55 for 3 consecutive seconds
- Frame time > 20ms
- Memory > 230 MB (92% of budget)
- Streaming queue > 20 chunks
- GPU compute > 6ms

---

## Stress Testing

### Test Scenarios

**Camera Movement**:
- Fly at max speed for 5 minutes
- Rapid 360° spins
- Sudden height changes

**Chunk Thrashing**:
- Teleport player rapidly
- Force max load/unload per frame
- Jump between distant locations

**Memory Pressure**:
- Load maximum chunks
- Enable all features simultaneously
- Sustained play for 30 minutes

### Acceptance Criteria

All stress tests must:
- ✅ Maintain 55+ FPS
- ✅ No crashes or freezes
- ✅ No memory leaks
- ✅ No visual artifacts
- ✅ Recover gracefully from spikes

---

## Platform Considerations

### PC (Primary Target)
- RTX 2060 / RX 5700 XT
- Settings: High
- 1920×1080 @ 60 FPS

### Console (Future)
- PS5 / Xbox Series X
- May need lower chunk count
- 4K @ 30 FPS or 1440p @ 60 FPS

### Mobile (Stretch Goal)
- High-end tablets
- Significantly reduced targets
- 720p @ 30 FPS with 100 chunks

---

## Scatter System Performance (Phase 7D)

### Compute Time Budget

| System | Before 7D | After 7D-1 (Async) | After 7D-2 (GPU) |
|--------|-----------|---------------------|-------------------|
| Scatter (game thread) | ~0.5-1.0ms | ~0.1ms | ~0.1ms |
| Scatter (thread pool) | 0ms | ~0.5-1.0ms | ~0.3ms (placement only) |
| Scatter (GPU) | 0ms | 0ms | ~0.1-0.2ms (extraction) |

### Surface Extraction Methods

| Method | Path | LOD-Independent | Notes |
|--------|------|-----------------|-------|
| Voxel-based (7D-5) | CPU (default) | Yes | Scans 32³ voxel columns, identical output at any LOD |
| GPU compute (7D-2) | GPU (optional) | No | Mesh vertex extraction, varies with LOD |
| Mesh vertex (legacy) | CPU | No | Original spatial hash extraction |

The voxel-based path (7D-5) is the default because it produces consistent scatter regardless of LOD level, preventing density changes and position shifts when chunks change LOD.

### Configuration Defaults
- `MaxAsyncScatterTasks = 2`: Concurrent thread pool tasks
- `MaxScatterGenerationsPerFrame = 2`: Queue drain rate
- `bUseGPUScatterExtraction = false`: GPU path off by default (enable for SM5+ hardware)

### Scatter Stress Test
- Fly fast through dense scatter terrain (Grass 50%, Rocks 5%, Trees 2%)
- Target: No frame drops below 55 FPS
- Target: No missing scatter or visual artifacts
- Target: No crashes from rapid unload/reload cycles
- Target: Scatter density consistent across LOD transitions (7D-5)

