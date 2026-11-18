# Runtime Nanite for the Voxel Terrain (research and fit assessment)

> Research note, 2026-11-16. Nothing is built. The question: can VoxelWorlds draw its runtime-generated chunk meshes
> through Nanite, as the commercial Voxel Plugin 2 does, and what would that buy (frame cost, shadows, tessellation and
> displacement)? Sources: the UE 5.8.4 source under `D:\Program Files\Epic Games\UE_5.8\Engine`, Voxel Plugin's
> open-source core (github.com/VoxelPlugin/VoxelCore, MIT, last pushed 2026-08-23), the plugin's documentation, and
> this repository's rendering docs and bench records. No measurement was taken for this note; the numbers quoted are
> the existing records.
>
> **Outcome (2026-11-18):** the spike this note proposes ran (the parent's
> `Documentation/RUNTIME_NANITE_SPIKE_PLAN.md`, frozen, its results in section 6; the prototype on the branch
> spike/runtime-nanite, never merged) and was dropped under D156: the terrain on Nanite costs about 1 ms of GPU and
> 1.5 ms of render thread on a world with no other Nanite content, Nanite's fixed cost, and neither the render
> thread, VSM, Nanite's own ray tracing nor tessellation shows a win. The note stands as the reasoning; sections 5
> and 8 are what the spike measured against.

## 1. The answer in short

- **The premise behind "No Nanite" is half wrong.** [ARCHITECTURE.md](../ARCHITECTURE.md) says Nanite geometry is
  built at cook time, so runtime terrain cannot be Nanite. True of Epic's *builder*: the `NaniteBuilder` module is a
  Developer module, absent from Shipping and Test targets, and every engine caller loads it on editor targets only.
  Not true of the *renderer*: it draws any `Nanite::FSceneProxyBase` subclass over any `Nanite::FResources` you hand
  it, and a resource whose pages are all root pages never streams from disk. Voxel Plugin 2 does exactly this with its
  own encoder, and its encoder is open source.
- **What runtime Nanite is, in practice, is Nanite as a rasterizer, not Nanite's LOD.** Voxel Plugin's encoder cuts the
  triangle list into 128-triangle clusters in index order, makes every page a root page, builds a flat 4-wide hierarchy
  and marks every cluster as always drawn. There is no simplification DAG. Chunk LOD, seams and crossfades stay ours.
- **The real wins are three:** virtual shadow maps become usable for the terrain (they are off today because of
  non-Nanite cost), the per-mesh draw and shadow-batch cost on the render thread goes to Nanite's GPU-driven path, and
  Nanite tessellation with material displacement becomes available (on by default in 5.8).
- **The real costs are three:** an encoder for a private, unversioned GPU page format that changes every engine
  release; the terrain's master material is Masked, which puts every terrain pixel on Nanite's slow programmable path
  until it is made opaque; and ray tracing, Lumen cards and the non-Nanite fallback must all be carried by our own proxy.
- **Fit: viable, medium-high cost, worth a time-boxed spike** that measures the frame and the shadows before any
  decision. Section 8 proposes it.

## 2. What the engine allows (UE 5.8.4)

### The builder is editor-only

- `Engine/Source/Developer/NaniteBuilder/NaniteBuilder.Build.cs:5`: "NaniteBuilder module is an editor module". UBT compiles
  Developer modules only when `bBuildDeveloperTools` is set, which defaults to false for Shipping and Test
  (`Engine/Source/Programs/UnrealBuildTool/Configuration/Rules/TargetRules.cs:1212-1217`).
- `UStaticMesh::Build` is `WITH_EDITOR` only; the non-editor branch is a fatal log
  (`Engine/Source/Runtime/Engine/Private/StaticMeshBuild.cpp:120-382`). The runtime fast path of `BuildFromMeshDescriptions`
  builds LOD0 vertex and index buffers and no Nanite data. Geometry Script's Nanite options apply only to the
  editor-side asset copy.
- Nothing new in 5.6-5.8 builds Nanite data at runtime: Nanite voxels (r.Nanite.AllowVoxels, a build-time brick LOD
  for foliage), assemblies (`FNaniteAssemblyDataBuilder`, editor only), Mesh Terrain (builds cooked static meshes in an
  editor module) and NaniteStreamOut (decodes Nanite to buffers, the other direction) all start from cooked data.

### The renderer takes a runtime resource

- `Nanite::FResources` (`Engine/Source/Runtime/Engine/Public/Rendering/NaniteResources.h:452-559`) is plain data: `RootData`,
  `HierarchyNodes`, `HierarchyRootOffsets`, `PageStreamingStates`, precision and counts. `InitResources` hands it to the
  streaming manager on the render thread; `ProcessNewResources` uploads the root pages and the GPU transcodes them.
- **Every page can be a root page.** When `NumRootPages == PageStreamingStates.Num()`, `HasStreamingData()` is false and
  `StreamablePages` stays empty: nothing streams. A root page is 32 KB on the GPU
  (`NANITE_ROOT_PAGE_GPU_SIZE_BITS 15`); the root pool starts at 2,048 pages (64 MB,
  `r.Nanite.Streaming.NumInitialRootPages`) and grows by default (r.Nanite.Streaming.DynamicallyGrowAllocations 1).
- **A resource is registered once.** In a non-editor build the streaming manager empties `RootData` and the hierarchy
  after the upload (`Engine/Source/Runtime/Engine/Private/Rendering/NaniteStreamingManager.cpp:1980`). A re-mesh is a new
  `FResources` and a new add, which is how our chunk primitives already work (a new mesh is a new primitive).
- **A custom proxy is supported.** The renderer only checks `IsNaniteMesh()` and casts to `FSceneProxyBase`
  (`Renderer/Private/PrimitiveSceneInfo.cpp:903-905`). The precedent is `FNaniteGeometryCollectionSceneProxy`
  (`GeometryCollectionEngine/Private/GeometryCollection/GeometryCollectionSceneProxy.h:343`): it fills its own material
  sections, returns its own resource IDs and builds its own ray tracing geometry.
- The stock `Nanite::FSceneProxy` is tied to a `UStaticMesh`: it reads sections, distance field, cards and the ray
  tracing fallback from the mesh's render data and asserts a ray tracing proxy when ray tracing is on.
  `UStaticMeshComponent::OnGetNaniteResources` lets a component substitute its own resource but still wants the mesh
  for the rest. For us the custom proxy is the cleaner route (section 6).

### Cluster format limits

128 triangles and 256 vertices per cluster, 4 UV channels, 8-bit vertex colour, 64 materials per resource
(`Engine/Shaders/Shared/NaniteDefinitions.h`). Our vertex contract fits: material IDs and blend data ride in UV1 and the
vertex colour ([RENDERING_SYSTEM.md](../Features/RENDERING_SYSTEM.md)), both of which Nanite carries.

### The format is private and moves

`FResources::Serialize` says the data is derived and unversioned (bump `NANITE_DERIVEDDATA_VER` instead). The fixup
chunk layout is in `Engine/Source/Runtime/Engine/Internal/Nanite/NaniteFixupChunk.h`, an Internal header a project plugin
cannot include, so an encoder mirrors it. Voxel Plugin's encoder carries separate branches for 5.5, 5.6 and 5.7 (the
hierarchy node layout, the fixup chunk and the page header all changed between them). An engine upgrade that changes
the format is an encoder port, checked against the engine's NaniteEncode*.cpp and the transcode shaders. The spike's N0
(the parent's Documentation/RUNTIME_NANITE_SPIKE_PLAN.md, section 6) found that 5.6 and 5.7 changed it and that 5.8 and
the 5.9 preview did not, as far as can be seen.

## 3. How Voxel Plugin 2 does it

The builder is in VoxelCore, Source/VoxelCore/Private/VoxelNaniteBuilder.cpp (739 lines) and VoxelNanite.cpp (849
lines, the cluster and page encoder). What it does:

1. **Clusters in index order.** It walks the triangle list and starts a new cluster at 128 triangles or 256 vertices.
   No spatial partitioning (no METIS), no simplification. An optional mode reuses vertices within a cluster through
   Nanite's strip encoding.
2. **Pages packed by GPU size**, every page a root page, so nothing streams.
3. **A flat hierarchy.** A 4-wide tree over the clusters, every node bounded by the whole mesh's box, LOD errors set so
   every cluster is always selected. Nanite still culls each cluster against the frustum and the HZB, and still picks
   hardware or software raster per cluster, but it never swaps detail inside the mesh.
4. **Extended data per cluster**: an ID for the world, the chunk index and each vertex's original index. The plugin's
   "mega material" uses it to look up per-vertex material data (its "any material, anywhere" painting).
5. **A transient `UStaticMesh`** holding the Nanite resource and a one-vertex dummy LOD so the engine's checks pass,
   drawn through the stock static mesh path. It sets `bSupportRayTracing = false` on that mesh ("not supported, among
   other issues FSceneProxy crashes"); how the plugin serves ray tracing and Lumen elsewhere is not in the open core.

The plugin's docs say Nanite is its main pipeline, tessellation and displacement are supported but off by default, the
material's Displacement output doubles as the height for its height blends, and translucent materials are not
supported on the terrain. A second vendor, Realtime Mesh Component Pro, claims runtime Nanite with a full cluster DAG on
stock 5.5-5.8; that claim is unverified.

## 4. How VoxelWorlds draws today, against that

| Today ([RENDERING_SYSTEM.md](../Features/RENDERING_SYSTEM.md)) | Under runtime Nanite |
|---|---|
| One `FVoxelChunkSceneProxy` per chunk mesh and per seam bucket, registered without components (G116), about 1,100 resident | One Nanite proxy per mesh, same registration. Each mesh needs at least one 32 KB root page, so about 35 MB of pool floor for ~1,100 meshes; seam buckets are small and waste most of their page |
| `FLocalVertexFactory`, 40 B/vertex, 32-bit indices, `RendGPUMB` about 40 MB at the chord's P50 | Nanite's compressed clusters, usually much smaller per triangle; the ray tracing BLAS still needs positions and indices somewhere (section 6) |
| CPU meshers only (DC mean 9,336 triangles per non-empty chunk, p95 41,598; meshing p50 1.3 ms, p95 51 ms) | An encode step on the mesh worker after meshing; linear in triangles, called cheap by Voxel Plugin, unmeasured for us |
| Chunk LOD by distance bands, crossfade by dithered Masked twin material for 0.35 s | Unchanged. The fade material becomes pixel-programmable raster for the fade's duration only |
| Shadows: CSM to 20,000 uu plus two far cascades; VSM off (`r.Shadow.Virtual.Enable=0`) after a non-Nanite job queue overflow | VSM becomes the natural choice for the terrain; its page caching suits static chunks |
| Hardware RT: one BLAS per chunk and seam mesh, 55-62 MB, +0.10-0.14 ms GPU | Our proxy keeps building it from a position/index copy, or the project moves to `r.RayTracing.Nanite.Mode` 1/2 (section 6) |
| Lumen cards: axis or fitted cards behind `voxel.Render.LumenCards` (default 0) | Same cards, supplied by our proxy; Nanite neither adds nor removes them |
| Master material `M_VoxelMaster`: **Masked**, Default Lit, triplanar `Texture2DArray`s, no WPO, PDO or displacement | Masked means every terrain pixel goes through programmable raster; the master wants an opaque variant (leaves already flag masking per material in the LUT, which could move to a separate masked section) |
| Far field: ~1,500 dynamic heightfield patches, no shadows | Untouched by the chunk work; a later candidate (bigger patches, Nanite's raster handles the triangles) |
| Water: translucent or Single Layer Water, region proxy | Stays non-Nanite (Nanite translucency is R&D, off) |

## 5. What it would buy

### Shadows (the clearest win)

VSM was turned off because the non-Nanite terrain overflowed its job queue (`VoxelSceneProxy.cpp`, the comment on the
overflow; [VISUAL_POLISH_PLAN.md](../VISUAL_POLISH_PLAN.md)). With the terrain on Nanite, VSM draws it through the
Nanite path and caches its pages while chunks stay still. That could replace the CSM range and the far cascades, whose
cost is per-batch: shadow batches went from 672 to 2,460 at a low sun on the chord with the far cascades on, +0.3 ms GPU
([RENDERING_SYSTEM.md](../Features/RENDERING_SYSTEM.md) "Far Shadow Cascades"). G126 (casters lost past the engine octree root)
was a CSM problem. The other non-Nanite casters (scatter, NPCs, dungeon modules) would need checking against the same
overflow before VSM goes back on.

### Render thread

The render thread is busy 9.8-12.8 ms P50 on the bench routes, more than the GPU frame (6.4-7.7 ms). Our chunk meshes
are on the cached static path already, so the per-mesh cost left is visibility, shadow batches and draw submission;
under Nanite the terrain is a few instances in GPU Scene and one GPU-driven pass per view. Expected to help, size
unknown: the bench does not record the engine's draw call count, so the spike must measure `RTBusyMs` directly.

### Tessellation and displacement

In 5.8 `r.Nanite.Tessellation` defaults to 1 (not marked experimental in the source); a material sets
`bEnableTessellation`, writes the Displacement output, and `FDisplacementScaling` (magnitude, centre) scales it along the
vertex normal. Displacement needs nothing from the mesh beyond what the material samples, and our triplanar material
samples by world position, so a height slice per texture in the arrays is the missing input. That gives close-range
rock and soil relief without more voxels. The caveats:

- **Cracks at mesh boundaries.** Chunk interiors and seam meshes are separate primitives. Displacement is crack-free
  only where both sides compute the same height along the same normal at the shared vertices; our seams duplicate
  border vertices, and the normals there must match bit for bit.
- **Collision does not move.** The character stands on the undisplaced mesh; displacement must stay small (centimetres)
  or the feet float and sink.
- **Cost and bugs.** Dicing at r.Nanite.DicingRate 2 px is real GPU work; a 5.8 forum report has displacement broken
  on landscapes that worked in 5.7.4. VSM can be told to skip tessellation (r.Shadow.Virtual.Nanite.AllowTessellationDirectional and ...Local, new in 5.8).

### What it does not buy

- **No continuous LOD.** The flat hierarchy has no coarser clusters, so Nanite draws every triangle we give it. Getting
  Nanite's LOD would mean building a cluster DAG at runtime (partitioning and simplification per chunk), the expensive
  half of the editor builder. Our distance bands already do that job; the spike should not attempt it.
- **No faster meshing.** CPU meshing and seams stay the streaming limit; the encode adds to it.
- **No change to Lumen's coverage.** Cards and the ray tracing scene are what Lumen sees, as now.
- **Triangle density is now cheap to draw, not to make.** Pushing the stride-1 band further out becomes a raster
  non-issue but stays a meshing, memory and collision cost.

## 6. The design questions a build would face

1. **Proxy.** A `Nanite::FSceneProxyBase` subclass registered the way chunk primitives are now
   (`FPrimitiveSceneDesc`, `BatchAddPrimitives`), filling its own material sections and returning its own ray tracing
   geometry and cards. The transient-`UStaticMesh` route Voxel Plugin uses trades that for a UObject per mesh and the
   stock proxy's assumptions (it asserts a ray tracing proxy).
2. **Ray tracing.** Three options: keep a position/index buffer per mesh only for the BLAS (most of today's vertex
   memory comes back); use `r.RayTracing.Nanite.Mode 1` (the engine streams Nanite out to buffers and builds the BLAS,
   project-wide); or mode 2 (CLAS, needs cluster ray tracing support in the RHI). The first is the safe start.
3. **Fallback.** Nanite needs SM6 and a supporting platform. The `FLocalVertexFactory` path stays as the fallback and the
   A/B, behind a cvar like `voxel.Render.ChunkPrimitives` is today.
4. **The material.** An opaque `M_VoxelMaster` for Nanite, masked only where a material is masked; the fade twin stays
   Masked. Substrate is on in this project; Nanite supports it.
5. **Seam buckets.** Small meshes waste a 32 KB page each. One resource per owner chunk holding interior and seams
   (`HierarchyRootOffsets`, as GeometryCollection does) halves the pages but re-registers the interior on a seam change.
6. **The encoder.** Ours, from the engine's NaniteEncode*.cpp and Voxel Plugin's MIT encoder as references (its
   files carry an "All Rights Reserved" header despite the repository's MIT licence; read, don't copy, until that is
   settled). A test per engine version that encodes known meshes and checks the GPU decode, since a format slip shows
   as garbage or a GPU hang, not an error.

## 7. Risks

- **Engine upgrades.** Every minor release can change the page, hierarchy or fixup layout. The encoder joins the list of things an upgrade must port and test.
- **Device removal.** A malformed page hangs the GPU. The in-editor gate already catches device loss (G38); the spike
  needs a validation mode that decodes on the CPU before upload.
- **Shipping builds.** The encoder is our code, so no Developer module is linked; the packaged demo must still be
  checked early, as forum reports show Nanite-only crashes in 5.8 packaged builds (procedural vegetation).
- **Two renderers to keep.** The non-Nanite path stays for platforms and as the reference, so every rendering change
  (fades, cards, RVT) is done twice.

## 8. Recommendation: a time-boxed spike

Goal: numbers to decide on, not a feature. On a branch, never merged as is (like the D-VP4 spike). Planned in
[RUNTIME_NANITE_SPIKE_PLAN.md](../../../../Documentation/RUNTIME_NANITE_SPIKE_PLAN.md) (phases N0-N4, D152-D156).

1. **Encode and draw.** An encoder for a chunk mesh (positions, normals, UV0, UV1, colour), a Nanite proxy beside
   `FVoxelChunkSceneProxy`, a cvar to switch, an opaque copy of the master material. Gate: the demo's terrain looks the
   same in a PIE screenshot A/B and the in-editor gate stays green.
2. **Measure.** The chord and great-circle bench (`Scripts/qa/bench_gate.py`), ABBA in fresh editors: GPU, `RTBusyMs`,
   GT, encode ms per chunk on the worker, root pool pages, `RendGPUMB` with the RT copy counted. Then the same with VSM
   on, against today's CSM with far cascades.
3. **Look.** Tessellation on one material with a height slice: a shot at a cliff and at a seam, at the noon and low sun.

Decision after the spike: build (a plan with phases: encoder, proxy, material, RT, VSM, tessellation), park, or drop.
If built, [ARCHITECTURE.md](../ARCHITECTURE.md) "No Nanite" and the archived engine-integration decision get rewritten;
until then they stand, with this note as the correction of their premise.

## Sources

- UE 5.8.4 source, paths as cited.
- VoxelCore: github.com/VoxelPlugin/VoxelCore, Source/VoxelCore/Private/VoxelNaniteBuilder.cpp and VoxelNanite.cpp.
- Voxel Plugin docs: docs.voxelplugin.com (Feature Overview; Authoring Materials).
- Realtime Mesh Component Pro listing on Fab (vendor claims).
- 80.lv, "A Cool Setup For Generating Nanite Geometry in UE5 at Runtime" (Voxel Plugin's 2024 demo).
- Tom Looman, "Unreal Engine 5.8 Performance Highlights" (VSM tessellation cvars).
- Epic forums: Nanite displacement in 5.8 vs 5.7.4; Nanite crashes in 5.8 packaged builds.
