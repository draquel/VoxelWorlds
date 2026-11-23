// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelEditWire.h"
#include "Misc/Compression.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace
{
	// The outer frame: one byte for the version, one for the compression, the raw size, the payload's size, the payload. The
	// payload's size lets a cut message be refused before it reaches the decompressor.
	constexpr uint8 FrameRaw = 0;
	constexpr uint8 FrameOodle = 1;

	// A cap on what a message claims, so a bad one cannot make the client allocate without bound.
	constexpr int32 MaxRawBytes = 16 * 1024 * 1024;

	void WritePayload(const FVoxelEditWireMessage& Message, FArchive& Ar)
	{
		uint8 Source = static_cast<uint8>(Message.Source);
		FVector3f Centre(Message.EditCenter);
		float Radius = Message.EditRadius;
		Ar << Source << Centre << Radius;
		uint32 ChunkCount = static_cast<uint32>(Message.Chunks.Num());
		Ar.SerializeIntPacked(ChunkCount);
		for (const FVoxelEditWireChunk& Chunk : Message.Chunks)
		{
			FIntVector Coord = Chunk.Coord;
			uint8 Kind = static_cast<uint8>(Chunk.Kind);
			Ar << Coord << Kind;
			uint32 VoxelCount = static_cast<uint32>(Chunk.Voxels.Num());
			Ar.SerializeIntPacked(VoxelCount);
			for (const FVoxelEditWireVoxel& Voxel : Chunk.Voxels)
			{
				uint32 Index = static_cast<uint32>(Voxel.Index);
				Ar.SerializeIntPacked(Index);
				uint8 Flags = Voxel.bHasEdit ? 1 : 0;
				Ar << Flags;
				if (!Voxel.bHasEdit)
				{
					continue;
				}
				uint8 Mode = static_cast<uint8>(Voxel.Edit.EditMode);
				uint32 Delta = static_cast<uint32>(FMath::Max(Voxel.Edit.DensityDelta, 0));
				uint8 BrushMaterial = Voxel.Edit.BrushMaterialID;
				FVoxelData Data = Voxel.Edit.NewData;
				Ar << Mode;
				Ar.SerializeIntPacked(Delta);
				Ar << BrushMaterial << Data.MaterialID << Data.Density << Data.BiomeID << Data.Metadata;
			}
		}
	}

	bool ReadPayload(FArchive& Ar, int32 ChunkSize, FVoxelEditWireMessage& Out)
	{
		const int64 VolumeSize = static_cast<int64>(ChunkSize) * ChunkSize * ChunkSize;
		uint8 Source = 0;
		FVector3f Centre;
		float Radius = 0.0f;
		Ar << Source << Centre << Radius;
		if (Ar.IsError() || Source > static_cast<uint8>(EEditSource::Editor))
		{
			return false;
		}
		Out.Source = static_cast<EEditSource>(Source);
		Out.EditCenter = FVector(Centre);
		Out.EditRadius = Radius;
		uint32 ChunkCount = 0;
		Ar.SerializeIntPacked(ChunkCount);
		if (Ar.IsError() || ChunkCount > static_cast<uint32>(Ar.TotalSize()))
		{
			return false;
		}
		Out.Chunks.Reset(ChunkCount);
		for (uint32 C = 0; C < ChunkCount; ++C)
		{
			FVoxelEditWireChunk& Chunk = Out.Chunks.AddDefaulted_GetRef();
			uint8 Kind = 0;
			Ar << Chunk.Coord << Kind;
			if (Ar.IsError() || Kind > static_cast<uint8>(EVoxelEditWireKind::FullMore))
			{
				return false;
			}
			Chunk.Kind = static_cast<EVoxelEditWireKind>(Kind);
			uint32 VoxelCount = 0;
			Ar.SerializeIntPacked(VoxelCount);
			if (Ar.IsError() || VoxelCount > static_cast<uint64>(VolumeSize))
			{
				return false;
			}
			Chunk.Voxels.Reset(VoxelCount);
			for (uint32 V = 0; V < VoxelCount; ++V)
			{
				FVoxelEditWireVoxel& Voxel = Chunk.Voxels.AddDefaulted_GetRef();
				uint32 Index = 0;
				Ar.SerializeIntPacked(Index);
				uint8 Flags = 0;
				Ar << Flags;
				if (Ar.IsError() || static_cast<int64>(Index) >= VolumeSize)
				{
					return false;
				}
				Voxel.Index = static_cast<int32>(Index);
				Voxel.bHasEdit = (Flags & 1) != 0;
				if (!Voxel.bHasEdit)
				{
					continue;
				}
				uint8 Mode = 0;
				uint32 Delta = 0;
				Ar << Mode;
				Ar.SerializeIntPacked(Delta);
				Ar << Voxel.Edit.BrushMaterialID << Voxel.Edit.NewData.MaterialID << Voxel.Edit.NewData.Density
					<< Voxel.Edit.NewData.BiomeID << Voxel.Edit.NewData.Metadata;
				if (Ar.IsError() || Mode > static_cast<uint8>(EEditMode::Paint) || Delta > static_cast<uint32>(MAX_int32))
				{
					return false;
				}
				Voxel.Edit.EditMode = static_cast<EEditMode>(Mode);
				Voxel.Edit.DensityDelta = static_cast<int32>(Delta);
				Voxel.Edit.LocalPosition = FIntVector(
					Voxel.Index % ChunkSize, (Voxel.Index / ChunkSize) % ChunkSize, Voxel.Index / (ChunkSize * ChunkSize));
			}
		}
		return !Ar.IsError();
	}
}

int32 FVoxelEditWireMessage::GetVoxelCount() const
{
	int32 Count = 0;
	for (const FVoxelEditWireChunk& Chunk : Chunks)
	{
		Count += Chunk.Voxels.Num();
	}
	return Count;
}

void FVoxelEditWire::Encode(const FVoxelEditWireMessage& Message, TArray<uint8>& OutBytes)
{
	TArray<uint8> Raw;
	FMemoryWriter RawWriter(Raw);
	WritePayload(Message, RawWriter);

	TArray<uint8> Compressed;
	int32 CompressedSize = FCompression::CompressMemoryBound(NAME_Oodle, Raw.Num());
	Compressed.SetNumUninitialized(CompressedSize);
	const bool bCompressed = Raw.Num() > 64
		&& FCompression::CompressMemory(NAME_Oodle, Compressed.GetData(), CompressedSize, Raw.GetData(), Raw.Num())
		&& CompressedSize < Raw.Num();

	OutBytes.Reset();
	FMemoryWriter Writer(OutBytes);
	uint8 WireVersion = Version;
	uint8 Frame = bCompressed ? FrameOodle : FrameRaw;
	int32 RawSize = Raw.Num();
	int32 BodySize = bCompressed ? CompressedSize : Raw.Num();
	Writer << WireVersion << Frame << RawSize << BodySize;
	if (bCompressed)
	{
		Writer.Serialize(Compressed.GetData(), CompressedSize);
	}
	else
	{
		Writer.Serialize(Raw.GetData(), Raw.Num());
	}
}

bool FVoxelEditWire::Decode(const TArray<uint8>& Bytes, int32 ChunkSize, FVoxelEditWireMessage& OutMessage)
{
	OutMessage = FVoxelEditWireMessage();
	if (ChunkSize <= 0)
	{
		return false;
	}
	FMemoryReader Reader(Bytes);
	uint8 WireVersion = 0;
	uint8 Frame = 0;
	int32 RawSize = 0;
	int32 BodySize = 0;
	Reader << WireVersion << Frame << RawSize << BodySize;
	if (Reader.IsError() || WireVersion != Version || RawSize < 0 || RawSize > MaxRawBytes)
	{
		return false;
	}
	const int32 HeaderBytes = static_cast<int32>(Reader.Tell());
	const int32 BodyBytes = Bytes.Num() - HeaderBytes;
	if (BodyBytes != BodySize)
	{
		return false; // cut short, or with bytes after it
	}

	TArray<uint8> Raw;
	if (Frame == FrameOodle)
	{
		Raw.SetNumUninitialized(RawSize);
		if (!FCompression::UncompressMemory(NAME_Oodle, Raw.GetData(), RawSize, Bytes.GetData() + HeaderBytes, BodyBytes))
		{
			return false;
		}
	}
	else if (Frame == FrameRaw && BodyBytes == RawSize)
	{
		Raw.Append(Bytes.GetData() + HeaderBytes, BodyBytes);
	}
	else
	{
		return false;
	}

	FMemoryReader RawReader(Raw);
	if (!ReadPayload(RawReader, ChunkSize, OutMessage) || RawReader.Tell() != Raw.Num())
	{
		OutMessage = FVoxelEditWireMessage();
		return false;
	}
	return true;
}
