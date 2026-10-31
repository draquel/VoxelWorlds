# Edit Layer System

**Module**: VoxelCore (VoxelEditTypes.h, VoxelEditManager.h/cpp)
**Dependencies**: VoxelCore, VoxelStreaming
**Status**: ✅ COMPLETE (Phase 6)

## Overview

The edit layer allows runtime modification of voxel terrain with add/subtract/paint operations, undo/redo support, and serialization.

> **Note**: This document contains the original design specification. For actual implementation details, see:
> - [IMPLEMENTATION_PHASES.md](IMPLEMENTATION_PHASES.md) - "Notes on Edit Layer Implementation" section
> - Source files: `VoxelCore/Public/VoxelEditTypes.h`, `VoxelCore/Public/VoxelEditManager.h`

## Implementation Highlights

The actual implementation uses an **overlay architecture**:
- Edits stored in `FChunkEditLayer` using sparse `TMap<int32, FVoxelEdit>` (memory-efficient)
- Edits merged at mesh time via `ApplyToProceduralData()` (not stored in procedural data)
- Relative edits with `DensityDelta` and `BrushMaterialID` for accumulation
- Binary serialization format v2 with magic number `VETI`
- Input-based testing via the game module's `AVoxelDemoActor` with mouse/keyboard controls
- Discrete editing mode (`bUseDiscreteEditing`) for single-block operations

### How a new edit combines with the edit already on a voxel

`UVoxelEditManager::ApplyEditInternal` keeps one edit per voxel. When a voxel already has one:

| Existing edit | New edit | Result |
|---------------|----------|--------|
| Add / Subtract | Add / Subtract | One relative edit with the summed delta; a sum of 0 removes the edit (or becomes a Paint when the new Add carries a material) |
| Set / Smooth (absolute) | Add / Subtract / Paint | A Set whose voxel is the new edit applied to the existing voxel: a dig under a POI pad takes density from the pad, not from the procedural terrain under it. Undo restores the pad's Set (below) |
| anything | Set / Smooth | The new absolute edit replaces it |
| Add / Subtract | Paint | The Paint replaces it (the delta is lost) |

Until 2026-10-14 a relative edit on a Set voxel replaced the Set, so the voxel fell back to procedural data minus
the delta; one dig under a pad standing over air removed the aimed voxel and its four face neighbours, which had
only received the brush falloff (WORLD_MODES_REVIEW_PLAN.md G2). Tests: `VoxelWorlds.Edit.Composition.*`.

### What `OnChunkEdited` tells its listeners

`OnChunkEdited(ChunkCoord, Source, EditCenter, EditRadius)` fires once per edited chunk when an operation ends (and for
an undo, redo, clear or load). The centre and radius are the operation's brush (`ApplyBrushEdit`), and a redo reports
the operation's again; an operation of single-voxel edits (a POI's terraform, a dungeon stamp, water propagation), an
undo, a clear and a load report the zero vector and 0. While it is broadcast, `GetBroadcastEditBounds` gives the
chunk-local bounds of the voxels the edit changed in that chunk, so a listener can tell which layers it reached (scatter's readers, world modes review G33),
or which voxels to look at (water propagation scans them for voxels the edit opened to the sea, G58; the movement
component's embed check, G55).
Until 2026-10-20 the centre and radius stayed set after a brush, so every later operation of single-voxel edits reported
the last brush's (G53): navigation invalidated around the old dig instead of the terraform, and collision's edge and
corner re-cook (G32) judged the wrong sphere. Tests: `VoxelWorlds.Scatter.EditReaders.Rule`.

### What an edit re-meshes

The chunk manager's handler for `OnChunkEdited` re-meshes the chunk the edit changed and restales its seams
(`MarkChunkDirty`), and re-cooks the collision of the chunk and its face neighbours (a cook reads its 26 neighbours'
voxels). A chunk mesh is built from the chunk's own voxels and edit layer, and a seam from its participants', so nothing
else needs a re-mesh. Until 2026-10-20 the handler also re-meshed the six face neighbours, which rebuilt the same meshes
and all their seams (world modes review G56; `voxel.Edit.NeighbourRemesh 1` restores it for A/B). An edit that reaches a
chunk already on its way to a mesh restales its seams too, and if that mesh is on a worker (built before the edit) the
chunk re-meshes when it lands (G57; before, both stayed stale). `voxel.Edit.MeshWork [reset]` logs the meshing work since
a reset. Tests: `VoxelWorlds.Streaming.EditRemesh.Plane`, `.Planet`.

### Undo and redo

Every operation between `BeginEditOperation` and `EndEditOperation` (or the one `ApplyEdit` and `ApplyBrushEdit` start
themselves) is one undo step of at most `MaxUndoHistory` (100). It records, for each voxel it changed, the voxel's chunk
and the whole edit the voxel held before and after (`FVoxelEditChange`, 112 bytes, none for the generated voxel), and when
it ends the source, brush centre and radius it broadcast. The history (undo and redo) also keeps within
`voxel.Edit.UndoHistoryMB` (32 MB; 0 = no budget): past it the oldest operations are dropped, the newest always kept
(`GetHistoryMemoryUsage`; world modes review G68).

- **Undo** puts each voxel's previous edit back, or removes the edit when there was none, last change first, in the
  voxel's own chunk; it broadcasts each chunk it changed as a System edit with no radius (the restored voxels re-mesh,
  re-cook and re-extract their scatter, the chunks that read them too, and water propagation scans them).
- **Redo** puts each voxel's edit back as the operation left it, first change first, and broadcasts each chunk with the
  operation's own source, centre and radius, so a player brush clears scatter in its sphere again and a system edit
  re-extracts the chunks that read it.
- **Cancel** (`CancelEditOperation`) restores the open operation's voxels as an undo does.
- An operation begun with `bAddToUndoHistory` false takes no undo step and leaves the redo stack as it is; it is
  broadcast like any other. Water propagation's flood is one (it follows from the edit that let the water in): undoing
  that edit puts its voxels back as they were before it, the flood there with them, and its redo floods again. Water the
  flood put beyond the edit's voxels stays.
  The world's own generation is outside it too (G68): dungeon stamps (`UDungeonVoxelStamper`) and their entrances
  (`UDungeonEntranceStitcher`; the runtime campsite levelling that was the third went in code review CR1, D-CR17), so
  an undo takes back the player's own last edit, never a POI.Until 2026-10-22 they were undo steps: on `Review_Planet_L` four POI operations
  around a site held 94 MB of history, and an undo took back the last of them. As with the flood, undoing a player's
  edit made before a stamp puts back that edit's voxels as they were before it, the stamp's there with them.
- A clear (`ClearChunkEdits`, `ClearAllEdits`) is outside the history; a load (`LoadEditsFromFile`) clears it.
- `FVoxelEdit::OriginalData` deleted in CR1 (CORE-12): the edits file is version 3 without it (version 2 still loads).

Until 2026-10-20 the record was the operation's list of edits with their `OriginalData`: undo took the first of the
operation's chunks holding an edit at the voxel's local index and set `OriginalData` on it (removing it when that was
air), redo wrote every edit into the operation's first chunk and broadcast only that chunk with the current source and no
radius. So a brush across a chunk face came back in one chunk, a voxel's previous relative edit (a dig under a dig), a
previous Set to air and an edit removed by cancelling deltas did not come back, and a system edit redone as a player's
re-extracted no scatter. Water propagation's flood was an undo step of its own, and its undo removed the dig at the voxels
it had reached (world modes review G60, WORLD_MODES_REVIEW_PLAN.md §19.17). Tests: `VoxelWorlds.Edit.UndoRedo.Records`,
`VoxelWorlds.Streaming.WaterPropagation.Undo.Plane`, `.Planet`; `Streaming.EditRemesh.*` edits 6 and 7 and
`Scatter.EditReaders.{Plane,Planet}` undo and redo too.

## Original Design Specification

## Edit Operations

```cpp
/**
 * Edit Mode
 */
enum class EEditMode : uint8
{
    Set,        // Set voxel value directly
    Add,        // Add material (additive)
    Subtract,   // Remove material (subtractive)
    Paint,      // Change material only
    Smooth      // Smooth terrain
};

/**
 * Voxel Edit
 * 
 * Single edit operation.
 */
struct FVoxelEdit
{
    /** World space position */
    FVector WorldPosition;
    
    /** Edit radius (cm) */
    float Radius;
    
    /** Edit strength (0-1) */
    float Strength;
    
    /** Edit mode */
    EEditMode Mode;
    
    /** Material ID (for Paint mode) */
    uint8 MaterialID;
    
    /** Density change (for Add/Subtract) */
    int8 DensityDelta;

    // Timestamp deleted in CR1 (CORE-13)
};
```

## Edit Layer Storage

Edits are stored per-chunk as sparse modifications:

```cpp
/**
 * Chunk Edit Layer
 * 
 * Stores edits for a single chunk.
 */
struct FChunkEditLayer
{
    /** Chunk coordinate */
    FIntVector ChunkCoord;
    
    /** Modified voxels (sparse storage) */
    TMap<FIntVector, FVoxelData> ModifiedVoxels;
    
    /** Edit history for undo/redo */
    TArray<FVoxelEdit> EditHistory;
    
    /** Current history index */
    int32 HistoryIndex;
    
    /** Is dirty (needs remeshing)? */
    bool bIsDirty;
};
```

## Edit Application

### CPU-Side Edit

```cpp
void UVoxelEditManager::ApplyEdit(const FVoxelEdit& Edit)
{
    // Find affected chunks
    TArray<FIntVector> AffectedChunks = GetChunksInRadius(Edit.WorldPosition, Edit.Radius);
    
    for (FIntVector ChunkCoord : AffectedChunks)
    {
        // Get or create edit layer
        FChunkEditLayer& EditLayer = GetOrCreateEditLayer(ChunkCoord);
        
        // Apply edit to voxels in chunk
        ApplyEditToChunk(Edit, ChunkCoord, EditLayer);
        
        // Mark dirty for remeshing
        EditLayer.bIsDirty = true;
        
        // Add to history
        EditLayer.EditHistory.Add(Edit);
        EditLayer.HistoryIndex = EditLayer.EditHistory.Num() - 1;
        
        // Request chunk remesh
        ChunkManager->RequestChunkRemesh(ChunkCoord);
    }
}

void UVoxelEditManager::ApplyEditToChunk(
    const FVoxelEdit& Edit,
    FIntVector ChunkCoord,
    FChunkEditLayer& EditLayer)
{
    FBox ChunkBounds = GetChunkBounds(ChunkCoord);
    FVector LocalEditPos = Edit.WorldPosition - ChunkBounds.Min;
    
    // Iterate voxels in edit radius
    int32 VoxelRadius = FMath::CeilToInt(Edit.Radius / VoxelSize);
    
    for (int32 x = -VoxelRadius; x <= VoxelRadius; ++x)
    {
        for (int32 y = -VoxelRadius; y <= VoxelRadius; ++y)
        {
            for (int32 z = -VoxelRadius; z <= VoxelRadius; ++z)
            {
                FIntVector LocalVoxel(x, y, z);
                FVector VoxelWorldPos = LocalEditPos + FVector(LocalVoxel) * VoxelSize;
                
                float Distance = FVector::Dist(VoxelWorldPos, LocalEditPos);
                if (Distance > Edit.Radius) continue;
                
                // Calculate edit strength with falloff
                float Falloff = 1.0f - (Distance / Edit.Radius);
                float EffectiveStrength = Edit.Strength * Falloff;
                
                // Get current voxel data
                FVoxelData CurrentData = GetVoxelData(ChunkCoord, LocalVoxel);
                
                // Apply edit based on mode
                FVoxelData NewData = CurrentData;
                
                switch (Edit.Mode)
                {
                    case EEditMode::Add:
                        NewData.Density = FMath::Clamp(
                            CurrentData.Density + (int32)(Edit.DensityDelta * EffectiveStrength),
                            0, 255
                        );
                        break;
                        
                    case EEditMode::Subtract:
                        NewData.Density = FMath::Clamp(
                            CurrentData.Density - (int32)(Edit.DensityDelta * EffectiveStrength),
                            0, 255
                        );
                        break;
                        
                    case EEditMode::Paint:
                        NewData.MaterialID = Edit.MaterialID;
                        break;
                        
                    case EEditMode::Set:
                        NewData.Density = Edit.DensityDelta;
                        NewData.MaterialID = Edit.MaterialID;
                        break;
                        
                    // EEditMode::Smooth deleted in CR1 (CORE-11)
                }
                
                // Store modified voxel
                EditLayer.ModifiedVoxels.Add(LocalVoxel, NewData);
            }
        }
    }
}
```

### GPU-Side Edit (Async)

```cpp
void UVoxelEditManager::ApplyEditGPU(const FVoxelEdit& Edit)
{
    FRDGBuilder GraphBuilder(RHICmdList);
    
    // Upload edit parameters
    FEditParametersUniform Params;
    Params.WorldPosition = Edit.WorldPosition;
    Params.Radius = Edit.Radius;
    Params.Strength = Edit.Strength;
    Params.Mode = (uint32)Edit.Mode;
    Params.MaterialID = Edit.MaterialID;
    Params.DensityDelta = Edit.DensityDelta;
    
    // Dispatch edit shader
    TShaderMapRef<FVoxelEditCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("ApplyVoxelEdit"),
        ComputeShader,
        &Params,
        GroupCount
    );
    
    GraphBuilder.Execute();
}
```

## Undo/Redo System

```cpp
void UVoxelEditManager::Undo()
{
    for (auto& Pair : EditLayers)
    {
        FChunkEditLayer& EditLayer = Pair.Value;
        
        if (EditLayer.HistoryIndex > 0)
        {
            --EditLayer.HistoryIndex;
            
            // Rebuild chunk from edits up to HistoryIndex
            RebuildChunkFromHistory(Pair.Key, EditLayer);
            
            EditLayer.bIsDirty = true;
            ChunkManager->RequestChunkRemesh(Pair.Key);
        }
    }
}

void UVoxelEditManager::Redo()
{
    for (auto& Pair : EditLayers)
    {
        FChunkEditLayer& EditLayer = Pair.Value;
        
        if (EditLayer.HistoryIndex < EditLayer.EditHistory.Num() - 1)
        {
            ++EditLayer.HistoryIndex;
            
            // Apply next edit in history
            ApplyEdit(EditLayer.EditHistory[EditLayer.HistoryIndex]);
            
            EditLayer.bIsDirty = true;
        }
    }
}
```

## Serialization

```cpp
/**
 * Save edit layer to disk.
 */
void UVoxelEditManager::SaveEditLayer(const FString& Filename)
{
    FArchive* Ar = IFileManager::Get().CreateFileWriter(*Filename);
    
    // Write header
    int32 Version = 1;
    int32 ChunkCount = EditLayers.Num();
    *Ar << Version;
    *Ar << ChunkCount;
    
    // Write each chunk's edits
    for (auto& Pair : EditLayers)
    {
        FIntVector ChunkCoord = Pair.Key;
        FChunkEditLayer& EditLayer = Pair.Value;
        
        *Ar << ChunkCoord;
        *Ar << EditLayer.ModifiedVoxels.Num();
        
        for (auto& VoxelPair : EditLayer.ModifiedVoxels)
        {
            *Ar << VoxelPair.Key;
            *Ar << VoxelPair.Value;
        }
    }
    
    Ar->Close();
    delete Ar;
}

/**
 * Load edit layer from disk.
 */
void UVoxelEditManager::LoadEditLayer(const FString& Filename)
{
    FArchive* Ar = IFileManager::Get().CreateFileReader(*Filename);
    
    int32 Version, ChunkCount;
    *Ar << Version;
    *Ar << ChunkCount;
    
    for (int32 i = 0; i < ChunkCount; ++i)
    {
        FIntVector ChunkCoord;
        int32 VoxelCount;
        *Ar << ChunkCoord;
        *Ar << VoxelCount;
        
        FChunkEditLayer& EditLayer = EditLayers.Add(ChunkCoord);
        EditLayer.ChunkCoord = ChunkCoord;
        
        for (int32 j = 0; j < VoxelCount; ++j)
        {
            FIntVector LocalVoxel;
            FVoxelData VoxelData;
            *Ar << LocalVoxel;
            *Ar << VoxelData;
            
            EditLayer.ModifiedVoxels.Add(LocalVoxel, VoxelData);
        }
        
        EditLayer.bIsDirty = true;
    }
    
    Ar->Close();
    delete Ar;
    
    // Remesh all affected chunks
    for (auto& Pair : EditLayers)
    {
        ChunkManager->RequestChunkRemesh(Pair.Key);
    }
}
```

## Edit Tools

### Sphere Brush

```cpp
FVoxelEdit CreateSphereBrush(FVector WorldPos, float Radius, EEditMode Mode, float Strength)
{
    FVoxelEdit Edit;
    Edit.WorldPosition = WorldPos;
    Edit.Radius = Radius;
    Edit.Strength = Strength;
    Edit.Mode = Mode;
    Edit.DensityDelta = (Mode == EEditMode::Add) ? 50 : -50;
    return Edit;
}
```

### Cylinder Brush Axis (spherical Phase 4, SP-F8)

`FVoxelBrushParams::Axis` is the Cylinder shape's axis. Zero (the default) means the world's local up at the brush
centre: +Z on heightmap worlds (the old arithmetic, the same voxels: `Edit.Brush.CylinderAxis`) and the radial direction
on a planet, so a dig there is a shaft along local up. A tilted axis widens the voxel box the brush visits by √2.

### Smooth Tool

```cpp
uint8 SmoothVoxel(FIntVector ChunkCoord, FIntVector LocalVoxel, float Strength)
{
    // Sample 3×3×3 neighborhood
    int32 Sum = 0;
    int32 Count = 0;
    
    for (int32 dx = -1; dx <= 1; ++dx)
    {
        for (int32 dy = -1; dy <= 1; ++dy)
        {
            for (int32 dz = -1; dz <= 1; ++dz)
            {
                FVoxelData Neighbor = GetVoxelData(ChunkCoord, LocalVoxel + FIntVector(dx, dy, dz));
                Sum += Neighbor.Density;
                ++Count;
            }
        }
    }
    
    uint8 Average = Sum / Count;
    uint8 Current = GetVoxelData(ChunkCoord, LocalVoxel).Density;
    
    return FMath::Lerp((float)Current, (float)Average, Strength);
}
```

## Integration Points

- **ChunkManager**: Triggers remeshing when edits occur
- **MaterialSystem**: Paint mode changes materials
- **Collision**: Updates collision after edits
- **Networking**: Replicate edits to clients (multiplayer)

## Next Steps

1. Implement UVoxelEditManager
2. Create edit tools (sphere, smooth, paint)
3. Add undo/redo system
4. Implement serialization
5. Create editor UI for editing
6. Add multiplayer replication (optional)

