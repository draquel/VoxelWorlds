// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelVertex.generated.h"

/**
 * Optimized vertex format for voxel meshes - 28 bytes per vertex.
 *
 * Designed for GPU efficiency with packed data formats.
 * Used by the custom vertex factory for runtime rendering.
 *
 * Memory: 28 bytes per vertex (vs 48+ bytes for PMC)
 * Thread Safety: POD type, safe to copy
 *
 * @see Documentation/DATA_STRUCTURES.md
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FVoxelVertex
{
	GENERATED_BODY()

	/** Vertex position in local chunk space */
	FVector3f Position;

	/**
	 * Packed normal and ambient occlusion:
	 * - Bits 0-9: Normal X (10 bits, signed)
	 * - Bits 10-19: Normal Y (10 bits, signed)
	 * - Bits 20-29: Normal Z (10 bits, signed)
	 * - Bits 30-31: AO (2 bits, 0-3)
	 */
	uint32 PackedNormalAndAO;

	/** Texture coordinates */
	FVector2f UV;

	/**
	 * Packed material data:
	 * - Bits 0-7: MaterialID
	 * - Bits 8-15: BiomeID
	 * - Bits 16-23: Reserved
	 * - Bits 24-31: Vertex flags
	 */
	uint32 PackedMaterialData;

	/** Default constructor */
	FVoxelVertex()
		: Position(FVector3f::ZeroVector)
		, PackedNormalAndAO(0)
		, UV(FVector2f::ZeroVector)
		, PackedMaterialData(0)
	{
	}

	/** Construct with explicit values */
	FVoxelVertex(const FVector3f& InPosition, const FVector3f& InNormal, const FVector2f& InUV,
		uint8 InMaterialID, uint8 InBiomeID = 0, uint8 InAO = 0)
		: Position(InPosition)
		, UV(InUV)
	{
		SetNormal(InNormal);
		SetAO(InAO);
		SetMaterialID(InMaterialID);
		SetBiomeID(InBiomeID);
	}

	/** Set normal vector (will be normalized and packed) */
	void SetNormal(const FVector3f& Normal)
	{
		// Normalize and convert to 10-bit signed values (-512 to 511)
		FVector3f N = Normal.GetSafeNormal();
		int32 NX = FMath::Clamp(FMath::RoundToInt(N.X * 511.0f), -512, 511);
		int32 NY = FMath::Clamp(FMath::RoundToInt(N.Y * 511.0f), -512, 511);
		int32 NZ = FMath::Clamp(FMath::RoundToInt(N.Z * 511.0f), -512, 511);

		// Pack into 30 bits, preserving AO in top 2 bits
		uint32 AO = PackedNormalAndAO & 0xC0000000;
		PackedNormalAndAO = AO
			| (static_cast<uint32>(NX & 0x3FF))
			| (static_cast<uint32>(NY & 0x3FF) << 10)
			| (static_cast<uint32>(NZ & 0x3FF) << 20);
	}

	/** Get unpacked normal vector */
	FVector3f GetNormal() const
	{
		// Extract 10-bit signed values
		int32 NX = static_cast<int32>(PackedNormalAndAO & 0x3FF);
		int32 NY = static_cast<int32>((PackedNormalAndAO >> 10) & 0x3FF);
		int32 NZ = static_cast<int32>((PackedNormalAndAO >> 20) & 0x3FF);

		// Sign extend from 10 bits
		if (NX & 0x200) NX |= 0xFFFFFC00;
		if (NY & 0x200) NY |= 0xFFFFFC00;
		if (NZ & 0x200) NZ |= 0xFFFFFC00;

		return FVector3f(
			static_cast<float>(NX) / 511.0f,
			static_cast<float>(NY) / 511.0f,
			static_cast<float>(NZ) / 511.0f
		);
	}

	/** Set ambient occlusion (0-3) */
	void SetAO(uint8 AO)
	{
		PackedNormalAndAO = (PackedNormalAndAO & 0x3FFFFFFF) | (static_cast<uint32>(AO & 0x3) << 30);
	}

	/** Get ambient occlusion (0-3) */
	uint8 GetAO() const
	{
		return static_cast<uint8>((PackedNormalAndAO >> 30) & 0x3);
	}

	/** Set material ID (0-255) */
	void SetMaterialID(uint8 MaterialID)
	{
		PackedMaterialData = (PackedMaterialData & 0xFFFFFF00) | static_cast<uint32>(MaterialID);
	}

	/** Get material ID */
	uint8 GetMaterialID() const
	{
		return static_cast<uint8>(PackedMaterialData & 0xFF);
	}

	/** Set biome ID (0-255) */
	void SetBiomeID(uint8 BiomeID)
	{
		PackedMaterialData = (PackedMaterialData & 0xFFFF00FF) | (static_cast<uint32>(BiomeID) << 8);
	}

	/** Get biome ID */
	uint8 GetBiomeID() const
	{
		return static_cast<uint8>((PackedMaterialData >> 8) & 0xFF);
	}

	/** Set vertex flags (0-255) */
	void SetFlags(uint8 Flags)
	{
		PackedMaterialData = (PackedMaterialData & 0x00FFFFFF) | (static_cast<uint32>(Flags) << 24);
	}

	/** Get vertex flags */
	uint8 GetFlags() const
	{
		return static_cast<uint8>((PackedMaterialData >> 24) & 0xFF);
	}
};

static_assert(sizeof(FVoxelVertex) == 28, "FVoxelVertex must be exactly 28 bytes");
