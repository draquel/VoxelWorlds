// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "ChunkRenderData.h"
#include "VoxelVertex.h"
#include "VoxelCrossfadeScheduler.h"
#include "VoxelWorldComponent.generated.h"

class FVoxelSceneProxy;
struct FVoxelChunkDrawInfo;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UVoxelMaterialAtlas;

/**
 * Primitive component for rendering voxel worlds.
 *
 * Creates and manages an FVoxelSceneProxy for GPU-driven rendering.
 * Handles game thread to render thread communication for chunk updates.
 *
 * Thread Safety: All public methods must be called from game thread
 *
 * @see FVoxelSceneProxy
 * @see FVoxelCustomVFRenderer
 * @see Documentation/RENDERING_SYSTEM.md
 */
UCLASS(ClassGroup = (Voxel), meta = (BlueprintSpawnableComponent))
class VOXELRENDERING_API UVoxelWorldComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UVoxelWorldComponent(const FObjectInitializer& ObjectInitializer);
	virtual ~UVoxelWorldComponent();

	// ==================== UPrimitiveComponent Interface ====================

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;
	virtual void SetMaterial(int32 ElementIndex, UMaterialInterface* Material) override;
	virtual int32 GetNumMaterials() const override { return 1; }

	// ==================== Chunk Management ====================

	/**
	 * Update chunk from CPU vertex/index data: the mesh goes straight to the render thread.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param Vertices CPU vertex array (will be moved)
	 * @param Indices CPU index array (will be moved)
	 * @param LODLevel LOD level of the mesh
	 * @param LocalBounds Local bounds of the mesh
	 */
	void UpdateChunkBuffersFromCPUData(
		const FIntVector& ChunkCoord,
		TArray<FVoxelVertex>&& Vertices,
		TArray<uint32>&& Indices,
		int32 LODLevel,
		const FBox& LocalBounds);

	/**
	 * Remove a chunk.
	 * Enqueues removal to render thread.
	 *
	 * @param ChunkCoord Chunk coordinate to remove
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	void RemoveChunk(const FIntVector& ChunkCoord);

	/**
	 * Clear all chunks.
	 * Enqueues clear to render thread.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	void ClearAllChunks();

	/**
	 * Flush all pending chunk operations as a single batched render command.
	 * Called once per frame to consolidate multiple add/remove operations.
	 */
	void FlushPendingOperations();

	// ==================== Mesh-Swap Crossfade ====================
	// Softens every chunk mesh swap (LOD flips AND correction remeshes) by drawing the old
	// and new mesh together for a short dithered crossfade, mesher-agnostic. Controlled by
	// cvars voxel.MeshFade (kill-switch) and voxel.MeshFade.Duration.

	/**
	 * Set the masked fade material used for crossfade draws.
	 * The material must be a Masked-blend variant of the terrain material with scalar
	 * parameters "FadeAlpha" (dither threshold) and "FadeInvert" (0 = dither in, 1 = dither
	 * out) driving OpacityMask via DitherTemporalAA.
	 *
	 * If never called, the component auto-derives it when SetMaterial runs: base material
	 * "<Path>/M_Foo" looks for "<Path>/M_Foo_Fade". No fade material = instant swaps (legacy).
	 * Note: name-derived materials are soft references — packaged games should either assign
	 * this explicitly or ensure the _Fade asset is cooked.
	 *
	 * @param InFadeMaterial Masked fade material, or nullptr to disable crossfading
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|LOD")
	void SetFadeMaterial(UMaterialInterface* InFadeMaterial);

	/** Get the active fade material (explicit or auto-derived), null if crossfade unavailable */
	UFUNCTION(BlueprintPure, Category = "Voxel|LOD")
	UMaterialInterface* GetFadeMaterial() const { return FadeMaterial; }

	// ==================== Water Tile Management ====================

	/**
	 * Set water material and forward to render thread.
	 *
	 * @param InMaterial Water material (typically translucent or Single Layer Water), or nullptr to disable
	 */
	void SetWaterMaterial(UMaterialInterface* InMaterial);

	/**
	 * Update water tile from CPU vertex data. Enqueues render command.
	 *
	 * @param TileCoord 2D tile coordinate (XY chunk column)
	 * @param Vertices CPU vertex array (will be moved)
	 * @param Indices CPU index array (will be moved)
	 * @param TileWorldPosition World position of tile origin
	 */
	void UpdateWaterTileFromCPUData(
		const FIntVector2& TileCoord,
		TArray<FVoxelVertex>&& Vertices,
		TArray<uint32>&& Indices,
		const FVector& TileWorldPosition);

	/**
	 * Remove a water tile. Enqueues render command.
	 *
	 * @param TileCoord 2D tile coordinate to remove
	 */
	void RemoveWaterTile(const FIntVector2& TileCoord);

	/**
	 * Clear all water tiles. Enqueues render command.
	 */
	void ClearAllWaterTiles();

	// ==================== Seam Meshes (seam-ownership P1) ====================

	/**
	 * Update a single-owner face-seam mesh from CPU vertex data. Enqueues render command.
	 * Positions are owner-local; the proxy applies OwnerWorldPosition.
	 *
	 * @param OwnerChunkCoord Owning (min-coordinate) chunk
	 * @param Axis Face axis: 0 = +X, 1 = +Y, 2 = +Z
	 * @param LODLevel Shared LOD the seam was meshed at
	 * @param Vertices CPU vertex array (will be moved)
	 * @param Indices CPU index array (will be moved)
	 * @param OwnerWorldPosition World position of the owner chunk's origin
	 * @param bCrossfade Crossfade from the bucket it replaces (R24). Also fades a NEW bucket in from
	 *        nothing: when its owner is held by a transition (it stays hidden until the release), or
	 *        when the owner lies beyond voxel.MeshFade.AppearMinDistance (R24 P3).
	 */
	void UpdateSeamMeshFromCPUData(
		const FIntVector& OwnerChunkCoord,
		uint8 Axis,
		int32 LODLevel,
		TArray<FVoxelVertex>&& Vertices,
		TArray<uint32>&& Indices,
		const FVector& OwnerWorldPosition,
		bool bCrossfade = false);

	/**
	 * Remove one seam bucket. Enqueues render command.
	 * @param bFadeOut Dither the bucket out instead of dropping it (an empty rebuild during a fade-enabled swap).
	 */
	void RemoveSeamMesh(const FIntVector& OwnerChunkCoord, uint8 Axis, bool bFadeOut = false);

	/** Clear all seam meshes. Enqueues render command. */
	void ClearAllSeamMeshes();

	/** Owners with at least one seam bucket, from the game-thread mirror of the proxy's seam map. */
	void GetSeamMeshOwners(TArray<FIntVector>& OutOwners) const;

	/**
	 * Report (once) that a recreated scene proxy dropped the geometry the old one held. The proxy keeps no
	 * copy of chunk meshes, seam buckets or water tiles, so the owner has to send them again. Game thread.
	 *
	 * @param OutChunks Receives the chunks that had a mesh (reset first)
	 * @return True if a proxy was recreated with geometry in it since the last call
	 */
	bool ConsumeLostRenderData(TArray<FIntVector>& OutChunks);

	/**
	 * Whether this component draws, or is about to draw, anything: chunk meshes, seam buckets, water tiles, active fades
	 * or queued chunk operations. On a planet the renderer keeps one component per render region (spherical Phase 6,
	 * WM-BP7) and retires a region's component once this is false.
	 *
	 * @return True while the component holds render content
	 */
	bool HasRenderContent() const;

	// ==================== Coordinated LOD Transitions (R24) ====================

	/**
	 * Hold a chunk interior's crossfade at alpha 0 (previous mesh fully visible, new mesh uploaded
	 * but dithered out). Set before submitting the new mesh; clearing it starts the fade now. A
	 * release on the same tick as other releases keeps them on one alpha. A failsafe releases holds
	 * older than three seconds on its own (the chunk manager's own cap, voxel.MeshFade.SeamHoldMs,
	 * is shorter; this only catches a hold nobody releases).
	 *
	 * @param ChunkCoord Chunk whose interior swap to hold
	 * @param bHold True to hold, false to release
	 */
	void SetChunkFadeHold(const FIntVector& ChunkCoord, bool bHold);

	/**
	 * Hold every seam bucket fade of an owner at alpha 0 (see SetChunkFadeHold).
	 *
	 * @param OwnerChunkCoord Seam bucket owner
	 * @param bHold True to hold, false to release
	 */
	void SetSeamFadeHold(const FIntVector& OwnerChunkCoord, bool bHold);

	/** True if seam bucket swaps crossfade right now (the chunk crossfade prerequisites). */
	bool CanCrossfadeSeams() const;

	// ==================== First-Appearance Fades (R24 P3) ====================

	/**
	 * Set the viewer position that gates first-appearance fades (the chunk manager's streaming
	 * viewer, sent every tick). Until it is set, new geometry appears at once.
	 *
	 * @param ViewerPosition World-space viewer position
	 */
	void SetFadeViewerPosition(const FVector& ViewerPosition);

	/**
	 * True if a first mesh submitted for this chunk now would fade in from nothing: the crossfade is
	 * on (voxel.MeshFade), the chunk has no mesh yet, a viewer is known, the fade cap has room,
	 * and the chunk's cell is at least voxel.MeshFade.AppearMinDistance from the viewer. The chunk
	 * manager holds such a chunk for its seams (SetChunkFadeHold) before submitting it.
	 *
	 * @param ChunkCoord Chunk about to receive its first mesh
	 * @return True if it would appear with a fade
	 */
	bool WillFadeInChunk(const FIntVector& ChunkCoord) const;

	/**
	 * Lifetime first-appearance fade count, for diagnostics.
	 * @param bSeams True for seam buckets, false for chunk interiors
	 */
	int64 GetFadeAppearCount(bool bSeams) const { return FadeAppearCounts[bSeams ? 1 : 0]; }

	/**
	 * Seconds until the owner's seam bucket finishes its running crossfade (0 when not fading, held,
	 * or seam crossfades are off).
	 *
	 * @param OwnerChunkCoord Seam bucket owner
	 * @return Remaining fade time in seconds
	 */
	float GetSeamFadeRemainingSeconds(const FIntVector& OwnerChunkCoord) const;

	/** Active fades (chunk interiors + seam buckets), for diagnostics. */
	int32 GetActiveFadeCount() const { return ActiveFades.Num(); }

	/**
	 * Lifetime crossfade counters, for diagnostics: fades started and, of those, restarts of a fade
	 * that was still running unheld (each drops the mesh it was fading out of).
	 *
	 * @param bSeams True for seam buckets, false for chunk interiors
	 * @param OutStarts Fades started (including restarts)
	 * @param OutMidFadeRestarts Restarts of a running, unheld fade
	 */
	void GetFadeCounters(bool bSeams, int64& OutStarts, int64& OutMidFadeRestarts) const
	{
		OutStarts = FadeStartCounts[bSeams ? 1 : 0];
		OutMidFadeRestarts = FadeMidRestartCounts[bSeams ? 1 : 0];
	}

	/**
	 * The crossfades this component drives, for diagnostics (voxel.MeshFade.Stats): its tick, the scheduler's fades
	 * (alpha, held, appearing), the hold requests, and the proxy's fade state (waits for the render thread).
	 *
	 * @param MaxEntries Fades to list one by one
	 * @param FocusPoint A world point whose chunk's meshes (here and in the proxy) to report and whose drawing the proxy
	 *                   watches from now on, or null to stop watching
	 * @return One line per item
	 */
	FString GetFadeDebugString(int32 MaxEntries, const FVector* FocusPoint = nullptr) const;

	/**
	 * How the scene proxy draws one chunk interior now (diagnostics and tests, world modes review G74). Waits for the
	 * render thread (FlushRenderingCommands): not for per-frame use.
	 *
	 * @param ChunkCoord The chunk
	 * @param OutInfo    Receives what the proxy holds for it (empty when there is no proxy)
	 * @return True if the proxy holds a current mesh for the chunk
	 */
	bool GetChunkDrawInfo(const FIntVector& ChunkCoord, FVoxelChunkDrawInfo& OutInfo) const;

	/**
	 * What this component and its proxy hold for a water tile and when it was last drawn (world modes review G77;
	 * flushes rendering commands). Game thread.
	 *
	 * @param TileCoord The tile (chunk column)
	 * @param Out       Receives the component's side (key, bounds, registration, proxy) and the proxy's line
	 * @return True if the proxy holds a mesh for the tile
	 */
	bool GetWaterTileDrawInfo(const FIntVector2& TileCoord, FString& Out) const;

	// ==================== Configuration ====================

	/**
	 * Set voxel size in world units.
	 * Should be called before chunks are added.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	void SetVoxelSize(float InVoxelSize);

	/** Get voxel size */
	UFUNCTION(BlueprintPure, Category = "Voxel")
	float GetVoxelSize() const { return VoxelSize; }

	/**
	 * Set chunk world size.
	 * Should be called before chunks are added.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	void SetChunkWorldSize(float InChunkWorldSize);

	/** Get chunk world size */
	UFUNCTION(BlueprintPure, Category = "Voxel")
	float GetChunkWorldSize() const { return ChunkWorldSize; }

	/**
	 * Set world origin offset.
	 * All chunk positions are relative to this origin.
	 * Should be called before chunks are added.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	void SetWorldOrigin(const FVector& InWorldOrigin);

	/** Get world origin offset */
	UFUNCTION(BlueprintPure, Category = "Voxel")
	FVector GetWorldOrigin() const { return WorldOrigin; }

	/**
	 * Classify each vertex's face type (top, side, bottom) against the radial direction from a planet centre instead of
	 * world +Z (spherical SP-D3). The renderer sets it for a planet; the scene proxy reads it when it is created (this
	 * recreates the render state when the value changes).
	 *
	 * @param bRadial Planet: the face type against the radial up
	 * @param Center The planet centre
	 */
	void SetRadialFaceUp(bool bRadial, const FVector& Center);

	/** Whether face types are classified against a planet's radial up (SetRadialFaceUp). */
	bool IsRadialFaceUp() const { return bRadialFaceUp; }

	/** The planet centre the radial face up is measured from. */
	FVector GetFaceUpCenter() const { return FaceUpCenter; }

	// ==================== Material Atlas ====================

	/**
	 * Set the material atlas for this voxel world.
	 * This binds atlas textures and parameters to the material.
	 *
	 * @param InAtlas The material atlas data asset to use
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Materials")
	void SetMaterialAtlas(UVoxelMaterialAtlas* InAtlas);

	/** Get the current material atlas */
	UFUNCTION(BlueprintPure, Category = "Voxel|Materials")
	UVoxelMaterialAtlas* GetMaterialAtlas() const { return MaterialAtlas; }

	/**
	 * Create a dynamic material instance from a master material.
	 * The instance will be configured with atlas parameters.
	 *
	 * @param MasterMaterial The master material to create an instance from
	 * @return The created dynamic material instance
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Materials")
	UMaterialInstanceDynamic* CreateVoxelMaterialInstance(UMaterialInterface* MasterMaterial);

	/**
	 * Update material parameters from the current atlas.
	 * Call this after changing atlas settings to refresh the material.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Materials")
	void UpdateMaterialAtlasParameters();

	/** Get whether smooth meshing mode is enabled */
	UFUNCTION(BlueprintPure, Category = "Voxel|Materials")
	bool GetUseSmoothMeshing() const { return bUseSmoothMeshing; }

	/** Set smooth meshing mode (triplanar vs packed atlas) */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Materials")
	void SetUseSmoothMeshing(bool bUseSmooth);

	// ==================== Queries ====================

	/**
	 * Check if a chunk is loaded.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return True if chunk has data loaded
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel")
	bool IsChunkLoaded(const FIntVector& ChunkCoord) const;

	/**
	 * Get number of loaded chunks.
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel")
	int32 GetLoadedChunkCount() const;

	/**
	 * Get all loaded chunk coordinates.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	void GetLoadedChunks(TArray<FIntVector>& OutChunks) const;

	/**
	 * Get world bounds for a chunk.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param OutBounds Output bounding box
	 * @return True if chunk exists
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel")
	bool GetChunkBounds(const FIntVector& ChunkCoord, FBox& OutBounds) const;

	// ==================== Statistics ====================

	/** Get the GPU memory, in bytes, of the chunk meshes currently held (seam meshes and water tiles not included) */
	UFUNCTION(BlueprintPure, Category = "Voxel|Stats")
	int64 GetGPUMemoryUsage() const;

	/** Get the vertex count of the chunk meshes currently held */
	UFUNCTION(BlueprintPure, Category = "Voxel|Stats")
	int64 GetTotalVertexCount() const;

	/** Get the triangle count of the chunk meshes currently held */
	UFUNCTION(BlueprintPure, Category = "Voxel|Stats")
	int64 GetTotalTriangleCount() const;

protected:
	/** Called when scene proxy is created */
	virtual void SendRenderDynamicData_Concurrent() override;

	/** Mark render state dirty */
	void MarkRenderStateDirtyAndNotify();

private:
	/** Get scene proxy pointer (may be null) */
	FVoxelSceneProxy* GetVoxelSceneProxy() const;

	/** Material used for terrain rendering */
	UPROPERTY(EditAnywhere, Category = "Voxel")
	TObjectPtr<UMaterialInterface> VoxelMaterial;

	/** Water material for water tile rendering */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> WaterMaterial;

	/** Material atlas data asset for texture configuration */
	UPROPERTY(EditAnywhere, Category = "Voxel|Materials")
	TObjectPtr<UVoxelMaterialAtlas> MaterialAtlas;

	/** Enable smooth meshing mode (triplanar projection with Texture2DArrays) vs cubic (packed atlas) */
	UPROPERTY(EditAnywhere, Category = "Voxel|Materials")
	bool bUseSmoothMeshing = false;

	/** Dynamic material instance created from the master material */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DynamicMaterialInstance;

	/** Voxel size in world units */
	UPROPERTY(EditAnywhere, Category = "Voxel", meta = (ClampMin = "1"))
	float VoxelSize = 100.0f;

	/** Chunk world size */
	UPROPERTY(EditAnywhere, Category = "Voxel", meta = (ClampMin = "100"))
	float ChunkWorldSize = 3200.0f;

	/** World origin offset - all chunk positions are relative to this */
	UPROPERTY(VisibleAnywhere, Category = "Voxel")
	FVector WorldOrigin = FVector::ZeroVector;

	/** Face types against a planet's radial up (SetRadialFaceUp) */
	bool bRadialFaceUp = false;

	/** The planet centre for the radial face up */
	FVector FaceUpCenter = FVector::ZeroVector;

	/** Cached chunk data for game thread tracking */
	struct FChunkInfo
	{
		FBox Bounds;
		int32 LODLevel = 0;

		/** This chunk's share of the statistics, subtracted when its mesh is replaced or removed. */
		int64 VertexCount = 0;
		int64 TriangleCount = 0;
		int64 GPUMemory = 0;
	};

	/**
	 * Replace a chunk's contribution to the statistics with a new mesh's (game thread, ChunkInfoLock held).
	 * @param Info          The chunk's tracking entry (fresh or existing)
	 * @param VertexCount   New mesh vertex count
	 * @param TriangleCount New mesh triangle count
	 * @param GPUMemory     New mesh GPU bytes
	 */
	void SetChunkStats(FChunkInfo& Info, int64 VertexCount, int64 TriangleCount, int64 GPUMemory);

	/** Game thread chunk tracking */
	TMap<FIntVector, FChunkInfo> ChunkInfoMap;

	/**
	 * Set by CreateSceneProxy when it replaces a proxy that held geometry (see ConsumeLostRenderData).
	 * Guarded by ChunkInfoLock: CreateSceneProxy can run off the game thread during concurrent render-state creation.
	 */
	bool bRenderDataLost = false;

	/**
	 * Game-thread mirror of the seam buckets submitted to the proxy: owner -> bitmask of live slots.
	 * Lets diagnostics ask which owners have seam geometry without touching render-thread state.
	 */
	TMap<FIntVector, uint8> SeamBucketSlotMasks;

	/** Water tiles sent to the proxy (HasRenderContent). */
	TSet<FIntVector2> WaterTileKeys;

	/**
	 * Each water tile's world box, part of the component's bounds (CalcBounds; guarded by ChunkInfoLock). A region
	 * component whose chunks hold no mesh (air above a lake at the water tiles' Z-0 layer) used to bound only a
	 * chunk-sized box at its location, so the engine culled its water tiles: a lake drawn without water in a block of
	 * chunk columns at a region's corner (world modes review G77).
	 */
	TMap<FIntVector2, FBox> WaterTileWorldBounds;

	// ==================== Batched Render Operations ====================
	// Instead of sending individual render commands, we batch operations
	// and flush them once per frame to reduce render command overhead

	/** Pending chunk data to add (batched) */
	struct FPendingChunkAdd
	{
		FIntVector ChunkCoord;
		TArray<FVoxelVertex> Vertices;
		TArray<uint32> Indices;
		int32 LODLevel;
		FBox LocalBounds;
		FVector ChunkWorldPosition;
		/** Crossfade this swap (chunk already had a mesh and fading is enabled) */
		bool bCrossfade = false;
	};
	TArray<FPendingChunkAdd> PendingAdds;

	/** Pending chunk removals (batched) */
	TArray<FIntVector> PendingRemovals;

	/** Whether we have pending operations to flush */
	bool HasPendingOperations() const { return PendingAdds.Num() > 0 || PendingRemovals.Num() > 0; }

	// ==================== Mesh-Swap Crossfade State ====================

	/** Masked fade material (explicit via SetFadeMaterial, or auto-derived from the base material name) */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> FadeMaterial;

	/** True once SetFadeMaterial was called explicitly — auto-derivation stops overriding it */
	bool bExplicitFadeMaterial = false;

	/** MID pools (GC owners). "In" MIDs have FadeInvert=0, "Out" MIDs FadeInvert=1. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> FadeInMIDPool;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> FadeOutMIDPool;

	/** Currently unused pool entries, ready for reuse */
	TArray<UMaterialInstanceDynamic*> FreeFadeInMIDs;
	TArray<UMaterialInstanceDynamic*> FreeFadeOutMIDs;

	/** Entries retired this tick — reusable next tick, after the render thread stops referencing them */
	TArray<UMaterialInstanceDynamic*> CoolingFadeInMIDs;
	TArray<UMaterialInstanceDynamic*> CoolingFadeOutMIDs;

	/** The fade MIDs of one active mesh-swap crossfade (its clock lives in FadeScheduler) */
	struct FActiveChunkFade
	{
		UMaterialInstanceDynamic* InMID = nullptr;   // owned by FadeInMIDPool
		UMaterialInstanceDynamic* OutMID = nullptr;  // owned by FadeOutMIDPool
	};

	/**
	 * Active crossfades — chunk interiors and seam buckets — keyed like FadeScheduler
	 * (FVoxelCrossfadeScheduler::ChunkKey / SeamKey). Transitioning geometry only: a few dozen.
	 */
	TMap<FIntVector4, FActiveChunkFade> ActiveFades;

	/** See GetFadeCounters: [0] chunk interiors, [1] seam buckets. */
	int64 FadeStartCounts[2] = { 0, 0 };
	int64 FadeMidRestartCounts[2] = { 0, 0 };

	/** See GetFadeAppearCount: [0] chunk interiors, [1] seam buckets. */
	int64 FadeAppearCounts[2] = { 0, 0 };

	/** Streaming viewer for the appearance distance gate (SetFadeViewerPosition). */
	FVector FadeViewerPosition = FVector::ZeroVector;
	bool bHasFadeViewer = false;

	/** Fade timing (start, hold, release, alpha) for every entry in ActiveFades. */
	FVoxelCrossfadeScheduler FadeScheduler;

	/** Hold requests (R24): a fade that starts for one of these starts held at alpha 0. */
	TSet<FIntVector> ChunkFadeHolds;

	/** Seam bucket owners whose fades start held (every slot of the owner). */
	TSet<FIntVector> SeamFadeHolds;

	/** Fades released this tick, waiting for one batched proxy command (sent by FlushPendingOperations / the fade tick). */
	TArray<FIntVector4> PendingHoldReleases;

	/** True if crossfading is possible right now (cvar on, fade material set, proxy alive) */
	bool CanCrossfade() const;

	/**
	 * Start (or restart) a crossfade for a chunk interior or seam bucket whose mesh is about to be
	 * replaced. Enqueues the fade-state attach, which must precede the swap command.
	 * @return False if no fade was started (cap reached or MIDs unavailable) — swap instantly.
	 */
	bool StartFade(const FIntVector4& Key);

	/**
	 * Fade decision for a chunk interior submit: a swap crossfades (an appearing chunk keeps fading in
	 * with its new mesh in place), a first mesh tries an appearance fade.
	 * @param Key          Interior fade key
	 * @param bMeshExisted The chunk already had a mesh
	 * @return True if the submit carries a fade (the proxy keeps the fade state through the swap)
	 */
	bool StartChunkMeshFade(const FIntVector4& Key, bool bMeshExisted);

	/**
	 * Start a first-appearance fade (R24 P3) for geometry that has no mesh yet: only a fade-in MID,
	 * held if a hold is requested for it. Enqueues the fade-state attach before the mesh lands.
	 * @param Key         Interior or seam bucket key
	 * @param bBypassGate Fade regardless of distance and the known viewer (a bucket joining a held transition)
	 * @return False if refused (near the viewer, fades off, cap, MID failure) — show the mesh at once.
	 */
	bool StartAppearFade(const FIntVector4& Key, bool bBypassGate);

	/** Distance from the fade viewer to the nearest point of a chunk's cell (the appearance gate's measure). */
	double GetFadeViewerDistance(const FIntVector& ChunkCoord) const;

	/** Drop a fade without waiting for completion (geometry removed / fade disabled). */
	void CancelFade(const FIntVector4& Key, bool bClearRenderState);

	/** Drop every fade (material change, clear-all). Render-side state is cleared by the caller's command. */
	void CancelAllFades();

	/** Release one held fade (it starts fading now) and queue the proxy-side held-flag clear. */
	void ReleaseHeldFade(const FIntVector4& Key);

	/** Send queued hold releases to the proxy in one command. */
	void FlushHoldReleases();

	/** Enqueue the proxy command that ends a fade (releases the retained previous mesh). */
	void EnqueueClearFadeState(const FIntVector4& Key);

	/** Per-tick fade driver: advances FadeAlpha on the MIDs, batches alphas to the proxy, completes finished fades. */
	void AdvanceCrossfades();

	/** Get a pooled fade MID (creating if needed), atlas parameters freshly copied, FadeInvert/FadeAlpha initialized. */
	UMaterialInstanceDynamic* AcquireFadeMID(bool bInvert);

	/** Return a fade's MIDs to the cooling lists */
	void RetireFadeMIDs(FActiveChunkFade& Fade);

	/** Auto-derive FadeMaterial from the current material's base ("M_Foo" -> "M_Foo_Fade") unless explicitly set */
	void RefreshDerivedFadeMaterial();

	/** Send the fade material + its relevance to a scene proxy (explicit target — CreateSceneProxy syncs before SceneProxy is assigned) */
	void PushFadeMaterialToProxy(FVoxelSceneProxy* TargetProxy);

	/** Critical section for chunk info access */
	mutable FCriticalSection ChunkInfoLock;

	/** Cached total bounds */
	mutable FBox CachedTotalBounds;
	mutable bool bTotalBoundsDirty = true;

	/** CalcBounds produced new bounds that the render scene has not received yet (sent once per tick). */
	mutable bool bRenderBoundsStale = false;

	/**
	 * Have the next tick send the bounds to the render scene (TickComponent: UpdateBounds, then MarkRenderTransformDirty
	 * when they moved), and make sure a tick comes: the component ticks only while fades run, so a region whose content
	 * changes without a fade (a water tile added to a region with no chunk mesh, a chunk with voxel.MeshFade 0) never
	 * refreshed the scene's copy of its bounds, which stayed the registration-time box at the region's origin, and the
	 * engine culled the region wherever that box was out of view (world modes review G77).
	 */
	void RequestRenderBoundsRefresh();

	/** The bounds last pushed to the render scene (MarkRenderTransformDirty), to skip no-op updates. */
	FBox LastSentRenderBounds = FBox(ForceInit);

	/** Statistics: totals over the chunks in ChunkInfoMap (each mesh counted once, removed on unload) */
	int64 CachedVertexCount = 0;
	int64 CachedTriangleCount = 0;
	int64 CachedGPUMemory = 0;
};
