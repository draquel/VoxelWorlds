// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWaterMesher.h"
#include "VoxelData.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelWaterMesher, Log, All);

void FVoxelWaterMesher::GenerateWaterMesh(const FVoxelMeshingRequest& Request, FChunkMeshData& OutMeshData)
{
	OutMeshData.Reset();

	if (!Request.IsValid())
	{
		return;
	}

	const int32 ChunkSize = Request.ChunkSize;
	const int32 SliceSize = ChunkSize * ChunkSize;

	// Allocate mask and processed arrays (reused per Z-slice)
	TArray<bool> SurfaceMask;
	TArray<bool> Processed;
	SurfaceMask.SetNumZeroed(SliceSize);
	Processed.SetNumZeroed(SliceSize);

	// Pre-allocate reasonable capacity (water surfaces are often large flat areas)
	OutMeshData.Positions.Reserve(SliceSize);
	OutMeshData.Normals.Reserve(SliceSize);
	OutMeshData.UVs.Reserve(SliceSize);
	OutMeshData.UV1s.Reserve(SliceSize);
	OutMeshData.Colors.Reserve(SliceSize);
	OutMeshData.Indices.Reserve(SliceSize * 6 / 4); // 6 indices per 4 vertices

	// Process each Z-slice looking for water surface voxels
	for (int32 Z = 0; Z < ChunkSize; Z++)
	{
		// Build surface mask for this Z-slice
		bool bHasAnySurface = false;
		for (int32 Y = 0; Y < ChunkSize; Y++)
		{
			for (int32 X = 0; X < ChunkSize; X++)
			{
				const int32 Index = X + Y * ChunkSize;
				const bool bIsSurface = IsWaterSurface(Request, X, Y, Z);
				SurfaceMask[Index] = bIsSurface;
				if (bIsSurface)
				{
					bHasAnySurface = true;
				}
			}
		}

		if (!bHasAnySurface)
		{
			continue;
		}

		// Reset processed array for this slice
		FMemory::Memzero(Processed.GetData(), SliceSize * sizeof(bool));

		// Greedy rectangle merging (same algorithm as cubic mesher)
		for (int32 Y = 0; Y < ChunkSize; Y++)
		{
			for (int32 X = 0; X < ChunkSize; X++)
			{
				const int32 Index = X + Y * ChunkSize;

				if (Processed[Index] || !SurfaceMask[Index])
				{
					continue;
				}

				// Find maximum width (extend along X axis)
				int32 Width = 1;
				while (X + Width < ChunkSize)
				{
					const int32 NextIndex = (X + Width) + Y * ChunkSize;
					if (Processed[NextIndex] || !SurfaceMask[NextIndex])
					{
						break;
					}
					Width++;
				}

				// Find maximum height (extend along Y axis) for this width
				int32 Height = 1;
				bool bCanExtend = true;
				while (bCanExtend && Y + Height < ChunkSize)
				{
					for (int32 DX = 0; DX < Width; DX++)
					{
						const int32 CheckIndex = (X + DX) + (Y + Height) * ChunkSize;
						if (Processed[CheckIndex] || !SurfaceMask[CheckIndex])
						{
							bCanExtend = false;
							break;
						}
					}
					if (bCanExtend)
					{
						Height++;
					}
				}

				// Mark all cells in this rectangle as processed
				for (int32 DY = 0; DY < Height; DY++)
				{
					for (int32 DX = 0; DX < Width; DX++)
					{
						const int32 MarkIndex = (X + DX) + (Y + DY) * ChunkSize;
						Processed[MarkIndex] = true;
					}
				}

				// Emit merged water quad
				EmitWaterQuad(OutMeshData, Request, Z, X, Y, Width, Height);
			}
		}
	}

	if (OutMeshData.IsValid())
	{
		UE_LOG(LogVoxelWaterMesher, Verbose, TEXT("Chunk (%d,%d,%d): Water mesh generated — %d verts, %d tris"),
			Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z,
			OutMeshData.GetVertexCount(), OutMeshData.GetTriangleCount());
	}
}

bool FVoxelWaterMesher::IsWaterSurface(const FVoxelMeshingRequest& Request, int32 X, int32 Y, int32 Z)
{
	const FVoxelData& Voxel = Request.GetVoxel(X, Y, Z);

	// Must be an air voxel with the water flag set
	if (!Voxel.IsAir() || !Voxel.HasWaterFlag())
	{
		return false;
	}

	// Check the voxel above (Z+1)
	FVoxelData AboveVoxel;
	if (!GetVoxelAbove(Request, X, Y, Z, AboveVoxel))
	{
		// Out of bounds with no neighbor data — treat as surface (top of chunk)
		return true;
	}

	// Surface if above is solid OR above doesn't have water flag
	return AboveVoxel.IsSolid() || !AboveVoxel.HasWaterFlag();
}

bool FVoxelWaterMesher::GetVoxelAbove(const FVoxelMeshingRequest& Request, int32 X, int32 Y, int32 Z, FVoxelData& OutVoxel)
{
	const int32 ChunkSize = Request.ChunkSize;
	const int32 AboveZ = Z + 1;

	if (AboveZ < ChunkSize)
	{
		// Within chunk bounds
		OutVoxel = Request.GetVoxel(X, Y, AboveZ);
		return true;
	}

	// Z+1 is out of bounds — check +Z neighbor slice
	if (Request.NeighborZPos.Num() == Request.GetNeighborSliceSize())
	{
		// Neighbor slice is indexed [X + Y * ChunkSize] for the Z=0 face of the +Z neighbor
		const int32 NeighborIndex = X + Y * ChunkSize;
		OutVoxel = Request.NeighborZPos[NeighborIndex];
		return true;
	}

	// No neighbor data available
	return false;
}

void FVoxelWaterMesher::EmitWaterQuad(
	FChunkMeshData& MeshData,
	const FVoxelMeshingRequest& Request,
	int32 SliceZ,
	int32 U, int32 V,
	int32 Width, int32 Height)
{
	const float VoxelSize = Request.VoxelSize;
	const uint32 BaseVertex = MeshData.Positions.Num();

	// Water surface is at the TOP of the water voxel (Z+1 face)
	const float SurfaceZ = static_cast<float>(SliceZ + 1) * VoxelSize;

	// Calculate the 4 corners of the quad in local chunk space
	// +Z face winding: CCW when viewed from above
	const FVector3f Corner0(static_cast<float>(U) * VoxelSize, static_cast<float>(V) * VoxelSize, SurfaceZ);
	const FVector3f Corner1(static_cast<float>(U + Width) * VoxelSize, static_cast<float>(V) * VoxelSize, SurfaceZ);
	const FVector3f Corner2(static_cast<float>(U + Width) * VoxelSize, static_cast<float>(V + Height) * VoxelSize, SurfaceZ);
	const FVector3f Corner3(static_cast<float>(U) * VoxelSize, static_cast<float>(V + Height) * VoxelSize, SurfaceZ);

	// Normal: straight up for water surface
	const FVector3f Normal(0.0f, 0.0f, 1.0f);

	// World-space UVs for seamless cross-chunk tiling
	// Chunk world position offset
	const FVector ChunkWorldPos = Request.GetChunkWorldPosition();
	const float WorldX0 = static_cast<float>(ChunkWorldPos.X) + static_cast<float>(U) * VoxelSize;
	const float WorldY0 = static_cast<float>(ChunkWorldPos.Y) + static_cast<float>(V) * VoxelSize;

	// UV scale: 1 unit per voxel for consistent tiling
	const float UVScale = 1.0f / VoxelSize;

	const FVector2f UV0(WorldX0 * UVScale, WorldY0 * UVScale);
	const FVector2f UV1((WorldX0 + Width * VoxelSize) * UVScale, WorldY0 * UVScale);
	const FVector2f UV2((WorldX0 + Width * VoxelSize) * UVScale, (WorldY0 + Height * VoxelSize) * UVScale);
	const FVector2f UV3(WorldX0 * UVScale, (WorldY0 + Height * VoxelSize) * UVScale);

	// UV1 channel: MaterialID (254 = water) and FaceType (0 = top)
	const FVector2f MaterialUV(static_cast<float>(WATER_MATERIAL_ID), 0.0f);

	// Vertex color: water uses MaterialID=254, BiomeID=0, AO=0
	const FColor WaterColor(WATER_MATERIAL_ID, 0, 0, 255);

	// Emit 4 vertices
	MeshData.Positions.Add(Corner0);
	MeshData.Positions.Add(Corner1);
	MeshData.Positions.Add(Corner2);
	MeshData.Positions.Add(Corner3);

	MeshData.Normals.Add(Normal);
	MeshData.Normals.Add(Normal);
	MeshData.Normals.Add(Normal);
	MeshData.Normals.Add(Normal);

	MeshData.UVs.Add(UV0);
	MeshData.UVs.Add(UV1);
	MeshData.UVs.Add(UV2);
	MeshData.UVs.Add(UV3);

	MeshData.UV1s.Add(MaterialUV);
	MeshData.UV1s.Add(MaterialUV);
	MeshData.UV1s.Add(MaterialUV);
	MeshData.UV1s.Add(MaterialUV);

	MeshData.Colors.Add(WaterColor);
	MeshData.Colors.Add(WaterColor);
	MeshData.Colors.Add(WaterColor);
	MeshData.Colors.Add(WaterColor);

	// Emit 6 indices (2 triangles, CCW winding when viewed from above +Z)
	// Water corners are: 0=(minX,minY) 1=(maxX,minY) 2=(maxX,maxY) 3=(minX,maxY)
	// Cubic mesher +Z face uses reversed Y corners, so we use 0-1-2, 0-2-3 here
	MeshData.Indices.Add(BaseVertex + 0);
	MeshData.Indices.Add(BaseVertex + 1);
	MeshData.Indices.Add(BaseVertex + 2);
	MeshData.Indices.Add(BaseVertex + 0);
	MeshData.Indices.Add(BaseVertex + 2);
	MeshData.Indices.Add(BaseVertex + 3);
}
