// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "VoxelPCGBiomeDecorationMapping.generated.h"

class UPCGGraph;

/**
 * One biome's decoration: a voxel BiomeID mapped to the PCG graph that decorates it.
 */
USTRUCT(BlueprintType)
struct FVoxelBiomeDecorationEntry
{
	GENERATED_BODY()

	/** Voxel biome id (matches FVoxelData::BiomeID / the sampler's BiomeID attribute). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome Decoration")
	uint8 BiomeID = 0;

	/** Optional human-readable label (documentation only). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome Decoration")
	FName BiomeLabel;

	/** The PCG graph that decorates this biome's surface points. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome Decoration")
	TSoftObjectPtr<UPCGGraph> DecorationGraph;
};

/**
 * "Biomes as configuration" — a data-driven mapping from voxel BiomeID to a PCG decoration graph.
 *
 * The Voxel Biome Dispatcher node consumes this to route each biome's surface points (tagged by the
 * Voxel Surface Sampler) to that biome's decoration subgraph. Adding a biome is a data edit here, not
 * a graph edit. Biome selection itself stays voxel-authoritative; this only maps the result to
 * decoration. See Documentation/Research/PCG_DECORATION_ARCHITECTURE.md.
 */
UCLASS(BlueprintType)
class VOXELPCG_API UVoxelPCGBiomeDecorationMapping : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Per-biome decoration graphs. A biome with no entry (or a null graph) gets no decoration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome Decoration")
	TArray<FVoxelBiomeDecorationEntry> Entries;

	/**
	 * Resolve the decoration graph for a biome id, loading the soft reference if needed.
	 * @return the mapped graph, or nullptr if the biome has no entry / a null graph.
	 */
	UFUNCTION(BlueprintCallable, Category = "Biome Decoration")
	UPCGGraph* GetGraphForBiome(uint8 BiomeID) const;
};
