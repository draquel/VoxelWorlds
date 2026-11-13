// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelSeaSurface.h"

class UVoxelWorldConfiguration;

/** What a body of water is. The water plan's W2 has the sea alone; lakes and reaches come with the hydrology plan. */
enum class EVoxelWaterBodyKind : uint8
{
	/** The one sea: a Z plane, or a sphere on a planet (FVoxelSeaSurface) */
	Sea,
	/** A flat body above the sea, at its spill height (W3; nothing makes one yet) */
	Lake,
	/** A sloped body along a polyline (W4; nothing makes one yet) */
	Reach
};

/**
 * One vertex of a reach's polyline (the water plan's section 7): where the river is, how deep its bed is carved and
 * where its surface lies. Declared for the spline work to plan against; nothing makes or evaluates a reach yet.
 */
struct FVoxelWaterReachVertex
{
	/** The vertex: world (X, Y) on a heightmap world (Z unused), the unit direction from the planet centre on a planet */
	FVector Position = FVector::ZeroVector;

	/** The bed's water level (the terms of FVoxelWaterBody::Level): what the corridor carves down to */
	double BedLevel = 0.0;

	/** The surface's water level here, interpolated along the polyline; it never rises downstream */
	double SurfaceLevel = 0.0;

	/** Half the reach's width (world units along the surface) */
	double HalfWidth = 0.0;
};

/** A reach's polyline, upstream to downstream: shared and immutable, as a conditioning line is (FVoxelConditioningLine). */
struct FVoxelWaterReach
{
	TArray<FVoxelWaterReachVertex> Vertices;
};

/**
 * One body of water, as data (the water plan's D112: a consumer holds a body's record and never a level it has baked
 * in, so a body's level could change later by replacing the record). The water plan's W2 has one, the sea.
 *
 * A water level is a surface's height in the world's own vertical measure: world Z on a heightmap world, the distance
 * from the planet centre on a planet (so the sea's is WaterLevel or WaterRadius). It is not an altitude: an altitude is
 * measured from the world mode's datum (IVoxelWorldMode::GetAltitude).
 */
struct VOXELCORE_API FVoxelWaterBody
{
	/** The sea's id. Every other body's id is its source's, stable for the same bounds on every machine. */
	static constexpr uint32 SeaId = 0;

	uint32 Id = SeaId;

	EVoxelWaterBodyKind Kind = EVoxelWaterBodyKind::Sea;

	/** The surface's water level; a reach's highest (its vertices carry the rest) */
	double Level = 0.0;

	/** Where the record holds: the body lies inside these bounds, and a change to it invalidates them (D112) */
	FBox Bounds = FBox(ForceInit);

	/** Kind Reach: the polyline (null otherwise) */
	TSharedPtr<const FVoxelWaterReach> Reach;
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

	/** The surface's normal: the local up; tilted along a reach (W4) */
	FVector SurfaceNormal = FVector::UpVector;

	/** A reach's current (W4); zero for the sea and a lake */
	FVector Flow = FVector::ZeroVector;
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

	/** One level over the whole box: a single scalar is the truth there. Always so with the sea alone. */
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
 * The world's water levels: an immutable snapshot of the bodies of water, readable on any thread and cheap to copy
 * (the water plan's W2). The one sea is its only body until the hydrology plan adds lakes and reaches.
 *
 * Three ways in, by what a reader does:
 *  - a point query asks Sample (the surface, the depth, the body), or its cheap forms GetDepth and IsUnder;
 *  - a reader that copied one level for an area asks GetRange over that area's bounds;
 *  - geometry drawn at the sea's own surface (the far field's sea, the far ocean, a planet's ocean shell) asks GetSea.
 * Nothing else reads the configuration's WaterLevel or WaterRadius: FromConfig is the one builder.
 *
 * UVoxelChunkManager::GetWaterLevels hands out the world's. A snapshot converts from a sea, so a caller that holds
 * one (a test, a frame-local sea such as navigation's altitude plane on a planet) passes it wherever the levels are
 * read.
 */
class VOXELCORE_API FVoxelWaterLevels
{
public:
	/** No water. */
	FVoxelWaterLevels() = default;

	/** A snapshot whose one body is this sea (none when the sea is off). */
	FVoxelWaterLevels(const FVoxelSeaSurface& InSea);

	/**
	 * The water levels of a world configuration: its sea (FVoxelSeaSurface::FromConfig). The one place the
	 * configuration's water is read.
	 *
	 * @param Config World configuration
	 * @return The snapshot
	 */
	static FVoxelWaterLevels FromConfig(const UVoxelWorldConfiguration& Config);

	/** The bounds of a body that lies everywhere, and of a range a reader takes for the whole world. */
	static FBox WholeWorld();

	// ---- The three functions ----

	/**
	 * The water over a world position: the body covering its column, that body's surface and the position's depth
	 * under it.
	 *
	 * @param WorldPos World position
	 * @param Out The sample (bWater false and Depth NoWaterDepth where no body covers the position)
	 * @return True when a body covers the position's column
	 */
	bool Sample(const FVector& WorldPos, FVoxelWaterSample& Out) const;

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

	// ---- The cheap forms, for readers that run per voxel or per vertex ----

	/** Whether any body exists (the sea is on). */
	bool HasWater() const { return Sea.bEnabled; }

	/** Whether water levels are distances from a planet centre (a planet) rather than world Z. */
	bool IsRadial() const { return Sea.bRadial; }

	/**
	 * The sample's Depth alone: a position's depth under the surface over it, NoWaterDepth where no body covers it.
	 *
	 * @param WorldPos World position
	 * @return Depth (positive below the surface)
	 */
	double GetDepth(const FVector& WorldPos) const
	{
		return Sea.bEnabled ? Sea.GetDepth(WorldPos) : FVoxelWaterSample::NoWaterDepth;
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
		return Sea.bEnabled && Sea.IsBelow(WorldPos);
	}

	/**
	 * Whether every point of a box is above every water surface over it, so nothing in it can take water (true with
	 * no water).
	 *
	 * @param Box World-space box
	 * @return True when the box lies wholly above the water
	 */
	bool IsAbove(const FBox& Box) const
	{
		return !Sea.bEnabled || Sea.IsAbove(Box);
	}

	/**
	 * Whether a water surface passes through a box: some of it at or below the water, some above.
	 *
	 * @param Box World-space box
	 * @return True when the box straddles a surface
	 */
	bool Crosses(const FBox& Box) const
	{
		return Sea.bEnabled && Sea.Crosses(Box);
	}

	// ---- The bodies ----

	/** The bodies in the snapshot: the sea when it is on. */
	TConstArrayView<FVoxelWaterBody> GetBodies() const { return Bodies; }

	/** The bounds the snapshot was taken over (WholeWorld: the chunk manager's holds everywhere). */
	const FBox& GetBounds() const { return Bounds; }

private:
	/** The sea body's surface (bEnabled false: no sea) */
	FVoxelSeaSurface Sea;

	/** The bodies as records; the sea's is first. One inline: copying a snapshot of the sea allocates nothing. */
	TArray<FVoxelWaterBody, TInlineAllocator<1>> Bodies;

	FBox Bounds = FBox(ForceInit);
};

/**
 * A source of water bodies, gathered by bounds: the twin of IVoxelTerrainConditioner (VoxelGeneration), implemented
 * beside it by whatever owns lakes or reaches (the hydrology plan's depression fill, the POI layer's rivers, a
 * hand-placed spline), so the owner of a polyline registers its carve and its water from one place.
 *
 * Declared by the water plan's W2 for the spline work to plan against: nothing implements it and nothing gathers
 * from one yet. The rules a source keeps:
 *  - Deterministic: the same bodies for the same bounds on every machine and in every session (from the seed, or from
 *    saved, replicated data). Water is generated, not replicated.
 *  - Bodies as records (D112): each body carries its level and the bounds it holds over; a level that changes is a
 *    new record for those bounds.
 *  - A reach's surface never rises downstream, stays under the bank the carve leaves, and ends in a body whose level
 *    is at or under its last surface level.
 */
class IVoxelWaterBodySource
{
public:
	virtual ~IVoxelWaterBodySource() = default;

	/**
	 * The bodies that reach into an XY region of a heightmap world.
	 *
	 * @param Bounds World XY region
	 * @param OutBodies Appended to, never reset
	 */
	virtual void GatherWaterBodies(const FBox2D& Bounds, TArray<FVoxelWaterBody>& OutBodies) const = 0;

	/**
	 * The bodies that reach into a cap of a planet's surface. Defaults to none.
	 *
	 * @param Direction Unit direction from the planet centre to the cap's centre
	 * @param AngularRadius The cap's half angle (radians)
	 * @param OutBodies Appended to, never reset
	 */
	virtual void GatherWaterBodiesOnSphere(const FVector& Direction, double AngularRadius, TArray<FVoxelWaterBody>& OutBodies) const {}
};
