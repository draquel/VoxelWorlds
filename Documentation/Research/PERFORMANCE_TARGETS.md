# Performance Targets

Performance budgets and optimization guidelines for VoxelWorlds.

The first section is the measured baseline that changes are judged against today. The sections after it are the
original design targets from 2026-02, written for 32³ chunks of 100 uu voxels and 1,000 chunks; the demo outgrew them
(64³ chunks of 75 uu, a 40,000 uu view, about 1,100-1,300 chunks), so they are kept as history and are not pass/fail.

## Current baseline: the demo (2026-10)

From the InfinitePlane smooth control of the world modes review (WORLD_MODES_REVIEW_PLAN.md §12.2, §12.8, §13.3,
§13.4, §13.11). **How changes are judged:** against a control run of the same build in the same batch, on the same
machine, viewport and route, not against these absolute numbers. Re-mesh counts move with frame rate and machine load
(§13.10), and the control itself moved with G6 (§12.8).

**Setup.** `VoxelDemo` with `DemoSmoothWorldConfig` (Dual Contouring, voxel 75 uu, 64³ chunks = 4,800 uu, view distance
40,000 uu, three LOD bands, GPU generation, CPU meshing), applied through `voxel.ConfigOverride` so every run starts from a
fresh world. PIE in the level viewport (2874×1360, D3D12 SM6), editor in the foreground, background throttle off. Bench:
`voxel.Bench.Run <tag> <v> 40000 start=0:0 dir=1:0 warmup=120`, one cold PIE per run (STREAMING_PERFORMANCE.md).

**Settled at the spawn** (initial load drained, `voxel.FarCompression.Stats`):

| Measure | Value |
|---------|-------|
| Loaded chunks | 1,123 (about 424 with geometry) |
| Time to settle | 13-16 s |
| Voxel data | 134 MB (112 resident, 79 uniform, 932 compressed chunks) |
| Renderer GPU | 23 MB, 0.86-0.98 M triangles |
| Collision / scatter | about 100 / 90 MB |
| Voxel systems in all | about 0.35 GB |

**Streaming** (focused PIE, 40,000 uu traverse; G6 build, `20261013_*_g6_ctrl_*`):

| Speed | Frame P50 / P95 / P99 ms | Manager P95 ms | Catch-up s | Peak mesh queue | LOD re-meshes | Peak voxel MB | Peak systems MB |
|-------|---------------------------|----------------|------------|-----------------|---------------|---------------|-----------------|
| v1500 | 16.7 / 27.4 / 34.5 | 8.6 | 0.17 | 4 | 280 | 828 | 1,039 |
| v3000 | 22.7 / 35.7-35.9 / 42.2-42.5 | 13.5 | 0.9-1.1 | 5-7 | 280 | 777-782 | 987-994 |
| v6000 | 33.3 / 47.5 / 54.7 | 20.0 | 5.0 | 9 | 280 | 764 | 1,053 |

Headless (`-game -nullrhi -VoxelForceCPU`, VoxelWorldsTest, CPU generation and meshing), v3000: frame 3.3 / 8.5 ms,
manager P95 5.8 ms, catch-up 8.5 s, peak voxel 230 MB, systems 566 MB. Headless runs are focus-immune and compare CPU
work; their frame times are not comparable to PIE.

**Where the manager's slowest ticks go** (v3000, mean over the slowest 5% of traverse ticks, §12.8): collision 4.6 ms,
seam pipeline 3.8, render submit 3.0, scatter 2.2, meshing 1.7, far-compression sweep 0.5. The LOD cache rebuild
(`LODUpdateMs`) measured 1.3-1.9 ms a tick on the control's chord and 0.6-0.7 ms standing still before WM-H1 (§13.11).

**Thresholds for another configuration** (a world mode, meshing mode or tier, against the same-build control; §1):

| Metric | Threshold |
|--------|-----------|
| Frame P95 (traverse) | within +10% |
| Chunk-manager P95 | within +15% |
| Catch-up after stopping | within +25% |
| Peak loaded chunks, resident voxel and mesh MB | within +25%, or explained |
| `voxel.Stream.AuditResidency` after settling | 0 findings |
| Fall-throughs (`vox.Debug.Capture`) | none |

**Memory budget (decision D3, 2026-10-14):** 0.6 GB of voxel systems memory (voxel data, renderer, collision, scatter)
settled for a fully resident S-class island (0.8 km across), and at most 1.5 GB at peak while streaming, at any speed and
tier. The control peaks at 1.05-1.27 GB.

**Per chunk** at the demo's size: a 64³ chunk is 262,144 voxels, 1 MB of voxel data resident. Far chunks are compressed
(Oodle) or collapsed to one uniform value, which is how 1,123 chunks fit in 134 MB settled. A rendered chunk's mesh averages
about 55 KB of GPU memory (23 MB over about 424 chunks with geometry).

## Original design targets (2026-02, 32³ chunks)

Everything from here to the end is the original plan, unchanged apart from this note: budgets for 32³ chunks of
100 uu voxels (128 KB of voxel data each) and 1,000 chunks.

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

**Phase 3: LOD Hysteresis** (Implemented)
- Asymmetric deadband around LOD band edges, in chunk-widths, exposed on the World config:
  `LODRefineHysteresis` (default 0.25, eager — detail appears as you approach) and
  `LODCoarsenHysteresis` (default 0.5, damped — terrain you left doesn't flicker)
- Plus a 2:1 adjacency balance (`MaxNeighborLODDelta`); both toggled by cvar `voxel.LODBalance`
- Damps band-edge churn; measured to remove ~25% of raw LOD transitions

**Phase 4: Per-job cost + stale-cull** (Implemented)
- Deep neighbour depth defaults to `stride+1` (geometry-watertight minimum) instead of `2*stride`
- **Stale-cull**: chunks the viewer has driven past are dropped before meshing, not meshed-then-unloaded
- Combined v6000: catch-up -57%, thrash -18%, peak resident -22%

See **[STREAMING_PERFORMANCE.md](STREAMING_PERFORMANCE.md)** for the benchmark harness, the
command-line tuning knobs, and the full lever-by-lever findings.

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
- `bUseGPUScatterExtraction`: retired in world modes review G27 and ignored; scatter extracts surface points from voxels on the CPU

### Scatter Stress Test
- Fly fast through dense scatter terrain (Grass 50%, Rocks 5%, Trees 2%)
- Target: No frame drops below 55 FPS
- Target: No missing scatter or visual artifacts
- Target: No crashes from rapid unload/reload cycles
- Target: Scatter density consistent across LOD transitions (7D-5)

