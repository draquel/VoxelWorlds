// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelChunkCodec.h"
#include "Misc/Compression.h"

FName FVoxelChunkCodec::CodecToFormat(EVoxelChunkCodec Codec)
{
	switch (Codec)
	{
	case EVoxelChunkCodec::LZ4:
	case EVoxelChunkCodec::LZ4Planar:
		return NAME_LZ4;
	case EVoxelChunkCodec::Oodle:
	case EVoxelChunkCodec::OodlePlanar:
		return NAME_Oodle;
	default:
		return NAME_None;
	}
}

void FVoxelChunkCodec::DeInterleave(const TArray<FVoxelData>& Data, TArray<uint8>& OutPlanes)
{
	const int32 N = Data.Num();
	OutPlanes.SetNumUninitialized(N * 4);
	// FVoxelData is exactly 4 bytes {MaterialID, Density, BiomeID, Metadata} in order (static_assert).
	const uint8* Src = reinterpret_cast<const uint8*>(Data.GetData());
	uint8* Mat  = OutPlanes.GetData();
	uint8* Den  = Mat + N;
	uint8* Bio  = Den + N;
	uint8* Meta = Bio + N;
	for (int32 i = 0; i < N; ++i)
	{
		Mat[i]  = Src[i * 4 + 0];
		Den[i]  = Src[i * 4 + 1];
		Bio[i]  = Src[i * 4 + 2];
		Meta[i] = Src[i * 4 + 3];
	}
}

void FVoxelChunkCodec::ReInterleave(const uint8* Planes, int32 NumVoxels, TArray<FVoxelData>& OutData)
{
	const uint8* Mat  = Planes;
	const uint8* Den  = Mat + NumVoxels;
	const uint8* Bio  = Den + NumVoxels;
	const uint8* Meta = Bio + NumVoxels;
	uint8* Dst = reinterpret_cast<uint8*>(OutData.GetData());
	for (int32 i = 0; i < NumVoxels; ++i)
	{
		Dst[i * 4 + 0] = Mat[i];
		Dst[i * 4 + 1] = Den[i];
		Dst[i * 4 + 2] = Bio[i];
		Dst[i * 4 + 3] = Meta[i];
	}
}

bool FVoxelChunkCodec::Compress(const TArray<FVoxelData>& Data, EVoxelChunkCodec PreferredCodec, int32 ChunkSize, TArray<uint8>& OutBuffer)
{
	const int32 NumVoxels = Data.Num();
	if (NumVoxels == 0)
	{
		return false;
	}
	const int32 RawBytes = NumVoxels * sizeof(FVoxelData);

	const bool bPlanar = (PreferredCodec == EVoxelChunkCodec::LZ4Planar || PreferredCodec == EVoxelChunkCodec::OodlePlanar);
	const FName Fmt = CodecToFormat(PreferredCodec);

	// Source bytes fed to the compressor: de-interleaved planes, or the raw AoS bytes.
	TArray<uint8> PlaneScratch;
	const uint8* Src;
	if (bPlanar)
	{
		DeInterleave(Data, PlaneScratch);
		Src = PlaneScratch.GetData();
	}
	else
	{
		Src = reinterpret_cast<const uint8*>(Data.GetData());
	}

	TArray<uint8> Payload;
	EVoxelChunkCodec UsedCodec = EVoxelChunkCodec::Raw;

	if (Fmt != NAME_None)
	{
		const int32 Bound = FCompression::CompressMemoryBound(Fmt, RawBytes);
		Payload.SetNumUninitialized(Bound);
		int32 CompressedSize = Bound;
		if (FCompression::CompressMemory(Fmt, Payload.GetData(), CompressedSize, Src, RawBytes)
			&& CompressedSize < RawBytes)
		{
			Payload.SetNum(CompressedSize);
			UsedCodec = PreferredCodec;
		}
		else
		{
			Payload.Reset();
		}
	}

	if (UsedCodec == EVoxelChunkCodec::Raw)
	{
		// Fallback: store the verbatim AoS bytes. Raw always decodes without a transform.
		Payload.SetNumUninitialized(RawBytes);
		FMemory::Memcpy(Payload.GetData(), Data.GetData(), RawBytes);
	}

	FVoxelChunkBufferHeader Header;
	Header.Magic = Magic;
	Header.FormatVersion = FormatVersion;
	Header.CodecId = static_cast<uint8>(UsedCodec);
	Header.VoxelFormatVersion = VoxelFormatVersion;
	Header.ChunkSize = static_cast<uint16>(ChunkSize);
	Header.Pad = 0;
	Header.UncompressedBytes = static_cast<uint32>(RawBytes);

	OutBuffer.SetNumUninitialized(sizeof(Header) + Payload.Num());
	FMemory::Memcpy(OutBuffer.GetData(), &Header, sizeof(Header));
	FMemory::Memcpy(OutBuffer.GetData() + sizeof(Header), Payload.GetData(), Payload.Num());
	return true;
}

bool FVoxelChunkCodec::Decompress(const TArray<uint8>& Buffer, TArray<FVoxelData>& OutData)
{
	if (Buffer.Num() < static_cast<int32>(sizeof(FVoxelChunkBufferHeader)))
	{
		return false;
	}

	FVoxelChunkBufferHeader Header;
	FMemory::Memcpy(&Header, Buffer.GetData(), sizeof(Header));
	if (Header.Magic != Magic || Header.FormatVersion != FormatVersion)
	{
		return false;
	}

	const int32 RawBytes = static_cast<int32>(Header.UncompressedBytes);
	const int32 NumVoxels = RawBytes / sizeof(FVoxelData);
	if (NumVoxels <= 0)
	{
		return false;
	}
	OutData.SetNumUninitialized(NumVoxels);

	const uint8* Payload = Buffer.GetData() + sizeof(Header);
	const int32 PayloadSize = Buffer.Num() - sizeof(Header);
	const EVoxelChunkCodec Codec = static_cast<EVoxelChunkCodec>(Header.CodecId);

	if (Codec == EVoxelChunkCodec::Raw)
	{
		if (PayloadSize < RawBytes)
		{
			return false;
		}
		FMemory::Memcpy(OutData.GetData(), Payload, RawBytes);
		return true;
	}

	const FName Fmt = CodecToFormat(Codec);
	if (Fmt == NAME_None)
	{
		return false;
	}
	const bool bPlanar = (Codec == EVoxelChunkCodec::LZ4Planar || Codec == EVoxelChunkCodec::OodlePlanar);

	if (bPlanar)
	{
		TArray<uint8> Planes;
		Planes.SetNumUninitialized(RawBytes);
		if (!FCompression::UncompressMemory(Fmt, Planes.GetData(), RawBytes, Payload, PayloadSize))
		{
			return false;
		}
		ReInterleave(Planes.GetData(), NumVoxels, OutData);
	}
	else
	{
		if (!FCompression::UncompressMemory(Fmt, OutData.GetData(), RawBytes, Payload, PayloadSize))
		{
			return false;
		}
	}
	return true;
}
