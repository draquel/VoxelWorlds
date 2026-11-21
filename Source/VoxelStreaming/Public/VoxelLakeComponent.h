// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelWaterBodyComponent.h"
#include "VoxelLakeComponent.generated.h"

/** Where a hand-placed lake takes its level from. */
UENUM(BlueprintType)
enum class EVoxelLakeLevel : uint8
{
	/**
	 * The lowest natural ground along the outline, less a margin: the highest level the drawn rim holds. The outline's
	 * own height is not read: only where it runs over the ground. The default, since nothing draws the voxel terrain in
	 * the editor.
	 */
	BelowOutline,

	/** The actor's own height: its world Z on a heightmap world, its altitude over the datum on a planet */
	ActorHeight
};

/** What a hand-placed lake is made of (UVoxelLakeComponent::BuildLake). */
struct FVoxelLakeBuild
{
	/** The footprint: the cells inside the outline whose ground lies under the level, grown by one cell */
	TSharedPtr<const FVoxelWaterLake> Footprint;

	/** The lake's water level: world Z, or the distance from the planet centre */
	double Level = 0.0;

	/** How many places along the outline were read, and the lowest and the highest natural ground among them (a line's terms: Z, or an altitude) */
	int32 NumOutlinePlaces = 0;
	double LowestOutlineGround = 0.0;
	double HighestOutlineGround = 0.0;

	/** How many of those places lie under the level (the water would end in a wall there), and by how much at most */
	int32 NumOutlinePlacesUnder = 0;
	double MostOutlineUnder = 0.0;

	/** How many cells inside the outline lie under the level, and how deep the deepest is */
	int32 NumWetCells = 0;
	double Deepest = 0.0;

	/** How many cells of the footprint's grown edge, outside the outline, lie under the level: water may stand past a rim narrower than a cell */
	int32 NumEdgeCellsUnder = 0;

	/** Whether the spline is a closed loop (an open one is closed from its last point to its first) */
	bool bClosedLoop = true;
};

/**
 * A hand-placed lake (the hydrology plan's section 5.7, D144; the how-to is Documentation/Guides/TERRAIN_LINES.md in
 * the plugin): turns a closed USplineComponent on the same actor, drawn round a hollow of the terrain, into a lake of
 * the level's voxel world. The lake fills what lies under its level inside the outline and moves no ground: the
 * voxels there hold water, a swimmer swims, the map shows it and a mesh draws it, in every world mode.
 *
 * When the world starts the component reads the natural ground along the outline, takes the lake's level (LevelMode:
 * the lowest ground along the outline less LevelMargin, so the drawn rim holds the water; or the actor's own height)
 * and makes the footprint: the cells inside the outline whose ground lies under the level, grown by one cell
 * (FVoxelWaterLake::MakeFromOutline, MakeFromOutlineOnSphere on a planet). It warns when nothing inside the outline
 * lies under the level, where the outline's own ground does (the water ends in a wall there), and where ground just
 * outside the outline does. The lake is added whatever the warnings say (GetBodyWarnings). There is no editor preview:
 * the lake shows in PIE.
 */
UCLASS(ClassGroup = (Voxel), meta = (BlueprintSpawnableComponent, DisplayName = "Voxel Lake"))
class VOXELSTREAMING_API UVoxelLakeComponent : public UVoxelWaterBodyComponent
{
	GENERATED_BODY()

public:
	/** Where the level comes from: the lowest ground along the outline, or the actor's own height */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lake")
	EVoxelLakeLevel LevelMode = EVoxelLakeLevel::BelowOutline;

	/** Below the outline: the level lies this far under the lowest ground along the outline, so the rim holds (world units; half a voxel by default) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lake", meta = (ClampMin = "0.0", EditCondition = "LevelMode == EVoxelLakeLevel::BelowOutline"))
	float LevelMargin = 37.5f;

	/** No cave opens within this many voxels under the lake's bed (0 leaves the caves their own depths) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lake", meta = (ClampMin = "0.0"))
	float MinCaveDepth = 8.0f;

	/**
	 * The width of a cell of the footprint, in voxels. The footprint follows the shore to within a cell, so a rim
	 * narrower than a cell wants a smaller one; a large lake draws fewer quads on a planet with a larger one.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lake|Advanced", meta = (ClampMin = "1.0", ClampMax = "32.0"))
	float CellSizeVoxels = 4.0f;

	/**
	 * Read the outline and make the lake, in a voxel world.
	 * @param Manager The world's chunk manager, initialized
	 * @param Out The footprint, the level and what was read
	 * @return False when there is no spline, the outline has fewer than three places or the manager is not initialized
	 */
	bool BuildLake(const UVoxelChunkManager& Manager, FVoxelLakeBuild& Out) const;

	/** @return What the registered lake was made of (no footprint before the world started) */
	const FVoxelLakeBuild& GetLakeBuild() const { return LakeBuild; }

protected:
	virtual bool AddBody(UVoxelChunkManager& Manager, FVoxelWaterBody& OutBody, TArray<FString>& OutWarnings) override;

private:
	/** What the registered lake was made of */
	FVoxelLakeBuild LakeBuild;
};

/**
 * A lake to drop into a level: a closed spline and a UVoxelLakeComponent on it. Drag the spline's points round the
 * hollow the lake should fill, along the ground that is to hold it, and the voxel world has the lake from its next
 * start. The actor is not replicated: it is in the level on every machine, which is what makes every machine generate
 * the same lake.
 */
UCLASS(meta = (DisplayName = "Voxel Lake"))
class VOXELSTREAMING_API AVoxelLakeActor : public AActor
{
	GENERATED_BODY()

public:
	AVoxelLakeActor();

	/** @return The closed spline that is the lake's outline */
	USplineComponent* GetSpline() const { return Spline; }

	/** @return The component that makes the lake */
	UVoxelLakeComponent* GetLake() const { return Lake; }

private:
	/** The lake's outline: a closed loop round the hollow */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lake", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USplineComponent> Spline;

	/** The lake's values */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lake", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UVoxelLakeComponent> Lake;
};
