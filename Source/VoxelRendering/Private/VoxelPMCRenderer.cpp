// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelPMCRenderer.h"
#include "VoxelRendering.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelMaterialAtlas.h"
#include "VoxelMaterialRegistry.h"
#include "ProceduralMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture2DArray.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
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

	// Sync material mode with configuration's meshing mode
	bUseSmoothMeshing = (WorldConfig->MeshingMode == EMeshingMode::Smooth);
	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelPMCRenderer: MeshingMode=%s, bUseSmoothMeshing=%s"),
		bUseSmoothMeshing ? TEXT("Smooth") : TEXT("Cubic"),
		bUseSmoothMeshing ? TEXT("true") : TEXT("false"));

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
	OpaqueMIC.Reset();
	MaskedMIC.Reset();
	DynamicMaterialInstance.Reset();
	MaskedMaterialInstance.Reset();
	MaskedMaterialIDs.Empty();
	MaterialAtlas.Reset();

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
	TArray<FVector2D> UV0;
	TArray<FVector2D> UV1;
	TArray<FColor> Colors;
	TArray<FProcMeshTangent> Tangents;

	ConvertMeshDataToPMCFormat(MeshData, Vertices, Triangles, Normals, UV0, UV1, Colors, Tangents);

	// Acquire or reuse a component
	UProceduralMeshComponent* PMC = nullptr;
	FPMCChunkData* ExistingData = ChunkDataMap.Find(ChunkCoord);

	if (ExistingData && ExistingData->MeshComponent.IsValid())
	{
		PMC = ExistingData->MeshComponent.Get();
		// Clear existing sections before rebuilding
		PMC->ClearAllMeshSections();
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

	// Check if this mesh has any masked materials
	bool bHasMasked = false;
	if (MaskedMaterialIDs.Num() > 0 && MaskedMaterialInstance.IsValid())
	{
		for (const FVector2D& UV1Val : UV1)
		{
			uint8 MatID = static_cast<uint8>(FMath::RoundToInt(UV1Val.X));
			if (MaskedMaterialIDs.Contains(MatID))
			{
				bHasMasked = true;
				break;
			}
		}
	}

	TArray<FVector2D> EmptyUV;  // UV2 and UV3 not used

	if (!bHasMasked)
	{
		// Single opaque section (fast path — most chunks)
		PMC->CreateMeshSection(
			0,
			Vertices,
			Triangles,
			Normals,
			UV0,
			UV1,
			EmptyUV,
			EmptyUV,
			Colors,
			Tangents,
			bGenerateCollision
		);

		if (CurrentMaterial.IsValid())
		{
			PMC->SetMaterial(0, CurrentMaterial.Get());
		}
	}
	else
	{
		// Partition triangles into opaque and masked groups
		const int32 NumTriangles = Triangles.Num() / 3;

		// Tag each triangle: false = opaque, true = masked
		TArray<bool> TriIsMasked;
		TriIsMasked.SetNumUninitialized(NumTriangles);

		int32 MaskedTriCount = 0;
		for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
		{
			// Check MaterialID of first vertex of triangle (all verts share same MaterialID in cubic mesh)
			int32 VertIdx = Triangles[TriIdx * 3];
			uint8 MatID = static_cast<uint8>(FMath::RoundToInt(UV1[VertIdx].X));
			bool bMasked = MaskedMaterialIDs.Contains(MatID);
			TriIsMasked[TriIdx] = bMasked;
			if (bMasked) ++MaskedTriCount;
		}

		int32 OpaqueTriCount = NumTriangles - MaskedTriCount;

		// Build vertex remapping and section data for each group
		auto BuildSection = [&](bool bWantMasked, int32 TriCount) -> bool
		{
			if (TriCount == 0) return false;

			TMap<int32, int32> VertRemap;
			VertRemap.Reserve(TriCount * 3);

			TArray<FVector> SecVerts;
			TArray<int32> SecTris;
			TArray<FVector> SecNormals;
			TArray<FVector2D> SecUV0;
			TArray<FVector2D> SecUV1;
			TArray<FColor> SecColors;
			TArray<FProcMeshTangent> SecTangents;

			SecVerts.Reserve(TriCount * 3);
			SecTris.Reserve(TriCount * 3);
			SecNormals.Reserve(TriCount * 3);
			SecUV0.Reserve(TriCount * 3);
			SecUV1.Reserve(TriCount * 3);
			SecColors.Reserve(TriCount * 3);
			SecTangents.Reserve(TriCount * 3);

			for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
			{
				if (TriIsMasked[TriIdx] != bWantMasked) continue;

				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					int32 OldIdx = Triangles[TriIdx * 3 + Corner];
					int32* NewIdxPtr = VertRemap.Find(OldIdx);
					int32 NewIdx;
					if (NewIdxPtr)
					{
						NewIdx = *NewIdxPtr;
					}
					else
					{
						NewIdx = SecVerts.Num();
						VertRemap.Add(OldIdx, NewIdx);
						SecVerts.Add(Vertices[OldIdx]);
						SecNormals.Add(Normals[OldIdx]);
						SecUV0.Add(UV0[OldIdx]);
						SecUV1.Add(UV1[OldIdx]);
						SecColors.Add(Colors[OldIdx]);
						SecTangents.Add(Tangents[OldIdx]);
					}
					SecTris.Add(NewIdx);
				}
			}

			int32 SectionIndex = bWantMasked ? 1 : 0;
			PMC->CreateMeshSection(
				SectionIndex,
				SecVerts,
				SecTris,
				SecNormals,
				SecUV0,
				SecUV1,
				EmptyUV,
				EmptyUV,
				SecColors,
				SecTangents,
				!bWantMasked && bGenerateCollision  // Only opaque section gets collision
			);
			return true;
		};

		// Build opaque section (index 0)
		BuildSection(false, OpaqueTriCount);
		if (CurrentMaterial.IsValid())
		{
			PMC->SetMaterial(0, CurrentMaterial.Get());
		}

		// Build masked section (index 1)
		if (BuildSection(true, MaskedTriCount))
		{
			PMC->SetMaterial(1, MaskedMaterialInstance.Get());
		}
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

	// Clear all material instances when base material changes
	// New instances will be created when SetMaterialAtlas is called
	OpaqueMIC.Reset();
	MaskedMIC.Reset();
	DynamicMaterialInstance.Reset();
	MaskedMaterialInstance.Reset();

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
	// Update atlas parameters if we have a dynamic instance
	UpdateMaterialAtlasParameters();
}

void FVoxelPMCRenderer::SetMaterialAtlas(UVoxelMaterialAtlas* Atlas)
{
	check(IsInGameThread());

	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelPMCRenderer::SetMaterialAtlas called - CurrentMaterial: %s, Atlas: %s"),
		CurrentMaterial.IsValid() ? *CurrentMaterial->GetName() : TEXT("NULL"),
		Atlas ? *Atlas->GetName() : TEXT("NULL"));

	MaterialAtlas = Atlas;

	if (MaterialAtlas.IsValid())
	{
		// Update registry with atlas positions
		FVoxelMaterialRegistry::SetAtlasPositions(
			MaterialAtlas->MaterialConfigs,
			MaterialAtlas->AtlasColumns,
			MaterialAtlas->AtlasRows);

		// Cache masked material IDs from atlas
		MaskedMaterialIDs = MaterialAtlas->GetMaskedMaterialIDs();

		// Create a dynamic material instance if we have a material but no dynamic instance yet
		if (CurrentMaterial.IsValid() && !DynamicMaterialInstance.IsValid())
		{
			UE_LOG(LogVoxelRendering, Log, TEXT("  Creating dynamic material instance..."));
			CreateVoxelMaterialInstance(CurrentMaterial.Get());
		}
		else
		{
			// Just update parameters on existing instance
			UpdateMaterialAtlasParameters();
		}
	}
}

UVoxelMaterialAtlas* FVoxelPMCRenderer::GetMaterialAtlas() const
{
	return MaterialAtlas.Get();
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

int64 FVoxelPMCRenderer::GetCPUMemoryUsage() const
{
	return static_cast<int64>(TotalMemoryUsage);
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
		TEXT("  Collision: %s\n")
		TEXT("  MeshingMode: %s\n")
		TEXT("  MaterialAtlas: %s"),
		ChunkDataMap.Num(),
		TotalVertexCount,
		TotalTriangleCount,
		TotalMemoryUsage / (1024.0 * 1024.0),
		ComponentPool.Num(),
		bGenerateCollision ? TEXT("Enabled") : TEXT("Disabled"),
		bUseSmoothMeshing ? TEXT("Smooth") : TEXT("Cubic"),
		MaterialAtlas.IsValid() ? *MaterialAtlas->GetName() : TEXT("None")
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
	TArray<FVector2D>& OutUV0,
	TArray<FVector2D>& OutUV1,
	TArray<FColor>& OutColors,
	TArray<FProcMeshTangent>& OutTangents)
{
	const int32 VertexCount = MeshData.Positions.Num();
	const int32 IndexCount = MeshData.Indices.Num();

	// Pre-allocate arrays
	OutVertices.SetNumUninitialized(VertexCount);
	OutNormals.SetNumUninitialized(VertexCount);
	OutUV0.SetNumUninitialized(VertexCount);
	OutUV1.SetNumUninitialized(VertexCount);
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

	// Convert UV0 (texture tiling): FVector2f -> FVector2D
	if (MeshData.UVs.Num() == VertexCount)
	{
		for (int32 i = 0; i < VertexCount; ++i)
		{
			OutUV0[i] = FVector2D(MeshData.UVs[i]);
		}
	}
	else
	{
		// Default UVs if not provided
		for (int32 i = 0; i < VertexCount; ++i)
		{
			OutUV0[i] = FVector2D::ZeroVector;
		}
	}

	// Convert UV1 (MaterialID + FaceType): FVector2f -> FVector2D
	// UV1.x = MaterialID as float (0-255)
	// UV1.y = FaceType as float (0=Top, 1=Side, 2=Bottom)
	if (MeshData.UV1s.Num() == VertexCount)
	{
		for (int32 i = 0; i < VertexCount; ++i)
		{
			OutUV1[i] = FVector2D(MeshData.UV1s[i]);
		}
	}
	else
	{
		// Default UV1 if not provided (MaterialID 0, FaceType Side)
		for (int32 i = 0; i < VertexCount; ++i)
		{
			OutUV1[i] = FVector2D(0.0, 1.0);
		}
	}

	// Copy colors directly (FColor -> FColor, no conversion needed)
	if (MeshData.Colors.Num() == VertexCount)
	{
		OutColors.SetNumUninitialized(VertexCount);
		for (int32 i = 0; i < VertexCount; ++i)
		{
			OutColors[i] = MeshData.Colors[i];
		}
	}
	else
	{
		// Default colors if not provided
		OutColors.SetNumUninitialized(VertexCount);
		for (int32 i = 0; i < VertexCount; ++i)
		{
			OutColors[i] = FColor::White;
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

UMaterialInstanceDynamic* FVoxelPMCRenderer::CreateVoxelMaterialInstance(UMaterialInterface* MasterMaterial)
{
	if (!MasterMaterial)
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("FVoxelPMCRenderer::CreateVoxelMaterialInstance: MasterMaterial is null"));
		return nullptr;
	}

	// Master material should be set to BLEND_Masked so the OpacityMask pin is available.
	// We create a MIC with BLEND_Opaque override (requires static permutation recompile),
	// then parent the opaque MID to it. The masked MID parents directly to master (already Masked).

	// Create opaque MIC with blend mode override (MICs support UpdateStaticPermutation)
	UMaterialInstanceConstant* NewOpaqueMIC = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
	if (!NewOpaqueMIC)
	{
		UE_LOG(LogVoxelRendering, Error, TEXT("FVoxelPMCRenderer: Failed to create opaque MIC"));
		return nullptr;
	}
	NewOpaqueMIC->Parent = MasterMaterial;
	NewOpaqueMIC->BasePropertyOverrides.bOverride_BlendMode = true;
	NewOpaqueMIC->BasePropertyOverrides.BlendMode = BLEND_Opaque;
	NewOpaqueMIC->UpdateStaticPermutation();
	OpaqueMIC.Reset(NewOpaqueMIC);

	// Create opaque MID from the MIC (inherits Opaque blend mode, dynamic atlas params)
	UMaterialInstanceDynamic* NewInstance = UMaterialInstanceDynamic::Create(NewOpaqueMIC, GetTransientPackage());

	if (NewInstance)
	{
		DynamicMaterialInstance.Reset(NewInstance);

		// Update the current material reference
		CurrentMaterial = NewInstance;

		// Create masked MIC with two-sided override (internal faces visible from both sides)
		UMaterialInstanceConstant* NewMaskedMIC = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
		if (NewMaskedMIC)
		{
			NewMaskedMIC->Parent = MasterMaterial;
			NewMaskedMIC->BasePropertyOverrides.bOverride_TwoSided = true;
			NewMaskedMIC->BasePropertyOverrides.TwoSided = true;
			NewMaskedMIC->UpdateStaticPermutation();
			MaskedMIC.Reset(NewMaskedMIC);

			// Create masked MID from the MIC (inherits Masked + TwoSided)
			UMaterialInstanceDynamic* MaskedInstance = UMaterialInstanceDynamic::Create(NewMaskedMIC, GetTransientPackage());
			if (MaskedInstance)
			{
				MaskedMaterialInstance.Reset(MaskedInstance);
				UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelPMCRenderer: Created masked material instance (two-sided)"));
			}
		}

		// Cache masked material IDs
		if (MaterialAtlas.IsValid())
		{
			MaskedMaterialIDs = MaterialAtlas->GetMaskedMaterialIDs();
		}
		else
		{
			MaskedMaterialIDs = FVoxelMaterialRegistry::GetMaskedMaterialIDs();
		}

		// Apply to all existing chunks
		for (auto& Pair : ChunkDataMap)
		{
			if (Pair.Value.MeshComponent.IsValid())
			{
				Pair.Value.MeshComponent->SetMaterial(0, NewInstance);
			}
		}

		// Configure with atlas parameters
		UpdateMaterialAtlasParameters();

		UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelPMCRenderer: Created dynamic material instance from: %s"), *MasterMaterial->GetName());
	}

	return NewInstance;
}

void FVoxelPMCRenderer::UpdateMaterialAtlasParameters()
{
	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelPMCRenderer::UpdateMaterialAtlasParameters called - DynamicMaterial: %s, MaterialAtlas: %s"),
		DynamicMaterialInstance.IsValid() ? TEXT("Valid") : TEXT("NULL"),
		MaterialAtlas.IsValid() ? TEXT("Valid") : TEXT("NULL"));

	if (!DynamicMaterialInstance.IsValid())
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("FVoxelPMCRenderer::UpdateMaterialAtlasParameters: No DynamicMaterialInstance, skipping"));
		return;
	}

	UMaterialInstanceDynamic* MID = DynamicMaterialInstance.Get();

	// Set smooth meshing switch (matches bSmoothTerrain parameter in M_VoxelMaster)
	MID->SetScalarParameterValue(FName("bSmoothTerrain"), bUseSmoothMeshing ? 1.0f : 0.0f);

	if (!MaterialAtlas.IsValid())
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("FVoxelPMCRenderer::UpdateMaterialAtlasParameters: No MaterialAtlas, skipping atlas setup"));
		return;
	}

	UVoxelMaterialAtlas* Atlas = MaterialAtlas.Get();

	// ===== Material LUT (Face Variant Lookup Table) =====

	// Build LUT if needed
	if (Atlas->IsLUTDirty() || !Atlas->GetMaterialLUT())
	{
		UE_LOG(LogVoxelRendering, Log, TEXT("Building MaterialLUT (Dirty=%s, Exists=%s)"),
			Atlas->IsLUTDirty() ? TEXT("Yes") : TEXT("No"),
			Atlas->GetMaterialLUT() ? TEXT("Yes") : TEXT("No"));
		Atlas->BuildMaterialLUT();
	}

	// Pass LUT texture to material
	if (UTexture2D* LUT = Atlas->GetMaterialLUT())
	{
		MID->SetTextureParameterValue(FName("MaterialLUT"), LUT);
		UE_LOG(LogVoxelRendering, Log, TEXT("Set MaterialLUT texture: %s (%dx%d)"),
			*LUT->GetName(), LUT->GetSizeX(), LUT->GetSizeY());
	}
	else
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("MaterialLUT is NULL after build attempt!"));
	}

	// ===== Packed Atlas Parameters (Cubic Terrain) =====

	if (Atlas->PackedAlbedoAtlas)
	{
		MID->SetTextureParameterValue(FName("PackedAlbedoAtlas"), Atlas->PackedAlbedoAtlas);
		UE_LOG(LogVoxelRendering, Log, TEXT("Set PackedAlbedoAtlas: %s"), *Atlas->PackedAlbedoAtlas->GetName());
	}
	else
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("PackedAlbedoAtlas is NULL!"));
	}

	if (Atlas->PackedNormalAtlas)
	{
		MID->SetTextureParameterValue(FName("PackedNormalAtlas"), Atlas->PackedNormalAtlas);
	}

	if (Atlas->PackedRoughnessAtlas)
	{
		MID->SetTextureParameterValue(FName("PackedRoughnessAtlas"), Atlas->PackedRoughnessAtlas);
	}

	MID->SetScalarParameterValue(FName("AtlasColumns"), static_cast<float>(Atlas->AtlasColumns));
	MID->SetScalarParameterValue(FName("AtlasRows"), static_cast<float>(Atlas->AtlasRows));
	UE_LOG(LogVoxelRendering, Log, TEXT("Set Atlas dimensions: %d x %d"), Atlas->AtlasColumns, Atlas->AtlasRows);

	// ===== Texture Array Parameters (Smooth Terrain) =====

	// Build texture arrays if needed (similar to LUT)
	if (Atlas->AreTextureArraysDirty() || !Atlas->AlbedoArray)
	{
		UE_LOG(LogVoxelRendering, Log, TEXT("Building Texture Arrays (Dirty=%s, AlbedoArray=%s)"),
			Atlas->AreTextureArraysDirty() ? TEXT("Yes") : TEXT("No"),
			Atlas->AlbedoArray ? TEXT("Exists") : TEXT("NULL"));
		Atlas->BuildTextureArrays();
	}

	if (Atlas->AlbedoArray)
	{
		MID->SetTextureParameterValue(FName("AlbedoArray"), Atlas->AlbedoArray);
		UE_LOG(LogVoxelRendering, Log, TEXT("Set AlbedoArray texture parameter"));
	}

	if (Atlas->NormalArray)
	{
		MID->SetTextureParameterValue(FName("NormalArray"), Atlas->NormalArray);
		UE_LOG(LogVoxelRendering, Log, TEXT("Set NormalArray texture parameter"));
	}

	if (Atlas->RoughnessArray)
	{
		MID->SetTextureParameterValue(FName("RoughnessArray"), Atlas->RoughnessArray);
		UE_LOG(LogVoxelRendering, Log, TEXT("Set RoughnessArray texture parameter"));
	}

	// Update masked material IDs cache
	MaskedMaterialIDs = Atlas->GetMaskedMaterialIDs();

	// Apply same atlas parameters to masked material instance
	if (MaskedMaterialInstance.IsValid())
	{
		UMaterialInstanceDynamic* MaskedMID = MaskedMaterialInstance.Get();

		MaskedMID->SetScalarParameterValue(FName("bSmoothTerrain"), bUseSmoothMeshing ? 1.0f : 0.0f);

		if (UTexture2D* LUT = Atlas->GetMaterialLUT())
		{
			MaskedMID->SetTextureParameterValue(FName("MaterialLUT"), LUT);
		}

		if (Atlas->PackedAlbedoAtlas)
		{
			MaskedMID->SetTextureParameterValue(FName("PackedAlbedoAtlas"), Atlas->PackedAlbedoAtlas);
		}
		if (Atlas->PackedNormalAtlas)
		{
			MaskedMID->SetTextureParameterValue(FName("PackedNormalAtlas"), Atlas->PackedNormalAtlas);
		}
		if (Atlas->PackedRoughnessAtlas)
		{
			MaskedMID->SetTextureParameterValue(FName("PackedRoughnessAtlas"), Atlas->PackedRoughnessAtlas);
		}

		MaskedMID->SetScalarParameterValue(FName("AtlasColumns"), static_cast<float>(Atlas->AtlasColumns));
		MaskedMID->SetScalarParameterValue(FName("AtlasRows"), static_cast<float>(Atlas->AtlasRows));

		if (Atlas->AlbedoArray)
		{
			MaskedMID->SetTextureParameterValue(FName("AlbedoArray"), Atlas->AlbedoArray);
		}
		if (Atlas->NormalArray)
		{
			MaskedMID->SetTextureParameterValue(FName("NormalArray"), Atlas->NormalArray);
		}
		if (Atlas->RoughnessArray)
		{
			MaskedMID->SetTextureParameterValue(FName("RoughnessArray"), Atlas->RoughnessArray);
		}

		UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelPMCRenderer: Updated masked material instance parameters"));
	}

	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelPMCRenderer::UpdateMaterialAtlasParameters COMPLETE: Columns=%d, Rows=%d, SmoothMeshing=%s, LUT=%s, AlbedoAtlas=%s, MaskedMaterials=%d"),
		Atlas->AtlasColumns, Atlas->AtlasRows,
		bUseSmoothMeshing ? TEXT("true") : TEXT("false"),
		Atlas->GetMaterialLUT() ? TEXT("valid") : TEXT("null"),
		Atlas->PackedAlbedoAtlas ? TEXT("valid") : TEXT("null"),
		MaskedMaterialIDs.Num());
}
