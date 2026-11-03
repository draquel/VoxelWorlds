# Lumen's Surface Cache for the Terrain (G116 plan)

> State as of 2026-11-03. Open: planned, not started (steps 1-4 below). Terms:
> `Documentation/GLOSSARY.md`. The finding is G116 in the project's `Documentation/FINDINGS.md`; the spike that chose the
> path is [LUMEN_OFFSCREEN_TERRAIN_SPIKE.md](LUMEN_OFFSCREEN_TERRAIN_SPIKE.md) (frozen); what ships is described in
> [RENDERING_SYSTEM.md](../Features/RENDERING_SYSTEM.md) "What Lumen Sees".

**The gap.** Since F3.3 the terrain is in the ray tracing scene, so Lumen's rays stop on it off screen, but a ray that
hits it finds no surface cache (the terrain has no mesh cards) and returns black: the occlusion is right and the bounce
off the terrain is lost. Lumen captures cards from a primitive's cached static mesh draw commands, so G116's fix was
written as two steps: the proxy on the static draw path, then a card representation per chunk.

**What this plan found.** The static draw path does not fit the proxy as it is: one `FVoxelSceneProxy` per render region
draws hundreds of chunk and seam meshes, and every part of the engine's static path works per primitive (§1). The static
path, the cached ray tracing instances and the cards all want **a primitive per chunk mesh** (§3). The two changes the
finding listed as the first step's easy part change nothing on their own (§2), so nothing of the first step ships
without the per-chunk primitives.

## 1. Why not static meshes in the region proxy

Read in the 5.8 renderer:

| Engine path | Works per | What a region primitive with N chunk meshes would get |
|---|---|---|
| Static mesh visibility (`SceneVisibility.cpp`, the relevance loop over `StaticMeshRelevances`) | primitive: its bounds, then a LOD mask per mesh | every chunk and seam mesh of a visible region drawn in every pass, with no frustum test per chunk; today the proxy culls each mesh per view and per shadow cascade |
| GPU instance culling of cached commands (`InstanceCullingContext.cpp`, `bFetchInstanceCountFromScene`) | instance: a cached command draws all of its primitive's instances | no way to give each chunk's command its own bounds inside one primitive (instance runs are for dynamic commands and Lumen's card capture) |
| Far shadow cascades (D-VP3) | primitive (`bCastFarShadow`) | the proxy's per-LOD filter (`voxel.Render.FarShadowMinLOD`) has no static equivalent: every fine chunk back in the far cascades |
| Cached ray tracing instances (`CacheInstances`) | primitive: a dirty primitive re-caches all its instances (`CacheRayTracingPrimitives`, one task per primitive) | every BLAS build completing dirties its geometry group (`RequestUpdateCachedRenderState`), so while streaming the region re-caches its 1,000 instances every frame on one task; a cached geometry the residency budget evicts trips the gather's check |
| Lumen card capture (`LumenSceneCardCapture.cpp`) | primitive: a card page draws every static mesh of the primitive at the chosen LOD | every card of every chunk would draw the whole region |
| Card representation (`GetMeshCardRepresentation`) | primitive: one `FCardRepresentationData` | one card set for a region of 16³ chunks |

## 2. What the region proxy can take now: nothing that changes the frame

Tried on 2026-11-03 (a prototype, VoxelWorlds branch `spike/g116-gpuscene-position-srv-wip`, not pushed) and read in the
engine:

- **The position SRV.** The finding said hit lighting needs the vertex factory's position SRV, which the proxy binds as
  the engine's null buffer. It does not: in 5.8 a hit group shader takes the hit triangle's positions from the ray
  tracing geometry's own vertex buffer (`LocalVertexFactory.ush`, `LoadVertexFactoryInputForHGS`, which loads
  the triangle's base attributes from the geometry); `VertexFetch_PositionBuffer` is read only by the compute pass that rebuilds dynamic
  ray tracing geometry (world position offset) and by spline meshes, neither of which the terrain has. Hit lighting
  (`r.Lumen.HardwareRayTracing.LightingMode 1`) works on main as it is (§5); a position buffer would cost 12 bytes a vertex
  for nothing.
- **GPUScene support** (`EnableGPUSceneSupportFlags`). The primitive's data is in GPUScene whatever the flag (the hit
  shaders read it there today); for a proxy that draws only dynamic batches the flag stops the primitive uniform buffer's
  updates (a region never moves) and changes the shadow selection: with virtual shadow maps on, a GPUScene primitive's
  dynamic batches go to the virtual shadow maps only (the renderer's shadow setup). It is a prerequisite of the static path and the
  cached instances, not a step on its own; the chunk primitives (§3) set it on their own proxy type.

## 3. Steps 1-3: a primitive per chunk mesh

**Approach.** Each chunk interior's current mesh and each seam bucket becomes its own primitive, registered without a
component through `FPrimitiveSceneDesc` (`FSceneInterface::BatchAddPrimitives` / `BatchRemovePrimitives`, the path
the engine's FastGeoStreaming plugin uses), so no `UPrimitiveComponent` is created per chunk (the spike measured
0.14 ms of game thread per registered component). The region component stays as the owner: it keeps the water tiles,
the crossfade driver and the recovery of lost render data (G15), and holds the chunk primitives' descriptions.

| Step | Work | Size |
|---|---|---|
| 1. Chunk primitives, static draw | `FVoxelChunkSceneProxy` (`EnableGPUSceneSupportFlags`, `DrawStaticElements`: the terrain batch and its RVT clones), one per chunk interior and seam bucket, located at its chunk's origin (float offsets a chunk across, finer than the region's); `bCastFarShadow` from the mesh's LOD (the far cascades' filter becomes per primitive); added, replaced and removed in batches from the renderer's flush on the game thread; the region proxy keeps the water tiles. Behind a cvar until the gates pass, then the default | L |
| 2. Fades | A fading chunk's primitive answers dynamic relevance for the fade's length (`GetViewRelevance`: static off, dynamic on) and draws the two dithered halves in `GetDynamicMeshElements` as today, so no cached command changes when a fade starts or ends; held and appearing fades the same way. The seam buckets' fades likewise | M |
| 3. Cached ray tracing | Each chunk primitive caches its one instance (`CacheInstances`, a geometry group of one LOD per primitive); a build or release dirties that primitive only. Retires the per-frame gather and `voxel.Render.RayTracing.TraceDistanceScale`, so every chunk is in the ray tracing scene again | S-M |

**What it changes for the frame.** Per-primitive frustum and occlusion culling replace the proxy's per-mesh loop (and
its 2,000-batch cap); the scene's cached mesh draw commands replace building every batch every frame, which is the
render thread's cost today. The cost moves to the uploads: a chunk upload adds a primitive and caches its commands for
each pass (depth, base pass, shadow depth, RVT, Lumen card capture), about 1,100 meshes in the demo's window and
19,000 uploads in a bench run.

## 4. Step 4: cards

Each chunk primitive returns an `FCardRepresentationData` (`GetMeshCardRepresentation`): six axis-aligned cards over the
mesh's bounds (`FLumenCardBuildData` per face direction), built on the mesh worker beside the mesh (the box is known
there; nothing reads a neighbour), released with the mesh. Lumen then captures each chunk's cards from its cached
commands, and terrain hits read the surface cache instead of black. Fitting cards to the surface's facing (a cave's
walls, an overhang's underside) is a refinement once the axis cards are measured.

| | |
|---|---|
| Size | M on top of steps 1-3 |
| Measure | the off/on screen luminance under the overhang (F3.3's repro: 0.90 today) and the sealed cave; Lumen's card captures per frame (its surface cache capture budget) against the uploads; surface cache memory |

## 5. Hit lighting today

F3.3's repro (the overhang slab and sealed cave of D-VP4's spike, noon with the clock stopped, exposure and
anti-aliasing off; mean luminance of the inner 60 % of the compared regions), on main's build and on the prototype's
(identical to within 0.3 of 255 per pixel, so the position SRV changes nothing):

| | Under the overhang, on screen → off screen | Off / on | Sealed cave |
|---|---|---|---|
| `VoxelDemo`, lighting mode 0 (surface cache) | 0.184 → 0.159 | 0.86 (F3.3: 0.90; the on-screen frame moves run to run) | 0.111 |
| `VoxelDemo`, lighting mode 1 (hit lighting) | 0.192 → 0.171 | 0.89 | 0.111 |
| `PlanetTest` (30N 20E), mode 0 | 0.430 → 0.376 | 0.87 | 0.029 |
| `PlanetTest`, mode 1 | 0.436 → 0.383 | 0.88 | 0.029 |

In Lumen's scene view the overhang's sunlit side turns from black to lit under hit lighting, and terrain in shadow stays
black: a hit evaluates the material and the direct light, and reads its indirect light from the surface cache, which the
terrain does not have. Cost: HITCOST. Hit lighting is therefore no interim for G116; the cards are.

## 6. Risks

- **Primitive count.** About 1,100 chunk and seam primitives in the demo's window and churn of thousands a minute while
  streaming: the scene's add and remove are batched per frame, but each add caches commands for every pass; measure
  the render thread at the uploads (the bench's chord) before making it the default.
- **Card capture budget.** Every re-mesh is a new primitive whose cards Lumen captures again; streaming at speed may
  keep the surface cache behind the window, and distant chunks' cards are coarse.
- **Fades.** A fading chunk switching relevance must not drop a frame between its static and dynamic draws.
- **Lost render data (G15).** The chunk primitives belong to the renderer, not to the region proxy: a recreated region
  proxy no longer loses them, and `RecoverLostRenderData` needs to re-send only the water tiles.
- **Tests.** The crossfade and ray tracing proxy tests read `FVoxelSceneProxy` (`GetChunkDrawInfo_RenderThread`); they
  move to the chunk primitives.
