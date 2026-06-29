# Engine Integration Strategy — Leveraging UE 5.8 Systems

**Status**: Planning / active investigation
**Scope**: Which built-in UE 5.8 systems VoxelWorlds should adopt, which it should keep hand-built, and why.
**Audience**: Anyone deciding how much of the engine's terrain/rendering toolchain to lean on instead of maintaining bespoke equivalents.

---

## TL;DR

| System | Decision | Why |
|--------|----------|-----|
| **Nanite** | ❌ Cannot use (runtime path) | Nanite geometry is **build/cook-time only**. It cannot be constructed from meshes generated at runtime in a packaged game. Our primary goal is runtime procedural generation, which structurally excludes Nanite. |
| **Runtime Virtual Textures (RVT)** | ✅ Adopt (first) | Offloads material/texture blending into a cached pass; biggest near-term rendering win. Works with our existing `FVoxelSceneProxy` once it emits RVT mesh batches. |
| **PCG (runtime generation)** | ✅ Adopt | "Generate at Runtime" PCG can place scatter/foliage/props on terrain at runtime. Candidate to replace parts of `SCATTER_SYSTEM`. |
| **World Partition** | ✅ Adopt (finite worlds) | Hand off finite-world streaming to a maintained engine system. |
| **Mesh Terrain** | ⚠️ Secondary only | Editor authoring tool (preview/compiled-section split, modifier stack). Useful only as an offline bake/finishing canvas for hand-crafted projects — not a runtime terrain engine. |
| **DynamicMeshComponent / GeometryScript** | ❌ Avoid | Dynamic draw path, no instancing, unoptimized buffers — strictly worse than our `FLocalVertexFactory` proxy for runtime-generated geometry. |

---

## The decision that drives everything: Nanite is build-time only

Confirmed against Epic documentation and community sources (UE 5.8):

- The Nanite builder analyzes a mesh and breaks it into hierarchical clusters **during import/cook**. There is no supported path to construct Nanite geometry from runtime-generated triangles in a packaged game.
- The only runtime-dynamic Nanite feature is **tessellation / programmable displacement**, which deforms a mesh that is *already* Nanite. It cannot turn runtime triangles into Nanite ones.

This forces a clean fork, because "generate at runtime" and "use Nanite" are mutually exclusive:

- **Path A — generate at cook/editor time, ship static.** Unlocks the full stack (Nanite, RVT, PCG, Mesh Terrain). World is fixed per build.
- **Path B — generate at runtime per session (from a seed), freeze in game.** Nanite is permanently excluded. Everything else remains available.

**Project goal (settled):**
- **Primary = Path B.** Runtime procedural terrain for environmental variation, finite worlds acceptable, static once generated in game.
- **Secondary = Path A bridge.** Export voxel geometry into editor tools (Mesh Terrain / static meshes) for projects that want a hand-crafted approach.

### Why our hand-built renderer is the *right* core for Path B

For runtime-generated geometry, our existing renderer is the best option available — not a poor substitute for Nanite:

- The only engine LOD system that would beat our custom morph (`LOD_SYSTEM.md`) is Nanite, and Nanite is excluded the moment geometry is born at runtime.
- `DynamicMeshComponent` / GeometryScript would be a **downgrade**: dynamic draw path, no instancing, unoptimized index/vertex buffers versus our zero-copy `FLocalVertexFactory` static-draw-equivalent path (see `RENDERING_SYSTEM.md`).
- Our custom LOD morph + DC weld solve a problem Nanite is not allowed to touch in this mode. They are not reinventing Nanite poorly.

The takeaway: **keep the core, bolt on the engine systems that work at runtime regardless of the Nanite line (RVT, runtime PCG, World Partition).**

---

## Adoption plan (priority order)

### 1. Runtime Virtual Textures (highest leverage) — investigation in progress

**Goal:** Render terrain material into an RVT so per-pixel material/texture blend cost is cached and decoupled from main-pass shading; large textured terrain benefits most.

**Why it's the first target:** It is the single engine system that most reduces our material cost, and it is compatible with a custom scene proxy — unlike Nanite.

**Integration reality (grounded in current code):**

Our `FVoxelSceneProxy::GetDynamicMeshElements` currently emits mesh batches for the **main pass only**. RVT support requires:

1. **Component side** (`UVoxelWorldComponent`): expose a `RuntimeVirtualTextures` array (`TArray<URuntimeVirtualTexture*>`) and the RVT pass flags, so the base `FPrimitiveSceneProxy` picks them up (it reads the component's RVT list in its constructor and populates `RuntimeVirtualTextureMaterialTypes`).
2. **Proxy side** (`FVoxelSceneProxy`): in `GetDynamicMeshElements`, also emit mesh batches flagged for the virtual texture pass — `MeshBatch.bRenderToVirtualTexture = true` with the appropriate `RuntimeVirtualTextureMaterialType`. Today only main-pass batches are produced.
3. **Relevance** (`GetViewRelevance`): ensure RVT relevance is reported so the renderer gathers the proxy for the RVT pass.
4. **Material**: the terrain master material (`MASTER_MATERIAL_SETUP.md` / `M_VoxelMaster`) must have **Output to Virtual Texture** wired (or a paired material that writes the RVT), and a sampling path that reads the RVT back for main-pass shading.

**Open question the spike must answer:** whether a `GetDynamicMeshElements` (dynamic-relevance) proxy writes to RVT cleanly, or whether RVT prefers static mesh elements. This is the main technical risk in the whole plan and the reason RVT is being de-risked first.

#### RVT spike / test plan

Objective: confirm `FVoxelSceneProxy` can write to an RVT and the terrain material reads it back correctly.

Progress legend: ✅ done · ⏳ needs running editor (post shader-recompile).

0. ✅ **Enable virtual texture support** — `r.VirtualTextures=True` added to `Config/DefaultEngine.ini`. Requires a one-time global shader recompile on next editor launch.
1. ✅ **Proxy wiring** — `FVoxelSceneProxy` opts in via `bSupportsRuntimeVirtualTexture = true` (ctor) and emits a cloned RVT mesh batch per `RuntimeVirtualTextureMaterialType` in the terrain loop of `GetDynamicMeshElements` (water excluded). Pattern mirrors `FBaseDynamicMeshSceneProxy`. The loop is a no-op when no RVT is assigned (zero overhead). Compiles clean (`UnrealEditor-VoxelRendering.dll`).
2. ✅ **Component wiring** — none required: `UVoxelWorldComponent` inherits `UPrimitiveComponent::RuntimeVirtualTextures`; the base proxy ctor populates `RuntimeVirtualTextureMaterialTypes` from it automatically.
3. ✅ **Asset setup** — `URuntimeVirtualTexture` asset (BaseColor_Normal_Roughness) at `/Game/PluginTesting/RVT/RVT_VoxelTerrain`; `ARuntimeVirtualTextureVolume` covering ±15000 XY / ±10000 Z over the terrain (unit box is local (0,0,0)-(1,1,1): set location=min, scale=size). RVT assigned to the volume's component and to the world component (via config, see step 1-permanent below).
4. ✅ **Material** — read-back material `M_RVT_Readback` (`Runtime Virtual Texture Sample` → BaseColor/Normal/Roughness, plus BaseColor→Emissive for visibility) authored via the `unreal-mcp` MaterialTools toolset. **Critical:** the terrain master material **`M_VoxelMaster`** also needed a `Runtime Virtual Texture Output` node (BaseColor/Normal/Roughness) — without it the RVT pass writes nothing and read-back reads black. Under `r.Substrate=True` the legacy output pins auto-convert to a slab; both materials compile clean.
5. ✅ **Verify (live)** — PIE on the test map; read-back plane (above terrain) shows the terrain biome albedo map, matching the lit terrain with the plane hidden. The black→color flip when the RVT Output node was added isolates the write path. `GT0–GT7` suite green (8/8); no proxy crashes.
6. ✅ **Decision gate** — dynamic-path RVT writes are clean → full material integration done.

#### Spike result (2026-06-27, live in PIE on `/Game/PluginTesting/VoxelWorldsTest`)

**Primary risk retired: a dynamic `GetDynamicMeshElements` proxy integrates with RVT cleanly.** Verified live:
- With a valid `URuntimeVirtualTexture` (BaseColor_Normal_Roughness) assigned to the live `UVoxelWorldComponent`, the recreated proxy logged `RVT material types=1` — i.e. the base `FPrimitiveSceneProxy` accepted our proxy as an RVT writer and populated `RuntimeVirtualTextureMaterialTypes`, so our per-chunk RVT batch-emission loop is active.
- `GetDynamicMeshElements` runs every frame with the RVT path active and **no ensures, asserts, or errors**. The proxy recreates safely when the RVT is (un)assigned (`PostEditChangeProperty` → render-state recreate).
- Decision gate → **PASS**: proceed to full material integration (read-back).

#### Round-trip result (2026-06-27, committed on `feature/rvt-integration`, submodule 578027e / parent 0fea79c)

**Visual round-trip works: terrain writes its albedo into the RVT and a read-back material samples it back.** Three pieces, all committed:
1. **Proxy opt-in** (spike) — `bSupportsRuntimeVirtualTexture=true` + per-`RuntimeVirtualTextureMaterialType` batch emission; logs `RVT material types=N`.
2. **Permanent C++ wiring** (replaces the spike's runtime Python injection) — `UVoxelWorldConfiguration::RuntimeVirtualTextures` (EditAnywhere TArray); `FVoxelCustomVFRenderer::Initialize` copies it onto `WorldComponent->RuntimeVirtualTextures` *before* `RegisterComponent()`. Logs `Assigned N Runtime Virtual Texture(s) to WorldComponent from configuration`.
3. **Terrain RVT Output node** (the piece the spike's "writes need no material change" missed) — `M_VoxelMaster` now has a `Runtime Virtual Texture Output` node fed by BaseColor/Normal/Roughness. The proxy emitting RVT batches is **necessary but not sufficient**; without this node the RVT pass writes nothing and pages read back black. Inert when no RVT is assigned to the primitive, so safe for all worlds.

**Verification:** read-back plane above the terrain showed the biome albedo map; hiding the plane showed the same biome boundary on the lit terrain. The black→color flip when the RVT Output node was added is the cleanest proof.

**Gotchas:** RVT assets/levels can't be saved during PIE (create/save outside PIE); the bound test config is `DemoWorldConfig` — assign the RVT **in-memory** (PIE duplicates the editor world, so in-memory config + spawned volume/plane are picked up) rather than mutating it. Force-killing the editor pops a "Restore Packages" modal on relaunch that hangs MCP/Claudius until dismissed.

**Next:** runtime PCG (§2) + World Partition (§3).

### 2. PCG with Runtime Generation

**Goal:** Place scatter/foliage/props on the generated terrain at runtime, driven by a generation source (player position).

**Why:** PCG's "Generate at Runtime" path is a maintained engine system that overlaps our `SCATTER_SYSTEM`. Candidate to replace parts of it rather than maintaining bespoke scatter.

**Decisions (locked with project owner):** (1) Surface source = **re-sample the procedural generator**
(not physics-trace, not the live voxel cache) — decoupled from chunk streaming, keeps biome/material/slope
attributes. (2) Scope = **coexist/additive** — PCG is a new opt-in scatter path; voxel-tree injection +
edit-driven removal stay bespoke; retire nothing until parity is proven.

**Integration notes:**
- Terrain surface is queried by a custom PCG node that re-samples the generator (not collision/landscape).
- Sequencing: PCG runtime gen must trigger *after* a chunk's terrain exists and re-run on edits (`EDIT_LAYER.md` dirty events).
- Evaluate against `SCATTER_SYSTEM` feature parity (voxel tree injection, biome-aware density) before committing to replacement vs. coexistence.

#### Implementation status (2026-06-27)

Phases 0–2 done & committed on `feature/pcg-integration`; **visual round-trip working in PIE** — PCG
places static meshes on the runtime voxel terrain via the custom node.

- **Phase 0** — `PCG` plugin enabled; new **`VoxelPCG`** runtime module isolates the PCG dependency
  (declared in `VoxelWorlds.uplugin` → transitively enables PCG).
- **Phase 1** — new public **`FVoxelSurfaceQuery`** (`VoxelGeneration`): stateless/thread-safe surface
  queries (height, normal, slope, biome, material) against `IVoxelWorldMode` for any world X,Y. The tree
  injector was refactored onto it (deleted its duplicated private slope/biome helpers; no behavior change).
- **Phase 2** — **Voxel Surface Sampler** node (`UPCGVoxelSurfaceSamplerSettings` /
  `FPCGVoxelSurfaceSamplerElement`): steps an XY grid over the sampling bounds, re-samples the generator
  via `FVoxelSurfaceQuery`, emits PCG points carrying `Normal`/`Slope`/`MaterialID`/`BiomeID` (names match
  `FScatterDefinition` filters). Resolves the live world's world-mode/config by finding an initialized
  `UVoxelChunkManager` (mirrors `UVoxelMapSubsystem::ResolveChunkManager`).
- **Phase 3 (round-trip verified)** — graph `Input → Voxel Surface Sampler (Bounding Shape) → Static Mesh
  Spawner → Output` on a PCGVolume over the terrain; cubes spawn on the runtime terrain in PIE. **Two
  follow-ups observed:** (a) the node only executes when it is on the graph's execution path — wire the
  graph **Input** node into its **Bounding Shape** pin (consider marking that pin required). (b) Points sit
  at the *analytic* generator height, which differs from the *voxelized* DC/MC surface by up to ~1 voxel
  (worst on slopes) — plus the cube's center pivot. This is the inherent surface-approximation of the
  re-sample approach (vs the bespoke scatter reading the exact voxel column). Mitigations: align-to-normal,
  a surface Z offset, or an optional voxel-cache snap mode for near points.

**Architecture (agreed 2026-06-27):** the full design — biome-keyed decoration, an **edit-aware hybrid
sampler** (voxel-merged data near / generator far, which also fixes Phase 3's Z-hugging), a priority stack
(POI > construction > ambient), and a standalone **Claims/World-Annotation system** feeding PCG (by tag),
Map and Compass — is in [PCG_DECORATION_ARCHITECTURE.md](PCG_DECORATION_ARCHITECTURE.md). PCG output is
strictly visual; biome selection stays voxel-authoritative; integration with game-level POI/construction
systems is boundary-safe (tagged PCG-readable data, no code dependency).

**Remaining (revised — supersedes the old blunt-toggle Phase 4):** Phase 4 = the edit-aware hybrid sampler
(now a hard requirement, not a toggle); then per-biome routing, claims-driven exclusion, edit/claim
reactivity, and finally a per-category coexistence/parity pass (which replaces the global `bUsePCGScatter`
idea). Full phase table in the design doc §9.

### 3. World Partition (finite worlds)

**Goal:** Use World Partition for finite-world spatial streaming instead of maintaining that concern ourselves where our streaming overlaps it.

**Integration notes:**
- Our runtime chunk streaming (`VoxelChunkManager`, `STREAMING_PERFORMANCE.md`) is bespoke. World Partition is most relevant for finite worlds with baked or session-static content; reconcile ownership of the streaming decision so the two don't fight.
- Lower priority than RVT/PCG; revisit once the runtime-static world flow is concrete.

---

## Secondary path: voxel → editor-tools bridge (deferred)

This is the *opposite* lifecycle — offline — so Nanite and Mesh Terrain are both fair game here.

- Our generator already produces CPU-side triangles (the PMC/collision path proves CPU mesh extraction works).
- A commandlet or editor utility can emit that geometry as **static meshes** (Nanite-enabled) or populate a **Mesh Partition**, giving hand-crafted projects a procedural starting point they then sculpt/paint.
- This reuses generation code wholesale and does **not** gate the primary runtime path. Build it when a project actually needs it.

Mesh Terrain specifics (UE 5.8, Experimental):
- Core object is a **Mesh Partition**; authoring is via stacked non-destructive **modifiers** (actor components that deform partition geometry), plus sculpt/paint and weight channels (injectable by modifiers or PCG).
- **Preview sections** (editor) vs **compiled sections** (runtime) confirm an author → compile/bake model — i.e. an editor tool, not a runtime engine.
- Integrates natively with Nanite, RVT, and PCG. "Use caution when shipping" (Experimental).
- If we pursue this, the live-engine inspection path (`unreal-mcp` reflection / semantic asset search) should confirm the actual modifier base UCLASS and whether any partition API is runtime/Blueprint-callable before designing the bridge.

---

## Risks & open questions

- **RVT × dynamic-path proxy** (primary risk) — does a `GetDynamicMeshElements` proxy write to RVT cleanly? Resolved by the spike above.
- **PCG surface source** — does PCG runtime gen need real collision, or can it sample voxel data directly? Affects whether scatter depends on the collision pipeline.
- **PCG vs SCATTER_SYSTEM** — replace or coexist? Decide after a parity pass.
- **World Partition vs bespoke streaming** — who owns the streaming decision for finite worlds?
- **Mesh Terrain extensibility** — is the modifier API usable programmatically for a bake bridge, or editor-interaction only? Requires live inspection.

---

## References

- Internal: [PCG_DECORATION_ARCHITECTURE.md](PCG_DECORATION_ARCHITECTURE.md) (PCG decoration design), [RENDERING_SYSTEM.md](../Features/RENDERING_SYSTEM.md), [LOD_SYSTEM.md](../Features/LOD_SYSTEM.md), [MATERIAL_SYSTEM.md](../Features/MATERIAL_SYSTEM.md), [MASTER_MATERIAL_SETUP.md](../Guides/MASTER_MATERIAL_SETUP.md), [SCATTER_SYSTEM.md](../Features/SCATTER_SYSTEM.md), [BIOME_SYSTEM.md](../Features/BIOME_SYSTEM.md), [EDIT_LAYER.md](../Features/EDIT_LAYER.md), [STREAMING_PERFORMANCE.md](STREAMING_PERFORMANCE.md)
- Epic: Nanite Virtualized Geometry, Runtime Virtual Textures, Mesh Terrain (UE 5.8 docs)
