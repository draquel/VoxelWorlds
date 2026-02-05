// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelPMCRenderer.h"
#include "VoxelRendering.h"
#include "VoxelWorldConfiguration.h"
#include "ProceduralMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionConstant.h"

// ==================== AVoxelPMCContainerActor ====================

AVoxelPMCContainerActor::AVoxelPMCContainerActor()
{
	PrimaryActorTick.bCanEverTick = false;

	RootSceneComponent = CreateDefaultSubobject<USceneComponent>(TEXT("RootComponent"));
	RootComponent = RootSceneComponent;
}

// ==================== FVoxelPMCRenderer ====================

FVoxelPMCRenderer::FVoxelPMCRenderer()
{
}

FVoxelPMCRenderer::~FVoxelPMCRenderer()
{
	if (bIsInitialized)
	{
		Shutdown();
	}
}

// ==================== Lifecycle ====================

void FVoxelPMCRenderer::Initialize(UWorld* World, const UVoxelWorldConfiguration* WorldConfig)
{
	check(IsInGameThread());
	check(World);
	check(WorldConfig);

	if (bIsInitialized)
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("FVoxelPMCRenderer::Initialize called when already initialized"));
		return;
	}

	CachedWorld = World;
	CachedConfig = WorldConfig;
	bGenerateCollision = WorldConfig->bGenerateCollision;

	// Spawn container actor
	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = TEXT("VoxelPMCContainer");
	SpawnParams.ObjectFlags |= RF_Transient;

	ContainerActor = World->SpawnActor<AVoxelPMCContainerActor>(SpawnParams);
	if (!ContainerActor.IsValid())
	{
		UE_LOG(LogVoxelRendering, Error, TEXT("FVoxelPMCRenderer: Failed to spawn container actor"));
		return;
	}

#if WITH_EDITOR
	ContainerActor->SetActorLabel(TEXT("VoxelPMCContainer"));
#endif

	// Create default vertex color material if none specified
	if (!CurrentMaterial.IsValid())
	{
		CreateDefaultVertexColorMaterial();
	}

	bIsInitialized = true;
	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelPMCRenderer initialized"));
}

void FVoxelPMCRenderer::Shutdown()
{
	check(IsInGameThread());

	if (!bIsInitialized)
	{
		return;
	}

	// Clear all chunk data
	ClearAllChunks();

	// Clear the component pool
	ComponentPool.Empty();

	// Destroy container actor
	if (ContainerActor.IsValid())
	{
		ContainerActor->Destroy();
		ContainerActor.Reset();
	}

	CachedWorld.Reset();
	CachedConfig.Reset();
	CurrentMaterial.Reset();

	bIsInitialized = false;
	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelPMCRenderer shutdown"));
}

bool FVoxelPMCRenderer::IsInitialized() const
{
	return bIsInitialized && ContainerActor.IsValid();
}

// ==================== Mesh Updates ====================

void FVoxelPMCRenderer::UpdateChunkMesh(const FChunkRenderData& RenderData)
{
	check(IsInGameThread());

	// GPU path is not ideal for PMC - log warning
	UE_LOG(LogVoxelRendering, Warning,
		TEXT("FVoxelPMCRenderer::UpdateChunkMesh called with GPU render data. Use UpdateChunkMeshFromCPU for PMC renderer."));
}

void FVoxelPMCRenderer::UpdateChunkMeshFromCPU(
	const FIntVector& ChunkCoord,
	int32 LODLevel,
	const FChunkMeshData& MeshData)
{
	check(IsInGameThread());

	if (!IsInitialized())
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("FVoxelPMCRenderer::UpdateChunkMeshFromCPU called before initialization"));
		return;
	}

	if (!MeshData.IsValid())
	{
		// Empty mesh - remove if exists
		RemoveChunk(ChunkCoord);
		return;
	}

	// Convert mesh data to PMC format
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FLinearColor> Colors;
	TArray<FProcMeshTangent> Tangents;

	ConvertMeshDataToPMCFormat(MeshData, Vertices, Triangles, Normals, UVs, Colors, Tangents);

	// Acquire or reuse a component
	UProceduralMeshComponent* PMC = nullptr;
	FPMCChunkData* ExistingData = ChunkDataMap.Find(ChunkCoord);

	if (ExistingData && ExistingData->MeshComponent.IsValid())
	{
		PMC = ExistingData->MeshComponent.Get();
		// Update statistics (subtract old, add new later)
		TotalVertexCount -= ExistingData->VertexCount;
		TotalTriangleCount -= ExistingData->TriangleCount;
		TotalMemoryUsage -= ExistingData->MemoryUsage;
	}
	else
	{
		PMC = AcquireComponent(ChunkCoord);
		if (!PMC)
		{
			UE_LOG(LogVoxelRendering, Error, TEXT("FVoxelPMCRenderer: Failed to acquire component for chunk %s"),
				*ChunkCoord.ToString());
			return;
		}
	}

	// Create the mesh section
	PMC->CreateMeshSection_LinearColor(
		0,              // Section index (one section per chunk)
		Vertices,
		Triangles,
		Normals,
		UVs,
		Colors,
		Tangents,
		bGenerateCollision
	);

	// Apply material
	if (CurrentMaterial.IsValid())
	{
		PMC->SetMaterial(0, CurrentMaterial.Get());
	}

	// Update chunk data
	FPMCChunkData& ChunkData = ChunkDataMap.FindOrAdd(ChunkCoord);
	ChunkData.MeshComponent = PMC;
	ChunkData.LODLevel = LODLevel;
	ChunkData.bIsVisible = true;
	ChunkData.Bounds = CalculateChunkBounds(ChunkCoord);
	ChunkData.VertexCount = MeshData.GetVertexCount();
	ChunkData.TriangleCount = MeshData.GetTriangleCount();
	ChunkData.MemoryUsage = MeshData.GetMemoryUsage();

	// Update statistics
	TotalVertexCount += ChunkData.VertexCount;
	TotalTriangleCount += ChunkData.TriangleCount;
	TotalMemoryUsage += ChunkData.MemoryUsage;
}

void FVoxelPMCRenderer::RemoveChunk(const FIntVector& ChunkCoord)
{
	check(IsInGameThread());

	FPMCChunkData* ChunkData = ChunkDataMap.Find(ChunkCoord);
	if (!ChunkData)
	{
		return;
	}

	// Update statistics
	TotalVertexCount -= ChunkData->VertexCount;
	TotalTriangleCount -= ChunkData->TriangleCount;
	TotalMemoryUsage -= ChunkData->MemoryUsage;

	// Release component to pool
	if (ChunkData->MeshComponent.IsValid())
	{
		ReleaseComponent(ChunkData->MeshComponent.Get());
	}

	ChunkDataMap.Remove(ChunkCoord);
}

void FVoxelPMCRenderer::ClearAllChunks()
{
	check(IsInGameThread());

	for (auto& Pair : ChunkDataMap)
	{
		if (Pair.Value.MeshComponent.IsValid())
		{
			ReleaseComponent(Pair.Value.MeshComponent.Get());
		}
	}

	ChunkDataMap.Empty();
	TotalVertexCount = 0;
	TotalTriangleCount = 0;
	TotalMemoryUsage = 0;
}

// ==================== Visibility ====================

void FVoxelPMCRenderer::SetChunkVisible(const FIntVector& ChunkCoord, bool bVisible)
{
	check(IsInGameThread());

	FPMCChunkData* ChunkData = ChunkDataMap.Find(ChunkCoord);
	if (ChunkData && ChunkData->MeshComponent.IsValid())
	{
		ChunkData->MeshComponent->SetVisibility(bVisible);
		ChunkData->bIsVisible = bVisible;
	}
}

void FVoxelPMCRenderer::SetAllChunksVisible(bool bVisible)
{
	check(IsInGameThread());

	for (auto& Pair : ChunkDataMap)
	{
		if (Pair.Value.MeshComponent.IsValid())
		{
			Pair.Value.MeshComponent->SetVisibility(bVisible);
			Pair.Value.bIsVisible = bVisible;
		}
	}
}

// ==================== Material Management ====================

void FVoxelPMCRenderer::SetMaterial(UMaterialInterface* Material)
{
	check(IsInGameThread());

	CurrentMaterial = Material;

	// Apply to all existing chunks
	for (auto& Pair : ChunkDataMap)
	{
		if (Pair.Value.MeshComponent.IsValid())
		{
			Pair.Value.MeshComponent->SetMaterial(0, Material);
		}
	}
}

UMaterialInterface* FVoxelPMCRenderer::GetMaterial() const
{
	return CurrentMaterial.Get();
}

void FVoxelPMCRenderer::UpdateMaterialParameters()
{
	// No-op for PMC - material parameters update automatically through UMaterialInstanceDynamic
}

// ==================== LOD Transitions ====================

void FVoxelPMCRenderer::UpdateLODTransition(const FIntVector& ChunkCoord, float MorphFactor)
{
	// No-op - PMC cannot do GPU morph-based LOD transitions
	// Smooth transitions require the custom vertex factory renderer
}

// ==================== Queries ====================

bool FVoxelPMCRenderer::IsChunkLoaded(const FIntVector& ChunkCoord) const
{
	return ChunkDataMap.Contains(ChunkCoord);
}

int32 FVoxelPMCRenderer::GetLoadedChunkCount() const
{
	return ChunkDataMap.Num();
}

void FVoxelPMCRenderer::GetLoadedChunks(TArray<FIntVector>& OutChunks) const
{
	OutChunks.Reset(ChunkDataMap.Num());
	for (const auto& Pair : ChunkDataMap)
	{
		OutChunks.Add(Pair.Key);
	}
}

int64 FVoxelPMCRenderer::GetGPUMemoryUsage() const
{
	// PMC uploads to GPU, so memory usage is roughly the same
	return static_cast<int64>(TotalMemoryUsage);
}

int64 FVoxelPMCRenderer::GetTotalVertexCount() const
{
	return TotalVertexCount;
}

int64 FVoxelPMCRenderer::GetTotalTriangleCount() const
{
	return TotalTriangleCount;
}

// ==================== Bounds ====================

bool FVoxelPMCRenderer::GetChunkBounds(const FIntVector& ChunkCoord, FBox& OutBounds) const
{
	const FPMCChunkData* ChunkData = ChunkDataMap.Find(ChunkCoord);
	if (ChunkData)
	{
		OutBounds = ChunkData->Bounds;
		return true;
	}
	return false;
}

FBox FVoxelPMCRenderer::GetTotalBounds() const
{
	FBox TotalBounds(ForceInit);

	for (const auto& Pair : ChunkDataMap)
	{
		if (Pair.Value.Bounds.IsValid)
		{
			TotalBounds += Pair.Value.Bounds;
		}
	}

	return TotalBounds;
}

// ==================== Debugging ====================

FString FVoxelPMCRenderer::GetDebugStats() const
{
	return FString::Printf(
		TEXT("PMC Renderer Stats:\n")
		TEXT("  Chunks: %d\n")
		TEXT("  Vertices: %lld\n")
		TEXT("  Triangles: %lld\n")
		TEXT("  Memory: %.2f MB\n")
		TEXT("  Pool Size: %d\n")
		TEXT("  Collision: %s"),
		ChunkDataMap.Num(),
		TotalVertexCount,
		TotalTriangleCount,
		TotalMemoryUsage / (1024.0 * 1024.0),
		ComponentPool.Num(),
		bGenerateCollision ? TEXT("Enabled") : TEXT("Disabled")
	);
}

FString FVoxelPMCRenderer::GetRendererTypeName() const
{
	return TEXT("PMC");
}

// ==================== Component Pool Management ====================

UProceduralMeshComponent* FVoxelPMCRenderer::AcquireComponent(const FIntVector& ChunkCoord)
{
	UProceduralMeshComponent* PMC = nullptr;

	// Try to get from pool first
	while (ComponentPool.Num() > 0)
	{
		TWeakObjectPtr<UProceduralMeshComponent> WeakPMC = ComponentPool.Pop(EAllowShrinking::No);
		if (WeakPMC.IsValid())
		{
			PMC = WeakPMC.Get();
			PMC->SetVisibility(true);
			break;
		}
	}

	// Create new component if pool was empty
	if (!PMC)
	{
		PMC = CreateNewComponent();
	}

	// Position the PMC at the chunk's world location (includes WorldOrigin offset)
	if (PMC && CachedConfig.IsValid())
	{
		const float ChunkWorldSize = CachedConfig->GetChunkWorldSize();
		const FVector ChunkWorldPos = CachedConfig->WorldOrigin + FVector(ChunkCoord) * ChunkWorldSize;
		PMC->SetWorldLocation(ChunkWorldPos);
	}

	return PMC;
}

void FVoxelPMCRenderer::ReleaseComponent(UProceduralMeshComponent* PMC)
{
	if (!PMC)
	{
		return;
	}

	// Clear mesh data
	PMC->ClearAllMeshSections();
	PMC->SetVisibility(false);

	// Add to pool for reuse
	ComponentPool.Add(PMC);
}

UProceduralMeshComponent* FVoxelPMCRenderer::CreateNewComponent()
{
	if (!ContainerActor.IsValid())
	{
		return nullptr;
	}

	UProceduralMeshComponent* PMC = NewObject<UProceduralMeshComponent>(
		ContainerActor.Get(),
		NAME_None,
		RF_Transient
	);

	if (PMC)
	{
		PMC->SetupAttachment(ContainerActor->GetRootComponent());
		PMC->RegisterComponent();

		// Set default rendering properties
		PMC->bUseAsyncCooking = true;
		PMC->SetCastShadow(true);
	}

	return PMC;
}

// ==================== Data Conversion ====================

void FVoxelPMCRenderer::ConvertMeshDataToPMCFormat(
	const FChunkMeshData& MeshData,
	TArray<FVector>& OutVertices,
	TArray<int32>& OutTriangles,
	TArray<FVector>& OutNormals,
	TArray<FVector2D>& OutUVs,
	TArray<FLinearColor>& OutColors,
	TArray<FProcMeshTangent>& OutTangents)
{
	const int32 VertexCount = MeshData.Positions.Num();
	const int32 IndexCount = MeshData.Indices.Num();

	// Pre-allocate arrays
	OutVertices.SetNumUninitialized(VertexCount);
	OutNormals.SetNumUninitialized(VertexCount);
	OutUVs.SetNumUninitialized(VertexCount);
	OutTangents.SetNumUninitialized(VertexCount);
	OutTriangles.SetNumUninitialized(IndexCount);

	// Convert vertices: FVector3f -> FVector
	for (int32 i = 0; i < VertexCount; ++i)
	{
		OutVertices[i] = FVector(MeshData.Positions[i]);
	}

	// Convert normals: FVector3f -> FVector
	if (MeshData.Normals.Num() == VertexCount)
	{
		for (int32 i = 0; i < VertexCount; ++i)
		{
			OutNormals[i] = FVector(MeshData.Normals[i]);
		}
	}
	else
	{
		// Default normals if not provided
		for (int32 i = 0; i < VertexCount; ++i)
		{
			OutNormals[i] = FVector::UpVector;
		}
	}

	// Convert UVs: FVector2f -> FVector2D
	if (MeshData.UVs.Num() == VertexCount)
	{
		for (int32 i = 0; i < VertexCount; ++i)
		{
			OutUVs[i] = FVector2D(MeshData.UVs[i]);
		}
	}
	else
	{
		// Default UVs if not provided
		for (int32 i = 0; i < VertexCount; ++i)
		{
			OutUVs[i] = FVector2D::ZeroVector;
		}
	}

	// Convert colors: FColor -> FLinearColor
	if (MeshData.Colors.Num() == VertexCount)
	{
		OutColors.SetNumUninitialized(VertexCount);
		for (int32 i = 0; i < VertexCount; ++i)
		{
			OutColors[i] = FLinearColor(MeshData.Colors[i]);
		}
	}
	else
	{
		// Default colors if not provided
		OutColors.SetNumUninitialized(VertexCount);
		for (int32 i = 0; i < VertexCount; ++i)
		{
			OutColors[i] = FLinearColor::White;
		}
	}

	// Calculate tangents from normals
	for (int32 i = 0; i < VertexCount; ++i)
	{
		// Simple tangent calculation perpendicular to normal
		FVector Normal = OutNormals[i];
		FVector Tangent;

		if (FMath::Abs(Normal.Z) < 0.999f)
		{
			Tangent = FVector::CrossProduct(FVector::UpVector, Normal).GetSafeNormal();
		}
		else
		{
			Tangent = FVector::CrossProduct(FVector::RightVector, Normal).GetSafeNormal();
		}

		OutTangents[i] = FProcMeshTangent(Tangent, false);
	}

	// Convert indices: uint32 -> int32
	for (int32 i = 0; i < IndexCount; ++i)
	{
		OutTriangles[i] = static_cast<int32>(MeshData.Indices[i]);
	}
}

FBox FVoxelPMCRenderer::CalculateChunkBounds(const FIntVector& ChunkCoord) const
{
	if (!CachedConfig.IsValid())
	{
		return FBox(ForceInit);
	}

	const float ChunkWorldSize = CachedConfig->GetChunkWorldSize();
	const FVector ChunkMin = CachedConfig->WorldOrigin + FVector(ChunkCoord) * ChunkWorldSize;
	const FVector ChunkMax = ChunkMin + FVector(ChunkWorldSize);

	return FBox(ChunkMin, ChunkMax);
}

void FVoxelPMCRenderer::CreateDefaultVertexColorMaterial()
{
#if WITH_EDITOR
	// Create a simple material that displays vertex colors (editor only)
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), TEXT("VoxelVertexColorMaterial"));
	if (!Material)
	{
		UE_LOG(LogVoxelRendering, Error, TEXT("FVoxelPMCRenderer: Failed to create vertex color material"));
		return;
	}

	// Create a Vertex Color expression node
	UMaterialExpressionVertexColor* VertexColorExpr = NewObject<UMaterialExpressionVertexColor>(Material);
	VertexColorExpr->MaterialExpressionEditorX = -200;
	VertexColorExpr->MaterialExpressionEditorY = 0;
	Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(VertexColorExpr);

	// Connect vertex color RGB to Base Color
	Material->GetEditorOnlyData()->BaseColor.Expression = VertexColorExpr;

	// Set material properties for voxel terrain
	Material->TwoSided = false;
	Material->SetShadingModel(MSM_DefaultLit);

	// Compile the material
	Material->PreEditChange(nullptr);
	Material->PostEditChange();

	CurrentMaterial = Material;
	DefaultVertexColorMaterial.Reset(Material);

	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelPMCRenderer: Created default vertex color material"));
#else
	// In packaged builds, a material asset must be provided via SetMaterial()
	// or configured in VoxelWorldConfiguration
	UE_LOG(LogVoxelRendering, Warning,
		TEXT("FVoxelPMCRenderer: No material set. In packaged builds, you must provide a vertex color material via SetMaterial() or VoxelWorldConfiguration."));
#endif
}
