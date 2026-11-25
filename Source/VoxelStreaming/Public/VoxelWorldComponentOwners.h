// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelWorldModeTypes.h"
#include "VoxelFarOcean.h"
#include "VoxelPlanetOcean.h"
#include "VoxelWaterLevels.h"
#include "VoxelWorldComponentOwners.generated.h"

class AActor;
class UBoxComponent;
class UProceduralMeshComponent;
class UVoxelFarFieldPatchComponent;
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
 * The drawn lakes and river reaches (the hydrology plan's section 5.6, D147): one procedural mesh component a body at
 * the body's own origin, section 0 its surface and section 1 its underside (FVoxelWaterBodyMesh), in every world mode,
 * inside the voxel window and beyond it. The chunk manager hands it the bodies to draw whenever they change; a body
 * already drawn is kept, a new one built, one no longer handed over destroyed. A lake draws with the sea's material, a
 * river reach with the river's (the same master, its ripples moving downstream). A dedicated server builds none.
 */
USTRUCT()
struct VOXELSTREAMING_API FVoxelWaterBodyMeshOwner
{
	GENERATED_BODY()

	/** What one Update did */
	struct FUpdateResult
	{
		int32 Built = 0;
		int32 Removed = 0;
		int32 Kept = 0;
		/** Bodies still to build: the tick budget ran out (G282); the next Update with the same bodies builds them */
		int32 Pending = 0;
	};

	/**
	 * Draw exactly these bodies: build the ones not drawn yet, keep the ones that are (a body is the same while its id,
	 * its level and its footprint's own data are), destroy the rest.
	 * @param Owner          The actor the components are added to
	 * @param Bodies         The lakes and river reaches to draw (a sea record is skipped)
	 * @param Frame          The world's frame
	 * @param VoxelSize      The voxel size (the meshes' UVs count voxels)
	 * @param LakeMaterial   The water's material (a lake's)
	 * @param RiverMaterial  A river reach's material (null: the lake's)
	 * @param bVisible       Whether the meshes are shown (voxel.Water.BodyMeshes)
	 * @param ViewerPosition The viewer: decides each underside's first state
	 * @param Budget         The chunk manager's tick budget (G282): with one, the meshes of the bodies not drawn yet are built
	 *                       on the thread pool (one body's mesh took 10 to 14 ms on VoxelDemo), and the built ones get their
	 *                       components nearest the viewer first while the budget has time left, the first always; the rest
	 *                       are left Pending for the next Update with the same bodies. Null builds them all here, in the
	 *                       order given (as before)
	 * @return What was built, kept, removed and left pending
	 */
	FUpdateResult Update(AActor* Owner, TConstArrayView<FVoxelWaterBody> Bodies, const FVoxelWaterFrame& Frame, double VoxelSize,
		UMaterialInterface* LakeMaterial, UMaterialInterface* RiverMaterial, bool bVisible, const FVector& ViewerPosition,
		const struct FVoxelTickBudget* Budget = nullptr);

	/** Destroy every component. */
	void Destroy();

	/** Show each body's underside while the viewer is under that body's surface, in its footprint; hide it otherwise. */
	void UpdateUndersides(const FVector& ViewerPosition);

	/** Show or hide every body's mesh (voxel.Water.BodyMeshes). */
	void SetVisible(bool bVisible);

	/** @return How many bodies are drawn */
	int32 Num() const { return Entries.Num(); }

	/** @return The component that draws a body (section 0 its surface, 1 its underside), or null */
	UProceduralMeshComponent* Find(uint32 BodyId) const;

	/** @return Whether a body's underside is shown now (false for a body not drawn) */
	bool IsUndersideShown(uint32 BodyId) const;

	/** @return The quads drawn over all bodies */
	int32 GetNumQuads() const;

private:
	/** One drawn body */
	struct FEntry
	{
		/** The record the mesh was built from */
		FVoxelWaterBody Body;
		/** The body alone as water levels: what the underside's test samples */
		FVoxelWaterLevels Levels;
		int32 Quads = 0;
		bool bUndersideShown = false;
	};

	/** One component per drawn body, in the entries' order (owned by the chunk manager's actor) */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UProceduralMeshComponent>> Components;

	/** The drawn bodies, parallel to Components */
	TArray<FEntry> Entries;

	/** Counts the components made, for their names */
	int32 Serial = 0;

	/** A body's mesh built on the thread pool (G282): written by the job, read once bDone is set. */
	struct FBuildSlot;
	struct FBuildJob
	{
		FVoxelWaterBody Body;
		TSharedPtr<FBuildSlot, ESPMode::ThreadSafe> Slot;
	};

	/** The bodies whose meshes are building on the thread pool or are built and wait for their components (G282) */
	TArray<FBuildJob> Building;

	/** Bodies whose mesh came out empty on the thread pool: not built again while handed over (G282) */
	TArray<FVoxelWaterBody> Unbuildable;

	/** Make a built body's component and entry. */
	void AddEntry(AActor* Owner, const FVoxelWaterBody& Body, const FVoxelWaterFrame& Frame, const struct FVoxelWaterBodyMesh& Mesh,
		UMaterialInterface* LakeMaterial, UMaterialInterface* RiverMaterial, bool bVisible);
};

/**
 * The far field's components (spherical Phase 6 WM-BP9; every world mode since MP3): one UVoxelFarFieldPatchComponent per
 * built patch at the patch's origin, section 0 the terrain, 1 the sea's top and 2 its underside (only the terrain traced, G121). FVoxelFarField selects
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
	 * @param Budget           The chunk manager's tick budget (G282): with one, a removed patch's component is hidden at once
	 *                         and destroyed within the budget, the first each update always; null destroys it at once
	 */
	void Apply(FVoxelFarField& FarField, const FVoxelFarFieldChanges& Changes, AActor* Owner, UMaterialInterface* TerrainMaterial,
		UMaterialInterface* WaterMaterial, bool bUnderSea, const struct FVoxelTickBudget* Budget = nullptr);

	/** @return Components of removed patches, hidden, still waiting to be destroyed (G282) */
	int32 GetPendingDestroyCount() const { return PendingDestroys.Num(); }

	/** Destroy every component. */
	void Destroy();

	/** Start the sea's underside as hidden (a new far field). */
	void ResetUnderside() { bUndersideShown = false; }

	/** @return A built patch's component, or null */
	UVoxelFarFieldPatchComponent* Find(uint64 PackedPatchKey) const;

	/** @return How many components are alive */
	int32 Num() const { return Components.Num(); }

	/** @return What the components hold on the CPU and the GPU (an estimate: a component's vertex and a GPU vertex) */
	int64 GetMemoryBytes() const;

private:
	/** One component per built patch (owned by the chunk manager's actor) */
	UPROPERTY(Transient)
	TMap<uint64, TObjectPtr<UVoxelFarFieldPatchComponent>> Components;

	/** Vertices and indices each component holds (its memory estimate) */
	TMap<uint64, FIntPoint> Sizes;
	int64 Vertices = 0;
	int64 Indices = 0;

	/** Whether the sea's underside sections are shown (the viewer is inside the sea radius) */
	bool bUndersideShown = false;

	/** The ray tracing mode the components were given (voxel.World.FarFieldRayTracing; -1 before the first update, G117) */
	int32 RayTracingMode = -1;

	/**
	 * Components of removed patches, hidden, to be destroyed within the tick budget (G282): a teleport removes the place
	 * left's patches together, about 500 on VoxelDemo, whose destroys took 12 ms of one update
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UVoxelFarFieldPatchComponent>> PendingDestroys;
};
