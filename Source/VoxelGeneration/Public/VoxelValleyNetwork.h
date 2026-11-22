// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/ScopeRWLock.h"
#include "VoxelCubeSphere.h"
#include "VoxelValleyConfiguration.h"

class UVoxelWorldConfiguration;

/**
 * The ground the valley network is made over (Documentation/VALLEYS_PLAN.md, section 6.1): the natural height without
 * the valleys, a closed form of the seed and the position (so any thread reads it and nothing waits), and the sea.
 * The spike (V0) routed on the full natural height sampled at the nodes: the hills' noise, not continentalness alone,
 * decides where the land and the sea are.
 *
 * Two kinds of world, as the hydrology's ground (the plan's V5). A heightmap world's height is a world Z at a world
 * (X, Y); a planet's is an altitude over the datum sphere along a unit direction from its centre, and its sea an
 * altitude too. The network's points are world (X, Y, 0) on a heightmap world and points on the datum sphere, relative
 * to the planet's centre (the direction times the datum radius), on a planet.
 */
struct VOXELGENERATION_API FVoxelValleyGround
{
	/** A heightmap world: the natural ground's height at a world (X, Y), a world Z, without the valleys. Thread-safe */
	TFunction<double(double X, double Y)> Height;

	/** A planet: the natural ground's altitude over the datum along a unit direction from the centre, without the valleys. Thread-safe */
	TFunction<double(const FVector& Direction)> Altitude;

	/** A planet, and its datum radius (the sphere the lattice's cells and the curves lie on) */
	bool bRadial = false;
	double PlanetRadius = 0.0;

	/** The world has a sea at SeaLevel (a world Z; a planet: an altitude). With no sea, SeaLevel is the terrain's sea level, the floor's base */
	bool bSea = false;
	double SeaLevel = 0.0;

	/** The seed the nodes are placed by */
	int32 Seed = 0;

	/** @return Whether the ground can be read: the height function of its kind is set */
	bool IsValid() const { return bRadial ? static_cast<bool>(Altitude) : static_cast<bool>(Height); }

	/**
	 * @param Point A network point: world (X, Y, 0), or a point on (or near) the datum sphere relative to the centre
	 * @return The natural height there: the world Z, or the altitude along the point's direction
	 */
	double HeightAt(const FVector& Point) const
	{
		return bRadial ? Altitude(Point.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector)) : Height(Point.X, Point.Y);
	}

	/**
	 * The ground of a world configuration: the world mode's natural height (the noise, continentalness, the island's
	 * falloff; a planet's radial displacement), computed from the configuration's values alone.
	 * @param Config the world configuration
	 * @return The ground
	 */
	static FVoxelValleyGround FromConfig(const UVoxelWorldConfiguration& Config);
};

/**
 * The lattice the network's nodes lie on: a node a cell. A heightmap world's cells are squares of CellSize, a cell
 * (X, Y) the square from (X, Y) x CellSize. A planet's are a cube sphere's (FVoxelCubeSphereGrid, the grid the
 * hydrology's cells use since its H8), a cell an atlas coordinate, as many along a face's edge as bring a cell nearest
 * to CellSize at a face's middle; a cell's neighbours are the cells it shares a side or a corner with on the sphere
 * (FVoxelCubeSphereGrid::GetAdjacentCell), the same relation from both sides of a face's edge: eight, seven at a
 * cube's corner. So a node, its link and its valley are the same whichever face they are worked out from.
 *
 * A tile is TileCells x TileCells cells: on a planet the cells of a face's square of them (a face's last tiles are
 * smaller when the cells along an edge are not a multiple of TileCells), its coordinate (Face x TilesPerEdge + TU, TV).
 * A value; pure and thread-safe.
 */
struct VOXELGENERATION_API FVoxelValleyLattice
{
	FVoxelValleyLattice() = default;

	/**
	 * @param Settings sanitized settings
	 * @param Ground the ground (its kind and its datum radius)
	 */
	FVoxelValleyLattice(const FVoxelValleySettings& Settings, const FVoxelValleyGround& Ground);

	bool IsRadial() const { return bRadial; }

	/** @return A cell's width: the settings' on a heightmap world; on a planet the cells' width at a face's middle */
	double GetCellSize() const { return CellSize; }

	/** @return A planet's cells */
	const FVoxelCubeSphereGrid& GetCells() const { return Cells; }

	/** @return The cell a network point lies in (a planet: the cell its direction points through) */
	FIntPoint CellOf(const FVector& Point) const;

	/** @return The tile a cell lies in */
	FIntPoint TileOf(const FIntPoint& Cell) const;

	/**
	 * A tile's cells.
	 * @param Tile The tile
	 * @param OutFirst The cell of its local (0, 0): a cell's local place is Cell - OutFirst
	 * @param OutSize Its cells along X (a planet: U) and Y (V)
	 * @return False for a planet's coordinate that names no tile
	 */
	bool GetTileCells(const FIntPoint& Tile, FIntPoint& OutFirst, FIntPoint& OutSize) const;

	/**
	 * A cell's neighbour, by index: (DX, DY) = (-1, -1), (0, -1), (1, -1), (-1, 0), (1, 0), (-1, 1), (0, 1), (1, 1).
	 * @param Cell The cell
	 * @param Index 0 to 7
	 * @param OutNeighbour The neighbour
	 * @return False where a planet's cell has none (the corner off a cube's corner)
	 */
	bool GetNeighbour(const FIntPoint& Cell, int32 Index, FIntPoint& OutNeighbour) const;

	/** @return The point at a fraction (FU, FV) of a cell ((0.5, 0.5) is its middle): world (X, Y, 0), or on the datum sphere */
	FVector PointAt(const FIntPoint& Cell, double FU, double FV) const;

	/** @return A point brought onto the lattice's surface: as it is on a heightmap world, onto the datum sphere on a planet */
	FVector OntoSurface(const FVector& Point) const
	{
		return bRadial ? Point.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector) * Radius : Point;
	}

	/** @return The datum radius (a planet) */
	double GetRadius() const { return Radius; }

	/** @return How many tiles a planet's face has along an edge */
	int32 GetTilesPerEdge() const { return TilesPerEdge; }

private:
	bool bRadial = false;
	double CellSize = 35000.0;
	double Radius = 0.0;
	int32 TileCells = 8;
	int32 TilesPerEdge = 0;
	FVoxelCubeSphereGrid Cells;
};

/** A node of the valley network: one per lattice cell. */
struct FVoxelValleyNode
{
	/** The node's lattice cell */
	FIntPoint Cell = FIntPoint::ZeroValue;

	/** Where the node lies (a network point: world (X, Y, 0), or on a planet's datum sphere) and its natural ground's height there */
	FVector Position = FVector::ZeroVector;
	double Height = 0.0;

	/** The valley floor's height at the node: a share of its ground over the sea, under the sea's level at a coast, a breach's sink's floor on a breach */
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
	/** Its two ends: network points (world (X, Y, 0), or on a planet's datum sphere) */
	FVector A = FVector::ZeroVector;
	FVector B = FVector::ZeroVector;

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

	/** The cell of its local (0, 0) and its cells along X and Y (a heightmap world: Tile x TileCells, TileCells x TileCells) */
	FIntPoint FirstCell = FIntPoint::ZeroValue;
	FIntPoint Size = FIntPoint::ZeroValue;

	/** Its cells' nodes, row by row (Y, then X) */
	TArray<FVoxelValleyNode> Nodes;

	/** The valley links out of its nodes, as curves of segments, link by link, upper node to lower */
	TArray<FVoxelValleySegment> Segments;

	/** Its bounds (network points), widened by every segment's valley half width */
	FBox Bounds = FBox(ForceInit);

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
 * the sea and at least MinFloorHeight over it (or its ground, if lower), MinFloorHeight under the sea's level at a coast, and never rises
 * along a link. A valley's links are joined into a curve, each
 * node's corner spread between its main link in and its link out.
 *
 * On a planet (the plan's V5) the same on a cube sphere's cells (FVoxelValleyLattice): the heights are altitudes, the
 * points lie on the datum sphere and every point of a curve is brought back onto it, a step's length is the chord
 * between two nodes.
 */
class VOXELGENERATION_API FVoxelValleyNetwork
{
public:
	/**
	 * The network's version, in the fingerprint's term: it moves when the same settings would make other valleys or other
	 * bodies in them (2: the valley rivers, the valleys plan's V3; 3: G268's, a short piece kept for the pieces into it and
	 * the brooks led across a valley's floor to its river)
	 */
	static constexpr int32 Version = 3;

	FVoxelValleyNetwork(const FVoxelValleySettings& InSettings, FVoxelValleyGround InGround);

	/**
	 * @return Whether a world has valleys: it names a valley configuration and is a plane, an island or a planet, meshed
	 *         smooth or in blocks (block worlds and planets since the plan's V5)
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
	 * The valley segments that reach a box of a heightmap world: every segment whose valley (its half width on either
	 * side) overlaps it. On a planet, none: GatherSegmentsNear.
	 * @param Bounds the box (world X, Y)
	 * @param Out the segments, appended
	 */
	void GatherSegments(const FBox2D& Bounds, TArray<FVoxelValleySegment>& Out) const;

	/**
	 * The valley segments whose valley comes within a distance of a point: on a heightmap world those of the box around
	 * it; on a planet a cap of the sphere, the distance a chord.
	 * @param Point A network point (world (X, Y, 0), or on the datum sphere relative to the planet's centre)
	 * @param Distance The distance; a negative one takes every segment of the world (a planet: a chunk that holds its centre)
	 * @param Out the segments, appended
	 */
	void GatherSegmentsNear(const FVector& Point, double Distance, TArray<FVoxelValleySegment>& Out) const;

	/** @return The lattice cell a world (X, Y) lies in (a heightmap world) */
	FIntPoint CellOf(const FVector2D& Position) const { return Lattice.CellOf(FVector(Position.X, Position.Y, 0.0)); }

	/** @return The lattice cell a network point lies in */
	FIntPoint CellOf(const FVector& Point) const { return Lattice.CellOf(Point); }

	/** @return The tile a lattice cell lies in */
	FIntPoint TileOf(const FIntPoint& Cell) const { return Lattice.TileOf(Cell); }

	/** @return A cell's neighbour by index (FVoxelValleyLattice::GetNeighbour) */
	bool GetNeighbour(const FIntPoint& Cell, int32 Index, FIntPoint& OutNeighbour) const { return Lattice.GetNeighbour(Cell, Index, OutNeighbour); }

	const FVoxelValleySettings& GetSettings() const { return Settings; }
	const FVoxelValleyGround& GetGround() const { return Ground; }
	const FVoxelValleyLattice& GetLattice() const { return Lattice; }
	bool IsRadial() const { return Ground.bRadial; }

	/** @return How many tiles the network has kept */
	int32 GetNumTiles() const;

	/**
	 * The height with the valleys at a world (X, Y) of a heightmap world (section 6.2): the world modes' point query, the
	 * same evaluation as the generators' (VoxelValleyCarve), from the segments that reach the point's tile. Any thread.
	 * @param X, Y The place
	 * @param Natural The natural height there (after the island's falloff)
	 * @return The height with the valleys
	 */
	double ApplyToHeight(double X, double Y, double Natural) const;

	/**
	 * A planet's altitude with the valleys along a direction: ApplyToHeight's evaluation at the direction's point on the
	 * datum sphere, from the segments that reach its lattice cell. Any thread.
	 * @param Direction A unit direction from the planet's centre
	 * @param Natural The natural altitude there
	 * @return The altitude with the valleys
	 */
	double ApplyToAltitude(const FVector& Direction, double Natural) const;

	/**
	 * The lowest and highest floor among the valley segments that reach a box of a heightmap world: what a column's slab
	 * widens by, since its samples may miss a valley narrower than the column or a floor over the hills it fills.
	 * @param Bounds The box (world X, Y)
	 * @param OutMin, OutMax The floors' range
	 * @return Whether any valley reaches the box
	 */
	bool GetFloorRange(const FBox2D& Bounds, double& OutMin, double& OutMax) const;

	/**
	 * GetFloorRange near a point (GatherSegmentsNear): a planet's slab cell.
	 * @param Point A network point
	 * @param Distance The distance (a chord on a planet)
	 * @param OutMin, OutMax The floors' range
	 * @return Whether any valley comes within the distance
	 */
	bool GetFloorRangeNear(const FVector& Point, double Distance, double& OutMin, double& OutMax) const;

	/**
	 * The valley floor a network point lies on (G267, G268): the nearest segment whose floor holds the point (within its
	 * FloorHalfWidth of the centre curve, where the carve's weight is whole but for a head's taper), from the segments
	 * that reach the point's tile (the point query's buckets). Ties by the link's cell. Any thread.
	 * @param Point A network point (world (X, Y, 0), or on the datum sphere)
	 * @param OutSegment The segment
	 * @return Whether the point lies on a valley's floor
	 */
	bool FindFloorAt(const FVector& Point, FVoxelValleySegment& OutSegment) const;

private:
	/**
	 * Every segment that reaches a tile, as rows in the tile's frame, bucketed for the point query: a heightmap world's
	 * tile in 8 x 8 squares, a planet's by its cells.
	 */
	struct FReach
	{
		FVector Origin = FVector::ZeroVector;
		double BucketSize = 1.0;
		int32 Buckets = 1;
		FIntPoint FirstCell = FIntPoint::ZeroValue;
		FIntPoint Size = FIntPoint::ZeroValue;
		TArray<FVector4f> Rows;
		TArray<FVoxelValleySegment> Segments;
		TArray<int32> First;
		TArray<int32> Items;
	};

	/** @return The segments that reach a tile, worked out on first use and kept */
	TSharedRef<const FReach> GetReach(const FIntPoint& Tile) const;

	/** @return A planet's tiles a cap of the sphere may draw segments from (its cells' tiles within reach) */
	void GetTilesNear(const FVector& Point, double Distance, TArray<FIntPoint>& OutTiles) const;

	/** @return The point query's evaluation of a bucket's rows at a point of the reach's frame */
	static double Evaluate(const FReach& Reach, int32 Bucket, const FVector3f& Local, double Natural);

	FVoxelValleySettings Settings;
	FVoxelValleyGround Ground;
	FVoxelValleyLattice Lattice;
	mutable FRWLock Lock;
	mutable TMap<FIntPoint, TSharedRef<const FVoxelValleyTile>> Tiles;
	mutable FRWLock ReachLock;
	mutable TMap<FIntPoint, TSharedRef<const FReach>> Reaches;
};
