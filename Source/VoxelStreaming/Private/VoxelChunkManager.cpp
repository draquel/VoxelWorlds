// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelChunkManager.h"
#include "VoxelStreaming.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelBiomeConfiguration.h"
#include "Algo/BinarySearch.h"
#include "VoxelCoordinates.h"
#include "IVoxelLODStrategy.h"
#include "IVoxelMeshRenderer.h"
#include "VoxelNoiseTypes.h"
#include "VoxelCPUCubicMesher.h"
#include "VoxelCPUSmoothMesher.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/Pawn.h"
#include "DrawDebugHelpers.h"

#if WITH_EDITOR
#include "Editor.h"
#include "LevelEditorViewport.h"
#endif

UVoxelChunkManager::UVoxelChunkManager()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

void UVoxelChunkManager::BeginPlay()
{
	Super::BeginPlay();
}

void UVoxelChunkManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Shutdown();
	Super::EndPlay(EndPlayReason);
}

void UVoxelChunkManager::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bIsInitialized || !bStreamingEnabled)
	{
		return;
	}

	++CurrentFrame;

	// Build query context from camera state
	FLODQueryContext Context = BuildQueryContext();

	// Debug: Log viewer position periodically
	static int32 ViewerLogCounter = 0;
	if (++ViewerLogCounter % 120 == 0)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("Viewer position: (%.0f, %.0f, %.0f)"),
			Context.ViewerPosition.X, Context.ViewerPosition.Y, Context.ViewerPosition.Z);
	}

	// Calculate current viewer chunk coordinate
	const FIntVector CurrentViewerChunk = WorldToChunkCoord(Context.ViewerPosition);

	// Determine if we need to update streaming decisions
	// Only update when:
	// 1. Force flag is set
	// 2. Viewer moved to a different chunk
	// 3. This is the first update (CachedViewerChunk is at sentinel value)
	const bool bViewerChunkChanged = (CurrentViewerChunk != CachedViewerChunk);
	const bool bNeedStreamingUpdate = bForceStreamingUpdate || bViewerChunkChanged;

	// Update LOD strategy (always update for morph factor interpolation)
	if (LODStrategy)
	{
		LODStrategy->Update(Context, DeltaTime);
	}

	// Update streaming decisions only when necessary
	if (bNeedStreamingUpdate)
	{
		UpdateStreamingDecisions(Context);
		CachedViewerChunk = CurrentViewerChunk;
		LastStreamingUpdatePosition = Context.ViewerPosition;
		bForceStreamingUpdate = false;

		// Debug: Log when streaming decisions are updated
		UE_LOG(LogVoxelStreaming, Verbose, TEXT("Streaming update: ViewerChunk=(%d,%d,%d), Forced=%s"),
			CurrentViewerChunk.X, CurrentViewerChunk.Y, CurrentViewerChunk.Z,
			bForceStreamingUpdate ? TEXT("Yes") : TEXT("No"));
	}

	// Process queues (time-sliced)
	const float TimeSlice = Configuration ? Configuration->StreamingTimeSliceMS : 2.0f;
	ProcessGenerationQueue(TimeSlice * 0.4f);
	ProcessMeshingQueue(TimeSlice * 0.4f);

	// Submit pending meshes to renderer
	// With batched render operations, multiple submits are consolidated into one render command
	// so we can process more per frame without overwhelming the render thread
	const int32 MaxRenderSubmitsPerFrame = Configuration ? Configuration->MaxChunksToLoadPerFrame : 8;

	if (PendingMeshQueue.Num() > 0)
	{
		int32 RenderSubmitCount = 0;
		while (PendingMeshQueue.Num() > 0 && RenderSubmitCount < MaxRenderSubmitsPerFrame)
		{
			const FIntVector ChunkCoord = PendingMeshQueue[0].ChunkCoord;
			OnChunkMeshingComplete(ChunkCoord);
			++RenderSubmitCount;
		}
	}

	// Process unloads - with batched operations we can handle more per frame
	const int32 MaxUnloadsPerFrame = Configuration ? Configuration->MaxChunksToUnloadPerFrame : 8;
	ProcessUnloadQueue(MaxUnloadsPerFrame);

	// Update LOD transitions only if viewer moved significantly
	// This reduces LOD morph factor calculations when stationary or moving slowly
	const float PositionDeltaSq = FVector::DistSquared(Context.ViewerPosition, LastLODUpdatePosition);
	if (bViewerChunkChanged || PositionDeltaSq > LODUpdateThresholdSq)
	{
		UpdateLODTransitions(Context);
		LastLODUpdatePosition = Context.ViewerPosition;
	}

	// Flush all pending render operations as a single batched command
	// This consolidates all chunk adds/removes into one render command per frame
	if (MeshRenderer)
	{
		MeshRenderer->FlushPendingOperations();
	}
}

// ==================== Initialization ====================

void UVoxelChunkManager::Initialize(
	UVoxelWorldConfiguration* InConfig,
	IVoxelLODStrategy* InLODStrategy,
	IVoxelMeshRenderer* InRenderer)
{
	if (bIsInitialized)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("ChunkManager::Initialize called when already initialized"));
		Shutdown();
	}

	if (!InConfig)
	{
		UE_LOG(LogVoxelStreaming, Error, TEXT("ChunkManager::Initialize called with null configuration"));
		return;
	}

	Configuration = InConfig;
	LODStrategy = InLODStrategy;
	MeshRenderer = InRenderer;

	// Disable LOD morphing for cubic mode - vertices cannot interpolate with hard edges
	// LOD bands and levels still apply (for distance-based loading and potential stride use)
	if (Configuration->MeshingMode == EMeshingMode::Cubic && Configuration->bEnableLODMorphing)
	{
		Configuration->bEnableLODMorphing = false;
		UE_LOG(LogVoxelStreaming, Log, TEXT("LOD morphing disabled for cubic mode (hard-edged vertices cannot interpolate)"));
	}

	// Initialize LOD strategy
	if (LODStrategy)
	{
		LODStrategy->Initialize(Configuration);
	}

	// Clear any existing state
	ChunkStates.Empty();
	LoadedChunkCoords.Empty();
	GenerationQueue.Empty();
	GenerationQueueSet.Empty();
	MeshingQueue.Empty();
	MeshingQueueSet.Empty();
	UnloadQueue.Empty();
	UnloadQueueSet.Empty();

	// Reset statistics
	TotalChunksGenerated = 0;
	TotalChunksMeshed = 0;
	TotalChunksUnloaded = 0;
	CurrentFrame = 0;

	// Reset streaming decision caching (sentinel values force update on first tick)
	CachedViewerChunk = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
	LastStreamingUpdatePosition = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
	LastLODUpdatePosition = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
	bForceStreamingUpdate = false;

	// Create generation components
	FWorldModeTerrainParams TerrainParams;
	TerrainParams.SeaLevel = Configuration->SeaLevel;
	TerrainParams.HeightScale = Configuration->HeightScale;
	TerrainParams.BaseHeight = Configuration->BaseHeight;
	WorldMode = MakeUnique<FInfinitePlaneWorldMode>(TerrainParams);

	NoiseGenerator = MakeUnique<FVoxelCPUNoiseGenerator>();
	NoiseGenerator->Initialize();

	// Create mesher based on configuration
	if (Configuration->MeshingMode == EMeshingMode::Smooth)
	{
		auto SmoothMesher = MakeUnique<FVoxelCPUSmoothMesher>();
		SmoothMesher->Initialize();

		// Configure smooth meshing parameters
		FVoxelMeshingConfig MeshConfig = SmoothMesher->GetConfig();
		MeshConfig.bUseSmoothMeshing = true;
		MeshConfig.IsoLevel = 0.5f;
		MeshConfig.bCalculateAO = Configuration->bCalculateAO;
		MeshConfig.UVScale = Configuration->UVScale;

		// Disable LOD seam handling if configured
		if (!Configuration->bEnableLODSeams)
		{
			MeshConfig.bUseTransvoxel = false;
			MeshConfig.bGenerateSkirts = false;
		}

		SmoothMesher->SetConfig(MeshConfig);

		Mesher = MoveTemp(SmoothMesher);
		UE_LOG(LogVoxelStreaming, Log, TEXT("Using Smooth (Marching Cubes) mesher (AO=%s, UVScale=%.2f)"),
			Configuration->bCalculateAO ? TEXT("true") : TEXT("false"),
			Configuration->UVScale);
	}
	else
	{
		auto CubicMesher = MakeUnique<FVoxelCPUCubicMesher>();
		CubicMesher->Initialize();

		// Configure cubic meshing parameters from world config
		FVoxelMeshingConfig MeshConfig = CubicMesher->GetConfig();
		MeshConfig.bUseGreedyMeshing = Configuration->bUseGreedyMeshing;
		MeshConfig.bCalculateAO = Configuration->bCalculateAO;
		MeshConfig.UVScale = Configuration->UVScale;
		CubicMesher->SetConfig(MeshConfig);

		Mesher = MoveTemp(CubicMesher);
		UE_LOG(LogVoxelStreaming, Log, TEXT("Using Cubic mesher (Greedy=%s, AO=%s, UVScale=%.2f)"),
			Configuration->bUseGreedyMeshing ? TEXT("true") : TEXT("false"),
			Configuration->bCalculateAO ? TEXT("true") : TEXT("false"),
			Configuration->UVScale);
	}

	// Clear pending mesh queue
	PendingMeshQueue.Empty();

	bIsInitialized = true;

	UE_LOG(LogVoxelStreaming, Log, TEXT("ChunkManager initialized with config: VoxelSize=%.1f, ChunkSize=%d"),
		Configuration->VoxelSize, Configuration->ChunkSize);
}

void UVoxelChunkManager::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	// Clear all chunks from renderer
	if (MeshRenderer)
	{
		MeshRenderer->ClearAllChunks();
	}

	// Clear state
	ChunkStates.Empty();
	LoadedChunkCoords.Empty();
	GenerationQueue.Empty();
	GenerationQueueSet.Empty();
	MeshingQueue.Empty();
	MeshingQueueSet.Empty();
	UnloadQueue.Empty();
	UnloadQueueSet.Empty();

	// Reset streaming decision caching
	CachedViewerChunk = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
	LastStreamingUpdatePosition = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
	LastLODUpdatePosition = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
	bForceStreamingUpdate = false;

	// Clean up LOD strategy (we own it)
	if (LODStrategy)
	{
		delete LODStrategy;
		LODStrategy = nullptr;
	}

	// Shutdown and cleanup generation components
	if (Mesher)
	{
		Mesher->Shutdown();
		Mesher.Reset();
	}

	if (NoiseGenerator)
	{
		NoiseGenerator->Shutdown();
		NoiseGenerator.Reset();
	}

	WorldMode.Reset();

	// Clear pending mesh queue
	PendingMeshQueue.Empty();

	// Don't delete renderer - we don't own it
	MeshRenderer = nullptr;
	Configuration = nullptr;

	bIsInitialized = false;

	UE_LOG(LogVoxelStreaming, Log, TEXT("ChunkManager shutdown. Stats: Generated=%lld, Meshed=%lld, Unloaded=%lld"),
		TotalChunksGenerated, TotalChunksMeshed, TotalChunksUnloaded);
}

// ==================== Streaming Control ====================

void UVoxelChunkManager::SetStreamingEnabled(bool bEnabled)
{
	bStreamingEnabled = bEnabled;

	if (bEnabled)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("Chunk streaming enabled"));
	}
	else
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("Chunk streaming disabled"));
	}
}

void UVoxelChunkManager::ForceStreamingUpdate()
{
	if (!bIsInitialized)
	{
		return;
	}

	// Set the force flag - TickComponent will handle the actual update
	// This ensures updates happen at the proper point in the frame sequence
	bForceStreamingUpdate = true;

	UE_LOG(LogVoxelStreaming, Log, TEXT("Force streaming update requested"));
}

// ==================== Chunk Requests ====================

void UVoxelChunkManager::RequestChunkLoad(const FIntVector& ChunkCoord, float Priority)
{
	if (!bIsInitialized)
	{
		return;
	}

	FVoxelChunkState& State = GetOrCreateChunkState(ChunkCoord);

	if (State.State == EChunkState::Unloaded)
	{
		// Add to generation queue with sorted insertion
		FChunkLODRequest Request;
		Request.ChunkCoord = ChunkCoord;
		Request.LODLevel = 0; // Will be determined by LOD strategy
		Request.Priority = Priority;

		if (AddToGenerationQueue(Request))
		{
			SetChunkState(ChunkCoord, EChunkState::PendingGeneration);

			UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d, %d, %d) requested for loading"),
				ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
		}
	}
}

void UVoxelChunkManager::RequestChunkUnload(const FIntVector& ChunkCoord)
{
	if (!bIsInitialized)
	{
		return;
	}

	if (ChunkStates.Contains(ChunkCoord))
	{
		const EChunkState CurrentState = ChunkStates[ChunkCoord].State;

		if (CurrentState != EChunkState::Unloaded && CurrentState != EChunkState::PendingUnload)
		{
			if (AddToUnloadQueue(ChunkCoord))
			{
				SetChunkState(ChunkCoord, EChunkState::PendingUnload);

				UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d, %d, %d) requested for unloading"),
					ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
			}
		}
	}
}

void UVoxelChunkManager::MarkChunkDirty(const FIntVector& ChunkCoord)
{
	if (!bIsInitialized)
	{
		return;
	}

	if (FVoxelChunkState* State = ChunkStates.Find(ChunkCoord))
	{
		if (State->State == EChunkState::Loaded)
		{
			State->Descriptor.bIsDirty = true;

			// Add to meshing queue for remeshing with sorted insertion
			FChunkLODRequest Request;
			Request.ChunkCoord = ChunkCoord;
			Request.LODLevel = State->LODLevel;
			Request.Priority = 100.0f; // High priority for dirty chunks

			if (AddToMeshingQueue(Request))
			{
				SetChunkState(ChunkCoord, EChunkState::PendingMeshing);
			}
		}
	}
}

// ==================== Queries ====================

EChunkState UVoxelChunkManager::GetChunkState(const FIntVector& ChunkCoord) const
{
	if (const FVoxelChunkState* State = ChunkStates.Find(ChunkCoord))
	{
		return State->State;
	}

	return EChunkState::Unloaded;
}

bool UVoxelChunkManager::IsChunkLoaded(const FIntVector& ChunkCoord) const
{
	return LoadedChunkCoords.Contains(ChunkCoord);
}

int32 UVoxelChunkManager::GetLoadedChunkCount() const
{
	return LoadedChunkCoords.Num();
}

void UVoxelChunkManager::GetLoadedChunks(TArray<FIntVector>& OutChunks) const
{
	OutChunks = LoadedChunkCoords.Array();
}

FIntVector UVoxelChunkManager::WorldToChunkCoord(const FVector& WorldPosition) const
{
	if (!Configuration)
	{
		return FIntVector::ZeroValue;
	}

	return FVoxelCoordinates::WorldToChunk(WorldPosition, Configuration->ChunkSize, Configuration->VoxelSize);
}

// ==================== Debug ====================

FString UVoxelChunkManager::GetDebugStats() const
{
	FString Stats = TEXT("=== VoxelChunkManager ===\n");
	Stats += FString::Printf(TEXT("Initialized: %s\n"), bIsInitialized ? TEXT("Yes") : TEXT("No"));
	Stats += FString::Printf(TEXT("Streaming: %s\n"), bStreamingEnabled ? TEXT("Enabled") : TEXT("Disabled"));
	Stats += FString::Printf(TEXT("Frame: %lld\n"), CurrentFrame);
	Stats += TEXT("\n");

	Stats += TEXT("--- Chunk Counts ---\n");
	Stats += FString::Printf(TEXT("Total Tracked: %d\n"), ChunkStates.Num());
	Stats += FString::Printf(TEXT("Loaded: %d\n"), LoadedChunkCoords.Num());
	Stats += FString::Printf(TEXT("Generation Queue: %d\n"), GenerationQueue.Num());
	Stats += FString::Printf(TEXT("Meshing Queue: %d\n"), MeshingQueue.Num());
	Stats += FString::Printf(TEXT("Unload Queue: %d\n"), UnloadQueue.Num());
	Stats += TEXT("\n");

	Stats += TEXT("--- Session Stats ---\n");
	Stats += FString::Printf(TEXT("Total Generated: %lld\n"), TotalChunksGenerated);
	Stats += FString::Printf(TEXT("Total Meshed: %lld\n"), TotalChunksMeshed);
	Stats += FString::Printf(TEXT("Total Unloaded: %lld\n"), TotalChunksUnloaded);
	Stats += TEXT("\n");

	// Count chunks by state
	int32 StateCounts[(int32)EChunkState::PendingUnload + 1] = {0};
	for (const auto& Pair : ChunkStates)
	{
		const int32 StateIndex = (int32)Pair.Value.State;
		if (StateIndex >= 0 && StateIndex < UE_ARRAY_COUNT(StateCounts))
		{
			++StateCounts[StateIndex];
		}
	}

	Stats += TEXT("--- Chunks by State ---\n");
	Stats += FString::Printf(TEXT("Unloaded: %d\n"), StateCounts[(int32)EChunkState::Unloaded]);
	Stats += FString::Printf(TEXT("PendingGeneration: %d\n"), StateCounts[(int32)EChunkState::PendingGeneration]);
	Stats += FString::Printf(TEXT("Generating: %d\n"), StateCounts[(int32)EChunkState::Generating]);
	Stats += FString::Printf(TEXT("PendingMeshing: %d\n"), StateCounts[(int32)EChunkState::PendingMeshing]);
	Stats += FString::Printf(TEXT("Meshing: %d\n"), StateCounts[(int32)EChunkState::Meshing]);
	Stats += FString::Printf(TEXT("Loaded: %d\n"), StateCounts[(int32)EChunkState::Loaded]);
	Stats += FString::Printf(TEXT("PendingUnload: %d\n"), StateCounts[(int32)EChunkState::PendingUnload]);

	if (LODStrategy)
	{
		Stats += TEXT("\n");
		Stats += LODStrategy->GetDebugInfo();
	}

	return Stats;
}

void UVoxelChunkManager::DrawDebugVisualization() const
{
#if ENABLE_DRAW_DEBUG
	if (!bIsInitialized || !GetWorld())
	{
		return;
	}

	// Draw LOD strategy visualization
	if (LODStrategy)
	{
		FLODQueryContext Context = const_cast<UVoxelChunkManager*>(this)->BuildQueryContext();
		LODStrategy->DrawDebugVisualization(GetWorld(), Context);
	}

	// Draw chunk bounds colored by state
	for (const auto& Pair : ChunkStates)
	{
		const FIntVector& ChunkCoord = Pair.Key;
		const FVoxelChunkState& State = Pair.Value;

		FColor Color;
		switch (State.State)
		{
			case EChunkState::Loaded:           Color = FColor::Green; break;
			case EChunkState::PendingGeneration:Color = FColor::Yellow; break;
			case EChunkState::Generating:       Color = FColor::Orange; break;
			case EChunkState::PendingMeshing:   Color = FColor::Cyan; break;
			case EChunkState::Meshing:          Color = FColor::Blue; break;
			case EChunkState::PendingUnload:    Color = FColor::Red; break;
			default:                            Color = FColor::White; break;
		}

		const FBox LocalBounds = FVoxelCoordinates::ChunkToWorldBounds(
			ChunkCoord,
			Configuration->ChunkSize,
			Configuration->VoxelSize
		);
		// Add WorldOrigin offset for correct world-space position
		const FBox Bounds(LocalBounds.Min + Configuration->WorldOrigin, LocalBounds.Max + Configuration->WorldOrigin);

		DrawDebugBox(GetWorld(), Bounds.GetCenter(), Bounds.GetExtent(), Color, false, -1.0f, 0, 2.0f);
	}
#endif
}

FVoxelCPUSmoothMesher* UVoxelChunkManager::GetSmoothMesher() const
{
	if (!Mesher.IsValid())
	{
		return nullptr;
	}

	// Check mesher type using the virtual GetMesherTypeName method
	// This avoids dynamic_cast which requires RTTI (disabled in UE)
	if (Mesher->GetMesherTypeName() == TEXT("CPU Smooth"))
	{
		return static_cast<FVoxelCPUSmoothMesher*>(Mesher.Get());
	}

	return nullptr;
}

// ==================== Internal Update Methods ====================

FLODQueryContext UVoxelChunkManager::BuildQueryContext() const
{
	FLODQueryContext Context;
	bool bFoundViewer = false;
	FString ViewerSource = TEXT("None");

	// Get viewer state from player controller
	if (UWorld* World = GetWorld())
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			FVector Location;
			FRotator Rotation;
			PC->GetPlayerViewPoint(Location, Rotation);
			ViewerSource = TEXT("GetPlayerViewPoint");

			// Check if we got a valid position (not at origin when player is elsewhere)
			if (PC->GetPawn())
			{
				// Use pawn location if available - more reliable than GetPlayerViewPoint in some cases
				Location = PC->GetPawn()->GetActorLocation();
				Rotation = PC->GetControlRotation();
				ViewerSource = TEXT("Pawn");
			}

			Context.ViewerPosition = Location;
			Context.ViewerForward = Rotation.Vector();
			Context.ViewerRight = Rotation.RotateVector(FVector::RightVector);
			Context.ViewerUp = Rotation.RotateVector(FVector::UpVector);
			bFoundViewer = true;

			if (PC->PlayerCameraManager)
			{
				Context.FieldOfView = PC->PlayerCameraManager->GetFOVAngle();
				// Camera manager has most accurate view location
				Context.ViewerPosition = PC->PlayerCameraManager->GetCameraLocation();
				Context.ViewerForward = PC->PlayerCameraManager->GetCameraRotation().Vector();
				ViewerSource = TEXT("CameraManager");
			}
		}

#if WITH_EDITOR
		// In editor, try to get the editor viewport camera if no player found
		if (!bFoundViewer)
		{
			if (GEditor)
			{
				for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
				{
					if (ViewportClient && ViewportClient->IsPerspective())
					{
						Context.ViewerPosition = ViewportClient->GetViewLocation();
						Context.ViewerForward = ViewportClient->GetViewRotation().Vector();
						Context.FieldOfView = ViewportClient->ViewFOV;
						bFoundViewer = true;
						ViewerSource = TEXT("EditorViewport");
						break;
					}
				}
			}
		}
#endif

		Context.GameTime = World->GetTimeSeconds();
		Context.DeltaTime = World->GetDeltaSeconds();
	}

	// Fallback: If no viewer found, use the owning actor's location
	if (!bFoundViewer)
	{
		if (AActor* Owner = GetOwner())
		{
			Context.ViewerPosition = Owner->GetActorLocation();
			Context.ViewerForward = Owner->GetActorForwardVector();
			ViewerSource = TEXT("OwnerActor");
			bFoundViewer = true;
		}
	}

	// Debug: Log viewer source and position periodically
	static int32 ContextDebugCounter = 0;
	if (++ContextDebugCounter % 180 == 0)  // Every 3 seconds at 60fps
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("BuildQueryContext: Source=%s, Pos=(%.0f, %.0f, %.0f), Found=%s"),
			*ViewerSource, Context.ViewerPosition.X, Context.ViewerPosition.Y, Context.ViewerPosition.Z,
			bFoundViewer ? TEXT("Yes") : TEXT("No"));
	}

	// Configuration values
	if (Configuration)
	{
		Context.ViewDistance = Configuration->ViewDistance;
		Context.WorldOrigin = Configuration->WorldOrigin;
		Context.WorldMode = Configuration->WorldMode;
		Context.WorldRadius = Configuration->WorldRadius;
		Context.MaxChunksToLoadPerFrame = Configuration->MaxChunksToLoadPerFrame;
		Context.MaxChunksToUnloadPerFrame = Configuration->MaxChunksToUnloadPerFrame;
		Context.TimeSliceMS = Configuration->StreamingTimeSliceMS;
	}

	Context.FrameNumber = CurrentFrame;

	return Context;
}

void UVoxelChunkManager::UpdateStreamingDecisions(const FLODQueryContext& Context)
{
	if (!LODStrategy)
	{
		return;
	}

	// Get chunks to load
	TArray<FChunkLODRequest> ChunksToLoad;
	LODStrategy->GetChunksToLoad(ChunksToLoad, LoadedChunkCoords, Context);

	// Limit how many chunks we add per frame to prevent queue explosion
	// Cap at 2x the processing rate to allow queue to stabilize
	const int32 MaxChunksToAddPerFrame = Configuration->MaxChunksToLoadPerFrame * 2;
	int32 ChunksAddedThisFrame = 0;

	// Debug: Log streaming decisions periodically
	static int32 DebugFrameCounter = 0;
	if (++DebugFrameCounter % 60 == 0)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("Streaming: Viewer at (%.0f, %.0f, %.0f), ChunksToLoad=%d, Loaded=%d, GenQueue=%d, MeshQueue=%d"),
			Context.ViewerPosition.X, Context.ViewerPosition.Y, Context.ViewerPosition.Z,
			ChunksToLoad.Num(), LoadedChunkCoords.Num(), GenerationQueue.Num(), MeshingQueue.Num());
	}

	// Add to generation queue with sorted insertion (O(1) duplicate check, O(log n) insert)
	for (const FChunkLODRequest& Request : ChunksToLoad)
	{
		// Respect per-frame limit to prevent unbounded queue growth
		if (ChunksAddedThisFrame >= MaxChunksToAddPerFrame)
		{
			break;
		}

		const EChunkState CurrentState = GetChunkState(Request.ChunkCoord);

		if (CurrentState == EChunkState::Unloaded)
		{
			FVoxelChunkState& State = GetOrCreateChunkState(Request.ChunkCoord);
			State.LODLevel = Request.LODLevel;
			State.Priority = Request.Priority;

			if (AddToGenerationQueue(Request))
			{
				SetChunkState(Request.ChunkCoord, EChunkState::PendingGeneration);
				++ChunksAddedThisFrame;
			}
		}
	}

	// Get chunks to unload
	TArray<FIntVector> ChunksToUnload;
	LODStrategy->GetChunksToUnload(ChunksToUnload, LoadedChunkCoords, Context);

	// Add to unload queue with O(1) duplicate check
	for (const FIntVector& ChunkCoord : ChunksToUnload)
	{
		const EChunkState CurrentState = GetChunkState(ChunkCoord);

		if (CurrentState == EChunkState::Loaded)
		{
			if (AddToUnloadQueue(ChunkCoord))
			{
				SetChunkState(ChunkCoord, EChunkState::PendingUnload);
			}
		}
	}
}

void UVoxelChunkManager::ProcessGenerationQueue(float TimeSliceMS)
{
	if (GenerationQueue.Num() == 0 || !NoiseGenerator || !Configuration)
	{
		return;
	}

	const double StartTime = FPlatformTime::Seconds();
	const double TimeLimit = TimeSliceMS / 1000.0;
	const int32 MaxChunks = Configuration->MaxChunksToLoadPerFrame;
	int32 ProcessedCount = 0;

	while (GenerationQueue.Num() > 0 && ProcessedCount < MaxChunks)
	{
		// Check time limit
		if (FPlatformTime::Seconds() - StartTime > TimeLimit)
		{
			break;
		}

		// Get highest priority chunk and remove from queue and tracking set
		FChunkLODRequest Request = GenerationQueue[0];
		GenerationQueue.RemoveAt(0);
		GenerationQueueSet.Remove(Request.ChunkCoord);

		// Skip if state changed
		if (GetChunkState(Request.ChunkCoord) != EChunkState::PendingGeneration)
		{
			continue;
		}

		// Mark as generating
		SetChunkState(Request.ChunkCoord, EChunkState::Generating);

		// Build generation request
		FVoxelNoiseGenerationRequest GenRequest;
		GenRequest.ChunkCoord = Request.ChunkCoord;
		GenRequest.LODLevel = Request.LODLevel;
		GenRequest.ChunkSize = Configuration->ChunkSize;
		GenRequest.VoxelSize = Configuration->VoxelSize;
		GenRequest.NoiseParams = Configuration->NoiseParams;
		GenRequest.WorldMode = Configuration->WorldMode;
		GenRequest.SeaLevel = Configuration->SeaLevel;
		GenRequest.HeightScale = Configuration->HeightScale;
		GenRequest.BaseHeight = Configuration->BaseHeight;
		GenRequest.WorldOrigin = Configuration->WorldOrigin;

		// Biome configuration (contains biome definitions, blend settings, height rules)
		GenRequest.bEnableBiomes = Configuration->bEnableBiomes;
		GenRequest.BiomeConfiguration = Configuration->BiomeConfiguration;

		// Get chunk state to store voxel data
		FVoxelChunkState* State = ChunkStates.Find(Request.ChunkCoord);
		if (!State)
		{
			// State was removed during processing
			continue;
		}

		// Generate voxel data using CPU noise generator
		TArray<FVoxelData> VoxelData;
		const bool bSuccess = NoiseGenerator->GenerateChunkCPU(GenRequest, VoxelData);

		if (bSuccess)
		{
			// Store voxel data in chunk descriptor
			State->Descriptor.VoxelData = MoveTemp(VoxelData);
			OnChunkGenerationComplete(Request.ChunkCoord);
		}
		else
		{
			// Generation failed - reset to Unloaded state
			UE_LOG(LogVoxelStreaming, Warning, TEXT("Chunk (%d, %d, %d) generation failed"),
				Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z);
			State->Descriptor.VoxelData.Empty();
			SetChunkState(Request.ChunkCoord, EChunkState::Unloaded);
		}

		++ProcessedCount;
	}
}

void UVoxelChunkManager::ProcessMeshingQueue(float TimeSliceMS)
{
	if (MeshingQueue.Num() == 0 || !Mesher || !Configuration)
	{
		return;
	}

	// Throttle if pending mesh queue is full
	if (PendingMeshQueue.Num() >= MaxPendingMeshes)
	{
		return;
	}

	const double StartTime = FPlatformTime::Seconds();
	const double TimeLimit = TimeSliceMS / 1000.0;
	const int32 MaxChunks = Configuration->MaxChunksToLoadPerFrame;
	int32 ProcessedCount = 0;

	while (MeshingQueue.Num() > 0 && ProcessedCount < MaxChunks && PendingMeshQueue.Num() < MaxPendingMeshes)
	{
		// Check time limit
		if (FPlatformTime::Seconds() - StartTime > TimeLimit)
		{
			break;
		}

		// Get highest priority chunk and remove from queue and tracking set
		FChunkLODRequest Request = MeshingQueue[0];
		MeshingQueue.RemoveAt(0);
		MeshingQueueSet.Remove(Request.ChunkCoord);

		// Skip if state changed
		if (GetChunkState(Request.ChunkCoord) != EChunkState::PendingMeshing)
		{
			continue;
		}

		// Get chunk state for voxel data
		FVoxelChunkState* State = ChunkStates.Find(Request.ChunkCoord);
		if (!State || State->Descriptor.VoxelData.Num() == 0)
		{
			// No voxel data available - skip
			continue;
		}

		// Mark as meshing
		SetChunkState(Request.ChunkCoord, EChunkState::Meshing);

		// Build meshing request
		FVoxelMeshingRequest MeshRequest;
		MeshRequest.ChunkCoord = Request.ChunkCoord;
		MeshRequest.LODLevel = Request.LODLevel;
		MeshRequest.ChunkSize = Configuration->ChunkSize;
		MeshRequest.VoxelSize = Configuration->VoxelSize;
		MeshRequest.WorldOrigin = Configuration->WorldOrigin;
		MeshRequest.VoxelData = State->Descriptor.VoxelData;

		// Extract neighbor edge slices for seamless boundaries
		ExtractNeighborEdgeSlices(Request.ChunkCoord, MeshRequest);

		// Calculate transition faces for Transvoxel LOD seam handling
		// A face needs transition cells if the neighbor is at a lower LOD level (coarser)
		MeshRequest.TransitionFaces = 0;
		const int32 CurrentLOD = Request.LODLevel;

		// Check each of 6 faces for lower-LOD neighbors
		static const FIntVector NeighborOffsets[6] = {
			FIntVector(-1, 0, 0),  // -X
			FIntVector(1, 0, 0),   // +X
			FIntVector(0, -1, 0),  // -Y
			FIntVector(0, 1, 0),   // +Y
			FIntVector(0, 0, -1),  // -Z
			FIntVector(0, 0, 1),   // +Z
		};
		static const uint8 TransitionFlags[6] = {
			FVoxelMeshingRequest::TRANSITION_XNEG,
			FVoxelMeshingRequest::TRANSITION_XPOS,
			FVoxelMeshingRequest::TRANSITION_YNEG,
			FVoxelMeshingRequest::TRANSITION_YPOS,
			FVoxelMeshingRequest::TRANSITION_ZNEG,
			FVoxelMeshingRequest::TRANSITION_ZPOS,
		};

		// Helper to check if neighbor data was successfully extracted
		const int32 SliceSize = Configuration->ChunkSize * Configuration->ChunkSize;
		const int32 ChunkSize = Configuration->ChunkSize;
		auto HasNeighborData = [&MeshRequest, SliceSize](int32 FaceIndex) -> bool
		{
			switch (FaceIndex)
			{
				case 0: return MeshRequest.NeighborXNeg.Num() == SliceSize;
				case 1: return MeshRequest.NeighborXPos.Num() == SliceSize;
				case 2: return MeshRequest.NeighborYNeg.Num() == SliceSize;
				case 3: return MeshRequest.NeighborYPos.Num() == SliceSize;
				case 4: return MeshRequest.NeighborZNeg.Num() == SliceSize;
				case 5: return MeshRequest.NeighborZPos.Num() == SliceSize;
				default: return false;
			}
		};

		// Helper to check if ALL edge data needed for a transition face is available
		// Transition cells at face edges need diagonal neighbor data
		auto HasAllEdgeDataForFace = [&MeshRequest, ChunkSize](int32 FaceIndex) -> bool
		{
			// For each face, check if the edge neighbors that would be needed exist
			// Face 0 (-X): needs edges with Y and Z neighbors
			// Face 1 (+X): needs edges with Y and Z neighbors
			// Face 2 (-Y): needs edges with X and Z neighbors
			// Face 3 (+Y): needs edges with X and Z neighbors
			// Face 4 (-Z): needs edges with X and Y neighbors
			// Face 5 (+Z): needs edges with X and Y neighbors
			switch (FaceIndex)
			{
				case 0: // -X face
					return MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_YNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_YPOS) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_ZNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_ZPOS);
				case 1: // +X face
					return MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_YNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_YPOS) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_ZNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_ZPOS);
				case 2: // -Y face
					return MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_YNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_YNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_YNEG_ZNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_YNEG_ZPOS);
				case 3: // +Y face
					return MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_YPOS) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_YPOS) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_YPOS_ZNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_YPOS_ZPOS);
				case 4: // -Z face
					return MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_ZNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_ZNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_YNEG_ZNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_YPOS_ZNEG);
				case 5: // +Z face
					return MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_ZPOS) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_ZPOS) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_YNEG_ZPOS) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_YPOS_ZPOS);
				default:
					return false;
			}
		};

		for (int32 i = 0; i < 6; i++)
		{
			const FIntVector NeighborCoord = Request.ChunkCoord + NeighborOffsets[i];
			if (const FVoxelChunkState* NeighborState = ChunkStates.Find(NeighborCoord))
			{
				// Store neighbor LOD level for transition cell stride calculation
				MeshRequest.NeighborLODLevels[i] = NeighborState->LODLevel;

				// Neighbor exists - check if it's at a lower LOD (higher LOD level number = coarser)
				// IMPORTANT: Verify that ALL neighbor data needed for this face is available!
				// This includes the face neighbor AND all edge neighbors for transition cells at edges.
				if (NeighborState->LODLevel > CurrentLOD)
				{
					if (HasNeighborData(i) && HasAllEdgeDataForFace(i))
					{
						MeshRequest.TransitionFaces |= TransitionFlags[i];
					}
					else
					{
						// Missing some neighbor data - skip transition cells for entire face
						// This prevents mixing transition and regular MC on the same boundary
						UE_LOG(LogVoxelStreaming, Verbose,
							TEXT("Chunk (%d,%d,%d) face %d: missing edge/face neighbor data - skipping all transition cells for this face"),
							Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z, i);
					}
				}
			}
			else
			{
				// No neighbor - mark as -1
				MeshRequest.NeighborLODLevels[i] = -1;
			}
			// If neighbor doesn't exist, no transition needed (will be at chunk boundary anyway)
		}

		// Generate mesh using CPU mesher
		FChunkMeshData MeshData;
		const bool bSuccess = Mesher->GenerateMeshCPU(MeshRequest, MeshData);

		if (bSuccess)
		{
			// Store mesh in pending queue (will be submitted later, throttled)
			FPendingMeshData PendingMesh;
			PendingMesh.ChunkCoord = Request.ChunkCoord;
			PendingMesh.LODLevel = Request.LODLevel;
			PendingMesh.MeshData = MoveTemp(MeshData);
			PendingMeshQueue.Add(MoveTemp(PendingMesh));

			// NOTE: Don't call OnChunkMeshingComplete here - it's called in TickComponent
			// after throttled render submission
		}
		else
		{
			// Meshing failed - reset to PendingGeneration to retry
			UE_LOG(LogVoxelStreaming, Warning, TEXT("Chunk (%d, %d, %d) meshing failed"),
				Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z);
			SetChunkState(Request.ChunkCoord, EChunkState::PendingMeshing);
		}

		++ProcessedCount;
	}
}

void UVoxelChunkManager::ProcessUnloadQueue(int32 MaxChunks)
{
	int32 ProcessedCount = 0;

	while (UnloadQueue.Num() > 0 && ProcessedCount < MaxChunks)
	{
		// Remove from queue and tracking set
		FIntVector ChunkCoord = UnloadQueue[0];
		UnloadQueue.RemoveAt(0);
		UnloadQueueSet.Remove(ChunkCoord);

		// Skip if state changed
		if (GetChunkState(ChunkCoord) != EChunkState::PendingUnload)
		{
			continue;
		}

		// Remove from renderer
		if (MeshRenderer)
		{
			MeshRenderer->RemoveChunk(ChunkCoord);
		}

		// Remove from loaded set
		LoadedChunkCoords.Remove(ChunkCoord);

		// Remove state tracking
		RemoveChunkState(ChunkCoord);

		// Fire event
		OnChunkUnloaded.Broadcast(ChunkCoord);

		++TotalChunksUnloaded;
		++ProcessedCount;

		UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d, %d, %d) unloaded"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
	}
}

void UVoxelChunkManager::UpdateLODTransitions(const FLODQueryContext& Context)
{
	if (!LODStrategy || !MeshRenderer)
	{
		return;
	}

	// Skip LOD transition processing when LOD system is completely disabled
	if (Configuration && !Configuration->bEnableLOD)
	{
		return;
	}

	// Note: For cubic mode, bEnableLOD is true but bEnableLODMorphing is false.
	// LOD level changes still trigger remeshing, but morph factors stay at 0.

	// Throttle LOD remeshing to prevent overwhelming the render system
	// Keep this low to avoid Non-Nanite job queue overflow
	constexpr int32 MaxLODRemeshPerFrame = 1;

	// Count how many chunks are already pending remesh
	int32 CurrentPendingRemesh = 0;
	for (const FChunkLODRequest& Existing : MeshingQueue)
	{
		if (FVoxelChunkState* State = ChunkStates.Find(Existing.ChunkCoord))
		{
			if (State->State == EChunkState::PendingMeshing)
			{
				++CurrentPendingRemesh;
			}
		}
	}

	// Batch update morph factors
	TArray<TPair<FIntVector, float>> Transitions;

	// Track chunks that need remeshing due to LOD level changes, with priority info
	struct FLODRemeshCandidate
	{
		FIntVector ChunkCoord;
		int32 NewLODLevel;
		float Distance;
		bool bIsUpgrade; // true if going to finer LOD (higher priority)
	};
	TArray<FLODRemeshCandidate> RemeshCandidates;

	for (const FIntVector& ChunkCoord : LoadedChunkCoords)
	{
		const float NewMorphFactor = LODStrategy->GetLODMorphFactor(ChunkCoord, Context);
		const int32 NewLODLevel = LODStrategy->GetLODForChunk(ChunkCoord, Context);

		if (FVoxelChunkState* State = ChunkStates.Find(ChunkCoord))
		{
			// Check if LOD level changed - this requires remeshing
			if (State->LODLevel != NewLODLevel)
			{
				// Calculate distance for prioritization (includes WorldOrigin)
				const float ChunkWorldSize = Configuration ? Configuration->GetChunkWorldSize() : 3200.0f;
				const FVector WorldOrigin = Configuration ? Configuration->WorldOrigin : FVector::ZeroVector;
				const FVector ChunkCenter = WorldOrigin + FVector(ChunkCoord) * ChunkWorldSize + FVector(ChunkWorldSize * 0.5f);
				const float Distance = FVector::Dist(ChunkCenter, Context.ViewerPosition);

				FLODRemeshCandidate Candidate;
				Candidate.ChunkCoord = ChunkCoord;
				Candidate.NewLODLevel = NewLODLevel;
				Candidate.Distance = Distance;
				Candidate.bIsUpgrade = NewLODLevel < State->LODLevel; // Lower LOD number = higher detail

				RemeshCandidates.Add(Candidate);
			}

			// Update morph factor
			if (FMath::Abs(State->MorphFactor - NewMorphFactor) > 0.01f)
			{
				State->MorphFactor = NewMorphFactor;
				Transitions.Add(TPair<FIntVector, float>(ChunkCoord, NewMorphFactor));
			}
		}
	}

	// Sort candidates: prioritize LOD upgrades (finer detail), then by distance (closer first)
	RemeshCandidates.Sort([](const FLODRemeshCandidate& A, const FLODRemeshCandidate& B)
	{
		// Upgrades (finer LOD) take priority over downgrades
		if (A.bIsUpgrade != B.bIsUpgrade)
		{
			return A.bIsUpgrade; // true (upgrade) comes before false (downgrade)
		}
		// Within same category, closer chunks first
		return A.Distance < B.Distance;
	});

	// Queue limited number of remeshes per frame
	int32 QueuedThisFrame = 0;
	const int32 MaxToQueue = FMath::Max(0, MaxLODRemeshPerFrame - CurrentPendingRemesh);

	for (const FLODRemeshCandidate& Candidate : RemeshCandidates)
	{
		if (QueuedThisFrame >= MaxToQueue)
		{
			break;
		}

		if (FVoxelChunkState* State = ChunkStates.Find(Candidate.ChunkCoord))
		{
			// Only queue if chunk is in Loaded state (not already being processed)
			if (State->State == EChunkState::Loaded)
			{
				// Update the stored LOD level
				State->LODLevel = Candidate.NewLODLevel;

				FChunkLODRequest Request;
				Request.ChunkCoord = Candidate.ChunkCoord;
				Request.LODLevel = Candidate.NewLODLevel;
				// Higher priority for upgrades and closer chunks
				Request.Priority = (Candidate.bIsUpgrade ? 100.0f : 50.0f) + (10000.0f / FMath::Max(Candidate.Distance, 1.0f));

				// O(1) duplicate check + sorted insertion
				if (AddToMeshingQueue(Request))
				{
					// IMPORTANT: Set state to PendingMeshing so ProcessMeshingQueue will process it
					SetChunkState(Candidate.ChunkCoord, EChunkState::PendingMeshing);
					++QueuedThisFrame;

					UE_LOG(LogVoxelStreaming, Verbose, TEXT("Queued chunk (%d,%d,%d) for LOD %s: %d (dist=%.0f)"),
						Candidate.ChunkCoord.X, Candidate.ChunkCoord.Y, Candidate.ChunkCoord.Z,
						Candidate.bIsUpgrade ? TEXT("upgrade") : TEXT("downgrade"),
						Candidate.NewLODLevel, Candidate.Distance);
				}
			}
		}
	}

	// Log if we queued any LOD transitions
	if (QueuedThisFrame > 0)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("LOD transitions: %d candidates, queued %d this frame (pending: %d)"),
			RemeshCandidates.Num(), QueuedThisFrame, CurrentPendingRemesh + QueuedThisFrame);
	}

	if (Transitions.Num() > 0)
	{
		MeshRenderer->UpdateLODTransitionsBatch(Transitions);
	}
}

// ==================== Chunk State Management ====================

FVoxelChunkState& UVoxelChunkManager::GetOrCreateChunkState(const FIntVector& ChunkCoord)
{
	if (FVoxelChunkState* Existing = ChunkStates.Find(ChunkCoord))
	{
		return *Existing;
	}

	FVoxelChunkState NewState(ChunkCoord);
	NewState.Descriptor.ChunkSize = Configuration ? Configuration->ChunkSize : VOXEL_DEFAULT_CHUNK_SIZE;

	return ChunkStates.Add(ChunkCoord, MoveTemp(NewState));
}

void UVoxelChunkManager::SetChunkState(const FIntVector& ChunkCoord, EChunkState NewState)
{
	if (FVoxelChunkState* State = ChunkStates.Find(ChunkCoord))
	{
		State->State = NewState;
		State->Descriptor.State = NewState;
		State->LastStateChangeFrame = CurrentFrame;
	}
}

void UVoxelChunkManager::RemoveChunkState(const FIntVector& ChunkCoord)
{
	ChunkStates.Remove(ChunkCoord);
}

// ==================== Generation/Meshing Callbacks ====================

void UVoxelChunkManager::OnChunkGenerationComplete(const FIntVector& ChunkCoord)
{
	FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
	if (!State || State->State != EChunkState::Generating)
	{
		return;
	}

	++TotalChunksGenerated;

	// Queue for meshing with sorted insertion
	FChunkLODRequest Request;
	Request.ChunkCoord = ChunkCoord;
	Request.LODLevel = State->LODLevel;
	Request.Priority = State->Priority;

	AddToMeshingQueue(Request);
	SetChunkState(ChunkCoord, EChunkState::PendingMeshing);

	// Queue neighbors for remeshing so they can incorporate this chunk's edge data
	// This ensures seamless boundaries when chunks load in different orders
	QueueNeighborsForRemesh(ChunkCoord);

	// Fire event
	OnChunkGenerated.Broadcast(ChunkCoord);

	UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d, %d, %d) generation complete"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
}

void UVoxelChunkManager::OnChunkMeshingComplete(const FIntVector& ChunkCoord)
{
	FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
	if (!State || State->State != EChunkState::Meshing)
	{
		return;
	}

	++TotalChunksMeshed;

	// Find mesh in pending queue
	int32 PendingIndex = INDEX_NONE;
	for (int32 i = 0; i < PendingMeshQueue.Num(); ++i)
	{
		if (PendingMeshQueue[i].ChunkCoord == ChunkCoord)
		{
			PendingIndex = i;
			break;
		}
	}

	if (PendingIndex != INDEX_NONE && MeshRenderer)
	{
		const FPendingMeshData& PendingMesh = PendingMeshQueue[PendingIndex];

		// Send mesh to renderer
		MeshRenderer->UpdateChunkMeshFromCPU(
			ChunkCoord,
			PendingMesh.LODLevel,
			PendingMesh.MeshData
		);

		// Remove from pending queue
		PendingMeshQueue.RemoveAt(PendingIndex);
	}

	// Mark as loaded
	LoadedChunkCoords.Add(ChunkCoord);
	State->Descriptor.bIsDirty = false;
	SetChunkState(ChunkCoord, EChunkState::Loaded);

	// Fire event
	OnChunkLoaded.Broadcast(ChunkCoord);

	UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d, %d, %d) loaded"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
}

void UVoxelChunkManager::QueueNeighborsForRemesh(const FIntVector& ChunkCoord)
{
	// For Marching Cubes, we need to remesh all 26 neighbors (faces, edges, corners)
	// because diagonal chunks may use our voxel data at their boundaries.
	// Build list of all neighbor offsets: 6 faces + 12 edges + 8 corners = 26 total
	static const FIntVector NeighborOffsets[26] = {
		// 6 Face neighbors
		FIntVector(1, 0, 0),   FIntVector(-1, 0, 0),
		FIntVector(0, 1, 0),   FIntVector(0, -1, 0),
		FIntVector(0, 0, 1),   FIntVector(0, 0, -1),
		// 12 Edge neighbors
		FIntVector(1, 1, 0),   FIntVector(1, -1, 0),   FIntVector(-1, 1, 0),   FIntVector(-1, -1, 0),
		FIntVector(1, 0, 1),   FIntVector(1, 0, -1),   FIntVector(-1, 0, 1),   FIntVector(-1, 0, -1),
		FIntVector(0, 1, 1),   FIntVector(0, 1, -1),   FIntVector(0, -1, 1),   FIntVector(0, -1, -1),
		// 8 Corner neighbors
		FIntVector(1, 1, 1),   FIntVector(1, 1, -1),   FIntVector(1, -1, 1),   FIntVector(1, -1, -1),
		FIntVector(-1, 1, 1),  FIntVector(-1, 1, -1),  FIntVector(-1, -1, 1),  FIntVector(-1, -1, -1)
	};

	// Check each neighbor
	for (int32 i = 0; i < 26; ++i)
	{
		const FIntVector NeighborCoord = ChunkCoord + NeighborOffsets[i];

		FVoxelChunkState* NeighborState = ChunkStates.Find(NeighborCoord);
		if (!NeighborState)
		{
			continue;
		}

		// Only remesh neighbors that are already in Loaded state
		// Neighbors in earlier states will get correct data during their initial meshing
		if (NeighborState->State == EChunkState::Loaded)
		{
			// Queue for remeshing - lower priority since it's a refinement
			FChunkLODRequest Request;
			Request.ChunkCoord = NeighborCoord;
			Request.LODLevel = NeighborState->LODLevel;
			Request.Priority = NeighborState->Priority * 0.5f; // Lower priority than new chunks

			// O(1) duplicate check + sorted insertion
			if (AddToMeshingQueue(Request))
			{
				SetChunkState(NeighborCoord, EChunkState::PendingMeshing);

				UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d, %d, %d) queued for remesh (neighbor of %d, %d, %d)"),
					NeighborCoord.X, NeighborCoord.Y, NeighborCoord.Z,
					ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
			}
		}
	}
}

void UVoxelChunkManager::ExtractNeighborEdgeSlices(const FIntVector& ChunkCoord, FVoxelMeshingRequest& OutRequest)
{
	if (!Configuration)
	{
		return;
	}

	const int32 ChunkSize = Configuration->ChunkSize;
	const int32 SliceSize = ChunkSize * ChunkSize;
	const int32 VolumeSize = ChunkSize * ChunkSize * ChunkSize;

	// Reset edge/corner flags
	OutRequest.EdgeCornerFlags = 0;

	// Helper lambda to get voxel data from a neighbor chunk
	auto GetNeighborVoxels = [this, VolumeSize](const FIntVector& NeighborCoord) -> const TArray<FVoxelData>*
	{
		const FVoxelChunkState* NeighborState = ChunkStates.Find(NeighborCoord);
		if (!NeighborState)
		{
			return nullptr;
		}
		const TArray<FVoxelData>& Voxels = NeighborState->Descriptor.VoxelData;
		if (Voxels.Num() != VolumeSize)
		{
			return nullptr;
		}
		return &Voxels;
	};

	// Helper to get voxel index
	auto GetIndex = [ChunkSize](int32 X, int32 Y, int32 Z) -> int32
	{
		return X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
	};

	// ==================== Extract Face Neighbors ====================

	// +X neighbor
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(1, 0, 0)))
	{
		OutRequest.NeighborXPos.SetNumUninitialized(SliceSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < ChunkSize; ++Y)
			{
				OutRequest.NeighborXPos[Y + Z * ChunkSize] = (*Voxels)[GetIndex(0, Y, Z)];
			}
		}
	}

	// -X neighbor
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(-1, 0, 0)))
	{
		OutRequest.NeighborXNeg.SetNumUninitialized(SliceSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < ChunkSize; ++Y)
			{
				OutRequest.NeighborXNeg[Y + Z * ChunkSize] = (*Voxels)[GetIndex(ChunkSize - 1, Y, Z)];
			}
		}
	}

	// +Y neighbor
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(0, 1, 0)))
	{
		OutRequest.NeighborYPos.SetNumUninitialized(SliceSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				OutRequest.NeighborYPos[X + Z * ChunkSize] = (*Voxels)[GetIndex(X, 0, Z)];
			}
		}
	}

	// -Y neighbor
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(0, -1, 0)))
	{
		OutRequest.NeighborYNeg.SetNumUninitialized(SliceSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				OutRequest.NeighborYNeg[X + Z * ChunkSize] = (*Voxels)[GetIndex(X, ChunkSize - 1, Z)];
			}
		}
	}

	// +Z neighbor
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(0, 0, 1)))
	{
		OutRequest.NeighborZPos.SetNumUninitialized(SliceSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				OutRequest.NeighborZPos[X + Y * ChunkSize] = (*Voxels)[GetIndex(X, Y, 0)];
			}
		}
	}

	// -Z neighbor
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(0, 0, -1)))
	{
		OutRequest.NeighborZNeg.SetNumUninitialized(SliceSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				OutRequest.NeighborZNeg[X + Y * ChunkSize] = (*Voxels)[GetIndex(X, Y, ChunkSize - 1)];
			}
		}
	}

	// ==================== Extract Edge Neighbors (for Marching Cubes) ====================

	// Edge X+Y+ (diagonal chunk at +X+Y, extract X=0, Y=0, Z varies)
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(1, 1, 0)))
	{
		OutRequest.EdgeXPosYPos.SetNumUninitialized(ChunkSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			OutRequest.EdgeXPosYPos[Z] = (*Voxels)[GetIndex(0, 0, Z)];
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XPOS_YPOS;
	}

	// Edge X+Y- (diagonal chunk at +X-Y, extract X=0, Y=ChunkSize-1, Z varies)
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(1, -1, 0)))
	{
		OutRequest.EdgeXPosYNeg.SetNumUninitialized(ChunkSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			OutRequest.EdgeXPosYNeg[Z] = (*Voxels)[GetIndex(0, ChunkSize - 1, Z)];
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XPOS_YNEG;
	}

	// Edge X-Y+ (diagonal chunk at -X+Y, extract X=ChunkSize-1, Y=0, Z varies)
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(-1, 1, 0)))
	{
		OutRequest.EdgeXNegYPos.SetNumUninitialized(ChunkSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			OutRequest.EdgeXNegYPos[Z] = (*Voxels)[GetIndex(ChunkSize - 1, 0, Z)];
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XNEG_YPOS;
	}

	// Edge X-Y- (diagonal chunk at -X-Y, extract X=ChunkSize-1, Y=ChunkSize-1, Z varies)
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(-1, -1, 0)))
	{
		OutRequest.EdgeXNegYNeg.SetNumUninitialized(ChunkSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			OutRequest.EdgeXNegYNeg[Z] = (*Voxels)[GetIndex(ChunkSize - 1, ChunkSize - 1, Z)];
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XNEG_YNEG;
	}

	// Edge X+Z+ (diagonal chunk at +X+Z, extract X=0, Z=0, Y varies)
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(1, 0, 1)))
	{
		OutRequest.EdgeXPosZPos.SetNumUninitialized(ChunkSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			OutRequest.EdgeXPosZPos[Y] = (*Voxels)[GetIndex(0, Y, 0)];
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XPOS_ZPOS;
	}

	// Edge X+Z- (diagonal chunk at +X-Z, extract X=0, Z=ChunkSize-1, Y varies)
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(1, 0, -1)))
	{
		OutRequest.EdgeXPosZNeg.SetNumUninitialized(ChunkSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			OutRequest.EdgeXPosZNeg[Y] = (*Voxels)[GetIndex(0, Y, ChunkSize - 1)];
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XPOS_ZNEG;
	}

	// Edge X-Z+ (diagonal chunk at -X+Z, extract X=ChunkSize-1, Z=0, Y varies)
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(-1, 0, 1)))
	{
		OutRequest.EdgeXNegZPos.SetNumUninitialized(ChunkSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			OutRequest.EdgeXNegZPos[Y] = (*Voxels)[GetIndex(ChunkSize - 1, Y, 0)];
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XNEG_ZPOS;
	}

	// Edge X-Z- (diagonal chunk at -X-Z, extract X=ChunkSize-1, Z=ChunkSize-1, Y varies)
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(-1, 0, -1)))
	{
		OutRequest.EdgeXNegZNeg.SetNumUninitialized(ChunkSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			OutRequest.EdgeXNegZNeg[Y] = (*Voxels)[GetIndex(ChunkSize - 1, Y, ChunkSize - 1)];
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XNEG_ZNEG;
	}

	// Edge Y+Z+ (diagonal chunk at +Y+Z, extract Y=0, Z=0, X varies)
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(0, 1, 1)))
	{
		OutRequest.EdgeYPosZPos.SetNumUninitialized(ChunkSize);
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			OutRequest.EdgeYPosZPos[X] = (*Voxels)[GetIndex(X, 0, 0)];
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_YPOS_ZPOS;
	}

	// Edge Y+Z- (diagonal chunk at +Y-Z, extract Y=0, Z=ChunkSize-1, X varies)
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(0, 1, -1)))
	{
		OutRequest.EdgeYPosZNeg.SetNumUninitialized(ChunkSize);
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			OutRequest.EdgeYPosZNeg[X] = (*Voxels)[GetIndex(X, 0, ChunkSize - 1)];
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_YPOS_ZNEG;
	}

	// Edge Y-Z+ (diagonal chunk at -Y+Z, extract Y=ChunkSize-1, Z=0, X varies)
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(0, -1, 1)))
	{
		OutRequest.EdgeYNegZPos.SetNumUninitialized(ChunkSize);
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			OutRequest.EdgeYNegZPos[X] = (*Voxels)[GetIndex(X, ChunkSize - 1, 0)];
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_YNEG_ZPOS;
	}

	// Edge Y-Z- (diagonal chunk at -Y-Z, extract Y=ChunkSize-1, Z=ChunkSize-1, X varies)
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(0, -1, -1)))
	{
		OutRequest.EdgeYNegZNeg.SetNumUninitialized(ChunkSize);
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			OutRequest.EdgeYNegZNeg[X] = (*Voxels)[GetIndex(X, ChunkSize - 1, ChunkSize - 1)];
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_YNEG_ZNEG;
	}

	// ==================== Extract Corner Neighbors (for Marching Cubes) ====================

	// Corner X+Y+Z+ (diagonal chunk at +X+Y+Z, extract voxel at 0,0,0)
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(1, 1, 1)))
	{
		OutRequest.CornerXPosYPosZPos = (*Voxels)[GetIndex(0, 0, 0)];
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZPOS;
	}

	// Corner X+Y+Z- (diagonal chunk at +X+Y-Z, extract voxel at 0,0,ChunkSize-1)
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(1, 1, -1)))
	{
		OutRequest.CornerXPosYPosZNeg = (*Voxels)[GetIndex(0, 0, ChunkSize - 1)];
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZNEG;
	}

	// Corner X+Y-Z+ (diagonal chunk at +X-Y+Z, extract voxel at 0,ChunkSize-1,0)
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(1, -1, 1)))
	{
		OutRequest.CornerXPosYNegZPos = (*Voxels)[GetIndex(0, ChunkSize - 1, 0)];
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZPOS;
	}

	// Corner X+Y-Z- (diagonal chunk at +X-Y-Z, extract voxel at 0,ChunkSize-1,ChunkSize-1)
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(1, -1, -1)))
	{
		OutRequest.CornerXPosYNegZNeg = (*Voxels)[GetIndex(0, ChunkSize - 1, ChunkSize - 1)];
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZNEG;
	}

	// Corner X-Y+Z+ (diagonal chunk at -X+Y+Z, extract voxel at ChunkSize-1,0,0)
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(-1, 1, 1)))
	{
		OutRequest.CornerXNegYPosZPos = (*Voxels)[GetIndex(ChunkSize - 1, 0, 0)];
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZPOS;
	}

	// Corner X-Y+Z- (diagonal chunk at -X+Y-Z, extract voxel at ChunkSize-1,0,ChunkSize-1)
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(-1, 1, -1)))
	{
		OutRequest.CornerXNegYPosZNeg = (*Voxels)[GetIndex(ChunkSize - 1, 0, ChunkSize - 1)];
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZNEG;
	}

	// Corner X-Y-Z+ (diagonal chunk at -X-Y+Z, extract voxel at ChunkSize-1,ChunkSize-1,0)
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(-1, -1, 1)))
	{
		OutRequest.CornerXNegYNegZPos = (*Voxels)[GetIndex(ChunkSize - 1, ChunkSize - 1, 0)];
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZPOS;
	}

	// Corner X-Y-Z- (diagonal chunk at -X-Y-Z, extract voxel at ChunkSize-1,ChunkSize-1,ChunkSize-1)
	if (const TArray<FVoxelData>* Voxels = GetNeighborVoxels(ChunkCoord + FIntVector(-1, -1, -1)))
	{
		OutRequest.CornerXNegYNegZNeg = (*Voxels)[GetIndex(ChunkSize - 1, ChunkSize - 1, ChunkSize - 1)];
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZNEG;
	}
}

// ==================== Queue Management ====================

bool UVoxelChunkManager::AddToGenerationQueue(const FChunkLODRequest& Request)
{
	// O(1) duplicate check
	if (GenerationQueueSet.Contains(Request.ChunkCoord))
	{
		return false;
	}

	// Add to tracking set
	GenerationQueueSet.Add(Request.ChunkCoord);

	// Binary search for sorted insertion (highest priority first)
	// FChunkLODRequest::operator< returns true if this.Priority > Other.Priority
	int32 InsertIndex = Algo::LowerBound(GenerationQueue, Request);
	GenerationQueue.Insert(Request, InsertIndex);

	return true;
}

bool UVoxelChunkManager::AddToMeshingQueue(const FChunkLODRequest& Request)
{
	// O(1) duplicate check
	if (MeshingQueueSet.Contains(Request.ChunkCoord))
	{
		return false;
	}

	// Add to tracking set
	MeshingQueueSet.Add(Request.ChunkCoord);

	// Binary search for sorted insertion (highest priority first)
	int32 InsertIndex = Algo::LowerBound(MeshingQueue, Request);
	MeshingQueue.Insert(Request, InsertIndex);

	return true;
}

bool UVoxelChunkManager::AddToUnloadQueue(const FIntVector& ChunkCoord)
{
	// O(1) duplicate check
	if (UnloadQueueSet.Contains(ChunkCoord))
	{
		return false;
	}

	// Add to tracking set and queue
	UnloadQueueSet.Add(ChunkCoord);
	UnloadQueue.Add(ChunkCoord);

	return true;
}

void UVoxelChunkManager::RemoveFromGenerationQueue(const FIntVector& ChunkCoord)
{
	// Remove from tracking set
	GenerationQueueSet.Remove(ChunkCoord);

	// Remove from queue (linear search, but only called when processing)
	for (int32 i = 0; i < GenerationQueue.Num(); ++i)
	{
		if (GenerationQueue[i].ChunkCoord == ChunkCoord)
		{
			GenerationQueue.RemoveAt(i);
			return;
		}
	}
}

void UVoxelChunkManager::RemoveFromMeshingQueue(const FIntVector& ChunkCoord)
{
	// Remove from tracking set
	MeshingQueueSet.Remove(ChunkCoord);

	// Remove from queue (linear search, but only called when processing)
	for (int32 i = 0; i < MeshingQueue.Num(); ++i)
	{
		if (MeshingQueue[i].ChunkCoord == ChunkCoord)
		{
			MeshingQueue.RemoveAt(i);
			return;
		}
	}
}

void UVoxelChunkManager::RemoveFromUnloadQueue(const FIntVector& ChunkCoord)
{
	// Remove from tracking set
	UnloadQueueSet.Remove(ChunkCoord);

	// Remove from queue
	UnloadQueue.Remove(ChunkCoord);
}
