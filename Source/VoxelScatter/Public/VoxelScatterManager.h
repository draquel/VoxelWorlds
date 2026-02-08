// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelScatterTypes.h"
#include "VoxelScatterManager.generated.h"

class UVoxelWorldConfiguration;
class UVoxelScatterRenderer;
struct FChunkMeshData;

/**
 * Delegate fired when scatter data is ready for a chunk.
 */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnChunkScatterReady, const FIntVector& /*ChunkCoord*/, int32 /*SpawnPointCount*/);

/**
 * Delegate fired when scatter data is removed for a chunk.
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnChunkScatterRemoved, const FIntVector& /*ChunkCoord*/);

/**
 * Voxel Scatter Manager.
 *
 * Manages scatter generation for voxel terrain. Coordinates surface point
 * extraction from mesh data and scatter placement based on definitions.
 *
 * Key responsibilities:
 * - Extract surface points when chunk meshes are ready
 * - Generate scatter spawn points based on placement rules
 * - Cache scatter data per chunk
 * - Provide debug visualization
 *
 * Integration:
 * - Created and owned by UVoxelChunkManager
 * - Listens for chunk mesh ready events
 * - Cleans up when chunks unload
 *
 * Thread Safety: Must be accessed from game thread only.
 *
 * @see UVoxelChunkManager
 * @see FVoxelSurfaceExtractor
 * @see FVoxelScatterPlacement
 */
UCLASS(BlueprintType)
class VOXELSCATTER_API UVoxelScatterManager : public UObject
{
	GENERATED_BODY()

public:
	UVoxelScatterManager();

	// ==================== Lifecycle ====================

	/**
	 * Initialize the scatter manager.
	 *
	 * @param Config World configuration
	 * @param World World reference for debug drawing
	 */
	void Initialize(UVoxelWorldConfiguration* Config, UWorld* World);

	/**
	 * Shutdown and cleanup all scatter resources.
	 */
	void Shutdown();

	/**
	 * Check if manager is initialized.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	bool IsInitialized() const { return bIsInitialized; }

	// ==================== Per-Frame Update ====================

	/**
	 * Update scatter state based on viewer position.
	 *
	 * Currently handles debug visualization updates.
	 *
	 * @param ViewerPosition Current viewer/camera position
	 * @param DeltaTime Time since last update
	 */
	void Update(const FVector& ViewerPosition, float DeltaTime);

	// ==================== Scatter Definitions ====================

	/**
	 * Add a scatter type definition.
	 *
	 * @param Definition The scatter definition to add
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	void AddScatterDefinition(const FScatterDefinition& Definition);

	/**
	 * Remove a scatter type by ID.
	 *
	 * @param ScatterID ID of the scatter type to remove
	 * @return True if found and removed
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	bool RemoveScatterDefinition(int32 ScatterID);

	/**
	 * Clear all scatter definitions.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	void ClearScatterDefinitions();

	/**
	 * Get all scatter definitions.
	 */
	const TArray<FScatterDefinition>& GetScatterDefinitions() const { return ScatterDefinitions; }

	/**
	 * Get a scatter definition by ID.
	 *
	 * @param ScatterID ID to look up
	 * @return Pointer to definition or nullptr if not found
	 */
	const FScatterDefinition* GetScatterDefinition(int32 ScatterID) const;

	// ==================== Scatter Data Access ====================

	/**
	 * Get scatter data for a chunk.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return Pointer to scatter data or nullptr if not available
	 */
	const FChunkScatterData* GetChunkScatterData(const FIntVector& ChunkCoord) const;

	/**
	 * Get surface data for a chunk.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return Pointer to surface data or nullptr if not available
	 */
	const FChunkSurfaceData* GetChunkSurfaceData(const FIntVector& ChunkCoord) const;

	/**
	 * Check if chunk has scatter data.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	bool HasScatterData(const FIntVector& ChunkCoord) const;

	/**
	 * Get number of chunks with scatter data.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	int32 GetScatterChunkCount() const { return ScatterDataCache.Num(); }

	/**
	 * Get number of chunks pending scatter generation.
	 * Used to detect if world is still loading.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	int32 GetPendingGenerationCount() const { return PendingGenerationQueue.Num(); }

	// ==================== Mesh Data Callback ====================

	/**
	 * Called when chunk mesh data is ready.
	 * Triggers surface extraction and scatter generation.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param MeshData The mesh data
	 */
	void OnChunkMeshDataReady(const FIntVector& ChunkCoord, const FChunkMeshData& MeshData);

	/**
	 * Called when a chunk is unloaded.
	 * Cleans up associated scatter data.
	 *
	 * @param ChunkCoord Chunk coordinate
	 */
	void OnChunkUnloaded(const FIntVector& ChunkCoord);

	/**
	 * Regenerate scatter for a chunk (e.g., after system edit).
	 * Clears existing scatter and allows regeneration when new mesh arrives.
	 *
	 * @param ChunkCoord Chunk coordinate
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	void RegenerateChunkScatter(const FIntVector& ChunkCoord);

	/**
	 * Clear scatter in a specific area (e.g., after player edit).
	 * Only removes instances within the edit radius, not the entire chunk.
	 * Prevents regeneration in the cleared area until chunk is fully unloaded/reloaded.
	 *
	 * @param WorldPosition Center of the edit in world space
	 * @param Radius Radius of the edit brush
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	void ClearScatterInRadius(const FVector& WorldPosition, float Radius);

	/**
	 * Check if a point falls within any cleared volume for a chunk.
	 * Used during scatter generation to skip points in player-edited areas.
	 */
	bool IsPointInClearedVolume(const FIntVector& ChunkCoord, const FVector& WorldPosition) const;

	// ==================== Configuration ====================

	/**
	 * Set the maximum distance for scatter generation.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	void SetScatterRadius(float Radius);

	/**
	 * Get the current scatter radius.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	float GetScatterRadius() const { return ScatterRadius; }

	/**
	 * Set the target spacing between surface sample points.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	void SetSurfacePointSpacing(float Spacing);

	/**
	 * Get the current surface point spacing.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	float GetSurfacePointSpacing() const { return SurfacePointSpacing; }

	/**
	 * Set the world seed for deterministic scatter.
	 */
	void SetWorldSeed(uint32 Seed);

	// ==================== Debug ====================

	/**
	 * Enable or disable debug visualization.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter|Debug")
	void SetDebugVisualizationEnabled(bool bEnabled);

	/**
	 * Check if debug visualization is enabled.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter|Debug")
	bool IsDebugVisualizationEnabled() const { return bDebugVisualization; }

	/**
	 * Draw debug visualization.
	 * Call from tick or debug draw function.
	 *
	 * @param World World to draw in
	 */
	void DrawDebugVisualization(UWorld* World) const;

	/**
	 * Get approximate total memory usage of scatter system in bytes.
	 */
	int64 GetTotalMemoryUsage() const;

	/**
	 * Get statistics for debugging.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter|Debug")
	FScatterStatistics GetStatistics() const;

	/**
	 * Get debug stats as string.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter|Debug")
	FString GetDebugStats() const;

	// ==================== Events ====================

	/** Fired when scatter data is ready for a chunk */
	FOnChunkScatterReady OnChunkScatterReady;

	/** Fired when scatter data is removed for a chunk */
	FOnChunkScatterRemoved OnChunkScatterRemoved;

protected:
	// ==================== Internal Methods ====================

	/**
	 * Generate scatter for a chunk from mesh data.
	 */
	void GenerateChunkScatter(const FIntVector& ChunkCoord, const FChunkMeshData& MeshData);

	/**
	 * Remove scatter data for a chunk.
	 */
	void RemoveChunkScatter(const FIntVector& ChunkCoord);

	/**
	 * Get chunk world origin for coordinate conversion.
	 */
	FVector GetChunkWorldOrigin(const FIntVector& ChunkCoord) const;

	/**
	 * Create default scatter definitions.
	 */
	void CreateDefaultDefinitions();

protected:
	// ==================== Sub-Managers ====================

	/** Renderer for HISM instances */
	UPROPERTY()
	TObjectPtr<UVoxelScatterRenderer> ScatterRenderer;

	// ==================== Configuration ====================

	/** World configuration reference */
	UPROPERTY()
	TObjectPtr<UVoxelWorldConfiguration> Configuration;

	/** Cached world reference */
	UPROPERTY()
	TObjectPtr<UWorld> CachedWorld;

	/** Whether the manager is initialized */
	bool bIsInitialized = false;

	// ==================== Scatter Settings ====================

	/** Maximum distance for scatter generation */
	float ScatterRadius = 10000.0f;

	/** Target spacing between surface sample points (cm) */
	float SurfacePointSpacing = 100.0f;

	/** World seed for deterministic scatter */
	uint32 WorldSeed = 12345;

	// ==================== Scatter Definitions ====================

	/** All scatter type definitions */
	UPROPERTY()
	TArray<FScatterDefinition> ScatterDefinitions;

	// ==================== Data Caches ====================

	/** Per-chunk surface data cache */
	TMap<FIntVector, FChunkSurfaceData> SurfaceDataCache;

	/** Per-chunk scatter data cache */
	TMap<FIntVector, FChunkScatterData> ScatterDataCache;

	/**
	 * Cleared scatter volume - represents an area where scatter should not regenerate.
	 * Used for player edits to surgically remove scatter in the affected area only.
	 */
	struct FClearedScatterVolume
	{
		FVector Center;
		float Radius;

		FClearedScatterVolume() : Center(FVector::ZeroVector), Radius(0.0f) {}
		FClearedScatterVolume(const FVector& InCenter, float InRadius) : Center(InCenter), Radius(InRadius) {}

		bool ContainsPoint(const FVector& Point) const
		{
			return FVector::DistSquared(Center, Point) <= (Radius * Radius);
		}
	};

	/** Per-chunk list of cleared volumes from player edits */
	TMap<FIntVector, TArray<FClearedScatterVolume>> ClearedVolumesPerChunk;

	// ==================== Debug ====================

	/** Whether debug visualization is enabled */
	bool bDebugVisualization = false;

	/** Last viewer position (for debug drawing) */
	FVector LastViewerPosition = FVector::ZeroVector;

	// ==================== Deferred Generation Queue ====================

	/**
	 * Pending scatter generation request.
	 * Stores lightweight copy of mesh data needed for surface extraction.
	 */
	struct FPendingScatterGeneration
	{
		FIntVector ChunkCoord;
		float DistanceToViewer;

		// Lightweight mesh data copy (only what's needed for surface extraction)
		TArray<FVector3f> Positions;
		TArray<FVector3f> Normals;
		TArray<FVector2f> UV1s;
		TArray<FColor> Colors;

		bool operator<(const FPendingScatterGeneration& Other) const
		{
			return DistanceToViewer > Other.DistanceToViewer; // Farthest first, closest at back for O(1) pop
		}
	};

	/** Queue of pending scatter generations, sorted by distance */
	TArray<FPendingScatterGeneration> PendingGenerationQueue;

	/** Set of chunks in the pending queue (O(1) duplicate check) */
	TSet<FIntVector> PendingQueueSet;

	/** Maximum scatter generations per frame (0 = unlimited) */
	int32 MaxScatterGenerationsPerFrame = 2;

	/** Process pending scatter generation queue */
	void ProcessPendingGenerationQueue();

	/** Generate scatter from cached pending data */
	void GenerateChunkScatterFromPending(const FPendingScatterGeneration& PendingData);

	// ==================== Statistics ====================

	/** Total chunks processed */
	int64 TotalChunksProcessed = 0;

	/** Total surface points extracted */
	int64 TotalSurfacePointsExtracted = 0;

	/** Total spawn points generated */
	int64 TotalSpawnPointsGenerated = 0;
};
