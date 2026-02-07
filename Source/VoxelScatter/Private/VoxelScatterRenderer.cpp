// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelScatterRenderer.h"
#include "VoxelScatterManager.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
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
		ContainerActor->SetActorLabel(TEXT("VoxelScatterContainer"));

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

void UVoxelScatterRenderer::Tick()
{
	if (!bIsInitialized)
	{
		return;
	}

	// Process any pending rebuilds accumulated from chunk updates
	FlushPendingRebuilds();
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

	// Collect scatter types that need rebuilding
	TSet<int32> ScatterTypesToRebuild;

	// Get existing scatter types for this chunk (before update)
	if (TSet<int32>* ExistingTypes = ChunkScatterTypes.Find(ChunkCoord))
	{
		ScatterTypesToRebuild.Append(*ExistingTypes);
	}

	// Update tracking for this chunk
	ChunkScatterTypes.Remove(ChunkCoord);

	if (ScatterData.bIsValid && ScatterData.SpawnPoints.Num() > 0)
	{
		// Collect new scatter types from this chunk
		TSet<int32>& NewTypes = ChunkScatterTypes.FindOrAdd(ChunkCoord);
		for (const FScatterSpawnPoint& Point : ScatterData.SpawnPoints)
		{
			NewTypes.Add(Point.ScatterTypeID);
			ScatterTypesToRebuild.Add(Point.ScatterTypeID);
		}
	}

	// Queue all affected scatter types for deferred rebuild
	for (int32 ScatterTypeID : ScatterTypesToRebuild)
	{
		QueueRebuild(ScatterTypeID);
	}
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
		return;
	}

	// Collect scatter types to rebuild
	TSet<int32> ScatterTypesToRebuild = *ScatterTypes;

	// Remove tracking for this chunk BEFORE rebuilding
	ChunkScatterTypes.Remove(ChunkCoord);

	// Queue all affected scatter types for deferred rebuild (they will now exclude this chunk)
	for (int32 ScatterTypeID : ScatterTypesToRebuild)
	{
		QueueRebuild(ScatterTypeID);
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

	// Clear tracking
	ChunkScatterTypes.Empty();
}

// ==================== HISM Management ====================

UHierarchicalInstancedStaticMeshComponent* UVoxelScatterRenderer::GetOrCreateHISM(int32 ScatterTypeID)
{
	if (!bIsInitialized || !ScatterManager || !ContainerActor)
	{
		return nullptr;
	}

	// Check if already exists
	if (TObjectPtr<UHierarchicalInstancedStaticMeshComponent>* ExistingPtr = HISMComponents.Find(ScatterTypeID))
	{
		return ExistingPtr->Get();
	}

	// Get scatter definition
	const FScatterDefinition* Definition = ScatterManager->GetScatterDefinition(ScatterTypeID);
	if (!Definition || Definition->Mesh.IsNull())
	{
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

FString UVoxelScatterRenderer::GetDebugStats() const
{
	const int32 TotalInstances = GetTotalInstanceCount();
	const int32 ChunksWithInstances = ChunkScatterTypes.Num();

	return FString::Printf(TEXT("ScatterRenderer: %d HISM, %d instances, %d chunks, Pending: %d, Added: %lld, Removed: %lld"),
		HISMComponents.Num(),
		TotalInstances,
		ChunksWithInstances,
		PendingRebuildScatterTypes.Num(),
		TotalInstancesAdded,
		TotalInstancesRemoved);
}

// ==================== Internal Methods ====================

UHierarchicalInstancedStaticMeshComponent* UVoxelScatterRenderer::CreateHISMComponent(const FScatterDefinition& Definition)
{
	if (!ContainerActor)
	{
		return nullptr;
	}

	// Load mesh synchronously (TSoftObjectPtr)
	UStaticMesh* Mesh = Definition.Mesh.LoadSynchronous();
	if (!Mesh)
	{
		UE_LOG(LogVoxelScatterRenderer, Warning, TEXT("Failed to load mesh for scatter type: %s"), *Definition.Name);
		return nullptr;
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

	// Register with world
	HISM->RegisterComponent();

	UE_LOG(LogVoxelScatterRenderer, Log, TEXT("Created HISM component for: %s (ID: %d)"), *Definition.Name, Definition.ScatterID);

	return HISM;
}

void UVoxelScatterRenderer::ConfigureHISMComponent(UHierarchicalInstancedStaticMeshComponent* HISM, const FScatterDefinition& Definition)
{
	if (!HISM)
	{
		return;
	}

	// Culling distances
	HISM->SetCullDistances(0, Definition.CullDistance);

	// Shadows
	HISM->SetCastShadow(Definition.bCastShadows);

	// Collision
	if (Definition.bEnableCollision)
	{
		HISM->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		HISM->SetCollisionResponseToAllChannels(ECR_Block);
	}
	else
	{
		HISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	// Decals
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

	// Performance settings
	HISM->SetMobility(EComponentMobility::Static);
	HISM->bDisableCollision = !Definition.bEnableCollision;
	HISM->bUseDefaultCollision = false;

	// HISM-specific settings for better performance
	HISM->bEnableDensityScaling = false;
	HISM->SetCanEverAffectNavigation(false);
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

	// Process pending rebuilds
	int32 Processed = 0;
	TArray<int32> ToRemove;
	ToRemove.Reserve(NumToProcess);

	for (int32 ScatterTypeID : PendingRebuildScatterTypes)
	{
		if (Processed >= NumToProcess)
		{
			break;
		}

		RebuildScatterType(ScatterTypeID);
		ToRemove.Add(ScatterTypeID);
		++Processed;
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
	if (!Definition || Definition->Mesh.IsNull())
	{
		return;
	}

	// Get or create HISM for this type
	UHierarchicalInstancedStaticMeshComponent* HISM = GetOrCreateHISM(ScatterTypeID);
	if (!HISM)
	{
		return;
	}

	// Track old instance count for statistics
	const int32 OldInstanceCount = HISM->GetInstanceCount();

	// Clear all existing instances
	HISM->ClearInstances();
	TotalInstancesRemoved += OldInstanceCount;

	// Collect all spawn points for this scatter type from all chunks
	TArray<FTransform> AllTransforms;
	AllTransforms.Reserve(1024); // Pre-allocate reasonable amount

	for (const auto& ChunkPair : ChunkScatterTypes)
	{
		const FIntVector& ChunkCoord = ChunkPair.Key;
		const TSet<int32>& ScatterTypes = ChunkPair.Value;

		// Skip chunks that don't have this scatter type
		if (!ScatterTypes.Contains(ScatterTypeID))
		{
			continue;
		}

		// Get scatter data from manager
		const FChunkScatterData* ScatterData = ScatterManager->GetChunkScatterData(ChunkCoord);
		if (!ScatterData || !ScatterData->bIsValid)
		{
			continue;
		}

		// Collect spawn points for this scatter type
		for (const FScatterSpawnPoint& Point : ScatterData->SpawnPoints)
		{
			if (Point.ScatterTypeID == ScatterTypeID)
			{
				FTransform Transform = Point.GetTransform(Definition->bAlignToSurfaceNormal, Definition->SurfaceOffset);
				AllTransforms.Add(Transform);
			}
		}
	}

	// Add all instances in batch using batch API for better performance
	if (AllTransforms.Num() > 0)
	{
		// Use batch AddInstances - more efficient than individual calls
		// Parameters: InstanceTransforms, bShouldReturnIndices=false, bWorldSpace=true
		HISM->AddInstances(AllTransforms, false, true);
		TotalInstancesAdded += AllTransforms.Num();

		// Mark render state dirty once after all additions
		HISM->MarkRenderStateDirty();
	}

	UE_LOG(LogVoxelScatterRenderer, Verbose, TEXT("Rebuilt scatter type %d (%s): %d -> %d instances"),
		ScatterTypeID, *Definition->Name, OldInstanceCount, AllTransforms.Num());
}
