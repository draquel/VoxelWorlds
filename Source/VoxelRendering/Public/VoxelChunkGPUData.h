// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"
#include "RHIResources.h"
#include "VoxelVertex.h"

/**
 * Index buffer for voxel chunks.
 * Wrapper around FIndexBuffer that can use a pre-created RHI buffer.
 * Must call InitResource() after InitWithRHIBuffer() to mark as initialized.
 */
class VOXELRENDERING_API FVoxelIndexBuffer : public FIndexBuffer
{
public:
	FVoxelIndexBuffer() : NumIndices(0) {}

	/** Set up with a pre-created RHI buffer. Call InitResource() after this. */
	void InitWithRHIBuffer(FBufferRHIRef InIndexBufferRHI, uint32 InNumIndices)
	{
		PendingBuffer = InIndexBufferRHI;
		NumIndices = InNumIndices;
	}

	/** Override InitRHI to use our pre-created buffer */
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		IndexBufferRHI = PendingBuffer;
	}

	/** Override ReleaseRHI to clean up */
	virtual void ReleaseRHI() override
	{
		IndexBufferRHI.SafeRelease();
		PendingBuffer.SafeRelease();
		NumIndices = 0;
	}

	/** Release the RHI buffer reference */
	void ReleaseRHIBuffer()
	{
		ReleaseResource();
		PendingBuffer.SafeRelease();
		NumIndices = 0;
	}

	uint32 GetNumIndices() const { return NumIndices; }

private:
	FBufferRHIRef PendingBuffer;
	uint32 NumIndices;
};

/**
 * Per-chunk GPU buffer container.
 *
 * Holds all GPU resources needed to render a single voxel chunk.
 * Managed by FVoxelSceneProxy on the render thread.
 *
 * Thread Safety: Must only be accessed on render thread
 */
struct VOXELRENDERING_API FVoxelChunkGPUData
{
	/** Chunk coordinate */
	FIntVector ChunkCoord = FIntVector::ZeroValue;

	/** LOD level */
	int32 LODLevel = 0;

	/** Number of vertices */
	uint32 VertexCount = 0;

	/** Number of indices */
	uint32 IndexCount = 0;

	/** Local-space bounding box */
	FBox LocalBounds = FBox(ForceInit);

	/** World position of chunk origin */
	FVector ChunkWorldPosition = FVector::ZeroVector;

	/** LOD morph factor for smooth transitions */
	float MorphFactor = 0.0f;

	/** Visibility flag */
	bool bIsVisible = true;

	/** Vertex buffer RHI resource */
	FBufferRHIRef VertexBufferRHI;

	/** Index buffer RHI resource */
	FBufferRHIRef IndexBufferRHI;

	/** Index buffer wrapper for mesh batch rendering */
	TSharedPtr<FVoxelIndexBuffer> IndexBuffer;

	/** Shader resource view for vertex buffer */
	FShaderResourceViewRHIRef VertexBufferSRV;

	/** Check if GPU buffers are valid */
	FORCEINLINE bool HasValidBuffers() const
	{
		return VertexBufferRHI.IsValid() && IndexBufferRHI.IsValid() && VertexCount > 0 && IndexCount > 0;
	}

	/** Get approximate GPU memory usage in bytes */
	FORCEINLINE SIZE_T GetGPUMemoryUsage() const
	{
		return (VertexCount * sizeof(FVoxelVertex)) + (IndexCount * sizeof(uint32));
	}

	/** Initialize index buffer wrapper from RHI buffer. Must be called on render thread. */
	void InitIndexBufferWrapper(FRHICommandListBase& RHICmdList)
	{
		if (IndexBufferRHI.IsValid() && IndexCount > 0)
		{
			IndexBuffer = MakeShared<FVoxelIndexBuffer>();
			IndexBuffer->InitWithRHIBuffer(IndexBufferRHI, IndexCount);
			// InitResource() will call InitRHI() which assigns our pre-created buffer
			// and marks the resource as initialized
			IndexBuffer->InitResource(RHICmdList);
		}
	}

	/** Release GPU resources. Can be called from any thread. */
	void ReleaseResources()
	{
		if (IndexBuffer.IsValid())
		{
			// ReleaseRHIBuffer calls ReleaseResource internally
			IndexBuffer->ReleaseRHIBuffer();
			IndexBuffer.Reset();
		}
		VertexBufferRHI.SafeRelease();
		IndexBufferRHI.SafeRelease();
		VertexBufferSRV.SafeRelease();
		VertexCount = 0;
		IndexCount = 0;
	}
};
