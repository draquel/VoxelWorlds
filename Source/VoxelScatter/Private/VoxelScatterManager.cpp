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

	// Clear all cached data
	SurfaceDataCache.Empty();
	ScatterDataCache.Empty();
	ScatterDefinitions.Empty();

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

	// Tick the scatter renderer to flush pending HISM rebuilds
	// This batches all chunk updates from this frame into single rebuilds per scatter type
	if (ScatterRenderer && ScatterRenderer->IsInitialized())
	{
		ScatterRenderer->Tick();
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

	GenerateChunkScatter(ChunkCoord, MeshData);
}

void UVoxelScatterManager::OnChunkUnloaded(const FIntVector& ChunkCoord)
{
	RemoveChunkScatter(ChunkCoord);
}

void UVoxelScatterManager::RegenerateChunkScatter(const FIntVector& ChunkCoord)
{
	if (!bIsInitialized)
	{
		return;
	}

	// Remove existing data
	RemoveChunkScatter(ChunkCoord);

	// Regeneration requires new mesh data - the caller should provide it via OnChunkMeshDataReady
	UE_LOG(LogVoxelScatter, Verbose, TEXT("Regenerate scatter requested for chunk (%d,%d,%d) - awaiting new mesh data"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
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
	AddScatterDefinition(GrassScatter);

	// Rock scatter - less dense, on stone and dirt
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
	AddScatterDefinition(RockScatter);

	// Tree scatter - very sparse on grass
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
	AddScatterDefinition(TreeScatter);

	UE_LOG(LogVoxelScatter, Log, TEXT("Created %d default scatter definitions"), ScatterDefinitions.Num());
}
