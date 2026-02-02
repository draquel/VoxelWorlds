// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelChunkManager.h"
#include "VoxelStreaming.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelCoordinates.h"
#include "IVoxelLODStrategy.h"
#include "IVoxelMeshRenderer.h"
#include "VoxelNoiseTypes.h"
#include "VoxelCPUCubicMesher.h"
#include "VoxelCPUSmoothMesher.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "DrawDebugHelpers.h"

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

	// Update LOD strategy
	if (LODStrategy)
	{
		LODStrategy->Update(Context, DeltaTime);
	}

	// Update streaming decisions
	UpdateStreamingDecisions(Context);

	// Process queues (time-sliced)
	const float TimeSlice = Configuration ? Configuration->StreamingTimeSliceMS : 2.0f;
	ProcessGenerationQueue(TimeSlice * 0.4f);
	ProcessMeshingQueue(TimeSlice * 0.4f);

	// Process unloads
	const int32 MaxUnloads = Configuration ? Configuration->MaxChunksToUnloadPerFrame : 8;
	ProcessUnloadQueue(MaxUnloads);

	// Update LOD transitions
	UpdateLODTransitions(Context);
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

	// Initialize LOD strategy
	if (LODStrategy)
	{
		LODStrategy->Initialize(Configuration);
	}

	// Clear any existing state
	ChunkStates.Empty();
	LoadedChunkCoords.Empty();
	GenerationQueue.Empty();
	MeshingQueue.Empty();
	UnloadQueue.Empty();

	// Reset statistics
	TotalChunksGenerated = 0;
	TotalChunksMeshed = 0;
	TotalChunksUnloaded = 0;
	CurrentFrame = 0;

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
		SmoothMesher->SetConfig(MeshConfig);

		Mesher = MoveTemp(SmoothMesher);
		UE_LOG(LogVoxelStreaming, Log, TEXT("Using Smooth (Marching Cubes) mesher"));
	}
	else
	{
		Mesher = MakeUnique<FVoxelCPUCubicMesher>();
		Mesher->Initialize();
		UE_LOG(LogVoxelStreaming, Log, TEXT("Using Cubic mesher"));
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
	MeshingQueue.Empty();
	UnloadQueue.Empty();

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

	FLODQueryContext Context = BuildQueryContext();

	if (LODStrategy)
	{
		LODStrategy->Update(Context, 0.0f);
	}

	UpdateStreamingDecisions(Context);
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
		// Add to generation queue
		FChunkLODRequest Request;
		Request.ChunkCoord = ChunkCoord;
		Request.LODLevel = 0; // Will be determined by LOD strategy
		Request.Priority = Priority;

		GenerationQueue.Add(Request);
		GenerationQueue.Sort();

		SetChunkState(ChunkCoord, EChunkState::PendingGeneration);

		UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d, %d, %d) requested for loading"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
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
			UnloadQueue.Add(ChunkCoord);
			SetChunkState(ChunkCoord, EChunkState::PendingUnload);

			UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d, %d, %d) requested for unloading"),
				ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
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

			// Add to meshing queue for remeshing
			FChunkLODRequest Request;
			Request.ChunkCoord = ChunkCoord;
			Request.LODLevel = State->LODLevel;
			Request.Priority = 100.0f; // High priority for dirty chunks

			MeshingQueue.Add(Request);
			MeshingQueue.Sort();

			SetChunkState(ChunkCoord, EChunkState::PendingMeshing);
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

		const FBox Bounds = FVoxelCoordinates::ChunkToWorldBounds(
			ChunkCoord,
			Configuration->ChunkSize,
			Configuration->VoxelSize
		);

		DrawDebugBox(GetWorld(), Bounds.GetCenter(), Bounds.GetExtent(), Color, false, -1.0f, 0, 2.0f);
	}
#endif
}

// ==================== Internal Update Methods ====================

FLODQueryContext UVoxelChunkManager::BuildQueryContext() const
{
	FLODQueryContext Context;

	// Get viewer state from player controller
	if (UWorld* World = GetWorld())
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			FVector Location;
			FRotator Rotation;
			PC->GetPlayerViewPoint(Location, Rotation);

			Context.ViewerPosition = Location;
			Context.ViewerForward = Rotation.Vector();
			Context.ViewerRight = Rotation.RotateVector(FVector::RightVector);
			Context.ViewerUp = Rotation.RotateVector(FVector::UpVector);

			if (PC->PlayerCameraManager)
			{
				Context.FieldOfView = PC->PlayerCameraManager->GetFOVAngle();
			}
		}

		Context.GameTime = World->GetTimeSeconds();
		Context.DeltaTime = World->GetDeltaSeconds();
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

	// Debug: Log streaming decisions periodically
	static int32 DebugFrameCounter = 0;
	if (++DebugFrameCounter % 60 == 0)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("Streaming: Viewer at (%.0f, %.0f, %.0f), ChunksToLoad=%d, Loaded=%d, GenQueue=%d"),
			Context.ViewerPosition.X, Context.ViewerPosition.Y, Context.ViewerPosition.Z,
			ChunksToLoad.Num(), LoadedChunkCoords.Num(), GenerationQueue.Num());
	}

	// Add to generation queue (avoiding duplicates)
	for (const FChunkLODRequest& Request : ChunksToLoad)
	{
		const EChunkState CurrentState = GetChunkState(Request.ChunkCoord);

		if (CurrentState == EChunkState::Unloaded)
		{
			FVoxelChunkState& State = GetOrCreateChunkState(Request.ChunkCoord);
			State.LODLevel = Request.LODLevel;
			State.Priority = Request.Priority;

			GenerationQueue.Add(Request);
			SetChunkState(Request.ChunkCoord, EChunkState::PendingGeneration);
		}
	}

	// Sort generation queue by priority
	GenerationQueue.Sort();

	// Get chunks to unload
	TArray<FIntVector> ChunksToUnload;
	LODStrategy->GetChunksToUnload(ChunksToUnload, LoadedChunkCoords, Context);

	// Add to unload queue
	for (const FIntVector& ChunkCoord : ChunksToUnload)
	{
		const EChunkState CurrentState = GetChunkState(ChunkCoord);

		if (CurrentState == EChunkState::Loaded)
		{
			UnloadQueue.Add(ChunkCoord);
			SetChunkState(ChunkCoord, EChunkState::PendingUnload);
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

		// Get highest priority chunk
		FChunkLODRequest Request = GenerationQueue[0];
		GenerationQueue.RemoveAt(0);

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

		// Get highest priority chunk
		FChunkLODRequest Request = MeshingQueue[0];
		MeshingQueue.RemoveAt(0);

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
		MeshRequest.VoxelData = State->Descriptor.VoxelData;

		// Extract neighbor edge slices for seamless boundaries
		ExtractNeighborEdgeSlices(Request.ChunkCoord, MeshRequest);

		// Generate mesh using CPU mesher
		FChunkMeshData MeshData;
		const bool bSuccess = Mesher->GenerateMeshCPU(MeshRequest, MeshData);

		if (bSuccess)
		{
			// Store mesh in pending queue
			FPendingMeshData PendingMesh;
			PendingMesh.ChunkCoord = Request.ChunkCoord;
			PendingMesh.LODLevel = Request.LODLevel;
			PendingMesh.MeshData = MoveTemp(MeshData);
			PendingMeshQueue.Add(MoveTemp(PendingMesh));

			OnChunkMeshingComplete(Request.ChunkCoord);
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
		FIntVector ChunkCoord = UnloadQueue[0];
		UnloadQueue.RemoveAt(0);

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

	// Batch update morph factors
	TArray<TPair<FIntVector, float>> Transitions;

	for (const FIntVector& ChunkCoord : LoadedChunkCoords)
	{
		const float NewMorphFactor = LODStrategy->GetLODMorphFactor(ChunkCoord, Context);

		if (FVoxelChunkState* State = ChunkStates.Find(ChunkCoord))
		{
			if (FMath::Abs(State->MorphFactor - NewMorphFactor) > 0.01f)
			{
				State->MorphFactor = NewMorphFactor;
				Transitions.Add(TPair<FIntVector, float>(ChunkCoord, NewMorphFactor));
			}
		}
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

	// Queue for meshing
	FChunkLODRequest Request;
	Request.ChunkCoord = ChunkCoord;
	Request.LODLevel = State->LODLevel;
	Request.Priority = State->Priority;

	MeshingQueue.Add(Request);
	MeshingQueue.Sort();

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

			// Check if not already in meshing queue
			bool bAlreadyQueued = false;
			for (const FChunkLODRequest& Existing : MeshingQueue)
			{
				if (Existing.ChunkCoord == NeighborCoord)
				{
					bAlreadyQueued = true;
					break;
				}
			}

			if (!bAlreadyQueued)
			{
				MeshingQueue.Add(Request);
				SetChunkState(NeighborCoord, EChunkState::PendingMeshing);

				UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d, %d, %d) queued for remesh (neighbor of %d, %d, %d)"),
					NeighborCoord.X, NeighborCoord.Y, NeighborCoord.Z,
					ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
			}
		}
	}

	// Re-sort the meshing queue after adding potential remesh requests
	MeshingQueue.Sort();
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
