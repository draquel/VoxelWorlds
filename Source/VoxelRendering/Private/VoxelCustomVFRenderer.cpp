// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCustomVFRenderer.h"
#include "VoxelRendering.h"
#include "VoxelWorldComponent.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelMaterialAtlas.h"
#include "VoxelVertex.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h" // CurrentLODCollection.Get(); VoxelWorldConfiguration.h only forward-declares it
#include "VT/RuntimeVirtualTexture.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

// ==================== FVoxelCustomVFRenderer ====================

FVoxelCustomVFRenderer::FVoxelCustomVFRenderer()
{
}

FVoxelCustomVFRenderer::~FVoxelCustomVFRenderer()
{
	if (bIsInitialized)
	{
		Shutdown();
	}
}

// ==================== Lifecycle ====================

void FVoxelCustomVFRenderer::Initialize(UWorld* World, const UVoxelWorldConfiguration* WorldConfig)
{
	check(IsInGameThread());
	check(World);
	check(WorldConfig);

	if (bIsInitialized)
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("FVoxelCustomVFRenderer::Initialize called when already initialized"));
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
		UE_LOG(LogVoxelRendering, Error, TEXT("FVoxelCustomVFRenderer: Failed to spawn container actor"));
		return;
	}

	ContainerActor = Actor;

#if WITH_EDITOR
	Actor->SetActorLabel(TEXT("VoxelCustomVFContainer"));
#endif

	// The primary world component: the only one on a heightmap world (at the world origin), region (0, 0, 0) on a planet.
	// It lives as long as the renderer; region components come and go with their content.
	WorldComponent = CreateRegionComponent(FIntVector::ZeroValue);
	if (!WorldComponent)
	{
		UE_LOG(LogVoxelRendering, Error, TEXT("FVoxelCustomVFRenderer: Failed to create world component"));
		Actor->Destroy();
		ContainerActor.Reset();
		return;
	}

	const bool bIsSmooth = (WorldConfig->MeshingMode != EMeshingMode::Cubic);
	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelCustomVFRenderer: MeshingMode=%s, bUseSmoothMeshing=%s, render regions %s"),
		bIsSmooth ? TEXT("Triplanar") : TEXT("Cubic"),
		bIsSmooth ? TEXT("true") : TEXT("false"),
		Regions.IsSplit() ? *FString::Printf(TEXT("of %d^3 chunks"), Regions.RegionChunks) : TEXT("off (one primitive at the origin)"));

	bIsInitialized = true;
	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelCustomVFRenderer initialized"));
}

UVoxelWorldComponent* FVoxelCustomVFRenderer::CreateRegionComponent(const FIntVector& Region)
{
	check(IsInGameThread());
	AActor* Actor = ContainerActor.Get();
	const UVoxelWorldConfiguration* WorldConfig = CachedConfig.Get();
	if (!Actor || !WorldConfig)
	{
		return nullptr;
	}

	UVoxelWorldComponent* Component = NewObject<UVoxelWorldComponent>(Actor, NAME_None, RF_Transient);
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
		UE_LOG(LogVoxelRendering, Verbose, TEXT("FVoxelCustomVFRenderer: Assigned %d Runtime Virtual Texture(s) to region %s"),
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
	if (CurrentLODCollection.IsValid())
	{
		Component->SetLODParameterCollection(CurrentLODCollection.Get());
	}
	if (bHasLODDistances)
	{
		Component->SetLODTransitionDistances(LODStartDistance, LODEndDistance);
	}
	if (bHasViewerPosition)
	{
		Component->SetFadeViewerPosition(CurrentViewerPosition);
	}

	RegionComponents.Add(Region, Component);
	return Component;
}

UVoxelWorldComponent* FVoxelCustomVFRenderer::FindRegionComponent(const FIntVector& ChunkCoord) const
{
	const TObjectPtr<UVoxelWorldComponent>* Found = RegionComponents.Find(Regions.GetRegion(ChunkCoord));
	return Found ? Found->Get() : nullptr;
}

UVoxelWorldComponent* FVoxelCustomVFRenderer::GetOrCreateRegionComponent(const FIntVector& ChunkCoord)
{
	const FIntVector Region = Regions.GetRegion(ChunkCoord);
	if (const TObjectPtr<UVoxelWorldComponent>* Found = RegionComponents.Find(Region))
	{
		return Found->Get();
	}
	return CreateRegionComponent(Region);
}

void FVoxelCustomVFRenderer::RetireEmptyRegions()
{
	if (!Regions.IsSplit())
	{
		return;
	}
	for (auto It = RegionComponents.CreateIterator(); It; ++It)
	{
		UVoxelWorldComponent* Component = It.Value();
		if (Component == WorldComponent)
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

void FVoxelCustomVFRenderer::GetRegionComponents(TArray<TPair<FIntVector, UVoxelWorldComponent*>>& OutComponents) const
{
	OutComponents.Reset(RegionComponents.Num());
	for (const TPair<FIntVector, TObjectPtr<UVoxelWorldComponent>>& Pair : RegionComponents)
	{
		OutComponents.Emplace(Pair.Key, Pair.Value.Get());
	}
}

void FVoxelCustomVFRenderer::Shutdown()
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
	for (const TPair<FIntVector, TObjectPtr<UVoxelWorldComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->DestroyComponent();
		}
	}
	RegionComponents.Empty();
	WorldComponent = nullptr;

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
	CurrentLODCollection.Reset();
	bHasLODDistances = false;
	bHasViewerPosition = false;

	ChunkStatsMap.Empty();
	TotalVertexCount = 0;
	TotalTriangleCount = 0;
	TotalGPUMemory = 0;

	bIsInitialized = false;
	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelCustomVFRenderer shutdown"));
}

bool FVoxelCustomVFRenderer::IsInitialized() const
{
	return bIsInitialized && WorldComponent != nullptr;
}

// ==================== Mesh Updates ====================

void FVoxelCustomVFRenderer::UpdateChunkMesh(const FChunkRenderData& RenderData)
{
	check(IsInGameThread());

	if (!IsInitialized())
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("FVoxelCustomVFRenderer::UpdateChunkMesh called before initialization"));
		return;
	}

	if (!RenderData.HasValidGeometry())
	{
		// Empty mesh - remove if exists
		RemoveChunk(RenderData.ChunkCoord);
		return;
	}

	// Check if GPU buffers are provided
	if (!RenderData.HasGPUBuffers())
	{
		UE_LOG(LogVoxelRendering, Warning,
			TEXT("FVoxelCustomVFRenderer::UpdateChunkMesh: Chunk %s has no GPU buffers. Use UpdateChunkMeshFromCPU for CPU data."),
			*RenderData.ChunkCoord.ToString());
		return;
	}

	const FIntVector& ChunkCoord = RenderData.ChunkCoord;

	// Update statistics
	FChunkStats* ExistingStats = ChunkStatsMap.Find(ChunkCoord);
	if (ExistingStats)
	{
		// Subtract old stats
		TotalVertexCount -= ExistingStats->VertexCount;
		TotalTriangleCount -= ExistingStats->TriangleCount;
		TotalGPUMemory -= ExistingStats->MemoryUsage;
	}

	// Add new stats
	FChunkStats& Stats = ChunkStatsMap.FindOrAdd(ChunkCoord);
	Stats.VertexCount = RenderData.VertexCount;
	Stats.TriangleCount = RenderData.IndexCount / 3;
	Stats.LODLevel = RenderData.LODLevel;
	Stats.MemoryUsage = RenderData.GetGPUMemoryUsage();
	Stats.Bounds = RenderData.Bounds;
	Stats.bIsVisible = true;

	TotalVertexCount += Stats.VertexCount;
	TotalTriangleCount += Stats.TriangleCount;
	TotalGPUMemory += Stats.MemoryUsage;

	// Forward to the chunk's region component
	if (UVoxelWorldComponent* Component = GetOrCreateRegionComponent(ChunkCoord))
	{
		Component->UpdateChunkBuffers(RenderData);
	}

	UE_LOG(LogVoxelRendering, Verbose, TEXT("FVoxelCustomVFRenderer: Updated chunk %s (GPU path) - %d verts, %d tris"),
		*ChunkCoord.ToString(), Stats.VertexCount, Stats.TriangleCount);
}

void FVoxelCustomVFRenderer::UpdateChunkMeshFromCPU(
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

void FVoxelCustomVFRenderer::UpdateChunkMeshFromCPU(
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

void FVoxelCustomVFRenderer::SubmitChunkMeshInternal(
	const FIntVector& ChunkCoord,
	int32 LODLevel,
	const FChunkMeshData& MeshData,
	TArray<uint32>&& Indices)
{
	check(IsInGameThread());

	if (!IsInitialized())
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("FVoxelCustomVFRenderer::UpdateChunkMeshFromCPU called before initialization"));
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
	Stats.bIsVisible = true;

	TotalVertexCount += Stats.VertexCount;
	TotalTriangleCount += Stats.TriangleCount;
	TotalGPUMemory += Stats.MemoryUsage;

	// Use DIRECT CPU PATH - no GPU buffer roundtrip!
	// This passes CPU arrays directly to the render thread, avoiding:
	// 1. Creating intermediate GPU buffers
	// 2. Bouncing back to game thread
	// 3. GPU readback stalls in scene proxy
	if (UVoxelWorldComponent* Component = GetOrCreateRegionComponent(ChunkCoord))
	{
		Component->UpdateChunkBuffersFromCPUData(
			ChunkCoord,
			MoveTemp(Vertices),
			MoveTemp(Indices),
			LODLevel,
			LocalBounds
		);
	}

	UE_LOG(LogVoxelRendering, Verbose, TEXT("FVoxelCustomVFRenderer: Updated chunk %s (DIRECT CPU path) - %d verts, %d tris"),
		*ChunkCoord.ToString(), Stats.VertexCount, Stats.TriangleCount);
}

void FVoxelCustomVFRenderer::RemoveChunk(const FIntVector& ChunkCoord)
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
	if (UVoxelWorldComponent* Component = FindRegionComponent(ChunkCoord))
	{
		Component->RemoveChunk(ChunkCoord);
	}
}

void FVoxelCustomVFRenderer::ClearAllChunks()
{
	check(IsInGameThread());

	ChunkStatsMap.Empty();
	TotalVertexCount = 0;
	TotalTriangleCount = 0;
	TotalGPUMemory = 0;

	for (const TPair<FIntVector, TObjectPtr<UVoxelWorldComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->ClearAllChunks();
		}
	}
}

// ==================== Visibility ====================

void FVoxelCustomVFRenderer::SetChunkVisible(const FIntVector& ChunkCoord, bool bVisible)
{
	check(IsInGameThread());

	FChunkStats* Stats = ChunkStatsMap.Find(ChunkCoord);
	if (Stats)
	{
		Stats->bIsVisible = bVisible;
	}

	if (UVoxelWorldComponent* Component = FindRegionComponent(ChunkCoord))
	{
		Component->SetChunkVisible(ChunkCoord, bVisible);
	}
}

void FVoxelCustomVFRenderer::SetAllChunksVisible(bool bVisible)
{
	check(IsInGameThread());

	for (auto& Pair : ChunkStatsMap)
	{
		Pair.Value.bIsVisible = bVisible;
	}

	// Set visibility for all chunks via their region components
	TArray<FIntVector> Chunks;
	GetLoadedChunks(Chunks);
	for (const FIntVector& ChunkCoord : Chunks)
	{
		if (UVoxelWorldComponent* Component = FindRegionComponent(ChunkCoord))
		{
			Component->SetChunkVisible(ChunkCoord, bVisible);
		}
	}
}

// ==================== Material Management ====================

void FVoxelCustomVFRenderer::SetMaterial(UMaterialInterface* Material)
{
	check(IsInGameThread());

	CurrentMaterial = Material;

	for (const TPair<FIntVector, TObjectPtr<UVoxelWorldComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->SetMaterial(0, Material);
		}
	}
}

UMaterialInterface* FVoxelCustomVFRenderer::GetMaterial() const
{
	return CurrentMaterial.Get();
}

UMaterialInterface* FVoxelCustomVFRenderer::GetSurfaceMaterial() const
{
	// The world component draws with its atlas instance once SetMaterialAtlas made one.
	UMaterialInterface* Drawn = WorldComponent ? WorldComponent->GetMaterial(0) : nullptr;
	return Drawn ? Drawn : CurrentMaterial.Get();
}

void FVoxelCustomVFRenderer::UpdateMaterialParameters()
{
	// Material parameters update automatically through UMaterialInstanceDynamic
	// Force a render state update to pick up any changes
	for (const TPair<FIntVector, TObjectPtr<UVoxelWorldComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->MarkRenderStateDirty();
		}
	}
}

void FVoxelCustomVFRenderer::SetMaterialAtlas(UVoxelMaterialAtlas* Atlas)
{
	check(IsInGameThread());

	UE_LOG(LogVoxelRendering, Log, TEXT("SetMaterialAtlas called - WorldComponent: %s, CurrentMaterial: %s, Atlas: %s"),
		WorldComponent ? TEXT("Valid") : TEXT("NULL"),
		CurrentMaterial.IsValid() ? *CurrentMaterial->GetName() : TEXT("NULL"),
		Atlas ? *Atlas->GetName() : TEXT("NULL"));

	CurrentAtlas = Atlas;
	for (const TPair<FIntVector, TObjectPtr<UVoxelWorldComponent>>& Pair : RegionComponents)
	{
		UVoxelWorldComponent* Component = Pair.Value;
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

UVoxelMaterialAtlas* FVoxelCustomVFRenderer::GetMaterialAtlas() const
{
	if (WorldComponent)
	{
		return WorldComponent->GetMaterialAtlas();
	}
	return nullptr;
}

// ==================== Water Tile Support ====================

void FVoxelCustomVFRenderer::SetWaterMaterial(UMaterialInterface* Material)
{
	check(IsInGameThread());

	CurrentWaterMaterial = Material;

	for (const TPair<FIntVector, TObjectPtr<UVoxelWorldComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->SetWaterMaterial(Material);
		}
	}
}

void FVoxelCustomVFRenderer::UpdateWaterTileMesh(
	const FIntVector2& TileCoord,
	const FChunkMeshData& WaterMeshData)
{
	check(IsInGameThread());

	if (!IsInitialized())
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("FVoxelCustomVFRenderer::UpdateWaterTileMesh called before initialization"));
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
	if (UVoxelWorldComponent* Component = GetOrCreateRegionComponent(WaterTileChunk(TileCoord)))
	{
		Component->UpdateWaterTileFromCPUData(TileCoord, MoveTemp(Vertices), MoveTemp(Indices), TileWorldPos);
	}

	UE_LOG(LogVoxelRendering, Verbose, TEXT("FVoxelCustomVFRenderer: Updated water tile (%d,%d) - %d verts"),
		TileCoord.X, TileCoord.Y, WaterMeshData.Positions.Num());
}

void FVoxelCustomVFRenderer::RemoveWaterTile(const FIntVector2& TileCoord)
{
	check(IsInGameThread());

	if (UVoxelWorldComponent* Component = FindRegionComponent(WaterTileChunk(TileCoord)))
	{
		Component->RemoveWaterTile(TileCoord);
	}
}

void FVoxelCustomVFRenderer::ClearAllWaterTiles()
{
	check(IsInGameThread());

	for (const TPair<FIntVector, TObjectPtr<UVoxelWorldComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->ClearAllWaterTiles();
		}
	}
}

// ==================== Seam Meshes (seam-ownership P1) ====================

void FVoxelCustomVFRenderer::UpdateSeamMeshFromCPU(
	const FIntVector& OwnerChunkCoord,
	uint8 Axis,
	int32 LODLevel,
	FChunkMeshData&& MeshData,
	bool bCrossfade)
{
	check(IsInGameThread());

	if (!IsInitialized())
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("FVoxelCustomVFRenderer::UpdateSeamMeshFromCPU called before initialization"));
		return;
	}

	if (!MeshData.IsValid())
	{
		// Seam dissolved (no surface crosses the face anymore) — drop the bucket (fading it out
		// with the surface it belonged to when the caller asked for a crossfade).
		if (UVoxelWorldComponent* Component = FindRegionComponent(OwnerChunkCoord))
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
	if (UVoxelWorldComponent* Component = GetOrCreateRegionComponent(OwnerChunkCoord))
	{
		Component->UpdateSeamMeshFromCPUData(
			OwnerChunkCoord, Axis, LODLevel,
			MoveTemp(Vertices), MoveTemp(MeshData.Indices), OwnerWorldPos, bCrossfade);
	}

	UE_LOG(LogVoxelRendering, Verbose, TEXT("FVoxelCustomVFRenderer: Updated seam mesh %s axis=%d - %d verts"),
		*OwnerChunkCoord.ToString(), Axis, MeshData.Positions.Num());
}

void FVoxelCustomVFRenderer::RemoveSeamMesh(const FIntVector& OwnerChunkCoord, uint8 Axis)
{
	check(IsInGameThread());

	if (UVoxelWorldComponent* Component = FindRegionComponent(OwnerChunkCoord))
	{
		Component->RemoveSeamMesh(OwnerChunkCoord, Axis);
	}
}

void FVoxelCustomVFRenderer::SetChunkFadeHold(const FIntVector& ChunkCoord, bool bHold)
{
	check(IsInGameThread());

	// A hold is set before the chunk's mesh arrives: the region's component may not exist yet.
	if (UVoxelWorldComponent* Component = bHold ? GetOrCreateRegionComponent(ChunkCoord) : FindRegionComponent(ChunkCoord))
	{
		Component->SetChunkFadeHold(ChunkCoord, bHold);
	}
}

void FVoxelCustomVFRenderer::SetSeamFadeHold(const FIntVector& OwnerChunkCoord, bool bHold)
{
	check(IsInGameThread());

	if (UVoxelWorldComponent* Component = bHold ? GetOrCreateRegionComponent(OwnerChunkCoord) : FindRegionComponent(OwnerChunkCoord))
	{
		Component->SetSeamFadeHold(OwnerChunkCoord, bHold);
	}
}

float FVoxelCustomVFRenderer::GetSeamFadeRemainingSeconds(const FIntVector& OwnerChunkCoord) const
{
	check(IsInGameThread());

	const UVoxelWorldComponent* Component = FindRegionComponent(OwnerChunkCoord);
	return Component ? Component->GetSeamFadeRemainingSeconds(OwnerChunkCoord) : 0.0f;
}

void FVoxelCustomVFRenderer::SetViewerPosition(const FVector& ViewerPosition)
{
	check(IsInGameThread());

	CurrentViewerPosition = ViewerPosition;
	bHasViewerPosition = true;
	for (const TPair<FIntVector, TObjectPtr<UVoxelWorldComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->SetFadeViewerPosition(ViewerPosition);
		}
	}
}

bool FVoxelCustomVFRenderer::WillFadeInChunk(const FIntVector& ChunkCoord) const
{
	check(IsInGameThread());

	// The answer belongs to the component the chunk's mesh will go to (its fade scheduler and cap). A region with no
	// component yet gets one now: the mesh follows this question at once.
	UVoxelWorldComponent* Component = const_cast<FVoxelCustomVFRenderer*>(this)->GetOrCreateRegionComponent(ChunkCoord);
	return Component && Component->WillFadeInChunk(ChunkCoord);
}

void FVoxelCustomVFRenderer::ClearAllSeamMeshes()
{
	check(IsInGameThread());

	for (const TPair<FIntVector, TObjectPtr<UVoxelWorldComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->ClearAllSeamMeshes();
		}
	}
}

void FVoxelCustomVFRenderer::GetSeamMeshOwners(TArray<FIntVector>& OutOwners) const
{
	OutOwners.Reset();
	TArray<FIntVector> RegionOwners;
	for (const TPair<FIntVector, TObjectPtr<UVoxelWorldComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->GetSeamMeshOwners(RegionOwners);
			OutOwners.Append(RegionOwners);
		}
	}
}

bool FVoxelCustomVFRenderer::ConsumeLostRenderData(TArray<FIntVector>& OutChunks)
{
	OutChunks.Reset();
	bool bLost = false;
	TArray<FIntVector> RegionChunks;
	for (const TPair<FIntVector, TObjectPtr<UVoxelWorldComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value && Pair.Value->ConsumeLostRenderData(RegionChunks))
		{
			OutChunks.Append(RegionChunks);
			bLost = true;
		}
	}
	return bLost;
}

bool FVoxelCustomVFRenderer::GetWaterTileDrawInfo(const FIntVector2& TileCoord, FString& Out) const
{
	check(IsInGameThread());
	const FIntVector Chunk = WaterTileChunk(TileCoord);
	const FIntVector Region = Regions.GetRegion(Chunk);
	UVoxelWorldComponent* Component = FindRegionComponent(Chunk);
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

// ==================== LOD Transitions ====================

void FVoxelCustomVFRenderer::UpdateLODTransition(const FIntVector& ChunkCoord, float MorphFactor)
{
	check(IsInGameThread());

	if (UVoxelWorldComponent* Component = FindRegionComponent(ChunkCoord))
	{
		Component->UpdateChunkMorphFactor(ChunkCoord, MorphFactor);
	}
}

void FVoxelCustomVFRenderer::UpdateLODTransitionsBatch(const TArray<TPair<FIntVector, float>>& Transitions)
{
	check(IsInGameThread());

	if (!WorldComponent || Transitions.Num() == 0)
	{
		return;
	}

	// DISABLED: Morph factor updates send too many render commands and cause overflow
	// The morph factor infrastructure exists but is not visually used yet (shader doesn't apply it)
	// Re-enable when vertex morphing is implemented in the shader
	//
	// For now, just skip morph factor updates entirely to prevent overflow
	return;
}

void FVoxelCustomVFRenderer::FlushPendingOperations()
{
	check(IsInGameThread());

	// Delegate to the world components, each batching its pending adds/removes into a single render command
	for (const TPair<FIntVector, TObjectPtr<UVoxelWorldComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->FlushPendingOperations();
		}
	}

	// Then retire the regions left with nothing to draw (their removals are queued ahead of the proxy's destruction)
	RetireEmptyRegions();
}

// ==================== LOD Configuration ====================

void FVoxelCustomVFRenderer::SetLODParameterCollection(UMaterialParameterCollection* Collection)
{
	check(IsInGameThread());

	CurrentLODCollection = Collection;
	for (const TPair<FIntVector, TObjectPtr<UVoxelWorldComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->SetLODParameterCollection(Collection);
		}
	}
}

void FVoxelCustomVFRenderer::SetLODTransitionDistances(float StartDistance, float EndDistance)
{
	check(IsInGameThread());

	bHasLODDistances = true;
	LODStartDistance = StartDistance;
	LODEndDistance = EndDistance;
	for (const TPair<FIntVector, TObjectPtr<UVoxelWorldComponent>>& Pair : RegionComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->SetLODTransitionDistances(StartDistance, EndDistance);
		}
	}
}

// ==================== Queries ====================

bool FVoxelCustomVFRenderer::IsChunkLoaded(const FIntVector& ChunkCoord) const
{
	return ChunkStatsMap.Contains(ChunkCoord);
}

int32 FVoxelCustomVFRenderer::GetLoadedChunkCount() const
{
	return ChunkStatsMap.Num();
}

void FVoxelCustomVFRenderer::GetLoadedChunks(TArray<FIntVector>& OutChunks) const
{
	OutChunks.Reset(ChunkStatsMap.Num());
	for (const auto& Pair : ChunkStatsMap)
	{
		OutChunks.Add(Pair.Key);
	}
}

int64 FVoxelCustomVFRenderer::GetGPUMemoryUsage() const
{
	return TotalGPUMemory;
}

int64 FVoxelCustomVFRenderer::GetTotalVertexCount() const
{
	return TotalVertexCount;
}

int64 FVoxelCustomVFRenderer::GetTotalTriangleCount() const
{
	return TotalTriangleCount;
}

// ==================== Bounds ====================

bool FVoxelCustomVFRenderer::GetChunkBounds(const FIntVector& ChunkCoord, FBox& OutBounds) const
{
	const FChunkStats* Stats = ChunkStatsMap.Find(ChunkCoord);
	if (Stats)
	{
		OutBounds = Stats->Bounds;
		return true;
	}
	return false;
}

FBox FVoxelCustomVFRenderer::GetTotalBounds() const
{
	FBox TotalBounds(ForceInit);

	for (const auto& Pair : ChunkStatsMap)
	{
		if (Pair.Value.bIsVisible && Pair.Value.Bounds.IsValid)
		{
			TotalBounds += Pair.Value.Bounds;
		}
	}

	return TotalBounds;
}

// ==================== Debugging ====================

FString FVoxelCustomVFRenderer::GetDebugStats() const
{
	int64 InteriorStarts = 0, InteriorRestarts = 0, SeamStarts = 0, SeamRestarts = 0;
	int64 InteriorAppears = 0, SeamAppears = 0;
	int32 ActiveFades = 0;
	for (const TPair<FIntVector, TObjectPtr<UVoxelWorldComponent>>& Pair : RegionComponents)
	{
		const UVoxelWorldComponent* Component = Pair.Value;
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

void FVoxelCustomVFRenderer::DrawDebugVisualization(const FLODQueryContext& Context) const
{
	// Optional debug visualization - draw chunk bounds, LOD levels, etc.
	// Implementation can use DrawDebugBox, DrawDebugString, etc.
}

FString FVoxelCustomVFRenderer::GetRendererTypeName() const
{
	return TEXT("CustomVF");
}

// ==================== Internal Helpers ====================

void FVoxelCustomVFRenderer::ConvertToVoxelVertices(
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

FBox FVoxelCustomVFRenderer::CalculateChunkBounds(const FIntVector& ChunkCoord) const
{
	const FVector WorldOrigin = CachedConfig.IsValid() ? CachedConfig->WorldOrigin : FVector::ZeroVector;
	const FVector ChunkMin = WorldOrigin + FVector(ChunkCoord) * ChunkWorldSize;
	const FVector ChunkMax = ChunkMin + FVector(ChunkWorldSize);
	return FBox(ChunkMin, ChunkMax);
}
