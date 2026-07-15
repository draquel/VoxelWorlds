// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelData.h"

/**
 * Compression codec used for a chunk's voxel-data side buffer. Stored as a byte in the buffer
 * header so a buffer written by one codec always decodes as long as that codec's branch remains —
 * LZ4 -> Oodle -> planar migrations only ADD enum values and never renumber. Raw is the permanent
 * always-decodable fallback. Uniform is handled out-of-band by FChunkDescriptor (single value, no
 * buffer) and is reserved here for the on-disk format only.
 *
 * "Planar" variants de-interleave the AoS voxel bytes into 4 contiguous byte planes
 * (all MaterialID, all Density, all BiomeID, all Metadata) before compressing: in a far chunk the
 * material/biome/metadata planes are near-constant while only density carries the gradient band,
 * so grouping like bytes lets a general compressor find far longer runs/matches.
 */
enum class EVoxelChunkCodec : uint8
{
	Uniform     = 0, // single value, no buffer (FChunkDescriptor::UniformValue); on-disk only here
	Raw         = 1, // verbatim AoS bytes (fallback when compression doesn't help)
	LZ4         = 2, // whole-buffer LZ4
	Oodle       = 3, // whole-buffer Oodle
	LZ4Planar   = 4, // de-interleaved planes + LZ4
	OodlePlanar = 5, // de-interleaved planes + Oodle
};

/**
 * Self-describing header prepended to a compressed voxel buffer. Fixed 16 bytes, versioned and
 * endian-explicit so the in-memory buffer can double as the on-disk chunk format for the future
 * world-save epic (the CRC / cross-endian machinery is deferred until persistence actually lands).
 */
struct FVoxelChunkBufferHeader
{
	uint32 Magic;              // 'VXCB'
	uint16 FormatVersion;      // container/framing version
	uint8  CodecId;            // EVoxelChunkCodec
	uint8  VoxelFormatVersion; // FVoxelData byte-layout version
	uint16 ChunkSize;          // voxels per edge (decoder never hardcodes the volume)
	uint16 Pad;                // reserved (0)
	uint32 UncompressedBytes;  // ChunkSize^3 * sizeof(FVoxelData)
};
static_assert(sizeof(FVoxelChunkBufferHeader) == 16, "FVoxelChunkBufferHeader must be exactly 16 bytes");

/** Codec for FChunkDescriptor's compressed voxel side buffer. Game-thread; lossless round-trip. */
class VOXELCORE_API FVoxelChunkCodec
{
public:
	static constexpr uint32 Magic = uint32('V') | (uint32('X') << 8) | (uint32('C') << 16) | (uint32('B') << 24);
	static constexpr uint16 FormatVersion = 1;
	static constexpr uint8  VoxelFormatVersion = 1;

	/**
	 * Compress Data into OutBuffer as [header][payload] using PreferredCodec, falling back to Raw
	 * when the compressor is unavailable or doesn't beat the raw size (so the buffer is never larger
	 * than raw + 16). Returns false only on an empty input.
	 */
	static bool Compress(const TArray<FVoxelData>& Data, EVoxelChunkCodec PreferredCodec, int32 ChunkSize, TArray<uint8>& OutBuffer);

	/** Decompress a [header][payload] buffer back into OutData (sized from the header). Lossless. */
	static bool Decompress(const TArray<uint8>& Buffer, TArray<FVoxelData>& OutData);

	/** Map a codec to its engine compressor FName, or NAME_None for Uniform/Raw. */
	static FName CodecToFormat(EVoxelChunkCodec Codec);

private:
	static void DeInterleave(const TArray<FVoxelData>& Data, TArray<uint8>& OutPlanes);
	static void ReInterleave(const uint8* Planes, int32 NumVoxels, TArray<FVoxelData>& OutData);
};
