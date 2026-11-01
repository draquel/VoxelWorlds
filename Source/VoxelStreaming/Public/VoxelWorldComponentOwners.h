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
class FVoxelFarField;
struct FVoxelFarFieldChanges;

/**
 * The owners of the components the chunk manager adds to its actor beyond the chunks' own: the walls at a bounded world's
 * edge, an island's far ocean, a planet's sea surface and the far field's patches. Each builds its components from settings the chunk manager
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

/**
 * The far field's components (spherical Phase 6 WM-BP9; every world mode since MP3): one procedural mesh component per
 * built patch at the patch's origin, section 0 the terrain, 1 the sea's top and 2 its underside. FVoxelFarField selects
 * and builds the patches; this applies an update's changes to the components (gone with their patches, filled when a
 * patch is built, its sections shown as the selection drew them) and keeps their memory estimate.
 */
USTRUCT()
struct VOXELSTREAMING_API FVoxelFarFieldPresenter
{
	GENERATED_BODY()

	/** Patches this many levels coarser than the edge level or finer get the sea's underside (seen only close by) */
	static constexpr int32 UndersideLevels = 4;

	/**
	 * Apply one far-field update's changes to the components.
	 * @param FarField         The far field (its built meshes are released once copied)
	 * @param Changes          What the update removed, built and reselected
	 * @param Owner            The actor new components are added to
	 * @param TerrainMaterial  The terrain's material (section 0)
	 * @param WaterMaterial    The water's material (sections 1 and 2)
	 * @param bUnderSea        Whether the camera is under the sea (the undersides show)
	 */
	void Apply(FVoxelFarField& FarField, const FVoxelFarFieldChanges& Changes, AActor* Owner, UMaterialInterface* TerrainMaterial,
		UMaterialInterface* WaterMaterial, bool bUnderSea);

	/** Destroy every component. */
	void Destroy();

	/** Start the sea's underside as hidden (a new far field). */
	void ResetUnderside() { bUndersideShown = false; }

	/** @return A built patch's component, or null */
	UProceduralMeshComponent* Find(uint64 PackedPatchKey) const;

	/** @return How many components are alive */
	int32 Num() const { return Components.Num(); }

	/** @return What the components hold on the CPU and the GPU (an estimate: a proc-mesh vertex and a GPU vertex) */
	int64 GetMemoryBytes() const;

private:
	/** One component per built patch (owned by the chunk manager's actor) */
	UPROPERTY(Transient)
	TMap<uint64, TObjectPtr<UProceduralMeshComponent>> Components;

	/** Vertices and indices each component holds (its memory estimate) */
	TMap<uint64, FIntPoint> Sizes;
	int64 Vertices = 0;
	int64 Indices = 0;

	/** Whether the sea's underside sections are shown (the viewer is inside the sea radius) */
	bool bUndersideShown = false;
};
