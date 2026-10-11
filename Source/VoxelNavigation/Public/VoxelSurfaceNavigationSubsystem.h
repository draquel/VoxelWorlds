// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelSurfacePathfinder.h"
#include "VoxelSurfaceNavigationSubsystem.generated.h"

class UVoxelChunkManager;
enum class EEditSource : uint8;

/**
 * World-level surface navigation over the voxel terrain (feature 10): the pathfinder plus everything it
 * needs from a live world.
 *
 * - Heights come from the chunk manager: the edit-merged surface when the chunk is loaded (dug ground,
 *   stamped dungeons, placed pads), the analytic generated surface otherwise, cached per cell in 32 × 32
 *   tiles. Cells sampled analytically are re-sampled when their chunk loads; edited chunks drop the cells
 *   the edit touched.
 * - Blocked boxes (POI structures, campsites …) and temporary blocked cells (a stuck walker's repath).
 * - Tunables through cvars (voxel.Nav.*), stats for logs, debug drawing (voxel.Nav.Debug).
 *
 * Game / PIE worlds only. Game thread only. Searches are synchronous and budgeted (MaxNodes), so callers
 * should spread requests over frames (an NPC repaths a few times a second at most).
 */
UCLASS()
class VOXELNAVIGATION_API UVoxelSurfaceNavigationSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelSurfaceNavigationSubsystem, STATGROUP_Tickables); }

	UFUNCTION(BlueprintPure, Category = "Voxel|Navigation", meta = (WorldContext = "WorldContextObject"))
	static UVoxelSurfaceNavigationSubsystem* Get(const UObject* WorldContextObject);

	/** The chunk manager to sample from; found by scanning for the component when not set. */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Navigation")
	void SetChunkManager(UVoxelChunkManager* InChunkManager);

	/** True once a chunk manager is known and initialized (searches fail before that). */
	UFUNCTION(BlueprintPure, Category = "Voxel|Navigation")
	bool IsReady() const;

	/**
	 * Find a walkable route over the surface.
	 * @param From      Start (world).
	 * @param To        Goal (world).
	 * @param OutPoints Waypoints on the surface, ending at the goal (or the closest reached cell when partial).
	 * @return True when a route was produced.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Navigation")
	bool FindPath(const FVector& From, const FVector& To, TArray<FVector>& OutPoints);

	/** FindPath with the search statistics. */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Navigation")
	bool FindPathWithStats(const FVector& From, const FVector& To, TArray<FVector>& OutPoints, FVoxelSurfacePathStats& OutStats);

	/** Surface Z under / over a point (edit-merged when loaded, analytic otherwise). @return False without a chunk manager. */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Navigation")
	bool ProjectToSurface(const FVector& Location, FVector& OutOnSurface);

	/** Is the cell at a location walkable (above water minus wading, not blocked)? */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Navigation")
	bool IsWalkableAt(const FVector& Location);

	/** Register an impassable box (world, axis aligned in XY). @return Handle for RemoveBlockedBox. */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Navigation")
	int32 AddBlockedBox(const FBox& WorldBox);

	UFUNCTION(BlueprintCallable, Category = "Voxel|Navigation")
	void RemoveBlockedBox(int32 Handle);

	/** Block every cell within Radius of a point for Seconds (a stuck walker marks where it failed). */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Navigation")
	void BlockCellsTemporarily(const FVector& Location, float Radius, float Seconds);

	/** Forget every cached height. */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Navigation")
	void InvalidateAll();

	/** Drop cached heights inside a world box (an actor was placed / removed, terrain changed). */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Navigation")
	void InvalidateBox(const FBox& WorldBox);

	/** Current tunables (cvars applied). */
	UFUNCTION(BlueprintPure, Category = "Voxel|Navigation")
	FVoxelSurfaceNavParams GetParams() const;

	/** Statistics of the last search. */
	UFUNCTION(BlueprintPure, Category = "Voxel|Navigation")
	const FVoxelSurfacePathStats& GetLastStats() const { return LastStats; }

	UFUNCTION(BlueprintPure, Category = "Voxel|Navigation")
	int32 GetCachedCellCount() const { return CachedCells; }

	UFUNCTION(BlueprintPure, Category = "Voxel|Navigation")
	int32 GetSearchCount() const { return SearchCount; }

	/** Keep drawing the walkability grid around a point while voxel.Nav.Debug is on (0 radius = off). */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Navigation")
	void SetDebugFocus(const FVector& Location, float Radius);

	/** Draw a route for a few seconds (voxel.Nav.Debug independent). */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Navigation")
	void DrawPath(const TArray<FVector>& Points, float Seconds, FLinearColor Color);

private:
	static constexpr int32 TileSize = 32;

	/** 32 × 32 cells of cached surface heights. Cells are sampled lazily. */
	struct FNavTile
	{
		TArray<float> Heights;
		TBitArray<> Sampled;
		TBitArray<> FromLoaded; // sampled from a loaded chunk (edit-merged) rather than the analytic surface
		FNavTile();
	};

	struct FBlockedBox
	{
		int32 Handle = 0;
		FBox2D Box;
	};

	struct FTempBlock
	{
		FIntPoint Cell;
		double ExpireTime = 0.0;
	};

	struct FDebugPath
	{
		TArray<FVector> Points;
		double ExpireTime = 0.0;
		FLinearColor Color;
	};

	UVoxelChunkManager* ResolveChunkManager();
	void BindChunkManager(UVoxelChunkManager* ChunkMgr);
	float SampleHeight(double WorldX, double WorldY);
	float HeightForCell(const FIntPoint& Cell);
	bool IsCellBlocked(const FIntPoint& Cell) const;
	FIntPoint CellOf(const FVector& World) const;
	FVector2D CellCentre(const FIntPoint& Cell) const;
	void InvalidateCells(const FIntPoint& MinCell, const FIntPoint& MaxCell, bool bOnlyAnalytic);
	void ApplyCVars();
	void DrawDebug();

	UFUNCTION()
	void HandleChunkLoaded(FIntVector ChunkCoord);
	void HandleChunkEdited(const FIntVector& ChunkCoord, EEditSource Source, const FVector& EditCenter, float EditRadius);

	TWeakObjectPtr<UVoxelChunkManager> ChunkManager;
	bool bBound = false;
	bool bEditBound = false;
	FDelegateHandle EditHandle;

	FVoxelSurfaceNavParams Params;
	float CachedCellSize = 0.f;
	TMap<FIntPoint, TUniquePtr<FNavTile>> Tiles;
	int32 CachedCells = 0;

	TArray<FBlockedBox> BlockedBoxes;
	int32 NextBlockHandle = 1;
	TArray<FTempBlock> TempBlocks;
	TSet<FIntPoint> TempBlockedCells;

	FVoxelSurfacePathStats LastStats;
	int32 SearchCount = 0;

	FVector DebugFocus = FVector::ZeroVector;
	float DebugFocusRadius = 0.f;
	TArray<FDebugPath> DebugPaths;
};
