// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelEditManager.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelCoordinates.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelEdit, Log, All);

// File format version for binary serialization
// Version 1: Original format with NewData/OriginalData only
// Version 2: Added EditMode, DensityDelta, BrushMaterialID for relative edits
static constexpr uint32 VOXEL_EDIT_FILE_VERSION = 2;
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

void UVoxelEditManager::BeginEditOperation(const FString& Description)
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
		return;
	}

	// Clear redo stack when new edits are made
	RedoStack.Empty();

	// Add to undo stack
	UndoStack.Add(MoveTemp(*CurrentOperation));
	CurrentOperation.Reset();

	// Trim if necessary
	TrimUndoStack();

	// Notify listeners
	OnUndoRedoStateChanged.Broadcast();

	UE_LOG(LogVoxelEdit, Verbose, TEXT("Edit operation completed (UndoStack=%d)"), UndoStack.Num());
}

void UVoxelEditManager::CancelEditOperation()
{
	if (!CurrentOperation.IsValid())
	{
		UE_LOG(LogVoxelEdit, Warning, TEXT("CancelEditOperation called with no operation in progress"));
		return;
	}

	// Revert all edits in the current operation
	for (const FVoxelEdit& Edit : CurrentOperation->Edits)
	{
		// Find which chunk this edit belongs to
		for (const FIntVector& ChunkCoord : CurrentOperation->AffectedChunks)
		{
			if (FChunkEditLayer* Layer = EditLayers.Find(ChunkCoord))
			{
				const int32 Index = Edit.GetVoxelIndex(Layer->ChunkSize);

				// Remove the edit or restore original
				if (Edit.OriginalData == FVoxelData::Air())
				{
					// Was a new edit - just remove it
					Layer->Edits.Remove(Index);
				}
				else
				{
					// Restore original edit
					FVoxelEdit RevertEdit = Edit;
					RevertEdit.NewData = Edit.OriginalData;
					Layer->Edits.Add(Index, RevertEdit);
				}
			}
		}
	}

	// Notify affected chunks (use current source - cancelling reverts player's work)
	for (const FIntVector& ChunkCoord : CurrentOperation->AffectedChunks)
	{
		OnChunkEdited.Broadcast(ChunkCoord, CurrentEditSource, CurrentEditCenter, CurrentEditRadius);
	}

	UE_LOG(LogVoxelEdit, Log, TEXT("Edit operation '%s' cancelled (%d edits reverted)"),
		*CurrentOperation->Description, CurrentOperation->GetEditCount());

	CurrentOperation.Reset();
}

bool UVoxelEditManager::ApplyEdit(const FVector& WorldPos, const FVoxelData& NewData, EEditMode Mode)
{
	if (!bIsInitialized || !Configuration)
	{
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

	// Get original data
	const FVoxelData OriginalData = GetOriginalVoxelData(ChunkCoord, LocalPos);

	// Apply the edit
	ApplyEditInternal(ChunkCoord, LocalPos, NewData, OriginalData, Mode);

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

	const float VoxelSize = Configuration->VoxelSize;
	const int32 ChunkSize = Configuration->ChunkSize;

	// Calculate voxel bounds of the brush
	const int32 VoxelRadius = FMath::CeilToInt(Brush.Radius / VoxelSize);

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
		case EEditMode::Smooth: ModeStr = TEXT("Smooth"); break;
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

	UE_LOG(LogVoxelEdit, Verbose, TEXT("Brush edit: %d voxels modified"), ModifiedCount);
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

	// Track affected chunks
	TSet<FIntVector> AffectedChunks;

	// Revert each edit
	for (const FVoxelEdit& Edit : Operation.Edits)
	{
		// Find the chunk this edit belongs to
		for (const FIntVector& ChunkCoord : Operation.AffectedChunks)
		{
			if (FChunkEditLayer* Layer = EditLayers.Find(ChunkCoord))
			{
				const int32 Index = Edit.GetVoxelIndex(Layer->ChunkSize);
				if (Layer->Edits.Contains(Index))
				{
					// Swap to original data
					FVoxelEdit* ExistingEdit = Layer->Edits.Find(Index);
					if (ExistingEdit)
					{
						ExistingEdit->NewData = Edit.OriginalData;

						// If reverting to air, remove the edit entirely
						if (Edit.OriginalData == FVoxelData::Air())
						{
							Layer->Edits.Remove(Index);
						}
					}
					AffectedChunks.Add(ChunkCoord);
					break;
				}
			}
		}
	}

	// Add to redo stack
	RedoStack.Add(MoveTemp(Operation));

	// Notify affected chunks (undo restores previous state, so use System to regenerate scatter)
	// Use zero radius since we want full regeneration, not targeted removal
	for (const FIntVector& ChunkCoord : AffectedChunks)
	{
		OnChunkEdited.Broadcast(ChunkCoord, EEditSource::System, FVector::ZeroVector, 0.0f);
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

	// Track affected chunks
	TSet<FIntVector> AffectedChunks;

	// Re-apply each edit
	for (const FVoxelEdit& Edit : Operation.Edits)
	{
		for (const FIntVector& ChunkCoord : Operation.AffectedChunks)
		{
			FChunkEditLayer* Layer = GetOrCreateEditLayer(ChunkCoord);
			if (Layer)
			{
				const int32 Index = Edit.GetVoxelIndex(Layer->ChunkSize);

				// Re-apply the edit
				FVoxelEdit RedoEdit = Edit;
				Layer->Edits.Add(Index, RedoEdit);
				AffectedChunks.Add(ChunkCoord);
				break;
			}
		}
	}

	// Add back to undo stack
	UndoStack.Add(MoveTemp(Operation));

	// Notify affected chunks (redo reapplies edits, use current source - typically Player)
	// Use zero radius since this is a full operation redo
	for (const FIntVector& ChunkCoord : AffectedChunks)
	{
		OnChunkEdited.Broadcast(ChunkCoord, CurrentEditSource, FVector::ZeroVector, 0.0f);
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
			Layer->Clear();
			// Clearing edits is a system action - scatter should regenerate
			OnChunkEdited.Broadcast(ChunkCoord, EEditSource::System, FVector::ZeroVector, 0.0f);
			return true;
		}
	}
	return false;
}

void UVoxelEditManager::ClearAllEdits()
{
	// Collect affected chunks before clearing
	TArray<FIntVector> AffectedChunks;
	for (const auto& Pair : EditLayers)
	{
		if (!Pair.Value.IsEmpty())
		{
			AffectedChunks.Add(Pair.Key);
		}
	}

	EditLayers.Empty();

	// Notify all affected chunks (clearing is system action - regenerate scatter)
	for (const FIntVector& ChunkCoord : AffectedChunks)
	{
		OnChunkEdited.Broadcast(ChunkCoord, EEditSource::System, FVector::ZeroVector, 0.0f);
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

			// Legacy NewData/OriginalData (kept for potential backwards compatibility)
			Writer << const_cast<uint8&>(Edit.NewData.MaterialID);
			Writer << const_cast<uint8&>(Edit.NewData.Density);
			Writer << const_cast<uint8&>(Edit.NewData.BiomeID);
			Writer << const_cast<uint8&>(Edit.NewData.Metadata);
			Writer << const_cast<uint8&>(Edit.OriginalData.MaterialID);
			Writer << const_cast<uint8&>(Edit.OriginalData.Density);
			Writer << const_cast<uint8&>(Edit.OriginalData.BiomeID);
			Writer << const_cast<uint8&>(Edit.OriginalData.Metadata);
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

	if (Version != VOXEL_EDIT_FILE_VERSION && Version != 1)
	{
		UE_LOG(LogVoxelEdit, Error, TEXT("LoadEditsFromFile: Unsupported version %d (expected %d or 1)"),
			Version, VOXEL_EDIT_FILE_VERSION);
		return false;
	}

	const bool bIsVersion2 = (Version >= 2);

	// Clear existing edits
	ClearAllEdits();
	ClearHistory();

	// Read number of chunks
	int32 ChunkCount = 0;
	Reader << ChunkCount;

	// Read each chunk's edits
	TArray<FIntVector> LoadedChunks;
	int32 TotalEdits = 0;

	for (int32 ChunkIndex = 0; ChunkIndex < ChunkCount; ++ChunkIndex)
	{
		FIntVector ChunkCoord;
		Reader << ChunkCoord;

		int32 EditCount = 0;
		Reader << EditCount;

		FChunkEditLayer* Layer = GetOrCreateEditLayer(ChunkCoord);

		for (int32 EditIndex = 0; EditIndex < EditCount; ++EditIndex)
		{
			FVoxelEdit Edit;

			// Core position
			Reader << Edit.LocalPosition;

			// Version 2+: Read edit mode and relative edit data
			if (bIsVersion2)
			{
				uint8 EditModeValue = 0;
				Reader << EditModeValue;
				Edit.EditMode = static_cast<EEditMode>(EditModeValue);
				Reader << Edit.DensityDelta;
				Reader << Edit.BrushMaterialID;
			}

			// Read NewData/OriginalData (all versions)
			Reader << Edit.NewData.MaterialID;
			Reader << Edit.NewData.Density;
			Reader << Edit.NewData.BiomeID;
			Reader << Edit.NewData.Metadata;
			Reader << Edit.OriginalData.MaterialID;
			Reader << Edit.OriginalData.Density;
			Reader << Edit.OriginalData.BiomeID;
			Reader << Edit.OriginalData.Metadata;

			// Version 1 fallback: Use Set mode with absolute data
			if (!bIsVersion2)
			{
				Edit.EditMode = EEditMode::Set;
				Edit.DensityDelta = 0;
				Edit.BrushMaterialID = Edit.NewData.MaterialID;
			}

			Edit.Timestamp = FPlatformTime::Seconds();

			Layer->ApplyEdit(Edit);
			++TotalEdits;
		}

		LoadedChunks.Add(ChunkCoord);
	}

	// Notify all loaded chunks (loading from file is system action - regenerate scatter)
	for (const FIntVector& ChunkCoord : LoadedChunks)
	{
		OnChunkEdited.Broadcast(ChunkCoord, EEditSource::System, FVector::ZeroVector, 0.0f);
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
	const FVoxelData& NewData,
	const FVoxelData& OriginalData,
	EEditMode Mode)
{
	// Create or get edit layer
	FChunkEditLayer* Layer = GetOrCreateEditLayer(ChunkCoord);
	if (!Layer)
	{
		return;
	}

	// Create edit record
	FVoxelEdit Edit(LocalPos, NewData, OriginalData, Mode);

	// Apply to layer
	Layer->ApplyEdit(Edit);

	// Track in current operation if active
	if (CurrentOperation.IsValid())
	{
		CurrentOperation->AddEdit(Edit, ChunkCoord);
	}

	// Notify listeners with current edit source and location
	OnChunkEdited.Broadcast(ChunkCoord, CurrentEditSource, CurrentEditCenter, CurrentEditRadius);
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

	// Check for existing edit at this location and accumulate if compatible
	if (const FVoxelEdit* ExistingEdit = Layer->GetEdit(LocalPos))
	{
		// For Add/Subtract modes, accumulate the density delta
		if ((EditCopy.EditMode == EEditMode::Add || EditCopy.EditMode == EEditMode::Subtract) &&
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

					// Track removal in current operation if active
					if (CurrentOperation.IsValid())
					{
						// Store a "removal" edit for undo purposes
						FVoxelEdit RemovalEdit = *ExistingEdit;
						RemovalEdit.DensityDelta = 0;
						CurrentOperation->AddEdit(RemovalEdit, ChunkCoord);
					}

					// Notify listeners that chunk changed
					OnChunkEdited.Broadcast(ChunkCoord, CurrentEditSource, CurrentEditCenter, CurrentEditRadius);
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
		// For Set mode or mixed modes, the new edit replaces (current behavior)
	}

	// Apply to layer
	Layer->ApplyEdit(EditCopy);

	// Track in current operation if active
	if (CurrentOperation.IsValid())
	{
		CurrentOperation->AddEdit(EditCopy, ChunkCoord);
	}

	// Notify listeners with current edit source and location
	OnChunkEdited.Broadcast(ChunkCoord, CurrentEditSource, CurrentEditCenter, CurrentEditRadius);
}

FVoxelData UVoxelEditManager::GetOriginalVoxelData(const FIntVector& ChunkCoord, const FIntVector& LocalPos) const
{
	// First check if there's an existing edit
	if (const FChunkEditLayer* Layer = GetEditLayer(ChunkCoord))
	{
		if (const FVoxelEdit* Edit = Layer->GetEdit(LocalPos))
		{
			// Return the current edited data (which becomes "original" for the new edit)
			return Edit->NewData;
		}
	}

	// No existing edit - return air as placeholder
	// NOTE: In a full implementation, we'd query the chunk manager for procedural data
	// For now, this means undo will restore to air for first-time edits
	return FVoxelData::Air();
}

void UVoxelEditManager::TrimUndoStack()
{
	while (UndoStack.Num() > MaxUndoHistory)
	{
		UndoStack.RemoveAt(0);
	}
}
