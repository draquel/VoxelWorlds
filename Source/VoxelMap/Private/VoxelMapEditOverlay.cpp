// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMapEditOverlay.h"
#include "VoxelSurfaceQuery.h"
#include "Algo/BinarySearch.h"
#include "Serialization/Archive.h"

namespace
{
	/** Largest supported tile edge: column indices are stored as uint16. */
	constexpr int32 MaxOverlayResolution = 256;

	bool IsBorderColumn(int32 Column, int32 Resolution)
	{
		const int32 PX = Column % Resolution;
		const int32 PY = Column / Resolution;
		return PX == 0 || PY == 0 || PX == Resolution - 1 || PY == Resolution - 1;
	}

	/**
	 * Index of the solid voxel under the crossing ExtractSurfaceFromColumn reported. It returns
	 * Height = BaseZ + (i + T) * VoxelSize with T in [0, 1], so floor() lands on i, or on i + 1 when T is 1, or one
	 * below through rounding. Only one of three neighbouring indices can be a solid-under-air crossing.
	 */
	int32 FindCrossingIndex(const TArray<FVoxelData>& Column, float Height, float BaseZ, float VoxelSize)
	{
		const int32 Guess = FMath::Clamp(FMath::FloorToInt((Height - BaseZ) / VoxelSize), 0, Column.Num() - 2);
		for (const int32 Candidate : { Guess, Guess - 1, Guess + 1 })
		{
			if (Candidate >= 0 && Candidate + 1 < Column.Num() && Column[Candidate].IsSolid() && Column[Candidate + 1].IsAir())
			{
				return Candidate;
			}
		}
		return INDEX_NONE;
	}
}

int32 FVoxelMapEditOverlay::FindColumn(int32 PX, int32 PY) const
{
	if (Resolution <= 0 || PX < 0 || PY < 0 || PX >= Resolution || PY >= Resolution)
	{
		return INDEX_NONE;
	}
	return Algo::BinarySearch(Columns, static_cast<uint16>(PY * Resolution + PX));
}

bool FVoxelMapEditOverlay::IsPending(int32 PX, int32 PY) const
{
	if (Resolution <= 0 || PX < 0 || PY < 0 || PX >= Resolution || PY >= Resolution)
	{
		return false;
	}
	return Algo::BinarySearch(PendingColumns, static_cast<uint16>(PY * Resolution + PX)) != INDEX_NONE;
}

FVoxelMapEditOverlay FVoxelMapEditOverlay::Build(
	const FVoxelMapOverlayBuildInput& Input,
	TConstArrayView<float> AnalyticHeights,
	TFunctionRef<uint8(int32 PX, int32 PY)> AnalyticMaterial)
{
	FVoxelMapEditOverlay Out;
	const int32 CS = Input.ChunkSize;
	const float VS = Input.VoxelSize;
	Out.Resolution = CS;
	if (CS <= 1 || CS > MaxOverlayResolution || VS <= 0.0f || AnalyticHeights.Num() != CS * CS)
	{
		return Out;
	}
	const int32 NumColumns = CS * CS;
	const int32 VolumeSize = NumColumns * CS;
	const float ChunkWorldSize = CS * VS;

	// The stack, sorted by Z. Malformed voxel arrays read as not loaded; malformed edit tables as unedited.
	TArray<const FVoxelMapOverlayChunk*> Stack;
	for (const FVoxelMapOverlayChunk& Chunk : Input.Chunks)
	{
		Stack.Add(&Chunk);
	}
	Stack.Sort([](const FVoxelMapOverlayChunk& A, const FVoxelMapOverlayChunk& B) { return A.ChunkZ < B.ChunkZ; });
	auto FindChunk = [&Stack](int32 ChunkZ) -> const FVoxelMapOverlayChunk*
	{
		for (const FVoxelMapOverlayChunk* Chunk : Stack)
		{
			if (Chunk->ChunkZ == ChunkZ)
			{
				return Chunk;
			}
		}
		return nullptr;
	};
	auto HasVoxels = [VolumeSize](const FVoxelMapOverlayChunk* Chunk)
	{
		return Chunk && Chunk->Voxels.IsValid() && Chunk->Voxels->Num() == VolumeSize;
	};
	auto TopEditedAt = [NumColumns](const FVoxelMapOverlayChunk* Chunk, int32 Column) -> int32
	{
		return (Chunk && Chunk->TopEditedZ.Num() == NumColumns) ? Chunk->TopEditedZ[Column] : -1;
	};

	TArray<FVoxelData> Column;
	TArray<bool, TInlineAllocator<16>> ChunkIsReal;

	for (int32 Col = 0; Col < NumColumns; ++Col)
	{
		// The column's edited extent across the stack, as global voxel indices (ChunkZ * CS + local Z).
		int64 TopEdited = MIN_int64;
		int32 MinEditedChunk = MAX_int32;
		int32 MaxEditedChunk = MIN_int32;
		for (const FVoxelMapOverlayChunk* Chunk : Stack)
		{
			const int32 LocalZ = TopEditedAt(Chunk, Col);
			if (LocalZ >= 0)
			{
				TopEdited = FMath::Max(TopEdited, static_cast<int64>(Chunk->ChunkZ) * CS + LocalZ);
				MinEditedChunk = FMath::Min(MinEditedChunk, Chunk->ChunkZ);
				MaxEditedChunk = FMath::Max(MaxEditedChunk, Chunk->ChunkZ);
			}
		}
		if (TopEdited == MIN_int64)
		{
			continue;
		}

		const int32 PX = Col % CS;
		const int32 PY = Col / CS;
		const float AnalyticHeight = AnalyticHeights[Col];
		const int32 AnalyticChunk = FMath::FloorToInt((AnalyticHeight - Input.WorldOriginZ) / ChunkWorldSize);

		// Scan from one chunk above the highest edited (or analytic surface) chunk, so a build reaching the top of
		// its chunk still has air above it, down to one chunk below the lowest, so a dig through the bottom of the
		// edited range meets the ground under it. Stop above the first chunk this column cannot read: one that is
		// edited here but has no data.
		const int32 TopChunk = FMath::Max(MaxEditedChunk, AnalyticChunk) + 1;
		const int32 BottomChunk = FMath::Min(MinEditedChunk, AnalyticChunk) - 1;
		int32 LowChunk = TopChunk;
		for (int32 ChunkZ = TopChunk; ChunkZ >= BottomChunk; --ChunkZ)
		{
			const FVoxelMapOverlayChunk* Chunk = FindChunk(ChunkZ);
			if (Chunk && !HasVoxels(Chunk) && TopEditedAt(Chunk, Col) >= 0)
			{
				break;
			}
			LowChunk = ChunkZ;
		}

		// Assemble the column bottom-up. Chunks without data that this column does not edit read as the analytic
		// terrain: a linear density across the analytic height, so the interpolated crossing lands on it.
		const int32 NumChunks = TopChunk - LowChunk + 1;
		Column.SetNumUninitialized(NumChunks * CS);
		ChunkIsReal.SetNumUninitialized(NumChunks);
		for (int32 ChunkIndex = 0; ChunkIndex < NumChunks; ++ChunkIndex)
		{
			const int32 ChunkZ = LowChunk + ChunkIndex;
			const FVoxelMapOverlayChunk* Chunk = FindChunk(ChunkZ);
			const int32 Base = ChunkIndex * CS;
			ChunkIsReal[ChunkIndex] = HasVoxels(Chunk);
			if (ChunkIsReal[ChunkIndex])
			{
				const TArray<FVoxelData>& Voxels = *Chunk->Voxels;
				for (int32 LocalZ = 0; LocalZ < CS; ++LocalZ)
				{
					Column[Base + LocalZ] = Voxels[PX + PY * CS + LocalZ * NumColumns];
				}
			}
			else
			{
				for (int32 LocalZ = 0; LocalZ < CS; ++LocalZ)
				{
					const float Z = Input.WorldOriginZ + (static_cast<float>(ChunkZ) * CS + LocalZ) * VS;
					FVoxelData Voxel = FVoxelData::Air();
					Voxel.Density = static_cast<uint8>(FMath::Clamp(
						FMath::FloorToInt(VOXEL_SURFACE_THRESHOLD + (AnalyticHeight - Z) / VS * 128.0f), 0, 255));
					if (Voxel.IsAir() && Input.bUseWater && Z < Input.WaterLevel)
					{
						Voxel.SetWaterFlag(true);
					}
					Column[Base + LocalZ] = Voxel;
				}
			}
		}

		const float BaseZ = Input.WorldOriginZ + static_cast<float>(LowChunk) * ChunkWorldSize;
		float Height = 0.0f;
		uint8 MaterialID = 0;
		uint8 BiomeID = 0;
		const int32 Crossing = FVoxelSurfaceQuery::ExtractSurfaceFromColumn(Column, BaseZ, VS, Height, MaterialID, BiomeID)
			? FindCrossingIndex(Column, Height, BaseZ, VS)
			: INDEX_NONE;
		if (Crossing == INDEX_NONE)
		{
			// No surface above the first unreadable chunk or the bottom of the stack: dug through.
			Out.PendingColumns.Add(static_cast<uint16>(Col));
			continue;
		}

		// Every edit lies below the surface voxel: the top surface is untouched (dungeon interiors, buried rooms).
		if (TopEdited < static_cast<int64>(LowChunk) * CS + Crossing)
		{
			continue;
		}

		if (!ChunkIsReal[Crossing / CS])
		{
			// The surface sits in a chunk read as analytic. That is the real surface when the analytic height lies
			// between this voxel and the next; when the real voxel above is air although the analytic terrain is
			// solid there, an edit opened the column into a chunk that is not loaded.
			const float ZAbove = BaseZ + (Crossing + 1) * VS;
			if (AnalyticHeight >= ZAbove)
			{
				Out.PendingColumns.Add(static_cast<uint16>(Col));
			}
			continue;
		}

		// Water: a water-flagged voxel in the air run above the surface. A pit dug into the seabed is water although
		// the voxels it opened are not flagged yet, because the sea above them is; a pit dug on land below the water
		// line has only dry air above it and stays land until water propagation floods it.
		bool bWater = false;
		for (int32 Above = Crossing + 1; Above < Column.Num() && Column[Above].IsAir(); ++Above)
		{
			if (Column[Above].HasWaterFlag())
			{
				bWater = true;
				break;
			}
		}
		const bool bAnalyticWater = Input.bUseWater && AnalyticHeight < Input.WaterLevel;
		if (FMath::Abs(Height - AnalyticHeight) <= 0.5f * VS && bWater == bAnalyticWater
			&& MaterialID == AnalyticMaterial(PX, PY))
		{
			// The edit left the surface as the analytic map already draws it (e.g. a sub-threshold brush).
			continue;
		}

		Out.Columns.Add(static_cast<uint16>(Col));
		Out.Heights.Add(Height);
		Out.Materials.Add(MaterialID);
		Out.Flags.Add(bWater ? FlagWater : 0);
	}
	return Out;
}

FVoxelMapEditOverlay FVoxelMapEditOverlay::MergePending(const FVoxelMapEditOverlay& Built, const FVoxelMapEditOverlay* Previous)
{
	if (!Previous || Previous->IsEmpty() || Built.PendingColumns.Num() == 0 || Previous->Resolution != Built.Resolution)
	{
		return Built;
	}

	FVoxelMapEditOverlay Out;
	Out.Resolution = Built.Resolution;
	Out.PendingColumns = Built.PendingColumns;
	const int32 Capacity = Built.Num() + FMath::Min(Previous->Num(), Built.PendingColumns.Num());
	Out.Columns.Reserve(Capacity);
	Out.Heights.Reserve(Capacity);
	Out.Materials.Reserve(Capacity);
	Out.Flags.Reserve(Capacity);

	// Three ascending streams: the fresh columns, the previous columns, the pending columns. A column is never both
	// masked and pending in a build, so taking previous entries only at pending columns cannot duplicate one.
	int32 BuiltIndex = 0;
	int32 PrevIndex = 0;
	int32 PendingIndex = 0;
	auto TakeBuilt = [&]()
	{
		Out.Columns.Add(Built.Columns[BuiltIndex]);
		Out.Heights.Add(Built.Heights[BuiltIndex]);
		Out.Materials.Add(Built.Materials[BuiltIndex]);
		Out.Flags.Add(Built.Flags[BuiltIndex]);
		++BuiltIndex;
	};
	while (PrevIndex < Previous->Num())
	{
		const uint16 PrevColumn = Previous->Columns[PrevIndex];
		while (PendingIndex < Built.PendingColumns.Num() && Built.PendingColumns[PendingIndex] < PrevColumn)
		{
			++PendingIndex;
		}
		if (PendingIndex >= Built.PendingColumns.Num())
		{
			break;
		}
		if (Built.PendingColumns[PendingIndex] == PrevColumn)
		{
			while (BuiltIndex < Built.Num() && Built.Columns[BuiltIndex] < PrevColumn)
			{
				TakeBuilt();
			}
			Out.Columns.Add(PrevColumn);
			Out.Heights.Add(Previous->Heights[PrevIndex]);
			Out.Materials.Add(Previous->Materials[PrevIndex]);
			Out.Flags.Add(Previous->Flags[PrevIndex]);
		}
		++PrevIndex;
	}
	while (BuiltIndex < Built.Num())
	{
		TakeBuilt();
	}
	return Out;
}

bool FVoxelMapEditOverlay::BordersEqual(const FVoxelMapEditOverlay* A, const FVoxelMapEditOverlay* B)
{
	static const FVoxelMapEditOverlay Empty;
	const FVoxelMapEditOverlay& L = A ? *A : Empty;
	const FVoxelMapEditOverlay& R = B ? *B : Empty;

	// Walk both in column order, comparing only border entries.
	int32 LI = 0;
	int32 RI = 0;
	while (true)
	{
		while (LI < L.Num() && !IsBorderColumn(L.Columns[LI], L.Resolution))
		{
			++LI;
		}
		while (RI < R.Num() && !IsBorderColumn(R.Columns[RI], R.Resolution))
		{
			++RI;
		}
		const bool bLeftDone = LI >= L.Num();
		const bool bRightDone = RI >= R.Num();
		if (bLeftDone || bRightDone)
		{
			return bLeftDone && bRightDone;
		}
		if (L.Resolution != R.Resolution || L.Columns[LI] != R.Columns[RI] || L.Heights[LI] != R.Heights[RI]
			|| L.Materials[LI] != R.Materials[RI] || L.Flags[LI] != R.Flags[RI])
		{
			return false;
		}
		++LI;
		++RI;
	}
}

bool FVoxelMapEditOverlay::Serialize(FArchive& Ar)
{
	uint8 Version = FormatVersion;
	Ar << Version;
	uint16 SavedResolution = static_cast<uint16>(Resolution);
	Ar << SavedResolution;

	if (Ar.IsLoading())
	{
		auto Fail = [this]()
		{
			*this = FVoxelMapEditOverlay();
			return false;
		};
		if (Ar.IsError() || Version != FormatVersion || SavedResolution < 2 || SavedResolution > MaxOverlayResolution)
		{
			return Fail();
		}
		Resolution = SavedResolution;
		const int32 NumColumns = Resolution * Resolution;
		TArray<uint8> Mask;
		Mask.SetNumZeroed((NumColumns + 7) / 8);
		Ar.Serialize(Mask.GetData(), Mask.Num());
		if (Ar.IsError())
		{
			return Fail();
		}

		Columns.Reset();
		PendingColumns.Reset();
		for (int32 Col = 0; Col < NumColumns; ++Col)
		{
			if (Mask[Col >> 3] & (1 << (Col & 7)))
			{
				Columns.Add(static_cast<uint16>(Col));
			}
		}
		const int32 Count = Columns.Num();
		Heights.SetNumUninitialized(Count);
		Materials.SetNumUninitialized(Count);
		Flags.SetNumUninitialized(Count);
		for (int32 i = 0; i < Count && !Ar.IsError(); ++i)
		{
			Ar << Heights[i];
		}
		if (Count > 0)
		{
			Ar.Serialize(Materials.GetData(), Count);
			Ar.Serialize(Flags.GetData(), Count);
		}
		if (Ar.IsError())
		{
			return Fail();
		}
		return true;
	}

	// Saving: the mask, then the values in column order.
	const int32 NumColumns = Resolution * Resolution;
	TArray<uint8> Mask;
	Mask.SetNumZeroed((NumColumns + 7) / 8);
	for (const uint16 Col : Columns)
	{
		Mask[Col >> 3] |= static_cast<uint8>(1 << (Col & 7));
	}
	Ar.Serialize(Mask.GetData(), Mask.Num());
	for (float& Height : Heights)
	{
		Ar << Height;
	}
	if (Columns.Num() > 0)
	{
		Ar.Serialize(Materials.GetData(), Materials.Num());
		Ar.Serialize(Flags.GetData(), Flags.Num());
	}
	return !Ar.IsError();
}

bool FVoxelMapEditOverlay::operator==(const FVoxelMapEditOverlay& Other) const
{
	return Resolution == Other.Resolution && Columns == Other.Columns && Heights == Other.Heights
		&& Materials == Other.Materials && Flags == Other.Flags;
}
