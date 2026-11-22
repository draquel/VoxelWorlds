// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "VoxelHydrologyConfiguration.generated.h"

/**
 * What the hydrology generator makes brooks and ponds with (Documentation/HYDROLOGY_PLAN.md, section 5.8; D145): the
 * grid it reads the natural ground on, where springs stand, which descents are kept as rivers, how wide and deep a
 * river's bed is, which hollows are kept as lakes. Plain values: copied to the generator's network and read on its
 * worker. Every value changes what a world generates, so the save fingerprint's term holds a hash of them
 * (GetFingerprint).
 *
 * Lengths are world units unless a name says voxels; a cell is a square of the generator's grid.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FVoxelHydrologySettings
{
	GENERATED_BODY()

	// ---- The grid and the springs ----

	/** A cell of the generator's grid, in voxels. The natural ground is read at the cells' corners, which are aligned to the world, so two descents that meet in a cell go on as one */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "2", ClampMax = "64"))
	int32 CellVoxels = 8;

	/**
	 * A square of this many cells holds one candidate spring, placed in it by a hash of the seed. The count of brooks
	 * goes with the square of it, and the game layer places no POI on a body: on the demo's ground a spring every 24
	 * cells gives four times the brooks and leaves 4 of 9 cities and 4 of 15 villages without a site, every 48 cells
	 * leaves 2 POIs of 126 without one (the hydrology plan's H7)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Springs", meta = (ClampMin = "2", ClampMax = "512"))
	int32 SpringSpacingCells = 48;

	/** A candidate is a spring when its ground stands at least this far over the sea (in a world with no sea every candidate is one) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Springs", meta = (ClampMin = "0.0"))
	float MinSpringHeight = 1500.0f;

	// ---- Which descents are rivers ----

	/** A descent shorter than this from its spring to its mouth is no river */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rivers", meta = (ClampMin = "0.0"))
	float MinRiverLength = 20000.0f;

	/**
	 * A descent longer than this is no river. It is also how far a river depends on the ground around it: a gather
	 * works out the springs within twice this distance of its bounds, so the cost of a region grows with its square
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rivers", meta = (ClampMin = "1000.0"))
	float MaxRiverLength = 80000.0f;

	/** A descent whose fall over its length, spring to mouth, is steeper than this is no river (rise over run) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rivers", meta = (ClampMin = "0.01"))
	float MaxMeanGrade = 0.2f;

	/** A descent that crosses more hollows than this is no river */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rivers", meta = (ClampMin = "0", ClampMax = "64"))
	int32 MaxLakesPerRiver = 12;

	// ---- A river's bed and surface ----

	/** Half the width of the bed's floor where one spring drains through it, in voxels */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rivers|Bed", meta = (ClampMin = "0.5", ClampMax = "16.0"))
	float HalfWidthVoxels = 1.5f;

	/** How much wider the floor is for each doubling of the springs that drain through a river reach, as a share of HalfWidthVoxels */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rivers|Bed", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float HalfWidthGrowth = 0.35f;

	/** The floor's half width is never over this, in voxels */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rivers|Bed", meta = (ClampMin = "0.5", ClampMax = "32.0"))
	float MaxHalfWidthVoxels = 3.0f;

	/** The width of each bank, from the floor's edge up to the natural ground, in voxels (the bed's falloff, and how far past the floor the river reach's footprint goes) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rivers|Bed", meta = (ClampMin = "1.0", ClampMax = "32.0"))
	float BankWidthVoxels = 3.0f;

	/** The water over the bed, in voxels */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rivers|Bed", meta = (ClampMin = "0.5", ClampMax = "16.0"))
	float DepthVoxels = 2.0f;

	/** The surface is held this far under the ground along the river, in voxels */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rivers|Bed", meta = (ClampMin = "0.0", ClampMax = "8.0"))
	float SurfaceBelowGroundVoxels = 1.0f;

	/** The surface is held this far under the lowest ground of both banks, in voxels (the plan's rule 2) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rivers|Bed", meta = (ClampMin = "0.0", ClampMax = "8.0"))
	float BankMarginVoxels = 0.5f;

	/** At a spring the depth and the width start from nothing and are whole after this length (the plan's rule 4) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rivers|Bed", meta = (ClampMin = "0.0"))
	float HeadLength = 1200.0f;

	/** The ground along a river is averaged this far each way before the surface is set under it */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rivers|Bed", meta = (ClampMin = "0.0"))
	float SmoothingDistance = 600.0f;

	/** A river whose bed would lie deeper than this under the natural ground anywhere is not made, in voxels. The engine's own cap on a line's cut is half a chunk */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rivers|Bed", meta = (ClampMin = "1.0", ClampMax = "64.0"))
	float MaxCutVoxels = 12.0f;

	/** How fast a river runs: what its ripples move at and what carries a swimmer (world units a second) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rivers", meta = (ClampMin = "0.0"))
	float FlowSpeed = 100.0f;

	/** The bed's material: an index into the world's material table, or -1 for the ground's own */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rivers|Bed", meta = (ClampMin = "-1", ClampMax = "255"))
	int32 BedMaterialID = -1;

	/** No cave opens within this many voxels under a river's bed (0: caves as elsewhere) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rivers|Bed", meta = (ClampMin = "0.0"))
	float BedMinCaveDepth = 8.0f;

	// ---- Lakes ----

	/** A hollow is kept as a lake when its water is at least this deep at its deepest, in voxels */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lakes", meta = (ClampMin = "0.25", ClampMax = "64.0"))
	float MinLakeDepthVoxels = 2.0f;

	/** ... and covers at least this area (square world units: 1.0e8 is a hectare) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lakes", meta = (ClampMin = "0.0"))
	float MinLakeArea = 6.0e6f;

	/** A lake's level lies this far under the lowest ground of its rim, in voxels, so the rim holds (the plan's rule 6) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lakes", meta = (ClampMin = "0.0", ClampMax = "8.0"))
	float LakeLevelMarginVoxels = 0.5f;

	/** A lake is never deeper than this: a deeper hollow holds a lake this deep, with no outlet. Held under a chunk and a quarter, which the LOD window's slab allows a level over the ground */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lakes", meta = (ClampMin = "100.0"))
	float MaxLakeDepth = 2400.0f;

	/** A lake never covers more cells than this: a wider hollow holds the largest lake under it, with no outlet */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lakes", meta = (ClampMin = "16", ClampMax = "65536"))
	int32 MaxLakeCells = 4096;

	/** A lake is never longer than this many cells along X or Y, so a gather finds every lake from the springs near it */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lakes", meta = (ClampMin = "8", ClampMax = "1024"))
	int32 MaxLakeSpanCells = 128;

	/** No cave opens within this many voxels under a lake's bed (0: caves as elsewhere) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lakes", meta = (ClampMin = "0.0"))
	float LakeMinCaveDepth = 8.0f;

	// ---- The network ----

	/** The network keeps what it works out by tiles of this many cells (it changes no body, only how the work is cut up) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Network", meta = (ClampMin = "16", ClampMax = "512"))
	int32 TileCells = 64;

	/**
	 * How far around each viewer the network keeps the bodies worked out ahead of the chunks that ask for them, in view
	 * distances (the world configuration's ViewDistance). What the registration waits for is this disc around each start
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Network", meta = (ClampMin = "1.0", ClampMax = "8.0"))
	float WarmUpViewDistances = 1.5f;

	/** How far around each viewer the worker works bodies out for what draws far away (the far field's beds, the drawn water), in view distances. Not waited for */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Network", meta = (ClampMin = "1.0", ClampMax = "32.0"))
	float FarViewDistances = 4.0f;

	/**
	 * A generated lake is drawn out to this distance from the viewer (and no further than the far field draws the
	 * ground). Generated bodies are many: every one within the far field's distance would be thousands of meshes
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Network", meta = (ClampMin = "10000.0"))
	float LakeDrawDistance = 600000.0f;

	/** A generated river reach is drawn out to this distance from the viewer: a brook is under a pixel wide past a few kilometres */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Network", meta = (ClampMin = "10000.0"))
	float ReachDrawDistance = 250000.0f;

	/**
	 * A valley river (a world with valleys, Documentation/VALLEYS_PLAN.md) is drawn out to this distance from the viewer
	 * (and no further than the far field draws the ground): it is meant to be seen from the hills on either side
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Network", meta = (ClampMin = "10000.0"))
	float ValleyRiverDrawDistance = 600000.0f;

	/**
	 * The values that decide which bodies a world has, as one line of text: the fingerprint's term is the generator's
	 * version and a hash of this. The network's values are left out (they change no body).
	 * @return The line
	 */
	FString DescribeGeneration() const;

	/** @return A hash of DescribeGeneration */
	uint32 GetGenerationHash() const;
};

/**
 * The hydrology of a voxel world: an asset a world configuration names (UVoxelWorldConfiguration::HydrologyConfiguration)
 * to have ponds and brooks generated from its seed (Documentation/HYDROLOGY_PLAN.md, D146: a setting per world). A
 * world with none has the sea and what a level's actors place by hand, as before the plan.
 *
 * Naming one changes the world's terrain (the beds), its POIs and its roads, so the world's saves made without it are
 * refused: the save fingerprint gains a term (FVoxelWorldModeFactory::MakeFingerprint).
 *
 * Create it in the Content Browser: right-click, Miscellaneous, Data Asset, VoxelHydrologyConfiguration.
 */
UCLASS(BlueprintType)
class VOXELCORE_API UVoxelHydrologyConfiguration : public UDataAsset
{
	GENERATED_BODY()

public:
	/** What the generator makes ponds and brooks with */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydrology", meta = (ShowOnlyInnerProperties))
	FVoxelHydrologySettings Settings;
};
