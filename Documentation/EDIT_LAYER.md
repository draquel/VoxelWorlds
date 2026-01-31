# Edit Layer System

**Module**: VoxelEditing  
**Dependencies**: VoxelCore, VoxelStreaming

## Overview

The edit layer allows runtime modification of voxel terrain with add/subtract/paint operations, undo/redo support, and serialization.

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
    
    /** Timestamp */
    double Timestamp;
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
                        
                    case EEditMode::Smooth:
                        NewData.Density = SmoothVoxel(ChunkCoord, LocalVoxel, EffectiveStrength);
                        break;
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

