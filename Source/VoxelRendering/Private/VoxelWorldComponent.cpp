// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWorldComponent.h"
#include "VoxelSceneProxy.h"
#include "VoxelRendering.h"
#include "VoxelMaterialAtlas.h"
#include "VoxelMaterialRegistry.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Engine/World.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture2DArray.h"
#include "UObject/SoftObjectPath.h"
#include "VT/RuntimeVirtualTexture.h"

// ==================== Console Variables ====================

// Mesh-swap crossfade (SEAM_OWNERSHIP_ARCHITECTURE.md §5): softens every per-chunk mesh swap
// (LOD flips AND correction remeshes) by dither-crossfading old and new mesh, mesher-agnostic.
static TAutoConsoleVariable<int32> CVarVoxelMeshFade(
	TEXT("voxel.MeshFade"),
	1,
	TEXT("Crossfade chunk mesh swaps (LOD flips and correction remeshes) instead of popping.\n")
	TEXT("  0 = instant swap (legacy pop, for A/B comparison)\n")
	TEXT("  1 = dithered crossfade (default)\n")
	TEXT("Requires a Masked fade material: auto-derived '<Master>_Fade' asset next to the terrain\n")
	TEXT("master material, or assigned via UVoxelWorldComponent::SetFadeMaterial."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarVoxelMeshFadeDuration(
	TEXT("voxel.MeshFade.Duration"),
	0.35f,
	TEXT("Chunk mesh-swap crossfade duration in seconds (default 0.35)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarVoxelMeshFadeSeams(
	TEXT("voxel.MeshFade.Seams"),
	1,
	TEXT("Crossfade seam bucket swaps like chunk swaps (R24): 1 = on (default), 0 = seams swap instantly\n")
	TEXT("(the pre-R24 behaviour, for A/B). Requires voxel.MeshFade."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarVoxelMeshFadeMaxConcurrent(
	TEXT("voxel.MeshFade.MaxConcurrent"),
	512,
	TEXT("Cap on concurrent mesh-swap fades (chunk interiors + seam buckets). Swaps beyond it are instant;\n")
	TEXT("the fade MID pools grow with this number."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarVoxelMeshFadeAppear(
	TEXT("voxel.MeshFade.Appear"),
	1,
	TEXT("Fade new chunks and new seam buckets in from nothing instead of popping them in (R24 P3), beyond\n")
	TEXT("voxel.MeshFade.AppearMinDistance: 1 = on (default), 0 = they appear at once (for A/B). Requires voxel.MeshFade\n")
	TEXT("(and voxel.MeshFade.Seams for seam buckets)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarVoxelMeshFadeAppearMinDistance(
	TEXT("voxel.MeshFade.AppearMinDistance"),
	7000.0f,
	TEXT("Nearest distance (uu, viewer to the closest point of the chunk cell) at which new geometry fades in. Nearer\n")
	TEXT("geometry appears at once, so nothing near the player turns briefly see-through (default 7000, the LOD 0 band edge)."),
	ECVF_Default);

/** Failsafe for a fade hold nobody releases (the chunk manager's own cap, voxel.MeshFade.SeamHoldMs, is 1 s). */
static constexpr double VoxelFadeHoldFailsafeSeconds = 3.0;

/** Scalar parameter names on the fade material */
static const FName VoxelFadeAlphaParamName(TEXT("FadeAlpha"));
static const FName VoxelFadeInvertParamName(TEXT("FadeInvert"));

// ==================== UVoxelWorldComponent ====================

UVoxelWorldComponent::UVoxelWorldComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Component settings. Tick exists only to advance mesh-swap crossfades and stays
	// disabled while no fade is active (StartFade enables it on demand).
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	bTickInEditor = true;

	// Rendering settings
	SetCastShadow(true);
	SetReceivesDecals(true);
	bUseAsOccluder = true;

	// Collision (handled separately, not by this component)
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);

	// Use custom bounds
	bUseAttachParentBound = false;

	// Initialize bounds
	CachedTotalBounds = FBox(ForceInit);
}

UVoxelWorldComponent::~UVoxelWorldComponent()
{
}

// ==================== UPrimitiveComponent Interface ====================

FPrimitiveSceneProxy* UVoxelWorldComponent::CreateSceneProxy()
{
	if (GetWorld() == nullptr)
	{
		return nullptr;
	}

	// Resolve the crossfade fade material (covers materials assigned without SetMaterial)
	RefreshDerivedFadeMaterial();

	// Ensure we have a material
	UMaterialInterface* MaterialToUse = VoxelMaterial;
	if (!MaterialToUse)
	{
		// WARNING: Default material does not compile with FVoxelVertexFactory!
		// Users must create and assign a custom material for the voxel world.
		UE_LOG(LogVoxelRendering, Warning,
			TEXT("UVoxelWorldComponent: No VoxelMaterial set! Create a simple opaque material and assign it. ")
			TEXT("Default materials do not work with the custom vertex factory."));

		// Still use default as a fallback, but rendering will likely fail
		MaterialToUse = UMaterial::GetDefaultMaterial(MD_Surface);
	}

	UE_LOG(LogVoxelRendering, Log, TEXT("UVoxelWorldComponent: Creating scene proxy with material: %s"),
		MaterialToUse ? *MaterialToUse->GetName() : TEXT("nullptr"));

	FVoxelSceneProxy* NewProxy = new FVoxelSceneProxy(this, MaterialToUse);

	// A new proxy starts empty: whatever the previous one held (chunk meshes, seam buckets, water tiles) is
	// gone with it. The engine recreates the proxy on material and render-state changes, including during
	// the first second of play (a runtime material compile), after the first chunks are submitted. The owner
	// sends the geometry again (ConsumeLostRenderData); the first proxy of a component has nothing to lose.
	{
		FScopeLock Lock(&ChunkInfoLock);
		if (ChunkInfoMap.Num() > 0)
		{
			bRenderDataLost = true;
		}
	}

	// Sync water material to the new proxy if already set
	if (WaterMaterial)
	{
		UWorld* World = GetWorld();
		ERHIFeatureLevel::Type FL = World ? World->GetFeatureLevel() : GMaxRHIFeatureLevel;
		FMaterialRelevance WaterRelevance = WaterMaterial->GetRelevance(GetFeatureLevelShaderPlatform(FL));
		UMaterialInterface* WaterMat = WaterMaterial;

		ENQUEUE_RENDER_COMMAND(SyncWaterMaterialToProxy)(
			[NewProxy, WaterMat, WaterRelevance](FRHICommandListImmediate& RHICmdList)
			{
				NewProxy->SetWaterMaterial_RenderThread(WaterMat, WaterRelevance);
			}
		);
	}

	// Sync fade material to the new proxy if crossfading is available
	if (FadeMaterial)
	{
		PushFadeMaterialToProxy(NewProxy);
	}

	return NewProxy;
}

void UVoxelWorldComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	AdvanceCrossfades();

	// Chunks stream in and out every frame: refresh the bounds and hand them to the render scene at most once per
	// tick, and only when they actually moved (UpdateBounds alone does not reach the scene).
	if (bTotalBoundsDirty || bRenderBoundsStale)
	{
		UpdateBounds();
		bRenderBoundsStale = false;
		const FBox NewBox = Bounds.GetBox();
		if (!NewBox.Equals(LastSentRenderBounds, 1.0))
		{
			LastSentRenderBounds = NewBox;
			MarkRenderTransformDirty();
		}
	}
}

FBoxSphereBounds UVoxelWorldComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// The whole terrain is ONE primitive, so the engine frustum- and occlusion-culls it as one box (individual chunks
	// are culled again in GetDynamicMeshElements). This used to be a fixed +-5 km box around the origin: once the
	// player travelled past it, the entire terrain vanished whenever the camera looked away from that box -- in a
	// flash with camera motion, or held still (QA, 2026-10-11, player at X -615,000). Bound what is actually loaded
	// instead: the union of the chunk bounds, padded by a chunk so water tiles, seam strips and crossfades stay inside.
	FBox Total(ForceInit);
	{
		FScopeLock Lock(&ChunkInfoLock);
		if (bTotalBoundsDirty)
		{
			CachedTotalBounds = FBox(ForceInit);
			for (const TPair<FIntVector, FChunkInfo>& Pair : ChunkInfoMap)
			{
				if (Pair.Value.Bounds.IsValid)
				{
					// Chunk bounds are chunk-local (the proxy shifts them the same way for its per-chunk culling).
					CachedTotalBounds += Pair.Value.Bounds.ShiftBy(WorldOrigin + FVector(Pair.Key) * ChunkWorldSize);
				}
			}
			bTotalBoundsDirty = false;
			bRenderBoundsStale = true;
		}
		Total = CachedTotalBounds;
	}

	if (!Total.IsValid)
	{
		// Nothing loaded yet, so nothing to draw: a chunk-sized box at the component.
		return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector(ChunkWorldSize), ChunkWorldSize * UE_SQRT_3);
	}

	// The union is in world space; the component sits at the world origin with identity transform.
	return FBoxSphereBounds(Total.ExpandBy(ChunkWorldSize));
}

UMaterialInterface* UVoxelWorldComponent::GetMaterial(int32 ElementIndex) const
{
	return VoxelMaterial;
}

void UVoxelWorldComponent::SetMaterial(int32 ElementIndex, UMaterialInterface* Material)
{
	if (ElementIndex != 0)
	{
		return;
	}

	if (VoxelMaterial != Material)
	{
		VoxelMaterial = Material;

		// Update render thread
		FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
		if (Proxy && Material)
		{
			// Compute MaterialRelevance on game thread (GetRelevance requires game thread)
			UWorld* World = GetWorld();
			ERHIFeatureLevel::Type FeatureLevel = World ? World->GetFeatureLevel() : GMaxRHIFeatureLevel;
			FMaterialRelevance MatRelevance = Material->GetRelevance(GetFeatureLevelShaderPlatform(FeatureLevel));

			ENQUEUE_RENDER_COMMAND(SetVoxelMaterial)(
				[Proxy, Material, MatRelevance](FRHICommandListImmediate& RHICmdList)
				{
					Proxy->SetMaterial_RenderThread(Material, MatRelevance);
				}
			);
		}

		// Keep the crossfade material paired with the terrain material
		RefreshDerivedFadeMaterial();

		MarkRenderStateDirtyAndNotify();
	}
}

// ==================== Chunk Management ====================

void UVoxelWorldComponent::UpdateChunkBuffers(const FChunkRenderData& RenderData)
{
	check(IsInGameThread());

	if (!RenderData.HasValidGeometry() || !RenderData.HasGPUBuffers())
	{
		// Empty or invalid data - remove chunk if it exists
		RemoveChunk(RenderData.ChunkCoord);
		return;
	}

	// Create GPU data from render data
	FVoxelChunkGPUData GPUData;
	GPUData.ChunkCoord = RenderData.ChunkCoord;
	GPUData.LODLevel = RenderData.LODLevel;
	GPUData.VertexCount = RenderData.VertexCount;
	GPUData.IndexCount = RenderData.IndexCount;
	GPUData.LocalBounds = RenderData.Bounds;
	GPUData.ChunkWorldPosition = WorldOrigin + FVector(RenderData.ChunkCoord) * ChunkWorldSize;
	GPUData.MorphFactor = RenderData.MorphFactor;
	GPUData.bIsVisible = true;
	GPUData.VertexBufferRHI = RenderData.VertexBufferRHI;
	GPUData.IndexBufferRHI = RenderData.IndexBufferRHI;
	GPUData.VertexBufferSRV = RenderData.VertexBufferSRV;

	UpdateChunkBuffersFromGPU(GPUData);
}

void UVoxelWorldComponent::UpdateChunkBuffersFromGPU(const FVoxelChunkGPUData& GPUData)
{
	check(IsInGameThread());

	const FIntVector& ChunkCoord = GPUData.ChunkCoord;

	// Update game thread tracking
	bool bChunkExisted = false;
	{
		FScopeLock Lock(&ChunkInfoLock);

		bChunkExisted = ChunkInfoMap.Contains(ChunkCoord);
		FChunkInfo& Info = ChunkInfoMap.FindOrAdd(ChunkCoord);

		// Chunk-local bounds (CalcBounds and the proxy shift them by the chunk's world position)
		Info.Bounds = GPUData.LocalBounds;
		Info.LODLevel = GPUData.LODLevel;
		Info.bIsVisible = GPUData.bIsVisible;
		SetChunkStats(Info, GPUData.VertexCount, GPUData.IndexCount / 3, GPUData.GetGPUMemoryUsage());

		bTotalBoundsDirty = true;
	}

	// Mesh replacement -> crossfade, first mesh -> appearance fade (decided here on the game thread;
	// the flag travels with the swap so game and render side can never disagree about it). The
	// fade-state attach is enqueued BEFORE the swap so a held fade's flag is in place when the swap lands.
	const FIntVector4 FadeKey = FVoxelCrossfadeScheduler::ChunkKey(ChunkCoord);
	const bool bCrossfade = StartChunkMeshFade(FadeKey, bChunkExisted);
	if (!bCrossfade && ActiveFades.Contains(FadeKey))
	{
		// Mesh replaced without fading (e.g. kill-switch flipped mid-fade) — drop the stale fade
		CancelFade(FadeKey, /*bClearRenderState=*/true);
	}

	// Enqueue render thread update
	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (Proxy)
	{
		FVoxelChunkGPUData GPUDataCopy = GPUData;

		ENQUEUE_RENDER_COMMAND(UpdateVoxelChunk)(
			[Proxy, ChunkCoord, GPUDataCopy, bCrossfade](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->UpdateChunkBuffers_RenderThread(RHICmdList, ChunkCoord, GPUDataCopy, bCrossfade);
			}
		);
	}

	// Update bounds
	UpdateBounds();
}

void UVoxelWorldComponent::UpdateChunkBuffersFromCPUData(
	const FIntVector& ChunkCoord,
	TArray<FVoxelVertex>&& Vertices,
	TArray<uint32>&& Indices,
	int32 LODLevel,
	const FBox& LocalBounds)
{
	check(IsInGameThread());

	const uint32 VertexCount = Vertices.Num();
	const uint32 IndexCount = Indices.Num();

	if (VertexCount == 0 || IndexCount == 0)
	{
		RemoveChunk(ChunkCoord);
		return;
	}

	// Update game thread tracking
	bool bChunkExisted = false;
	{
		FScopeLock Lock(&ChunkInfoLock);

		bChunkExisted = ChunkInfoMap.Contains(ChunkCoord);
		FChunkInfo& Info = ChunkInfoMap.FindOrAdd(ChunkCoord);
		Info.Bounds = LocalBounds;
		Info.LODLevel = LODLevel;
		Info.bIsVisible = true;
		SetChunkStats(Info, VertexCount, IndexCount / 3,
			static_cast<int64>(VertexCount) * sizeof(FVoxelVertex) + static_cast<int64>(IndexCount) * sizeof(uint32));

		bTotalBoundsDirty = true;
	}

	// Calculate chunk world position (includes WorldOrigin offset)
	FVector ChunkWorldPos = WorldOrigin + FVector(ChunkCoord) * ChunkWorldSize;

	// Mesh replacement -> crossfade, first mesh -> appearance fade. The flag rides the pending add
	// into the batched swap; the fade-state attach lands BEFORE the flush, so a held fade's flag is
	// in place when the swap retires the old mesh.
	const FIntVector4 FadeKey = FVoxelCrossfadeScheduler::ChunkKey(ChunkCoord);
	const bool bCrossfade = StartChunkMeshFade(FadeKey, bChunkExisted);

	// Queue for batched submission instead of immediate render command
	FPendingChunkAdd PendingAdd;
	PendingAdd.ChunkCoord = ChunkCoord;
	PendingAdd.Vertices = MoveTemp(Vertices);
	PendingAdd.Indices = MoveTemp(Indices);
	PendingAdd.LODLevel = LODLevel;
	PendingAdd.LocalBounds = LocalBounds;
	PendingAdd.ChunkWorldPosition = ChunkWorldPos;
	PendingAdd.bCrossfade = bCrossfade;
	PendingAdds.Add(MoveTemp(PendingAdd));

	if (!bCrossfade && ActiveFades.Contains(FadeKey))
	{
		CancelFade(FadeKey, /*bClearRenderState=*/true);
	}

	// Update bounds
	UpdateBounds();
}

void UVoxelWorldComponent::RemoveChunk(const FIntVector& ChunkCoord)
{
	check(IsInGameThread());

	// Update game thread tracking (and take the chunk's mesh out of the statistics)
	{
		FScopeLock Lock(&ChunkInfoLock);

		if (FChunkInfo* Info = ChunkInfoMap.Find(ChunkCoord))
		{
			SetChunkStats(*Info, 0, 0, 0);
			ChunkInfoMap.Remove(ChunkCoord);
			bTotalBoundsDirty = true;
		}
	}

	// A removed chunk cannot keep fading. Clear the proxy-side state immediately (the batched
	// removal would too, but its flush cadence should not gate MID recycling safety).
	ChunkFadeHolds.Remove(ChunkCoord);
	const FIntVector4 FadeKey = FVoxelCrossfadeScheduler::ChunkKey(ChunkCoord);
	if (ActiveFades.Contains(FadeKey))
	{
		CancelFade(FadeKey, /*bClearRenderState=*/true);
	}

	// Queue for batched submission instead of immediate render command
	PendingRemovals.AddUnique(ChunkCoord);

	UpdateBounds();
}

void UVoxelWorldComponent::ClearAllChunks()
{
	check(IsInGameThread());

	// Clear game thread tracking
	{
		FScopeLock Lock(&ChunkInfoLock);
		ChunkInfoMap.Empty();
		bTotalBoundsDirty = true;
	}

	// Drop all crossfades, chunk and seam (the clear commands below release the proxy-side state)
	CancelAllFades();
	ChunkFadeHolds.Empty();
	SeamFadeHolds.Empty();

	// Reset statistics
	CachedVertexCount = 0;
	CachedTriangleCount = 0;
	CachedGPUMemory = 0;

	// Clear any pending batched operations - they're now obsolete
	PendingAdds.Empty();
	PendingRemovals.Empty();
	SeamBucketSlotMasks.Empty(); // the render command below clears the seam meshes too

	// Enqueue render thread clear (chunks + water tiles + seam meshes)
	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (Proxy)
	{
		ENQUEUE_RENDER_COMMAND(ClearVoxelChunksAndWater)(
			[Proxy](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->ClearAllChunks_RenderThread();
				Proxy->ClearAllWaterTiles_RenderThread();
				Proxy->ClearAllSeamMeshes_RenderThread();
			}
		);
	}

	UpdateBounds();
}

// ==================== Water Tile Management ====================

void UVoxelWorldComponent::SetWaterMaterial(UMaterialInterface* InMaterial)
{
	check(IsInGameThread());

	WaterMaterial = InMaterial;

	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (Proxy && InMaterial)
	{
		// Compute MaterialRelevance on game thread (GetRelevance requires game thread)
		UWorld* World = GetWorld();
		ERHIFeatureLevel::Type FL = World ? World->GetFeatureLevel() : GMaxRHIFeatureLevel;
		FMaterialRelevance WaterRelevance = InMaterial->GetRelevance(GetFeatureLevelShaderPlatform(FL));

		ENQUEUE_RENDER_COMMAND(SetVoxelWaterMaterial)(
			[Proxy, InMaterial, WaterRelevance](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->SetWaterMaterial_RenderThread(InMaterial, WaterRelevance);
			}
		);
	}
	else if (Proxy && !InMaterial)
	{
		// Clear water material
		ENQUEUE_RENDER_COMMAND(ClearVoxelWaterMaterial)(
			[Proxy](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->SetWaterMaterial_RenderThread(nullptr, FMaterialRelevance());
			}
		);
	}

	MarkRenderStateDirtyAndNotify();
}

void UVoxelWorldComponent::UpdateWaterTileFromCPUData(
	const FIntVector2& TileCoord,
	TArray<FVoxelVertex>&& Vertices,
	TArray<uint32>&& Indices,
	const FVector& TileWorldPosition)
{
	check(IsInGameThread());

	if (Vertices.Num() == 0 || Indices.Num() == 0)
	{
		RemoveWaterTile(TileCoord);
		return;
	}

	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (Proxy)
	{
		ENQUEUE_RENDER_COMMAND(UpdateVoxelWaterTile)(
			[Proxy, TileCoord, Verts = MoveTemp(Vertices), Idxs = MoveTemp(Indices), TileWorldPosition](FRHICommandListImmediate& RHICmdList) mutable
			{
				Proxy->UpdateWaterTileFromCPUData_RenderThread(RHICmdList, TileCoord, MoveTemp(Verts), MoveTemp(Idxs), TileWorldPosition);
			}
		);
	}
}

void UVoxelWorldComponent::RemoveWaterTile(const FIntVector2& TileCoord)
{
	check(IsInGameThread());

	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (Proxy)
	{
		ENQUEUE_RENDER_COMMAND(RemoveVoxelWaterTile)(
			[Proxy, TileCoord](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->RemoveWaterTile_RenderThread(TileCoord);
			}
		);
	}
}

void UVoxelWorldComponent::ClearAllWaterTiles()
{
	check(IsInGameThread());

	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (Proxy)
	{
		ENQUEUE_RENDER_COMMAND(ClearVoxelWaterTiles)(
			[Proxy](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->ClearAllWaterTiles_RenderThread();
			}
		);
	}
}

// ==================== Seam Meshes (seam-ownership P1) ====================

void UVoxelWorldComponent::UpdateSeamMeshFromCPUData(
	const FIntVector& OwnerChunkCoord,
	uint8 Axis,
	int32 LODLevel,
	TArray<FVoxelVertex>&& Vertices,
	TArray<uint32>&& Indices,
	const FVector& OwnerWorldPosition,
	bool bCrossfade)
{
	check(IsInGameThread());

	if (Vertices.Num() == 0 || Indices.Num() == 0)
	{
		RemoveSeamMesh(OwnerChunkCoord, Axis, /*bFadeOut*/ bCrossfade);
		return;
	}

	const uint8 SlotBit = static_cast<uint8>(1u << (Axis & 7));
	uint8& SlotMask = SeamBucketSlotMasks.FindOrAdd(OwnerChunkCoord);
	const bool bExisted = (SlotMask & SlotBit) != 0;
	SlotMask |= SlotBit;

	// Crossfade a replaced bucket like a chunk swap (R24). A NEW bucket fades in from nothing: always
	// when its owner is held by a transition (it belongs to the incoming surface and must stay hidden
	// until the transition releases, then fade in with the interior it meets), otherwise when it lies
	// beyond voxel.MeshFade.AppearMinDistance (R24 P3: frontier seams stop popping in). A bucket that is
	// still appearing keeps fading in on the same alpha, the new merge replacing it in place.
	const FIntVector4 FadeKey = FVoxelCrossfadeScheduler::SeamKey(OwnerChunkCoord, Axis);
	const bool bOwnerHeld = SeamFadeHolds.Contains(OwnerChunkCoord);
	bool bFade = false;
	if (bCrossfade && CanCrossfadeSeams())
	{
		if (bExisted)
		{
			bFade = FadeScheduler.IsAppearing(FadeKey) || StartFade(FadeKey);
		}
		else if (ActiveFades.Contains(FadeKey))
		{
			// The slot's previous bucket is still fading out to nothing. A held owner keeps that remnant
			// as the outgoing half of the restarted fade; otherwise the new bucket replaces it at once.
			bFade = bOwnerHeld && StartFade(FadeKey);
		}
		else
		{
			bFade = StartAppearFade(FadeKey, /*bBypassGate*/ bOwnerHeld);
		}
	}
	if (!bFade && ActiveFades.Contains(FadeKey))
	{
		CancelFade(FadeKey, /*bClearRenderState=*/true);
	}

	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (Proxy)
	{
		ENQUEUE_RENDER_COMMAND(UpdateVoxelSeamMesh)(
			[Proxy, OwnerChunkCoord, Axis, LODLevel, Verts = MoveTemp(Vertices), Idxs = MoveTemp(Indices), OwnerWorldPosition, bFade](FRHICommandListImmediate& RHICmdList) mutable
			{
				Proxy->UpdateSeamMeshFromCPUData_RenderThread(RHICmdList, OwnerChunkCoord, Axis, LODLevel, MoveTemp(Verts), MoveTemp(Idxs), OwnerWorldPosition, bFade);
			}
		);
	}
}

void UVoxelWorldComponent::RemoveSeamMesh(const FIntVector& OwnerChunkCoord, uint8 Axis, bool bFadeOut)
{
	check(IsInGameThread());

	bool bExisted = false;
	if (uint8* Mask = SeamBucketSlotMasks.Find(OwnerChunkCoord))
	{
		const uint8 SlotBit = static_cast<uint8>(1u << (Axis & 7));
		bExisted = (*Mask & SlotBit) != 0;
		*Mask &= static_cast<uint8>(~SlotBit);
		if (*Mask == 0)
		{
			SeamBucketSlotMasks.Remove(OwnerChunkCoord);
		}
	}

	// Fade to nothing (R24): a seam that dissolves during a fade-enabled rebuild dithers out with
	// the surface it belonged to instead of vanishing on the spot. A bucket still appearing just goes
	// (it is not fully shown; fading it out would first snap it to solid).
	const FIntVector4 FadeKey = FVoxelCrossfadeScheduler::SeamKey(OwnerChunkCoord, Axis);
	const bool bFade = bFadeOut && bExisted && CanCrossfadeSeams() && !FadeScheduler.IsAppearing(FadeKey) && StartFade(FadeKey);
	if (!bFade && ActiveFades.Contains(FadeKey))
	{
		// The removal command below releases the proxy-side fade state with the bucket.
		CancelFade(FadeKey, /*bClearRenderState=*/false);
	}

	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (Proxy)
	{
		ENQUEUE_RENDER_COMMAND(RemoveVoxelSeamMesh)(
			[Proxy, OwnerChunkCoord, Axis, bFade](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->RemoveSeamMesh_RenderThread(OwnerChunkCoord, Axis, bFade);
			}
		);
	}
}

void UVoxelWorldComponent::ClearAllSeamMeshes()
{
	check(IsInGameThread());

	SeamBucketSlotMasks.Empty();
	SeamFadeHolds.Empty();

	// Seam fades end with their buckets (ClearAllSeamMeshes_RenderThread releases the proxy side).
	TArray<FIntVector4> SeamFadeKeys;
	for (const TPair<FIntVector4, FActiveChunkFade>& Pair : ActiveFades)
	{
		if (FVoxelCrossfadeScheduler::IsSeamKey(Pair.Key))
		{
			SeamFadeKeys.Add(Pair.Key);
		}
	}
	for (const FIntVector4& Key : SeamFadeKeys)
	{
		CancelFade(Key, /*bClearRenderState=*/false);
	}

	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (Proxy)
	{
		ENQUEUE_RENDER_COMMAND(ClearVoxelSeamMeshes)(
			[Proxy](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->ClearAllSeamMeshes_RenderThread();
			}
		);
	}
}

void UVoxelWorldComponent::SetChunkVisible(const FIntVector& ChunkCoord, bool bNewVisibility)
{
	check(IsInGameThread());

	// Update game thread tracking
	{
		FScopeLock Lock(&ChunkInfoLock);

		FChunkInfo* Info = ChunkInfoMap.Find(ChunkCoord);
		if (Info)
		{
			Info->bIsVisible = bNewVisibility;
			bTotalBoundsDirty = true;
		}
	}

	// Enqueue render thread update
	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (Proxy)
	{
		ENQUEUE_RENDER_COMMAND(SetVoxelChunkVisible)(
			[Proxy, ChunkCoord, bNewVisibility](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->SetChunkVisible_RenderThread(ChunkCoord, bNewVisibility);
			}
		);
	}
}

void UVoxelWorldComponent::UpdateChunkMorphFactor(const FIntVector& ChunkCoord, float MorphFactor)
{
	check(IsInGameThread());

	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (Proxy)
	{
		ENQUEUE_RENDER_COMMAND(UpdateVoxelMorphFactor)(
			[Proxy, ChunkCoord, MorphFactor](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->UpdateChunkMorphFactor_RenderThread(ChunkCoord, MorphFactor);
			}
		);
	}
}

// ==================== Mesh-Swap Crossfade ====================

void UVoxelWorldComponent::SetFadeMaterial(UMaterialInterface* InFadeMaterial)
{
	check(IsInGameThread());

	bExplicitFadeMaterial = true;

	if (FadeMaterial == InFadeMaterial)
	{
		return;
	}

	// Active fades reference MIDs of the old material — finish them instantly
	if (ActiveFades.Num() > 0)
	{
		CancelAllFades();

		if (FVoxelSceneProxy* Proxy = GetVoxelSceneProxy())
		{
			ENQUEUE_RENDER_COMMAND(ClearVoxelChunkFades)(
				[Proxy](FRHICommandListImmediate& RHICmdList)
				{
					Proxy->ClearAllChunkFadeStates_RenderThread();
				}
			);
		}
	}

	// Old-material MIDs cannot be reused (wrong parent) — let GC collect them
	FadeInMIDPool.Empty();
	FadeOutMIDPool.Empty();
	FreeFadeInMIDs.Empty();
	FreeFadeOutMIDs.Empty();
	CoolingFadeInMIDs.Empty();
	CoolingFadeOutMIDs.Empty();

	FadeMaterial = InFadeMaterial;
	PushFadeMaterialToProxy(GetVoxelSceneProxy());

	UE_LOG(LogVoxelRendering, Log, TEXT("UVoxelWorldComponent: Fade material set to %s (crossfade %s)"),
		FadeMaterial ? *FadeMaterial->GetName() : TEXT("null"),
		FadeMaterial ? TEXT("available") : TEXT("disabled"));
}

bool UVoxelWorldComponent::CanCrossfade() const
{
	return CVarVoxelMeshFade.GetValueOnGameThread() != 0
		&& FadeMaterial != nullptr
		&& GetVoxelSceneProxy() != nullptr;
}

bool UVoxelWorldComponent::CanCrossfadeSeams() const
{
	return CVarVoxelMeshFadeSeams.GetValueOnGameThread() != 0 && CanCrossfade();
}

void UVoxelWorldComponent::SetFadeViewerPosition(const FVector& ViewerPosition)
{
	FadeViewerPosition = ViewerPosition;
	bHasFadeViewer = true;
}

/** voxel.MeshFade.AppearMinDistance, never negative (the cvar voxel.MeshFade.Appear is the off switch). */
static double GetVoxelAppearMinDistance()
{
	return FMath::Max(0.0, static_cast<double>(CVarVoxelMeshFadeAppearMinDistance.GetValueOnGameThread()));
}

double UVoxelWorldComponent::GetFadeViewerDistance(const FIntVector& ChunkCoord) const
{
	// Nearest point of the chunk's cell: a seam bucket lies on its owner's max faces, so the owner's
	// cell bounds it from below too. Nothing inside the gate ever fades, wherever its vertices are.
	const FVector CellMin = WorldOrigin + FVector(ChunkCoord) * ChunkWorldSize;
	const FBox Cell(CellMin, CellMin + FVector(ChunkWorldSize));
	return FMath::Sqrt(Cell.ComputeSquaredDistanceToPoint(FadeViewerPosition));
}

bool UVoxelWorldComponent::WillFadeInChunk(const FIntVector& ChunkCoord) const
{
	if (!bHasFadeViewer || CVarVoxelMeshFadeAppear.GetValueOnGameThread() == 0 || !CanCrossfade() || IsChunkLoaded(ChunkCoord))
	{
		return false;
	}
	const FIntVector4 Key = FVoxelCrossfadeScheduler::ChunkKey(ChunkCoord);
	return !FadeScheduler.Contains(Key)
		&& FadeScheduler.Num() < CVarVoxelMeshFadeMaxConcurrent.GetValueOnGameThread()
		&& GetFadeViewerDistance(ChunkCoord) >= GetVoxelAppearMinDistance();
}

bool UVoxelWorldComponent::StartChunkMeshFade(const FIntVector4& Key, bool bMeshExisted)
{
	if (!CanCrossfade())
	{
		return false;
	}
	if (bMeshExisted)
	{
		// A chunk still appearing keeps fading in: the new mesh replaces the half-shown one in place,
		// on the same alpha (there is nothing behind it to crossfade from).
		return FadeScheduler.IsAppearing(Key) || StartFade(Key);
	}
	return StartAppearFade(Key, /*bBypassGate*/ false);
}

bool UVoxelWorldComponent::StartAppearFade(const FIntVector4& Key, bool bBypassGate)
{
	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	UWorld* World = GetWorld();
	if (!Proxy || !World)
	{
		return false;
	}

	const bool bSeam = FVoxelCrossfadeScheduler::IsSeamKey(Key);
	const FIntVector Coord = FVoxelCrossfadeScheduler::KeyCoord(Key);
	if (bSeam ? !CanCrossfadeSeams() : !CanCrossfade())
	{
		return false;
	}

	// A bucket joining a held transition fades in with it wherever it is (it must stay hidden until the
	// release). Anything else needs the appear switch, a known viewer and the distance gate.
	double Distance = TNumericLimits<double>::Max();
	if (!bBypassGate)
	{
		if (CVarVoxelMeshFadeAppear.GetValueOnGameThread() == 0 || !bHasFadeViewer)
		{
			return false;
		}
		Distance = GetFadeViewerDistance(Coord);
	}

	const bool bHold = bSeam ? SeamFadeHolds.Contains(Coord) : ChunkFadeHolds.Contains(Coord);
	FadeScheduler.SetMaxConcurrent(CVarVoxelMeshFadeMaxConcurrent.GetValueOnGameThread());
	FadeScheduler.SetAppearMinDistance(GetVoxelAppearMinDistance());
	if (!FadeScheduler.StartAppear(Key, World->GetRealTimeSeconds(), bHold, Distance))
	{
		return false;
	}

	// Only the fade-in MID: there is no previous mesh to dither out (a later swap acquires the other).
	FActiveChunkFade& Fade = ActiveFades.FindOrAdd(Key);
	if (!Fade.InMID)
	{
		Fade.InMID = AcquireFadeMID(/*bInvert=*/false);
	}
	if (!Fade.InMID)
	{
		UE_LOG(LogVoxelRendering, Warning,
			TEXT("StartAppearFade: failed to create a fade MID for %s %s — showing it at once"),
			bSeam ? TEXT("seam bucket") : TEXT("chunk"), *Coord.ToString());
		CancelFade(Key, /*bClearRenderState=*/true);
		return false;
	}
	Fade.InMID->SetScalarParameterValue(VoxelFadeAlphaParamName, 0.0f);
	const FMaterialRenderProxy* FadeInProxy = Fade.InMID->GetRenderProxy();

	if (bSeam)
	{
		const uint8 Slot = static_cast<uint8>(Key.W);
		ENQUEUE_RENDER_COMMAND(AttachVoxelSeamAppearFade)(
			[Proxy, Coord, Slot, FadeInProxy, bHold](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->SetSeamFadeState_RenderThread(Coord, Slot, FadeInProxy, nullptr, bHold, /*bAppear*/ true);
			}
		);
	}
	else
	{
		ENQUEUE_RENDER_COMMAND(AttachVoxelChunkAppearFade)(
			[Proxy, Coord, FadeInProxy, bHold](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->SetChunkFadeState_RenderThread(Coord, FadeInProxy, nullptr, bHold, /*bAppear*/ true);
			}
		);
	}

	++FadeAppearCounts[bSeam ? 1 : 0];
	SetComponentTickEnabled(true);
	return true;
}

float UVoxelWorldComponent::GetSeamFadeRemainingSeconds(const FIntVector& OwnerChunkCoord) const
{
	const UWorld* World = GetWorld();
	if (!World || !CanCrossfadeSeams())
	{
		return 0.0f;
	}
	const double Now = World->GetRealTimeSeconds();
	const float Duration = CVarVoxelMeshFadeDuration.GetValueOnGameThread();
	float Remaining = 0.0f;
	for (uint8 Slot = 0; Slot <= 6; ++Slot)
	{
		Remaining = FMath::Max(Remaining, FadeScheduler.GetRemainingSeconds(FVoxelCrossfadeScheduler::SeamKey(OwnerChunkCoord, Slot), Now, Duration));
	}
	return Remaining;
}

bool UVoxelWorldComponent::StartFade(const FIntVector4& Key)
{
	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	UWorld* World = GetWorld();
	if (!Proxy || !World)
	{
		return false;
	}

	const bool bSeam = FVoxelCrossfadeScheduler::IsSeamKey(Key);
	const FIntVector Coord = FVoxelCrossfadeScheduler::KeyCoord(Key);
	const bool bHold = bSeam ? SeamFadeHolds.Contains(Coord) : ChunkFadeHolds.Contains(Coord);

	// A resubmit during an active fade restarts it (the proxy keeps one retained generation) —
	// same MIDs, alpha back to 0. Over the concurrent cap a NEW fade is refused: swap instantly.
	FadeScheduler.SetMaxConcurrent(CVarVoxelMeshFadeMaxConcurrent.GetValueOnGameThread());
	// Diagnostics: a resubmit while the key fades unheld (alpha < 1) restarts it and drops the mesh it was
	// fading out of (one retained generation). The seam pipeline defers such resubmits; this shows if any
	// slip through. A fade at alpha 1 that AdvanceCrossfades has not retired yet has nothing left to drop.
	{
		const int32 Kind = bSeam ? 1 : 0;
		++FadeStartCounts[Kind];
		if (FadeScheduler.GetRemainingSeconds(Key, World->GetRealTimeSeconds(), CVarVoxelMeshFadeDuration.GetValueOnGameThread()) > 0.0f)
		{
			++FadeMidRestartCounts[Kind];
		}
	}
	if (!FadeScheduler.Start(Key, World->GetRealTimeSeconds(), bHold))
	{
		return false;
	}

	FActiveChunkFade& Fade = ActiveFades.FindOrAdd(Key);
	if (!Fade.InMID)
	{
		Fade.InMID = AcquireFadeMID(/*bInvert=*/false);
	}
	if (!Fade.OutMID)
	{
		Fade.OutMID = AcquireFadeMID(/*bInvert=*/true);
	}
	if (!Fade.InMID || !Fade.OutMID)
	{
		// MID creation failed — no fade; the caller swaps instantly. Clear any render-side state a
		// previous fade of this key left, so nothing lingers.
		UE_LOG(LogVoxelRendering, Warning,
			TEXT("StartFade: failed to create fade MIDs for %s %s — swapping instantly"),
			bSeam ? TEXT("seam bucket") : TEXT("chunk"), *Coord.ToString());
		CancelFade(Key, /*bClearRenderState=*/true);
		return false;
	}

	Fade.InMID->SetScalarParameterValue(VoxelFadeAlphaParamName, 0.0f);
	Fade.OutMID->SetScalarParameterValue(VoxelFadeAlphaParamName, 0.0f);

	const FMaterialRenderProxy* FadeInProxy = Fade.InMID->GetRenderProxy();
	const FMaterialRenderProxy* FadeOutProxy = Fade.OutMID->GetRenderProxy();

	if (bSeam)
	{
		const uint8 Slot = static_cast<uint8>(Key.W);
		ENQUEUE_RENDER_COMMAND(AttachVoxelSeamFade)(
			[Proxy, Coord, Slot, FadeInProxy, FadeOutProxy, bHold](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->SetSeamFadeState_RenderThread(Coord, Slot, FadeInProxy, FadeOutProxy, bHold);
			}
		);
	}
	else
	{
		ENQUEUE_RENDER_COMMAND(AttachVoxelChunkFade)(
			[Proxy, Coord, FadeInProxy, FadeOutProxy, bHold](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->SetChunkFadeState_RenderThread(Coord, FadeInProxy, FadeOutProxy, bHold);
			}
		);
	}

	SetComponentTickEnabled(true);
	return true;
}

void UVoxelWorldComponent::EnqueueClearFadeState(const FIntVector4& Key)
{
	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (!Proxy)
	{
		return;
	}
	const FIntVector Coord = FVoxelCrossfadeScheduler::KeyCoord(Key);
	if (FVoxelCrossfadeScheduler::IsSeamKey(Key))
	{
		const uint8 Slot = static_cast<uint8>(Key.W);
		ENQUEUE_RENDER_COMMAND(ClearVoxelSeamFade)(
			[Proxy, Coord, Slot](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->ClearSeamFadeState_RenderThread(Coord, Slot);
			}
		);
	}
	else
	{
		ENQUEUE_RENDER_COMMAND(ClearVoxelChunkFade)(
			[Proxy, Coord](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->ClearChunkFadeState_RenderThread(Coord);
			}
		);
	}
}

void UVoxelWorldComponent::CancelFade(const FIntVector4& Key, bool bClearRenderState)
{
	FadeScheduler.Cancel(Key);

	FActiveChunkFade Fade;
	if (ActiveFades.RemoveAndCopyValue(Key, Fade))
	{
		RetireFadeMIDs(Fade);
	}

	if (bClearRenderState)
	{
		EnqueueClearFadeState(Key);
	}
}

void UVoxelWorldComponent::CancelAllFades()
{
	for (auto& FadePair : ActiveFades)
	{
		RetireFadeMIDs(FadePair.Value);
	}
	ActiveFades.Empty();

	TArray<FIntVector4> Cancelled;
	FadeScheduler.CancelAll(Cancelled);
	PendingHoldReleases.Reset();
}

void UVoxelWorldComponent::ReleaseHeldFade(const FIntVector4& Key)
{
	UWorld* World = GetWorld();
	if (World && FadeScheduler.Release(Key, World->GetRealTimeSeconds()))
	{
		PendingHoldReleases.Add(Key);
	}
}

void UVoxelWorldComponent::FlushHoldReleases()
{
	if (PendingHoldReleases.Num() == 0)
	{
		return;
	}
	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (!Proxy)
	{
		PendingHoldReleases.Reset();
		return;
	}
	ENQUEUE_RENDER_COMMAND(ReleaseVoxelFadeHolds)(
		[Proxy, Keys = MoveTemp(PendingHoldReleases)](FRHICommandListImmediate& RHICmdList)
		{
			Proxy->ReleaseFadeHolds_RenderThread(Keys);
		}
	);
	PendingHoldReleases.Reset();
}

void UVoxelWorldComponent::SetChunkFadeHold(const FIntVector& ChunkCoord, bool bHold)
{
	check(IsInGameThread());

	if (bHold)
	{
		ChunkFadeHolds.Add(ChunkCoord);
		return;
	}
	ChunkFadeHolds.Remove(ChunkCoord);
	ReleaseHeldFade(FVoxelCrossfadeScheduler::ChunkKey(ChunkCoord));
}

void UVoxelWorldComponent::SetSeamFadeHold(const FIntVector& OwnerChunkCoord, bool bHold)
{
	check(IsInGameThread());

	if (bHold)
	{
		SeamFadeHolds.Add(OwnerChunkCoord);
		return;
	}
	SeamFadeHolds.Remove(OwnerChunkCoord);
	for (uint8 Slot = 0; Slot <= 6; ++Slot)
	{
		ReleaseHeldFade(FVoxelCrossfadeScheduler::SeamKey(OwnerChunkCoord, Slot));
	}
}

void UVoxelWorldComponent::AdvanceCrossfades()
{
	check(IsInGameThread());

	// Recycle MIDs retired last tick — by now the render thread has executed the fade-clear
	// commands that referenced their proxies, so reuse is safe.
	FreeFadeInMIDs.Append(CoolingFadeInMIDs);
	CoolingFadeInMIDs.Reset();
	FreeFadeOutMIDs.Append(CoolingFadeOutMIDs);
	CoolingFadeOutMIDs.Reset();

	FlushHoldReleases();

	if (ActiveFades.Num() == 0)
	{
		SetComponentTickEnabled(false);
		return;
	}

	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	UWorld* World = GetWorld();
	if (!Proxy || !World)
	{
		CancelAllFades();
		SetComponentTickEnabled(false);
		return;
	}

	TArray<FIntVector4> Completed;
	if (CVarVoxelMeshFade.GetValueOnGameThread() == 0)
	{
		// Kill-switch mid-fade completes everything instantly (releases retained meshes)
		FadeScheduler.CancelAll(Completed);
	}
	else
	{
		const float Duration = FMath::Max(0.02f, CVarVoxelMeshFadeDuration.GetValueOnGameThread());
		FVoxelCrossfadeScheduler::FAdvanceResult Result;
		FadeScheduler.Advance(World->GetRealTimeSeconds(), Duration, VoxelFadeHoldFailsafeSeconds, Result);

		// Holds nobody released: drop the request too, and tell the proxy they are no longer held.
		for (const FIntVector4& Key : Result.TimedOutHolds)
		{
			const FIntVector Coord = FVoxelCrossfadeScheduler::KeyCoord(Key);
			if (FVoxelCrossfadeScheduler::IsSeamKey(Key))
			{
				SeamFadeHolds.Remove(Coord);
			}
			else
			{
				ChunkFadeHolds.Remove(Coord);
			}
			PendingHoldReleases.Add(Key);
		}
		if (Result.TimedOutHolds.Num() > 0)
		{
			UE_LOG(LogVoxelRendering, Verbose, TEXT("AdvanceCrossfades: %d fade hold(s) released by the failsafe"), Result.TimedOutHolds.Num());
			FlushHoldReleases();
		}

		// The MIDs carry the visually-effective parameter to both fade batches; the proxy keeps
		// bookkeeping copies (one batched command per kind per tick).
		TArray<TPair<FIntVector, float>> ChunkAlphas;
		TArray<TPair<FIntVector4, float>> SeamAlphas;
		for (const TPair<FIntVector4, float>& Pair : Result.Alphas)
		{
			if (FActiveChunkFade* Fade = ActiveFades.Find(Pair.Key))
			{
				if (Fade->InMID)
				{
					Fade->InMID->SetScalarParameterValue(VoxelFadeAlphaParamName, Pair.Value);
				}
				if (Fade->OutMID)
				{
					Fade->OutMID->SetScalarParameterValue(VoxelFadeAlphaParamName, Pair.Value);
				}
			}
			if (FVoxelCrossfadeScheduler::IsSeamKey(Pair.Key))
			{
				SeamAlphas.Add(Pair);
			}
			else
			{
				ChunkAlphas.Emplace(FVoxelCrossfadeScheduler::KeyCoord(Pair.Key), Pair.Value);
			}
		}
		if (ChunkAlphas.Num() > 0)
		{
			ENQUEUE_RENDER_COMMAND(UpdateVoxelFadeAlphas)(
				[Proxy, Batch = MoveTemp(ChunkAlphas)](FRHICommandListImmediate& RHICmdList)
				{
					Proxy->UpdateChunkFadeAlphasBatch_RenderThread(Batch);
				}
			);
		}
		if (SeamAlphas.Num() > 0)
		{
			ENQUEUE_RENDER_COMMAND(UpdateVoxelSeamFadeAlphas)(
				[Proxy, Batch = MoveTemp(SeamAlphas)](FRHICommandListImmediate& RHICmdList)
				{
					Proxy->UpdateSeamFadeAlphasBatch_RenderThread(Batch);
				}
			);
		}

		Completed = MoveTemp(Result.Completed);
	}

	for (const FIntVector4& Key : Completed)
	{
		FActiveChunkFade Fade;
		if (ActiveFades.RemoveAndCopyValue(Key, Fade))
		{
			RetireFadeMIDs(Fade);
		}
		EnqueueClearFadeState(Key);
	}

	if (ActiveFades.Num() == 0 && CoolingFadeInMIDs.Num() == 0 && CoolingFadeOutMIDs.Num() == 0)
	{
		SetComponentTickEnabled(false);
	}
}

UMaterialInstanceDynamic* UVoxelWorldComponent::AcquireFadeMID(bool bInvert)
{
	TArray<UMaterialInstanceDynamic*>& FreeList = bInvert ? FreeFadeOutMIDs : FreeFadeInMIDs;

	UMaterialInstanceDynamic* MID = FreeList.Num() > 0 ? FreeList.Pop(EAllowShrinking::No) : nullptr;

	if (!MID)
	{
		if (!FadeMaterial)
		{
			return nullptr;
		}

		MID = UMaterialInstanceDynamic::Create(FadeMaterial, this);
		if (!MID)
		{
			return nullptr;
		}

		(bInvert ? FadeOutMIDPool : FadeInMIDPool).Add(MID);
	}

	// Mirror the EFFECTIVE parameter state of the active terrain material onto the fade MID.
	// The active material may be a runtime MID over a MaterialInstanceConstant over the master
	// (the demo uses MID -> MI_VoxelDefault -> M_VoxelMaster); the fade MID's parent is the
	// fade duplicate of the MASTER, so it starts from master defaults. CopyParameterOverrides
	// would only see the top layer's overrides — query resolved values through the whole
	// chain instead so MIC-level textures/params survive.
	if (UMaterialInterface* SourceMaterial = VoxelMaterial)
	{
		TArray<FMaterialParameterInfo> ParameterInfos;
		TArray<FGuid> ParameterIds;

		SourceMaterial->GetAllScalarParameterInfo(ParameterInfos, ParameterIds);
		for (const FMaterialParameterInfo& Info : ParameterInfos)
		{
			float Value = 0.0f;
			if (SourceMaterial->GetScalarParameterValue(Info, Value))
			{
				MID->SetScalarParameterValue(Info.Name, Value);
			}
		}

		SourceMaterial->GetAllVectorParameterInfo(ParameterInfos, ParameterIds);
		for (const FMaterialParameterInfo& Info : ParameterInfos)
		{
			FLinearColor Value = FLinearColor::White;
			if (SourceMaterial->GetVectorParameterValue(Info, Value))
			{
				MID->SetVectorParameterValue(Info.Name, Value);
			}
		}

		SourceMaterial->GetAllTextureParameterInfo(ParameterInfos, ParameterIds);
		for (const FMaterialParameterInfo& Info : ParameterInfos)
		{
			UTexture* Value = nullptr;
			if (SourceMaterial->GetTextureParameterValue(Info, Value) && Value)
			{
				MID->SetTextureParameterValue(Info.Name, Value);
			}
		}

		SourceMaterial->GetAllRuntimeVirtualTextureParameterInfo(ParameterInfos, ParameterIds);
		for (const FMaterialParameterInfo& Info : ParameterInfos)
		{
			URuntimeVirtualTexture* Value = nullptr;
			if (SourceMaterial->GetRuntimeVirtualTextureParameterValue(Info, Value) && Value)
			{
				MID->SetRuntimeVirtualTextureParameterValue(Info.Name, Value);
			}
		}
	}

	// Fade params last (the terrain chain has none of these, but be explicit about ordering)
	MID->SetScalarParameterValue(VoxelFadeInvertParamName, bInvert ? 1.0f : 0.0f);
	MID->SetScalarParameterValue(VoxelFadeAlphaParamName, 0.0f);

	return MID;
}

void UVoxelWorldComponent::RetireFadeMIDs(FActiveChunkFade& Fade)
{
	if (Fade.InMID)
	{
		CoolingFadeInMIDs.Add(Fade.InMID);
		Fade.InMID = nullptr;
	}
	if (Fade.OutMID)
	{
		CoolingFadeOutMIDs.Add(Fade.OutMID);
		Fade.OutMID = nullptr;
	}
}

void UVoxelWorldComponent::RefreshDerivedFadeMaterial()
{
	if (bExplicitFadeMaterial)
	{
		return;
	}

	UMaterialInterface* NewFadeMaterial = nullptr;

	// The active material is usually the runtime atlas MID — walk instance parents to the
	// base asset material, whose package name can derive the companion asset path.
	UMaterialInterface* Base = VoxelMaterial;
	while (UMaterialInstance* Instance = Cast<UMaterialInstance>(Base))
	{
		if (!Instance->Parent)
		{
			break;
		}
		Base = Instance->Parent;
	}

	if (UMaterial* BaseMaterial = Cast<UMaterial>(Base))
	{
		const FString PackageName = BaseMaterial->GetOutermost()->GetName();
		if (!PackageName.StartsWith(TEXT("/Engine/")))
		{
			const FString FadeObjectPath = FString::Printf(TEXT("%s_Fade.%s_Fade"), *PackageName, *BaseMaterial->GetName());
			NewFadeMaterial = Cast<UMaterialInterface>(FSoftObjectPath(FadeObjectPath).TryLoad());
		}
	}

	if (FadeMaterial != NewFadeMaterial)
	{
		// Same lifecycle handling as an explicit change; un-latch the flag afterwards
		// (we only get here when it was false) so future derivation keeps working.
		SetFadeMaterial(NewFadeMaterial);
		bExplicitFadeMaterial = false;
	}
}

void UVoxelWorldComponent::PushFadeMaterialToProxy(FVoxelSceneProxy* TargetProxy)
{
	FVoxelSceneProxy* Proxy = TargetProxy;
	if (!Proxy)
	{
		return;
	}

	UMaterialInterface* FadeMat = FadeMaterial;
	FMaterialRelevance FadeRelevance;
	if (FadeMat)
	{
		UWorld* World = GetWorld();
		ERHIFeatureLevel::Type FL = World ? World->GetFeatureLevel() : GMaxRHIFeatureLevel;
		FadeRelevance = FadeMat->GetRelevance(GetFeatureLevelShaderPlatform(FL));
	}

	ENQUEUE_RENDER_COMMAND(SetVoxelFadeMaterial)(
		[Proxy, FadeMat, FadeRelevance](FRHICommandListImmediate& RHICmdList)
		{
			Proxy->SetFadeMaterial_RenderThread(FadeMat, FadeRelevance);
		}
	);
}

// ==================== Configuration ====================

void UVoxelWorldComponent::SetVoxelSize(float InVoxelSize)
{
	VoxelSize = FMath::Max(1.0f, InVoxelSize);
}

void UVoxelWorldComponent::SetChunkWorldSize(float InChunkWorldSize)
{
	ChunkWorldSize = FMath::Max(100.0f, InChunkWorldSize);
}

void UVoxelWorldComponent::SetWorldOrigin(const FVector& InWorldOrigin)
{
	WorldOrigin = InWorldOrigin;
}

void UVoxelWorldComponent::SetRadialFaceUp(bool bRadial, const FVector& Center)
{
	if (bRadialFaceUp == bRadial && FaceUpCenter == Center)
	{
		return;
	}
	bRadialFaceUp = bRadial;
	FaceUpCenter = Center;
	MarkRenderStateDirty();
}

// ==================== Material Atlas ====================

void UVoxelWorldComponent::SetMaterialAtlas(UVoxelMaterialAtlas* InAtlas)
{
	MaterialAtlas = InAtlas;

	if (MaterialAtlas)
	{
		// Update registry with atlas positions
		FVoxelMaterialRegistry::SetAtlasPositions(
			MaterialAtlas->MaterialConfigs,
			MaterialAtlas->AtlasColumns,
			MaterialAtlas->AtlasRows);

		// Update material parameters if we have a dynamic instance
		UpdateMaterialAtlasParameters();
	}
}

UMaterialInstanceDynamic* UVoxelWorldComponent::CreateVoxelMaterialInstance(UMaterialInterface* MasterMaterial)
{
	if (!MasterMaterial)
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("CreateVoxelMaterialInstance: MasterMaterial is null"));
		return nullptr;
	}

	// Create dynamic material instance
	DynamicMaterialInstance = UMaterialInstanceDynamic::Create(MasterMaterial, this);

	if (DynamicMaterialInstance)
	{
		// Set as the voxel material
		SetMaterial(0, DynamicMaterialInstance);

		// Configure with atlas parameters
		UpdateMaterialAtlasParameters();

		UE_LOG(LogVoxelRendering, Log, TEXT("Created dynamic material instance from: %s"), *MasterMaterial->GetName());
	}

	return DynamicMaterialInstance;
}

void UVoxelWorldComponent::UpdateMaterialAtlasParameters()
{
	UE_LOG(LogVoxelRendering, Log, TEXT("UpdateMaterialAtlasParameters called - DynamicMaterial: %s, MaterialAtlas: %s"),
		DynamicMaterialInstance ? TEXT("Valid") : TEXT("NULL"),
		MaterialAtlas ? TEXT("Valid") : TEXT("NULL"));

	if (!DynamicMaterialInstance)
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("UpdateMaterialAtlasParameters: No DynamicMaterialInstance, skipping"));
		return;
	}

	// Set smooth meshing switch (matches bSmoothTerrain parameter in M_VoxelMaster)
	// Note: Static switches can't be changed at runtime, this is a scalar parameter fallback
	DynamicMaterialInstance->SetScalarParameterValue(FName("bSmoothTerrain"), bUseSmoothMeshing ? 1.0f : 0.0f);

	if (!MaterialAtlas)
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("UpdateMaterialAtlasParameters: No MaterialAtlas, skipping atlas setup"));
		return;
	}

	// ===== Material LUT (Face Variant Lookup Table) =====

	// Build LUT if needed
	if (MaterialAtlas->IsLUTDirty() || !MaterialAtlas->GetMaterialLUT())
	{
		UE_LOG(LogVoxelRendering, Log, TEXT("Building MaterialLUT (Dirty=%s, Exists=%s)"),
			MaterialAtlas->IsLUTDirty() ? TEXT("Yes") : TEXT("No"),
			MaterialAtlas->GetMaterialLUT() ? TEXT("Yes") : TEXT("No"));
		MaterialAtlas->BuildMaterialLUT();
	}

	// Pass LUT texture to material
	if (UTexture2D* LUT = MaterialAtlas->GetMaterialLUT())
	{
		DynamicMaterialInstance->SetTextureParameterValue(FName("MaterialLUT"), LUT);
		UE_LOG(LogVoxelRendering, Log, TEXT("Set MaterialLUT texture: %s (%dx%d)"),
			*LUT->GetName(), LUT->GetSizeX(), LUT->GetSizeY());
	}
	else
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("MaterialLUT is NULL after build attempt!"));
	}

	// ===== Packed Atlas Parameters (Cubic Terrain) =====

	if (MaterialAtlas->PackedAlbedoAtlas)
	{
		DynamicMaterialInstance->SetTextureParameterValue(FName("PackedAlbedoAtlas"), MaterialAtlas->PackedAlbedoAtlas);
		UE_LOG(LogVoxelRendering, Log, TEXT("Set PackedAlbedoAtlas: %s"), *MaterialAtlas->PackedAlbedoAtlas->GetName());
	}
	else
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("PackedAlbedoAtlas is NULL!"));
	}

	if (MaterialAtlas->PackedNormalAtlas)
	{
		DynamicMaterialInstance->SetTextureParameterValue(FName("PackedNormalAtlas"), MaterialAtlas->PackedNormalAtlas);
	}

	if (MaterialAtlas->PackedRoughnessAtlas)
	{
		DynamicMaterialInstance->SetTextureParameterValue(FName("PackedRoughnessAtlas"), MaterialAtlas->PackedRoughnessAtlas);
	}

	DynamicMaterialInstance->SetScalarParameterValue(FName("AtlasColumns"), static_cast<float>(MaterialAtlas->AtlasColumns));
	DynamicMaterialInstance->SetScalarParameterValue(FName("AtlasRows"), static_cast<float>(MaterialAtlas->AtlasRows));
	UE_LOG(LogVoxelRendering, Log, TEXT("Set Atlas dimensions: %d x %d"), MaterialAtlas->AtlasColumns, MaterialAtlas->AtlasRows);

	// ===== Texture Array Parameters (Smooth Terrain) =====

	// Build texture arrays if needed (similar to LUT)
	if (MaterialAtlas->AreTextureArraysDirty() || !MaterialAtlas->AlbedoArray)
	{
		UE_LOG(LogVoxelRendering, Log, TEXT("Building Texture Arrays (Dirty=%s, AlbedoArray=%s, NumConfigs=%d)"),
			MaterialAtlas->AreTextureArraysDirty() ? TEXT("Yes") : TEXT("No"),
			MaterialAtlas->AlbedoArray ? TEXT("Exists") : TEXT("NULL"),
			MaterialAtlas->GetMaterialCount());
		MaterialAtlas->BuildTextureArrays();
	}

	// Warn if arrays are still null after build attempt
	if (!MaterialAtlas->AlbedoArray)
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("AlbedoArray is NULL after BuildTextureArrays! Check that MaterialConfigs have AlbedoTexture assigned."));
	}

	if (MaterialAtlas->AlbedoArray)
	{
		DynamicMaterialInstance->SetTextureParameterValue(FName("AlbedoArray"), MaterialAtlas->AlbedoArray);
		UE_LOG(LogVoxelRendering, Log, TEXT("Set AlbedoArray texture parameter"));
	}

	if (MaterialAtlas->NormalArray)
	{
		DynamicMaterialInstance->SetTextureParameterValue(FName("NormalArray"), MaterialAtlas->NormalArray);
		UE_LOG(LogVoxelRendering, Log, TEXT("Set NormalArray texture parameter"));
	}

	if (MaterialAtlas->RoughnessArray)
	{
		DynamicMaterialInstance->SetTextureParameterValue(FName("RoughnessArray"), MaterialAtlas->RoughnessArray);
		UE_LOG(LogVoxelRendering, Log, TEXT("Set RoughnessArray texture parameter"));
	}

	UE_LOG(LogVoxelRendering, Log, TEXT("UpdateMaterialAtlasParameters COMPLETE: Columns=%d, Rows=%d, SmoothMeshing=%s, LUT=%s, AlbedoAtlas=%s"),
		MaterialAtlas->AtlasColumns, MaterialAtlas->AtlasRows,
		bUseSmoothMeshing ? TEXT("true") : TEXT("false"),
		MaterialAtlas->GetMaterialLUT() ? TEXT("valid") : TEXT("null"),
		MaterialAtlas->PackedAlbedoAtlas ? TEXT("valid") : TEXT("null"));
}

void UVoxelWorldComponent::SetUseSmoothMeshing(bool bUseSmooth)
{
	if (bUseSmoothMeshing != bUseSmooth)
	{
		bUseSmoothMeshing = bUseSmooth;
		UpdateMaterialAtlasParameters();
	}
}

// ==================== LOD Configuration ====================

void UVoxelWorldComponent::SetLODTransitionDistances(float InLODStartDistance, float InLODEndDistance)
{
	LODStartDistance = FMath::Max(0.0f, InLODStartDistance);
	LODEndDistance = FMath::Max(LODStartDistance + 1.0f, InLODEndDistance);

	// Update Material Parameter Collection with new values
	UpdateLODParameterCollection();

	UE_LOG(LogVoxelRendering, Log, TEXT("LOD Transition: Start=%.0f, End=%.0f"), LODStartDistance, LODEndDistance);
}

void UVoxelWorldComponent::SetLODParameterCollection(UMaterialParameterCollection* InCollection)
{
	LODParameterCollection = InCollection;

	// Update with current values
	UpdateLODParameterCollection();
}

void UVoxelWorldComponent::UpdateLODParameterCollection()
{
	if (!LODParameterCollection)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Get the parameter collection instance for this world
	UMaterialParameterCollectionInstance* MPCInstance = World->GetParameterCollectionInstance(LODParameterCollection);
	if (!MPCInstance)
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("Failed to get MPC instance for LOD parameters"));
		return;
	}

	// Calculate inverse range for efficient shader calculation
	const float InvRange = 1.0f / FMath::Max(LODEndDistance - LODStartDistance, 1.0f);

	// Update the parameters
	// These correspond to CollectionParameter nodes in the material
	MPCInstance->SetScalarParameterValue(FName("LODStartDistance"), LODStartDistance);
	MPCInstance->SetScalarParameterValue(FName("LODEndDistance"), LODEndDistance);
	MPCInstance->SetScalarParameterValue(FName("LODInvRange"), InvRange);

	UE_LOG(LogVoxelRendering, Verbose, TEXT("Updated MPC: Start=%.0f, End=%.0f, InvRange=%.6f"),
		LODStartDistance, LODEndDistance, InvRange);
}

// ==================== Queries ====================

bool UVoxelWorldComponent::IsChunkLoaded(const FIntVector& ChunkCoord) const
{
	FScopeLock Lock(&ChunkInfoLock);
	return ChunkInfoMap.Contains(ChunkCoord);
}

int32 UVoxelWorldComponent::GetLoadedChunkCount() const
{
	FScopeLock Lock(&ChunkInfoLock);
	return ChunkInfoMap.Num();
}

void UVoxelWorldComponent::GetSeamMeshOwners(TArray<FIntVector>& OutOwners) const
{
	check(IsInGameThread());
	SeamBucketSlotMasks.GetKeys(OutOwners);
}

bool UVoxelWorldComponent::ConsumeLostRenderData(TArray<FIntVector>& OutChunks)
{
	check(IsInGameThread());
	OutChunks.Reset();
	FScopeLock Lock(&ChunkInfoLock);
	if (!bRenderDataLost)
	{
		return false;
	}
	bRenderDataLost = false;
	ChunkInfoMap.GetKeys(OutChunks);
	return true;
}

void UVoxelWorldComponent::GetLoadedChunks(TArray<FIntVector>& OutChunks) const
{
	FScopeLock Lock(&ChunkInfoLock);

	OutChunks.Reset(ChunkInfoMap.Num());
	for (const auto& Pair : ChunkInfoMap)
	{
		OutChunks.Add(Pair.Key);
	}
}

bool UVoxelWorldComponent::GetChunkBounds(const FIntVector& ChunkCoord, FBox& OutBounds) const
{
	FScopeLock Lock(&ChunkInfoLock);

	const FChunkInfo* Info = ChunkInfoMap.Find(ChunkCoord);
	if (Info)
	{
		OutBounds = Info->Bounds;
		return true;
	}

	return false;
}

// ==================== Statistics ====================

void UVoxelWorldComponent::SetChunkStats(FChunkInfo& Info, int64 VertexCount, int64 TriangleCount, int64 GPUMemory)
{
	// Replace, not add: a re-meshed chunk used to count its old mesh too, so the totals only ever grew
	// (54.7 M "vertices" where the renderer held 7.7 M; WORLD_MODES_REVIEW_PLAN.md G1).
	CachedVertexCount += VertexCount - Info.VertexCount;
	CachedTriangleCount += TriangleCount - Info.TriangleCount;
	CachedGPUMemory += GPUMemory - Info.GPUMemory;
	Info.VertexCount = VertexCount;
	Info.TriangleCount = TriangleCount;
	Info.GPUMemory = GPUMemory;
}

int64 UVoxelWorldComponent::GetGPUMemoryUsage() const
{
	return CachedGPUMemory;
}

int64 UVoxelWorldComponent::GetTotalVertexCount() const
{
	return CachedVertexCount;
}

int64 UVoxelWorldComponent::GetTotalTriangleCount() const
{
	return CachedTriangleCount;
}

// ==================== Internal ====================

void UVoxelWorldComponent::SendRenderDynamicData_Concurrent()
{
	Super::SendRenderDynamicData_Concurrent();

	// Scene proxy will handle data updates via render commands
}

void UVoxelWorldComponent::MarkRenderStateDirtyAndNotify()
{
	MarkRenderStateDirty();
	MarkRenderDynamicDataDirty();
}

FVoxelSceneProxy* UVoxelWorldComponent::GetVoxelSceneProxy() const
{
	return static_cast<FVoxelSceneProxy*>(SceneProxy);
}

void UVoxelWorldComponent::FlushPendingOperations()
{
	check(IsInGameThread());

	// Fade holds released this tick (coordinated LOD transitions) reach the proxy in one command.
	FlushHoldReleases();

	// Early out if nothing to do
	if (!HasPendingOperations())
	{
		return;
	}

	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (!Proxy)
	{
		// No proxy - just clear the pending operations
		PendingAdds.Empty();
		PendingRemovals.Empty();
		return;
	}

	// Convert pending adds to batch format
	TArray<FVoxelSceneProxy::FBatchChunkAdd> BatchAdds;
	BatchAdds.Reserve(PendingAdds.Num());

	for (FPendingChunkAdd& PendingAdd : PendingAdds)
	{
		FVoxelSceneProxy::FBatchChunkAdd BatchAdd;
		BatchAdd.ChunkCoord = PendingAdd.ChunkCoord;
		BatchAdd.Vertices = MoveTemp(PendingAdd.Vertices);
		BatchAdd.Indices = MoveTemp(PendingAdd.Indices);
		BatchAdd.LODLevel = PendingAdd.LODLevel;
		BatchAdd.LocalBounds = PendingAdd.LocalBounds;
		BatchAdd.ChunkWorldPosition = PendingAdd.ChunkWorldPosition;
		BatchAdd.bCrossfade = PendingAdd.bCrossfade;
		BatchAdds.Add(MoveTemp(BatchAdd));
	}

	// Move pending removals
	TArray<FIntVector> BatchRemovals = MoveTemp(PendingRemovals);

	// Store counts before move for logging
	const int32 NumAdds = BatchAdds.Num();
	const int32 NumRemovals = BatchRemovals.Num();

	// Clear pending arrays
	PendingAdds.Empty();
	PendingRemovals.Empty();

	// Send single batched render command
	ENQUEUE_RENDER_COMMAND(FlushVoxelBatchUpdate)(
		[Proxy, BatchAdds = MoveTemp(BatchAdds), BatchRemovals = MoveTemp(BatchRemovals)](FRHICommandListImmediate& RHICmdList) mutable
		{
			Proxy->ProcessBatchUpdate_RenderThread(RHICmdList, MoveTemp(BatchAdds), MoveTemp(BatchRemovals));
		}
	);

	UE_LOG(LogVoxelRendering, Verbose, TEXT("UVoxelWorldComponent: Flushed %d adds, %d removals in single batch"),
		NumAdds, NumRemovals);
}
