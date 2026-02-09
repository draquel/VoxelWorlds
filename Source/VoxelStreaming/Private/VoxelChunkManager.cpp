// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelChunkManager.h"
#include "VoxelStreaming.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelBiomeConfiguration.h"
#include "Algo/BinarySearch.h"
#include "Async/Async.h"
#include "VoxelCoordinates.h"
#include "IVoxelLODStrategy.h"
#include "IVoxelMeshRenderer.h"
#include "VoxelNoiseTypes.h"
#include "VoxelCPUCubicMesher.h"
#include "VoxelCPUSmoothMesher.h"
#include "VoxelEditManager.h"
#include "VoxelEditTypes.h"
#include "VoxelCollisionManager.h"
#include "VoxelScatterManager.h"
#include "VoxelTreeInjector.h"
#include "VoxelTreeTypes.h"
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

	const double TickStartTime = FPlatformTime::Seconds();
	FVoxelTimingStats Timing;

	// === Adaptive Throttling: update smoothed frame time ===
	{
		const float FrameTimeMs = DeltaTime * 1000.0f;
		constexpr float Alpha = 0.1f;
		SmoothedFrameTimeMs = SmoothedFrameTimeMs + Alpha * (FrameTimeMs - SmoothedFrameTimeMs);

		const float TargetFPS = Configuration ? Configuration->TargetFrameRate : 60.0f;
		const bool bAdaptive = Configuration ? Configuration->bAdaptiveThrottling : true;
		const int32 ConfigMaxAsyncGen = Configuration ? Configuration->MaxAsyncGenerationTasks : 2;
		const int32 ConfigMaxAsync = Configuration ? Configuration->MaxAsyncMeshTasks : 4;
		const int32 ConfigMaxLODRemesh = Configuration ? Configuration->MaxLODRemeshPerFrame : 4;
		const int32 ConfigMaxPending = Configuration ? Configuration->MaxPendingMeshes : 4;

		if (bAdaptive && TargetFPS > 0.0f)
		{
			const float TargetMs = 1000.0f / TargetFPS;
			if (SmoothedFrameTimeMs > TargetMs * 1.2f)
			{
				// Over budget: halve effective limits (min 1)
				EffectiveMaxAsyncGenerationTasks = FMath::Max(1, ConfigMaxAsyncGen / 2);
				EffectiveMaxAsyncMeshTasks = FMath::Max(1, ConfigMaxAsync / 2);
				EffectiveMaxLODRemeshPerFrame = FMath::Max(1, ConfigMaxLODRemesh / 2);
				EffectiveMaxPendingMeshes = FMath::Max(2, ConfigMaxPending / 2);
			}
			else if (SmoothedFrameTimeMs < TargetMs * 0.8f)
			{
				// Under budget: restore configured limits
				EffectiveMaxAsyncGenerationTasks = ConfigMaxAsyncGen;
				EffectiveMaxAsyncMeshTasks = ConfigMaxAsync;
				EffectiveMaxLODRemeshPerFrame = ConfigMaxLODRemesh;
				EffectiveMaxPendingMeshes = ConfigMaxPending;
			}
			// else: in the 80-120% band, keep current values
		}
		else
		{
			EffectiveMaxAsyncGenerationTasks = ConfigMaxAsyncGen;
			EffectiveMaxAsyncMeshTasks = ConfigMaxAsync;
			EffectiveMaxLODRemeshPerFrame = ConfigMaxLODRemesh;
			EffectiveMaxPendingMeshes = ConfigMaxPending;
		}
	}

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
	const bool bViewerChunkChanged = (CurrentViewerChunk != CachedViewerChunk);
	const bool bNeedStreamingUpdate = bForceStreamingUpdate || bViewerChunkChanged;

	// Update LOD strategy (always update for morph factor interpolation)
	if (LODStrategy)
	{
		LODStrategy->Update(Context, DeltaTime);
	}

	// === Streaming decisions ===
	double SectionStart = FPlatformTime::Seconds();
	if (bNeedStreamingUpdate)
	{
		const bool bWasForced = bForceStreamingUpdate;
		bForceStreamingUpdate = false;

		UpdateLoadDecisions(Context);
		CachedViewerChunk = CurrentViewerChunk;
		LastStreamingUpdatePosition = Context.ViewerPosition;

		UE_LOG(LogVoxelStreaming, Verbose, TEXT("Streaming update: ViewerChunk=(%d,%d,%d), WasForced=%s, ContinueNextFrame=%s"),
			CurrentViewerChunk.X, CurrentViewerChunk.Y, CurrentViewerChunk.Z,
			bWasForced ? TEXT("Yes") : TEXT("No"),
			bForceStreamingUpdate ? TEXT("Yes") : TEXT("No"));
	}

	// ALWAYS update UNLOAD decisions (cheap operation, prevents orphaned chunks)
	UpdateUnloadDecisions(Context);

	// Re-prioritize queues when viewer moves to a new chunk
	// This ensures closest chunks are always processed first during fast movement
	// Also updates LOD levels for queued items so they mesh at the correct LOD
	if (bViewerChunkChanged)
	{
		ReprioritizeQueues(Context);
	}
	Timing.StreamingMs = static_cast<float>((FPlatformTime::Seconds() - SectionStart) * 1000.0);

	// === Generation queue (async launch + completed result processing) ===
	SectionStart = FPlatformTime::Seconds();
	const float TimeSlice = Configuration ? Configuration->StreamingTimeSliceMS : 2.0f;
	ProcessGenerationQueue(TimeSlice * 0.4f);
	ProcessCompletedAsyncGenerations();
	Timing.GenerationMs = static_cast<float>((FPlatformTime::Seconds() - SectionStart) * 1000.0);

	// === Meshing queue ===
	SectionStart = FPlatformTime::Seconds();
	ProcessMeshingQueue(TimeSlice * 0.4f);
	ProcessCompletedAsyncMeshes();
	Timing.MeshingMs = static_cast<float>((FPlatformTime::Seconds() - SectionStart) * 1000.0);

	// === Render submit ===
	SectionStart = FPlatformTime::Seconds();
	const int32 MaxRenderSubmitsPerFrame = Configuration ? Configuration->MaxChunksToLoadPerFrame : 8;
	if (PendingMeshQueue.Num() > 0)
	{
		int32 RenderSubmitCount = 0;
		while (PendingMeshQueue.Num() > 0 && RenderSubmitCount < MaxRenderSubmitsPerFrame)
		{
			const FIntVector ChunkCoord = PendingMeshQueue.Last().ChunkCoord;
			OnChunkMeshingComplete(ChunkCoord);
			++RenderSubmitCount;
		}
	}

	const int32 MaxUnloadsPerFrame = Configuration ? Configuration->MaxChunksToUnloadPerFrame : 8;
	ProcessUnloadQueue(MaxUnloadsPerFrame);
	Timing.RenderSubmitMs = static_cast<float>((FPlatformTime::Seconds() - SectionStart) * 1000.0);

	// === LOD level changes and morph factor updates ===
	SectionStart = FPlatformTime::Seconds();
	{
		// Detect when all queues drain — signal that a LOD sweep is needed
		const bool bQueuesEmpty = GenerationQueue.Num() == 0
			&& AsyncGenerationInProgress.Num() == 0
			&& MeshingQueue.Num() == 0
			&& AsyncMeshingInProgress.Num() == 0
			&& PendingMeshQueue.Num() == 0;

		if (bQueuesEmpty && !bPendingLODSweep)
		{
			bPendingLODSweep = true;
		}

		// LOD level evaluation: runs on viewer chunk change OR pending sweep
		if (bViewerChunkChanged || bPendingLODSweep)
		{
			EvaluateLODLevelChanges(Context);

			// Clear sweep flag only if queues are still empty after evaluation
			// (no new work was generated)
			if (bPendingLODSweep && bQueuesEmpty && MeshingQueue.Num() == 0)
			{
				bPendingLODSweep = false;
			}
		}

		// Morph factor updates: gated by movement threshold
		const float PositionDeltaSq = FVector::DistSquared(Context.ViewerPosition, LastLODUpdatePosition);
		if (bViewerChunkChanged || PositionDeltaSq > LODUpdateThresholdSq)
		{
			UpdateLODMorphFactors(Context);
			LastLODUpdatePosition = Context.ViewerPosition;
		}
	}
	Timing.LODMs = static_cast<float>((FPlatformTime::Seconds() - SectionStart) * 1000.0);

	// === Subsystem deferral check ===
	const int32 DeferThreshold = Configuration ? Configuration->DeferSubsystemsThreshold : 20;
	bSubsystemsDeferred = (DeferThreshold > 0 && GenerationQueue.Num() > DeferThreshold);

	// === Collision manager ===
	SectionStart = FPlatformTime::Seconds();
	if (!bSubsystemsDeferred && CollisionManager && Configuration && Configuration->bGenerateCollision)
	{
		CollisionManager->Update(Context.ViewerPosition, DeltaTime);
	}
	Timing.CollisionMs = static_cast<float>((FPlatformTime::Seconds() - SectionStart) * 1000.0);

	// === Scatter manager ===
	// Always update scatter (drains async results, updates HISM instances).
	// New task launches are throttled internally; skipping Update() entirely
	// when deferred causes async results to pile up and blocks new generations
	// once the generation queue drains.
	SectionStart = FPlatformTime::Seconds();
	if (ScatterManager && Configuration && Configuration->bEnableScatter)
	{
		ScatterManager->Update(Context.ViewerPosition, DeltaTime);

		if (Configuration->bScatterDebugVisualization)
		{
			ScatterManager->DrawDebugVisualization(GetWorld());
		}
	}
	Timing.ScatterMs = static_cast<float>((FPlatformTime::Seconds() - SectionStart) * 1000.0);

	// Flush all pending render operations as a single batched command
	if (MeshRenderer)
	{
		MeshRenderer->FlushPendingOperations();
	}

	Timing.TotalMs = static_cast<float>((FPlatformTime::Seconds() - TickStartTime) * 1000.0);
	LastTimingStats = Timing;
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
	bPendingLODSweep = false;

	// Reset adaptive throttle state
	SmoothedFrameTimeMs = 16.67f;
	bSubsystemsDeferred = false;
	EffectiveMaxAsyncGenerationTasks = Configuration->MaxAsyncGenerationTasks;
	EffectiveMaxAsyncMeshTasks = Configuration->MaxAsyncMeshTasks;
	EffectiveMaxLODRemeshPerFrame = Configuration->MaxLODRemeshPerFrame;
	EffectiveMaxPendingMeshes = Configuration->MaxPendingMeshes;
	LastTimingStats = FVoxelTimingStats();

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

	// Create edit manager
	EditManager = NewObject<UVoxelEditManager>(this);
	EditManager->Initialize(Configuration);

	// Subscribe to edit events - mark chunks dirty when edited
	EditManager->OnChunkEdited.AddLambda([this](const FIntVector& ChunkCoord, EEditSource Source, const FVector& EditCenter, float EditRadius)
	{
		// Mark the edited chunk dirty
		MarkChunkDirty(ChunkCoord);
		if (CollisionManager)
		{
			CollisionManager->MarkChunkDirty(ChunkCoord);
		}
		if (ScatterManager)
		{
			// Handle scatter based on edit source
			if (Source == EEditSource::Player && EditRadius > 0.0f)
			{
				// Player edits: surgically remove scatter in the affected radius only
				// Pad by half a VoxelSize so scatter on block faces above/around the
				// edit center is also cleared (block-face-snapped scatter sits at the
				// face center, which is 0.5*VoxelSize from the block center)
				const float ScatterClearRadius = EditRadius + Configuration->VoxelSize * 0.6f;
				ScatterManager->ClearScatterInRadius(EditCenter, ScatterClearRadius);
			}
			else if (Source != EEditSource::Player)
			{
				// System/Editor edits allow scatter to regenerate with new mesh
				ScatterManager->RegenerateChunkScatter(ChunkCoord);
			}
			// Note: Player edits with zero radius (undo/redo) don't need scatter handling
			// since the targeted removal already happened during the original edit
		}

		// Also mark neighboring chunks dirty so they re-extract boundary data
		// This ensures seamless edits across chunk borders
		static const FIntVector NeighborOffsets[6] = {
			FIntVector(-1, 0, 0), FIntVector(1, 0, 0),
			FIntVector(0, -1, 0), FIntVector(0, 1, 0),
			FIntVector(0, 0, -1), FIntVector(0, 0, 1)
		};

		for (const FIntVector& Offset : NeighborOffsets)
		{
			const FIntVector NeighborCoord = ChunkCoord + Offset;
			if (ChunkStates.Contains(NeighborCoord))
			{
				MarkChunkDirty(NeighborCoord);
				if (CollisionManager)
				{
					CollisionManager->MarkChunkDirty(NeighborCoord);
				}
				// Note: Scatter for neighbors is already handled by ClearScatterInRadius
				// which affects all chunks within the edit radius, not just the primary chunk
				if (ScatterManager && Source != EEditSource::Player)
				{
					ScatterManager->RegenerateChunkScatter(NeighborCoord);
				}
			}
		}
	});

	UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelEditManager created and initialized"));

	// Create collision manager if enabled
	if (Configuration->bGenerateCollision)
	{
		CollisionManager = NewObject<UVoxelCollisionManager>(this);
		CollisionManager->Initialize(Configuration, this);
		CollisionManager->SetCollisionRadius(Configuration->ViewDistance * 0.5f);
		CollisionManager->SetCollisionLODLevel(Configuration->CollisionLODLevel);

		CollisionManager->SetMaxAsyncCollisionTasks(Configuration->MaxAsyncCollisionTasks);

		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelCollisionManager created (Radius=%.0f, LOD=%d, MaxAsyncTasks=%d)"),
			Configuration->ViewDistance * 0.5f, Configuration->CollisionLODLevel, Configuration->MaxAsyncCollisionTasks);
	}

	// Create scatter manager if enabled
	if (Configuration->bEnableScatter)
	{
		ScatterManager = NewObject<UVoxelScatterManager>(this);
		ScatterManager->Initialize(Configuration, GetWorld());
		ScatterManager->SetScatterRadius(Configuration->ScatterRadius);
		ScatterManager->SetWorldSeed(static_cast<uint32>(Configuration->WorldSeed));

		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelScatterManager created (Radius=%.0f)"),
			Configuration->ScatterRadius);
	}

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

	// Clear async generation state
	// Note: In-flight async tasks will safely no-op due to weak pointer check
	AsyncGenerationInProgress.Empty();
	{
		FAsyncGenerationResult DiscardedGenResult;
		while (CompletedGenerationQueue.Dequeue(DiscardedGenResult)) {}
	}

	// Clear async meshing state
	AsyncMeshingInProgress.Empty();
	PendingMeshQueue.Empty();
	{
		FAsyncMeshResult DiscardedMeshResult;
		while (CompletedMeshQueue.Dequeue(DiscardedMeshResult)) {}
	}

	// Reset streaming decision caching
	CachedViewerChunk = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
	LastStreamingUpdatePosition = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
	LastLODUpdatePosition = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
	bForceStreamingUpdate = false;
	bPendingLODSweep = false;
	bSubsystemsDeferred = false;

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

	// Shutdown collision manager
	if (CollisionManager)
	{
		CollisionManager->Shutdown();
		CollisionManager = nullptr;
	}

	// Shutdown scatter manager
	if (ScatterManager)
	{
		ScatterManager->Shutdown();
		ScatterManager = nullptr;
	}

	// Shutdown edit manager
	if (EditManager)
	{
		EditManager->Shutdown();
		EditManager = nullptr;
	}

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

UVoxelChunkManager::FVoxelMemoryStats UVoxelChunkManager::GetVoxelMemoryStats() const
{
	FVoxelMemoryStats Stats;

	// Voxel data in chunk states
	for (const auto& Pair : ChunkStates)
	{
		Stats.VoxelDataBytes += Pair.Value.Descriptor.GetMemoryUsage();
	}

	// Edit manager
	if (EditManager)
	{
		Stats.EditDataBytes = static_cast<int64>(EditManager->GetMemoryUsage());
	}

	// Renderer
	if (MeshRenderer)
	{
		Stats.RendererCPUBytes = MeshRenderer->GetCPUMemoryUsage();
		Stats.RendererGPUBytes = MeshRenderer->GetGPUMemoryUsage();
	}

	// Collision
	if (CollisionManager)
	{
		Stats.CollisionBytes = CollisionManager->GetTotalMemoryUsage();
	}

	// Scatter
	if (ScatterManager)
	{
		Stats.ScatterBytes = ScatterManager->GetTotalMemoryUsage();
	}

	Stats.TotalBytes = Stats.VoxelDataBytes + Stats.EditDataBytes + Stats.RendererCPUBytes
		+ Stats.RendererGPUBytes + Stats.CollisionBytes + Stats.ScatterBytes;

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

void UVoxelChunkManager::UpdateLoadDecisions(const FLODQueryContext& Context)
{
	if (!LODStrategy)
	{
		return;
	}

	// Get chunks to load (expensive operation - iterates visible area)
	TArray<FChunkLODRequest> ChunksToLoad;
	LODStrategy->GetChunksToLoad(ChunksToLoad, LoadedChunkCoords, Context);

	// Limit how many chunks we add per frame to prevent overwhelming the queue
	// Use a higher limit to ensure view distance fills in reasonable time
	const int32 MaxChunksToAddPerFrame = Configuration->MaxChunksToLoadPerFrame * 4;
	int32 ChunksAddedThisFrame = 0;
	int32 ChunksRemaining = 0;

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
		const EChunkState CurrentState = GetChunkState(Request.ChunkCoord);

		if (CurrentState == EChunkState::Unloaded)
		{
			// Respect per-frame limit to prevent overwhelming the queue
			if (ChunksAddedThisFrame >= MaxChunksToAddPerFrame)
			{
				++ChunksRemaining;
				continue;  // Count remaining but don't add yet
			}

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

	// If we hit the limit and there are still chunks to add, force an update next frame
	if (ChunksRemaining > 0)
	{
		bForceStreamingUpdate = true;
		UE_LOG(LogVoxelStreaming, Verbose, TEXT("Streaming: %d chunks remaining, will continue next frame"), ChunksRemaining);
	}
}

void UVoxelChunkManager::UpdateUnloadDecisions(const FLODQueryContext& Context)
{
	if (!LODStrategy)
	{
		return;
	}

	// Get chunks to unload (cheap operation - just iterates loaded chunks)
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

	// Throttle: limit concurrent async generation tasks
	if (AsyncGenerationInProgress.Num() >= EffectiveMaxAsyncGenerationTasks)
	{
		return;
	}

	const int32 MaxChunks = Configuration->MaxChunksToLoadPerFrame;
	int32 ProcessedCount = 0;

	while (GenerationQueue.Num() > 0 && ProcessedCount < MaxChunks &&
	       AsyncGenerationInProgress.Num() < EffectiveMaxAsyncGenerationTasks)
	{
		// Skip chunks already being generated asynchronously
		if (AsyncGenerationInProgress.Contains(GenerationQueue.Last().ChunkCoord))
		{
			GenerationQueueSet.Remove(GenerationQueue.Last().ChunkCoord);
			GenerationQueue.Pop(EAllowShrinking::No);
			continue;
		}

		// Get highest priority chunk (at back) and remove from queue and tracking set
		FChunkLODRequest Request = GenerationQueue.Last();
		GenerationQueue.Pop(EAllowShrinking::No);
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

		// Island mode parameters (used when WorldMode == IslandBowl)
		if (Configuration->WorldMode == EWorldMode::IslandBowl)
		{
			GenRequest.IslandParams.Shape = static_cast<uint8>(Configuration->IslandShape);
			GenRequest.IslandParams.IslandRadius = Configuration->IslandRadius;
			GenRequest.IslandParams.SizeY = Configuration->IslandSizeY;
			GenRequest.IslandParams.FalloffWidth = Configuration->IslandFalloffWidth;
			GenRequest.IslandParams.FalloffType = static_cast<uint8>(Configuration->IslandFalloffType);
			GenRequest.IslandParams.CenterX = Configuration->IslandCenterX;
			GenRequest.IslandParams.CenterY = Configuration->IslandCenterY;
			GenRequest.IslandParams.EdgeHeight = Configuration->IslandEdgeHeight;
			GenRequest.IslandParams.bBowlShape = Configuration->bIslandBowlShape;
		}

		// Spherical planet mode parameters (used when WorldMode == SphericalPlanet)
		if (Configuration->WorldMode == EWorldMode::SphericalPlanet)
		{
			GenRequest.SphericalPlanetParams.PlanetRadius = Configuration->WorldRadius;
			GenRequest.SphericalPlanetParams.MaxTerrainHeight = Configuration->PlanetMaxTerrainHeight;
			GenRequest.SphericalPlanetParams.MaxTerrainDepth = Configuration->PlanetMaxTerrainDepth;
			GenRequest.SphericalPlanetParams.PlanetCenter = Configuration->WorldOrigin;
			// Use PlanetHeightScale for terrain generation
			GenRequest.HeightScale = Configuration->PlanetHeightScale;
		}

		// Water level parameters
		GenRequest.bEnableWaterLevel = Configuration->bEnableWaterLevel;
		GenRequest.WaterLevel = Configuration->WaterLevel;
		GenRequest.WaterRadius = Configuration->WaterRadius;

		// Launch async generation on thread pool
		LaunchAsyncGeneration(Request, MoveTemp(GenRequest));

		++ProcessedCount;
	}
}

void UVoxelChunkManager::LaunchAsyncGeneration(const FChunkLODRequest& Request, FVoxelNoiseGenerationRequest GenRequest)
{
	// Mark as in-progress
	AsyncGenerationInProgress.Add(Request.ChunkCoord);

	// Capture raw pointer (TUniquePtr, safe because ChunkManager outlives tasks)
	IVoxelNoiseGenerator* GeneratorPtr = NoiseGenerator.Get();
	const FIntVector ChunkCoord = Request.ChunkCoord;

	// Tree injection captures (value copies for thread safety)
	const bool bInjectTrees = Configuration &&
		Configuration->MeshingMode == EMeshingMode::Cubic &&
		Configuration->TreeMode != EVoxelTreeMode::HISM &&
		Configuration->TreeTemplates.Num() > 0 &&
		Configuration->TreeDensity > 0.0f;
	TArray<FVoxelTreeTemplate> CapturedTreeTemplates;
	float CapturedTreeDensity = 0.0f;
	int32 CapturedWorldSeed = 0;
	FVector CapturedWorldOrigin = FVector::ZeroVector;
	FVoxelNoiseParams CapturedNoiseParams;
	IVoxelWorldMode* WorldModePtr = nullptr;
	UVoxelBiomeConfiguration* CapturedBiomeConfig = nullptr;
	bool CapturedEnableWaterLevel = false;
	float CapturedWaterLevel = 0.0f;

	if (bInjectTrees)
	{
		CapturedTreeTemplates = Configuration->TreeTemplates;
		CapturedTreeDensity = Configuration->TreeDensity;
		CapturedWorldSeed = Configuration->WorldSeed;
		CapturedWorldOrigin = Configuration->WorldOrigin;
		CapturedNoiseParams = Configuration->NoiseParams;
		WorldModePtr = WorldMode.Get(); // Raw ptr, same lifetime as NoiseGenerator
		CapturedBiomeConfig = Configuration->BiomeConfiguration;
		CapturedEnableWaterLevel = Configuration->bEnableWaterLevel;
		CapturedWaterLevel = Configuration->WaterLevel;
	}

	TWeakObjectPtr<UVoxelChunkManager> WeakThis(this);

	Async(EAsyncExecution::ThreadPool, [WeakThis, GeneratorPtr, GenRequest = MoveTemp(GenRequest), ChunkCoord,
		bInjectTrees, CapturedTreeTemplates = MoveTemp(CapturedTreeTemplates),
		CapturedTreeDensity, CapturedWorldSeed, CapturedWorldOrigin,
		CapturedNoiseParams, WorldModePtr,
		CapturedBiomeConfig, CapturedEnableWaterLevel, CapturedWaterLevel]() mutable
	{
		// Generate voxel data on background thread
		TArray<FVoxelData> VoxelData;
		const bool bSuccess = GeneratorPtr->GenerateChunkCPU(GenRequest, VoxelData);

		// Inject voxel trees (runs on same thread pool worker, before enqueue)
		if (bSuccess && bInjectTrees && WorldModePtr)
		{
			FVoxelTreeInjector::InjectTrees(
				ChunkCoord,
				GenRequest.ChunkSize,
				GenRequest.VoxelSize,
				CapturedWorldOrigin,
				CapturedWorldSeed,
				CapturedTreeTemplates,
				CapturedNoiseParams,
				*WorldModePtr,
				CapturedTreeDensity,
				CapturedBiomeConfig,
				CapturedEnableWaterLevel,
				CapturedWaterLevel,
				VoxelData);
		}

		// Queue result for game thread
		if (UVoxelChunkManager* This = WeakThis.Get())
		{
			FAsyncGenerationResult Result;
			Result.ChunkCoord = ChunkCoord;
			Result.bSuccess = bSuccess;
			if (bSuccess)
			{
				Result.VoxelData = MoveTemp(VoxelData);
			}
			This->CompletedGenerationQueue.Enqueue(MoveTemp(Result));
		}
	});
}

void UVoxelChunkManager::ProcessCompletedAsyncGenerations()
{
	FAsyncGenerationResult Result;
	int32 ProcessedCount = 0;
	const int32 MaxProcessPerFrame = 8;

	while (CompletedGenerationQueue.Dequeue(Result) && ProcessedCount < MaxProcessPerFrame)
	{
		// Remove from in-progress tracking
		AsyncGenerationInProgress.Remove(Result.ChunkCoord);

		// Check if chunk is still in valid state
		const EChunkState CurrentState = GetChunkState(Result.ChunkCoord);
		if (CurrentState != EChunkState::Generating)
		{
			UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d,%d,%d) async generation discarded - state changed to %d"),
				Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z, static_cast<int32>(CurrentState));
			continue;
		}

		if (Result.bSuccess)
		{
			// Store voxel data in chunk state
			FVoxelChunkState* State = ChunkStates.Find(Result.ChunkCoord);
			if (State)
			{
				State->Descriptor.VoxelData = MoveTemp(Result.VoxelData);
				OnChunkGenerationComplete(Result.ChunkCoord);
			}
		}
		else
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("Chunk (%d,%d,%d) async generation failed"),
				Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z);

			FVoxelChunkState* State = ChunkStates.Find(Result.ChunkCoord);
			if (State)
			{
				State->Descriptor.VoxelData.Empty();
			}
			SetChunkState(Result.ChunkCoord, EChunkState::Unloaded);
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

	// Throttle if too many async tasks in flight or pending queue is full
	if (AsyncMeshingInProgress.Num() >= EffectiveMaxAsyncMeshTasks)
	{
		return;
	}
	if (PendingMeshQueue.Num() >= EffectiveMaxPendingMeshes)
	{
		return;
	}

	// For async, we don't need time slicing - just limit concurrent tasks
	const int32 MaxChunks = Configuration->MaxChunksToLoadPerFrame;
	int32 ProcessedCount = 0;

	while (MeshingQueue.Num() > 0 && ProcessedCount < MaxChunks &&
	       AsyncMeshingInProgress.Num() < EffectiveMaxAsyncMeshTasks &&
	       PendingMeshQueue.Num() < EffectiveMaxPendingMeshes)
	{
		// Skip chunks already being meshed asynchronously
		if (AsyncMeshingInProgress.Contains(MeshingQueue.Last().ChunkCoord))
		{
			MeshingQueueSet.Remove(MeshingQueue.Last().ChunkCoord);
			MeshingQueue.Pop(EAllowShrinking::No);
			continue;
		}

		// Get highest priority chunk (at back) and remove from queue and tracking set
		FChunkLODRequest Request = MeshingQueue.Last();
		MeshingQueue.Pop(EAllowShrinking::No);
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

		// Merge edit layer if present
		if (EditManager && EditManager->ChunkHasEdits(Request.ChunkCoord))
		{
			const FChunkEditLayer* EditLayer = EditManager->GetEditLayer(Request.ChunkCoord);
			if (EditLayer && !EditLayer->IsEmpty())
			{
				for (const auto& EditPair : EditLayer->Edits)
				{
					const int32 Index = EditPair.Key;
					const FVoxelEdit& Edit = EditPair.Value;
					if (MeshRequest.VoxelData.IsValidIndex(Index))
					{
						// Apply edit relative to procedural data using edit mode and delta
						const FVoxelData& ProceduralData = MeshRequest.VoxelData[Index];
						MeshRequest.VoxelData[Index] = Edit.ApplyToProceduralData(ProceduralData);
					}
				}
				State->Descriptor.bHasEdits = true;

				UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d,%d,%d) merged %d edits from edit layer"),
					Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z, EditLayer->GetEditCount());
			}
		}

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

		// Launch async mesh generation instead of blocking
		LaunchAsyncMeshGeneration(Request, MeshRequest);

		++ProcessedCount;
	}
}

void UVoxelChunkManager::LaunchAsyncMeshGeneration(const FChunkLODRequest& Request, FVoxelMeshingRequest MeshRequest)
{
	// Mark as in-progress
	AsyncMeshingInProgress.Add(Request.ChunkCoord);

	// Capture mesher pointer (it's a TUniquePtr, so we need raw pointer for lambda)
	IVoxelMesher* MesherPtr = Mesher.Get();
	const FIntVector ChunkCoord = Request.ChunkCoord;
	const int32 LODLevel = Request.LODLevel;

	// Use a weak pointer to safely check if ChunkManager is still valid
	TWeakObjectPtr<UVoxelChunkManager> WeakThis(this);

	// Launch async task on thread pool
	Async(EAsyncExecution::ThreadPool, [WeakThis, MesherPtr, MeshRequest = MoveTemp(MeshRequest), ChunkCoord, LODLevel]() mutable
	{
		// Generate mesh on background thread
		FChunkMeshData MeshData;
		const bool bSuccess = MesherPtr->GenerateMeshCPU(MeshRequest, MeshData);

		// Queue result for game thread (thread-safe MPSC queue)
		if (UVoxelChunkManager* This = WeakThis.Get())
		{
			FAsyncMeshResult Result;
			Result.ChunkCoord = ChunkCoord;
			Result.LODLevel = LODLevel;
			Result.bSuccess = bSuccess;
			if (bSuccess)
			{
				Result.MeshData = MoveTemp(MeshData);
			}
			This->CompletedMeshQueue.Enqueue(MoveTemp(Result));
		}
	});
}

void UVoxelChunkManager::ProcessCompletedAsyncMeshes()
{
	FAsyncMeshResult Result;
	int32 ProcessedCount = 0;
	const int32 MaxProcessPerFrame = 8; // Limit how many we process to spread render uploads

	while (CompletedMeshQueue.Dequeue(Result) && ProcessedCount < MaxProcessPerFrame)
	{
		// Remove from in-progress tracking
		AsyncMeshingInProgress.Remove(Result.ChunkCoord);

		// Check if chunk is still in a valid state (might have been unloaded while meshing)
		const EChunkState CurrentState = GetChunkState(Result.ChunkCoord);
		if (CurrentState != EChunkState::Meshing)
		{
			// Chunk state changed while we were meshing - discard result
			UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d,%d,%d) async mesh discarded - state changed to %d"),
				Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z, static_cast<int32>(CurrentState));
			continue;
		}

		if (Result.bSuccess)
		{
			// Store mesh in pending queue (will be submitted later, throttled)
			FPendingMeshData PendingMesh;
			PendingMesh.ChunkCoord = Result.ChunkCoord;
			PendingMesh.LODLevel = Result.LODLevel;
			PendingMesh.MeshData = MoveTemp(Result.MeshData);
			PendingMeshQueue.Add(MoveTemp(PendingMesh));
		}
		else
		{
			// Meshing failed - reset to PendingMeshing to retry
			UE_LOG(LogVoxelStreaming, Warning, TEXT("Chunk (%d,%d,%d) async meshing failed"),
				Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z);
			SetChunkState(Result.ChunkCoord, EChunkState::PendingMeshing);
		}

		++ProcessedCount;
	}
}

void UVoxelChunkManager::ProcessUnloadQueue(int32 MaxChunks)
{
	int32 ProcessedCount = 0;

	while (UnloadQueue.Num() > 0 && ProcessedCount < MaxChunks)
	{
		// Remove from queue and tracking set (pop from back for O(1))
		FIntVector ChunkCoord = UnloadQueue.Last();
		UnloadQueue.Pop(EAllowShrinking::No);
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

		// Notify scatter manager
		if (ScatterManager)
		{
			ScatterManager->OnChunkUnloaded(ChunkCoord);
		}

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

void UVoxelChunkManager::EvaluateLODLevelChanges(const FLODQueryContext& Context)
{
	if (!LODStrategy || !MeshRenderer)
	{
		return;
	}

	if (Configuration && !Configuration->bEnableLOD)
	{
		return;
	}

	// Use effective (adaptive) throttle value as a true per-frame cap
	// This is how many LOD remeshes we can QUEUE THIS FRAME, regardless of total pending
	const int32 MaxLODRemeshThisFrame = EffectiveMaxLODRemeshPerFrame;

	// Track chunks that need remeshing due to LOD level changes
	struct FLODRemeshCandidate
	{
		FIntVector ChunkCoord;
		int32 NewLODLevel;
		float Distance;
		bool bIsUpgrade;
	};
	TArray<FLODRemeshCandidate> RemeshCandidates;

	for (const FIntVector& ChunkCoord : LoadedChunkCoords)
	{
		const int32 NewLODLevel = LODStrategy->GetLODForChunk(ChunkCoord, Context);

		if (FVoxelChunkState* State = ChunkStates.Find(ChunkCoord))
		{
			if (State->LODLevel != NewLODLevel)
			{
				const float ChunkWorldSize = Configuration ? Configuration->GetChunkWorldSize() : 3200.0f;
				const FVector WorldOrigin = Configuration ? Configuration->WorldOrigin : FVector::ZeroVector;
				const FVector ChunkCenter = WorldOrigin + FVector(ChunkCoord) * ChunkWorldSize + FVector(ChunkWorldSize * 0.5f);
				const float Distance = FVector::Dist(ChunkCenter, Context.ViewerPosition);

				FLODRemeshCandidate Candidate;
				Candidate.ChunkCoord = ChunkCoord;
				Candidate.NewLODLevel = NewLODLevel;
				Candidate.Distance = Distance;
				Candidate.bIsUpgrade = NewLODLevel < State->LODLevel;

				RemeshCandidates.Add(Candidate);
			}
		}
	}

	// Sort: upgrades first, then closer chunks first
	RemeshCandidates.Sort([](const FLODRemeshCandidate& A, const FLODRemeshCandidate& B)
	{
		if (A.bIsUpgrade != B.bIsUpgrade)
		{
			return A.bIsUpgrade;
		}
		return A.Distance < B.Distance;
	});

	// Queue limited number of remeshes per frame (true per-frame cap, not limited by existing pending)
	int32 QueuedThisFrame = 0;

	for (const FLODRemeshCandidate& Candidate : RemeshCandidates)
	{
		if (QueuedThisFrame >= MaxLODRemeshThisFrame)
		{
			break;
		}

		if (FVoxelChunkState* State = ChunkStates.Find(Candidate.ChunkCoord))
		{
			if (State->State == EChunkState::Loaded)
			{
				State->LODLevel = Candidate.NewLODLevel;

				FChunkLODRequest Request;
				Request.ChunkCoord = Candidate.ChunkCoord;
				Request.LODLevel = Candidate.NewLODLevel;
				Request.Priority = (Candidate.bIsUpgrade ? 100.0f : 50.0f) + (10000.0f / FMath::Max(Candidate.Distance, 1.0f));

				if (AddToMeshingQueue(Request))
				{
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

	if (QueuedThisFrame > 0)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("LOD level changes: %d candidates, queued %d/%d this frame"),
			RemeshCandidates.Num(), QueuedThisFrame, MaxLODRemeshThisFrame);
	}
}

void UVoxelChunkManager::UpdateLODMorphFactors(const FLODQueryContext& Context)
{
	if (!LODStrategy || !MeshRenderer)
	{
		return;
	}

	if (Configuration && !Configuration->bEnableLOD)
	{
		return;
	}

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

	// Find mesh in pending queue (search from back since we pop from back)
	int32 PendingIndex = INDEX_NONE;
	for (int32 i = PendingMeshQueue.Num() - 1; i >= 0; --i)
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

		// Notify scatter manager with voxel data for LOD-independent surface extraction
		if (ScatterManager && Configuration && Configuration->bEnableScatter)
		{
			ScatterManager->OnChunkMeshDataReady(ChunkCoord, PendingMesh.LODLevel, PendingMesh.MeshData,
				State->Descriptor.VoxelData, State->Descriptor.ChunkSize, Configuration->VoxelSize);
		}

		// Remove from pending queue — O(1) swap since order doesn't matter (accessed by coord)
		PendingMeshQueue.RemoveAtSwap(PendingIndex, EAllowShrinking::No);
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

	// Cache structure for neighbor chunk data to avoid repeated TMap lookups
	struct FNeighborCache
	{
		const FVoxelChunkState* State = nullptr;
		const FChunkEditLayer* EditLayer = nullptr;
		bool bHasData = false;
	};

	// Pre-cache neighbor data (26 potential neighbors: 6 faces + 12 edges + 8 corners)
	// Use a simple lambda to initialize cache on first access per neighbor
	TMap<FIntVector, FNeighborCache> NeighborCaches;
	NeighborCaches.Reserve(26);

	auto GetNeighborCache = [this, VolumeSize, &NeighborCaches](const FIntVector& NeighborCoord) -> const FNeighborCache&
	{
		if (const FNeighborCache* Cached = NeighborCaches.Find(NeighborCoord))
		{
			return *Cached;
		}

		FNeighborCache Cache;
		Cache.State = ChunkStates.Find(NeighborCoord);
		if (Cache.State && Cache.State->Descriptor.VoxelData.Num() == VolumeSize)
		{
			Cache.bHasData = true;
			// Cache edit layer lookup (only once per neighbor)
			if (EditManager)
			{
				Cache.EditLayer = EditManager->GetEditLayer(NeighborCoord);
			}
		}
		return NeighborCaches.Add(NeighborCoord, Cache);
	};

	// Optimized helper lambda to get a single voxel from a neighbor chunk
	// Uses cached state and edit layer to avoid repeated TMap lookups
	auto GetNeighborVoxel = [ChunkSize, &GetNeighborCache](const FIntVector& NeighborCoord, int32 X, int32 Y, int32 Z) -> FVoxelData
	{
		const FNeighborCache& Cache = GetNeighborCache(NeighborCoord);
		if (!Cache.bHasData)
		{
			return FVoxelData::Air();
		}

		const int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
		const TArray<FVoxelData>& Voxels = Cache.State->Descriptor.VoxelData;
		if (!Voxels.IsValidIndex(Index))
		{
			return FVoxelData::Air();
		}

		FVoxelData Result = Voxels[Index];

		// Apply edit if present (using cached edit layer)
		if (Cache.EditLayer)
		{
			if (const FVoxelEdit* Edit = Cache.EditLayer->GetEdit(FIntVector(X, Y, Z)))
			{
				Result = Edit->ApplyToProceduralData(Result);
			}
		}

		return Result;
	};

	// Helper to check if neighbor chunk exists and has valid data (uses cache)
	auto HasNeighborData = [&GetNeighborCache](const FIntVector& NeighborCoord) -> bool
	{
		return GetNeighborCache(NeighborCoord).bHasData;
	};

	// Helper to get voxel index
	auto GetIndex = [ChunkSize](int32 X, int32 Y, int32 Z) -> int32
	{
		return X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
	};

	// ==================== Extract Face Neighbors ====================

	// +X neighbor (extract X=0 slice from neighbor)
	FIntVector NeighborXPosCoord = ChunkCoord + FIntVector(1, 0, 0);
	if (HasNeighborData(NeighborXPosCoord))
	{
		OutRequest.NeighborXPos.SetNumUninitialized(SliceSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < ChunkSize; ++Y)
			{
				OutRequest.NeighborXPos[Y + Z * ChunkSize] = GetNeighborVoxel(NeighborXPosCoord, 0, Y, Z);
			}
		}
	}

	// -X neighbor (extract X=ChunkSize-1 slice from neighbor)
	FIntVector NeighborXNegCoord = ChunkCoord + FIntVector(-1, 0, 0);
	if (HasNeighborData(NeighborXNegCoord))
	{
		OutRequest.NeighborXNeg.SetNumUninitialized(SliceSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < ChunkSize; ++Y)
			{
				OutRequest.NeighborXNeg[Y + Z * ChunkSize] = GetNeighborVoxel(NeighborXNegCoord, ChunkSize - 1, Y, Z);
			}
		}
	}

	// +Y neighbor (extract Y=0 slice from neighbor)
	FIntVector NeighborYPosCoord = ChunkCoord + FIntVector(0, 1, 0);
	if (HasNeighborData(NeighborYPosCoord))
	{
		OutRequest.NeighborYPos.SetNumUninitialized(SliceSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				OutRequest.NeighborYPos[X + Z * ChunkSize] = GetNeighborVoxel(NeighborYPosCoord, X, 0, Z);
			}
		}
	}

	// -Y neighbor (extract Y=ChunkSize-1 slice from neighbor)
	FIntVector NeighborYNegCoord = ChunkCoord + FIntVector(0, -1, 0);
	if (HasNeighborData(NeighborYNegCoord))
	{
		OutRequest.NeighborYNeg.SetNumUninitialized(SliceSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				OutRequest.NeighborYNeg[X + Z * ChunkSize] = GetNeighborVoxel(NeighborYNegCoord, X, ChunkSize - 1, Z);
			}
		}
	}

	// +Z neighbor (extract Z=0 slice from neighbor)
	FIntVector NeighborZPosCoord = ChunkCoord + FIntVector(0, 0, 1);
	if (HasNeighborData(NeighborZPosCoord))
	{
		OutRequest.NeighborZPos.SetNumUninitialized(SliceSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				OutRequest.NeighborZPos[X + Y * ChunkSize] = GetNeighborVoxel(NeighborZPosCoord, X, Y, 0);
			}
		}
	}

	// -Z neighbor (extract Z=ChunkSize-1 slice from neighbor)
	FIntVector NeighborZNegCoord = ChunkCoord + FIntVector(0, 0, -1);
	if (HasNeighborData(NeighborZNegCoord))
	{
		OutRequest.NeighborZNeg.SetNumUninitialized(SliceSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				OutRequest.NeighborZNeg[X + Y * ChunkSize] = GetNeighborVoxel(NeighborZNegCoord, X, Y, ChunkSize - 1);
			}
		}
	}

	// ==================== Extract Edge Neighbors (for Marching Cubes) ====================

	// Edge X+Y+ (diagonal chunk at +X+Y, extract X=0, Y=0, Z varies)
	FIntVector EdgeXPosYPos = ChunkCoord + FIntVector(1, 1, 0);
	if (HasNeighborData(EdgeXPosYPos))
	{
		OutRequest.EdgeXPosYPos.SetNumUninitialized(ChunkSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			OutRequest.EdgeXPosYPos[Z] = GetNeighborVoxel(EdgeXPosYPos, 0, 0, Z);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XPOS_YPOS;
	}

	// Edge X+Y- (diagonal chunk at +X-Y, extract X=0, Y=ChunkSize-1, Z varies)
	FIntVector EdgeXPosYNeg = ChunkCoord + FIntVector(1, -1, 0);
	if (HasNeighborData(EdgeXPosYNeg))
	{
		OutRequest.EdgeXPosYNeg.SetNumUninitialized(ChunkSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			OutRequest.EdgeXPosYNeg[Z] = GetNeighborVoxel(EdgeXPosYNeg, 0, ChunkSize - 1, Z);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XPOS_YNEG;
	}

	// Edge X-Y+ (diagonal chunk at -X+Y, extract X=ChunkSize-1, Y=0, Z varies)
	FIntVector EdgeXNegYPos = ChunkCoord + FIntVector(-1, 1, 0);
	if (HasNeighborData(EdgeXNegYPos))
	{
		OutRequest.EdgeXNegYPos.SetNumUninitialized(ChunkSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			OutRequest.EdgeXNegYPos[Z] = GetNeighborVoxel(EdgeXNegYPos, ChunkSize - 1, 0, Z);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XNEG_YPOS;
	}

	// Edge X-Y- (diagonal chunk at -X-Y, extract X=ChunkSize-1, Y=ChunkSize-1, Z varies)
	FIntVector EdgeXNegYNeg = ChunkCoord + FIntVector(-1, -1, 0);
	if (HasNeighborData(EdgeXNegYNeg))
	{
		OutRequest.EdgeXNegYNeg.SetNumUninitialized(ChunkSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			OutRequest.EdgeXNegYNeg[Z] = GetNeighborVoxel(EdgeXNegYNeg, ChunkSize - 1, ChunkSize - 1, Z);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XNEG_YNEG;
	}

	// Edge X+Z+ (diagonal chunk at +X+Z, extract X=0, Z=0, Y varies)
	FIntVector EdgeXPosZPos = ChunkCoord + FIntVector(1, 0, 1);
	if (HasNeighborData(EdgeXPosZPos))
	{
		OutRequest.EdgeXPosZPos.SetNumUninitialized(ChunkSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			OutRequest.EdgeXPosZPos[Y] = GetNeighborVoxel(EdgeXPosZPos, 0, Y, 0);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XPOS_ZPOS;
	}

	// Edge X+Z- (diagonal chunk at +X-Z, extract X=0, Z=ChunkSize-1, Y varies)
	FIntVector EdgeXPosZNeg = ChunkCoord + FIntVector(1, 0, -1);
	if (HasNeighborData(EdgeXPosZNeg))
	{
		OutRequest.EdgeXPosZNeg.SetNumUninitialized(ChunkSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			OutRequest.EdgeXPosZNeg[Y] = GetNeighborVoxel(EdgeXPosZNeg, 0, Y, ChunkSize - 1);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XPOS_ZNEG;
	}

	// Edge X-Z+ (diagonal chunk at -X+Z, extract X=ChunkSize-1, Z=0, Y varies)
	FIntVector EdgeXNegZPos = ChunkCoord + FIntVector(-1, 0, 1);
	if (HasNeighborData(EdgeXNegZPos))
	{
		OutRequest.EdgeXNegZPos.SetNumUninitialized(ChunkSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			OutRequest.EdgeXNegZPos[Y] = GetNeighborVoxel(EdgeXNegZPos, ChunkSize - 1, Y, 0);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XNEG_ZPOS;
	}

	// Edge X-Z- (diagonal chunk at -X-Z, extract X=ChunkSize-1, Z=ChunkSize-1, Y varies)
	FIntVector EdgeXNegZNeg = ChunkCoord + FIntVector(-1, 0, -1);
	if (HasNeighborData(EdgeXNegZNeg))
	{
		OutRequest.EdgeXNegZNeg.SetNumUninitialized(ChunkSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			OutRequest.EdgeXNegZNeg[Y] = GetNeighborVoxel(EdgeXNegZNeg, ChunkSize - 1, Y, ChunkSize - 1);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XNEG_ZNEG;
	}

	// Edge Y+Z+ (diagonal chunk at +Y+Z, extract Y=0, Z=0, X varies)
	FIntVector EdgeYPosZPos = ChunkCoord + FIntVector(0, 1, 1);
	if (HasNeighborData(EdgeYPosZPos))
	{
		OutRequest.EdgeYPosZPos.SetNumUninitialized(ChunkSize);
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			OutRequest.EdgeYPosZPos[X] = GetNeighborVoxel(EdgeYPosZPos, X, 0, 0);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_YPOS_ZPOS;
	}

	// Edge Y+Z- (diagonal chunk at +Y-Z, extract Y=0, Z=ChunkSize-1, X varies)
	FIntVector EdgeYPosZNeg = ChunkCoord + FIntVector(0, 1, -1);
	if (HasNeighborData(EdgeYPosZNeg))
	{
		OutRequest.EdgeYPosZNeg.SetNumUninitialized(ChunkSize);
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			OutRequest.EdgeYPosZNeg[X] = GetNeighborVoxel(EdgeYPosZNeg, X, 0, ChunkSize - 1);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_YPOS_ZNEG;
	}

	// Edge Y-Z+ (diagonal chunk at -Y+Z, extract Y=ChunkSize-1, Z=0, X varies)
	FIntVector EdgeYNegZPos = ChunkCoord + FIntVector(0, -1, 1);
	if (HasNeighborData(EdgeYNegZPos))
	{
		OutRequest.EdgeYNegZPos.SetNumUninitialized(ChunkSize);
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			OutRequest.EdgeYNegZPos[X] = GetNeighborVoxel(EdgeYNegZPos, X, ChunkSize - 1, 0);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_YNEG_ZPOS;
	}

	// Edge Y-Z- (diagonal chunk at -Y-Z, extract Y=ChunkSize-1, Z=ChunkSize-1, X varies)
	FIntVector EdgeYNegZNeg = ChunkCoord + FIntVector(0, -1, -1);
	if (HasNeighborData(EdgeYNegZNeg))
	{
		OutRequest.EdgeYNegZNeg.SetNumUninitialized(ChunkSize);
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			OutRequest.EdgeYNegZNeg[X] = GetNeighborVoxel(EdgeYNegZNeg, X, ChunkSize - 1, ChunkSize - 1);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_YNEG_ZNEG;
	}

	// ==================== Extract Corner Neighbors (for Marching Cubes) ====================

	// Corner X+Y+Z+ (diagonal chunk at +X+Y+Z, extract voxel at 0,0,0)
	FIntVector CornerPPP = ChunkCoord + FIntVector(1, 1, 1);
	if (HasNeighborData(CornerPPP))
	{
		OutRequest.CornerXPosYPosZPos = GetNeighborVoxel(CornerPPP, 0, 0, 0);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZPOS;
	}

	// Corner X+Y+Z- (diagonal chunk at +X+Y-Z, extract voxel at 0,0,ChunkSize-1)
	FIntVector CornerPPN = ChunkCoord + FIntVector(1, 1, -1);
	if (HasNeighborData(CornerPPN))
	{
		OutRequest.CornerXPosYPosZNeg = GetNeighborVoxel(CornerPPN, 0, 0, ChunkSize - 1);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZNEG;
	}

	// Corner X+Y-Z+ (diagonal chunk at +X-Y+Z, extract voxel at 0,ChunkSize-1,0)
	FIntVector CornerPNP = ChunkCoord + FIntVector(1, -1, 1);
	if (HasNeighborData(CornerPNP))
	{
		OutRequest.CornerXPosYNegZPos = GetNeighborVoxel(CornerPNP, 0, ChunkSize - 1, 0);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZPOS;
	}

	// Corner X+Y-Z- (diagonal chunk at +X-Y-Z, extract voxel at 0,ChunkSize-1,ChunkSize-1)
	FIntVector CornerPNN = ChunkCoord + FIntVector(1, -1, -1);
	if (HasNeighborData(CornerPNN))
	{
		OutRequest.CornerXPosYNegZNeg = GetNeighborVoxel(CornerPNN, 0, ChunkSize - 1, ChunkSize - 1);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZNEG;
	}

	// Corner X-Y+Z+ (diagonal chunk at -X+Y+Z, extract voxel at ChunkSize-1,0,0)
	FIntVector CornerNPP = ChunkCoord + FIntVector(-1, 1, 1);
	if (HasNeighborData(CornerNPP))
	{
		OutRequest.CornerXNegYPosZPos = GetNeighborVoxel(CornerNPP, ChunkSize - 1, 0, 0);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZPOS;
	}

	// Corner X-Y+Z- (diagonal chunk at -X+Y-Z, extract voxel at ChunkSize-1,0,ChunkSize-1)
	FIntVector CornerNPN = ChunkCoord + FIntVector(-1, 1, -1);
	if (HasNeighborData(CornerNPN))
	{
		OutRequest.CornerXNegYPosZNeg = GetNeighborVoxel(CornerNPN, ChunkSize - 1, 0, ChunkSize - 1);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZNEG;
	}

	// Corner X-Y-Z+ (diagonal chunk at -X-Y+Z, extract voxel at ChunkSize-1,ChunkSize-1,0)
	FIntVector CornerNNP = ChunkCoord + FIntVector(-1, -1, 1);
	if (HasNeighborData(CornerNNP))
	{
		OutRequest.CornerXNegYNegZPos = GetNeighborVoxel(CornerNNP, ChunkSize - 1, ChunkSize - 1, 0);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZPOS;
	}

	// Corner X-Y-Z- (diagonal chunk at -X-Y-Z, extract voxel at ChunkSize-1,ChunkSize-1,ChunkSize-1)
	FIntVector CornerNNN = ChunkCoord + FIntVector(-1, -1, -1);
	if (HasNeighborData(CornerNNN))
	{
		OutRequest.CornerXNegYNegZNeg = GetNeighborVoxel(CornerNNN, ChunkSize - 1, ChunkSize - 1, ChunkSize - 1);
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

	// Binary search for sorted insertion (ascending — highest priority at back for O(1) pop)
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

	// Binary search for sorted insertion (ascending — highest priority at back for O(1) pop)
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

// ==================== Queue Re-Prioritization ====================

void UVoxelChunkManager::ReprioritizeQueues(const FLODQueryContext& Context)
{
	if (!Configuration)
	{
		return;
	}

	const FVector& ViewerPosition = Context.ViewerPosition;
	const float ChunkWorldSize = Configuration->GetChunkWorldSize();
	const FVector WorldOrigin = Configuration->WorldOrigin;
	const float ViewDistance = Configuration->ViewDistance;
	// Add 20% buffer to prevent flip-flopping at ViewDistance boundary
	const float EvictDistanceSq = FMath::Square(ViewDistance * 1.2f);

	int32 EvictedCount = 0;
	int32 LODUpdatedCount = 0;

	// Re-prioritize, update LOD levels, and evict stale items from generation queue
	for (int32 i = GenerationQueue.Num() - 1; i >= 0; --i)
	{
		FChunkLODRequest& Request = GenerationQueue[i];
		const FVector ChunkCenter = WorldOrigin + FVector(Request.ChunkCoord) * ChunkWorldSize + FVector(ChunkWorldSize * 0.5f);
		const float DistSq = FVector::DistSquared(ChunkCenter, ViewerPosition);

		if (DistSq > EvictDistanceSq)
		{
			// Beyond view distance — evict from queue and reset chunk state
			GenerationQueueSet.Remove(Request.ChunkCoord);
			SetChunkState(Request.ChunkCoord, EChunkState::Unloaded);
			RemoveChunkState(Request.ChunkCoord);
			GenerationQueue.RemoveAtSwap(i);
			++EvictedCount;
		}
		else
		{
			// Update priority: closer = higher priority
			Request.Priority = 1.0f / FMath::Max(FMath::Sqrt(DistSq), 1.0f);

			// Update LOD level based on current viewer position
			// This prevents chunks from being generated at a stale LOD level
			if (LODStrategy)
			{
				const int32 NewLOD = LODStrategy->GetLODForChunk(Request.ChunkCoord, Context);
				if (Request.LODLevel != NewLOD)
				{
					Request.LODLevel = NewLOD;
					// Also update the chunk state's LOD level
					if (FVoxelChunkState* State = ChunkStates.Find(Request.ChunkCoord))
					{
						State->LODLevel = NewLOD;
					}
					++LODUpdatedCount;
				}
			}
		}
	}

	// Re-sort generation queue (ascending — highest priority at back)
	if (GenerationQueue.Num() > 1)
	{
		GenerationQueue.Sort();
	}

	// Re-prioritize and update LOD levels in meshing queue (don't evict — generation data already computed)
	for (FChunkLODRequest& Request : MeshingQueue)
	{
		const FVector ChunkCenter = WorldOrigin + FVector(Request.ChunkCoord) * ChunkWorldSize + FVector(ChunkWorldSize * 0.5f);
		const float Dist = FVector::Dist(ChunkCenter, ViewerPosition);
		Request.Priority = 1.0f / FMath::Max(Dist, 1.0f);

		// Update LOD level for meshing queue items too
		if (LODStrategy)
		{
			const int32 NewLOD = LODStrategy->GetLODForChunk(Request.ChunkCoord, Context);
			if (Request.LODLevel != NewLOD)
			{
				Request.LODLevel = NewLOD;
				if (FVoxelChunkState* State = ChunkStates.Find(Request.ChunkCoord))
				{
					State->LODLevel = NewLOD;
				}
				++LODUpdatedCount;
			}
		}
	}

	if (MeshingQueue.Num() > 1)
	{
		MeshingQueue.Sort();
	}

	if (EvictedCount > 0 || LODUpdatedCount > 0)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("ReprioritizeQueues: Evicted %d gen items, updated %d LOD levels, Gen=%d Mesh=%d remaining"),
			EvictedCount, LODUpdatedCount, GenerationQueue.Num(), MeshingQueue.Num());
	}
}

// ==================== Collision Mesh Generation ====================

bool UVoxelChunkManager::PrepareCollisionMeshRequest(
	const FIntVector& ChunkCoord,
	int32 LODLevel,
	FVoxelMeshingRequest& OutMeshRequest)
{
	if (!bIsInitialized || !Configuration || !Mesher)
	{
		return false;
	}

	// Get chunk state
	const FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
	if (!State || State->State == EChunkState::Unloaded)
	{
		return false;
	}

	// Verify we have voxel data
	const int32 ChunkSize = Configuration->ChunkSize;
	const int32 VolumeSize = ChunkSize * ChunkSize * ChunkSize;
	if (State->Descriptor.VoxelData.Num() != VolumeSize)
	{
		return false;
	}

	// Build meshing request for collision LOD
	OutMeshRequest.ChunkCoord = ChunkCoord;
	OutMeshRequest.LODLevel = LODLevel;
	OutMeshRequest.ChunkSize = ChunkSize;
	OutMeshRequest.VoxelSize = Configuration->VoxelSize;
	OutMeshRequest.WorldOrigin = Configuration->WorldOrigin;

	// Copy voxel data
	OutMeshRequest.VoxelData = State->Descriptor.VoxelData;

	// Merge edit layer if present
	if (EditManager && EditManager->ChunkHasEdits(ChunkCoord))
	{
		const FChunkEditLayer* EditLayer = EditManager->GetEditLayer(ChunkCoord);
		if (EditLayer && !EditLayer->IsEmpty())
		{
			for (const auto& EditPair : EditLayer->Edits)
			{
				const int32 Index = EditPair.Key;
				const FVoxelEdit& Edit = EditPair.Value;
				if (OutMeshRequest.VoxelData.IsValidIndex(Index))
				{
					const FVoxelData& ProceduralData = OutMeshRequest.VoxelData[Index];
					OutMeshRequest.VoxelData[Index] = Edit.ApplyToProceduralData(ProceduralData);
				}
			}
		}
	}

	// Extract neighbor data for seamless boundaries
	ExtractNeighborEdgeSlices(ChunkCoord, OutMeshRequest);

	// For collision, we don't need transition cells
	OutMeshRequest.TransitionFaces = 0;
	for (int32 i = 0; i < 6; ++i)
	{
		OutMeshRequest.NeighborLODLevels[i] = LODLevel;
	}

	return true;
}

bool UVoxelChunkManager::GetChunkCollisionMesh(
	const FIntVector& ChunkCoord,
	int32 LODLevel,
	FChunkMeshData& OutMeshData)
{
	FVoxelMeshingRequest MeshRequest;
	if (!PrepareCollisionMeshRequest(ChunkCoord, LODLevel, MeshRequest))
	{
		return false;
	}

	OutMeshData.Reset();
	return Mesher->GenerateMeshCPU(MeshRequest, OutMeshData);
}
