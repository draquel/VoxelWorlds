// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelEditTypes.h"
#include "VoxelEditManager.generated.h"

class UVoxelWorldConfiguration;

/**
 * Delegate fired when a chunk's edits are modified.
 * @param ChunkCoord The chunk that was edited
 * @param Source The source of the edit (Player, System, Editor)
 * @param EditCenter World position of the edit center (for targeted scatter removal)
 * @param EditRadius Radius of the edit brush (for targeted scatter removal); 0 when the edit is not a brush's (a batch of
 *                   single-voxel edits, undo, redo, a clear or a load), with EditCenter the zero vector. While it is
 *                   broadcast, UVoxelEditManager::GetBroadcastEditBounds gives the voxels the edit changed in the chunk.
 */
DECLARE_MULTICAST_DELEGATE_FourParams(FOnChunkEdited, const FIntVector& /*ChunkCoord*/, EEditSource /*Source*/, const FVector& /*EditCenter*/, float /*EditRadius*/);

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
 * - Command pattern for undo/redo support: each operation records, per voxel, the edit before and after it, so undo
 *   and redo restore the edit layer exactly (FVoxelEditChange); operations begun outside the history take no undo step
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
	 * @param bAddToUndoHistory False for edits that follow from others rather than being made, and for the world's own
	 *        generation: the operation is broadcast as any other but takes no undo step and leaves the redo stack as it is.
	 *        Water propagation's flood (world modes review G60): undoing the dig that let the water in puts the dig's voxels
	 *        back as they were, flood included, and redoing it floods again. Dungeon stamps, their entrances and campsites
	 *        (G68): a player's undo takes back the player's own last edit, never a POI, and the history does not hold their
	 *        hundreds of thousands of voxel changes (112 bytes each)
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	void BeginEditOperation(const FString& Description = TEXT("Edit"), bool bAddToUndoHistory = true);

	/**
	 * End the current edit operation and add it to undo stack (unless it began with bAddToUndoHistory false).
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
	 * Set the source of subsequent edit operations.
	 * This determines how systems like scatter respond to edits.
	 * @param Source The edit source (Player, System, Editor)
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	void SetEditSource(EEditSource Source) { CurrentEditSource = Source; }

	/**
	 * Get the current edit source.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	EEditSource GetEditSource() const { return CurrentEditSource; }

	/**
	 * Install an edit validator — a pluggable veto consulted per voxel (see IVoxelEditValidator).
	 * Non-owning, like SetTerrainConditioner: the game-level owner must outlive this manager or
	 * clear it. Null disables validation.
	 */
	void SetEditValidator(const class IVoxelEditValidator* Validator) { EditValidator = Validator; }

	/**
	 * Would the installed edit validator let an edit from Source change the voxel at WorldPos? Asked before an edit that
	 * takes time to happen (a cubic block's timed break, world modes review WM-C15), so a protected voxel is refused at the
	 * start rather than once the time has passed.
	 *
	 * @param WorldPos World-space position of the voxel
	 * @param Source   Who would edit it
	 * @return True when no validator is installed or it allows the edit
	 */
	bool CanEditAt(const FVector& WorldPos, EEditSource Source) const;

	/**
	 * Voxels skipped by the validator during the most recent ApplyEdit/ApplyBrushEdit call.
	 * Zero when no validator is installed. Lets callers/UI message "protected area" later.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	int32 GetLastRejectedEditCount() const { return LastRejectedEditCount; }

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
	 * Undo the last edit operation: every voxel it changed gets back the edit it had before (or none), in each voxel's own
	 * chunk. Edits made since outside the history (water propagation's) at those voxels go with it.
	 * @return True if undo was performed
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	bool Undo();

	/**
	 * Redo the last undone operation: every voxel it changed gets the edit it left (or none) again, and its chunks are
	 * broadcast with the source, brush centre and radius the operation had (an undo broadcasts System, no radius).
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
	 * Collect the coordinates of every chunk with a NON-EMPTY edit layer.
	 *
	 * Streaming consumes this to pin edited chunks resident: the LOD strategy's load/cull decisions
	 * derive from the ANALYTIC terrain, so a chunk whose only content is edits (e.g. a player build
	 * reaching into the all-air chunk above the surface band) never qualifies on its own — without
	 * the pin its edits would never generate, mesh, or cook.
	 *
	 * @param OutChunkCoords Filled with the edited chunk coordinates (reset first).
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	void GetEditedChunkCoords(TArray<FIntVector>& OutChunkCoords) const;

	/**
	 * Apply this chunk's edit layer to a voxel array in place (procedural -> edit-merged),
	 * matching the merge meshing performs. No-op if the chunk has no edits. Lets scatter
	 * classify surface points against the terrain the player actually sees, so digging a
	 * hole removes grass over it and carving into a hill reclassifies buried scatter.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param VoxelData Voxel array (chunk-local index order) mutated in place
	 */
	void ApplyEditsToVoxelData(const FIntVector& ChunkCoord, TArray<FVoxelData>& VoxelData) const;

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

	/**
	 * The chunk-local bounds (inclusive, 0..ChunkSize-1 on each axis) of the voxels the edit being broadcast changed in its
	 * chunk, for OnChunkEdited's listeners: which of the chunk's layers the edit reached, e.g. whether it reached a layer a
	 * neighbour's scatter extraction reads (world modes review G33). A bounding box, so it can include voxels the edit did
	 * not change. Valid only during an OnChunkEdited broadcast.
	 *
	 * @param OutMin Receives the lowest local coordinate on each axis
	 * @param OutMax Receives the highest local coordinate on each axis
	 * @return False outside a broadcast (the outputs are left unchanged)
	 */
	bool GetBroadcastEditBounds(FIntVector& OutMin, FIntVector& OutMax) const;

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

	/**
	 * Approximate memory of the undo and redo history in bytes: what voxel.Edit.UndoHistoryMB bounds (world modes review
	 * G68). Each operation records 112 bytes per voxel it changed (FVoxelEditChange).
	 * @return The history's bytes
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	int64 GetHistoryMemoryUsage() const;

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
	 * Apply a pre-constructed edit internally, tracking it in the current operation.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param LocalPos Local position within chunk
	 * @param Edit The edit to apply (with DensityDelta for relative edits)
	 */
	void ApplyEditInternal(
		const FIntVector& ChunkCoord,
		const FIntVector& LocalPos,
		const FVoxelEdit& Edit);

	/**
	 * Trim the undo stack, oldest operations first, while it holds more than MaxUndoHistory operations or the history is
	 * over voxel.Edit.UndoHistoryMB (G68). The newest operation is always kept, however large.
	 */
	void TrimUndoStack();

	/**
	 * Put an operation's voxels back as they were before it (its changes' Before, last change first: undo and cancel) or as
	 * it left them (their After, first change first: redo), each in its own chunk.
	 *
	 * @param Operation The operation
	 * @param bAsLeft True for the state the operation left (redo), false for the state before it (undo, cancel)
	 * @param OutChunkBounds Receives each chunk's restored voxels (chunk-local), for its OnChunkEdited broadcast
	 */
	void RestoreOperation(const FVoxelEditOperation& Operation, bool bAsLeft, TMap<FIntVector, FVoxelEditedVoxelBounds>& OutChunkBounds);

	/**
	 * Broadcast OnChunkEdited for one chunk with the voxels the edit changed there, which GetBroadcastEditBounds reports
	 * to the listeners for the broadcast's duration.
	 *
	 * @param ChunkCoord The edited chunk
	 * @param Source The edit's source
	 * @param EditCenter The brush's centre, or the zero vector
	 * @param EditRadius The brush's radius, or 0
	 * @param Bounds The voxels changed in the chunk (chunk-local)
	 */
	void BroadcastChunkEdited(const FIntVector& ChunkCoord, EEditSource Source, const FVector& EditCenter, float EditRadius,
		const FVoxelEditedVoxelBounds& Bounds);

protected:
	// ==================== Configuration ====================

	/** World configuration reference */
	UPROPERTY()
	TObjectPtr<UVoxelWorldConfiguration> Configuration;

	/** Whether the manager is initialized */
	bool bIsInitialized = false;

	/** Current edit source - determines how scatter and other systems respond */
	EEditSource CurrentEditSource = EEditSource::Player;

	/** Pluggable per-voxel edit veto (non-owning; see SetEditValidator). */
	const class IVoxelEditValidator* EditValidator = nullptr;

	/** Voxels the validator skipped in the most recent apply call. */
	int32 LastRejectedEditCount = 0;

	/**
	 * The current operation's brush centre in world space (for scatter removal). Set by ApplyBrushEdit, reset when the
	 * operation ends, so an operation of single-voxel edits (a POI's terraform, a dungeon stamp) reports none instead of
	 * the last brush's (world modes review G53).
	 */
	FVector CurrentEditCenter = FVector::ZeroVector;

	/** The current operation's brush radius (for scatter removal); 0 when it has none. Reset with CurrentEditCenter. */
	float CurrentEditRadius = 0.0f;

	/** The voxels each chunk's edits changed in the current operation (chunk-local), for its OnChunkEdited broadcasts. */
	TMap<FIntVector, FVoxelEditedVoxelBounds> OperationBounds;

	/** The voxels the chunk being broadcast had changed, while OnChunkEdited is broadcast (GetBroadcastEditBounds). */
	const FVoxelEditedVoxelBounds* ActiveBroadcastBounds = nullptr;

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

	/** Maximum number of operations in undo stack (the memory budget, voxel.Edit.UndoHistoryMB, may hold fewer: G68) */
	static constexpr int32 MaxUndoHistory = 100;

	/** Counter for generating unique operation IDs */
	uint64 NextOperationId = 1;
};
