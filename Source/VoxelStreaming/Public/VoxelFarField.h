// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelCoreTypes.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelSeaSurface.h"
#include "VoxelTerrainConditioning.h"

class IVoxelWorldMode;
class FSphericalPlanetWorldMode;
class UVoxelWorldConfiguration;

/**
 * A far-field patch's address (spherical Phase 6, WM-BP9): a node of one cube face's quadtree on the planet's surface
 * coordinate (FVoxelCubeSphere: the face's warped U and V in [-1, 1]). Level 0 is the whole face; each level halves the
 * patch along U and V.
 *
 * On a heightmap world (world modes review WM-MP1, D36) Face is HeightmapFace and the key is a node of one quadtree over
 * the XY plane whose level-0 cells are root tiles of FVoxelFarFieldSettings::RootChunks chunks, aligned to the chunk grid:
 * X and Y are the cell's signed indices at Level (the tile's minimum corner at GridOrigin + (X, Y) x the level's cell
 * size), so a root tile is (0, X, Y) and the plane extends either way. Children and parents are the same arithmetic.
 */
struct VOXELSTREAMING_API FVoxelFarFieldPatchKey
{
	/** Cube face 0..5 (+X, -X, +Y, -Y, +Z, -Z); HeightmapFace on a heightmap world */
	int32 Face = 0;

	/** Quadtree level, 0 = the whole face (a root tile on a heightmap world) */
	int32 Level = 0;

	/** Column along the face's U, 0 .. 2^Level - 1 (signed cell index on a heightmap world) */
	int32 X = 0;

	/** Row along the face's V, 0 .. 2^Level - 1 (signed cell index on a heightmap world) */
	int32 Y = 0;

	/** The Face of a heightmap world's keys */
	static constexpr int32 HeightmapFace = 7;

	/** Heightmap keys pack X and Y offset by this (28 unsigned bits hold cells -2^27 .. 2^27 - 1) */
	static constexpr int32 HeightmapBias = 1 << 27;

	FVoxelFarFieldPatchKey() = default;
	FVoxelFarFieldPatchKey(int32 InFace, int32 InLevel, int32 InX, int32 InY)
		: Face(InFace), Level(InLevel), X(InX), Y(InY)
	{
	}

	bool IsHeightmap() const { return Face == HeightmapFace; }

	/** Packed for maps: face 3 bits, level 5 bits, X and Y 28 bits each (levels up to 28; heightmap cells biased). */
	uint64 Pack() const
	{
		const int32 Bias = IsHeightmap() ? HeightmapBias : 0;
		return (static_cast<uint64>(Face & 7) << 61) | (static_cast<uint64>(Level & 31) << 56)
			| (static_cast<uint64>((X + Bias) & 0x0FFFFFFF) << 28) | static_cast<uint64>((Y + Bias) & 0x0FFFFFFF);
	}

	/** The key a Pack() came from. */
	static FVoxelFarFieldPatchKey Unpack(uint64 Packed)
	{
		const int32 Face = static_cast<int32>((Packed >> 61) & 7);
		const int32 Bias = Face == HeightmapFace ? HeightmapBias : 0;
		return FVoxelFarFieldPatchKey(Face, static_cast<int32>((Packed >> 56) & 31),
			static_cast<int32>((Packed >> 28) & 0x0FFFFFFF) - Bias, static_cast<int32>(Packed & 0x0FFFFFFF) - Bias);
	}

	/** Heightmap worlds: the cell's edge at this level, from the root tile's. */
	double GetTileSize(double RootTileSize) const { return RootTileSize / static_cast<double>(1ll << FMath::Clamp(Level, 0, 40)); }

	/**
	 * Heightmap worlds: a world XY at a fraction of the patch: (0, 0) its minimum corner, (1, 1) the maximum. Fractions
	 * outside [0, 1] continue past it.
	 *
	 * @param GridOrigin The chunk grid's origin (its XY)
	 * @param RootTileSize A root tile's edge (RootChunks x ChunkWorldSize)
	 * @param FU Fraction along X
	 * @param FV Fraction along Y
	 * @return World (X, Y)
	 */
	FVector2D GetPoint2D(const FVector& GridOrigin, double RootTileSize, double FU, double FV) const
	{
		const double Size = GetTileSize(RootTileSize);
		return FVector2D(GridOrigin.X + (static_cast<double>(X) + FU) * Size, GridOrigin.Y + (static_cast<double>(Y) + FV) * Size);
	}

	/** Heightmap worlds: the patch's footprint. */
	FBox2D GetBox2D(const FVector& GridOrigin, double RootTileSize) const
	{
		return FBox2D(GetPoint2D(GridOrigin, RootTileSize, 0.0, 0.0), GetPoint2D(GridOrigin, RootTileSize, 1.0, 1.0));
	}

	/** Child 0..3: (2X + (Index & 1), 2Y + (Index >> 1)) one level down. */
	FVoxelFarFieldPatchKey GetChild(int32 Index) const
	{
		return FVoxelFarFieldPatchKey(Face, Level + 1, X * 2 + (Index & 1), Y * 2 + (Index >> 1));
	}

	/** The patch one level up (the root's parent is the root). */
	FVoxelFarFieldPatchKey GetParent() const
	{
		return Level > 0 ? FVoxelFarFieldPatchKey(Face, Level - 1, X >> 1, Y >> 1) : *this;
	}

	/** The patch's extent in warped UV: 2 / 2^Level. */
	double GetUVSize() const { return 2.0 / static_cast<double>(1ll << Level); }

	/** The patch's lowest U and V corner. */
	FVector2D GetUVMin() const { return FVector2D(-1.0 + X * GetUVSize(), -1.0 + Y * GetUVSize()); }

	/**
	 * The unit direction at a fraction of the patch: (0, 0) its lowest corner, (1, 1) the highest. Fractions outside
	 * [0, 1] continue past it, across the face's edge too (the face's plane extended).
	 *
	 * @param FU Fraction along U
	 * @param FV Fraction along V
	 * @return Unit direction from the planet centre
	 */
	FVector GetDirection(double FU, double FV) const;

	bool operator==(const FVoxelFarFieldPatchKey& Other) const
	{
		return Face == Other.Face && Level == Other.Level && X == Other.X && Y == Other.Y;
	}

	FString ToString() const { return FString::Printf(TEXT("%s%d L%d (%d,%d)"), IsHeightmap() ? TEXT("h") : TEXT("f"), Face, Level, X, Y); }
};

/**
 * What the far field draws and how finely (built by the chunk manager from the world configuration:
 * UVoxelChunkManager::RebuildFarField).
 */
struct VOXELSTREAMING_API FVoxelFarFieldSettings
{
	/** The planet centre */
	FVector Center = FVector::ZeroVector;

	/** The datum radius (PlanetRadius) */
	double Radius = 0.0;

	/** The chunk grid's origin (WorldOrigin): which voxel chunk holds a point, for the hole */
	FVector GridOrigin = FVector::ZeroVector;

	/** A chunk's edge (ChunkSize x VoxelSize) */
	double ChunkWorldSize = 6400.0;

	/** Voxel size: the sink, and the sea's UVs (voxels of arc, as the sea sphere's) */
	double VoxelSize = 100.0;

	/** Quads along a patch's edge (PatchQuads + 1 vertices) */
	int32 PatchQuads = 16;

	/**
	 * How far under the analytic surface the far field is drawn, along local down: about a voxel, so wherever a voxel
	 * chunk is drawn over a patch the voxel surface hides it without z-fighting (the band at the voxel window's edge).
	 * The same for every patch, so neighbouring levels meet.
	 */
	double Sink = 100.0;

	/** The terrain's displacement bounds over the whole planet (horizon culling; unbuilt patches) */
	float MinAltitude = 0.0f;
	float MaxAltitude = 0.0f;

	/** The far field draws the sea (patches at sea level replace the sea sphere: ShouldDrawSea) */
	bool bSea = false;

	/** The sea radius (WaterRadius) */
	double SeaRadius = 0.0;

	/** A sea cell is drawn where the terrain under it is within this of the sea or below it (uu) */
	double SeaMargin = 200.0;

	/**
	 * Sea cells along a patch's edge: the sea is a smooth sphere, so a grid coarser than the terrain's sags under a uu
	 * where it is close (an edge patch's cell on a 1,000 km planet: 0.002 uu; on the small planet: 0.3 uu)
	 */
	static constexpr int32 SeaQuads = 8;

	/** The finest level: patches between one and two chunks across (ComputeEdgeLevel) */
	int32 EdgeLevel = 0;

	/**
	 * Levels of coarser neighbour a patch's skirts cover: a patch drops a skirt from each edge as deep as the gap to the
	 * same edge drawn by a patch up to this many levels coarser.
	 */
	int32 SkirtLevels = 3;

	/**
	 * Patches wider than this take no conditioning zones: a POI pad is a few chunks across, so at their resolution it does
	 * not show, and a cap of a face would ask the POI system for every POI on it. On a planet a cap's angular radius
	 * (radians); on a heightmap world a footprint's edge (uu).
	 */
	double ZoneGatherMax = 0.0;

	/**
	 * The finest level for a planet: the deepest at which a patch is still at least a chunk across along a face's centre
	 * lines (pi / 2 x Radius / 2^Level >= ChunkWorldSize), so edge patches are one to two chunks across.
	 *
	 * @param Radius Datum radius
	 * @param ChunkWorldSize A chunk's edge
	 * @return Edge level (0 for a planet smaller than a few chunks)
	 */
	static int32 ComputeEdgeLevel(double Radius, double ChunkWorldSize);

	// ---- Heightmap worlds (world modes review WM-MP1, D36) ----

	/**
	 * A heightmap world (InfinitePlane, IslandBowl): one quadtree over the XY plane whose roots are tiles of RootChunks
	 * chunks aligned to the chunk grid, the surface the generated height (Z), local up +Z, the sea the WaterLevel plane.
	 * Center, Radius and SeaRadius are unused. Read through FVoxelFarFieldFrame: the far field itself never branches on it.
	 */
	bool bHeightmap = false;

	/** Heightmap worlds: chunks along a root tile's edge, a power of two (the edge level is its log2: edge patches one chunk) */
	int32 RootChunks = 64;

	/**
	 * Heightmap worlds: roots exist within this of the camera's ground point (uu) and patches farther are culled; on a
	 * bounded world (an island) also only over its bounds. 0 = no limit (every root over the bounds)
	 */
	double FarDistance = 0.0;

	/** Heightmap worlds: the world's bounds (IVoxelWorldMode::GetWorldBounds): an island's footprint, Unbounded on a plane */
	FVoxelWorldBounds Bounds;

	/** Heightmap worlds: the sea's world Z (WaterLevel), where the water tiles lie */
	double SeaZ = 0.0;

	/** Heightmap worlds: a root tile's edge (RootChunks x ChunkWorldSize). */
	double GetRootTileSize() const { return static_cast<double>(FMath::Max(RootChunks, 1)) * ChunkWorldSize; }

	/**
	 * Heightmap worlds: the finest level, at which a patch is one chunk across: log2(RootChunks).
	 *
	 * @param InRootChunks Chunks along a root tile's edge (a power of two; rounded down otherwise)
	 * @return Edge level
	 */
	static int32 ComputeHeightmapEdgeLevel(int32 InRootChunks);

	/**
	 * Heightmap worlds: the root tile size for a far reach (voxel.World.FarFieldRootChunks 0): about a quarter of the
	 * reach in chunks, rounded to a power of two from 4 to 4,096, so a plane keeps a few rows of roots about the camera
	 * whatever its chunk size (64 with 6,400 uu chunks and a 2,000,000 uu reach, 256 with 1,600 uu chunks).
	 *
	 * @param Reach The far distance, or a bounded world's half extent
	 * @param InChunkWorldSize A chunk's edge
	 * @return Chunks along a root tile's edge
	 */
	static int32 ComputeHeightmapRootChunks(double Reach, double InChunkWorldSize);

	/**
	 * A patch's edge at a level: on the datum sphere along a face's centre lines, pi / 2 x Radius / 2^Level; on a heightmap
	 * world the root tile's edge / 2^Level.
	 */
	double GetPatchArc(int32 Level) const
	{
		const double Root = bHeightmap ? GetRootTileSize() : UE_DOUBLE_HALF_PI * Radius;
		return Root / static_cast<double>(1ll << FMath::Clamp(Level, 0, 40));
	}
};

/**
 * A point of the far field's surface domain (code review CR2, STREAM-18): on a planet a unit direction from the centre, on a
 * heightmap world a world (X, Y) with local up +Z. Direction is local up in both (the radial direction; +Z on a heightmap
 * world), so a vertex's up, tangent and face type read it alike.
 */
struct FVoxelFarFieldSample
{
	/** Local up at the sample (the radial direction; +Z on a heightmap world) */
	FVector Direction = FVector::UpVector;

	/** Heightmap worlds: the sample's world (X, Y) */
	FVector2D XY = FVector2D::ZeroVector;

	static FVoxelFarFieldSample OfDirection(const FVector& InDirection)
	{
		FVoxelFarFieldSample Sample;
		Sample.Direction = InDirection;
		return Sample;
	}

	static FVoxelFarFieldSample OfXY(const FVector2D& InXY)
	{
		FVoxelFarFieldSample Sample;
		Sample.XY = InXY;
		return Sample;
	}
};

/** A region of the surface domain: a cap (centre direction and angular radius) on a planet, a world XY box on a heightmap world. */
struct FVoxelFarFieldRegion
{
	/** A cap (else a box) */
	bool bCap = false;
	FVector Direction = FVector::UpVector;
	double AngularRadius = 0.0;
	FBox2D Box = FBox2D(ForceInit);

	bool IsValid() const { return bCap || Box.bIsValid; }
};

/** A patch's extent on the surface domain: its cap on a planet, its footprint box (world XY) on a heightmap world. */
struct FVoxelFarFieldFootprint
{
	/** The patch's cap on the sphere: its centre direction and the largest angle from it to a corner */
	FVector CapDirection = FVector::UpVector;
	double CapAngle = 0.0;

	/** Heightmap worlds: the patch's footprint (world XY) */
	FBox2D Box = FBox2D(ForceInit);
};

/**
 * The far field's surface frame (code review CR2, STREAM-18): everything that depends on the kind of surface the far
 * field draws, a planet's sphere or a heightmap world's plane, behind one set of questions, so the far field itself never
 * tests the world mode. Samples (a point of the domain from a patch key and fractions, or from a world point), the world
 * point of a sample at an altitude and the drawn point's error against the surface, the sea's points and UVs, a patch's
 * footprint and its distances along the ground and to the camera, the zone regions, culling, the root keys and the key
 * holding a sample. Every expression is the one its mode had: the two forms are kept side by side in each method, so the
 * far field's geometry is bit-identical to the branches it replaces. Built from the settings; a value type.
 */
struct VOXELSTREAMING_API FVoxelFarFieldFrame
{
	FVoxelFarFieldFrame() = default;
	explicit FVoxelFarFieldFrame(const FVoxelFarFieldSettings& InSettings) : Settings(InSettings) {}

	const FVoxelFarFieldSettings& GetSettings() const { return Settings; }

	/** A heightmap world's frame (the plane); false on a planet (the sphere). */
	bool IsHeightmap() const { return Settings.bHeightmap; }

	// ---- Samples ----

	/** The sample at a fraction of a patch ((0, 0) its lowest corner, (1, 1) the highest; fractions outside [0, 1] continue past it). */
	FVoxelFarFieldSample SampleOf(const FVoxelFarFieldPatchKey& Key, double FU, double FV) const;

	/**
	 * The sample a world point lies over: its direction from the centre (normalized unless bNormalize is false: a lookup
	 * normalizes once itself), or its (X, Y).
	 */
	FVoxelFarFieldSample SampleAt(const FVector& WorldPoint, bool bNormalize = true) const;

	/** The sample with its direction normalized (a lookup's); a heightmap sample as it is. */
	FVoxelFarFieldSample Normalized(const FVoxelFarFieldSample& Sample) const;

	/**
	 * The world point of a sample at an altitude over the datum (a heightmap world's altitude is its world Z), Sink under it:
	 * centre + direction x (radius + altitude - sink), or (X, Y, altitude - sink).
	 */
	FVector PointAt(const FVoxelFarFieldSample& Sample, double Altitude, double Sink = 0.0) const;

	/** How far a drawn point lies from the drawn surface (altitude - sink) along local up, unsigned. */
	double ErrorAt(const FVector& DrawnPoint, double Altitude, double Sink) const;

	/**
	 * A patch's origin: on the datum sphere under the patch's centre (its vertices are offsets of its own size), on a
	 * heightmap world the surface at the patch's centre.
	 */
	FVector OriginOf(const FVoxelFarFieldPatchKey& Key, const struct FVoxelFarFieldSurface& Surface, const TArray<FVoxelConditioningZone>& Zones) const;

	/** The face tangent a patch's vertices take their tangent from (the cube face's T; forward on a heightmap world). */
	FVector GetFaceTangent(const FVoxelFarFieldPatchKey& Key) const;

	// ---- The sea ----

	/** Whether the far field draws a sea (bSea, and on a planet a sea radius). */
	bool HasSea() const;

	/** The sea's altitude over the datum (SeaRadius - Radius; SeaZ on a heightmap world). */
	double GetSeaAltitude() const;

	/** The sea's point at a sample: centre + direction x SeaRadius, or (X, Y, SeaZ). */
	FVector SeaPointAt(const FVoxelFarFieldSample& Sample) const;

	/** How far a sea quad's drawn middle sags under the sea's sphere (0 on a plane). */
	double SeaSagAt(const FVector& DrawnMiddle) const;

	/** The sea's UV0 over a patch: voxels of arc along the face as the sea sphere's, or world X and Y over the voxel size as the tiles'. */
	struct FSeaUV
	{
		bool bHeightmap = false;
		double VoxelDiv = 1.0;
		FVector2D UVMin = FVector2D::ZeroVector;
		double UVSize = 0.0;
		double Scale = 0.0;
		FVector2D Offset = FVector2D::ZeroVector;

		/** The UV0 of the grid vertex (I, J) of a Q-quad patch at a sample. */
		FVector2f At(const FVoxelFarFieldSample& Sample, int32 I, int32 J, int32 Q) const;
	};
	FSeaUV MakeSeaUV(const FVoxelFarFieldPatchKey& Key) const;

	/** Whether a camera is under the sea (inside the sea sphere; under SeaZ). */
	bool IsUnderSea(const FVector& Camera) const;

	// ---- Footprints and regions ----

	/** A patch's footprint: its cap (the centre direction and the largest angle to a corner) or its box. */
	FVoxelFarFieldFootprint MakeFootprint(const FVoxelFarFieldPatchKey& Key) const;

	/** The region a footprint covers (for the zone gatherer). */
	FVoxelFarFieldRegion RegionOf(const FVoxelFarFieldFootprint& Footprint) const;

	/** The region a conditioning zone reaches (its inner radius plus its falloff): invalid when the zone's kind is not this frame's. */
	FVoxelFarFieldRegion RegionOfZone(const FVoxelConditioningZone& Zone) const;

	/** Whether a footprint meets a region. */
	bool Intersects(const FVoxelFarFieldFootprint& Footprint, const FVoxelFarFieldRegion& Region) const;

	/** Whether a patch is narrow enough to take conditioning zones (ZoneGatherMax). */
	bool TakesZones(const FVoxelFarFieldFootprint& Footprint) const;

	/** Heightmap worlds: the chunk column (chunk X, Y) under a footprint's centre. False on a planet. */
	bool GetColumn(const FVoxelFarFieldFootprint& Footprint, FIntPoint& OutColumn) const;

	// ---- Distances ----

	/** The viewer's ground point as the distances along the ground measure from it. */
	struct FGround
	{
		FVector Direction = FVector::UpVector;
		double Radius = 0.0;
		FVector2D XY = FVector2D::ZeroVector;
	};

	/** The ground point under a viewer: its direction (up when at the centre) and the ground radius, or its (X, Y). */
	FGround GroundOf(const FVector& Viewer, double GroundRadius) const;

	/** The nearest and farthest distance along the ground from a ground point to a footprint (0 inside it). */
	double GroundDistanceMin(const FGround& Ground, const FVoxelFarFieldFootprint& Footprint) const;
	double GroundDistanceMax(const FGround& Ground, const FVoxelFarFieldFootprint& Footprint) const;

	/** The camera's distance to the nearest point of a footprint's datum (0 inside it), the build order's. */
	double CameraDistanceMin(const FVector& Camera, const FVoxelFarFieldFootprint& Footprint) const;

	/**
	 * Whether nothing of a patch can be seen from a camera: wholly under the horizon (the lowest terrain the occluder, the
	 * patch's highest top kept in view past it), or on a plane past the far distance and, on a bounded world, wholly outside
	 * its box.
	 *
	 * @param Camera The camera
	 * @param Footprint The patch's footprint
	 * @param MaxAltitude The highest the patch draws (its own when built, the world's otherwise)
	 */
	bool IsCulled(const FVector& Camera, const FVoxelFarFieldFootprint& Footprint, float MaxAltitude) const;

	// ---- Keys ----

	/**
	 * The root patches wanted for a camera: a planet's six faces; a heightmap world's root tiles overlapping the square of
	 * FarDistance about the camera (and a bounded world's box), nearest rows first and at most 64 a side.
	 * @return False when the frame has no roots (a root tile of no size)
	 */
	bool GetRootKeys(const FVector& Camera, TArray<FVoxelFarFieldPatchKey>& OutKeys) const;

	/** The root patch holding a sample (its face; its root tile). False when the frame has no roots. */
	bool RootKeyOf(const FVoxelFarFieldSample& Sample, FVoxelFarFieldPatchKey& OutKey) const;

	/** The child of a patch holding a sample. */
	FVoxelFarFieldPatchKey ChildKeyOf(const FVoxelFarFieldPatchKey& Key, const FVoxelFarFieldSample& Sample) const;

private:
	FVoxelFarFieldSettings Settings;
};

/**
 * The analytic surface the far field samples: the planet's terrain altitude (FSphericalPlanetWorldMode, conditioning
 * zones applied as generation applies them) and its surface material and biome
 * (FVoxelSurfaceQuery::QueryPlanetSurfaceConditionsAt); on a heightmap world (WM-MP1) the generated height at a world
 * (X, Y) (IVoxelWorldMode::GetTerrainHeightAt with the zones, FVoxelTerrainConditioning::ApplyToHeight; the block top of
 * it in a cubic world, FVoxelBlockSurface) and FVoxelSurfaceQuery::QuerySurfaceConditions. Immutable once made; read from
 * build tasks on any thread.
 */
struct VOXELSTREAMING_API FVoxelFarFieldSurface
{
	/** The world mode: a planet (an FSphericalPlanetWorldMode; its biome snapshot carries continentalness) or a heightmap one */
	TSharedPtr<const IVoxelWorldMode> WorldMode;

	/** Terrain noise */
	FVoxelNoiseParams NoiseParams;

	/** Biomes for the surface material; invalid without biomes (the material then comes from GetMaterialAtDepth) */
	FVoxelBiomeSnapshot Biomes;

	/** The sea (the material's underwater variant) */
	FVoxelSeaSurface Sea;

	/** Voxel size (generation's depth in voxels) */
	float VoxelSize = 100.0f;

	/** A heightmap world (WorldMode is InfinitePlane or IslandBowl) */
	bool bHeightmap = false;

	/** Heightmap worlds: the terrain noise's seed (the climate seeds derive from it), and the sea as the material rules read it */
	int32 WorldSeed = 0;
	bool bWater = false;
	float WaterLevel = 0.0f;

	/** Heightmap worlds: a cubic world whose surface is block tops (FVoxelBlockSurface::UsesBlockSurface), and the grid's origin */
	bool bBlockSurface = false;
	FVector WorldOrigin = FVector::ZeroVector;

	/**
	 * The surface of a world configuration's planet or heightmap world.
	 *
	 * @param Config World configuration
	 * @param InWorldMode The configuration's world mode (FVoxelWorldModeFactory::Create)
	 * @return The surface, or null without a world mode
	 */
	static TSharedPtr<const FVoxelFarFieldSurface> Make(const UVoxelWorldConfiguration& Config, const TSharedPtr<IVoxelWorldMode>& InWorldMode);

	/** The planet world mode (planets only). */
	const FSphericalPlanetWorldMode& GetPlanet() const;

	/**
	 * The terrain's altitude over the datum at a sample, zones applied: along a direction the radial displacement
	 * (GetConditionedTerrainAltitude); at a world (X, Y) the generated height (its world Z), the block top in a cubic world.
	 * The one sample triple of both kinds of surface (code review CR2, STREAM-18).
	 *
	 * @param Sample The sample
	 * @param Zones Conditioning zones there (may be empty)
	 * @param OutContinentalness Optional: the continentalness sampled on a planet (the material's biome blend reuses it); 0 on a heightmap world
	 * @return Altitude (uu)
	 */
	float SampleAltitude(const FVoxelFarFieldSample& Sample, const TArray<FVoxelConditioningZone>& Zones, float* OutContinentalness = nullptr) const;

	/**
	 * The surface material and biome where the terrain is at Altitude at a sample (depth 0), as generation writes them.
	 *
	 * @param Sample The sample
	 * @param Altitude The terrain's altitude there (SampleAltitude)
	 * @param Continentalness SampleAltitude's continentalness
	 * @param OutMaterial Surface material ID
	 * @param OutBiome Dominant biome ID
	 */
	void SampleConditions(const FVoxelFarFieldSample& Sample, float Altitude, float Continentalness, uint8& OutMaterial, uint8& OutBiome) const;

	/**
	 * The surface material mix where the terrain is at Altitude at a sample (G45): its two leading materials and the
	 * second's share (FVoxelSurfaceQuery::QueryPlanetSurfaceMaterialMixAt / QuerySurfaceMaterialMix), what the voxels'
	 * per-voxel picks cover there on average. Without biomes: generation's material by depth, alone.
	 *
	 * @param Sample The sample
	 * @param Altitude The terrain's altitude there (SampleAltitude)
	 * @param Continentalness SampleAltitude's continentalness
	 * @return The mix
	 */
	FVoxelSurfaceMaterialMix SampleMaterialMix(const FVoxelFarFieldSample& Sample, float Altitude, float Continentalness) const;

	/** Whether far-field quads blend their corners' two leading materials (voxel.World.FarFieldMaterialBlend, G45). */
	static bool IsMaterialBlendEnabled();

private:
	// The two kinds' halves of the triple above.
	float SampleHeight(double WorldX, double WorldY, const TArray<FVoxelConditioningZone>& Zones) const;
	void SampleConditionsXY(double WorldX, double WorldY, float Height, uint8& OutMaterial, uint8& OutBiome) const;
	FVoxelSurfaceMaterialMix SampleMaterialMixXY(double WorldX, double WorldY, float Height) const;
	float SampleAltitude(const FVector& Direction, const TArray<FVoxelConditioningZone>& Zones, float* OutContinentalness) const;
	void SampleConditions(const FVector& Direction, float Altitude, float Continentalness, uint8& OutMaterial, uint8& OutBiome) const;
	FVoxelSurfaceMaterialMix SampleMaterialMix(const FVector& Direction, float Altitude, float Continentalness) const;
};

struct VOXELSTREAMING_API FVoxelFarFieldSection
{
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector3f> Tangents;
	TArray<FVector2f> UV0;
	TArray<FVector2f> UV1;
	TArray<FColor> Colors;
	TArray<int32> Triangles;

	void Reset()
	{
		Positions.Reset();
		Normals.Reset();
		Tangents.Reset();
		UV0.Reset();
		UV1.Reset();
		Colors.Reset();
		Triangles.Reset();
	}

	bool IsEmpty() const { return Positions.Num() == 0 || Triangles.Num() == 0; }

	/** Memory the arrays hold (bytes). */
	SIZE_T GetAllocatedSize() const
	{
		return Positions.GetAllocatedSize() + Normals.GetAllocatedSize() + Tangents.GetAllocatedSize() + UV0.GetAllocatedSize()
			+ UV1.GetAllocatedSize() + Colors.GetAllocatedSize() + Triangles.GetAllocatedSize();
	}
};

/**
 * A built patch: its meshes, relative to its origin, and what the selection needs to know about it.
 *
 * Vertex attributes match the voxel terrain's (FChunkMeshData from the CPU meshers): UV1 = (material id, 0 = top face),
 * colour G the biome id. Each quad has one material and biome, the same at its four corners, and a grid vertex is
 * duplicated where quads of different materials meet, since the terrain material reads the id from the interpolated UV1.
 * G45 (voxel.World.FarFieldMaterialBlend): a quad also carries a second material, colour R, the same at its corners, and
 * each corner the second's weight there as 1 - colour A; the terrain material takes the second where a temporal dither
 * falls under the interpolated weight (M_VoxelMaster's FarFieldMaterialPick node), so a border or a blend zone draws as a
 * gradient across the quad. The quad's materials are the two largest shares of its corners' mixes
 * (FVoxelFarFieldSurface::SampleMaterialMix); voxel chunks carry alpha 1 and keep their one material. Without the blend
 * each quad takes the most common material and biome of its corners, colour R that material and alpha 1. The sea's
 * attributes match the sea sphere's (FVoxelPlanetOcean).
 */
struct VOXELSTREAMING_API FVoxelFarFieldPatchMesh
{
	FVoxelFarFieldPatchKey Key;

	/** World position the vertices are relative to (their component sits here): the patch centre's point on the datum */
	FVector Origin = FVector::ZeroVector;

	/** The terrain: the surface, then the skirts (both windings) */
	FVoxelFarFieldSection Terrain;

	/** Vertices of Terrain that belong to the surface (the rest are the skirts') */
	int32 TerrainSurfaceVertices = 0;

	/** The sea's top (faces away from the centre); the underside is the same with every triangle reversed */
	FVoxelFarFieldSection Sea;

	/**
	 * The terrain's geometric error: the largest radial distance between the drawn triangles and the (sunk) analytic
	 * surface at the quads' centres, curvature included (uu)
	 */
	double Error = 0.0;

	/** The sea quads' largest sag under the sea sphere, at a quad's diagonal's middle (uu) */
	double SeaSag = 0.0;

	/** How far the skirts reach under the patch's edges (uu) */
	double SkirtDepth = 0.0;

	/** Terrain altitudes over the patch's vertices (the analytic surface, before the sink) */
	float MinAltitude = 0.0f;
	float MaxAltitude = 0.0f;

	/** Whether any quad is drawn as sea, and whether any lies above it */
	bool bHasSea = false;
	bool bHasLand = true;

	/** A sphere around everything drawn (world) */
	FVector BoundsCenter = FVector::ZeroVector;
	double BoundsRadius = 0.0;

	/**
	 * The voxel chunks the patch's surface passes through (edge patches): those of its vertices, its quad centres and the
	 * points a vertex spacing from each vertex along each axis. The hole's test: the patch hides only when all are drawn
	 */
	TArray<FIntVector> SurfaceChunks;

	/** Conditioning zones the build applied */
	int32 ZoneCount = 0;

	/** Time the build took (ms, on its thread) */
	double BuildMs = 0.0;

	/** Memory the meshes hold (bytes). */
	SIZE_T GetAllocatedSize() const
	{
		return Terrain.GetAllocatedSize() + Sea.GetAllocatedSize() + SurfaceChunks.GetAllocatedSize();
	}
};

/** The view a far-field update selects patches for. */
struct VOXELSTREAMING_API FVoxelFarFieldView
{
	/** Where the screen-space error and the horizon are measured from (the camera) */
	FVector Camera = FVector::ZeroVector;

	/** Pixels per radian of view: the viewport's width / (2 tan(horizontal field of view / 2)) */
	double PixelsPerRadian = 640.0;

	/** A patch splits while its error projects to more than this (pixels) */
	double MaxScreenError = 2.0;

	/** The streaming viewer: the voxel window's centre (its ground point is the hole's centre) */
	FVector Viewer = FVector::ZeroVector;

	/** Distance from the centre to the viewer's ground point (the voxel window's horizons are measured on it) */
	double GroundRadius = 0.0;

	/**
	 * Along the ground from the viewer's ground point: within it the voxel window may hold chunks (its unload distance
	 * plus a chunk's half diagonal), so patches there split down to the edge level and hide where their chunks are drawn
	 */
	double EdgeRadius = 0.0;

	/**
	 * Along the ground: edge patches reaching past it are built while their chunks are drawn (hidden), ready for when the
	 * chunks unload as the viewer moves away; nearer ones are built only when a chunk under them is missing
	 */
	double PrebuildRadius = 0.0;

	/** Seconds (any clock): when an edge patch's chunks were first all drawn */
	double Time = 0.0;

	/** An edge patch hides once its chunks have all been drawn this long (they fade in over it) */
	double HideDelay = 0.0;

	/** Version of the voxel chunks' states: the hole is recomputed only when it changes (-1: every update) */
	int64 ChunkStateVersion = -1;

	/**
	 * A patch hidden this long (seconds) is released: its component goes and it builds again when it is needed. Roots and
	 * edge patches near the window's edge stay. Negative: never
	 */
	double ReleaseDelay = 10.0;

	/**
	 * No stand-in where nothing would show (visual polish VP4). A patch that is not ready to draw is stood in for by its
	 * nearest built ancestor, which draws its whole area instead of everything under it; past the voxel window's band the
	 * ancestors are released after ReleaseDelay, so that is often a cube face's root, whose coarse surface replaced the
	 * horizon and rose through the voxel terrain for the frame or more the build took. With this:
	 * - an edge patch whose chunks are all drawn covers its area with nothing to draw, built or not (past PrebuildRadius
	 *   once they have been drawn HideDelay; one that must stay built for when its chunks unload builds meanwhile);
	 * - a patch culled (wholly under the horizon) in its last visit, so drawing nothing, waits for its build drawing nothing
	 *   while the camera moves (not after a cut: the camera more than a chunk from the last update's, where it may lie in
	 *   full view).
	 * False = before: both stood in for, as a covered edge patch the viewer moved past PrebuildRadius did
	 */
	bool bAvoidStandIns = true;

	/**
	 * Build every cube face's root, also one wholly under the horizon (visual polish F4.2). A root is the stand-in of last
	 * resort, kept built once built (IsKeptBuilt), but a culled patch requests no build, so a face the camera had never
	 * looked at had no root: after a teleport onto it the first update drew nothing there (the sky through the planet;
	 * the voxel window is not loaded there yet either) until builds landed. Six patches in all, built in the first
	 * updates, nearest first. False = before: a root was built once some of its face came into view
	 */
	bool bBuildAllRoots = true;
};

/** What an update changed, for the owner to apply to its components. */
struct VOXELSTREAMING_API FVoxelFarFieldChanges
{
	/** Patches built or rebuilt: (re)create their components from the meshes (FVoxelFarField::FindPatch) */
	TArray<uint64> Built;

	/** Patches removed or released: destroy their components */
	TArray<uint64> Removed;

	/** Patches whose drawn parts changed: apply bDrawTerrain / bDrawSea */
	TArray<uint64> Visibility;

	void Reset()
	{
		Built.Reset();
		Removed.Reset();
		Visibility.Reset();
	}

	bool IsEmpty() const { return Built.Num() == 0 && Removed.Num() == 0 && Visibility.Num() == 0; }
};

/**
 * A planet's far field (spherical Phase 6, WM-BP9, decision D19; WORLD_MODES_REVIEW_PLAN.md §20): the analytic surface
 * drawn beyond the voxel window, out to the horizon and from orbit, as a cube-sphere quadtree of patches.
 *
 * - Six face quadtrees on the planet's surface coordinate; every patch is a grid of PatchQuads x PatchQuads quads of the
 *   analytic surface (GetConditionedTerrainAltitude), Sink under it, with normals from the grid (one ring of samples past
 *   its edges, so neighbours of a level shade alike), materials and biomes as generation writes them, and skirts that
 *   cover the cracks between levels. Vertices are relative to the patch's own origin, so no float holds a value of the
 *   planet's size: a patch's offsets are of its own size, and its precision scales with it (patches near the viewer are
 *   a chunk or two across).
 * - A patch splits while its error projects to more than MaxScreenError pixels (curvature and the sea's sag included),
 *   and stays whole below 0.7 of it; patches wholly under the camera's horizon are neither drawn nor split. Built on
 *   the thread pool; a patch keeps drawing until its four children are ready, so the planet never has a hole while it
 *   refines.
 * - The voxel window's hole: within EdgeRadius of the viewer's ground point patches split to the edge level (one to two
 *   chunks), and an edge patch hides its terrain once every voxel chunk its surface passes through is drawn (after
 *   HideDelay). Partly covered patches stay drawn under the voxel surface (Sink): no gap and no overlap at the window's
 *   ragged edge, and no far-field floor in holes and caves dug where the chunks are drawn. Patch geometry never depends
 *   on the window, so the hole moves with no rebuild (the far ocean's window-and-frame split, FVoxelFarOcean, done by
 *   visibility alone).
 * - The sea (bSea): a section of each patch at sea level over the quads whose terrain lies near or under it, drawn over
 *   the voxel window too (planets have no voxel water tiles). Its sag is part of the error, so it is fine near the
 *   viewer wherever it is.
 *
 * Pure: the owner (UVoxelChunkManager) owns one procedural mesh component per built patch and applies the changes an
 * Update reports. Game thread, except the builds.
 */
class VOXELSTREAMING_API FVoxelFarField
{
public:
	/** A patch as the selection sees it: its footprint (FVoxelFarFieldFootprint: the cap or the box) and its state. */
	struct FPatchState : public FVoxelFarFieldFootprint
	{
		FVoxelFarFieldPatchKey Key;

		/** Built once (the metadata below is valid) */
		bool bBuilt = false;

		/** Its mesh is held by the owner's component (built and not released since): it can be drawn */
		bool bResident = false;

		/** When it was last drawn (View.Time); a patch hidden for View.ReleaseDelay is released */
		double LastDrawnTime = 0.0;

		/** A build is in flight */
		bool bBuilding = false;

		/** Its children exist (it is split or splitting) */
		bool bHasChildren = false;

		/** Drawn this update: the terrain section, the sea section */
		bool bDrawTerrain = false;
		bool bDrawSea = false;

		/** The built mesh, null when unbuilt or released (ReleaseMesh) */
		TSharedPtr<FVoxelFarFieldPatchMesh> Mesh;

		/** From the build */
		FVector Origin = FVector::ZeroVector;
		double Error = 0.0;
		double SeaSag = 0.0;
		double SkirtDepth = 0.0;
		float MinAltitude = 0.0f;
		float MaxAltitude = 0.0f;
		bool bHasSea = false;
		bool bHasLand = true;
		FVector BoundsCenter = FVector::ZeroVector;
		double BoundsRadius = 0.0;
		TArray<FIntVector> SurfaceChunks;

		/** Edge patches: whether every chunk under it is drawn, and since when */
		bool bCovered = false;
		double CoveredSince = -1.0;
		int64 CoveredVersion = -2;

		/** Unbuilt edge patches: the chunks at a 9 x 9 grid of surface samples (the lazy test), filled once */
		TArray<FIntVector> SampleChunks;

		/** What the owner was last told is drawn */
		bool bAppliedTerrain = false;
		bool bAppliedSea = false;

		/** An edge patch standing in for its part of its parent without a mesh this update (nothing to draw) */
		bool bLazy = false;

		/** Culled (wholly under the horizon) in its last visit: nothing was drawn for it */
		bool bCulled = false;

		/** Builds launched for it (a result for an older one is dropped) */
		uint32 BuildSerial = 0;

		/** Rebuild requested (its zones changed) */
		bool bDirty = false;
	};

	FVoxelFarField();
	~FVoxelFarField();

	FVoxelFarField(const FVoxelFarField&) = delete;
	FVoxelFarField& operator=(const FVoxelFarField&) = delete;

	/**
	 * Start over with a planet: drops every patch (builds in flight are discarded when they finish).
	 *
	 * @param InSettings What to draw and how finely
	 * @param InSurface The analytic surface (shared with the build tasks)
	 */
	void Initialize(const FVoxelFarFieldSettings& InSettings, TSharedPtr<const FVoxelFarFieldSurface> InSurface);

	/** Drop every patch (no changes are reported: the owner destroys its components itself). */
	void Reset();

	/** Whether Initialize has run with a surface. */
	bool IsInitialized() const { return Surface.IsValid(); }

	const FVoxelFarFieldSettings& GetSettings() const { return Settings; }

	/** The surface frame the settings describe (the sphere or the plane; FVoxelFarFieldFrame). */
	const FVoxelFarFieldFrame& GetFrame() const { return Frame; }

	/** Run builds inline in Update instead of on the thread pool (tests). */
	void SetSynchronousBuilds(bool bInSynchronous) { bSynchronousBuilds = bInSynchronous; }

	/** Most builds in flight at once (and launched per update). */
	void SetMaxBuildsInFlight(int32 InMax) { MaxBuildsInFlight = FMath::Max(1, InMax); }

	/**
	 * Where a build's conditioning zones come from (UVoxelChunkManager::GatherConditioningZonesOnSphere for a cap, its box
	 * gather for a box), called on the game thread when a build launches for a patch narrow enough to take zones
	 * (ZoneGatherMax) with the region its footprint covers.
	 */
	void SetZoneGatherer(TFunction<void(const FVoxelFarFieldRegion& Region, TArray<FVoxelConditioningZone>& OutZones)> InGatherer)
	{
		ZoneGatherer = MoveTemp(InGatherer);
	}

	/**
	 * Rebuild every built patch narrow enough to take zones that a conditioning zone reaches (it was added or removed). The
	 * old meshes keep drawing until the new ones arrive. A zone of the other kind of surface (a radial zone on a heightmap
	 * world) reaches nothing.
	 */
	void InvalidateZone(const FVoxelConditioningZone& Zone);

	/**
	 * Heightmap worlds: whether the voxel water tile of a chunk column (chunk X, Y) is drawn. An edge patch (one chunk column)
	 * hides its sea where the tile draws the same water and draws it where none does, so the ring between the window's load
	 * and unload horizons, whose chunks are drawn without tiles, keeps its sea. Without the query an edge patch's sea hides
	 * with its terrain.
	 */
	void SetWaterTileQuery(TFunction<bool(const FIntPoint& Column)> InQuery)
	{
		WaterTileQuery = MoveTemp(InQuery);
	}

	/**
	 * One step: take finished builds, select what to draw for the view, launch builds.
	 *
	 * @param View The camera, the voxel window and the error budget
	 * @param IsChunkDrawn Whether the voxel chunk at a coordinate is drawn (its mesh is on screen)
	 * @param OutChanges What changed, for the owner's components (reset first)
	 */
	void Update(const FVoxelFarFieldView& View, TFunctionRef<bool(const FIntVector&)> IsChunkDrawn, FVoxelFarFieldChanges& OutChanges);

	/** A patch by its packed key, or null. */
	const FPatchState* FindPatch(uint64 PackedKey) const;

	/** Calls Visit for every patch in the trees. */
	void ForEachPatch(TFunctionRef<void(const FPatchState&)> Visit) const;

	/**
	 * The drawn patch whose area holds a sample (the deepest drawn one on the way down its root's tree), or null.
	 *
	 * @param Sample A direction from the centre (normalized here), or a world (X, Y)
	 * @param bTerrainOnly Only a patch drawing its terrain
	 * @return The patch, or null
	 */
	const FPatchState* FindDrawnPatch(const FVoxelFarFieldSample& Sample, bool bTerrainOnly = true) const;

	/**
	 * The drawn patch whose area holds a world point: the deepest drawn one under its direction from the centre, or on a
	 * heightmap world over its (X, Y) (any point on the vertical through the place).
	 *
	 * @param WorldPoint The point
	 * @param bTerrainOnly Only a patch drawing its terrain
	 * @return The patch, or null
	 */
	const FPatchState* FindDrawnPatchAt(const FVector& WorldPoint, bool bTerrainOnly = true) const;

	/** The roots the last update visited (packed keys): a planet's six faces, a heightmap world's root tiles within reach. */
	const TSet<uint64>& GetRoots() const { return Roots; }

	/** Drop a built patch's CPU meshes once the owner has uploaded them (the metadata stays). */
	void ReleaseMesh(uint64 PackedKey);

	/** Builds waiting to launch or in flight. */
	int32 GetBuildsPending() const { return PendingBuildCount + BuildsInFlight; }
	int32 GetBuildsInFlight() const { return BuildsInFlight; }

	/** Patches whose meshes the owner holds (built and not released). */
	int32 GetResidentPatchCount() const
	{
		int32 Count = 0;
		for (const TPair<uint64, TUniquePtr<FPatchState>>& Pair : Patches)
		{
			Count += Pair.Value->bResident ? 1 : 0;
		}
		return Count;
	}

	/** Patches in the trees, and drawn ones (terrain or sea) after the last update. */
	int32 GetPatchCount() const { return Patches.Num(); }
	int32 GetDrawnPatchCount() const { return DrawnPatchCount; }

	/** Patches built since the last call (resets the count). */
	int32 ConsumeBuiltCount()
	{
		const int32 Count = BuiltSinceRead;
		BuiltSinceRead = 0;
		return Count;
	}

	/** Patches released in all (rebuilt when needed again). */
	int64 GetTotalReleased() const { return TotalReleased; }

	/**
	 * Stand-ins over the voxel window in the last update (visual polish VP4): patches drawn because a patch under them was
	 * not ready to draw, over edge patches the voxel chunks cover. Coarser than the voxel terrain, they show through it
	 * wherever their chords rise over it (a flash of the analytic surface for as long as the stand-in lasts).
	 */
	int32 GetStandInCount() const { return StandInCount; }

	/** The coarsest of the last update's stand-ins over the window (its level), -1 with none. */
	int32 GetStandInMinLevel() const { return StandInMinLevel; }

	/** Stand-ins over the window in all, and the updates that had any. */
	int64 GetTotalStandIns() const { return TotalStandIns; }
	int64 GetStandInUpdates() const { return StandInUpdates; }

	/**
	 * Edge patches that were not ready to draw in the last update (unbuilt or released, and not standing in without a
	 * mesh): covered (every chunk under them drawn) or not.
	 */
	int32 GetUnreadyEdgeCount(bool bCovered) const { return bCovered ? UnreadyCoveredEdge : UnreadyUncoveredEdge; }

	/** A chunk under one of the last update's unready, uncovered edge patches that was not drawn. False with none. */
	bool GetUndrawnChunk(FIntVector& OutChunk) const
	{
		OutChunk = UndrawnChunk;
		return bHasUndrawnChunk;
	}

	/** Patches built in all, and their total build time (ms, on the build threads). */
	int64 GetTotalBuilt() const { return TotalBuilt; }
	double GetTotalBuildMs() const { return TotalBuildMs; }

	/** CPU memory the patches' meshes hold before release (bytes). */
	int64 GetMeshMemoryBytes() const { return MeshMemoryBytes; }

	/**
	 * Build a patch (pure; any thread).
	 *
	 * @param Settings The far field's settings
	 * @param Surface The analytic surface
	 * @param Zones Conditioning zones over the patch (may be empty)
	 * @param Key The patch
	 * @param Out Receives the meshes and metadata
	 */
	static void BuildPatch(const FVoxelFarFieldSettings& Settings, const FVoxelFarFieldSurface& Surface,
		const TArray<FVoxelConditioningZone>& Zones, const FVoxelFarFieldPatchKey& Key, FVoxelFarFieldPatchMesh& Out);

	/**
	 * Whether a patch lies wholly under the horizon seen from a camera: past the angle at which the camera's line of sight
	 * grazes the lowest possible terrain (OccluderRadius), plus the angle a top at TopRadius stays visible beyond it.
	 *
	 * @param Center Planet centre
	 * @param OccluderRadius Radius of the lowest terrain
	 * @param Camera Camera position
	 * @param CapDirection Patch cap centre
	 * @param CapAngle Patch cap radius (radians)
	 * @param TopRadius Highest radius of anything the patch draws
	 * @return True when nothing of it can be seen
	 */
	static bool IsBelowHorizon(const FVector& Center, double OccluderRadius, const FVector& Camera, const FVector& CapDirection,
		double CapAngle, double TopRadius);

	/** Whether the sea is the far field's to draw: the sea sphere's sag past its budget at its most quads (at scale). */
	static bool ShouldDrawSea(double SeaRadius, double MaxSag, int32 MinSegments, int32 MaxSegments);

private:
	struct FBuildResult
	{
		uint64 Key = 0;
		uint32 Serial = 0;
		TSharedPtr<FVoxelFarFieldPatchMesh> Mesh;
	};

	/** Where build tasks put their results (shared with them, so a task that outlives a Reset writes into a dropped sink). */
	struct FBuildSink
	{
		FCriticalSection Lock;
		TArray<FBuildResult> Done;
	};

	struct FBuildRequest
	{
		uint64 Key = 0;
		double Priority = 0.0;
	};

	FPatchState* FindPatchMutable(uint64 PackedKey);
	FPatchState& GetOrAddPatch(const FVoxelFarFieldPatchKey& Key);

	/**
	 * The selection's recursion: sets what the patch and its subtree draw.
	 * @return Whether its area is covered (drawn by it or its subtree, culled, or a lazy edge patch with nothing to draw)
	 */
	bool Visit(uint64 PackedKey, FVoxelFarFieldChanges& OutChanges);

	/**
	 * Whether an edge patch without a mesh (unbuilt or released) can stand in for its area drawing nothing: every chunk under
	 * it drawn and no sea; inside the prebuild radius, or past it once the chunks have been drawn HideDelay (with
	 * View.bAvoidStandIns, VP4).
	 */
	bool IsLazy(FPatchState& Patch, const FPatchState& Parent);

	/** Whether a patch stays built while hidden: a root, or an edge patch near the window's edge. */
	bool IsKeptBuilt(const FPatchState& Patch) const;

	/** Clear the drawn flags of a patch's descendants. */
	void ClearDrawnSubtree(const FPatchState& Patch);

	/** Release a resident patch's component (reported as removed). */
	void ReleasePatch(FPatchState& Patch, FVoxelFarFieldChanges& OutChanges);

	/** Whether a patch wants its children this update. */
	bool WantsSplit(const FPatchState& Patch) const;

	/** Remove a patch's descendants. */
	void PruneChildren(FPatchState& Patch, FVoxelFarFieldChanges& OutChanges);

	/** Set what a patch draws (built). */
	void SetDrawn(FPatchState& Patch);

	/** Ground distance range from the viewer's ground point along the ground (the frame's). */
	double GetGroundDistanceMin(const FPatchState& Patch) const;
	double GetGroundDistanceMax(const FPatchState& Patch) const;

	/** Visit the roots the frame wants for the camera (a planet's six faces; a heightmap world's tiles within reach), dropping the rest. */
	void UpdateRoots(FVoxelFarFieldChanges& OutChanges);

	/** Whether a patch lies within the band region (with hysteresis while it is split). */
	bool IsEdgePatch(const FPatchState& Patch) const;

	/** Whether every chunk under an edge patch is drawn (cached per chunk-state version). */
	bool UpdateCovered(FPatchState& Patch);

	/** Count a patch that is not ready to draw, if it is an edge patch (the stand-in figures, VP4). */
	void NoteUnready(FPatchState& Patch);

	/** The chunk holding a world point. */
	FIntVector GetChunkOf(const FVector& WorldPoint) const;

	/** Whether a patch is wholly under the horizon for this update's camera. */
	bool IsCulled(const FPatchState& Patch) const;

	void RequestBuild(FPatchState& Patch);
	void LaunchBuilds();
	void DrainBuilds(FVoxelFarFieldChanges& OutChanges);
	void ApplyBuild(FPatchState& Patch, const TSharedPtr<FVoxelFarFieldPatchMesh>& Mesh, FVoxelFarFieldChanges& OutChanges);

	FVoxelFarFieldSettings Settings;
	FVoxelFarFieldFrame Frame;
	TSharedPtr<const FVoxelFarFieldSurface> Surface;
	TSharedPtr<FBuildSink, ESPMode::ThreadSafe> Sink;
	TFunction<void(const FVoxelFarFieldRegion&, TArray<FVoxelConditioningZone>&)> ZoneGatherer;
	TFunction<bool(const FIntPoint&)> WaterTileQuery;

	/** The roots of the last update (packed keys) */
	TSet<uint64> Roots;

	/** Every patch in the trees, by packed key (stable addresses: children are added during the recursion) */
	TMap<uint64, TUniquePtr<FPatchState>> Patches;

	/** This update's view and chunk test (valid during Update) */
	FVoxelFarFieldView View;
	const TFunctionRef<bool(const FIntVector&)>* ChunkDrawn = nullptr;
	/** The viewer's ground point in the frame (valid during Update) */
	FVoxelFarFieldFrame::FGround Ground;

	TArray<FBuildRequest> Requests;
	TSet<uint64> BuiltThisUpdate;
	int32 PendingBuildCount = 0;
	int32 BuildsInFlight = 0;
	int32 MaxBuildsInFlight = 6;
	bool bSynchronousBuilds = false;

	int32 DrawnPatchCount = 0;
	int32 BuiltSinceRead = 0;
	int64 TotalBuilt = 0;
	int64 TotalReleased = 0;
	double TotalBuildMs = 0.0;
	int64 MeshMemoryBytes = 0;

	/** The stand-in figures (VP4): this update's, and in all */
	int32 StandInCount = 0;
	int32 StandInMinLevel = -1;
	int64 TotalStandIns = 0;
	int64 StandInUpdates = 0;
	int32 UnreadyCoveredEdge = 0;
	int32 UnreadyUncoveredEdge = 0;
	/** Edge patches visited covered by drawn chunks (a stand-in counts when its subtree had one) */
	int64 CoveredEdgeVisits = 0;
	/** The last update's camera, and whether this update's moved more than a chunk from it (a cut: VP4) */
	FVector LastCamera = FVector::ZeroVector;
	bool bHasLastCamera = false;
	bool bCameraCut = true;
	FIntVector UndrawnChunk = FIntVector::ZeroValue;
	bool bHasUndrawnChunk = false;
};
