// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "PCGSettings.h"
#include "PCGElement.h"

#include "PCGVoxelSurfaceSampler.generated.h"

namespace PCGVoxelSurfaceSamplerConstants
{
	const FName BoundingShapeLabel = TEXT("Bounding Shape");

	// Output point attribute names (match the FScatterDefinition filter fields so PCG
	// graphs can replicate the bespoke scatter rules: slope, AllowedMaterials, AllowedBiomes).
	const FName NormalAttribute = TEXT("Normal");
	const FName SlopeAttribute = TEXT("Slope");
	const FName MaterialIDAttribute = TEXT("MaterialID");
	const FName BiomeIDAttribute = TEXT("BiomeID");
}

/**
 * Voxel Surface Sampler — PCG node that emits terrain surface points by re-sampling the
 * procedural voxel generator (via FVoxelSurfaceQuery), decoupled from chunk streaming.
 *
 * For each cell of an XY grid over the sampling bounds it queries the active world's
 * IVoxelWorldMode for the surface height, normal, slope, and the biome/material at that
 * point, and writes a PCG point there with those values as metadata attributes. Downstream
 * PCG graph nodes (attribute filters, density, static-mesh spawner) then place scatter/
 * foliage/props on the runtime-generated terrain.
 *
 * Bounds come from the optional Bounding Shape input pin, or (when none is connected and
 * bUnbounded is false) the executing actor's bounds — which, for a partitioned
 * "Generate at Runtime" PCG component, is the grid cell being generated.
 */
UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural))
class UPCGVoxelSurfaceSamplerSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	/** Spacing (world units) between sampled surface points on the XY grid. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (ClampMin = "1.0", PCG_Overridable))
	float PointSpacing = 100.0f;

	/** Orient each point to the terrain surface normal. When false, points face straight up. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	bool bAlignToSurfaceNormal = false;

	/**
	 * If no Bounding Shape input is provided, the executing actor's bounds limit the sample
	 * domain. Enable this to ignore actor bounds — requires a Bounding Shape input, otherwise
	 * nothing is generated (guards against sampling an unbounded world).
	 */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	bool bUnbounded = false;

	//~Begin UPCGSettings interface
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("VoxelSurfaceSampler")); }
	virtual FText GetDefaultNodeTitle() const override { return NSLOCTEXT("PCGVoxelSurfaceSampler", "NodeTitle", "Voxel Surface Sampler"); }
	virtual FText GetNodeTooltipText() const override;
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::Sampler; }
#endif

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
	//~End UPCGSettings interface
};

/**
 * Synchronous element backing UPCGVoxelSurfaceSamplerSettings.
 *
 * Forced to the game thread (resolves the voxel chunk manager by iterating world actors,
 * which is not thread-safe) and marked non-cacheable (output depends on live world state).
 * The surface-query math itself is thread-safe; a time-sliced/async version is a later
 * optimization.
 */
class FPCGVoxelSurfaceSamplerElement : public IPCGElement
{
public:
	virtual bool CanExecuteOnlyOnMainThread(FPCGContext* Context) const override { return true; }
	virtual bool IsCacheable(const UPCGSettings* InSettings) const override { return false; }

protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};
