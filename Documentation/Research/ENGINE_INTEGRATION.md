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
3. ⏳ **Asset setup** — create a `URuntimeVirtualTexture` asset (BaseColor_Normal_Roughness material type) and an `ARuntimeVirtualTextureVolume` covering the test terrain bounds; assign the RVT to the voxel world component's `RuntimeVirtualTextures` array.
4. ⏳ **Material** — author (Substrate) a material that outputs to the RVT and a main-pass terrain material that samples it (`Runtime Virtual Texture Sample` node). Note: project uses `r.Substrate=True`, so RVT output wiring differs from legacy materials.
5. ⏳ **Verify (live)** — PIE on the CPU-MC test map; use the live-verify workflow (`unreal-mcp` for typed inspection + Claudius PIE screenshots). Pass criteria:
   - RVT volume shows terrain content (not blank) in the RVT debug view.
   - Main-pass terrain shading reads from the RVT (toggle RVT off → visible difference).
   - No regression in the `GT0–GT7` correctness suite; no proxy crashes on chunk add/remove/LOD morph.
6. ⏳ **Decision gate** — if dynamic-path RVT writes are clean → proceed to full material integration. If not → evaluate a static-element path or a dedicated RVT-writer primitive.

#### Spike result (2026-06-27, live in PIE on `/Game/PluginTesting/VoxelWorldsTest`)

**Primary risk retired: a dynamic `GetDynamicMeshElements` proxy integrates with RVT cleanly.** Verified live:
- With a valid `URuntimeVirtualTexture` (BaseColor_Normal_Roughness) assigned to the live `UVoxelWorldComponent`, the recreated proxy logged `RVT material types=1` — i.e. the base `FPrimitiveSceneProxy` accepted our proxy as an RVT writer and populated `RuntimeVirtualTextureMaterialTypes`, so our per-chunk RVT batch-emission loop is active.
- `GetDynamicMeshElements` runs every frame with the RVT path active and **no ensures, asserts, or errors**. The proxy recreates safely when the RVT is (un)assigned (`PostEditChangeProperty` → render-state recreate).
- Decision gate → **PASS**: proceed to full material integration (read-back).

**Method note (temporary):** the spike injected the RVT at runtime via Python (`set_editor_property("runtime_virtual_textures", ...)`) because the `UVoxelWorldComponent` is created at runtime by `FVoxelCustomVFRenderer`. For the real feature this must become **permanent C++ wiring** — carry an optional RVT list on `UVoxelWorldConfiguration` and assign `WorldComponent->RuntimeVirtualTextures` before `RegisterComponent()` in the renderer. Also note `RuntimeVirtualTexture` assets cannot be saved while PIE is active.

**Still to verify (visual round-trip):** needs (a) permanent C++ wiring + a saved RVT asset, (b) an `ARuntimeVirtualTextureVolume` covering the terrain (no volume = no capture pages), (c) a Substrate read-back material (`Runtime Virtual Texture Sample`), and (d) camera on the terrain (anti-fall) for the screenshot. None of these are blocked — they're the next session's work.

### 2. PCG with Runtime Generation

**Goal:** Place scatter/foliage/props on the generated terrain at runtime, driven by a generation source (player position).

**Why:** PCG's "Generate at Runtime" path is a maintained engine system that overlaps our `SCATTER_SYSTEM`. Candidate to replace parts of it rather than maintaining bespoke scatter.

**Integration notes:**
- Terrain surface must be queryable by PCG (collision/landscape-equivalent or a custom sampler). Our async collision trimesh (`RENDERING_SYSTEM.md` → Collision System) may serve as the surface source; otherwise a custom PCG point sampler reading voxel data.
- Sequencing: PCG runtime gen must trigger *after* a chunk's terrain exists and re-run on edits (`EDIT_LAYER.md` dirty events).
- Evaluate against `SCATTER_SYSTEM` feature parity (voxel tree injection, biome-aware density) before committing to replacement vs. coexistence.

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

- Internal: [RENDERING_SYSTEM.md](../Features/RENDERING_SYSTEM.md), [LOD_SYSTEM.md](../Features/LOD_SYSTEM.md), [MATERIAL_SYSTEM.md](../Features/MATERIAL_SYSTEM.md), [MASTER_MATERIAL_SETUP.md](../Guides/MASTER_MATERIAL_SETUP.md), [SCATTER_SYSTEM.md](../Features/SCATTER_SYSTEM.md), [STREAMING_PERFORMANCE.md](STREAMING_PERFORMANCE.md)
- Epic: Nanite Virtualized Geometry, Runtime Virtual Textures, Mesh Terrain (UE 5.8 docs)
