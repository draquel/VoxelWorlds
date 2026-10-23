// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelBiomeBlendDither.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarBiomeBlendDither(
	TEXT("voxel.Biome.BlendDither"),
	1,
	TEXT("Biome blend zones (world modes review G39): 1 = each voxel's material pick by 3D value noise over a 4-voxel lattice, "
	     "equalised, walking the biomes in BiomeID order (patches); 0 = frac(W0 * 17.3 + W1 * 31.7 + depth * 0.1) in weight "
	     "order, as before (bands along the voxel grid on slopes). Read when chunks generate; GPU and CPU alike."),
	ECVF_Default);

namespace VoxelBiomeBlendDitherPrivate
{
	/** The ore Rarity hash's mix (FVoxelCPUNoiseGenerator::OreRarityHash) on a lattice point: its top 16 bits. */
	FORCEINLINE uint32 LatticeHash16(int32 X, int32 Y, int32 Z, int32 Seed)
	{
		uint32 H = static_cast<uint32>(X) * 0x8DA6B343u;
		H ^= static_cast<uint32>(Y) * 0xD8163841u;
		H ^= static_cast<uint32>(Z) * 0xCB1AB31Fu;
		H ^= static_cast<uint32>(Seed) * 0x9E3779B9u;
		H ^= H >> 16;
		H *= 0x7FEB352Du;
		H ^= H >> 15;
		H *= 0x846CA68Bu;
		H ^= H >> 16;
		return H >> 16;
	}

	/** Smoothstep at a voxel's offset 0..3 in its cell, in 32nds: 3t^2 - 2t^3 at t = 0, 1/4, 1/2, 3/4. */
	constexpr uint32 FarWeight[4] = { 0u, 5u, 16u, 27u };
}

// The raw value noise's CDF, from a Monte Carlo over the 64 offsets in a cell with independent uniform corner hashes (256M
// samples; WORLD_MODES_REVIEW_PLAN.md, G39): Cdf[b] = P(raw < b * 2^23), in 2^-24 units. The largest step is 135,388, so
// Equalise's product stays in 32 bits. A test checks the equalised value is uniform (Biome.BlendDither.Uniform).
const uint32 FVoxelBiomeBlendDither::Cdf[257] = {
	0u, 1144u, 2511u, 4118u, 5976u, 8053u, 10436u, 13114u,
	16089u, 19368u, 23007u, 27023u, 31443u, 36279u, 41571u, 47347u,
	53617u, 60473u, 67882u, 75875u, 84542u, 93894u, 103961u, 114815u,
	126514u, 139018u, 152469u, 166812u, 182232u, 198664u, 216171u, 234854u,
	254725u, 275898u, 298251u, 321907u, 347027u, 373598u, 401710u, 431300u,
	462397u, 494995u, 528885u, 564200u, 601035u, 639251u, 678931u, 719987u,
	762433u, 806435u, 851908u, 899044u, 947496u, 997525u, 1048975u, 1101898u,
	1156321u, 1212255u, 1269510u, 1328282u, 1388433u, 1449970u, 1512889u, 1577391u,
	1643288u, 1710351u, 1778680u, 1848404u, 1919407u, 1991743u, 2065398u, 2140147u,
	2216198u, 2293512u, 2372187u, 2451933u, 2533020u, 2615237u, 2699020u, 2783856u,
	2869817u, 2957084u, 3045531u, 3135443u, 3226522u, 3318677u, 3412338u, 3507187u,
	3603298u, 3700634u, 3799246u, 3899080u, 4000105u, 4102324u, 4205902u, 4310827u,
	4416751u, 4524055u, 4632642u, 4742375u, 4853333u, 4965650u, 5079087u, 5193667u,
	5309672u, 5426895u, 5545127u, 5664518u, 5785210u, 5906971u, 6029910u, 6153920u,
	6278942u, 6404988u, 6532078u, 6660031u, 6789019u, 6918920u, 7049699u, 7181200u,
	7313457u, 7446168u, 7579745u, 7713495u, 7847783u, 7982542u, 8117597u, 8252766u,
	8388154u, 8523442u, 8658687u, 8793842u, 8928518u, 9063067u, 9197124u, 9330524u,
	9463379u, 9595595u, 9726967u, 9857635u, 9987304u, 10116177u, 10244301u, 10371413u,
	10497342u, 10622222u, 10746299u, 10869423u, 10991204u, 11111875u, 11231370u, 11349666u,
	11466919u, 11582890u, 11697578u, 11811165u, 11923381u, 12034415u, 12144260u, 12252875u,
	12360317u, 12466417u, 12571227u, 12674831u, 12777111u, 12878154u, 12978042u, 13076693u,
	13173906u, 13270037u, 13364775u, 13458282u, 13550504u, 13641626u, 13731327u, 13819851u,
	13906958u, 13992981u, 14077857u, 14161624u, 14243957u, 14325015u, 14404879u, 14483442u,
	14560870u, 14637087u, 14711894u, 14785339u, 14857519u, 14928559u, 14998115u, 15066600u,
	15133753u, 15199584u, 15263949u, 15326781u, 15388303u, 15448603u, 15507343u, 15564727u,
	15620640u, 15675033u, 15727992u, 15779398u, 15829344u, 15877882u, 15924992u, 15970609u,
	16014700u, 16057378u, 16098386u, 16138063u, 16176315u, 16213056u, 16248476u, 16282441u,
	16314991u, 16346060u, 16375586u, 16403659u, 16430215u, 16455368u, 16479041u, 16501427u,
	16522498u, 16542289u, 16560888u, 16578399u, 16594759u, 16610173u, 16624525u, 16638031u,
	16650565u, 16662259u, 16673120u, 16683217u, 16692565u, 16701191u, 16709228u, 16716617u,
	16723472u, 16729787u, 16735542u, 16740848u, 16745725u, 16750158u, 16754184u, 16757847u,
	16761146u, 16764116u, 16766784u, 16769139u, 16771250u, 16773084u, 16774691u, 16776066u,
	16777216u
};

uint32 FVoxelBiomeBlendDither::RawValue(const FIntVector& Key, int32 WorldSeed)
{
	using namespace VoxelBiomeBlendDitherPrivate;
	const int32 Seed = WorldSeed + SeedOffset;
	// The lattice cell (floor division) and the voxel's offset in it; arithmetic shifts and masks, exact for negatives.
	const int32 LX = Key.X >> LatticeShift, LY = Key.Y >> LatticeShift, LZ = Key.Z >> LatticeShift;
	const uint32 Mask = (1u << LatticeShift) - 1u;
	const uint32 WX1 = FarWeight[static_cast<uint32>(Key.X) & Mask];
	const uint32 WY1 = FarWeight[static_cast<uint32>(Key.Y) & Mask];
	const uint32 WZ1 = FarWeight[static_cast<uint32>(Key.Z) & Mask];
	const uint32 WX[2] = { 32u - WX1, WX1 };
	const uint32 WY[2] = { 32u - WY1, WY1 };
	const uint32 WZ[2] = { 32u - WZ1, WZ1 };
	uint32 Sum = 0u;
	for (int32 DZ = 0; DZ < 2; ++DZ)
	{
		for (int32 DY = 0; DY < 2; ++DY)
		{
			for (int32 DX = 0; DX < 2; ++DX)
			{
				Sum += LatticeHash16(LX + DX, LY + DY, LZ + DZ, Seed) * (WX[DX] * WY[DY] * WZ[DZ]);
			}
		}
	}
	return Sum; // the weights sum to 32^3 = 2^15, so Sum <= 65535 * 2^15 < 2^31
}

uint32 FVoxelBiomeBlendDither::Equalise(uint32 Raw)
{
	const uint32 Bucket = FMath::Min(Raw >> 23, 255u);
	const uint32 Frac14 = (Raw >> 9) & 0x3FFFu;
	const uint32 Lo = Cdf[Bucket];
	const uint32 Hi = Cdf[Bucket + 1];
	return Lo + (((Hi - Lo) * Frac14) >> 14);
}

float FVoxelBiomeBlendDither::Sample(const FVoxelBlendDitherKey& Dither)
{
	return static_cast<float>(Equalise(RawValue(Dither.Key, Dither.Seed))) * (1.0f / 16777216.0f);
}

FIntVector FVoxelBiomeBlendDither::GridBase(const FIntVector& ChunkCoord, int32 ChunkSize, const FVector& WorldOrigin, float VoxelSize)
{
	const double Inv = 1.0 / FMath::Max(static_cast<double>(VoxelSize), 1.0e-6);
	return ChunkCoord * ChunkSize + FIntVector(
		FMath::RoundToInt32(WorldOrigin.X * Inv), FMath::RoundToInt32(WorldOrigin.Y * Inv), FMath::RoundToInt32(WorldOrigin.Z * Inv));
}

FIntVector FVoxelBiomeBlendDither::KeyAt(const FVector& WorldPos, float VoxelSize)
{
	const double Inv = 1.0 / FMath::Max(static_cast<double>(VoxelSize), 1.0e-6);
	return FIntVector(FMath::RoundToInt32(WorldPos.X * Inv), FMath::RoundToInt32(WorldPos.Y * Inv), FMath::RoundToInt32(WorldPos.Z * Inv));
}

bool FVoxelBiomeBlendDither::IsEnabled()
{
	return CVarBiomeBlendDither.GetValueOnAnyThread() != 0;
}
