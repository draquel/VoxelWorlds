// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelSeaSurface.h"
#include "VoxelSurfacePathfinder.generated.h"

/** Tunables of the surface pathfinder (feature 10). Distances in world units. */
USTRUCT(BlueprintType)
struct VOXELNAVIGATION_API FVoxelSurfaceNavParams
{
	GENERATED_BODY()

	/** Grid cell size. 150 = two voxels of the demo world: fine enough for a capsule, coarse enough to search far. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation", meta = (ClampMin = "25"))
	float CellSize = 150.f;

	/**
	 * Largest height difference between two neighbouring cells a walker can take. 100 at a 150 cell = ~34°:
	 * voxel terrain is locally steeper than the cell average, so routes keep a wide margin under a walker's
	 * walkable floor angle (a capsule that loses its footing slides downhill).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation", meta = (ClampMin = "0"))
	float MaxStepRise = 100.f;

	/**
	 * World (X, Y) where the cell grid starts: cell (0, 0) covers [GridOrigin, GridOrigin + CellSize). Zero by default; a
	 * block world aligns it with the voxel grid (WorldOrigin) so each cell is one block column (world modes review WM-C7).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation")
	FVector2D GridOrigin = FVector2D::ZeroVector;

	/**
	 * A block world's step rule (cubic, world modes review WM-C7, decision D29): every rise is whole blocks and a walker
	 * steps one block, so MaxStepRise (one block) is not scaled on diagonals, and a diagonal step also needs both cells
	 * beside it walkable and within a step of either end, so a route never cuts a corner past a pillar.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation")
	bool bBlockSteps = false;

	/** World Z of the water surface, used when bHasWater. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation")
	float WaterLevel = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation")
	bool bHasWater = false;

	/** A cell this far below the water surface is still walkable (wading). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation", meta = (ClampMin = "0"))
	float MaxWadeDepth = 60.f;

	/** Extra cost per unit of rise / run between cells (1.5 = a 45° step costs 2.5× flat). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation", meta = (ClampMin = "0"))
	float SlopeCostScale = 1.5f;

	/** Node budget per search; a search that runs out returns the partial route when allowed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation", meta = (ClampMin = "16"))
	int32 MaxNodes = 4096;

	/** Cells farther than this from the start are never expanded. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation", meta = (ClampMin = "0"))
	float MaxSearchRadius = 20000.f;

	/** When the goal cell is not walkable, accept the nearest walkable cell within this many cells (0 = fail). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation", meta = (ClampMin = "0"))
	int32 GoalSnapCells = 2;

	/** Return the route to the closest expanded cell when the budget / radius ran out before the goal. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation")
	bool bAllowPartial = true;

	/**
	 * A search that has expanded this many nodes without reaching the goal checks, from the goal's side, whether the goal
	 * lies in a pocket of at most PocketCells cells it can step out of nowhere (a pillar top, a walled hollow, cells walkers
	 * marked blocked about it); if so it stops there with the partial route, where it went on to exhaust MaxNodes (G73).
	 * 0 = never check.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation", meta = (ClampMin = "0"))
	int32 PocketCheckNodes = 1024;

	/** The largest pocket the check above recognises (cells). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation", meta = (ClampMin = "1"))
	int32 PocketCells = 512;

	/**
	 * The world's sea (spherical SP-E1), set by UVoxelSurfaceNavigationSubsystem from the chunk manager
	 * (UVoxelChunkManager::GetSeaSurface, the WaterLevel plane generation floods, G19): when enabled it decides
	 * IsUnderwater instead of bHasWater / WaterLevel, so a planet's cells test the sphere of WaterRadius. Not exposed to
	 * Blueprint.
	 */
	FVoxelSeaSurface Sea;

	/**
	 * The world (X, Y) of a cell's centre.
	 *
	 * @param Cell Grid cell
	 * @return GridOrigin + (Cell + 0.5) * CellSize
	 */
	FVector2D CellCentre(const FIntPoint& Cell) const
	{
		return GridOrigin + FVector2D((Cell.X + 0.5) * CellSize, (Cell.Y + 0.5) * CellSize);
	}

	/**
	 * The cell containing a world (X, Y).
	 *
	 * @param X World X
	 * @param Y World Y
	 * @return Grid cell
	 */
	FIntPoint CellOf(double X, double Y) const
	{
		const double Size = FMath::Max(CellSize, 1.f);
		return FIntPoint(FMath::FloorToInt((X - GridOrigin.X) / Size), FMath::FloorToInt((Y - GridOrigin.Y) / Size));
	}

	/**
	 * Whether a cell's surface is deeper under the water than a walker wades (MaxWadeDepth): by Sea when it is enabled,
	 * else by bHasWater and WaterLevel. On a plane both are Height < WaterLevel - MaxWadeDepth.
	 *
	 * @param X World X of the cell's centre
	 * @param Y World Y of the cell's centre
	 * @param Height The cell's surface Z
	 * @return True when too deep to walk
	 */
	bool IsUnderwater(double X, double Y, float Height) const
	{
		if (Sea.bEnabled)
		{
			return Sea.bRadial
				? Sea.GetDepth(FVector(X, Y, Height)) > MaxWadeDepth
				: Height < static_cast<float>(Sea.PlaneZ) - MaxWadeDepth;
		}
		return bHasWater && Height < WaterLevel - MaxWadeDepth;
	}
};

/** What a search did (logs, debug, tests). */
USTRUCT(BlueprintType)
struct VOXELNAVIGATION_API FVoxelSurfacePathStats
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Navigation") int32 NodesExpanded = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Navigation") int32 HeightSamples = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Navigation") int32 RawCells = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Navigation") bool bPartial = false;
	UPROPERTY(BlueprintReadOnly, Category = "Navigation") bool bGoalSnapped = false;
	/** The goal lies in a pocket the start is outside of (FVoxelSurfaceNavParams::PocketCheckNodes): the route is partial. */
	UPROPERTY(BlueprintReadOnly, Category = "Navigation") bool bGoalInPocket = false;
	/** Cells the sampler had no height for (the subsystem's per-search sampling cap) were left out of the search. */
	UPROPERTY(BlueprintReadOnly, Category = "Navigation") bool bSamplesCapped = false;
};

/**
 * Pure A* over a 2.5-D grid of the voxel surface (feature 10). Knows nothing about worlds: a height sampler
 * answers "surface Z at (X, Y)" and an optional blocked query vetoes cells. A step between neighbouring
 * cells is walkable when both cells are above water (minus wading) and unblocked and the rise is at most
 * MaxStepRise (× √2 on diagonals); its cost grows with the slope. Routes are string-pulled so the walker
 * gets a few waypoints, not one per cell.
 *
 * Each cell is sampled and judged walkable once per search and kept with the search's bookkeeping in FCellGrid; ties
 * in the open list go to the node nearer the goal; a search that runs long checks whether the goal is in a small pocket
 * it cannot reach (PocketCheckNodes) and stops there (G73).
 *
 * Surface only: caves, overhangs and bridges do not exist for it (the sampler returns one height per column).
 */
class VOXELNAVIGATION_API FVoxelSurfacePathfinder
{
public:
	/**
	 * Surface Z at a world (X, Y). Called at most once per cell per search. NaN = no height for the cell in this search (the
	 * subsystem's per-search sampling cap): the cell is not walkable for it.
	 */
	using FHeightSampler = TFunction<float(double X, double Y)>;
	/** True when a cell may not be entered (structures, temporary blocks). Optional. */
	using FBlockedQuery = TFunction<bool(const FIntPoint& Cell)>;

	FVoxelSurfacePathfinder(const FVoxelSurfaceNavParams& InParams, FHeightSampler InSampler, FBlockedQuery InBlocked = nullptr);

	const FVoxelSurfaceNavParams& GetParams() const { return Params; }

	/** Cell containing a world position. */
	FIntPoint WorldToCell(const FVector& World) const;

	/** World centre of a cell (Z = sampled surface). */
	FVector CellToWorld(const FIntPoint& Cell) const;

	/** Surface Z at a cell centre (samples through the cache). */
	float HeightAt(const FIntPoint& Cell) const;

	/** Above water (minus wading) and not blocked. */
	bool IsCellWalkable(const FIntPoint& Cell) const;

	/** Both cells walkable and the rise between them within MaxStepRise (scaled for diagonals). */
	bool CanStep(const FIntPoint& From, const FIntPoint& To) const;

	/**
	 * Find a route.
	 * @param From      Start (world; its cell must be walkable or snap within GoalSnapCells).
	 * @param To        Goal (world).
	 * @param OutPoints Waypoints (world, Z on the surface) ending at the goal cell's centre, or at the
	 *                  closest reached cell for a partial route. Empty on failure.
	 * @param OutStats  Optional search statistics.
	 * @return True when a route (complete or allowed-partial) was produced.
	 */
	bool FindPath(const FVector& From, const FVector& To, TArray<FVector>& OutPoints, FVoxelSurfacePathStats* OutStats = nullptr) const;

	/** Drop intermediate cells wherever the straight run between the kept cells is walkable. */
	void SimplifyPath(TArray<FIntPoint>& Cells) const;

	/** Every cell on the straight line between two cells is walkable and each step is within the rise limit. */
	bool IsStraightRunWalkable(const FIntPoint& From, const FIntPoint& To) const;

	/** Forget sampled heights (a new search starts with an empty cache anyway). */
	void ClearCache() const;

	/** A cell as one search knows it: its height and walkability, sampled once, and the search's bookkeeping. */
	struct FCell
	{
		float Height = 0.f;
		float G = TNumericLimits<float>::Max();
		FIntPoint Parent = FIntPoint::ZeroValue;
		bool bSampled = false;
		bool bWalkable = false;
		bool bClosed = false;
		bool bHasParent = false;
		/** The sampler had no height for it (not walkable in this search, and not known to be ground or not). */
		bool bUnknown = false;
		/** Reached by the pocket check's flood from the goal. */
		bool bInGoalPocket = false;
	};

	/**
	 * The cells one search touched, in pages of 32 x 32 found through a map with the last page remembered (G73): a
	 * neighbour is one array index away, where a TMap of heights cost a hash lookup per height and per walkability test.
	 * Pages come from a per-thread pool and go back to it when the grid is reset or destroyed.
	 */
	class FCellGrid
	{
	public:
		FCellGrid() = default;
		FCellGrid(const FCellGrid&) = delete;
		FCellGrid& operator=(const FCellGrid&) = delete;
		~FCellGrid();

		/** The cell's record, made (unsampled) on first use. The reference stays valid until Reset. */
		FCell& Get(const FIntPoint& Cell);

		/** Forget every cell; the pages return to the pool. */
		void Reset();

	private:
		static constexpr int32 PageShift = 5;
		static constexpr int32 PageSize = 1 << PageShift;
		struct FPage
		{
			FCell Cells[PageSize * PageSize];
		};
		TMap<FIntPoint, FPage*> Pages;
		FIntPoint LastKey = FIntPoint(TNumericLimits<int32>::Max(), 0);
		FPage* LastPage = nullptr;
	};

private:
	FVoxelSurfaceNavParams Params;
	FHeightSampler Sampler;
	FBlockedQuery Blocked;

	mutable FCellGrid Cells;
	mutable int32 SampleCount = 0;
	mutable bool bSamplesCapped = false;

	/** A cell's record with its height sampled and its walkability decided (above water, not blocked). */
	const FCell& Sampled(const FIntPoint& Cell) const;

	/** The step rule between two sampled cells next to each other (orthogonally, or diagonally for the rise limit). */
	bool CanStepBetween(const FCell& From, const FCell& To, bool bDiagonal) const;

	/**
	 * Whether the goal lies in a pocket of at most PocketCells cells that Start is not in: a flood from the goal over the
	 * cells one can step between (orthogonal steps; the rules are symmetric) that runs out before PocketCells.
	 */
	bool IsGoalInPocket(const FIntPoint& Goal, const FIntPoint& Start) const;

	bool SnapToWalkable(const FIntPoint& Cell, FIntPoint& OutCell) const;
	static float Heuristic(const FIntPoint& A, const FIntPoint& B, float CellSize);
};
