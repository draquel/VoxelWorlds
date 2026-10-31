// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ChunkRenderData.generated.h"

/**
 * A chunk's mesh as the meshers produce it: the chunk manager uploads it to the renderer and
 * cooks collision from it. (FChunkRenderData, the GPU-buffer twin of this struct, went with the
 * GPU meshers' upload path in code review CR1, REND-03.)
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

	/** Whether this mesh contains any triangles with masked (alpha-cutout) materials.
	 *  Used by renderers to decide whether to create a masked mesh section. */
	bool bHasMaskedMaterial = false;

	/** Clear all mesh data */
	void Reset()
	{
		Positions.Reset();
		Normals.Reset();
		UVs.Reset();
		UV1s.Reset();
		Colors.Reset();
		Indices.Reset();
		bHasMaskedMaterial = false;
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
