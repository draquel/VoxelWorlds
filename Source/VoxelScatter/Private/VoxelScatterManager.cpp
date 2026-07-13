// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelScatterManager.h"
#include "VoxelScatterRenderer.h"
#include "VoxelScatterConfiguration.h"
#include "VoxelScatterExclusionSubsystem.h"
#include "VoxelSurfaceExtractor.h"
#include "VoxelScatterPlacement.h"
#include "VoxelWorldConfiguration.h"
#include "IVoxelWorldMode.h"
#include "ChunkRenderData.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelBiomeRegistry.h"
#include "VoxelScatter.h"
#include "DrawDebugHelpers.h"
#include "Algo/BinarySearch.h"
#include "Async/Async.h"
#include "Engine/World.h"
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

	// Auto-override scatter definitions for cubic mode
	if (Config->bAutoCubicScatterDefaults && Config->MeshingMode == EMeshingMode::Cubic)
	{
		for (FScatterDefinition& Def : ScatterDefinitions)
		{
			if (Def.PlacementMode != EScatterPlacementMode::BlockFaceSnap)
			{
				Def.PlacementMode = EScatterPlacementMode::BlockFaceSnap;
			}
		}
		UE_LOG(LogVoxelScatter, Log, TEXT("Cubic mode: auto-overriding %d definitions to BlockFaceSnap"), ScatterDefinitions.Num());
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

	// Register with the world's exclusion subsystem, which replays any volumes registered before
	// this manager existed (e.g. claims placed during world startup) — initialization order between
	// volume producers and the voxel world never matters.
	if (UVoxelScatterExclusionSubsystem* ExclusionSubsystem = World->GetSubsystem<UVoxelScatterExclusionSubsystem>())
	{
		ExclusionSubsystem->NotifyManagerInitialized(this);
	}

	UE_LOG(LogVoxelScatter, Log, TEXT("VoxelScatterManager initialized (Radius=%.0f, PointSpacing=%.0f, Definitions=%d)"),
		ScatterRadius, SurfacePointSpacing, ScatterDefinitions.Num());
}

void UVoxelScatterManager::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	// Detach from the exclusion subsystem while the world reference is still valid
	if (CachedWorld)
	{
		if (UVoxelScatterExclusionSubsystem* ExclusionSubsystem = CachedWorld->GetSubsystem<UVoxelScatterExclusionSubsystem>())
		{
			ExclusionSubsystem->NotifyManagerShutdown(this);
		}
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

	// Drain distance stream queue
	{
		FDistanceStreamResult DiscardedResult;
		while (DistanceStreamQueue.Dequeue(DiscardedResult)) {}
	}
	DistanceStreamInProgress.Empty();

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
	ExclusionVolumes.Empty();
	PendingExclusionRegrow.Empty();

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

	// Launch regrow placements for chunks uncovered by an unregistered exclusion volume
	// (throttled; runs right after completions so freed async slots are usable this tick)
	ProcessPendingExclusionRegrow();

	// Launch new async scatter tasks from pending queue (throttled)
	ProcessPendingGenerationQueue();

	// Distance-based scatter streaming — fully decoupled from chunk generation.
	// Uses its own async pipeline (DistanceStreamQueue / DistanceStreamInProgress).
	// Process completed results every frame; launch new tasks periodically.
	ProcessCompletedDistanceStream();

	TimeSinceLastDistanceCheck += DeltaTime;
	if (TimeSinceLastDistanceCheck >= DistanceStreamingInterval)
	{
		TimeSinceLastDistanceCheck = 0.0f;
		PerformDistanceSpawn();
	}

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

void UVoxelScatterManager::SetSurfaceScatterVisible(bool bVisible)
{
	if (ScatterRenderer)
	{
		ScatterRenderer->SetSurfaceScatterVisible(bVisible);
	}
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

	// Tree mode filtering
	const EVoxelTreeMode TreeMode = Configuration->TreeMode;
	const float VoxelTreeMaxDist = Configuration->VoxelTreeMaxDistance;

	// Build definitions to generate:
	//  - All enabled defs within their SpawnDistance/ScatterRadius range
	//  - Exclude types already completed for this chunk
	//  - No LOD-based filtering: voxel data is always full resolution
	//  - Tree mode filtering: VoxelInjection defs handled based on TreeMode
	TArray<FScatterDefinition> DefsToGenerate;
	for (const FScatterDefinition& Def : ScatterDefinitions)
	{
		if (!Def.bEnabled)
		{
			UE_LOG(LogVoxelScatter, Verbose, TEXT("  Scatter %d (%s): SKIPPED — disabled"),
				Def.ScatterID, *Def.Name);
			continue;
		}

		// Tree mode filtering for VoxelInjection types
		if (Def.MeshType == EScatterMeshType::VoxelInjection)
		{
			// VoxelInjection definitions are rendered as HISM scatter ONLY in HISM or Both (far) mode
			if (TreeMode == EVoxelTreeMode::VoxelData)
			{
				UE_LOG(LogVoxelScatter, Warning, TEXT("  Scatter %d (%s): SKIPPED — VoxelInjection type with TreeMode=VoxelData (trees only in terrain, no HISM). Set TreeMode to HISM or Both to see trees as HISM instances."),
					Def.ScatterID, *Def.Name);
				continue; // Trees are in VoxelData already, skip HISM
			}
			if (TreeMode == EVoxelTreeMode::Both && ChunkDistance <= VoxelTreeMaxDist)
			{
				UE_LOG(LogVoxelScatter, Verbose, TEXT("  Scatter %d (%s): SKIPPED — VoxelInjection type within VoxelTree range (%.0f <= %.0f)"),
					Def.ScatterID, *Def.Name, ChunkDistance, VoxelTreeMaxDist);
				continue; // Within VoxelData range, skip HISM
			}
		}

		// Skip types already generated for this chunk
		if (CompletedTypes && CompletedTypes->Contains(Def.ScatterID))
		{
			UE_LOG(LogVoxelScatter, Verbose, TEXT("  Scatter %d (%s): SKIPPED — already completed for this chunk"),
				Def.ScatterID, *Def.Name);
			continue;
		}

		// Distance check
		const float EffectiveSpawnDistance = Def.SpawnDistance > 0.0f ? Def.SpawnDistance : ScatterRadius;
		if (ChunkDistance > EffectiveSpawnDistance)
		{
			UE_LOG(LogVoxelScatter, Verbose, TEXT("  Scatter %d (%s): SKIPPED — distance %.0f > spawn distance %.0f"),
				Def.ScatterID, *Def.Name, ChunkDistance, EffectiveSpawnDistance);
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

	// Remove from distance stream tracking (stale result will be discarded on arrival)
	DistanceStreamInProgress.Remove(ChunkCoord);

	// Remove deferred supplemental passes
	DeferredSupplementalPasses.Remove(ChunkCoord);

	// Remove from GPU pending placement
	GPUExtractionPendingPlacement.Remove(ChunkCoord);
	GPUExtractionPendingLODLevel.Remove(ChunkCoord);
	GPUExtractionPendingVoxelInfo.Remove(ChunkCoord);

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
	GPUExtractionPendingVoxelInfo.Remove(ChunkCoord);

	// Remove deferred supplemental passes
	DeferredSupplementalPasses.Remove(ChunkCoord);

	// Clear cleared volumes and completed types so scatter can fully regenerate
	ClearedVolumesPerChunk.Remove(ChunkCoord);
	CompletedScatterTypes.Remove(ChunkCoord);

	// Drop cached surface/scatter data so the re-extraction is treated as a fresh first pass (a full
	// replace, not a supplemental append). Deliberately keep the rendered HISM instances: the completed
	// re-extraction replaces this chunk's instances per-type via the smooth UpdateChunkInstances path
	// (release + budgeted re-add), so an edit reclassifies scatter with no visible gap — unlike
	// RemoveChunkScatter (used on unload), which blinks the chunk's scatter out until async
	// re-extraction repopulates it. Stale in-flight results are dropped by the completion paths'
	// AsyncScatterInProgress / cache checks (both cleared above).
	SurfaceDataCache.Remove(ChunkCoord);
	ScatterDataCache.Remove(ChunkCoord);

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

	const float RadiusSq = Radius * Radius;
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

				// Scrub cached surface points inside the radius so distance streaming
				// can't resurrect scatter here from pre-edit surface data (placement
				// never consults cleared volumes; the cache must not contain them)
				if (FChunkSurfaceData* SurfaceData = SurfaceDataCache.Find(ChunkCoord))
				{
					SurfaceData->SurfacePoints.RemoveAll([&WorldPosition, RadiusSq](const FVoxelSurfacePoint& Point)
					{
						return FVector::DistSquared(Point.Position, WorldPosition) <= RadiusSq;
					});
				}

				// Remove spawn points that fall within the radius
				TSet<int32> AffectedTypes;
				FChunkScatterData* ScatterData = ScatterDataCache.Find(ChunkCoord);
				if (ScatterData && ScatterData->bIsValid)
				{
					for (int32 i = ScatterData->SpawnPoints.Num() - 1; i >= 0; --i)
					{
						const FScatterSpawnPoint& Point = ScatterData->SpawnPoints[i];
						if (FVector::DistSquared(Point.Position, WorldPosition) <= RadiusSq)
						{
							AffectedTypes.Add(Point.ScatterTypeID);
							ScatterData->SpawnPoints.RemoveAt(i);
							++TotalRemoved;
						}
					}
				}

				// Replace this chunk's instances for the affected types with the surviving
				// points — immediate release + re-add via the per-frame add budget, which
				// runs every Tick. A full-type rebuild would wait for a stationary viewer
				// AND an idle generation pipeline, leaving cleared scatter visible
				// (floating/clipped) for as long as the player keeps moving.
				if (AffectedTypes.Num() > 0 && ScatterRenderer && ScatterRenderer->IsInitialized())
				{
					for (int32 TypeID : AffectedTypes)
					{
						const FScatterDefinition* Def = GetScatterDefinition(TypeID);
						if (!Def)
						{
							continue;
						}

						TArray<FTransform> RemainingTransforms;
						for (const FScatterSpawnPoint& Point : ScatterData->SpawnPoints)
						{
							if (Point.ScatterTypeID == TypeID)
							{
								RemainingTransforms.Add(Point.GetTransform(Def->bAlignToSurfaceNormal, Def->SurfaceOffset));
							}
						}
						ScatterRenderer->UpdateChunkTypeInstances(ChunkCoord, TypeID, MoveTemp(RemainingTransforms));
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

// ==================== Exclusion Volumes ====================

bool UVoxelScatterManager::RegisterScatterExclusionVolume(const FScatterExclusionVolume& Volume)
{
	if (!Volume.IsUsable())
	{
		UE_LOG(LogVoxelScatter, Warning, TEXT("RegisterScatterExclusionVolume rejected: invalid Id or non-positive extent"));
		return false;
	}

	// Replacing a volume: the old footprint may extend beyond the new one, so queue it for regrow.
	// Regrow placement is filtered against the post-replace volume set at consumption, so any area
	// still covered stays bare and only the newly uncovered part regrows.
	if (const FScatterExclusionVolume* Existing = ExclusionVolumes.Find(Volume.Id))
	{
		QueueExclusionRegrowForBounds(Existing->GetWorldBounds());
	}

	ExclusionVolumes.Add(Volume.Id, Volume);

	// Clear existing foliage inside the volume right away via the smooth per-(chunk,type) replace
	// path. In-flight async results are filtered at consumption (ApplyClearedVolumesToResult), and
	// chunks that stream in later are born bare — so this immediate pass is the only catch-up needed.
	ClearSpawnPointsInExclusionVolume(Volume);

	UE_LOG(LogVoxelScatter, Log, TEXT("Registered scatter exclusion volume %s (half-extent %.0fx%.0fx%.0f at %s)"),
		*Volume.Id.ToString(), Volume.HalfExtent.X, Volume.HalfExtent.Y, Volume.HalfExtent.Z,
		*Volume.Frame.GetLocation().ToCompactString());
	return true;
}

bool UVoxelScatterManager::UnregisterScatterExclusionVolume(const FGuid& VolumeId)
{
	FScatterExclusionVolume Removed;
	if (!ExclusionVolumes.RemoveAndCopyValue(VolumeId, Removed))
	{
		return false;
	}

	// Regrow foliage where the volume used to be: overlapped chunks re-run placement from cached
	// surface data (throttled in Update). Deterministic chunk seeds reproduce the identical points
	// outside the removed volume, so the per-type replace is visually a no-op there.
	QueueExclusionRegrowForBounds(Removed.GetWorldBounds());

	UE_LOG(LogVoxelScatter, Log, TEXT("Unregistered scatter exclusion volume %s (%d chunk(s) queued to regrow)"),
		*VolumeId.ToString(), PendingExclusionRegrow.Num());
	return true;
}

bool UVoxelScatterManager::IsPointInExclusionVolume(const FVector& WorldPosition) const
{
	return IsPointExcluded(WorldPosition);
}

bool UVoxelScatterManager::IsPointExcluded(const FVector& Position) const
{
	// Flat scan: exclusion volumes are few and long-lived (claim footprints), matching the
	// claim registry's own MVP storage judgment. Revisit with an AABB pre-filter if counts grow.
	for (const auto& Pair : ExclusionVolumes)
	{
		if (Pair.Value.ContainsPoint(Position))
		{
			return true;
		}
	}
	return false;
}

void UVoxelScatterManager::ClearSpawnPointsInExclusionVolume(const FScatterExclusionVolume& Volume)
{
	if (!bIsInitialized || !Configuration)
	{
		return;
	}

	const FBox Bounds = Volume.GetWorldBounds();
	const float ChunkWorldSize = Configuration->GetChunkWorldSize();
	const FVector WorldOrigin = Configuration->WorldOrigin;

	const FIntVector MinChunk(
		FMath::FloorToInt((Bounds.Min.X - WorldOrigin.X) / ChunkWorldSize),
		FMath::FloorToInt((Bounds.Min.Y - WorldOrigin.Y) / ChunkWorldSize),
		FMath::FloorToInt((Bounds.Min.Z - WorldOrigin.Z) / ChunkWorldSize));
	const FIntVector MaxChunk(
		FMath::FloorToInt((Bounds.Max.X - WorldOrigin.X) / ChunkWorldSize),
		FMath::FloorToInt((Bounds.Max.Y - WorldOrigin.Y) / ChunkWorldSize),
		FMath::FloorToInt((Bounds.Max.Z - WorldOrigin.Z) / ChunkWorldSize));

	int32 TotalRemoved = 0;
	for (int32 CX = MinChunk.X; CX <= MaxChunk.X; ++CX)
	{
		for (int32 CY = MinChunk.Y; CY <= MaxChunk.Y; ++CY)
		{
			for (int32 CZ = MinChunk.Z; CZ <= MaxChunk.Z; ++CZ)
			{
				const FIntVector ChunkCoord(CX, CY, CZ);
				FChunkScatterData* ScatterData = ScatterDataCache.Find(ChunkCoord);
				if (!ScatterData || !ScatterData->bIsValid)
				{
					continue;
				}

				// Remove spawn points inside the oriented box; note which types lost instances.
				// The surface cache is deliberately left whole — unregister regrows from it.
				TSet<int32> AffectedTypes;
				for (int32 i = ScatterData->SpawnPoints.Num() - 1; i >= 0; --i)
				{
					if (Volume.ContainsPoint(ScatterData->SpawnPoints[i].Position))
					{
						AffectedTypes.Add(ScatterData->SpawnPoints[i].ScatterTypeID);
						ScatterData->SpawnPoints.RemoveAt(i);
						++TotalRemoved;
					}
				}

				// Replace each affected type's instances with the survivors — immediate release +
				// budgeted re-add per (chunk,type), the same flash-free path player edits use.
				if (AffectedTypes.Num() > 0 && ScatterRenderer && ScatterRenderer->IsInitialized())
				{
					for (int32 TypeID : AffectedTypes)
					{
						const FScatterDefinition* Def = GetScatterDefinition(TypeID);
						if (!Def)
						{
							continue;
						}

						TArray<FTransform> RemainingTransforms;
						for (const FScatterSpawnPoint& Point : ScatterData->SpawnPoints)
						{
							if (Point.ScatterTypeID == TypeID)
							{
								RemainingTransforms.Add(Point.GetTransform(Def->bAlignToSurfaceNormal, Def->SurfaceOffset));
							}
						}
						ScatterRenderer->UpdateChunkTypeInstances(ChunkCoord, TypeID, MoveTemp(RemainingTransforms));
					}
				}
			}
		}
	}

	if (TotalRemoved > 0)
	{
		UE_LOG(LogVoxelScatter, Verbose, TEXT("Exclusion volume %s cleared %d spawn point(s)"),
			*Volume.Id.ToString(), TotalRemoved);
	}
}

void UVoxelScatterManager::QueueExclusionRegrowForBounds(const FBox& Bounds)
{
	if (!bIsInitialized || !Configuration)
	{
		return;
	}

	const float ChunkWorldSize = Configuration->GetChunkWorldSize();
	const FVector WorldOrigin = Configuration->WorldOrigin;

	const FIntVector MinChunk(
		FMath::FloorToInt((Bounds.Min.X - WorldOrigin.X) / ChunkWorldSize),
		FMath::FloorToInt((Bounds.Min.Y - WorldOrigin.Y) / ChunkWorldSize),
		FMath::FloorToInt((Bounds.Min.Z - WorldOrigin.Z) / ChunkWorldSize));
	const FIntVector MaxChunk(
		FMath::FloorToInt((Bounds.Max.X - WorldOrigin.X) / ChunkWorldSize),
		FMath::FloorToInt((Bounds.Max.Y - WorldOrigin.Y) / ChunkWorldSize),
		FMath::FloorToInt((Bounds.Max.Z - WorldOrigin.Z) / ChunkWorldSize));

	for (int32 CX = MinChunk.X; CX <= MaxChunk.X; ++CX)
	{
		for (int32 CY = MinChunk.Y; CY <= MaxChunk.Y; ++CY)
		{
			for (int32 CZ = MinChunk.Z; CZ <= MaxChunk.Z; ++CZ)
			{
				const FIntVector ChunkCoord(CX, CY, CZ);
				// Only chunks with cached surface data can regrow in place. Anything else
				// (unloaded, awaiting re-mesh) regenerates naturally on its next fresh pass,
				// which consults the current volume set anyway.
				if (SurfaceDataCache.Contains(ChunkCoord))
				{
					PendingExclusionRegrow.Add(ChunkCoord);
				}
			}
		}
	}
}

void UVoxelScatterManager::ProcessPendingExclusionRegrow()
{
	if (PendingExclusionRegrow.Num() == 0)
	{
		return;
	}

	int32 Launched = 0;
	TArray<FIntVector> Processed;

	for (const FIntVector& ChunkCoord : PendingExclusionRegrow)
	{
		if (Launched >= MaxExclusionRegrowLaunchesPerTick || AsyncScatterInProgress.Num() >= MaxAsyncScatterTasks)
		{
			break;
		}

		// A busy chunk stays queued for a later tick: the replacement result must not
		// interleave with an in-flight (or queued) extraction/stream for the same chunk.
		if (AsyncScatterInProgress.Contains(ChunkCoord)
			|| DistanceStreamInProgress.Contains(ChunkCoord)
			|| PendingQueueSet.Contains(ChunkCoord))
		{
			continue;
		}

		const FChunkSurfaceData* Surface = SurfaceDataCache.Find(ChunkCoord);
		if (!Surface || !Surface->bIsValid || Surface->SurfacePoints.Num() == 0)
		{
			// Surface cache gone (chunk unloaded or a regenerate dropped it) — the next fresh
			// generation is volume-aware, so this entry is obsolete.
			Processed.Add(ChunkCoord);
			continue;
		}

		const FVector ChunkCenter = GetChunkWorldOrigin(ChunkCoord) + FVector(Configuration->GetChunkWorldSize() * 0.5f);
		const float ChunkDistance = FVector::Dist(ChunkCenter, LastViewerPosition);
		TArray<FScatterDefinition> Definitions = BuildEligibleDefinitionsForDistance(ChunkDistance);
		if (Definitions.Num() == 0)
		{
			Processed.Add(ChunkCoord);
			continue;
		}

		// Reset bookkeeping so the completed result goes through the FIRST-PASS route of
		// ProcessCompletedAsyncScatter (full replace via the smooth UpdateChunkInstances path)
		// instead of appending. Rendered instances stay up until the result swaps them per type.
		CompletedScatterTypes.Remove(ChunkCoord);
		ScatterDataCache.Remove(ChunkCoord);
		AsyncScatterInProgress.Add(ChunkCoord);

		FChunkSurfaceData SurfaceCopy = *Surface;
		const uint32 ChunkSeed = FVoxelScatterPlacement::ComputeChunkSeed(ChunkCoord, WorldSeed);
		TWeakObjectPtr<UVoxelScatterManager> WeakThis(this);

		Async(EAsyncExecution::ThreadPool,
			[WeakThis, ChunkCoord, ChunkSeed, SurfaceCopy = MoveTemp(SurfaceCopy),
			 Definitions = MoveTemp(Definitions)]() mutable
		{
			// === THREAD POOL: placement-only relaunch from cached surface data ===
			FAsyncScatterResult Result;
			Result.ChunkCoord = ChunkCoord;
			FVoxelScatterPlacement::GenerateSpawnPoints(SurfaceCopy, Definitions, ChunkSeed, Result.ScatterData);
			for (const FScatterDefinition& Def : Definitions)
			{
				Result.GeneratedTypeIDs.Add(Def.ScatterID);
			}
			Result.SurfaceData = MoveTemp(SurfaceCopy);
			Result.bSuccess = true;

			if (UVoxelScatterManager* This = WeakThis.Get())
			{
				This->CompletedScatterQueue.Enqueue(MoveTemp(Result));
			}
		});

		Processed.Add(ChunkCoord);
		++Launched;
	}

	for (const FIntVector& ChunkCoord : Processed)
	{
		PendingExclusionRegrow.Remove(ChunkCoord);
	}
}

TArray<FScatterDefinition> UVoxelScatterManager::BuildEligibleDefinitionsForDistance(float ChunkDistance) const
{
	TArray<FScatterDefinition> Result;
	if (!Configuration)
	{
		return Result;
	}

	// Same eligibility rules as the OnChunkMeshDataReady hand-off (minus the per-chunk
	// completed-types check — regrow clears that set before launching).
	const EVoxelTreeMode TreeMode = Configuration->TreeMode;
	const float VoxelTreeMaxDist = Configuration->VoxelTreeMaxDistance;

	for (const FScatterDefinition& Def : ScatterDefinitions)
	{
		if (!Def.bEnabled)
		{
			continue;
		}
		if (Def.MeshType == EScatterMeshType::VoxelInjection)
		{
			if (TreeMode == EVoxelTreeMode::VoxelData)
			{
				continue;
			}
			if (TreeMode == EVoxelTreeMode::Both && ChunkDistance <= VoxelTreeMaxDist)
			{
				continue;
			}
		}
		const float EffectiveSpawnDistance = Def.SpawnDistance > 0.0f ? Def.SpawnDistance : ScatterRadius;
		if (ChunkDistance > EffectiveSpawnDistance)
		{
			continue;
		}
		Result.Add(Def);
	}
	return Result;
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

	// Async in-progress sets
	Total += AsyncScatterInProgress.GetAllocatedSize();
	Total += DistanceStreamInProgress.GetAllocatedSize();

	// Cleared volumes
	Total += ClearedVolumesPerChunk.GetAllocatedSize();
	for (const auto& Pair : ClearedVolumesPerChunk)
	{
		Total += Pair.Value.GetAllocatedSize();
	}

	// Exclusion volumes + pending regrow queue
	Total += ExclusionVolumes.GetAllocatedSize();
	Total += PendingExclusionRegrow.GetAllocatedSize();

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
		TEXT("Distance Stream In-Flight: %d / %d\n")
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
		DistanceStreamInProgress.Num(), MaxDistanceStreamTasks,
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

	// Persistent exclusion volumes suppress spawn points on this direct-placement path too
	// (async completion paths are covered by ApplyClearedVolumesToResult).
	if (ExclusionVolumes.Num() > 0)
	{
		ScatterData.SpawnPoints.RemoveAll([this](const FScatterSpawnPoint& Point)
		{
			return IsPointExcluded(Point.Position);
		});
	}

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

void UVoxelScatterManager::ApplyClearedVolumesToResult(const FIntVector& ChunkCoord, FChunkScatterData* ScatterData, FChunkSurfaceData* SurfaceData) const
{
	// Persistent exclusion volumes suppress SPAWN points on every async completion path (CPU,
	// GPU-placement, distance stream) — this is what makes a chunk that streams in while a volume
	// is active born bare. Surface points are deliberately left whole: the surface cache must
	// survive so unregistering a volume can regrow foliage from it.
	if (ScatterData && ExclusionVolumes.Num() > 0)
	{
		ScatterData->SpawnPoints.RemoveAll([this](const FScatterSpawnPoint& Point)
		{
			return IsPointExcluded(Point.Position);
		});
	}

	const TArray<FClearedScatterVolume>* Volumes = ClearedVolumesPerChunk.Find(ChunkCoord);
	if (!Volumes || Volumes->Num() == 0)
	{
		return;
	}

	auto InAnyVolume = [Volumes](const FVector& Position)
	{
		for (const FClearedScatterVolume& Volume : *Volumes)
		{
			if (Volume.ContainsPoint(Position))
			{
				return true;
			}
		}
		return false;
	};

	if (ScatterData)
	{
		ScatterData->SpawnPoints.RemoveAll([&InAnyVolume](const FScatterSpawnPoint& Point)
		{
			return InAnyVolume(Point.Position);
		});
	}

	if (SurfaceData)
	{
		SurfaceData->SurfacePoints.RemoveAll([&InAnyVolume](const FVoxelSurfacePoint& Point)
		{
			return InAnyVolume(Point.Position);
		});
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

void UVoxelScatterManager::PerformDistanceSpawn()
{
	if (!Configuration || SurfaceDataCache.Num() == 0)
	{
		return;
	}

	// Collect definitions that have per-definition SpawnDistance
	TArray<const FScatterDefinition*> DistanceLimitedDefs;
	for (const FScatterDefinition& Def : ScatterDefinitions)
	{
		if (Def.bEnabled && Def.SpawnDistance > 0.0f)
		{
			DistanceLimitedDefs.Add(&Def);
		}
	}

	if (DistanceLimitedDefs.Num() == 0)
	{
		return;
	}

	const float ChunkWorldSize = Configuration->GetChunkWorldSize();

	// --- Out-of-range cleanup pass ---
	// For types beyond SpawnDistance * (1 + hysteresis):
	// 1. Clear from CompletedScatterTypes (allows regeneration on return)
	// 2. Remove spawn points from ScatterDataCache (prevents data bloat)
	// 3. Release instances in renderer (zero-scaled, returned to pool — no flicker)

	for (auto& CompletedPair : CompletedScatterTypes)
	{
		const FIntVector& ChunkCoord = CompletedPair.Key;
		TSet<int32>& CompletedTypeIDs = CompletedPair.Value;

		const FVector ChunkCenter = GetChunkWorldOrigin(ChunkCoord) +
			FVector(ChunkWorldSize * 0.5f);
		const float ChunkDistance = FVector::Dist(ChunkCenter, LastViewerPosition);

		TArray<int32> TypesToRemove;
		for (const FScatterDefinition* Def : DistanceLimitedDefs)
		{
			const float CleanupDistance = Def->SpawnDistance * (1.0f + DistanceCleanupHysteresis);
			if (ChunkDistance > CleanupDistance && CompletedTypeIDs.Contains(Def->ScatterID))
			{
				TypesToRemove.Add(Def->ScatterID);
			}
		}

		if (TypesToRemove.Num() == 0)
		{
			continue;
		}

		// Clear from completed tracking (allows regeneration when player returns)
		for (int32 TypeID : TypesToRemove)
		{
			CompletedTypeIDs.Remove(TypeID);
		}

		// Release instances directly — zero-scaled, returned to pool
		if (ScatterRenderer && ScatterRenderer->IsInitialized())
		{
			for (int32 TypeID : TypesToRemove)
			{
				ScatterRenderer->ReleaseChunkScatterType(ChunkCoord, TypeID);
			}
		}

		// Remove spawn points for these types from the scatter data cache
		FChunkScatterData* ScatterData = ScatterDataCache.Find(ChunkCoord);
		if (ScatterData && ScatterData->bIsValid)
		{
			TSet<int32> TypeIDSet(TypesToRemove);
			ScatterData->SpawnPoints.RemoveAll([&TypeIDSet](const FScatterSpawnPoint& Point)
			{
				return TypeIDSet.Contains(Point.ScatterTypeID);
			});
		}
	}

	// --- Spawn pass ---
	// Don't launch more if our dedicated pipeline is saturated
	const int32 AvailableSlots = MaxDistanceStreamTasks - DistanceStreamInProgress.Num();
	if (AvailableSlots <= 0)
	{
		return;
	}

	// Collect candidates: chunks with cached surface data that need new scatter types
	struct FSpawnCandidate
	{
		FIntVector ChunkCoord;
		float Distance;
		TArray<FScatterDefinition> Definitions;
	};
	TArray<FSpawnCandidate> Candidates;

	for (const auto& Pair : SurfaceDataCache)
	{
		const FIntVector& ChunkCoord = Pair.Key;
		const FChunkSurfaceData& SurfaceData = Pair.Value;

		if (!SurfaceData.bIsValid || SurfaceData.SurfacePoints.Num() == 0)
		{
			continue;
		}

		// Skip chunks already being processed by distance streaming
		if (DistanceStreamInProgress.Contains(ChunkCoord))
		{
			continue;
		}

		// Skip chunks with a chunk-generation-pipeline task in flight. Matters for the exclusion
		// regrow relaunch, which clears CompletedScatterTypes while KEEPING the surface cache: a
		// distance-spawn here would see every type as un-generated and append a duplicate set on
		// top of the regrow's full replace. The skip just delays this chunk to a later pass.
		if (AsyncScatterInProgress.Contains(ChunkCoord))
		{
			continue;
		}

		const FVector ChunkCenter = GetChunkWorldOrigin(ChunkCoord) +
			FVector(ChunkWorldSize * 0.5f);
		const float ChunkDistance = FVector::Dist(ChunkCenter, LastViewerPosition);

		// Get completed types for this chunk
		const TSet<int32>* CompletedTypes = CompletedScatterTypes.Find(ChunkCoord);

		// Find definitions that are now in range but haven't been generated
		TArray<FScatterDefinition> NewlyInRangeDefs;
		for (const FScatterDefinition* Def : DistanceLimitedDefs)
		{
			if (CompletedTypes && CompletedTypes->Contains(Def->ScatterID))
			{
				continue;
			}

			if (ChunkDistance <= Def->SpawnDistance)
			{
				NewlyInRangeDefs.Add(*Def);
			}
		}

		if (NewlyInRangeDefs.Num() > 0)
		{
			FSpawnCandidate Candidate;
			Candidate.ChunkCoord = ChunkCoord;
			Candidate.Distance = ChunkDistance;
			Candidate.Definitions = MoveTemp(NewlyInRangeDefs);
			Candidates.Add(MoveTemp(Candidate));
		}
	}

	if (Candidates.Num() == 0)
	{
		return;
	}

	// Sort closest first for best player experience
	Candidates.Sort([](const FSpawnCandidate& A, const FSpawnCandidate& B)
	{
		return A.Distance < B.Distance;
	});

	// Launch async tasks on thread pool — dedicated pipeline, no competition with chunk generation
	int32 ChunksLaunched = 0;
	const int32 MaxToLaunch = FMath::Min(MaxDistanceSpawnChunksPerPass, AvailableSlots);

	for (const FSpawnCandidate& Candidate : Candidates)
	{
		if (ChunksLaunched >= MaxToLaunch)
		{
			break;
		}

		const FChunkSurfaceData* SurfaceData = SurfaceDataCache.Find(Candidate.ChunkCoord);
		if (!SurfaceData)
		{
			continue;
		}

		const uint32 ChunkSeed = FVoxelScatterPlacement::ComputeChunkSeed(Candidate.ChunkCoord, WorldSeed);

		// Mark as in-progress in our dedicated tracking (not AsyncScatterInProgress)
		DistanceStreamInProgress.Add(Candidate.ChunkCoord);

		// Pre-mark types as completed to prevent duplicate launches on next pass
		TSet<int32>& Completed = CompletedScatterTypes.FindOrAdd(Candidate.ChunkCoord);
		TArray<int32> TypeIDs;
		for (const FScatterDefinition& Def : Candidate.Definitions)
		{
			Completed.Add(Def.ScatterID);
			TypeIDs.Add(Def.ScatterID);
		}

		// Capture data for thread pool — copy surface data, move definitions
		TWeakObjectPtr<UVoxelScatterManager> WeakThis(this);
		const FIntVector ChunkCoord = Candidate.ChunkCoord;
		FChunkSurfaceData CapturedSurface = *SurfaceData;
		TArray<FScatterDefinition> DefsToGenerate = Candidate.Definitions;

		Async(EAsyncExecution::ThreadPool,
			[WeakThis, ChunkCoord, ChunkSeed,
			 CapturedSurface = MoveTemp(CapturedSurface),
			 DefsToGenerate = MoveTemp(DefsToGenerate),
			 TypeIDs = MoveTemp(TypeIDs)]() mutable
		{
			FDistanceStreamResult Result;
			Result.ChunkCoord = ChunkCoord;

			FVoxelScatterPlacement::GenerateSpawnPoints(
				CapturedSurface,
				DefsToGenerate,
				ChunkSeed,
				Result.ScatterData
			);

			Result.GeneratedTypeIDs = MoveTemp(TypeIDs);
			Result.bSuccess = true;

			if (UVoxelScatterManager* This = WeakThis.Get())
			{
				This->DistanceStreamQueue.Enqueue(MoveTemp(Result));
			}
		});

		++ChunksLaunched;
	}

	if (ChunksLaunched > 0)
	{
		UE_LOG(LogVoxelScatter, Verbose, TEXT("Distance spawn: launched %d async tasks (%d candidates, %d in-flight)"),
			ChunksLaunched, Candidates.Num(), DistanceStreamInProgress.Num());
	}
}

void UVoxelScatterManager::ProcessCompletedDistanceStream()
{
	FDistanceStreamResult Result;
	int32 ProcessedCount = 0;

	// Count check BEFORE dequeue — see ProcessCompletedAsyncScatter: dequeue-first
	// drops one result on the exiting iteration and leaks its in-progress entry.
	while (ProcessedCount < MaxDistanceStreamResultsPerFrame && DistanceStreamQueue.Dequeue(Result))
	{
		++ProcessedCount;

		// Remove from distance stream tracking
		DistanceStreamInProgress.Remove(Result.ChunkCoord);

		if (!Result.bSuccess)
		{
			continue;
		}

		// Discard if chunk was unloaded while async was in-flight
		if (!SurfaceDataCache.Contains(Result.ChunkCoord))
		{
			continue;
		}

		// Filter against cleared volumes registered AFTER the task launched (player
		// edits during flight) — the launch-time surface snapshot can't have seen them
		ApplyClearedVolumesToResult(Result.ChunkCoord, &Result.ScatterData, nullptr);

		const int32 SpawnCount = Result.ScatterData.SpawnPoints.Num();

		// Send to renderer for budget-limited HISM addition
		if (SpawnCount > 0 && ScatterRenderer && ScatterRenderer->IsInitialized())
		{
			ScatterRenderer->AddSupplementalInstances(Result.ChunkCoord, Result.ScatterData);
		}

		// Append to scatter data cache
		FChunkScatterData* ExistingScatter = ScatterDataCache.Find(Result.ChunkCoord);
		if (ExistingScatter && ExistingScatter->bIsValid)
		{
			ExistingScatter->SpawnPoints.Append(Result.ScatterData.SpawnPoints);
		}
		else
		{
			ScatterDataCache.Add(Result.ChunkCoord, MoveTemp(Result.ScatterData));
		}

		TotalSpawnPointsGenerated += SpawnCount;
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
	const FScatterExtractionContext ExtractionContext = MakeExtractionContext();
	const bool bUseCubicExtraction = Configuration && Configuration->MeshingMode == EMeshingMode::Cubic;
	if (bUseCubicExtraction)
	{
		ExtractSurfacePointsCubic(
			PendingData.ChunkVoxelData,
			ChunkCoord,
			ChunkWorldOrigin,
			PendingData.ChunkSize,
			PendingData.VoxelSize,
			ClearedVolumes,
			ExtractionContext,
			SurfaceData
		);
	}
	else
	{
		ExtractSurfacePointsFromVoxelData(
			PendingData.ChunkVoxelData,
			ChunkCoord,
			ChunkWorldOrigin,
			PendingData.ChunkSize,
			PendingData.VoxelSize,
			SurfacePointSpacing,
			ClearedVolumes,
			ExtractionContext,
			SurfaceData
		);
	}

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

	// Persistent exclusion volumes suppress spawn points on this direct-placement path too
	// (async completion paths are covered by ApplyClearedVolumesToResult).
	if (ExclusionVolumes.Num() > 0)
	{
		ScatterData.SpawnPoints.RemoveAll([this](const FScatterSpawnPoint& Point)
		{
			return IsPointExcluded(Point.Position);
		});
	}

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
	// Fall back to CPU when LOD > 0: GPU extraction uses mesh vertices which vary
	// with LOD level, causing inconsistent scatter density. CPU path always uses
	// full-resolution voxel data (LOD-independent).
	const bool bUseCPUFallback = bUseGPUExtraction && PendingData.LODLevel > 0;
	if (bUseGPUExtraction && !bUseCPUFallback)
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

		// Store filtered definitions, LOD level, and voxel data for placement after GPU extraction completes
		GPUExtractionPendingPlacement.Add(ChunkCoord, MoveTemp(FilteredDefinitions));
		GPUExtractionPendingLODLevel.Add(ChunkCoord, PendingData.LODLevel);

		// Store voxel data for underground classification of GPU-extracted surface points
		if (PendingData.ChunkVoxelData.Num() > 0)
		{
			FGPUExtractionVoxelInfo VoxelInfo;
			VoxelInfo.VoxelData = MoveTemp(PendingData.ChunkVoxelData);
			VoxelInfo.ChunkSize = PendingData.ChunkSize;
			VoxelInfo.VoxelSize = PendingData.VoxelSize;
			GPUExtractionPendingVoxelInfo.Add(ChunkCoord, MoveTemp(VoxelInfo));
		}

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

	const bool bCubicExtraction = Configuration && Configuration->MeshingMode == EMeshingMode::Cubic;
	// Build the classification context on the game thread and capture it by value. It holds a
	// non-owning world-mode pointer (same lifetime as the owning ChunkManager, per its async-gen
	// pattern) plus water level and noise params.
	const FScatterExtractionContext ExtractionContext = MakeExtractionContext();
	TWeakObjectPtr<UVoxelScatterManager> WeakThis(this);

	Async(EAsyncExecution::ThreadPool,
		[WeakThis, PendingData = MoveTemp(PendingData), ChunkWorldOrigin,
		 CapturedSurfacePointSpacing, CapturedWorldSeed, ChunkCoord,
		 FilteredDefinitions = MoveTemp(FilteredDefinitions),
		 CapturedClearedVolumes = MoveTemp(CapturedClearedVolumes),
		 ExtractionContext, bCubicExtraction]() mutable
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
		if (bCubicExtraction)
		{
			ExtractSurfacePointsCubic(
				PendingData.ChunkVoxelData,
				ChunkCoord,
				ChunkWorldOrigin,
				PendingData.ChunkSize,
				PendingData.VoxelSize,
				CapturedClearedVolumes,
				ExtractionContext,
				SurfaceData
			);
		}
		else
		{
			ExtractSurfacePointsFromVoxelData(
				PendingData.ChunkVoxelData,
				ChunkCoord,
				ChunkWorldOrigin,
				PendingData.ChunkSize,
				PendingData.VoxelSize,
				CapturedSurfacePointSpacing,
				CapturedClearedVolumes,
				ExtractionContext,
				SurfaceData
			);
		}

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
	const int32 MaxProcessPerFrame = 2;

	// Count check BEFORE dequeue: dequeuing first would pop a result on the
	// iteration that exits the loop and silently drop it, leaving its chunk
	// stuck in AsyncScatterInProgress forever.
	while (ProcessedCount < MaxProcessPerFrame && CompletedScatterQueue.Dequeue(Result))
	{
		++ProcessedCount;

		// Remove from in-progress tracking. If the chunk is no longer tracked,
		// it was unloaded or regeneration was requested while the task was in
		// flight — discard the stale result rather than resurrecting scatter
		// (cache entries + HISM instances) for a chunk that no longer exists.
		if (AsyncScatterInProgress.Remove(Result.ChunkCoord) == 0)
		{
			UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): Async scatter result discarded - chunk no longer tracked"),
				Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z);
			continue;
		}

		if (!Configuration)
		{
			continue;
		}

		if (!Result.bSuccess)
		{
			continue;
		}

		// Filter against cleared volumes registered AFTER the task launched (player
		// edits during flight) — the launch-time snapshot can't have seen them
		ApplyClearedVolumesToResult(Result.ChunkCoord, &Result.ScatterData, &Result.SurfaceData);

		const int32 SurfacePointCount = Result.SurfaceData.SurfacePoints.Num();
		const int32 SpawnCount = Result.ScatterData.SpawnPoints.Num();

		// Track which scatter types were generated (never regenerated)
		TSet<int32>& Completed = CompletedScatterTypes.FindOrAdd(Result.ChunkCoord);
		Completed.Append(Result.GeneratedTypeIDs);

		// Append or create scatter/surface data
		FChunkScatterData* ExistingScatter = ScatterDataCache.Find(Result.ChunkCoord);
		const bool bSupplementalPass = (ExistingScatter && ExistingScatter->bIsValid);
		if (bSupplementalPass)
		{
			// Supplemental pass: append new spawn points to existing data. CompletedScatterTypes
			// guarantees this pass only generated types NOT already on the chunk, so the render
			// update below is append-only and never disturbs existing instances.
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

		// Update HISM instances. A first pass adds the whole chunk via the deferred-add budget;
		// a supplemental pass appends ONLY the newly generated points, leaving every existing
		// instance (this chunk and all others) in place. Routing supplemental updates through
		// AddSupplementalInstances instead of UpdateChunkInstances is what prevents a per-chunk
		// streaming update from triggering a world-wide per-type rebuild — the cause of the
		// "everything blinks out and streams back" refresh flash.
		if (ScatterRenderer && ScatterRenderer->IsInitialized())
		{
			if (bSupplementalPass)
			{
				ScatterRenderer->AddSupplementalInstances(Result.ChunkCoord, Result.ScatterData);
			}
			else
			{
				ScatterRenderer->UpdateChunkInstances(Result.ChunkCoord, ScatterDataCache[Result.ChunkCoord]);
			}
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

	// Count check BEFORE dequeue — see ProcessCompletedAsyncScatter: dequeue-first
	// drops one result on the exiting iteration and leaks its in-progress entry.
	while (ProcessedCount < MaxGPUProcessPerFrame && CompletedGPUExtractionQueue.Dequeue(GPUResult))
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
			GPUExtractionPendingVoxelInfo.Remove(ChunkCoord);
			continue;
		}

		if (!GPUResult.bSuccess || FilteredDefinitions.Num() == 0)
		{
			// Remove from in-progress tracking for failed/empty results
			AsyncScatterInProgress.Remove(ChunkCoord);
			GPUExtractionPendingVoxelInfo.Remove(ChunkCoord);
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

		// Retrieve voxel data for underground classification (if available)
		TArray<FVoxelData> CapturedVoxelData;
		int32 CapturedChunkSize = 0;
		float CapturedVoxelSize = 0.0f;
		if (FGPUExtractionVoxelInfo* VoxelInfo = GPUExtractionPendingVoxelInfo.Find(ChunkCoord))
		{
			CapturedVoxelData = MoveTemp(VoxelInfo->VoxelData);
			CapturedChunkSize = VoxelInfo->ChunkSize;
			CapturedVoxelSize = VoxelInfo->VoxelSize;
			GPUExtractionPendingVoxelInfo.Remove(ChunkCoord);
		}

		const FVector CapturedChunkWorldOrigin = GetChunkWorldOrigin(ChunkCoord);

		// Launch placement on thread pool (same as CPU async path from here)
		const uint32 CapturedWorldSeed = WorldSeed;
		const int32 GPUSurfacePointCount = SurfaceData.SurfacePoints.Num();
		const FScatterExtractionContext ExtractionContext = MakeExtractionContext();
		TWeakObjectPtr<UVoxelScatterManager> WeakThis(this);

		Async(EAsyncExecution::ThreadPool,
			[WeakThis, SurfaceData = MoveTemp(SurfaceData), FilteredDefinitions = MoveTemp(FilteredDefinitions),
			 CapturedWorldSeed, ChunkCoord,
			 CapturedVoxelData = MoveTemp(CapturedVoxelData), CapturedChunkSize, CapturedVoxelSize,
			 CapturedChunkWorldOrigin, ExtractionContext]() mutable
		{
			// Classify GPU-extracted surface points as underground / underwater using voxel data plus
			// the analytic terrain height and water level (the cross-chunk coverage + water-level fixes).
			if (CapturedVoxelData.Num() == CapturedChunkSize * CapturedChunkSize * CapturedChunkSize && CapturedChunkSize > 0)
			{
				ClassifySurfacePointsUnderground(
					SurfaceData.SurfacePoints,
					CapturedVoxelData,
					CapturedChunkWorldOrigin,
					CapturedChunkSize,
					CapturedVoxelSize,
					ExtractionContext);
			}

			// Count underground points for diagnostic
			int32 UGCount = 0;
			for (const FVoxelSurfacePoint& Pt : SurfaceData.SurfacePoints)
			{
				if (Pt.bIsUnderground) ++UGCount;
			}

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

			UE_LOG(LogVoxelScatter, Verbose, TEXT("GPUScatter (%d,%d,%d): surfPts=%d underground=%d spawnPts=%d defs=%d"),
				ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z,
				SurfaceData.SurfacePoints.Num(), UGCount, ScatterData.SpawnPoints.Num(), FilteredDefinitions.Num());

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

void UVoxelScatterManager::ClassifySurfacePointsUnderground(
	TArray<FVoxelSurfacePoint>& SurfacePoints,
	const TArray<FVoxelData>& VoxelData,
	const FVector& ChunkWorldOrigin,
	int32 ChunkSize,
	float VoxelSize,
	const FScatterExtractionContext& Context)
{
	const int32 ExpectedVoxels = ChunkSize * ChunkSize * ChunkSize;
	if (VoxelData.Num() != ExpectedVoxels || ChunkSize <= 0 || VoxelSize <= 0.0f)
		return;

	const int32 SliceSize = ChunkSize * ChunkSize;
	int32 UndergroundCount = 0;

	// World Z of this chunk's top, for the edit-independent cross-chunk coverage test below.
	const float ChunkTopWorldZ = ChunkWorldOrigin.Z + ChunkSize * VoxelSize;

	for (FVoxelSurfacePoint& Point : SurfacePoints)
	{
		const FVector LocalPos = (Point.Position - ChunkWorldOrigin) / VoxelSize;
		const int32 VX = FMath::Clamp(FMath::FloorToInt(LocalPos.X), 0, ChunkSize - 1);
		const int32 VY = FMath::Clamp(FMath::FloorToInt(LocalPos.Y), 0, ChunkSize - 1);
		const int32 VZ = FMath::Clamp(FMath::FloorToInt(LocalPos.Z), 0, ChunkSize - 1);
		const int32 Idx = VX + VY * ChunkSize + VZ * SliceSize;

		bool bFlaggedUnderground = false;

		// Direct flag check — covers both air and solid boundary voxels
		if (VoxelData[Idx].HasUndergroundFlag())
		{
			bFlaggedUnderground = true;
		}
		// For mesh-interpolated positions on solid voxels, check air neighbor along normal
		else if (VoxelData[Idx].IsSolid())
		{
			const FVector AirLocal = (Point.Position + FVector(Point.Normal) * VoxelSize * 0.5f - ChunkWorldOrigin) / VoxelSize;
			const int32 AX = FMath::Clamp(FMath::FloorToInt(AirLocal.X), 0, ChunkSize - 1);
			const int32 AY = FMath::Clamp(FMath::FloorToInt(AirLocal.Y), 0, ChunkSize - 1);
			const int32 AZ = FMath::Clamp(FMath::FloorToInt(AirLocal.Z), 0, ChunkSize - 1);
			const int32 AirIdx = AX + AY * ChunkSize + AZ * SliceSize;

			if (VoxelData[AirIdx].HasUndergroundFlag())
			{
				bFlaggedUnderground = true;
			}
		}

		// Column scan override for upward-facing surfaces (floors, slopes).
		// Two checks:
		// 1. Find first air above — if surface air (not underground), de-classify
		// 2. If genuinely underground, check cave headroom — de-classify if ceiling
		//    is too close (mushroom meshes would poke through thin terrain)
		if (bFlaggedUnderground && Point.Normal.Z > 0.3f)
		{
			bool bFoundUndergroundAir = false;
			int32 Headroom = 0; // air voxels above before hitting solid ceiling

			for (int32 dz = 1; dz < ChunkSize; ++dz)
			{
				const int32 CheckZ = VZ + dz;
				if (CheckZ >= ChunkSize)
					break;

				const int32 CheckIdx = VX + VY * ChunkSize + CheckZ * SliceSize;
				const FVoxelData& CheckVoxel = VoxelData[CheckIdx];

				if (!CheckVoxel.IsSolid())
				{
					if (!bFoundUndergroundAir)
					{
						// First air voxel — determines underground vs surface
						if (CheckVoxel.HasUndergroundFlag())
							bFoundUndergroundAir = true;
						else
							break; // Surface air → will de-classify below
					}
					++Headroom;
				}
				else if (bFoundUndergroundAir)
				{
					// Hit ceiling after finding underground air
					break;
				}
			}

			// De-classify if surface air above, or chunk boundary (all solid)
			if (!bFoundUndergroundAir)
			{
				bFlaggedUnderground = false;
			}
			// De-classify if cave headroom is insufficient (< 3 voxels = 300 units).
			// Prevents mushroom meshes from poking through thin cave ceilings.
			else if (Headroom < 3)
			{
				bFlaggedUnderground = false;
			}
		}

		// Cross-chunk coverage: if the whole chunk sits below the analytic terrain surface it is buried
		// under terrain that lives in a neighboring chunk — the per-chunk flag / column scan above cannot
		// see it. Comparing the CHUNK top (not the per-point Z) keeps this edit-independent: carving into a
		// surface chunk exposes floors to sky that the per-point flag / column scan (run on edit-merged
		// voxels) classify as surface, instead of this analytic height pinning them underground.
		// Same fix as the CPU extractor's bCoveredByTerrain.
		if (!bFlaggedUnderground && Context.WorldMode != nullptr && Context.WorldMode->IsHeightmapBased())
		{
			const float TerrainHeight = Context.WorldMode->GetTerrainHeightAt(
				static_cast<float>(Point.Position.X), static_cast<float>(Point.Position.Y), Context.NoiseParams);
			if (ChunkTopWorldZ < TerrainHeight - VoxelSize * 1.5f)
			{
				bFlaggedUnderground = true;
			}
		}

		if (bFlaggedUnderground)
		{
			Point.bIsUnderground = true;
			++UndergroundCount;
		}
		else
		{
			// Underwater: open water above the point (water flag on the air voxel above) OR the
			// point is below the world water level. The water-level test catches submerged seabeds
			// even where the per-voxel water flag never propagated. Underground takes precedence.
			const FVector AboveLocal = (Point.Position + FVector(0.0f, 0.0f, VoxelSize * 0.5f) - ChunkWorldOrigin) / VoxelSize;
			const int32 UX = FMath::Clamp(FMath::FloorToInt(AboveLocal.X), 0, ChunkSize - 1);
			const int32 UY = FMath::Clamp(FMath::FloorToInt(AboveLocal.Y), 0, ChunkSize - 1);
			const int32 UZ = FMath::Clamp(FMath::FloorToInt(AboveLocal.Z), 0, ChunkSize - 1);
			const int32 UIdx = UX + UY * ChunkSize + UZ * SliceSize;
			const bool bWaterAbove = VoxelData[UIdx].IsAir() && VoxelData[UIdx].HasWaterFlag();
			const bool bBelowWater = Context.bEnableWaterLevel && (Point.Position.Z < Context.WaterLevel);
			if (bWaterAbove || bBelowWater)
			{
				Point.bIsUnderwater = true;
			}
		}
	}

	UE_LOG(LogVoxelScatter, Log, TEXT("ClassifyUnderground: %d/%d surface points marked underground"),
		UndergroundCount, SurfacePoints.Num());
}

void UVoxelScatterManager::ExtractSurfacePointsFromVoxelData(
	const TArray<FVoxelData>& VoxelData,
	const FIntVector& ChunkCoord,
	const FVector& ChunkWorldOrigin,
	int32 ChunkSize,
	float VoxelSize,
	float SurfacePointSpacing,
	const TArray<FClearedScatterVolume>& ClearedVolumes,
	const FScatterExtractionContext& Context,
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

	// A chunk whose top sits more than this far below the analytic terrain surface is treated as
	// buried (its surface points are covered by terrain in the chunk(s) above). ~1.5 voxels absorbs
	// isosurface-interpolation jitter around the real surface.
	const float CoverThreshold = VoxelSize * 1.5f;

	// World Z of this chunk's top. The cross-chunk coverage test below compares the CHUNK top (not the
	// per-point Z) against the analytic surface, so it only fires when the whole chunk is buried under a
	// neighbor. Comparing the chunk top keeps the test edit-independent: when the player carves into a
	// surface chunk (exposing a cave floor to sky), the now-topmost points reclassify to surface via
	// bFoundColumnTop instead of staying underground against an edit-blind analytic height.
	const float ChunkTopWorldZ = ChunkWorldOrigin.Z + ChunkSize * VoxelSize;

	// Helper lambda: get density at voxel position with bounds clamping
	auto GetDensity = [&](int32 X, int32 Y, int32 Z) -> float
	{
		X = FMath::Clamp(X, 0, ChunkSize - 1);
		Y = FMath::Clamp(Y, 0, ChunkSize - 1);
		Z = FMath::Clamp(Z, 0, ChunkSize - 1);
		const int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
		return static_cast<float>(VoxelData[Index].Density);
	};

	// Scan each column at stride intervals, finding all solid-air transitions.
	// Underground classification uses the voxel underground flag (set by
	// ApplyUndergroundClassificationPass) rather than a column-based heuristic,
	// which avoids falsely classifying exposed slope faces as underground.
	for (int32 X = 0; X < ChunkSize; X += Stride)
	{
		for (int32 Y = 0; Y < ChunkSize; Y += Stride)
		{
			// The topmost solid-air transition in a column is the open-sky surface; any
			// transition below it necessarily has solid terrain above (a cave floor, or the
			// ground under an overhang), so it must be classified underground. This closes the
			// gap where ApplyUndergroundClassificationPass leaves a covered floor unflagged
			// (ceiling < MinSolidThickness, or a cave spanning a chunk boundary) — without it,
			// tall surface scatter (trees) lands on those floors and pokes up through the roof.
			// Slope-safe: on a slope each column's terrain surface IS the topmost transition.
			bool bFoundColumnTop = false;

			// Analytic terrain-surface height at this column (computed once) for the cross-chunk
			// coverage check below: a point well below this has solid terrain above it even when that
			// terrain lives in a neighboring chunk. Only heightmap-based world modes provide a height.
			float ColumnTerrainHeight = 0.0f;
			bool bHaveColumnHeight = false;
			if (Context.WorldMode != nullptr && Context.WorldMode->IsHeightmapBased())
			{
				ColumnTerrainHeight = Context.WorldMode->GetTerrainHeightAt(
					static_cast<float>(ChunkWorldOrigin.X + X * VoxelSize),
					static_cast<float>(ChunkWorldOrigin.Y + Y * VoxelSize),
					Context.NoiseParams);
				bHaveColumnHeight = true;
			}

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
				bool bAirAboveIsUnderground = false;
				bool bAirAboveIsWater = false;
				if (Z + 1 < ChunkSize)
				{
					const int32 AboveIndex = X + Y * ChunkSize + (Z + 1) * ChunkSize * ChunkSize;
					const FVoxelData& AboveVoxel = VoxelData[AboveIndex];
					if (AboveVoxel.IsSolid())
					{
						continue; // Not a surface transition
					}
					AirDensity = static_cast<float>(AboveVoxel.Density);
					bAirAboveIsUnderground = AboveVoxel.HasUndergroundFlag();
					bAirAboveIsWater = AboveVoxel.HasWaterFlag();
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
					break; // Skip rest of this column
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

				// Underground if: the cave/air flag says so; OR this is not the topmost surface in the
				// column (a lower transition is always covered by solid above); OR the whole chunk is
				// buried beneath the analytic terrain surface — solid terrain covers it from a NEIGHBORING
				// chunk (the cross-chunk cave floor the flag and column scan miss). The buried-chunk test is
				// edit-independent (see ChunkTopWorldZ), so carving into a surface chunk reclassifies
				// exposed floors to surface instead of pinning them underground.
				const bool bCoveredByTerrain = bHaveColumnHeight && (ChunkTopWorldZ < ColumnTerrainHeight - CoverThreshold);
				Point.bIsUnderground = bAirAboveIsUnderground || Voxel.HasUndergroundFlag() || bFoundColumnTop || bCoveredByTerrain;

				// Submerged: open water above it (water flag on the air voxel above) OR the point is below
				// the world water level. The water-level test catches submerged seabeds even where the
				// per-voxel water flag did not propagate. Underground takes precedence.
				const bool bBelowWater = Context.bEnableWaterLevel && (WorldPos.Z < Context.WaterLevel);
				Point.bIsUnderwater = (bAirAboveIsWater || bBelowWater) && !Point.bIsUnderground;

				OutSurfaceData.SurfacePoints.Add(Point);
				bFoundColumnTop = true;
			}
		}
	}

	OutSurfaceData.SurfaceAreaEstimate = OutSurfaceData.SurfacePoints.Num() * SurfacePointSpacing * SurfacePointSpacing;
	OutSurfaceData.bIsValid = true;

	// Diagnostic: log underground classification results
	int32 UGCount = 0;
	float MinUGZ = TNumericLimits<float>::Max();
	float MaxSurfZ = TNumericLimits<float>::Lowest();
	for (const FVoxelSurfacePoint& Pt : OutSurfaceData.SurfacePoints)
	{
		if (Pt.bIsUnderground)
		{
			++UGCount;
			MinUGZ = FMath::Min(MinUGZ, Pt.Position.Z);
		}
		else
		{
			MaxSurfZ = FMath::Max(MaxSurfZ, Pt.Position.Z);
		}
	}
	if (UGCount > 0)
	{
		UE_LOG(LogVoxelScatter, Warning, TEXT("DIAG Extract chunk(%d,%d,%d): %d/%d underground. SurfMaxZ=%.0f, UGMinZ=%.0f (gap=%.0f)"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z,
			UGCount, OutSurfaceData.SurfacePoints.Num(),
			MaxSurfZ, MinUGZ, MaxSurfZ - MinUGZ);
	}
}

void UVoxelScatterManager::ExtractSurfacePointsCubic(
	const TArray<FVoxelData>& VoxelData,
	const FIntVector& ChunkCoord,
	const FVector& ChunkWorldOrigin,
	int32 ChunkSize,
	float VoxelSize,
	const TArray<FClearedScatterVolume>& ClearedVolumes,
	const FScatterExtractionContext& Context,
	FChunkSurfaceData& OutSurfaceData)
{
	OutSurfaceData = FChunkSurfaceData(ChunkCoord);
	OutSurfaceData.AveragePointSpacing = VoxelSize;
	OutSurfaceData.LODLevel = 0;

	const int32 ExpectedVoxels = ChunkSize * ChunkSize * ChunkSize;
	if (VoxelData.Num() != ExpectedVoxels)
	{
		OutSurfaceData.bIsValid = false;
		return;
	}

	// Surface points per exposed top face — scans all solid-air transitions per column.
	OutSurfaceData.SurfacePoints.Reserve(ChunkSize * ChunkSize);

	// A chunk whose top sits more than this far below the analytic terrain surface is buried
	// (its surface points are covered by terrain above). See ExtractSurfacePointsFromVoxelData.
	const float CoverThreshold = VoxelSize * 1.5f;
	const float ChunkTopWorldZ = ChunkWorldOrigin.Z + ChunkSize * VoxelSize;

	for (int32 X = 0; X < ChunkSize; ++X)
	{
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			// Topmost transition in the column = open-sky surface; anything below it is
			// covered by solid above (cave floor / under an overhang) → underground. Closes
			// the same covered-floor gap as the smooth extractor (see ExtractSurfacePointsFromVoxelData).
			bool bFoundColumnTop = false;

			// Analytic terrain height at this column for the cross-chunk coverage check (below).
			float ColumnTerrainHeight = 0.0f;
			bool bHaveColumnHeight = false;
			if (Context.WorldMode != nullptr && Context.WorldMode->IsHeightmapBased())
			{
				ColumnTerrainHeight = Context.WorldMode->GetTerrainHeightAt(
					static_cast<float>(ChunkWorldOrigin.X + (X + 0.5f) * VoxelSize),
					static_cast<float>(ChunkWorldOrigin.Y + (Y + 0.5f) * VoxelSize),
					Context.NoiseParams);
				bHaveColumnHeight = true;
			}

			for (int32 Z = ChunkSize - 1; Z >= 0; --Z)
			{
				const int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
				const FVoxelData& Voxel = VoxelData[Index];

				if (!Voxel.IsSolid())
				{
					continue;
				}

				// Check air above
				bool bAirAboveIsUnderground = false;
				bool bAirAboveIsWater = false;
				if (Z + 1 < ChunkSize)
				{
					const int32 AboveIndex = X + Y * ChunkSize + (Z + 1) * ChunkSize * ChunkSize;
					const FVoxelData& AboveVoxel = VoxelData[AboveIndex];
					if (AboveVoxel.IsSolid())
					{
						continue; // Not a surface
					}
					bAirAboveIsUnderground = AboveVoxel.HasUndergroundFlag();
					bAirAboveIsWater = AboveVoxel.HasWaterFlag();
				}

				// Block face center: center of block XY, top of block Z
				const FVector WorldPos(
					ChunkWorldOrigin.X + (X + 0.5f) * VoxelSize,
					ChunkWorldOrigin.Y + (Y + 0.5f) * VoxelSize,
					ChunkWorldOrigin.Z + (Z + 1.0f) * VoxelSize
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
					break; // Skip rest of this column
				}

				// Cubic: normal is always up, face type is always Top
				FVoxelSurfacePoint Point;
				Point.Position = WorldPos;
				Point.Normal = FVector::UpVector;
				Point.MaterialID = Voxel.MaterialID;
				Point.BiomeID = Voxel.BiomeID;
				Point.FaceType = EVoxelFaceType::Top;
				Point.AmbientOcclusion = Voxel.GetAO() & 0x03;
				Point.SlopeAngle = 0.0f; // Flat top face

				// Underground if flagged, OR not the topmost surface in the column (covered within this
				// chunk), OR the whole chunk is buried below the analytic terrain surface (covered by a
				// neighboring chunk). The chunk-level test is edit-independent (see smooth extractor).
				const bool bCoveredByTerrain = bHaveColumnHeight && (ChunkTopWorldZ < ColumnTerrainHeight - CoverThreshold);
				Point.bIsUnderground = bAirAboveIsUnderground || Voxel.HasUndergroundFlag() || bFoundColumnTop || bCoveredByTerrain;

				// Submerged: open water above (water flag) OR below the world water level (catches
				// seabeds the water flag missed). Underground takes precedence.
				const bool bBelowWater = Context.bEnableWaterLevel && (WorldPos.Z < Context.WaterLevel);
				Point.bIsUnderwater = (bAirAboveIsWater || bBelowWater) && !Point.bIsUnderground;

				OutSurfaceData.SurfacePoints.Add(Point);
				bFoundColumnTop = true;
			}
		}
	}

	OutSurfaceData.SurfaceAreaEstimate = OutSurfaceData.SurfacePoints.Num() * VoxelSize * VoxelSize;
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

FScatterExtractionContext UVoxelScatterManager::MakeExtractionContext() const
{
	FScatterExtractionContext Context;
	Context.WorldMode = WorldMode;
	if (Configuration)
	{
		Context.NoiseParams = Configuration->NoiseParams;
		Context.bEnableWaterLevel = Configuration->bEnableWaterLevel;
		Context.WaterLevel = Configuration->WaterLevel;
	}
	return Context;
}

void UVoxelScatterManager::CreateDefaultDefinitions()
{
	const bool bCubicMode = Configuration && Configuration->MeshingMode == EMeshingMode::Cubic;

	if (bCubicMode)
	{
		// ==================== Cubic Mode Defaults ====================

		// Cubic Grass - cross-billboard on grass blocks
		FScatterDefinition CubicGrass;
		CubicGrass.ScatterID = 100;
		CubicGrass.Name = TEXT("CubicGrass");
		CubicGrass.DebugColor = FColor::Green;
		CubicGrass.DebugSphereRadius = 8.0f;
		CubicGrass.bEnabled = true;
		CubicGrass.Density = 0.4f;
		CubicGrass.MinSlopeDegrees = 0.0f;
		CubicGrass.MaxSlopeDegrees = 10.0f;
		CubicGrass.AllowedMaterials = { EVoxelMaterial::Grass };
		CubicGrass.bTopFacesOnly = true;
		CubicGrass.ScaleRange = FVector2D(0.7f, 1.2f);
		CubicGrass.bRandomYawRotation = true;
		CubicGrass.bAlignToSurfaceNormal = false;
		CubicGrass.SurfaceOffset = 0.0f;
		CubicGrass.PositionJitter = 0.0f; // No jitter for block-snapped
		CubicGrass.MeshType = EScatterMeshType::CrossBillboard;
		CubicGrass.PlacementMode = EScatterPlacementMode::BlockFaceSnap;
		CubicGrass.BillboardWidth = 80.0f;
		CubicGrass.BillboardHeight = 80.0f;
		CubicGrass.bUseBillboardAtlas = true;
		CubicGrass.BillboardAtlasColumn = 0;
		CubicGrass.BillboardAtlasRow = 0;
		CubicGrass.BillboardAtlasColumns = 4;
		CubicGrass.BillboardAtlasRows = 4;
		CubicGrass.LODStartDistance = 3000.0f;
		CubicGrass.CullDistance = 5000.0f;
		CubicGrass.MinScreenSize = 0.005f;
		CubicGrass.bCastShadows = false;
		AddScatterDefinition(CubicGrass);

		// Cubic Flowers - cross-billboard, sparser
		FScatterDefinition CubicFlowers;
		CubicFlowers.ScatterID = 101;
		CubicFlowers.Name = TEXT("CubicFlowers");
		CubicFlowers.DebugColor = FColor::Yellow;
		CubicFlowers.DebugSphereRadius = 6.0f;
		CubicFlowers.bEnabled = true;
		CubicFlowers.Density = 0.08f;
		CubicFlowers.MinSlopeDegrees = 0.0f;
		CubicFlowers.MaxSlopeDegrees = 10.0f;
		CubicFlowers.AllowedMaterials = { EVoxelMaterial::Grass };
		CubicFlowers.bTopFacesOnly = true;
		CubicFlowers.ScaleRange = FVector2D(0.6f, 1.0f);
		CubicFlowers.bRandomYawRotation = true;
		CubicFlowers.bAlignToSurfaceNormal = false;
		CubicFlowers.SurfaceOffset = 0.0f;
		CubicFlowers.PositionJitter = 0.0f;
		CubicFlowers.MeshType = EScatterMeshType::CrossBillboard;
		CubicFlowers.PlacementMode = EScatterPlacementMode::BlockFaceSnap;
		CubicFlowers.BillboardWidth = 60.0f;
		CubicFlowers.BillboardHeight = 60.0f;
		CubicFlowers.bUseBillboardAtlas = true;
		CubicFlowers.BillboardAtlasColumn = 1;
		CubicFlowers.BillboardAtlasRow = 0;
		CubicFlowers.BillboardAtlasColumns = 4;
		CubicFlowers.BillboardAtlasRows = 4;
		CubicFlowers.LODStartDistance = 2000.0f;
		CubicFlowers.CullDistance = 4000.0f;
		CubicFlowers.MinScreenSize = 0.005f;
		CubicFlowers.bCastShadows = false;
		AddScatterDefinition(CubicFlowers);

		// Cubic Rocks - static mesh on stone/dirt
		FScatterDefinition CubicRocks;
		CubicRocks.ScatterID = 102;
		CubicRocks.Name = TEXT("CubicRocks");
		CubicRocks.DebugColor = FColor(128, 128, 128);
		CubicRocks.DebugSphereRadius = 12.0f;
		CubicRocks.bEnabled = true;
		CubicRocks.Density = 0.05f;
		CubicRocks.MinSlopeDegrees = 0.0f;
		CubicRocks.MaxSlopeDegrees = 45.0f;
		CubicRocks.AllowedMaterials = { EVoxelMaterial::Stone, EVoxelMaterial::Dirt };
		CubicRocks.bTopFacesOnly = true;
		CubicRocks.ScaleRange = FVector2D(0.5f, 1.5f);
		CubicRocks.bRandomYawRotation = true;
		CubicRocks.bAlignToSurfaceNormal = false;
		CubicRocks.SurfaceOffset = 0.0f;
		CubicRocks.PositionJitter = 0.0f;
		CubicRocks.MeshType = EScatterMeshType::StaticMesh;
		CubicRocks.PlacementMode = EScatterPlacementMode::BlockFaceSnap;
		CubicRocks.LODStartDistance = 8000.0f;
		CubicRocks.CullDistance = 20000.0f;
		CubicRocks.MinScreenSize = 0.002f;
		CubicRocks.bCastShadows = true;
		AddScatterDefinition(CubicRocks);

		// HISM Block Trees - used in HISM or Both mode (VoxelInjection type for tree mode filtering)
		// User should assign a block-style tree mesh in ScatterConfiguration
		FScatterDefinition HISMTree;
		HISMTree.ScatterID = 10;
		HISMTree.Name = TEXT("HISMBlockTree");
		HISMTree.DebugColor = FColor(34, 139, 34);
		HISMTree.DebugSphereRadius = 25.0f;
		HISMTree.bEnabled = true;
		HISMTree.Density = 0.02f;
		HISMTree.MinSlopeDegrees = 0.0f;
		HISMTree.MaxSlopeDegrees = 20.0f;
		HISMTree.AllowedMaterials = { EVoxelMaterial::Grass };
		HISMTree.bTopFacesOnly = true;
		HISMTree.ScaleRange = FVector2D(0.8f, 1.3f);
		HISMTree.bRandomYawRotation = true;
		HISMTree.bAlignToSurfaceNormal = false;
		HISMTree.SurfaceOffset = 0.0f;
		HISMTree.PositionJitter = 0.0f;
		HISMTree.MeshType = EScatterMeshType::VoxelInjection; // Filtered by TreeMode
		HISMTree.PlacementMode = EScatterPlacementMode::BlockFaceSnap;
		HISMTree.TreeTemplateID = 0;
		HISMTree.LODStartDistance = 15000.0f;
		HISMTree.CullDistance = 50000.0f;
		HISMTree.MinScreenSize = 0.001f;
		HISMTree.bCastShadows = true;
		HISMTree.SpawnDistance = 20000.0f;
		AddScatterDefinition(HISMTree);
	}
	else
	{
		// ==================== Smooth Mode Defaults ====================

		// Grass scatter - dense on grass material
		FScatterDefinition GrassScatter;
		GrassScatter.ScatterID = 0;
		GrassScatter.Name = TEXT("Grass");
		GrassScatter.DebugColor = FColor::Green;
		GrassScatter.DebugSphereRadius = 8.0f;
		GrassScatter.bEnabled = true;
		GrassScatter.Density = 0.5f;
		GrassScatter.MinSlopeDegrees = 0.0f;
		GrassScatter.MaxSlopeDegrees = 30.0f;
		GrassScatter.AllowedMaterials = { EVoxelMaterial::Grass };
		GrassScatter.bTopFacesOnly = true;
		GrassScatter.ScaleRange = FVector2D(0.7f, 1.3f);
		GrassScatter.bRandomYawRotation = true;
		GrassScatter.bAlignToSurfaceNormal = true;
		GrassScatter.SurfaceOffset = 0.0f;
		GrassScatter.PositionJitter = 25.0f;
		GrassScatter.LODStartDistance = 3000.0f;
		GrassScatter.CullDistance = 8000.0f;
		GrassScatter.MinScreenSize = 0.005f;
		GrassScatter.bCastShadows = false;
		AddScatterDefinition(GrassScatter);

		// Rock scatter - less dense, on stone and dirt
		FScatterDefinition RockScatter;
		RockScatter.ScatterID = 1;
		RockScatter.Name = TEXT("Rocks");
		RockScatter.DebugColor = FColor(128, 128, 128);
		RockScatter.DebugSphereRadius = 15.0f;
		RockScatter.bEnabled = true;
		RockScatter.Density = 0.05f;
		RockScatter.MinSlopeDegrees = 0.0f;
		RockScatter.MaxSlopeDegrees = 60.0f;
		RockScatter.AllowedMaterials = { EVoxelMaterial::Stone, EVoxelMaterial::Dirt };
		RockScatter.bTopFacesOnly = false;
		RockScatter.ScaleRange = FVector2D(0.5f, 2.0f);
		RockScatter.bRandomYawRotation = true;
		RockScatter.bAlignToSurfaceNormal = false;
		RockScatter.SurfaceOffset = 0.0f;
		RockScatter.PositionJitter = 50.0f;
		RockScatter.LODStartDistance = 8000.0f;
		RockScatter.CullDistance = 20000.0f;
		RockScatter.MinScreenSize = 0.002f;
		RockScatter.bCastShadows = true;
		AddScatterDefinition(RockScatter);

		// Tree scatter - very sparse on grass
		FScatterDefinition TreeScatter;
		TreeScatter.ScatterID = 2;
		TreeScatter.Name = TEXT("Trees");
		TreeScatter.DebugColor = FColor(34, 139, 34);
		TreeScatter.DebugSphereRadius = 25.0f;
		TreeScatter.bEnabled = true;
		TreeScatter.Density = 0.02f;
		TreeScatter.MinSlopeDegrees = 0.0f;
		TreeScatter.MaxSlopeDegrees = 20.0f;
		TreeScatter.AllowedMaterials = { EVoxelMaterial::Grass };
		TreeScatter.bTopFacesOnly = true;
		TreeScatter.ScaleRange = FVector2D(0.8f, 1.5f);
		TreeScatter.bRandomYawRotation = true;
		TreeScatter.bAlignToSurfaceNormal = false;
		TreeScatter.SurfaceOffset = 0.0f;
		TreeScatter.PositionJitter = 100.0f;
		TreeScatter.LODStartDistance = 15000.0f;
		TreeScatter.CullDistance = 50000.0f;
		TreeScatter.MinScreenSize = 0.001f;
		TreeScatter.bCastShadows = true;
		TreeScatter.SpawnDistance = 20000.0f;
		AddScatterDefinition(TreeScatter);
	}

	UE_LOG(LogVoxelScatter, Log, TEXT("Created %d default scatter definitions (mode: %s)"),
		ScatterDefinitions.Num(), bCubicMode ? TEXT("Cubic") : TEXT("Smooth"));
}
