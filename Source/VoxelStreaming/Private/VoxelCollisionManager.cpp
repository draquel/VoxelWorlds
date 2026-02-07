// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCollisionManager.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelChunkManager.h"
#include "VoxelCoordinates.h"
#include "ChunkRenderData.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "DrawDebugHelpers.h"
#include "Algo/BinarySearch.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Serialization/ArchiveCountMem.h"

// Chaos physics includes (UE5 always uses Chaos)
#include "Chaos/TriangleMeshImplicitObject.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelCollision, Log, All);

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

	// Clear any existing state
	CollisionData.Empty();
	CookingQueue.Empty();
	CookingQueueSet.Empty();
	CurrentlyCooking.Empty();

	// Reset cached state
	LastViewerPosition = FVector(FLT_MAX);
	FrameCounter = 0;
	bPendingInitialUpdate = true;

	// Reset statistics
	TotalCollisionsGenerated = 0;
	TotalCollisionsRemoved = 0;

	bIsInitialized = true;

	UE_LOG(LogVoxelCollision, Log, TEXT("VoxelCollisionManager initialized (Radius=%.0f, LOD=%d)"),
		CollisionRadius, CollisionLODLevel);
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
	CurrentlyCooking.Empty();

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

	// Increment frame counter
	++FrameCounter;

	// Only process expensive collision operations every N frames to reduce stuttering
	// Dirty chunk processing and cooking are the expensive parts (mesh generation on game thread)
	// Exception: During initial load, process every frame until collision is established
	const bool bShouldProcessThisFrame = bPendingInitialUpdate || (FrameCounter % FrameSkipInterval) == 0;

	// Check if viewer moved enough to warrant full collision update
	// This is relatively cheap (just iterating loaded chunks), so do it more often
	// Also run on every frame during initial load to catch chunks as they become available
	const float DistanceMoved = FVector::Dist(ViewerPosition, LastViewerPosition);
	if (DistanceMoved > UpdateThreshold || LastViewerPosition.X == FLT_MAX || bPendingInitialUpdate)
	{
		UpdateCollisionDecisions(ViewerPosition);
		LastViewerPosition = ViewerPosition;

		// Once we've queued or generated any collision, initial load is complete
		if (bPendingInitialUpdate && (CookingQueue.Num() > 0 || CurrentlyCooking.Num() > 0 || CollisionData.Num() > 0))
		{
			bPendingInitialUpdate = false;
			UE_LOG(LogVoxelCollision, Log, TEXT("Initial collision load complete (queued=%d, cooking=%d, ready=%d)"),
				CookingQueue.Num(), CurrentlyCooking.Num(), CollisionData.Num());
		}
	}

	// Process dirty chunks and cooking queue only on designated frames
	if (bShouldProcessThisFrame)
	{
		// Process dirty chunks (from edits)
		ProcessDirtyChunks(ViewerPosition);

		// Process cooking queue (mesh gen is synchronous but heavily throttled)
		ProcessCookingQueue();
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

// ==================== Debug ====================

FString UVoxelCollisionManager::GetDebugStats() const
{
	FString Stats = TEXT("=== VoxelCollisionManager ===\n");
	Stats += FString::Printf(TEXT("Initialized: %s\n"), bIsInitialized ? TEXT("Yes") : TEXT("No"));
	Stats += FString::Printf(TEXT("Collision Radius: %.0f\n"), CollisionRadius);
	Stats += FString::Printf(TEXT("Collision LOD: %d\n"), CollisionLODLevel);
	Stats += FString::Printf(TEXT("Chunks with Collision: %d\n"), CollisionData.Num());
	Stats += FString::Printf(TEXT("Cook Queue: %d\n"), CookingQueue.Num());
	Stats += FString::Printf(TEXT("Currently Cooking: %d\n"), CurrentlyCooking.Num());
	Stats += FString::Printf(TEXT("Total Generated: %lld\n"), TotalCollisionsGenerated);
	Stats += FString::Printf(TEXT("Total Removed: %lld\n"), TotalCollisionsRemoved);

	return Stats;
}

int64 UVoxelCollisionManager::GetTotalMemoryUsage() const
{
	int64 Total = sizeof(UVoxelCollisionManager);

	// Collision data map overhead
	Total += CollisionData.GetAllocatedSize();

	// Per-chunk: BodySetup resource size estimate
	for (const auto& Pair : CollisionData)
	{
		Total += sizeof(FChunkCollisionData);
		if (Pair.Value.BodySetup)
		{
			// Approximate: BodySetup stores cooked tri-mesh data
			FResourceSizeEx ResSize;
			Pair.Value.BodySetup->GetResourceSizeEx(ResSize);
			Total += ResSize.GetTotalMemoryBytes();
		}
	}

	// Cooking queue
	Total += CookingQueue.GetAllocatedSize();
	for (const FCollisionCookRequest& Req : CookingQueue)
	{
		Total += Req.Vertices.GetAllocatedSize() + Req.Indices.GetAllocatedSize();
	}

	Total += CookingQueueSet.GetAllocatedSize();
	Total += CurrentlyCooking.GetAllocatedSize();

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
	const float CollisionRadiusSq = CollisionRadius * CollisionRadius;

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
	// Start new cooking operations
	int32 NewCooksThisFrame = 0;

	while (CookingQueue.Num() > 0 &&
		CurrentlyCooking.Num() < MaxConcurrentCooks &&
		NewCooksThisFrame < MaxCooksPerFrame)
	{
		// Get highest priority request
		FCollisionCookRequest Request = MoveTemp(CookingQueue[0]);
		CookingQueue.RemoveAt(0);
		CookingQueueSet.Remove(Request.ChunkCoord);

		// Generate mesh data if not already provided
		// Note: This is synchronous but heavily throttled (MaxCooksPerFrame=1, every 5 frames)
		if (Request.Vertices.Num() == 0)
		{
			if (!GenerateCollisionMesh(Request.ChunkCoord, Request.Vertices, Request.Indices))
			{
				UE_LOG(LogVoxelCollision, Verbose, TEXT("Chunk (%d,%d,%d) collision mesh generation failed (empty or error)"),
					Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z);
				continue;
			}
		}

		// Start async cook
		StartAsyncCook(Request);
		++NewCooksThisFrame;
	}
}

void UVoxelCollisionManager::RequestCollision(const FIntVector& ChunkCoord, float Priority)
{
	// O(1) duplicate check
	if (CookingQueueSet.Contains(ChunkCoord) || CurrentlyCooking.Contains(ChunkCoord))
	{
		return;
	}

	// Create request
	FCollisionCookRequest Request;
	Request.ChunkCoord = ChunkCoord;
	Request.LODLevel = CollisionLODLevel;
	Request.Priority = Priority;

	// Add to tracking set
	CookingQueueSet.Add(ChunkCoord);

	// Sorted insertion (highest priority first)
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

bool UVoxelCollisionManager::GenerateCollisionMesh(
	const FIntVector& ChunkCoord,
	TArray<FVector3f>& OutVerts,
	TArray<uint32>& OutIndices)
{
	if (!ChunkManager || !Configuration)
	{
		return false;
	}

	// Generate mesh at collision LOD level
	FChunkMeshData MeshData;
	if (!ChunkManager->GetChunkCollisionMesh(ChunkCoord, CollisionLODLevel, MeshData))
	{
		// Chunk may not be loaded yet or has no geometry
		return false;
	}

	// Check if mesh has valid geometry
	if (!MeshData.IsValid())
	{
		// Empty chunk (air or solid) - valid but no collision needed
		return false;
	}

	// Copy positions and indices for collision
	OutVerts = MoveTemp(MeshData.Positions);
	OutIndices = MoveTemp(MeshData.Indices);

	UE_LOG(LogVoxelCollision, Verbose, TEXT("Generated collision mesh for chunk (%d,%d,%d) at LOD %d: %d verts, %d tris"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, CollisionLODLevel,
		OutVerts.Num(), OutIndices.Num() / 3);

	return OutVerts.Num() > 0 && OutIndices.Num() > 0;
}

void UVoxelCollisionManager::StartAsyncCook(const FCollisionCookRequest& Request)
{
	// Create or update collision data entry
	FChunkCollisionData& Data = CollisionData.FindOrAdd(Request.ChunkCoord);
	Data.ChunkCoord = Request.ChunkCoord;
	Data.CollisionLODLevel = Request.LODLevel;
	Data.bIsCooking = true;
	Data.bNeedsUpdate = false;

	// Track as currently cooking
	CurrentlyCooking.Add(Request.ChunkCoord);

	// Create body setup if needed
	if (!Data.BodySetup)
	{
		Data.BodySetup = CreateBodySetup(Request.ChunkCoord);
	}

	if (!Data.BodySetup)
	{
		UE_LOG(LogVoxelCollision, Error, TEXT("Failed to create BodySetup for chunk (%d,%d,%d)"),
			Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z);
		OnCookComplete(Request.ChunkCoord, false);
		return;
	}

	// Handle empty mesh case
	if (Request.Vertices.Num() == 0 || Request.Indices.Num() == 0)
	{
		Data.BodySetup->bCreatedPhysicsMeshes = true;
		Data.bIsCooking = false;
		CurrentlyCooking.Remove(Request.ChunkCoord);
		OnCookComplete(Request.ChunkCoord, true);
		return;
	}

	// Clear existing collision data
	Data.BodySetup->AggGeom.ConvexElems.Empty();
	Data.BodySetup->AggGeom.BoxElems.Empty();
	Data.BodySetup->AggGeom.SphereElems.Empty();
	Data.BodySetup->AggGeom.SphylElems.Empty();
	Data.BodySetup->AggGeom.TaperedCapsuleElems.Empty();

	// Set up for complex collision using trimesh
	Data.BodySetup->bMeshCollideAll = true;
	Data.BodySetup->CollisionTraceFlag = CTF_UseComplexAsSimple;
	Data.BodySetup->bHasCookedCollisionData = false;

	// Clear any existing trimeshes (UE 5.7+ uses TriMeshGeometries)
	Data.BodySetup->TriMeshGeometries.Empty();

	// For terrain collision, we create trimesh collision directly
	const int32 NumTriangles = Request.Indices.Num() / 3;
	bool bCookSuccess = false;

	// Build triangle data for Chaos physics
	TArray<Chaos::TVec3<Chaos::FRealSingle>> ChaosVertices;
	TArray<Chaos::TVector<int32, 3>> ChaosTriangles;

	ChaosVertices.Reserve(Request.Vertices.Num());
	for (const FVector3f& V : Request.Vertices)
	{
		ChaosVertices.Add(Chaos::TVec3<Chaos::FRealSingle>(V.X, V.Y, V.Z));
	}

	ChaosTriangles.Reserve(NumTriangles);
	for (int32 i = 0; i < NumTriangles; ++i)
	{
		ChaosTriangles.Add(Chaos::TVector<int32, 3>(
			static_cast<int32>(Request.Indices[i * 3 + 0]),
			static_cast<int32>(Request.Indices[i * 3 + 1]),
			static_cast<int32>(Request.Indices[i * 3 + 2])
		));
	}

	// Create the Chaos trimesh implicit object (UE 5.7+ uses TRefCountPtr)
	TRefCountPtr<Chaos::FTriangleMeshImplicitObject> TriMesh = new Chaos::FTriangleMeshImplicitObject(
		MoveTemp(ChaosVertices),
		MoveTemp(ChaosTriangles),
		TArray<uint16>() // Empty materials array (single material for terrain)
	);

	if (TriMesh.IsValid())
	{
		Data.BodySetup->TriMeshGeometries.Add(TriMesh);
		Data.BodySetup->bCreatedPhysicsMeshes = true;
		bCookSuccess = true;
	}

	// Mark cooking complete
	Data.bIsCooking = false;
	CurrentlyCooking.Remove(Request.ChunkCoord);

	if (bCookSuccess)
	{
		// Destroy old collision component if it exists (from previous cook/edit)
		if (Data.CollisionComponent)
		{
			Data.CollisionComponent->DestroyComponent();
			Data.CollisionComponent = nullptr;
		}

		// Create collision component to register with physics
		Data.CollisionComponent = CreateCollisionComponent(Request.ChunkCoord, Data.BodySetup);

		if (Data.CollisionComponent)
		{
			++TotalCollisionsGenerated;
			OnCollisionReady.Broadcast(Request.ChunkCoord);

			UE_LOG(LogVoxelCollision, Log, TEXT("Created collision for chunk (%d,%d,%d) (%d verts, %d tris)"),
				Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z,
				Request.Vertices.Num(), NumTriangles);
		}
		else
		{
			UE_LOG(LogVoxelCollision, Warning, TEXT("Failed to create collision component for chunk (%d,%d,%d)"),
				Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z);
		}
	}
	else
	{
		UE_LOG(LogVoxelCollision, Warning, TEXT("Collision cooking failed for chunk (%d,%d,%d)"),
			Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z);
		OnCookComplete(Request.ChunkCoord, false);
	}
}

void UVoxelCollisionManager::OnCookComplete(const FIntVector& ChunkCoord, bool bSuccess)
{
	CurrentlyCooking.Remove(ChunkCoord);

	if (FChunkCollisionData* Data = CollisionData.Find(ChunkCoord))
	{
		Data->bIsCooking = false;

		if (bSuccess)
		{
			++TotalCollisionsGenerated;
			OnCollisionReady.Broadcast(ChunkCoord);

			UE_LOG(LogVoxelCollision, Verbose, TEXT("Chunk (%d,%d,%d) collision ready"),
				ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
		}
		else
		{
			UE_LOG(LogVoxelCollision, Warning, TEXT("Chunk (%d,%d,%d) collision cooking failed"),
				ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);

			// Remove failed collision data
			if (Data->BodySetup)
			{
				Data->BodySetup->MarkAsGarbage();
			}
			CollisionData.Remove(ChunkCoord);
		}
	}
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

	// Create a box component as the collision holder
	// We override its body setup after creation
	UBoxComponent* CollisionComp = NewObject<UBoxComponent>(
		CollisionContainerActor,
		FName(*ComponentName)
	);

	if (!CollisionComp)
	{
		return nullptr;
	}

	// Set up the component - make it large enough to cover the chunk
	CollisionComp->SetupAttachment(CollisionContainerActor->GetRootComponent());
	CollisionComp->SetWorldLocation(ChunkWorldPos);
	CollisionComp->SetBoxExtent(FVector(ChunkWorldSize * 0.5f)); // Box extent is half-size
	CollisionComp->SetVisibility(false);
	CollisionComp->SetHiddenInGame(true);

	// Configure collision
	CollisionComp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	CollisionComp->SetCollisionObjectType(ECollisionChannel::ECC_WorldStatic);
	CollisionComp->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);

	// Register the component first (needed before we can modify physics)
	CollisionComp->RegisterComponent();

	// Get the body instance and override its body setup with our trimesh
	FBodyInstance* BodyInst = CollisionComp->GetBodyInstance();
	if (BodyInst)
	{
		// Terminate existing physics state
		BodyInst->TermBody();

		// Set our custom body setup
		BodyInst->BodySetup = BodySetup;

		// Reinitialize physics with our body setup
		BodyInst->InitBody(BodySetup, CollisionComp->GetComponentTransform(), CollisionComp, CachedWorld->GetPhysicsScene());
	}

	UE_LOG(LogVoxelCollision, Log, TEXT("Created collision component for chunk (%d,%d,%d) at (%.0f, %.0f, %.0f)"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z,
		ChunkWorldPos.X, ChunkWorldPos.Y, ChunkWorldPos.Z);

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
