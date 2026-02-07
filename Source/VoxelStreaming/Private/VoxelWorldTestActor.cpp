// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWorldTestActor.h"
#include "VoxelStreaming.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelMaterialAtlas.h"
#include "DistanceBandLODStrategy.h"
#include "VoxelPMCRenderer.h"
#include "VoxelCustomVFRenderer.h"
#include "VoxelCPUSmoothMesher.h"
#include "VoxelEditManager.h"
#include "VoxelEditTypes.h"
#include "VoxelCollisionManager.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "DrawDebugHelpers.h"
#include "Misc/Paths.h"
#include "GameFramework/PlayerController.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"

AVoxelWorldTestActor::AVoxelWorldTestActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	// Create chunk manager component
	ChunkManager = CreateDefaultSubobject<UVoxelChunkManager>(TEXT("ChunkManager"));
}

void AVoxelWorldTestActor::BeginPlay()
{
	Super::BeginPlay();

	InitializeVoxelWorld();
}

void AVoxelWorldTestActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ShutdownVoxelWorld();

	Super::EndPlay(EndPlayReason);
}

void AVoxelWorldTestActor::Tick(float DeltaSeconds)
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

	// Sync Transvoxel debug flags to mesher each tick (allows runtime toggling)
	static bool bWasDebuggingEnabled = false;
	const bool bDebuggingEnabled = bDebugLogTransitionCells || bDrawTransitionCellDebug;

	if (FVoxelCPUSmoothMesher* SmoothMesher = ChunkManager ? ChunkManager->GetSmoothMesher() : nullptr)
	{
		SmoothMesher->SetDebugLogging(bDebugLogTransitionCells);
		SmoothMesher->SetDebugVisualization(bDrawTransitionCellDebug);

		// Clear debug data when debugging is just enabled (fresh start)
		if (bDebuggingEnabled && !bWasDebuggingEnabled)
		{
			SmoothMesher->ClearDebugData();
			UE_LOG(LogVoxelStreaming, Warning, TEXT("Transvoxel debugging enabled - cleared debug data for fresh start"));
		}
	}
	bWasDebuggingEnabled = bDebuggingEnabled;

	// Transvoxel debug visualization
	if (bDrawTransitionCellDebug)
	{
		DrawTransitionCellDebug();
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

	// Process edit inputs if enabled
	if (bEnableEditInputs)
	{
		ProcessEditInputs();
	}

	// Draw edit crosshair (can be enabled independently of edit inputs)
	if (bShowEditCrosshair)
	{
		DrawEditCrosshair();
	}
}

void AVoxelWorldTestActor::InitializeVoxelWorld()
{
	if (bIsVoxelWorldInitialized)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("VoxelWorldTestActor: Already initialized"));
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogVoxelStreaming, Error, TEXT("VoxelWorldTestActor: No world available"));
		return;
	}

	// Get or create configuration
	UVoxelWorldConfiguration* Config = Configuration;
	if (!Config)
	{
		Config = CreateDefaultConfiguration();
		RuntimeConfiguration = Config;
		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: Using runtime config (no asset), ViewDistance=%.0f"),
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

		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: Using Configuration asset, ViewDistance=%.0f clamped LOD bands to %d"),
			MaxViewDist, Config->LODBands.Num());
	}

	if (!Config)
	{
		UE_LOG(LogVoxelStreaming, Error, TEXT("VoxelWorldTestActor: Failed to create configuration"));
		return;
	}

	// Create LOD strategy
	FDistanceBandLODStrategy* DistanceBandStrategy = new FDistanceBandLODStrategy();
	LODStrategy = DistanceBandStrategy;

	// Create mesh renderer based on configuration
	if (Config->bUseGPURenderer)
	{
		// Use GPU-driven Custom Vertex Factory renderer
		FVoxelCustomVFRenderer* CustomVFRenderer = new FVoxelCustomVFRenderer();

		// Set material BEFORE Initialize - REQUIRED for Custom VF renderer
		// The scene proxy is created during Initialize, so material must be set first
		if (VoxelMaterial)
		{
			CustomVFRenderer->SetMaterial(VoxelMaterial);
			UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: Using material '%s'"), *VoxelMaterial->GetName());
		}
		else
		{
			UE_LOG(LogVoxelStreaming, Warning,
				TEXT("VoxelWorldTestActor: No VoxelMaterial assigned! Custom VF renderer requires a custom material. ")
				TEXT("Create a simple opaque material and assign it to the VoxelMaterial property."));
		}

		CustomVFRenderer->Initialize(World, Config);

		// Configure LOD material parameters (after Initialize creates the WorldComponent)
		if (Config->LODParameterCollection)
		{
			CustomVFRenderer->SetLODParameterCollection(Config->LODParameterCollection);

			const float LODStart = Config->GetMaterialLODStartDistance();
			const float LODEnd = Config->GetMaterialLODEndDistance();
			CustomVFRenderer->SetLODTransitionDistances(LODStart, LODEnd);

			UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: LOD MPC configured (Start=%.0f, End=%.0f, derived from LODBands)"),
				LODStart, LODEnd);
		}
		else
		{
			UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: No LOD Parameter Collection assigned in Configuration. ")
				TEXT("Material-based LOD morphing disabled."));
		}

		// Configure material atlas for face variants and texture lookup
		if (MaterialAtlas)
		{
			CustomVFRenderer->SetMaterialAtlas(MaterialAtlas);
			UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: Material atlas configured with %d materials"),
				MaterialAtlas->GetMaterialCount());
		}
		else
		{
			UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: No Material Atlas assigned. ")
				TEXT("Face variants and LUT-based texture lookup disabled."));
		}

		MeshRenderer = CustomVFRenderer;
		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: Using Custom Vertex Factory renderer (GPU-driven)"));
	}
	else
	{
		// Use PMC fallback renderer
		FVoxelPMCRenderer* PMCRenderer = new FVoxelPMCRenderer();

		// Set material BEFORE Initialize (similar to CustomVF path)
		if (VoxelMaterial)
		{
			PMCRenderer->SetMaterial(VoxelMaterial);
			UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: PMC using material '%s'"), *VoxelMaterial->GetName());
		}
		else
		{
			UE_LOG(LogVoxelStreaming, Warning,
				TEXT("VoxelWorldTestActor: No VoxelMaterial assigned for PMC renderer. Using default vertex color material."));
		}

		PMCRenderer->Initialize(World, Config);

		// Configure material atlas for face variants and texture lookup
		if (MaterialAtlas)
		{
			PMCRenderer->SetMaterialAtlas(MaterialAtlas);
			UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: PMC material atlas configured with %d materials"),
				MaterialAtlas->GetMaterialCount());
		}
		else
		{
			UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: No Material Atlas assigned for PMC. ")
				TEXT("Face variants and LUT-based texture lookup disabled."));
		}

		MeshRenderer = PMCRenderer;
		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: Using PMC renderer (CPU fallback)"));
	}

	// Initialize chunk manager
	if (ChunkManager)
	{
		ChunkManager->Initialize(Config, LODStrategy, MeshRenderer);
		ChunkManager->SetStreamingEnabled(true);
	}

	bIsVoxelWorldInitialized = true;

	UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: Initialized successfully"));
	UE_LOG(LogVoxelStreaming, Log, TEXT("  VoxelSize: %.1f, ChunkSize: %d"), Config->VoxelSize, Config->ChunkSize);
	UE_LOG(LogVoxelStreaming, Log, TEXT("  ViewDistance: %.1f, SeaLevel: %.1f, HeightScale: %.1f"),
		Config->ViewDistance, Config->SeaLevel, Config->HeightScale);

	// Log LOD bands to verify halved values are applied
	UE_LOG(LogVoxelStreaming, Warning, TEXT("VoxelWorldTestActor: LOD Bands configured:"));
	for (int32 i = 0; i < Config->LODBands.Num(); ++i)
	{
		const FLODBand& Band = Config->LODBands[i];
		UE_LOG(LogVoxelStreaming, Warning, TEXT("  Band %d: LOD%d, %.0f-%.0f, stride=%d"),
			i, Band.LODLevel, Band.MinDistance, Band.MaxDistance, Band.VoxelStride);
	}

	// Log spawn position for spherical planet mode
	if (Config->WorldMode == EWorldMode::SphericalPlanet)
	{
		const FVector SpawnPos = Config->GetPlanetSpawnPosition();
		static const TCHAR* SpawnLocationNames[] = { TEXT("+X Equator"), TEXT("+Y Equator"), TEXT("+Z North Pole"), TEXT("-Z South Pole") };
		const int32 SpawnLocIdx = FMath::Clamp(Config->PlanetSpawnLocation, 0, 3);

		UE_LOG(LogVoxelStreaming, Warning, TEXT("VoxelWorldTestActor: Spherical Planet Mode"));
		UE_LOG(LogVoxelStreaming, Warning, TEXT("  Planet Radius: %.0f, Max Height: %.0f, Max Depth: %.0f"),
			Config->WorldRadius, Config->PlanetMaxTerrainHeight, Config->PlanetMaxTerrainDepth);
		UE_LOG(LogVoxelStreaming, Warning, TEXT("  Spawn Location: %s, Altitude: %.0f"),
			SpawnLocationNames[SpawnLocIdx], Config->PlanetSpawnAltitude);
		UE_LOG(LogVoxelStreaming, Warning, TEXT("  Recommended Spawn Position: (%.0f, %.0f, %.0f)"),
			SpawnPos.X, SpawnPos.Y, SpawnPos.Z);
		UE_LOG(LogVoxelStreaming, Warning, TEXT("  Place PlayerStart at this position or call GetPlanetSpawnPosition()"));
	}

	// Propagate debug flags to mesher if enabled
	if (bDebugLogTransitionCells || bDrawTransitionCellDebug)
	{
		SetTransitionCellDebugging(true);
		UE_LOG(LogVoxelStreaming, Warning, TEXT("VoxelWorldTestActor: Transvoxel debugging ENABLED (Log=%s, Viz=%s)"),
			bDebugLogTransitionCells ? TEXT("Yes") : TEXT("No"),
			bDrawTransitionCellDebug ? TEXT("Yes") : TEXT("No"));
	}

	// Create water plane visualization if enabled
	UpdateWaterVisualization();
}

void AVoxelWorldTestActor::ShutdownVoxelWorld()
{
	if (!bIsVoxelWorldInitialized)
	{
		return;
	}

	// Destroy water visualization
	DestroyWaterVisualization();

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

	UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: Shutdown complete"));
}

UVoxelWorldConfiguration* AVoxelWorldTestActor::CreateDefaultConfiguration()
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
	// MorphRange = 25% of band width for smooth transitions
	Config->LODBands.Empty();

	FLODBand Band0;
	Band0.LODLevel = 0;
	Band0.MinDistance = 0.0f;
	Band0.MaxDistance = 4000.0f;
	Band0.VoxelStride = 1;
	Band0.MorphRange = 1000.0f;
	Config->LODBands.Add(Band0);

	FLODBand Band1;
	Band1.LODLevel = 1;
	Band1.MinDistance = 4000.0f;
	Band1.MaxDistance = 7000.0f;
	Band1.VoxelStride = 2;
	Band1.MorphRange = 750.0f;
	Config->LODBands.Add(Band1);

	FLODBand Band2;
	Band2.LODLevel = 2;
	Band2.MinDistance = 7000.0f;
	Band2.MaxDistance = 10000.0f;
	Band2.VoxelStride = 4;
	Band2.MorphRange = 750.0f;
	Config->LODBands.Add(Band2);

	// Streaming settings - balanced for ViewDistance=10000 (~500 chunks)
	Config->MaxChunksToLoadPerFrame = 8;
	Config->MaxChunksToUnloadPerFrame = 16;
	Config->StreamingTimeSliceMS = 4.0f;
	Config->MaxLoadedChunks = 1000;

	// Rendering settings
	Config->bUseGPURenderer = true;  // Use Custom Vertex Factory renderer
	Config->bGenerateCollision = false;  // Disable for faster testing
	Config->bEnableLODMorphing = true;
	Config->bEnableFrustumCulling = true;

	return Config;
}

void AVoxelWorldTestActor::PrintDebugStats()
{
	if (!ChunkManager)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: ChunkManager not available"));
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

void AVoxelWorldTestActor::ForceStreamingUpdate()
{
	if (ChunkManager)
	{
		ChunkManager->ForceStreamingUpdate();
	}
}

FVector AVoxelWorldTestActor::GetPlanetSpawnPosition() const
{
	UVoxelWorldConfiguration* Config = Configuration ? Configuration.Get() : RuntimeConfiguration.Get();
	if (Config && Config->WorldMode == EWorldMode::SphericalPlanet)
	{
		return Config->GetPlanetSpawnPosition();
	}

	// For non-spherical modes, return the world origin (actor location)
	return Config ? Config->WorldOrigin : GetActorLocation();
}

void AVoxelWorldTestActor::SetTransitionCellDebugging(bool bEnable)
{
	// When called with bEnable, set both flags. Otherwise, this function
	// can be called internally to sync existing flag values to the mesher.
	if (bEnable)
	{
		bDebugLogTransitionCells = true;
		bDrawTransitionCellDebug = true;
	}

	// Set the debug flags on the mesher via the chunk manager
	if (ChunkManager)
	{
		if (FVoxelCPUSmoothMesher* SmoothMesher = ChunkManager->GetSmoothMesher())
		{
			SmoothMesher->SetDebugLogging(bDebugLogTransitionCells);
			SmoothMesher->SetDebugVisualization(bDrawTransitionCellDebug);

			UE_LOG(LogVoxelStreaming, Warning, TEXT("Transvoxel debug flags synced to mesher: Logging=%s, Visualization=%s"),
				bDebugLogTransitionCells ? TEXT("ON") : TEXT("OFF"),
				bDrawTransitionCellDebug ? TEXT("ON") : TEXT("OFF"));
		}
		else
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("SetTransitionCellDebugging: Smooth mesher not available (GetSmoothMesher returned nullptr)"));
		}
	}
}

void AVoxelWorldTestActor::DrawTransitionCellDebug()
{
	if (!bDrawTransitionCellDebug || !ChunkManager)
	{
		return;
	}

	FVoxelCPUSmoothMesher* SmoothMesher = ChunkManager->GetSmoothMesher();
	if (!SmoothMesher)
	{
		static bool bLoggedOnce = false;
		if (!bLoggedOnce)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("DrawTransitionCellDebug: SmoothMesher is nullptr"));
			bLoggedOnce = true;
		}
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const TArray<FVoxelCPUSmoothMesher::FTransitionCellDebugData>& DebugCells = SmoothMesher->GetTransitionCellDebugData();

	// Log debug cell count periodically
	static int32 FrameCounter = 0;
	if (++FrameCounter % 60 == 0)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("DrawTransitionCellDebug: %d transition cells in debug data"), DebugCells.Num());
	}

	// Face colors for visualization
	static const FColor FaceColors[6] = {
		FColor::Red,     // -X
		FColor::Green,   // +X
		FColor::Blue,    // -Y
		FColor::Yellow,  // +Y
		FColor::Cyan,    // -Z
		FColor::Magenta  // +Z
	};

	// Get chunk world offset
	UVoxelWorldConfiguration* Config = ChunkManager->GetConfiguration();
	const float ChunkWorldSize = Config ? Config->ChunkSize * Config->VoxelSize : 3200.0f;

	for (const auto& Cell : DebugCells)
	{
		// Calculate world position offset for this chunk
		const FVector ChunkWorldOffset = FVector(Cell.ChunkCoord) * ChunkWorldSize;
		const FColor FaceColor = (Cell.FaceIndex >= 0 && Cell.FaceIndex < 6) ? FaceColors[Cell.FaceIndex] : FColor::White;

		// Draw cell bounding box
		if (bShowTransitionCellBounds)
		{
			const FVector CellMin = ChunkWorldOffset + FVector(Cell.CellBasePos);
			const float CellSize = Cell.Stride * (Config ? Config->VoxelSize : 100.0f);
			const FVector CellMax = CellMin + FVector(CellSize, CellSize, CellSize);
			const FVector CellCenter = (CellMin + CellMax) * 0.5f;
			const FVector CellExtent = FVector(CellSize * 0.5f);

			DrawDebugBox(World, CellCenter, CellExtent, FaceColor, false, 0.0f, 0, 2.0f);

			// Draw face label
			FString Label = FString::Printf(TEXT("F%d C%d"), Cell.FaceIndex, Cell.CaseIndex);
			DrawDebugString(World, CellCenter + FVector(0, 0, CellSize * 0.6f), Label, nullptr, FaceColor, 0.0f, true);
		}

		// Draw sample points
		if (bShowTransitionSamplePoints && Cell.SamplePositions.Num() == 13)
		{
			for (int32 i = 0; i < 13; i++)
			{
				const FVector SamplePos = ChunkWorldOffset + FVector(Cell.SamplePositions[i]);
				const bool bInside = Cell.SampleDensities[i] >= 0.5f;
				const FColor SampleColor = bInside ? FColor::Green : FColor::Red;

				// Draw larger spheres for corner samples (0,2,6,8), smaller for others
				const bool bIsCorner = (i == 0 || i == 2 || i == 6 || i == 8);
				const float PointSize = bIsCorner ? DebugPointSize * 1.5f : DebugPointSize;

				DrawDebugSphere(World, SamplePos, PointSize, 8, SampleColor, false, 0.0f, 0, 1.0f);

				// Label with index and density
				if (bIsCorner || i == 4) // Label corners and center
				{
					FString SampleLabel = FString::Printf(TEXT("%d:%.2f"), i, Cell.SampleDensities[i]);
					DrawDebugString(World, SamplePos + FVector(0, 0, PointSize * 2.0f), SampleLabel, nullptr, FColor::White, 0.0f, true);
				}
			}
		}

		// Draw generated vertices
		if (bShowTransitionVertices)
		{
			for (int32 i = 0; i < Cell.GeneratedVertices.Num(); i++)
			{
				const FVector VertexPos = ChunkWorldOffset + FVector(Cell.GeneratedVertices[i]);

				// Vertices in bright yellow
				DrawDebugPoint(World, VertexPos, DebugPointSize * 2.0f, FColor::Yellow, false, 0.0f, 0);

				// Connect vertices with lines (for first few to show structure)
				if (i > 0)
				{
					const FVector PrevPos = ChunkWorldOffset + FVector(Cell.GeneratedVertices[i - 1]);
					DrawDebugLine(World, PrevPos, VertexPos, FColor::Orange, false, 0.0f, 0, 1.0f);
				}
			}
		}
	}

	// Draw summary in top-left
	if (DebugCells.Num() > 0)
	{
		UE_LOG(LogVoxelStreaming, Verbose, TEXT("Drawing %d transition cells debug visualization"), DebugCells.Num());
	}
}

void AVoxelWorldTestActor::UpdateWaterVisualization()
{
	// Get the active configuration
	UVoxelWorldConfiguration* Config = Configuration ? Configuration.Get() : RuntimeConfiguration.Get();
	if (!Config)
	{
		return;
	}

	// Check if water visualization should be shown
	const bool bShouldShowWater = Config->bEnableWaterLevel && Config->bShowWaterPlane;

	if (!bShouldShowWater)
	{
		DestroyWaterVisualization();
		return;
	}

	// Different visualization based on world mode
	if (Config->WorldMode == EWorldMode::SphericalPlanet)
	{
		// Destroy plane if it exists (switching modes)
		if (WaterPlaneMesh)
		{
			WaterPlaneMesh->DestroyComponent();
			WaterPlaneMesh = nullptr;
		}

		// Create the water sphere mesh component if it doesn't exist
		if (!WaterSphereMesh)
		{
			WaterSphereMesh = NewObject<UStaticMeshComponent>(this, TEXT("WaterSphereMesh"));
			if (WaterSphereMesh)
			{
				WaterSphereMesh->SetupAttachment(GetRootComponent());
				WaterSphereMesh->RegisterComponent();

				// Use the default sphere mesh from the engine
				UStaticMesh* SphereMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
				if (SphereMesh)
				{
					WaterSphereMesh->SetStaticMesh(SphereMesh);
				}

				// Disable collision for visualization
				WaterSphereMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

				// Set cast shadows to false
				WaterSphereMesh->SetCastShadow(false);

				UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: Created water sphere visualization component"));
			}
		}

		if (!WaterSphereMesh)
		{
			return;
		}

		// Position at planet center (WorldOrigin)
		WaterSphereMesh->SetWorldLocation(Config->WorldOrigin);

		// Scale to WaterRadius
		// The default sphere is 100 units diameter (50 unit radius), so scale = WaterRadius / 50
		const float Scale = Config->WaterRadius / 50.0f;
		WaterSphereMesh->SetWorldScale3D(FVector(Scale, Scale, Scale));

		// Set material
		if (WaterMaterial)
		{
			WaterSphereMesh->SetMaterial(0, WaterMaterial);
		}
		else
		{
			// Create a simple translucent blue material if none provided
			UMaterialInstanceDynamic* DefaultWaterMat = UMaterialInstanceDynamic::Create(
				WaterSphereMesh->GetMaterial(0), this);
			if (DefaultWaterMat)
			{
				DefaultWaterMat->SetVectorParameterValue(TEXT("BaseColor"), FLinearColor(0.1f, 0.3f, 0.6f, 0.5f));
				WaterSphereMesh->SetMaterial(0, DefaultWaterMat);
			}
		}

		WaterSphereMesh->SetVisibility(true);

		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: Water sphere at origin (%.0f, %.0f, %.0f), Radius=%.0f"),
			Config->WorldOrigin.X, Config->WorldOrigin.Y, Config->WorldOrigin.Z, Config->WaterRadius);
	}
	else
	{
		// Flat world modes (InfinitePlane, IslandBowl) - use plane

		// Destroy sphere if it exists (switching modes)
		if (WaterSphereMesh)
		{
			WaterSphereMesh->DestroyComponent();
			WaterSphereMesh = nullptr;
		}

		// Create the water plane mesh component if it doesn't exist
		if (!WaterPlaneMesh)
		{
			WaterPlaneMesh = NewObject<UStaticMeshComponent>(this, TEXT("WaterPlaneMesh"));
			if (WaterPlaneMesh)
			{
				WaterPlaneMesh->SetupAttachment(GetRootComponent());
				WaterPlaneMesh->RegisterComponent();

				// Use the default plane mesh from the engine
				UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
				if (PlaneMesh)
				{
					WaterPlaneMesh->SetStaticMesh(PlaneMesh);
				}

				// Disable collision for visualization
				WaterPlaneMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

				// Set cast shadows to false (water plane shouldn't cast shadows)
				WaterPlaneMesh->SetCastShadow(false);

				UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: Created water plane visualization component"));
			}
		}

		if (!WaterPlaneMesh)
		{
			return;
		}

		// Set position at water level (relative to WorldOrigin)
		FVector WaterPosition = Config->WorldOrigin;
		WaterPosition.Z = Config->WaterLevel;
		WaterPlaneMesh->SetWorldLocation(WaterPosition);

		// Scale based on ViewDistance and WaterPlaneScale
		// The default plane is 100x100 units, so we scale to cover ViewDistance
		const float PlaneSize = Config->ViewDistance * WaterPlaneScale;
		const float Scale = PlaneSize / 100.0f;  // Default plane is 100 units
		WaterPlaneMesh->SetWorldScale3D(FVector(Scale, Scale, 1.0f));

		// Set material
		if (WaterMaterial)
		{
			WaterPlaneMesh->SetMaterial(0, WaterMaterial);
		}
		else
		{
			// Create a simple translucent blue material if none provided
			UMaterialInstanceDynamic* DefaultWaterMat = UMaterialInstanceDynamic::Create(
				WaterPlaneMesh->GetMaterial(0), this);
			if (DefaultWaterMat)
			{
				// Try to set some basic parameters (depends on the base material supporting them)
				DefaultWaterMat->SetVectorParameterValue(TEXT("BaseColor"), FLinearColor(0.1f, 0.3f, 0.6f, 0.5f));
				WaterPlaneMesh->SetMaterial(0, DefaultWaterMat);
			}
		}

		WaterPlaneMesh->SetVisibility(true);

		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: Water plane at Z=%.0f, Scale=%.0f"),
			Config->WaterLevel, PlaneSize);
	}
}

void AVoxelWorldTestActor::DestroyWaterVisualization()
{
	if (WaterPlaneMesh)
	{
		WaterPlaneMesh->DestroyComponent();
		WaterPlaneMesh = nullptr;

		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: Destroyed water plane visualization"));
	}

	if (WaterSphereMesh)
	{
		WaterSphereMesh->DestroyComponent();
		WaterSphereMesh = nullptr;

		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: Destroyed water sphere visualization"));
	}
}

// ==================== Edit System Testing ====================

int32 AVoxelWorldTestActor::TestDigAt(FVector WorldLocation, float Radius)
{
	if (!ChunkManager)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("TestDigAt: ChunkManager not available"));
		return 0;
	}

	UVoxelEditManager* EditManager = ChunkManager->GetEditManager();
	if (!EditManager)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("TestDigAt: EditManager not available"));
		return 0;
	}

	// Configure brush for digging (subtract mode)
	FVoxelBrushParams Brush;
	Brush.Shape = EVoxelBrushShape::Sphere;
	Brush.Radius = Radius;
	Brush.Strength = 1.0f;
	Brush.FalloffType = EVoxelBrushFalloff::Smooth;
	Brush.DensityDelta = 100;  // Full subtraction

	// Apply the edit
	EditManager->BeginEditOperation(TEXT("Dig"));
	const int32 VoxelsModified = EditManager->ApplyBrushEdit(WorldLocation, Brush, EEditMode::Subtract);
	EditManager->EndEditOperation();

	UE_LOG(LogVoxelStreaming, Log, TEXT("TestDigAt: Dug at (%.0f, %.0f, %.0f) with radius %.0f - %d voxels modified"),
		WorldLocation.X, WorldLocation.Y, WorldLocation.Z, Radius, VoxelsModified);

	return VoxelsModified;
}

int32 AVoxelWorldTestActor::TestBuildAt(FVector WorldLocation, float Radius, uint8 MaterialID)
{
	if (!ChunkManager)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("TestBuildAt: ChunkManager not available"));
		return 0;
	}

	UVoxelEditManager* EditManager = ChunkManager->GetEditManager();
	if (!EditManager)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("TestBuildAt: EditManager not available"));
		return 0;
	}

	// Configure brush for building (add mode)
	FVoxelBrushParams Brush;
	Brush.Shape = EVoxelBrushShape::Sphere;
	Brush.Radius = Radius;
	Brush.Strength = 1.0f;
	Brush.FalloffType = EVoxelBrushFalloff::Smooth;
	Brush.MaterialID = MaterialID;
	Brush.DensityDelta = 100;  // Full addition

	// Apply the edit
	EditManager->BeginEditOperation(TEXT("Build"));
	const int32 VoxelsModified = EditManager->ApplyBrushEdit(WorldLocation, Brush, EEditMode::Add);
	EditManager->EndEditOperation();

	UE_LOG(LogVoxelStreaming, Log, TEXT("TestBuildAt: Built at (%.0f, %.0f, %.0f) with radius %.0f, material %d - %d voxels modified"),
		WorldLocation.X, WorldLocation.Y, WorldLocation.Z, Radius, MaterialID, VoxelsModified);

	return VoxelsModified;
}

int32 AVoxelWorldTestActor::TestPaintAt(FVector WorldLocation, float Radius, uint8 MaterialID)
{
	if (!ChunkManager)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("TestPaintAt: ChunkManager not available"));
		return 0;
	}

	UVoxelEditManager* EditManager = ChunkManager->GetEditManager();
	if (!EditManager)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("TestPaintAt: EditManager not available"));
		return 0;
	}

	// Configure brush for painting (paint mode - only changes material)
	FVoxelBrushParams Brush;
	Brush.Shape = EVoxelBrushShape::Sphere;
	Brush.Radius = Radius;
	Brush.Strength = 1.0f;
	Brush.FalloffType = EVoxelBrushFalloff::Smooth;
	Brush.MaterialID = MaterialID;
	Brush.DensityDelta = 0;  // Paint mode doesn't change density

	// Apply the edit
	EditManager->BeginEditOperation(TEXT("Paint"));
	const int32 VoxelsModified = EditManager->ApplyBrushEdit(WorldLocation, Brush, EEditMode::Paint);
	EditManager->EndEditOperation();

	UE_LOG(LogVoxelStreaming, Log, TEXT("TestPaintAt: Painted at (%.0f, %.0f, %.0f) with radius %.0f, material %d - %d voxels modified"),
		WorldLocation.X, WorldLocation.Y, WorldLocation.Z, Radius, MaterialID, VoxelsModified);

	return VoxelsModified;
}

bool AVoxelWorldTestActor::TestUndo()
{
	if (!ChunkManager)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("TestUndo: ChunkManager not available"));
		return false;
	}

	UVoxelEditManager* EditManager = ChunkManager->GetEditManager();
	if (!EditManager)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("TestUndo: EditManager not available"));
		return false;
	}

	if (!EditManager->CanUndo())
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("TestUndo: Nothing to undo"));
		return false;
	}

	const bool bSuccess = EditManager->Undo();
	UE_LOG(LogVoxelStreaming, Log, TEXT("TestUndo: %s"), bSuccess ? TEXT("Success") : TEXT("Failed"));
	return bSuccess;
}

bool AVoxelWorldTestActor::TestRedo()
{
	if (!ChunkManager)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("TestRedo: ChunkManager not available"));
		return false;
	}

	UVoxelEditManager* EditManager = ChunkManager->GetEditManager();
	if (!EditManager)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("TestRedo: EditManager not available"));
		return false;
	}

	if (!EditManager->CanRedo())
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("TestRedo: Nothing to redo"));
		return false;
	}

	const bool bSuccess = EditManager->Redo();
	UE_LOG(LogVoxelStreaming, Log, TEXT("TestRedo: %s"), bSuccess ? TEXT("Success") : TEXT("Failed"));
	return bSuccess;
}

bool AVoxelWorldTestActor::TestSaveEdits(const FString& FileName)
{
	if (!ChunkManager)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("TestSaveEdits: ChunkManager not available"));
		return false;
	}

	UVoxelEditManager* EditManager = ChunkManager->GetEditManager();
	if (!EditManager)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("TestSaveEdits: EditManager not available"));
		return false;
	}

	// Save to project's Saved folder
	const FString FilePath = FPaths::ProjectSavedDir() / FileName;
	const bool bSuccess = EditManager->SaveEditsToFile(FilePath);

	UE_LOG(LogVoxelStreaming, Log, TEXT("TestSaveEdits: %s to '%s'"),
		bSuccess ? TEXT("Saved") : TEXT("Failed to save"), *FilePath);

	return bSuccess;
}

bool AVoxelWorldTestActor::TestLoadEdits(const FString& FileName)
{
	if (!ChunkManager)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("TestLoadEdits: ChunkManager not available"));
		return false;
	}

	UVoxelEditManager* EditManager = ChunkManager->GetEditManager();
	if (!EditManager)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("TestLoadEdits: EditManager not available"));
		return false;
	}

	// Load from project's Saved folder
	const FString FilePath = FPaths::ProjectSavedDir() / FileName;
	const bool bSuccess = EditManager->LoadEditsFromFile(FilePath);

	UE_LOG(LogVoxelStreaming, Log, TEXT("TestLoadEdits: %s from '%s'"),
		bSuccess ? TEXT("Loaded") : TEXT("Failed to load"), *FilePath);

	return bSuccess;
}

void AVoxelWorldTestActor::PrintEditStats()
{
	if (!ChunkManager)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("PrintEditStats: ChunkManager not available"));
		return;
	}

	UVoxelEditManager* EditManager = ChunkManager->GetEditManager();
	if (!EditManager)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("PrintEditStats: EditManager not available"));
		return;
	}

	UE_LOG(LogVoxelStreaming, Log, TEXT("=== Voxel Edit System Statistics ==="));
	UE_LOG(LogVoxelStreaming, Log, TEXT("  Chunks with edits: %d"), EditManager->GetEditedChunkCount());
	UE_LOG(LogVoxelStreaming, Log, TEXT("  Total individual edits: %d"), EditManager->GetTotalEditCount());
	UE_LOG(LogVoxelStreaming, Log, TEXT("  Undo stack size: %d"), EditManager->GetUndoCount());
	UE_LOG(LogVoxelStreaming, Log, TEXT("  Redo stack size: %d"), EditManager->GetRedoCount());
	UE_LOG(LogVoxelStreaming, Log, TEXT("  Can Undo: %s"), EditManager->CanUndo() ? TEXT("Yes") : TEXT("No"));
	UE_LOG(LogVoxelStreaming, Log, TEXT("  Can Redo: %s"), EditManager->CanRedo() ? TEXT("Yes") : TEXT("No"));
}

void AVoxelWorldTestActor::PrintCollisionStats()
{
	if (!ChunkManager)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("PrintCollisionStats: ChunkManager not available"));
		return;
	}

	UVoxelCollisionManager* CollisionMgr = ChunkManager->GetCollisionManager();
	if (!CollisionMgr)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("PrintCollisionStats: CollisionManager not available (is bGenerateCollision enabled?)"));
		return;
	}

	UE_LOG(LogVoxelStreaming, Log, TEXT("=== Voxel Collision System Statistics ==="));
	UE_LOG(LogVoxelStreaming, Log, TEXT("  Collision Radius: %.0f"), CollisionMgr->GetCollisionRadius());
	UE_LOG(LogVoxelStreaming, Log, TEXT("  Collision LOD Level: %d"), CollisionMgr->GetCollisionLODLevel());
	UE_LOG(LogVoxelStreaming, Log, TEXT("  Active collision chunks: %d"), CollisionMgr->GetCollisionChunkCount());
	UE_LOG(LogVoxelStreaming, Log, TEXT("  Pending cook requests: %d"), CollisionMgr->GetCookQueueCount());
	UE_LOG(LogVoxelStreaming, Log, TEXT("  Currently cooking: %d"), CollisionMgr->GetCookingCount());
}

// ==================== Edit Input Processing ====================

void AVoxelWorldTestActor::ProcessEditInputs()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC)
	{
		return;
	}

	// Get current mouse button states
	const bool bLeftMouseDown = PC->IsInputKeyDown(EKeys::LeftMouseButton);
	const bool bRightMouseDown = PC->IsInputKeyDown(EKeys::RightMouseButton);
	const bool bMiddleMouseDown = PC->IsInputKeyDown(EKeys::MiddleMouseButton);

	// Handle mouse wheel for brush radius adjustment using scroll up/down keys
	const bool bScrollUp = PC->WasInputKeyJustPressed(EKeys::MouseScrollUp);
	const bool bScrollDown = PC->WasInputKeyJustPressed(EKeys::MouseScrollDown);

	if (bScrollUp || bScrollDown)
	{
		// Adjust radius by 10% per scroll tick
		const float Direction = bScrollUp ? 1.0f : -1.0f;
		const float RadiusAdjustment = EditBrushRadius * 0.1f * Direction;
		EditBrushRadius = FMath::Clamp(EditBrushRadius + RadiusAdjustment, 50.0f, 2000.0f);

		// Screen feedback for radius change
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Cyan,
				FString::Printf(TEXT("Brush Radius: %.0f"), EditBrushRadius));
		}
	}

	// Handle keyboard shortcuts for edit operations
	if (PC->WasInputKeyJustPressed(EKeys::Z))
	{
		const bool bSuccess = TestUndo();
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 1.5f, bSuccess ? FColor::Yellow : FColor::Red,
				bSuccess ? TEXT("Undo") : TEXT("Nothing to undo"));
		}
	}

	if (PC->WasInputKeyJustPressed(EKeys::Y))
	{
		const bool bSuccess = TestRedo();
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 1.5f, bSuccess ? FColor::Yellow : FColor::Red,
				bSuccess ? TEXT("Redo") : TEXT("Nothing to redo"));
		}
	}

	if (PC->WasInputKeyJustPressed(EKeys::F9))
	{
		const bool bSuccess = TestSaveEdits();
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 2.0f, bSuccess ? FColor::Green : FColor::Red,
				bSuccess ? TEXT("Edits saved to VoxelEdits.dat") : TEXT("Failed to save edits"));
		}
	}

	if (PC->WasInputKeyJustPressed(EKeys::F10))
	{
		const bool bSuccess = TestLoadEdits();
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 2.0f, bSuccess ? FColor::Green : FColor::Red,
				bSuccess ? TEXT("Edits loaded from VoxelEdits.dat") : TEXT("Failed to load edits"));
		}
	}

	// Detect button press (transition from not-pressed to pressed)
	const bool bLeftMousePressed = bLeftMouseDown && !bWasLeftMouseDown;
	const bool bRightMousePressed = bRightMouseDown && !bWasRightMouseDown;
	const bool bMiddleMousePressed = bMiddleMouseDown && !bWasMiddleMouseDown;

	// Update previous state
	bWasLeftMouseDown = bLeftMouseDown;
	bWasRightMouseDown = bRightMouseDown;
	bWasMiddleMouseDown = bMiddleMouseDown;

	// Handle edit actions on press
	if (bLeftMousePressed)
	{
		FVector HitLocation;
		if (TraceTerrainFromCamera(HitLocation))
		{
			UE_LOG(LogVoxelStreaming, Log, TEXT("LEFT CLICK: Dig at (%.0f, %.0f, %.0f)"),
				HitLocation.X, HitLocation.Y, HitLocation.Z);
			const int32 VoxelsModified = TestDigAt(HitLocation, EditBrushRadius);
			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 2.0f, FColor::Orange,
					FString::Printf(TEXT("Dig at (%.0f, %.0f, %.0f): %d voxels"),
						HitLocation.X, HitLocation.Y, HitLocation.Z, VoxelsModified));
			}
		}
		else
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("LEFT CLICK: No terrain hit"));
			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Red, TEXT("Dig: No terrain under crosshair"));
			}
		}
	}

	if (bRightMousePressed)
	{
		FVector HitLocation;
		if (TraceTerrainFromCamera(HitLocation))
		{
			UE_LOG(LogVoxelStreaming, Log, TEXT("RIGHT CLICK: Build at (%.0f, %.0f, %.0f)"),
				HitLocation.X, HitLocation.Y, HitLocation.Z);
			const int32 VoxelsModified = TestBuildAt(HitLocation, EditBrushRadius, EditMaterialID);
			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 2.0f, FColor::Green,
					FString::Printf(TEXT("Build at (%.0f, %.0f, %.0f): %d voxels (Mat %d)"),
						HitLocation.X, HitLocation.Y, HitLocation.Z, VoxelsModified, EditMaterialID));
			}
		}
		else
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("RIGHT CLICK: No terrain hit"));
			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Red, TEXT("Build: No terrain under crosshair"));
			}
		}
	}

	if (bMiddleMousePressed)
	{
		FVector HitLocation;
		if (TraceTerrainFromCamera(HitLocation))
		{
			UE_LOG(LogVoxelStreaming, Log, TEXT("MIDDLE CLICK: Paint at (%.0f, %.0f, %.0f)"),
				HitLocation.X, HitLocation.Y, HitLocation.Z);
			const int32 VoxelsModified = TestPaintAt(HitLocation, EditBrushRadius, EditMaterialID);
			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 2.0f, FColor::Magenta,
					FString::Printf(TEXT("Paint at (%.0f, %.0f, %.0f): %d voxels (Mat %d)"),
						HitLocation.X, HitLocation.Y, HitLocation.Z, VoxelsModified, EditMaterialID));
			}
		}
		else
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("MIDDLE CLICK: No terrain hit"));
			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Red, TEXT("Paint: No terrain under crosshair"));
			}
		}
	}
}

bool AVoxelWorldTestActor::TraceTerrainFromCamera(FVector& OutHitLocation) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC)
	{
		return false;
	}

	// Get camera location and rotation
	FVector CameraLocation;
	FRotator CameraRotation;
	PC->GetPlayerViewPoint(CameraLocation, CameraRotation);

	// Calculate trace end point (very far in camera direction)
	const FVector TraceDirection = CameraRotation.Vector();
	const float TraceDistance = 100000.0f;  // 1km trace distance
	const FVector TraceEnd = CameraLocation + TraceDirection * TraceDistance;

	// Set up trace parameters
	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = false;  // Use simple collision for terrain
	QueryParams.AddIgnoredActor(this);

	// Ignore the player pawn to avoid hitting ourselves
	if (APawn* PlayerPawn = PC->GetPawn())
	{
		QueryParams.AddIgnoredActor(PlayerPawn);
	}

	// Trace for terrain collision
	FHitResult HitResult;
	const bool bHit = World->LineTraceSingleByChannel(
		HitResult,
		CameraLocation,
		TraceEnd,
		ECC_WorldStatic,
		QueryParams
	);

	if (bHit)
	{
		OutHitLocation = HitResult.ImpactPoint;
		return true;
	}

	return false;
}

void AVoxelWorldTestActor::DrawEditCrosshair() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC)
	{
		return;
	}

	// Trace to find target location
	FVector HitLocation;
	const bool bHasTarget = TraceTerrainFromCamera(HitLocation);

	// Draw on-screen status text (two lines)
	if (GEngine)
	{
		// Line 1: Mouse controls
		const FString MouseText = bHasTarget
			? FString::Printf(TEXT("[Edit] LMB: Dig | RMB: Build | MMB: Paint | Wheel: Radius %.0f | Mat: %d"), EditBrushRadius, EditMaterialID)
			: FString::Printf(TEXT("[Edit] No terrain target | Wheel: Radius %.0f | Mat: %d"), EditBrushRadius, EditMaterialID);

		const FColor TextColor = bHasTarget ? FColor::Cyan : FColor(128, 128, 128);
		GEngine->AddOnScreenDebugMessage(-2, 0.0f, TextColor, MouseText);

		// Line 2: Keyboard shortcuts
		GEngine->AddOnScreenDebugMessage(-3, 0.0f, FColor::White, TEXT("[Keys] Z: Undo | Y: Redo | F9: Save | F10: Load"));
	}

	// Draw 3D crosshair/target indicator at hit location
	if (bHasTarget)
	{
		const FColor TargetColor = FColor::Cyan;

		// Draw cross lines at target - same size as brush radius
		DrawDebugLine(World, HitLocation - FVector(EditBrushRadius, 0, 0), HitLocation + FVector(EditBrushRadius, 0, 0),
			TargetColor, false, 0.0f, 0, 3.0f);
		DrawDebugLine(World, HitLocation - FVector(0, EditBrushRadius, 0), HitLocation + FVector(0, EditBrushRadius, 0),
			TargetColor, false, 0.0f, 0, 3.0f);
		DrawDebugLine(World, HitLocation - FVector(0, 0, EditBrushRadius), HitLocation + FVector(0, 0, EditBrushRadius),
			TargetColor, false, 0.0f, 0, 3.0f);

		// Draw sphere showing brush radius
		DrawDebugSphere(World, HitLocation, EditBrushRadius, 24, FColor::Yellow, false, 0.0f, 0, 1.0f);
	}
}
