// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "PCGSettings.h"
#include "PCGElement.h"
#include "PCGContext.h"

#include "PCGVoxelBiomeDispatcher.generated.h"

class UVoxelPCGBiomeDecorationMapping;

namespace PCGVoxelBiomeDispatcherConstants
{
	const FName InputLabel = TEXT("In");
}

/**
 * Voxel Biome Dispatcher — routes biome-tagged surface points to per-biome decoration subgraphs,
 * driven by a data-driven UVoxelPCGBiomeDecorationMapping ("biomes as configuration").
 *
 * Reads the BiomeID attribute (from the Voxel Surface Sampler) on the input points, partitions them by
 * biome, and dynamically schedules each biome's mapped PCG graph with that biome's points, then merges
 * the results. Biomes with no mapping entry are dropped. Adding a biome is a data edit on the mapping
 * asset, not a graph edit. See Documentation/Research/PCG_DECORATION_ARCHITECTURE.md.
 */
UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural))
class UPCGVoxelBiomeDispatcherSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	/** BiomeID -> decoration graph mapping. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	TSoftObjectPtr<UVoxelPCGBiomeDecorationMapping> BiomeMapping;

	/** Name of the integer attribute carrying the biome id (emitted by the Voxel Surface Sampler). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	FName BiomeAttribute = TEXT("BiomeID");

	//~Begin UPCGSettings interface
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("VoxelBiomeDispatcher")); }
	virtual FText GetDefaultNodeTitle() const override { return NSLOCTEXT("PCGVoxelBiomeDispatcher", "NodeTitle", "Voxel Biome Dispatcher"); }
	virtual FText GetNodeTooltipText() const override;
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::Subgraph; }
#endif

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
	//~End UPCGSettings interface
};

/** Carries the scheduled per-biome subgraph task ids across the dispatcher's pause/resume. */
struct FPCGVoxelBiomeDispatcherContext : public FPCGContext
{
	TArray<FPCGTaskId> SubgraphTaskIds;
	bool bScheduledSubgraphs = false;
};

/**
 * Element backing UPCGVoxelBiomeDispatcherSettings. Schedules one dynamic subgraph per biome
 * (mirrors FPCGSubgraphElement's schedule→pause→gather pattern), then merges their outputs.
 * Non-cacheable (depends on the live biome distribution + mapping asset).
 */
class FPCGVoxelBiomeDispatcherElement : public IPCGElement
{
public:
	virtual FPCGContext* Initialize(const FPCGInitializeElementParams& InParams) override;
	virtual bool IsCacheable(const UPCGSettings* InSettings) const override { return false; }

protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};
