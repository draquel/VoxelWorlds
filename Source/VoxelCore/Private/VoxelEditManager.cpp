// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelEditManager.h"
#include "IVoxelEditValidator.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelCoordinates.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Optional.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelEdit, Log, All);

static TAutoConsoleVariable<float> CVarVoxelEditUndoHistoryMB(
	TEXT("voxel.Edit.UndoHistoryMB"),
	32.0f,
	TEXT("Memory budget of the edit manager's undo and redo history, in MB (world modes review G68): past it the oldest ")
	TEXT("operations are dropped, down to the newest, which is always kept. 0 = no budget (the 100-operation limit only). ")
	TEXT("An operation records 112 bytes per voxel it changed (G60): a player brush of radius 8 voxels about 0.25 MB."),
	ECVF_Default);

// File format version for binary serialization
// Version 2: EditMode, DensityDelta, BrushMaterialID, NewData, then the pre-G60 OriginalData (4 bytes, skipped on read)
// Version 3: without OriginalData (CR1, CORE-12). Version 1 (NewData/OriginalData only) is no longer read.
static constexpr uint32 VOXEL_EDIT_FILE_VERSION = 3;
static constexpr uint32 VOXEL_EDIT_FILE_MAGIC = 0x56455449; // "VETI" - Voxel Edit

UVoxelEditManager::UVoxelEditManager()
{
}

// ==================== Initialization ====================

void UVoxelEditManager::Initialize(UVoxelWorldConfiguration* Config)
{
	if (bIsInitialized)
	{
		UE_LOG(LogVoxelEdit, Warning, TEXT("VoxelEditManager::Initialize called when already initialized"));
		Shutdown();
	}

	if (!Config)
	{
		UE_LOG(LogVoxelEdit, Error, TEXT("VoxelEditManager::Initialize called with null configuration"));
		return;
	}

	Configuration = Config;

	// Clear any existing state
	EditLayers.Empty();
	UndoStack.Empty();
	RedoStack.Empty();
	CurrentOperation.Reset();
	NextOperationId = 1;

	bIsInitialized = true;

	UE_LOG(LogVoxelEdit, Log, TEXT("VoxelEditManager initialized (ChunkSize=%d, VoxelSize=%.1f)"),
		Configuration->ChunkSize, Configuration->VoxelSize);
}

void UVoxelEditManager::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	// Cancel any in-progress operation
	if (CurrentOperation.IsValid())
	{
		CurrentOperation.Reset();
	}

	// Clear all data
	EditLayers.Empty();
	UndoStack.Empty();
	RedoStack.Empty();

	Configuration = nullptr;
	bIsInitialized = false;

	UE_LOG(LogVoxelEdit, Log, TEXT("VoxelEditManager shutdown"));
}

// ==================== Edit Operations ====================

void UVoxelEditManager::BeginEditOperation(const FString& Description, bool bAddToUndoHistory)
{
	if (!bIsInitialized)
	{
		UE_LOG(LogVoxelEdit, Warning, TEXT("BeginEditOperation called on uninitialized manager"));
		return;
	}

	if (CurrentOperation.IsValid())
	{
		UE_LOG(LogVoxelEdit, Warning, TEXT("BeginEditOperation called while operation already in progress - ending previous"));
		EndEditOperation();
	}

	CurrentOperation = MakeUnique<FVoxelEditOperation>(NextOperationId++, Description);
	CurrentOperation->bAddToUndoHistory = bAddToUndoHistory;
	OperationBounds.Reset();

	UE_LOG(LogVoxelEdit, Verbose, TEXT("Edit operation started: '%s' (ID=%llu)"),
		*Description, CurrentOperation->OperationId);
}

void UVoxelEditManager::EndEditOperation()
{
	if (!CurrentOperation.IsValid())
	{
		UE_LOG(LogVoxelEdit, Warning, TEXT("EndEditOperation called with no operation in progress"));
		return;
	}

	if (CurrentOperation->IsEmpty())
	{
		UE_LOG(LogVoxelEdit, Verbose, TEXT("Edit operation '%s' had no edits - discarding"),
			*CurrentOperation->Description);
		CurrentOperation.Reset();
		OperationBounds.Reset();
		CurrentEditCenter = FVector::ZeroVector;
		CurrentEditRadius = 0.0f;
		return;
	}

	// Capture the affected chunks before moving the operation. ApplyEditInternal defers its
	// per-voxel notification during a batch op; we fire ONE OnChunkEdited per affected chunk here
	// (deduped set), so the chunk-manager handler's scatter-clear + dirty cascade runs a handful of
	// times instead of once per edited voxel (the max-brush freeze).
	const TArray<FIntVector> AffectedChunks = CurrentOperation->AffectedChunks;
	const bool bAddToUndoHistory = CurrentOperation->bAddToUndoHistory;

	// What the broadcasts below carry, for a redo to broadcast again (G60).
	CurrentOperation->Source = CurrentEditSource;
	CurrentOperation->EditCenter = CurrentEditCenter;
	CurrentOperation->EditRadius = CurrentEditRadius;

	if (bAddToUndoHistory)
	{
		// Clear redo stack when new edits are made
		RedoStack.Empty();

		// Add to undo stack
		UndoStack.Add(MoveTemp(*CurrentOperation));

		// Trim if necessary
		TrimUndoStack();
	}
	// Else edits that follow from others (water propagation's flood): no undo step, and the redo stack stays. Undoing the
	// operation that caused them restores its voxels as they were before it, these edits there included (G60).
	CurrentOperation.Reset();

	// Batched edit notification: one broadcast per affected chunk (operation-level source/center/radius), with the voxels
	// the operation changed in that chunk.
	const TMap<FIntVector, FVoxelEditedVoxelBounds> Bounds = MoveTemp(OperationBounds);
	OperationBounds.Reset();
	for (const FIntVector& ChunkCoord : AffectedChunks)
	{
		const FVoxelEditedVoxelBounds* ChunkBounds = Bounds.Find(ChunkCoord);
		BroadcastChunkEdited(ChunkCoord, CurrentEditSource, CurrentEditCenter, CurrentEditRadius,
			ChunkBounds ? *ChunkBounds : FVoxelEditedVoxelBounds());
	}

	// The centre and radius belong to this operation's brush. Left set, the next operation of single-voxel edits (a POI's
	// terraform, a dungeon stamp, water propagation) broadcast them as its own (world modes review G53).
	CurrentEditCenter = FVector::ZeroVector;
	CurrentEditRadius = 0.0f;

	// Notify listeners
	if (bAddToUndoHistory)
	{
		OnUndoRedoStateChanged.Broadcast();
	}

	UE_LOG(LogVoxelEdit, Verbose, TEXT("Edit operation completed (UndoStack=%d%s)"), UndoStack.Num(),
		bAddToUndoHistory ? TEXT("") : TEXT(", outside the history"));
}

void UVoxelEditManager::CancelEditOperation()
{
	if (!CurrentOperation.IsValid())
	{
		UE_LOG(LogVoxelEdit, Warning, TEXT("CancelEditOperation called with no operation in progress"));
		return;
	}

	// Revert all edits in the current operation: every voxel as it was before it, in its own chunk
	TMap<FIntVector, FVoxelEditedVoxelBounds> Reverted;
	RestoreOperation(*CurrentOperation, /*bAsLeft=*/false, Reverted);

	// Notify affected chunks (use current source - cancelling reverts player's work)
	for (const TPair<FIntVector, FVoxelEditedVoxelBounds>& Pair : Reverted)
	{
		BroadcastChunkEdited(Pair.Key, CurrentEditSource, CurrentEditCenter, CurrentEditRadius, Pair.Value);
	}

	UE_LOG(LogVoxelEdit, Log, TEXT("Edit operation '%s' cancelled (%d edits reverted)"),
		*CurrentOperation->Description, CurrentOperation->GetEditCount());

	CurrentOperation.Reset();
	OperationBounds.Reset();
	CurrentEditCenter = FVector::ZeroVector;
	CurrentEditRadius = 0.0f;
}

bool UVoxelEditManager::CanEditAt(const FVector& WorldPos, EEditSource Source) const
{
	return !EditValidator || EditValidator->CanApplyEdit(WorldPos, Source);
}

bool UVoxelEditManager::ApplyEdit(const FVector& WorldPos, const FVoxelData& NewData, EEditMode Mode)
{
	if (!bIsInitialized || !Configuration)
	{
		return false;
	}

	// Edit-protection veto (see IVoxelEditValidator).
	LastRejectedEditCount = 0;
	if (EditValidator && !EditValidator->CanApplyEdit(WorldPos, CurrentEditSource))
	{
		LastRejectedEditCount = 1;
		UE_LOG(LogVoxelEdit, Verbose, TEXT("ApplyEdit at (%.0f, %.0f, %.0f) rejected by edit protection"),
			WorldPos.X, WorldPos.Y, WorldPos.Z);
		return false;
	}

	// Auto-start operation if none in progress
	const bool bAutoOperation = !CurrentOperation.IsValid();
	if (bAutoOperation)
	{
		BeginEditOperation(TEXT("Single Edit"));
	}

	// Convert world position to chunk and local coordinates
	const FIntVector ChunkCoord = WorldToChunkCoord(WorldPos);
	const FIntVector LocalPos = WorldToLocalPos(WorldPos, ChunkCoord);

	// Validate local position
	if (LocalPos.X < 0 || LocalPos.X >= Configuration->ChunkSize ||
		LocalPos.Y < 0 || LocalPos.Y >= Configuration->ChunkSize ||
		LocalPos.Z < 0 || LocalPos.Z >= Configuration->ChunkSize)
	{
		UE_LOG(LogVoxelEdit, Warning, TEXT("ApplyEdit: Invalid local position (%d,%d,%d)"),
			LocalPos.X, LocalPos.Y, LocalPos.Z);
		if (bAutoOperation)
		{
			CancelEditOperation();
		}
		return false;
	}

	// Apply the edit
	ApplyEditInternal(ChunkCoord, LocalPos, FVoxelEdit(LocalPos, NewData, Mode));

	// Auto-end operation if we auto-started
	if (bAutoOperation)
	{
		EndEditOperation();
	}

	return true;
}

int32 UVoxelEditManager::ApplyBrushEdit(const FVector& WorldPos, const FVoxelBrushParams& Brush, EEditMode Mode)
{
	if (!bIsInitialized || !Configuration)
	{
		return 0;
	}

	// Track edit center and radius for scatter removal
	CurrentEditCenter = WorldPos;
	CurrentEditRadius = Brush.Radius;
	LastRejectedEditCount = 0;

	const float VoxelSize = Configuration->VoxelSize;
	const int32 ChunkSize = Configuration->ChunkSize;

	// The cylinder's axis: the brush's own, else local up at the centre (radial on a planet; SP-F8).
	FVector CylinderAxis = Brush.Axis.GetSafeNormal();
	if (CylinderAxis.IsZero())
	{
		CylinderAxis = (Configuration->WorldMode == EWorldMode::SphericalPlanet && !(WorldPos - Configuration->WorldOrigin).IsNearlyZero())
			? (WorldPos - Configuration->WorldOrigin).GetSafeNormal()
			: FVector::UpVector;
	}
	const bool bTiltedCylinder = Brush.Shape == EVoxelBrushShape::Cylinder && CylinderAxis != FVector::UpVector;

	// Calculate voxel bounds of the brush (a tilted cylinder reaches sqrt(2) of its radius along the world axes)
	const int32 VoxelRadius = FMath::CeilToInt((bTiltedCylinder ? Brush.Radius * UE_SQRT_2 : Brush.Radius) / VoxelSize);

	// Auto-start operation if none in progress
	const bool bAutoOperation = !CurrentOperation.IsValid();
	if (bAutoOperation)
	{
		FString ModeStr;
		switch (Mode)
		{
		case EEditMode::Set: ModeStr = TEXT("Set"); break;
		case EEditMode::Add: ModeStr = TEXT("Add"); break;
		case EEditMode::Subtract: ModeStr = TEXT("Subtract"); break;
		case EEditMode::Paint: ModeStr = TEXT("Paint"); break;
		}
		BeginEditOperation(FString::Printf(TEXT("Brush %s (R=%.0f)"), *ModeStr, Brush.Radius));
	}

	int32 ModifiedCount = 0;

	// Iterate over all voxels in the bounding box
	for (int32 DZ = -VoxelRadius; DZ <= VoxelRadius; ++DZ)
	{
		for (int32 DY = -VoxelRadius; DY <= VoxelRadius; ++DY)
		{
			for (int32 DX = -VoxelRadius; DX <= VoxelRadius; ++DX)
			{
				// Calculate world position of this voxel
				const FVector VoxelWorldPos = WorldPos + FVector(DX, DY, DZ) * VoxelSize;

				// Calculate distance from brush center
				float Distance = 0.0f;
				switch (Brush.Shape)
				{
				case EVoxelBrushShape::Sphere:
					Distance = FVector::Dist(VoxelWorldPos, WorldPos);
					break;

				case EVoxelBrushShape::Cube:
					{
						const FVector Delta = (VoxelWorldPos - WorldPos).GetAbs();
						Distance = FMath::Max3(Delta.X, Delta.Y, Delta.Z);
					}
					break;

				case EVoxelBrushShape::Cylinder:
					{
						const FVector Delta = VoxelWorldPos - WorldPos;
						if (bTiltedCylinder)
						{
							// Along the axis is handled like Z below; the distance is across it.
							const double Along = FVector::DotProduct(Delta, CylinderAxis);
							if (FMath::Abs(Along) > Brush.Radius)
							{
								continue;
							}
							Distance = static_cast<float>((Delta - CylinderAxis * Along).Size());
							break;
						}
						Distance = FMath::Sqrt(Delta.X * Delta.X + Delta.Y * Delta.Y);
						// Z is handled separately - if outside Z range, skip
						if (FMath::Abs(Delta.Z) > Brush.Radius)
						{
							continue;
						}
					}
					break;
				}

				// Check if within brush radius
				if (Distance > Brush.Radius)
				{
					continue;
				}

				// Calculate falloff
				const float NormalizedDistance = Distance / Brush.Radius;
				const float Falloff = Brush.GetFalloff(NormalizedDistance);
				const float EffectiveStrength = Brush.Strength * Falloff;

				if (EffectiveStrength < 0.01f)
				{
					continue;
				}

				// Convert to chunk/local coordinates
				const FIntVector ChunkCoord = WorldToChunkCoord(VoxelWorldPos);
				const FIntVector LocalPos = WorldToLocalPos(VoxelWorldPos, ChunkCoord);

				// Validate
				if (LocalPos.X < 0 || LocalPos.X >= ChunkSize ||
					LocalPos.Y < 0 || LocalPos.Y >= ChunkSize ||
					LocalPos.Z < 0 || LocalPos.Z >= ChunkSize)
				{
					continue;
				}

				// Edit-protection veto (per voxel): a brush overlapping a protected region edits
				// only the unprotected part. Skipped voxels are counted, not errored — the UI can
				// message "protected" from GetLastRejectedEditCount.
				if (EditValidator && !EditValidator->CanApplyEdit(VoxelWorldPos, CurrentEditSource))
				{
					++LastRejectedEditCount;
					continue;
				}

				// Calculate density delta for this voxel (affected by falloff)
				const int32 DensityChange = FMath::RoundToInt(Brush.DensityDelta * EffectiveStrength);

				// Skip if no meaningful change
				if (DensityChange < 1 && Mode != EEditMode::Paint && Mode != EEditMode::Set)
				{
					continue;
				}

				// Create edit with delta values (will be applied to procedural data at merge time)
				FVoxelEdit Edit(LocalPos, Mode, DensityChange, Brush.MaterialID);

				// For Set mode, pre-compute the NewData since it's absolute
				if (Mode == EEditMode::Set)
				{
					Edit.NewData.MaterialID = Brush.MaterialID;
					Edit.NewData.Density = 255;
				}

				// Apply the edit
				ApplyEditInternal(ChunkCoord, LocalPos, Edit);
				++ModifiedCount;
			}
		}
	}

	// Auto-end operation
	if (bAutoOperation)
	{
		EndEditOperation();
	}

	if (LastRejectedEditCount > 0)
	{
		UE_LOG(LogVoxelEdit, Log, TEXT("Brush edit: %d voxels modified, %d rejected by edit protection"),
			ModifiedCount, LastRejectedEditCount);
	}
	else
	{
		UE_LOG(LogVoxelEdit, Verbose, TEXT("Brush edit: %d voxels modified"), ModifiedCount);
	}
	return ModifiedCount;
}

// ==================== Undo/Redo ====================

bool UVoxelEditManager::Undo()
{
	if (!CanUndo())
	{
		return false;
	}

	// Pop from undo stack
	FVoxelEditOperation Operation = MoveTemp(UndoStack.Last());
	UndoStack.RemoveAt(UndoStack.Num() - 1);

	// Every voxel the operation changed gets back the edit it had before (or none), and the voxels reverted in each chunk
	// are tracked. Edits made outside the history since (water propagation's flood) at those voxels go with it (G60).
	TMap<FIntVector, FVoxelEditedVoxelBounds> AffectedChunks;
	RestoreOperation(Operation, /*bAsLeft=*/false, AffectedChunks);

	// Add to redo stack
	RedoStack.Add(MoveTemp(Operation));

	// Notify affected chunks (undo restores previous state, so use System to regenerate scatter)
	// Use zero radius since we want full regeneration, not targeted removal
	for (const TPair<FIntVector, FVoxelEditedVoxelBounds>& Pair : AffectedChunks)
	{
		BroadcastChunkEdited(Pair.Key, EEditSource::System, FVector::ZeroVector, 0.0f, Pair.Value);
	}

	OnUndoRedoStateChanged.Broadcast();

	UE_LOG(LogVoxelEdit, Log, TEXT("Undo: '%s' (%d edits)"),
		*RedoStack.Last().Description, RedoStack.Last().GetEditCount());

	return true;
}

bool UVoxelEditManager::Redo()
{
	if (!CanRedo())
	{
		return false;
	}

	// Pop from redo stack
	FVoxelEditOperation Operation = MoveTemp(RedoStack.Last());
	RedoStack.RemoveAt(RedoStack.Num() - 1);

	// Every voxel the operation changed gets the edit it left (or none) again, in its own chunk; the voxels re-applied in
	// each chunk are tracked. Edits that followed from it before its undo (a flood) come again from its broadcast (G60).
	TMap<FIntVector, FVoxelEditedVoxelBounds> AffectedChunks;
	RestoreOperation(Operation, /*bAsLeft=*/true, AffectedChunks);
	const EEditSource Source = Operation.Source;
	const FVector EditCenter = Operation.EditCenter;
	const float EditRadius = Operation.EditRadius;

	// Add back to undo stack
	UndoStack.Add(MoveTemp(Operation));

	// Notify affected chunks with what the operation broadcast when it was made (its source, brush centre and radius), so
	// they respond as they did then: a player brush clears scatter in its sphere again (its undo regenerated it), a system
	// edit re-extracts the chunks that read it. It used the current source with no radius, so a system edit redone as a
	// player's re-extracted nothing (G60).
	for (const TPair<FIntVector, FVoxelEditedVoxelBounds>& Pair : AffectedChunks)
	{
		BroadcastChunkEdited(Pair.Key, Source, EditCenter, EditRadius, Pair.Value);
	}

	OnUndoRedoStateChanged.Broadcast();

	UE_LOG(LogVoxelEdit, Log, TEXT("Redo: '%s' (%d edits)"),
		*UndoStack.Last().Description, UndoStack.Last().GetEditCount());

	return true;
}

void UVoxelEditManager::ClearHistory()
{
	UndoStack.Empty();
	RedoStack.Empty();
	OnUndoRedoStateChanged.Broadcast();

	UE_LOG(LogVoxelEdit, Log, TEXT("Undo/redo history cleared"));
}

// ==================== Edit Layer Access ====================

FChunkEditLayer* UVoxelEditManager::GetOrCreateEditLayer(const FIntVector& ChunkCoord)
{
	if (FChunkEditLayer* Existing = EditLayers.Find(ChunkCoord))
	{
		return Existing;
	}

	const int32 ChunkSize = Configuration ? Configuration->ChunkSize : VOXEL_DEFAULT_CHUNK_SIZE;
	return &EditLayers.Add(ChunkCoord, FChunkEditLayer(ChunkCoord, ChunkSize));
}

const FChunkEditLayer* UVoxelEditManager::GetEditLayer(const FIntVector& ChunkCoord) const
{
	return EditLayers.Find(ChunkCoord);
}

bool UVoxelEditManager::ChunkHasEdits(const FIntVector& ChunkCoord) const
{
	if (const FChunkEditLayer* Layer = EditLayers.Find(ChunkCoord))
	{
		return !Layer->IsEmpty();
	}
	return false;
}

void UVoxelEditManager::GetEditedChunkCoords(TArray<FIntVector>& OutChunkCoords) const
{
	OutChunkCoords.Reset();
	for (const auto& Pair : EditLayers)
	{
		if (!Pair.Value.IsEmpty())
		{
			OutChunkCoords.Add(Pair.Key);
		}
	}
}

void UVoxelEditManager::ApplyEditsToVoxelData(const FIntVector& ChunkCoord, TArray<FVoxelData>& VoxelData) const
{
	const FChunkEditLayer* EditLayer = EditLayers.Find(ChunkCoord);
	if (!EditLayer || EditLayer->IsEmpty())
	{
		return;
	}

	// Mirror the merge meshing performs (see VoxelChunkManager mesh request build):
	// apply each sparse edit on top of the procedural voxel at its linear index.
	for (const auto& EditPair : EditLayer->Edits)
	{
		const int32 Index = EditPair.Key;
		if (VoxelData.IsValidIndex(Index))
		{
			VoxelData[Index] = EditPair.Value.ApplyToProceduralData(VoxelData[Index]);
		}
	}
}

int32 UVoxelEditManager::GetTotalEditCount() const
{
	int32 Total = 0;
	for (const auto& Pair : EditLayers)
	{
		Total += Pair.Value.GetEditCount();
	}
	return Total;
}

bool UVoxelEditManager::ClearChunkEdits(const FIntVector& ChunkCoord)
{
	if (FChunkEditLayer* Layer = EditLayers.Find(ChunkCoord))
	{
		if (!Layer->IsEmpty())
		{
			FVoxelEditedVoxelBounds Cleared;
			for (const TPair<int32, FVoxelEdit>& Pair : Layer->Edits)
			{
				Cleared.Add(Pair.Value.LocalPosition);
			}
			Layer->Clear();
			// Clearing edits is a system action - scatter should regenerate
			BroadcastChunkEdited(ChunkCoord, EEditSource::System, FVector::ZeroVector, 0.0f, Cleared);
			return true;
		}
	}
	return false;
}

void UVoxelEditManager::ClearAllEdits()
{
	// Collect affected chunks, and the voxels each loses, before clearing
	TArray<TPair<FIntVector, FVoxelEditedVoxelBounds>> AffectedChunks;
	for (const auto& Pair : EditLayers)
	{
		if (!Pair.Value.IsEmpty())
		{
			FVoxelEditedVoxelBounds Cleared;
			for (const TPair<int32, FVoxelEdit>& Edit : Pair.Value.Edits)
			{
				Cleared.Add(Edit.Value.LocalPosition);
			}
			AffectedChunks.Emplace(Pair.Key, Cleared);
		}
	}

	EditLayers.Empty();

	// Notify all affected chunks (clearing is system action - regenerate scatter)
	for (const TPair<FIntVector, FVoxelEditedVoxelBounds>& Pair : AffectedChunks)
	{
		BroadcastChunkEdited(Pair.Key, EEditSource::System, FVector::ZeroVector, 0.0f, Pair.Value);
	}

	UE_LOG(LogVoxelEdit, Log, TEXT("All edits cleared (%d chunks affected)"), AffectedChunks.Num());
}

// ==================== Serialization ====================

bool UVoxelEditManager::SaveEditsToFile(const FString& FilePath)
{
	if (!bIsInitialized)
	{
		UE_LOG(LogVoxelEdit, Error, TEXT("SaveEditsToFile: Manager not initialized"));
		return false;
	}

	TArray<uint8> Buffer;
	FMemoryWriter Writer(Buffer);

	// Write header
	Writer << const_cast<uint32&>(VOXEL_EDIT_FILE_MAGIC);
	Writer << const_cast<uint32&>(VOXEL_EDIT_FILE_VERSION);

	// Write number of chunks with edits
	int32 ChunkCount = EditLayers.Num();
	Writer << ChunkCount;

	// Write each chunk's edits
	for (const auto& Pair : EditLayers)
	{
		const FIntVector& ChunkCoord = Pair.Key;
		const FChunkEditLayer& Layer = Pair.Value;

		// Write chunk coordinate
		Writer << const_cast<FIntVector&>(ChunkCoord);

		// Write number of edits
		int32 EditCount = Layer.GetEditCount();
		Writer << EditCount;

		// Write each edit
		for (const auto& EditPair : Layer.Edits)
		{
			const FVoxelEdit& Edit = EditPair.Value;

			// Core position
			Writer << const_cast<FIntVector&>(Edit.LocalPosition);

			// Edit mode and relative edit data (Version 2+)
			uint8 EditModeValue = static_cast<uint8>(Edit.EditMode);
			Writer << EditModeValue;
			Writer << const_cast<int32&>(Edit.DensityDelta);
			Writer << const_cast<uint8&>(Edit.BrushMaterialID);

			// NewData
			Writer << const_cast<uint8&>(Edit.NewData.MaterialID);
			Writer << const_cast<uint8&>(Edit.NewData.Density);
			Writer << const_cast<uint8&>(Edit.NewData.BiomeID);
			Writer << const_cast<uint8&>(Edit.NewData.Metadata);
		}
	}

	// Save to file
	if (!FFileHelper::SaveArrayToFile(Buffer, *FilePath))
	{
		UE_LOG(LogVoxelEdit, Error, TEXT("SaveEditsToFile: Failed to write file '%s'"), *FilePath);
		return false;
	}

	UE_LOG(LogVoxelEdit, Log, TEXT("Saved %d edits across %d chunks to '%s' (%d bytes)"),
		GetTotalEditCount(), ChunkCount, *FilePath, Buffer.Num());

	return true;
}

bool UVoxelEditManager::LoadEditsFromFile(const FString& FilePath)
{
	if (!bIsInitialized)
	{
		UE_LOG(LogVoxelEdit, Error, TEXT("LoadEditsFromFile: Manager not initialized"));
		return false;
	}

	// Load file
	TArray<uint8> Buffer;
	if (!FFileHelper::LoadFileToArray(Buffer, *FilePath))
	{
		UE_LOG(LogVoxelEdit, Error, TEXT("LoadEditsFromFile: Failed to read file '%s'"), *FilePath);
		return false;
	}

	FMemoryReader Reader(Buffer);

	// Read and verify header
	uint32 Magic = 0;
	uint32 Version = 0;
	Reader << Magic;
	Reader << Version;

	if (Magic != VOXEL_EDIT_FILE_MAGIC)
	{
		UE_LOG(LogVoxelEdit, Error, TEXT("LoadEditsFromFile: Invalid file magic"));
		return false;
	}

	if (Version != VOXEL_EDIT_FILE_VERSION && Version != 2)
	{
		UE_LOG(LogVoxelEdit, Error, TEXT("LoadEditsFromFile: Unsupported version %d (expected %d or 2)"),
			Version, VOXEL_EDIT_FILE_VERSION);
		return false;
	}

	// Clear existing edits
	ClearAllEdits();
	ClearHistory();

	// Read number of chunks
	int32 ChunkCount = 0;
	Reader << ChunkCount;

	// Read each chunk's edits
	TArray<TPair<FIntVector, FVoxelEditedVoxelBounds>> LoadedChunks;
	int32 TotalEdits = 0;

	for (int32 ChunkIndex = 0; ChunkIndex < ChunkCount; ++ChunkIndex)
	{
		FIntVector ChunkCoord;
		Reader << ChunkCoord;

		int32 EditCount = 0;
		Reader << EditCount;

		FChunkEditLayer* Layer = GetOrCreateEditLayer(ChunkCoord);
		FVoxelEditedVoxelBounds Loaded;

		for (int32 EditIndex = 0; EditIndex < EditCount; ++EditIndex)
		{
			FVoxelEdit Edit;

			// Core position
			Reader << Edit.LocalPosition;

			// Edit mode and relative edit data
			uint8 EditModeValue = 0;
			Reader << EditModeValue;
			Edit.EditMode = static_cast<EEditMode>(EditModeValue);
			Reader << Edit.DensityDelta;
			Reader << Edit.BrushMaterialID;

			// NewData
			Reader << Edit.NewData.MaterialID;
			Reader << Edit.NewData.Density;
			Reader << Edit.NewData.BiomeID;
			Reader << Edit.NewData.Metadata;

			// Version 2 carried the pre-G60 OriginalData after it (4 bytes, unused)
			if (Version == 2)
			{
				uint8 Legacy[4];
				Reader.Serialize(Legacy, sizeof(Legacy));
			}

			Layer->ApplyEdit(Edit);
			Loaded.Add(Edit.LocalPosition);
			++TotalEdits;
		}

		LoadedChunks.Emplace(ChunkCoord, Loaded);
	}

	// Notify all loaded chunks (loading from file is system action - regenerate scatter)
	for (const TPair<FIntVector, FVoxelEditedVoxelBounds>& Pair : LoadedChunks)
	{
		BroadcastChunkEdited(Pair.Key, EEditSource::System, FVector::ZeroVector, 0.0f, Pair.Value);
	}

	UE_LOG(LogVoxelEdit, Log, TEXT("Loaded %d edits across %d chunks from '%s'"),
		TotalEdits, ChunkCount, *FilePath);

	return true;
}

// ==================== Debug ====================

FString UVoxelEditManager::GetDebugStats() const
{
	FString Stats = TEXT("=== VoxelEditManager ===\n");
	Stats += FString::Printf(TEXT("Initialized: %s\n"), bIsInitialized ? TEXT("Yes") : TEXT("No"));
	Stats += FString::Printf(TEXT("Edited Chunks: %d\n"), EditLayers.Num());
	Stats += FString::Printf(TEXT("Total Edits: %d\n"), GetTotalEditCount());
	Stats += FString::Printf(TEXT("Undo Stack: %d\n"), UndoStack.Num());
	Stats += FString::Printf(TEXT("Redo Stack: %d\n"), RedoStack.Num());
	Stats += FString::Printf(TEXT("Undo History Memory: %.2f MB (budget %.1f MB, voxel.Edit.UndoHistoryMB)\n"),
		GetHistoryMemoryUsage() / (1024.0 * 1024.0), CVarVoxelEditUndoHistoryMB.GetValueOnAnyThread());
	Stats += FString::Printf(TEXT("Operation In Progress: %s\n"),
		CurrentOperation.IsValid() ? TEXT("Yes") : TEXT("No"));
	Stats += FString::Printf(TEXT("Memory Usage: %.2f KB\n"), GetMemoryUsage() / 1024.0f);

	return Stats;
}

SIZE_T UVoxelEditManager::GetMemoryUsage() const
{
	SIZE_T Total = sizeof(UVoxelEditManager);

	// Edit layers
	Total += EditLayers.GetAllocatedSize();
	for (const auto& Pair : EditLayers)
	{
		Total += Pair.Value.GetMemoryUsage();
	}

	// Undo/redo stacks
	Total += UndoStack.GetAllocatedSize();
	for (const FVoxelEditOperation& Op : UndoStack)
	{
		Total += Op.GetMemoryUsage();
	}

	Total += RedoStack.GetAllocatedSize();
	for (const FVoxelEditOperation& Op : RedoStack)
	{
		Total += Op.GetMemoryUsage();
	}

	// Current operation
	if (CurrentOperation.IsValid())
	{
		Total += CurrentOperation->GetMemoryUsage();
	}

	return Total;
}

// ==================== Internal Methods ====================

FIntVector UVoxelEditManager::WorldToChunkCoord(const FVector& WorldPos) const
{
	if (!Configuration)
	{
		return FIntVector::ZeroValue;
	}

	// Subtract WorldOrigin to get position relative to voxel world
	const FVector RelativePos = WorldPos - Configuration->WorldOrigin;

	return FVoxelCoordinates::WorldToChunk(RelativePos, Configuration->ChunkSize, Configuration->VoxelSize);
}

FIntVector UVoxelEditManager::WorldToLocalPos(const FVector& WorldPos, const FIntVector& ChunkCoord) const
{
	if (!Configuration)
	{
		return FIntVector::ZeroValue;
	}

	const float VoxelSize = Configuration->VoxelSize;
	const int32 ChunkSize = Configuration->ChunkSize;
	const float ChunkWorldSize = ChunkSize * VoxelSize;

	// Subtract WorldOrigin to get position relative to voxel world
	const FVector RelativePos = WorldPos - Configuration->WorldOrigin;

	// Chunk origin in voxel world space (relative to WorldOrigin)
	const FVector ChunkOrigin = FVector(ChunkCoord) * ChunkWorldSize;

	// Offset from chunk origin
	const FVector LocalOffset = RelativePos - ChunkOrigin;

	// Convert to voxel indices
	return FIntVector(
		FMath::FloorToInt(LocalOffset.X / VoxelSize),
		FMath::FloorToInt(LocalOffset.Y / VoxelSize),
		FMath::FloorToInt(LocalOffset.Z / VoxelSize)
	);
}

FVector UVoxelEditManager::LocalToWorldPos(const FIntVector& ChunkCoord, const FIntVector& LocalPos) const
{
	if (!Configuration)
	{
		return FVector::ZeroVector;
	}

	const float VoxelSize = Configuration->VoxelSize;
	const int32 ChunkSize = Configuration->ChunkSize;
	const float ChunkWorldSize = ChunkSize * VoxelSize;

	// Chunk origin + local offset + half voxel (center) + WorldOrigin
	return Configuration->WorldOrigin
		+ FVector(ChunkCoord) * ChunkWorldSize
		+ FVector(LocalPos) * VoxelSize
		+ FVector(VoxelSize * 0.5f);
}

void UVoxelEditManager::ApplyEditInternal(
	const FIntVector& ChunkCoord,
	const FIntVector& LocalPos,
	const FVoxelEdit& Edit)
{
	// Create or get edit layer
	FChunkEditLayer* Layer = GetOrCreateEditLayer(ChunkCoord);
	if (!Layer)
	{
		return;
	}

	// Make a copy with correct local position
	FVoxelEdit EditCopy = Edit;
	EditCopy.LocalPosition = LocalPos;

	// The edit the voxel held before, for undo (G60: a relative edit has no previous voxel value of its own to restore)
	TOptional<FVoxelEdit> Before;
	if (const FVoxelEdit* Existing = Layer->GetEdit(LocalPos))
	{
		Before = *Existing;
	}

	// Check for existing edit at this location and accumulate if compatible
	if (const FVoxelEdit* ExistingEdit = Before.GetPtrOrNull())
	{
		const bool bExistingAbsolute = ExistingEdit->EditMode == EEditMode::Set;
		const bool bNewRelative = EditCopy.EditMode == EEditMode::Add || EditCopy.EditMode == EEditMode::Subtract
			|| EditCopy.EditMode == EEditMode::Paint;

		// A relative edit on a voxel that holds an absolute one (a POI pad, a dungeon wall, a smoothed voxel)
		// applies on top of it, and the result stays absolute. It used to replace the Set, so the voxel fell
		// back to procedural data minus the delta: one dig under a Set slab removed the target and its four
		// face neighbours, which had only received the brush falloff (G2).
		if (bExistingAbsolute && bNewRelative)
		{
			const FVoxelData Composed = EditCopy.ApplyToProceduralData(ExistingEdit->NewData);
			EditCopy.NewData = Composed;
			EditCopy.EditMode = EEditMode::Set;
			EditCopy.DensityDelta = 0;
			EditCopy.BrushMaterialID = Composed.MaterialID;
		}
		// For Add/Subtract modes, accumulate the density delta
		else if ((EditCopy.EditMode == EEditMode::Add || EditCopy.EditMode == EEditMode::Subtract) &&
			(ExistingEdit->EditMode == EEditMode::Add || ExistingEdit->EditMode == EEditMode::Subtract))
		{
			// Convert existing edit's effect to a signed delta
			int32 ExistingSignedDelta = ExistingEdit->DensityDelta;
			if (ExistingEdit->EditMode == EEditMode::Subtract)
			{
				ExistingSignedDelta = -ExistingSignedDelta;
			}

			// Convert new edit's effect to a signed delta
			int32 NewSignedDelta = EditCopy.DensityDelta;
			if (EditCopy.EditMode == EEditMode::Subtract)
			{
				NewSignedDelta = -NewSignedDelta;
			}

			// Accumulate
			int32 TotalSignedDelta = ExistingSignedDelta + NewSignedDelta;

			// If edits cancel out to zero density change...
			if (TotalSignedDelta == 0)
			{
				// Check if the new edit is an Add with a material - convert to Paint instead of removing
				// This handles the case: dig block, then place different material at same location
				if (Edit.EditMode == EEditMode::Add && Edit.BrushMaterialID != 0)
				{
					// Convert to Paint operation - density unchanged, material changes
					EditCopy.EditMode = EEditMode::Paint;
					EditCopy.DensityDelta = 0;
					EditCopy.BrushMaterialID = Edit.BrushMaterialID;
					// Skip the normal mode/delta logic below - go directly to apply
				}
				else
				{
					// No material change, remove the edit entirely
					// This reverts the voxel to pure procedural state
					Layer->RemoveEdit(LocalPos);

					// Track removal in current operation if active: undo puts the removed edit back, redo removes it again
					if (CurrentOperation.IsValid())
					{
						CurrentOperation->RecordChange(ChunkCoord, LocalPos, ExistingEdit, nullptr);
						OperationBounds.FindOrAdd(ChunkCoord).Add(LocalPos);
					}

					// Notify listeners that chunk changed (standalone edit only; batch defers to EndEditOperation)
					if (!CurrentOperation.IsValid())
					{
						FVoxelEditedVoxelBounds One;
						One.Add(LocalPos);
						BroadcastChunkEdited(ChunkCoord, CurrentEditSource, CurrentEditCenter, CurrentEditRadius, One);
					}
					return;
				}
			}
			else
			{
				// Store as unsigned delta with appropriate mode
				if (TotalSignedDelta > 0)
				{
					EditCopy.EditMode = EEditMode::Add;
					EditCopy.DensityDelta = TotalSignedDelta;
				}
				else
				{
					EditCopy.EditMode = EEditMode::Subtract;
					EditCopy.DensityDelta = -TotalSignedDelta;
				}

				// Keep material from whichever edit is adding material
				if (Edit.EditMode == EEditMode::Add)
				{
					EditCopy.BrushMaterialID = Edit.BrushMaterialID;
				}
				else if (ExistingEdit->EditMode == EEditMode::Add)
				{
					EditCopy.BrushMaterialID = ExistingEdit->BrushMaterialID;
				}
			}
		}
		// An absolute edit (Set) replaces whatever was there; so does Paint over Add/Subtract (as before).
	}

	// Apply to layer
	Layer->ApplyEdit(EditCopy);

	// Track in current operation if active
	if (CurrentOperation.IsValid())
	{
		CurrentOperation->RecordChange(ChunkCoord, LocalPos, Before.GetPtrOrNull(), &EditCopy);
		OperationBounds.FindOrAdd(ChunkCoord).Add(LocalPos);
	}

	// Notify listeners — but only for standalone edits. During a Begin/End batch operation (brush),
	// defer to EndEditOperation, which broadcasts ONCE per affected chunk. Broadcasting per voxel made
	// the chunk-manager handler run its full-radius scatter clear + chunk/neighbour dirty cascade once
	// per edited voxel — a large brush (hundreds of thousands of voxels) froze the game thread.
	if (!CurrentOperation.IsValid())
	{
		FVoxelEditedVoxelBounds One;
		One.Add(LocalPos);
		BroadcastChunkEdited(ChunkCoord, CurrentEditSource, CurrentEditCenter, CurrentEditRadius, One);
	}
}

void UVoxelEditManager::BroadcastChunkEdited(const FIntVector& ChunkCoord, EEditSource Source, const FVector& EditCenter,
	float EditRadius, const FVoxelEditedVoxelBounds& Bounds)
{
	// Listeners may edit in turn (water propagation): keep the outer broadcast's bounds for the rest of its listeners.
	const FVoxelEditedVoxelBounds* Outer = ActiveBroadcastBounds;
	ActiveBroadcastBounds = &Bounds;
	OnChunkEdited.Broadcast(ChunkCoord, Source, EditCenter, EditRadius);
	ActiveBroadcastBounds = Outer;
}

bool UVoxelEditManager::GetBroadcastEditBounds(FIntVector& OutMin, FIntVector& OutMax) const
{
	if (!ActiveBroadcastBounds || !ActiveBroadcastBounds->IsValid())
	{
		return false;
	}
	OutMin = ActiveBroadcastBounds->Min;
	OutMax = ActiveBroadcastBounds->Max;
	return true;
}

void UVoxelEditManager::TrimUndoStack()
{
	while (UndoStack.Num() > MaxUndoHistory)
	{
		UndoStack.RemoveAt(0);
	}

	// The memory budget (G68): oldest first, the newest operation kept whatever its size.
	const double BudgetMB = CVarVoxelEditUndoHistoryMB.GetValueOnAnyThread();
	if (BudgetMB <= 0.0)
	{
		return;
	}
	const int64 Budget = static_cast<int64>(BudgetMB * 1024.0 * 1024.0);
	int64 Bytes = GetHistoryMemoryUsage();
	int32 Dropped = 0;
	while (Bytes > Budget && UndoStack.Num() > 1)
	{
		Bytes -= static_cast<int64>(UndoStack[0].GetMemoryUsage());
		UndoStack.RemoveAt(0);
		++Dropped;
	}
	if (Dropped > 0)
	{
		UE_LOG(LogVoxelEdit, Log, TEXT("Undo history over its %.2f MB budget (voxel.Edit.UndoHistoryMB): dropped the %d oldest operation(s), %d kept in %.2f MB"),
			BudgetMB, Dropped, UndoStack.Num(), GetHistoryMemoryUsage() / (1024.0 * 1024.0));
	}
}

int64 UVoxelEditManager::GetHistoryMemoryUsage() const
{
	int64 Total = static_cast<int64>(UndoStack.GetAllocatedSize() + RedoStack.GetAllocatedSize());
	for (const FVoxelEditOperation& Op : UndoStack)
	{
		Total += static_cast<int64>(Op.GetMemoryUsage());
	}
	for (const FVoxelEditOperation& Op : RedoStack)
	{
		Total += static_cast<int64>(Op.GetMemoryUsage());
	}
	return Total;
}

void UVoxelEditManager::RestoreOperation(const FVoxelEditOperation& Operation, bool bAsLeft,
	TMap<FIntVector, FVoxelEditedVoxelBounds>& OutChunkBounds)
{
	// An operation can change a voxel more than once (a batch writing it twice): the state before it is its first change's
	// Before, the state it left its last change's After, hence the two directions.
	const int32 Count = Operation.Changes.Num();
	for (int32 Step = 0; Step < Count; ++Step)
	{
		const FVoxelEditChange& Change = Operation.Changes[bAsLeft ? Step : Count - 1 - Step];
		const bool bHasEdit = bAsLeft ? Change.bHasAfter : Change.bHadBefore;
		if (bHasEdit)
		{
			FVoxelEdit Edit = bAsLeft ? Change.After : Change.Before;
			Edit.LocalPosition = Change.LocalPosition;
			GetOrCreateEditLayer(Change.ChunkCoord)->ApplyEdit(Edit);
		}
		else if (FChunkEditLayer* Layer = EditLayers.Find(Change.ChunkCoord))
		{
			Layer->RemoveEdit(Change.LocalPosition);
		}
		OutChunkBounds.FindOrAdd(Change.ChunkCoord).Add(Change.LocalPosition);
	}
}
