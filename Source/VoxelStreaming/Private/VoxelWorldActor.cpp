// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWorldActor.h"
#include "VoxelStreaming.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelMaterialAtlas.h"
#include "DistanceBandLODStrategy.h"
#include "VoxelMeshRenderer.h"
#include "Engine/World.h"

AVoxelWorldActor::AVoxelWorldActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	// Create chunk manager component
	ChunkManager = CreateDefaultSubobject<UVoxelChunkManager>(TEXT("ChunkManager"));
}

void AVoxelWorldActor::BeginPlay()
{
	Super::BeginPlay();

	InitializeVoxelWorld();
}

void AVoxelWorldActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ShutdownVoxelWorld();

	Super::EndPlay(EndPlayReason);
}

void AVoxelWorldActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bIsVoxelWorldInitialized)
	{
		return;
	}

	// Debug visualization
	if (bDrawDebugVisualization && ChunkManager)
	{
		ChunkManager->DrawDebugVisualization();
	}

	// Periodic debug stats printing
	if (DebugStatsPrintInterval > 0.0f)
	{
		DebugStatsTimer += DeltaSeconds;
		if (DebugStatsTimer >= DebugStatsPrintInterval)
		{
			DebugStatsTimer = 0.0f;
			PrintDebugStats();
		}
	}
}

void AVoxelWorldActor::InitializeVoxelWorld()
{
	if (bIsVoxelWorldInitialized)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("VoxelWorldActor: Already initialized"));
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogVoxelStreaming, Error, TEXT("VoxelWorldActor: No world available"));
		return;
	}

	// A runtime override (-VoxelConfig= or voxel.World.ConfigOverride) replaces the placed config, so a
	// benchmark or review can switch world mode or meshing mode without editing the map. It is
	// assigned to the property (not just used locally) because the rest of this actor reads it.
	const FString OverridePath = UVoxelWorldConfiguration::GetOverridePath();
	if (!OverridePath.IsEmpty())
	{
		if (UVoxelWorldConfiguration* Override = LoadObject<UVoxelWorldConfiguration>(nullptr, *OverridePath))
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("VoxelWorldActor: configuration override %s replaces %s"),
				*Override->GetPathName(), Configuration ? *Configuration->GetPathName() : TEXT("(none)"));
			Configuration = Override;
		}
		else
		{
			UE_LOG(LogVoxelStreaming, Error, TEXT("VoxelWorldActor: configuration override '%s' did not load; keeping %s"),
				*OverridePath, Configuration ? *Configuration->GetPathName() : TEXT("(none)"));
		}
	}

	// Get or create configuration
	UVoxelWorldConfiguration* Config = Configuration;
	if (!Config)
	{
		Config = CreateDefaultConfiguration();
		RuntimeConfiguration = Config;
		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldActor: Using runtime config (no asset), ViewDistance=%.0f"),
			Config->ViewDistance);
	}
	else
	{
		// Clamp LOD bands to ViewDistance limit
		const float MaxViewDist = Config->ViewDistance;
		for (FLODBand& Band : Config->LODBands)
		{
			if (Band.MaxDistance > MaxViewDist)
			{
				Band.MaxDistance = MaxViewDist;
			}
			if (Band.MinDistance > MaxViewDist)
			{
				Band.MinDistance = MaxViewDist;
			}
		}
		// Remove bands that are entirely beyond ViewDistance
		Config->LODBands.RemoveAll([MaxViewDist](const FLODBand& Band)
		{
			return Band.MinDistance >= MaxViewDist;
		});

		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldActor: Using Configuration asset, ViewDistance=%.0f clamped LOD bands to %d"),
			MaxViewDist, Config->LODBands.Num());
	}

	if (!Config)
	{
		UE_LOG(LogVoxelStreaming, Error, TEXT("VoxelWorldActor: Failed to create configuration"));
		return;
	}

	// Create LOD strategy
	FDistanceBandLODStrategy* DistanceBandStrategy = new FDistanceBandLODStrategy();
	LODStrategy = DistanceBandStrategy;

	// Create the mesh renderer
	FVoxelMeshRenderer* CustomVFRenderer = new FVoxelMeshRenderer();

	// Set material BEFORE Initialize - REQUIRED for Custom VF renderer
	// The scene proxy is created during Initialize, so material must be set first
	if (VoxelMaterial)
	{
		CustomVFRenderer->SetMaterial(VoxelMaterial);
		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldActor: Using material '%s'"), *VoxelMaterial->GetName());
	}
	else
	{
		UE_LOG(LogVoxelStreaming, Warning,
			TEXT("VoxelWorldActor: No VoxelMaterial assigned! Custom VF renderer requires a custom material. ")
			TEXT("Create a simple opaque material and assign it to the VoxelMaterial property."));
	}

	CustomVFRenderer->Initialize(World, Config);

	// Configure material atlas for face variants and texture lookup
	if (MaterialAtlas)
	{
		CustomVFRenderer->SetMaterialAtlas(MaterialAtlas);
		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldActor: Material atlas configured with %d materials"),
			MaterialAtlas->GetMaterialCount());
	}
	else
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldActor: No Material Atlas assigned. ")
			TEXT("Face variants and LUT-based texture lookup disabled."));
	}

	MeshRenderer = CustomVFRenderer;
	UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldActor: Using Custom Vertex Factory renderer (GPU-driven)"));

	// Initialize chunk manager
	if (ChunkManager)
	{
		ChunkManager->Initialize(Config, LODStrategy, MeshRenderer);
		ChunkManager->SetStreamingEnabled(true);
	}

	bIsVoxelWorldInitialized = true;

	UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldActor: Initialized successfully"));
	UE_LOG(LogVoxelStreaming, Log, TEXT("  VoxelSize: %.1f, ChunkSize: %d"), Config->VoxelSize, Config->ChunkSize);
	UE_LOG(LogVoxelStreaming, Log, TEXT("  ViewDistance: %.1f, SeaLevel: %.1f, HeightScale: %.1f"),
		Config->ViewDistance, Config->SeaLevel, Config->HeightScale);

	// Log LOD bands to verify halved values are applied
	UE_LOG(LogVoxelStreaming, Warning, TEXT("VoxelWorldActor: LOD Bands configured:"));
	for (int32 i = 0; i < Config->LODBands.Num(); ++i)
	{
		const FLODBand& Band = Config->LODBands[i];
		UE_LOG(LogVoxelStreaming, Warning, TEXT("  Band %d: LOD%d, %.0f-%.0f, stride=%d"),
			i, Band.LODLevel, Band.MinDistance, Band.MaxDistance, Band.VoxelStride);
	}

	// Log spawn position for spherical planet mode
	if (Config->WorldMode == EWorldMode::SphericalPlanet)
	{
		// The terrain along the spawn direction (SP-B11), once the chunk manager has its world mode.
		const FVector SpawnPos = GetSpawnPosition();
		static const TCHAR* SpawnLocationNames[] = { TEXT("+X Equator"), TEXT("+Y Equator"), TEXT("+Z North Pole"), TEXT("-Z South Pole") };
		const int32 SpawnLocIdx = FMath::Clamp(Config->PlanetSpawnLocation, 0, 3);

		UE_LOG(LogVoxelStreaming, Warning, TEXT("VoxelWorldActor: Spherical Planet Mode"));
		UE_LOG(LogVoxelStreaming, Warning, TEXT("  Planet Radius: %.0f, Max Height: %.0f, Max Depth: %.0f"),
			Config->WorldRadius, Config->PlanetMaxTerrainHeight, Config->PlanetMaxTerrainDepth);
		UE_LOG(LogVoxelStreaming, Warning, TEXT("  Spawn Location: %s, Altitude: %.0f"),
			SpawnLocationNames[SpawnLocIdx], Config->PlanetSpawnAltitude);
		UE_LOG(LogVoxelStreaming, Warning, TEXT("  Recommended Spawn Position: (%.0f, %.0f, %.0f)"),
			SpawnPos.X, SpawnPos.Y, SpawnPos.Z);
		UE_LOG(LogVoxelStreaming, Warning, TEXT("  Place PlayerStart at this position or call GetSpawnPosition()"));
	}

}

void AVoxelWorldActor::ShutdownVoxelWorld()
{
	if (!bIsVoxelWorldInitialized)
	{
		return;
	}

	// Shutdown chunk manager (this also cleans up LOD strategy)
	if (ChunkManager)
	{
		ChunkManager->Shutdown();
	}

	// LODStrategy is deleted by ChunkManager::Shutdown()
	LODStrategy = nullptr;

	// Clean up renderer (we own it)
	if (MeshRenderer)
	{
		MeshRenderer->Shutdown();
		delete MeshRenderer;
		MeshRenderer = nullptr;
	}

	// Clear runtime config
	RuntimeConfiguration = nullptr;

	bIsVoxelWorldInitialized = false;

	UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldActor: Shutdown complete"));
}

UVoxelWorldConfiguration* AVoxelWorldActor::CreateDefaultConfiguration()
{
	// Create a transient configuration object
	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>(
		this,
		UVoxelWorldConfiguration::StaticClass(),
		TEXT("RuntimeVoxelConfig"),
		RF_Transient
	);

	if (!Config)
	{
		return nullptr;
	}

	// Apply settings from actor properties
	Config->VoxelSize = VoxelSize;
	Config->ChunkSize = ChunkSize;
	Config->ViewDistance = ViewDistance;
	Config->SeaLevel = SeaLevel;
	Config->HeightScale = HeightScale;
	Config->BaseHeight = 0.0f;

	// World settings
	Config->WorldMode = EWorldMode::InfinitePlane;
	Config->MeshingMode = EMeshingMode::Cubic;
	Config->WorldOrigin = GetActorLocation();

	// Noise parameters - reasonable defaults for terrain
	Config->NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	Config->NoiseParams.Seed = 12345;
	Config->NoiseParams.Frequency = 0.001f;  // Low frequency for large features
	Config->NoiseParams.Octaves = 4;
	Config->NoiseParams.Lacunarity = 2.0f;
	Config->NoiseParams.Persistence = 0.5f;
	Config->NoiseParams.Amplitude = 1.0f;

	// LOD bands matched to ViewDistance (default 10000)
	// ChunkSize=32, VoxelSize=100 -> 3200 units per chunk
	Config->LODBands.Empty();

	FLODBand Band0;
	Band0.LODLevel = 0;
	Band0.MinDistance = 0.0f;
	Band0.MaxDistance = 4000.0f;
	Band0.VoxelStride = 1;
	Config->LODBands.Add(Band0);

	FLODBand Band1;
	Band1.LODLevel = 1;
	Band1.MinDistance = 4000.0f;
	Band1.MaxDistance = 7000.0f;
	Band1.VoxelStride = 2;
	Config->LODBands.Add(Band1);

	FLODBand Band2;
	Band2.LODLevel = 2;
	Band2.MinDistance = 7000.0f;
	Band2.MaxDistance = 10000.0f;
	Band2.VoxelStride = 4;
	Config->LODBands.Add(Band2);

	// Streaming settings - balanced for ViewDistance=10000 (~500 chunks)
	// Lower MaxChunksToLoadPerFrame reduces stuttering during movement
	Config->MaxChunksToLoadPerFrame = 2;
	Config->MaxChunksToUnloadPerFrame = 8;
	Config->StreamingTimeSliceMS = 3.0f;

	// Rendering settings
	Config->bGenerateCollision = true;
	Config->bEnableFrustumCulling = true;

	return Config;
}

void AVoxelWorldActor::PrintDebugStats()
{
	if (!ChunkManager)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldActor: ChunkManager not available"));
		return;
	}

	FString Stats = ChunkManager->GetDebugStats();

	// Split into lines and log each
	TArray<FString> Lines;
	Stats.ParseIntoArrayLines(Lines);

	for (const FString& Line : Lines)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("%s"), *Line);
	}
}

void AVoxelWorldActor::ForceStreamingUpdate()
{
	if (ChunkManager)
	{
		ChunkManager->ForceStreamingUpdate();
	}
}

FVector AVoxelWorldActor::GetSpawnPosition() const
{
	UVoxelWorldConfiguration* Config = GetActiveConfiguration();
	if (Config && Config->WorldMode == EWorldMode::SphericalPlanet)
	{
		// The terrain along the spawn direction plus PlanetSpawnAltitude (SP-B11); the datum's point before the world starts.
		return (ChunkManager && ChunkManager->IsInitialized()) ? ChunkManager->GetSpawnPosition() : Config->GetPlanetSpawnPosition();
	}

	// For non-spherical modes, return the world origin (actor location)
	return Config ? Config->WorldOrigin : GetActorLocation();
}

UVoxelWorldConfiguration* AVoxelWorldActor::GetActiveConfiguration() const
{
	return Configuration ? Configuration.Get() : RuntimeConfiguration.Get();
}
