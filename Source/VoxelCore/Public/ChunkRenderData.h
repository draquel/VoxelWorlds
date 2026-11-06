// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ChunkRenderData.generated.h"

/**
 * A vertex's material blend (VP2-O4, soft material borders), as FChunkMeshData::MaterialBlends carries it: 0 for none,
 * else the blend flag, the second material in bits 8-15 and the weight flag where the vertex shows the second material.
 * The Dual Contouring meshers set it on the vertices of a quad whose cells have two materials; the renderer passes it to
 * the shader as the far field's blend (vertex colour R the second material, A 255 less its weight: M_VoxelMaster's
 * FarFieldMaterialPick), so the weight interpolates across the triangles and a border fades over a cell.
 */
namespace VoxelMaterialBlend
{
	inline constexpr uint16 BlendFlag = 1;
	inline constexpr uint16 WeightFlag = 2;

	/** @return The code of a vertex in a blend with Secondary, showing it where bShowsSecondary */
	FORCEINLINE uint16 Encode(uint8 Secondary, bool bShowsSecondary)
	{
		return static_cast<uint16>((static_cast<uint16>(Secondary) << 8) | BlendFlag | (bShowsSecondary ? WeightFlag : 0));
	}

	/** @return Whether the code is a blend */
	FORCEINLINE bool IsBlend(uint16 Code) { return (Code & BlendFlag) != 0; }

	/** @return Whether the vertex shows the second material (weight 1) */
	FORCEINLINE bool ShowsSecondary(uint16 Code) { return (Code & WeightFlag) != 0; }

	/** @return The blend's second material */
	FORCEINLINE uint8 Secondary(uint16 Code) { return static_cast<uint8>(Code >> 8); }
}

/** The Lumen cards a mesh worker fitted to a mesh for the renderer (G116): defined by VoxelRendering, opaque here */
struct FVoxelPrebuiltChunkMeshCards;

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

	/**
	 * Each vertex's material blend (VoxelMaterialBlend, VP2-O4): the Dual Contouring meshers fill it for every vertex,
	 * other meshers leave it empty (no blend)
	 */
	UPROPERTY()
	TArray<uint16> MaterialBlends;

	/** Triangle indices */
	UPROPERTY()
	TArray<uint32> Indices;

	/** Whether this mesh contains any triangles with masked (alpha-cutout) materials.
	 *  Used by renderers to decide whether to create a masked mesh section. */
	bool bHasMaskedMaterial = false;

	/** The Lumen cards its mesh worker fitted to it (G116, VoxelRendering's PrebuildVoxelChunkMeshCards): the renderer takes
	 *  them with the mesh; null when the fitted cards are off or the mesh came from elsewhere (the renderer fits them then) */
	TSharedPtr<const FVoxelPrebuiltChunkMeshCards, ESPMode::ThreadSafe> PrebuiltLumenCards;

	/** Clear all mesh data */
	void Reset()
	{
		Positions.Reset();
		Normals.Reset();
		UVs.Reset();
		UV1s.Reset();
		Colors.Reset();
		MaterialBlends.Reset();
		Indices.Reset();
		bHasMaskedMaterial = false;
		PrebuiltLumenCards.Reset();
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
			+ MaterialBlends.GetAllocatedSize()
			+ Indices.GetAllocatedSize();
	}
};
