// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCollisionManager.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelChunkManager.h"
#include "VoxelCoordinates.h"
#include "ChunkRenderData.h"
#include "IVoxelMesher.h"
#include "VoxelMeshingTypes.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Engine/CollisionProfile.h"
#include "DrawDebugHelpers.h"
#include "Algo/BinarySearch.h"
#include "Serialization/ArchiveCountMem.h"
#include "Async/Async.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"

// Chaos physics includes (UE5 always uses Chaos)
#include "Chaos/TriangleMeshImplicitObject.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelCollision, Log, All);

// ==================== Tier 0 fall-through fix cvars (see UVoxelCollisionManager::Update) ====================
// All gated behind LeadFix so the legacy behavior is one toggle away for A/B validation.

static TAutoConsoleVariable<int32> CVarCollisionLeadFix(
	TEXT("voxel.Collision.LeadFix"),
	1,
	TEXT("Tier-0 collision fall-through fix. 1 = center collision on the PAWN (not the trailing 3rd-person camera), ")
	TEXT("expand the collision radius with speed, and re-decide eagerly while moving. 0 = legacy behavior."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarCollisionBaseRadius(
	TEXT("voxel.Collision.BaseRadius"),
	0.0f,
	TEXT("Base collision radius (uu) when LeadFix is on. <= 0 uses the configured radius (ViewDistance * 0.5)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarCollisionLeadSeconds(
	TEXT("voxel.Collision.LeadSeconds"),
	0.75f,
	TEXT("Seconds of focus velocity added to the collision radius as forward lead when LeadFix is on ")
	TEXT("(effective radius = base + speed * LeadSeconds)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarCollisionMoveUpdateThreshold(
	TEXT("voxel.Collision.MoveUpdateThreshold"),
	200.0f,
	TEXT("Min focus movement (uu) between collision decision sweeps when LeadFix is on (legacy = 1000)."),
	ECVF_Default);

// ---- Tier 1: cook throughput + path/descent coverage (all under LeadFix) ----

static TAutoConsoleVariable<int32> CVarCollisionMaxApplies(
	TEXT("voxel.Collision.MaxAppliesPerFrame"),
	4,
	TEXT("Max collision cooks physics-registered per frame. Higher keeps collision up during sustained traversal, "
	     "at some game-thread cost. Clamped [1,16]."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarCollisionMaxAsyncTasks(
	TEXT("voxel.Collision.MaxAsyncTasks"),
	0,
	TEXT("Override max concurrent async collision cook tasks. 0 = use the configured value. Clamped [1,8]."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarCollisionLookAheadChunks(
	TEXT("voxel.Collision.LookAheadChunks"),
	2,
	TEXT("When LeadFix is on, force-cook this many chunks ahead along the pawn's horizontal velocity — plus the "
	     "chunk it is descending into — at top priority, so ground under imminent (incl. downhill) footfalls is "
	     "cooked before entry. 0 disables path coverage."),
	ECVF_Default);

// ==================== UVoxelCollisionComponent ====================

UVoxelCollisionComponent::UVoxelCollisionComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, LocalBounds(ForceInit)
{
	// Invisible, collision-only
	SetVisibility(false);
	SetHiddenInGame(true);
	bCastStaticShadow = false;
	bCastDynamicShadow = false;
	SetCanEverAffectNavigation(false);

	// Use the standard "BlockAll" collision profile (WorldStatic, blocks all channels).
	// This ensures proper physics filter data for character movement sweeps (ECC_Pawn),
	// line traces (ECC_WorldStatic, ECC_Visibility), and camera collision (ECC_Camera).
	// Using individual SetCollisionResponse calls with no named profile can leave the
	// BodyInstance's filter data in an ambiguous state that sweeps don't detect.
	SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
}

void UVoxelCollisionComponent::SetCollisionBodySetup(UBodySetup* InBodySetup, const FBox& InLocalBounds)
{
	CollisionBodySetup = InBodySetup;
	LocalBounds = InLocalBounds;

	// Mark bounds dirty so broadphase picks up the new geometry
	UpdateBounds();
}

FBoxSphereBounds UVoxelCollisionComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (LocalBounds.IsValid)
	{
		return FBoxSphereBounds(LocalBounds).TransformBy(LocalToWorld);
	}
	return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector::ZeroVector, 0.f);
}

UVoxelCollisionManager::UVoxelCollisionManager()
{
}

// ==================== Lifecycle ====================

void UVoxelCollisionManager::Initialize(UVoxelWorldConfiguration* Config, UVoxelChunkManager* ChunkMgr)
{
	if (bIsInitialized)
	{
		UE_LOG(LogVoxelCollision, Warning, TEXT("VoxelCollisionManager::Initialize called when already initialized"));
		Shutdown();
	}

	if (!Config)
	{
		UE_LOG(LogVoxelCollision, Error, TEXT("VoxelCollisionManager::Initialize called with null configuration"));
		return;
	}

	if (!ChunkMgr)
	{
		UE_LOG(LogVoxelCollision, Error, TEXT("VoxelCollisionManager::Initialize called with null chunk manager"));
		return;
	}

	Configuration = Config;
	ChunkManager = ChunkMgr;

	// Get world from chunk manager's owner
	if (AActor* Owner = ChunkMgr->GetOwner())
	{
		CachedWorld = Owner->GetWorld();
	}

	if (!CachedWorld)
	{
		UE_LOG(LogVoxelCollision, Error, TEXT("VoxelCollisionManager::Initialize - Could not get world reference"));
		return;
	}

	// Create container actor for collision components
	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = TEXT("VoxelCollisionContainer");
	SpawnParams.ObjectFlags |= RF_Transient;
	CollisionContainerActor = CachedWorld->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);

	if (!CollisionContainerActor)
	{
		UE_LOG(LogVoxelCollision, Error, TEXT("VoxelCollisionManager::Initialize - Failed to create collision container actor"));
		return;
	}

	// Add a root component to the container
	USceneComponent* RootComponent = NewObject<USceneComponent>(CollisionContainerActor, TEXT("RootComponent"));
	CollisionContainerActor->SetRootComponent(RootComponent);
	RootComponent->RegisterComponent();

	// Apply configuration
	CollisionLODLevel = Config->CollisionLODLevel;
	MaxAsyncCollisionTasks = FMath::Clamp(Config->MaxAsyncCollisionTasks, 1, 4);

	// Clear any existing state
	CollisionData.Empty();
	CookingQueue.Empty();
	CookingQueueSet.Empty();
	AsyncCollisionInProgress.Empty();

	// Drain any stale results from the MPSC queue
	{
		FAsyncCollisionResult Stale;
		while (CompletedCollisionQueue.Dequeue(Stale)) {}
	}

	// Reset cached state
	LastViewerPosition = FVector(FLT_MAX);
	bPendingInitialUpdate = true;

	// Reset statistics
	TotalCollisionsGenerated = 0;
	TotalCollisionsRemoved = 0;

	bIsInitialized = true;

	UE_LOG(LogVoxelCollision, Log, TEXT("VoxelCollisionManager initialized (Radius=%.0f, LOD=%d, MaxAsyncTasks=%d)"),
		CollisionRadius, CollisionLODLevel, MaxAsyncCollisionTasks);
}

void UVoxelCollisionManager::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	// Cancel all pending cooking
	CookingQueue.Empty();
	CookingQueueSet.Empty();
	AsyncCollisionInProgress.Empty();

	// Drain completed queue (results may still be arriving)
	{
		FAsyncCollisionResult Stale;
		while (CompletedCollisionQueue.Dequeue(Stale)) {}
	}

	// Release all collision data and components
	for (auto& Pair : CollisionData)
	{
		if (Pair.Value.CollisionComponent)
		{
			Pair.Value.CollisionComponent->DestroyComponent();
			Pair.Value.CollisionComponent = nullptr;
		}
		if (Pair.Value.BodySetup)
		{
			Pair.Value.BodySetup->MarkAsGarbage();
			Pair.Value.BodySetup = nullptr;
		}
	}
	CollisionData.Empty();

	// Destroy container actor
	if (CollisionContainerActor)
	{
		CollisionContainerActor->Destroy();
		CollisionContainerActor = nullptr;
	}

	CachedWorld = nullptr;
	Configuration = nullptr;
	ChunkManager = nullptr;
	bIsInitialized = false;

	UE_LOG(LogVoxelCollision, Log, TEXT("VoxelCollisionManager shutdown. Stats: Generated=%lld, Removed=%lld"),
		TotalCollisionsGenerated, TotalCollisionsRemoved);
}

// ==================== Per-Frame Update ====================

void UVoxelCollisionManager::Update(const FVector& ViewerPosition, float DeltaTime)
{
	if (!bIsInitialized)
	{
		return;
	}

	// 1. Always drain completed async results first (lightweight game-thread work)
	ProcessCompletedCollisionCooks();

	// Tier 0 fall-through fix (voxel.Collision.LeadFix): center collision on the PAWN, not the
	// passed viewer position. In 3rd person the camera trails the pawn, which shifts the collision
	// shell behind it and shrinks the forward lead — the fall-through signature. Falls back to the
	// passed viewer position when the fix is off or no pawn is resolvable.
	const bool bLeadFix = CVarCollisionLeadFix.GetValueOnGameThread() != 0;
	const FVector FocusPosition = ResolveFocusPosition(ViewerPosition);

	// Focus velocity for the speed-aware radius + path coverage. Use HORIZONTAL speed for the lead:
	// vertical velocity (walking downhill or falling) must NOT inflate the radius, or it floods the
	// cook queue with far chunks and starves the pawn's own next chunk — the sustained-traversal
	// fall-through we saw on descents (effRadius ballooned to ~11k while dropping). Lightly smoothed
	// so a teleport spike doesn't blow the radius out for a frame.
	FVector FocusVelocity = FVector::ZeroVector;
	float FocusSpeed = 0.0f;
	if (DeltaTime > SMALL_NUMBER && LastFocusPosition.X != FLT_MAX)
	{
		FocusVelocity = (FocusPosition - LastFocusPosition) / DeltaTime;
		const float InstHorizSpeed = FVector2D(FocusVelocity.X, FocusVelocity.Y).Size();
		SmoothedFocusSpeed = FMath::FInterpTo(SmoothedFocusSpeed, InstHorizSpeed, DeltaTime, 5.0f);
		FocusSpeed = SmoothedFocusSpeed;
	}

	// Effective radius = base (+ forward lead proportional to speed) when the fix is on, never
	// smaller than the configured CollisionRadius.
	float EffectiveRadius = CollisionRadius;
	if (bLeadFix)
	{
		const float ConfiguredBase = CVarCollisionBaseRadius.GetValueOnGameThread();
		const float Base = ConfiguredBase > 0.0f ? ConfiguredBase : CollisionRadius;
		EffectiveRadius = Base + FocusSpeed * CVarCollisionLeadSeconds.GetValueOnGameThread();
	}
	CurrentEffectiveRadius = FMath::Max(EffectiveRadius, CollisionRadius);

	// 2. Re-run collision decisions when the focus moved enough. With the fix on, use a much smaller
	// deadband and force an immediate re-decision on a stop->start transition, so the first steps
	// after standing still don't outrun the (legacy 10 m) deadband.
	const float MoveThreshold = bLeadFix
		? FMath::Max(CVarCollisionMoveUpdateThreshold.GetValueOnGameThread(), 1.0f)
		: UpdateThreshold;
	const bool bStartedMoving = bLeadFix && bWasStationary && FocusSpeed > StationarySpeedThreshold;
	const float DistanceMoved = FVector::Dist(FocusPosition, LastViewerPosition);
	if (DistanceMoved > MoveThreshold || LastViewerPosition.X == FLT_MAX || bPendingInitialUpdate || bStartedMoving)
	{
		UpdateCollisionDecisions(FocusPosition);
		LastViewerPosition = FocusPosition;

		// Once we've queued or generated any collision, initial load is complete
		if (bPendingInitialUpdate && (CookingQueue.Num() > 0 || AsyncCollisionInProgress.Num() > 0 || CollisionData.Num() > 0))
		{
			bPendingInitialUpdate = false;
			UE_LOG(LogVoxelCollision, Log, TEXT("Initial collision load complete (queued=%d, async=%d, ready=%d)"),
				CookingQueue.Num(), AsyncCollisionInProgress.Num(), CollisionData.Num());
		}
	}

	bWasStationary = FocusSpeed <= StationarySpeedThreshold;
	LastFocusPosition = FocusPosition;

	// Tier 1: guarantee the ground under the pawn and the chunks it is about to enter — including the
	// one it is descending into — are cooked FIRST, overriding their center-distance priority (a
	// descending chunk's center is far below, so distance-priority deprioritizes it until the pawn is
	// already dropping in). This closes the residual sustained-traversal / downhill fall-through.
	if (bLeadFix)
	{
		ForcePathCoverage(FocusPosition, FocusVelocity);
	}

	// 3. Process dirty chunks (from edits) — just queues requests, lightweight
	ProcessDirtyChunks(FocusPosition);

	// 4. Launch async tasks from queue (no mesh gen here, just dispatch)
	ProcessCookingQueue();
}

FVector UVoxelCollisionManager::ResolveFocusPosition(const FVector& FallbackViewerPosition) const
{
	if (CVarCollisionLeadFix.GetValueOnGameThread() != 0 && CachedWorld)
	{
		if (const APlayerController* PC = CachedWorld->GetFirstPlayerController())
		{
			if (const APawn* Pawn = PC->GetPawn())
			{
				return Pawn->GetActorLocation();
			}
		}
	}
	return FallbackViewerPosition;
}

void UVoxelCollisionManager::ForcePathCoverage(const FVector& FocusPosition, const FVector& FocusVelocity)
{
	if (!Configuration || !ChunkManager)
	{
		return;
	}

	const float ChunkWorldSize = Configuration->GetChunkWorldSize();
	if (ChunkWorldSize <= 0.f)
	{
		return;
	}
	const int32 LookAhead = FMath::Clamp(CVarCollisionLookAheadChunks.GetValueOnGameThread(), 0, 8);

	// Boost path priorities far above the distance-based ones (which max at ~CurrentEffectiveRadius),
	// so imminent-footfall chunks cook first. RequestCollision re-prioritizes an already-queued chunk.
	constexpr float PathBoost = 1.0e6f;

	auto CoverAt = [&](const FVector& WorldPos, float Bias)
	{
		const FIntVector Chunk = ChunkManager->WorldToChunkCoord(WorldPos);
		if (ChunkManager->IsChunkLoaded(Chunk) && !HasCollision(Chunk))
		{
			RequestCollision(Chunk, PathBoost + Bias);
		}
	};

	// The pawn's own chunk + a sample below its feet (covers standing atop a vertical chunk seam).
	CoverAt(FocusPosition, 1000.f);
	CoverAt(FocusPosition - FVector(0.f, 0.f, ChunkWorldSize * 0.5f), 950.f);

	if (LookAhead <= 0)
	{
		return;
	}

	FVector HeadingH(FocusVelocity.X, FocusVelocity.Y, 0.f);
	if (HeadingH.IsNearlyZero())
	{
		return; // stationary — nothing ahead to pre-cover
	}
	HeadingH = HeadingH.GetSafeNormal();
	const bool bDescending = FocusVelocity.Z < -1.0f;

	for (int32 i = 1; i <= LookAhead; ++i)
	{
		const FVector Ahead = FocusPosition + HeadingH * (ChunkWorldSize * i);
		CoverAt(Ahead, 800.f - i);
		// Descending: also cover the chunk one below the look-ahead point — terrain (and the pawn's
		// feet) drop as it advances, so the chunk it will actually land in is lower than straight ahead.
		if (bDescending)
		{
			CoverAt(Ahead - FVector(0.f, 0.f, ChunkWorldSize), 780.f - i);
		}
	}
}

void UVoxelCollisionManager::ProcessDirtyChunks(const FVector& ViewerPosition)
{
	if (!Configuration)
	{
		return;
	}

	const float ChunkWorldSize = Configuration->ChunkSize * Configuration->VoxelSize;

	// Check all collision data for dirty chunks that need regeneration
	for (auto& Pair : CollisionData)
	{
		FChunkCollisionData& Data = Pair.Value;

		if (Data.bNeedsUpdate && !Data.bIsCooking)
		{
			// Calculate priority based on distance
			const FVector ChunkCenter = Configuration->WorldOrigin
				+ FVector(Data.ChunkCoord) * ChunkWorldSize
				+ FVector(ChunkWorldSize * 0.5f);
			const float Distance = FVector::Dist(ChunkCenter, ViewerPosition);
			const float Priority = CollisionRadius - Distance + 500.0f; // Dirty chunks get priority boost

			// Request regeneration
			RequestCollision(Data.ChunkCoord, Priority);

			UE_LOG(LogVoxelCollision, Log, TEXT("Chunk (%d,%d,%d) dirty collision queued for regeneration"),
				Data.ChunkCoord.X, Data.ChunkCoord.Y, Data.ChunkCoord.Z);
		}
	}
}

// ==================== Dirty Marking ====================

void UVoxelCollisionManager::MarkChunkDirty(const FIntVector& ChunkCoord)
{
	if (FChunkCollisionData* Data = CollisionData.Find(ChunkCoord))
	{
		Data->bNeedsUpdate = true;

		UE_LOG(LogVoxelCollision, Verbose, TEXT("Chunk (%d,%d,%d) collision marked dirty"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
	}
}

void UVoxelCollisionManager::RegenerateChunkCollision(const FIntVector& ChunkCoord)
{
	if (!bIsInitialized)
	{
		return;
	}

	// Remove existing collision
	RemoveCollision(ChunkCoord);

	// Request new collision with high priority
	RequestCollision(ChunkCoord, 1000.0f);
}

// ==================== Queries ====================

bool UVoxelCollisionManager::HasCollision(const FIntVector& ChunkCoord) const
{
	if (const FChunkCollisionData* Data = CollisionData.Find(ChunkCoord))
	{
		return Data->IsReady();
	}
	return false;
}

UBodySetup* UVoxelCollisionManager::GetChunkBodySetup(const FIntVector& ChunkCoord) const
{
	if (const FChunkCollisionData* Data = CollisionData.Find(ChunkCoord))
	{
		return Data->BodySetup;
	}
	return nullptr;
}

// ==================== Configuration ====================

void UVoxelCollisionManager::SetCollisionRadius(float Radius)
{
	CollisionRadius = FMath::Max(Radius, 1000.0f);

	// Force update on next tick
	LastViewerPosition = FVector(FLT_MAX);

	UE_LOG(LogVoxelCollision, Log, TEXT("Collision radius set to %.0f"), CollisionRadius);
}

void UVoxelCollisionManager::SetCollisionLODLevel(int32 LODLevel)
{
	CollisionLODLevel = FMath::Clamp(LODLevel, 0, VOXEL_MAX_LOD_LEVELS - 1);

	UE_LOG(LogVoxelCollision, Log, TEXT("Collision LOD level set to %d"), CollisionLODLevel);
}

void UVoxelCollisionManager::SetMaxAsyncCollisionTasks(int32 MaxTasks)
{
	MaxAsyncCollisionTasks = FMath::Clamp(MaxTasks, 1, 4);
}

// ==================== Debug ====================

FString UVoxelCollisionManager::GetDebugStats() const
{
	FString Stats = TEXT("=== VoxelCollisionManager ===\n");
	Stats += FString::Printf(TEXT("Initialized: %s\n"), bIsInitialized ? TEXT("Yes") : TEXT("No"));
	Stats += FString::Printf(TEXT("Collision Radius: %.0f\n"), CollisionRadius);
	Stats += FString::Printf(TEXT("Collision LOD: %d\n"), CollisionLODLevel);
	Stats += FString::Printf(TEXT("Chunks with Collision: %d\n"), CollisionData.Num());
	Stats += FString::Printf(TEXT("Cook Queue: %d\n"), CookingQueue.Num());
	Stats += FString::Printf(TEXT("Async In-Progress: %d\n"), AsyncCollisionInProgress.Num());
	Stats += FString::Printf(TEXT("Total Generated: %lld\n"), TotalCollisionsGenerated);
	Stats += FString::Printf(TEXT("Total Removed: %lld\n"), TotalCollisionsRemoved);

	return Stats;
}

int64 UVoxelCollisionManager::GetTotalMemoryUsage() const
{
	int64 Total = sizeof(UVoxelCollisionManager);

	// Collision data map overhead
	Total += CollisionData.GetAllocatedSize();

	// Per-chunk: cooked-trimesh estimate captured at apply time. Do NOT call
	// BodySetup->GetResourceSizeEx here — it walks the Chaos triangle-mesh geometry
	// (~1ms per body), and this getter is called per frame by the debug HUD; with a
	// settled collision shell that walk alone was 80-100ms/frame (6 fps in standalone).
	for (const auto& Pair : CollisionData)
	{
		Total += sizeof(FChunkCollisionData) + Pair.Value.EstimatedBytes;
	}

	// Cooking queue (lightweight — no mesh data stored in requests)
	Total += CookingQueue.GetAllocatedSize();
	Total += CookingQueueSet.GetAllocatedSize();
	Total += AsyncCollisionInProgress.GetAllocatedSize();

	return Total;
}

void UVoxelCollisionManager::DrawDebugVisualization(UWorld* World, const FVector& ViewerPosition) const
{
#if ENABLE_DRAW_DEBUG
	if (!World || !Configuration)
	{
		return;
	}

	// Draw collision radius
	DrawDebugSphere(World, ViewerPosition, CollisionRadius, 32, FColor::Cyan, false, -1.0f, 0, 2.0f);

	// Draw collision chunk bounds
	for (const auto& Pair : CollisionData)
	{
		const FIntVector& ChunkCoord = Pair.Key;
		const FChunkCollisionData& Data = Pair.Value;

		FColor Color;
		if (Data.bIsCooking)
		{
			Color = FColor::Yellow; // Cooking
		}
		else if (Data.bNeedsUpdate)
		{
			Color = FColor::Orange; // Needs update
		}
		else if (Data.IsReady())
		{
			Color = FColor::Green; // Ready
		}
		else
		{
			Color = FColor::Red; // Error
		}

		const FBox Bounds = FVoxelCoordinates::ChunkToWorldBounds(
			ChunkCoord,
			Configuration->ChunkSize,
			Configuration->VoxelSize
		);
		// Add WorldOrigin
		const FBox WorldBounds(
			Bounds.Min + Configuration->WorldOrigin,
			Bounds.Max + Configuration->WorldOrigin
		);

		DrawDebugBox(World, WorldBounds.GetCenter(), WorldBounds.GetExtent() * 0.9f, Color, false, -1.0f, 0, 1.0f);
	}
#endif
}

// ==================== Internal Methods ====================

void UVoxelCollisionManager::UpdateCollisionDecisions(const FVector& ViewerPosition)
{
	if (!Configuration || !ChunkManager)
	{
		return;
	}

	const float ChunkWorldSize = Configuration->GetChunkWorldSize();
	// Tier 0: use the speed-expanded effective radius when voxel.Collision.LeadFix is on
	// (CurrentEffectiveRadius is refreshed each frame in Update()); never below CollisionRadius.
	const float EffectiveRadius = FMath::Max(CurrentEffectiveRadius, CollisionRadius);
	const float CollisionRadiusSq = EffectiveRadius * EffectiveRadius;

	// Find chunks that need collision loaded
	TSet<FIntVector> ChunksNeedingCollision;

	// Get all loaded chunks from chunk manager
	TArray<FIntVector> LoadedChunks;
	ChunkManager->GetLoadedChunks(LoadedChunks);

	for (const FIntVector& ChunkCoord : LoadedChunks)
	{
		// Calculate chunk center in world space
		const FVector ChunkCenter = Configuration->WorldOrigin
			+ FVector(ChunkCoord) * ChunkWorldSize
			+ FVector(ChunkWorldSize * 0.5f);

		const float DistanceSq = FVector::DistSquared(ChunkCenter, ViewerPosition);

		if (DistanceSq <= CollisionRadiusSq)
		{
			ChunksNeedingCollision.Add(ChunkCoord);
		}
	}

	// Request collision for chunks that need it but don't have it
	for (const FIntVector& ChunkCoord : ChunksNeedingCollision)
	{
		FChunkCollisionData* Data = CollisionData.Find(ChunkCoord);

		if (!Data)
		{
			// No collision data - request it
			const FVector ChunkCenter = Configuration->WorldOrigin
				+ FVector(ChunkCoord) * ChunkWorldSize
				+ FVector(ChunkWorldSize * 0.5f);
			const float Distance = FVector::Dist(ChunkCenter, ViewerPosition);
			const float Priority = CollisionRadius - Distance; // Closer = higher priority

			RequestCollision(ChunkCoord, Priority);
		}
		else if (Data->bNeedsUpdate && !Data->bIsCooking)
		{
			// Needs update - re-request
			const FVector ChunkCenter = Configuration->WorldOrigin
				+ FVector(ChunkCoord) * ChunkWorldSize
				+ FVector(ChunkWorldSize * 0.5f);
			const float Distance = FVector::Dist(ChunkCenter, ViewerPosition);
			const float Priority = CollisionRadius - Distance + 500.0f; // Extra priority for updates

			RequestCollision(ChunkCoord, Priority);
		}
	}

	// Remove collision for chunks that are now too far
	TArray<FIntVector> ChunksToRemove;
	for (const auto& Pair : CollisionData)
	{
		if (!ChunksNeedingCollision.Contains(Pair.Key))
		{
			ChunksToRemove.Add(Pair.Key);
		}
	}

	for (const FIntVector& ChunkCoord : ChunksToRemove)
	{
		RemoveCollision(ChunkCoord);
	}
}

void UVoxelCollisionManager::ProcessCookingQueue()
{
	// Launch async tasks from the queue up to the concurrency limit (cvar override for Tier 1 headroom).
	const int32 AsyncOverride = CVarCollisionMaxAsyncTasks.GetValueOnGameThread();
	const int32 MaxAsync = AsyncOverride > 0 ? FMath::Clamp(AsyncOverride, 1, 8) : MaxAsyncCollisionTasks;
	while (CookingQueue.Num() > 0 &&
		AsyncCollisionInProgress.Num() < MaxAsync)
	{
		// Pop highest priority from the back (O(1) — queue sorted ascending, highest at back)
		FCollisionCookRequest Request = MoveTemp(CookingQueue.Last());
		CookingQueue.Pop();
		CookingQueueSet.Remove(Request.ChunkCoord);

		// Launch async mesh generation + trimesh construction
		LaunchAsyncCollisionCook(Request);
	}
}

void UVoxelCollisionManager::LaunchAsyncCollisionCook(const FCollisionCookRequest& Request)
{
	if (!ChunkManager || !Configuration)
	{
		return;
	}

	// Mark as in-progress (both in collision data and async tracking)
	FChunkCollisionData& Data = CollisionData.FindOrAdd(Request.ChunkCoord);
	Data.ChunkCoord = Request.ChunkCoord;
	Data.CollisionLODLevel = Request.LODLevel;
	Data.bIsCooking = true;
	Data.bNeedsUpdate = false;

	AsyncCollisionInProgress.Add(Request.ChunkCoord);

	// Prepare meshing request on the game thread (reads ChunkStates, EditManager — game thread only)
	FVoxelMeshingRequest MeshRequest;
	if (!ChunkManager->PrepareCollisionMeshRequest(Request.ChunkCoord, CollisionLODLevel, MeshRequest))
	{
		// Chunk may not be loaded yet or has no geometry — remove the CollisionData
		// entry so UpdateCollisionDecisions() can re-request on a future sweep.
		CollisionData.Remove(Request.ChunkCoord);
		AsyncCollisionInProgress.Remove(Request.ChunkCoord);

		UE_LOG(LogVoxelCollision, Verbose, TEXT("Chunk (%d,%d,%d) collision mesh request preparation failed (not loaded)"),
			Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z);
		return;
	}

	// Capture mesher pointer (stateless, thread-safe — same pattern as LaunchAsyncMeshGeneration)
	IVoxelMesher* MesherPtr = ChunkManager->GetMesherPtr();
	if (!MesherPtr)
	{
		Data.bIsCooking = false;
		AsyncCollisionInProgress.Remove(Request.ChunkCoord);
		return;
	}

	// Capture data for async task
	const FIntVector ChunkCoord = Request.ChunkCoord;
	const int32 LODLevel = Request.LODLevel;
	TWeakObjectPtr<UVoxelCollisionManager> WeakThis(this);

	// Launch async task: mesh generation + Chaos trimesh construction on thread pool
	Async(EAsyncExecution::ThreadPool, [WeakThis, MesherPtr, MeshRequest = MoveTemp(MeshRequest), ChunkCoord, LODLevel]() mutable
	{
		FAsyncCollisionResult Result;
		Result.ChunkCoord = ChunkCoord;
		Result.LODLevel = LODLevel;

		// Step 1: Generate mesh on background thread (the expensive part, ~2-4ms)
		FChunkMeshData MeshData;
		const bool bMeshSuccess = MesherPtr->GenerateMeshCPU(MeshRequest, MeshData);

		if (bMeshSuccess && MeshData.IsValid())
		{
			const TArray<FVector3f>& Vertices = MeshData.Positions;
			const TArray<uint32>& Indices = MeshData.Indices;

			Result.NumVertices = Vertices.Num();
			Result.NumTriangles = Indices.Num() / 3;

			// Step 2: Build Chaos trimesh (~1-2ms)
			TArray<Chaos::TVec3<Chaos::FRealSingle>> ChaosVertices;
			TArray<Chaos::TVector<int32, 3>> ChaosTriangles;

			ChaosVertices.Reserve(Vertices.Num());
			for (const FVector3f& V : Vertices)
			{
				ChaosVertices.Add(Chaos::TVec3<Chaos::FRealSingle>(V.X, V.Y, V.Z));
			}

			const int32 NumTriangles = Indices.Num() / 3;
			ChaosTriangles.Reserve(NumTriangles);
			for (int32 i = 0; i < NumTriangles; ++i)
			{
				ChaosTriangles.Add(Chaos::TVector<int32, 3>(
					static_cast<int32>(Indices[i * 3 + 0]),
					static_cast<int32>(Indices[i * 3 + 1]),
					static_cast<int32>(Indices[i * 3 + 2])
				));
			}

			// Create the Chaos trimesh implicit object (thread-safe — pure data construction)
			TRefCountPtr<Chaos::FTriangleMeshImplicitObject> TriMesh = new Chaos::FTriangleMeshImplicitObject(
				MoveTemp(ChaosVertices),
				MoveTemp(ChaosTriangles),
				TArray<uint16>() // Empty materials array
			);

			if (TriMesh.IsValid())
			{
				Result.TriMesh = MoveTemp(TriMesh);
				Result.bSuccess = true;
			}
		}

		// Enqueue result for game thread (thread-safe MPSC queue)
		if (UVoxelCollisionManager* This = WeakThis.Get())
		{
			This->CompletedCollisionQueue.Enqueue(MoveTemp(Result));
		}
	});
}

void UVoxelCollisionManager::ProcessCompletedCollisionCooks()
{
	FAsyncCollisionResult Result;
	int32 AppliedCount = 0;

	// Tier 1: cvar-tunable applies-per-frame (was a hardcoded 2 — a throughput bottleneck during
	// sustained traversal). Clamped so a bad value can't stall the game thread.
	const int32 MaxApplies = FMath::Clamp(CVarCollisionMaxApplies.GetValueOnGameThread(), 1, 16);
	while (CompletedCollisionQueue.Dequeue(Result) && AppliedCount < MaxApplies)
	{
		// Remove from in-progress tracking
		AsyncCollisionInProgress.Remove(Result.ChunkCoord);

		// Check if chunk is still relevant (may have been removed while cooking)
		FChunkCollisionData* Data = CollisionData.Find(Result.ChunkCoord);
		if (!Data)
		{
			// Chunk was unloaded while we were cooking — discard result
			UE_LOG(LogVoxelCollision, Verbose, TEXT("Chunk (%d,%d,%d) async collision discarded - chunk removed"),
				Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z);
			continue;
		}

		if (Result.bSuccess)
		{
			ApplyCollisionResult(Result);
			++AppliedCount;
		}
		else
		{
			UE_LOG(LogVoxelCollision, Warning, TEXT("Chunk (%d,%d,%d) async collision cooking failed"),
				Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z);
			Data->bIsCooking = false;
		}
	}
}

void UVoxelCollisionManager::ApplyCollisionResult(FAsyncCollisionResult& Result)
{
	FChunkCollisionData* Data = CollisionData.Find(Result.ChunkCoord);
	if (!Data)
	{
		return;
	}

	// Create or reuse BodySetup
	if (!Data->BodySetup)
	{
		Data->BodySetup = CreateBodySetup(Result.ChunkCoord);
	}

	if (!Data->BodySetup)
	{
		UE_LOG(LogVoxelCollision, Error, TEXT("Failed to create BodySetup for chunk (%d,%d,%d)"),
			Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z);
		Data->bIsCooking = false;
		return;
	}

	UBodySetup* BS = Data->BodySetup;

	// Reset physics mesh state — clears old AggGeom, TriMeshGeometries,
	// bCreatedPhysicsMeshes so the BodySetup accepts our pre-built trimesh
	BS->InvalidatePhysicsData();

	// Set up for complex collision using trimesh
	BS->bMeshCollideAll = true;
	BS->CollisionTraceFlag = CTF_UseComplexAsSimple;

	// Set the trimesh from the async result
	BS->TriMeshGeometries.Empty();
	BS->TriMeshGeometries.Add(Result.TriMesh);

	// Mark as cooked so InitBody uses our TriMeshGeometries directly
	BS->bCreatedPhysicsMeshes = true;
	BS->bHasCookedCollisionData = true;

	// Estimated cooked-trimesh footprint: positions (12B/vert) + indices (12B/tri) + Chaos
	// BVH/metadata (~16B/tri). Cheap arithmetic stand-in for GetResourceSizeEx, which walks
	// the Chaos geometry and is far too slow to call per frame (see GetTotalMemoryUsage).
	Data->EstimatedBytes = static_cast<int64>(Result.NumVertices) * 12
		+ static_cast<int64>(Result.NumTriangles) * 28;

	// Mark cooking complete
	Data->bIsCooking = false;

	// Destroy old collision component if it exists (from previous cook/edit)
	if (Data->CollisionComponent)
	{
		Data->CollisionComponent->DestroyComponent();
		Data->CollisionComponent = nullptr;
	}

	// Create collision component to register with physics (game thread only, ~0.5ms)
	Data->CollisionComponent = CreateCollisionComponent(Result.ChunkCoord, BS);

	if (Data->CollisionComponent)
	{
		++TotalCollisionsGenerated;
		OnCollisionReady.Broadcast(Result.ChunkCoord);

		UE_LOG(LogVoxelCollision, Log, TEXT("Created collision for chunk (%d,%d,%d) (%d verts, %d tris)"),
			Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z,
			Result.NumVertices, Result.NumTriangles);
	}
	else
	{
		UE_LOG(LogVoxelCollision, Warning, TEXT("Failed to create collision component for chunk (%d,%d,%d)"),
			Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z);
	}
}

void UVoxelCollisionManager::RequestCollision(const FIntVector& ChunkCoord, float Priority)
{
	// Already cooking — can't reprioritize an in-flight task.
	if (AsyncCollisionInProgress.Contains(ChunkCoord))
	{
		return;
	}

	// Already queued — bump its priority if this request is higher, then re-sort. Lets ForcePathCoverage
	// promote a chunk that UpdateCollisionDecisions queued at a low center-distance priority.
	if (CookingQueueSet.Contains(ChunkCoord))
	{
		for (int32 i = 0; i < CookingQueue.Num(); ++i)
		{
			if (CookingQueue[i].ChunkCoord == ChunkCoord)
			{
				if (Priority > CookingQueue[i].Priority)
				{
					CookingQueue.RemoveAt(i);
					FCollisionCookRequest Bumped;
					Bumped.ChunkCoord = ChunkCoord;
					Bumped.LODLevel = CollisionLODLevel;
					Bumped.Priority = Priority;
					const int32 BumpIdx = Algo::LowerBound(CookingQueue, Bumped);
					CookingQueue.Insert(Bumped, BumpIdx);
				}
				return;
			}
		}
		return;
	}

	// Create request
	FCollisionCookRequest Request;
	Request.ChunkCoord = ChunkCoord;
	Request.LODLevel = CollisionLODLevel;
	Request.Priority = Priority;

	// Add to tracking set
	CookingQueueSet.Add(ChunkCoord);

	// Sorted insertion (lowest priority first, highest at back for O(1) Pop)
	int32 InsertIndex = Algo::LowerBound(CookingQueue, Request);
	CookingQueue.Insert(Request, InsertIndex);

	UE_LOG(LogVoxelCollision, Verbose, TEXT("Chunk (%d,%d,%d) collision requested (priority=%.1f, queue=%d)"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, Priority, CookingQueue.Num());
}

void UVoxelCollisionManager::RemoveCollision(const FIntVector& ChunkCoord)
{
	// Remove from queue if pending
	if (CookingQueueSet.Remove(ChunkCoord))
	{
		for (int32 i = 0; i < CookingQueue.Num(); ++i)
		{
			if (CookingQueue[i].ChunkCoord == ChunkCoord)
			{
				CookingQueue.RemoveAt(i);
				break;
			}
		}
	}

	// Note: if async is in-progress, the result will be discarded when it completes
	// (ProcessCompletedCollisionCooks checks if chunk is still in CollisionData)
	AsyncCollisionInProgress.Remove(ChunkCoord);

	// Remove collision data and component
	if (FChunkCollisionData* Data = CollisionData.Find(ChunkCoord))
	{
		// Destroy collision component first
		if (Data->CollisionComponent)
		{
			Data->CollisionComponent->DestroyComponent();
			Data->CollisionComponent = nullptr;
		}
		if (Data->BodySetup)
		{
			Data->BodySetup->MarkAsGarbage();
		}
		CollisionData.Remove(ChunkCoord);
		++TotalCollisionsRemoved;

		OnCollisionRemoved.Broadcast(ChunkCoord);

		UE_LOG(LogVoxelCollision, Verbose, TEXT("Chunk (%d,%d,%d) collision removed"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
	}
}

/** Shared physical material for all voxel collision geometry. Created once, reused by all chunks. */
static UPhysicalMaterial* GetSharedVoxelPhysicalMaterial()
{
	static TWeakObjectPtr<UPhysicalMaterial> WeakPhysMat;

	UPhysicalMaterial* PhysMat = WeakPhysMat.Get();
	if (!PhysMat)
	{
		PhysMat = NewObject<UPhysicalMaterial>(GetTransientPackage(), TEXT("VoxelTerrainPhysMat"));
		PhysMat->AddToRoot(); // Prevent GC — lives for the entire session
		PhysMat->Friction = 0.8f;
		PhysMat->Restitution = 0.0f;
		PhysMat->FrictionCombineMode = EFrictionCombineMode::Max;
		PhysMat->RestitutionCombineMode = EFrictionCombineMode::Min;

		WeakPhysMat = PhysMat;

		UE_LOG(LogVoxelCollision, Log, TEXT("Created shared VoxelTerrainPhysMat (Friction=%.1f, Restitution=%.1f, CombineMode=Max)"),
			PhysMat->Friction, PhysMat->Restitution);
	}

	return PhysMat;
}

UBodySetup* UVoxelCollisionManager::CreateBodySetup(const FIntVector& ChunkCoord)
{
	// Create body setup with unique name
	FString BodySetupName = FString::Printf(TEXT("VoxelCollision_%d_%d_%d"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);

	UBodySetup* NewBodySetup = NewObject<UBodySetup>(this, FName(*BodySetupName));
	if (!NewBodySetup)
	{
		return nullptr;
	}

	// Configure for trimesh collision
	NewBodySetup->BodySetupGuid = FGuid::NewGuid();
	NewBodySetup->bGenerateMirroredCollision = false;
	NewBodySetup->bDoubleSidedGeometry = true;
	NewBodySetup->CollisionTraceFlag = CTF_UseComplexAsSimple;

	// Assign shared physical material for consistent friction/restitution
	NewBodySetup->PhysMaterial = GetSharedVoxelPhysicalMaterial();

	return NewBodySetup;
}

UPrimitiveComponent* UVoxelCollisionManager::CreateCollisionComponent(const FIntVector& ChunkCoord, UBodySetup* BodySetup)
{
	if (!CollisionContainerActor || !Configuration || !BodySetup)
	{
		return nullptr;
	}

	// Calculate chunk world position
	const float ChunkWorldSize = Configuration->GetChunkWorldSize();
	const FVector ChunkWorldPos = Configuration->WorldOrigin + FVector(ChunkCoord) * ChunkWorldSize;

	// Create a unique name for this collision component
	FString ComponentName = FString::Printf(TEXT("VoxelCollision_%d_%d_%d"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);

	// Create our custom collision component that properly exposes the BodySetup
	UVoxelCollisionComponent* CollisionComp = NewObject<UVoxelCollisionComponent>(
		CollisionContainerActor,
		FName(*ComponentName)
	);

	if (!CollisionComp)
	{
		return nullptr;
	}

	// Assign the trimesh BodySetup BEFORE registration so the physics body
	// is created from our trimesh geometry, not a default shape.
	// Local bounds: mesh vertices span (0,0,0) to (ChunkWorldSize) in chunk-local space.
	const FBox ChunkLocalBounds(FVector::ZeroVector, FVector(ChunkWorldSize));
	CollisionComp->SetCollisionBodySetup(BodySetup, ChunkLocalBounds);

	CollisionComp->SetupAttachment(CollisionContainerActor->GetRootComponent());
	CollisionComp->SetWorldLocation(ChunkWorldPos);

	// Register creates the physics body via GetBodySetup() → our trimesh
	CollisionComp->RegisterComponent();

	// Verify physics body was created — if not, force recreation
	if (!CollisionComp->BodyInstance.IsValidBodyInstance())
	{
		UE_LOG(LogVoxelCollision, Warning,
			TEXT("Chunk (%d,%d,%d) physics body not created during RegisterComponent — forcing RecreatePhysicsState"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
		CollisionComp->RecreatePhysicsState();
	}

	UE_LOG(LogVoxelCollision, Log,
		TEXT("Created collision component for chunk (%d,%d,%d) at (%.0f, %.0f, %.0f) — PhysicsValid=%s, TriMeshCount=%d"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z,
		ChunkWorldPos.X, ChunkWorldPos.Y, ChunkWorldPos.Z,
		CollisionComp->BodyInstance.IsValidBodyInstance() ? TEXT("Yes") : TEXT("NO"),
		BodySetup->TriMeshGeometries.Num());

	// Verification trace: confirm the collision component responds to line traces.
	// Traces downward through the center of the chunk — should hit terrain if any surface exists.
	if (CachedWorld)
	{
		const FVector TraceStart = ChunkWorldPos + FVector(ChunkWorldSize * 0.5f, ChunkWorldSize * 0.5f, ChunkWorldSize + 100.f);
		const FVector TraceEnd = ChunkWorldPos + FVector(ChunkWorldSize * 0.5f, ChunkWorldSize * 0.5f, -100.f);
		FHitResult VerifyHit;
		const bool bVerifyHit = CachedWorld->LineTraceSingleByChannel(VerifyHit, TraceStart, TraceEnd, ECC_WorldStatic);
		UE_LOG(LogVoxelCollision, Log,
			TEXT("  Verification trace for chunk (%d,%d,%d): %s%s"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z,
			bVerifyHit ? TEXT("HIT") : TEXT("MISS"),
			bVerifyHit ? *FString::Printf(TEXT(" at Z=%.0f comp=%s"), VerifyHit.ImpactPoint.Z,
				VerifyHit.GetComponent() ? *VerifyHit.GetComponent()->GetName() : TEXT("null")) : TEXT(""));
	}

	return CollisionComp;
}

void UVoxelCollisionManager::DestroyCollisionComponent(const FIntVector& ChunkCoord)
{
	if (FChunkCollisionData* Data = CollisionData.Find(ChunkCoord))
	{
		if (Data->CollisionComponent)
		{
			Data->CollisionComponent->DestroyComponent();
			Data->CollisionComponent = nullptr;

			UE_LOG(LogVoxelCollision, Verbose, TEXT("Destroyed collision component for chunk (%d,%d,%d)"),
				ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
		}
	}
}
