// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelVertex.h"
#include "ChunkRenderData.generated.h"

/**
 * A vertex's material blend (VP2-O4, soft material borders), as FChunkMeshData::MaterialBlends carries it: 0 for none,
 * else the blend flag, the second material in bits 8-15 and, where the quad has a third, the third flag and the third
 * material in bits 16-23; the weight flags mark the material the vertex shows (the second, the third, or neither: the
 * quad's own). The Dual Contouring meshers set it on the vertices of a quad whose cells have more than one material; the
 * renderer passes it to the shader as vertex colour (R the second material, A its weight; G the third, B its weight:
 * M_VoxelMaster's FarFieldMaterialPick), so the weights interpolate across the triangles and a border fades over a cell.
 */
namespace VoxelMaterialBlend
{
	inline constexpr uint32 BlendFlag = 1;
	inline constexpr uint32 WeightFlag = 2;       // the vertex shows the second material
	inline constexpr uint32 ThirdWeightFlag = 4;  // the vertex shows the third material
	inline constexpr uint32 ThirdFlag = 8;        // the quad has a third material
	inline constexpr uint32 FlagMask = 0xF;

	/**
	 * @param Secondary The quad's second material
	 * @param bShowsSecondary Whether the vertex shows it
	 * @param bHasThird Whether the quad has a third material
	 * @param Third The third material (with bHasThird)
	 * @param bShowsThird Whether the vertex shows it
	 * @return The code of a vertex in the blend
	 */
	FORCEINLINE uint32 Encode(uint8 Secondary, bool bShowsSecondary, bool bHasThird = false, uint8 Third = 0, bool bShowsThird = false)
	{
		return (static_cast<uint32>(Secondary) << 8) | BlendFlag | (bShowsSecondary ? WeightFlag : 0)
			| (bHasThird ? (ThirdFlag | (static_cast<uint32>(Third) << 16) | (bShowsThird ? ThirdWeightFlag : 0)) : 0);
	}

	/** @return Whether the code is a blend */
	FORCEINLINE bool IsBlend(uint32 Code) { return (Code & BlendFlag) != 0; }

	/** @return Whether the vertex shows the second material (its weight 1) */
	FORCEINLINE bool ShowsSecondary(uint32 Code) { return (Code & WeightFlag) != 0; }

	/** @return The blend's second material */
	FORCEINLINE uint8 Secondary(uint32 Code) { return static_cast<uint8>(Code >> 8); }

	/** @return Whether the blend has a third material */
	FORCEINLINE bool HasThird(uint32 Code) { return (Code & ThirdFlag) != 0; }

	/** @return Whether the vertex shows the third material (its weight 1) */
	FORCEINLINE bool ShowsThird(uint32 Code) { return (Code & ThirdWeightFlag) != 0; }

	/** @return The blend's third material (valid with HasThird) */
	FORCEINLINE uint8 Third(uint32 Code) { return static_cast<uint8>(Code >> 16); }

	/** @return The code without its weight flags: equal across a triangle (the blend is flat per triangle) */
	FORCEINLINE uint32 WithoutWeights(uint32 Code) { return Code & ~(WeightFlag | ThirdWeightFlag); }
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
	TArray<uint32> MaterialBlends;

	/** Triangle indices */
	UPROPERTY()
	TArray<uint32> Indices;

	/** Whether this mesh contains any triangles with masked (alpha-cutout) materials.
	 *  Used by renderers to decide whether to create a masked mesh section. */
	bool bHasMaskedMaterial = false;

	/** The Lumen cards its mesh worker fitted to it (G116, VoxelRendering's PrebuildVoxelChunkMeshCards): the renderer takes
	 *  them with the mesh; null when the fitted cards are off or the mesh came from elsewhere (the renderer fits them then) */
	TSharedPtr<const FVoxelPrebuiltChunkMeshCards, ESPMode::ThreadSafe> PrebuiltLumenCards;

	/**
	 * The mesh's vertices in the renderer's format, made on its mesh worker (G294, PrepareRenderVertices) so the submit on
	 * the game thread moves them in instead of converting; empty when the mesh came another way (the renderer converts
	 * then). Not reflected: the renderer's alone.
	 */
	TArray<FVoxelVertex> RenderVertices;

	/** The positions' bounds, made with RenderVertices (the renderer's chunk-local bounds) */
	FBox RenderBounds = FBox(ForceInit);

	/**
	 * Convert a mesh to the renderer's vertices, its positions' bounds in the same pass (FVoxelMeshRenderer's conversion,
	 * shared with the mesh workers). Pure; any thread.
	 *
	 * @param MeshData The mesher's mesh (normals, UVs, colours and blends full-size or absent)
	 * @param OutVertices Receives a vertex per position
	 * @param OutLocalBounds Receives the positions' bounds (invalid for no vertex)
	 */
	static void ConvertToVoxelVertices(const FChunkMeshData& MeshData, TArray<FVoxelVertex>& OutVertices, FBox& OutLocalBounds);

	/**
	 * Make RenderVertices and RenderBounds from this mesh (a mesh worker, G294), and drop the attributes only the
	 * conversion reads (normals, UVs, colours, blends; the positions and indices stay: the mesh's validity, its counts
	 * and the renderer's indices). Any thread, on a mesh no other thread reads.
	 *
	 * @param bDropConverted Drop the converted attributes (a mesh only the renderer takes; false keeps them)
	 */
	void PrepareRenderVertices(bool bDropConverted);

	/** @return Whether RenderVertices hold this mesh's vertices (one per position) */
	FORCEINLINE bool HasRenderVertices() const
	{
		return Positions.Num() > 0 && RenderVertices.Num() == Positions.Num();
	}

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
		RenderVertices.Reset();
		RenderBounds = FBox(ForceInit);
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
			+ Indices.GetAllocatedSize()
			+ RenderVertices.GetAllocatedSize();
	}
};
