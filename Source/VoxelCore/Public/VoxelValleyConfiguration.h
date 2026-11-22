// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "VoxelValleyConfiguration.generated.h"

/**
 * What the valley network is made with (Documentation/VALLEYS_PLAN.md, section 6; D177, D180, D181): a lattice of nodes
 * placed by a hash of the seed, each linked to its steepest fall over the natural ground, a sink breached through its
 * lowest saddle, the drainage that makes a link a valley, the floor and the widths. Plain values: copied to the network
 * (FVoxelValleyNetwork) and read on any thread. Every value but the network's own changes what a world generates, so
 * the save fingerprint's term holds a hash of them (GetGenerationHash).
 *
 * The defaults are D181's (set B at threshold 3, chosen on the spike's shots of VoxelDemo). Lengths are world units.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FVoxelValleySettings
{
	GENERATED_BODY()

	// ---- The lattice ----

	/** A cell of the lattice; each holds one node. A valley's links are a cell or two long, so this sets how finely a valley follows the hills */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lattice", meta = (ClampMin = "5000.0"))
	float CellSize = 35000.0f;

	/** How far a node lies from its cell's middle, placed by a hash of the cell and the seed, as a share of the cell (0: a regular grid) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lattice", meta = (ClampMin = "0.0", ClampMax = "0.95"))
	float Jitter = 0.7f;

	/** A node whose natural ground stands less than this over the sea is a coast: a valley that reaches it has reached the sea */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lattice", meta = (ClampMin = "0.0"))
	float MinLandHeight = 200.0f;

	// ---- Sinks (D180) ----

	/**
	 * A node lower than its eight neighbours (a sink) links on through the lowest saddle out of its hollow to the first
	 * node lower than it, found within this many cells of it: a valley's floor runs through the saddle at the sink's
	 * floor (a gorge). A hollow wider than this keeps its sink, and the valleys into it end there
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sinks", meta = (ClampMin = "1", ClampMax = "16"))
	int32 BreachCells = 6;

	// ---- Drainage ----

	/** A node's drainage counts the nodes whose links reach it within this many links upstream (itself included) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drainage", meta = (ClampMin = "1", ClampMax = "16"))
	int32 DrainageReach = 4;

	/** A link is a valley when the drainage of its upper node is at least this: the spacing of valleys */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drainage", meta = (ClampMin = "1", ClampMax = "64"))
	int32 ValleyDrainage = 3;

	/** At this drainage a valley has its greatest widths (between, they grow with the square root of the drainage) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drainage", meta = (ClampMin = "2", ClampMax = "1024"))
	int32 FullWidthDrainage = 40;

	// ---- The valley's shape ----

	/** A node's floor stands this share of its ground's height over the sea (0: at the sea's level; 1: at the ground) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FloorFraction = 0.2f;

	/** Half the floor's width at the valley drainage */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape", meta = (ClampMin = "0.0"))
	float FloorHalfWidthMin = 4000.0f;

	/** Half the floor's width at the full-width drainage */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape", meta = (ClampMin = "0.0"))
	float FloorHalfWidthMax = 12000.0f;

	/** Half the valley's width (the floor and a side up to the hills) at the valley drainage */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape", meta = (ClampMin = "0.0"))
	float ValleyHalfWidthMin = 12000.0f;

	/** Half the valley's width at the full-width drainage */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape", meta = (ClampMin = "0.0"))
	float ValleyHalfWidthMax = 30000.0f;

	// ---- The valley's river (D179; read from V3) ----

	/** Half the valley river's width at the valley drainage */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "River", meta = (ClampMin = "0.0"))
	float RiverHalfWidthMin = 450.0f;

	/** Half the valley river's width at the full-width drainage (never over 0.8 of the floor's half width) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "River", meta = (ClampMin = "0.0"))
	float RiverHalfWidthMax = 1500.0f;

	/** The river winds across the floor over this share of the room between its edge and the floor's */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "River", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float RiverWinding = 0.8f;

	/** Half-waves of the river's winding along a link (none at the link's two nodes) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "River", meta = (ClampMin = "1", ClampMax = "8"))
	int32 RiverHalfWaves = 2;

	// ---- The network ----

	/** The network keeps what it works out by tiles of this many cells a side (it changes no valley, only how the work is cut up) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Network", meta = (ClampMin = "2", ClampMax = "64"))
	int32 TileCells = 8;

	/**
	 * The values that decide the valleys, as one line of text: the fingerprint's term is the network's version and a
	 * hash of this. TileCells is left out (it changes no valley).
	 * @return The line
	 */
	FString DescribeGeneration() const;

	/** @return A hash of DescribeGeneration */
	uint32 GetGenerationHash() const;
};

/**
 * The valleys of a voxel world: an asset a world configuration names (UVoxelWorldConfiguration::ValleyConfiguration)
 * to have a valley network carved into its natural height (Documentation/VALLEYS_PLAN.md, D176: a setting per world).
 * A world with none generates what it did before the plan.
 *
 * Naming one changes the world's terrain, so its saves made without it are refused: the save fingerprint gains a term
 * (FVoxelWorldModeFactory::MakeFingerprint).
 *
 * Create it in the Content Browser: right-click, Miscellaneous, Data Asset, VoxelValleyConfiguration.
 */
UCLASS(BlueprintType)
class VOXELCORE_API UVoxelValleyConfiguration : public UDataAsset
{
	GENERATED_BODY()

public:
	/** What the valley network is made with */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Valleys", meta = (ShowOnlyInnerProperties))
	FVoxelValleySettings Settings;
};
