// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "ChunkRenderData.generated.h"

/**
 * Render-specific data for a chunk.
 *
 * Contains GPU buffer references and render state.
 * Used by the rendering system to display chunk geometry.
 *
 * Thread Safety: Must only be accessed on render thread
 *
 * @see Documentation/DATA_STRUCTURES.md
 */
USTRUCT()
struct VOXELCORE_API FChunkRenderData
{
	GENERATED_BODY()

	/** Chunk position in chunk coordinate space */
	UPROPERTY()
	FIntVector ChunkCoord = FIntVector::ZeroValue;

	/** Current LOD level */
	UPROPERTY()
	int32 LODLevel = 0;

	/** Number of vertices in the mesh */
	UPROPERTY()
	uint32 VertexCount = 0;

	/** Number of indices in the mesh */
	UPROPERTY()
	uint32 IndexCount = 0;

	/** Chunk needs collision mesh update */
	UPROPERTY()
	bool bNeedsCollisionUpdate = false;

	/** World-space bounding box */
	UPROPERTY()
	FBox Bounds = FBox(ForceInit);

	/** LOD morph factor for smooth transitions */
	UPROPERTY()
	float MorphFactor = 0.0f;

	// GPU buffer references (non-UPROPERTY, managed manually)

	/** Vertex buffer on GPU (managed by RDG or persistent) */
	FBufferRHIRef VertexBufferRHI;

	/** Index buffer on GPU */
	FBufferRHIRef IndexBufferRHI;

	/** Shader resource view for vertex buffer */
	FShaderResourceViewRHIRef VertexBufferSRV;

	/** Default constructor */
	FChunkRenderData() = default;

	/** Construct with chunk info */
	FChunkRenderData(const FIntVector& InChunkCoord, int32 InLODLevel)
		: ChunkCoord(InChunkCoord)
		, LODLevel(InLODLevel)
	{
	}

	/** Check if render data has valid geometry */
	FORCEINLINE bool HasValidGeometry() const
	{
		return VertexCount > 0 && IndexCount > 0;
	}

	/** Check if GPU buffers are allocated */
	FORCEINLINE bool HasGPUBuffers() const
	{
		return VertexBufferRHI.IsValid() && IndexBufferRHI.IsValid();
	}

	/** Release GPU resources */
	void ReleaseGPUResources()
	{
		VertexBufferRHI.SafeRelease();
		IndexBufferRHI.SafeRelease();
		VertexBufferSRV.SafeRelease();
		VertexCount = 0;
		IndexCount = 0;
	}

	/** Get approximate GPU memory usage in bytes */
	SIZE_T GetGPUMemoryUsage() const
	{
		// FVoxelVertex is 28 bytes, indices are 32-bit
		return (VertexCount * 28) + (IndexCount * sizeof(uint32));
	}
};

/**
 * CPU-side mesh data for chunks.
 *
 * Used for editor/PMC rendering path and collision generation.
 * Not used in the optimized runtime GPU path.
 */
USTRUCT()
struct VOXELCORE_API FChunkMeshData
{
	GENERATED_BODY()

	/** Vertex positions */
	UPROPERTY()
	TArray<FVector3f> Positions;

	/** Vertex normals */
	UPROPERTY()
	TArray<FVector3f> Normals;

	/** Texture coordinates (face UVs for texture tiling) */
	UPROPERTY()
	TArray<FVector2f> UVs;

	/**
	 * Secondary texture coordinates for material data.
	 * UV1.x = MaterialID as float (0-255)
	 * UV1.y = FaceType as float (0=Top, 1=Side, 2=Bottom)
	 * Using UV channel avoids sRGB color space conversion issues with vertex colors.
	 */
	UPROPERTY()
	TArray<FVector2f> UV1s;

	/** Vertex colors (packed AO + BiomeID, legacy MaterialID kept for compatibility) */
	UPROPERTY()
	TArray<FColor> Colors;

	/** Triangle indices */
	UPROPERTY()
	TArray<uint32> Indices;

	/** Clear all mesh data */
	void Reset()
	{
		Positions.Reset();
		Normals.Reset();
		UVs.Reset();
		UV1s.Reset();
		Colors.Reset();
		Indices.Reset();
	}

	/** Check if mesh has valid data */
	FORCEINLINE bool IsValid() const
	{
		return Positions.Num() > 0 && Indices.Num() > 0;
	}

	/** Get vertex count */
	FORCEINLINE int32 GetVertexCount() const
	{
		return Positions.Num();
	}

	/** Get triangle count */
	FORCEINLINE int32 GetTriangleCount() const
	{
		return Indices.Num() / 3;
	}

	/** Get approximate CPU memory usage */
	SIZE_T GetMemoryUsage() const
	{
		return Positions.GetAllocatedSize()
			+ Normals.GetAllocatedSize()
			+ UVs.GetAllocatedSize()
			+ UV1s.GetAllocatedSize()
			+ Colors.GetAllocatedSize()
			+ Indices.GetAllocatedSize();
	}
};
