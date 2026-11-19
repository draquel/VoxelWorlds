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
 * What a hand-placed line did about its bends (UVoxelTerrainLineComponent; Documentation/TERRAIN_LINES_PLAN.md, section
 * 4.5, D134). A line sampled from a spline that keeps the bend bound is left as sampled, and every value here but the
 * two ratios is zero.
 */
struct FVoxelTerrainLineBends
{
	/** Whether the line as sampled from the spline breaks the bend bound */
	bool bBrokeBound = false;

	/**
	 * The line as sampled, at its worst vertex: the vertex's turn over the turn the bend bound allows there (the lesser
	 * of the turn whose bisector meets a neighbour's outside the reach and, on a line that levels, the turn whose step
	 * across the bisector stays under a tenth of a voxel on the grade there). Over 1 breaks the bound
	 */
	double DrawnBendRatio = 0.0;

	/** The same for the line as made: at most 1 unless the bound was out of the rounding's reach */
	double BendRatio = 0.0;

	/**
	 * The radius of the arcs the line's corners were rounded to (world units; FVoxelConditioningLineShape::FitCornerArcs).
	 * 0: every vertex is where the spline has it
	 */
	double CornerRadius = 0.0;

	/**
	 * The farthest the line as made runs from the spline, or the spline from it (world units, over the ground: a rounded
	 * corner's move, the arc's middle from the spline's corner); 0 when no corner was rounded
	 */
	double FarthestFromSpline = 0.0;

	/** Whether the profile was made again, flatter through the bends (a line that follows the terrain) */
	bool bProfileFlattened = false;

	/** @return Whether the line's corners were rounded, so that it leaves its spline */
	bool WasRounded() const { return CornerRadius > 0.0; }
};

/**
 * The values a spline is sampled to a conditioning line with (UVoxelTerrainLineComponent::BuildLineParamsFromSpline):
 * a terrain line component's own, or the ones another maker of a line from a spline gives (a hand-placed river's bed,
 * UVoxelRiverComponent). Each is the component's property of the same name.
 */
struct FVoxelTerrainLineSettings
{
	EVoxelTerrainLineHeight HeightMode = EVoxelTerrainLineHeight::FollowTerrain;
	float TerrainOffset = 0.0f;
	float SmoothingDistance = 1200.0f;
	float MaxGrade = 0.15f;
	float HalfWidth = 225.0f;
	float FalloffWidth = 300.0f;
	float Strength = 1.0f;
	EVoxelConditioningLineBlend Blend = EVoxelConditioningLineBlend::Level;
	FVoxelConditioningLineSurface Surface;
	float VertexSpacingVoxels = 2.0f;

	/** The line's id (UVoxelTerrainLineComponent::GetLineId) */
	int64 Id = 0;
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
 * A spline that turns more sharply than the bend bound allows (a corner, a tight curve on a slope) does not make the
 * line it draws: the terrain would step on the inside of the bend. The component rounds each such corner with an arc
 * of the radius the bound asks for (about the line's reach, the half width plus the falloff: G225,
 * FVoxelConditioningLineShape::FitCornerArcs), so a corner moves by the radius times (1 / cos(turn / 2) - 1): a fifth
 * of the reach at 60 degrees, a reach at 120; a corner near an end keeps the end and lets its direction go. A line
 * that follows the terrain then has its profile made again, flatter through what is left of each bend, and a line at
 * the spline's height keeps the spline's heights along it, with a wider radius where the turn must also fit its grade.
 * A spline that keeps the bound makes the line it draws, vertex for vertex. GetLineBends and the first sentence of
 * GetLineWarnings say what was done and how far the line runs from its spline (D134).
 *
 * The limits a line must respect (the bend bound, the cut and fill cap, the widths, a line that comes back on itself)
 * are checked on the line as made when it is registered and logged as warnings (GetLineWarnings); the line is added
 * whatever they say. A bend the rounding could not bring within the bound (a corner at the spline's height on a steep
 * grade, a hairpin of over 160 degrees, an S too tight for two arcs) is rounded as far as helps and keeps its warning.
 * There is no editor preview: the terrain shows the line in PIE.
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
	 * @return What a level designer should know about the registered line, one sentence each: first, when the spline
	 *         broke the bend bound, what the component did about it and how far the line runs from the spline at most;
	 *         then the limits the line as made breaks (the bend bound, the cut and fill cap, the widths, a line that
	 *         comes back on itself, ...). Empty when the spline's line keeps them all or is not registered
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
	 * Where that line breaks the bend bound its corners are rounded and, following the terrain, its profile is made
	 * again (the class comment); a line that keeps the bound is returned as sampled.
	 * @param Manager The world's chunk manager, initialized (its configuration and world mode say what the world is)
	 * @param OutParams The line's values
	 * @param OutBends What was done about the line's bends (optional)
	 * @return False when there is no spline, the spline has no length or the manager is not initialized
	 */
	bool BuildLineParams(const UVoxelChunkManager& Manager, FVoxelConditioningLineParams& OutParams, FVoxelTerrainLineBends* OutBends = nullptr) const;

	/** @return This component's values, as the settings a spline is sampled with */
	FVoxelTerrainLineSettings GetLineSettings() const;

	/**
	 * BuildLineParams for any spline and any values: what the component does with its own, for another maker of a line
	 * from a spline (a hand-placed river samples its bed's vertices with it, so a river's corners are rounded as a
	 * road's are).
	 * @param Spline The spline (null: no line)
	 * @param Manager The world's chunk manager, initialized
	 * @param Settings The values
	 * @param OutParams The line's values
	 * @param OutBends What was done about the line's bends (optional)
	 * @return False when there is no spline, the spline has no length or the manager is not initialized
	 */
	static bool BuildLineParamsFromSpline(const USplineComponent* Spline, const UVoxelChunkManager& Manager, const FVoxelTerrainLineSettings& Settings,
		FVoxelConditioningLineParams& OutParams, FVoxelTerrainLineBends* OutBends = nullptr);

	/**
	 * @param Bends What was done about a line's bends
	 * @return The sentence a line's first warning is when its spline broke the bend bound: what was done about it and how
	 *         far the line runs from the spline. Empty when the line is the spline's, vertex for vertex
	 */
	static FString DescribeBends(const FVoxelTerrainLineBends& Bends);

	/** @return The registered line; null before the world started */
	TSharedPtr<const FVoxelConditioningLine> GetRegisteredLine() const { return RegisteredLine; }

	/** @return What was done about the registered line's bends (all zero before the world started) */
	const FVoxelTerrainLineBends& GetLineBends() const { return LineBends; }

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** A chunk manager started: register with it when it is this world's. */
	void HandleChunkManagerInitialized(UVoxelChunkManager& Manager);

	/** Sample the spline and add the line to the manager, once. */
	void RegisterLine(UVoxelChunkManager& Manager);

	/** The line added to the chunk manager (kept: the manager's sources share it) */
	TSharedPtr<const FVoxelConditioningLine> RegisteredLine;

	/** What was done about the registered line's bends, and the limits the line as made breaks */
	TArray<FString> LineWarnings;

	/** What was done about the registered line's bends */
	FVoxelTerrainLineBends LineBends;

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
