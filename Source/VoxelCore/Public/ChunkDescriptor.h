// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.h"
#include "VoxelData.h"
#include "VoxelChunkCodec.h"
#include "VoxelResidencyStats.h"
#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ChunkDescriptor.generated.h"

/**
 * A chunk's compact form copied off its descriptor, so a worker can expand it without touching the descriptor (world
 * modes review G65): the snapshot of a far-compressed chunk is expanded by a task that the mesh jobs, seam jobs and
 * collision cooks reading it wait for on their workers.
 */
struct FVoxelCompactSource
{
	/** Uniform or Compressed; anything else is not a source. */
	EVoxelDataResidency Residency = EVoxelDataResidency::Empty;

	/** The value of a Uniform chunk. */
	FVoxelData UniformValue;

	/** The FVoxelChunkCodec buffer of a Compressed chunk. */
	TArray<uint8> Buffer;

	/** ChunkSize^3. */
	int32 TotalVoxels = 0;

	bool IsValid() const
	{
		return Residency == EVoxelDataResidency::Uniform || Residency == EVoxelDataResidency::Compressed;
	}

	/**
	 * Expand into Out (any thread).
	 *
	 * @param Out Receives the ChunkSize^3 array
	 * @return False if this is not a source or the buffer did not decode to the chunk's size
	 */
	bool Expand(TArray<FVoxelData>& Out) const
	{
		if (Residency == EVoxelDataResidency::Uniform)
		{
			Out.Init(UniformValue, TotalVoxels);
			return true;
		}
		if (Residency == EVoxelDataResidency::Compressed)
		{
			FVoxelChunkCodec::Decompress(Buffer, Out);
			return Out.Num() == TotalVoxels;
		}
		return false;
	}
};

/**
 * The compact form a chunk's voxel array reduces to (far-chunk compression). Computed from the
 * array by FChunkDescriptor::ComputeCompactForm, which is pure and runs on any thread, and installed
 * on the game thread by FChunkDescriptor::InstallCompactForm.
 */
struct FVoxelChunkCompactForm
{
	enum class EKind : uint8
	{
		/** Neither the uniform collapse nor the codec beat the raw size: the chunk stays resident. */
		Incompressible,
		/** Every voxel equals UniformValue. */
		Uniform,
		/** Buffer holds an FVoxelChunkCodec buffer smaller than the raw array. */
		Compressed,
	};

	EKind Kind = EKind::Incompressible;

	/** The single value of a Uniform array. */
	FVoxelData UniformValue;

	/** [header][payload] codec buffer of a Compressed array. */
	TArray<uint8> Buffer;
};

/**
 * Chunk metadata and voxel storage.
 *
 * Contains all data needed to describe a chunk's state and contents.
 * VoxelArray is sized to ChunkSize^3 elements (shared copy-on-write with its readers, G71).
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

	/**
	 * The resident voxel array (ChunkSize^3 elements), or null. Shared copy-on-write (world modes review G71): a snapshot
	 * of a resident chunk without edits (seam jobs, collision cooks, the water pull) and the far-compression encode hold
	 * this array instead of a copy, and a write (GetVoxelDataMutable, SetVoxel, SetVoxelByIndex) first clones it while
	 * anything else holds it, so a reader's array never changes under it. A copy of the descriptor shares it the same way.
	 * Read it through GetResidentArray / GetVoxelDataForRead and never write through it directly. Not reflected.
	 */
	TSharedPtr<TArray<FVoxelData>> VoxelArray;

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
	 * Where the voxel payload currently lives. The raw VoxelArray remains the fine-grained
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

	/**
	 * Monotonic content-identity counter: bumped on every change to the LOGICAL voxel content
	 * (generation-install, allocate, per-voxel edit, and — via BumpContentVersion() at the
	 * chunk-manager edit hook — edit-layer changes), and deliberately NOT on residency
	 * (de)compression (EnsureResident / TryCollapse* preserve content). Unlike bDataMutated (a flag
	 * that toggles) this only ever increases, so a reader can snapshot it and later detect "did this
	 * chunk's content change since?" — used by the mesh completion-time boundary revalidation (a
	 * neighbour that gained/changed data while a chunk was in-flight meshing) and reserved as the
	 * invalidation key for neighbour boundary-slice memoization. Transient, not serialized.
	 */
	uint32 ContentVersion = 0;

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

	/**
	 * Non-zero while an async far-compression job is encoding a copy of the resident array (the
	 * job's ticket). Every write to the array and every residency change clears it, so the job's
	 * result is installed only if the array is still exactly the copy it encoded. ContentVersion is
	 * not enough for that: water-flag propagation writes through GetVoxelDataMutable() without
	 * bumping it, and a reloaded chunk starts its count again. Transient, not serialized.
	 */
	uint32 AsyncCompactTicket = 0;

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
		VoxelArray = MakeShared<TArray<FVoxelData>>();
		VoxelArray->SetNumZeroed(TotalVoxels);
		Residency = EVoxelDataResidency::Resident;
		bDataMutated = false;
		bUniformValueValid = false;
		bCompressionEvaluated = false;
		CompressedVoxelData.Empty();
		AsyncCompactTicket = 0;
		++ContentVersion;
	}

	/** Install a freshly generated resident voxel array (the sole population point). */
	void SetResidentVoxelData(TArray<FVoxelData>&& InData)
	{
		// A new array: a reader of the old one keeps it.
		VoxelArray = MakeShared<TArray<FVoxelData>>(MoveTemp(InData));
		Residency = EVoxelDataResidency::Resident;
		bDataMutated = false;
		bUniformValueValid = false;
		bCompressionEvaluated = false;
		CompressedVoxelData.Empty();
		AsyncCompactTicket = 0;
		++ContentVersion;
	}

	/** Clear voxel data to free memory */
	void ClearVoxelData()
	{
		VoxelArray.Reset();
		Residency = EVoxelDataResidency::Empty;
		bDataMutated = false;
		bUniformValueValid = false;
		bCompressionEvaluated = false;
		CompressedVoxelData.Empty();
		AsyncCompactTicket = 0;
		// NOTE: no ContentVersion bump — clearing is a memory teardown, not a logical content change.
		// Boundary revalidation ignores a neighbour that no longer has data (re-meshing against a
		// gone neighbour would only clamp), so a version bump here would cause pointless remeshes.
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

	/** The resident array as it is (an empty array when none is resident); no residency change. Read-only. */
	FORCEINLINE const TArray<FVoxelData>& GetResidentArray() const
	{
		static const TArray<FVoxelData> Empty;
		return VoxelArray.IsValid() ? *VoxelArray : Empty;
	}

	/** Get voxel at local position */
	FORCEINLINE FVoxelData GetVoxel(const FIntVector& LocalPos) const
	{
		const int32 Index = GetVoxelIndex(LocalPos);
		const TArray<FVoxelData>& Data = GetResidentArray();
		return Data.IsValidIndex(Index) ? Data[Index] : FVoxelData::Air();
	}

	/** Set voxel at local position */
	FORCEINLINE void SetVoxel(const FIntVector& LocalPos, const FVoxelData& Data)
	{
		const int32 Index = GetVoxelIndex(LocalPos);
		if (GetResidentArray().IsValidIndex(Index))
		{
			DetachVoxelArray()[Index] = Data;
			bIsDirty = true;
			bDataMutated = true;
			bUniformValueValid = false;
			bCompressionEvaluated = false;
			CompressedVoxelData.Empty();
			AsyncCompactTicket = 0;
			++ContentVersion;
		}
	}

	/**
	 * Advance ContentVersion for a LOGICAL content change that lives OUTSIDE this descriptor's own
	 * array — i.e. the edit-manager's edit layer. Consumers key edit-MERGED content off ContentVersion
	 * (seam registry staleness, the shared seam/collision voxel-snapshot cache, completion-time
	 * boundary revalidation), so an edit-layer change must bump it even though VoxelArray is untouched.
	 * Deliberately does NOT set bDataMutated / clear the uniform+compression caches: the procedural
	 * array really is unchanged, and edits are merged over it at snapshot-build time.
	 */
	FORCEINLINE void BumpContentVersion()
	{
		++ContentVersion;
	}

	/** Get voxel by linear index */
	FORCEINLINE FVoxelData GetVoxelByIndex(int32 Index) const
	{
		const TArray<FVoxelData>& Data = GetResidentArray();
		return Data.IsValidIndex(Index) ? Data[Index] : FVoxelData::Air();
	}

	/** Set voxel by linear index */
	FORCEINLINE void SetVoxelByIndex(int32 Index, const FVoxelData& Data)
	{
		if (GetResidentArray().IsValidIndex(Index))
		{
			DetachVoxelArray()[Index] = Data;
			bIsDirty = true;
			bDataMutated = true;
			bUniformValueValid = false;
			bCompressionEvaluated = false;
			CompressedVoxelData.Empty();
			AsyncCompactTicket = 0;
			++ContentVersion;
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
		return GetResidentArray().Num() == GetTotalVoxels();
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
	 * Read-only: writes go through GetVoxelDataMutable, which clones a shared array first (G71). Game-thread only.
	 */
	const TArray<FVoxelData>& EnsureResident()
	{
		if (Residency == EVoxelDataResidency::Uniform)
		{
			// Expand the single value back into a full array. UniformValue stays valid — the array is
			// exactly UniformValue everywhere until mutated — so re-collapsing is free (see TryCollapseUniform).
			// A new array: the compact form dropped the old one.
			TRACE_CPUPROFILER_EVENT_SCOPE(VoxelChunk_ExpandUniform);
			const uint64 StartCycles = FPlatformTime::Cycles64();
			VoxelArray = MakeShared<TArray<FVoxelData>>();
			VoxelArray->Init(UniformValue, GetTotalVoxels());
			Residency = EVoxelDataResidency::Resident;
			bDataMutated = false;
			bUniformValueValid = true;
			VoxelResidencyStats::RecordExpand(false, FPlatformTime::Cycles64() - StartCycles, ChunkCoord);
		}
		else if (Residency == EVoxelDataResidency::Compressed)
		{
			// Decompress in place. CompressedVoxelData is retained (still valid until mutated) so a
			// re-qualifying chunk re-compresses for free by just dropping the raw array again.
			TRACE_CPUPROFILER_EVENT_SCOPE(VoxelChunk_Decompress);
			const uint64 StartCycles = FPlatformTime::Cycles64();
			VoxelArray = MakeShared<TArray<FVoxelData>>();
			FVoxelChunkCodec::Decompress(CompressedVoxelData, *VoxelArray);
			Residency = EVoxelDataResidency::Resident;
			bDataMutated = false;
			VoxelResidencyStats::RecordExpand(true, FPlatformTime::Cycles64() - StartCycles, ChunkCoord);
		}
		return GetResidentArray();
	}

	/**
	 * The resident array to share with a reader instead of a copy (G71): it never changes while the reader holds it (a
	 * write clones it first). Expands a compact chunk in place, as EnsureResident does. Game-thread only.
	 *
	 * @return The chunk's array, or null when it has no voxel data
	 */
	TSharedPtr<const TArray<FVoxelData>> ShareVoxelData()
	{
		EnsureResident();
		return VoxelArray;
	}

	/**
	 * Whether Array is the chunk's resident array (a reader sharing it holds no voxels of its own).
	 *
	 * @param Array A reader's array
	 * @return True if it is this chunk's current resident array
	 */
	FORCEINLINE bool SharesVoxelArray(const TArray<FVoxelData>* Array) const
	{
		return Array && VoxelArray.IsValid() && VoxelArray.Get() == Array;
	}

	/**
	 * The resident array for writing: cloned first while a reader shares it (G71), so the reader's stays as it was.
	 * Creates an empty array when there is none. Callers flag the write (bDataMutated and the caches) themselves.
	 */
	TArray<FVoxelData>& DetachVoxelArray()
	{
		if (!VoxelArray.IsValid())
		{
			VoxelArray = MakeShared<TArray<FVoxelData>>();
		}
		else if (!VoxelArray.IsUnique())
		{
			// IsUnique on the game thread is exact enough: a reader that let go meanwhile only costs a needless copy, and
			// no reader can take the array again except from the game thread.
			TRACE_CPUPROFILER_EVENT_SCOPE(VoxelChunk_CopyOnWrite);
			const uint64 StartCycles = FPlatformTime::Cycles64();
			VoxelArray = MakeShared<TArray<FVoxelData>>(*VoxelArray);
			VoxelResidencyStats::RecordSharedArrayClone(FPlatformTime::Cycles64() - StartCycles);
		}
		return *VoxelArray;
	}

	/**
	 * Whether the voxels are held in a compact form (Uniform or Compressed) that a read would expand.
	 */
	FORCEINLINE bool IsCompact() const
	{
		return Residency == EVoxelDataResidency::Uniform || Residency == EVoxelDataResidency::Compressed;
	}

	/**
	 * Copy the compact form out for a worker to expand (G65); the descriptor stays compact. Game-thread only.
	 *
	 * @param Out Receives the form (its Residency stays Empty when there is none)
	 * @return True if the chunk is held Uniform or Compressed
	 */
	bool CopyCompactSource(FVoxelCompactSource& Out) const
	{
		Out = FVoxelCompactSource();
		Out.TotalVoxels = GetTotalVoxels();
		if (Residency == EVoxelDataResidency::Uniform)
		{
			Out.Residency = Residency;
			Out.UniformValue = UniformValue;
			return true;
		}
		if (Residency == EVoxelDataResidency::Compressed)
		{
			Out.Residency = Residency;
			Out.Buffer = CompressedVoxelData;
			return true;
		}
		return false;
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
			VoxelArray.Reset();
			Residency = EVoxelDataResidency::Uniform;
			AsyncCompactTicket = 0;
			return true;
		}
		bCompressionEvaluated = true;
		FVoxelData Value;
		if (ComputeUniformValue(GetResidentArray(), Value))
		{
			UniformValue = Value;
			bUniformValueValid = true;
			VoxelArray.Reset();
			Residency = EVoxelDataResidency::Uniform;
			AsyncCompactTicket = 0;
			return true;
		}
		return false;
	}

	/**
	 * Sweep-side compression entry point. Compacts a resident chunk into the smallest available form:
	 * a free re-collapse/re-compress if a compact form is already cached (chunk was expanded but not
	 * mutated), else a uniform collapse, else the general codec. Non-collapsible, poorly-compressing
	 * chunks stay Resident and are marked evaluated so the sweep skips them. Returns true if compacted.
	 * Game-thread only; the async sweep runs the encode on a worker instead (ComputeCompactForm on a
	 * copy, then InstallCompactForm).
	 *
	 * @param Codec General codec for a non-uniform chunk
	 * @return True if the raw array was dropped
	 */
	bool TryCompress(EVoxelChunkCodec Codec)
	{
		if (Residency != EVoxelDataResidency::Resident)
		{
			return false;
		}
		// Free re-collapse / re-compress (cached form, unmutated): no scan, no re-encode.
		if (bUniformValueValid || CompressedVoxelData.Num() > 0)
		{
			return DropResidentForCachedForm();
		}
		// Fresh chunk: the uniform scan, then the general codec.
		return InstallCompactForm(ComputeCompactForm(GetResidentArray(), Codec, ChunkSize));
	}

	/**
	 * Reduce a voxel array to its smallest compact form: Uniform if every voxel is identical, else the
	 * general codec's buffer if it beats the raw size (no codec for EVoxelChunkCodec::Uniform / Raw).
	 * Pure: it reads only Data, so it runs on any thread.
	 *
	 * @param Data      Voxel array to reduce (ChunkSize^3 elements)
	 * @param Codec     General codec for non-uniform data
	 * @param ChunkSize Chunk edge length, recorded in the codec buffer's header
	 * @return The compact form (Incompressible when neither beats the raw array)
	 */
	static FVoxelChunkCompactForm ComputeCompactForm(const TArray<FVoxelData>& Data, EVoxelChunkCodec Codec, int32 ChunkSize)
	{
		FVoxelChunkCompactForm Form;
		if (ComputeUniformValue(Data, Form.UniformValue))
		{
			Form.Kind = FVoxelChunkCompactForm::EKind::Uniform;
			return Form;
		}
		if (Codec != EVoxelChunkCodec::Uniform && Codec != EVoxelChunkCodec::Raw)
		{
			const int32 RawBytes = Data.Num() * sizeof(FVoxelData);
			if (FVoxelChunkCodec::Compress(Data, Codec, ChunkSize, Form.Buffer) && Form.Buffer.Num() < RawBytes)
			{
				Form.Kind = FVoxelChunkCompactForm::EKind::Compressed;
				return Form;
			}
			Form.Buffer.Empty();
		}
		return Form;
	}

	/**
	 * Install a compact form computed from this chunk's current resident array. The form becomes the
	 * chunk's cached compact form (the uniform value, the retained buffer, or the "evaluated" mark when
	 * nothing beat raw), valid until the array is next written; with bDropResident the raw array is then
	 * dropped, as TryCompress does. The caller guarantees Form was computed from exactly the current
	 * array: TryCompress computes it in place, the async sweep checks AsyncCompactTicket. Game-thread only.
	 *
	 * @param Form          Compact form of the current resident array
	 * @param bDropResident Drop the raw array now; false only caches the form (TryCompress applies it later for free)
	 * @return True if the raw array was dropped
	 */
	bool InstallCompactForm(FVoxelChunkCompactForm&& Form, bool bDropResident = true)
	{
		if (Residency != EVoxelDataResidency::Resident)
		{
			return false;
		}
		AsyncCompactTicket = 0;
		bCompressionEvaluated = true;
		switch (Form.Kind)
		{
		case FVoxelChunkCompactForm::EKind::Uniform:
			UniformValue = Form.UniformValue;
			bUniformValueValid = true;
			break;
		case FVoxelChunkCompactForm::EKind::Compressed:
			CompressedVoxelData = MoveTemp(Form.Buffer);
			// The retained buffer now matches the array, as after a decompress.
			bDataMutated = false;
			break;
		default:
			return false;
		}
		return bDropResident && DropResidentForCachedForm();
	}

	/**
	 * Drop the raw array in favour of the cached compact form (bUniformValueValid or a retained
	 * CompressedVoxelData), which writes clear, so it still matches the array. Game-thread only.
	 *
	 * @return True if the raw array was dropped; false when no compact form is cached
	 */
	bool DropResidentForCachedForm()
	{
		if (Residency != EVoxelDataResidency::Resident)
		{
			return false;
		}
		// A reader sharing the array keeps it.
		if (bUniformValueValid)
		{
			VoxelArray.Reset();
			Residency = EVoxelDataResidency::Uniform;
		}
		else if (CompressedVoxelData.Num() > 0)
		{
			VoxelArray.Reset();
			Residency = EVoxelDataResidency::Compressed;
		}
		else
		{
			return false;
		}
		AsyncCompactTicket = 0;
		return true;
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
	 * rather than reusing a stale buffer. Use for in-place mutation (e.g. water-flag propagation). A shared array is
	 * cloned first (G71): drop the readers that need not keep the old voxels (a cached snapshot) before calling this.
	 */
	FORCEINLINE TArray<FVoxelData>& GetVoxelDataMutable()
	{
		EnsureResident();
		TArray<FVoxelData>& Data = DetachVoxelArray();
		bDataMutated = true;
		bUniformValueValid = false;
		bCompressionEvaluated = false;
		CompressedVoxelData.Empty();
		AsyncCompactTicket = 0;
		// NOTE: deliberately no ContentVersion bump here. The only streaming caller is water-flag
		// propagation, which (a) is a material change, not the density change that causes geometric
		// boundary seams, and (b) calls this unconditionally on every coastal mesh submit — bumping
		// would churn neighbour boundary revalidation for no geometric benefit. ContentVersion tracks
		// generation + explicit voxel edits (the geometry that boundary meshing depends on). If a future
		// caller mutates density through this accessor, bump ContentVersion at that call site.
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
		return sizeof(FChunkDescriptor) + GetResidentArray().GetAllocatedSize() + CompressedVoxelData.GetAllocatedSize();
	}
};
