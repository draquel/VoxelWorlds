// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Components/StaticMeshComponent.h"
#include "VoxelWorldTestActor.generated.h"

class UVoxelMaterialAtlas;

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

	/** Show performance stats HUD on screen */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Debug")
	bool bShowPerformanceHUD = false;

	// ==================== Water Visualization ====================

	/**
	 * Material for the water plane visualization.
	 * Create a translucent water material and assign it here.
	 * If null, a default semi-transparent blue material will be used.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Water")
	TObjectPtr<UMaterialInterface> WaterMaterial;

	/**
	 * Scale multiplier for the water plane size.
	 * The plane will be scaled based on ViewDistance * WaterPlaneScale.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Water", meta = (ClampMin = "1.0", ClampMax = "100.0"))
	float WaterPlaneScale = 10.0f;

	// ==================== Transvoxel Debug ====================

	/** Enable detailed logging for Transvoxel transition cells */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Debug|Transvoxel")
	bool bDebugLogTransitionCells = false;

	/** Enable visualization of Transvoxel transition cells */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Debug|Transvoxel")
	bool bDrawTransitionCellDebug = false;

	/** Show sample points in transition cells (red=outside, green=inside) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Debug|Transvoxel", meta = (EditCondition = "bDrawTransitionCellDebug"))
	bool bShowTransitionSamplePoints = true;

	/** Show generated vertices in transition cells */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Debug|Transvoxel", meta = (EditCondition = "bDrawTransitionCellDebug"))
	bool bShowTransitionVertices = true;

	/** Show transition cell bounding boxes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Debug|Transvoxel", meta = (EditCondition = "bDrawTransitionCellDebug"))
	bool bShowTransitionCellBounds = true;

	/** Size of debug points in world units */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Debug|Transvoxel", meta = (ClampMin = "1", ClampMax = "50"))
	float DebugPointSize = 10.0f;

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
	 * Get the recommended spawn position for spherical planet mode.
	 * Returns the position on the planet surface based on PlanetSpawnLocation setting.
	 * Only valid when WorldMode == SphericalPlanet.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	FVector GetPlanetSpawnPosition() const;

	/** Draw transition cell debug visualization */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Debug|Transvoxel")
	void DrawTransitionCellDebug();

	/** Toggle transition cell debugging on/off */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Debug|Transvoxel")
	void SetTransitionCellDebugging(bool bEnable);

	// ==================== Edit System Testing ====================

	/**
	 * Enable mouse-based terrain editing.
	 * Left click = Dig, Right click = Build, Mouse wheel = Adjust radius.
	 * Requires being in PIE with a player controller.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Edit")
	bool bEnableEditInputs = false;

	/**
	 * Current brush radius for mouse-based editing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Edit", meta = (ClampMin = "50", ClampMax = "2000", EditCondition = "bEnableEditInputs"))
	float EditBrushRadius = 300.0f;

	/**
	 * Material ID for building (right-click).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Edit", meta = (ClampMin = "1", ClampMax = "255", EditCondition = "bEnableEditInputs"))
	uint8 EditMaterialID = 1;

	/**
	 * Show crosshair when edit inputs are enabled.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Edit", meta = (EditCondition = "bEnableEditInputs"))
	bool bShowEditCrosshair = true;

	/**
	 * Use discrete voxel editing (for cubic mode).
	 * When enabled, edits snap to voxel grid and affect single blocks.
	 * Building places blocks adjacent to the hit face.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Edit", meta = (EditCondition = "bEnableEditInputs"))
	bool bUseDiscreteEditing = false;

	/**
	 * Apply a test brush edit at the specified world location.
	 * Uses Subtract mode to dig a hole in the terrain.
	 *
	 * @param WorldLocation Center of the edit in world space
	 * @param Radius Brush radius in world units
	 * @return Number of voxels modified
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	int32 TestDigAt(FVector WorldLocation, float Radius = 300.0f);

	/**
	 * Apply a test brush edit at the specified world location.
	 * Uses Add mode to build terrain.
	 *
	 * @param WorldLocation Center of the edit in world space
	 * @param Radius Brush radius in world units
	 * @param MaterialID Material to use for new terrain
	 * @return Number of voxels modified
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	int32 TestBuildAt(FVector WorldLocation, float Radius = 300.0f, uint8 MaterialID = 1);

	/**
	 * Apply a test brush edit at the specified world location.
	 * Uses Paint mode to change material without modifying terrain shape.
	 *
	 * @param WorldLocation Center of the edit in world space
	 * @param Radius Brush radius in world units
	 * @param MaterialID Material to paint
	 * @return Number of voxels modified
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	int32 TestPaintAt(FVector WorldLocation, float Radius = 300.0f, uint8 MaterialID = 1);

	// ==================== Discrete Voxel Editing (Cubic Mode) ====================

	/**
	 * Remove a single voxel at the specified world location.
	 * Uses hit normal to ensure we target the solid voxel, not air.
	 *
	 * @param WorldLocation Hit location on terrain surface
	 * @param HitNormal Normal of the face that was hit
	 * @return True if a block was removed
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	bool TestRemoveBlock(FVector WorldLocation, FVector HitNormal);

	/**
	 * Place a single voxel adjacent to the hit location.
	 * Uses the hit normal to determine which face was hit and places block on that side.
	 *
	 * @param WorldLocation Hit location on existing terrain
	 * @param HitNormal Normal of the face that was hit
	 * @param MaterialID Material for the new block
	 * @return True if a block was placed
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	bool TestPlaceBlock(FVector WorldLocation, FVector HitNormal, uint8 MaterialID = 1);

	/**
	 * Paint a single voxel at the specified world location.
	 * Uses hit normal to ensure we target the solid voxel, not air.
	 *
	 * @param WorldLocation Hit location on terrain surface
	 * @param HitNormal Normal of the face that was hit
	 * @param MaterialID Material to apply
	 * @return True if a block was painted
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	bool TestPaintBlock(FVector WorldLocation, FVector HitNormal, uint8 MaterialID = 1);

	/**
	 * Undo the last edit operation.
	 * @return True if undo was performed
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	bool TestUndo();

	/**
	 * Redo the last undone operation.
	 * @return True if redo was performed
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	bool TestRedo();

	/**
	 * Save all edits to a file.
	 * @param FileName Name of the save file (saved to project's Saved folder)
	 * @return True if save was successful
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	bool TestSaveEdits(const FString& FileName = TEXT("VoxelEdits.dat"));

	/**
	 * Load edits from a file.
	 * @param FileName Name of the save file to load
	 * @return True if load was successful
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	bool TestLoadEdits(const FString& FileName = TEXT("VoxelEdits.dat"));

	/**
	 * Print edit system statistics to the log.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Edit")
	void PrintEditStats();

	/**
	 * Print collision system statistics to the log.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Collision")
	void PrintCollisionStats();

protected:
	/** Initialize the voxel world systems */
	void InitializeVoxelWorld();

	/** Shutdown the voxel world systems */
	void ShutdownVoxelWorld();

	/** Create default configuration if none provided */
	UVoxelWorldConfiguration* CreateDefaultConfiguration();

	/** Create or update the water plane visualization */
	void UpdateWaterVisualization();

	/** Destroy the water plane visualization */
	void DestroyWaterVisualization();

	/** Process edit inputs (called from Tick when bEnableEditInputs is true) */
	void ProcessEditInputs();

	/**
	 * Perform a line trace from the camera to find terrain hit location.
	 * @param OutHitLocation Output hit location in world space
	 * @return True if terrain was hit
	 */
	bool TraceTerrainFromCamera(FVector& OutHitLocation) const;

	/**
	 * Perform a line trace from the camera to find terrain hit location and normal.
	 * @param OutHitLocation Output hit location in world space
	 * @param OutHitNormal Output hit normal
	 * @return True if terrain was hit
	 */
	bool TraceTerrainFromCamera(FVector& OutHitLocation, FVector& OutHitNormal) const;

	/**
	 * Snap a world position to the center of the containing voxel.
	 * @param WorldPos World position to snap
	 * @return Snapped position at voxel center
	 */
	FVector SnapToVoxelCenter(const FVector& WorldPos) const;

	/**
	 * Get the voxel position adjacent to a hit point based on hit normal (for placing blocks).
	 * @param HitLocation Location on terrain surface
	 * @param HitNormal Normal of the hit face
	 * @return World position of the adjacent (air) voxel center
	 */
	FVector GetAdjacentVoxelPosition(const FVector& HitLocation, const FVector& HitNormal) const;

	/**
	 * Get the solid voxel position from a hit point based on hit normal (for removing/painting blocks).
	 * @param HitLocation Location on terrain surface
	 * @param HitNormal Normal of the hit face
	 * @return World position of the solid voxel center that was hit
	 */
	FVector GetSolidVoxelPosition(const FVector& HitLocation, const FVector& HitNormal) const;

	/**
	 * Get the world-space bounding box for the voxel at a given position.
	 * @param VoxelCenter Center of the voxel
	 * @return Bounding box for the voxel
	 */
	FBox GetVoxelBounds(const FVector& VoxelCenter) const;

	/** Draw edit crosshair on screen */
	void DrawEditCrosshair() const;

	/** Draw performance HUD on screen */
	void DrawPerformanceHUD() const;

protected:
	/** Chunk manager component */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel")
	TObjectPtr<UVoxelChunkManager> ChunkManager;

	/** Water plane static mesh component (for flat world modes) */
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> WaterPlaneMesh;

	/** Water sphere static mesh component (for spherical planet mode) */
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> WaterSphereMesh;

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

	// ==================== Edit Input State ====================

	/** Previous frame left mouse button state (for press detection) */
	bool bWasLeftMouseDown = false;

	/** Previous frame right mouse button state (for press detection) */
	bool bWasRightMouseDown = false;

	/** Previous frame middle mouse button state (for press detection) */
	bool bWasMiddleMouseDown = false;

	/** Previous frame mouse wheel value (for delta detection) */
	float LastMouseWheelDelta = 0.0f;
};
