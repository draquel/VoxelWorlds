// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.h"
#include "VoxelData.generated.h"

/**
 * Core voxel data structure - 4 bytes per voxel.
 *
 * Optimized for GPU transfer and cache efficiency.
 * Density determines solid/air: 0-126 = air, 127 = surface, 128-255 = solid.
 *
 * Memory: 4 bytes per voxel
 * Thread Safety: POD type, safe to copy
 *
 * @see Documentation/DATA_STRUCTURES.md
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FVoxelData
{
	GENERATED_BODY()

	/** Material type index (0-255) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel")
	uint8 MaterialID = 0;

	/** Density value: <127 = air, 127 = surface, >127 = solid */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel")
	uint8 Density = 0;

	/** Biome type index (0-255) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel")
	uint8 BiomeID = 0;

	/**
	 * Packed metadata byte:
	 * - Bits 0-3: Ambient occlusion (0-15)
	 * - Bits 4-7: Flags (user-defined)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel")
	uint8 Metadata = 0;

	/** Default constructor - creates air voxel */
	FVoxelData() = default;

	/** Construct with specific values */
	FVoxelData(uint8 InMaterialID, uint8 InDensity, uint8 InBiomeID = 0, uint8 InMetadata = 0)
		: MaterialID(InMaterialID)
		, Density(InDensity)
		, BiomeID(InBiomeID)
		, Metadata(InMetadata)
	{
	}

	/** Check if voxel is solid (density >= surface threshold) */
	FORCEINLINE bool IsSolid() const
	{
		return Density >= VOXEL_SURFACE_THRESHOLD;
	}

	/** Check if voxel is air (density < surface threshold) */
	FORCEINLINE bool IsAir() const
	{
		return Density < VOXEL_SURFACE_THRESHOLD;
	}

	/** Check if voxel is exactly at surface threshold */
	FORCEINLINE bool IsSurface() const
	{
		return Density == VOXEL_SURFACE_THRESHOLD;
	}

	/** Get ambient occlusion value (0-15) */
	FORCEINLINE uint8 GetAO() const
	{
		return Metadata & 0x0F;
	}

	/** Set ambient occlusion value (0-15) */
	FORCEINLINE void SetAO(uint8 AO)
	{
		Metadata = (Metadata & 0xF0) | (AO & 0x0F);
	}

	/** Get flags value (0-15) */
	FORCEINLINE uint8 GetFlags() const
	{
		return (Metadata >> 4) & 0x0F;
	}

	/** Set flags value (0-15) */
	FORCEINLINE void SetFlags(uint8 Flags)
	{
		Metadata = (Metadata & 0x0F) | ((Flags & 0x0F) << 4);
	}

	/** Water flag bit within the flags nibble */
	static constexpr uint8 VOXEL_FLAG_WATER = 0x01;

	/** Cave-carved air flag bit within the flags nibble (temporary, cleared after water fill) */
	static constexpr uint8 VOXEL_FLAG_CAVE = 0x02;

	/** Check if this voxel is marked as containing water */
	FORCEINLINE bool HasWaterFlag() const
	{
		return (GetFlags() & VOXEL_FLAG_WATER) != 0;
	}

	/** Set or clear the water flag */
	FORCEINLINE void SetWaterFlag(bool bHasWater)
	{
		uint8 Flags = GetFlags();
		if (bHasWater)
			Flags |= VOXEL_FLAG_WATER;
		else
			Flags &= ~VOXEL_FLAG_WATER;
		SetFlags(Flags);
	}

	/** Check if this voxel was carved by cave generation (temporary flag) */
	FORCEINLINE bool HasCaveFlag() const
	{
		return (GetFlags() & VOXEL_FLAG_CAVE) != 0;
	}

	/** Set or clear the cave-carved flag */
	FORCEINLINE void SetCaveFlag(bool bIsCave)
	{
		uint8 Flags = GetFlags();
		if (bIsCave)
			Flags |= VOXEL_FLAG_CAVE;
		else
			Flags &= ~VOXEL_FLAG_CAVE;
		SetFlags(Flags);
	}

	/** Pack to uint32 for GPU transfer */
	FORCEINLINE uint32 Pack() const
	{
		return static_cast<uint32>(MaterialID)
			| (static_cast<uint32>(Density) << 8)
			| (static_cast<uint32>(BiomeID) << 16)
			| (static_cast<uint32>(Metadata) << 24);
	}

	/** Unpack from uint32 */
	FORCEINLINE static FVoxelData Unpack(uint32 Packed)
	{
		return FVoxelData(
			static_cast<uint8>(Packed & 0xFF),
			static_cast<uint8>((Packed >> 8) & 0xFF),
			static_cast<uint8>((Packed >> 16) & 0xFF),
			static_cast<uint8>((Packed >> 24) & 0xFF)
		);
	}

	/** Create an air voxel */
	FORCEINLINE static FVoxelData Air()
	{
		return FVoxelData(0, 0, 0, 0);
	}

	/** Create a water voxel (air with water flag set) */
	FORCEINLINE static FVoxelData Water()
	{
		return FVoxelData(0, 0, 0, 0x10); // bit 4 = water flag (bit 0 of flags nibble)
	}

	/** Create a solid voxel with given material */
	FORCEINLINE static FVoxelData Solid(uint8 InMaterialID, uint8 InBiomeID = 0)
	{
		return FVoxelData(InMaterialID, 255, InBiomeID, 0);
	}

	/** Equality comparison */
	FORCEINLINE bool operator==(const FVoxelData& Other) const
	{
		return Pack() == Other.Pack();
	}

	FORCEINLINE bool operator!=(const FVoxelData& Other) const
	{
		return !(*this == Other);
	}
};

static_assert(sizeof(FVoxelData) == 4, "FVoxelData must be exactly 4 bytes");
