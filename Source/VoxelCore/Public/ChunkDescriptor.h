// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.h"
#include "VoxelData.h"
#include "VoxelChunkCodec.h"
#include "ChunkDescriptor.generated.h"

/**
 * Chunk metadata and voxel storage.
 *
 * Contains all data needed to describe a chunk's state and contents.
 * VoxelData array is sized to ChunkSize^3 elements.
 *
 * Memory: ~128 KB for 32^3 chunk (voxel data only)
 * Thread Safety: Not thread-safe, use external synchronization
 *
 * @see Documentation/DATA_STRUCTURES.md
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FChunkDescriptor
{
	GENERATED_BODY()

	/** Chunk position in chunk coordinate space */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chunk")
	FIntVector ChunkCoord = FIntVector::ZeroValue;

	/** Current LOD level (0 = finest detail) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chunk")
	int32 LODLevel = 0;

	/** Number of voxels per edge (32, 64, etc.) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chunk")
	int32 ChunkSize = VOXEL_DEFAULT_CHUNK_SIZE;

	/** Voxel data array (ChunkSize^3 elements) */
	UPROPERTY()
	TArray<FVoxelData> VoxelData;

	/** World-space bounding box */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chunk")
	FBox Bounds = FBox(ForceInit);

	/** Chunk needs mesh regeneration */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chunk")
	bool bIsDirty = false;

	/** Chunk has player edits applied */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chunk")
	bool bHasEdits = false;

	/** LOD transition blend factor (0 = this LOD, 1 = next LOD) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chunk")
	float MorphFactor = 0.0f;

	/** Seed used for procedural generation (stored as int32 for Blueprint compatibility) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chunk")
	int32 GenerationSeed = 0;

	/** Current streaming state */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chunk")
	EChunkState State = EChunkState::Unloaded;

	// --- Far-chunk compression runtime state (non-UPROPERTY: transient, not serialized) ---

	/**
	 * Where the voxel payload currently lives. The raw VoxelData array remains the fine-grained
	 * source of truth (IsVoxelDataResident()); Residency additionally tracks the compact forms so
	 * EnsureResident() can materialize them. PR A: only Empty|Resident occur.
	 */
	EVoxelDataResidency Residency = EVoxelDataResidency::Empty;

	/**
	 * The resident array has been written since it was last (de)compressed. When a chunk
	 * re-qualifies for compression, a clean (!bDataMutated) chunk can drop its raw array and reuse
	 * a still-valid compressed buffer instead of re-encoding.
	 */
	bool bDataMutated = false;

	/** The single value a Uniform chunk collapses to (valid when bUniformValueValid). */
	FVoxelData UniformValue;

	/**
	 * UniformValue faithfully represents the current logical content — i.e. the chunk is (or, while
	 * temporarily Resident, was) all-UniformValue and nothing has mutated it since. Lets a re-qualifying
	 * chunk re-collapse to Uniform for free (drop the raw array, no re-scan). Cleared on any content write.
	 */
	bool bUniformValueValid = false;

	/**
	 * A resident chunk has already been evaluated for compression and neither the uniform nor the
	 * general codec beat the raw size, so the sweep skips re-attempting it until its content changes.
	 * Cleared on any content write.
	 */
	bool bCompressionEvaluated = false;

	/**
	 * Compressed voxel side buffer ([header][payload], see FVoxelChunkCodec) — valid when
	 * Residency == Compressed, or retained as a still-valid cache after EnsureResident() expands a
	 * compressed chunk that hasn't been mutated (enables a free re-compress). Non-UPROPERTY: runtime
	 * cache, not reflected/serialized. The format is persistence-ready for the future world-save epic.
	 */
	TArray<uint8> CompressedVoxelData;

	/** Default constructor */
	FChunkDescriptor() = default;

	/** Construct with coordinates and size */
	FChunkDescriptor(const FIntVector& InChunkCoord, int32 InChunkSize = VOXEL_DEFAULT_CHUNK_SIZE, int32 InLODLevel = 0)
		: ChunkCoord(InChunkCoord)
		, LODLevel(InLODLevel)
		, ChunkSize(InChunkSize)
	{
	}

	/** Allocate voxel data array for current chunk size */
	void AllocateVoxelData()
	{
		const int32 TotalVoxels = ChunkSize * ChunkSize * ChunkSize;
		VoxelData.SetNumZeroed(TotalVoxels);
		Residency = EVoxelDataResidency::Resident;
		bDataMutated = false;
		bUniformValueValid = false;
		bCompressionEvaluated = false;
		CompressedVoxelData.Empty();
	}

	/** Install a freshly generated resident voxel array (the sole population point). */
	void SetResidentVoxelData(TArray<FVoxelData>&& InData)
	{
		VoxelData = MoveTemp(InData);
		Residency = EVoxelDataResidency::Resident;
		bDataMutated = false;
		bUniformValueValid = false;
		bCompressionEvaluated = false;
		CompressedVoxelData.Empty();
	}

	/** Clear voxel data to free memory */
	void ClearVoxelData()
	{
		VoxelData.Empty();
		Residency = EVoxelDataResidency::Empty;
		bDataMutated = false;
		bUniformValueValid = false;
		bCompressionEvaluated = false;
		CompressedVoxelData.Empty();
	}

	/** Get total number of voxels in this chunk */
	FORCEINLINE int32 GetTotalVoxels() const
	{
		return ChunkSize * ChunkSize * ChunkSize;
	}

	/** Convert local voxel position to linear index */
	FORCEINLINE int32 GetVoxelIndex(const FIntVector& LocalPos) const
	{
		check(LocalPos.X >= 0 && LocalPos.X < ChunkSize);
		check(LocalPos.Y >= 0 && LocalPos.Y < ChunkSize);
		check(LocalPos.Z >= 0 && LocalPos.Z < ChunkSize);
		return LocalPos.X + LocalPos.Y * ChunkSize + LocalPos.Z * ChunkSize * ChunkSize;
	}

	/** Convert linear index to local voxel position */
	FORCEINLINE FIntVector GetVoxelPosition(int32 Index) const
	{
		check(Index >= 0 && Index < GetTotalVoxels());
		return FIntVector(
			Index % ChunkSize,
			(Index / ChunkSize) % ChunkSize,
			Index / (ChunkSize * ChunkSize)
		);
	}

	/** Get voxel at local position */
	FORCEINLINE FVoxelData GetVoxel(const FIntVector& LocalPos) const
	{
		const int32 Index = GetVoxelIndex(LocalPos);
		return VoxelData.IsValidIndex(Index) ? VoxelData[Index] : FVoxelData::Air();
	}

	/** Set voxel at local position */
	FORCEINLINE void SetVoxel(const FIntVector& LocalPos, const FVoxelData& Data)
	{
		const int32 Index = GetVoxelIndex(LocalPos);
		if (VoxelData.IsValidIndex(Index))
		{
			VoxelData[Index] = Data;
			bIsDirty = true;
			bDataMutated = true;
			bUniformValueValid = false;
			bCompressionEvaluated = false;
			CompressedVoxelData.Empty();
		}
	}

	/** Get voxel by linear index */
	FORCEINLINE FVoxelData GetVoxelByIndex(int32 Index) const
	{
		return VoxelData.IsValidIndex(Index) ? VoxelData[Index] : FVoxelData::Air();
	}

	/** Set voxel by linear index */
	FORCEINLINE void SetVoxelByIndex(int32 Index, const FVoxelData& Data)
	{
		if (VoxelData.IsValidIndex(Index))
		{
			VoxelData[Index] = Data;
			bIsDirty = true;
			bDataMutated = true;
			bUniformValueValid = false;
			bCompressionEvaluated = false;
			CompressedVoxelData.Empty();
		}
	}

	/** Check if local position is within chunk bounds */
	FORCEINLINE bool IsValidLocalPosition(const FIntVector& LocalPos) const
	{
		return LocalPos.X >= 0 && LocalPos.X < ChunkSize
			&& LocalPos.Y >= 0 && LocalPos.Y < ChunkSize
			&& LocalPos.Z >= 0 && LocalPos.Z < ChunkSize;
	}

	/**
	 * The raw ChunkSize^3 array is present in memory right now (the fine-grained truth).
	 * Use this only where code needs the array materialized *this instant* without decompressing.
	 */
	FORCEINLINE bool IsVoxelDataResident() const
	{
		return VoxelData.Num() == GetTotalVoxels();
	}

	/**
	 * Voxel data exists in some form (resident, or a compact uniform/compressed form that
	 * EnsureResident() can materialize). This is the predicate for "is it safe to mesh/read against
	 * this chunk" — a compressed neighbor is still available. PR A: equivalent to IsVoxelDataResident().
	 */
	FORCEINLINE bool HasVoxelDataAvailable() const
	{
		return IsVoxelDataResident()
			|| Residency == EVoxelDataResidency::Uniform
			|| Residency == EVoxelDataResidency::Compressed;
	}

	/**
	 * Materialize the raw voxel array in place if it is held in a compact form, and return it.
	 * All resident-array access funnels through here so later tiers decompress transparently.
	 * Game-thread only. PR A: only Empty|Resident occur, so this is a pass-through.
	 */
	TArray<FVoxelData>& EnsureResident()
	{
		if (Residency == EVoxelDataResidency::Uniform)
		{
			// Expand the single value back into a full array. UniformValue stays valid — the array is
			// exactly UniformValue everywhere until mutated — so re-collapsing is free (see TryCollapseUniform).
			VoxelData.Init(UniformValue, GetTotalVoxels());
			Residency = EVoxelDataResidency::Resident;
			bDataMutated = false;
			bUniformValueValid = true;
		}
		else if (Residency == EVoxelDataResidency::Compressed)
		{
			// Decompress in place. CompressedVoxelData is retained (still valid until mutated) so a
			// re-qualifying chunk re-compresses for free by just dropping the raw array again.
			FVoxelChunkCodec::Decompress(CompressedVoxelData, VoxelData);
			Residency = EVoxelDataResidency::Resident;
			bDataMutated = false;
		}
		return VoxelData;
	}

	/**
	 * Sweep-side compression: collapse a resident chunk whose voxels are all identical into
	 * Residency::Uniform + UniformValue, dropping the ~1MB raw array. Returns true if collapsed.
	 * A chunk with a still-valid UniformValue (expanded-but-unmutated) re-collapses for free; a
	 * non-uniform chunk is marked evaluated so the sweep won't re-scan it until its content changes.
	 * Game-thread only.
	 */
	bool TryCollapseUniform()
	{
		if (Residency != EVoxelDataResidency::Resident)
		{
			return false;
		}
		if (bUniformValueValid)
		{
			VoxelData.Empty();
			Residency = EVoxelDataResidency::Uniform;
			return true;
		}
		bCompressionEvaluated = true;
		FVoxelData Value;
		if (ComputeUniformValue(VoxelData, Value))
		{
			UniformValue = Value;
			bUniformValueValid = true;
			VoxelData.Empty();
			Residency = EVoxelDataResidency::Uniform;
			return true;
		}
		return false;
	}

	/**
	 * Sweep-side compression entry point. Compacts a resident chunk into the smallest available form:
	 * a free re-collapse/re-compress if a compact form is already cached (chunk was expanded but not
	 * mutated), else a uniform collapse, else the general codec. Non-collapsible, poorly-compressing
	 * chunks stay Resident and are marked evaluated so the sweep skips them. Returns true if compacted.
	 * Game-thread only.
	 */
	bool TryCompress(EVoxelChunkCodec Codec)
	{
		if (Residency != EVoxelDataResidency::Resident)
		{
			return false;
		}
		// Free re-collapse to Uniform (cached single value, unmutated) — no scan.
		if (bUniformValueValid)
		{
			VoxelData.Empty();
			Residency = EVoxelDataResidency::Uniform;
			return true;
		}
		// Free re-compress (cached buffer, unmutated) — no re-encode.
		if (CompressedVoxelData.Num() > 0)
		{
			VoxelData.Empty();
			Residency = EVoxelDataResidency::Compressed;
			return true;
		}
		// Fresh chunk: try the cheap uniform collapse first (also marks non-uniform as evaluated).
		if (TryCollapseUniform())
		{
			return true;
		}
		// General codec for a non-uniform chunk (skipped in uniform-only mode).
		if (Codec != EVoxelChunkCodec::Uniform && Codec != EVoxelChunkCodec::Raw)
		{
			const int32 RawBytes = VoxelData.Num() * sizeof(FVoxelData);
			TArray<uint8> Buffer;
			if (FVoxelChunkCodec::Compress(VoxelData, Codec, ChunkSize, Buffer) && Buffer.Num() < RawBytes)
			{
				CompressedVoxelData = MoveTemp(Buffer);
				VoxelData.Empty();
				Residency = EVoxelDataResidency::Compressed;
				bDataMutated = false;
				return true;
			}
		}
		return false;
	}

	/** True iff every voxel in Data is identical; writes that value to Out. False for an empty array. */
	static bool ComputeUniformValue(const TArray<FVoxelData>& Data, FVoxelData& Out)
	{
		if (Data.Num() == 0)
		{
			return false;
		}
		const FVoxelData First = Data[0];
		for (int32 i = 1; i < Data.Num(); ++i)
		{
			if (Data[i] != First)
			{
				return false;
			}
		}
		Out = First;
		return true;
	}

	/** Read access: guarantees residency, returns the array. Callers copy or read without mutating. */
	FORCEINLINE const TArray<FVoxelData>& GetVoxelDataForRead()
	{
		return EnsureResident();
	}

	/**
	 * Const read access for const call sites (point queries, const neighbor-state pointers).
	 * Lazily materializing the array is logically const — a cache fill, not an observable change —
	 * so a scoped const_cast is the correct idiom. PR A: pass-through.
	 */
	FORCEINLINE const TArray<FVoxelData>& GetVoxelDataForRead() const
	{
		return const_cast<FChunkDescriptor*>(this)->EnsureResident();
	}

	/**
	 * Write access: guarantees residency and flags the payload mutated so re-compression re-encodes
	 * rather than reusing a stale buffer. Use for in-place mutation (e.g. water-flag propagation).
	 */
	FORCEINLINE TArray<FVoxelData>& GetVoxelDataMutable()
	{
		TArray<FVoxelData>& Data = EnsureResident();
		bDataMutated = true;
		bUniformValueValid = false;
		bCompressionEvaluated = false;
		CompressedVoxelData.Empty();
		return Data;
	}

	/** Point-query accessor: guarantees residency (lazy-decompresses), then returns one voxel. */
	FORCEINLINE FVoxelData GetVoxelResident(const FIntVector& LocalPos) const
	{
		const_cast<FChunkDescriptor*>(this)->EnsureResident();
		return GetVoxel(LocalPos);
	}

	/** Get memory usage in bytes */
	SIZE_T GetMemoryUsage() const
	{
		return sizeof(FChunkDescriptor) + VoxelData.GetAllocatedSize() + CompressedVoxelData.GetAllocatedSize();
	}

	/** Unique identifier combining coords and LOD */
	FORCEINLINE uint64 GetUniqueID() const
	{
		// Pack coords (16 bits each, signed) and LOD (8 bits) into 64 bits
		uint64 X = static_cast<uint16>(ChunkCoord.X);
		uint64 Y = static_cast<uint16>(ChunkCoord.Y);
		uint64 Z = static_cast<uint16>(ChunkCoord.Z);
		uint64 LOD = static_cast<uint8>(LODLevel);
		return (X) | (Y << 16) | (Z << 32) | (LOD << 48);
	}
};
