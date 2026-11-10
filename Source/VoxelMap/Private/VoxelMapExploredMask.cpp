// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMapExploredMask.h"
#include "Serialization/Archive.h"

namespace
{
	/** Format of FVoxelMapExploredMask::Save. */
	constexpr uint8 ExploredMaskVersion = 1;

	/** A saved block's state: full (no bits follow) or partial (its 64 rows follow). */
	constexpr uint8 SavedBlockFull = 2;
	constexpr uint8 SavedBlockPartial = 1;

	constexpr uint64 IndexMask = 0x1FFFFFFFull;

	bool GetBit(const TArray<uint64>& Bits, int32 Index)
	{
		return (Bits[Index >> 6] >> (Index & 63)) & 1ull;
	}

	void SetBitAt(TArray<uint64>& Bits, int32 Index)
	{
		Bits[Index >> 6] |= 1ull << (Index & 63);
	}

	/** Bits [From, To) of a row word. */
	uint64 RowMask(int32 From, int32 To)
	{
		const int32 Width = To - From;
		return Width <= 0 ? 0ull : (Width >= 64 ? ~0ull : ((1ull << Width) - 1ull) << From);
	}

	/** A key's 29-bit index, sign-extended. */
	int32 SignExtend29(uint64 Bits)
	{
		return static_cast<int32>(static_cast<uint32>(Bits & IndexMask) << 3) >> 3;
	}

	void DecodeKey(uint64 Key, int32& OutFace, int32& OutBX, int32& OutBY)
	{
		OutFace = static_cast<int32>((Key >> 58) & 7ull);
		OutBX = SignExtend29(Key >> 29);
		OutBY = SignExtend29(Key);
	}
}

uint64 FVoxelMapExploredMask::KeyOf(int32 Face, int32 BX, int32 BY)
{
	return (static_cast<uint64>(Face & 7) << 58) | ((static_cast<uint64>(static_cast<uint32>(BX)) & IndexMask) << 29)
		| (static_cast<uint64>(static_cast<uint32>(BY)) & IndexMask);
}

void FVoxelMapExploredMask::Configure(int32 InFaces, int32 InFaceTiles)
{
	Faces = FMath::Clamp(InFaces, 1, 6);
	FaceTiles = FMath::Max(0, InFaceTiles);
	Reset();
}

void FVoxelMapExploredMask::Reset()
{
	Blocks.Empty();
	for (TMap<uint64, int32>& Level : Coarse)
	{
		Level.Empty();
	}
	Count = 0;
	FullBlocks = 0;
}

bool FVoxelMapExploredMask::IsInRange(int32 Face, int32 U, int32 V) const
{
	return Face >= 0 && Face < Faces && (FaceTiles <= 0 || (U >= 0 && V >= 0 && U < FaceTiles && V < FaceTiles));
}

int32 FVoxelMapExploredMask::CapacityOf(int32 BX, int32 BY) const
{
	if (FaceTiles <= 0)
	{
		return BlockArea;
	}
	const int32 W = FMath::Clamp(FaceTiles - BX * BlockSize, 0, BlockSize);
	const int32 H = FMath::Clamp(FaceTiles - BY * BlockSize, 0, BlockSize);
	return W * H;
}

bool FVoxelMapExploredMask::SetBit(FBlock& Block, int32 LU, int32 LV)
{
	const int32 Index = LV * BlockSize + LU;
	if (GetBit(Block.Bits, Index))
	{
		return false;
	}
	SetBitAt(Block.Bits, Index);
	// The squares over it: level k's 2^k square holding the tile.
	for (int32 K = 1; K < BlockShift; ++K)
	{
		const int32 Side = BlockSize >> K;
		SetBitAt(Block.Bits, LevelBitOffset[K] + (LV >> K) * Side + (LU >> K));
	}
	return true;
}

void FVoxelMapExploredMask::RebuildSquares(FBlock& Block)
{
	for (int32 Word = LevelBitOffset[1] >> 6; Word < BitWords; ++Word)
	{
		Block.Bits[Word] = 0;
	}
	for (int32 K = 1; K < BlockShift; ++K)
	{
		const int32 Side = BlockSize >> K;
		const int32 Finer = LevelBitOffset[K - 1];
		const int32 FinerSide = Side * 2;
		for (int32 Y = 0; Y < Side; ++Y)
		{
			for (int32 X = 0; X < Side; ++X)
			{
				const int32 F = Finer + (2 * Y) * FinerSide + 2 * X;
				if (GetBit(Block.Bits, F) || GetBit(Block.Bits, F + 1) || GetBit(Block.Bits, F + FinerSide) || GetBit(Block.Bits, F + FinerSide + 1))
				{
					SetBitAt(Block.Bits, LevelBitOffset[K] + Y * Side + X);
				}
			}
		}
	}
}

void FVoxelMapExploredMask::AddCoarse(int32 Face, int32 BX, int32 BY)
{
	for (int32 M = 1; M <= MaxCoarseLevels; ++M)
	{
		++Coarse[M - 1].FindOrAdd(KeyOf(Face, BX >> M, BY >> M));
	}
}

void FVoxelMapExploredMask::RemoveCoarse(int32 Face, int32 BX, int32 BY)
{
	for (int32 M = 1; M <= MaxCoarseLevels; ++M)
	{
		const uint64 Key = KeyOf(Face, BX >> M, BY >> M);
		if (int32* Under = Coarse[M - 1].Find(Key))
		{
			if (--*Under <= 0)
			{
				Coarse[M - 1].Remove(Key);
			}
		}
	}
}

void FVoxelMapExploredMask::CollapseIfFull(FBlock& Block, int32 BX, int32 BY)
{
	if (Block.Bits.Num() > 0 && Block.Count >= CapacityOf(BX, BY))
	{
		Block.Bits.Empty();
		++FullBlocks;
	}
}

bool FVoxelMapExploredMask::Set(int32 Face, int32 U, int32 V)
{
	if (!IsInRange(Face, U, V))
	{
		return false;
	}
	const int32 BX = U >> BlockShift;
	const int32 BY = V >> BlockShift;
	const uint64 Key = KeyOf(Face, BX, BY);
	FBlock* Block = Blocks.Find(Key);
	if (!Block)
	{
		Block = &Blocks.Add(Key);
		Block->Bits.SetNumZeroed(BitWords);
		AddCoarse(Face, BX, BY);
	}
	else if (Block->Bits.Num() == 0)
	{
		return false; // full
	}
	if (!SetBit(*Block, U & (BlockSize - 1), V & (BlockSize - 1)))
	{
		return false;
	}
	++Block->Count;
	++Count;
	CollapseIfFull(*Block, BX, BY);
	return true;
}

int64 FVoxelMapExploredMask::MakeFull(int32 Face, int32 BX, int32 BY)
{
	const int32 Capacity = CapacityOf(BX, BY);
	if (Capacity <= 0)
	{
		return 0;
	}
	const uint64 Key = KeyOf(Face, BX, BY);
	FBlock* Block = Blocks.Find(Key);
	if (!Block)
	{
		Block = &Blocks.Add(Key);
		AddCoarse(Face, BX, BY);
	}
	else if (Block->Bits.Num() == 0)
	{
		return 0;
	}
	const int64 Added = Capacity - Block->Count;
	Block->Count = Capacity;
	Block->Bits.Empty();
	++FullBlocks;
	Count += Added;
	return Added;
}

int64 FVoxelMapExploredMask::OrRows(int32 Face, int32 BX, int32 BY, const uint64 (&Rows)[BlockSize])
{
	bool bAny = false;
	for (int32 Row = 0; Row < BlockSize && !bAny; ++Row)
	{
		bAny = Rows[Row] != 0;
	}
	if (!bAny)
	{
		return 0;
	}
	const uint64 Key = KeyOf(Face, BX, BY);
	FBlock* Block = Blocks.Find(Key);
	if (!Block)
	{
		Block = &Blocks.Add(Key);
		Block->Bits.SetNumZeroed(BitWords);
		AddCoarse(Face, BX, BY);
	}
	else if (Block->Bits.Num() == 0)
	{
		return 0;
	}
	int64 Added = 0;
	for (int32 Row = 0; Row < BlockSize; ++Row)
	{
		const uint64 New = Rows[Row] & ~Block->Bits[Row];
		Added += FMath::CountBits64(New);
		Block->Bits[Row] |= New;
	}
	if (Added > 0)
	{
		Block->Count += static_cast<int32>(Added);
		Count += Added;
		RebuildSquares(*Block);
		CollapseIfFull(*Block, BX, BY);
	}
	return Added;
}

int64 FVoxelMapExploredMask::SetRect(int32 Face, const FIntRect& Rect)
{
	if (Face < 0 || Face >= Faces)
	{
		return 0;
	}
	FIntRect Clipped = Rect;
	if (FaceTiles > 0)
	{
		Clipped.Min = Clipped.Min.ComponentMax(FIntPoint(0, 0));
		Clipped.Max = Clipped.Max.ComponentMin(FIntPoint(FaceTiles, FaceTiles));
	}
	if (Clipped.Max.X <= Clipped.Min.X || Clipped.Max.Y <= Clipped.Min.Y)
	{
		return 0;
	}
	int64 Added = 0;
	const int32 BX0 = Clipped.Min.X >> BlockShift;
	const int32 BX1 = (Clipped.Max.X - 1) >> BlockShift;
	const int32 BY0 = Clipped.Min.Y >> BlockShift;
	const int32 BY1 = (Clipped.Max.Y - 1) >> BlockShift;
	for (int32 BY = BY0; BY <= BY1; ++BY)
	{
		for (int32 BX = BX0; BX <= BX1; ++BX)
		{
			// The rectangle's part of the block, in the block's tiles.
			const int32 X0 = FMath::Max(Clipped.Min.X - BX * BlockSize, 0);
			const int32 X1 = FMath::Min(Clipped.Max.X - BX * BlockSize, BlockSize);
			const int32 Y0 = FMath::Max(Clipped.Min.Y - BY * BlockSize, 0);
			const int32 Y1 = FMath::Min(Clipped.Max.Y - BY * BlockSize, BlockSize);
			if ((X1 - X0) * (Y1 - Y0) >= CapacityOf(BX, BY))
			{
				// Every tile the block holds: the block collapses at once (a face's last blocks hold fewer).
				Added += MakeFull(Face, BX, BY);
				continue;
			}
			uint64 Rows[BlockSize] = {};
			const uint64 Mask = RowMask(X0, X1);
			for (int32 Y = Y0; Y < Y1; ++Y)
			{
				Rows[Y] = Mask;
			}
			Added += OrRows(Face, BX, BY, Rows);
		}
	}
	return Added;
}

bool FVoxelMapExploredMask::IsExplored(int32 Face, int32 U, int32 V) const
{
	return FReader(*this).IsAnyExplored(Face, U, V, 0);
}

bool FVoxelMapExploredMask::IsAnyExplored(int32 Face, int32 U, int32 V, int32 Log2Size) const
{
	return FReader(*this).IsAnyExplored(Face, U, V, Log2Size);
}

FVoxelMapExploredMask::EBlockState FVoxelMapExploredMask::GetBlockState(int32 Face, int32 U, int32 V) const
{
	const FBlock* Block = IsInRange(Face, U, V) ? Blocks.Find(KeyOf(Face, U >> BlockShift, V >> BlockShift)) : nullptr;
	return !Block ? EBlockState::None : (Block->Bits.Num() == 0 ? EBlockState::All : EBlockState::Some);
}

SIZE_T FVoxelMapExploredMask::GetAllocatedSize() const
{
	SIZE_T Bytes = Blocks.GetAllocatedSize();
	for (const TPair<uint64, FBlock>& Pair : Blocks)
	{
		Bytes += Pair.Value.Bits.GetAllocatedSize();
	}
	for (const TMap<uint64, int32>& Level : Coarse)
	{
		Bytes += Level.GetAllocatedSize();
	}
	return Bytes;
}

void FVoxelMapExploredMask::ForEach(TFunctionRef<void(int32 Face, int32 U, int32 V)> Visit) const
{
	for (const TPair<uint64, FBlock>& Pair : Blocks)
	{
		int32 Face = 0, BX = 0, BY = 0;
		DecodeKey(Pair.Key, Face, BX, BY);
		const int32 W = FaceTiles > 0 ? FMath::Clamp(FaceTiles - BX * BlockSize, 0, BlockSize) : BlockSize;
		const int32 H = FaceTiles > 0 ? FMath::Clamp(FaceTiles - BY * BlockSize, 0, BlockSize) : BlockSize;
		const bool bFull = Pair.Value.Bits.Num() == 0;
		for (int32 Y = 0; Y < H; ++Y)
		{
			uint64 Row = bFull ? RowMask(0, W) : Pair.Value.Bits[Y];
			while (Row != 0)
			{
				const int32 X = static_cast<int32>(FMath::CountTrailingZeros64(Row));
				Row &= Row - 1;
				Visit(Face, BX * BlockSize + X, BY * BlockSize + Y);
			}
		}
	}
}

int64 FVoxelMapExploredMask::RemoveIf(TFunctionRef<bool(int32 Face, const FIntRect& BlockTiles)> BlockMayHold,
	TFunctionRef<bool(int32 Face, int32 U, int32 V)> ShouldRemove)
{
	int64 Removed = 0;
	for (auto It = Blocks.CreateIterator(); It; ++It)
	{
		int32 Face = 0, BX = 0, BY = 0;
		DecodeKey(It.Key(), Face, BX, BY);
		const int32 W = FaceTiles > 0 ? FMath::Clamp(FaceTiles - BX * BlockSize, 0, BlockSize) : BlockSize;
		const int32 H = FaceTiles > 0 ? FMath::Clamp(FaceTiles - BY * BlockSize, 0, BlockSize) : BlockSize;
		const FIntPoint Origin(BX * BlockSize, BY * BlockSize);
		if (!BlockMayHold(Face, FIntRect(Origin, Origin + FIntPoint(W, H))))
		{
			continue;
		}
		FBlock& Block = It.Value();
		const bool bWasFull = Block.Bits.Num() == 0;
		if (bWasFull)
		{
			Block.Bits.SetNumZeroed(BitWords);
			for (int32 Y = 0; Y < H; ++Y)
			{
				Block.Bits[Y] = RowMask(0, W);
			}
		}
		int32 Dropped = 0;
		for (int32 Y = 0; Y < H; ++Y)
		{
			uint64 Row = Block.Bits[Y];
			while (Row != 0)
			{
				const int32 X = static_cast<int32>(FMath::CountTrailingZeros64(Row));
				Row &= Row - 1;
				if (ShouldRemove(Face, Origin.X + X, Origin.Y + Y))
				{
					Block.Bits[Y] &= ~(1ull << X);
					++Dropped;
				}
			}
		}
		if (Dropped == 0)
		{
			if (bWasFull)
			{
				Block.Bits.Empty();
			}
			continue;
		}
		FullBlocks -= bWasFull ? 1 : 0;
		Block.Count -= Dropped;
		Count -= Dropped;
		Removed += Dropped;
		if (Block.Count <= 0)
		{
			RemoveCoarse(Face, BX, BY);
			It.RemoveCurrent();
			continue;
		}
		RebuildSquares(Block);
	}
	return Removed;
}

int64 FVoxelMapExploredMask::Merge(const FVoxelMapExploredMask& Other)
{
	if (&Other == this || Other.Faces != Faces || Other.FaceTiles != FaceTiles)
	{
		return 0;
	}
	int64 Added = 0;
	for (const TPair<uint64, FBlock>& Pair : Other.Blocks)
	{
		int32 Face = 0, BX = 0, BY = 0;
		DecodeKey(Pair.Key, Face, BX, BY);
		if (Pair.Value.Bits.Num() == 0)
		{
			Added += MakeFull(Face, BX, BY);
			continue;
		}
		uint64 Rows[BlockSize];
		for (int32 Row = 0; Row < BlockSize; ++Row)
		{
			Rows[Row] = Pair.Value.Bits[Row];
		}
		Added += OrRows(Face, BX, BY, Rows);
	}
	return Added;
}

void FVoxelMapExploredMask::Save(FArchive& Ar) const
{
	uint8 Version = ExploredMaskVersion;
	int32 SavedFaces = Faces;
	int32 SavedFaceTiles = FaceTiles;
	int32 NumSaved = Blocks.Num();
	Ar << Version << SavedFaces << SavedFaceTiles << NumSaved;
	// In key order (face, then the block indices), so an unchanged mask writes the same bytes.
	TArray<uint64> Keys;
	Blocks.GetKeys(Keys);
	Keys.Sort();
	for (const uint64 Key : Keys)
	{
		const FBlock& Block = Blocks[Key];
		int32 Face = 0, BX = 0, BY = 0;
		DecodeKey(Key, Face, BX, BY);
		uint8 SavedFace = static_cast<uint8>(Face);
		uint8 State = Block.Bits.Num() == 0 ? SavedBlockFull : SavedBlockPartial;
		Ar << SavedFace << BX << BY << State;
		if (State == SavedBlockPartial)
		{
			for (int32 Row = 0; Row < BlockSize; ++Row)
			{
				uint64 Word = Block.Bits[Row];
				Ar << Word;
			}
		}
	}
}

bool FVoxelMapExploredMask::Load(FArchive& Ar)
{
	uint8 Version = 0;
	int32 LoadedFaces = 0;
	int32 LoadedFaceTiles = 0;
	int32 NumLoaded = 0;
	Ar << Version << LoadedFaces << LoadedFaceTiles << NumLoaded;
	// A block takes at least 10 bytes, so a count beyond what the archive holds is corrupt.
	const int64 Remaining = Ar.TotalSize() - Ar.Tell();
	if (Ar.IsError() || Version != ExploredMaskVersion || (LoadedFaces != 1 && LoadedFaces != 6) || LoadedFaceTiles < 0 || NumLoaded < 0
		|| static_cast<int64>(NumLoaded) * 10 > Remaining)
	{
		Reset();
		return false;
	}
	Configure(LoadedFaces, LoadedFaceTiles);
	const int32 BlocksPerEdge = FaceTiles > 0 ? (FaceTiles + BlockSize - 1) >> BlockShift : 0;
	for (int32 I = 0; I < NumLoaded; ++I)
	{
		uint8 Face = 0;
		int32 BX = 0, BY = 0;
		uint8 State = 0;
		Ar << Face << BX << BY << State;
		const bool bBlockInRange = Face < Faces && (BlocksPerEdge == 0 || (BX >= 0 && BY >= 0 && BX < BlocksPerEdge && BY < BlocksPerEdge))
			&& BX >= -(1 << 28) && BX < (1 << 28) && BY >= -(1 << 28) && BY < (1 << 28);
		if (Ar.IsError() || !bBlockInRange || (State != SavedBlockFull && State != SavedBlockPartial))
		{
			Reset();
			return false;
		}
		if (State == SavedBlockFull)
		{
			MakeFull(Face, BX, BY);
			continue;
		}
		// The rows, kept to the tiles the block holds (a face's last blocks hold fewer).
		const int32 W = FaceTiles > 0 ? FMath::Clamp(FaceTiles - BX * BlockSize, 0, BlockSize) : BlockSize;
		const int32 H = FaceTiles > 0 ? FMath::Clamp(FaceTiles - BY * BlockSize, 0, BlockSize) : BlockSize;
		uint64 Rows[BlockSize];
		for (int32 Row = 0; Row < BlockSize; ++Row)
		{
			Ar << Rows[Row];
			Rows[Row] = Row < H ? Rows[Row] & RowMask(0, W) : 0ull;
		}
		if (Ar.IsError())
		{
			Reset();
			return false;
		}
		OrRows(Face, BX, BY, Rows);
	}
	return true;
}

bool FVoxelMapExploredMask::FReader::IsAnyExplored(int32 Face, int32 U, int32 V, int32 Log2Size)
{
	if (!Mask.IsInRange(Face, U, V))
	{
		return false;
	}
	const int32 BX = U >> BlockShift;
	const int32 BY = V >> BlockShift;
	if (Log2Size > BlockShift)
	{
		// A square of 2^M blocks: the coarse count over it (squares past the coarsest summary answer for its square).
		const int32 M = FMath::Min(Log2Size - BlockShift, MaxCoarseLevels);
		const uint64 Key = KeyOf(Face, BX >> M, BY >> M);
		if (!bCoarseValid[M] || CoarseKey[M] != Key)
		{
			bCoarseValid[M] = true;
			CoarseKey[M] = Key;
			bCoarseHit[M] = Mask.Coarse[M - 1].Contains(Key);
		}
		return bCoarseHit[M];
	}
	const uint64 Key = KeyOf(Face, BX, BY);
	if (Key != BlockKey)
	{
		BlockKey = Key;
		Block = Mask.Blocks.Find(Key);
	}
	if (!Block)
	{
		return false;
	}
	if (Log2Size == BlockShift || Block->Bits.Num() == 0)
	{
		return true;
	}
	const int32 K = FMath::Max(Log2Size, 0);
	const int32 LU = U & (BlockSize - 1);
	const int32 LV = V & (BlockSize - 1);
	return GetBit(Block->Bits, LevelBitOffset[K] + (LV >> K) * (BlockSize >> K) + (LU >> K));
}
