// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelWorldModeTypes.h"
#include "VoxelFarOcean.h"
#include "VoxelPlanetOcean.h"
#include "VoxelWorldComponentOwners.generated.h"

class AActor;
class UBoxComponent;
class UProceduralMeshComponent;
class UMaterialInterface;

/**
 * The owners of the components the chunk manager adds to its actor beyond the chunks' own: the walls at a bounded world's
 * edge, an island's far ocean and a planet's sea surface. Each builds its components from settings the chunk manager
 * makes from the configuration and the world mode, and holds them (UPROPERTY, so the garbage collector sees them while
 * the chunk manager holds the owner). The geometry is the static builders' (FVoxelWorldBoundary, FVoxelFarOcean,
 * FVoxelPlanetOcean).
 */

/** A sea's underside: shown only while the viewer is under its surface (the far ocean, the planet ocean, the far field's sea). */
struct VOXELSTREAMING_API FVoxelSeaUnderside
{
	/**
	 * Apply a sea underside's visibility when it changes.
	 * @param bBelow  Whether the viewer is under the sea's surface now
	 * @param bShown  Whether the underside is shown (updated)
	 * @param Apply   Shows (true) or hides (false) the underside; called only on a change
	 */
	static void Update(bool bBelow, bool& bShown, TFunctionRef<void(bool)> Apply);
};

/** The invisible collision walls at a bounded world's edge (world modes review WM-I5), box components of the actor. */
USTRUCT()
struct VOXELSTREAMING_API FVoxelWorldBoundaryOwner
{
	GENERATED_BODY()

	/**
	 * Build the walls along a bounded world's edge (FVoxelWorldBoundary::BuildWalls), replacing any.
	 * @param Owner           The actor the walls are added to
	 * @param Bounds          The world's footprint (IVoxelWorldMode::GetWorldBounds)
	 * @param ChunkWorldSize  A chunk's width: the walls reach this far under the lowest terrain
	 * @param bCollision      Whether the walls block (voxel.World.Boundary)
	 */
	void Build(AActor* Owner, const FVoxelWorldBounds& Bounds, double ChunkWorldSize, bool bCollision);

	/** Destroy the walls. */
	void Destroy();

	/** Turn the walls' collision on or off; the walls stay. */
	void SetCollisionEnabled(bool bEnabled);

	/** @return How many walls stand */
	int32 Num() const { return Walls.Num(); }

private:
	/** The walls, owned by the chunk manager's actor */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UBoxComponent>> Walls;
};

/** An island's far ocean (world modes review WM-E1): one procedural mesh component, the seabed and the open sea. */
USTRUCT()
struct VOXELSTREAMING_API FVoxelFarOceanOwner
{
	GENERATED_BODY()

	/**
	 * Build the far ocean's component, replacing any.
	 * @param Owner            The actor the component is added to
	 * @param InSettings       What it covers (made by the chunk manager from the configuration)
	 * @param SeabedMaterial   The terrain's material (section 0)
	 * @param WaterMaterial    The water tiles' material (sections 1 and 2); null draws no water
	 * @param bVisible         Whether it is shown (voxel.World.FarOcean)
	 * @param ViewerPosition   The streaming viewer: decides the underside's first state
	 * @param StartTime        When the build began (FPlatformTime::Seconds), for the log line
	 */
	void Build(AActor* Owner, const FVoxelFarOceanSettings& InSettings, UMaterialInterface* SeabedMaterial,
		UMaterialInterface* WaterMaterial, bool bVisible, const FVector& ViewerPosition, double StartTime);

	/** Destroy the component. */
	void Destroy();

	/** Show the water's underside while the viewer is under the water plane, hide it when it is not. */
	void UpdateUnderside(const FVector& ViewerPosition);

	/** Show or hide the far ocean (voxel.World.FarOcean). */
	void SetVisible(bool bVisible);

	/** @return One-line summary for logs (voxel.World.FarOceanStats), or "none" without a far ocean */
	FString Describe() const;

	/** @return The component: section 0 the seabed, 1 the open-sea water, 2 its underside (null without one) */
	UProceduralMeshComponent* GetComponent() const { return Component; }

	/** @return What the far ocean was built with */
	const FVoxelFarOceanSettings& GetSettings() const { return Settings; }

private:
	/** The far ocean's meshes (owned by the chunk manager's actor) */
	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> Component;

	/** What the far ocean was built with */
	FVoxelFarOceanSettings Settings;

	/** Whether the water's underside (section 2) is shown: only while the viewer is under the water plane */
	bool bUndersideShown = false;
};

/** A planet's sea surface (FVoxelPlanetOcean): one procedural mesh component at the planet centre. */
USTRUCT()
struct VOXELSTREAMING_API FVoxelPlanetOceanOwner
{
	GENERATED_BODY()

	/**
	 * Build the planet ocean's component, replacing any.
	 * @param Owner           The actor the component is added to
	 * @param InSettings      Its centre, radius and resolution
	 * @param WaterMaterial   The configuration's water material
	 * @param bVisible        Whether it is shown (voxel.World.PlanetOcean)
	 * @param ViewerPosition  The streaming viewer: decides the underside's first state
	 */
	void Build(AActor* Owner, const FVoxelPlanetOceanSettings& InSettings, UMaterialInterface* WaterMaterial, bool bVisible,
		const FVector& ViewerPosition);

	/** Destroy the component. */
	void Destroy();

	/** Show the underside while the viewer is inside the sea radius, hide it outside. */
	void UpdateUnderside(const FVector& ViewerPosition);

	/** Show or hide the planet ocean (voxel.World.PlanetOcean). */
	void SetVisible(bool bVisible);

	/** @return The component: section 0 the top, 1 the underside (null without one) */
	UProceduralMeshComponent* GetComponent() const { return Component; }

	/** @return Whether the underside is shown now */
	bool IsUndersideShown() const { return bUndersideShown; }

	/** @return What the planet ocean was built with (its defaults until it is built: the resolution limits) */
	const FVoxelPlanetOceanSettings& GetSettings() const { return Settings; }

private:
	/** The sea surface's mesh (owned by the chunk manager's actor) */
	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> Component;

	/** What the planet ocean was built with */
	FVoxelPlanetOceanSettings Settings;

	/** Whether the underside (section 1) is shown: only while the viewer is inside the sea radius */
	bool bUndersideShown = false;
};
