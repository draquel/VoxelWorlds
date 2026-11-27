// Copyright Daniel Raquel. All Rights Reserved.

#include "ChunkRenderData.h"

void FChunkMeshData::ConvertToVoxelVertices(const FChunkMeshData& MeshData, TArray<FVoxelVertex>& OutVertices, FBox& OutLocalBounds)
{
	const int32 VertexCount = MeshData.Positions.Num();
	OutVertices.SetNumUninitialized(VertexCount);
	OutLocalBounds.Init();

	// Attribute arrays are either full-size or absent — decide once, not per vertex.
	// (The per-vertex `i < Num()` checks this replaces also handled partially-filled arrays,
	// which no mesher produces; short arrays now fall back to defaults wholesale.)
	const bool bHasNormals = MeshData.Normals.Num() >= VertexCount;
	const bool bHasUVs = MeshData.UVs.Num() >= VertexCount;
	const bool bHasColors = MeshData.Colors.Num() >= VertexCount;
	const bool bHasBlends = MeshData.MaterialBlends.Num() >= VertexCount;

	FVector3f BoundsMin(FLT_MAX, FLT_MAX, FLT_MAX);
	FVector3f BoundsMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);

	for (int32 i = 0; i < VertexCount; ++i)
	{
		FVoxelVertex& Vertex = OutVertices[i];
		// The packed words from zero: the setters below keep the bits they do not own, and bits 28-31 of the material
		// word (vertex flags nothing reads) were left as the fresh allocation held them (G294: the same bytes each time)
		Vertex.PackedNormalAndAO = 0;
		Vertex.PackedMaterialData = 0;

		// Position + bounds (fused: one pass over the positions)
		const FVector3f& Pos = MeshData.Positions[i];
		Vertex.Position = Pos;
		BoundsMin = FVector3f::Min(BoundsMin, Pos);
		BoundsMax = FVector3f::Max(BoundsMax, Pos);

		Vertex.SetNormal(bHasNormals ? MeshData.Normals[i] : FVector3f::UpVector);
		Vertex.UV = bHasUVs ? MeshData.UVs[i] : FVector2f::ZeroVector;

		// Extract material data from vertex color
		if (bHasColors)
		{
			const FColor& Color = MeshData.Colors[i];
			Vertex.SetMaterialID(Color.R);
			Vertex.SetBiomeID(Color.G);
			Vertex.SetAO(Color.B >> 6); // Top 2 bits of blue channel
		}
		else
		{
			Vertex.SetMaterialID(0);
			Vertex.SetBiomeID(0);
			Vertex.SetAO(0);
		}
		// The soft material border's blend (VP2-O4; 0 = none). Set on every vertex: the array is uninitialized, and
		// the setters above leave these bits alone
		Vertex.SetMaterialBlend(bHasBlends ? MeshData.MaterialBlends[i] : 0);
	}

	if (VertexCount > 0)
	{
		OutLocalBounds += FVector(BoundsMin);
		OutLocalBounds += FVector(BoundsMax);
	}
}

void FChunkMeshData::PrepareRenderVertices(bool bDropConverted)
{
	ConvertToVoxelVertices(*this, RenderVertices, RenderBounds);
	if (bDropConverted)
	{
		// What only the conversion reads; UV1s no renderer reads
		Normals.Empty();
		UVs.Empty();
		UV1s.Empty();
		Colors.Empty();
		MaterialBlends.Empty();
	}
}
