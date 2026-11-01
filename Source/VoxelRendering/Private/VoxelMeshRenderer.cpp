// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMeshRenderer.h"
#include "VoxelRendering.h"
#include "VoxelRegionComponent.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelMaterialAtlas.h"
#include "VoxelVertex.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "VT/RuntimeVirtualTexture.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

// ==================== FVoxelMeshRenderer ====================

FVoxelMeshRenderer::FVoxelMeshRenderer()
{
}

FVoxelMeshRenderer::~FVoxelMeshRenderer()
{
	if (bIsInitialized)
	{
		Shutdown();
	}
}

// ==================== Lifecycle ====================

void FVoxelMeshRenderer::Initialize(UWorld* World, const UVoxelWorldConfiguration* WorldConfig)
{
	check(IsInGameThread());
	check(World);
	check(WorldConfig);

	if (bIsInitialized)
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("FVoxelMeshRenderer::Initialize called when already initialized"));
		return;
	}

	CachedWorld = World;
	CachedConfig = WorldConfig;

	// Cache configuration values
	VoxelSize = WorldConfig->VoxelSize;
	ChunkWorldSize = WorldConfig->GetChunkWorldSize();
	// Render regions (WM-BP7): a planet splits into regions of voxel.Render.RegionChunks^3 chunks, heightmaps keep one
	Regions = FVoxelRenderRegions::FromConfig(*WorldConfig);

	// Spawn container actor (no fixed Name — multiple renderers may coexist)
	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags |= RF_Transient;

	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), SpawnParams);
	if (!Actor)
	{
		UE_LOG(LogVoxelRendering, Error, TEXT("FVoxelMeshRenderer: Failed to spawn container actor"));
		return;
	}

	ContainerActor = Actor;

#if WITH_EDITOR
	Actor->SetActorLabel(TEXT("VoxelCustomVFContainer"));
#endif

	// The primary world component: the only one on a heightmap world (at the world origin), region (0, 0, 0) on a planet.
	// It lives as long as the renderer; region components come and go with their content.
	OriginRegionComponent = CreateRegionComponent(FIntVector::ZeroValue);
	if (!OriginRegionComponent)
	{
		UE_LOG(LogVoxelRendering, Error, TEXT("FVoxelMeshRenderer: Failed to create world component"));
		Actor->Destroy();
		ContainerActor.Reset();
		return;
	}

	const bool bIsSmooth = (WorldConfig->MeshingMode != EMeshingMode::Cubic);
	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelMeshRenderer: MeshingMode=%s, bUseSmoothMeshing=%s, render regions %s"),
		bIsSmooth ? TEXT("Triplanar") : TEXT("Cubic"),
		bIsSmooth ? TEXT("true") : TEXT("false"),
		Regions.IsSplit() ? *FString::Printf(TEXT("of %d^3 chunks"), Regions.RegionChunks) : TEXT("off (one primitive at the origin)"));

	bIsInitialized = true;
	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelMeshRenderer initialized"));
}

UVoxelRegionComponent* FVoxelMeshRenderer::CreateRegionComponent(const FIntVector& Region)
{
	check(IsInGameThread());
	AActor* Actor = ContainerActor.Get();
	const UVoxelWorldConfiguration* WorldConfig = CachedConfig.Get();
	if (!Actor || !WorldConfig)
	{
		return nullptr;
	}

	UVoxelRegionComponent* Component = NewObject<UVoxelRegionComponent>(Actor, NAME_None, RF_Transient);
	if (!Component)
	{
		return nullptr;
	}

	// Configure component
	Component->SetVoxelSize(VoxelSize);
	Component->SetChunkWorldSize(ChunkWorldSize);
	Component->SetWorldOrigin(WorldConfig->WorldOrigin);
	// A planet's face types (top, side, bottom) go by the radial up (spherical SP-D3); its centre is WorldOrigin.
	Component->SetRadialFaceUp(WorldConfig->WorldMode == EWorldMode::SphericalPlanet, WorldConfig->WorldOrigin);

	// Sync material mode with configuration's meshing mode
	// Both MarchingCubes and DualContouring use triplanar texturing instead of atlas faces
	Component->SetUseSmoothMeshing(WorldConfig->MeshingMode != EMeshingMode::Cubic);

	// The region's origin (WM-BP7): the proxy stores its vertices relative to the component's location. The world origin
	// (0, 0, 0) when not split, as the single component always was.
	Component->SetUsingAbsoluteLocation(true);
	Component->SetUsingAbsoluteRotation(true);
	Component->SetUsingAbsoluteScale(true);
	Component->SetWorldLocation(Regions.GetRegionOrigin(Region));

	// Set initial material BEFORE registration - scene proxy is created during RegisterComponent
	if (CurrentMaterial.IsValid())
	{
		Component->SetMaterial(0, CurrentMaterial.Get());
	}

	// Assign Runtime Virtual Textures from the configuration BEFORE registration. The scene proxy
	// is created during RegisterComponent() and its base FPrimitiveSceneProxy ctor populates
	// RuntimeVirtualTextureMaterialTypes from the component's RuntimeVirtualTextures array, so this
	// must be set first for the proxy to opt into the RVT pass and emit RVT mesh batches.
	if (WorldConfig->RuntimeVirtualTextures.Num() > 0)
	{
		Component->RuntimeVirtualTextures.Reset(WorldConfig->RuntimeVirtualTextures.Num());
		for (const TObjectPtr<URuntimeVirtualTexture>& RVT : WorldConfig->RuntimeVirtualTextures)
		{
			if (RVT)
			{
				Component->RuntimeVirtualTextures.Add(RVT);
			}
		}
		UE_LOG(LogVoxelRendering, Verbose, TEXT("FVoxelMeshRenderer: Assigned %d Runtime Virtual Texture(s) to region %s"),
			Component->RuntimeVirtualTextures.Num(), *Region.ToString());
	}

	// Attach and register (this creates the scene proxy with the material set above)
	Component->SetupAttachment(Actor->GetRootComponent());
	Component->RegisterComponent();

	// Replay the settings every component has (SetWaterMaterial, SetMaterialAtlas, the LOD parameters, the viewer)
	if (CurrentWaterMaterial.IsValid())
	{
		Component->SetWaterMaterial(CurrentWaterMaterial.Get());
	}
	if (CurrentAtlas.IsValid())
	{
		Component->SetMaterialAtlas(CurrentAtlas.Get());
		if (CurrentMaterial.IsValid() && Component->GetMaterial(0) == CurrentMaterial.Get())
		{
			Component->CreateVoxelMaterialInstance(CurrentMaterial.Get());
		}
	}
	if (bHasViewerPosition)
	{
		Component->SetFadeViewerPosition(CurrentViewerPosition);
	}

	RegionComponents.Add(Region, Component);
	return Component;
}

UVoxelRegionComponent* FVoxelMeshRenderer::FindRegionComponent(const FIntVector& ChunkCoord) const
{
	const TObjectPtr<UVoxelRegionComponent>* Found = RegionComponents.Find(Regions.GetRegion(ChunkCoord));
	return Found ? Found->Get() : nullptr;
}

UVoxelRegionComponent* FVoxelMeshRenderer::GetOrCreateRegionComponent(const FIntVector& ChunkCoord)
{
	const FIntVector Region = Regions.GetRegion(ChunkCoord);
	if (const TObjectPtr<UVoxelRegionComponent>* Found = RegionComponents.Find(Region))
	{
		return Found->Get();
	}
	return CreateRegionComponent(Region);
}

void FVoxelMeshRenderer::RetireEmptyRegions()
{
	if (!Regions.IsSplit())
	{
		return;
	}
	for (auto It = RegionComponents.CreateIterator(); It; ++It)
	{
		UVoxelRegionComponent* Component = It.Value();
		if (Component == OriginRegionComponent)
		{
			continue;
		}
		if (!Component || !Component->HasRenderContent())
		{
			if (Component)
			{
				Component->DestroyComponent();
			}
			It.RemoveCurrent();
		}
	}
}

void FVoxelMeshRenderer::GetRegionComponents(TArray<TPair<FIntVector, UVoxelRegionComponent*>>& OutComponents) const
{
	OutComponents.Reset(RegionComponents.Num());
	for (const TPair<FIntVector, TObjectPtr<UVoxelRegionComponent>>& Pair : RegionComponents)
	{
		OutComponents.Emplace(Pair.Key, Pair.Value.Get());
	}
}

void FVoxelMeshRenderer::Shutdown()
{
	check(IsInGameThread());

	if (!bIsInitialized)
	{
		return;
	}

	// Clear all chunks and water tiles first
	ClearAllChunks();
	ClearAllWaterTiles();

	// Destroy the components (the primary one is among them)
	for (const TPair<FIntVector, TObjectPtr<UVoxelRegionComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->DestroyComponent();
		}
	}
	RegionComponents.Empty();
	OriginRegionComponent = nullptr;

	// Destroy container actor
	if (ContainerActor.IsValid())
	{
		ContainerActor->Destroy();
		ContainerActor.Reset();
	}

	CachedWorld.Reset();
	CachedConfig.Reset();
	CurrentMaterial.Reset();
	CurrentWaterMaterial.Reset();
	CurrentAtlas.Reset();
	bHasViewerPosition = false;

	ChunkStatsMap.Empty();
	TotalVertexCount = 0;
	TotalTriangleCount = 0;
	TotalGPUMemory = 0;

	bIsInitialized = false;
	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelMeshRenderer shutdown"));
}

bool FVoxelMeshRenderer::IsInitialized() const
{
	return bIsInitialized && OriginRegionComponent != nullptr;
}

// ==================== Mesh Updates ====================

void FVoxelMeshRenderer::UpdateChunkMeshFromCPU(
	const FIntVector& ChunkCoord,
	int32 LODLevel,
	const FChunkMeshData& MeshData)
{
	if (!MeshData.IsValid())
	{
		RemoveChunk(ChunkCoord);
		return;
	}
	// Caller keeps the mesh data — copy the index buffer
	SubmitChunkMeshInternal(ChunkCoord, LODLevel, MeshData, TArray<uint32>(MeshData.Indices));
}

void FVoxelMeshRenderer::UpdateChunkMeshFromCPU(
	const FIntVector& ChunkCoord,
	int32 LODLevel,
	FChunkMeshData&& MeshData)
{
	if (!MeshData.IsValid())
	{
		RemoveChunk(ChunkCoord);
		return;
	}
	// Caller is done with the mesh data — move the index buffer straight into the pending
	// batch. (Vertices always need converting to FVoxelVertex, so only indices benefit.)
	SubmitChunkMeshInternal(ChunkCoord, LODLevel, MeshData, MoveTemp(MeshData.Indices));
}

void FVoxelMeshRenderer::SubmitChunkMeshInternal(
	const FIntVector& ChunkCoord,
	int32 LODLevel,
	const FChunkMeshData& MeshData,
	TArray<uint32>&& Indices)
{
	check(IsInGameThread());

	if (!IsInitialized())
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("FVoxelMeshRenderer::UpdateChunkMeshFromCPU called before initialization"));
		return;
	}

	// Convert CPU mesh data to FVoxelVertex array; local bounds are computed in the same pass
	// (this used to be a second full sweep over the positions)
	TArray<FVoxelVertex> Vertices;
	FBox LocalBounds(ForceInit);
	ConvertToVoxelVertices(MeshData, Vertices, LocalBounds);

	// Update statistics
	FChunkStats* ExistingStats = ChunkStatsMap.Find(ChunkCoord);
	if (ExistingStats)
	{
		TotalVertexCount -= ExistingStats->VertexCount;
		TotalTriangleCount -= ExistingStats->TriangleCount;
		TotalGPUMemory -= ExistingStats->MemoryUsage;
	}

	FChunkStats& Stats = ChunkStatsMap.FindOrAdd(ChunkCoord);
	Stats.VertexCount = Vertices.Num();
	Stats.TriangleCount = Indices.Num() / 3;
	Stats.LODLevel = LODLevel;
	Stats.MemoryUsage = (Vertices.Num() * sizeof(FVoxelVertex)) + (Indices.Num() * sizeof(uint32));
	// Bounds are in local space here, will be offset in scene proxy
	Stats.Bounds = LocalBounds;

	TotalVertexCount += Stats.VertexCount;
	TotalTriangleCount += Stats.TriangleCount;
	TotalGPUMemory += Stats.MemoryUsage;

	// Use DIRECT CPU PATH - no GPU buffer roundtrip!
	// This passes CPU arrays directly to the render thread, avoiding:
	// 1. Creating intermediate GPU buffers
	// 2. Bouncing back to game thread
	// 3. GPU readback stalls in scene proxy
	if (UVoxelRegionComponent* Component = GetOrCreateRegionComponent(ChunkCoord))
	{
		Component->UpdateChunkBuffersFromCPUData(
			ChunkCoord,
			MoveTemp(Vertices),
			MoveTemp(Indices),
			LODLevel,
			LocalBounds
		);
	}

	UE_LOG(LogVoxelRendering, Verbose, TEXT("FVoxelMeshRenderer: Updated chunk %s (DIRECT CPU path) - %d verts, %d tris"),
		*ChunkCoord.ToString(), Stats.VertexCount, Stats.TriangleCount);
}

void FVoxelMeshRenderer::RemoveChunk(const FIntVector& ChunkCoord)
{
	check(IsInGameThread());

	// Update statistics
	FChunkStats* Stats = ChunkStatsMap.Find(ChunkCoord);
	if (Stats)
	{
		TotalVertexCount -= Stats->VertexCount;
		TotalTriangleCount -= Stats->TriangleCount;
		TotalGPUMemory -= Stats->MemoryUsage;
		ChunkStatsMap.Remove(ChunkCoord);
	}

	// Forward to the chunk's region component
	if (UVoxelRegionComponent* Component = FindRegionComponent(ChunkCoord))
	{
		Component->RemoveChunk(ChunkCoord);
	}
}

void FVoxelMeshRenderer::ClearAllChunks()
{
	check(IsInGameThread());

	ChunkStatsMap.Empty();
	TotalVertexCount = 0;
	TotalTriangleCount = 0;
	TotalGPUMemory = 0;

	for (const TPair<FIntVector, TObjectPtr<UVoxelRegionComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->ClearAllChunks();
		}
	}
}

// ==================== Material Management ====================

void FVoxelMeshRenderer::SetMaterial(UMaterialInterface* Material)
{
	check(IsInGameThread());

	CurrentMaterial = Material;

	for (const TPair<FIntVector, TObjectPtr<UVoxelRegionComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->SetMaterial(0, Material);
		}
	}
}

UMaterialInterface* FVoxelMeshRenderer::GetMaterial() const
{
	return CurrentMaterial.Get();
}

UMaterialInterface* FVoxelMeshRenderer::GetSurfaceMaterial() const
{
	// The world component draws with its atlas instance once SetMaterialAtlas made one.
	UMaterialInterface* Drawn = OriginRegionComponent ? OriginRegionComponent->GetMaterial(0) : nullptr;
	return Drawn ? Drawn : CurrentMaterial.Get();
}

void FVoxelMeshRenderer::UpdateMaterialParameters()
{
	// Material parameters update automatically through UMaterialInstanceDynamic
	// Force a render state update to pick up any changes
	for (const TPair<FIntVector, TObjectPtr<UVoxelRegionComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->MarkRenderStateDirty();
		}
	}
}

void FVoxelMeshRenderer::SetMaterialAtlas(UVoxelMaterialAtlas* Atlas)
{
	check(IsInGameThread());

	UE_LOG(LogVoxelRendering, Log, TEXT("SetMaterialAtlas called - OriginRegionComponent: %s, CurrentMaterial: %s, Atlas: %s"),
		OriginRegionComponent ? TEXT("Valid") : TEXT("NULL"),
		CurrentMaterial.IsValid() ? *CurrentMaterial->GetName() : TEXT("NULL"),
		Atlas ? *Atlas->GetName() : TEXT("NULL"));

	CurrentAtlas = Atlas;
	for (const TPair<FIntVector, TObjectPtr<UVoxelRegionComponent>>& Pair : RegionComponents)
	{
		UVoxelRegionComponent* Component = Pair.Value;
		if (!Component)
		{
			continue;
		}
		// Set the atlas FIRST - CreateVoxelMaterialInstance calls UpdateMaterialAtlasParameters
		// which needs the atlas to be set
		Component->SetMaterialAtlas(Atlas);

		UMaterialInterface* ComponentMaterial = Component->GetMaterial(0);
		UE_LOG(LogVoxelRendering, Log, TEXT("  ComponentMaterial: %s, CurrentMaterial.Get(): %s, Match: %s"),
			ComponentMaterial ? *ComponentMaterial->GetName() : TEXT("NULL"),
			CurrentMaterial.IsValid() ? *CurrentMaterial.Get()->GetName() : TEXT("NULL"),
			(ComponentMaterial == CurrentMaterial.Get()) ? TEXT("YES") : TEXT("NO"));

		// Create a dynamic material instance if we have a material but no dynamic instance yet
		// This is required for the LUT texture to be passed to the material
		if (CurrentMaterial.IsValid() && Component->GetMaterial(0) == CurrentMaterial.Get())
		{
			UE_LOG(LogVoxelRendering, Log, TEXT("  Creating dynamic material instance..."));
			// Create dynamic instance from the current material
			Component->CreateVoxelMaterialInstance(CurrentMaterial.Get());
		}
		else
		{
			UE_LOG(LogVoxelRendering, Warning, TEXT("  NOT creating dynamic material instance - condition failed"));
		}
	}
}

UVoxelMaterialAtlas* FVoxelMeshRenderer::GetMaterialAtlas() const
{
	if (OriginRegionComponent)
	{
		return OriginRegionComponent->GetMaterialAtlas();
	}
	return nullptr;
}

// ==================== Water Tile Support ====================

void FVoxelMeshRenderer::SetWaterMaterial(UMaterialInterface* Material)
{
	check(IsInGameThread());

	CurrentWaterMaterial = Material;

	for (const TPair<FIntVector, TObjectPtr<UVoxelRegionComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->SetWaterMaterial(Material);
		}
	}
}

void FVoxelMeshRenderer::UpdateWaterTileMesh(
	const FIntVector2& TileCoord,
	const FChunkMeshData& WaterMeshData)
{
	check(IsInGameThread());

	if (!IsInitialized())
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("FVoxelMeshRenderer::UpdateWaterTileMesh called before initialization"));
		return;
	}

	if (!WaterMeshData.IsValid())
	{
		RemoveWaterTile(TileCoord);
		return;
	}

	// Convert CPU mesh data to FVoxelVertex array (bounds unused for water tiles)
	TArray<FVoxelVertex> Vertices;
	FBox UnusedBounds(ForceInit);
	ConvertToVoxelVertices(WaterMeshData, Vertices, UnusedBounds);

	// Copy indices
	TArray<uint32> Indices = WaterMeshData.Indices;

	// Calculate tile world position
	const FVector WorldOrigin = CachedConfig.IsValid() ? CachedConfig->WorldOrigin : FVector::ZeroVector;
	const FVector TileWorldPos = WorldOrigin + FVector(TileCoord.X * ChunkWorldSize, TileCoord.Y * ChunkWorldSize, 0.0);

	// Forward to the tile's region component (the region of its column's z-0 chunk)
	if (UVoxelRegionComponent* Component = GetOrCreateRegionComponent(WaterTileChunk(TileCoord)))
	{
		Component->UpdateWaterTileFromCPUData(TileCoord, MoveTemp(Vertices), MoveTemp(Indices), TileWorldPos);
	}

	UE_LOG(LogVoxelRendering, Verbose, TEXT("FVoxelMeshRenderer: Updated water tile (%d,%d) - %d verts"),
		TileCoord.X, TileCoord.Y, WaterMeshData.Positions.Num());
}

void FVoxelMeshRenderer::RemoveWaterTile(const FIntVector2& TileCoord)
{
	check(IsInGameThread());

	if (UVoxelRegionComponent* Component = FindRegionComponent(WaterTileChunk(TileCoord)))
	{
		Component->RemoveWaterTile(TileCoord);
	}
}

void FVoxelMeshRenderer::ClearAllWaterTiles()
{
	check(IsInGameThread());

	for (const TPair<FIntVector, TObjectPtr<UVoxelRegionComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->ClearAllWaterTiles();
		}
	}
}

// ==================== Seam Meshes (seam-ownership P1) ====================

void FVoxelMeshRenderer::UpdateSeamMeshFromCPU(
	const FIntVector& OwnerChunkCoord,
	uint8 Axis,
	int32 LODLevel,
	FChunkMeshData&& MeshData,
	bool bCrossfade)
{
	check(IsInGameThread());

	if (!IsInitialized())
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("FVoxelMeshRenderer::UpdateSeamMeshFromCPU called before initialization"));
		return;
	}

	if (!MeshData.IsValid())
	{
		// Seam dissolved (no surface crosses the face anymore) — drop the bucket (fading it out
		// with the surface it belonged to when the caller asked for a crossfade).
		if (UVoxelRegionComponent* Component = FindRegionComponent(OwnerChunkCoord))
		{
			Component->RemoveSeamMesh(OwnerChunkCoord, Axis, /*bFadeOut*/ bCrossfade);
		}
		return;
	}

	// Convert CPU mesh data to FVoxelVertex array (bounds computed in the proxy for seams)
	TArray<FVoxelVertex> Vertices;
	FBox UnusedBounds(ForceInit);
	ConvertToVoxelVertices(MeshData, Vertices, UnusedBounds);

	// Owner chunk world position — same layout the chunk path uses (WorldOrigin + coord * chunk size)
	const FVector WorldOrigin = CachedConfig.IsValid() ? CachedConfig->WorldOrigin : FVector::ZeroVector;
	const FVector OwnerWorldPos = WorldOrigin + FVector(OwnerChunkCoord) * ChunkWorldSize;

	// A seam lives with its owner chunk's region
	if (UVoxelRegionComponent* Component = GetOrCreateRegionComponent(OwnerChunkCoord))
	{
		Component->UpdateSeamMeshFromCPUData(
			OwnerChunkCoord, Axis, LODLevel,
			MoveTemp(Vertices), MoveTemp(MeshData.Indices), OwnerWorldPos, bCrossfade);
	}

	UE_LOG(LogVoxelRendering, Verbose, TEXT("FVoxelMeshRenderer: Updated seam mesh %s axis=%d - %d verts"),
		*OwnerChunkCoord.ToString(), Axis, MeshData.Positions.Num());
}

void FVoxelMeshRenderer::RemoveSeamMesh(const FIntVector& OwnerChunkCoord, uint8 Axis)
{
	check(IsInGameThread());

	if (UVoxelRegionComponent* Component = FindRegionComponent(OwnerChunkCoord))
	{
		Component->RemoveSeamMesh(OwnerChunkCoord, Axis);
	}
}

void FVoxelMeshRenderer::SetChunkFadeHold(const FIntVector& ChunkCoord, bool bHold)
{
	check(IsInGameThread());

	// A hold is set before the chunk's mesh arrives: the region's component may not exist yet.
	if (UVoxelRegionComponent* Component = bHold ? GetOrCreateRegionComponent(ChunkCoord) : FindRegionComponent(ChunkCoord))
	{
		Component->SetChunkFadeHold(ChunkCoord, bHold);
	}
}

void FVoxelMeshRenderer::SetSeamFadeHold(const FIntVector& OwnerChunkCoord, bool bHold)
{
	check(IsInGameThread());

	if (UVoxelRegionComponent* Component = bHold ? GetOrCreateRegionComponent(OwnerChunkCoord) : FindRegionComponent(OwnerChunkCoord))
	{
		Component->SetSeamFadeHold(OwnerChunkCoord, bHold);
	}
}

float FVoxelMeshRenderer::GetSeamFadeRemainingSeconds(const FIntVector& OwnerChunkCoord) const
{
	check(IsInGameThread());

	const UVoxelRegionComponent* Component = FindRegionComponent(OwnerChunkCoord);
	return Component ? Component->GetSeamFadeRemainingSeconds(OwnerChunkCoord) : 0.0f;
}

void FVoxelMeshRenderer::SetViewerPosition(const FVector& ViewerPosition)
{
	check(IsInGameThread());

	CurrentViewerPosition = ViewerPosition;
	bHasViewerPosition = true;
	for (const TPair<FIntVector, TObjectPtr<UVoxelRegionComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->SetFadeViewerPosition(ViewerPosition);
		}
	}
}

bool FVoxelMeshRenderer::WillFadeInChunk(const FIntVector& ChunkCoord) const
{
	check(IsInGameThread());

	// The answer belongs to the component the chunk's mesh will go to (its fade scheduler and cap). A region with no
	// component yet gets one now: the mesh follows this question at once.
	UVoxelRegionComponent* Component = const_cast<FVoxelMeshRenderer*>(this)->GetOrCreateRegionComponent(ChunkCoord);
	return Component && Component->WillFadeInChunk(ChunkCoord);
}

void FVoxelMeshRenderer::ClearAllSeamMeshes()
{
	check(IsInGameThread());

	for (const TPair<FIntVector, TObjectPtr<UVoxelRegionComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->ClearAllSeamMeshes();
		}
	}
}

void FVoxelMeshRenderer::GetSeamMeshOwners(TArray<FIntVector>& OutOwners) const
{
	OutOwners.Reset();
	TArray<FIntVector> RegionOwners;
	for (const TPair<FIntVector, TObjectPtr<UVoxelRegionComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->GetSeamMeshOwners(RegionOwners);
			OutOwners.Append(RegionOwners);
		}
	}
}

bool FVoxelMeshRenderer::ConsumeLostRenderData(TArray<FIntVector>& OutChunks)
{
	OutChunks.Reset();
	bool bLost = false;
	TArray<FIntVector> RegionChunks;
	for (const TPair<FIntVector, TObjectPtr<UVoxelRegionComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value && Pair.Value->ConsumeLostRenderData(RegionChunks))
		{
			OutChunks.Append(RegionChunks);
			bLost = true;
		}
	}
	return bLost;
}

bool FVoxelMeshRenderer::GetWaterTileDrawInfo(const FIntVector2& TileCoord, FString& Out) const
{
	check(IsInGameThread());
	const FIntVector Chunk = WaterTileChunk(TileCoord);
	const FIntVector Region = Regions.GetRegion(Chunk);
	UVoxelRegionComponent* Component = FindRegionComponent(Chunk);
	if (!Component)
	{
		Out = FString::Printf(TEXT("region %s: NO component"), *Region.ToString());
		return false;
	}
	FString Line;
	const bool bHeld = Component->GetWaterTileDrawInfo(TileCoord, Line);
	Out = FString::Printf(TEXT("region %s (origin %s): %s"), *Region.ToString(), *Regions.GetRegionOrigin(Region).ToString(), *Line);
	return bHeld;
}

// ==================== Batched Operations ====================

void FVoxelMeshRenderer::FlushPendingOperations()
{
	check(IsInGameThread());

	// Delegate to the world components, each batching its pending adds/removes into a single render command
	for (const TPair<FIntVector, TObjectPtr<UVoxelRegionComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->FlushPendingOperations();
		}
	}

	// Then retire the regions left with nothing to draw (their removals are queued ahead of the proxy's destruction)
	RetireEmptyRegions();
}

// ==================== Queries ====================

bool FVoxelMeshRenderer::IsChunkLoaded(const FIntVector& ChunkCoord) const
{
	return ChunkStatsMap.Contains(ChunkCoord);
}

int32 FVoxelMeshRenderer::GetLoadedChunkCount() const
{
	return ChunkStatsMap.Num();
}

void FVoxelMeshRenderer::GetLoadedChunks(TArray<FIntVector>& OutChunks) const
{
	OutChunks.Reset(ChunkStatsMap.Num());
	for (const auto& Pair : ChunkStatsMap)
	{
		OutChunks.Add(Pair.Key);
	}
}

int64 FVoxelMeshRenderer::GetGPUMemoryUsage() const
{
	return TotalGPUMemory;
}

int64 FVoxelMeshRenderer::GetTotalVertexCount() const
{
	return TotalVertexCount;
}

int64 FVoxelMeshRenderer::GetTotalTriangleCount() const
{
	return TotalTriangleCount;
}

// ==================== Bounds ====================

bool FVoxelMeshRenderer::GetChunkBounds(const FIntVector& ChunkCoord, FBox& OutBounds) const
{
	const FChunkStats* Stats = ChunkStatsMap.Find(ChunkCoord);
	if (Stats)
	{
		OutBounds = Stats->Bounds;
		return true;
	}
	return false;
}

FBox FVoxelMeshRenderer::GetTotalBounds() const
{
	FBox TotalBounds(ForceInit);

	for (const auto& Pair : ChunkStatsMap)
	{
		if (Pair.Value.Bounds.IsValid)
		{
			TotalBounds += Pair.Value.Bounds;
		}
	}

	return TotalBounds;
}

// ==================== Debugging ====================

FString FVoxelMeshRenderer::GetDebugStats() const
{
	int64 InteriorStarts = 0, InteriorRestarts = 0, SeamStarts = 0, SeamRestarts = 0;
	int64 InteriorAppears = 0, SeamAppears = 0;
	int32 ActiveFades = 0;
	for (const TPair<FIntVector, TObjectPtr<UVoxelRegionComponent>>& Pair : RegionComponents)
	{
		const UVoxelRegionComponent* Component = Pair.Value;
		if (!Component)
		{
			continue;
		}
		int64 Starts = 0, Restarts = 0;
		Component->GetFadeCounters(/*bSeams*/ false, Starts, Restarts);
		InteriorStarts += Starts;
		InteriorRestarts += Restarts;
		Component->GetFadeCounters(/*bSeams*/ true, Starts, Restarts);
		SeamStarts += Starts;
		SeamRestarts += Restarts;
		InteriorAppears += Component->GetFadeAppearCount(/*bSeams*/ false);
		SeamAppears += Component->GetFadeAppearCount(/*bSeams*/ true);
		ActiveFades += Component->GetActiveFadeCount();
	}
	return FString::Printf(
		TEXT("Custom VF Renderer Stats:\n")
		TEXT("  Chunks: %d\n")
		TEXT("  Vertices: %lld\n")
		TEXT("  Triangles: %lld\n")
		TEXT("  GPU Memory: %.2f MB\n")
		TEXT("  Voxel Size: %.1f\n")
		TEXT("  Chunk Size: %.1f\n")
		TEXT("  Crossfades: interior %lld (mid-fade restarts %lld), seam bucket %lld (mid-fade restarts %lld)\n")
		TEXT("  Appearance fades: interior %lld, seam bucket %lld (active fades %d)\n")
		TEXT("  Render regions: %s, %d primitive(s)"),
		ChunkStatsMap.Num(),
		TotalVertexCount,
		TotalTriangleCount,
		TotalGPUMemory / (1024.0 * 1024.0),
		VoxelSize,
		ChunkWorldSize,
		InteriorStarts, InteriorRestarts, SeamStarts, SeamRestarts,
		InteriorAppears, SeamAppears, ActiveFades,
		Regions.IsSplit() ? *FString::Printf(TEXT("%d^3 chunks"), Regions.RegionChunks) : TEXT("off"), RegionComponents.Num()
	);
}

void FVoxelMeshRenderer::DrawDebugVisualization(const FLODQueryContext& Context) const
{
	// Optional debug visualization - draw chunk bounds, LOD levels, etc.
	// Implementation can use DrawDebugBox, DrawDebugString, etc.
}

FString FVoxelMeshRenderer::GetRendererTypeName() const
{
	return TEXT("CustomVF");
}

// ==================== Internal Helpers ====================

void FVoxelMeshRenderer::ConvertToVoxelVertices(
	const FChunkMeshData& MeshData,
	TArray<FVoxelVertex>& OutVertices,
	FBox& OutLocalBounds)
{
	const int32 VertexCount = MeshData.Positions.Num();
	OutVertices.SetNumUninitialized(VertexCount);
	OutLocalBounds.Init();

	// Attribute arrays are either full-size or absent — decide once, not per vertex.
	// (The per-vertex `i < Num()` checks this replaces also handled partially-filled arrays,
	// which no mesher produces; short arrays now fall back to defaults wholesale.)
	const bool bHasNormals = MeshData.Normals.Num() >= VertexCount;
	const bool bHasUVs = MeshData.UVs.Num() >= VertexCount;
	const bool bHasColors = MeshData.Colors.Num() >= VertexCount;

	FVector3f BoundsMin(FLT_MAX, FLT_MAX, FLT_MAX);
	FVector3f BoundsMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);

	for (int32 i = 0; i < VertexCount; ++i)
	{
		FVoxelVertex& Vertex = OutVertices[i];

		// Position + bounds (fused — this loop runs per submitted chunk on the game thread)
		const FVector3f& Pos = MeshData.Positions[i];
		Vertex.Position = Pos;
		BoundsMin = FVector3f::Min(BoundsMin, Pos);
		BoundsMax = FVector3f::Max(BoundsMax, Pos);

		Vertex.SetNormal(bHasNormals ? MeshData.Normals[i] : FVector3f::UpVector);
		Vertex.UV = bHasUVs ? MeshData.UVs[i] : FVector2f::ZeroVector;

		// Extract material data from vertex color
		if (bHasColors)
		{
			const FColor& Color = MeshData.Colors[i];
			Vertex.SetMaterialID(Color.R);
			Vertex.SetBiomeID(Color.G);
			Vertex.SetAO(Color.B >> 6); // Top 2 bits of blue channel
		}
		else
		{
			Vertex.SetMaterialID(0);
			Vertex.SetBiomeID(0);
			Vertex.SetAO(0);
		}
	}

	if (VertexCount > 0)
	{
		OutLocalBounds += FVector(BoundsMin);
		OutLocalBounds += FVector(BoundsMax);
	}
}

FBox FVoxelMeshRenderer::CalculateChunkBounds(const FIntVector& ChunkCoord) const
{
	const FVector WorldOrigin = CachedConfig.IsValid() ? CachedConfig->WorldOrigin : FVector::ZeroVector;
	const FVector ChunkMin = WorldOrigin + FVector(ChunkCoord) * ChunkWorldSize;
	const FVector ChunkMax = ChunkMin + FVector(ChunkWorldSize);
	return FBox(ChunkMin, ChunkMax);
}
