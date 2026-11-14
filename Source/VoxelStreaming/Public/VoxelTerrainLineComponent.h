// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "VoxelConditioningLine.h"
#include "VoxelTerrainLineComponent.generated.h"

class USplineComponent;
class UVoxelChunkManager;

/** Where a hand-placed terrain line takes its target heights from. */
UENUM(BlueprintType)
enum class EVoxelTerrainLineHeight : uint8
{
	/**
	 * The natural terrain along the line, averaged along it (less toward each end, where the line meets the ground),
	 * held to a grade, plus an offset. The spline's own height is not read: only where it runs over the ground. The
	 * default, since nothing draws the voxel terrain in the editor.
	 */
	FollowTerrain,

	/** The spline's own height: its world Z on a heightmap world, its altitude over the datum on a planet */
	SplineHeight
};

/**
 * A hand-placed conditioning line: turns a USplineComponent on the same actor into a conditioning line of the level's
 * voxel world (Documentation/TERRAIN_LINES_PLAN.md, section 4.5; the how-to is Documentation/Guides/TERRAIN_LINES.md in
 * the plugin). A road, a cutting, an embankment, a painted trail or a river bed drawn in a level.
 *
 * When the world starts the component samples the spline to a polyline (a vertex every VertexSpacingVoxels voxels along
 * it), gives each vertex a target height (HeightMode) and a half width (HalfWidth times the spline's scale Y there), and
 * adds the line to the world's chunk manager with the line's own values (the falloff, the strength, the blend, the
 * surface). On a planet the vertices are the directions of the sampled points from the planet centre and the targets
 * altitudes.
 *
 * The line is registered before the chunk manager builds its first generation request: at BeginPlay when the voxel
 * world has started, or else when it starts (UVoxelChunkManager::OnInitialized). So every chunk generates with it, on
 * every machine: the actor is in the level on the server and on each client, and nothing is replicated. A component
 * that begins play after chunks have generated (an actor spawned at runtime) is added too, with the chunk manager's
 * warning: the chunks generated before it keep their terrain.
 *
 * The limits a line must respect (the bend bound on the sampled spline, the cut and fill cap, the widths, a line that
 * comes back on itself) are checked when it is registered and logged as warnings (GetLineWarnings); the line is added
 * whatever they say. There is no editor preview: the terrain shows the line in PIE.
 */
UCLASS(ClassGroup = (Voxel), meta = (BlueprintSpawnableComponent), HideCategories = (Activation, Cooking, AssetUserData, Collision, Navigation))
class VOXELSTREAMING_API UVoxelTerrainLineComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UVoxelTerrainLineComponent();

	/** The spline's name on the owning actor. None: the actor's first spline component */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Terrain Line")
	FName SplineName;

	/** Where the target heights come from */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Terrain Line|Height")
	EVoxelTerrainLineHeight HeightMode = EVoxelTerrainLineHeight::FollowTerrain;

	/** Follow the terrain: added to the target (world units; under zero for a cutting or a bed, over it for a causeway) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Terrain Line|Height", meta = (EditCondition = "HeightMode == EVoxelTerrainLineHeight::FollowTerrain"))
	float TerrainOffset = 0.0f;

	/**
	 * Follow the terrain: the natural height is averaged over this far each way along the line (world units; 0: not at
	 * all). The average narrows toward each end, so the line meets the ground at its ends.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Terrain Line|Height", meta = (ClampMin = "0.0", EditCondition = "HeightMode == EVoxelTerrainLineHeight::FollowTerrain"))
	float SmoothingDistance = 1200.0f;

	/**
	 * Follow the terrain: the target's steepest slope along the line, as a fraction (0.15 is 15 %; 0: no limit). A line
	 * that levels turns less sharply the steeper it is (the bend bound), so a road through curves wants one.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Terrain Line|Height", meta = (ClampMin = "0.0", EditCondition = "HeightMode == EVoxelTerrainLineHeight::FollowTerrain"))
	float MaxGrade = 0.15f;

	/**
	 * Half the width of the fully blended core (world units). Each spline point's scale Y multiplies it there (1 by
	 * default), so a road widens where a point is scaled up.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Terrain Line|Shape", meta = (ClampMin = "0.0"))
	float HalfWidth = 225.0f;

	/** Width (world units) of the falloff outside the core, easing back to the natural terrain */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Terrain Line|Shape", meta = (ClampMin = "0.0"))
	float FalloffWidth = 300.0f;

	/** Strength [0, 1]: 1 reaches the target on the core, 0 leaves the height alone (a line that only paints and clears) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Terrain Line|Shape", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Strength = 1.0f;

	/** How the terrain moves toward the target: level (cut and fill), lower only (a cutting, a bed) or raise only */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Terrain Line|Shape")
	EVoxelConditioningLineBlend Blend = EVoxelConditioningLineBlend::Level;

	/** The ground along the line: a band of material, the caves' minimum depth under it, the width kept clear */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Terrain Line|Surface")
	FVoxelConditioningLineSurface Surface;

	/** A vertex every this many voxels along the spline (2: what the bend bound is stated for; more makes bends coarser) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Terrain Line|Advanced", meta = (ClampMin = "0.25", ClampMax = "16.0"))
	float VertexSpacingVoxels = 2.0f;

	/**
	 * Where lines meet, the one with the higher order is applied later and so wins on its core. Lines of one order are
	 * applied in a fixed order of their own (by the actor's and the component's names), the same on every machine.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Terrain Line|Advanced")
	int32 Order = 0;

	/** @return The spline the line is sampled from: the owner's spline named SplineName, or its first; null when it has none */
	UFUNCTION(BlueprintPure, Category = "Voxel|Terrain Line")
	USplineComponent* GetSpline() const;

	/** @return Whether the line was added to a voxel world's chunk manager */
	UFUNCTION(BlueprintPure, Category = "Voxel|Terrain Line")
	bool IsLineRegistered() const { return RegisteredLine.IsValid(); }

	/**
	 * @return The limits the registered line breaks, one sentence each (the bend bound, the cut and fill cap, the widths,
	 *         a line that comes back on itself, ...); empty when it keeps them all or is not registered
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Terrain Line")
	const TArray<FString>& GetLineWarnings() const { return LineWarnings; }

	/**
	 * The id the line is ordered by: Order in the high half and a hash of the owner's and the component's names in the
	 * low half, so it is stable for the same level and the same on every machine.
	 * @return The id
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Terrain Line")
	int64 GetLineId() const;

	/**
	 * Sample the spline to the values a conditioning line is made from, in a voxel world: the vertices every
	 * VertexSpacingVoxels voxels, their targets by HeightMode and their half widths, and this component's line values.
	 * @param Manager The world's chunk manager, initialized (its configuration and world mode say what the world is)
	 * @param OutParams The line's values
	 * @return False when there is no spline, the spline has no length or the manager is not initialized
	 */
	bool BuildLineParams(const UVoxelChunkManager& Manager, FVoxelConditioningLineParams& OutParams) const;

	/** @return The registered line; null before the world started */
	TSharedPtr<const FVoxelConditioningLine> GetRegisteredLine() const { return RegisteredLine; }

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** A chunk manager started: register with it when it is this world's. */
	void HandleChunkManagerInitialized(UVoxelChunkManager& Manager);

	/** Sample the spline and add the line to the manager, once. */
	void RegisterLine(UVoxelChunkManager& Manager);

	/** The line added to the chunk manager (kept: the manager's sources share it) */
	TSharedPtr<const FVoxelConditioningLine> RegisteredLine;

	/** The limits the registered line breaks */
	TArray<FString> LineWarnings;

	/** Binding on UVoxelChunkManager::OnInitialized while the voxel world has not started */
	FDelegateHandle InitializedHandle;
};

/**
 * A terrain line to drop into a level: a spline and a UVoxelTerrainLineComponent on it. Drag the spline's points over
 * where the line should run, set the component's values, and the voxel terrain carries the line from the next start of
 * the world. The actor is not replicated: it is in the level on every machine, which is what makes every machine
 * generate the same terrain.
 */
UCLASS(meta = (DisplayName = "Voxel Terrain Line"))
class VOXELSTREAMING_API AVoxelTerrainLineActor : public AActor
{
	GENERATED_BODY()

public:
	AVoxelTerrainLineActor();

	/** @return The spline the line follows */
	USplineComponent* GetSpline() const { return Spline; }

	/** @return The component that makes the conditioning line */
	UVoxelTerrainLineComponent* GetTerrainLine() const { return TerrainLine; }

private:
	/** Where the line runs: its points' positions and, through their scale Y, the half width at each */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Terrain Line", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USplineComponent> Spline;

	/** The line's values */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Terrain Line", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UVoxelTerrainLineComponent> TerrainLine;
};
