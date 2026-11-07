// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UTexture2D;
class UVoxelBiomeConfiguration;

/**
 * The biome lookup the voxel master materials read (the weather plan's P3): one texel per biome ID (256 x 1, RGBA8) with
 * the biome's climate as the materials need it to localise the weather's global inputs. R the low end and G the high end
 * of the biome's temperature range, each mapped from [-1, 1] to 0..1 (the same mapping the weather reads the climate in),
 * so a material can tell a narrow cold biome (the tundra) from a wide one (an alpine or coastal biome spanning every
 * temperature, where the viewer's own cold decides); B 1 where the biome exists, 0 for an unused ID; A 255. A vertex
 * carries its biome in vertex colour G (every terrain
 * and far-field vertex; a Dual Contouring border vertex carries its third material there instead), so the material
 * finds the texel by that byte. The world's renderer builds one per world from the configuration's biomes and hands it
 * to every region component as the texture parameter BiomeLUT; without a biome configuration every texel is the middle
 * (0.5, 0.5) with B 0.
 */
struct VOXELRENDERING_API FVoxelBiomeLookup
{
	/** The texel count: one per biome ID. */
	static constexpr int32 Width = 256;

	/** The name of the master materials' texture parameter. */
	static const FName ParameterName;

	/**
	 * The texels, pure: 256 entries, one per biome ID.
	 * @param Config The world's biome configuration, or null (the middle everywhere)
	 * @param OutTexels The 256 texels (R the temperature range's low end, G its high end, B 255 where the biome exists, A 255)
	 */
	static void Encode(const UVoxelBiomeConfiguration* Config, TArray<FColor>& OutTexels);

	/**
	 * A transient texture of the texels (uncompressed, no mips, nearest filtering, never streamed), ready for the materials.
	 * @param Config The world's biome configuration, or null
	 * @param Name The texture's name (for the log and the stats)
	 * @return The texture, or null when no RHI can hold one (the headless suite)
	 */
	static UTexture2D* Build(const UVoxelBiomeConfiguration* Config, const FName& Name = TEXT("VoxelBiomeLookup"));
};
