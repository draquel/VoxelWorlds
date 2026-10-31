// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "VoxelData.h"
#include "VoxelSeaSurface.h"
#include "VoxelEditTypes.h"
#include "VoxelScatterTypes.h"
#include "VoxelScatterManager.generated.h"

class UVoxelWorldConfiguration;
class UVoxelScatterRenderer;
class IVoxelWorldMode;
class UVoxelCaveConfiguration;
struct FVoxelConditioningZone;

/**
 * Global signals used to classify extracted surface points as underground / underwater — the things a
 * single chunk's voxel data cannot know on its own. A surface point can be covered by terrain that lives
 * in a NEIGHBORING chunk (a cave floor, or ground under an overhang, whose roof is in the chunk above),
 * or it can be submerged; per-chunk data can't see either. These provide the cross-chunk answers: the
 * analytic terrain-height query (solid terrain above the point → underground) and the world water level.
 */
struct FScatterExtractionContext
{
	/** World mode for the analytic terrain-height query. Null = coverage check skipped. Non-owning. */
	const IVoxelWorldMode* WorldMode = nullptr;
	/** Noise params for IVoxelWorldMode::GetTerrainHeightAt. */
	FVoxelNoiseParams NoiseParams;
	/**
	 * The sea (generation's: FVoxelSeaSurface::FromConfig): points below it that are not underground are underwater.
	 * The WaterLevel plane in heightmap modes, the sphere of WaterRadius on a planet (spherical SP-E1). Disabled (no
	 * underwater class from it) when water is off.
	 */
	FVoxelSeaSurface Sea;
	/**
	 * The terrain conditioning zones over the chunk (POI pads and their falloff rings), so the analytic surface the
	 * extractors compare with is the generated ground, not the natural one. Heightmap worlds: the buried-chunk test
	 * compares the chunk's top with the natural height plus these zones (FVoxelTerrainConditioning::ApplyToHeight;
	 * world modes review G23). Planets (spherical SP-B10): the voxels past a chunk's +X, +Y and +Z faces
	 * (GetPlanetVoxelOutsideChunk) are generated on the flattened ground (ApplyToAltitude). Each kind of zone applies only
	 * in its own kind of world.
	 * Null or empty: the world mode's natural surface.
	 */
	TSharedPtr<const TArray<FVoxelConditioningZone>> ConditioningZones;

	/**
	 * Heightmap worlds: the edits of the chunk above on its bottom layer (local Z 0), keyed by local X + Y * ChunkSize
	 * (world modes review G26). The voxel over a chunk's top layer is that layer's, which the chunk's own data cannot
	 * show: GetVoxelAboveChunk applies these to the voxel the generator made there. Null: no edits on that layer.
	 */
	TSharedPtr<const TMap<int32, FVoxelEdit>> AboveLayerEdits;

	/**
	 * The world's cave configuration when its caves are on, so the voxels a chunk's extraction reads past its own data are
	 * carved as generation carves them: on a heightmap the voxel over the chunk's top layer under deeper ground
	 * (GetVoxelAboveChunk, world modes review G28), on a planet the voxels past its +X, +Y and +Z faces
	 * (GetPlanetVoxelOutsideChunk, G72). Null: no caves (the terrain's voxel alone). Non-owning: the world configuration's
	 * asset.
	 */
	const UVoxelCaveConfiguration* CaveConfiguration = nullptr;

	/** Whether the world's biomes are on (bEnableBiomes): which biome generation gives a planet voxel, for the cave
	 *  configuration's biome overrides (GetPlanetVoxelOutsideChunk). */
	bool bEnableBiomes = false;

	/**
	 * Planets (G72, voxel.Scatter.PlanetFaceCaves): the voxels past a chunk's +X, +Y and +Z faces are carved by the caves,
	 * as generation carves them. False: the generated altitude alone, as before (a cave opening such a voxel read as rock).
	 */
	bool bCarvePlanetFaceVoxels = true;

	/**
	 * Planets, for audits and tests (G72): when set, the voxels past the chunk's +X, +Y and +Z faces come from here (the
	 * neighbours' own voxels, LocalCoord in this chunk's frame as GetPlanetVoxelOutsideChunk takes it) instead of being
	 * made; returning false falls back to GetPlanetVoxelOutsideChunk. Unset in the streaming.
	 */
	TFunction<bool(const FIntVector& LocalCoord, FVoxelData& OutVoxel)> OutsideVoxelSource;

	/**
	 * Heightmap worlds (G28, voxel.Scatter.BuriedTopFace): the scans decide the voxel over a chunk's top layer at any depth
	 * (GetVoxelAboveChunk, caves included), so a buried chunk's top face carries no copy of the rock above it. False: only
	 * within the band the buried-chunk test lets through (G26), the voxel taken for air under deeper ground, as before.
	 */
	bool bDecideVoxelAboveAtAnyDepth = true;

	/**
	 * Planets: the edits in the voxels just past the chunk's +X, +Y and +Z faces, which its last cell on each axis reaches
	 * (world modes review G29, G31): the layers of the seven neighbours on those sides (GetPlanetShellNeighbours) that
	 * touch the chunk. Keyed by the voxel's coordinate in this chunk's frame (each component in [0, ChunkSize], one of
	 * them ChunkSize). GetPlanetVoxelOutsideChunk applies these to the voxel the generator made there. Null: no edits there.
	 */
	TSharedPtr<const TMap<FIntVector, FVoxelEdit>> TouchingEdits;

	/**
	 * Heightmap worlds: the generated terrain height at a world X,Y, the world mode's natural height with
	 * ConditioningZones layered on as the generator does. Requires a heightmap WorldMode.
	 * @param WorldX World X
	 * @param WorldY World Y
	 * @return The generated surface's world Z
	 */
	float GetGeneratedTerrainHeight(double WorldX, double WorldY) const;

	/**
	 * Heightmap worlds (world modes review G26, G28): the voxel over a chunk's top layer at a column, the chunk above's
	 * bottom voxel, as the heightmap generators make it: the generated height's signed distance at the chunk's top (solid
	 * where that height reaches the top), less the cave carve there (CaveConfiguration: FVoxelCPUNoiseGenerator's
	 * CalculateCaveDensity at the voxel's depth under the generated ground, for the column's biome, under the sea or not),
	 * with the chunk above's edit there applied (AboveLayerEdits). Only Density and MaterialID are meaningful.
	 * @param LocalX Column X in the chunk
	 * @param LocalY Column Y in the chunk
	 * @param ChunkSize Voxels per chunk edge
	 * @param SamplePos The voxel's sample position: the column's X, Y (the chunk's origin + LocalX, LocalY * VoxelSize) at
	 *        the chunk's top Z, as generation samples the chunk above's bottom layer
	 * @param GeneratedHeight The generated height at the column's sample position (GetGeneratedTerrainHeight)
	 * @param VoxelSize Voxel edge in world units
	 * @param BiomeID The column's biome (a column has one: any of its generated voxels' BiomeID), for the cave
	 *        configuration's biome overrides
	 * @return The voxel above the chunk's top layer
	 */
	FVoxelData GetVoxelAboveChunk(int32 LocalX, int32 LocalY, int32 ChunkSize, const FVector& SamplePos, float GeneratedHeight, float VoxelSize,
		uint8 BiomeID) const;

	/**
	 * Planets (world modes review G29, G72): a voxel just outside the chunk, in a neighbour's layer touching it, which the
	 * chunk's own data cannot show, as the planet generator makes it: the signed distance to the generated altitude along
	 * its direction (ConditioningZones applied), less the cave carve where it is solid under the ground (CaveConfiguration:
	 * CalculateCaveDensity at its depth along local up, for the biome generation gives its direction, under the sea or not;
	 * bCarvePlanetFaceVoxels), with the neighbour's edit there applied (TouchingEdits). Only Density and MaterialID are
	 * meaningful. Requires a spherical planet WorldMode.
	 * @param LocalCoord The voxel's coordinate in this chunk's frame (ChunkSize on at least one axis)
	 * @param WorldPos The voxel's sample position (the chunk's origin + LocalCoord * VoxelSize, as generation samples it)
	 * @param VoxelSize Voxel edge in world units
	 * @return The voxel as generated, with any edit applied
	 */
	FVoxelData GetPlanetVoxelOutsideChunk(const FIntVector& LocalCoord, const FVector& WorldPos, float VoxelSize) const;
};

/**
 * Delegate fired when scatter data is ready for a chunk.
 */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnChunkScatterReady, const FIntVector& /*ChunkCoord*/, int32 /*SpawnPointCount*/);

/**
 * Voxel Scatter Manager.
 *
 * Manages scatter generation for voxel terrain. Coordinates surface point
 * extraction from mesh data and scatter placement based on definitions.
 *
 * Key responsibilities:
 * - Extract surface points when chunk meshes are ready
 * - Generate scatter spawn points based on placement rules
 * - Cache scatter data per chunk
 * - Provide debug visualization
 *
 * Integration:
 * - Created and owned by UVoxelChunkManager
 * - Listens for chunk mesh ready events
 * - Cleans up when chunks unload
 *
 * Thread Safety: Game thread for all public API. Surface extraction and placement
 * run on thread pool via LaunchAsyncScatterGeneration(). Results are consumed
 * on game thread via ProcessCompletedAsyncScatter().
 *
 * @see UVoxelChunkManager
 * @see FVoxelScatterPlacement
 */
UCLASS(BlueprintType)
class VOXELSCATTER_API UVoxelScatterManager : public UObject
{
	GENERATED_BODY()

public:
	UVoxelScatterManager();

	// ==================== Lifecycle ====================

	/**
	 * Initialize the scatter manager.
	 *
	 * @param Config World configuration
	 * @param World World reference for debug drawing
	 */
	void Initialize(UVoxelWorldConfiguration* Config, UWorld* World);

	/**
	 * Shutdown and cleanup all scatter resources.
	 */
	void Shutdown();

	/**
	 * Warn about scatter definitions whose distance settings guarantee visible popping:
	 * LODStartDistance >= CullDistance (no fade range) or CullDistance >= the effective
	 * SpawnDistance (instances drawn out to where they stop being generated). Run once at
	 * Initialize; these relationships are otherwise unenforced and only show up in motion.
	 */
	void ValidateScatterDistances() const;

	/**
	 * Check if manager is initialized.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	bool IsInitialized() const { return bIsInitialized; }

	// ==================== Per-Frame Update ====================

	/**
	 * Update scatter state based on viewer position.
	 *
	 * Currently handles debug visualization updates.
	 *
	 * @param ViewerPosition Current viewer/camera position
	 * @param DeltaTime Time since last update
	 */
	void Update(const FVector& ViewerPosition, float DeltaTime);

	// ==================== Scatter Definitions ====================

	/**
	 * Add a scatter type definition.
	 *
	 * @param Definition The scatter definition to add
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	void AddScatterDefinition(const FScatterDefinition& Definition);

	/**
	 * Get all scatter definitions.
	 */
	const TArray<FScatterDefinition>& GetScatterDefinitions() const { return ScatterDefinitions; }

	/**
	 * Get a scatter definition by ID.
	 *
	 * @param ScatterID ID to look up
	 * @return Pointer to definition or nullptr if not found
	 */
	const FScatterDefinition* GetScatterDefinition(int32 ScatterID) const;

	// ==================== Visibility ====================

	/**
	 * Show or hide surface-only scatter (trees, grass, etc.) based on viewer position.
	 * When the viewer is underground, surface scatter should be hidden to prevent
	 * foliage from being visible through cave ceilings.
	 *
	 * @param bVisible True to show surface scatter, false to hide
	 */
	void SetSurfaceScatterVisible(bool bVisible);

	// ==================== Scatter Data Access ====================

	/**
	 * Get scatter data for a chunk.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return Pointer to scatter data or nullptr if not available
	 */
	const FChunkScatterData* GetChunkScatterData(const FIntVector& ChunkCoord) const;

	/**
	 * Get surface data for a chunk.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return Pointer to surface data or nullptr if not available
	 */
	const FChunkSurfaceData* GetChunkSurfaceData(const FIntVector& ChunkCoord) const;

	/**
	 * Check if chunk has scatter data.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	bool HasScatterData(const FIntVector& ChunkCoord) const;

	/**
	 * Get read-only access to the scatter data cache.
	 * Used by the renderer to iterate all cached scatter data during rebuilds.
	 */
	const TMap<FIntVector, FChunkScatterData>& GetScatterDataCache() const { return ScatterDataCache; }

	/**
	 * Chunks handed off with every scatter type out of range and so not extracted yet (world modes review G79): the
	 * distance pass asks for their hand-off again once they come within the scatter horizon. For tests and audits.
	 */
	int32 GetChunksAwaitingFirstExtractionCount() const { return ChunksAwaitingFirstExtraction.Num(); }

	/** The chunks awaiting their first extraction (GetChunksAwaitingFirstExtractionCount), for tests and audits. */
	TArray<FIntVector> GetChunksAwaitingFirstExtraction() const { return ChunksAwaitingFirstExtraction.Array(); }

	/** Hand-offs the distance pass asked for (G79: a chunk's first extraction once within the scatter horizon), since Initialize. */
	int64 GetTotalDeferredFirstExtractions() const { return TotalDeferredFirstExtractions; }

	/**
	 * The scatter horizon: the largest distance any enabled scatter type spawns at (its SpawnDistance, or ScatterRadius
	 * when 0; VoxelInjection types the tree mode rules out left out). A chunk beyond it has nothing to extract.
	 */
	float GetScatterHorizon() const;

	/**
	 * The renderer holding the HISM instances per chunk and scatter type, for tests and tools.
	 * @return The renderer; null before Initialize and after Shutdown.
	 */
	const UVoxelScatterRenderer* GetScatterRenderer() const;

	/**
	 * A digest of every cached spawn point, for comparing two builds' scatter on the same route (spherical Phase 4's
	 * heightmap A/B, WORLD_MODES_REVIEW_PLAN.md §18): per scatter type the point count and a CRC of the points' type,
	 * position (to 0.1 uu), yaw, scale and local up, visited in chunk order. The console command voxel.Scatter.Digest
	 * logs it for every game world.
	 *
	 * @param OutChunkLines Optional: appended with one "X,Y,Z,count,crc" line per chunk (every type's points, in the same
	 *                      order), so two builds' digests can be compared chunk by chunk (voxel.Scatter.Digest file)
	 * @return "types=N chunks=M points=P | type <id>: <count> crc <hex> | ..."
	 */
	FString ComputeScatterDigest(TArray<FString>* OutChunkLines = nullptr) const;

	/**
	 * The two caches against each other and the renderer, to check that every chunk's scatter renders once (world modes
	 * review G30; voxel.Scatter.Digest logs it under the digest): chunks with spawn points, chunks with surface data,
	 * chunks with spawn points but no surface data (none since G30: an unloaded chunk drops both), spawn points, rendered
	 * instances (UVoxelScatterRenderer::GetChunkInstanceCount, queued adds included), chunks whose instance count
	 * differs from their spawn point count (a type whose mesh cannot load has spawn points and no instances), and spawn
	 * points that repeat another of their chunk (same type and position to 0.1 uu: a pass appended twice).
	 *
	 * @param OutMismatchLines Optional: appended with one "X,Y,Z,spawnPoints,instances,surface|noSurface,types" line per
	 *                         chunk whose counts differ; types lists "<id>:<spawn points>/<instances>" for each type that
	 *                         differs, with '!' where the renderer does not track the type for the chunk
	 * @return "scatterChunks=N surfaceChunks=M scatterOnly=S spawnPoints=P instances=I instanceMismatch=K duplicates=D (C chunks)"
	 */
	FString ComputeCacheResidency(TArray<FString>* OutMismatchLines = nullptr) const;

	/**
	 * The heightmap extractors without cleared volumes, for tests and tools (world modes review G23): the column scan
	 * (ExtractSurfacePointsFromVoxelData) or, with bCubic, the block-face scan (ExtractSurfacePointsCubic, one point per
	 * voxel). Pure and thread-safe.
	 */
	static void ExtractHeightmapSurfacePoints(
		const TArray<FVoxelData>& VoxelData,
		const FIntVector& ChunkCoord,
		const FVector& ChunkWorldOrigin,
		int32 ChunkSize,
		float VoxelSize,
		float PointSpacing,
		bool bCubic,
		const FScatterExtractionContext& Context,
		FChunkSurfaceData& OutSurfaceData);

	/**
	 * ExtractSurfacePointsAlongUp without cleared volumes (spherical SP-G1): a planet chunk's surface points along local
	 * up, for tests and tools. Pure and thread-safe.
	 */
	static void ExtractPlanetSurfacePoints(
		const TArray<FVoxelData>& VoxelData,
		const FIntVector& ChunkCoord,
		const FVector& ChunkWorldOrigin,
		int32 ChunkSize,
		float VoxelSize,
		float PointSpacing,
		const FScatterExtractionContext& Context,
		FChunkSurfaceData& OutSurfaceData);

	/**
	 * Planets (world modes review G31): the neighbours whose voxels a planet chunk's rays read past its faces, as chunk
	 * coordinate offsets: the seven on its +X, +Y and +Z sides (the three faces, their three edges and their corner), into
	 * which the chunk's last cell on each axis reaches. Pure.
	 * @param OutOffsets Receives the offsets (appended)
	 */
	static void GetPlanetShellNeighbours(TArray<FIntVector>& OutOffsets);

	/**
	 * Planets (world modes review G29, G31): the edits in the layers of GetPlanetShellNeighbours's neighbours touching a
	 * chunk, keyed by their coordinate in the chunk's frame, for FScatterExtractionContext::TouchingEdits.
	 * MakeExtractionContext calls it with the chunk manager's edit layers (SetEditLayerSource).
	 * @param ChunkCoord The chunk
	 * @param ChunkSize Voxels per chunk edge
	 * @param EditLayerSource A chunk's edit layer, or null when it has none
	 * @return The edits, or null when there are none
	 */
	static TSharedPtr<const TMap<FIntVector, FVoxelEdit>> GatherPlanetTouchingEdits(const FIntVector& ChunkCoord, int32 ChunkSize,
		TFunctionRef<const FChunkEditLayer*(const FIntVector&)> EditLayerSource);

	/**
	 * Which other chunks' extraction reads a chunk's edited voxels, as chunk coordinate offsets from it (world modes review
	 * G33): the inverse of what MakeExtractionContext reads past a chunk. On a heightmap world (bHeightmapReads) the chunk
	 * below, whose top layer's voxel above is the bottom layer here (AboveLayerEdits, G26): offset (0, 0, -1) when the edit
	 * reached local Z 0. On a planet (bPlanetReads) the chunks whose GetPlanetShellNeighbours include this one: offset -D
	 * for each of their offsets D, when the edit reached local 0 on every axis where D is 1 (a face neighbour reads the
	 * layer facing it, an edge neighbour the column along the shared edge, the corner neighbour the corner voxel;
	 * TouchingEdits, G29, G31). Pure.
	 *
	 * @param bHeightmapReads Whether extraction reads the voxels over a chunk's top layer (a heightmap world with edits)
	 * @param bPlanetReads Whether extraction reads the voxels past a chunk's +X, +Y and +Z faces (a planet with edits)
	 * @param EditMin The lowest chunk-local coordinate the edit reached on each axis (UVoxelEditManager::
	 *                GetBroadcastEditBounds); null when not known, which gives every chunk that could read it
	 * @param OutOffsets Receives the offsets (appended)
	 */
	static void GetEditReaderOffsets(bool bHeightmapReads, bool bPlanetReads, const FIntVector* EditMin, TArray<FIntVector>& OutOffsets);

	/**
	 * The chunks whose extraction reads an edited chunk's voxels on this world (GetEditReaderOffsets with this manager's
	 * world mode and edit layer source, as MakeExtractionContext reads them). The edited chunk itself is not among them.
	 *
	 * @param EditedChunk The edited chunk
	 * @param EditMin The lowest chunk-local coordinate the edit reached on each axis; null when not known
	 * @param OutChunks Receives the chunk coordinates (appended)
	 */
	void GetEditReaders(const FIntVector& EditedChunk, const FIntVector* EditMin, TArray<FIntVector>& OutChunks) const;

	/** One cached chunk's surface points against a fresh extraction now (CompareWithFreshExtraction). */
	struct FFreshExtractionComparison
	{
		FIntVector ChunkCoord = FIntVector::ZeroValue;
		int32 CachedPoints = 0;
		int32 FreshPoints = 0;
		/** Points in one list with no equal point in the other (position to 0.1 uu, normal, material, classification). */
		int32 ChangedPoints = 0;
	};

	/** What CompareWithFreshExtraction compared and skipped. */
	struct FFreshExtractionStats
	{
		int32 Compared = 0;
		int32 Stale = 0;
		/** Queued or in flight: their cache is about to be replaced. */
		int32 SkippedPending = 0;
		/** With a cleared volume (a player edit clears scatter and never re-extracts, by design). */
		int32 SkippedCleared = 0;
		int32 SkippedNoVoxels = 0;
	};

	/**
	 * Re-run every cached chunk's surface extraction now, from its current voxels and a context made now (the edits of the
	 * chunks it reads past its faces, the conditioning zones), and compare it with the cached surface points: a chunk whose
	 * cache differs holds a stale view of something it reads (world modes review G33: a neighbour's edit it was not
	 * re-extracted for). For audits (voxel.Scatter.EditAudit) and tests; game thread (the extractions run in parallel).
	 *
	 * @param VoxelSource A chunk's edit-merged voxels, or null when it has none (the chunk is skipped)
	 * @param OutStale Receives the chunks whose cache differs, most changed points first
	 * @return The counts
	 */
	FFreshExtractionStats CompareWithFreshExtraction(TFunctionRef<TSharedPtr<const TArray<FVoxelData>>(const FIntVector&)> VoxelSource,
		TArray<FFreshExtractionComparison>& OutStale) const;

	/** Surface extractions launched since Initialize (each chunk pass on the thread pool), for tests and audits. */
	int64 GetTotalExtractionsLaunched() const { return TotalExtractionsLaunched; }

	/** RegenerateChunkScatter calls since Initialize, for tests and audits. */
	int64 GetTotalRegenerations() const { return TotalRegenerations; }

	/** Game-thread time of the last Update's steps (ms) and what they did, for the streaming bench (world modes review G42). */
	struct FUpdateTimings
	{
		float ApplyMs = 0.0f;     // ProcessCompletedAsyncScatter: completed extractions into the caches and the renderer
		float RegrowMs = 0.0f;    // ProcessPendingExclusionRegrow
		float LaunchMs = 0.0f;    // ProcessPendingGenerationQueue: extraction contexts and launches
		float DistanceMs = 0.0f;  // ProcessCompletedDistanceStream + PerformDistanceSpawn
		float FlushMs = 0.0f;     // the renderer's Tick: queued instance adds into the HISMs
		int32 Applied = 0;        // completed extractions applied
		int32 Launched = 0;       // extractions launched
	};

	/** The last Update's step timings. */
	const FUpdateTimings& GetLastUpdateTimings() const { return LastUpdateTimings; }

	/**
	 * Get number of chunks pending scatter generation (queued + in-flight async).
	 * Used to detect if world is still loading.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	int32 GetPendingGenerationCount() const { return PendingGenerationQueue.Num() + AsyncScatterInProgress.Num(); }

	// ==================== Mesh Data Callback ====================

	/**
	 * Called when a chunk's mesh is ready (the chunk manager's hand-off, before the mesh goes to the renderer).
	 * Queues surface extraction and scatter generation from the chunk's voxels, which are full resolution whatever the
	 * mesh's LOD, so a chunk's scatter does not depend on the LOD it was meshed at. The mesh itself is not read: it has
	 * no vertex in the cells along the chunk's max faces, which belong to its seams (world modes review G27).
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param VoxelData Full-resolution (edit-merged) voxel data of the chunk
	 * @param ChunkSize Number of voxels per edge
	 * @param VoxelSize World-space size of each voxel
	 */
	void OnChunkMeshDataReady(const FIntVector& ChunkCoord, const TArray<FVoxelData>& VoxelData, int32 ChunkSize, float VoxelSize);

	/**
	 * Called when a chunk is unloaded.
	 * Cleans up associated scatter data.
	 *
	 * @param ChunkCoord Chunk coordinate
	 */
	void OnChunkUnloaded(const FIntVector& ChunkCoord);

	/**
	 * Regenerate scatter for a chunk (e.g., after system edit).
	 * Clears existing scatter and allows regeneration when new mesh arrives.
	 *
	 * @param ChunkCoord Chunk coordinate
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	void RegenerateChunkScatter(const FIntVector& ChunkCoord);

	/**
	 * Clear scatter in a specific area (e.g., after player edit).
	 * Only removes instances within the edit radius, not the entire chunk.
	 * Prevents regeneration in the cleared area until chunk is fully unloaded/reloaded.
	 *
	 * @param WorldPosition Center of the edit in world space
	 * @param Radius Radius of the edit brush
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	void ClearScatterInRadius(const FVector& WorldPosition, float Radius);

	/**
	 * Check if a point falls within any cleared volume for a chunk.
	 * Used during scatter generation to skip points in player-edited areas.
	 */
	bool IsPointInClearedVolume(const FIntVector& ChunkCoord, const FVector& WorldPosition) const;

	// ==================== Exclusion Volumes ====================

	/**
	 * Register (or replace, by Id) a persistent scatter exclusion volume: an oriented box inside
	 * which scatter never spawns. Existing spawn points inside the volume are cleared immediately
	 * via the smooth per-(chunk,type) replace path (no flash), in-flight async results are filtered
	 * at consumption, and chunks that stream in later are born bare inside the volume. Unlike
	 * ClearScatterInRadius (transient, resets on chunk reload), exclusion volumes persist until
	 * unregistered. The canonical external entry point is UVoxelScatterExclusionSubsystem, which
	 * replays volumes to managers that initialize later.
	 *
	 * @param Volume The volume to register. Must have a valid Id and positive extents.
	 * @return True if the volume was accepted.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter|Exclusion")
	bool RegisterScatterExclusionVolume(const FScatterExclusionVolume& Volume);

	/**
	 * Remove a previously registered exclusion volume. Foliage regrows inside it: overlapped chunks
	 * re-run placement from their cached surface data (async, deterministic seeds) and replace their
	 * instances per type through the smooth update path — identical points outside the volume come
	 * back unchanged, points inside reappear.
	 *
	 * @param VolumeId Id the volume was registered under.
	 * @return True if a volume with this Id existed.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter|Exclusion")
	bool UnregisterScatterExclusionVolume(const FGuid& VolumeId);

	/** True if a volume with this Id is currently registered. */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter|Exclusion")
	bool HasScatterExclusionVolume(const FGuid& VolumeId) const { return ExclusionVolumes.Contains(VolumeId); }

	// ==================== Harvest (gameplay) ====================

	/**
	 * Find the nearest harvestable scatter instance (a definition with a HarvestCategory) within
	 * Radius of a world point, and optionally remove it for good.
	 *
	 * Removal registers an exclusion volume around the instance base (MakeHarvestExclusionVolume),
	 * which clears the instance now, keeps it from regrowing when its chunk re-scatters, and is
	 * replayed to managers created later. Call with bRemove on every machine that renders scatter
	 * (scatter is local): the server decides, then tells the clients.
	 *
	 * @param WorldLocation  Query point (an aim point in front of the player, typically).
	 * @param Radius         Search radius (world units).
	 * @param bRemove        Remove the found instance (exclusion volume).
	 * @param OutResult      What was found.
	 * @return True when a harvestable instance was within Radius.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter|Harvest")
	bool HarvestScatterNear(const FVector& WorldLocation, float Radius, bool bRemove, FScatterHarvestResult& OutResult);

	/**
	 * Pure: the exclusion volume a harvested instance leaves behind — a thin box around its base,
	 * with an Id derived from the base position so every machine builds the same volume.
	 * @param InstanceBase  World position of the instance base.
	 * @param TrunkRadius   Half-extent in X / Y (keeps neighbouring grass).
	 * @param Height        Box height above the base.
	 */
	static FScatterExclusionVolume MakeHarvestExclusionVolume(const FVector& InstanceBase, float TrunkRadius = 60.0f, float Height = 400.0f,
		const FVector& Up = FVector::UpVector);

	/** Pure: was this volume made by MakeHarvestExclusionVolume (a felled instance, worth persisting). */
	UFUNCTION(BlueprintPure, Category = "Voxel|Scatter|Harvest")
	static bool IsHarvestVolume(const FScatterExclusionVolume& Volume) { return Volume.Id.A == 0x48415256u; }

	// ==================== Configuration ====================

	/**
	 * Set the maximum distance for scatter generation.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	void SetScatterRadius(float Radius);

	/**
	 * Set the target spacing between surface sample points.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter")
	void SetSurfacePointSpacing(float Spacing);

	/**
	 * Set the world seed for deterministic scatter.
	 */
	void SetWorldSeed(uint32 Seed);

	/**
	 * Provide the world mode for the analytic terrain-height coverage check during surface extraction.
	 * Lets the extractor classify surface points that are covered by terrain in a neighboring chunk
	 * (cross-chunk cave floors / overhangs) as underground. Non-owning — the ChunkManager owns the world
	 * mode and outlives this manager. Optional: when null, only the per-chunk/flag classification is used.
	 */
	void SetWorldMode(const IVoxelWorldMode* InWorldMode) { WorldMode = InWorldMode; }

	/**
	 * How to gather the terrain conditioning zones over a chunk's world box, the zones generation layered onto it: the
	 * chunk manager's region gather on a heightmap (its XY footprint, world modes review G23), the cap the box subtends
	 * on a planet (GetPlanetCapOfBox + GatherConditioningZonesOnSphere, spherical SP-B10). Called on the game thread
	 * when a chunk's extraction is set up (FScatterExtractionContext::ConditioningZones). Unset: no zones (the world
	 * mode's natural surface).
	 */
	void SetConditioningGatherer(TFunction<void(const FBox& /*ChunkBox*/, TArray<FVoxelConditioningZone>& /*OutZones*/)> InGatherer)
	{
		ConditioningGatherer = MoveTemp(InGatherer);
	}

	/**
	 * Where a chunk's edit layer lives (the chunk manager's edit manager), so a heightmap chunk's extraction knows the
	 * voxel over its top layer, the chunk above's bottom voxel (world modes review G26): MakeExtractionContext copies the
	 * chunk above's edits on that layer into FScatterExtractionContext::AboveLayerEdits. On a planet it copies the edits in
	 * the layers past the chunk's +X, +Y and +Z faces into FScatterExtractionContext::TouchingEdits (G29, G31). Called on
	 * the game thread when a chunk's extraction is set up. Unset: no edits (the generated voxel).
	 */
	void SetEditLayerSource(TFunction<const FChunkEditLayer*(const FIntVector& /*ChunkCoord*/)> InSource)
	{
		EditLayerSource = MoveTemp(InSource);
	}

	/**
	 * How to hand a chunk off again (the chunk manager's HandOffChunkScatter: its edit-merged voxels through
	 * OnChunkMeshDataReady), for a chunk whose hand-off found every type out of range (world modes review G79). A chunk
	 * is handed off only when it submits a mesh, and while walking every chunk arrives at the window's front beyond every
	 * spawn distance, so without this its scatter waited for its next mesh, the LOD refine near the viewer, and popped
	 * there. The distance pass calls it on the game thread for the chunks awaiting their first extraction that have come
	 * within the scatter horizon, nearest first, MaxDistanceSpawnChunksPerPass a pass. Returns false when the chunk has
	 * no voxel data any more (it is dropped). Unset: those chunks wait for their next mesh (the behaviour before).
	 */
	void SetChunkHandOffRequester(TFunction<bool(const FIntVector& /*ChunkCoord*/)> InRequester)
	{
		ChunkHandOffRequester = MoveTemp(InRequester);
	}

	// ==================== Debug ====================

	/**
	 * Draw debug visualization.
	 * Call from tick or debug draw function.
	 *
	 * @param World World to draw in
	 */
	void DrawDebugVisualization(UWorld* World) const;

	/**
	 * Get approximate total memory usage of scatter system in bytes.
	 */
	int64 GetTotalMemoryUsage() const;

	/**
	 * Get statistics for debugging.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter|Debug")
	FScatterStatistics GetStatistics() const;

	/**
	 * Get debug stats as string.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter|Debug")
	FString GetDebugStats() const;

	/**
	 * Restart the worker extraction timing GetDebugStats reports (world modes review G28: chunks extracted on the thread
	 * pool, their extraction and placement time, the surface points they found). voxel.Scatter.Stats reset.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter|Debug")
	void ResetExtractionTiming();

	// ==================== Events ====================

	/** Fired when scatter data is ready for a chunk */
	FOnChunkScatterReady OnChunkScatterReady;

protected:
	// ==================== Internal Methods ====================

	/**
	 * Remove scatter data for a chunk.
	 */
	void RemoveChunkScatter(const FIntVector& ChunkCoord);

	/**
	 * Remove spawn/surface points that fall inside any cleared volume registered for
	 * the chunk, and spawn points inside any persistent exclusion volume. Applied to
	 * async results at consumption time (game thread): a result may have been generated
	 * from a snapshot taken before a player edit registered a new cleared volume (or a
	 * new exclusion volume arrived), so launch-time filtering alone is not sufficient.
	 * Exclusion volumes never remove SURFACE points — the surface cache must stay whole
	 * so unregistering a volume can regrow foliage from it. Either pointer may be null.
	 */
	void ApplyClearedVolumesToResult(const FIntVector& ChunkCoord, FChunkScatterData* ScatterData, FChunkSurfaceData* SurfaceData) const;

	/**
	 * Get chunk world origin for coordinate conversion.
	 */
	FVector GetChunkWorldOrigin(const FIntVector& ChunkCoord) const;

	/**
	 * Create default scatter definitions.
	 */
	void CreateDefaultDefinitions();

protected:
	// ==================== Sub-Managers ====================

	/** Renderer for HISM instances */
	UPROPERTY()
	TObjectPtr<UVoxelScatterRenderer> ScatterRenderer;

	// ==================== Configuration ====================

	/** World configuration reference */
	UPROPERTY()
	TObjectPtr<UVoxelWorldConfiguration> Configuration;

	/** Cached world reference */
	UPROPERTY()
	TObjectPtr<UWorld> CachedWorld;

	/** World mode for analytic terrain-height coverage queries during extraction (non-owning; see SetWorldMode). */
	const IVoxelWorldMode* WorldMode = nullptr;

	/** The chunk's conditioning zones (see SetConditioningGatherer). */
	TFunction<void(const FBox&, TArray<FVoxelConditioningZone>&)> ConditioningGatherer;

	/** A chunk's edit layer (see SetEditLayerSource). */
	TFunction<const FChunkEditLayer*(const FIntVector&)> EditLayerSource;

	/** How to hand a chunk off again (see SetChunkHandOffRequester). */
	TFunction<bool(const FIntVector&)> ChunkHandOffRequester;

	/**
	 * Chunks handed off with every type out of range and nothing cached (G79). The distance pass hands them off again
	 * once within the scatter horizon (ProcessDeferredFirstExtractions); an unload or a regeneration drops them.
	 */
	TSet<FIntVector> ChunksAwaitingFirstExtraction;

	/** Hand-offs the distance pass asked for (GetTotalDeferredFirstExtractions). */
	int64 TotalDeferredFirstExtractions = 0;

	/** A type's spawn distance: its SpawnDistance, or ScatterRadius when 0. */
	float GetEffectiveSpawnDistance(const FScatterDefinition& Def) const
	{
		return Def.SpawnDistance > 0.0f ? Def.SpawnDistance : ScatterRadius;
	}

	/** Whether the hand-off's tree-mode rule lets a type spawn at this distance (VoxelInjection types only). */
	bool PassesTreeModeRule(const FScatterDefinition& Def, float ChunkDistance) const;

	/** G79: hand off the chunks awaiting their first extraction that have come within the scatter horizon, nearest first. */
	void ProcessDeferredFirstExtractions();

	/** Build the cross-chunk classification context from the world mode + configuration (water level, noise params). */
	FScatterExtractionContext MakeExtractionContext(const FIntVector& ChunkCoord) const;

	/** Whether the manager is initialized */
	bool bIsInitialized = false;

	// ==================== Scatter Settings ====================

	/** Maximum distance for scatter generation */
	float ScatterRadius = 10000.0f;

	/** Target spacing between surface sample points (cm) */
	float SurfacePointSpacing = 100.0f;

	/** World seed for deterministic scatter */
	uint32 WorldSeed = 12345;

	// ==================== Scatter Definitions ====================

	/** All scatter type definitions */
	UPROPERTY()
	TArray<FScatterDefinition> ScatterDefinitions;

	// ==================== Data Caches ====================

	/** Per-chunk surface data cache */
	TMap<FIntVector, FChunkSurfaceData> SurfaceDataCache;

	/** Per-chunk scatter data cache */
	TMap<FIntVector, FChunkScatterData> ScatterDataCache;

	/**
	 * Cleared scatter volume - represents an area where scatter should not regenerate.
	 * Used for player edits to surgically remove scatter in the affected area only.
	 */
	struct FClearedScatterVolume
	{
		FVector Center;
		float Radius;

		FClearedScatterVolume() : Center(FVector::ZeroVector), Radius(0.0f) {}
		FClearedScatterVolume(const FVector& InCenter, float InRadius) : Center(InCenter), Radius(InRadius) {}

		bool ContainsPoint(const FVector& Point) const
		{
			return FVector::DistSquared(Center, Point) <= (Radius * Radius);
		}
	};

	/** Per-chunk list of cleared volumes from player edits */
	TMap<FIntVector, TArray<FClearedScatterVolume>> ClearedVolumesPerChunk;

	// ==================== Exclusion Volumes (persistent, world-level) ====================

	/**
	 * Registered exclusion volumes by Id. Persistent across chunk streaming — deliberately NOT
	 * per-chunk and NOT cleared on chunk unload/regeneration (unlike ClearedVolumesPerChunk,
	 * which models transient player-edit clears).
	 */
	TMap<FGuid, FScatterExclusionVolume> ExclusionVolumes;

	/**
	 * Chunks queued to regrow after an exclusion volume was unregistered. Processed a few per
	 * Update tick, and only when the chunk is quiet (no async extraction/stream in flight) so
	 * the replacement result can't interleave with another pipeline's append. Entries for
	 * chunks whose surface cache is gone (unloaded / awaiting re-mesh) drop out harmlessly.
	 */
	TSet<FIntVector> PendingExclusionRegrow;

	/** Max regrow placements to launch per Update tick. */
	int32 MaxExclusionRegrowLaunchesPerTick = 2;

	/** Remove cached spawn points inside Volume and release their instances (smooth per-type replace). */
	void ClearSpawnPointsInExclusionVolume(const FScatterExclusionVolume& Volume);

	/** Queue every loaded chunk overlapping Bounds for a regrow placement pass. */
	void QueueExclusionRegrowForBounds(const FBox& Bounds);

	/** Launch queued regrow placements (throttled; skips chunks with work in flight). */
	void ProcessPendingExclusionRegrow();

	/** True if Position is inside any registered exclusion volume (hot-path helper). */
	bool IsPointExcluded(const FVector& Position) const;

	/**
	 * Definitions eligible at ChunkDistance under the same rules as a fresh mesh hand-off
	 * (enabled, tree-mode HISM gating, per-definition spawn distance). Used by the exclusion
	 * regrow relaunch, which re-places every eligible type from cached surface data.
	 */
	TArray<FScatterDefinition> BuildEligibleDefinitionsForDistance(float ChunkDistance) const;

	/**
	 * Extract surface points from voxel data (LOD-independent).
	 * Scans voxel columns top-down for surface transitions, interpolates exact Z,
	 * computes normals from density gradient, reads MaterialID/BiomeID from voxel data.
	 * Produces consistent results regardless of which LOD level the mesh was generated at.
	 * The voxel over the top layer is the chunk above's: the context's GetVoxelAboveChunk (solid: the top voxel is
	 * no surface and the column below it is covered, world modes review G26; at any depth, caves included, G28).
	 */
	static void ExtractSurfacePointsFromVoxelData(
		const TArray<FVoxelData>& VoxelData,
		const FIntVector& ChunkCoord,
		const FVector& ChunkWorldOrigin,
		int32 ChunkSize,
		float VoxelSize,
		float SurfacePointSpacing,
		const TArray<FClearedScatterVolume>& ClearedVolumes,
		const FScatterExtractionContext& Context,
		FChunkSurfaceData& OutSurfaceData);


	/**
	 * ExtractSurfacePointsFromVoxelData on a planet (spherical SP-G1; world modes review G31): the column scan's columns
	 * are the planet's radial lines, one through the centre of each cell of the cube-sphere grid at the point spacing
	 * (FVoxelCubeSphereGrid, Stride voxels per cell on the datum sphere), the same lattice for every chunk. Down each ray
	 * through the chunk the density is the trilinear interpolation of the voxels (sampled in the cells whose corners change
	 * sign); every rise through the surface threshold going down is a point (the first on the ray open, the rest
	 * underground, as in a column, and all underground when the ray enters the chunk solid), and the chunk keeps the points
	 * that lie in its own box [0, ChunkSize) on each axis, so each crossing has one owner. The chunk's last cell on each
	 * axis reaches the voxels past its +X, +Y and +Z faces: the context's GetPlanetVoxelOutsideChunk, the planet
	 * generator's voxel there with the neighbour's edit. Material, biome and the underground and water flags come from the
	 * crossing cell's nearest own solid and air voxels; the normal is the trilinear gradient; local up is the ray. Face
	 * type, slope and the elevation filter go by the point's radial up and altitude; SpawnWeight is the cell's area at the
	 * point over the column scan's per point, so the weighted density per area is the column scan's. The async pipeline
	 * calls it for planet chunks.
	 */
	static void ExtractSurfacePointsAlongUp(
		const TArray<FVoxelData>& VoxelData,
		const FIntVector& ChunkCoord,
		const FVector& ChunkWorldOrigin,
		int32 ChunkSize,
		float VoxelSize,
		float PointSpacing,
		const TArray<FClearedScatterVolume>& ClearedVolumes,
		const FScatterExtractionContext& Context,
		FChunkSurfaceData& OutSurfaceData);

	/**
	 * Extract surface points using block-face snapping (cubic terrain).
	 * Positions snap to block face centers instead of interpolated positions.
	 * One point per exposed top face, normal is always up, no interpolation.
	 * The block over the top layer is decided as in ExtractSurfacePointsFromVoxelData (G26).
	 */
	static void ExtractSurfacePointsCubic(
		const TArray<FVoxelData>& VoxelData,
		const FIntVector& ChunkCoord,
		const FVector& ChunkWorldOrigin,
		int32 ChunkSize,
		float VoxelSize,
		const TArray<FClearedScatterVolume>& ClearedVolumes,
		const FScatterExtractionContext& Context,
		FChunkSurfaceData& OutSurfaceData);

	/**
	 * The extractor a chunk's pass runs: the block-face scan in cubic (bCubic), the radial rays on a planet (the context's
	 * world mode), the column scan otherwise. The async pass and CompareWithFreshExtraction both call it. Pure and
	 * thread-safe.
	 */
	static void ExtractChunkSurfacePoints(
		bool bCubic,
		const TArray<FVoxelData>& VoxelData,
		const FIntVector& ChunkCoord,
		const FVector& ChunkWorldOrigin,
		int32 ChunkSize,
		float VoxelSize,
		float PointSpacing,
		const TArray<FClearedScatterVolume>& ClearedVolumes,
		const FScatterExtractionContext& Context,
		FChunkSurfaceData& OutSurfaceData);

	// ==================== Debug ====================

	/** Whether debug visualization is enabled */
	bool bDebugVisualization = false;

	/** Last viewer position (for debug drawing) */
	FVector LastViewerPosition = FVector::ZeroVector;

	// ==================== Deferred Generation Queue ====================

	/**
	 * Pending scatter generation request: the chunk's voxels and the definitions to place.
	 */
	struct FPendingScatterGeneration
	{
		FIntVector ChunkCoord;
		float DistanceToViewer;

		// Definitions to generate (captured at queue time based on distance rules)
		TArray<FScatterDefinition> CapturedDefinitions;

		// Voxel data for LOD-independent surface extraction
		TArray<FVoxelData> ChunkVoxelData;
		int32 ChunkSize = VOXEL_DEFAULT_CHUNK_SIZE;
		float VoxelSize = 100.0f;

		bool operator<(const FPendingScatterGeneration& Other) const
		{
			return DistanceToViewer > Other.DistanceToViewer; // Farthest first, closest at back for O(1) pop
		}
	};

	/** Queue of pending scatter generations, sorted by distance */
	TArray<FPendingScatterGeneration> PendingGenerationQueue;

	/** Set of chunks in the pending queue (O(1) duplicate check) */
	TSet<FIntVector> PendingQueueSet;

	/** Maximum scatter generations per frame (0 = unlimited) */
	int32 MaxScatterGenerationsPerFrame = 2;

	/** Process pending scatter generation queue */
	void ProcessPendingGenerationQueue();

	// ==================== Async Scatter Generation ====================

	/** Result from async scatter generation on thread pool */
	struct FAsyncScatterResult
	{
		FIntVector ChunkCoord;
		FChunkSurfaceData SurfaceData;
		FChunkScatterData ScatterData;
		TSet<int32> GeneratedTypeIDs;
		bool bSuccess = false;
		/** Worker time on the chunk (G28 timing): surface extraction, then spawn placement. */
		double ExtractSeconds = 0.0;
		double PlaceSeconds = 0.0;
		/** Surface points the extraction found (0 when it found none: no surface data cached). */
		int32 ExtractedPoints = 0;
	};

	/** The async results' worker timing since the last reset (G28), summed on the game thread as they are consumed. */
	struct FExtractionTiming
	{
		int64 Chunks = 0;
		int64 Points = 0;
		double ExtractSeconds = 0.0;
		double ExtractMaxSeconds = 0.0;
		double PlaceSeconds = 0.0;
		double PlaceMaxSeconds = 0.0;
	};
	FExtractionTiming ExtractionTiming;

	/** MPSC queue for completed async scatter results (written by thread pool, read by game thread) */
	TQueue<FAsyncScatterResult, EQueueMode::Mpsc> CompletedScatterQueue;

	/** Chunks currently being processed asynchronously */
	TSet<FIntVector> AsyncScatterInProgress;

	/** Supplemental passes deferred because async scatter was in-flight.
	 *  E.g., LOD 0 mesh arrived while LOD > 0 trees are being generated.
	 *  Re-queued when async result arrives in ProcessCompletedAsyncScatter(). */
	TMap<FIntVector, FPendingScatterGeneration> DeferredSupplementalPasses;

	/** Per-chunk set of scatter type IDs that have been fully generated.
	 *  Types are cleared when they go out of SpawnDistance range,
	 *  allowing regeneration when the player returns. */
	TMap<FIntVector, TSet<int32>> CompletedScatterTypes;

	/** Maximum concurrent async scatter tasks */
	int32 MaxAsyncScatterTasks = 2;

	// ==================== Distance-Based Streaming ====================

	/**
	 * Generate scatter for definitions that have come into SpawnDistance range.
	 * Launches async tasks on the thread pool using cached surface data.
	 * Fully decoupled from the chunk generation async pipeline — uses its own
	 * DistanceStreamQueue and DistanceStreamInProgress tracking.
	 * Also clears CompletedScatterTypes entries for out-of-range definitions
	 * so they can regenerate when the player returns.
	 */
	void PerformDistanceSpawn();

	/** Process completed distance stream results on game thread */
	void ProcessCompletedDistanceStream();

	/** Result from async distance stream generation */
	struct FDistanceStreamResult
	{
		FIntVector ChunkCoord;
		FChunkScatterData ScatterData;
		TArray<int32> GeneratedTypeIDs;
		bool bSuccess = false;
	};

	/** Dedicated MPSC queue for distance stream results (separate from chunk generation) */
	TQueue<FDistanceStreamResult, EQueueMode::Mpsc> DistanceStreamQueue;

	/** Chunks currently being processed by distance streaming (separate from AsyncScatterInProgress) */
	TSet<FIntVector> DistanceStreamInProgress;

	/** Maximum concurrent distance stream async tasks */
	int32 MaxDistanceStreamTasks = 4;

	/** Maximum distance stream results to process per frame */
	int32 MaxDistanceStreamResultsPerFrame = 8;

	/** Interval between distance streaming checks (seconds) */
	float DistanceStreamingInterval = 0.25f;

	/** Time accumulated since last distance streaming check */
	float TimeSinceLastDistanceCheck = 0.0f;

	/** Hysteresis multiplier for cleanup distance.
	 *  Scatter is removed at SpawnDistance * (1 + Hysteresis) to prevent thrashing.
	 *  E.g., 0.1 means remove at 110% of SpawnDistance, respawn at 100%. */
	float DistanceCleanupHysteresis = 0.1f;

	/** Maximum chunks to launch per distance-spawn pass */
	int32 MaxDistanceSpawnChunksPerPass = 8;

	/** Launch async scatter generation on thread pool */
	void LaunchAsyncScatterGeneration(FPendingScatterGeneration PendingData);

	/** Process completed async scatter results on game thread */
	void ProcessCompletedAsyncScatter();

	// ==================== Statistics ====================

	/** Total chunks processed */
	int64 TotalChunksProcessed = 0;

	/** Surface extractions launched (GetTotalExtractionsLaunched) */
	int64 TotalExtractionsLaunched = 0;

	/** RegenerateChunkScatter calls (GetTotalRegenerations) */
	int64 TotalRegenerations = 0;

	/** The last Update's step timings (GetLastUpdateTimings) */
	FUpdateTimings LastUpdateTimings;

	/** Total surface points extracted */
	int64 TotalSurfacePointsExtracted = 0;

	/** Total spawn points generated */
	int64 TotalSpawnPointsGenerated = 0;
};
