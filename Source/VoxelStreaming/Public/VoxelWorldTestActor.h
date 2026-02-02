// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "VoxelWorldTestActor.generated.h"

// Forward declarations
class UVoxelChunkManager;
class UVoxelWorldConfiguration;
class IVoxelLODStrategy;
class IVoxelMeshRenderer;

/**
 * Test actor for verifying the voxel chunk generation pipeline.
 *
 * This is a minimal test harness that wires up:
 * - UVoxelWorldConfiguration (created programmatically or from asset)
 * - FDistanceBandLODStrategy (LOD management)
 * - FVoxelPMCRenderer (ProceduralMeshComponent rendering)
 * - UVoxelChunkManager (streaming coordinator)
 *
 * Place this actor in a level and enter PIE to test chunk generation.
 *
 * Debug Commands:
 * - Console: "VoxelStats" to print debug statistics
 */
UCLASS(Blueprintable, meta = (DisplayName = "Voxel World Test Actor"))
class VOXELSTREAMING_API AVoxelWorldTestActor : public AActor
{
	GENERATED_BODY()

public:
	AVoxelWorldTestActor();

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

protected:
	/** Initialize the voxel world systems */
	void InitializeVoxelWorld();

	/** Shutdown the voxel world systems */
	void ShutdownVoxelWorld();

	/** Create default configuration if none provided */
	UVoxelWorldConfiguration* CreateDefaultConfiguration();

protected:
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
