// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWaterMesher.h"
#include "VoxelData.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelWaterMesher, Log, All);

bool FVoxelWaterMesher::BuildColumnMask(
	const TArray<FVoxelData>& VoxelData,
	int32 ChunkSize,
	TArray<bool>& OutMask)
{
	const int32 SliceSize = ChunkSize * ChunkSize;

	if (VoxelData.Num() < SliceSize * ChunkSize || OutMask.Num() < SliceSize)
	{
		return false;
	}

	bool bHasAnyWater = false;

	for (int32 Y = 0; Y < ChunkSize; Y++)
	{
		for (int32 X = 0; X < ChunkSize; X++)
		{
			for (int32 Z = 0; Z < ChunkSize; Z++)
			{
				const int32 Index = X + Y * ChunkSize + Z * SliceSize;
				const FVoxelData& Voxel = VoxelData[Index];
				if (Voxel.IsAir() && Voxel.HasWaterFlag() && !Voxel.HasUndergroundFlag())
				{
					OutMask[X + Y * ChunkSize] = true;
					bHasAnyWater = true;
					break;
				}
			}
		}
	}

	return bHasAnyWater;
}

void FVoxelWaterMesher::GenerateWaterMeshFromMask(
	const TArray<bool>& ColumnMask,
	int32 ChunkSize,
	float VoxelSize,
	const FVector& TileWorldPosition,
	float WaterLevel,
	FChunkMeshData& OutMeshData)
{
	OutMeshData.Reset();

	const int32 SliceSize = ChunkSize * ChunkSize;

	if (ColumnMask.Num() < SliceSize)
	{
		return;
	}

	// Check if any water exists in the mask
	bool bHasAnyWater = false;
	for (int32 i = 0; i < SliceSize; i++)
	{
		if (ColumnMask[i])
		{
			bHasAnyWater = true;
			break;
		}
	}

	if (!bHasAnyWater)
	{
		return;
	}

	// Dilate the column mask by 3 cells in each direction.
	// This extends water quads 3 voxels past the actual water boundary so they tuck
	// under the terrain mesh at shorelines. Three cells are needed because terrain
	// surface transitions can be up to 2 voxels inward from the actual water boundary,
	// plus 1 cell for marching cubes surface transition.
	// Dilation happens BEFORE greedy merge so the resulting quads are guaranteed
	// non-overlapping — no double-blending artifacts with translucent materials.
	constexpr int32 DilationRadius = 3;
	TArray<bool> FinalMask;
	FinalMask.SetNumZeroed(SliceSize);

	for (int32 Y = 0; Y < ChunkSize; Y++)
	{
		for (int32 X = 0; X < ChunkSize; X++)
		{
			if (!ColumnMask[X + Y * ChunkSize])
			{
				continue;
			}

			// Stamp a square of radius DilationRadius around this cell
			const int32 MinDX = FMath::Max(0, X - DilationRadius);
			const int32 MaxDX = FMath::Min(ChunkSize - 1, X + DilationRadius);
			const int32 MinDY = FMath::Max(0, Y - DilationRadius);
			const int32 MaxDY = FMath::Min(ChunkSize - 1, Y + DilationRadius);

			for (int32 DY = MinDY; DY <= MaxDY; DY++)
			{
				for (int32 DX = MinDX; DX <= MaxDX; DX++)
				{
					FinalMask[DX + DY * ChunkSize] = true;
				}
			}
		}
	}

	// Pre-allocate reasonable capacity
	OutMeshData.Positions.Reserve(SliceSize);
	OutMeshData.Normals.Reserve(SliceSize);
	OutMeshData.UVs.Reserve(SliceSize);
	OutMeshData.UV1s.Reserve(SliceSize);
	OutMeshData.Colors.Reserve(SliceSize);
	OutMeshData.Indices.Reserve(SliceSize * 6 / 4);

	// Compute local water Z relative to tile origin
	const float LocalWaterZ = static_cast<float>(WaterLevel - TileWorldPosition.Z);

	// Greedy rectangle merging on the dilated mask
	TArray<bool> Processed;
	Processed.SetNumZeroed(SliceSize);

	for (int32 Y = 0; Y < ChunkSize; Y++)
	{
		for (int32 X = 0; X < ChunkSize; X++)
		{
			const int32 Index = X + Y * ChunkSize;

			if (Processed[Index] || !FinalMask[Index])
			{
				continue;
			}

			// Find maximum width (extend along X axis)
			int32 Width = 1;
			while (X + Width < ChunkSize)
			{
				const int32 NextIndex = (X + Width) + Y * ChunkSize;
				if (Processed[NextIndex] || !FinalMask[NextIndex])
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
					if (Processed[CheckIndex] || !FinalMask[CheckIndex])
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
					Processed[(X + DX) + (Y + DY) * ChunkSize] = true;
				}
			}

			// Emit merged water quad at the actual water level
			EmitWaterQuad(OutMeshData, VoxelSize, TileWorldPosition, LocalWaterZ, X, Y, Width, Height);
		}
	}

	if (OutMeshData.IsValid())
	{
		UE_LOG(LogVoxelWaterMesher, Verbose, TEXT("Water tile at (%.0f, %.0f): mesh generated — %d verts, %d tris"),
			TileWorldPosition.X, TileWorldPosition.Y,
			OutMeshData.GetVertexCount(), OutMeshData.GetTriangleCount());
	}
}

void FVoxelWaterMesher::GenerateWaterMesh(const FVoxelMeshingRequest& Request, FChunkMeshData& OutMeshData, float WaterLevel)
{
	OutMeshData.Reset();

	if (!Request.IsValid())
	{
		return;
	}

	const int32 ChunkSize = Request.ChunkSize;
	const int32 SliceSize = ChunkSize * ChunkSize;

	// Build column mask from the request's voxel data
	TArray<bool> ColumnMask;
	ColumnMask.SetNumZeroed(SliceSize);
	bool bHasAnyWater = false;

	for (int32 Y = 0; Y < ChunkSize; Y++)
	{
		for (int32 X = 0; X < ChunkSize; X++)
		{
			for (int32 Z = 0; Z < ChunkSize; Z++)
			{
				const FVoxelData& Voxel = Request.GetVoxel(X, Y, Z);
				if (Voxel.IsAir() && Voxel.HasWaterFlag() && !Voxel.HasUndergroundFlag())
				{
					ColumnMask[X + Y * ChunkSize] = true;
					bHasAnyWater = true;
					break;
				}
			}
		}
	}

	if (!bHasAnyWater)
	{
		return;
	}

	// Delegate to GenerateWaterMeshFromMask for dilation + greedy merge
	const FVector ChunkWorldPos = Request.GetChunkWorldPosition();
	GenerateWaterMeshFromMask(ColumnMask, ChunkSize, Request.VoxelSize, ChunkWorldPos, WaterLevel, OutMeshData);

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

	// Must be air with water flag, and NOT underground (cave interior)
	if (!Voxel.IsAir() || !Voxel.HasWaterFlag() || Voxel.HasUndergroundFlag())
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
	float VoxelSize,
	const FVector& TileWorldPos,
	float LocalWaterZ,
	int32 U, int32 V,
	int32 Width, int32 Height)
{
	const uint32 BaseVertex = MeshData.Positions.Num();

	// Water surface sits at the actual water level, not a voxel boundary
	const float SurfaceZ = LocalWaterZ;

	float MinX = static_cast<float>(U) * VoxelSize;
	float MaxX = static_cast<float>(U + Width) * VoxelSize;
	float MinY = static_cast<float>(V) * VoxelSize;
	float MaxY = static_cast<float>(V + Height) * VoxelSize;

	// Calculate the 4 corners of the quad in local space
	const FVector3f Corner0(MinX, MinY, SurfaceZ);
	const FVector3f Corner1(MaxX, MinY, SurfaceZ);
	const FVector3f Corner2(MaxX, MaxY, SurfaceZ);
	const FVector3f Corner3(MinX, MaxY, SurfaceZ);

	// Normal: straight up for water surface
	const FVector3f Normal(0.0f, 0.0f, 1.0f);

	// World-space UVs for seamless cross-chunk tiling
	const float WorldX0 = static_cast<float>(TileWorldPos.X) + static_cast<float>(U) * VoxelSize;
	const float WorldY0 = static_cast<float>(TileWorldPos.Y) + static_cast<float>(V) * VoxelSize;

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

	// Emit 6 indices (2 triangles, CW winding for Unreal's left-handed coordinate system)
	// Water corners are: 0=(minX,minY) 1=(maxX,minY) 2=(maxX,maxY) 3=(minX,maxY)
	MeshData.Indices.Add(BaseVertex + 0);
	MeshData.Indices.Add(BaseVertex + 2);
	MeshData.Indices.Add(BaseVertex + 1);
	MeshData.Indices.Add(BaseVertex + 0);
	MeshData.Indices.Add(BaseVertex + 3);
	MeshData.Indices.Add(BaseVertex + 2);
}
