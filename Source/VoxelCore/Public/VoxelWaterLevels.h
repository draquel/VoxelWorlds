// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelGatherNeed.h"
#include "VoxelPolyline.h"
#include "VoxelSeaSurface.h"

class UVoxelWorldConfiguration;

/** What a body of water is (the water plan's W2; lakes and river reaches since the hydrology plan's H1). */
enum class EVoxelWaterBodyKind : uint8
{
	/** The one sea: a Z plane, or a sphere on a planet (FVoxelSeaSurface) */
	Sea,
	/** A flat body above the sea, at one level over its footprint (FVoxelWaterLake) */
	Lake,
	/** A river reach: a sloped body along a polyline (FVoxelWaterReach) */
	Reach
};

/**
 * The frame the water levels are measured in: world Z on a heightmap world, the distance from the planet centre on a
 * planet. A snapshot carries it so a lake or a river reach is evaluated without the sea (a world with the sea off may
 * still hold a lake), and a planet's widths are arcs on the datum sphere, as a conditioning line's are.
 */
struct VOXELCORE_API FVoxelWaterFrame
{
	/** Levels are distances from Center (a planet) rather than world Z */
	bool bRadial = false;

	/** A planet: the planet centre */
	FVector Center = FVector::ZeroVector;

	/** A planet: the datum radius, on which a river reach's widths and a lake's cells are measured */
	double DatumRadius = 0.0;

	/** The frame of a world configuration: a planet's centre and datum radius, or a heightmap world's plain Z. */
	static FVoxelWaterFrame FromConfig(const UVoxelWorldConfiguration& Config);

	/** @return A world position's height in the frame's terms: its distance from the centre, or its Z */
	double HeightOf(const FVector& WorldPos) const
	{
		return bRadial ? FVector::Dist(WorldPos, Center) : WorldPos.Z;
	}

	/** @return The local up at a world position: the direction from the centre, or +Z */
	FVector UpAt(const FVector& WorldPos) const
	{
		return bRadial ? (WorldPos - Center).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector) : FVector::UpVector;
	}
};

/**
 * One vertex of a river reach's polyline (the water plan's section 7, the hydrology plan's section 5.1): where the
 * river is, how deep its bed is carved, where its surface lies and how far to each side it may hold water.
 */
struct FVoxelWaterReachVertex
{
	/** The vertex: world (X, Y) on a heightmap world (Z unused), the unit direction from the planet centre on a planet */
	FVector Position = FVector::ZeroVector;

	/** The bed's water level (the terms of FVoxelWaterBody::Level): what the bed's conditioning line carves down to */
	double BedLevel = 0.0;

	/** The surface's water level here, interpolated along the polyline; it never rises downstream */
	double SurfaceLevel = 0.0;

	/**
	 * Half the width of the river reach's footprint here (world units, along the surface on a planet): the bed's half
	 * width and its falloff, so the footprint reaches the banks. Water lies in a column of it where the ground is under
	 * the surface level.
	 */
	double HalfWidth = 0.0;

	FVoxelWaterReachVertex() = default;

	FVoxelWaterReachVertex(const FVector& InPosition, double InBedLevel, double InSurfaceLevel, double InHalfWidth)
		: Position(InPosition), BedLevel(InBedLevel), SurfaceLevel(InSurfaceLevel), HalfWidth(InHalfWidth)
	{
	}
};

/** A river reach at a place (FVoxelWaterReach::SampleAt, SampleOnSphere): what its nearest segment gives there. */
struct FVoxelWaterReachSample
{
	/** The nearest segment (its first vertex's index) and the place along it, 0 to 1 */
	int32 Segment = 0;
	double T = 0.0;

	/** The distance to the polyline (world units; the arc on a planet) */
	double Distance = 0.0;

	/** The surface's and the bed's water level at the nearest place: level across the river, sloped along it */
	double SurfaceLevel = 0.0;
	double BedLevel = 0.0;

	/** The footprint's half width at the nearest place */
	double HalfWidth = 0.0;

	/** The river's direction downstream at the nearest place: a unit vector along the surface (the tangent on a planet) */
	FVector Downstream = FVector::ForwardVector;

	/** The surface's slope along Downstream: the level's change per unit of length (negative where it falls) */
	double Slope = 0.0;
};

/**
 * A river reach's polyline, upstream to downstream: shared and immutable, made once (Make), as a conditioning line is
 * (FVoxelConditioningLine). At a place the nearest segment alone answers, found by the same search as the bed's line
 * (FVoxelPolylineBounds), so the surface is read at the same place along the same segment that gives the bed's target:
 * level across the river, sloped along it.
 */
struct VOXELCORE_API FVoxelWaterReach
{
	/**
	 * Make a river reach. Consecutive vertices at the same place are merged and a planet reach's directions normalized,
	 * by the conditioning line's own rule (FVoxelConditioningLine::Make), so a reach and a bed made from the same
	 * vertices have the same segments. Fewer than two distinct vertices: no segment, and the reach covers nothing.
	 *
	 * @param InVertices The polyline, upstream to downstream
	 * @param bInRadial A planet reach: the positions are directions from the planet centre, the levels distances from it
	 * @param InFlowSpeed How fast the river runs downstream (world units a second; 0: still)
	 * @return The reach
	 */
	static TSharedRef<const FVoxelWaterReach> Make(TArray<FVoxelWaterReachVertex> InVertices, bool bInRadial, float InFlowSpeed = 0.0f);

	/** @return The vertices (a planet reach's positions are unit directions) */
	const TArray<FVoxelWaterReachVertex>& GetVertices() const { return Vertices; }

	/** @return How many segments the reach has */
	int32 NumSegments() const { return Polyline.Segments; }

	/** @return Whether this is a planet reach */
	bool IsRadial() const { return Polyline.bRadial; }

	/** @return The largest half width of the footprint: how far from its segments the reach may hold water */
	double GetReach() const { return MaxHalfWidth; }

	/** @return The river's speed downstream (world units a second) */
	float GetFlowSpeed() const { return FlowSpeed; }

	/** @return The lowest and the highest surface level along the reach */
	double GetMinLevel() const { return MinLevel; }
	double GetMaxLevel() const { return MaxLevel; }

	/** @return The polyline's bounds and blocks */
	const FVoxelPolylineBounds& GetPolyline() const { return Polyline; }

	/**
	 * A heightmap reach at (X, Y).
	 * @param X World X
	 * @param Y World Y
	 * @param Out The sample (untouched when the place is outside the footprint)
	 * @return Whether (X, Y) lies in the footprint: within the half width of the nearest segment
	 */
	bool SampleAt(double X, double Y, FVoxelWaterReachSample& Out) const;

	/**
	 * A planet reach along a direction, as SampleAt.
	 * @param Direction Unit direction from the planet centre
	 * @param PlanetRadius The datum radius
	 * @param Out The sample
	 * @return Whether the direction lies in the footprint
	 */
	bool SampleOnSphere(const FVector& Direction, double PlanetRadius, FVoxelWaterReachSample& Out) const;

	/**
	 * The lowest and the highest surface level over the part of a heightmap reach that may reach a world XY region: the
	 * vertices of the blocks whose boxes, grown by the footprint, meet it. Conservative.
	 * @return False when no block meets the region
	 */
	bool GetLevelRange(const FBox2D& Region, double& OutMin, double& OutMax) const;

	/** GetLevelRange over a cap of a planet. */
	bool GetLevelRangeOnSphere(const FVector& CapDirection, double CapAngle, double PlanetRadius, double& OutMin, double& OutMax) const;

private:
	/** The sample of a segment the search found. */
	void FillSample(int32 Segment, double T, double Distance, double PlanetRadius, FVoxelWaterReachSample& Out) const;

	TArray<FVoxelWaterReachVertex> Vertices;
	FVoxelPolylineBounds Polyline;
	double MaxHalfWidth = 0.0;
	double MinLevel = 0.0;
	double MaxLevel = 0.0;
	float FlowSpeed = 0.0f;
};

/**
 * A lake's footprint: the columns it may hold water over, as a grid of cells (the hydrology plan's section 5.1). Water
 * is in a column of the footprint where the generated ground lies under the lake's level; the footprint does not draw
 * the shore, it keeps the lake's level off other low ground nearby. Shared and immutable, made once.
 *
 * On a heightmap world the grid is in world XY. On a planet it lies in the plane tangent to the datum sphere at the
 * lake's centre: a direction's cell is its datum point's offset from the centre's, along the plane's two axes.
 */
struct VOXELCORE_API FVoxelWaterLake
{
	/**
	 * A heightmap lake's footprint.
	 * @param InOrigin The world XY of the grid's minimum corner
	 * @param InCellSize A cell's width (world units)
	 * @param InSize How many cells along X and Y
	 * @param InCells Size.X * Size.Y bits, row by row (Y * Size.X + X): the cells of the footprint
	 */
	static TSharedRef<const FVoxelWaterLake> Make(const FVector2D& InOrigin, double InCellSize, const FIntPoint& InSize, TBitArray<> InCells);

	/**
	 * A planet lake's footprint, centred on a direction: the grid's middle lies at the centre's datum point.
	 * @param InCentre The direction from the planet centre to the lake's centre
	 * @param InPlanetRadius The datum radius
	 * @param InCellSize A cell's width (world units on the tangent plane)
	 * @param InSize How many cells along the plane's two axes
	 * @param InCells Size.X * Size.Y bits, row by row
	 */
	static TSharedRef<const FVoxelWaterLake> MakeOnSphere(const FVector& InCentre, double InPlanetRadius, double InCellSize,
		const FIntPoint& InSize, TBitArray<> InCells);

	/**
	 * The footprint of a drawn outline on a heightmap world: the cells whose centres lie inside the closed polygon,
	 * grown by one cell (a hand-placed lake's, the hydrology plan's section 5.7).
	 * @param Outline The polygon's corners in world XY, in order (closed from the last to the first)
	 * @param InCellSize A cell's width
	 */
	static TSharedRef<const FVoxelWaterLake> MakeFromOutline(TConstArrayView<FVector2D> Outline, double InCellSize);

	/** @return Whether this is a planet lake */
	bool IsRadial() const { return bRadial; }

	/** @return How many cells the footprint has */
	int32 NumCells() const { return CellCount; }

	/** @return A cell's width */
	double GetCellSize() const { return CellSize; }

	/** @return The grid's size in cells */
	FIntPoint GetSize() const { return Size; }

	/** @return Whether a cell of the grid is in the footprint (false outside the grid) */
	bool HasCell(int32 CellX, int32 CellY) const
	{
		return CellX >= 0 && CellY >= 0 && CellX < Size.X && CellY < Size.Y && Cells[CellY * Size.X + CellX];
	}

	/** @return Whether a heightmap column lies in the footprint (false for a planet lake) */
	bool Covers(double X, double Y) const;

	/** @return Whether a planet column, by its unit direction from the centre, lies in the footprint (false for a heightmap lake) */
	bool CoversDirection(const FVector& Direction) const;

	/** @return A heightmap lake's XY box: the cells of the footprint (invalid for a planet lake or an empty one) */
	const FBox2D& GetBounds2D() const { return Bounds; }

	/**
	 * A planet lake's footprint as a cap.
	 * @param OutDirection The cap's centre direction
	 * @param OutAngle The cap's half-angle (radians)
	 * @return False for a heightmap lake or an empty one
	 */
	bool GetCap(FVector& OutDirection, double& OutAngle) const;

	/** @return Whether some cell of a heightmap lake's footprint overlaps a world XY region */
	bool MeetsRegion(const FBox2D& Region) const;

	/** @return Whether a planet lake's cap meets a cap (conservative: the footprint's bounding cap) */
	bool MeetsCap(const FVector& CapDirection, double CapAngle) const;

	/** @return The world position of a cell's centre at a water level: (X, Y, Level), or on a planet the point at that distance from PlanetCenter */
	FVector GetCellCentre(int32 CellX, int32 CellY, double Level, const FVector& PlanetCenter) const;

private:
	/** Count the cells and build the bounds. */
	void Finish();

	bool bRadial = false;

	/** Heightmap: the grid's minimum corner. Planet: the grid's minimum corner on the tangent plane, from the centre's datum point */
	FVector2D Origin = FVector2D::ZeroVector;
	double CellSize = 1.0;
	FIntPoint Size = FIntPoint::ZeroValue;
	TBitArray<> Cells;
	int32 CellCount = 0;
	FBox2D Bounds = FBox2D(ForceInit);

	/** Planet: the centre's direction, the tangent plane's two axes, the datum radius and the footprint's half-angle */
	FVector Centre = FVector::UpVector;
	FVector AxisX = FVector::ForwardVector;
	FVector AxisY = FVector::RightVector;
	double PlanetRadius = 0.0;
	double CapAngle = 0.0;
};

/**
 * One body of water, as data (the water plan's D112: a consumer holds a body's record and never a level it has baked
 * in, so a body's level could change later by replacing the record): the sea, a lake or a river reach.
 *
 * A water level is a surface's height in the world's own vertical measure: world Z on a heightmap world, the distance
 * from the planet centre on a planet (so the sea's is WaterLevel or WaterRadius). It is not an altitude: an altitude is
 * measured from the world mode's datum (IVoxelWorldMode::GetAltitude).
 *
 * A body other than the sea has a footprint, the columns it may hold water over: a lake's cells (Lake), a river
 * reach's polyline with its half widths (Reach). Water is in a column of the footprint where the generated ground lies
 * under the level.
 */
struct VOXELCORE_API FVoxelWaterBody
{
	/** The sea's id. Every other body's id is its source's, stable for the same body on every machine, and not 0. */
	static constexpr uint32 SeaId = 0;

	uint32 Id = SeaId;

	EVoxelWaterBodyKind Kind = EVoxelWaterBodyKind::Sea;

	/** The surface's water level; a river reach's highest (its vertices carry the rest) */
	double Level = 0.0;

	/** Where the record holds: the body's water lies inside these bounds, and a change to it invalidates them (D112) */
	FBox Bounds = FBox(ForceInit);

	/** Kind Reach: the polyline (null otherwise) */
	TSharedPtr<const FVoxelWaterReach> Reach;

	/** Kind Lake: the footprint (null otherwise) */
	TSharedPtr<const FVoxelWaterLake> Lake;

	/**
	 * Kind Lake: the shallowest depth under the generated ground a cave may open at over the lake's footprint, in
	 * voxels (the hydrology plan's section 5.4), beside the cave configuration's own minimum depths: each cave layer's
	 * is at least this there, as under a conditioning line (FVoxelConditioningLineSurface). 0 leaves the caves their
	 * own. A river reach has none: its bed's line carries it.
	 */
	float MinCaveDepth = 0.0f;

	/**
	 * A lake's record.
	 * @param InId The body's id (not 0)
	 * @param InLevel The lake's level
	 * @param InLake Its footprint
	 * @param Frame The world's frame (a planet lake's bounds are around its cap at the level)
	 * @param MaxDepth How far under the level the lake's water may lie (the bounds' depth)
	 * @param InMinCaveDepth The shallowest depth a cave may open at over the footprint, in voxels (MinCaveDepth)
	 */
	static FVoxelWaterBody MakeLake(uint32 InId, double InLevel, const TSharedRef<const FVoxelWaterLake>& InLake, const FVoxelWaterFrame& Frame, double MaxDepth,
		float InMinCaveDepth = 0.0f);

	/**
	 * A river reach's record.
	 * @param InId The body's id (not 0)
	 * @param InReach Its polyline
	 * @param Frame The world's frame
	 */
	static FVoxelWaterBody MakeReach(uint32 InId, const TSharedRef<const FVoxelWaterReach>& InReach, const FVoxelWaterFrame& Frame);

	/** @return Whether the body's footprint may reach a heightmap world's XY region (the sea: always) */
	bool MeetsRegion(const FBox2D& Region) const;

	/** @return Whether the body's footprint may reach a cap of a planet (the sea: always) */
	bool MeetsCap(const FVector& CapDirection, double CapAngle, double PlanetRadius) const;
};

/** The water over a position (FVoxelWaterLevels::Sample). */
struct FVoxelWaterSample
{
	/** FVoxelWaterSample::Depth where no body covers the position: far above any surface */
	static constexpr double NoWaterDepth = -1.0e9;

	/** A body covers this column (the position may be above its surface: Depth says) */
	bool bWater = false;

	uint32 BodyId = FVoxelWaterBody::SeaId;

	EVoxelWaterBodyKind Kind = EVoxelWaterBodyKind::Sea;

	/** The surface's water level over the position: world Z, or the distance from the planet centre (FVoxelWaterBody) */
	double SurfaceLevel = 0.0;

	/** The position's depth under the surface: positive below, negative above (FVoxelSeaSurface::GetDepth's sign) */
	double Depth = NoWaterDepth;

	/** The surface's normal: the local up, tilted along a river reach */
	FVector SurfaceNormal = FVector::UpVector;

	/** A river reach's current: its direction downstream times its speed; zero for the sea and a lake */
	FVector Flow = FVector::ZeroVector;

	/** The answering body's minimum cave depth, in voxels (FVoxelWaterBody::MinCaveDepth): a lake's; 0 otherwise */
	float MinCaveDepth = 0.0f;
};

/**
 * The water levels over a box (FVoxelWaterLevels::GetRange): what a reader that copies one level for an area (a
 * chunk, a column, a map tile, a navigation region, a POI's footprint) asks. The range names the bounds it was taken
 * over, so a copy of a level always says where it holds (D112).
 */
struct FVoxelWaterLevelRange
{
	/** Some body covers a column of the box (the box itself may lie above or below the surface) */
	bool bAny = false;

	/**
	 * One level over the whole box: a single scalar is the truth there. Always so with the sea alone; false wherever a
	 * lake's or a river reach's footprint may reach the box (a reader then asks per column or per point).
	 */
	bool bUniform = true;

	/** The lowest and highest surface over the box (water levels; 0 when !bAny) */
	double MinLevel = 0.0;
	double MaxLevel = 0.0;

	/** The bounds asked */
	FBox Bounds = FBox(ForceInit);

	/** The level, for a reader that needs one scalar: valid while bUniform. */
	double GetLevel() const { return MaxLevel; }
};

/**
 * The water levels: an immutable snapshot of the bodies of water over some bounds, readable on any thread and cheap to
 * copy (the water plan's W2). The sea is one body; lakes and river reaches are the others (the hydrology plan).
 *
 * Three ways in, by what a reader does:
 *  - a point query asks Sample (the surface, the depth, the body), or its cheap forms GetDepth and IsUnder;
 *  - a reader that copied one level for an area asks GetRange over that area's bounds;
 *  - geometry drawn at the sea's own surface (the far field's sea, the far ocean, a planet's ocean shell) asks GetSea.
 * Nothing else reads the configuration's WaterLevel or WaterRadius: FromConfig is the one builder.
 *
 * Where several bodies cover a column the highest level answers (a pond inside a larger lake; a river reach where it
 * enters a lake or the sea, at that body's level). A snapshot holds over its bounds (GetBounds) and answers nothing
 * true outside them: UVoxelChunkManager::GatherWaterLevels makes one over a box, a chunk or a cap, and
 * UVoxelChunkManager::SampleWater asks at a point. GetWaterLevels hands out the sea's, which holds everywhere and is
 * for what means the sea itself. A snapshot converts from a sea, so a caller that holds one (a test, a frame-local sea
 * such as navigation's altitude plane on a planet) passes it wherever the levels are read.
 */
class VOXELCORE_API FVoxelWaterLevels
{
public:
	/** No water. */
	FVoxelWaterLevels() = default;

	/** A snapshot whose one body is this sea (none when the sea is off), holding everywhere. */
	FVoxelWaterLevels(const FVoxelSeaSurface& InSea);

	/**
	 * The water levels of a world configuration: its sea (FVoxelSeaSurface::FromConfig), in the configuration's frame.
	 * The one place the configuration's water is read.
	 *
	 * @param Config World configuration
	 * @return The snapshot
	 */
	static FVoxelWaterLevels FromConfig(const UVoxelWorldConfiguration& Config);

	/**
	 * A snapshot of the sea and the bodies gathered over some bounds (UVoxelChunkManager::GatherWaterLevels).
	 *
	 * @param InSea The sea (bEnabled false: none)
	 * @param InFrame The world's frame
	 * @param InBounds The bounds the bodies were gathered over
	 * @param InBodies The lakes and river reaches whose footprints may reach the bounds (a sea record is ignored; a body
	 *                 gathered twice is kept once; they end ordered by id)
	 * @return The snapshot
	 */
	static FVoxelWaterLevels Make(const FVoxelSeaSurface& InSea, const FVoxelWaterFrame& InFrame, const FBox& InBounds, TArray<FVoxelWaterBody> InBodies);

	/** The bounds of a body that lies everywhere, and of a range a reader takes for the whole world. */
	static FBox WholeWorld();

	// ---- The three functions ----

	/**
	 * The water over a world position: the body covering its column (the highest level, where several do), that body's
	 * surface and the position's depth under it.
	 *
	 * @param WorldPos World position
	 * @param Out The sample (bWater false and Depth NoWaterDepth where no body covers the position)
	 * @return True when a body covers the position's column
	 */
	bool Sample(const FVector& WorldPos, FVoxelWaterSample& Out) const
	{
		return bSeaAlone ? SampleSea(WorldPos, Out) : SampleBodies(WorldPos, Out);
	}

	/**
	 * The water levels over a world box, by column: the bodies that lie over or under any part of the box's footprint,
	 * whatever the box's own height.
	 *
	 * @param WorldBounds The area (WholeWorld for a reader that works on all of it)
	 * @return The range, with the bounds it was taken over
	 */
	FVoxelWaterLevelRange GetRange(const FBox& WorldBounds) const;

	/**
	 * The sea body's surface, for the geometry drawn at it and the water rules' seed tests. bEnabled false: the world
	 * has no sea.
	 */
	const FVoxelSeaSurface& GetSea() const { return Sea; }

	/** The frame the levels are measured in. */
	const FVoxelWaterFrame& GetFrame() const { return Frame; }

	// ---- The cheap forms, for readers that run per voxel or per vertex ----
	// With the sea alone each is the sea's own arithmetic, as before the hydrology plan.

	/** Whether any body exists (the sea is on, or the snapshot holds a lake or a river reach). */
	bool HasWater() const { return Bodies.Num() > 0; }

	/** Whether the sea is the snapshot's only body, or it has none: every range is then uniform. */
	bool IsSeaAlone() const { return bSeaAlone; }

	/** Whether water levels are distances from a planet centre (a planet) rather than world Z. */
	bool IsRadial() const { return Frame.bRadial; }

	/**
	 * The sample's Depth alone: a position's depth under the surface over it, NoWaterDepth where no body covers it.
	 *
	 * @param WorldPos World position
	 * @return Depth (positive below the surface)
	 */
	double GetDepth(const FVector& WorldPos) const
	{
		if (bSeaAlone)
		{
			return Sea.bEnabled ? Sea.GetDepth(WorldPos) : FVoxelWaterSample::NoWaterDepth;
		}
		return GetDepthBodies(WorldPos);
	}

	/**
	 * Whether a position is at or below the surface of the body over it (FVoxelSeaSurface::IsBelow's test, which the
	 * water rules flood by: exact where a depth's sign is not, since a sphere's is a squared distance).
	 *
	 * @param WorldPos World position
	 * @return True at or below a water surface
	 */
	bool IsUnder(const FVector& WorldPos) const
	{
		if (bSeaAlone)
		{
			return Sea.bEnabled && Sea.IsBelow(WorldPos);
		}
		return IsUnderBodies(WorldPos);
	}

	/**
	 * Whether every point of a box is above every water surface over it, so nothing in it can take water (true with
	 * no water). Conservative with a lake or a river reach: false where one may reach the box and the box reaches down
	 * to its highest level there.
	 *
	 * @param Box World-space box
	 * @return True when the box lies wholly above the water
	 */
	bool IsAbove(const FBox& Box) const
	{
		if (bSeaAlone)
		{
			return !Sea.bEnabled || Sea.IsAbove(Box);
		}
		return IsAboveBodies(Box);
	}

	/**
	 * Whether a water surface passes through a box: some of it at or below the water, some above. Conservative with a
	 * lake or a river reach: true where one's levels over the box lie within the box's heights.
	 *
	 * @param Box World-space box
	 * @return True when the box straddles a surface
	 */
	bool Crosses(const FBox& Box) const
	{
		if (bSeaAlone)
		{
			return Sea.bEnabled && Sea.Crosses(Box);
		}
		return CrossesBodies(Box);
	}

	// ---- The bodies ----

	/** The bodies in the snapshot: the sea first when it is on, then the lakes and river reaches by id. */
	TConstArrayView<FVoxelWaterBody> GetBodies() const { return Bodies; }

	/** The bounds the snapshot holds over (WholeWorld: the sea's holds everywhere). */
	const FBox& GetBounds() const { return Bounds; }

private:
	/** A body other than the sea at a column: whether its footprint covers it, and its level there. */
	struct FBodyAt
	{
		const FVoxelWaterBody* Body = nullptr;
		double Level = 0.0;
		FVoxelWaterReachSample Reach;
	};

	bool SampleSea(const FVector& WorldPos, FVoxelWaterSample& Out) const;
	bool SampleBodies(const FVector& WorldPos, FVoxelWaterSample& Out) const;
	double GetDepthBodies(const FVector& WorldPos) const;
	bool IsUnderBodies(const FVector& WorldPos) const;
	bool IsAboveBodies(const FBox& Box) const;
	bool CrossesBodies(const FBox& Box) const;

	/**
	 * The lake or river reach with the highest level over a position's column that stands over the sea's, if any.
	 * @return False when the sea (or nothing) answers there
	 */
	bool FindBodyAt(const FVector& WorldPos, FBodyAt& Out) const;

	/** The lowest and the highest height of a box in the frame's terms (its Z span, or its distances from the centre). */
	void GetBoxHeights(const FBox& Box, double& OutMin, double& OutMax) const;

	/** The sea body's surface (bEnabled false: no sea) */
	FVoxelSeaSurface Sea;

	FVoxelWaterFrame Frame;

	/** No lake and no river reach: every function is the sea's own arithmetic */
	bool bSeaAlone = true;

	/** The bodies as records; the sea's is first. One inline: copying a snapshot of the sea allocates nothing. */
	TArray<FVoxelWaterBody, TInlineAllocator<1>> Bodies;

	FBox Bounds = FBox(ForceInit);
};

/**
 * A source of water bodies, gathered by bounds: the twin of IVoxelTerrainConditioner (VoxelGeneration), implemented
 * beside it by whatever owns lakes or river reaches (the hydrology generator's network), so the owner of a polyline
 * registers its bed and its water from one place. Registered on the chunk manager
 * (UVoxelChunkManager::AddWaterBodySource) on OnInitialized, before the first generation request; a hand-placed river
 * or lake adds its own record instead (AddWaterBody).
 *
 * The rules a source keeps:
 *  - Deterministic: the same bodies for the same bounds on every machine and in every session (from the seed, or from
 *    saved, replicated data). Water is generated, not replicated.
 *  - Bodies as records (D112): each body carries its level and the bounds it holds over; a level that changes is a
 *    new record for those bounds.
 *  - A river reach's surface never rises downstream, stays under the bank the bed leaves, and ends in a body whose
 *    level is at or under its last surface level (the hydrology plan's section 5.3).
 *  - A body worked out after a Ready gather missed it is announced (UVoxelChunkManager::NotifyWaterBodiesChanged).
 */
class IVoxelWaterBodySource
{
public:
	virtual ~IVoxelWaterBodySource() = default;

	/**
	 * The bodies whose footprints reach into an XY region of a heightmap world.
	 *
	 * @param Bounds World XY region
	 * @param Need Complete: every such body, worked out here if it is not at hand. Ready: only the bodies at hand
	 * @param OutBodies Appended to, never reset
	 */
	virtual void GatherWaterBodies(const FBox2D& Bounds, EVoxelConditioningGather Need, TArray<FVoxelWaterBody>& OutBodies) const = 0;

	/**
	 * The bodies whose footprints reach into a cap of a planet's surface. Defaults to none.
	 *
	 * @param Direction Unit direction from the planet centre to the cap's centre
	 * @param AngularRadius The cap's half angle (radians)
	 * @param Need Complete or Ready, as GatherWaterBodies
	 * @param OutBodies Appended to, never reset
	 */
	virtual void GatherWaterBodiesOnSphere(const FVector& Direction, double AngularRadius, EVoxelConditioningGather Need,
		TArray<FVoxelWaterBody>& OutBodies) const {}
};
