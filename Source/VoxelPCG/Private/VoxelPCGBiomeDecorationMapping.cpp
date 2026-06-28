// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelPCGBiomeDecorationMapping.h"
#include "PCGGraph.h"

UPCGGraph* UVoxelPCGBiomeDecorationMapping::GetGraphForBiome(uint8 BiomeID) const
{
	for (const FVoxelBiomeDecorationEntry& Entry : Entries)
	{
		if (Entry.BiomeID == BiomeID)
		{
			return Entry.DecorationGraph.LoadSynchronous();
		}
	}
	return nullptr;
}
