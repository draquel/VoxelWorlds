// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "VoxelWorldActor.generated.h"

class UVoxelMaterialAtlas;
class UVoxelChunkManager;
class UVoxelWorldConfiguration;
class IVoxelLODStrategy;
class IVoxelMeshRenderer;

/**
 * The voxel world in a level. Owns the chunk manager, builds the LOD strategy and the renderer from its configuration,
 * material and atlas, and streams from BeginPlay to EndPlay. Place one (or a Blueprint of it) in a level; the game's
 * demo tooling (the game module's AVoxelDemoActor: the mouse brush, the crosshair, the performance HUD, the fallback
 * water plane) derives from it. Until code review CR1 (D-CR11) this was AVoxelWorldTestActor with the demo half on it;
 * a class redirect in DefaultEngine.ini loads the old name as the demo actor.
 */
UCLASS(Blueprintable, meta = (DisplayName = "Voxel World Actor"))
class VOXELSTREAMING_API AVoxelWorldActor : public AActor
{
	GENERATED_BODY()

public:
	AVoxelWorldActor();

	// ==================== AActor Interface ====================

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

	// ==================== Configuration ====================

	/** Optional configuration asset. If null, default config is created. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Configuration")
	TObjectPtr<UVoxelWorldConfiguration> Configuration;

	/** Voxel size in world units (cm). Used when creating default config. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Configuration", meta = (EditCondition = "Configuration == nullptr", ClampMin = "10", ClampMax = "1000"))
	float VoxelSize = 100.0f;

	/** Chunk size in voxels per edge. Used when creating default config. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Configuration", meta = (EditCondition = "Configuration == nullptr", ClampMin = "8", ClampMax = "64"))
	int32 ChunkSize = 32;

	/** View distance for chunk loading. Used when creating default config. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Configuration", meta = (EditCondition = "Configuration == nullptr", ClampMin = "1000"))
	float ViewDistance = 10000.0f;

	/** Sea level height for terrain generation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Configuration", meta = (EditCondition = "Configuration == nullptr"))
	float SeaLevel = 0.0f;

	/** Height scale for terrain variation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Configuration", meta = (EditCondition = "Configuration == nullptr", ClampMin = "100"))
	float HeightScale = 3000.0f;

	/**
	 * Material for voxel rendering when using Custom Vertex Factory renderer.
	 * REQUIRED: Create a simple opaque surface material and assign it here.
	 * The default engine materials do NOT work with the custom vertex factory.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Rendering")
	TObjectPtr<UMaterialInterface> VoxelMaterial;

	/**
	 * Material atlas for texture lookup and face variants.
	 *
	 * The atlas defines how MaterialIDs map to texture atlas positions.
	 * Supports per-face texture variants (top/side/bottom).
	 *
	 * Create a VoxelMaterialAtlas data asset in the Content Browser,
	 * configure materials and atlas positions, then assign it here.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Rendering")
	TObjectPtr<UVoxelMaterialAtlas> MaterialAtlas;

	/** Enable debug visualization */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Debug")
	bool bDrawDebugVisualization = false;

	/** Print stats to log every N seconds (0 = disabled) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Debug", meta = (ClampMin = "0"))
	float DebugStatsPrintInterval = 5.0f;

	// ==================== Runtime Access ====================

	/** Get the chunk manager component */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	UVoxelChunkManager* GetChunkManager() const { return ChunkManager; }

	/** Print current debug statistics to log */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Debug")
	void PrintDebugStats();

	/** Force a streaming update */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	void ForceStreamingUpdate();

	/**
	 * Get the recommended spawn position (UVoxelChunkManager::GetSpawnPosition): on a planet PlanetSpawnAltitude over the
	 * terrain along the PlanetSpawnLocation direction, or over the datum before the world starts; in the heightmap modes
	 * the configuration's WorldOrigin. GetPlanetSpawnPosition until CR3 (CM-22).
	 * @return World position
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	FVector GetSpawnPosition() const;

	/**
	 * The configuration the world runs on: the Configuration asset (or its runtime override), else the one made from this
	 * actor's own properties at BeginPlay. Null before BeginPlay.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	UVoxelWorldConfiguration* GetActiveConfiguration() const;

	/** Whether BeginPlay built the world (the chunk manager streams). */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	bool IsVoxelWorldInitialized() const { return bIsVoxelWorldInitialized; }

protected:
	/** Initialize the voxel world systems */
	void InitializeVoxelWorld();

	/** Shutdown the voxel world systems */
	void ShutdownVoxelWorld();

	/** Create default configuration if none provided */
	UVoxelWorldConfiguration* CreateDefaultConfiguration();

	/** Chunk manager component */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel")
	TObjectPtr<UVoxelChunkManager> ChunkManager;

	/** LOD strategy (owned by this actor) */
	IVoxelLODStrategy* LODStrategy = nullptr;

	/** Mesh renderer (owned by this actor) */
	IVoxelMeshRenderer* MeshRenderer = nullptr;

	/** Runtime-created configuration (if no asset provided) */
	UPROPERTY()
	TObjectPtr<UVoxelWorldConfiguration> RuntimeConfiguration;

	/** Timer for debug stats printing */
	float DebugStatsTimer = 0.0f;

	/** Whether the voxel world has been initialized */
	bool bIsVoxelWorldInitialized = false;
};
