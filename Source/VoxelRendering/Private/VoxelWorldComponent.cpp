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

/** Scalar parameter names on the fade material */
static const FName VoxelFadeAlphaParamName(TEXT("FadeAlpha"));
static const FName VoxelFadeInvertParamName(TEXT("FadeInvert"));

// ==================== UVoxelWorldComponent ====================

UVoxelWorldComponent::UVoxelWorldComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Component settings. Tick exists only to advance mesh-swap crossfades and stays
	// disabled while no fade is active (StartChunkCrossfade enables it on demand).
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

	AdvanceChunkCrossfades();
}

FBoxSphereBounds UVoxelWorldComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// Use very large bounds to prevent the component from being culled
	// Individual chunks are culled in GetDynamicMeshElements
	// This is standard practice for infinite/streaming terrain systems
	const float HalfWorldSize = 500000.0f;  // 5km in each direction
	const float VerticalExtent = 100000.0f;  // 1km up/down

	FBox LargeBounds(
		FVector(-HalfWorldSize, -HalfWorldSize, -VerticalExtent),
		FVector(HalfWorldSize, HalfWorldSize, VerticalExtent)
	);

	return FBoxSphereBounds(LargeBounds.TransformBy(LocalToWorld));
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

		// Bounds are already in world space (vertices include chunk world position)
		Info.Bounds = GPUData.LocalBounds;
		Info.LODLevel = GPUData.LODLevel;
		Info.bIsVisible = GPUData.bIsVisible;

		bTotalBoundsDirty = true;
	}

	// Update statistics
	CachedVertexCount += GPUData.VertexCount;
	CachedTriangleCount += GPUData.IndexCount / 3;
	CachedGPUMemory += GPUData.GetGPUMemoryUsage();

	// Mesh replacement -> crossfade (decided here on the game thread; the flag travels with
	// the swap so game and render side can never disagree about it)
	const bool bCrossfade = bChunkExisted && CanCrossfade();

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

	if (bCrossfade)
	{
		// Attach enqueued after the swap command — ordering is deterministic
		StartChunkCrossfade(ChunkCoord);
	}
	else if (ActiveFades.Contains(ChunkCoord))
	{
		// Mesh replaced without fading (e.g. kill-switch flipped mid-fade) — drop the stale fade
		CancelChunkCrossfade(ChunkCoord, /*bClearRenderState=*/true);
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

		bTotalBoundsDirty = true;
	}

	// Update statistics
	CachedVertexCount += VertexCount;
	CachedTriangleCount += IndexCount / 3;
	CachedGPUMemory += (VertexCount * sizeof(FVoxelVertex)) + (IndexCount * sizeof(uint32));

	// Calculate chunk world position (includes WorldOrigin offset)
	FVector ChunkWorldPos = WorldOrigin + FVector(ChunkCoord) * ChunkWorldSize;

	// Mesh replacement -> crossfade. The flag rides the pending add into the batched swap;
	// the fade-state attach below lands BEFORE the flush, which is fine — fading only renders
	// once both the attach and the retained Previous mesh exist (order-independent).
	const bool bCrossfade = bChunkExisted && CanCrossfade();

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

	if (bCrossfade)
	{
		StartChunkCrossfade(ChunkCoord);
	}
	else if (ActiveFades.Contains(ChunkCoord))
	{
		CancelChunkCrossfade(ChunkCoord, /*bClearRenderState=*/true);
	}

	// Update bounds
	UpdateBounds();
}

void UVoxelWorldComponent::RemoveChunk(const FIntVector& ChunkCoord)
{
	check(IsInGameThread());

	// Update game thread tracking
	{
		FScopeLock Lock(&ChunkInfoLock);

		if (ChunkInfoMap.Remove(ChunkCoord) > 0)
		{
			bTotalBoundsDirty = true;
		}
	}

	// A removed chunk cannot keep fading. Clear the proxy-side state immediately (the batched
	// removal would too, but its flush cadence should not gate MID recycling safety).
	if (ActiveFades.Contains(ChunkCoord))
	{
		CancelChunkCrossfade(ChunkCoord, /*bClearRenderState=*/true);
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

	// Drop all crossfades (ClearAllChunks_RenderThread clears the proxy-side state)
	for (auto& FadePair : ActiveFades)
	{
		RetireFadeMIDs(FadePair.Value);
	}
	ActiveFades.Empty();

	// Reset statistics
	CachedVertexCount = 0;
	CachedTriangleCount = 0;
	CachedGPUMemory = 0;

	// Clear any pending batched operations - they're now obsolete
	PendingAdds.Empty();
	PendingRemovals.Empty();

	// Enqueue render thread clear (chunks + water tiles)
	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (Proxy)
	{
		ENQUEUE_RENDER_COMMAND(ClearVoxelChunksAndWater)(
			[Proxy](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->ClearAllChunks_RenderThread();
				Proxy->ClearAllWaterTiles_RenderThread();
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
		for (auto& FadePair : ActiveFades)
		{
			RetireFadeMIDs(FadePair.Value);
		}
		ActiveFades.Empty();

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

void UVoxelWorldComponent::StartChunkCrossfade(const FIntVector& ChunkCoord)
{
	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	UWorld* World = GetWorld();
	if (!Proxy || !World)
	{
		return;
	}

	// A resubmit during an active fade restarts it (the proxy released the older Previous
	// and retained the just-replaced mesh) — same MIDs, alpha snaps back to 0.
	FActiveChunkFade& Fade = ActiveFades.FindOrAdd(ChunkCoord);
	Fade.StartTime = World->GetRealTimeSeconds();
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
		// MID creation failed — abort the fade and release the mesh the swap already
		// retained, so nothing lingers on chunks that never remesh again.
		UE_LOG(LogVoxelRendering, Warning,
			TEXT("StartChunkCrossfade: failed to create fade MIDs for chunk %s — swapping instantly"),
			*ChunkCoord.ToString());
		RetireFadeMIDs(Fade);
		ActiveFades.Remove(ChunkCoord);

		ENQUEUE_RENDER_COMMAND(AbortVoxelChunkFade)(
			[Proxy, ChunkCoord](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->ClearChunkFadeState_RenderThread(ChunkCoord);
			}
		);
		return;
	}

	Fade.InMID->SetScalarParameterValue(VoxelFadeAlphaParamName, 0.0f);
	Fade.OutMID->SetScalarParameterValue(VoxelFadeAlphaParamName, 0.0f);

	const FMaterialRenderProxy* FadeInProxy = Fade.InMID->GetRenderProxy();
	const FMaterialRenderProxy* FadeOutProxy = Fade.OutMID->GetRenderProxy();

	ENQUEUE_RENDER_COMMAND(AttachVoxelChunkFade)(
		[Proxy, ChunkCoord, FadeInProxy, FadeOutProxy](FRHICommandListImmediate& RHICmdList)
		{
			Proxy->SetChunkFadeState_RenderThread(ChunkCoord, FadeInProxy, FadeOutProxy);
		}
	);

	SetComponentTickEnabled(true);
}

void UVoxelWorldComponent::CancelChunkCrossfade(const FIntVector& ChunkCoord, bool bClearRenderState)
{
	FActiveChunkFade Fade;
	if (!ActiveFades.RemoveAndCopyValue(ChunkCoord, Fade))
	{
		return;
	}

	RetireFadeMIDs(Fade);

	if (bClearRenderState)
	{
		if (FVoxelSceneProxy* Proxy = GetVoxelSceneProxy())
		{
			ENQUEUE_RENDER_COMMAND(ClearVoxelChunkFade)(
				[Proxy, ChunkCoord](FRHICommandListImmediate& RHICmdList)
				{
					Proxy->ClearChunkFadeState_RenderThread(ChunkCoord);
				}
			);
		}
	}
}

void UVoxelWorldComponent::AdvanceChunkCrossfades()
{
	check(IsInGameThread());

	// Recycle MIDs retired last tick — by now the render thread has executed the fade-clear
	// commands that referenced their proxies, so reuse is safe.
	FreeFadeInMIDs.Append(CoolingFadeInMIDs);
	CoolingFadeInMIDs.Reset();
	FreeFadeOutMIDs.Append(CoolingFadeOutMIDs);
	CoolingFadeOutMIDs.Reset();

	if (ActiveFades.Num() == 0)
	{
		SetComponentTickEnabled(false);
		return;
	}

	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	UWorld* World = GetWorld();
	if (!Proxy || !World)
	{
		for (auto& FadePair : ActiveFades)
		{
			RetireFadeMIDs(FadePair.Value);
		}
		ActiveFades.Empty();
		SetComponentTickEnabled(false);
		return;
	}

	const bool bFadeEnabled = CVarVoxelMeshFade.GetValueOnGameThread() != 0;
	const float Duration = FMath::Max(0.02f, CVarVoxelMeshFadeDuration.GetValueOnGameThread());
	const double Now = World->GetRealTimeSeconds();

	TArray<TPair<FIntVector, float>> AlphaBatch;
	AlphaBatch.Reserve(ActiveFades.Num());
	TArray<FIntVector> Completed;

	for (auto& FadePair : ActiveFades)
	{
		// Kill-switch mid-fade completes everything instantly (releases retained meshes)
		const float Alpha = bFadeEnabled
			? static_cast<float>((Now - FadePair.Value.StartTime) / Duration)
			: 1.0f;

		if (Alpha >= 1.0f)
		{
			Completed.Add(FadePair.Key);
			continue;
		}

		// The MIDs carry the visually-effective parameter to both fade batches
		FadePair.Value.InMID->SetScalarParameterValue(VoxelFadeAlphaParamName, Alpha);
		FadePair.Value.OutMID->SetScalarParameterValue(VoxelFadeAlphaParamName, Alpha);
		AlphaBatch.Emplace(FadePair.Key, Alpha);
	}

	// One batched render command per tick for the proxy-side bookkeeping copies
	if (AlphaBatch.Num() > 0)
	{
		ENQUEUE_RENDER_COMMAND(UpdateVoxelFadeAlphas)(
			[Proxy, Batch = MoveTemp(AlphaBatch)](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->UpdateChunkFadeAlphasBatch_RenderThread(Batch);
			}
		);
	}

	for (const FIntVector& ChunkCoord : Completed)
	{
		FActiveChunkFade Fade;
		if (ActiveFades.RemoveAndCopyValue(ChunkCoord, Fade))
		{
			RetireFadeMIDs(Fade);
		}

		ENQUEUE_RENDER_COMMAND(CompleteVoxelChunkFade)(
			[Proxy, ChunkCoord](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->ClearChunkFadeState_RenderThread(ChunkCoord);
			}
		);
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
