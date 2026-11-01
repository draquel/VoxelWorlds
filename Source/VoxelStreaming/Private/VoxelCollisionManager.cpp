// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCollisionManager.h"
#include "VoxelBoundedDrain.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelChunkManager.h"
#include "VoxelCoordinates.h"
#include "ChunkRenderData.h"
#include "IVoxelMesher.h"
#include "VoxelMeshingTypes.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "Misc/ScopeExit.h"
#include "VoxelNeighborSliceExtraction.h"
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
#include "VoxelResidencyStats.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelCollision, Log, All);

// ==================== Tier 0 pawn-centred, speed-aware collision cvars (see UVoxelCollisionManager::Update) ====================

static TAutoConsoleVariable<float> CVarCollisionBaseRadius(
	TEXT("voxel.Collision.BaseRadius"),
	0.0f,
	TEXT("Base collision radius (uu). <= 0 uses the configured radius (ViewDistance * 0.5)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarCollisionLeadSeconds(
	TEXT("voxel.Collision.LeadSeconds"),
	0.75f,
	TEXT("Seconds of focus velocity added to the collision radius as forward lead ")
	TEXT("(effective radius = base + speed * LeadSeconds)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarCollisionResweepSeconds(
	TEXT("voxel.Collision.ResweepSeconds"),
	1.0f,
	TEXT("Re-run the collision decision sweep at least this often (seconds) even when the focus is stationary, so ")
	TEXT("chunks that finish loading around a pawn that stopped after a teleport still get collision. 0 = movement only."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarCollisionMoveUpdateThreshold(
	TEXT("voxel.Collision.MoveUpdateThreshold"),
	200.0f,
	TEXT("Min focus movement (uu) between collision decision sweeps."),
	ECVF_Default);

// ---- Tier 1: cook throughput + path/descent coverage ----

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
	TEXT("Force-cook this many chunks ahead along the pawn's horizontal velocity — plus the "
	     "chunk it is descending into — at top priority, so ground under imminent (incl. downhill) footfalls is "
	     "cooked before entry. 0 disables path coverage."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarCollisionNeighborRecook(
	TEXT("voxel.Collision.NeighborRecook"),
	1,
	TEXT("Re-cook a chunk's collision when a neighbour that had no voxel data at its cook arrives (G32). The mesher reads "
	     "a missing neighbour as air, so without it the boundary cells facing that neighbour stay wrong: voxel-wide strips "
	     "along chunk faces where line traces miss the ground. 0 = off (the behaviour before the fix, for A/B checks)."),
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
	TotalNeighborRecooks = 0;
	TotalNeighborArrivalsSkipped = 0;
	TotalNeighborArrivalsSkippedSolid = 0;
	PendingNeighborRecooks.Empty();

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
	PendingNeighborRecooks.Empty();

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

	UE_LOG(LogVoxelCollision, Log, TEXT("VoxelCollisionManager shutdown. Stats: Generated=%lld, Removed=%lld, NeighborRecooks=%lld"),
		TotalCollisionsGenerated, TotalCollisionsRemoved, TotalNeighborRecooks);
}

// ==================== Per-Frame Update ====================

void UVoxelCollisionManager::Update(const FVector& ViewerPosition, float DeltaTime)
{
	if (!bIsInitialized)
	{
		return;
	}

	// Reset per-tick sub-phase attribution (accumulated by cook launches + result applies below).
	LastPrepMs = 0.0f;
	LastApplyMs = 0.0f;

	// 1. Always drain completed async results first (lightweight game-thread work)
	ProcessCompletedCollisionCooks();

	// Tier 0 fall-through fix: center collision on the PAWN, not the passed viewer position. In 3rd
	// person the camera trails the pawn, which shifts the collision shell behind it and shrinks the
	// forward lead — the fall-through signature. Falls back to the passed viewer position when no
	// pawn is resolvable.
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

	// Effective radius = base + forward lead proportional to speed, never smaller than the
	// configured CollisionRadius.
	const float ConfiguredBase = CVarCollisionBaseRadius.GetValueOnGameThread();
	const float Base = ConfiguredBase > 0.0f ? ConfiguredBase : CollisionRadius;
	const float EffectiveRadius = Base + FocusSpeed * CVarCollisionLeadSeconds.GetValueOnGameThread();
	CurrentEffectiveRadius = FMath::Max(EffectiveRadius, CollisionRadius);

	// 2. Re-run collision decisions when the focus moved enough: a small deadband, and an immediate
	// re-decision on a stop->start transition, so the first steps after standing still don't outrun it.
	const float MoveThreshold = FMath::Max(CVarCollisionMoveUpdateThreshold.GetValueOnGameThread(), 1.0f);
	const bool bStartedMoving = bWasStationary && FocusSpeed > StationarySpeedThreshold;
	const float DistanceMoved = FVector::Dist(FocusPosition, LastViewerPosition);
	SecondsSinceDecisions += DeltaTime;
	const float ResweepSeconds = CVarCollisionResweepSeconds.GetValueOnGameThread();
	const bool bPeriodicResweep = ResweepSeconds > 0.0f && SecondsSinceDecisions >= ResweepSeconds;
	if (DistanceMoved > MoveThreshold || LastViewerPosition.X == FLT_MAX || bPendingInitialUpdate || bStartedMoving || bPeriodicResweep)
	{
		UpdateCollisionDecisions(FocusPosition);
		LastViewerPosition = FocusPosition;
		SecondsSinceDecisions = 0.0f;

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
	ForcePathCoverage(FocusPosition, FocusVelocity);

	// 3. Process dirty chunks (from edits, and neighbours that arrived after a cook: G32) — just queues requests, lightweight
	ProcessNeighborRecooks();
	ProcessDirtyChunks(FocusPosition);

	// 4. Launch async tasks from queue (no mesh gen here, just dispatch)
	ProcessCookingQueue();
}

FVector UVoxelCollisionManager::ResolveFocusPosition(const FVector& FallbackViewerPosition) const
{
	if (CachedWorld)
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

	// Local down and the heading across it come from the world mode: world -Z and the horizontal velocity in heightmap
	// modes, the radius and the tangent on a planet (SP-C4).
	TArray<FVoxelCollisionCoverSample> Samples;
	ComputePathCoverageSamples(FocusPosition, FocusVelocity, ChunkManager->GetUpVectorAt(FocusPosition), ChunkWorldSize, LookAhead, Samples);
	for (const FVoxelCollisionCoverSample& Sample : Samples)
	{
		const FIntVector Chunk = ChunkManager->WorldToChunkCoord(Sample.Position);
		if (ChunkManager->IsChunkLoaded(Chunk) && !HasCollision(Chunk))
		{
			RequestCollision(Chunk, PathBoost + Sample.Bias);
		}
	}
}

void UVoxelCollisionManager::ComputePathCoverageSamples(const FVector& Focus, const FVector& Velocity, const FVector& Up,
	float ChunkWorldSize, int32 LookAhead, TArray<FVoxelCollisionCoverSample>& OutSamples)
{
	OutSamples.Reset();

	// The pawn's own chunk + a sample below its feet (covers standing atop a chunk seam).
	OutSamples.Add({ Focus, 1000.f });
	OutSamples.Add({ Focus - Up * (ChunkWorldSize * 0.5f), 950.f });

	if (LookAhead <= 0)
	{
		return;
	}

	// The velocity across local up (with Up = +Z: (X, Y, 0) exactly, the dot product being Z).
	const double AlongUp = FVector::DotProduct(Velocity, Up);
	FVector Heading = Velocity - Up * AlongUp;
	if (Heading.IsNearlyZero())
	{
		return; // stationary — nothing ahead to pre-cover
	}
	Heading = Heading.GetSafeNormal();
	const bool bDescending = AlongUp < -1.0f;

	for (int32 i = 1; i <= LookAhead; ++i)
	{
		const FVector Ahead = Focus + Heading * (ChunkWorldSize * i);
		OutSamples.Add({ Ahead, 800.f - i });
		// Descending: also cover the chunk one below the look-ahead point — terrain (and the pawn's
		// feet) drop as it advances, so the chunk it will actually land in is lower than straight ahead.
		if (bDescending)
		{
			OutSamples.Add({ Ahead - Up * ChunkWorldSize, 780.f - i });
		}
	}
}

float UVoxelCollisionManager::GetDirtyPriority(const FChunkCollisionData& Data, float Distance) const
{
	// A first cook's priority is CollisionRadius - Distance with Distance within the effective radius.
	const float FirstCook = CollisionRadius - Distance;
	if (Data.bNeighborRecookOnly)
	{
		// Below every first cook: the old body is live and only its boundary cells facing the new neighbour are off, while a
		// chunk without collision has nothing. Waiting in the queue also folds the neighbours still arriving into one re-cook.
		return FirstCook - 2.0f * FMath::Max(CurrentEffectiveRadius, CollisionRadius) - 1000.0f;
	}
	return FirstCook + 500.0f; // Edits get a priority boost
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
			const float Priority = GetDirtyPriority(Data, Distance);

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
		// An edit's re-cook keeps the edit priority, even when a neighbour's arrival marked the chunk first.
		Data->bNeighborRecookOnly = false;

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

uint32 UVoxelCollisionManager::GetNeighborBit(const FIntVector& Offset)
{
	if (FMath::Abs(Offset.X) > 1 || FMath::Abs(Offset.Y) > 1 || FMath::Abs(Offset.Z) > 1 || Offset == FIntVector::ZeroValue)
	{
		return 0;
	}
	// The 3 x 3 x 3 neighbourhood's index; 13 is the chunk itself and never set.
	const int32 Index = (Offset.X + 1) + (Offset.Y + 1) * 3 + (Offset.Z + 1) * 9;
	return 1u << Index;
}

namespace VoxelCollisionNeighbors
{
	/** A slab's voxels: all air, all solid, or both. */
	enum class ESlab : uint8 { Air, Solid, Mixed };

	/**
	 * Classify a chunk's slab facing Side: per axis, Side +1 is the last Depth layers, -1 the first Depth, 0 the whole
	 * range (a face slab, an edge column or a corner box). Stops at the first voxel that makes it mixed.
	 */
	ESlab ClassifySlab(const TArray<FVoxelData>& Voxels, int32 ChunkSize, const FIntVector& Side, int32 Depth)
	{
		if (Voxels.Num() < ChunkSize * ChunkSize * ChunkSize)
		{
			return ESlab::Mixed;
		}
		int32 Min[3], Max[3];
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const int32 S = Side[Axis];
			Min[Axis] = S > 0 ? ChunkSize - Depth : 0;
			Max[Axis] = S < 0 ? Depth - 1 : ChunkSize - 1;
		}
		bool bAir = false;
		bool bSolid = false;
		for (int32 Z = Min[2]; Z <= Max[2]; ++Z)
		for (int32 Y = Min[1]; Y <= Max[1]; ++Y)
		for (int32 X = Min[0]; X <= Max[0]; ++X)
		{
			(Voxels[X + Y * ChunkSize + Z * ChunkSize * ChunkSize].IsAir() ? bAir : bSolid) = true;
			if (bAir && bSolid)
			{
				return ESlab::Mixed;
			}
		}
		return bSolid ? ESlab::Solid : ESlab::Air;
	}
}

int32 UVoxelCollisionManager::OnChunkVoxelDataAvailable(const FIntVector& ChunkCoord)
{
	const VoxelResidencyStats::FReadScope ResidencyReadScope(TEXT("collision neighbour arrival"));
	if (!bIsInitialized || CVarCollisionNeighborRecook.GetValueOnGameThread() == 0 || !ChunkManager || !Configuration)
	{
		return 0;
	}

	// The arriving chunk's voxels, fetched only if some cooked neighbour read it as air.
	TSharedPtr<const TArray<FVoxelData>> Arrived;
	bool bFetchedArrived = false;
	const int32 ChunkSize = Configuration->ChunkSize;
	// How deep a cook's results depend on the voxels across a face at its stride. The neighbour's planes go stride + 1
	// deep (2 * stride is taken, the larger). On the chunk's own side the boundary cells span the
	// stride, and the gradient normals at a crossing up to a stride inside it reach across the face too: with two
	// strides of air there, no crossing's normal reads the neighbour.
	const int32 Stride = 1 << FMath::Clamp(CollisionLODLevel, 0, 7);
	const int32 OwnDepth = FMath::Min(2 * Stride, ChunkSize);
	const int32 OtherDepth = FMath::Min(Stride <= 1 ? 1 : 2 * Stride, ChunkSize);

	int32 Noted = 0;
	for (int32 DZ = -1; DZ <= 1; ++DZ)
	for (int32 DY = -1; DY <= 1; ++DY)
	for (int32 DX = -1; DX <= 1; ++DX)
	{
		const FIntVector Offset(DX, DY, DZ);
		if (Offset == FIntVector::ZeroValue)
		{
			continue;
		}
		// The neighbour at -Offset sees this chunk at +Offset.
		const FIntVector NeighborCoord = ChunkCoord - Offset;
		FChunkCollisionData* Data = CollisionData.Find(NeighborCoord);
		const uint32 Bit = GetNeighborBit(Offset);
		if (!Data || (Data->MissingNeighborMask & Bit) == 0)
		{
			continue;
		}
		Data->MissingNeighborMask &= ~Bit;

		// The cook read this chunk as air. Where the voxels on both sides of the boundary are one solidity, nothing that can
		// be reached changes with the real voxels, so there is nothing to re-cook:
		// - air on both sides: no sign change across the boundary either way, the cook is already exact. Most arrivals
		//   after a cook are the air layers over the terrain that load near the viewer;
		// - solid on both sides: the cook put a wall along the boundary (solid against the air it read), now buried inside
		//   the rock where no pawn or trace from the open can reach it. The layers under the terrain arrive this way. A dig
		//   that opens the rock there re-cooks the chunk (MarkNeighborsDirtyForEdit), so a buried wall is never exposed.
		if (!bFetchedArrived)
		{
			Arrived = ChunkManager->GetVoxelSnapshot(ChunkCoord);
			bFetchedArrived = true;
		}
		if (Arrived.IsValid())
		{
			using VoxelCollisionNeighbors::ESlab;
			const ESlab Other = VoxelCollisionNeighbors::ClassifySlab(*Arrived, ChunkSize, FIntVector::ZeroValue - Offset, OtherDepth);
			if (Other != ESlab::Mixed)
			{
				const TSharedPtr<const TArray<FVoxelData>> OwnVoxels = ChunkManager->GetVoxelSnapshot(NeighborCoord);
				const ESlab Own = OwnVoxels.IsValid() ? VoxelCollisionNeighbors::ClassifySlab(*OwnVoxels, ChunkSize, Offset, OwnDepth) : ESlab::Mixed;
				if (Own == Other)
				{
					++(Own == ESlab::Air ? TotalNeighborArrivalsSkipped : TotalNeighborArrivalsSkippedSolid);
					continue;
				}
			}
		}

		// Moved from missing to arrived; ProcessNeighborRecooks requests the re-cook once no other missing neighbour is
		// still being generated. A cook in flight read the air too: the request after it applies re-cooks it.
		Data->ArrivedNeighborMask |= Bit;
		PendingNeighborRecooks.Add(NeighborCoord);
		++Noted;
	}
	if (Noted > 0)
	{
		UE_LOG(LogVoxelCollision, Verbose, TEXT("Chunk (%d,%d,%d) voxel data arrived: %d neighbour(s) cooked without it await a re-cook"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, Noted);
	}
	return Noted;
}

int32 UVoxelCollisionManager::MarkNeighborsDirtyForEdit(const FIntVector& EditedChunk, const FVector& EditCenter, float EditRadius)
{
	if (!bIsInitialized || !Configuration)
	{
		return 0;
	}
	const double ChunkWorld = Configuration->GetChunkWorldSize();
	const int32 Stride = 1 << FMath::Clamp(CollisionLODLevel, 0, 7);
	const double Reach = EditRadius + (Stride + 2) * static_cast<double>(Configuration->VoxelSize);
	int32 Marked = 0;
	for (int32 DZ = -1; DZ <= 1; ++DZ)
	for (int32 DY = -1; DY <= 1; ++DY)
	for (int32 DX = -1; DX <= 1; ++DX)
	{
		// Edge and corner neighbours only: the face neighbours are the chunk manager's.
		if ((DX != 0) + (DY != 0) + (DZ != 0) < 2)
		{
			continue;
		}
		const FIntVector NeighborCoord = EditedChunk + FIntVector(DX, DY, DZ);
		FChunkCollisionData* Data = CollisionData.Find(NeighborCoord);
		if (!Data)
		{
			continue;
		}
		if (EditRadius > 0.0f)
		{
			const FVector Min = Configuration->WorldOrigin + FVector(NeighborCoord) * ChunkWorld;
			const FBox Box(Min, Min + FVector(ChunkWorld));
			if (Box.ComputeSquaredDistanceToPoint(EditCenter) > Reach * Reach)
			{
				continue;
			}
		}
		Data->bNeedsUpdate = true;
		Data->bNeighborRecookOnly = false;
		++Marked;
	}
	return Marked;
}

void UVoxelCollisionManager::ProcessNeighborRecooks()
{
	if (PendingNeighborRecooks.Num() == 0 || !ChunkManager)
	{
		return;
	}
	for (auto It = PendingNeighborRecooks.CreateIterator(); It; ++It)
	{
		FChunkCollisionData* Data = CollisionData.Find(*It);
		if (!Data || Data->ArrivedNeighborMask == 0)
		{
			// Removed, or a cook launched since read every arrival.
			It.RemoveCurrent();
			continue;
		}
		// Wait while another missing neighbour is on its way, so the arrivals around the chunk fold into one re-cook. A
		// neighbour that is not tracked (outside the window) does not hold it.
		bool bWaiting = false;
		for (int32 Index = 0; Index < 27 && !bWaiting; ++Index)
		{
			if ((Data->MissingNeighborMask & (1u << Index)) != 0)
			{
				const FIntVector Offset(Index % 3 - 1, (Index / 3) % 3 - 1, Index / 9 - 1);
				const EChunkState State = ChunkManager->GetChunkState(*It + Offset);
				bWaiting = State == EChunkState::PendingGeneration || State == EChunkState::Generating;
			}
		}
		if (bWaiting)
		{
			continue;
		}
		Data->ArrivedNeighborMask = 0;
		if (!Data->bNeedsUpdate)
		{
			Data->bNeedsUpdate = true;
			Data->bNeighborRecookOnly = true;
			++TotalNeighborRecooks;
		}
		It.RemoveCurrent();
	}
}

void UVoxelCollisionManager::AuditNeighborCooks(int32& OutCooked, int32& OutWithMissing, int32& OutStale, int32& OutAwaiting,
	TArray<FIntVector>* OutStaleChunks) const
{
	OutCooked = OutWithMissing = OutStale = OutAwaiting = 0;
	if (!ChunkManager)
	{
		return;
	}
	for (const auto& Pair : CollisionData)
	{
		const FChunkCollisionData& Data = Pair.Value;
		if (!Data.HasLiveCollision())
		{
			continue;
		}
		++OutCooked;
		const bool bAwaiting = (Data.bNeedsUpdate && !Data.bIsCooking) || Data.ArrivedNeighborMask != 0;
		if (bAwaiting)
		{
			++OutAwaiting;
		}
		if (Data.MissingNeighborMask == 0 && Data.ArrivedNeighborMask == 0)
		{
			continue;
		}
		++OutWithMissing;
		if (bAwaiting || Data.bIsCooking || Data.MissingNeighborMask == 0)
		{
			continue;
		}
		bool bStale = false;
		for (int32 Index = 0; Index < 27 && !bStale; ++Index)
		{
			if ((Data.MissingNeighborMask & (1u << Index)) != 0)
			{
				const FIntVector Offset(Index % 3 - 1, (Index / 3) % 3 - 1, Index / 9 - 1);
				bStale = ChunkManager->HasChunkVoxelData(Pair.Key + Offset);
			}
		}
		if (bStale)
		{
			++OutStale;
			if (OutStaleChunks)
			{
				OutStaleChunks->Add(Pair.Key);
			}
		}
	}
}

// ==================== Queries ====================

bool UVoxelCollisionManager::HasCollision(const FIntVector& ChunkCoord) const
{
	if (const FChunkCollisionData* Data = CollisionData.Find(ChunkCoord))
	{
		return Data->HasLiveCollision();
	}
	return false;
}

bool UVoxelCollisionManager::IsCollisionUpToDate(const FIntVector& ChunkCoord) const
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
	Stats += FString::Printf(TEXT("Neighbour Re-cooks: %lld (arrivals with no re-cook: %lld air on both sides, %lld solid on both sides)\n"),
		TotalNeighborRecooks, TotalNeighborArrivalsSkipped, TotalNeighborArrivalsSkippedSolid);

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
	// Tier 0: use the speed-expanded effective radius (CurrentEffectiveRadius is refreshed each
	// frame in Update()); never below CollisionRadius.
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
			const float Priority = GetDirtyPriority(*Data, Distance);

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
	Data.bNeighborRecookOnly = false;
	// This cook reads every neighbour that has arrived; ProcessNeighborRecooks drops the chunk.
	Data.ArrivedNeighborMask = 0;

	AsyncCollisionInProgress.Add(Request.ChunkCoord);

	// Prepare meshing request on the game thread (reads ChunkStates, EditManager — game thread
	// only). Voxel data comes back as SHARED snapshots (edit-merged, cached per content version
	// by the seam pipeline): the chunk's own volume AND the 26-neighborhood. The worker below
	// materializes the volume and runs the full slice extraction off the game thread — the
	// per-cook game-thread cost is snapshot map lookups plus request metadata.
	const double PrepT0 = FPlatformTime::Seconds();
	FVoxelMeshingRequest MeshRequest;
	TSharedPtr<const TArray<FVoxelData>> SharedVoxels;
	TMap<FIntVector, TSharedPtr<const TArray<FVoxelData>>> NeighborSnapshots;
	// The neighbours' boundary shells when voxel.Collision.NeighborShells is on (G69); NeighborSnapshots is then empty.
	TMap<FIntVector, TSharedPtr<const FVoxelFaceSlabs>> NeighborShells;
	uint32 MissingNeighborMask = 0;
	// Far-compressed chunks' snapshots may still be decoding on workers (G65): the cook's worker waits for them.
	TArray<UE::Tasks::FTask> SnapshotDecodes;
	if (!ChunkManager->PrepareCollisionMeshRequest(Request.ChunkCoord, CollisionLODLevel, MeshRequest, SharedVoxels, NeighborSnapshots,
		&MissingNeighborMask, &SnapshotDecodes, &NeighborShells))
	{
		// Chunk may not be loaded yet or has no geometry — remove the CollisionData
		// entry so UpdateCollisionDecisions() can re-request on a future sweep.
		CollisionData.Remove(Request.ChunkCoord);
		AsyncCollisionInProgress.Remove(Request.ChunkCoord);

		UE_LOG(LogVoxelCollision, Verbose, TEXT("Chunk (%d,%d,%d) collision mesh request preparation failed (not loaded)"),
			Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z);
		return;
	}
	// The neighbours this cook reads as air; OnChunkVoxelDataAvailable re-cooks the chunk when one of them arrives (G32).
	Data.MissingNeighborMask = MissingNeighborMask;

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
	const int32 ChunkSizeCapture = Configuration->ChunkSize;

	LastPrepMs += static_cast<float>((FPlatformTime::Seconds() - PrepT0) * 1000.0);

	// Launch async task: mesh generation + Chaos trimesh construction on thread pool
	Async(EAsyncExecution::ThreadPool, [WeakThis, MesherPtr, MeshRequest = MoveTemp(MeshRequest), SharedVoxels = MoveTemp(SharedVoxels), NeighborSnapshots = MoveTemp(NeighborSnapshots), NeighborShells = MoveTemp(NeighborShells), ChunkCoord, LODLevel, ChunkSizeCapture, SnapshotDecodes = MoveTemp(SnapshotDecodes)]() mutable
	{
		for (UE::Tasks::FTask& Decode : SnapshotDecodes)
		{
			Decode.Wait();
		}
		FAsyncCollisionResult Result;
		Result.ChunkCoord = ChunkCoord;
		Result.LODLevel = LODLevel;

		// Materialize the shared voxel snapshot into the request off the game thread (the
		// snapshot outlives cache eviction via this ref; see PrepareCollisionMeshRequest).
		if (SharedVoxels.IsValid())
		{
			MeshRequest.VoxelData = *SharedVoxels;
			SharedVoxels.Reset();
		}

		// Extract the 26-neighborhood slices from the shared snapshots off the game thread.
		// Snapshots are already edit-merged; a missing map entry means that neighbor had no
		// data at launch time (same semantics as the game-thread extraction path).
		{
			const int32 CS = ChunkSizeCapture;
			auto HasNeighborData = [&NeighborSnapshots, &NeighborShells](const FIntVector& NCoord) -> bool
			{
				return NeighborShells.Contains(NCoord) || NeighborSnapshots.Contains(NCoord);
			};
			// Memoize the last-resolved snapshot: each extraction loop reads one neighbor at
			// a time, so this avoids a TMap find per voxel (mirrors the game-thread path).
			FIntVector MemoCoord(INT32_MAX, INT32_MAX, INT32_MAX);
			const TArray<FVoxelData>* MemoArr = nullptr;
			const FVoxelFaceSlabs* MemoShell = nullptr;
			auto GetNeighborVoxel = [CS, &NeighborSnapshots, &NeighborShells, &MemoCoord, &MemoArr, &MemoShell](const FIntVector& NCoord,
				int32 X, int32 Y, int32 Z) -> FVoxelData
			{
				if (NCoord != MemoCoord)
				{
					const TSharedPtr<const FVoxelFaceSlabs>* FoundShell = NeighborShells.Find(NCoord);
					MemoShell = FoundShell ? FoundShell->Get() : nullptr;
					const TSharedPtr<const TArray<FVoxelData>>* Found = MemoShell ? nullptr : NeighborSnapshots.Find(NCoord);
					MemoArr = Found ? Found->Get() : nullptr;
					MemoCoord = NCoord;
				}
				if (MemoShell)
				{
					// The extraction reads only within GetNeighborDepth of the shared faces, which the shell holds (G69).
					ensureMsgf(MemoShell->Contains(X, Y, Z), TEXT("Collision neighbour read (%d,%d,%d) outside its depth-%d shell"),
						X, Y, Z, MemoShell->Depth);
					return MemoShell->Get(X, Y, Z);
				}
				if (!MemoArr)
				{
					return FVoxelData::Air();
				}
				const int32 Index = X + Y * CS + Z * CS * CS;
				return MemoArr->IsValidIndex(Index) ? (*MemoArr)[Index] : FVoxelData::Air();
			};
			VoxelNeighborSlices::Extract(CS, ChunkCoord, HasNeighborData, GetNeighborVoxel, MeshRequest);
			NeighborSnapshots.Empty();
			NeighborShells.Empty();
		}

		// Step 1: Generate mesh on background thread (the expensive part, ~2-4ms)
		FChunkMeshData MeshData;
		const bool bMeshSuccess = MesherPtr->GenerateMeshCPU(MeshRequest, MeshData);

		if (bMeshSuccess && !MeshData.IsValid())
		{
			// An entirely solid or entirely air chunk has no surface: nothing to collide with. That is a
			// result, not a failure -- reporting it as a failure left the chunk never ready, and the path
			// look-ahead re-requested it every frame at top priority (one chunk under a dungeon was
			// re-cooked 11,340 times in a single session, holding a cook slot the whole time).
			Result.bSuccess = true;
			Result.bEmpty = true;
		}
		else if (bMeshSuccess && MeshData.IsValid())
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

			// Render winding -> physics winding (see BuildPhysicsTriangles).
			BuildPhysicsTriangles(Indices, ChaosTriangles);

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
	// Tier 1: cvar-tunable applies-per-frame (was a hardcoded 2 — a throughput bottleneck during
	// sustained traversal). Clamped so a bad value can't stall the game thread.
	const int32 MaxApplies = FMath::Clamp(CVarCollisionMaxApplies.GetValueOnGameThread(), 1, 16);

	// Bounded drain: the budget is checked before each dequeue, so a cook beyond it stays queued for
	// the next tick. The old `while (Dequeue(Result) && Applied < Max)` popped one extra result per
	// capped tick and dropped it, leaving that chunk marked as cooking with no result coming (R23b).
	VoxelDrainBounded(CompletedCollisionQueue, MaxApplies, [this](FAsyncCollisionResult& Result) -> bool
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
			return false;
		}

		if (Result.bSuccess && Result.bEmpty)
		{
			// Cheap (no physics registration): does not count against the per-frame apply budget.
			ApplyEmptyCollisionResult(Result.ChunkCoord);
			return false;
		}
		if (Result.bSuccess)
		{
			ApplyCollisionResult(Result);
			return true;
		}

		UE_LOG(LogVoxelCollision, Warning, TEXT("Chunk (%d,%d,%d) async collision cooking failed"),
			Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z);
		Data->bIsCooking = false;
		return false;
	});
}

void UVoxelCollisionManager::BuildPhysicsTriangles(const TArray<uint32>& RenderIndices, TArray<Chaos::TVector<int32, 3>>& OutTriangles)
{
	// Physics expects the opposite winding to rendering. The engine cooker swaps the first two indices of
	// every render-derived triangle (ChaosCooking.cpp; bFlipNormals = true for static, skeletal, dynamic and
	// MR meshes). The voxel trimesh is built directly as an implicit object and skips the cooker, so it does
	// the same swap here. Without it every face points into the terrain: queries only hit thanks to
	// bDoubleSidedGeometry (and report downward normals), and rigid bodies such as ragdolls fall through,
	// because Chaos treats mesh triangles as one-sided for contacts (p.Chaos.Collision.OneSidedTriangleMesh).
	const int32 NumTriangles = RenderIndices.Num() / 3;
	OutTriangles.Reset(NumTriangles);
	for (int32 i = 0; i < NumTriangles; ++i)
	{
		OutTriangles.Add(Chaos::TVector<int32, 3>(
			static_cast<int32>(RenderIndices[i * 3 + 1]),
			static_cast<int32>(RenderIndices[i * 3 + 0]),
			static_cast<int32>(RenderIndices[i * 3 + 2])));
	}
}

void UVoxelCollisionManager::ApplyEmptyCollisionResult(const FIntVector& ChunkCoord)
{
	FChunkCollisionData* Data = CollisionData.Find(ChunkCoord);
	if (!Data)
	{
		return;
	}

	// An edit can hollow a chunk out completely: drop the body that described the old surface.
	if (Data->CollisionComponent)
	{
		Data->CollisionComponent->DestroyComponent();
		Data->CollisionComponent = nullptr;
	}
	if (Data->BodySetup)
	{
		Data->BodySetup->MarkAsGarbage();
		Data->BodySetup = nullptr;
	}

	Data->bIsCooking = false;
	Data->bEmpty = true;
	Data->EstimatedBytes = 0;

	// Waiters (spawn settle, NPC ground checks) treat "nothing to collide with" as ready too.
	OnCollisionReady.Broadcast(ChunkCoord);

	UE_LOG(LogVoxelCollision, Verbose, TEXT("Chunk (%d,%d,%d) cooked empty (no surface)"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
}

void UVoxelCollisionManager::ApplyCollisionResult(FAsyncCollisionResult& Result)
{
	const double ApplyT0 = FPlatformTime::Seconds();
	ON_SCOPE_EXIT { LastApplyMs += static_cast<float>((FPlatformTime::Seconds() - ApplyT0) * 1000.0); };

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
	Data->bEmpty = false;

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

	// The cook reads the chunk and its 26 neighbours: start the far-compressed neighbours' boundary shells decoding on
	// workers while it waits in the queue (G65, G69); the chunk's own snapshot is decoded when the cook starts.
	if (ChunkManager)
	{
		ChunkManager->PrefetchCollisionInputs(ChunkCoord, CollisionLODLevel);
	}

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
