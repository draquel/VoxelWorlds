// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWorldComponent.h"
#include "VoxelSceneProxy.h"
#include "VoxelRendering.h"
#include "Materials/MaterialInterface.h"
#include "Engine/World.h"

// ==================== UVoxelWorldComponent ====================

UVoxelWorldComponent::UVoxelWorldComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Component settings
	PrimaryComponentTick.bCanEverTick = false;
	bTickInEditor = false;

	// Rendering settings
	SetCastShadow(true);
	SetReceivesDecals(true);
	bUseAsOccluder = true;

	// Collision (handled separately, not by this component)
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);

	// Use custom bounds
	bUseAttachParentBound = false;

	// Initialize bounds
	CachedTotalBounds = FBox(ForceInit);
}

UVoxelWorldComponent::~UVoxelWorldComponent()
{
}

// ==================== UPrimitiveComponent Interface ====================

FPrimitiveSceneProxy* UVoxelWorldComponent::CreateSceneProxy()
{
	if (GetWorld() == nullptr)
	{
		return nullptr;
	}

	// Ensure we have a material
	UMaterialInterface* MaterialToUse = VoxelMaterial;
	if (!MaterialToUse)
	{
		// WARNING: Default material does not compile with FVoxelVertexFactory!
		// Users must create and assign a custom material for the voxel world.
		UE_LOG(LogVoxelRendering, Warning,
			TEXT("UVoxelWorldComponent: No VoxelMaterial set! Create a simple opaque material and assign it. ")
			TEXT("Default materials do not work with the custom vertex factory."));

		// Still use default as a fallback, but rendering will likely fail
		MaterialToUse = UMaterial::GetDefaultMaterial(MD_Surface);
	}

	UE_LOG(LogVoxelRendering, Log, TEXT("UVoxelWorldComponent: Creating scene proxy with material: %s"),
		MaterialToUse ? *MaterialToUse->GetName() : TEXT("nullptr"));

	return new FVoxelSceneProxy(this, MaterialToUse);
}

FBoxSphereBounds UVoxelWorldComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// Use very large bounds to prevent the component from being culled
	// Individual chunks are culled in GetDynamicMeshElements
	// This is standard practice for infinite/streaming terrain systems
	const float HalfWorldSize = 500000.0f;  // 5km in each direction
	const float VerticalExtent = 100000.0f;  // 1km up/down

	FBox LargeBounds(
		FVector(-HalfWorldSize, -HalfWorldSize, -VerticalExtent),
		FVector(HalfWorldSize, HalfWorldSize, VerticalExtent)
	);

	return FBoxSphereBounds(LargeBounds.TransformBy(LocalToWorld));
}

UMaterialInterface* UVoxelWorldComponent::GetMaterial(int32 ElementIndex) const
{
	return VoxelMaterial;
}

void UVoxelWorldComponent::SetMaterial(int32 ElementIndex, UMaterialInterface* Material)
{
	if (ElementIndex != 0)
	{
		return;
	}

	if (VoxelMaterial != Material)
	{
		VoxelMaterial = Material;

		// Update render thread
		FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
		if (Proxy)
		{
			ENQUEUE_RENDER_COMMAND(SetVoxelMaterial)(
				[Proxy, Material](FRHICommandListImmediate& RHICmdList)
				{
					Proxy->SetMaterial_RenderThread(Material);
				}
			);
		}

		MarkRenderStateDirtyAndNotify();
	}
}

// ==================== Chunk Management ====================

void UVoxelWorldComponent::UpdateChunkBuffers(const FChunkRenderData& RenderData)
{
	check(IsInGameThread());

	if (!RenderData.HasValidGeometry() || !RenderData.HasGPUBuffers())
	{
		// Empty or invalid data - remove chunk if it exists
		RemoveChunk(RenderData.ChunkCoord);
		return;
	}

	// Create GPU data from render data
	FVoxelChunkGPUData GPUData;
	GPUData.ChunkCoord = RenderData.ChunkCoord;
	GPUData.LODLevel = RenderData.LODLevel;
	GPUData.VertexCount = RenderData.VertexCount;
	GPUData.IndexCount = RenderData.IndexCount;
	GPUData.LocalBounds = RenderData.Bounds;
	GPUData.ChunkWorldPosition = FVector(RenderData.ChunkCoord) * ChunkWorldSize;
	GPUData.MorphFactor = RenderData.MorphFactor;
	GPUData.bIsVisible = true;
	GPUData.VertexBufferRHI = RenderData.VertexBufferRHI;
	GPUData.IndexBufferRHI = RenderData.IndexBufferRHI;
	GPUData.VertexBufferSRV = RenderData.VertexBufferSRV;

	UpdateChunkBuffersFromGPU(GPUData);
}

void UVoxelWorldComponent::UpdateChunkBuffersFromGPU(const FVoxelChunkGPUData& GPUData)
{
	check(IsInGameThread());

	const FIntVector& ChunkCoord = GPUData.ChunkCoord;

	// Update game thread tracking
	{
		FScopeLock Lock(&ChunkInfoLock);

		FChunkInfo& Info = ChunkInfoMap.FindOrAdd(ChunkCoord);

		// Update statistics (subtract old, add new)
		if (ChunkInfoMap.Contains(ChunkCoord))
		{
			// Existing chunk - stats will be updated
		}

		// Bounds are already in world space (vertices include chunk world position)
		Info.Bounds = GPUData.LocalBounds;
		Info.LODLevel = GPUData.LODLevel;
		Info.bIsVisible = GPUData.bIsVisible;

		bTotalBoundsDirty = true;
	}

	// Update statistics
	CachedVertexCount += GPUData.VertexCount;
	CachedTriangleCount += GPUData.IndexCount / 3;
	CachedGPUMemory += GPUData.GetGPUMemoryUsage();

	// Enqueue render thread update
	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (Proxy)
	{
		FVoxelChunkGPUData GPUDataCopy = GPUData;

		ENQUEUE_RENDER_COMMAND(UpdateVoxelChunk)(
			[Proxy, ChunkCoord, GPUDataCopy](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->UpdateChunkBuffers_RenderThread(RHICmdList, ChunkCoord, GPUDataCopy);
			}
		);
	}

	// Update bounds
	UpdateBounds();
}

void UVoxelWorldComponent::RemoveChunk(const FIntVector& ChunkCoord)
{
	check(IsInGameThread());

	// Update game thread tracking
	{
		FScopeLock Lock(&ChunkInfoLock);

		if (ChunkInfoMap.Remove(ChunkCoord) > 0)
		{
			bTotalBoundsDirty = true;
		}
	}

	// Enqueue render thread removal
	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (Proxy)
	{
		ENQUEUE_RENDER_COMMAND(RemoveVoxelChunk)(
			[Proxy, ChunkCoord](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->RemoveChunk_RenderThread(ChunkCoord);
			}
		);
	}

	UpdateBounds();
}

void UVoxelWorldComponent::ClearAllChunks()
{
	check(IsInGameThread());

	// Clear game thread tracking
	{
		FScopeLock Lock(&ChunkInfoLock);
		ChunkInfoMap.Empty();
		bTotalBoundsDirty = true;
	}

	// Reset statistics
	CachedVertexCount = 0;
	CachedTriangleCount = 0;
	CachedGPUMemory = 0;

	// Enqueue render thread clear
	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (Proxy)
	{
		ENQUEUE_RENDER_COMMAND(ClearVoxelChunks)(
			[Proxy](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->ClearAllChunks_RenderThread();
			}
		);
	}

	UpdateBounds();
}

void UVoxelWorldComponent::SetChunkVisible(const FIntVector& ChunkCoord, bool bNewVisibility)
{
	check(IsInGameThread());

	// Update game thread tracking
	{
		FScopeLock Lock(&ChunkInfoLock);

		FChunkInfo* Info = ChunkInfoMap.Find(ChunkCoord);
		if (Info)
		{
			Info->bIsVisible = bNewVisibility;
			bTotalBoundsDirty = true;
		}
	}

	// Enqueue render thread update
	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (Proxy)
	{
		ENQUEUE_RENDER_COMMAND(SetVoxelChunkVisible)(
			[Proxy, ChunkCoord, bNewVisibility](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->SetChunkVisible_RenderThread(ChunkCoord, bNewVisibility);
			}
		);
	}
}

void UVoxelWorldComponent::UpdateChunkMorphFactor(const FIntVector& ChunkCoord, float MorphFactor)
{
	check(IsInGameThread());

	FVoxelSceneProxy* Proxy = GetVoxelSceneProxy();
	if (Proxy)
	{
		ENQUEUE_RENDER_COMMAND(UpdateVoxelMorphFactor)(
			[Proxy, ChunkCoord, MorphFactor](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->UpdateChunkMorphFactor_RenderThread(ChunkCoord, MorphFactor);
			}
		);
	}
}

// ==================== Configuration ====================

void UVoxelWorldComponent::SetVoxelSize(float InVoxelSize)
{
	VoxelSize = FMath::Max(1.0f, InVoxelSize);
}

void UVoxelWorldComponent::SetChunkWorldSize(float InChunkWorldSize)
{
	ChunkWorldSize = FMath::Max(100.0f, InChunkWorldSize);
}

// ==================== Queries ====================

bool UVoxelWorldComponent::IsChunkLoaded(const FIntVector& ChunkCoord) const
{
	FScopeLock Lock(&ChunkInfoLock);
	return ChunkInfoMap.Contains(ChunkCoord);
}

int32 UVoxelWorldComponent::GetLoadedChunkCount() const
{
	FScopeLock Lock(&ChunkInfoLock);
	return ChunkInfoMap.Num();
}

void UVoxelWorldComponent::GetLoadedChunks(TArray<FIntVector>& OutChunks) const
{
	FScopeLock Lock(&ChunkInfoLock);

	OutChunks.Reset(ChunkInfoMap.Num());
	for (const auto& Pair : ChunkInfoMap)
	{
		OutChunks.Add(Pair.Key);
	}
}

bool UVoxelWorldComponent::GetChunkBounds(const FIntVector& ChunkCoord, FBox& OutBounds) const
{
	FScopeLock Lock(&ChunkInfoLock);

	const FChunkInfo* Info = ChunkInfoMap.Find(ChunkCoord);
	if (Info)
	{
		OutBounds = Info->Bounds;
		return true;
	}

	return false;
}

// ==================== Statistics ====================

int64 UVoxelWorldComponent::GetGPUMemoryUsage() const
{
	return CachedGPUMemory;
}

int64 UVoxelWorldComponent::GetTotalVertexCount() const
{
	return CachedVertexCount;
}

int64 UVoxelWorldComponent::GetTotalTriangleCount() const
{
	return CachedTriangleCount;
}

// ==================== Internal ====================

void UVoxelWorldComponent::SendRenderDynamicData_Concurrent()
{
	Super::SendRenderDynamicData_Concurrent();

	// Scene proxy will handle data updates via render commands
}

void UVoxelWorldComponent::MarkRenderStateDirtyAndNotify()
{
	MarkRenderStateDirty();
	MarkRenderDynamicDataDirty();
}

FVoxelSceneProxy* UVoxelWorldComponent::GetVoxelSceneProxy() const
{
	return static_cast<FVoxelSceneProxy*>(SceneProxy);
}
