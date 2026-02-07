// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelScatterManager.h"
#include "VoxelScatterRenderer.h"
#include "VoxelScatterConfiguration.h"
#include "VoxelSurfaceExtractor.h"
#include "VoxelScatterPlacement.h"
#include "VoxelWorldConfiguration.h"
#include "ChunkRenderData.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelBiomeRegistry.h"
#include "VoxelScatter.h"
#include "DrawDebugHelpers.h"
#include "Algo/BinarySearch.h"

UVoxelScatterManager::UVoxelScatterManager()
{
}

// ==================== Lifecycle ====================

void UVoxelScatterManager::Initialize(UVoxelWorldConfiguration* Config, UWorld* World)
{
	if (bIsInitialized)
	{
		UE_LOG(LogVoxelScatter, Warning, TEXT("VoxelScatterManager::Initialize called when already initialized"));
		Shutdown();
	}

	if (!Config)
	{
		UE_LOG(LogVoxelScatter, Error, TEXT("VoxelScatterManager::Initialize called with null configuration"));
		return;
	}

	if (!World)
	{
		UE_LOG(LogVoxelScatter, Error, TEXT("VoxelScatterManager::Initialize called with null world"));
		return;
	}

	Configuration = Config;
	CachedWorld = World;

	// Apply configuration
	ScatterRadius = Config->ScatterRadius;
	bDebugVisualization = Config->bScatterDebugVisualization;
	WorldSeed = Config->WorldSeed;

	// Clear any existing data
	SurfaceDataCache.Empty();
	ScatterDataCache.Empty();
	ScatterDefinitions.Empty();

	// Load scatter definitions from configuration asset if available
	bool bLoadedFromConfig = false;
	if (Config->ScatterConfiguration)
	{
		UVoxelScatterConfiguration* ScatterConfig = Config->ScatterConfiguration;
		if (ScatterConfig)
		{
			// Copy definitions from configuration
			for (const FScatterDefinition& Def : ScatterConfig->ScatterDefinitions)
			{
				ScatterDefinitions.Add(Def);
			}

			// Apply surface point spacing from config
			if (ScatterConfig->SurfacePointSpacing > 0.0f)
			{
				SurfacePointSpacing = ScatterConfig->SurfacePointSpacing;
			}

			bLoadedFromConfig = ScatterDefinitions.Num() > 0;

			// Use defaults if config is empty and flag is set
			if (!bLoadedFromConfig && ScatterConfig->bUseDefaultsIfEmpty)
			{
				UE_LOG(LogVoxelScatter, Log, TEXT("ScatterConfiguration is empty, using defaults"));
			}
			else if (bLoadedFromConfig)
			{
				UE_LOG(LogVoxelScatter, Log, TEXT("Loaded %d scatter definitions from configuration asset: %s"),
					ScatterDefinitions.Num(), *ScatterConfig->GetPathName());
			}
		}
	}
	else
	{
		UE_LOG(LogVoxelScatter, Warning, TEXT("No ScatterConfiguration asset assigned in VoxelWorldConfiguration"));
	}

	// Create default scatter definitions if none loaded
	if (!bLoadedFromConfig)
	{
		CreateDefaultDefinitions();
	}

	// Reset statistics
	TotalChunksProcessed = 0;
	TotalSurfacePointsExtracted = 0;
	TotalSpawnPointsGenerated = 0;

	// Create scatter renderer for HISM instance management
	ScatterRenderer = NewObject<UVoxelScatterRenderer>(this);
	ScatterRenderer->Initialize(this, World);

	bIsInitialized = true;

	UE_LOG(LogVoxelScatter, Log, TEXT("VoxelScatterManager initialized (Radius=%.0f, PointSpacing=%.0f, Definitions=%d)"),
		ScatterRadius, SurfacePointSpacing, ScatterDefinitions.Num());
}

void UVoxelScatterManager::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	// Shutdown scatter renderer first
	if (ScatterRenderer)
	{
		ScatterRenderer->Shutdown();
		ScatterRenderer = nullptr;
	}

	// Clear pending queue
	PendingGenerationQueue.Empty();
	PendingQueueSet.Empty();

	// Clear all cached data
	SurfaceDataCache.Empty();
	ScatterDataCache.Empty();
	ScatterDefinitions.Empty();
	ClearedVolumesPerChunk.Empty();

	Configuration = nullptr;
	CachedWorld = nullptr;
	bIsInitialized = false;

	UE_LOG(LogVoxelScatter, Log, TEXT("VoxelScatterManager shutdown. Stats: Chunks=%lld, SurfacePoints=%lld, SpawnPoints=%lld"),
		TotalChunksProcessed, TotalSurfacePointsExtracted, TotalSpawnPointsGenerated);
}

// ==================== Per-Frame Update ====================

void UVoxelScatterManager::Update(const FVector& ViewerPosition, float DeltaTime)
{
	if (!bIsInitialized)
	{
		return;
	}

	LastViewerPosition = ViewerPosition;

	// Process pending scatter generations (throttled)
	ProcessPendingGenerationQueue();

	// Tick the scatter renderer to flush pending HISM rebuilds
	// Rebuilds are deferred while viewer is moving to prevent flicker
	if (ScatterRenderer && ScatterRenderer->IsInitialized())
	{
		ScatterRenderer->Tick(ViewerPosition, DeltaTime);
	}

	// Debug visualization is drawn separately via DrawDebugVisualization()
}

// ==================== Scatter Definitions ====================

void UVoxelScatterManager::AddScatterDefinition(const FScatterDefinition& Definition)
{
	// Check for duplicate ID
	for (int32 i = 0; i < ScatterDefinitions.Num(); ++i)
	{
		if (ScatterDefinitions[i].ScatterID == Definition.ScatterID)
		{
			// Replace existing
			ScatterDefinitions[i] = Definition;
			UE_LOG(LogVoxelScatter, Log, TEXT("Replaced scatter definition: %s (ID=%d)"),
				*Definition.Name, Definition.ScatterID);
			return;
		}
	}

	// Add new
	ScatterDefinitions.Add(Definition);
	UE_LOG(LogVoxelScatter, Log, TEXT("Added scatter definition: %s (ID=%d)"),
		*Definition.Name, Definition.ScatterID);
}

bool UVoxelScatterManager::RemoveScatterDefinition(int32 ScatterID)
{
	for (int32 i = 0; i < ScatterDefinitions.Num(); ++i)
	{
		if (ScatterDefinitions[i].ScatterID == ScatterID)
		{
			UE_LOG(LogVoxelScatter, Log, TEXT("Removed scatter definition: %s (ID=%d)"),
				*ScatterDefinitions[i].Name, ScatterID);
			ScatterDefinitions.RemoveAt(i);
			return true;
		}
	}
	return false;
}

void UVoxelScatterManager::ClearScatterDefinitions()
{
	ScatterDefinitions.Empty();
	UE_LOG(LogVoxelScatter, Log, TEXT("Cleared all scatter definitions"));
}

const FScatterDefinition* UVoxelScatterManager::GetScatterDefinition(int32 ScatterID) const
{
	for (const FScatterDefinition& Def : ScatterDefinitions)
	{
		if (Def.ScatterID == ScatterID)
		{
			return &Def;
		}
	}
	return nullptr;
}

// ==================== Scatter Data Access ====================

const FChunkScatterData* UVoxelScatterManager::GetChunkScatterData(const FIntVector& ChunkCoord) const
{
	return ScatterDataCache.Find(ChunkCoord);
}

const FChunkSurfaceData* UVoxelScatterManager::GetChunkSurfaceData(const FIntVector& ChunkCoord) const
{
	return SurfaceDataCache.Find(ChunkCoord);
}

bool UVoxelScatterManager::HasScatterData(const FIntVector& ChunkCoord) const
{
	const FChunkScatterData* Data = ScatterDataCache.Find(ChunkCoord);
	return Data && Data->bIsValid;
}

// ==================== Mesh Data Callback ====================

void UVoxelScatterManager::OnChunkMeshDataReady(const FIntVector& ChunkCoord, const FChunkMeshData& MeshData)
{
	if (!bIsInitialized || !Configuration)
	{
		return;
	}

	// Validate mesh data
	if (!MeshData.IsValid())
	{
		return;
	}

	// Calculate chunk distance from viewer
	const FVector ChunkCenter = GetChunkWorldOrigin(ChunkCoord) +
		FVector(Configuration->GetChunkWorldSize() * 0.5f);
	const float ChunkDistance = FVector::Dist(ChunkCenter, LastViewerPosition);

	// Check which definitions should be generated at this distance
	TSet<int32> DefinitionsInRange;
	for (const FScatterDefinition& Def : ScatterDefinitions)
	{
		if (!Def.bEnabled)
		{
			continue;
		}

		const float EffectiveSpawnDistance = Def.SpawnDistance > 0.0f ? Def.SpawnDistance : ScatterRadius;
		if (ChunkDistance <= EffectiveSpawnDistance)
		{
			DefinitionsInRange.Add(Def.ScatterID);
		}
	}

	// If no definitions in range, skip
	if (DefinitionsInRange.Num() == 0)
	{
		return;
	}

	// Check if we already have scatter data for this chunk
	FChunkScatterData* ExistingData = ScatterDataCache.Find(ChunkCoord);
	if (ExistingData && ExistingData->bIsValid)
	{
		// Check which definitions are already generated
		TSet<int32> ExistingDefinitions;
		for (const FScatterSpawnPoint& Point : ExistingData->SpawnPoints)
		{
			ExistingDefinitions.Add(Point.ScatterTypeID);
		}

		// Find definitions that are now in range but weren't generated yet
		TSet<int32> NewDefinitions = DefinitionsInRange.Difference(ExistingDefinitions);

		if (NewDefinitions.Num() == 0)
		{
			// All in-range definitions already generated, nothing to do
			return;
		}

		// Need to add more scatter types - regenerate with current mesh data
		// (We regenerate rather than append to ensure consistent surface point sampling)
		RemoveChunkScatter(ChunkCoord);
	}

	// Skip if already in pending queue
	if (PendingQueueSet.Contains(ChunkCoord))
	{
		return;
	}

	// Create pending generation request with lightweight mesh data copy
	FPendingScatterGeneration PendingRequest;
	PendingRequest.ChunkCoord = ChunkCoord;
	PendingRequest.DistanceToViewer = ChunkDistance;

	// Copy only the mesh data needed for surface extraction
	PendingRequest.Positions = MeshData.Positions;
	PendingRequest.Normals = MeshData.Normals;
	PendingRequest.UV1s = MeshData.UV1s;
	PendingRequest.Colors = MeshData.Colors;

	// Insert sorted by distance (closer chunks first)
	int32 InsertIndex = Algo::LowerBound(PendingGenerationQueue, PendingRequest);
	PendingGenerationQueue.Insert(MoveTemp(PendingRequest), InsertIndex);
	PendingQueueSet.Add(ChunkCoord);

	UE_LOG(LogVoxelScatter, Verbose, TEXT("Queued scatter generation for chunk (%d,%d,%d) at distance %.0f (queue size: %d)"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, ChunkDistance, PendingGenerationQueue.Num());
}

void UVoxelScatterManager::OnChunkUnloaded(const FIntVector& ChunkCoord)
{
	// Remove from pending queue if present
	if (PendingQueueSet.Remove(ChunkCoord) > 0)
	{
		PendingGenerationQueue.RemoveAll([&ChunkCoord](const FPendingScatterGeneration& Request)
		{
			return Request.ChunkCoord == ChunkCoord;
		});
	}

	// Clear cleared volumes - when chunk is fully unloaded/reloaded, scatter can regenerate
	ClearedVolumesPerChunk.Remove(ChunkCoord);

	RemoveChunkScatter(ChunkCoord);
}

void UVoxelScatterManager::RegenerateChunkScatter(const FIntVector& ChunkCoord)
{
	if (!bIsInitialized)
	{
		return;
	}

	// Remove from pending queue if present
	if (PendingQueueSet.Remove(ChunkCoord) > 0)
	{
		PendingGenerationQueue.RemoveAll([&ChunkCoord](const FPendingScatterGeneration& Request)
		{
			return Request.ChunkCoord == ChunkCoord;
		});
	}

	// Clear cleared volumes so scatter can regenerate
	ClearedVolumesPerChunk.Remove(ChunkCoord);

	// Remove existing scatter data
	RemoveChunkScatter(ChunkCoord);

	// Regeneration requires new mesh data - the caller should provide it via OnChunkMeshDataReady
	UE_LOG(LogVoxelScatter, Verbose, TEXT("Regenerate scatter requested for chunk (%d,%d,%d) - awaiting new mesh data"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
}

void UVoxelScatterManager::ClearScatterInRadius(const FVector& WorldPosition, float Radius)
{
	if (!bIsInitialized || !Configuration)
	{
		return;
	}

	// Find which chunks are affected by this edit
	const float ChunkWorldSize = Configuration->GetChunkWorldSize();
	const FVector WorldOrigin = Configuration->WorldOrigin;

	// Calculate affected chunk range
	const FVector MinWorld = WorldPosition - FVector(Radius);
	const FVector MaxWorld = WorldPosition + FVector(Radius);

	const FIntVector MinChunk(
		FMath::FloorToInt((MinWorld.X - WorldOrigin.X) / ChunkWorldSize),
		FMath::FloorToInt((MinWorld.Y - WorldOrigin.Y) / ChunkWorldSize),
		FMath::FloorToInt((MinWorld.Z - WorldOrigin.Z) / ChunkWorldSize)
	);
	const FIntVector MaxChunk(
		FMath::FloorToInt((MaxWorld.X - WorldOrigin.X) / ChunkWorldSize),
		FMath::FloorToInt((MaxWorld.Y - WorldOrigin.Y) / ChunkWorldSize),
		FMath::FloorToInt((MaxWorld.Z - WorldOrigin.Z) / ChunkWorldSize)
	);

	// Track which scatter types need rebuilding
	TSet<int32> ScatterTypesToRebuild;
	int32 TotalRemoved = 0;

	// Process each potentially affected chunk
	for (int32 CX = MinChunk.X; CX <= MaxChunk.X; ++CX)
	{
		for (int32 CY = MinChunk.Y; CY <= MaxChunk.Y; ++CY)
		{
			for (int32 CZ = MinChunk.Z; CZ <= MaxChunk.Z; ++CZ)
			{
				const FIntVector ChunkCoord(CX, CY, CZ);

				// Add cleared volume to this chunk
				TArray<FClearedScatterVolume>& ClearedVolumes = ClearedVolumesPerChunk.FindOrAdd(ChunkCoord);
				ClearedVolumes.Add(FClearedScatterVolume(WorldPosition, Radius));

				// Remove spawn points that fall within the radius
				FChunkScatterData* ScatterData = ScatterDataCache.Find(ChunkCoord);
				if (ScatterData && ScatterData->bIsValid)
				{
					const float RadiusSq = Radius * Radius;

					for (int32 i = ScatterData->SpawnPoints.Num() - 1; i >= 0; --i)
					{
						const FScatterSpawnPoint& Point = ScatterData->SpawnPoints[i];
						if (FVector::DistSquared(Point.Position, WorldPosition) <= RadiusSq)
						{
							ScatterTypesToRebuild.Add(Point.ScatterTypeID);
							ScatterData->SpawnPoints.RemoveAt(i);
							++TotalRemoved;
						}
					}
				}

				// Also remove from pending queue to prevent stale data
				if (PendingQueueSet.Contains(ChunkCoord))
				{
					PendingQueueSet.Remove(ChunkCoord);
					PendingGenerationQueue.RemoveAll([&ChunkCoord](const FPendingScatterGeneration& Request)
					{
						return Request.ChunkCoord == ChunkCoord;
					});
				}
			}
		}
	}

	// Queue rebuilds for affected scatter types
	if (ScatterRenderer && ScatterRenderer->IsInitialized())
	{
		for (int32 ScatterTypeID : ScatterTypesToRebuild)
		{
			ScatterRenderer->QueueRebuild(ScatterTypeID);
		}
	}

	if (TotalRemoved > 0)
	{
		UE_LOG(LogVoxelScatter, Verbose, TEXT("Cleared %d scatter instances at (%.0f, %.0f, %.0f) radius %.0f"),
			TotalRemoved, WorldPosition.X, WorldPosition.Y, WorldPosition.Z, Radius);
	}
}

bool UVoxelScatterManager::IsPointInClearedVolume(const FIntVector& ChunkCoord, const FVector& WorldPosition) const
{
	const TArray<FClearedScatterVolume>* ClearedVolumes = ClearedVolumesPerChunk.Find(ChunkCoord);
	if (!ClearedVolumes)
	{
		return false;
	}

	for (const FClearedScatterVolume& Volume : *ClearedVolumes)
	{
		if (Volume.ContainsPoint(WorldPosition))
		{
			return true;
		}
	}

	return false;
}

// ==================== Configuration ====================

void UVoxelScatterManager::SetScatterRadius(float Radius)
{
	ScatterRadius = FMath::Max(Radius, 1000.0f);
	UE_LOG(LogVoxelScatter, Log, TEXT("Scatter radius set to %.0f"), ScatterRadius);
}

void UVoxelScatterManager::SetSurfacePointSpacing(float Spacing)
{
	SurfacePointSpacing = FMath::Max(Spacing, 10.0f);
	UE_LOG(LogVoxelScatter, Log, TEXT("Surface point spacing set to %.0f"), SurfacePointSpacing);
}

void UVoxelScatterManager::SetWorldSeed(uint32 Seed)
{
	WorldSeed = Seed;
	UE_LOG(LogVoxelScatter, Log, TEXT("World seed set to %u"), WorldSeed);
}

// ==================== Debug ====================

void UVoxelScatterManager::SetDebugVisualizationEnabled(bool bEnabled)
{
	bDebugVisualization = bEnabled;
	UE_LOG(LogVoxelScatter, Log, TEXT("Debug visualization %s"), bEnabled ? TEXT("enabled") : TEXT("disabled"));
}

void UVoxelScatterManager::DrawDebugVisualization(UWorld* World) const
{
#if ENABLE_DRAW_DEBUG
	if (!World || !bDebugVisualization)
	{
		return;
	}

	// Draw spawn points for each chunk
	for (const auto& Pair : ScatterDataCache)
	{
		const FChunkScatterData& Data = Pair.Value;
		if (!Data.bIsValid)
		{
			continue;
		}

		for (const FScatterSpawnPoint& Point : Data.SpawnPoints)
		{
			// Get color from definition
			FColor Color = FColor::White;
			if (const FScatterDefinition* Def = GetScatterDefinition(Point.ScatterTypeID))
			{
				Color = Def->DebugColor;
			}

			// Draw sphere at spawn point
			const float Radius = 10.0f;
			DrawDebugSphere(World, Point.Position, Radius, 4, Color, false, -1.0f, 0, 1.0f);

			// Draw normal direction
			DrawDebugLine(World, Point.Position, Point.Position + Point.Normal * 30.0f, FColor::Blue, false, -1.0f, 0, 0.5f);
		}
	}

	// Draw scatter radius around viewer
	DrawDebugSphere(World, LastViewerPosition, ScatterRadius, 32, FColor::Yellow, false, -1.0f, 0, 1.0f);
#endif
}

int64 UVoxelScatterManager::GetTotalMemoryUsage() const
{
	int64 Total = sizeof(UVoxelScatterManager);

	// Surface data cache
	Total += SurfaceDataCache.GetAllocatedSize();
	for (const auto& Pair : SurfaceDataCache)
	{
		Total += Pair.Value.GetAllocatedSize();
	}

	// Scatter data cache
	Total += ScatterDataCache.GetAllocatedSize();
	for (const auto& Pair : ScatterDataCache)
	{
		Total += Pair.Value.GetAllocatedSize();
	}

	// Pending generation queue
	Total += PendingGenerationQueue.GetAllocatedSize();
	for (const auto& Pending : PendingGenerationQueue)
	{
		Total += Pending.Positions.GetAllocatedSize()
			+ Pending.Normals.GetAllocatedSize()
			+ Pending.UV1s.GetAllocatedSize()
			+ Pending.Colors.GetAllocatedSize();
	}
	Total += PendingQueueSet.GetAllocatedSize();

	// Cleared volumes
	Total += ClearedVolumesPerChunk.GetAllocatedSize();
	for (const auto& Pair : ClearedVolumesPerChunk)
	{
		Total += Pair.Value.GetAllocatedSize();
	}

	// Scatter renderer
	if (ScatterRenderer)
	{
		Total += ScatterRenderer->GetTotalMemoryUsage();
	}

	return Total;
}

FScatterStatistics UVoxelScatterManager::GetStatistics() const
{
	FScatterStatistics Stats;
	Stats.ChunksWithScatter = ScatterDataCache.Num();
	Stats.TotalSurfacePoints = TotalSurfacePointsExtracted;
	Stats.TotalSpawnPoints = TotalSpawnPointsGenerated;

	// Calculate memory usage
	for (const auto& Pair : SurfaceDataCache)
	{
		Stats.SurfaceDataMemory += Pair.Value.GetAllocatedSize();
	}
	for (const auto& Pair : ScatterDataCache)
	{
		Stats.ScatterDataMemory += Pair.Value.GetAllocatedSize();
	}

	return Stats;
}

FString UVoxelScatterManager::GetDebugStats() const
{
	const FScatterStatistics Stats = GetStatistics();

	FString RendererStats = TEXT("Not initialized");
	if (ScatterRenderer && ScatterRenderer->IsInitialized())
	{
		RendererStats = ScatterRenderer->GetDebugStats();
	}

	return FString::Printf(
		TEXT("=== VoxelScatterManager ===\n")
		TEXT("Initialized: %s\n")
		TEXT("Definitions: %d\n")
		TEXT("Chunks with Scatter: %d\n")
		TEXT("Pending Queue: %d\n")
		TEXT("Total Surface Points: %lld\n")
		TEXT("Total Spawn Points: %lld\n")
		TEXT("Avg Surface/Chunk: %.1f\n")
		TEXT("Avg Spawn/Chunk: %.1f\n")
		TEXT("Surface Data Memory: %.2f KB\n")
		TEXT("Scatter Data Memory: %.2f KB\n")
		TEXT("%s\n"),
		bIsInitialized ? TEXT("Yes") : TEXT("No"),
		ScatterDefinitions.Num(),
		Stats.ChunksWithScatter,
		PendingGenerationQueue.Num(),
		Stats.TotalSurfacePoints,
		Stats.TotalSpawnPoints,
		Stats.GetAverageSurfacePointsPerChunk(),
		Stats.GetAverageSpawnPointsPerChunk(),
		Stats.SurfaceDataMemory / 1024.0f,
		Stats.ScatterDataMemory / 1024.0f,
		*RendererStats
	);
}

// ==================== Internal Methods ====================

void UVoxelScatterManager::GenerateChunkScatter(const FIntVector& ChunkCoord, const FChunkMeshData& MeshData)
{
	if (!MeshData.IsValid())
	{
		return;
	}

	const FVector ChunkWorldOrigin = GetChunkWorldOrigin(ChunkCoord);

	// Calculate chunk distance from viewer
	float ChunkDistance = 0.0f;
	if (Configuration)
	{
		const FVector ChunkCenter = ChunkWorldOrigin + FVector(Configuration->GetChunkWorldSize() * 0.5f);
		ChunkDistance = FVector::Dist(ChunkCenter, LastViewerPosition);
	}

	// Filter definitions to only those within their spawn distance
	TArray<FScatterDefinition> FilteredDefinitions;
	for (const FScatterDefinition& Def : ScatterDefinitions)
	{
		if (!Def.bEnabled)
		{
			continue;
		}

		// Use per-definition SpawnDistance if set, otherwise use global ScatterRadius
		const float EffectiveSpawnDistance = Def.SpawnDistance > 0.0f ? Def.SpawnDistance : ScatterRadius;

		if (ChunkDistance <= EffectiveSpawnDistance)
		{
			FilteredDefinitions.Add(Def);
		}
	}

	// Skip surface extraction if no definitions are in range
	if (FilteredDefinitions.Num() == 0)
	{
		return;
	}

	// Extract surface points
	FChunkSurfaceData SurfaceData;
	FVoxelSurfaceExtractor::ExtractSurfacePoints(
		MeshData,
		ChunkCoord,
		ChunkWorldOrigin,
		SurfacePointSpacing,
		0, // LOD level
		SurfaceData
	);

	if (!SurfaceData.bIsValid || SurfaceData.SurfacePoints.Num() == 0)
	{
		return;
	}

	// Cache surface data
	SurfaceDataCache.Add(ChunkCoord, MoveTemp(SurfaceData));
	TotalSurfacePointsExtracted += SurfaceDataCache[ChunkCoord].SurfacePoints.Num();

	// Generate spawn points using only definitions within range
	const uint32 ChunkSeed = FVoxelScatterPlacement::ComputeChunkSeed(ChunkCoord, WorldSeed);

	FChunkScatterData ScatterData;
	FVoxelScatterPlacement::GenerateSpawnPoints(
		SurfaceDataCache[ChunkCoord],
		FilteredDefinitions,
		ChunkSeed,
		ScatterData
	);

	const int32 SpawnCount = ScatterData.SpawnPoints.Num();

	// Cache scatter data
	ScatterDataCache.Add(ChunkCoord, MoveTemp(ScatterData));
	TotalSpawnPointsGenerated += SpawnCount;
	++TotalChunksProcessed;

	// Update HISM instances
	if (ScatterRenderer && ScatterRenderer->IsInitialized())
	{
		ScatterRenderer->UpdateChunkInstances(ChunkCoord, ScatterDataCache[ChunkCoord]);
	}

	// Broadcast event
	OnChunkScatterReady.Broadcast(ChunkCoord, SpawnCount);

	UE_LOG(LogVoxelScatter, Log, TEXT("Chunk (%d,%d,%d): Generated scatter (%d surface points, %d spawn points)"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z,
		SurfaceDataCache[ChunkCoord].SurfacePoints.Num(), SpawnCount);
}

void UVoxelScatterManager::RemoveChunkScatter(const FIntVector& ChunkCoord)
{
	// Remove HISM instances first (before clearing cache data)
	if (ScatterRenderer && ScatterRenderer->IsInitialized())
	{
		ScatterRenderer->RemoveChunkInstances(ChunkCoord);
	}

	if (SurfaceDataCache.Remove(ChunkCoord) > 0 || ScatterDataCache.Remove(ChunkCoord) > 0)
	{
		OnChunkScatterRemoved.Broadcast(ChunkCoord);

		UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): Scatter data removed"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
	}
}

void UVoxelScatterManager::ProcessPendingGenerationQueue()
{
	if (PendingGenerationQueue.Num() == 0)
	{
		return;
	}

	// Determine how many to process this frame
	int32 NumToProcess = PendingGenerationQueue.Num();
	if (MaxScatterGenerationsPerFrame > 0 && NumToProcess > MaxScatterGenerationsPerFrame)
	{
		NumToProcess = MaxScatterGenerationsPerFrame;
	}

	// Process from front of queue (closest chunks first, due to sorted insertion)
	for (int32 i = 0; i < NumToProcess; ++i)
	{
		if (PendingGenerationQueue.Num() == 0)
		{
			break;
		}

		// Take first item (closest chunk)
		FPendingScatterGeneration Request = MoveTemp(PendingGenerationQueue[0]);
		PendingGenerationQueue.RemoveAt(0);
		PendingQueueSet.Remove(Request.ChunkCoord);

		// Generate scatter from the pending data
		GenerateChunkScatterFromPending(Request);
	}

	if (NumToProcess > 0)
	{
		UE_LOG(LogVoxelScatter, Verbose, TEXT("Processed %d pending scatter generations (%d remaining)"),
			NumToProcess, PendingGenerationQueue.Num());
	}
}

void UVoxelScatterManager::GenerateChunkScatterFromPending(const FPendingScatterGeneration& PendingData)
{
	const FIntVector& ChunkCoord = PendingData.ChunkCoord;
	const FVector ChunkWorldOrigin = GetChunkWorldOrigin(ChunkCoord);

	// Recalculate chunk distance (viewer may have moved)
	float ChunkDistance = 0.0f;
	if (Configuration)
	{
		const FVector ChunkCenter = ChunkWorldOrigin + FVector(Configuration->GetChunkWorldSize() * 0.5f);
		ChunkDistance = FVector::Dist(ChunkCenter, LastViewerPosition);
	}

	// Filter definitions to only those within their spawn distance
	TArray<FScatterDefinition> FilteredDefinitions;
	for (const FScatterDefinition& Def : ScatterDefinitions)
	{
		if (!Def.bEnabled)
		{
			continue;
		}

		const float EffectiveSpawnDistance = Def.SpawnDistance > 0.0f ? Def.SpawnDistance : ScatterRadius;
		if (ChunkDistance <= EffectiveSpawnDistance)
		{
			FilteredDefinitions.Add(Def);
		}
	}

	// Skip if no definitions in range (viewer moved away while queued)
	if (FilteredDefinitions.Num() == 0)
	{
		UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): Skipped scatter - viewer moved out of range"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
		return;
	}

	// Validate mesh data
	const int32 VertexCount = PendingData.Positions.Num();
	const bool bHasNormals = PendingData.Normals.Num() == VertexCount;

	if (VertexCount == 0 || !bHasNormals)
	{
		UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): Skipped scatter - invalid mesh data"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
		return;
	}

	// Extract surface points directly from pending data
	// (Inline version of what VoxelSurfaceExtractor does)
	const bool bHasUV1 = PendingData.UV1s.Num() == VertexCount;
	const bool bHasColors = PendingData.Colors.Num() == VertexCount;

	const float CellSize = SurfacePointSpacing;
	TMap<FIntVector, int32> OccupiedCells;

	FChunkSurfaceData SurfaceData(ChunkCoord);
	SurfaceData.LODLevel = 0;
	SurfaceData.AveragePointSpacing = SurfacePointSpacing;
	SurfaceData.SurfacePoints.Reserve(VertexCount / 4);
	OccupiedCells.Reserve(VertexCount / 4);

	for (int32 VertIndex = 0; VertIndex < VertexCount; ++VertIndex)
	{
		const FVector LocalPos(PendingData.Positions[VertIndex]);
		const FVector WorldPos = ChunkWorldOrigin + LocalPos;

		// Spatial hash cell check
		const FIntVector Cell(
			FMath::FloorToInt(WorldPos.X / CellSize),
			FMath::FloorToInt(WorldPos.Y / CellSize),
			FMath::FloorToInt(WorldPos.Z / CellSize)
		);

		if (OccupiedCells.Contains(Cell))
		{
			continue;
		}

		const FVector Normal(PendingData.Normals[VertIndex]);

		// Decode UV1 data
		uint8 MaterialID = 0;
		EVoxelFaceType FaceType = EVoxelFaceType::Top;
		if (bHasUV1)
		{
			MaterialID = static_cast<uint8>(FMath::RoundToInt(PendingData.UV1s[VertIndex].X));
			const int32 FaceTypeInt = FMath::RoundToInt(PendingData.UV1s[VertIndex].Y);
			FaceType = (FaceTypeInt == 1) ? EVoxelFaceType::Side :
					   (FaceTypeInt == 2) ? EVoxelFaceType::Bottom : EVoxelFaceType::Top;
		}

		// Decode color data
		uint8 BiomeID = 0;
		uint8 AO = 0;
		if (bHasColors)
		{
			BiomeID = PendingData.Colors[VertIndex].G;
			AO = PendingData.Colors[VertIndex].B & 0x03;
		}

		// Skip points in cleared volumes (player-edited areas)
		if (IsPointInClearedVolume(ChunkCoord, WorldPos))
		{
			continue;
		}

		// Create surface point
		FVoxelSurfacePoint Point;
		Point.Position = WorldPos;
		Point.Normal = FVector(Normal).GetSafeNormal();
		Point.MaterialID = MaterialID;
		Point.BiomeID = BiomeID;
		Point.FaceType = FaceType;
		Point.AmbientOcclusion = AO;

		const int32 NewIndex = SurfaceData.SurfacePoints.Add(Point);
		OccupiedCells.Add(Cell, NewIndex);
	}

	SurfaceData.SurfaceAreaEstimate = SurfaceData.SurfacePoints.Num() * CellSize * CellSize;
	SurfaceData.bIsValid = true;

	if (SurfaceData.SurfacePoints.Num() == 0)
	{
		return;
	}

	// Cache surface data
	SurfaceDataCache.Add(ChunkCoord, MoveTemp(SurfaceData));
	TotalSurfacePointsExtracted += SurfaceDataCache[ChunkCoord].SurfacePoints.Num();

	// Generate spawn points
	const uint32 ChunkSeed = FVoxelScatterPlacement::ComputeChunkSeed(ChunkCoord, WorldSeed);

	FChunkScatterData ScatterData;
	FVoxelScatterPlacement::GenerateSpawnPoints(
		SurfaceDataCache[ChunkCoord],
		FilteredDefinitions,
		ChunkSeed,
		ScatterData
	);

	const int32 SpawnCount = ScatterData.SpawnPoints.Num();

	// Cache scatter data
	ScatterDataCache.Add(ChunkCoord, MoveTemp(ScatterData));
	TotalSpawnPointsGenerated += SpawnCount;
	++TotalChunksProcessed;

	// Update HISM instances
	if (ScatterRenderer && ScatterRenderer->IsInitialized())
	{
		ScatterRenderer->UpdateChunkInstances(ChunkCoord, ScatterDataCache[ChunkCoord]);
	}

	// Broadcast event
	OnChunkScatterReady.Broadcast(ChunkCoord, SpawnCount);

	UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): Generated scatter from queue (%d surface points, %d spawn points)"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z,
		SurfaceDataCache[ChunkCoord].SurfacePoints.Num(), SpawnCount);
}

FVector UVoxelScatterManager::GetChunkWorldOrigin(const FIntVector& ChunkCoord) const
{
	if (!Configuration)
	{
		return FVector::ZeroVector;
	}

	const float ChunkWorldSize = Configuration->GetChunkWorldSize();
	return Configuration->WorldOrigin + FVector(ChunkCoord) * ChunkWorldSize;
}

void UVoxelScatterManager::CreateDefaultDefinitions()
{
	// Grass scatter - dense on grass material
	// Short view distance, aggressive culling for performance
	FScatterDefinition GrassScatter;
	GrassScatter.ScatterID = 0;
	GrassScatter.Name = TEXT("Grass");
	GrassScatter.DebugColor = FColor::Green;
	GrassScatter.DebugSphereRadius = 8.0f;
	GrassScatter.bEnabled = true;
	GrassScatter.Density = 0.5f; // 50% of valid points
	GrassScatter.MinSlopeDegrees = 0.0f;
	GrassScatter.MaxSlopeDegrees = 30.0f;
	GrassScatter.AllowedMaterials = { EVoxelMaterial::Grass };
	GrassScatter.bTopFacesOnly = true;
	GrassScatter.ScaleRange = FVector2D(0.7f, 1.3f);
	GrassScatter.bRandomYawRotation = true;
	GrassScatter.bAlignToSurfaceNormal = true;
	GrassScatter.SurfaceOffset = 0.0f;
	GrassScatter.PositionJitter = 25.0f;
	// LOD settings - grass is small, cull aggressively
	GrassScatter.LODStartDistance = 3000.0f;   // LOD transitions start at 30m
	GrassScatter.CullDistance = 8000.0f;       // Fully culled at 80m
	GrassScatter.MinScreenSize = 0.005f;       // Cull tiny grass instances
	GrassScatter.bCastShadows = false;         // Grass doesn't cast shadows (performance)
	AddScatterDefinition(GrassScatter);

	// Rock scatter - less dense, on stone and dirt
	// Medium view distance
	FScatterDefinition RockScatter;
	RockScatter.ScatterID = 1;
	RockScatter.Name = TEXT("Rocks");
	RockScatter.DebugColor = FColor(128, 128, 128); // Gray
	RockScatter.DebugSphereRadius = 15.0f;
	RockScatter.bEnabled = true;
	RockScatter.Density = 0.05f; // 5% of valid points
	RockScatter.MinSlopeDegrees = 0.0f;
	RockScatter.MaxSlopeDegrees = 60.0f;
	RockScatter.AllowedMaterials = { EVoxelMaterial::Stone, EVoxelMaterial::Dirt };
	RockScatter.bTopFacesOnly = false; // Can appear on slopes
	RockScatter.ScaleRange = FVector2D(0.5f, 2.0f);
	RockScatter.bRandomYawRotation = true;
	RockScatter.bAlignToSurfaceNormal = false;
	RockScatter.SurfaceOffset = 0.0f;
	RockScatter.PositionJitter = 50.0f;
	// LOD settings - rocks are medium sized
	RockScatter.LODStartDistance = 8000.0f;    // LOD transitions start at 80m
	RockScatter.CullDistance = 20000.0f;       // Fully culled at 200m
	RockScatter.MinScreenSize = 0.002f;        // Cull very small rock instances
	RockScatter.bCastShadows = true;           // Rocks cast shadows (nearby only)
	AddScatterDefinition(RockScatter);

	// Tree scatter - very sparse on grass
	// Long view distance to prevent pop-in
	FScatterDefinition TreeScatter;
	TreeScatter.ScatterID = 2;
	TreeScatter.Name = TEXT("Trees");
	TreeScatter.DebugColor = FColor(34, 139, 34); // Forest green
	TreeScatter.DebugSphereRadius = 25.0f;
	TreeScatter.bEnabled = true;
	TreeScatter.Density = 0.02f; // 2% of valid points
	TreeScatter.MinSlopeDegrees = 0.0f;
	TreeScatter.MaxSlopeDegrees = 20.0f;
	TreeScatter.AllowedMaterials = { EVoxelMaterial::Grass };
	TreeScatter.bTopFacesOnly = true;
	TreeScatter.ScaleRange = FVector2D(0.8f, 1.5f);
	TreeScatter.bRandomYawRotation = true;
	TreeScatter.bAlignToSurfaceNormal = false;
	TreeScatter.SurfaceOffset = 0.0f;
	TreeScatter.PositionJitter = 100.0f;
	// LOD settings - trees are large, visible from far
	TreeScatter.LODStartDistance = 15000.0f;   // LOD transitions start at 150m
	TreeScatter.CullDistance = 50000.0f;       // Fully culled at 500m
	TreeScatter.MinScreenSize = 0.001f;        // Minimal screen size culling
	TreeScatter.bCastShadows = true;           // Trees cast shadows
	TreeScatter.SpawnDistance = 20000.0f;      // Spawn trees at distance to prevent pop-in
	AddScatterDefinition(TreeScatter);

	UE_LOG(LogVoxelScatter, Log, TEXT("Created %d default scatter definitions"), ScatterDefinitions.Num());
}
