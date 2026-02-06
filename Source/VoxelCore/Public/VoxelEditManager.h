// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelEditTypes.h"
#include "VoxelEditManager.generated.h"

class UVoxelWorldConfiguration;

/**
 * Delegate fired when a chunk's edits are modified.
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnChunkEdited, const FIntVector& /*ChunkCoord*/);

/**
 * Delegate fired when undo/redo state changes.
 */
DECLARE_MULTICAST_DELEGATE(FOnUndoRedoStateChanged);

/**
 * Voxel Edit Manager.
 *
 * Manages terrain modifications using an overlay architecture:
 * - Edits are stored separately from procedural voxel data
 * - Sparse TMap storage for memory efficiency
 * - Command pattern for undo/redo support
 * - Binary serialization for save/load
 *
 * The edit manager does not directly modify chunk voxel data.
 * Instead, the VoxelChunkManager merges edits during meshing.
 *
 * Thread Safety: Not thread-safe, must be accessed from game thread only.
 *
 * @see FChunkEditLayer
 * @see FVoxelEditOperation
 */
UCLASS(BlueprintType)
class VOXELCORE_API UVoxelEditManager : public UObject
{
	GENERATED_BODY()

public:
	UVoxelEditManager();

	// ==================== Initialization ====================

	/**
	 * Initialize the edit manager with world configuration.
	 * @param Config World configuration for coordinate conversion
	 */
	void Initialize(UVoxelWorldConfiguration* Config);

	/**
	 * Shutdown and cleanup all resources.
	 */
	void Shutdown();

	/**
	 * Check if manager is initialized.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	bool IsInitialized() const { return bIsInitialized; }

	// ==================== Edit Operations ====================

	/**
	 * Begin a new edit operation (for undo/redo grouping).
	 *
	 * All edits applied between BeginEditOperation and EndEditOperation
	 * will be grouped into a single undo/redo operation.
	 *
	 * @param Description Human-readable description of the operation
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	void BeginEditOperation(const FString& Description = TEXT("Edit"));

	/**
	 * End the current edit operation and add it to undo stack.
	 *
	 * If no edits were applied, the operation is discarded.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	void EndEditOperation();

	/**
	 * Cancel the current edit operation without adding to undo stack.
	 *
	 * Reverts all edits made since BeginEditOperation.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	void CancelEditOperation();

	/**
	 * Check if an edit operation is currently in progress.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	bool IsEditOperationInProgress() const { return CurrentOperation.IsValid(); }

	/**
	 * Apply a single voxel edit at a world position.
	 *
	 * @param WorldPos World-space position to edit
	 * @param NewData New voxel data to set
	 * @param Mode Edit mode (Set, Add, Subtract, Paint, Smooth)
	 * @return True if edit was applied successfully
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	bool ApplyEdit(const FVector& WorldPos, const FVoxelData& NewData, EEditMode Mode);

	/**
	 * Apply a brush edit at a world position.
	 *
	 * Affects multiple voxels within the brush radius.
	 *
	 * @param WorldPos Center of the brush in world space
	 * @param Brush Brush parameters (shape, size, strength, etc.)
	 * @param Mode Edit mode (Set, Add, Subtract, Paint, Smooth)
	 * @return Number of voxels modified
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	int32 ApplyBrushEdit(const FVector& WorldPos, const FVoxelBrushParams& Brush, EEditMode Mode);

	// ==================== Undo/Redo ====================

	/**
	 * Check if undo is available.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	bool CanUndo() const { return UndoStack.Num() > 0; }

	/**
	 * Check if redo is available.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	bool CanRedo() const { return RedoStack.Num() > 0; }

	/**
	 * Undo the last edit operation.
	 * @return True if undo was performed
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	bool Undo();

	/**
	 * Redo the last undone operation.
	 * @return True if redo was performed
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	bool Redo();

	/**
	 * Clear all undo/redo history.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	void ClearHistory();

	/**
	 * Get number of operations in undo stack.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	int32 GetUndoCount() const { return UndoStack.Num(); }

	/**
	 * Get number of operations in redo stack.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	int32 GetRedoCount() const { return RedoStack.Num(); }

	// ==================== Edit Layer Access ====================

	/**
	 * Get or create an edit layer for a chunk.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return Pointer to the edit layer
	 */
	FChunkEditLayer* GetOrCreateEditLayer(const FIntVector& ChunkCoord);

	/**
	 * Get an existing edit layer for a chunk.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return Pointer to the edit layer, or nullptr if none exists
	 */
	const FChunkEditLayer* GetEditLayer(const FIntVector& ChunkCoord) const;

	/**
	 * Check if a chunk has any edits.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return True if chunk has edits
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	bool ChunkHasEdits(const FIntVector& ChunkCoord) const;

	/**
	 * Get total number of chunks with edits.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	int32 GetEditedChunkCount() const { return EditLayers.Num(); }

	/**
	 * Get total number of individual edits across all chunks.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	int32 GetTotalEditCount() const;

	/**
	 * Clear all edits for a specific chunk.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return True if any edits were cleared
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	bool ClearChunkEdits(const FIntVector& ChunkCoord);

	/**
	 * Clear all edits from all chunks.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	void ClearAllEdits();

	// ==================== Serialization ====================

	/**
	 * Save all edits to a binary file.
	 *
	 * @param FilePath Full path to the save file
	 * @return True if save was successful
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	bool SaveEditsToFile(const FString& FilePath);

	/**
	 * Load edits from a binary file.
	 *
	 * Existing edits are cleared before loading.
	 *
	 * @param FilePath Full path to the load file
	 * @return True if load was successful
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	bool LoadEditsFromFile(const FString& FilePath);

	// ==================== Events ====================

	/** Called when a chunk's edits are modified */
	FOnChunkEdited OnChunkEdited;

	/** Called when undo/redo state changes */
	FOnUndoRedoStateChanged OnUndoRedoStateChanged;

	// ==================== Debug ====================

	/**
	 * Get debug statistics string.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	FString GetDebugStats() const;

	/**
	 * Get approximate total memory usage in bytes.
	 */
	SIZE_T GetMemoryUsage() const;

protected:
	// ==================== Internal Methods ====================

	/**
	 * Convert world position to chunk coordinate.
	 */
	FIntVector WorldToChunkCoord(const FVector& WorldPos) const;

	/**
	 * Convert world position to local position within a chunk.
	 */
	FIntVector WorldToLocalPos(const FVector& WorldPos, const FIntVector& ChunkCoord) const;

	/**
	 * Convert chunk coord + local pos back to world position.
	 */
	FVector LocalToWorldPos(const FIntVector& ChunkCoord, const FIntVector& LocalPos) const;

	/**
	 * Apply a single edit internally, tracking it in the current operation.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param LocalPos Local position within chunk
	 * @param NewData New voxel data
	 * @param OriginalData Original voxel data (for undo)
	 * @param Mode Edit mode
	 */
	void ApplyEditInternal(
		const FIntVector& ChunkCoord,
		const FIntVector& LocalPos,
		const FVoxelData& NewData,
		const FVoxelData& OriginalData,
		EEditMode Mode);

	/**
	 * Get the original voxel data at a position (from edit layer or procedural).
	 * For now, returns air if no edit exists (procedural data not accessible here).
	 */
	FVoxelData GetOriginalVoxelData(const FIntVector& ChunkCoord, const FIntVector& LocalPos) const;

	/**
	 * Trim undo stack if it exceeds maximum size.
	 */
	void TrimUndoStack();

protected:
	// ==================== Configuration ====================

	/** World configuration reference */
	UPROPERTY()
	TObjectPtr<UVoxelWorldConfiguration> Configuration;

	/** Whether the manager is initialized */
	bool bIsInitialized = false;

	// ==================== Edit Storage ====================

	/** Map of chunk coordinates to their edit layers */
	UPROPERTY()
	TMap<FIntVector, FChunkEditLayer> EditLayers;

	// ==================== Undo/Redo ====================

	/** Current operation being built (between Begin/End) */
	TUniquePtr<FVoxelEditOperation> CurrentOperation;

	/** Undo stack (most recent at end) */
	UPROPERTY()
	TArray<FVoxelEditOperation> UndoStack;

	/** Redo stack (most recent at end) */
	UPROPERTY()
	TArray<FVoxelEditOperation> RedoStack;

	/** Maximum number of operations in undo stack */
	static constexpr int32 MaxUndoHistory = 100;

	/** Counter for generating unique operation IDs */
	uint64 NextOperationId = 1;
};
