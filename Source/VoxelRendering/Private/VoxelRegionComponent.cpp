// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelRegionComponent.h"
#include "VoxelWorldsSettings.h"
#include "VoxelSceneProxy.h"
#include "VoxelChunkSceneProxy.h"
#include "PrimitiveSceneDesc.h"
#include "PrimitiveSceneProxyDesc.h"
#include "SceneInterface.h"
#include "Components/ComponentInterfaces.h"
#include "Streaming/RenderAssetOwnerStreamingState.h"
#if WITH_EDITOR
#include "ObjectCacheEventSink.h"
#endif
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
#include "Engine/Engine.h"
#include "Async/Async.h"
#include "RenderingThread.h"
#include "UObject/UObjectIterator.h"
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
	TEXT("master material, or assigned via UVoxelRegionComponent::SetFadeMaterial."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarVoxelMeshFadeDuration(
	TEXT("voxel.MeshFade.Duration"),
	0.35f,
	TEXT("Chunk mesh-swap crossfade duration in seconds (default 0.35)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarVoxelMeshFadeMaxConcurrent(
	TEXT("voxel.MeshFade.MaxConcurrent"),
	512,
	TEXT("Cap on concurrent mesh-swap fades (chunk interiors + seam buckets). Swaps beyond it are instant;\n")
	TEXT("the fade MID pools grow with this number."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarVoxelMeshFadeAppearMinDistance(
	TEXT("voxel.MeshFade.AppearMinDistance"),
	7000.0f,
	TEXT("Nearest distance (uu, viewer to the closest point of the chunk cell) at which new geometry fades in. Nearer\n")
	TEXT("geometry appears at once, so nothing near the player turns briefly see-through (default 7000, the LOD 0 band edge)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarVoxelChunkPrimitives(
	TEXT("voxel.Render.ChunkPrimitives"),
	1,
	TEXT("1 = each chunk interior mesh and seam bucket is a primitive of its own on the static draw path (G116: GPUScene,\n")
	TEXT("cached draw commands and ray tracing instances, the engine's per-primitive culling), registered without a component.\n")
	TEXT("0 = the render region's scene proxy draws them, culling each mesh itself (the A/B). A change re-meshes the terrain."),
	ECVF_Default);

/** Failsafe for a fade hold nobody releases (the chunk manager's own cap, voxel.MeshFade.SeamHoldMs, is 1 s). */
static constexpr double VoxelFadeHoldFailsafeSeconds = 3.0;

/** Scalar parameter names on the fade material */
static const FName VoxelFadeAlphaParamName(TEXT("FadeAlpha"));
static const FName VoxelFadeInvertParamName(TEXT("FadeInvert"));
/**
 * The local up the smooth terrain's slope-aware rock is measured against (visual polish VP2-O3, the triplanar functions'
 * LocalUpCenter parameter): xyz a planet's centre, w 1 for radial up from it, 0 for world +Z (heightmap worlds). A
 * float centre is precise enough for a direction: a planet centred 10^8 uu out is off by 8 uu, under 10^-5 rad at 1 km.
 */
static const FName VoxelLocalUpCenterParamName(TEXT("LocalUpCenter"));

static TAutoConsoleVariable<float> CVarVoxelMaterialMacroVariation(
	TEXT("voxel.Material.MacroVariation"),
	0.3f,
	TEXT("Large-scale brightness variation of the smooth terrain (MPC_VoxelMaterial's MacroVariation,\n")
	TEXT("read by MF_TriplanarSampleAlbedoRoughness): the albedo is scaled by 1 +- this at most, by noise about 13 and 41\n")
	TEXT("tiles across. Default 0.3; 0 = none."),
	ECVF_Default);

namespace
{
	/**
	 * The collection the voxel master materials read MacroVariation from, set from its console variable (VP2-O2). The
	 * graphs take GradientSampling, TileVariation and ConstantRoughness as constants (VoxelWorlds 8 retired their scalars).
	 */
	const TCHAR* VoxelMaterialCollectionPath = TEXT("/VoxelWorlds/Materials/MPC_VoxelMaterial.MPC_VoxelMaterial");

	/** Each switch: the collection's scalar and the console variable that sets it. */
	struct FVoxelMaterialSwitch
	{
		FName ParamName;
		TAutoConsoleVariable<float>* ValueCVar = nullptr;

		float GetWanted() const
		{
			return ValueCVar->GetValueOnGameThread();
		}
	};

	/** Push voxel.Material.MacroVariation into the world's MPC_VoxelMaterial instance where it differs. */
	void ApplyVoxelMaterialSwitches(UWorld* World)
	{
		static const FVoxelMaterialSwitch Switches[] = {
			{ FName(TEXT("MacroVariation")), &CVarVoxelMaterialMacroVariation },
		};
		static TWeakObjectPtr<UMaterialParameterCollection> Collection;
		static bool bLoadAttempted = false;
		if (!Collection.IsValid() && !bLoadAttempted)
		{
			bLoadAttempted = true;
			Collection = LoadObject<UMaterialParameterCollection>(nullptr, VoxelMaterialCollectionPath);
			UE_CLOG(!Collection.IsValid(), LogVoxelRendering, Warning, TEXT("ApplyVoxelMaterialSwitches: %s not found, the voxel.Material.* switches have no effect"),
				VoxelMaterialCollectionPath);
		}
		UMaterialParameterCollectionInstance* Instance = (World && Collection.IsValid()) ? World->GetParameterCollectionInstance(Collection.Get()) : nullptr;
		if (!Instance)
		{
			return;
		}
		for (const FVoxelMaterialSwitch& Switch : Switches)
		{
			const float Wanted = Switch.GetWanted();
			float Current = -1.0f;
			if (!Instance->GetScalarParameterValue(Switch.ParamName, Current) || Current != Wanted)
			{
				Instance->SetScalarParameterValue(Switch.ParamName, Wanted);
			}
		}
	}

	/** A console variable changed: apply the switches to every game and PIE world (each world has its own MPC instance). */
	void OnVoxelMaterialCVarsChanged()
	{
		if (!GEngine)
		{
			return;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World && (Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE))
			{
				ApplyVoxelMaterialSwitches(World);
			}
		}
	}

	FAutoConsoleVariableSink VoxelMaterialCVarSink(FConsoleCommandDelegate::CreateStatic(&OnVoxelMaterialCVarsChanged));
}

// ==================== UVoxelRegionComponent ====================

UVoxelRegionComponent::UVoxelRegionComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Component settings. Tick exists only to advance mesh-swap crossfades and stays
	// disabled while no fade is active (StartFade enables it on demand).
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	bTickInEditor = true;

	// Rendering settings
	SetCastShadow(true);
	// Into the sun's far cascades too, where the proxy draws the coarse chunks only (FVoxelViewCull, D-VP3)
	bCastFarShadow = true;
	SetReceivesDecals(true);
	bUseAsOccluder = true;

	// Collision (handled separately, not by this component)
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);

	// Use custom bounds
	bUseAttachParentBound = false;

	// Initialize bounds
	CachedTotalBounds = FBox(ForceInit);

	ChunkMeshBytes = MakeShared<std::atomic<int64>, ESPMode::ThreadSafe>(0);
}

UVoxelRegionComponent::~UVoxelRegionComponent()
{
}

// ==================== UPrimitiveComponent Interface ====================

FPrimitiveSceneProxy* UVoxelRegionComponent::CreateSceneProxy()
{
	if (GetWorld() == nullptr)
	{
		return nullptr;
	}

	// Resolve the crossfade fade material (covers materials assigned without SetMaterial)
	RefreshDerivedFadeMaterial();

	// A new world's MPC instance starts at the collection's defaults: give it voxel.Material.MacroVariation (VP2-O2). Render
	// state can be created off the game thread, and the MPC instance belongs to it.
	AsyncTask(ENamedThreads::GameThread, [WeakWorld = TWeakObjectPtr<UWorld>(GetWorld())]()
	{
		ApplyVoxelMaterialSwitches(WeakWorld.Get());
	});

	// Ensure we have a material
	UMaterialInterface* MaterialToUse = VoxelMaterial;
	if (!MaterialToUse)
	{
		// WARNING: Default material does not compile with FVoxelVertexFactory!
		// Users must create and assign a custom material for the voxel world.
		UE_LOG(LogVoxelRendering, Warning,
			TEXT("UVoxelRegionComponent: No VoxelMaterial set! Create a simple opaque material and assign it. ")
			TEXT("Default materials do not work with the custom vertex factory."));

		// Still use default as a fallback, but rendering will likely fail
		MaterialToUse = UMaterial::GetDefaultMaterial(MD_Surface);
	}

	UE_LOG(LogVoxelRendering, Log, TEXT("UVoxelRegionComponent: Creating scene proxy with material: %s"),
		MaterialToUse ? *MaterialToUse->GetName() : TEXT("nullptr"));

	FVoxelSceneProxy* NewProxy = new FVoxelSceneProxy(this, MaterialToUse);

	// A new proxy starts empty: whatever the previous one held (chunk meshes, seam buckets, water tiles) is
	// gone with it. The engine recreates the proxy on material and render-state changes, including during
	// the first second of play (a runtime material compile), after the first chunks are submitted. The owner
	// sends the geometry again (ConsumeLostRenderData); the first proxy of a component has nothing to lose.
	// With chunk primitives (G116) the chunks and seams are primitives of their own, which the new proxy does not touch,
	// and the component re-sends the water tiles itself from its copies.
	if (bUseChunkPrimitives)
	{
		FScopeLock Lock(&ChunkInfoLock);
		for (const TPair<FIntVector2, FWaterTileCopy>& Pair : WaterTileCopies)
		{
			ENQUEUE_RENDER_COMMAND(ResendVoxelWaterTile)(
				[NewProxy, TileCoord = Pair.Key, Verts = Pair.Value.Vertices, Idxs = Pair.Value.Indices, TileWorldPosition = Pair.Value.TileWorldPosition](FRHICommandListImmediate& RHICmdList) mutable
				{
					NewProxy->UpdateWaterTileFromCPUData_RenderThread(RHICmdList, TileCoord, MoveTemp(Verts), MoveTemp(Idxs), TileWorldPosition);
				}
			);
		}
	}
	else
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

void UVoxelRegionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	AdvanceFades();

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

FBoxSphereBounds UVoxelRegionComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// The whole terrain is ONE primitive, so the engine frustum- and occlusion-culls it as one box (individual chunks
	// are culled again in GetDynamicMeshElements). This used to be a fixed +-5 km box around the origin: once the
	// player travelled past it, the entire terrain vanished whenever the camera looked away from that box -- in a
	// flash with camera motion, or held still (QA, 2026-10-11, player at X -615,000). Bound what is actually loaded
	// instead: the union of the chunk bounds and the water tiles' boxes, padded by a chunk so seam strips and crossfades
	// stay inside. The water tiles count on their own (G77): a region component (MP2) whose chunks hold no mesh, air
	// above a lake at the tiles' Z-0 layer, had no box but the fallback below, and the engine culled its tiles.
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
			for (const TPair<FIntVector2, FBox>& Pair : WaterTileWorldBounds)
			{
				if (Pair.Value.IsValid)
				{
					CachedTotalBounds += Pair.Value;
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

void UVoxelRegionComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	for (UMaterialInterface* Used : { VoxelMaterial.Get(), WaterMaterial.Get(), FadeMaterial.Get() })
	{
		if (Used)
		{
			OutMaterials.AddUnique(Used);
		}
	}
}

UMaterialInterface* UVoxelRegionComponent::GetMaterial(int32 ElementIndex) const
{
	return VoxelMaterial;
}

void UVoxelRegionComponent::SetMaterial(int32 ElementIndex, UMaterialInterface* Material)
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

		// Chunk primitives (G116) cached their draws with the old material: they go, and the owner sends the meshes again
		if (bUseChunkPrimitives && ChunkPrimitives.Num() > 0)
		{
			RemoveAllChunkPrimitives(/*bSeamsOnly*/ false);
			FScopeLock Lock(&ChunkInfoLock);
			bRenderDataLost = true;
		}

		// Keep the crossfade material paired with the terrain material
		RefreshDerivedFadeMaterial();

		MarkRenderStateDirtyAndNotify();
	}
}

// ==================== Chunk Management ====================

void UVoxelRegionComponent::UpdateChunkBuffersFromCPUData(
	const FIntVector& ChunkCoord,
	TArray<FVoxelVertex>&& Vertices,
	TArray<uint32>&& Indices,
	int32 LODLevel,
	const FBox& LocalBounds)
{
	check(IsInGameThread());
	EnsureChunkPrimitiveMode();

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
		SetChunkStats(Info, VertexCount, IndexCount / 3,
			static_cast<int64>(VertexCount) * sizeof(FVoxelVertex) + static_cast<int64>(IndexCount) * sizeof(uint32));

		bTotalBoundsDirty = true;
	}

	// Calculate chunk world position (includes WorldOrigin offset)
	FVector ChunkWorldPos = WorldOrigin + FVector(ChunkCoord) * ChunkWorldSize;

	// Mesh replacement -> crossfade, first mesh -> appearance fade. The flag rides the pending add
	// into the batched swap; the fade-state attach lands BEFORE the flush, so a held fade's flag is
	// in place when the swap retires the old mesh.
	const FIntVector4 FadeKey = FVoxelFadeScheduler::ChunkKey(ChunkCoord);
	const bool bCrossfade = StartChunkMeshFade(FadeKey, bChunkExisted);

	if (bUseChunkPrimitives)
	{
		// The chunk's own primitive (G116), replacing the one it had
		SubmitChunkPrimitive(FadeKey, MoveTemp(Vertices), MoveTemp(Indices), LODLevel, LocalBounds, ChunkWorldPos, bCrossfade);
		if (!bCrossfade && ActiveFades.Contains(FadeKey))
		{
			CancelFade(FadeKey, /*bClearRenderState=*/true);
		}
		UpdateBounds();
		RequestRenderBoundsRefresh();
		return;
	}

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

	// Update bounds, and have the tick send them to the scene (G77: a mesh without a fade is no tick on its own)
	UpdateBounds();
	RequestRenderBoundsRefresh();
}

void UVoxelRegionComponent::RemoveChunk(const FIntVector& ChunkCoord)
{
	check(IsInGameThread());
	EnsureChunkPrimitiveMode();

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
	const FIntVector4 FadeKey = FVoxelFadeScheduler::ChunkKey(ChunkCoord);
	if (ActiveFades.Contains(FadeKey))
	{
		CancelFade(FadeKey, /*bClearRenderState=*/true);
	}

	// Queue for batched submission instead of immediate render command
	if (bUseChunkPrimitives)
	{
		RemoveChunkPrimitive(FadeKey);
	}
	else
	{
		PendingRemovals.AddUnique(ChunkCoord);
	}

	UpdateBounds();
	RequestRenderBoundsRefresh();
}

void UVoxelRegionComponent::ClearAllChunks()
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

	// The chunk primitives (G116) go with the chunks and seams; the water copies with the proxy's tiles
	RemoveAllChunkPrimitives(/*bSeamsOnly*/ false);
	{
		FScopeLock Lock(&ChunkInfoLock);
		WaterTileCopies.Empty();
	}

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

void UVoxelRegionComponent::SetWaterMaterial(UMaterialInterface* InMaterial)
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

void UVoxelRegionComponent::UpdateWaterTileFromCPUData(
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

	EnsureChunkPrimitiveMode();
	WaterTileKeys.Add(TileCoord);
	// With chunk primitives (G116) the component keeps what it sent, for a recreated proxy (CreateSceneProxy)
	if (bUseChunkPrimitives)
	{
		FScopeLock Lock(&ChunkInfoLock);
		WaterTileCopies.Add(TileCoord, FWaterTileCopy{ Vertices, Indices, TileWorldPosition });
	}
	// The tile's world box (the vertices are tile-local, as the proxy reads them): part of the component's bounds (G77).
	{
		FBox TileBounds(ForceInit);
		for (const FVoxelVertex& Vertex : Vertices)
		{
			TileBounds += FVector(Vertex.Position);
		}
		FScopeLock Lock(&ChunkInfoLock);
		WaterTileWorldBounds.Add(TileCoord, TileBounds.ShiftBy(TileWorldPosition));
		bTotalBoundsDirty = true;
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
	RequestRenderBoundsRefresh();
}

void UVoxelRegionComponent::RemoveWaterTile(const FIntVector2& TileCoord)
{
	check(IsInGameThread());

	WaterTileKeys.Remove(TileCoord);
	{
		FScopeLock Lock(&ChunkInfoLock);
		WaterTileCopies.Remove(TileCoord);
		if (WaterTileWorldBounds.Remove(TileCoord) > 0)
		{
			bTotalBoundsDirty = true;
		}
	}
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
	RequestRenderBoundsRefresh();
}

void UVoxelRegionComponent::ClearAllWaterTiles()
{
	check(IsInGameThread());

	WaterTileKeys.Empty();
	{
		FScopeLock Lock(&ChunkInfoLock);
		WaterTileCopies.Empty();
		WaterTileWorldBounds.Empty();
		bTotalBoundsDirty = true;
	}
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
	RequestRenderBoundsRefresh();
}

void UVoxelRegionComponent::RequestRenderBoundsRefresh()
{
	bRenderBoundsStale = true;
	// The tick refreshes the bounds and hands them to the scene; a component with no fade running does not tick on its
	// own (AdvanceFades turns the tick off when idle, after this frame's refresh).
	SetComponentTickEnabled(true);
}

// ==================== Seam Meshes ====================

void UVoxelRegionComponent::UpdateSeamMeshFromCPUData(
	const FIntVector& OwnerChunkCoord,
	uint8 Axis,
	int32 LODLevel,
	TArray<FVoxelVertex>&& Vertices,
	TArray<uint32>&& Indices,
	const FVector& OwnerWorldPosition,
	bool bCrossfade)
{
	check(IsInGameThread());
	EnsureChunkPrimitiveMode();

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
	const FIntVector4 FadeKey = FVoxelFadeScheduler::SeamKey(OwnerChunkCoord, Axis);
	const bool bOwnerHeld = SeamFadeHolds.Contains(OwnerChunkCoord);
	bool bFade = false;
	if (bCrossfade && CanSwapFadeSeams())
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

	if (bUseChunkPrimitives)
	{
		// The bucket's own primitive (G116) at its owner chunk's origin; its box from the vertices (owner-local)
		FBox SeamBounds(ForceInit);
		for (const FVoxelVertex& Vertex : Vertices)
		{
			SeamBounds += FVector(Vertex.Position);
		}
		SubmitChunkPrimitive(FadeKey, MoveTemp(Vertices), MoveTemp(Indices), LODLevel, SeamBounds, OwnerWorldPosition, bFade);
		return;
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

void UVoxelRegionComponent::RemoveSeamMesh(const FIntVector& OwnerChunkCoord, uint8 Axis, bool bFadeOut)
{
	check(IsInGameThread());
	EnsureChunkPrimitiveMode();

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
	const FIntVector4 FadeKey = FVoxelFadeScheduler::SeamKey(OwnerChunkCoord, Axis);
	const bool bFade = bFadeOut && bExisted && CanSwapFadeSeams() && !FadeScheduler.IsAppearing(FadeKey) && StartFade(FadeKey);
	if (!bFade && ActiveFades.Contains(FadeKey))
	{
		// The removal command below releases the proxy-side fade state with the bucket.
		CancelFade(FadeKey, /*bClearRenderState=*/false);
	}

	if (bUseChunkPrimitives)
	{
		// Fading out to nothing: a primitive of the bucket's last mesh dithering out until the fade ends (step 2)
		if (bFade)
		{
			SubmitChunkPrimitiveMesh(FadeKey, nullptr, /*bFade*/ true);
		}
		else
		{
			RemoveChunkPrimitive(FadeKey);
		}
		return;
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

void UVoxelRegionComponent::ClearAllSeamMeshes()
{
	check(IsInGameThread());

	SeamBucketSlotMasks.Empty();
	SeamFadeHolds.Empty();

	// Seam fades end with their buckets (ClearAllSeamMeshes_RenderThread releases the proxy side).
	TArray<FIntVector4> SeamFadeKeys;
	for (const TPair<FIntVector4, FActiveChunkFade>& Pair : ActiveFades)
	{
		if (FVoxelFadeScheduler::IsSeamKey(Pair.Key))
		{
			SeamFadeKeys.Add(Pair.Key);
		}
	}
	for (const FIntVector4& Key : SeamFadeKeys)
	{
		CancelFade(Key, /*bClearRenderState=*/false);
	}

	RemoveAllChunkPrimitives(/*bSeamsOnly*/ true);

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

// ==================== Mesh-Swap Crossfade ====================

void UVoxelRegionComponent::SetFadeMaterial(UMaterialInterface* InFadeMaterial)
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
		TArray<FIntVector4> FadeKeys;
		ActiveFades.GetKeys(FadeKeys);
		CancelAllFades();

		if (bUseChunkPrimitives)
		{
			for (const FIntVector4& Key : FadeKeys)
			{
				ClearChunkPrimitiveFade(Key);
			}
		}
		else if (FVoxelSceneProxy* Proxy = GetVoxelSceneProxy())
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

	UE_LOG(LogVoxelRendering, Log, TEXT("UVoxelRegionComponent: Fade material set to %s (crossfade %s)"),
		FadeMaterial ? *FadeMaterial->GetName() : TEXT("null"),
		FadeMaterial ? TEXT("available") : TEXT("disabled"));
}

bool UVoxelRegionComponent::CanSwapFade() const
{
	return CVarVoxelMeshFade.GetValueOnGameThread() != 0
		&& FadeMaterial != nullptr
		&& GetVoxelSceneProxy() != nullptr;
}

bool UVoxelRegionComponent::CanSwapFadeSeams() const
{
	return CanSwapFade();
}

void UVoxelRegionComponent::SetFadeViewerPosition(const FVector& ViewerPosition)
{
	FadeViewerPosition = ViewerPosition;
	bHasFadeViewer = true;
}

/** voxel.MeshFade.AppearMinDistance, never negative (voxel.MeshFade is the off switch). */
static double GetVoxelAppearMinDistance()
{
	return FMath::Max(0.0, static_cast<double>(VoxelSettings::Resolve(CVarVoxelMeshFadeAppearMinDistance, UVoxelWorldsSettings::Get().MeshFadeAppearMinDistance)));
}

double UVoxelRegionComponent::GetFadeViewerDistance(const FIntVector& ChunkCoord) const
{
	// Nearest point of the chunk's cell: a seam bucket lies on its owner's max faces, so the owner's
	// cell bounds it from below too. Nothing inside the gate ever fades, wherever its vertices are.
	const FVector CellMin = WorldOrigin + FVector(ChunkCoord) * ChunkWorldSize;
	const FBox Cell(CellMin, CellMin + FVector(ChunkWorldSize));
	return FMath::Sqrt(Cell.ComputeSquaredDistanceToPoint(FadeViewerPosition));
}

bool UVoxelRegionComponent::WillFadeInChunk(const FIntVector& ChunkCoord) const
{
	if (!bHasFadeViewer || !CanSwapFade() || IsChunkLoaded(ChunkCoord))
	{
		return false;
	}
	const FIntVector4 Key = FVoxelFadeScheduler::ChunkKey(ChunkCoord);
	return !FadeScheduler.Contains(Key)
		&& FadeScheduler.Num() < CVarVoxelMeshFadeMaxConcurrent.GetValueOnGameThread()
		&& GetFadeViewerDistance(ChunkCoord) >= GetVoxelAppearMinDistance();
}

bool UVoxelRegionComponent::StartChunkMeshFade(const FIntVector4& Key, bool bMeshExisted)
{
	if (!CanSwapFade())
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

bool UVoxelRegionComponent::StartAppearFade(const FIntVector4& Key, bool bBypassGate)
{
	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	UWorld* World = GetWorld();
	if (!Proxy || !World)
	{
		return false;
	}

	const bool bSeam = FVoxelFadeScheduler::IsSeamKey(Key);
	const FIntVector Coord = FVoxelFadeScheduler::KeyCoord(Key);
	if (bSeam ? !CanSwapFadeSeams() : !CanSwapFade())
	{
		return false;
	}

	// A bucket joining a held transition fades in with it wherever it is (it must stay hidden until the
	// release). Anything else needs a known viewer and the distance gate.
	double Distance = TNumericLimits<double>::Max();
	if (!bBypassGate)
	{
		if (!bHasFadeViewer)
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

	// A chunk primitive (G116) gets the fade when it is created for the submit that follows (SubmitChunkPrimitiveMesh)
	if (bUseChunkPrimitives)
	{
	}
	else if (bSeam)
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

float UVoxelRegionComponent::GetSeamFadeRemainingSeconds(const FIntVector& OwnerChunkCoord) const
{
	const UWorld* World = GetWorld();
	if (!World || !CanSwapFadeSeams())
	{
		return 0.0f;
	}
	const double Now = World->GetRealTimeSeconds();
	const float Duration = VoxelSettings::Resolve(CVarVoxelMeshFadeDuration, UVoxelWorldsSettings::Get().MeshFadeDuration);
	float Remaining = 0.0f;
	for (uint8 Slot = 0; Slot <= 6; ++Slot)
	{
		Remaining = FMath::Max(Remaining, FadeScheduler.GetRemainingSeconds(FVoxelFadeScheduler::SeamKey(OwnerChunkCoord, Slot), Now, Duration));
	}
	return Remaining;
}

FString UVoxelRegionComponent::GetFadeDebugString(int32 MaxEntries, const FVector* FocusPoint) const
{
	check(IsInGameThread());

	const UWorld* World = GetWorld();
	const double Now = World ? World->GetRealTimeSeconds() : 0.0;
	const float Duration = VoxelSettings::Resolve(CVarVoxelMeshFadeDuration, UVoxelWorldsSettings::Get().MeshFadeDuration);
	FString Out = FString::Printf(TEXT("%s: can crossfade %d (fade material %s), tick registered %d enabled %d, active fades %d, scheduled %d, chunk holds %d, seam holds %d, pending releases %d, interior fades started %lld (appearances %lld)"),
		*GetName(), CanSwapFade() ? 1 : 0, FadeMaterial ? *FadeMaterial->GetName() : TEXT("none"),
		PrimaryComponentTick.IsTickFunctionRegistered() ? 1 : 0, IsComponentTickEnabled() ? 1 : 0,
		ActiveFades.Num(), FadeScheduler.Num(), ChunkFadeHolds.Num(), SeamFadeHolds.Num(), PendingHoldReleases.Num(),
		FadeStartCounts[0], FadeAppearCounts[0]);

	TOptional<FIntVector> FocusChunk;
	if (FocusPoint && ChunkWorldSize > 0.0f)
	{
		const FVector Local = (*FocusPoint - WorldOrigin) / ChunkWorldSize;
		FocusChunk = FIntVector(FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), FMath::FloorToInt32(Local.Z));
		int32 Pending = 0;
		for (const FPendingChunkAdd& Add : PendingAdds)
		{
			Pending += (Add.ChunkCoord == FocusChunk.GetValue()) ? 1 : 0;
		}
		FScopeLock Lock(&ChunkInfoLock);
		const FChunkInfo* Info = ChunkInfoMap.Find(FocusChunk.GetValue());
		const FIntVector4 Key = FVoxelFadeScheduler::ChunkKey(FocusChunk.GetValue());
		Out += FString::Printf(TEXT("\n  focus chunk %s: game side %s, pending adds %d, fade scheduled %d held %d appearing %d"),
			*FocusChunk->ToString(), Info ? *FString::Printf(TEXT("%lld tris, LOD %d"), Info->TriangleCount, Info->LODLevel) : TEXT("none"),
			Pending, FadeScheduler.Contains(Key) ? 1 : 0, FadeScheduler.IsHeld(Key) ? 1 : 0, FadeScheduler.IsAppearing(Key) ? 1 : 0);
	}
	int32 Listed = 0;
	for (const TPair<FIntVector4, FActiveChunkFade>& Pair : ActiveFades)
	{
		if (Listed++ >= MaxEntries)
		{
			break;
		}
		const FIntVector4& Key = Pair.Key;
		Out += FString::Printf(TEXT("\n  %s %s: scheduled %d held %d appearing %d, %.2f s left"),
			FVoxelFadeScheduler::IsSeamKey(Key) ? *FString::Printf(TEXT("seam slot %d of"), Key.W) : TEXT("chunk"),
			*FVoxelFadeScheduler::KeyCoord(Key).ToString(), FadeScheduler.Contains(Key) ? 1 : 0, FadeScheduler.IsHeld(Key) ? 1 : 0,
			FadeScheduler.IsAppearing(Key) ? 1 : 0, FadeScheduler.GetRemainingSeconds(Key, Now, Duration));
	}

	if (bUseChunkPrimitives)
	{
		Out += FString::Printf(TEXT("\n  chunk primitives: %d (%d queued to add, %d to remove)"), ChunkPrimitives.Num(), PendingPrimitiveAdds.Num(), PendingPrimitiveRemovals.Num());
	}
	else if (FVoxelSceneProxy* Proxy = GetVoxelSceneProxy())
	{
		FString ProxyString;
		ENQUEUE_RENDER_COMMAND(VoxelFadeDebugString)(
			[Proxy, MaxEntries, FocusChunk, &ProxyString](FRHICommandListImmediate& RHICmdList)
			{
				ProxyString = Proxy->GetFadeDebugString_RenderThread(MaxEntries, FocusChunk.GetPtrOrNull());
			}
		);
		FlushRenderingCommands();
		Out += TEXT("\n  ") + ProxyString;
	}
	else
	{
		Out += TEXT("\n  no scene proxy");
	}
	return Out;
}

bool UVoxelRegionComponent::GetChunkDrawInfo(const FIntVector& ChunkCoord, FVoxelChunkDrawInfo& OutInfo) const
{
	check(IsInGameThread());

	OutInfo = FVoxelChunkDrawInfo();
	if (bUseChunkPrimitives)
	{
		// The chunk's primitive (G116), and the totals over this component's primitives
		const FChunkPrimitive* Primitive = ChunkPrimitives.Find(FVoxelFadeScheduler::ChunkKey(ChunkCoord));
		const FVoxelChunkSceneProxy* ChunkProxy = Primitive ? Primitive->Proxy : nullptr;
		TArray<const FVoxelChunkSceneProxy*> AllProxies;
		AllProxies.Reserve(ChunkPrimitives.Num());
		for (const TPair<FIntVector4, FChunkPrimitive>& Pair : ChunkPrimitives)
		{
			if (Pair.Value.Proxy)
			{
				AllProxies.Add(Pair.Value.Proxy);
			}
		}
		FVoxelChunkDrawInfo Info;
		ENQUEUE_RENDER_COMMAND(VoxelChunkPrimitiveDrawInfo)(
			[ChunkProxy, AllProxies = MoveTemp(AllProxies), &Info](FRHICommandListImmediate& RHICmdList)
			{
				if (ChunkProxy)
				{
					ChunkProxy->GetDrawInfo_RenderThread(Info);
				}
				for (const FVoxelChunkSceneProxy* Each : AllProxies)
				{
					Info.ProxyMeshBufferBytes += Each->GetMeshBufferBytes_RenderThread();
					const uint64 Bytes = Each->GetRayTracingBytes_RenderThread();
					if (Bytes > 0)
					{
						(Each->IsSeam() ? Info.ProxyRayTracingSeams : Info.ProxyRayTracingChunks) += 1;
						Info.ProxyRayTracingBytes += Bytes;
					}
				}
			}
		);
		FlushRenderingCommands();
		// The buffers of the meshes this component's primitives draw, and the region proxy's water tiles (a removed
		// primitive's mesh goes when the scene deletes it, at its next update; GetProxyGPUMemoryUsage counts it until then)
		OutInfo = Info;
		OutInfo.bRayTracing = bChunkPrimitiveRayTracing;
		if (const FVoxelSceneProxy* Proxy = GetVoxelSceneProxy())
		{
			OutInfo.ProxyMeshBufferBytes += Proxy->GetMeshBufferBytes();
		}
		return OutInfo.CurrentTriangles > 0;
	}
	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (!Proxy)
	{
		return false;
	}
	FVoxelChunkDrawInfo Info;
	ENQUEUE_RENDER_COMMAND(VoxelChunkDrawInfo)(
		[Proxy, ChunkCoord, &Info](FRHICommandListImmediate& RHICmdList)
		{
			Proxy->GetChunkDrawInfo_RenderThread(ChunkCoord, Info);
		}
	);
	FlushRenderingCommands();
	OutInfo = Info;
	return OutInfo.CurrentTriangles > 0;
}

bool UVoxelRegionComponent::GetWaterTileDrawInfo(const FIntVector2& TileCoord, FString& Out) const
{
	check(IsInGameThread());

	FBox TileBox(ForceInit);
	{
		FScopeLock Lock(&ChunkInfoLock);
		if (const FBox* Found = WaterTileWorldBounds.Find(TileCoord))
		{
			TileBox = *Found;
		}
	}
	Out = FString::Printf(TEXT("component %s at %s: key %s, tile box %s, bounds %s (last sent %s), tick %s, registered %s, render state %s, visible %s, hidden in game %s, draw distance LD %.0f cached %.0f (cull volumes %s), bounds scale %.2f; "),
		*GetName(), *GetComponentLocation().ToString(), WaterTileKeys.Contains(TileCoord) ? TEXT("held") : TEXT("MISSING"),
		TileBox.IsValid ? *TileBox.ToString() : TEXT("none"), *Bounds.GetBox().ToString(), *LastSentRenderBounds.ToString(),
		IsComponentTickEnabled() ? TEXT("on") : TEXT("off"),
		IsRegistered() ? TEXT("yes") : TEXT("NO"), IsRenderStateCreated() ? TEXT("created") : TEXT("NOT created"),
		GetVisibleFlag() ? TEXT("yes") : TEXT("NO"), bHiddenInGame ? TEXT("YES") : TEXT("no"), LDMaxDrawDistance, CachedMaxDrawDistance,
		bAllowCullDistanceVolume ? TEXT("allowed") : TEXT("off"), BoundsScale);
	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (!Proxy)
	{
		Out += TEXT("no proxy");
		return false;
	}
	FString ProxyLine;
	bool bHeld = false;
	ENQUEUE_RENDER_COMMAND(VoxelWaterTileDrawInfo)(
		[Proxy, TileCoord, &ProxyLine, &bHeld](FRHICommandListImmediate& RHICmdList)
		{
			bHeld = Proxy->GetWaterTileDrawInfo_RenderThread(TileCoord, ProxyLine);
		}
	);
	FlushRenderingCommands();
	Out += ProxyLine;
	return bHeld;
}

// Console: voxel.MeshFade.Stats [fades=8] [x y z]
// World modes review G74: what the mesh-swap crossfades are doing, and for a world point what its chunk's proxy holds and
// what the last frame drew of it (the current mesh with the terrain material, a crossfade or an appearance).
static FAutoConsoleCommandWithWorldAndArgs GVoxelMeshFadeStatsCmd(
	TEXT("voxel.MeshFade.Stats"),
	TEXT("Log the mesh-swap crossfades of every voxel world component in the world: its tick, the fades it drives (alpha, held, ")
	TEXT("appearing, time left), the hold requests and its scene proxy's fade states. Optional args: fades to list per component (default 8), ")
	TEXT("and a world point x y z: its chunk's mesh here and in the proxy, and what the last frame drew of it (watched from then on; ")
	TEXT("a call without a point stops watching)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		const int32 MaxEntries = Args.Num() > 0 ? FMath::Max(0, FCString::Atoi(*Args[0])) : 8;
		TOptional<FVector> FocusPoint;
		if (Args.Num() >= 4)
		{
			FocusPoint = FVector(FCString::Atod(*Args[1]), FCString::Atod(*Args[2]), FCString::Atod(*Args[3]));
		}
		int32 Components = 0;
		for (TObjectIterator<UVoxelRegionComponent> It; It; ++It)
		{
			if (It->IsRegistered() && (!World || It->GetWorld() == World))
			{
				UE_LOG(LogVoxelRendering, Log, TEXT("voxel.MeshFade.Stats: %s"), *It->GetFadeDebugString(MaxEntries, FocusPoint.GetPtrOrNull()));
				++Components;
			}
		}
		UE_LOG(LogVoxelRendering, Log, TEXT("voxel.MeshFade.Stats: %d voxel world component(s) in %s"), Components,
			World ? *World->GetName() : TEXT("any world"));
	}));

bool UVoxelRegionComponent::StartFade(const FIntVector4& Key)
{
	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	UWorld* World = GetWorld();
	if (!Proxy || !World)
	{
		return false;
	}

	const bool bSeam = FVoxelFadeScheduler::IsSeamKey(Key);
	const FIntVector Coord = FVoxelFadeScheduler::KeyCoord(Key);
	const bool bHold = bSeam ? SeamFadeHolds.Contains(Coord) : ChunkFadeHolds.Contains(Coord);

	// A resubmit during an active fade restarts it (the proxy keeps one retained generation) —
	// same MIDs, alpha back to 0. Over the concurrent cap a NEW fade is refused: swap instantly.
	FadeScheduler.SetMaxConcurrent(CVarVoxelMeshFadeMaxConcurrent.GetValueOnGameThread());
	// Diagnostics: a resubmit while the key fades unheld (alpha < 1) restarts it and drops the mesh it was
	// fading out of (one retained generation). The seam pipeline defers such resubmits; this shows if any
	// slip through. A fade at alpha 1 that AdvanceFades has not retired yet has nothing left to drop.
	{
		const int32 Kind = bSeam ? 1 : 0;
		++FadeStartCounts[Kind];
		if (FadeScheduler.GetRemainingSeconds(Key, World->GetRealTimeSeconds(), VoxelSettings::Resolve(CVarVoxelMeshFadeDuration, UVoxelWorldsSettings::Get().MeshFadeDuration)) > 0.0f)
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

	// A chunk primitive (G116) gets the fade when it is created for the submit that follows (SubmitChunkPrimitiveMesh)
	if (bUseChunkPrimitives)
	{
	}
	else if (bSeam)
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

void UVoxelRegionComponent::EnqueueClearFadeState(const FIntVector4& Key)
{
	if (bUseChunkPrimitives)
	{
		ClearChunkPrimitiveFade(Key);
		return;
	}
	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (!Proxy)
	{
		return;
	}
	const FIntVector Coord = FVoxelFadeScheduler::KeyCoord(Key);
	if (FVoxelFadeScheduler::IsSeamKey(Key))
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

void UVoxelRegionComponent::CancelFade(const FIntVector4& Key, bool bClearRenderState)
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

void UVoxelRegionComponent::CancelAllFades()
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

void UVoxelRegionComponent::ReleaseHeldFade(const FIntVector4& Key)
{
	UWorld* World = GetWorld();
	if (World && FadeScheduler.Release(Key, World->GetRealTimeSeconds()))
	{
		PendingHoldReleases.Add(Key);
	}
}

void UVoxelRegionComponent::FlushHoldReleases()
{
	if (PendingHoldReleases.Num() == 0)
	{
		return;
	}
	if (bUseChunkPrimitives)
	{
		// To the primitives the released fades belong to (G116), in one command
		TArray<FVoxelChunkSceneProxy*> Released;
		for (const FIntVector4& Key : PendingHoldReleases)
		{
			if (const FChunkPrimitive* Primitive = ChunkPrimitives.Find(Key); Primitive && Primitive->Proxy)
			{
				Released.Add(Primitive->Proxy);
			}
		}
		PendingHoldReleases.Reset();
		if (Released.Num() > 0)
		{
			ENQUEUE_RENDER_COMMAND(ReleaseVoxelChunkPrimitiveHolds)(
				[Released = MoveTemp(Released)](FRHICommandListImmediate& RHICmdList)
				{
					for (FVoxelChunkSceneProxy* Each : Released)
					{
						Each->ReleaseFadeHold_RenderThread();
					}
				}
			);
		}
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

void UVoxelRegionComponent::SetChunkFadeHold(const FIntVector& ChunkCoord, bool bHold)
{
	check(IsInGameThread());

	if (bHold)
	{
		ChunkFadeHolds.Add(ChunkCoord);
		return;
	}
	ChunkFadeHolds.Remove(ChunkCoord);
	ReleaseHeldFade(FVoxelFadeScheduler::ChunkKey(ChunkCoord));
}

void UVoxelRegionComponent::SetSeamFadeHold(const FIntVector& OwnerChunkCoord, bool bHold)
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
		ReleaseHeldFade(FVoxelFadeScheduler::SeamKey(OwnerChunkCoord, Slot));
	}
}

void UVoxelRegionComponent::AdvanceFades()
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
	if ((!Proxy && !bUseChunkPrimitives) || !World)
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
		const float Duration = FMath::Max(0.02f, VoxelSettings::Resolve(CVarVoxelMeshFadeDuration, UVoxelWorldsSettings::Get().MeshFadeDuration));
		FVoxelFadeScheduler::FAdvanceResult Result;
		FadeScheduler.Advance(World->GetRealTimeSeconds(), Duration, VoxelFadeHoldFailsafeSeconds, Result);

		// Holds nobody released: drop the request too, and tell the proxy they are no longer held.
		for (const FIntVector4& Key : Result.TimedOutHolds)
		{
			const FIntVector Coord = FVoxelFadeScheduler::KeyCoord(Key);
			if (FVoxelFadeScheduler::IsSeamKey(Key))
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
			UE_LOG(LogVoxelRendering, Verbose, TEXT("AdvanceFades: %d fade hold(s) released by the failsafe"), Result.TimedOutHolds.Num());
			FlushHoldReleases();
		}

		// The MIDs carry the visually-effective parameter to both fade batches; the proxy keeps
		// bookkeeping copies (one batched command per kind per tick).
		TArray<TPair<FIntVector, float>> ChunkAlphas;
		TArray<TPair<FIntVector4, float>> SeamAlphas;
		TArray<TPair<FVoxelChunkSceneProxy*, float>> PrimitiveAlphas;
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
			if (bUseChunkPrimitives)
			{
				if (const FChunkPrimitive* Primitive = ChunkPrimitives.Find(Pair.Key); Primitive && Primitive->Proxy)
				{
					PrimitiveAlphas.Emplace(Primitive->Proxy, Pair.Value);
				}
			}
			else if (FVoxelFadeScheduler::IsSeamKey(Pair.Key))
			{
				SeamAlphas.Add(Pair);
			}
			else
			{
				ChunkAlphas.Emplace(FVoxelFadeScheduler::KeyCoord(Pair.Key), Pair.Value);
			}
		}
		if (PrimitiveAlphas.Num() > 0)
		{
			ENQUEUE_RENDER_COMMAND(UpdateVoxelChunkPrimitiveFadeAlphas)(
				[Batch = MoveTemp(PrimitiveAlphas)](FRHICommandListImmediate& RHICmdList)
				{
					for (const TPair<FVoxelChunkSceneProxy*, float>& Pair : Batch)
					{
						Pair.Key->SetFadeAlpha_RenderThread(Pair.Value);
					}
				}
			);
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

UMaterialInstanceDynamic* UVoxelRegionComponent::AcquireFadeMID(bool bInvert)
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

void UVoxelRegionComponent::RetireFadeMIDs(FActiveChunkFade& Fade)
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

void UVoxelRegionComponent::RefreshDerivedFadeMaterial()
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

void UVoxelRegionComponent::PushFadeMaterialToProxy(FVoxelSceneProxy* TargetProxy)
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

void UVoxelRegionComponent::SetVoxelSize(float InVoxelSize)
{
	VoxelSize = FMath::Max(1.0f, InVoxelSize);
}

void UVoxelRegionComponent::SetChunkWorldSize(float InChunkWorldSize)
{
	ChunkWorldSize = FMath::Max(100.0f, InChunkWorldSize);
}

void UVoxelRegionComponent::SetWorldOrigin(const FVector& InWorldOrigin)
{
	WorldOrigin = InWorldOrigin;
}

void UVoxelRegionComponent::SetRadialFaceUp(bool bRadial, const FVector& Center)
{
	if (bRadialFaceUp == bRadial && FaceUpCenter == Center)
	{
		return;
	}
	bRadialFaceUp = bRadial;
	FaceUpCenter = Center;
	// The chunk primitives' face types were converted against the old up (G116): they go and the owner sends them again
	if (bUseChunkPrimitives && ChunkPrimitives.Num() > 0)
	{
		RemoveAllChunkPrimitives(/*bSeamsOnly*/ false);
		FScopeLock Lock(&ChunkInfoLock);
		bRenderDataLost = true;
	}
	if (DynamicMaterialInstance)
	{
		DynamicMaterialInstance->SetVectorParameterValue(VoxelLocalUpCenterParamName,
			FLinearColor(static_cast<float>(FaceUpCenter.X), static_cast<float>(FaceUpCenter.Y), static_cast<float>(FaceUpCenter.Z), bRadialFaceUp ? 1.0f : 0.0f));
	}
	MarkRenderStateDirty();
}

// ==================== Material Atlas ====================

void UVoxelRegionComponent::SetMaterialAtlas(UVoxelMaterialAtlas* InAtlas)
{
	MaterialAtlas = InAtlas;

	if (MaterialAtlas)
	{
		// The atlas's material table is its world's (the chunk manager's meshers hold it), not the registry's (G101)
		// Update material parameters if we have a dynamic instance
		UpdateMaterialAtlasParameters();
	}
}

UMaterialInstanceDynamic* UVoxelRegionComponent::CreateVoxelMaterialInstance(UMaterialInterface* MasterMaterial)
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

void UVoxelRegionComponent::UpdateMaterialAtlasParameters()
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

	// The local up for the slope-aware rock (VP2-O3): the planet's radial up, or world +Z
	DynamicMaterialInstance->SetVectorParameterValue(VoxelLocalUpCenterParamName,
		FLinearColor(static_cast<float>(FaceUpCenter.X), static_cast<float>(FaceUpCenter.Y), static_cast<float>(FaceUpCenter.Z), bRadialFaceUp ? 1.0f : 0.0f));

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

	// The saved arrays (the editor rebuilds missing or stale ones first)
	if (!MaterialAtlas->EnsureTextureArrays())
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("%s has no AlbedoArray to bind. Check that MaterialConfigs have AlbedoTexture assigned and the arrays are built."),
			*MaterialAtlas->GetName());
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

void UVoxelRegionComponent::SetUseSmoothMeshing(bool bUseSmooth)
{
	if (bUseSmoothMeshing != bUseSmooth)
	{
		bUseSmoothMeshing = bUseSmooth;
		// Before the material instance exists there is nothing to update (CreateVoxelMaterialInstance applies the mode);
		// a planet's renderer configures a component per render region this way (WM-BP7)
		if (DynamicMaterialInstance)
		{
			UpdateMaterialAtlasParameters();
		}
	}
}

// ==================== Queries ====================

bool UVoxelRegionComponent::IsChunkLoaded(const FIntVector& ChunkCoord) const
{
	FScopeLock Lock(&ChunkInfoLock);
	return ChunkInfoMap.Contains(ChunkCoord);
}

int32 UVoxelRegionComponent::GetLoadedChunkCount() const
{
	FScopeLock Lock(&ChunkInfoLock);
	return ChunkInfoMap.Num();
}

void UVoxelRegionComponent::GetSeamMeshOwners(TArray<FIntVector>& OutOwners) const
{
	check(IsInGameThread());
	SeamBucketSlotMasks.GetKeys(OutOwners);
}

bool UVoxelRegionComponent::ConsumeLostRenderData(TArray<FIntVector>& OutChunks)
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

bool UVoxelRegionComponent::HasRenderContent() const
{
	check(IsInGameThread());
	{
		FScopeLock Lock(&ChunkInfoLock);
		if (ChunkInfoMap.Num() > 0)
		{
			return true;
		}
	}
	return SeamBucketSlotMasks.Num() > 0 || WaterTileKeys.Num() > 0 || ActiveFades.Num() > 0 || FadeScheduler.Num() > 0
		|| HasPendingOperations() || PendingHoldReleases.Num() > 0
		|| ChunkPrimitives.Num() > 0 || PendingPrimitiveAdds.Num() > 0 || PendingPrimitiveRemovals.Num() > 0;
}

void UVoxelRegionComponent::GetLoadedChunks(TArray<FIntVector>& OutChunks) const
{
	FScopeLock Lock(&ChunkInfoLock);

	OutChunks.Reset(ChunkInfoMap.Num());
	for (const auto& Pair : ChunkInfoMap)
	{
		OutChunks.Add(Pair.Key);
	}
}

bool UVoxelRegionComponent::GetChunkBounds(const FIntVector& ChunkCoord, FBox& OutBounds) const
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

void UVoxelRegionComponent::SetChunkStats(FChunkInfo& Info, int64 VertexCount, int64 TriangleCount, int64 GPUMemory)
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

int64 UVoxelRegionComponent::GetGPUMemoryUsage() const
{
	return CachedGPUMemory;
}

int64 UVoxelRegionComponent::GetProxyGPUMemoryUsage() const
{
	check(IsInGameThread());
	// The game thread clears SceneProxy before it queues the proxy's deletion, so a proxy seen here is alive
	const FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	// And the chunk primitives' meshes (G116), which count themselves
	return (Proxy ? Proxy->GetMeshBufferBytes() : 0) + ChunkMeshBytes->load(std::memory_order_relaxed);
}

int64 UVoxelRegionComponent::GetTotalVertexCount() const
{
	return CachedVertexCount;
}

int64 UVoxelRegionComponent::GetTotalTriangleCount() const
{
	return CachedTriangleCount;
}

// ==================== Internal ====================

void UVoxelRegionComponent::SendRenderDynamicData_Concurrent()
{
	Super::SendRenderDynamicData_Concurrent();

	// Scene proxy will handle data updates via render commands
}

void UVoxelRegionComponent::MarkRenderStateDirtyAndNotify()
{
	MarkRenderStateDirty();
	MarkRenderDynamicDataDirty();
}

FVoxelSceneProxy* UVoxelRegionComponent::GetVoxelSceneProxy() const
{
	return static_cast<FVoxelSceneProxy*>(SceneProxy);
}

void UVoxelRegionComponent::FlushPendingOperations()
{
	check(IsInGameThread());

	EnsureChunkPrimitiveMode();

	// Fade holds released this tick (coordinated LOD transitions) reach the proxy in one command.
	FlushHoldReleases();

	// The chunk primitives (G116): queued removals, then adds, in two batches; and the meshes the scene let go
	FlushChunkPrimitives();
	EnqueueDrainChunkMeshReleases();

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

	UE_LOG(LogVoxelRendering, Verbose, TEXT("UVoxelRegionComponent: Flushed %d adds, %d removals in single batch"),
		NumAdds, NumRemovals);
}

// ==================== Chunk Primitives (G116) ====================

/**
 * One chunk primitive slot as the engine's component interface (G116): what the engine's material and shader updates ask
 * a component (its scene proxy, materials, registration) and the recreation of its render state, answered by the region
 * component that owns the slot. Game thread; it lives as long as the slot.
 */
class FVoxelChunkPrimitiveInterface final : public IPrimitiveComponent
{
public:
	FVoxelChunkPrimitiveInterface(UVoxelRegionComponent* InOwner, const FIntVector4& InKey)
		: Owner(InOwner)
		, Key(InKey)
	{
	}

	const UVoxelRegionComponent::FChunkPrimitive* GetSlot() const { return Owner->ChunkPrimitives.Find(Key); }

	/**
	 * The editor's reverse lookup from materials to the primitives that use them (FObjectCacheEventSink), which the shader
	 * compiler reads to recreate the primitives whose material got new shaders: the slot's materials once its primitive is
	 * in the scene, none once it leaves (a component's proxy and render state notify the same)
	 */
	void NotifyAdded()
	{
#if WITH_EDITOR
		TArray<UMaterialInterface*> Materials;
		GetUsedMaterials(Materials);
		FObjectCacheEventSink::NotifyUsedMaterialsChanged_Concurrent(this, Materials);
#endif
	}
	void NotifyRemoved()
	{
#if WITH_EDITOR
		FObjectCacheEventSink::NotifyRenderStateChanged_Concurrent(this);
#endif
	}

	virtual bool IsRenderStateCreated() const override
	{
		const UVoxelRegionComponent::FChunkPrimitive* Slot = GetSlot();
		return Slot && Slot->Proxy && !Slot->bPendingAdd;
	}
	virtual bool IsRenderStateDirty() const override { return false; }
	virtual bool ShouldCreateRenderState() const override { return true; }
	virtual bool IsRegistered() const override { return Owner->IsRegistered(); }
	virtual bool IsUnreachable() const override { return Owner->IsUnreachable(); }
	virtual bool IsStaticMobility() const override { return false; }
	virtual bool IsMipStreamingForced() const override { return false; }
	virtual UWorld* GetWorld() const override { return Owner->GetWorld(); }
	virtual FSceneInterface* GetScene() const override { return Owner->GetScene(); }
	virtual FPrimitiveSceneProxy* GetSceneProxy() const override
	{
		return IsRenderStateCreated() ? GetSlot()->Proxy : nullptr;
	}
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override
	{
		// The region component's: the terrain material the static batches draw with (and its water and fade materials)
		Owner->GetUsedMaterials(OutMaterials, bGetDebugMaterials);
	}
	virtual void MarkRenderStateDirty() override
	{
		Owner->DestroyChunkPrimitiveRenderState(Key);
		Owner->CreateChunkPrimitiveRenderState(Key);
	}
	virtual void DestroyRenderState() override { Owner->DestroyChunkPrimitiveRenderState(Key); }
	virtual void CreateRenderState(FRegisterComponentContext* Context) override { Owner->CreateChunkPrimitiveRenderState(Key); }
	virtual FString GetName() const override
	{
		return FString::Printf(TEXT("%s.Chunk(%d,%d,%d,%d)"), *Owner->GetName(), Key.X, Key.Y, Key.Z, Key.W);
	}
	virtual FString GetFullName() const override
	{
		return FString::Printf(TEXT("%s.Chunk(%d,%d,%d,%d)"), *Owner->GetFullName(), Key.X, Key.Y, Key.Z, Key.W);
	}
	virtual FTransform GetTransform() const override
	{
		const UVoxelRegionComponent::FChunkPrimitive* Slot = GetSlot();
		return Slot ? FTransform(Owner->MakePendingChunkPrimitive(Key, *Slot).Origin) : FTransform::Identity;
	}
	virtual const FBoxSphereBounds& GetBounds() const override { return Owner->Bounds; }
	virtual float GetLastRenderTimeOnScreen() const override { return Owner->GetLastRenderTimeOnScreen(); }
	virtual void GetPrimitiveStats(FPrimitiveStats& PrimitiveStats) const override {}
	virtual UObject* GetUObject() override { return nullptr; }
	virtual const UObject* GetUObject() const override { return nullptr; }
	virtual void PrecachePSOs() override {}
	virtual UObject* GetOwner() const override { return Owner->GetOwner(); }
	virtual FString GetOwnerName() const override { return Owner->GetOwner() ? Owner->GetOwner()->GetName() : Owner->GetName(); }
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override { return nullptr; }
	virtual FRenderAssetOwnerStreamingState& GetStreamingState() const override { return StreamingState; }
	virtual ULevel* GetComponentLevel() const override { return Owner->GetComponentLevel(); }
	virtual IPrimitiveComponent* GetLODParentPrimitive() const override { return nullptr; }
	virtual float GetMinDrawDistance() const override { return 0.0f; }
	virtual float GetStreamingScale() const override { return 1.0f; }
	virtual void OnRenderAssetFirstLodChange(const UStreamableRenderAsset* RenderAsset, int32 FirstLodIndex) override {}
	virtual UStreamableRenderAsset* GetStreamableNaniteAsset() const override { return nullptr; }
	// The terrain material's textures stream with the region component, whose bounds hold every chunk
	virtual void GetStreamingRenderAssetInfo(FStreamingTextureLevelContext& LevelContext, TArray<FStreamingRenderAssetPrimitiveInfo>& OutStreamingRenderAssets) const override {}
#if WITH_EDITOR
	virtual HHitProxy* CreateMeshHitProxy(int32 SectionIndex, int32 MaterialIndex) override { return nullptr; }
#endif
	virtual HHitProxy* CreatePrimitiveHitProxies(TArray<TRefCountPtr<HHitProxy>>& OutHitProxies) override { return nullptr; }

private:
	UVoxelRegionComponent* Owner;
	FIntVector4 Key;
	mutable FRenderAssetOwnerStreamingState StreamingState;
};

TArray<IPrimitiveComponent*> UVoxelRegionComponent::GetChunkPrimitiveInterfaces() const
{
	TArray<IPrimitiveComponent*> Interfaces;
	Interfaces.Reserve(ChunkPrimitives.Num());
	for (const TPair<FIntVector4, FChunkPrimitive>& Pair : ChunkPrimitives)
	{
		if (Pair.Value.Interface.IsValid())
		{
			Interfaces.Add(Pair.Value.Interface.Get());
		}
	}
	return Interfaces;
}

UVoxelRegionComponent::FPendingChunkPrimitive UVoxelRegionComponent::MakePendingChunkPrimitive(const FIntVector4& Key, const FChunkPrimitive& Primitive) const
{
	// The box the scene culls it by, padded by two voxels as the region proxy padded each mesh's (seam strips, the surface
	// motion of a swap), around both meshes of a crossfade (one slot, one origin)
	FPendingChunkPrimitive Pending;
	Pending.Key = Key;
	Pending.Proxy = Primitive.Proxy;
	const TSharedPtr<FVoxelChunkMesh, ESPMode::ThreadSafe>& Any = Primitive.Mesh.IsValid() ? Primitive.Mesh : Primitive.Previous;
	if (Any.IsValid())
	{
		FBox LocalBox(ForceInit);
		LocalBox += Primitive.Mesh.IsValid() ? Primitive.Mesh->LocalBounds : FBox(ForceInit);
		LocalBox += Primitive.Previous.IsValid() ? Primitive.Previous->LocalBounds : FBox(ForceInit);
		Pending.Origin = Any->Origin;
		Pending.LocalBox = LocalBox.ExpandBy(FVector(VoxelSize * 2.0f));
	}
	return Pending;
}

void UVoxelRegionComponent::FillChunkPrimitiveSceneDesc(const FPendingChunkPrimitive& Pending, FPrimitiveSceneDesc& OutDesc)
{
	OutDesc.SceneProxy = Pending.Proxy;
	OutDesc.PrimitiveSceneData = &Pending.Proxy->SceneData;
	OutDesc.PrimitiveUObject = this;
	OutDesc.Mobility = Mobility;
	// At the chunk's origin, its vertices float offsets a chunk across
	OutDesc.RenderMatrix = FTranslationMatrix(Pending.Origin);
	OutDesc.AttachmentRootPosition = Pending.Origin;
	OutDesc.LocalBounds = FBoxSphereBounds(Pending.LocalBox);
	OutDesc.Bounds = FBoxSphereBounds(Pending.LocalBox.ShiftBy(Pending.Origin));
}

void UVoxelRegionComponent::DestroyChunkPrimitiveRenderState(const FIntVector4& Key)
{
	check(IsInGameThread());

	FChunkPrimitive* Primitive = ChunkPrimitives.Find(Key);
	FSceneInterface* Scene = GetScene();
	if (!Primitive || !Primitive->Proxy || Primitive->bPendingAdd || !Scene)
	{
		return;
	}
	FVoxelChunkPrimitiveCounters::AddRemoved(1);
	Scene->BatchRemovePrimitives(TArray<FPrimitiveSceneProxy*>{ Primitive->Proxy });
	Primitive->Proxy = nullptr;
	Primitive->Interface->NotifyRemoved();
	// The engine recreates a primitive after a material recompile or a feature level change: the next one reads the
	// materials' relevance again
	ChunkMaterialRelevanceSource = nullptr;
	ChunkFadeMaterialRelevanceSource = nullptr;
}

void UVoxelRegionComponent::CreateChunkPrimitiveRenderState(const FIntVector4& Key)
{
	check(IsInGameThread());

	FChunkPrimitive* Primitive = ChunkPrimitives.Find(Key);
	FSceneInterface* Scene = GetScene();
	if (!Primitive || Primitive->Proxy || (!Primitive->Mesh.IsValid() && !Primitive->Previous.IsValid()) || !Scene)
	{
		return;
	}
	// A new primitive over the meshes the slot holds (their buffers stay: the render thread made them once), with the
	// slot's fade if one runs
	Primitive->Proxy = CreateChunkPrimitive(Key, Primitive->Mesh, Primitive->Previous, ActiveFades.Contains(Key));
	Primitive->bPendingAdd = false;
	FPrimitiveSceneDesc Desc;
	FillChunkPrimitiveSceneDesc(MakePendingChunkPrimitive(Key, *Primitive), Desc);
	FVoxelChunkPrimitiveCounters::AddAdded(1);
	Scene->AddPrimitive(&Desc);
	Primitive->Interface->NotifyAdded();
}

void UVoxelRegionComponent::OnRegister()
{
	Super::OnRegister();
	// A component holding nothing takes the mode now; one that holds meshes keeps its mode until the flush's
	// EnsureChunkPrimitiveMode, which has the owner send them again in the new one
	FScopeLock Lock(&ChunkInfoLock);
	if (ChunkInfoMap.Num() == 0 && SeamBucketSlotMasks.Num() == 0)
	{
		bUseChunkPrimitives = CVarVoxelChunkPrimitives.GetValueOnGameThread() != 0;
		bChunkPrimitiveRayTracing = IsVoxelRayTracingWanted();
	}
}

void UVoxelRegionComponent::OnUnregister()
{
	// The chunk primitives belong to this component's scene: they leave it now, and a registration later has the owner
	// send the meshes again (ConsumeLostRenderData), as a recreated scene proxy does in the region mode
	if (ChunkPrimitives.Num() > 0 || PendingPrimitiveAdds.Num() > 0 || PendingPrimitiveRemovals.Num() > 0)
	{
		const bool bHadPrimitives = ChunkPrimitives.Num() > 0;
		RemoveAllChunkPrimitives(/*bSeamsOnly*/ false);
		FlushChunkPrimitives();
		if (bHadPrimitives)
		{
			FScopeLock Lock(&ChunkInfoLock);
			bRenderDataLost |= ChunkInfoMap.Num() > 0 || SeamBucketSlotMasks.Num() > 0;
		}
	}
	Super::OnUnregister();
}

void UVoxelRegionComponent::EnsureChunkPrimitiveMode()
{
	check(IsInGameThread());

	const bool bWant = CVarVoxelChunkPrimitives.GetValueOnGameThread() != 0;
	const bool bWantRayTracing = IsVoxelRayTracingWanted();
	if (bWant == bUseChunkPrimitives && (!bWant || bWantRayTracing == bChunkPrimitiveRayTracing))
	{
		return;
	}

	// Drop what the old mode drew: the primitives (their ray tracing geometry is fixed at creation), or the region proxy's
	// chunk and seam meshes with their fades
	CancelAllFades();
	if (bUseChunkPrimitives)
	{
		RemoveAllChunkPrimitives(/*bSeamsOnly*/ false);
		FlushChunkPrimitives();
		FScopeLock Lock(&ChunkInfoLock);
		WaterTileCopies.Empty();
	}
	else
	{
		PendingAdds.Empty();
		PendingRemovals.Empty();
		if (FVoxelSceneProxy* Proxy = GetVoxelSceneProxy())
		{
			ENQUEUE_RENDER_COMMAND(ClearVoxelChunksForPrimitives)(
				[Proxy](FRHICommandListImmediate& RHICmdList)
				{
					Proxy->ClearAllChunks_RenderThread();
					Proxy->ClearAllSeamMeshes_RenderThread();
				}
			);
		}
	}

	UE_LOG(LogVoxelRendering, Log, TEXT("%s: chunk primitives %s (ray tracing %s): the owner sends the meshes again"), *GetName(),
		bWant ? TEXT("on") : TEXT("off"), bWantRayTracing ? TEXT("on") : TEXT("off"));
	bUseChunkPrimitives = bWant;
	bChunkPrimitiveRayTracing = bWantRayTracing;
	{
		FScopeLock Lock(&ChunkInfoLock);
		bRenderDataLost |= ChunkInfoMap.Num() > 0 || SeamBucketSlotMasks.Num() > 0;
		bTotalBoundsDirty = true;
	}
	RequestRenderBoundsRefresh();
}

FVoxelChunkSceneProxy* UVoxelRegionComponent::CreateChunkPrimitive(const FIntVector4& Key, const TSharedPtr<FVoxelChunkMesh, ESPMode::ThreadSafe>& Mesh,
	const TSharedPtr<FVoxelChunkMesh, ESPMode::ThreadSafe>& Previous, bool bFading)
{
	UMaterialInterface* MaterialToUse = VoxelMaterial ? VoxelMaterial.Get() : UMaterial::GetDefaultMaterial(MD_Surface);
	const ERHIFeatureLevel::Type FeatureLevel = GetScene()->GetFeatureLevel();
	if (ChunkMaterialRelevanceSource != MaterialToUse)
	{
		ChunkMaterialRelevance = MaterialToUse->GetRelevance(GetFeatureLevelShaderPlatform(FeatureLevel));
		ChunkMaterialRelevanceSource = MaterialToUse;
	}

	// The component's description (its shadow, decal, ray tracing and virtual texture settings), with an id of the
	// primitive's own and no component interface: the scene never asks this component for the primitive
	FPrimitiveSceneProxyDesc Desc(this);
	Desc.PrimitiveComponentInterface = nullptr;
	Desc.ComponentId = FPrimitiveSceneInfoData().PrimitiveSceneId;
	// The sun's far cascades take the coarse terrain only (D-VP3: the region proxy's per-mesh filter, per primitive)
	Desc.bCastFarShadow = bCastFarShadow && Mesh.IsValid() && Mesh->LODLevel >= GetVoxelFarShadowMinLOD();

	FVoxelChunkSceneProxy::FInit Init;
	Init.Mesh = Mesh;
	if (bFading)
	{
		// The fade this submit started or continues: its pooled MIDs (alpha 0 or where an appearance is), held and
		// appearance flags (step 2)
		const FActiveChunkFade* Active = ActiveFades.Find(Key);
		Init.bFading = true;
		Init.Previous = Previous;
		Init.Fade.FadeInProxy = (Active && Active->InMID) ? Active->InMID->GetRenderProxy() : nullptr;
		Init.Fade.FadeOutProxy = (Active && Active->OutMID) ? Active->OutMID->GetRenderProxy() : nullptr;
		Init.Fade.bHeld = FadeScheduler.IsHeld(Key);
		Init.Fade.bAppear = FadeScheduler.IsAppearing(Key);
		if (FadeMaterial && ChunkFadeMaterialRelevanceSource != FadeMaterial)
		{
			ChunkFadeMaterialRelevance = FadeMaterial->GetRelevance(GetFeatureLevelShaderPlatform(FeatureLevel));
			ChunkFadeMaterialRelevanceSource = FadeMaterial;
		}
		Init.FadeMaterialRelevance = ChunkFadeMaterialRelevance;
	}
	Init.Material = MaterialToUse;
	Init.MaterialRelevance = ChunkMaterialRelevance;
	Init.FeatureLevel = FeatureLevel;
	Init.bRayTracing = bChunkPrimitiveRayTracing;
	Init.bPositionOnly = FLocalVertexFactory::StaticType.SupportsManualVertexFetch(FeatureLevel);
	Init.bRadialFaceUp = bRadialFaceUp;
	Init.FaceUpCenter = FaceUpCenter;
	Init.bSeam = FVoxelFadeScheduler::IsSeamKey(Key);
	return new FVoxelChunkSceneProxy(Desc, MoveTemp(Init));
}

void UVoxelRegionComponent::SubmitChunkPrimitive(const FIntVector4& Key, TArray<FVoxelVertex>&& Vertices, TArray<uint32>&& Indices,
	int32 LODLevel, const FBox& LocalBounds, const FVector& Origin, bool bFade)
{
	check(IsInGameThread());

	if (!GetScene())
	{
		// Not registered: nothing to draw into (the registration's ConsumeLostRenderData brings it back)
		return;
	}
	SubmitChunkPrimitiveMesh(Key, MakeShared<FVoxelChunkMesh, ESPMode::ThreadSafe>(MoveTemp(Vertices), MoveTemp(Indices), LODLevel,
		LocalBounds, Origin, ChunkMeshBytes.ToSharedRef()), bFade);
}

void UVoxelRegionComponent::SubmitChunkPrimitiveMesh(const FIntVector4& Key, const TSharedPtr<FVoxelChunkMesh, ESPMode::ThreadSafe>& Mesh, bool bFade)
{
	check(IsInGameThread());

	if (!GetScene())
	{
		return;
	}

	FChunkPrimitive& Primitive = ChunkPrimitives.FindOrAdd(Key);

	// What the new primitive dithers out (the region proxy's rules, SEAM_OWNERSHIP_ARCHITECTURE.md §5, R24): nothing for
	// an appearance (the new mesh takes over its fade-in); the mesh on screen while a held fade replaces a never-shown
	// one; the replaced mesh otherwise (one generation: an older one goes); and a bucket fading out to nothing that gets a
	// new mesh keeps its remnant
	const FActiveChunkFade* Active = bFade ? ActiveFades.Find(Key) : nullptr;
	const bool bFading = Active && (Active->InMID || Active->OutMID);
	TSharedPtr<FVoxelChunkMesh, ESPMode::ThreadSafe> Previous;
	if (bFading && !FadeScheduler.IsAppearing(Key))
	{
		Previous = (FadeScheduler.IsHeld(Key) && Primitive.Previous.IsValid()) || !Primitive.Mesh.IsValid() ? Primitive.Previous : Primitive.Mesh;
	}
	if (!Mesh.IsValid() && !Previous.IsValid())
	{
		// Nothing to draw or to fade out
		RetireChunkPrimitive(Primitive);
		ChunkPrimitives.Remove(Key);
		return;
	}

	RetireChunkPrimitive(Primitive);
	if (!Primitive.Interface.IsValid())
	{
		Primitive.Interface = MakeShared<FVoxelChunkPrimitiveInterface>(this, Key);
	}
	Primitive.Mesh = Mesh;
	Primitive.Previous = Previous;
	Primitive.Proxy = CreateChunkPrimitive(Key, Mesh, Previous, bFading);
	Primitive.bPendingAdd = true;
	PendingPrimitiveAdds.Add(MakePendingChunkPrimitive(Key, Primitive));
}

void UVoxelRegionComponent::ClearChunkPrimitiveFade(const FIntVector4& Key)
{
	FChunkPrimitive* Primitive = ChunkPrimitives.Find(Key);
	if (!Primitive)
	{
		return;
	}
	if (!Primitive->Mesh.IsValid())
	{
		// A bucket that faded out to nothing: its primitive leaves the scene
		RemoveChunkPrimitive(Key);
		return;
	}
	if (FVoxelChunkSceneProxy* Proxy = Primitive->Proxy)
	{
		ENQUEUE_RENDER_COMMAND(ClearVoxelChunkPrimitiveFade)(
			[Proxy](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->ClearFade_RenderThread();
			}
		);
	}
	ReleaseChunkMeshOnRenderThread(MoveTemp(Primitive->Previous));
}

void UVoxelRegionComponent::RetireChunkPrimitive(FChunkPrimitive& Primitive)
{
	if (Primitive.Proxy)
	{
		Primitive.Interface->NotifyRemoved();
		if (Primitive.bPendingAdd)
		{
			// Never handed to the scene: out of the queue, and deleted on the render thread (after any command that names it)
			FVoxelChunkSceneProxy* Proxy = Primitive.Proxy;
			PendingPrimitiveAdds.RemoveAllSwap([Proxy](const FPendingChunkPrimitive& Pending) { return Pending.Proxy == Proxy; });
			ENQUEUE_RENDER_COMMAND(DeleteVoxelChunkPrimitive)(
				[Proxy](FRHICommandListImmediate&)
				{
					delete Proxy;
				}
			);
		}
		else
		{
			PendingPrimitiveRemovals.Add(Primitive.Proxy);
		}
	}
	Primitive.Proxy = nullptr;
	Primitive.bPendingAdd = false;
	ReleaseChunkMeshOnRenderThread(MoveTemp(Primitive.Mesh));
	ReleaseChunkMeshOnRenderThread(MoveTemp(Primitive.Previous));
}

void UVoxelRegionComponent::RemoveChunkPrimitive(const FIntVector4& Key)
{
	check(IsInGameThread());

	if (FChunkPrimitive* Primitive = ChunkPrimitives.Find(Key))
	{
		RetireChunkPrimitive(*Primitive);
		ChunkPrimitives.Remove(Key);
	}
}

void UVoxelRegionComponent::RemoveAllChunkPrimitives(bool bSeamsOnly)
{
	check(IsInGameThread());

	for (auto It = ChunkPrimitives.CreateIterator(); It; ++It)
	{
		if (!bSeamsOnly || FVoxelFadeScheduler::IsSeamKey(It.Key()))
		{
			RetireChunkPrimitive(It.Value());
			It.RemoveCurrent();
		}
	}
}

void UVoxelRegionComponent::FlushChunkPrimitives()
{
	check(IsInGameThread());

	if (PendingPrimitiveAdds.Num() == 0 && PendingPrimitiveRemovals.Num() == 0)
	{
		return;
	}

	FSceneInterface* Scene = GetScene();
	if (!Scene)
	{
		// No scene to add to: the queued primitives are deleted unseen (a removal needs none: its scene took it already)
		for (const FPendingChunkPrimitive& Pending : PendingPrimitiveAdds)
		{
			if (FChunkPrimitive* Primitive = ChunkPrimitives.Find(Pending.Key); Primitive && Primitive->Proxy == Pending.Proxy)
			{
				ReleaseChunkMeshOnRenderThread(MoveTemp(Primitive->Mesh));
				ReleaseChunkMeshOnRenderThread(MoveTemp(Primitive->Previous));
				ChunkPrimitives.Remove(Pending.Key);
			}
			ENQUEUE_RENDER_COMMAND(DeleteVoxelChunkPrimitive)(
				[Proxy = Pending.Proxy](FRHICommandListImmediate&)
				{
					delete Proxy;
				}
			);
		}
		PendingPrimitiveAdds.Reset();
		PendingPrimitiveRemovals.Reset();
		return;
	}

	// The removals first: a replaced primitive leaves the scene in the update that adds its successor
	if (PendingPrimitiveRemovals.Num() > 0)
	{
		FVoxelChunkPrimitiveCounters::AddRemoved(PendingPrimitiveRemovals.Num());
		Scene->BatchRemovePrimitives(MoveTemp(PendingPrimitiveRemovals));
		PendingPrimitiveRemovals.Reset();
	}

	if (PendingPrimitiveAdds.Num() > 0)
	{
		TArray<FPrimitiveSceneDesc> Descs;
		Descs.SetNum(PendingPrimitiveAdds.Num());
		TArray<FPrimitiveSceneDesc*> DescPtrs;
		DescPtrs.Reserve(PendingPrimitiveAdds.Num());
		for (int32 i = 0; i < PendingPrimitiveAdds.Num(); ++i)
		{
			const FPendingChunkPrimitive& Pending = PendingPrimitiveAdds[i];
			FPrimitiveSceneDesc& Desc = Descs[i];
			FillChunkPrimitiveSceneDesc(Pending, Desc);
			DescPtrs.Add(&Desc);

		}
		FVoxelChunkPrimitiveCounters::AddAdded(DescPtrs.Num());
		Scene->BatchAddPrimitives(DescPtrs);
		for (const FPendingChunkPrimitive& Pending : PendingPrimitiveAdds)
		{
			if (FChunkPrimitive* Primitive = ChunkPrimitives.Find(Pending.Key); Primitive && Primitive->Proxy == Pending.Proxy)
			{
				Primitive->bPendingAdd = false;
				Primitive->Interface->NotifyAdded();
			}
		}
		PendingPrimitiveAdds.Reset();
	}
}
