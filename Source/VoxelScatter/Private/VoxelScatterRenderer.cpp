// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelScatterRenderer.h"
#include "VoxelScatterManager.h"
#include "VoxelBillboardMeshGenerator.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelScatterRenderer, Log, All);

static TAutoConsoleVariable<float> CVarVoxelScatterHISMBatchSeconds(
	TEXT("voxel.Scatter.HISMBatchSeconds"),
	0.1f,
	TEXT("Scatter HISM writes (world modes review G42): a HISM takes the instance changes held for it at most this often\n")
	TEXT("(seconds), and not while its cluster tree builds; releases zero-scale each instance where it stands. 0 = write every\n")
	TEXT("change to its HISM at once, releases parked at the HISM's origin (the behaviour before G42)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarVoxelScatterHISMMaxHoldSeconds(
	TEXT("voxel.Scatter.HISMMaxHoldSeconds"),
	0.5f,
	TEXT("Scatter HISM writes (world modes review G42): the longest a HISM's held changes wait for its cluster tree build (seconds)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarVoxelScatterHISMCompactMinPooled(
	TEXT("voxel.Scatter.HISMCompactMinPooled"),
	1024,
	TEXT("Scatter HISM writes (world modes review G42): a HISM taking a batch whose pooled (released, zero-scaled) instances are at\n")
	TEXT("least this many and outnumber its live ones is rewritten with its live instances only. 0 = never (pools only grow)."),
	ECVF_Default);

UVoxelScatterRenderer::UVoxelScatterRenderer()
{
}

// ==================== Lifecycle ====================

void UVoxelScatterRenderer::Initialize(UVoxelScatterManager* Manager, UWorld* World)
{
	if (bIsInitialized)
	{
		UE_LOG(LogVoxelScatterRenderer, Warning, TEXT("ScatterRenderer already initialized"));
		return;
	}

	if (!Manager || !World)
	{
		UE_LOG(LogVoxelScatterRenderer, Error, TEXT("ScatterRenderer::Initialize - Invalid Manager or World"));
		return;
	}

	ScatterManager = Manager;
	CachedWorld = World;

	// Create container actor for HISM components. Requested name mode: keep the recognizable
	// name when free, but auto-uniquify instead of the default fatal error when a container
	// already exists (a second scatter manager in the same world — e.g. two voxel worlds, or
	// a manager cycled before GC reclaimed the old container).
	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = TEXT("VoxelScatterContainer");
	SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
	SpawnParams.ObjectFlags |= RF_Transient;

	ContainerActor = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	if (ContainerActor)
	{
#if WITH_EDITOR
		ContainerActor->SetActorLabel(TEXT("VoxelScatterContainer"));
#endif

		// Add a root component for the actor
		USceneComponent* RootComponent = NewObject<USceneComponent>(ContainerActor, TEXT("RootComponent"));
		ContainerActor->SetRootComponent(RootComponent);
		RootComponent->RegisterComponent();
	}
	else
	{
		UE_LOG(LogVoxelScatterRenderer, Error, TEXT("Failed to create container actor"));
		return;
	}

	bIsInitialized = true;
	UE_LOG(LogVoxelScatterRenderer, Log, TEXT("ScatterRenderer initialized"));
}

void UVoxelScatterRenderer::Tick(const FVector& /*ViewerPosition*/, float /*DeltaTime*/)
{
	if (!bIsInitialized || !ScatterManager)
	{
		return;
	}

	// Flush deferred instance additions within the per-frame budget. Every scatter update —
	// new chunks, supplemental streaming, edits, and per-chunk regen — flows through this one
	// budgeted path. There are no deferred whole-type rebuilds, so nothing needs to be gated on
	// viewer movement or a quiet generation pipeline (which is what used to make queued rebuilds
	// all fire at once when the player stopped — the refresh flash).
	FlushPendingInstanceAdds();

	// The HISMs whose batch is due take their held writes (world modes review G42)
	ApplyPendingHISMWrites(/*bAll=*/false);
}

void UVoxelScatterRenderer::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	// Clear all instances first
	ClearAllInstances();

	// Destroy HISM components
	for (auto& Pair : HISMComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->DestroyComponent();
		}
	}
	HISMComponents.Empty();
	AllHISMComponents.Empty();

	// Destroy container actor
	if (ContainerActor)
	{
		ContainerActor->Destroy();
		ContainerActor = nullptr;
	}

	ChunkScatterTypes.Empty();
	InstancePools.Empty();
	ScatterManager = nullptr;
	CachedWorld = nullptr;
	bIsInitialized = false;

	UE_LOG(LogVoxelScatterRenderer, Log, TEXT("ScatterRenderer shutdown (Added: %lld, Removed: %lld)"),
		TotalInstancesAdded, TotalInstancesRemoved);
}

// ==================== Instance Management ====================

void UVoxelScatterRenderer::UpdateChunkInstances(const FIntVector& ChunkCoord, const FChunkScatterData& ScatterData)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelScatter_UpdateChunkInstances);
	if (!bIsInitialized || !ScatterManager)
	{
		return;
	}

	// Check if this chunk already has scatter (update case vs new chunk case)
	TSet<int32>* ExistingTypes = ChunkScatterTypes.Find(ChunkCoord);
	const bool bIsNewChunk = (ExistingTypes == nullptr || ExistingTypes->Num() == 0);

	if (bIsNewChunk && ScatterData.bIsValid && ScatterData.SpawnPoints.Num() > 0)
	{
		// NEW CHUNK: Queue instances for deferred addition (budget-limited per frame)
		// This prevents frame spikes when many chunks complete scatter simultaneously

		// Group spawn points by scatter type for batch adding
		TMap<int32, TArray<FTransform>> TransformsByType;
		for (const FScatterSpawnPoint& Point : ScatterData.SpawnPoints)
		{
			const FScatterDefinition* Definition = ScatterManager->GetScatterDefinition(Point.ScatterTypeID);
			if (Definition)
			{
				FTransform Transform = Point.GetTransform(Definition->bAlignToSurfaceNormal, Definition->SurfaceOffset);
				TransformsByType.FindOrAdd(Point.ScatterTypeID).Add(Transform);
			}
		}

		// Track scatter types for this chunk and queue instance additions
		TSet<int32>& NewTypes = ChunkScatterTypes.FindOrAdd(ChunkCoord);
		for (auto& Pair : TransformsByType)
		{
			const int32 ScatterTypeID = Pair.Key;
			TArray<FTransform>& Transforms = Pair.Value;

			if (Transforms.Num() > 0)
			{
				NewTypes.Add(ScatterTypeID);

				FPendingInstanceAdd PendingAdd;
				PendingAdd.ScatterTypeID = ScatterTypeID;
				PendingAdd.ChunkCoord = ChunkCoord;
				PendingAdd.Transforms = MoveTemp(Transforms);
				PendingInstanceAdds.Add(MoveTemp(PendingAdd));
			}
		}

		UE_LOG(LogVoxelScatterRenderer, Verbose, TEXT("Chunk (%d,%d,%d): Queued %d instances for deferred addition (new chunk)"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, ScatterData.SpawnPoints.Num());
	}
	else
	{
		// EXISTING CHUNK UPDATE: replace this chunk's instances per affected type, in place.
		// This is deliberately NOT a whole-type rebuild — a rebuild releases and re-streams
		// every instance of the type across the entire world (the "refresh flash").
		// UpdateChunkTypeInstances touches only this chunk+type and flushes via the per-frame
		// add budget every tick, so a re-mesh/regen of one chunk stays local and immediate.

		// Affected types = union of what this chunk had and what it now has.
		TSet<int32> AffectedTypes;
		if (ExistingTypes)
		{
			AffectedTypes.Append(*ExistingTypes);
		}

		// Group this chunk's new spawn points by scatter type.
		TMap<int32, TArray<FTransform>> TransformsByType;
		if (ScatterData.bIsValid)
		{
			for (const FScatterSpawnPoint& Point : ScatterData.SpawnPoints)
			{
				const FScatterDefinition* Definition = ScatterManager->GetScatterDefinition(Point.ScatterTypeID);
				if (Definition)
				{
					FTransform Transform = Point.GetTransform(Definition->bAlignToSurfaceNormal, Definition->SurfaceOffset);
					TransformsByType.FindOrAdd(Point.ScatterTypeID).Add(Transform);
					AffectedTypes.Add(Point.ScatterTypeID);
				}
			}
		}

		// Replace each affected type's instances for this chunk. Empty transforms (a type the
		// chunk no longer has) releases that type's instances back to the pool.
		for (int32 ScatterTypeID : AffectedTypes)
		{
			TArray<FTransform>* Found = TransformsByType.Find(ScatterTypeID);
			UpdateChunkTypeInstances(ChunkCoord, ScatterTypeID,
				Found ? MoveTemp(*Found) : TArray<FTransform>());
		}
	}
}

void UVoxelScatterRenderer::AddSupplementalInstances(const FIntVector& ChunkCoord, const FChunkScatterData& NewScatterData)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelScatter_AddSupplemental);
	if (!bIsInitialized || !ScatterManager)
	{
		return;
	}

	if (!NewScatterData.bIsValid || NewScatterData.SpawnPoints.Num() == 0)
	{
		return;
	}

	// Group new spawn points by scatter type
	TMap<int32, TArray<FTransform>> TransformsByType;
	for (const FScatterSpawnPoint& Point : NewScatterData.SpawnPoints)
	{
		const FScatterDefinition* Definition = ScatterManager->GetScatterDefinition(Point.ScatterTypeID);
		if (Definition)
		{
			FTransform Transform = Point.GetTransform(Definition->bAlignToSurfaceNormal, Definition->SurfaceOffset);
			TransformsByType.FindOrAdd(Point.ScatterTypeID).Add(Transform);
		}
	}

	// Update chunk tracking and queue deferred instance additions
	// No stale type reconciliation needed — cleanup uses ReleaseChunkScatterType
	// which zero-scales instances and returns them to the pool silently.
	TSet<int32>& ChunkTypes = ChunkScatterTypes.FindOrAdd(ChunkCoord);
	for (auto& Pair : TransformsByType)
	{
		const int32 ScatterTypeID = Pair.Key;
		TArray<FTransform>& Transforms = Pair.Value;

		if (Transforms.Num() > 0)
		{
			ChunkTypes.Add(ScatterTypeID);

			FPendingInstanceAdd PendingAdd;
			PendingAdd.ScatterTypeID = ScatterTypeID;
			PendingAdd.ChunkCoord = ChunkCoord;
			PendingAdd.Transforms = MoveTemp(Transforms);
			PendingInstanceAdds.Add(MoveTemp(PendingAdd));
		}
	}

	UE_LOG(LogVoxelScatterRenderer, Verbose, TEXT("Chunk (%d,%d,%d): Queued %d supplemental instances"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, NewScatterData.SpawnPoints.Num());
}

void UVoxelScatterRenderer::UpdateChunkTypeInstances(const FIntVector& ChunkCoord, int32 ScatterTypeID, TArray<FTransform> Transforms)
{
	if (!bIsInitialized)
	{
		return;
	}

	// Zero-scale current instances for this chunk+type and return them to the pool; this also drops the pair's queued
	// adds, so they don't stack on top of the replacement
	ReleaseChunkScatterType(ChunkCoord, ScatterTypeID);

	if (Transforms.Num() == 0)
	{
		return;
	}

	ChunkScatterTypes.FindOrAdd(ChunkCoord).Add(ScatterTypeID);

	FPendingInstanceAdd PendingAdd;
	PendingAdd.ScatterTypeID = ScatterTypeID;
	PendingAdd.ChunkCoord = ChunkCoord;
	PendingAdd.Transforms = MoveTemp(Transforms);
	PendingInstanceAdds.Add(MoveTemp(PendingAdd));
}

void UVoxelScatterRenderer::RemoveChunkInstances(const FIntVector& ChunkCoord)
{
	if (!bIsInitialized)
	{
		return;
	}

	TSet<int32>* ScatterTypes = ChunkScatterTypes.Find(ChunkCoord);
	if (!ScatterTypes || ScatterTypes->Num() == 0)
	{
		ChunkScatterTypes.Remove(ChunkCoord);
		return;
	}

	// Release all scatter types for this chunk back to the pool
	// Copy the set since ReleaseChunkScatterType modifies ChunkScatterTypes
	TArray<int32> TypesToRelease = ScatterTypes->Array();
	for (int32 ScatterTypeID : TypesToRelease)
	{
		ReleaseChunkScatterType(ChunkCoord, ScatterTypeID);
	}

	// Also discard any pending instance adds for this chunk
	PendingInstanceAdds.RemoveAll([&ChunkCoord](const FPendingInstanceAdd& Add)
	{
		return Add.ChunkCoord == ChunkCoord;
	});
}

void UVoxelScatterRenderer::ReleaseChunkScatterType(const FIntVector& ChunkCoord, int32 ScatterTypeID)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelScatter_ReleaseChunkType);
	if (!bIsInitialized)
	{
		return;
	}

	// Queued adds for the pair go too: flushed after the release they would land in the pool for a type the chunk no
	// longer tracks, which RemoveChunkInstances never releases (world modes review G30, the distance cleanup's release).
	PendingInstanceAdds.RemoveAll([&ChunkCoord, ScatterTypeID](const FPendingInstanceAdd& Add)
	{
		return Add.ChunkCoord == ChunkCoord && Add.ScatterTypeID == ScatterTypeID;
	});

	// The chunk's HISM: its type's in the chunk's render region (WM-BP7)
	const FVoxelScatterHISMKey Key{ ScatterTypeID, Regions.GetRegion(ChunkCoord) };
	FHISMInstancePool* Pool = InstancePools.Find(Key);
	if (!Pool)
	{
		// No pool means no instances were ever added for this type — just update tracking
		TSet<int32>* ChunkTypes = ChunkScatterTypes.Find(ChunkCoord);
		if (ChunkTypes)
		{
			ChunkTypes->Remove(ScatterTypeID);
			if (ChunkTypes->Num() == 0)
			{
				ChunkScatterTypes.Remove(ChunkCoord);
			}
		}
		return;
	}

	TArray<int32>* Indices = Pool->ChunkInstanceIndices.Find(ChunkCoord);
	if (!Indices || Indices->Num() == 0)
	{
		Pool->ChunkInstanceIndices.Remove(ChunkCoord);
		// Update chunk tracking
		TSet<int32>* ChunkTypes = ChunkScatterTypes.Find(ChunkCoord);
		if (ChunkTypes)
		{
			ChunkTypes->Remove(ScatterTypeID);
			if (ChunkTypes->Num() == 0)
			{
				ChunkScatterTypes.Remove(ChunkCoord);
			}
		}
		return;
	}

	// Zero-scale all instances and move to free list
	UHierarchicalInstancedStaticMeshComponent* HISM = nullptr;
	if (TObjectPtr<UHierarchicalInstancedStaticMeshComponent>* Found = HISMComponents.Find(Key))
	{
		HISM = Found->Get();
	}

	if (HISM)
	{
		if (IsBatchingHISMWrites())
		{
			// Held for the HISM's next batch (world modes review G42), zero scale where each instance stands: an in-place
			// change, which the HISM takes without rebuilding its cluster tree. A placement still held for an index is
			// dropped (its location never reached the HISM), so the release stays in place against what the HISM holds.
			FPendingHISMWrites& Writes = PendingHISMWrites.FindOrAdd(Key);
			const int32 Applied = HISM->GetInstanceCount();
			for (int32 Index : *Indices)
			{
				if (Index >= Applied)
				{
					if (Writes.Grows.IsValidIndex(Index - Applied))
					{
						FTransform& Grow = Writes.Grows[Index - Applied];
						Grow = FTransform(FQuat::Identity, Grow.GetLocation(), FVector::ZeroVector);
					}
					continue;
				}
				FTransform Stored;
				if (HISM->GetInstanceTransform(Index, Stored, /*bWorldSpace=*/false))
				{
					Writes.Updates.Add(Index, FTransform(FQuat::Identity, Stored.GetLocation(), FVector::ZeroVector));
				}
			}
		}
		else
		{
			// Writes held while batching was on land first, so the indices below are all in the HISM. Parked at the HISM's
			// own origin (local space): its region's origin, the world origin on a heightmap world, so a region's bounds
			// never reach back to the world origin
			ApplyPendingHISMWrites(/*bAll=*/true);
			const FTransform ZeroTransform(FQuat::Identity, FVector::ZeroVector, FVector::ZeroVector);
			for (int32 Index : *Indices)
			{
				HISM->UpdateInstanceTransform(Index, ZeroTransform,
					/*bWorldSpace=*/false, /*bMarkRenderStateDirty=*/false, /*bTeleport=*/true);
			}
			HISM->MarkRenderStateDirty();
		}
	}

	// Move indices to free list
	Pool->FreeIndices.Append(*Indices);
	TotalInstancesRemoved += Indices->Num();

	// Clean up chunk entry from pool
	Pool->ChunkInstanceIndices.Remove(ChunkCoord);

	// Update chunk scatter type tracking
	TSet<int32>* ChunkTypes = ChunkScatterTypes.Find(ChunkCoord);
	if (ChunkTypes)
	{
		ChunkTypes->Remove(ScatterTypeID);
		if (ChunkTypes->Num() == 0)
		{
			ChunkScatterTypes.Remove(ChunkCoord);
		}
	}
}

void UVoxelScatterRenderer::ClearAllInstances()
{
	if (!bIsInitialized)
	{
		return;
	}

	// Held writes go with the instances
	PendingHISMWrites.Empty();
	LastHISMWriteSeconds.Empty();

	// Clear all HISM components
	for (auto& Pair : HISMComponents)
	{
		if (Pair.Value)
		{
			TotalInstancesRemoved += Pair.Value->GetInstanceCount();
			Pair.Value->ClearInstances();
		}
	}

	// Clear tracking and pools
	ChunkScatterTypes.Empty();
	InstancePools.Empty();
}

// ==================== HISM Management ====================

UHierarchicalInstancedStaticMeshComponent* UVoxelScatterRenderer::GetOrCreateHISM(int32 ScatterTypeID, const FIntVector& Region)
{
	if (!bIsInitialized || !ScatterManager || !ContainerActor)
	{
		return nullptr;
	}

	// Check if already exists
	const FVoxelScatterHISMKey Key{ ScatterTypeID, Region };
	if (TObjectPtr<UHierarchicalInstancedStaticMeshComponent>* ExistingHISM = HISMComponents.Find(Key))
	{
		return ExistingHISM->Get();
	}

	// Get scatter definition
	const FScatterDefinition* Definition = ScatterManager->GetScatterDefinition(ScatterTypeID);
	if (!Definition)
	{
		return nullptr;
	}

	// CrossBillboard types use runtime-generated meshes, so Mesh soft pointer is not set
	if (Definition->MeshType != EScatterMeshType::CrossBillboard && Definition->Mesh.IsNull())
	{
		UE_LOG(LogVoxelScatterRenderer, Warning,
			TEXT("Scatter type %d (%s): Mesh soft reference is null — cannot create HISM. "
				 "Assign a static mesh in the ScatterConfiguration data asset."),
			ScatterTypeID, *Definition->Name);
		return nullptr;
	}

	// Create new HISM
	UHierarchicalInstancedStaticMeshComponent* HISM = CreateHISMComponent(*Definition, Region);
	if (HISM)
	{
		HISMComponents.Add(Key, HISM);
		AllHISMComponents.Add(HISM);
	}

	return HISM;
}

void UVoxelScatterRenderer::GetTypeHISMs(int32 ScatterTypeID, TArray<TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>>& OutHISMs) const
{
	OutHISMs.Reset();
	for (const auto& Pair : HISMComponents)
	{
		if (Pair.Key.ScatterTypeID == ScatterTypeID && Pair.Value)
		{
			OutHISMs.Emplace(Pair.Key.Region, Pair.Value.Get());
		}
	}
}

int32 UVoxelScatterRenderer::GetTotalInstanceCount() const
{
	int32 Total = 0;
	for (const auto& Pair : HISMComponents)
	{
		if (Pair.Value)
		{
			Total += GetEffectiveInstanceCount(Pair.Key, Pair.Value.Get());
		}
	}
	return Total;
}

// ==================== Batched HISM Writes (world modes review G42) ====================

bool UVoxelScatterRenderer::IsBatchingHISMWrites()
{
	return CVarVoxelScatterHISMBatchSeconds.GetValueOnGameThread() > 0.0f;
}

bool UVoxelScatterRenderer::GetEffectiveInstanceTransform(const FVoxelScatterHISMKey& Key, const UHierarchicalInstancedStaticMeshComponent* HISM,
	int32 Index, FTransform& OutLocal) const
{
	if (!HISM)
	{
		return false;
	}
	const int32 Applied = HISM->GetInstanceCount();
	if (const FPendingHISMWrites* Writes = PendingHISMWrites.Find(Key))
	{
		if (Index >= Applied)
		{
			if (!Writes->Grows.IsValidIndex(Index - Applied))
			{
				return false;
			}
			OutLocal = Writes->Grows[Index - Applied];
			return true;
		}
		if (const FTransform* Held = Writes->Updates.Find(Index))
		{
			OutLocal = *Held;
			return true;
		}
	}
	return HISM->GetInstanceTransform(Index, OutLocal, /*bWorldSpace=*/false);
}

int32 UVoxelScatterRenderer::GetEffectiveInstanceCount(const FVoxelScatterHISMKey& Key, const UHierarchicalInstancedStaticMeshComponent* HISM) const
{
	const FPendingHISMWrites* Writes = PendingHISMWrites.Find(Key);
	return (HISM ? HISM->GetInstanceCount() : 0) + (Writes ? Writes->Grows.Num() : 0);
}

int32 UVoxelScatterRenderer::GetPendingHISMWriteCount() const
{
	int32 Count = 0;
	for (const auto& Pair : PendingHISMWrites)
	{
		Count += Pair.Value.Updates.Num() + Pair.Value.Grows.Num();
	}
	return Count;
}

void UVoxelScatterRenderer::CompactHISM(const FVoxelScatterHISMKey& Key, UHierarchicalInstancedStaticMeshComponent* HISM, FHISMInstancePool& Pool)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelScatter_CompactHISM);
	// Each chunk's instances as they will be once the held writes land, renumbered from 0 in that order. Read before the
	// caller drops the held writes and before the HISM is cleared.
	TArray<FTransform> Live;
	Live.Reserve(GetEffectiveInstanceCount(Key, HISM) - Pool.FreeIndices.Num());
	for (TPair<FIntVector, TArray<int32>>& ChunkPair : Pool.ChunkInstanceIndices)
	{
		for (int32& Index : ChunkPair.Value)
		{
			// An index the HISM cannot give back (none expected) stays hidden rather than drawn at the origin
			FTransform Local(FQuat::Identity, FVector::ZeroVector, FVector::ZeroVector);
			GetEffectiveInstanceTransform(Key, HISM, Index, Local);
			Index = Live.Add(Local);
		}
	}
	HISM->ClearInstances();
	if (Live.Num() > 0)
	{
		HISM->AddInstances(Live, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false);
	}
	Pool.FreeIndices.Reset();
	Pool.TotalAllocated = Live.Num();
	++TotalHISMCompactions;
}

void UVoxelScatterRenderer::FlushHISMWrites()
{
	ApplyPendingHISMWrites(/*bAll=*/true);
}

void UVoxelScatterRenderer::ApplyPendingHISMWrites(bool bAll)
{
	if (PendingHISMWrites.Num() == 0)
	{
		return;
	}
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelScatter_ApplyHISMWrites);
	const double Now = FPlatformTime::Seconds();
	const double Interval = FMath::Max(0.0f, CVarVoxelScatterHISMBatchSeconds.GetValueOnGameThread());
	const double MaxHold = FMath::Max(Interval, static_cast<double>(CVarVoxelScatterHISMMaxHoldSeconds.GetValueOnGameThread()));
	for (auto It = PendingHISMWrites.CreateIterator(); It; ++It)
	{
		UHierarchicalInstancedStaticMeshComponent* HISM = nullptr;
		if (const TObjectPtr<UHierarchicalInstancedStaticMeshComponent>* Found = HISMComponents.Find(It.Key()))
		{
			HISM = Found->Get();
		}
		if (!HISM)
		{
			It.RemoveCurrent();
			continue;
		}
		if (!bAll)
		{
			// At most once per interval, and not while the last batch's tree builds: a change during the build would throw
			// it away and start another, unless the writes have waited too long
			const double* Last = LastHISMWriteSeconds.Find(It.Key());
			const double Since = Last ? Now - *Last : TNumericLimits<double>::Max();
			if (Since < Interval || (HISM->IsAsyncBuilding() && Since < MaxHold))
			{
				continue;
			}
		}
		// A HISM whose pool outweighs its live instances is rewritten with the live ones, which lands its writes too
		const int32 MinPooled = CVarVoxelScatterHISMCompactMinPooled.GetValueOnGameThread();
		if (FHISMInstancePool* Pool = InstancePools.Find(It.Key()); Pool && MinPooled > 0)
		{
			const int32 Pooled = Pool->FreeIndices.Num();
			if (Pooled >= MinPooled && Pooled > GetEffectiveInstanceCount(It.Key(), HISM) - Pooled)
			{
				CompactHISM(It.Key(), HISM, *Pool);
				LastHISMWriteSeconds.Add(It.Key(), Now);
				++TotalHISMWriteBatches;
				It.RemoveCurrent();
				continue;
			}
		}
		FPendingHISMWrites& Writes = It.Value();
		for (const TPair<int32, FTransform>& Update : Writes.Updates)
		{
			HISM->UpdateInstanceTransform(Update.Key, Update.Value, /*bWorldSpace=*/false, /*bMarkRenderStateDirty=*/false, /*bTeleport=*/true);
		}
		if (Writes.Grows.Num() > 0)
		{
			HISM->AddInstances(Writes.Grows, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false);
		}
		// No MarkRenderStateDirty: the HISM sends the changed instances to its scene proxy itself (in-place zero scales at
		// once; placements and new instances drawn unbuilt until its tree build lands, which recreates the proxy). Marking it
		// dirty recreated the proxy, and every instance's distance-field object, a second time per batch.
		LastHISMWriteSeconds.Add(It.Key(), Now);
		++TotalHISMWriteBatches;
		It.RemoveCurrent();
	}
}

// ==================== Debug ====================

FString UVoxelScatterRenderer::GetHISMBreakdown(int32 TopN) const
{
	struct FRow
	{
		FVoxelScatterHISMKey Key;
		int32 Instances = 0;
		int32 Pooled = 0;
	};
	TArray<FRow> Rows;
	TMap<int32, FRow> ByType;
	for (const auto& Pair : HISMComponents)
	{
		const UHierarchicalInstancedStaticMeshComponent* HISM = Pair.Value.Get();
		if (!HISM)
		{
			continue;
		}
		FRow Row;
		Row.Key = Pair.Key;
		Row.Instances = GetEffectiveInstanceCount(Pair.Key, HISM);
		if (const FHISMInstancePool* Pool = InstancePools.Find(Pair.Key))
		{
			Row.Pooled = Pool->FreeIndices.Num();
		}
		Rows.Add(Row);
		FRow& Type = ByType.FindOrAdd(Pair.Key.ScatterTypeID);
		Type.Key.ScatterTypeID = Pair.Key.ScatterTypeID;
		Type.Key.Region.X += 1; // HISM count
		Type.Instances += Row.Instances;
		Type.Pooled += Row.Pooled;
		Type.Key.Region.Y = FMath::Max(Type.Key.Region.Y, Row.Instances); // largest HISM
	}
	FString Out = FString::Printf(TEXT("Scatter HISMs: %d in regions of %d chunks"), Rows.Num(), Regions.RegionChunks);
	TArray<FRow> Types;
	ByType.GenerateValueArray(Types);
	Types.Sort([](const FRow& A, const FRow& B) { return A.Instances > B.Instances; });
	for (const FRow& Type : Types)
	{
		const FScatterDefinition* Definition = ScatterManager ? ScatterManager->GetScatterDefinition(Type.Key.ScatterTypeID) : nullptr;
		Out += FString::Printf(TEXT("\n  type %d %s: %d HISMs, %d instances (%d pooled), largest %d"), Type.Key.ScatterTypeID,
			Definition ? *Definition->Name : TEXT("?"), Type.Key.Region.X, Type.Instances, Type.Pooled, Type.Key.Region.Y);
	}
	Rows.Sort([](const FRow& A, const FRow& B) { return A.Instances > B.Instances; });
	for (int32 i = 0; i < FMath::Min(TopN, Rows.Num()); ++i)
	{
		Out += FString::Printf(TEXT("\n  HISM type %d region (%d,%d,%d): %d instances (%d pooled)"), Rows[i].Key.ScatterTypeID,
			Rows[i].Key.Region.X, Rows[i].Key.Region.Y, Rows[i].Key.Region.Z, Rows[i].Instances, Rows[i].Pooled);
	}
	return Out;
}

int64 UVoxelScatterRenderer::GetTotalMemoryUsage() const
{
	int64 Total = sizeof(UVoxelScatterRenderer);

	// HISM components map
	Total += HISMComponents.GetAllocatedSize();

	// Instance count × FTransform size (approximate HISM instance storage)
	for (const auto& Pair : HISMComponents)
	{
		if (Pair.Value)
		{
			Total += Pair.Value->GetInstanceCount() * static_cast<int64>(sizeof(FTransform));
		}
	}

	// Chunk-to-scatter-type tracking
	Total += ChunkScatterTypes.GetAllocatedSize();
	for (const auto& Pair : ChunkScatterTypes)
	{
		Total += Pair.Value.GetAllocatedSize();
	}

	// Instance pool tracking
	Total += InstancePools.GetAllocatedSize();
	for (const auto& PoolPair : InstancePools)
	{
		const FHISMInstancePool& Pool = PoolPair.Value;
		Total += Pool.FreeIndices.GetAllocatedSize();
		Total += Pool.ChunkInstanceIndices.GetAllocatedSize();
		for (const auto& ChunkPair : Pool.ChunkInstanceIndices)
		{
			Total += ChunkPair.Value.GetAllocatedSize();
		}
	}

	return Total;
}

// ==================== Queries ====================

int32 UVoxelScatterRenderer::GetChunkInstanceCount(const FIntVector& ChunkCoord, int32 ScatterTypeID) const
{
	int32 Count = 0;
	for (const auto& Pair : InstancePools)
	{
		const TArray<int32>* Indices = (ScatterTypeID == INDEX_NONE || Pair.Key.ScatterTypeID == ScatterTypeID) ? Pair.Value.ChunkInstanceIndices.Find(ChunkCoord) : nullptr;
		Count += Indices ? Indices->Num() : 0;
	}
	for (const FPendingInstanceAdd& Add : PendingInstanceAdds)
	{
		Count += (Add.ChunkCoord == ChunkCoord && (ScatterTypeID == INDEX_NONE || Add.ScatterTypeID == ScatterTypeID)) ? Add.Transforms.Num() : 0;
	}
	return Count;
}

TArray<int32> UVoxelScatterRenderer::GetChunkScatterTypes(const FIntVector& ChunkCoord) const
{
	const TSet<int32>* Types = ChunkScatterTypes.Find(ChunkCoord);
	return Types ? Types->Array() : TArray<int32>();
}

bool UVoxelScatterRenderer::FindNearestInstance(int32 ScatterTypeID, const FVector& Location, float Radius, int32& OutIndex, FTransform& OutTransform) const
{
	OutIndex = INDEX_NONE;
	if (Radius <= 0.0f)
	{
		return false;
	}
	// Every region's HISM of the type (one at the world origin unless a planet splits them, WM-BP7); OutIndex is the
	// instance's index in its region's HISM.
	float BestDist = Radius;
	for (const auto& Pair : HISMComponents)
	{
		UHierarchicalInstancedStaticMeshComponent* HISM = Pair.Value.Get();
		if (Pair.Key.ScatterTypeID != ScatterTypeID || !HISM)
		{
			continue;
		}
		// The sphere overlap uses instance bounds; a tall tree whose base is a little outside the radius
		// still reports, so the base distance is re-checked below. Writes held for the HISM's next batch count as made
		// (world modes review G42): a held release hides its instance, a held placement or new instance is where it goes.
		const FTransform& ComponentTransform = HISM->GetComponentTransform();
		auto Consider = [&](int32 Index)
		{
			FTransform Local;
			if (!GetEffectiveInstanceTransform(Pair.Key, HISM, Index, Local) || Local.GetScale3D().IsNearlyZero())
			{
				return; // hidden (free-list) instance
			}
			const FTransform Xf = Local * ComponentTransform;
			const float Dist = FVector::Dist(Xf.GetLocation(), Location);
			if (Dist <= BestDist)
			{
				BestDist = Dist;
				OutIndex = Index;
				OutTransform = Xf;
			}
		};
		const TArray<int32> Candidates = HISM->GetInstancesOverlappingSphere(Location, BestDist, /*bSphereInWorldSpace=*/true);
		for (int32 Index : Candidates)
		{
			Consider(Index);
		}
		if (const FPendingHISMWrites* Writes = PendingHISMWrites.Find(Pair.Key))
		{
			for (const TPair<int32, FTransform>& Update : Writes->Updates)
			{
				Consider(Update.Key);
			}
			const int32 Applied = HISM->GetInstanceCount();
			for (int32 i = 0; i < Writes->Grows.Num(); ++i)
			{
				Consider(Applied + i);
			}
		}
	}
	return OutIndex != INDEX_NONE;
}

void UVoxelScatterRenderer::SetSurfaceScatterVisible(bool bVisible)
{
	if (!ScatterManager)
	{
		return;
	}

	for (auto& Pair : HISMComponents)
	{
		const int32 ScatterTypeID = Pair.Key.ScatterTypeID;
		UHierarchicalInstancedStaticMeshComponent* HISM = Pair.Value;
		if (!HISM)
		{
			continue;
		}

		const FScatterDefinition* Def = ScatterManager->GetScatterDefinition(ScatterTypeID);
		if (!Def)
		{
			continue;
		}

		// Hide scatter types that can't legitimately appear underground (no Underground
		// flag in the mask) — e.g. dry-land or underwater foliage seen through cave ceilings.
		// Underground-capable types stay visible so cave scatter doesn't blink when descending.
		const bool bCanAppearUnderground =
			(Def->SurfaceLocationMask & static_cast<int32>(EScatterSurfaceLocationFlags::Underground)) != 0;
		if (!bCanAppearUnderground)
		{
			HISM->SetVisibility(bVisible);
		}
	}
}

FString UVoxelScatterRenderer::GetDebugStats() const
{
	const int32 TotalInstances = GetTotalInstanceCount();
	const int32 ChunksWithInstances = ChunkScatterTypes.Num();

	// Calculate pool statistics
	int32 TotalPooled = 0;
	int32 TotalAllocated = 0;
	for (const auto& PoolPair : InstancePools)
	{
		const FHISMInstancePool& Pool = PoolPair.Value;
		TotalPooled += Pool.FreeIndices.Num();
		TotalAllocated += Pool.TotalAllocated;
	}
	const int32 ActiveInstances = TotalAllocated - TotalPooled;
	const float Utilization = TotalAllocated > 0 ? (static_cast<float>(ActiveInstances) / TotalAllocated * 100.0f) : 0.0f;

	return FString::Printf(TEXT("ScatterRenderer: %d HISM, %d instances (Active: %d, Pooled: %d, Util: %.0f%%), %d chunks, Pending: %d adds"),
		HISMComponents.Num(),
		TotalAllocated,
		ActiveInstances,
		TotalPooled,
		Utilization,
		ChunksWithInstances,
		PendingInstanceAdds.Num());
}

// ==================== Internal Methods ====================

UHierarchicalInstancedStaticMeshComponent* UVoxelScatterRenderer::CreateHISMComponent(const FScatterDefinition& Definition, const FIntVector& Region)
{
	if (!ContainerActor)
	{
		return nullptr;
	}

	UStaticMesh* Mesh = nullptr;

	if (Definition.MeshType == EScatterMeshType::CrossBillboard)
	{
		// Compute UV bounds: atlas tile or full [0,1]
		FVector2f UVMin(0.0f, 0.0f);
		FVector2f UVMax(1.0f, 1.0f);

		if (Definition.bUseBillboardAtlas && Definition.BillboardAtlasColumns > 0 && Definition.BillboardAtlasRows > 0)
		{
			const float TileU = 1.0f / static_cast<float>(Definition.BillboardAtlasColumns);
			const float TileV = 1.0f / static_cast<float>(Definition.BillboardAtlasRows);
			UVMin.X = Definition.BillboardAtlasColumn * TileU;
			UVMin.Y = Definition.BillboardAtlasRow * TileV;
			UVMax.X = UVMin.X + TileU;
			UVMax.Y = UVMin.Y + TileV;

			// Inset UVs by half a texel to prevent bilinear filtering from
			// sampling across atlas tile boundaries (causes visible border)
			constexpr float AtlasTexelInset = 0.001f; // ~1 texel at 1024x1024
			UVMin.X += AtlasTexelInset;
			UVMin.Y += AtlasTexelInset;
			UVMax.X -= AtlasTexelInset;
			UVMax.Y -= AtlasTexelInset;
		}

		// Generate cross-billboard mesh at runtime with atlas UVs baked in
		Mesh = FVoxelBillboardMeshGenerator::GetOrCreateBillboardMesh(
			Definition.BillboardWidth, Definition.BillboardHeight, UVMin, UVMax);
		if (!Mesh)
		{
			UE_LOG(LogVoxelScatterRenderer, Warning, TEXT("Failed to create billboard mesh for scatter type: %s"), *Definition.Name);
			return nullptr;
		}
	}
	else
	{
		// Load mesh synchronously (TSoftObjectPtr)
		Mesh = Definition.Mesh.LoadSynchronous();
		if (!Mesh)
		{
			UE_LOG(LogVoxelScatterRenderer, Warning, TEXT("Failed to load mesh for scatter type: %s"), *Definition.Name);
			return nullptr;
		}
	}

	// Create component name (one HISM per type and render region when a planet splits them, WM-BP7)
	FName ComponentName = Regions.IsSplit()
		? FName(*FString::Printf(TEXT("HISM_%s_%d_R%d_%d_%d"), *Definition.Name, Definition.ScatterID, Region.X, Region.Y, Region.Z))
		: FName(*FString::Printf(TEXT("HISM_%s_%d"), *Definition.Name, Definition.ScatterID));

	// Create HISM component
	UHierarchicalInstancedStaticMeshComponent* HISM = NewObject<UHierarchicalInstancedStaticMeshComponent>(
		ContainerActor,
		ComponentName,
		RF_Transient
	);

	if (!HISM)
	{
		UE_LOG(LogVoxelScatterRenderer, Error, TEXT("Failed to create HISM component for: %s"), *Definition.Name);
		return nullptr;
	}

	// Attach to container actor (at the world origin), located at the region's origin (WM-BP7): the world origin when the
	// world is not split, as before
	HISM->AttachToComponent(ContainerActor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
	HISM->SetRelativeLocation(Regions.GetRegionOrigin(Region));

	// Set mesh
	HISM->SetStaticMesh(Mesh);

	// Configure the component
	ConfigureHISMComponent(HISM, Definition);

	// Apply billboard-specific material override
	if (Definition.MeshType == EScatterMeshType::CrossBillboard)
	{
		UTexture2D* BillboardTex = nullptr;

		if (Definition.bUseBillboardAtlas)
		{
			// Atlas mode: use the shared atlas texture (UVs already baked into mesh)
			BillboardTex = Definition.BillboardAtlasTexture.LoadSynchronous();
		}
		else
		{
			// Standalone texture mode
			BillboardTex = Definition.BillboardTexture.LoadSynchronous();
		}

		if (BillboardTex)
		{
			UMaterialInstanceDynamic* BillboardMat = FVoxelBillboardMeshGenerator::CreateBillboardMaterial(
				BillboardTex, HISM);
			if (BillboardMat)
			{
				HISM->SetMaterial(0, BillboardMat);
			}
		}
		else
		{
			UE_LOG(LogVoxelScatterRenderer, Warning, TEXT("No billboard texture assigned for scatter type: %s (ID %d). "
				"Set BillboardTexture or BillboardAtlasTexture for proper rendering."),
				*Definition.Name, Definition.ScatterID);

			// Apply a basic two-sided material so geometry is at least visible for debugging
			UMaterialInstanceDynamic* FallbackMat = FVoxelBillboardMeshGenerator::CreateBillboardMaterial(nullptr, HISM);
			if (FallbackMat)
			{
				HISM->SetMaterial(0, FallbackMat);
			}
		}
	}

	// Register with world
	HISM->RegisterComponent();

	UE_LOG(LogVoxelScatterRenderer, Log, TEXT("Created HISM component for: %s (ID: %d, MeshType: %d)"),
		*Definition.Name, Definition.ScatterID, static_cast<int32>(Definition.MeshType));

	return HISM;
}

void UVoxelScatterRenderer::ConfigureHISMComponent(UHierarchicalInstancedStaticMeshComponent* HISM, const FScatterDefinition& Definition)
{
	if (!HISM)
	{
		return;
	}

	// ==================== LOD & Culling ====================

	// Set cull distances (start fade, end cull)
	// LODStartDistance = where instances start fading/simplifying
	// CullDistance = where instances are completely culled
	HISM->SetCullDistances(Definition.LODStartDistance, Definition.CullDistance);

	// ==================== Shadows ====================

	HISM->SetCastShadow(Definition.bCastShadows);

	// Note: Do NOT set CachedMaxDrawDistance here — it culls the entire HISM component
	// (all instances) when the camera exceeds that distance from the component bounds.
	// Per-instance culling is already handled by SetCullDistances above.

	// ==================== Collision ====================

	if (Definition.bEnableCollision)
	{
		HISM->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		HISM->SetCollisionResponseToAllChannels(ECR_Block);
	}
	else
	{
		HISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	// ==================== Visual Settings ====================

	HISM->bReceivesDecals = Definition.bReceivesDecals;

	// Override materials if specified
	for (int32 i = 0; i < Definition.OverrideMaterials.Num(); ++i)
	{
		if (!Definition.OverrideMaterials[i].IsNull())
		{
			UMaterialInterface* Material = Definition.OverrideMaterials[i].LoadSynchronous();
			if (Material)
			{
				HISM->SetMaterial(i, Material);
			}
		}
	}

	// ==================== Performance Settings ====================

	HISM->SetMobility(EComponentMobility::Static);
	HISM->bDisableCollision = !Definition.bEnableCollision;
	HISM->bUseDefaultCollision = false;

	// HISM-specific performance settings
	HISM->bEnableDensityScaling = false;
	HISM->SetCanEverAffectNavigation(false);

	// Screen size culling - cull instances that are very small on screen
	if (Definition.MinScreenSize > 0.0f)
	{
		HISM->MinLOD = 0;
		// Note: Screen size thresholds are typically set per-LOD in the static mesh asset
		// The MinScreenSize here affects the overall component visibility
	}

	// Use the mesh's built-in LODs if available
	// The static mesh should have LOD0, LOD1, etc. defined with screen size thresholds
	// HISM will automatically switch between them based on distance and screen size

	UE_LOG(LogVoxelScatterRenderer, Verbose, TEXT("Configured HISM for %s: LODStart=%.0f, Cull=%.0f"),
		*Definition.Name, Definition.LODStartDistance, Definition.CullDistance);
}

void UVoxelScatterRenderer::FlushPendingInstanceAdds()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelScatter_FlushInstanceAdds);
	if (PendingInstanceAdds.Num() == 0)
	{
		return;
	}

	int32 InstanceBudget = MaxInstanceAddsPerFrame;
	int32 EntriesProcessed = 0;
	const bool bBatching = IsBatchingHISMWrites();
	if (!bBatching)
	{
		// Writes held while batching was on land first, so recycled indices are all in their HISMs
		ApplyPendingHISMWrites(/*bAll=*/true);
	}

	// Track which HISMs need render state marked dirty after batch updates
	TSet<UHierarchicalInstancedStaticMeshComponent*> DirtyHISMs;

	for (int32 i = 0; i < PendingInstanceAdds.Num() && InstanceBudget > 0; ++i)
	{
		FPendingInstanceAdd& PendingAdd = PendingInstanceAdds[i];

		// Skip entries for chunks that were unloaded while pending
		if (!ChunkScatterTypes.Contains(PendingAdd.ChunkCoord))
		{
			++EntriesProcessed;
			continue;
		}

		// The chunk's region's HISM of the type (WM-BP7)
		const FIntVector Region = Regions.GetRegion(PendingAdd.ChunkCoord);
		UHierarchicalInstancedStaticMeshComponent* HISM = GetOrCreateHISM(PendingAdd.ScatterTypeID, Region);
		if (!HISM)
		{
			// Can't create HISM for this type — discard
			++EntriesProcessed;
			continue;
		}

		FHISMInstancePool& Pool = InstancePools.FindOrAdd(FVoxelScatterHISMKey{ PendingAdd.ScatterTypeID, Region });
		TArray<int32>& ChunkIndices = Pool.ChunkInstanceIndices.FindOrAdd(PendingAdd.ChunkCoord);

		const int32 TotalNeeded = PendingAdd.Transforms.Num();
		const int32 ToProcess = FMath::Min(TotalNeeded, InstanceBudget);

		// Determine how many can be recycled from free list
		const int32 ToRecycle = FMath::Min(ToProcess, Pool.FreeIndices.Num());
		const int32 ToGrow = ToProcess - ToRecycle;

		if (bBatching)
		{
			// Held for the HISM's next batch (world modes review G42), local to the HISM as AddInstances and
			// UpdateInstanceTransform make world transforms. The indices are what the HISM gives them when the batch lands.
			FPendingHISMWrites& Writes = PendingHISMWrites.FindOrAdd(FVoxelScatterHISMKey{ PendingAdd.ScatterTypeID, Region });
			const FTransform& ComponentTransform = HISM->GetComponentTransform();
			const int32 Applied = HISM->GetInstanceCount();
			for (int32 j = 0; j < ToRecycle; ++j)
			{
				const int32 RecycledIndex = Pool.FreeIndices.Pop(EAllowShrinking::No);
				const FTransform Local = PendingAdd.Transforms[j].GetRelativeTransform(ComponentTransform);
				if (RecycledIndex >= Applied)
				{
					Writes.Grows[RecycledIndex - Applied] = Local;
				}
				else
				{
					Writes.Updates.Add(RecycledIndex, Local);
				}
				ChunkIndices.Add(RecycledIndex);
			}
			const int32 FirstNewIndex = Applied + Writes.Grows.Num();
			for (int32 j = 0; j < ToGrow; ++j)
			{
				Writes.Grows.Add(PendingAdd.Transforms[ToRecycle + j].GetRelativeTransform(ComponentTransform));
				ChunkIndices.Add(FirstNewIndex + j);
			}
			Pool.TotalAllocated += ToGrow;
		}
		else
		{
			// Recycle from free list: update transforms of zero-scaled instances
			for (int32 j = 0; j < ToRecycle; ++j)
			{
				const int32 RecycledIndex = Pool.FreeIndices.Pop(EAllowShrinking::No);
				HISM->UpdateInstanceTransform(RecycledIndex, PendingAdd.Transforms[j],
					/*bWorldSpace=*/true, /*bMarkRenderStateDirty=*/false, /*bTeleport=*/true);
				ChunkIndices.Add(RecycledIndex);
			}

			// Grow pool: add new instances for remainder
			if (ToGrow > 0)
			{
				TArrayView<FTransform> GrowTransforms = MakeArrayView(
					PendingAdd.Transforms.GetData() + ToRecycle, ToGrow);
				TArray<FTransform> GrowBatch(GrowTransforms.GetData(), GrowTransforms.Num());

				const int32 FirstNewIndex = HISM->GetInstanceCount();
				HISM->AddInstances(GrowBatch, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);

				for (int32 j = 0; j < ToGrow; ++j)
				{
					ChunkIndices.Add(FirstNewIndex + j);
				}
				Pool.TotalAllocated += ToGrow;
			}
			DirtyHISMs.Add(HISM);
		}

		TotalInstancesAdded += ToProcess;
		InstanceBudget -= ToProcess;

		if (ToProcess >= TotalNeeded)
		{
			// Entire batch processed
			++EntriesProcessed;
		}
		else
		{
			// Partial batch: remove processed transforms, keep rest for next frame
			PendingAdd.Transforms.RemoveAt(0, ToProcess);
			// Don't increment EntriesProcessed — this entry still has remaining transforms
		}
	}

	// Batch mark dirty after all updates
	for (UHierarchicalInstancedStaticMeshComponent* HISM : DirtyHISMs)
	{
		if (HISM)
		{
			HISM->MarkRenderStateDirty();
		}
	}

	// Remove fully processed entries from the front
	if (EntriesProcessed > 0)
	{
		PendingInstanceAdds.RemoveAt(0, EntriesProcessed);
	}

	if (PendingInstanceAdds.Num() > 0)
	{
		UE_LOG(LogVoxelScatterRenderer, Verbose, TEXT("Deferred instance adds: %d entries remaining (%d budget used)"),
			PendingInstanceAdds.Num(), MaxInstanceAddsPerFrame - InstanceBudget);
	}
}
