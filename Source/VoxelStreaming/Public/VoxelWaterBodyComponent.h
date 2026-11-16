// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "VoxelWaterLevels.h"
#include "VoxelWaterBodyComponent.generated.h"

class USplineComponent;
class UVoxelChunkManager;

/**
 * What a hand-placed river and a hand-placed lake share (UVoxelRiverComponent, UVoxelLakeComponent; the hydrology
 * plan's section 5.7, D144): a spline on the same actor, a water body made from it when the voxel world starts, and
 * the order the bodies of a level are made in.
 *
 * The body is registered before the chunk manager builds its first generation request: at BeginPlay when the voxel
 * world has started, or else when it starts (UVoxelChunkManager::OnInitialized), as a terrain line is. So every chunk
 * generates with it, on every machine: the actor is in the level on the server and on each client, and nothing is
 * replicated. A component that begins play after chunks have generated (an actor spawned at runtime) is added too,
 * with the chunk manager's warning: the chunks generated before it keep their voxels.
 *
 * The order. A river is made against the water it meets: it ends in the sea, a lake or another river, or leaves a
 * lake. So whichever body of a level registers first makes the ones that come before it register first: every lake
 * before any river, and of two rivers the one with the higher Order, then the one whose name sorts first. The order
 * depends on the level alone, so every machine makes the same bodies.
 */
UCLASS(Abstract, ClassGroup = (Voxel), HideCategories = (Activation, Cooking, AssetUserData, Collision, Navigation))
class VOXELSTREAMING_API UVoxelWaterBodyComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UVoxelWaterBodyComponent();

	/** The spline's name on the owning actor. None: the actor's first spline component */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Water Body")
	FName SplineName;

	/** @return The spline the body is made from: the owner's spline named SplineName, or its first; null when it has none */
	UFUNCTION(BlueprintPure, Category = "Voxel|Water Body")
	USplineComponent* GetSpline() const;

	/** @return Whether the body was made for a voxel world and added to its chunk manager (a lake with no water under its level is made and adds none) */
	UFUNCTION(BlueprintPure, Category = "Voxel|Water Body")
	bool IsBodyRegistered() const { return bRegistered; }

	/**
	 * @return What a level designer should know about the registered body, one sentence each: where it is not what was
	 *         drawn (a river lowered where its spline rises, an end on land, a lake's outline over ground under its
	 *         level) and the limits it breaks. Empty when there is nothing to say or the body is not registered
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Water Body")
	const TArray<FString>& GetBodyWarnings() const { return BodyWarnings; }

	/**
	 * The body's id: a hash of the owner's and the component's names (never the sea's 0), so it is stable for the same
	 * level and the same on every machine.
	 * @return The id
	 */
	uint32 GetWaterBodyId() const;

	/** @return The registered body's record (the sea's kind and no footprint before the world started, or when no body was made) */
	const FVoxelWaterBody& GetRegisteredBody() const { return RegisteredBody; }

	/**
	 * Make the body and add it to the manager, once; the bodies of this world that come before this one (the class
	 * comment's order) register first. BeginPlay and the chunk manager's start call it; a test may.
	 * @param Manager The world's chunk manager, initialized
	 */
	void RegisterBody(UVoxelChunkManager& Manager);

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

protected:
	/** 0 for a lake, 1 for a river: a lower rank registers first. */
	virtual int32 GetRegistrationRank() const { return 0; }

	/** Within a rank the higher order registers first (a river's Order). */
	virtual int32 GetRegistrationOrder() const { return 0; }

	/**
	 * Make the body's record and whatever comes with it (a river's bed), and add them to the manager.
	 * @param Manager The world's chunk manager, initialized
	 * @param OutBody The record that was added
	 * @param OutWarnings What the designer should know, one sentence each
	 * @return False when no body could be made (no spline, a spline with no length)
	 */
	virtual bool AddBody(UVoxelChunkManager& Manager, FVoxelWaterBody& OutBody, TArray<FString>& OutWarnings) PURE_VIRTUAL(UVoxelWaterBodyComponent::AddBody, return false;);

	/** @return The name the log calls this body by: its actor's */
	FString GetBodyName() const;

	/** @return The owner's and the component's names, joined: what the id and the order among equals are made from */
	FString GetBodyKey() const;

	/**
	 * The natural terrain's height at a place, in a line's terms: world Z over (X, Y) on a heightmap world, the altitude
	 * over the datum along a direction on a planet.
	 * @param Manager The world's chunk manager, initialized
	 * @param Place World (X, Y, any Z), or on a planet a unit direction from the planet centre
	 * @return The height
	 */
	static double GetNaturalHeight(const UVoxelChunkManager& Manager, const FVector& Place);

private:
	/** A chunk manager started: register with it when it is this world's. */
	void HandleChunkManagerInitialized(UVoxelChunkManager& Manager);

	/** @return Whether this body registers before another of the same world */
	bool ComesBefore(const UVoxelWaterBodyComponent& Other) const;

	/** The record added to the chunk manager */
	FVoxelWaterBody RegisteredBody;

	/** What the designer should know about the registered body */
	TArray<FString> BodyWarnings;

	bool bRegistered = false;

	/** Inside RegisterBody: a body that asks the ones before it to register is not asked back */
	bool bRegistering = false;

	/** Binding on UVoxelChunkManager::OnInitialized while the voxel world has not started */
	FDelegateHandle InitializedHandle;
};
