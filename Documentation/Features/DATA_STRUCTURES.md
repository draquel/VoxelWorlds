# Core Data Structures Reference

> State as of 2026-11-02 (code review CR5). Terms: `Documentation/GLOSSARY.md`.

The plugin's core value types, as declared in `Source/VoxelCore/Public` (fields abbreviated; the headers carry the
full declarations and their comments), and the version fingerprints that protect saved data.

## Blueprint Type Compatibility

Blueprint supports no unsigned integer wider than `uint8`:
- `uint8` is the Blueprint "Byte" type.
- `uint16`, `uint32` and `uint64` are not supported; reflected counters and seeds use `int32` or `int64`.

## Core Voxel Data

### FVoxelData (4 bytes, `VoxelData.h`)
```cpp
struct FVoxelData {
    uint8 MaterialID;   // 0-255 material types
    uint8 Density;      // 0-255 (127 = surface, <127 = air, >127 = solid)
    uint8 BiomeID;      // 0-255 biome types
    uint8 Metadata;     // AO (low 4 bits) + flags (high 4 bits)
};

// Metadata layout:
//   Bits 0-3: ambient occlusion (0-15)            GetAO() / SetAO()
//   Bits 4-7: flags                               GetFlags() / SetFlags()
//     flag 0x01 VOXEL_FLAG_WATER       water-filled air voxel
//     flag 0x02 VOXEL_FLAG_CAVE        carved by cave generation
//     flag 0x04 VOXEL_FLAG_UNDERGROUND underground air or cave boundary solid
//     flag 0x08 reserved
//
// Flag accessors: HasWaterFlag(), HasCaveFlag(), HasUndergroundFlag() and their setters.
```

The flag values are `VOXELGEN_FLAG_*` in `Shaders/Shared/VoxelGenerationShared.ush`, which C++ and the generation
shaders share. See [UNDERGROUND_CLASSIFICATION.md](UNDERGROUND_CLASSIFICATION.md) for how `VOXEL_FLAG_UNDERGROUND`
propagates.

### FChunkDescriptor (`ChunkDescriptor.h`)
```cpp
struct FChunkDescriptor {
    FIntVector ChunkCoord;                      // chunk position in chunk space
    int32 LODLevel;                             // 0 = finest
    int32 ChunkSize;                            // voxels per edge (the configuration's; VOXEL_DEFAULT_CHUNK_SIZE 32)
    TSharedPtr<TArray<FVoxelData>> VoxelArray;  // resident voxels (or null), shared copy-on-write
    FBox Bounds;
    bool bIsDirty;
    bool bHasEdits;
    int32 GenerationSeed;
    EChunkState State;                          // Unloaded, PendingGeneration, Generating, PendingMeshing,
                                                // Meshing, Loaded, PendingUnload
    EVoxelDataResidency Residency;              // Empty, Resident, Uniform, Compressed
    uint32 ContentVersion;                      // bumped on every change to the chunk's voxels
    FVoxelData UniformValue;                    // the value of a Uniform chunk
    TArray<uint8> CompressedVoxelData;          // a far-compressed chunk's compact form
};
```

The resident array is read through `GetVoxelDataForRead()` (or `GetResidentArray()`, which does not expand a compact
chunk) and written through `GetVoxelDataMutable()`, `SetVoxel()` or `SetVoxelByIndex()`. It is shared copy-on-write:
a snapshot of a resident chunk without edits and the far-compression encode take `ShareVoxelData()` instead of a copy,
and a write while one of them still holds the array clones it first, so a reader's voxels never change under it
(G71). A chunk whose generated voxels are all one value (all air, all rock, open sea) is stored collapsed
(`CollapseToUniform`, residency `Uniform`) from the moment it is generated (G67); a far chunk is compacted by far
compression (`LOD_SYSTEM.md`, `STREAMING_AND_SEAMS.md`).

## Rendering Data

### FVoxelVertex (28 bytes, `VoxelVertex.h`, custom vertex factory)
```cpp
struct FVoxelVertex {
    FVector3f Position;          // 12 bytes, relative to the render region's origin
    uint32 PackedNormalAndAO;    // 4 bytes
    FVector2f UV;                // 8 bytes
    uint32 PackedMaterialData;   // 4 bytes: material 0-7, biome 8-15, a material blend's second material 16-23,
                                 // flags 24-31 (24 the blend, 25 its weight; VP2-O4)
};
```

## LOD System Data (`LODTypes.h`)

### FLODQueryContext
```cpp
struct FLODQueryContext {
    FVector ViewerPosition;              // the streaming viewer
    FVector ViewerForward;
    double ViewerLead;                   // the descent prefetch's lead ahead of the camera
    bool bFarFieldActive;                // the cut-over is allowed only with a far field
    float FieldOfView;
    TArray<FPlane> FrustumPlanes;
    FVector WorldOrigin;
    EWorldMode WorldMode;
    int32 MaxChunksToUnloadPerFrame;
};
```

### FChunkLODRequest
```cpp
struct FChunkLODRequest {
    FIntVector ChunkCoord;
    int32 LODLevel;
    float Priority;
};
```

### FLODBand (distance band strategy)
```cpp
struct FLODBand {
    float MinDistance;
    float MaxDistance;
    int32 LODLevel;
    int32 VoxelStride;
    int32 ChunkSize;
};
```

## Biome System Data

### FBiomeDefinition (`VoxelBiomeDefinition.h`)
```cpp
struct FBiomeDefinition {
    uint8 BiomeID;
    FString Name;
    FVector2D TemperatureRange;
    FVector2D MoistureRange;
    FVector2D ContinentalnessRange;
    uint8 SurfaceMaterial, SubsurfaceMaterial, DeepMaterial;
    float SurfaceDepth, SubsurfaceDepth;              // in voxels
    uint8 UnderwaterSurfaceMaterial, UnderwaterSubsurfaceMaterial;
    TArray<FOreVeinConfig> BiomeOreVeins;
    bool bAddToGlobalOres;
    int32 SelectionPriority;
};
```

See [BIOME_SYSTEM.md](BIOME_SYSTEM.md) and [ORE_VEINS.md](ORE_VEINS.md).

## Material System Data

### FVoxelMaterialDefinition (`VoxelMaterialDefinition.h`)
```cpp
struct FVoxelMaterialDefinition {
    uint8 MaterialID;           // 0-255 material ID
    FString Name;               // display name
    FColor Color;               // fallback colour (used when no atlas)

    // Atlas position (cubic mode)
    int32 AtlasColumn = 0;
    int32 AtlasRow = 0;

    // Texture array (smooth mode)
    int32 ArrayIndex = -1;
    float TriplanarScale = 1.0f;
    float UVScale = 1.0f;

    // Opacity flags
    bool bIsMasked = false;     // alpha cutout (BLEND_Masked)
    bool bNonOccluding = false; // render all faces against other materials

    float Hardness = 1.0f;      // seconds to break bare-handed in a cubic world
};
```

- `bIsMasked`: triangles with this material render with `BLEND_Masked`, the albedo alpha as the opacity mask (leaves
  with holes).
- `bNonOccluding`: faces between this material and any different material always generate; same-material adjacency
  still culls internal faces (leaves keep their visual density).
- `Hardness`: read through `FVoxelMaterialRegistry::GetHardness`; a timed break takes hardness / MiningSpeed.

## Edit Layer Data

### FVoxelEdit (`VoxelEditTypes.h`)
```cpp
struct FVoxelEdit {
    FIntVector LocalPosition;
    FVoxelData NewData;          // the value for Set and Paint
    EEditMode EditMode;          // Set, Add, Subtract, Paint
    int32 DensityDelta;          // relative edits: added to the generated density
    uint8 BrushMaterialID;
};
```

See [EDIT_LAYER.md](EDIT_LAYER.md).

## Enums (`VoxelCoreTypes.h`)

```cpp
enum class EWorldMode : uint8 { InfinitePlane, SphericalPlanet, IslandBowl };
enum class EMeshingMode : uint8 { Cubic, MarchingCubes, DualContouring };
enum class EEditMode : uint8 { Set, Add, Subtract, Paint };
```

`EWorldMode`'s integers are the shaders' `VOXELGEN_WORLD_MODE_*` (static-asserted).

## Memory per chunk

- Voxel data: `ChunkSize`^3 x 4 bytes: 128 KB at 32^3, 1 MB at 64^3 while resident; a uniform chunk holds one value,
  a far-compressed one its compact form.
- Mesh data depends on the surface it holds; the renderer reports it (`voxel.Bench.Run`'s memory columns).

## Version fingerprints

Saved data carries versions so that data written by older code is refused or rebuilt rather than misread. The
fingerprint (`FVoxelWorldFingerprint`, `VoxelWorldModeTypes.h`) is built by `FVoxelWorldModeFactory::MakeFingerprint`,
read through `UVoxelChunkManager::GetWorldFingerprint`, completed by the save subsystem
(`UVoxelWorldSaveSubsystem::GetRunningFingerprint`) and compared by `UVoxelWorldSaveSubsystem::CheckCompatibility`
(`Matches`, `DescribeDifferences`): a mismatched save is refused and its file kept; a save from before fingerprints
(`GenerationVersion` 0) is loaded.

| Version | Lives in | Value | Covers | What bumps it | What a bump costs |
|---|---|---|---|---|---|
| `FVoxelWorldModeFactory::GenerationVersion` | `VoxelWorldModeFactory.h` | 3 | any change to the terrain a configuration generates, every mode (1 WM-A1 island origin, 2 G18 GPU caves, 3 MP4 one noise) | by hand, with the change | every save of every world refused |
| `FVoxelWorldModeFactory::PlanetGenerationVersion` | `VoxelWorldModeFactory.h` | 6 | planet-only terrain changes (2 Phase 1 ... 5 Phase 6 noise, 6 G43 kernel); written into the fingerprint's `Shape` as `PlanetGeneration=N` | by hand | planet saves refused |
| `FVoxelWorldFingerprint::Shape` | built in `VoxelWorldModeFactory.cpp` | `Name=Value` pairs joined by `;` | the world mode's shape (island shape, radius, size, falloff, centre, edge height; planet radius, terrain height and depth, centre, `PlanetGeneration`), `VoxelSize`, `ChunkSize`, `WorldOrigin`, `MeshingMode` and `NoiseSeed` (the seed in use, `WorldSeed` when the noise seed is 0); terrain tuning and the island seabed left out on purpose | a change to any of those fields | that world's saves refused |
| `FVoxelWorldFingerprint::Content` | filled by `UVoxelWorldSaveSubsystem::GetRunningFingerprint` | `Name=Value` pairs joined by `;` | what the records key on beyond the terrain: `POITypes=A,B,C`, the POI catalog's type order (`UPOIPlacementSubsystem::GetCatalogFingerprintTerm`), which keys the dungeon records | a reordered, added or removed POI type | that world's saves refused |
| the save key `<KeyPrefix>_<Map>_<WorldSeed>` | `UVoxelWorldSaveSubsystem::MakeSaveKey` / `GetSaveKey` | e.g. `World_VoxelDemo_12345` | which file a world reads; a runtime configuration override (`-VoxelConfig=`, `voxel.World.ConfigOverride`) appends the configuration's name | n/a | n/a (the fingerprint guards what the key does not) |
| `FVoxelWorldSaveDocument::Version` | `VoxelWorldSaveTypes.h` | 1 | the JSON document's layout | by hand | none: `LoadWorld` warns and loads anyway |
| `VOXEL_EDIT_FILE_VERSION` / `VOXEL_EDIT_FILE_MAGIC` | `VoxelEditManager.cpp` | 3 / `'VETI'` | the `.edits.bin` per-edit layout (2 added mode, delta and brush material; 3 dropped the original voxel, CR1) | by hand | `LoadEditsFromFile` reads 3 and 2 and refuses anything else |
| `FVoxelMapEditOverlay::FormatVersion` / `PlanetFormatVersion` | `VoxelMapEditOverlay.h` | 1 / 2 | the map edit overlays saved with the world (2: heights as altitudes on a planet) | by hand | older overlays dropped with a warning; edits show on the map again as their areas load |
| `TextureArrayBuildVersion` (in `TextureArrayBuildKey`) | `VoxelMaterialAtlasArrays.cpp` | 2 | how a material atlas's saved texture arrays were built (2: one slice per distinct texture, F2.12), with the array size and the source textures' package hashes | by hand | every atlas rebuilt and resaved (`Scripts/rebuild_atlas_arrays.py`), the LFS files change; until then the editor rebuilds stale arrays with a warning (`EnsureTextureArrays`), a cook fails on a missing array (`PreSave`) and `IsDataValid` errors |
| `FVoxelChunkCodec::FormatVersion` / `VoxelFormatVersion` | `VoxelChunkCodec.h` | 1 / 1 | the far-compression buffer header; in memory only | by hand | none (nothing persisted) |
| `FChunkDescriptor::ContentVersion` | `ChunkDescriptor.h` | a runtime counter | a chunk's voxel content (generation and edits; not water flags) | `SetVoxel`, `GetVoxelDataMutable`, the edit layer's bump | n/a; the seam registry and the snapshot and shell caches key on it |
