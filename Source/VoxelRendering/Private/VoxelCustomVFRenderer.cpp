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

	// Spawn container actor
	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = TEXT("VoxelCustomVFContainer");
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

	// Create world component
	WorldComponent = NewObject<UVoxelWorldComponent>(Actor, NAME_None, RF_Transient);
	if (!WorldComponent)
	{
		UE_LOG(LogVoxelRendering, Error, TEXT("FVoxelCustomVFRenderer: Failed to create world component"));
		Actor->Destroy();
		ContainerActor.Reset();
		return;
	}

	// Configure component
	WorldComponent->SetVoxelSize(VoxelSize);
	WorldComponent->SetChunkWorldSize(ChunkWorldSize);

	// Sync material mode with configuration's meshing mode
	const bool bIsSmooth = (WorldConfig->MeshingMode == EMeshingMode::Smooth);
	WorldComponent->SetUseSmoothMeshing(bIsSmooth);
	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelCustomVFRenderer: MeshingMode=%s, bUseSmoothMeshing=%s"),
		bIsSmooth ? TEXT("Smooth") : TEXT("Cubic"),
		bIsSmooth ? TEXT("true") : TEXT("false"));

	// Set initial material BEFORE registration - scene proxy is created during RegisterComponent
	if (CurrentMaterial.IsValid())
	{
		WorldComponent->SetMaterial(0, CurrentMaterial.Get());
	}

	// Attach and register (this creates the scene proxy with the material set above)
	WorldComponent->SetupAttachment(Actor->GetRootComponent());
	WorldComponent->RegisterComponent();

	bIsInitialized = true;
	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelCustomVFRenderer initialized"));
}

void FVoxelCustomVFRenderer::Shutdown()
{
	check(IsInGameThread());

	if (!bIsInitialized)
	{
		return;
	}

	// Clear all chunks first
	ClearAllChunks();

	// Destroy component
	if (WorldComponent)
	{
		WorldComponent->DestroyComponent();
		WorldComponent = nullptr;
	}

	// Destroy container actor
	if (ContainerActor.IsValid())
	{
		ContainerActor->Destroy();
		ContainerActor.Reset();
	}

	CachedWorld.Reset();
	CachedConfig.Reset();
	CurrentMaterial.Reset();

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

	// Forward to world component
	WorldComponent->UpdateChunkBuffers(RenderData);

	UE_LOG(LogVoxelRendering, Verbose, TEXT("FVoxelCustomVFRenderer: Updated chunk %s (GPU path) - %d verts, %d tris"),
		*ChunkCoord.ToString(), Stats.VertexCount, Stats.TriangleCount);
}

void FVoxelCustomVFRenderer::UpdateChunkMeshFromCPU(
	const FIntVector& ChunkCoord,
	int32 LODLevel,
	const FChunkMeshData& MeshData)
{
	check(IsInGameThread());

	if (!IsInitialized())
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("FVoxelCustomVFRenderer::UpdateChunkMeshFromCPU called before initialization"));
		return;
	}

	if (!MeshData.IsValid())
	{
		RemoveChunk(ChunkCoord);
		return;
	}

	// Convert CPU mesh data to FVoxelVertex array
	TArray<FVoxelVertex> Vertices;
	ConvertToVoxelVertices(MeshData, Vertices);

	// Copy indices
	TArray<uint32> Indices = MeshData.Indices;

	// Calculate bounds
	FBox LocalBounds(ForceInit);
	for (const FVector3f& Pos : MeshData.Positions)
	{
		LocalBounds += FVector(Pos);
	}

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
	WorldComponent->UpdateChunkBuffersFromCPUData(
		ChunkCoord,
		MoveTemp(Vertices),
		MoveTemp(Indices),
		LODLevel,
		LocalBounds
	);

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

	// Forward to component
	if (WorldComponent)
	{
		WorldComponent->RemoveChunk(ChunkCoord);
	}
}

void FVoxelCustomVFRenderer::ClearAllChunks()
{
	check(IsInGameThread());

	ChunkStatsMap.Empty();
	TotalVertexCount = 0;
	TotalTriangleCount = 0;
	TotalGPUMemory = 0;

	if (WorldComponent)
	{
		WorldComponent->ClearAllChunks();
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

	if (WorldComponent)
	{
		WorldComponent->SetChunkVisible(ChunkCoord, bVisible);
	}
}

void FVoxelCustomVFRenderer::SetAllChunksVisible(bool bVisible)
{
	check(IsInGameThread());

	for (auto& Pair : ChunkStatsMap)
	{
		Pair.Value.bIsVisible = bVisible;
	}

	// Set visibility for all chunks via component
	if (WorldComponent)
	{
		TArray<FIntVector> Chunks;
		GetLoadedChunks(Chunks);

		for (const FIntVector& ChunkCoord : Chunks)
		{
			WorldComponent->SetChunkVisible(ChunkCoord, bVisible);
		}
	}
}

// ==================== Material Management ====================

void FVoxelCustomVFRenderer::SetMaterial(UMaterialInterface* Material)
{
	check(IsInGameThread());

	CurrentMaterial = Material;

	if (WorldComponent)
	{
		WorldComponent->SetMaterial(0, Material);
	}
}

UMaterialInterface* FVoxelCustomVFRenderer::GetMaterial() const
{
	return CurrentMaterial.Get();
}

void FVoxelCustomVFRenderer::UpdateMaterialParameters()
{
	// Material parameters update automatically through UMaterialInstanceDynamic
	// Force a render state update to pick up any changes
	if (WorldComponent)
	{
		WorldComponent->MarkRenderStateDirty();
	}
}

void FVoxelCustomVFRenderer::SetMaterialAtlas(UVoxelMaterialAtlas* Atlas)
{
	check(IsInGameThread());

	UE_LOG(LogVoxelRendering, Log, TEXT("SetMaterialAtlas called - WorldComponent: %s, CurrentMaterial: %s, Atlas: %s"),
		WorldComponent ? TEXT("Valid") : TEXT("NULL"),
		CurrentMaterial.IsValid() ? *CurrentMaterial->GetName() : TEXT("NULL"),
		Atlas ? *Atlas->GetName() : TEXT("NULL"));

	if (WorldComponent)
	{
		// Set the atlas FIRST - CreateVoxelMaterialInstance calls UpdateMaterialAtlasParameters
		// which needs the atlas to be set
		WorldComponent->SetMaterialAtlas(Atlas);

		UMaterialInterface* ComponentMaterial = WorldComponent->GetMaterial(0);
		UE_LOG(LogVoxelRendering, Log, TEXT("  ComponentMaterial: %s, CurrentMaterial.Get(): %s, Match: %s"),
			ComponentMaterial ? *ComponentMaterial->GetName() : TEXT("NULL"),
			CurrentMaterial.IsValid() ? *CurrentMaterial.Get()->GetName() : TEXT("NULL"),
			(ComponentMaterial == CurrentMaterial.Get()) ? TEXT("YES") : TEXT("NO"));

		// Create a dynamic material instance if we have a material but no dynamic instance yet
		// This is required for the LUT texture to be passed to the material
		if (CurrentMaterial.IsValid() && WorldComponent->GetMaterial(0) == CurrentMaterial.Get())
		{
			UE_LOG(LogVoxelRendering, Log, TEXT("  Creating dynamic material instance..."));
			// Create dynamic instance from the current material
			WorldComponent->CreateVoxelMaterialInstance(CurrentMaterial.Get());
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

// ==================== LOD Transitions ====================

void FVoxelCustomVFRenderer::UpdateLODTransition(const FIntVector& ChunkCoord, float MorphFactor)
{
	check(IsInGameThread());

	if (WorldComponent)
	{
		WorldComponent->UpdateChunkMorphFactor(ChunkCoord, MorphFactor);
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

	if (!WorldComponent)
	{
		return;
	}

	// Delegate to the world component which batches all pending adds/removes
	// into a single render command
	WorldComponent->FlushPendingOperations();
}

// ==================== LOD Configuration ====================

void FVoxelCustomVFRenderer::SetLODParameterCollection(UMaterialParameterCollection* Collection)
{
	check(IsInGameThread());

	if (WorldComponent)
	{
		WorldComponent->SetLODParameterCollection(Collection);
	}
}

void FVoxelCustomVFRenderer::SetLODTransitionDistances(float StartDistance, float EndDistance)
{
	check(IsInGameThread());

	if (WorldComponent)
	{
		WorldComponent->SetLODTransitionDistances(StartDistance, EndDistance);
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
	return FString::Printf(
		TEXT("Custom VF Renderer Stats:\n")
		TEXT("  Chunks: %d\n")
		TEXT("  Vertices: %lld\n")
		TEXT("  Triangles: %lld\n")
		TEXT("  GPU Memory: %.2f MB\n")
		TEXT("  Voxel Size: %.1f\n")
		TEXT("  Chunk Size: %.1f"),
		ChunkStatsMap.Num(),
		TotalVertexCount,
		TotalTriangleCount,
		TotalGPUMemory / (1024.0 * 1024.0),
		VoxelSize,
		ChunkWorldSize
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
	TArray<FVoxelVertex>& OutVertices)
{
	const int32 VertexCount = MeshData.Positions.Num();
	OutVertices.SetNumUninitialized(VertexCount);

	for (int32 i = 0; i < VertexCount; ++i)
	{
		FVoxelVertex& Vertex = OutVertices[i];

		// Position
		Vertex.Position = MeshData.Positions[i];

		// Normal
		if (i < MeshData.Normals.Num())
		{
			Vertex.SetNormal(MeshData.Normals[i]);
		}
		else
		{
			Vertex.SetNormal(FVector3f::UpVector);
		}

		// UV
		if (i < MeshData.UVs.Num())
		{
			Vertex.UV = MeshData.UVs[i];
		}
		else
		{
			Vertex.UV = FVector2f::ZeroVector;
		}

		// Extract material data from vertex color
		if (i < MeshData.Colors.Num())
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
}

FBox FVoxelCustomVFRenderer::CalculateChunkBounds(const FIntVector& ChunkCoord) const
{
	const FVector ChunkMin = FVector(ChunkCoord) * ChunkWorldSize;
	const FVector ChunkMax = ChunkMin + FVector(ChunkWorldSize);
	return FBox(ChunkMin, ChunkMax);
}
