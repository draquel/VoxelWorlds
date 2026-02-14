// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelMapTypes.h"
#include "VoxelCoreTypes.h"
#include "VoxelMapSubsystem.generated.h"

class IVoxelWorldMode;
class UVoxelChunkManager;
class UVoxelBiomeConfiguration;

/**
 * World subsystem that manages 2D map tile data for voxel terrain.
 *
 * Generates tile images by sampling terrain height and material from
 * IVoxelWorldMode — works with any world mode without casting.
 *
 * Tile generation is dual-strategy:
 * 1. Event-driven: binds to UVoxelChunkManager::OnChunkGenerated to auto-generate
 *    tiles as chunks stream in.
 * 2. Predictive: RequestTilesInRadius() generates tiles ahead of chunk streaming
 *    using deterministic height queries (no loaded chunk data needed).
 *
 * All tile generation runs on background threads. The subsystem has zero
 * knowledge of players, characters, or UI — purely manages tile data.
 */
UCLASS()
class VOXELMAP_API UVoxelMapSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	// --- Tile Queries ---

	/** Get a generated tile. Returns nullptr if not yet generated. */
	const FVoxelMapTile* GetTile(FIntPoint TileCoord) const;

	/** Check if a tile has been generated (regardless of exploration state). */
	bool HasTile(FIntPoint TileCoord) const;

	/** Get the full tile cache (for bulk iteration by UI). */
	const TMap<uint64, FVoxelMapTile>& GetTileCache() const { return TileCache; }

	// --- Exploration ---

	/**
	 * Mark tiles in radius as explored and request generation for any not yet cached.
	 * Call this from external code (character plugin) to drive predictive generation.
	 *
	 * @param WorldPos Player or camera world position
	 * @param Radius World-unit radius around the position to explore
	 */
	void RequestTilesInRadius(const FVector& WorldPos, float Radius);

	/** Check if a tile has been explored. */
	bool IsTileExplored(FIntPoint TileCoord) const;

	/** Get all explored tile coords. */
	const TSet<uint64>& GetExploredTiles() const { return ExploredTiles; }

	// --- Coordinate Helpers ---

	/** Convert a world position to a tile coordinate (chunk XY). */
	FIntPoint WorldToTileCoord(const FVector& WorldPos) const;

	/** Convert a tile coordinate to world position (tile origin corner). */
	FVector TileCoordToWorld(FIntPoint TileCoord) const;

	/** Get the world size of a single tile edge (ChunkSize * VoxelSize). */
	float GetTileWorldSize() const;

	/** Get the tile resolution (pixels per edge, matches ChunkSize). */
	int32 GetTileResolution() const { return CachedChunkSize; }

	// --- Events ---

	/** Fired on game thread when a tile finishes generating. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnMapTileReady, FIntPoint);
	FOnMapTileReady OnMapTileReady;

protected:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

private:
	/** Pack an FIntPoint into a uint64 key for TMap/TSet (avoids GetTypeHash issues). */
	static uint64 PackTileKey(FIntPoint Coord);

	/** Unpack a uint64 key back to FIntPoint. */
	static FIntPoint UnpackTileKey(uint64 Key);

	/** Event-driven: called when VoxelChunkManager generates a new chunk. */
	UFUNCTION()
	void OnChunkGenerated(FIntVector ChunkCoord);

	/** Queue a tile for background generation. */
	void QueueTileGeneration(FIntPoint TileCoord);

	/** Background thread: generate pixel data for a tile. */
	void GenerateTileAsync(FIntPoint TileCoord);

	/** Resolve the chunk manager and cache configuration. Returns true if ready. */
	bool ResolveChunkManager();

	// Cached references (resolved in Initialize or lazily)
	TWeakObjectPtr<UVoxelChunkManager> CachedChunkManagerWeak;
	const IVoxelWorldMode* CachedWorldMode = nullptr;
	FVoxelNoiseParams CachedNoiseParams;
	int32 CachedChunkSize = 32;
	float CachedVoxelSize = 100.0f;
	FVector CachedWorldOrigin = FVector::ZeroVector;
	bool bCacheResolved = false;

	// Biome configuration (cached from VoxelWorldConfiguration)
	UPROPERTY()
	TObjectPtr<UVoxelBiomeConfiguration> CachedBiomeConfig;
	bool bBiomesEnabled = false;

	// Water level (cached from VoxelWorldConfiguration)
	bool bWaterEnabled = false;
	float CachedWaterLevel = 0.0f;

	// Tile storage
	TMap<uint64, FVoxelMapTile> TileCache;
	TSet<uint64> ExploredTiles;
	TSet<uint64> PendingTiles;		// Tiles queued for generation (prevents duplicates)
	FCriticalSection TileMutex;		// Protects TileCache writes from async tasks

	/** Max concurrent async tile generation tasks. */
	static constexpr int32 MaxConcurrentTileGenTasks = 4;

	/** Number of async tasks currently in flight. */
	TAtomic<int32> ActiveAsyncTasks{0};

	/** Whether the subsystem has bound to chunk manager delegates. */
	bool bDelegatesBound = false;
};
