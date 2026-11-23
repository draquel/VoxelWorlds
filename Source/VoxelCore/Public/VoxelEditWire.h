// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelEditTypes.h"

/**
 * What a chunk's part of an edit message does on the client (G273).
 */
enum class EVoxelEditWireKind : uint8
{
	/** The voxels a live operation changed: written over the client's. */
	Delta = 0,

	/** The first part of a whole chunk: the client's replicated voxels there are removed first, then these written. */
	FullFirst = 1,

	/** A later part of a whole chunk that did not fit in one message: written as a delta. */
	FullMore = 2,
};

/** One voxel in an edit message: its linear index, and the edit it now holds on the server (or none). */
struct VOXELCORE_API FVoxelEditWireVoxel
{
	int32 Index = 0;
	bool bHasEdit = false;
	FVoxelEdit Edit;
};

/** One chunk's voxels in an edit message. */
struct VOXELCORE_API FVoxelEditWireChunk
{
	FIntVector Coord = FIntVector::ZeroValue;
	EVoxelEditWireKind Kind = EVoxelEditWireKind::Delta;
	TArray<FVoxelEditWireVoxel> Voxels;
};

/**
 * An edit message from the server to a client (G273, D187): the chunks and voxels an operation changed, or a chunk's whole
 * replicated state, with what the client's OnChunkEdited broadcasts (the operation's source, brush centre and radius).
 */
struct VOXELCORE_API FVoxelEditWireMessage
{
	EEditSource Source = EEditSource::System;
	FVector EditCenter = FVector::ZeroVector;
	float EditRadius = 0.0f;
	TArray<FVoxelEditWireChunk> Chunks;

	/** @return The number of voxels over every chunk */
	int32 GetVoxelCount() const;
};

/**
 * The edit message's bytes: a version, the header, each chunk's voxels (index, whether it has an edit, the edit's mode,
 * delta, brush material and voxel), compressed with Oodle when that is smaller. A few bytes a voxel.
 */
namespace FVoxelEditWire
{
	/** The format version Encode writes and Decode accepts. */
	constexpr uint8 Version = 1;

	/**
	 * Encode a message.
	 * @param Message The message
	 * @param OutBytes Receives the bytes (replaced)
	 */
	VOXELCORE_API void Encode(const FVoxelEditWireMessage& Message, TArray<uint8>& OutBytes);

	/**
	 * Decode a message Encode wrote.
	 * @param Bytes The bytes
	 * @param ChunkSize The world's chunk size: an index outside the chunk fails the decode
	 * @param OutMessage Receives the message
	 * @return False when the bytes are not a message of this version, are cut short or hold a bad index or mode
	 */
	VOXELCORE_API bool Decode(const TArray<uint8>& Bytes, int32 ChunkSize, FVoxelEditWireMessage& OutMessage);
}
