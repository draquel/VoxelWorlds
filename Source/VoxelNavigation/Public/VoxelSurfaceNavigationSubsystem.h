// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelSurfacePathfinder.h"
#include "VoxelCubeSphere.h"
#include "VoxelSeaSurface.h"
#include "VoxelSurfaceNavigationSubsystem.generated.h"

class UVoxelChunkManager;
enum class EEditSource : uint8;

/**
 * A planet search's frame (spherical Phase 4, WORLD_MODES_REVIEW_PLAN.md §18, WM-SY11; SP-G4): the tangent plane of the
 * datum sphere at a region's centre, east and north there (FVoxelCubeSphere::GetTangentFrame). Frame point (x, y, h) is
 * the point at altitude h along the direction from the centre through the plane's point (x, y) (a gnomonic projection:
 * within 20 km of a region on a 250 km planet cells stay within 2% of their size). The surface pathfinder runs on
 * (x, y) with the altitude as its height. Pure.
 */
struct VOXELNAVIGATION_API FVoxelPlanetNavFrame
{
	/** The region (a cell of the subsystem's region grid) this frame belongs to. */
	FIntPoint Region = FIntPoint::ZeroValue;

	/** Planet centre and datum radius. */
	FVector Center = FVector::ZeroVector;
	double Radius = 1.0;

	/** Unit up, east and north at the frame's origin (the region centre on the datum sphere). */
	FVector Up = FVector::UpVector;
	FVector East = FVector::RightVector;
	FVector North = FVector::ForwardVector;

	/**
	 * The frame at a direction.
	 * @param InCenter Planet centre
	 * @param InRadius Datum radius
	 * @param Direction The frame's centre direction (normalised here)
	 * @param InRegion The region it belongs to
	 * @return The frame
	 */
	static FVoxelPlanetNavFrame Make(const FVector& InCenter, double InRadius, const FVector& Direction, const FIntPoint& InRegion = FIntPoint::ZeroValue);

	/**
	 * A world position in the frame.
	 * @param World World position
	 * @param OutFramePoint (x, y) on the tangent plane, altitude as Z
	 * @return False more than 60 degrees from the frame's centre (outside any search from it)
	 */
	bool ToFrame(const FVector& World, FVector& OutFramePoint) const;

	/** The unit direction through the tangent plane's point (x, y). */
	FVector DirectionAt(double X, double Y) const;

	/** The world position of a frame point (x, y, altitude). */
	FVector ToWorld(const FVector& FramePoint) const;
};

/**
 * World-level surface navigation over the voxel terrain (feature 10): the pathfinder plus everything it
 * needs from a live world.
 *
 * - Heights come from the chunk manager: the edit-merged surface when the chunk is loaded (dug ground,
 *   stamped dungeons, placed pads), the analytic generated surface otherwise, cached per cell in 32 × 32
 *   tiles. Cells sampled analytically are re-sampled when their chunk loads; edited chunks drop the cells
 *   the edit touched.
 * - Blocked boxes (POI structures, campsites …), temporary blocked cells (a stuck walker's repath) and every cell
 *   outside the world mode's bounds (an island's R + falloff; world modes review WM-I2), water on or off.
 * - Tunables through cvars (voxel.Nav.*), stats for logs, debug drawing (voxel.Nav.Debug).
 * - Planets (spherical Phase 4, WM-SY11): a search runs in the tangent-plane frame (FVoxelPlanetNavFrame) of the region
 *   its start lies in (regions: cells of a cube-sphere grid about four chunks across), with altitudes for heights and
 *   the sea as the altitude WaterRadius - PlanetRadius; heights come from QueryEditMergedSurfacePoint along each cell's
 *   direction (the analytic surface otherwise) and are cached per region frame (dropped after a minute unused);
 *   blocked boxes and temporary blocks are 3D there. FVoxelSurfacePathfinder itself is the plane's.
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
		FBox Box3D; // the planet's test (the cell's surface point inside the box)
	};

	/** A planet's temporary block: every cell whose surface point lies within Radius (plus half a cell) of Center. */
	struct FTempSphere
	{
		FVector Center = FVector::ZeroVector;
		float Radius = 0.f;
		double ExpireTime = 0.0;
	};

	/** A planet region's frame and its height cache. */
	struct FRegionCache
	{
		FVoxelPlanetNavFrame Frame;
		TMap<FIntPoint, TUniquePtr<FNavTile>> Tiles;
		int32 Cells = 0;
		double LastUsedSeconds = 0.0;
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

	/** Clear cached cells of a tile map in a cell rectangle. @return How many were cleared */
	static int32 InvalidateTileCells(TMap<FIntPoint, TUniquePtr<FNavTile>>& InTiles, const FIntPoint& MinCell, const FIntPoint& MaxCell, bool bOnlyAnalytic);

	// --- Planets (WM-SY11) ---

	/** The region frame (and its cache) a world position belongs to; made on first use. */
	FRegionCache& RegionFor(const FVector& World);

	/** A planet cell's surface altitude in a region's frame (cached). */
	float HeightForPlanetCell(FRegionCache& Region, const FIntPoint& Cell);

	/** Temporary blocks and blocked boxes against a planet cell's surface point. */
	bool IsPlanetCellBlocked(FRegionCache& Region, const FIntPoint& Cell);

	/** Clear the cached planet cells whose frame points a world box's corners span, in every region. */
	void InvalidatePlanetBox(const FBox& WorldBox, bool bOnlyAnalytic);
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

	// --- Planets (WM-SY11) ---
	bool bPlanet = false;
	FVector PlanetCenter = FVector::ZeroVector;
	double PlanetRadius = 0.0;
	FVoxelSeaSurface PlanetSea;
	FVoxelCubeSphereGrid RegionGrid;
	TMap<FIntPoint, TUniquePtr<FRegionCache>> Regions;
	TArray<FTempSphere> TempSpheres;

	FVoxelSurfacePathStats LastStats;
	int32 SearchCount = 0;

	FVector DebugFocus = FVector::ZeroVector;
	float DebugFocusRadius = 0.f;
	TArray<FDebugPath> DebugPaths;
};
