// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelConditioningLine.h"
#include "VoxelConditioningLineShape.h"
#include "VoxelTerrainLineComponent.h"
#include "VoxelWaterBodyComponent.h"
#include "VoxelRiverComponent.generated.h"

/** What a hand-placed river is made of (UVoxelRiverComponent::BuildRiver): its bed's line, its river reach, and what was done to make them. */
struct FVoxelRiverBuild
{
	/** The bed: a lower-only conditioning line through the river's vertices, its targets the bed's heights */
	FVoxelConditioningLineParams Bed;

	/** The river reach's vertices, the bed's own: the bed's and the surface's water levels and the footprint's half width */
	TArray<FVoxelWaterReachVertex> Vertices;

	/** What was done about the spline's bends (the terrain line's rounding) */
	FVoxelTerrainLineBends Bends;

	/** The profile over every vertex of the line as sampled, and the ones the river keeps (First, Num) */
	FVoxelConditioningLineShape::FRiverProfile Profile;

	/** The line as sampled from the spline: how many vertices, before the ones deep inside another body were dropped */
	int32 NumSampled = 0;
};

/**
 * A hand-placed river (the hydrology plan's section 5.7, D144; the how-to is Documentation/Guides/TERRAIN_LINES.md in
 * the plugin): turns a USplineComponent on the same actor, drawn from the river's head to its mouth, into a river
 * reach of the level's voxel world and the bed it runs in. The bed is a lower-only conditioning line and the water a
 * body of the water levels, made from the same vertices, so the voxels hold water, a swimmer is carried downstream,
 * the map shows the river and a mesh draws it, in every world mode.
 *
 * When the world starts the component samples the spline as a terrain line does (a vertex every VertexSpacingVoxels
 * voxels, the corners rounded where the spline breaks the bend bound) and gives the vertices a profile by the rules
 * that keep a river watertight (FVoxelConditioningLineShape::MakeRiverProfile; the plan's section 5.3):
 *
 *  - Following the terrain (the default: nothing draws the voxel terrain in the editor) the surface lies
 *    SurfaceBelowGround under the natural ground along the line. At the spline's height the spline's own heights are
 *    the surface.
 *  - Either way the surface is only ever lowered: where it would rise downstream, and where a bank's ground, out to
 *    the footprint's edge, does not stand BankMargin over it. A spline that climbs is lowered and the log says so.
 *  - The bed lies Depth under the surface, HalfWidth wide to each side with a bank BankWidth wide beyond it.
 *  - At its head the river's depth and width start from nothing over HeadLength. A river that ends in no other water
 *    thins to nothing the same way, with a warning; one that ends in the sea, a lake or another river comes down to
 *    that water's level and stops just inside it; one whose head lies in a lake starts at the lake's level, and its
 *    bed notches the rim.
 *
 * What is drawn is the river's whole length; no ground is raised. The river is added whatever the warnings say
 * (GetBodyWarnings). There is no editor preview: the river shows in PIE.
 */
UCLASS(ClassGroup = (Voxel), meta = (BlueprintSpawnableComponent, DisplayName = "Voxel River"))
class VOXELSTREAMING_API UVoxelRiverComponent : public UVoxelWaterBodyComponent
{
	GENERATED_BODY()

public:
	UVoxelRiverComponent();

	/** Where the surface comes from: the natural ground along the line, or the spline's own height */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "River|Height")
	EVoxelTerrainLineHeight HeightMode = EVoxelTerrainLineHeight::FollowTerrain;

	/** Follow the terrain: the surface lies this far under the natural ground along the line (world units; a voxel by default) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "River|Height", meta = (ClampMin = "0.0", EditCondition = "HeightMode == EVoxelTerrainLineHeight::FollowTerrain"))
	float SurfaceBelowGround = 75.0f;

	/** Follow the terrain: the ground is averaged this far each way along the line before the surface is set under it (world units; 0: not at all) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "River|Height", meta = (ClampMin = "0.0", EditCondition = "HeightMode == EVoxelTerrainLineHeight::FollowTerrain"))
	float SmoothingDistance = 600.0f;

	/**
	 * The surface stays this far under the lowest ground of both banks, out to the footprint's edge (world units). Where a
	 * bank is lower the surface is lowered there and from there on, so the water never stands over its banks.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "River|Height", meta = (ClampMin = "0.0"))
	float BankMargin = 37.5f;

	/** The water over the bed where the river is at its full depth (world units; two voxels by default) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "River|Shape", meta = (ClampMin = "0.0"))
	float Depth = 150.0f;

	/**
	 * Half the width of the bed's flat floor (world units). Each spline point's scale Y multiplies it there (1 by
	 * default), so the river widens where a point is scaled up.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "River|Shape", meta = (ClampMin = "0.0"))
	float HalfWidth = 225.0f;

	/** The width of each bank: outside the floor the bed eases back up to the natural ground over this far (world units) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "River|Shape", meta = (ClampMin = "0.0"))
	float BankWidth = 300.0f;

	/**
	 * The river's depth and width start from nothing at its head and reach their full values over this length; a river
	 * that ends on land thins to nothing over as much (world units; 0: full width and depth from the first vertex).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "River|Shape", meta = (ClampMin = "0.0"))
	float HeadLength = 600.0f;

	/**
	 * How fast the river runs downstream (world units a second; 0: still): what carries a swimmer and moves the
	 * ripples. A swimmer makes about 150.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "River", meta = (ClampMin = "0.0"))
	float FlowSpeed = 100.0f;

	/** The bed's ground: a band of material, the caves' minimum depth under it, the width kept clear of scatter and trees */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "River|Surface")
	FVoxelConditioningLineSurface Surface;

	/** A vertex every this many voxels along the spline (2: what the bend bound is stated for) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "River|Advanced", meta = (ClampMin = "0.25", ClampMax = "16.0"))
	float VertexSpacingVoxels = 2.0f;

	/**
	 * Where rivers meet, the one with the higher order is made first and the other ends in it; its bed is also applied
	 * later and so wins where the two beds meet (a terrain line's Order). Give a river a higher order than its
	 * tributaries. Rivers of one order are made in the order of their actors' names.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "River|Advanced")
	int32 Order = 0;

	/**
	 * The id the bed's line is ordered by, as a terrain line's: Order in the high half and a hash of the owner's and
	 * the component's names in the low half.
	 * @return The id
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|River")
	int64 GetLineId() const;

	/**
	 * Sample the spline and make the river: the bed's line and the river reach's vertices, in a voxel world. The profile
	 * meets the water bodies the manager holds when it is called (the lakes and the rivers made before this one, a
	 * source's) and the sea.
	 * @param Manager The world's chunk manager, initialized
	 * @param Out The bed, the vertices and what was done
	 * @return False when there is no spline, the spline has no length or the manager is not initialized
	 */
	bool BuildRiver(const UVoxelChunkManager& Manager, FVoxelRiverBuild& Out) const;

	/** @return The registered bed's line; null before the world started */
	TSharedPtr<const FVoxelConditioningLine> GetRegisteredBed() const { return RegisteredBed; }

	/** @return What the registered river was made of (empty before the world started) */
	const FVoxelRiverBuild& GetRiverBuild() const { return RiverBuild; }

protected:
	virtual int32 GetRegistrationRank() const override { return 1; }
	virtual int32 GetRegistrationOrder() const override { return Order; }
	virtual bool AddBody(UVoxelChunkManager& Manager, FVoxelWaterBody& OutBody, TArray<FString>& OutWarnings) override;

private:
	/** The bed added to the chunk manager (kept: the manager's sources share it) */
	TSharedPtr<const FVoxelConditioningLine> RegisteredBed;

	/** What the registered river was made of */
	FVoxelRiverBuild RiverBuild;
};

/**
 * A river to drop into a level: a spline and a UVoxelRiverComponent on it. Drag the spline's points from where the
 * river rises to where it runs into the sea or a lake, set the component's values, and the voxel world carries the
 * river from its next start. The actor is not replicated: it is in the level on every machine, which is what makes
 * every machine generate the same river.
 */
UCLASS(meta = (DisplayName = "Voxel River"))
class VOXELSTREAMING_API AVoxelRiverActor : public AActor
{
	GENERATED_BODY()

public:
	AVoxelRiverActor();

	/** @return The spline the river follows, head to mouth */
	USplineComponent* GetSpline() const { return Spline; }

	/** @return The component that makes the river */
	UVoxelRiverComponent* GetRiver() const { return River; }

private:
	/** Where the river runs, head to mouth: its points' positions and, through their scale Y, the half width at each */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "River", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USplineComponent> Spline;

	/** The river's values */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "River", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UVoxelRiverComponent> River;
};
