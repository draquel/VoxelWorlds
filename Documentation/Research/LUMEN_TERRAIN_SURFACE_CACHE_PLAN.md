# Lumen's Surface Cache for the Terrain (G116 plan)

> State as of 2026-11-04. Steps 1-3 done (§3: the chunk primitives, their fades, cached ray tracing; the default since
> 2026-11-04, results in [RENDERING_SYSTEM.md](../Features/RENDERING_SYSTEM.md) "Chunk Primitives"); step 4 done (§4: axis
> cards behind `voxel.Render.LumenCards`, off by default; the terrain's bounce is back, the overhang's off-screen gap is
> not); G116 open for cards fitted to the surface, which turn them on. Terms:
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

**Done 2026-11-04** as written below, with two things the plan did not foresee: the engine reaches a primitive without a
component for its material and shader updates only through `IPrimitiveComponent` (a resolver on the region component
gives one per primitive), and a removed primitive's render resources are released on the render thread, not in its
destructor. The render thread's cost at the uploads (§6's first risk) did not show; numbers and lessons in
RENDERING_SYSTEM.md "Chunk Primitives".

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

**Results (2026-11-04).** Done as written, behind `voxel.Render.LumenCards` (0, the default; 1 = the cards: off until
cards fitted to the surface close the overhang's gap, since the axis cards bring the bounce back but not that), with two
differences: the cards are built when the region component takes the mesh (`FVoxelChunkMesh`'s constructor, on the game
thread), not on the mesh worker, since six boxes from the mesh's box cost nothing measurable and the meshing modules then
need no engine card types; and `voxel.Render.LumenCards.Slices` (1) cuts the box into layers. What a componentless
primitive needs to reach Lumen: `GetMeshCardRepresentation`, `bAffectDynamicIndirectLighting` (the chunk primitives had
it off) and `UpdateVisibleInLumenScene()` after the derived constructor sets it (the base constructor decides the scene
membership before the derived one runs); with the flag the scene caches the static batch's `LumenCardCapture` command,
from which Lumen captures the cards. Its ray tracing representation (step 3) is what makes it traceable for hardware
Lumen.

| `VoxelDemo` | On screen → off screen | Off/on | Cave |
|---|---|---|---|
| cards 0 | 0.174-0.177 → 0.160-0.162 | 0.91-0.92 | 0.111 |
| cards 1 | 0.192-0.193 → 0.175-0.177 | 0.91-0.92 | 0.111 |
| 2 / 3 slices | 0.191 → 0.178 / 0.194 → 0.181 | 0.93 / 0.93 | 0.111 |
| an engine cube slab in the overhang's place, cards 0 / 1 | 0.162 → 0.153 / 0.203 → 0.199 | 0.94 / 0.98 | 0.111 |
| `PlanetTest` cards 0 / 1 | 0.433 → 0.378 / 0.437 → 0.383 | 0.87 / 0.88 | 0.029 |

- **The bounce is back, the gap under the voxel overhang is not.** With the cards the overhang's top and the ground
  around it bounce light again (10 % on the demo, 1 % on the planet, where the site's ground is mostly in the overhang's
  shadow), and the sealed caves are unchanged (no leak). The off/on ratio stays where it was. The engine's own cube in
  the overhang's place reads 0.98, so the gap is the voxel overhang's coverage: Lumen's coverage view
  (`r.Lumen.Visualize 5`) shows its underside without a card where a generator cave's ceiling lies below it in the same
  chunk (the -Z card over the whole box captures the lower surface). Three slices cover the underside but reach only 0.93,
  and take 82 % of the surface cache's pages (52 % with one slice on the demo, 66 % on the planet).
- **The capture budget keeps up.** Streaming at the bench's speed, Lumen captures about 26 card pages a frame against
  8.5 without the terrain's cards (P95 78, at most 235: under its cap of 300, `r.LumenScene.SurfaceCache.CardCapturesPerFrame`),
  mesh cards waiting to be added at most 8, the cache's pages at most 441 (chord) and 825 (great circle) of 1,024, the
  odd frame dropping resolution levels.
- **Cost** (the streaming bench, cards 0 and 1 interleaved, three runs each, noon with the clock stopped, 2,874 x 1,360):
  GPU P50 +0.25 ms on the chord (6.94 → 7.19) and +0.22 ms on the great circle (6.25 → 6.47); the render thread's busy
  time +1.0 and +0.7 ms (9.48 → 10.47, 11.63 → 12.30): the card capture passes' setup and the commands cached per upload;
  game thread +1.6 ms on the chord (19.43 → 21.03) and none on the great circle. Memory: the surface cache's atlas is a
  fixed allocation the cards fill, plus about 1.5 MB on the CPU and 0.6 MB of GPU buffers.
- **Lumen's far field** (D42), measured once with the cards: still no frame changes, as the far field's patches have no
  cards of their own.

**Next (G116):** cards fitted to the surface (the mesh's facing clusters on the mesh worker, so stacked surfaces each get
theirs, bounded by Lumen's 32 cards a mesh), measured against the slices and the render thread's cost at the uploads;
cards for the far field's patches before D42 is asked again.

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
terrain does not have. Cost: +1.29 ms of GPU frame P50 on `VoxelDemo`'s chord (4.91 → 6.20 ms, P95 6.46 → 7.73) and +0.85 ms on `PlanetTest`'s great circle (4.83 → 5.68, P95 6.93 → 7.67), game thread P50 +0.40 and +0.16 ms (the streaming bench, modes 0 and 1 interleaved, three runs each, noon with the clock stopped, the 2,874 x 1,358 PIE viewport). Hit lighting is therefore no interim for G116; the cards are.

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
