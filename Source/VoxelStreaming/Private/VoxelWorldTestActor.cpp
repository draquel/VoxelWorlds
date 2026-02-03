// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWorldTestActor.h"
#include "VoxelStreaming.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "DistanceBandLODStrategy.h"
#include "VoxelPMCRenderer.h"
#include "VoxelCustomVFRenderer.h"
#include "VoxelCPUSmoothMesher.h"
#include "Engine/World.h"
#include "DrawDebugHelpers.h"

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
		if (LODParameterCollection)
		{
			CustomVFRenderer->SetLODParameterCollection(LODParameterCollection);
			CustomVFRenderer->SetLODTransitionDistances(LODStartDistance, LODEndDistance);
			UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: LOD MPC configured (Start=%.0f, End=%.0f)"),
				LODStartDistance, LODEndDistance);
		}
		else
		{
			UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: No LOD Parameter Collection assigned. ")
				TEXT("Material-based LOD morphing disabled."));
		}

		MeshRenderer = CustomVFRenderer;
		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWorldTestActor: Using Custom Vertex Factory renderer (GPU-driven)"));
	}
	else
	{
		// Use PMC fallback renderer
		FVoxelPMCRenderer* PMCRenderer = new FVoxelPMCRenderer();
		PMCRenderer->Initialize(World, Config);
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

	// Propagate debug flags to mesher if enabled
	if (bDebugLogTransitionCells || bDrawTransitionCellDebug)
	{
		SetTransitionCellDebugging(true);
		UE_LOG(LogVoxelStreaming, Warning, TEXT("VoxelWorldTestActor: Transvoxel debugging ENABLED (Log=%s, Viz=%s)"),
			bDebugLogTransitionCells ? TEXT("Yes") : TEXT("No"),
			bDrawTransitionCellDebug ? TEXT("Yes") : TEXT("No"));
	}
}

void AVoxelWorldTestActor::ShutdownVoxelWorld()
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

	// LOD bands - MINIMAL for testing smooth meshing performance
	// ChunkSize=32, VoxelSize=100 -> 3200 units per chunk
	// Max view distance: 6400 units (2 chunk radius) = ~50 chunks total
	Config->LODBands.Empty();

	// LOD 0: 0-3200 units (1 chunk radius), full detail
	FLODBand Band0;
	Band0.LODLevel = 0;
	Band0.MinDistance = 0.0f;
	Band0.MaxDistance = 3200.0f;
	Band0.VoxelStride = 1;
	Band0.MorphRange = 800.0f;
	Config->LODBands.Add(Band0);

	// LOD 1: 3200-6400 units (2 chunk radius), half detail
	FLODBand Band1;
	Band1.LODLevel = 1;
	Band1.MinDistance = 3200.0f;
	Band1.MaxDistance = 6400.0f;
	Band1.VoxelStride = 2;
	Band1.MorphRange = 800.0f;
	Config->LODBands.Add(Band1);

	// Streaming settings - minimal for testing
	Config->MaxChunksToLoadPerFrame = 2;
	Config->MaxChunksToUnloadPerFrame = 4;
	Config->StreamingTimeSliceMS = 2.0f;
	Config->MaxLoadedChunks = 100;  // Small view distance = fewer chunks

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
