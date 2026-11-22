// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/ScopeRWLock.h"
#include "VoxelValleyConfiguration.h"

class UVoxelWorldConfiguration;

/**
 * The ground the valley network is made over (Documentation/VALLEYS_PLAN.md, section 6.1): the natural height without
 * the valleys, a closed form of the seed and the position (so any thread reads it and nothing waits), and the sea.
 * The spike (V0) routed on the full natural height sampled at the nodes: the hills' noise, not continentalness alone,
 * decides where the land and the sea are.
 */
struct VOXELGENERATION_API FVoxelValleyGround
{
	/** The natural ground's height at a world (X, Y), a world Z, without the valleys. Thread-safe */
	TFunction<double(double X, double Y)> Height;

	/** The world has a sea at SeaLevel (a world Z). With no sea, SeaLevel is the terrain's sea level, the floor's base */
	bool bSea = false;
	double SeaLevel = 0.0;

	/** The seed the nodes are placed by */
	int32 Seed = 0;

	/** @return Whether the ground can be read */
	bool IsValid() const { return static_cast<bool>(Height); }

	/**
	 * The ground of a plane or an island world: the world mode's natural height (the noise, continentalness, the
	 * island's falloff), computed from the configuration's values alone.
	 * @param Config the world configuration
	 * @return The ground
	 */
	static FVoxelValleyGround FromConfig(const UVoxelWorldConfiguration& Config);
};

/** A node of the valley network: one per lattice cell. */
struct FVoxelValleyNode
{
	/** The node's lattice cell */
	FIntPoint Cell = FIntPoint::ZeroValue;

	/** Where the node lies (world X, Y) and its natural ground's height there */
	FVector2D Position = FVector2D::ZeroVector;
	double Height = 0.0;

	/** The valley floor's height at the node: a share of its ground over the sea, the sea's level at a coast, a breach's sink's floor on a breach */
	double Floor = 0.0;

	/** The cell of the node it links to (when bHasLink) */
	FIntPoint Down = FIntPoint::ZeroValue;
	bool bHasLink = false;

	/** A coast or sea node: a link that reaches it has reached the sea; it has no link of its own */
	bool bSea = false;

	/** Lower than its eight neighbours (D180's sink); one that kept no link has a hollow too wide to breach */
	bool bSink = false;

	/** Its link is a step of a breach through a sink's lowest saddle (it may rise in height; its floor does not) */
	bool bBreach = false;

	/** The nodes whose links reach it within the drainage reach, itself included */
	int32 Drainage = 1;

	/** Its link carries a valley (its drainage is at least the valley drainage) */
	bool bValley = false;

	/** Its valley starts here: no valley link comes into it */
	bool bHead = false;
};

/** A straight piece of a valley's centre curve, with what the floor and the widths are at its two ends. */
struct FVoxelValleySegment
{
	FVector2D A = FVector2D::ZeroVector;
	FVector2D B = FVector2D::ZeroVector;

	/** The floor's height at A and at B (never rising from A to B) */
	double FloorA = 0.0;
	double FloorB = 0.0;

	/** Half the floor's width, half the valley's, half the valley river's (V3) */
	float FloorHalfWidth = 0.0f;
	float ValleyHalfWidth = 0.0f;
	float RiverHalfWidth = 0.0f;

	/** The carve's weight scale at A and at B: 0 at a valley's head node, 1 from half a link on */
	float TaperA = 1.0f;
	float TaperB = 1.0f;

	/** Where A and B lie along their link, from its upper node (0) to its lower (1) */
	float LinkA = 0.0f;
	float LinkB = 1.0f;

	/** The cell of the link's upper node */
	FIntPoint Link = FIntPoint::ZeroValue;
};

/** The network over one tile: the nodes of its cells, and the segments of the valley links out of them. */
struct FVoxelValleyTile
{
	FIntPoint Tile = FIntPoint::ZeroValue;
	int32 TileCells = 0;

	/** Its cells' nodes, row by row (Y, then X) */
	TArray<FVoxelValleyNode> Nodes;

	/** The valley links out of its nodes, as curves of segments, link by link, upper node to lower */
	TArray<FVoxelValleySegment> Segments;

	/** Its bounds, widened by every segment's valley half width */
	FBox2D Bounds = FBox2D(ForceInit);

	/** @return The node of a cell of this tile, or null */
	const FVoxelValleyNode* FindNode(const FIntPoint& Cell) const;
};

/**
 * The valley network of a world (Documentation/VALLEYS_PLAN.md, section 6.1): a pure function of the seed, the settings
 * and the natural ground, worked out by tiles that any thread may build and keeps once built. Two tiles never disagree:
 * each is built from the ground around it out to a fixed margin (GetMarginCells), so a node, its link, its drainage
 * and its floor are the same from any tile. Nothing waits on another thread: a tile asked for twice at once is built
 * twice and the first kept.
 *
 * Each lattice cell holds a node placed by a hash of the cell and the seed. A node links to the neighbour (of eight)
 * with the steepest fall of the ground; a node lower than all its neighbours is a sink, which links on through the
 * lowest saddle out of its hollow to the first lower node within BreachCells (a bounded priority-flood; D180), the
 * steps of that path at the sink's floor. A node's drainage counts the nodes linking into it within DrainageReach
 * links; a link from a node of ValleyDrainage or more is a valley. A node's floor is FloorFraction of its ground over
 * the sea and at least MinFloorHeight over it (or its ground, if lower), the sea's level at a coast, and never rises
 * along a link. A valley's links are joined into a curve, each
 * node's corner spread between its main link in and its link out.
 */
class VOXELGENERATION_API FVoxelValleyNetwork
{
public:
	/** The network's version, in the fingerprint's term: it moves when the same settings would make other valleys */
	static constexpr int32 Version = 1;

	FVoxelValleyNetwork(const FVoxelValleySettings& InSettings, FVoxelValleyGround InGround);

	/**
	 * @return Whether a world has valleys: it names a valley configuration, and is a plane or an island meshed smooth
	 *         (block worlds and planets are the plan's V5)
	 */
	static bool IsEnabled(const UVoxelWorldConfiguration& Config);

	/** @return The save fingerprint's term ("Valleys=<version>:<hash>") of a world that has valleys, else empty */
	static FString GetFingerprintTerm(const UVoxelWorldConfiguration& Config);

	/** @return The network of a world that has valleys, else null */
	static TSharedPtr<FVoxelValleyNetwork> Create(const UVoxelWorldConfiguration& Config);

	/** @return The settings with every value in its range */
	static FVoxelValleySettings Sanitize(const FVoxelValleySettings& Settings);

	/**
	 * @return How many cells around a tile its build reads: a node's drainage reaches DrainageReach links up, its
	 *         curve a node beyond either end, a breach's path BreachCells from its sink and the sink's from the node
	 */
	static int32 GetMarginCells(const FVoxelValleySettings& Settings);

	/**
	 * Builds the network over a tile from nothing (pure; any thread).
	 * @param Settings sanitized settings
	 * @param Ground the ground
	 * @param Tile the tile
	 * @return The tile
	 */
	static TSharedRef<FVoxelValleyTile> BuildTile(const FVoxelValleySettings& Settings, const FVoxelValleyGround& Ground, const FIntPoint& Tile);

	/** @return A tile of the network, built on first use and kept (any thread) */
	TSharedRef<const FVoxelValleyTile> GetTile(const FIntPoint& Tile) const;

	/** @return The node of a lattice cell (its tile built on first use) */
	FVoxelValleyNode GetNode(const FIntPoint& Cell) const;

	/**
	 * The valley segments that reach a box: every segment whose valley (its half width on either side) overlaps it.
	 * @param Bounds the box (world X, Y)
	 * @param Out the segments, appended
	 */
	void GatherSegments(const FBox2D& Bounds, TArray<FVoxelValleySegment>& Out) const;

	/** @return The lattice cell a world (X, Y) lies in */
	FIntPoint CellOf(const FVector2D& Position) const;

	/** @return The tile a lattice cell lies in */
	FIntPoint TileOf(const FIntPoint& Cell) const;

	const FVoxelValleySettings& GetSettings() const { return Settings; }
	const FVoxelValleyGround& GetGround() const { return Ground; }

	/** @return How many tiles the network has kept */
	int32 GetNumTiles() const;

	/**
	 * The height with the valleys at a world (X, Y) (section 6.2): the world modes' point query, the same evaluation as
	 * the generators' (VoxelValleyCarve), from the segments that reach the point's tile. Any thread.
	 * @param X, Y The place
	 * @param Natural The natural height there (after the island's falloff)
	 * @return The height with the valleys
	 */
	double ApplyToHeight(double X, double Y, double Natural) const;

	/**
	 * The lowest and highest floor among the valley segments that reach a box: what a column's slab widens by, since its
	 * samples may miss a valley narrower than the column or a floor over the hills it fills.
	 * @param Bounds The box (world X, Y)
	 * @param OutMin, OutMax The floors' range
	 * @return Whether any valley reaches the box
	 */
	bool GetFloorRange(const FBox2D& Bounds, double& OutMin, double& OutMax) const;

private:
	/** Every segment that reaches a tile, as rows in the tile's frame, bucketed for the point query. */
	struct FReach
	{
		FVector2D Origin = FVector2D::ZeroVector;
		double BucketSize = 1.0;
		int32 Buckets = 1;
		TArray<FVector4f> Rows;
		TArray<int32> First;
		TArray<int32> Items;
	};

	/** @return The segments that reach a tile, worked out on first use and kept */
	TSharedRef<const FReach> GetReach(const FIntPoint& Tile) const;

	FVoxelValleySettings Settings;
	FVoxelValleyGround Ground;
	mutable FRWLock Lock;
	mutable TMap<FIntPoint, TSharedRef<const FVoxelValleyTile>> Tiles;
	mutable FRWLock ReachLock;
	mutable TMap<FIntPoint, TSharedRef<const FReach>> Reaches;
};
