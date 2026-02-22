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

DEFINE_LOG_CATEGORY_STATIC(LogVoxelScatterRenderer, Log, All);

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

	// Create container actor for HISM components
	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = TEXT("VoxelScatterContainer");
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

	// Clear any pending rebuilds from previous session
	PendingRebuildScatterTypes.Empty();

	bIsInitialized = true;
	UE_LOG(LogVoxelScatterRenderer, Log, TEXT("ScatterRenderer initialized"));
}

void UVoxelScatterRenderer::Tick(const FVector& ViewerPosition, float DeltaTime)
{
	if (!bIsInitialized || !ScatterManager)
	{
		return;
	}

	// Check if viewer has moved significantly
	const float ViewerMovement = FVector::Dist(ViewerPosition, LastViewerPosition);
	if (ViewerMovement > ViewerMovementThreshold)
	{
		// Viewer is moving - reset stationary timer
		TimeSinceViewerMoved = 0.0f;
		LastViewerPosition = ViewerPosition;
	}
	else
	{
		// Viewer is stationary - accumulate time
		TimeSinceViewerMoved += DeltaTime;
	}

	// Always flush deferred instance additions (budget-limited)
	FlushPendingInstanceAdds();

	// Only process full rebuilds when:
	// 1. Viewer has been stationary for a bit (prevents flicker during movement)
	// 2. No chunks are pending generation (prevents flicker during initial load)
	// Rebuilds are triggered by: chunk unload, edits/regeneration, and distance
	// cleanup (removing out-of-range scatter types). Distance streaming additions
	// use AddSupplementalInstances() → FlushPendingInstanceAdds() (no rebuild).
	const bool bViewerStationary = TimeSinceViewerMoved >= RebuildStationaryDelay;
	const bool bWorldStable = ScatterManager->GetPendingGenerationCount() == 0;

	if (bViewerStationary && bWorldStable)
	{
		FlushPendingRebuilds();
	}
}

void UVoxelScatterRenderer::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	// Clear pending rebuilds
	PendingRebuildScatterTypes.Empty();

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
		// EXISTING CHUNK UPDATE: Need to rebuild affected scatter types
		// This happens when chunk scatter is regenerated (e.g., after edit)

		TSet<int32> ScatterTypesToRebuild;

		// Mark existing types for rebuild (they may have changed)
		if (ExistingTypes)
		{
			ScatterTypesToRebuild.Append(*ExistingTypes);
		}

		// Update tracking for this chunk
		ChunkScatterTypes.Remove(ChunkCoord);

		if (ScatterData.bIsValid && ScatterData.SpawnPoints.Num() > 0)
		{
			// Track new scatter types
			TSet<int32>& NewTypes = ChunkScatterTypes.FindOrAdd(ChunkCoord);
			for (const FScatterSpawnPoint& Point : ScatterData.SpawnPoints)
			{
				NewTypes.Add(Point.ScatterTypeID);
				ScatterTypesToRebuild.Add(Point.ScatterTypeID);
			}
		}

		// Queue affected scatter types for deferred rebuild
		for (int32 ScatterTypeID : ScatterTypesToRebuild)
		{
			QueueRebuild(ScatterTypeID);
		}
	}
}

void UVoxelScatterRenderer::AddSupplementalInstances(const FIntVector& ChunkCoord, const FChunkScatterData& NewScatterData)
{
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
	if (!bIsInitialized)
	{
		return;
	}

	FHISMInstancePool* Pool = InstancePools.Find(ScatterTypeID);
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
	if (TObjectPtr<UHierarchicalInstancedStaticMeshComponent>* Found = HISMComponents.Find(ScatterTypeID))
	{
		HISM = Found->Get();
	}

	if (HISM)
	{
		const FTransform ZeroTransform(FQuat::Identity, FVector::ZeroVector, FVector::ZeroVector);
		for (int32 Index : *Indices)
		{
			HISM->UpdateInstanceTransform(Index, ZeroTransform,
				/*bWorldSpace=*/true, /*bMarkRenderStateDirty=*/false, /*bTeleport=*/true);
		}
		HISM->MarkRenderStateDirty();
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

void UVoxelScatterRenderer::ReleaseAllForScatterType(int32 ScatterTypeID)
{
	if (!bIsInitialized)
	{
		return;
	}

	FHISMInstancePool* Pool = InstancePools.Find(ScatterTypeID);
	if (!Pool)
	{
		return;
	}

	UHierarchicalInstancedStaticMeshComponent* HISM = nullptr;
	if (TObjectPtr<UHierarchicalInstancedStaticMeshComponent>* Found = HISMComponents.Find(ScatterTypeID))
	{
		HISM = Found->Get();
	}

	const FTransform ZeroTransform(FQuat::Identity, FVector::ZeroVector, FVector::ZeroVector);

	for (auto& ChunkPair : Pool->ChunkInstanceIndices)
	{
		const FIntVector& ChunkCoord = ChunkPair.Key;
		TArray<int32>& Indices = ChunkPair.Value;

		if (HISM)
		{
			for (int32 Index : Indices)
			{
				HISM->UpdateInstanceTransform(Index, ZeroTransform,
					/*bWorldSpace=*/true, /*bMarkRenderStateDirty=*/false, /*bTeleport=*/true);
			}
		}

		Pool->FreeIndices.Append(Indices);
		TotalInstancesRemoved += Indices.Num();

		// Remove this type from chunk tracking
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

	Pool->ChunkInstanceIndices.Empty();

	if (HISM)
	{
		HISM->MarkRenderStateDirty();
	}
}

void UVoxelScatterRenderer::ClearAllInstances()
{
	if (!bIsInitialized)
	{
		return;
	}

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

UHierarchicalInstancedStaticMeshComponent* UVoxelScatterRenderer::GetOrCreateHISM(int32 ScatterTypeID)
{
	if (!bIsInitialized || !ScatterManager || !ContainerActor)
	{
		return nullptr;
	}

	// Check if already exists
	if (TObjectPtr<UHierarchicalInstancedStaticMeshComponent>* ExistingHISM = HISMComponents.Find(ScatterTypeID))
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
	UHierarchicalInstancedStaticMeshComponent* HISM = CreateHISMComponent(*Definition);
	if (HISM)
	{
		HISMComponents.Add(ScatterTypeID, HISM);
	}

	return HISM;
}

void UVoxelScatterRenderer::RefreshAllComponents()
{
	if (!bIsInitialized || !ScatterManager)
	{
		return;
	}

	// Update configuration for all existing HISM components
	for (auto& Pair : HISMComponents)
	{
		const int32 ScatterTypeID = Pair.Key;
		UHierarchicalInstancedStaticMeshComponent* HISM = Pair.Value;

		if (!HISM)
		{
			continue;
		}

		const FScatterDefinition* Definition = ScatterManager->GetScatterDefinition(ScatterTypeID);
		if (Definition)
		{
			ConfigureHISMComponent(HISM, *Definition);
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
			Total += Pair.Value->GetInstanceCount();
		}
	}
	return Total;
}

// ==================== Debug ====================

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

	Total += PendingRebuildScatterTypes.GetAllocatedSize();

	return Total;
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

	return FString::Printf(TEXT("ScatterRenderer: %d HISM, %d instances (Active: %d, Pooled: %d, Util: %.0f%%), %d chunks, Pending: %d rebuilds/%d adds"),
		HISMComponents.Num(),
		TotalAllocated,
		ActiveInstances,
		TotalPooled,
		Utilization,
		ChunksWithInstances,
		PendingRebuildScatterTypes.Num(),
		PendingInstanceAdds.Num());
}

// ==================== Internal Methods ====================

UHierarchicalInstancedStaticMeshComponent* UVoxelScatterRenderer::CreateHISMComponent(const FScatterDefinition& Definition)
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

	// Create component name
	FName ComponentName = *FString::Printf(TEXT("HISM_%s_%d"), *Definition.Name, Definition.ScatterID);

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

	// Attach to container actor
	HISM->AttachToComponent(ContainerActor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);

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

void UVoxelScatterRenderer::AddInstancesToHISM(
	UHierarchicalInstancedStaticMeshComponent* HISM,
	const TArray<FScatterSpawnPoint>& SpawnPoints,
	const FScatterDefinition& Definition,
	TArray<int32>& OutInstanceIndices)
{
	if (!HISM || SpawnPoints.Num() == 0)
	{
		return;
	}

	// Reserve space for indices
	OutInstanceIndices.Reserve(OutInstanceIndices.Num() + SpawnPoints.Num());

	// Build transforms array for batch add
	TArray<FTransform> Transforms;
	Transforms.Reserve(SpawnPoints.Num());

	for (const FScatterSpawnPoint& Point : SpawnPoints)
	{
		// Get transform from spawn point (includes scale and rotation)
		FTransform Transform = Point.GetTransform(Definition.bAlignToSurfaceNormal, Definition.SurfaceOffset);
		Transforms.Add(Transform);
	}

	// Add instances in batch
	// Note: AddInstances returns the index of the first added instance
	const int32 FirstIndex = HISM->GetInstanceCount();

	// Use batch add if available (UE 5.x)
	for (int32 i = 0; i < Transforms.Num(); ++i)
	{
		const int32 NewIndex = HISM->AddInstance(Transforms[i], true); // bWorldSpace = true
		OutInstanceIndices.Add(NewIndex);
		TotalInstancesAdded++;
	}

	// Mark render state dirty for the component
	HISM->MarkRenderStateDirty();
}

void UVoxelScatterRenderer::FlushPendingInstanceAdds()
{
	if (PendingInstanceAdds.Num() == 0)
	{
		return;
	}

	int32 InstanceBudget = MaxInstanceAddsPerFrame;
	int32 EntriesProcessed = 0;

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

		UHierarchicalInstancedStaticMeshComponent* HISM = GetOrCreateHISM(PendingAdd.ScatterTypeID);
		if (!HISM)
		{
			// Can't create HISM for this type — discard
			++EntriesProcessed;
			continue;
		}

		FHISMInstancePool& Pool = InstancePools.FindOrAdd(PendingAdd.ScatterTypeID);
		TArray<int32>& ChunkIndices = Pool.ChunkInstanceIndices.FindOrAdd(PendingAdd.ChunkCoord);

		const int32 TotalNeeded = PendingAdd.Transforms.Num();
		const int32 ToProcess = FMath::Min(TotalNeeded, InstanceBudget);

		// Determine how many can be recycled from free list
		const int32 ToRecycle = FMath::Min(ToProcess, Pool.FreeIndices.Num());
		const int32 ToGrow = ToProcess - ToRecycle;

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

		TotalInstancesAdded += ToProcess;
		InstanceBudget -= ToProcess;
		DirtyHISMs.Add(HISM);

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

void UVoxelScatterRenderer::QueueRebuild(int32 ScatterTypeID)
{
	PendingRebuildScatterTypes.Add(ScatterTypeID);
}

void UVoxelScatterRenderer::FlushPendingRebuilds()
{
	if (PendingRebuildScatterTypes.Num() == 0)
	{
		return;
	}

	// Determine how many to process this frame
	int32 NumToProcess = PendingRebuildScatterTypes.Num();
	if (MaxRebuildsPerFrame > 0 && NumToProcess > MaxRebuildsPerFrame)
	{
		NumToProcess = MaxRebuildsPerFrame;
	}

	// Collect which types will be rebuilt
	int32 Processed = 0;
	TArray<int32> ToRemove;
	ToRemove.Reserve(NumToProcess);

	for (int32 ScatterTypeID : PendingRebuildScatterTypes)
	{
		if (Processed >= NumToProcess)
		{
			break;
		}
		ToRemove.Add(ScatterTypeID);
		++Processed;
	}

	// Clear any PendingInstanceAdds for types about to be rebuilt,
	// otherwise deferred adds would stack on top of the rebuild result.
	if (PendingInstanceAdds.Num() > 0 && ToRemove.Num() > 0)
	{
		TSet<int32> RebuildTypeSet(ToRemove);
		PendingInstanceAdds.RemoveAll([&RebuildTypeSet](const FPendingInstanceAdd& Add)
		{
			return RebuildTypeSet.Contains(Add.ScatterTypeID);
		});
	}

	// Execute rebuilds
	for (int32 ScatterTypeID : ToRemove)
	{
		RebuildScatterType(ScatterTypeID);
	}

	// Remove processed items
	for (int32 ID : ToRemove)
	{
		PendingRebuildScatterTypes.Remove(ID);
	}

	if (Processed > 0)
	{
		UE_LOG(LogVoxelScatterRenderer, Verbose, TEXT("Flushed %d pending rebuilds (%d remaining)"),
			Processed, PendingRebuildScatterTypes.Num());
	}
}

void UVoxelScatterRenderer::RebuildScatterType(int32 ScatterTypeID)
{
	if (!bIsInitialized || !ScatterManager)
	{
		return;
	}

	// Get scatter definition
	const FScatterDefinition* Definition = ScatterManager->GetScatterDefinition(ScatterTypeID);
	if (!Definition)
	{
		return;
	}

	// CrossBillboard types use runtime-generated meshes, so Mesh soft pointer is not set
	if (Definition->MeshType != EScatterMeshType::CrossBillboard && Definition->Mesh.IsNull())
	{
		UE_LOG(LogVoxelScatterRenderer, Warning,
			TEXT("RebuildScatterType %d (%s): Mesh is null — skipping rebuild"),
			ScatterTypeID, *Definition->Name);
		return;
	}

	UHierarchicalInstancedStaticMeshComponent* HISM = GetOrCreateHISM(ScatterTypeID);
	if (!HISM)
	{
		return;
	}

	// Release all existing instances for this type back to the pool
	ReleaseAllForScatterType(ScatterTypeID);

	// Collect all transforms from all chunks that have this scatter type in the cache
	// Note: ChunkScatterTypes was cleared by ReleaseAllForScatterType, so we scan
	// the manager's ScatterDataCache directly
	TMap<FIntVector, TArray<FTransform>> TransformsByChunk;

	for (const auto& CachePair : ScatterManager->GetScatterDataCache())
	{
		const FIntVector& ChunkCoord = CachePair.Key;
		const FChunkScatterData& ScatterData = CachePair.Value;

		if (!ScatterData.bIsValid)
		{
			continue;
		}

		TArray<FTransform> ChunkTransforms;
		for (const FScatterSpawnPoint& Point : ScatterData.SpawnPoints)
		{
			if (Point.ScatterTypeID == ScatterTypeID)
			{
				FTransform Transform = Point.GetTransform(Definition->bAlignToSurfaceNormal, Definition->SurfaceOffset);
				ChunkTransforms.Add(Transform);
			}
		}

		if (ChunkTransforms.Num() > 0)
		{
			TransformsByChunk.Add(ChunkCoord, MoveTemp(ChunkTransforms));
		}
	}

	// Queue as PendingInstanceAdd entries — FlushPendingInstanceAdds will recycle from free list
	for (auto& Pair : TransformsByChunk)
	{
		// Re-establish chunk tracking
		ChunkScatterTypes.FindOrAdd(Pair.Key).Add(ScatterTypeID);

		FPendingInstanceAdd PendingAdd;
		PendingAdd.ScatterTypeID = ScatterTypeID;
		PendingAdd.ChunkCoord = Pair.Key;
		PendingAdd.Transforms = MoveTemp(Pair.Value);
		PendingInstanceAdds.Add(MoveTemp(PendingAdd));
	}

	UE_LOG(LogVoxelScatterRenderer, Verbose, TEXT("Rebuilt scatter type %d (%s): released to pool, queued %d chunks for re-add"),
		ScatterTypeID, *Definition->Name, TransformsByChunk.Num());
}
