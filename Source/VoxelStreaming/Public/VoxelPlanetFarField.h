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
 */
struct VOXELSTREAMING_API FVoxelFarFieldPatchKey
{
	/** Cube face 0..5 (+X, -X, +Y, -Y, +Z, -Z) */
	int32 Face = 0;

	/** Quadtree level, 0 = the whole face */
	int32 Level = 0;

	/** Column along the face's U, 0 .. 2^Level - 1 */
	int32 X = 0;

	/** Row along the face's V, 0 .. 2^Level - 1 */
	int32 Y = 0;

	FVoxelFarFieldPatchKey() = default;
	FVoxelFarFieldPatchKey(int32 InFace, int32 InLevel, int32 InX, int32 InY)
		: Face(InFace), Level(InLevel), X(InX), Y(InY)
	{
	}

	/** Packed for maps: face 3 bits, level 5 bits, X and Y 28 bits each (levels up to 28). */
	uint64 Pack() const
	{
		return (static_cast<uint64>(Face & 7) << 61) | (static_cast<uint64>(Level & 31) << 56)
			| (static_cast<uint64>(X & 0x0FFFFFFF) << 28) | static_cast<uint64>(Y & 0x0FFFFFFF);
	}

	/** The key a Pack() came from. */
	static FVoxelFarFieldPatchKey Unpack(uint64 Packed)
	{
		return FVoxelFarFieldPatchKey(static_cast<int32>((Packed >> 61) & 7), static_cast<int32>((Packed >> 56) & 31),
			static_cast<int32>((Packed >> 28) & 0x0FFFFFFF), static_cast<int32>(Packed & 0x0FFFFFFF));
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

	FString ToString() const { return FString::Printf(TEXT("f%d L%d (%d,%d)"), Face, Level, X, Y); }
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

	/** Voxel size: the sink, and the sea's UVs (voxels of arc, as the ocean shell's) */
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

	/** The far field draws the sea (patches at sea level replace the ocean shell: ShouldDrawSea) */
	bool bSea = false;

	/** The sea radius (WaterRadius) */
	double SeaRadius = 0.0;

	/** A sea cell is drawn where the terrain under it is within this of the sea or below it (uu) */
	double SeaMargin = 200.0;

	/**
	 * Sea cells along a patch's edge: the sea is a smooth sphere, so a grid coarser than the terrain's sags under a uu
	 * where it is close (a band patch's cell on a 1,000 km planet: 0.002 uu; on the small planet: 0.3 uu)
	 */
	static constexpr int32 SeaQuads = 8;

	/** The finest level: patches between one and two chunks across (ComputeBandLevel) */
	int32 BandLevel = 0;

	/**
	 * Levels of coarser neighbour a patch's skirts cover: a patch drops a skirt from each edge as deep as the gap to the
	 * same edge drawn by a patch up to this many levels coarser.
	 */
	int32 SkirtLevels = 3;

	/**
	 * Patches whose cap is wider than this (radians) take no conditioning zones: a POI pad is a few chunks across, so at
	 * their resolution it does not show, and a cap of a face would ask the POI system for every POI on it.
	 */
	double ZoneGatherMaxAngle = 0.0;

	/**
	 * The finest level for a planet: the deepest at which a patch is still at least a chunk across along a face's centre
	 * lines (pi / 2 x Radius / 2^Level >= ChunkWorldSize), so band patches are one to two chunks across.
	 *
	 * @param Radius Datum radius
	 * @param ChunkWorldSize A chunk's edge
	 * @return Band level (0 for a planet smaller than a few chunks)
	 */
	static int32 ComputeBandLevel(double Radius, double ChunkWorldSize);

	/** A patch's edge on the datum sphere along a face's centre lines at a level: pi / 2 x Radius / 2^Level. */
	double GetPatchArc(int32 Level) const { return UE_DOUBLE_HALF_PI * Radius / static_cast<double>(1ll << FMath::Clamp(Level, 0, 40)); }
};

/**
 * The analytic surface the far field samples: the planet's terrain altitude (FSphericalPlanetWorldMode, conditioning
 * zones applied as generation applies them) and its surface material and biome
 * (FVoxelSurfaceQuery::QueryPlanetSurfaceConditionsAt). Immutable once made; read from build tasks on any thread.
 */
struct VOXELSTREAMING_API FVoxelFarFieldSurface
{
	/** The planet (an FSphericalPlanetWorldMode; its biome snapshot carries continentalness) */
	TSharedPtr<const IVoxelWorldMode> WorldMode;

	/** Terrain noise */
	FVoxelNoiseParams NoiseParams;

	/** Biomes for the surface material; invalid without biomes (the material then comes from GetMaterialAtDepth) */
	FVoxelBiomeSnapshot Biomes;

	/** The sea (the material's underwater variant) */
	FVoxelSeaSurface Sea;

	/** Voxel size (generation's depth in voxels) */
	float VoxelSize = 100.0f;

	/**
	 * The surface of a world configuration's planet.
	 *
	 * @param Config World configuration (a SphericalPlanet)
	 * @param InWorldMode The configuration's world mode (FVoxelWorldModeFactory::Create): must be a planet
	 * @return The surface, or null when the world mode is not a planet
	 */
	static TSharedPtr<const FVoxelFarFieldSurface> Make(const UVoxelWorldConfiguration& Config, const TSharedPtr<IVoxelWorldMode>& InWorldMode);

	/** The planet world mode. */
	const FSphericalPlanetWorldMode& GetPlanet() const;

	/**
	 * The terrain's altitude over the datum along a direction, zones applied (GetConditionedTerrainAltitude).
	 *
	 * @param Direction Unit direction from the centre
	 * @param Zones Planet conditioning zones there (may be empty)
	 * @param OutContinentalness Optional: the continentalness sampled (the material's biome blend reuses it)
	 * @return Altitude (uu)
	 */
	float SampleAltitude(const FVector& Direction, const TArray<FVoxelConditioningZone>& Zones, float* OutContinentalness = nullptr) const;

	/**
	 * The surface material and biome where the terrain is at Altitude along Direction (depth 0), as generation writes them.
	 *
	 * @param Direction Unit direction from the centre
	 * @param Altitude The terrain's altitude there (SampleAltitude)
	 * @param Continentalness SampleAltitude's continentalness
	 * @param OutMaterial Surface material ID
	 * @param OutBiome Dominant biome ID
	 */
	void SampleConditions(const FVector& Direction, float Altitude, float Continentalness, uint8& OutMaterial, uint8& OutBiome) const;

	/**
	 * The surface material mix where the terrain is at Altitude along Direction (G45): its two leading materials and the
	 * second's share (FVoxelSurfaceQuery::QueryPlanetSurfaceMaterialMixAt), what the voxels' per-voxel picks cover there on
	 * average. Without biomes: generation's material by depth, alone.
	 *
	 * @param Direction Unit direction from the centre
	 * @param Altitude The terrain's altitude there (SampleAltitude)
	 * @param Continentalness SampleAltitude's continentalness
	 * @return The mix
	 */
	FVoxelSurfaceMaterialMix SampleMaterialMix(const FVector& Direction, float Altitude, float Continentalness) const;

	/** voxel.World.FarFieldMaterialBlend: whether patches blend each quad's two leading materials (G45) or draw one. */
	static bool IsMaterialBlendEnabled();
};

/** One mesh section of a patch, ready for a procedural mesh section (positions relative to the patch's origin). */
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
 * attributes match the ocean shell's (FVoxelPlanetOcean).
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
	 * The voxel chunks the patch's surface passes through (band patches): those of its vertices, its quad centres and the
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
	 * plus a chunk's half diagonal), so patches there split down to the band level and hide where their chunks are drawn
	 */
	double BandRadius = 0.0;

	/**
	 * Along the ground: band patches reaching past it are built while their chunks are drawn (hidden), ready for when the
	 * chunks unload as the viewer moves away; nearer ones are built only when a chunk under them is missing
	 */
	double PrebuildRadius = 0.0;

	/** Seconds (any clock): when a band patch's chunks were first all drawn */
	double Time = 0.0;

	/** A band patch hides once its chunks have all been drawn this long (they fade in over it) */
	double HideDelay = 0.0;

	/** Version of the voxel chunks' states: the hole is recomputed only when it changes (-1: every update) */
	int64 ChunkStateVersion = -1;

	/**
	 * A patch hidden this long (seconds) is released: its component goes and it builds again when it is needed. Roots and
	 * band patches near the window's edge stay. Negative: never
	 */
	double ReleaseDelay = 10.0;

	/**
	 * No stand-in where nothing would show (visual polish VP4). A patch that is not ready to draw is stood in for by its
	 * nearest built ancestor, which draws its whole area instead of everything under it; past the voxel window's band the
	 * ancestors are released after ReleaseDelay, so that is often a cube face's root, whose coarse surface replaced the
	 * horizon and rose through the voxel terrain for the frame or more the build took. With this:
	 * - a band patch whose chunks are all drawn covers its area with nothing to draw, built or not (past PrebuildRadius
	 *   once they have been drawn HideDelay; one that must stay built for when its chunks unload builds meanwhile);
	 * - a patch culled (wholly under the horizon) in its last visit, so drawing nothing, waits for its build drawing nothing
	 *   while the camera moves (not after a cut: the camera more than a chunk from the last update's, where it may lie in
	 *   full view).
	 * False = before: both stood in for, as a covered band patch the viewer moved past PrebuildRadius did
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
	/** Patches built or rebuilt: (re)create their components from the meshes (FVoxelPlanetFarField::FindPatch) */
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
 * - The voxel window's hole: within BandRadius of the viewer's ground point patches split to the band level (one to two
 *   chunks), and a band patch hides its terrain once every voxel chunk its surface passes through is drawn (after
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
class VOXELSTREAMING_API FVoxelPlanetFarField
{
public:
	/** A patch as the selection sees it. */
	struct FPatchState
	{
		FVoxelFarFieldPatchKey Key;

		/** The patch's cap on the sphere: its centre direction and the largest angle from it to a corner */
		FVector CapDirection = FVector::UpVector;
		double CapAngle = 0.0;

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

		/** Band patches: whether every chunk under it is drawn, and since when */
		bool bCovered = false;
		double CoveredSince = -1.0;
		int64 CoveredVersion = -2;

		/** Unbuilt band patches: the chunks at a 9 x 9 grid of surface samples (the lazy test), filled once */
		TArray<FIntVector> SampleChunks;

		/** What the owner was last told is drawn */
		bool bAppliedTerrain = false;
		bool bAppliedSea = false;

		/** A band patch standing in for its part of its parent without a mesh this update (nothing to draw) */
		bool bLazy = false;

		/** Culled (wholly under the horizon) in its last visit: nothing was drawn for it */
		bool bCulled = false;

		/** Builds launched for it (a result for an older one is dropped) */
		uint32 BuildSerial = 0;

		/** Rebuild requested (its zones changed) */
		bool bDirty = false;
	};

	FVoxelPlanetFarField();
	~FVoxelPlanetFarField();

	FVoxelPlanetFarField(const FVoxelPlanetFarField&) = delete;
	FVoxelPlanetFarField& operator=(const FVoxelPlanetFarField&) = delete;

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

	/** Run builds inline in Update instead of on the thread pool (tests). */
	void SetSynchronousBuilds(bool bInSynchronous) { bSynchronousBuilds = bInSynchronous; }

	/** Most builds in flight at once (and launched per update). */
	void SetMaxBuildsInFlight(int32 InMax) { MaxBuildsInFlight = FMath::Max(1, InMax); }

	/**
	 * Where a build's conditioning zones come from (UVoxelChunkManager::GatherConditioningZonesOnSphere), called on the
	 * game thread when a build launches for a patch no wider than ZoneGatherMaxAngle.
	 */
	void SetZoneGatherer(TFunction<void(const FVector& Direction, double AngularRadius, TArray<FVoxelConditioningZone>& OutZones)> InGatherer)
	{
		ZoneGatherer = MoveTemp(InGatherer);
	}

	/**
	 * Rebuild every built patch narrow enough to take zones that meets a cap (a conditioning zone was added or removed
	 * there). The old meshes keep drawing until the new ones arrive.
	 *
	 * @param Direction Cap centre
	 * @param AngularRadius Cap radius (radians)
	 */
	void InvalidateCap(const FVector& Direction, double AngularRadius);

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
	 * The drawn patch whose area holds a direction (the deepest drawn one on the way down its face's tree), or null.
	 *
	 * @param Direction Unit direction from the centre
	 * @param bTerrainOnly Only a patch drawing its terrain
	 * @return The patch, or null
	 */
	const FPatchState* FindDrawnPatch(const FVector& Direction, bool bTerrainOnly = true) const;

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
	 * not ready to draw, over band patches the voxel chunks cover. Coarser than the voxel terrain, they show through it
	 * wherever their chords rise over it (a flash of the analytic surface for as long as the stand-in lasts).
	 */
	int32 GetStandInCount() const { return StandInCount; }

	/** The coarsest of the last update's stand-ins over the window (its level), -1 with none. */
	int32 GetStandInMinLevel() const { return StandInMinLevel; }

	/** Stand-ins over the window in all, and the updates that had any. */
	int64 GetTotalStandIns() const { return TotalStandIns; }
	int64 GetStandInUpdates() const { return StandInUpdates; }

	/**
	 * Band patches that were not ready to draw in the last update (unbuilt or released, and not standing in without a
	 * mesh): covered (every chunk under them drawn) or not.
	 */
	int32 GetUnreadyBandCount(bool bCovered) const { return bCovered ? UnreadyCoveredBand : UnreadyUncoveredBand; }

	/** A chunk under one of the last update's unready, uncovered band patches that was not drawn. False with none. */
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
	 * The cap a patch covers: its centre direction and the largest angle from it to its corners (the patch is bounded by
	 * great-circle arcs, so a corner is its farthest point).
	 *
	 * @param Key The patch
	 * @param OutDirection Centre direction
	 * @param OutAngle Angular radius (radians)
	 */
	static void GetPatchCap(const FVoxelFarFieldPatchKey& Key, FVector& OutDirection, double& OutAngle);

	/**
	 * Build a patch (pure; any thread).
	 *
	 * @param Settings The far field's settings
	 * @param Surface The analytic surface
	 * @param Zones Planet conditioning zones over the patch (may be empty)
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

	/** Whether the sea is the far field's to draw: the ocean shell's sag past its budget at its most quads (at scale). */
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
	 * @return Whether its area is covered (drawn by it or its subtree, culled, or a lazy band patch with nothing to draw)
	 */
	bool Visit(uint64 PackedKey, FVoxelFarFieldChanges& OutChanges);

	/**
	 * Whether a band patch without a mesh (unbuilt or released) can stand in for its area drawing nothing: every chunk under
	 * it drawn and no sea; inside the prebuild radius, or past it once the chunks have been drawn HideDelay (with
	 * View.bAvoidStandIns, VP4).
	 */
	bool IsLazy(FPatchState& Patch, const FPatchState& Parent);

	/** Whether a patch stays built while hidden: a root, or a band patch near the window's edge. */
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

	/** Ground distance range from the viewer's ground point (along the ground). */
	double GetGroundDistanceMin(const FPatchState& Patch) const;
	double GetGroundDistanceMax(const FPatchState& Patch) const;

	/** Whether a patch lies within the band region (with hysteresis while it is split). */
	bool IsInBand(const FPatchState& Patch) const;

	/** Whether every chunk under a band patch is drawn (cached per chunk-state version). */
	bool UpdateCovered(FPatchState& Patch);

	/** Count a patch that is not ready to draw, if it is a band patch (the stand-in figures, VP4). */
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
	TSharedPtr<const FVoxelFarFieldSurface> Surface;
	TSharedPtr<FBuildSink, ESPMode::ThreadSafe> Sink;
	TFunction<void(const FVector&, double, TArray<FVoxelConditioningZone>&)> ZoneGatherer;

	/** Every patch in the trees, by packed key (stable addresses: children are added during the recursion) */
	TMap<uint64, TUniquePtr<FPatchState>> Patches;

	/** This update's view and chunk test (valid during Update) */
	FVoxelFarFieldView View;
	const TFunctionRef<bool(const FIntVector&)>* ChunkDrawn = nullptr;
	FVector GroundDirection = FVector::UpVector;

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
	int32 UnreadyCoveredBand = 0;
	int32 UnreadyUncoveredBand = 0;
	/** Band patches visited covered by drawn chunks (a stand-in counts when its subtree had one) */
	int64 CoveredBandVisits = 0;
	/** The last update's camera, and whether this update's moved more than a chunk from it (a cut: VP4) */
	FVector LastCamera = FVector::ZeroVector;
	bool bHasLastCamera = false;
	bool bCameraCut = true;
	FIntVector UndrawnChunk = FIntVector::ZeroValue;
	bool bHasUndrawnChunk = false;
};
