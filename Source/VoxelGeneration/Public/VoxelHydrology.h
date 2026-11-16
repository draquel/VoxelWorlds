// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelConditioningLine.h"
#include "VoxelConditioningLineShape.h"
#include "VoxelHydrologyConfiguration.h"
#include "VoxelWaterLevels.h"

class IVoxelWorldMode;
class UVoxelWorldConfiguration;

/**
 * The natural ground the hydrology generator reads, and the world's values it works in. The height is a function any
 * thread may call (a world mode's natural surface; a test's made ground).
 */
struct VOXELGENERATION_API FVoxelHydrologyGround
{
	/** The natural ground's height at a world (X, Y): a world Z. Thread-safe */
	TFunction<double(double X, double Y)> Height;

	/** The world has a sea, at this level (a world Z) */
	bool bSea = false;
	double SeaLevel = 0.0;

	/** The world's voxel size, its chunk's width in world units, and its seed */
	double VoxelSize = 100.0;
	double ChunkWorldSize = 3200.0;
	int32 Seed = 0;

	/**
	 * The ground of a world configuration: the world mode's natural surface (IVoxelWorldMode::GetSurfaceOnUpLine) with
	 * the configuration's noise, and its sea.
	 * @param Config The world configuration
	 * @param WorldMode The world's mode (shared: the function holds it)
	 * @return The ground
	 */
	static FVoxelHydrologyGround FromConfig(const UVoxelWorldConfiguration& Config, const TSharedRef<const IVoxelWorldMode>& WorldMode);
};

/**
 * The natural ground on the generator's grid, kept as it is read. One per piece of work and per thread: not shared.
 * A cell (X, Y) is the world place (X, Y) times the cell's width.
 */
class VOXELGENERATION_API FVoxelHydrologyHeights
{
public:
	FVoxelHydrologyHeights(const FVoxelHydrologyGround& InGround, const FVoxelHydrologySettings& InSettings);

	/** @return The ground's height at a cell */
	double At(const FIntPoint& Cell);

	/** @return A cell's width (world units) */
	double GetCellSize() const { return CellSize; }

	/** @return The world (X, Y) of a cell */
	FVector2D PlaceOf(const FIntPoint& Cell) const { return FVector2D(Cell.X * CellSize, Cell.Y * CellSize); }

	/** @return Whether a cell's ground is at or under the sea */
	bool IsSea(const FIntPoint& Cell) { return Ground.bSea && At(Cell) <= Ground.SeaLevel; }

	/**
	 * Where water runs from a cell: the lowest of its eight neighbours that lies under it, by the steepest fall (a
	 * diagonal step is root two long).
	 * @param Cell The cell
	 * @param OutNext The neighbour
	 * @return False at a pit: no neighbour lies under the cell
	 */
	bool GetDownhill(const FIntPoint& Cell, FIntPoint& OutNext);

	/** @return How many cells were read from the ground (not from the kept values) */
	int64 GetNumSamples() const { return NumSamples; }

	const FVoxelHydrologyGround& GetGround() const { return Ground; }

private:
	const FVoxelHydrologyGround& Ground;
	double CellSize = 800.0;
	TMap<FIntPoint, double> Kept;
	int64 NumSamples = 0;
};

/**
 * A pit's lake (the hydrology plan's sections 3.1 and 5.8): what the hollow around a pit holds when it fills. Made by
 * one flood from the pit, lowest cell first. While the flood climbs, every cell it has taken lies under the level it
 * has reached: the hollow so far. When it takes a cell under that level it has passed a saddle, and goes on down the
 * ground beyond it: if that ground runs away (to the sea, or further down than a lake may be deep, or wider than a
 * lake may be) the saddle is the lake's rim and the first cell beyond it the outlet; if it is one more hollow and the
 * flood climbs out of it again, the two are one lake. So nested pits make one lake at their true spill level, from
 * whichever pit the flood starts, and no lake needs another's to be made first.
 *
 * A lake that would be deeper or wider than the settings allow is closed: it stands at the cap, with no outlet.
 * Shared and immutable.
 */
struct VOXELGENERATION_API FVoxelHydrologyLake
{
	/** The pit the flood started from, and the lake's lowest cell (the same lake from another of its pits has the same) */
	FIntPoint Pit = FIntPoint::ZeroValue;
	FIntPoint LowestCell = FIntPoint::ZeroValue;

	/** The lowest ground in the lake */
	double LowestGround = 0.0;

	/**
	 * The level the hollow fills to: the rim's lowest ground (an open lake's spill level; a lake closed for its width
	 * the rim of the largest hollow the caps allow), or the depth cap's level. A cell of the hollow lies under it.
	 */
	double RimLevel = 0.0;

	/** The water's level: under the rim by the margin, and under every ground on the footprint's edge (FVoxelWaterBody::Level) */
	double WaterLevel = 0.0;

	/** No outlet: closed for its depth (1), for its width (2), or because the flood found no end within its budget (3). 0: open */
	uint8 Closed = 0;

	/** An open lake: the rim's cell the water leaves over, the cell beyond it, and the lake's cell before it */
	FIntPoint Saddle = FIntPoint::ZeroValue;
	FIntPoint Outlet = FIntPoint::ZeroValue;
	FIntPoint PreSaddle = FIntPoint::ZeroValue;

	/** The hollow's cells: those under the rim that the flood reached from the pit */
	TSet<FIntPoint> Cells;

	/** An open lake: from each cell of the hollow the next one on the way to the saddle (PreSaddle maps to itself) */
	TMap<FIntPoint, FIntPoint> ToSaddle;

	/** Whether the lake is deep and wide enough to be a body, and its record when it is */
	bool bKept = false;
	FVoxelWaterBody Body;

	/** The cells whose ground lies under the water's level, the water's area and its depth at the deepest */
	int32 NumWetCells = 0;
	double Area = 0.0;
	double Depth = 0.0;

	/** How far the level was lowered from the rim's margin for ground on the footprint's edge (0: not at all) */
	double LoweredForEdge = 0.0;

	/** Cells the flood took (the cost) */
	int32 NumFlooded = 0;

	/** @return Whether a cell's ground lies under the water of a kept lake */
	bool IsOpen() const { return Closed == 0; }
};

/** How a spring's descent ended. */
enum class EVoxelHydrologyEnd : uint8
{
	/** The lattice square holds no spring (its candidate stands too low, or in the sea) */
	NoSpring,
	/** In the sea */
	Sea,
	/** In a lake with no outlet */
	ClosedLake,
	/** Longer than the longest river */
	TooLong,
	/** Across more hollows than a river may cross */
	TooManyLakes
};

/**
 * A spring's descent: the cells from the spring down to where the water ends, by steepest descent over the natural
 * ground, through each hollow on the way from where it enters it to its saddle. A function of the spring's lattice
 * square and the ground alone, so it is the same from any region. Shared and immutable.
 */
struct VOXELGENERATION_API FVoxelHydrologyTrace
{
	/** The spring's lattice square, and its cell */
	FIntPoint Square = FIntPoint::ZeroValue;
	FIntPoint Spring = FIntPoint::ZeroValue;

	EVoxelHydrologyEnd End = EVoxelHydrologyEnd::NoSpring;

	/** The cells, spring first. Each is one of the eight neighbours of the one before */
	TArray<FIntPoint> Cells;

	/** Per cell: the lake whose hollow it lies in (an index into Lakes), or -1 */
	TArray<int32> LakeOf;

	/** The lakes the descent came to, in order */
	TArray<TSharedPtr<const FVoxelHydrologyLake>> Lakes;

	/** The descent's length along its cells, and its fall, spring to end */
	double Length = 0.0;
	double Fall = 0.0;

	/** Whether the descent is a river: it ends in the sea or in a kept lake with no outlet, long enough and gentle enough */
	bool bKept = false;

	/** A kept descent's mouth: the first cell in the sea, or the cell it enters the closed lake at. Every river through one mouth is one system */
	FIntPoint Mouth = FIntPoint::ZeroValue;
};

/** One river reach of a system: a chain of the tree between a spring, a join, a lake and the mouth, with its bed. */
struct VOXELGENERATION_API FVoxelHydrologyReach
{
	/** The chain's first cell: the reach's identity (its body id and its bed's line id come from it) */
	FIntPoint StartCell = FIntPoint::ZeroValue;

	/** How many springs drain through it (its width class) */
	int32 Flow = 1;

	/** The river reach, and its bed's line */
	FVoxelWaterBody Body;
	FVoxelConditioningLineParams BedParams;
	TSharedPtr<const FVoxelConditioningLine> Bed;

	/** The profile it was given (the rules' counters, the deepest cut, the fall) */
	FVoxelConditioningLineShape::FRiverProfile Profile;

	/** The reach's length along its vertices */
	double Length = 0.0;

	/** What ends it: a join into another reach of the system, a lake, the sea */
	bool bEndsAtJoin = false;
};

/**
 * A river system: every kept descent that ends at one mouth, as river reaches. Whole or not at all: a system one of
 * whose reaches cannot be made within the limits (the cut's cap, the bend bound) has no reach, so no river ends on
 * land. Shared and immutable.
 */
struct VOXELGENERATION_API FVoxelHydrologySystem
{
	FIntPoint Mouth = FIntPoint::ZeroValue;

	/** The springs that drain to the mouth */
	int32 NumSprings = 0;

	/** The reaches (none when the system is refused) */
	TArray<FVoxelHydrologyReach> Reaches;

	/** Why the system has no reach, when it has none: empty otherwise */
	FString Refused;

	/** The deepest cut and the longest way from a spring to the mouth */
	double DeepestCut = 0.0;
	double LongestRiver = 0.0;

	/** Cells where two descents disagreed about the way on (0 in a sound network: the way on is the cell's alone) */
	int32 NumConflicts = 0;

	/** How many times the system was made: again, with its corners spread wider, while a bed broke the bend bound */
	int32 NumAttempts = 1;

	/** The first such cell, for a diagnostic: the cell and the two ways on */
	FString ConflictNote;
};

/** What the generator keeps between pieces of work (the network's cache; a test may pass none). Thread-safe. */
class IVoxelHydrologyCache
{
public:
	virtual ~IVoxelHydrologyCache() = default;

	virtual TSharedPtr<const FVoxelHydrologyTrace> FindTrace(const FIntPoint& Square) = 0;
	/** Keep a trace; the one kept is returned (the first stored for a square wins) */
	virtual TSharedRef<const FVoxelHydrologyTrace> StoreTrace(const TSharedRef<const FVoxelHydrologyTrace>& Trace) = 0;

	virtual TSharedPtr<const FVoxelHydrologyLake> FindLake(const FIntPoint& Pit) = 0;
	virtual TSharedRef<const FVoxelHydrologyLake> StoreLake(const TSharedRef<const FVoxelHydrologyLake>& Lake) = 0;

	virtual TSharedPtr<const FVoxelHydrologySystem> FindSystem(const FIntPoint& Mouth) = 0;
	virtual TSharedRef<const FVoxelHydrologySystem> StoreSystem(const TSharedRef<const FVoxelHydrologySystem>& System) = 0;
};

/** The bodies of a region (FVoxelHydrology::BuildRegion), and what working them out cost. */
struct VOXELGENERATION_API FVoxelHydrologyRegion
{
	/** The kept lakes whose footprints meet the region, by body id */
	TArray<TSharedRef<const FVoxelHydrologyLake>> Lakes;

	/** The river systems with a reach that meets the region, by mouth */
	TArray<TSharedRef<const FVoxelHydrologySystem>> Systems;

	/** The systems whose mouths are near enough to reach the region and that were refused (no reach), by mouth */
	TArray<TSharedRef<const FVoxelHydrologySystem>> Refused;

	/** Lattice squares looked at, of them springs, descents worked out here (not from the cache), of them kept as rivers */
	int32 NumSquares = 0;
	int32 NumSprings = 0;
	int32 NumTraced = 0;
	int32 NumRivers = 0;

	/** Systems and lakes worked out here, and systems refused */
	int32 NumSystemsBuilt = 0;
	int32 NumLakesBuilt = 0;
	int32 NumRefused = 0;

	/** Ground samples read */
	int64 NumSamples = 0;
};

/**
 * The hydrology generator (Documentation/HYDROLOGY_PLAN.md, section 5.8; D145): ponds and brooks as a pure function
 * of the seed and the natural ground, the same from any region, on any thread and on every machine.
 *
 *  - Springs. A square of the lattice holds one candidate, placed by a hash; it is a spring when it stands high
 *    enough over the sea (GetSpring).
 *  - Descents. A spring's water runs to the lowest neighbour, cell by cell; at a pit it fills the pit's lake
 *    (MakeLake) and goes on from its outlet; it ends in the sea or in a lake with no outlet (TraceSpring). A descent
 *    long enough and gentle enough is a river.
 *  - Lakes. A pit's lake is a body when it is deep and wide enough. Its level is under its rim by a margin, and under
 *    every natural ground on its footprint's edge, read a voxel apart, so no water stands against the footprint's edge.
 *  - Systems. The rivers that end at one mouth form a tree; a river reach is a chain of it between a spring, a join,
 *    a lake and the mouth, as wide as the springs that drain through it. Each chain is shaped as a conditioning line
 *    (FVoxelConditioningLineShape::RoundCorners), reads its ground and banks as a hand-placed river does
 *    (FVoxelRiverCourse) and takes the river profile (MakeRiverProfile): the surface never rises along a chain or from
 *    one chain to the next, a tributary comes into the reach it joins a tuck under that reach's surface, and a reach
 *    that leaves a lake starts at the lake's level (BuildSystem).
 *
 * Why any region finds the same bodies: a descent depends on the ground within the longest river's length of its
 * spring; a lake on its pit; a system on the springs within that length of its mouth. BuildRegion works out the
 * springs within twice the length of its region, and from them every lake and every system that can reach it.
 *
 * Heightmap worlds with a smooth surface (the plane and the island). A planet and a block world are the plan's H8.
 */
struct VOXELGENERATION_API FVoxelHydrology
{
	/** The generator's version: part of the save fingerprint's term. Raise it when the same settings would make other bodies */
	static constexpr int32 Version = 1;

	/** @return Whether a world generates hydrology: its configuration names a hydrology configuration and the world is one the generator works on */
	static bool IsEnabled(const UVoxelWorldConfiguration& Config);

	/** @return The save fingerprint's term of a world that generates hydrology ("Hydrology=<version>:<settings hash>"), else empty */
	static FString GetFingerprintTerm(const UVoxelWorldConfiguration& Config);

	/** @return The settings as the generator uses them: the caps held to what the engine allows (a lake's depth under a chunk and a quarter, its span under the longest river) */
	static FVoxelHydrologySettings Sanitize(const FVoxelHydrologySettings& Settings, const FVoxelHydrologyGround& Ground);

	/** @return How far a region's bodies depend on the ground around it: twice the longest river and a lattice square */
	static double GetDependencyDistance(const FVoxelHydrologySettings& Settings, const FVoxelHydrologyGround& Ground);

	/**
	 * A lattice square's spring.
	 * @param Square The square
	 * @param OutCell The spring's cell
	 * @return Whether the square's candidate is a spring
	 */
	static bool GetSpring(const FVoxelHydrologySettings& Settings, FVoxelHydrologyHeights& Heights, const FIntPoint& Square, FIntPoint& OutCell);

	/**
	 * A pit's lake.
	 * @param Pit A cell none of whose neighbours lies under it
	 * @return The lake
	 */
	static TSharedRef<const FVoxelHydrologyLake> MakeLake(const FVoxelHydrologySettings& Settings, FVoxelHydrologyHeights& Heights, const FIntPoint& Pit);

	/**
	 * A lattice square's descent.
	 * @param Square The square
	 * @param Cache Where the lakes are kept (may be null)
	 * @return The descent (End NoSpring when the square has no spring)
	 */
	static TSharedRef<const FVoxelHydrologyTrace> TraceSpring(const FVoxelHydrologySettings& Settings, FVoxelHydrologyHeights& Heights, const FIntPoint& Square,
		IVoxelHydrologyCache* Cache);

	/**
	 * The river system of a mouth.
	 * @param Mouth The mouth
	 * @param Rivers Every kept descent that ends at it
	 * @return The system
	 */
	static TSharedRef<const FVoxelHydrologySystem> BuildSystem(const FVoxelHydrologySettings& Settings, FVoxelHydrologyHeights& Heights, const FIntPoint& Mouth,
		TConstArrayView<TSharedRef<const FVoxelHydrologyTrace>> Rivers);

	/**
	 * The lakes and the river systems that reach a region.
	 * @param Region World XY
	 * @param Cache What is kept between calls (may be null: everything is worked out here)
	 * @param Out The bodies, in a fixed order
	 */
	static void BuildRegion(const FVoxelHydrologySettings& Settings, const FVoxelHydrologyGround& Ground, const FBox2D& Region, IVoxelHydrologyCache* Cache,
		FVoxelHydrologyRegion& Out);

	/** @return The lattice squares whose springs a region's bodies may depend on: those within the dependency distance of it */
	static FIntRect GetSquaresFor(const FVoxelHydrologySettings& Settings, const FVoxelHydrologyGround& Ground, const FBox2D& Region);

	/** @return A river reach's body id from its chain's first cell, and a lake's from its lowest cell and its size (never the sea's 0) */
	static uint32 MakeReachId(int32 Seed, const FIntPoint& StartCell);
	static uint32 MakeLakeId(int32 Seed, const FIntPoint& LowestCell, int32 NumCells);

	/**
	 * A bed's line id from its chain's first cell: over every road's and trail's between POIs (those lie under -2^61),
	 * so a bed is applied after them, and under a hand-placed line's of order 0 or more.
	 */
	static int64 MakeBedLineId(const FIntPoint& StartCell);
};
