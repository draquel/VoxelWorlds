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
#include "Async/Async.h"
#include "VoxelGPUSurfaceExtractor.h"

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

	// Async scatter configuration
	MaxAsyncScatterTasks = FMath::Clamp(Config->MaxAsyncScatterTasks, 1, 4);

	// GPU extraction: enabled only if config says so AND platform supports SM5
	bUseGPUExtraction = Config->bUseGPUScatterExtraction && FVoxelGPUSurfaceExtractor::IsGPUExtractionSupported();
	if (Config->bUseGPUScatterExtraction && !bUseGPUExtraction)
	{
		UE_LOG(LogVoxelScatter, Warning, TEXT("GPU scatter extraction requested but SM5 not supported, falling back to CPU"));
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

	// Drain completed async scatter queue
	{
		FAsyncScatterResult DiscardedResult;
		while (CompletedScatterQueue.Dequeue(DiscardedResult)) {}
	}
	AsyncScatterInProgress.Empty();

	// Drain GPU extraction queue
	{
		FGPUExtractionResult DiscardedGPUResult;
		while (CompletedGPUExtractionQueue.Dequeue(DiscardedGPUResult)) {}
	}
	GPUExtractionPendingPlacement.Empty();
	GPUExtractionPendingLODLevel.Empty();

	// Clear pending queue and deferred upgrades
	PendingGenerationQueue.Empty();
	PendingQueueSet.Empty();
	DeferredSupplementalPasses.Empty();

	// Clear all cached data
	SurfaceDataCache.Empty();
	ScatterDataCache.Empty();
	CompletedScatterTypes.Empty();
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

	// Process completed GPU extractions (dispatches placement tasks)
	if (bUseGPUExtraction)
	{
		ProcessCompletedGPUExtractions();
	}

	// Process completed async scatter results (game thread only: cache + HISM update)
	ProcessCompletedAsyncScatter();

	// Launch new async scatter tasks from pending queue (throttled)
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

void UVoxelScatterManager::OnChunkMeshDataReady(const FIntVector& ChunkCoord, int32 LODLevel, const FChunkMeshData& MeshData,
	const TArray<FVoxelData>& VoxelData, int32 ChunkSize, float VoxelSize)
{
	if (!bIsInitialized || !Configuration)
	{
		return;
	}

	// Voxel data is required for CPU extraction (always full resolution, LOD-independent)
	const bool bHasVoxelData = VoxelData.Num() == ChunkSize * ChunkSize * ChunkSize;
	if (!bHasVoxelData && !bUseGPUExtraction)
	{
		UE_LOG(LogVoxelScatter, Warning, TEXT("Chunk (%d,%d,%d): No voxel data for scatter extraction (expected %d, got %d)"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, ChunkSize * ChunkSize * ChunkSize, VoxelData.Num());
		return;
	}

	// GPU path still needs valid mesh data
	if (bUseGPUExtraction && !MeshData.IsValid())
	{
		return;
	}

	const FVector ChunkCenter = GetChunkWorldOrigin(ChunkCoord) +
		FVector(Configuration->GetChunkWorldSize() * 0.5f);
	const float ChunkDistance = FVector::Dist(ChunkCenter, LastViewerPosition);

	// Get the set of scatter types already generated for this chunk (never regenerated)
	const TSet<int32>* CompletedTypes = CompletedScatterTypes.Find(ChunkCoord);

	// Build definitions to generate:
	//  - All enabled defs within their SpawnDistance/ScatterRadius range
	//  - Exclude types already completed for this chunk
	//  - No LOD-based filtering: voxel data is always full resolution
	TArray<FScatterDefinition> DefsToGenerate;
	for (const FScatterDefinition& Def : ScatterDefinitions)
	{
		if (!Def.bEnabled)
		{
			continue;
		}

		// Skip types already generated for this chunk
		if (CompletedTypes && CompletedTypes->Contains(Def.ScatterID))
		{
			continue;
		}

		// Distance check
		const float EffectiveSpawnDistance = Def.SpawnDistance > 0.0f ? Def.SpawnDistance : ScatterRadius;
		if (ChunkDistance > EffectiveSpawnDistance)
		{
			continue;
		}

		DefsToGenerate.Add(Def);
	}

	if (DefsToGenerate.Num() == 0)
	{
		return;
	}

	// Build pending request
	auto MakePendingRequest = [&]() -> FPendingScatterGeneration
	{
		FPendingScatterGeneration Req;
		Req.ChunkCoord = ChunkCoord;
		Req.DistanceToViewer = ChunkDistance;
		Req.LODLevel = LODLevel;
		Req.CapturedDefinitions = DefsToGenerate;

		// Always store voxel data for CPU extraction path
		if (bHasVoxelData)
		{
			Req.ChunkVoxelData = VoxelData;
			Req.ChunkSize = ChunkSize;
			Req.VoxelSize = VoxelSize;
		}

		// Only store mesh data when GPU extraction is enabled
		if (bUseGPUExtraction && MeshData.IsValid())
		{
			Req.Positions = MeshData.Positions;
			Req.Normals = MeshData.Normals;
			Req.UV1s = MeshData.UV1s;
			Req.Colors = MeshData.Colors;
		}
		return Req;
	};

	// If already in pending queue: merge new definitions into pending entry
	if (PendingQueueSet.Contains(ChunkCoord))
	{
		for (FPendingScatterGeneration& Pending : PendingGenerationQueue)
		{
			if (Pending.ChunkCoord == ChunkCoord)
			{
				// Build set of already-pending type IDs
				TSet<int32> PendingTypeIDs;
				for (const FScatterDefinition& Def : Pending.CapturedDefinitions)
				{
					PendingTypeIDs.Add(Def.ScatterID);
				}

				// Add any new definitions not already pending
				for (const FScatterDefinition& Def : DefsToGenerate)
				{
					if (!PendingTypeIDs.Contains(Def.ScatterID))
					{
						Pending.CapturedDefinitions.Add(Def);
					}
				}
				break;
			}
		}
		return;
	}

	// If async scatter is in-flight: defer as supplemental pass
	if (AsyncScatterInProgress.Contains(ChunkCoord))
	{
		DeferredSupplementalPasses.Add(ChunkCoord, MakePendingRequest());
		UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): Deferred supplemental scatter (%d defs) — async in-flight"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, DefsToGenerate.Num());
		return;
	}

	// Queue new scatter generation
	FPendingScatterGeneration PendingRequest = MakePendingRequest();
	int32 InsertIndex = Algo::LowerBound(PendingGenerationQueue, PendingRequest);
	PendingGenerationQueue.Insert(MoveTemp(PendingRequest), InsertIndex);
	PendingQueueSet.Add(ChunkCoord);

	UE_LOG(LogVoxelScatter, Verbose, TEXT("Queued scatter for chunk (%d,%d,%d) %d defs, dist %.0f (queue: %d)"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, DefsToGenerate.Num(),
		ChunkDistance, PendingGenerationQueue.Num());
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

	// Remove from async in-progress tracking (stale result will be discarded on arrival)
	AsyncScatterInProgress.Remove(ChunkCoord);

	// Remove deferred supplemental passes
	DeferredSupplementalPasses.Remove(ChunkCoord);

	// Remove from GPU pending placement
	GPUExtractionPendingPlacement.Remove(ChunkCoord);
	GPUExtractionPendingLODLevel.Remove(ChunkCoord);

	// Clear cleared volumes and completed type tracking - allow full regeneration on reload
	ClearedVolumesPerChunk.Remove(ChunkCoord);
	CompletedScatterTypes.Remove(ChunkCoord);

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

	// Remove from async in-progress (stale result will be discarded on arrival)
	AsyncScatterInProgress.Remove(ChunkCoord);

	// Remove from GPU pending placement
	GPUExtractionPendingPlacement.Remove(ChunkCoord);
	GPUExtractionPendingLODLevel.Remove(ChunkCoord);

	// Remove deferred supplemental passes
	DeferredSupplementalPasses.Remove(ChunkCoord);

	// Clear cleared volumes and completed types so scatter can fully regenerate
	ClearedVolumesPerChunk.Remove(ChunkCoord);
	CompletedScatterTypes.Remove(ChunkCoord);

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
		Total += Pending.ChunkVoxelData.GetAllocatedSize()
			+ Pending.Positions.GetAllocatedSize()
			+ Pending.Normals.GetAllocatedSize()
			+ Pending.UV1s.GetAllocatedSize()
			+ Pending.Colors.GetAllocatedSize();
	}
	Total += PendingQueueSet.GetAllocatedSize();

	// Async in-progress set
	Total += AsyncScatterInProgress.GetAllocatedSize();

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
	Stats.TotalHISMInstances = (ScatterRenderer && ScatterRenderer->IsInitialized()) ? ScatterRenderer->GetTotalInstanceCount() : 0;
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
		TEXT("Async In-Flight: %d / %d\n")
		TEXT("GPU Extraction: %s\n")
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
		AsyncScatterInProgress.Num(), MaxAsyncScatterTasks,
		bUseGPUExtraction ? TEXT("Enabled") : TEXT("Disabled"),
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

	// Throttle by number of in-flight async tasks
	const int32 AvailableSlots = MaxAsyncScatterTasks - AsyncScatterInProgress.Num();
	if (AvailableSlots <= 0)
	{
		return;
	}

	// Also respect per-frame limit
	int32 NumToLaunch = FMath::Min(AvailableSlots, PendingGenerationQueue.Num());
	if (MaxScatterGenerationsPerFrame > 0)
	{
		NumToLaunch = FMath::Min(NumToLaunch, MaxScatterGenerationsPerFrame);
	}

	int32 LaunchedCount = 0;

	// Launch from back of queue (closest chunks are at back, due to reversed sort)
	for (int32 i = 0; i < NumToLaunch; ++i)
	{
		if (PendingGenerationQueue.Num() == 0)
		{
			break;
		}

		// Take last item (closest chunk — O(1) pop from back)
		FPendingScatterGeneration Request = MoveTemp(PendingGenerationQueue.Last());
		PendingGenerationQueue.Pop(EAllowShrinking::No);
		PendingQueueSet.Remove(Request.ChunkCoord);

		// Launch async scatter generation on thread pool
		LaunchAsyncScatterGeneration(MoveTemp(Request));
		++LaunchedCount;
	}

	if (LaunchedCount > 0)
	{
		UE_LOG(LogVoxelScatter, Verbose, TEXT("Launched %d async scatter tasks (%d queued, %d in-flight)"),
			LaunchedCount, PendingGenerationQueue.Num(), AsyncScatterInProgress.Num());
	}
}

void UVoxelScatterManager::GenerateChunkScatterFromPending(const FPendingScatterGeneration& PendingData)
{
	const FIntVector& ChunkCoord = PendingData.ChunkCoord;
	const FVector ChunkWorldOrigin = GetChunkWorldOrigin(ChunkCoord);

	// Use definitions captured at queue time (already filtered by distance)
	const TArray<FScatterDefinition>& FilteredDefinitions = PendingData.CapturedDefinitions;

	if (FilteredDefinitions.Num() == 0)
	{
		return;
	}

	// Validate voxel data
	if (PendingData.ChunkVoxelData.Num() != PendingData.ChunkSize * PendingData.ChunkSize * PendingData.ChunkSize)
	{
		UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): Skipped scatter - invalid voxel data"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
		return;
	}

	// Extract surface points from voxel data (LOD-independent)
	TArray<FClearedScatterVolume> ClearedVolumes;
	if (const TArray<FClearedScatterVolume>* Volumes = ClearedVolumesPerChunk.Find(ChunkCoord))
	{
		ClearedVolumes = *Volumes;
	}

	FChunkSurfaceData SurfaceData;
	ExtractSurfacePointsFromVoxelData(
		PendingData.ChunkVoxelData,
		ChunkCoord,
		ChunkWorldOrigin,
		PendingData.ChunkSize,
		PendingData.VoxelSize,
		SurfacePointSpacing,
		ClearedVolumes,
		SurfaceData
	);

	if (!SurfaceData.bIsValid || SurfaceData.SurfacePoints.Num() == 0)
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

	// Track completed types
	TSet<int32>& Completed = CompletedScatterTypes.FindOrAdd(ChunkCoord);
	for (const FScatterDefinition& Def : FilteredDefinitions)
	{
		Completed.Add(Def.ScatterID);
	}

	// Append or create scatter data
	FChunkScatterData* ExistingScatter = ScatterDataCache.Find(ChunkCoord);
	if (ExistingScatter && ExistingScatter->bIsValid)
	{
		ExistingScatter->SpawnPoints.Append(ScatterData.SpawnPoints);
	}
	else
	{
		ScatterDataCache.Add(ChunkCoord, MoveTemp(ScatterData));
	}
	TotalSpawnPointsGenerated += SpawnCount;
	++TotalChunksProcessed;

	// Update HISM instances with full (possibly merged) scatter data
	if (ScatterRenderer && ScatterRenderer->IsInitialized())
	{
		ScatterRenderer->UpdateChunkInstances(ChunkCoord, ScatterDataCache[ChunkCoord]);
	}

	OnChunkScatterReady.Broadcast(ChunkCoord, SpawnCount);

	UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): Generated scatter from queue (%d surface pts, %d spawn pts)"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z,
		SurfaceDataCache[ChunkCoord].SurfacePoints.Num(), SpawnCount);
}

void UVoxelScatterManager::LaunchAsyncScatterGeneration(FPendingScatterGeneration PendingData)
{
	const FIntVector ChunkCoord = PendingData.ChunkCoord;

	// Mark as in-progress
	AsyncScatterInProgress.Add(ChunkCoord);

	// Capture all values needed by the background thread (no UObject access)
	const FVector ChunkWorldOrigin = GetChunkWorldOrigin(ChunkCoord);
	const float CapturedSurfacePointSpacing = SurfacePointSpacing;
	const uint32 CapturedWorldSeed = WorldSeed;

	// Use definitions captured at queue time (already filtered by distance)
	TArray<FScatterDefinition> FilteredDefinitions = MoveTemp(PendingData.CapturedDefinitions);

	if (FilteredDefinitions.Num() == 0)
	{
		AsyncScatterInProgress.Remove(ChunkCoord);
		return;
	}

	// === GPU Extraction Path (uses mesh vertex data) ===
	if (bUseGPUExtraction)
	{
		// Dispatch GPU surface extraction
		FGPUExtractionRequest GPURequest;
		GPURequest.ChunkCoord = ChunkCoord;
		GPURequest.ChunkWorldOrigin = ChunkWorldOrigin;
		GPURequest.CellSize = CapturedSurfacePointSpacing;
		GPURequest.Positions = MoveTemp(PendingData.Positions);
		GPURequest.Normals = MoveTemp(PendingData.Normals);
		GPURequest.UV1s = MoveTemp(PendingData.UV1s);
		GPURequest.Colors = MoveTemp(PendingData.Colors);

		// Store filtered definitions and LOD level for placement after GPU extraction completes
		GPUExtractionPendingPlacement.Add(ChunkCoord, MoveTemp(FilteredDefinitions));
		GPUExtractionPendingLODLevel.Add(ChunkCoord, PendingData.LODLevel);

		FVoxelGPUSurfaceExtractor::DispatchExtraction(
			MoveTemp(GPURequest),
			&CompletedGPUExtractionQueue);

		UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): Dispatched GPU scatter extraction"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
		return;
	}

	// === CPU Async Path (voxel-based extraction — LOD-independent) ===

	// Snapshot cleared volumes for this chunk (read-only on thread pool)
	TArray<FClearedScatterVolume> CapturedClearedVolumes;
	if (const TArray<FClearedScatterVolume>* Volumes = ClearedVolumesPerChunk.Find(ChunkCoord))
	{
		CapturedClearedVolumes = *Volumes;
	}

	TWeakObjectPtr<UVoxelScatterManager> WeakThis(this);

	Async(EAsyncExecution::ThreadPool,
		[WeakThis, PendingData = MoveTemp(PendingData), ChunkWorldOrigin,
		 CapturedSurfacePointSpacing, CapturedWorldSeed, ChunkCoord,
		 FilteredDefinitions = MoveTemp(FilteredDefinitions),
		 CapturedClearedVolumes = MoveTemp(CapturedClearedVolumes)]() mutable
	{
		// === THREAD POOL: Voxel-based surface extraction + placement ===

		FAsyncScatterResult Result;
		Result.ChunkCoord = ChunkCoord;
		Result.bSuccess = false;

		// Validate voxel data
		const int32 ExpectedVoxels = PendingData.ChunkSize * PendingData.ChunkSize * PendingData.ChunkSize;
		if (PendingData.ChunkVoxelData.Num() != ExpectedVoxels)
		{
			if (UVoxelScatterManager* This = WeakThis.Get())
			{
				This->CompletedScatterQueue.Enqueue(MoveTemp(Result));
			}
			return;
		}

		// Extract surface points from voxel data (LOD-independent)
		FChunkSurfaceData SurfaceData;
		ExtractSurfacePointsFromVoxelData(
			PendingData.ChunkVoxelData,
			ChunkCoord,
			ChunkWorldOrigin,
			PendingData.ChunkSize,
			PendingData.VoxelSize,
			CapturedSurfacePointSpacing,
			CapturedClearedVolumes,
			SurfaceData
		);

		if (!SurfaceData.bIsValid || SurfaceData.SurfacePoints.Num() == 0)
		{
			if (UVoxelScatterManager* This = WeakThis.Get())
			{
				This->CompletedScatterQueue.Enqueue(MoveTemp(Result));
			}
			return;
		}

		// Scatter placement
		const uint32 ChunkSeed = FVoxelScatterPlacement::ComputeChunkSeed(ChunkCoord, CapturedWorldSeed);

		FChunkScatterData ScatterData;
		FVoxelScatterPlacement::GenerateSpawnPoints(
			SurfaceData,
			FilteredDefinitions,
			ChunkSeed,
			ScatterData
		);

		Result.SurfaceData = MoveTemp(SurfaceData);
		Result.ScatterData = MoveTemp(ScatterData);
		Result.bSuccess = true;

		// Track which types were generated
		for (const FScatterDefinition& Def : FilteredDefinitions)
		{
			Result.GeneratedTypeIDs.Add(Def.ScatterID);
		}

		// Enqueue result for game thread consumption
		if (UVoxelScatterManager* This = WeakThis.Get())
		{
			This->CompletedScatterQueue.Enqueue(MoveTemp(Result));
		}
	});
}

void UVoxelScatterManager::ProcessCompletedAsyncScatter()
{
	FAsyncScatterResult Result;
	int32 ProcessedCount = 0;
	const int32 MaxProcessPerFrame = 4;

	while (CompletedScatterQueue.Dequeue(Result) && ProcessedCount < MaxProcessPerFrame)
	{
		++ProcessedCount;

		// Remove from in-progress tracking
		AsyncScatterInProgress.Remove(Result.ChunkCoord);

		if (!Configuration)
		{
			continue;
		}

		if (!Result.bSuccess)
		{
			continue;
		}

		const int32 SurfacePointCount = Result.SurfaceData.SurfacePoints.Num();
		const int32 SpawnCount = Result.ScatterData.SpawnPoints.Num();

		// Track which scatter types were generated (never regenerated)
		TSet<int32>& Completed = CompletedScatterTypes.FindOrAdd(Result.ChunkCoord);
		Completed.Append(Result.GeneratedTypeIDs);

		// Append or create scatter/surface data
		FChunkScatterData* ExistingScatter = ScatterDataCache.Find(Result.ChunkCoord);
		if (ExistingScatter && ExistingScatter->bIsValid)
		{
			// Supplemental pass: append new spawn points to existing data
			ExistingScatter->SpawnPoints.Append(Result.ScatterData.SpawnPoints);

			// Update surface data with the better LOD mesh if applicable
			FChunkSurfaceData* ExistingSurface = SurfaceDataCache.Find(Result.ChunkCoord);
			if (ExistingSurface && Result.SurfaceData.LODLevel < ExistingSurface->LODLevel)
			{
				*ExistingSurface = MoveTemp(Result.SurfaceData);
			}

			UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): Supplemental scatter appended (+%d spawn, total %d)"),
				Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z,
				SpawnCount, ExistingScatter->SpawnPoints.Num());
		}
		else
		{
			// First pass: cache new data
			SurfaceDataCache.Add(Result.ChunkCoord, MoveTemp(Result.SurfaceData));
			ScatterDataCache.Add(Result.ChunkCoord, MoveTemp(Result.ScatterData));
		}

		TotalSurfacePointsExtracted += SurfacePointCount;
		TotalSpawnPointsGenerated += SpawnCount;
		++TotalChunksProcessed;

		// Update HISM instances with full (possibly merged) scatter data
		if (ScatterRenderer && ScatterRenderer->IsInitialized())
		{
			ScatterRenderer->UpdateChunkInstances(Result.ChunkCoord, ScatterDataCache[Result.ChunkCoord]);
		}

		OnChunkScatterReady.Broadcast(Result.ChunkCoord, SpawnCount);

		const FChunkSurfaceData* CachedSurface = SurfaceDataCache.Find(Result.ChunkCoord);
		UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): Async scatter complete (%d surface pts, %d spawn pts, LOD %d)"),
			Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z,
			SurfacePointCount, SpawnCount, CachedSurface ? CachedSurface->LODLevel : -1);

		// Check for deferred supplemental pass (e.g., LOD 0 defs arrived while LOD > 0 was in-flight)
		FPendingScatterGeneration DeferredPass;
		if (DeferredSupplementalPasses.RemoveAndCopyValue(Result.ChunkCoord, DeferredPass))
		{
			// Remove any types that were just completed
			DeferredPass.CapturedDefinitions.RemoveAll([&Completed](const FScatterDefinition& Def)
			{
				return Completed.Contains(Def.ScatterID);
			});

			if (DeferredPass.CapturedDefinitions.Num() > 0)
			{
				UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): Re-queuing deferred supplemental (%d defs)"),
					Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z,
					DeferredPass.CapturedDefinitions.Num());

				int32 InsertIndex = Algo::LowerBound(PendingGenerationQueue, DeferredPass);
				PendingGenerationQueue.Insert(MoveTemp(DeferredPass), InsertIndex);
				PendingQueueSet.Add(Result.ChunkCoord);
			}
		}
	}
}

void UVoxelScatterManager::ProcessCompletedGPUExtractions()
{
	FGPUExtractionResult GPUResult;
	int32 ProcessedCount = 0;
	const int32 MaxGPUProcessPerFrame = 4;

	while (CompletedGPUExtractionQueue.Dequeue(GPUResult) && ProcessedCount < MaxGPUProcessPerFrame)
	{
		++ProcessedCount;

		const FIntVector ChunkCoord = GPUResult.ChunkCoord;

		// Look up the pending placement definitions for this chunk
		TArray<FScatterDefinition> FilteredDefinitions;
		if (TArray<FScatterDefinition>* PendingDefs = GPUExtractionPendingPlacement.Find(ChunkCoord))
		{
			FilteredDefinitions = MoveTemp(*PendingDefs);
			GPUExtractionPendingPlacement.Remove(ChunkCoord);
		}

		// Discard if chunk is no longer in async tracking (was unloaded)
		if (!AsyncScatterInProgress.Contains(ChunkCoord))
		{
			UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): GPU extraction result discarded - chunk no longer tracked"),
				ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
			continue;
		}

		if (!GPUResult.bSuccess || FilteredDefinitions.Num() == 0)
		{
			// Remove from in-progress tracking for failed/empty results
			AsyncScatterInProgress.Remove(ChunkCoord);
			continue;
		}

		// GPU extraction complete - now launch CPU placement on thread pool
		// Build surface data from GPU extraction result
		FChunkSurfaceData SurfaceData(ChunkCoord);
		SurfaceData.LODLevel = GPUExtractionPendingLODLevel.FindRef(ChunkCoord);
		GPUExtractionPendingLODLevel.Remove(ChunkCoord);
		SurfaceData.AveragePointSpacing = SurfacePointSpacing;
		SurfaceData.SurfacePoints = MoveTemp(GPUResult.SurfacePoints);
		SurfaceData.SurfaceAreaEstimate = SurfaceData.SurfacePoints.Num() * SurfacePointSpacing * SurfacePointSpacing;
		SurfaceData.bIsValid = true;

		if (SurfaceData.SurfacePoints.Num() == 0)
		{
			AsyncScatterInProgress.Remove(ChunkCoord);
			continue;
		}

		// Filter out points in cleared volumes (GPU doesn't know about these)
		if (const TArray<FClearedScatterVolume>* Volumes = ClearedVolumesPerChunk.Find(ChunkCoord))
		{
			SurfaceData.SurfacePoints.RemoveAll([&Volumes](const FVoxelSurfacePoint& Point)
			{
				for (const FClearedScatterVolume& Volume : *Volumes)
				{
					if (Volume.ContainsPoint(Point.Position))
					{
						return true;
					}
				}
				return false;
			});

			if (SurfaceData.SurfacePoints.Num() == 0)
			{
				AsyncScatterInProgress.Remove(ChunkCoord);
				continue;
			}
		}

		// Launch placement on thread pool (same as CPU async path from here)
		const uint32 CapturedWorldSeed = WorldSeed;
		const int32 GPUSurfacePointCount = SurfaceData.SurfacePoints.Num();
		TWeakObjectPtr<UVoxelScatterManager> WeakThis(this);

		Async(EAsyncExecution::ThreadPool,
			[WeakThis, SurfaceData = MoveTemp(SurfaceData), FilteredDefinitions = MoveTemp(FilteredDefinitions),
			 CapturedWorldSeed, ChunkCoord]() mutable
		{
			FAsyncScatterResult Result;
			Result.ChunkCoord = ChunkCoord;

			const uint32 ChunkSeed = FVoxelScatterPlacement::ComputeChunkSeed(ChunkCoord, CapturedWorldSeed);

			FChunkScatterData ScatterData;
			FVoxelScatterPlacement::GenerateSpawnPoints(
				SurfaceData,
				FilteredDefinitions,
				ChunkSeed,
				ScatterData
			);

			Result.SurfaceData = MoveTemp(SurfaceData);
			Result.ScatterData = MoveTemp(ScatterData);
			Result.bSuccess = true;

			for (const FScatterDefinition& Def : FilteredDefinitions)
			{
				Result.GeneratedTypeIDs.Add(Def.ScatterID);
			}

			if (UVoxelScatterManager* This = WeakThis.Get())
			{
				This->CompletedScatterQueue.Enqueue(MoveTemp(Result));
			}
		});

		UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): GPU extraction complete (%d surface points), launching placement"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, GPUSurfacePointCount);
	}
}

void UVoxelScatterManager::ExtractSurfacePointsFromVoxelData(
	const TArray<FVoxelData>& VoxelData,
	const FIntVector& ChunkCoord,
	const FVector& ChunkWorldOrigin,
	int32 ChunkSize,
	float VoxelSize,
	float SurfacePointSpacing,
	const TArray<FClearedScatterVolume>& ClearedVolumes,
	FChunkSurfaceData& OutSurfaceData)
{
	OutSurfaceData = FChunkSurfaceData(ChunkCoord);
	OutSurfaceData.AveragePointSpacing = SurfacePointSpacing;
	OutSurfaceData.LODLevel = 0; // Voxel-based extraction is always full resolution

	const int32 ExpectedVoxels = ChunkSize * ChunkSize * ChunkSize;
	if (VoxelData.Num() != ExpectedVoxels)
	{
		OutSurfaceData.bIsValid = false;
		return;
	}

	// Stride to match SurfacePointSpacing (e.g., 100cm spacing with 100cm voxels = stride 1)
	const int32 Stride = FMath::Max(1, FMath::RoundToInt(SurfacePointSpacing / VoxelSize));
	const int32 ColumnsPerAxis = (ChunkSize + Stride - 1) / Stride;
	OutSurfaceData.SurfacePoints.Reserve(ColumnsPerAxis * ColumnsPerAxis);

	// Helper lambda: get density at voxel position with bounds clamping
	auto GetDensity = [&](int32 X, int32 Y, int32 Z) -> float
	{
		X = FMath::Clamp(X, 0, ChunkSize - 1);
		Y = FMath::Clamp(Y, 0, ChunkSize - 1);
		Z = FMath::Clamp(Z, 0, ChunkSize - 1);
		const int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
		return static_cast<float>(VoxelData[Index].Density);
	};

	// Scan each column at stride intervals
	for (int32 X = 0; X < ChunkSize; X += Stride)
	{
		for (int32 Y = 0; Y < ChunkSize; Y += Stride)
		{
			// Scan top-down to find topmost surface transition (solid below, air above)
			for (int32 Z = ChunkSize - 1; Z >= 0; --Z)
			{
				const int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
				const FVoxelData& Voxel = VoxelData[Index];

				if (!Voxel.IsSolid())
				{
					continue;
				}

				// Found solid voxel — check if there's air above
				float AirDensity = 0.0f; // Above chunk boundary = air
				if (Z + 1 < ChunkSize)
				{
					const int32 AboveIndex = X + Y * ChunkSize + (Z + 1) * ChunkSize * ChunkSize;
					const FVoxelData& AboveVoxel = VoxelData[AboveIndex];
					if (AboveVoxel.IsSolid())
					{
						continue; // Not a surface transition
					}
					AirDensity = static_cast<float>(AboveVoxel.Density);
				}

				// Interpolate exact Z position (same formula as Marching Cubes edge interpolation)
				const float SolidDensity = static_cast<float>(Voxel.Density);
				float Fraction = 0.5f;
				const float DensityRange = AirDensity - SolidDensity;
				if (FMath::Abs(DensityRange) > SMALL_NUMBER)
				{
					Fraction = (static_cast<float>(VOXEL_SURFACE_THRESHOLD) - SolidDensity) / DensityRange;
					Fraction = FMath::Clamp(Fraction, 0.0f, 1.0f);
				}

				// World position (voxel grid positions, matching mesher convention)
				const FVector WorldPos(
					ChunkWorldOrigin.X + X * VoxelSize,
					ChunkWorldOrigin.Y + Y * VoxelSize,
					ChunkWorldOrigin.Z + (Z + Fraction) * VoxelSize
				);

				// Check cleared volumes
				bool bInClearedVolume = false;
				for (const FClearedScatterVolume& Volume : ClearedVolumes)
				{
					if (Volume.ContainsPoint(WorldPos))
					{
						bInClearedVolume = true;
						break;
					}
				}
				if (bInClearedVolume)
				{
					break; // Skip this column entirely
				}

				// Compute normal from density gradient (central differences)
				const float GradX = GetDensity(X + 1, Y, Z) - GetDensity(X - 1, Y, Z);
				const float GradY = GetDensity(X, Y + 1, Z) - GetDensity(X, Y - 1, Z);
				const float GradZ = GetDensity(X, Y, Z + 1) - GetDensity(X, Y, Z - 1);

				// Normal points from solid toward air (negative gradient direction)
				FVector Normal(-GradX, -GradY, -GradZ);
				if (!Normal.Normalize())
				{
					Normal = FVector::UpVector; // Fallback for flat areas
				}

				// Determine face type from normal direction
				EVoxelFaceType FaceType;
				if (Normal.Z > 0.5f)
				{
					FaceType = EVoxelFaceType::Top;
				}
				else if (Normal.Z < -0.5f)
				{
					FaceType = EVoxelFaceType::Bottom;
				}
				else
				{
					FaceType = EVoxelFaceType::Side;
				}

				// Create surface point
				FVoxelSurfacePoint Point;
				Point.Position = WorldPos;
				Point.Normal = Normal;
				Point.MaterialID = Voxel.MaterialID;
				Point.BiomeID = Voxel.BiomeID;
				Point.FaceType = FaceType;
				Point.AmbientOcclusion = Voxel.GetAO() & 0x03;
				Point.ComputeSlopeAngle();

				OutSurfaceData.SurfacePoints.Add(Point);
				break; // Found topmost surface for this column
			}
		}
	}

	OutSurfaceData.SurfaceAreaEstimate = OutSurfaceData.SurfacePoints.Num() * SurfacePointSpacing * SurfacePointSpacing;
	OutSurfaceData.bIsValid = true;
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
