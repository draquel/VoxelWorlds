// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelSurfaceExtractor.h"
#include "ChunkRenderData.h"
#include "VoxelScatter.h"

void FVoxelSurfaceExtractor::ExtractSurfacePoints(
	const FChunkMeshData& MeshData,
	const FIntVector& ChunkCoord,
	const FVector& ChunkWorldOrigin,
	float TargetPointSpacing,
	int32 LODLevel,
	FChunkSurfaceData& OutSurfaceData)
{
	ExtractSurfacePointsFiltered(
		MeshData,
		ChunkCoord,
		ChunkWorldOrigin,
		TargetPointSpacing,
		LODLevel,
		false, // Don't filter by face type
		OutSurfaceData);
}

void FVoxelSurfaceExtractor::ExtractSurfacePointsFiltered(
	const FChunkMeshData& MeshData,
	const FIntVector& ChunkCoord,
	const FVector& ChunkWorldOrigin,
	float TargetPointSpacing,
	int32 LODLevel,
	bool bTopFacesOnly,
	FChunkSurfaceData& OutSurfaceData)
{
	// Initialize output
	OutSurfaceData = FChunkSurfaceData(ChunkCoord);
	OutSurfaceData.LODLevel = LODLevel;
	OutSurfaceData.AveragePointSpacing = TargetPointSpacing;

	// Validate input
	if (!MeshData.IsValid())
	{
		OutSurfaceData.bIsValid = false;
		return;
	}

	const int32 VertexCount = MeshData.GetVertexCount();
	const bool bHasUV1 = MeshData.UV1s.Num() == VertexCount;
	const bool bHasColors = MeshData.Colors.Num() == VertexCount;
	const bool bHasNormals = MeshData.Normals.Num() == VertexCount;

	if (!bHasNormals)
	{
		UE_LOG(LogVoxelScatter, Warning, TEXT("ExtractSurfacePoints: Mesh data missing normals"));
		OutSurfaceData.bIsValid = false;
		return;
	}

	// Use spatial hashing to deduplicate points within grid cells
	// Only keep one point per cell to ensure even distribution
	TMap<FIntVector, int32> OccupiedCells; // Cell -> Index in output array

	// Reserve approximate capacity
	const float CellSize = TargetPointSpacing;
	const int32 ExpectedPoints = FMath::Max(1, VertexCount / 4); // Rough estimate after dedup
	OutSurfaceData.SurfacePoints.Reserve(ExpectedPoints);
	OccupiedCells.Reserve(ExpectedPoints);

	// Process each vertex
	for (int32 VertIndex = 0; VertIndex < VertexCount; ++VertIndex)
	{
		// Get world position
		const FVector LocalPos(MeshData.Positions[VertIndex]);
		const FVector WorldPos = ChunkWorldOrigin + LocalPos;

		// Check if this cell is already occupied
		const FIntVector Cell = GetGridCell(WorldPos, CellSize);
		if (OccupiedCells.Contains(Cell))
		{
			continue; // Skip - already have a point in this cell
		}

		// Get normal
		const FVector Normal(MeshData.Normals[VertIndex]);

		// Decode UV1 data (material, face type)
		uint8 MaterialID = 0;
		EVoxelFaceType FaceType = EVoxelFaceType::Top;
		if (bHasUV1)
		{
			DecodeUV1Data(MeshData.UV1s[VertIndex], MaterialID, FaceType);
		}

		// Filter by face type if requested
		if (bTopFacesOnly && FaceType != EVoxelFaceType::Top)
		{
			continue;
		}

		// Decode color data (biome, AO)
		uint8 BiomeID = 0;
		uint8 AO = 0;
		if (bHasColors)
		{
			DecodeColorData(MeshData.Colors[VertIndex], BiomeID, AO);
		}

		// Create surface point
		FVoxelSurfacePoint Point;
		Point.Position = WorldPos;
		Point.Normal = FVector(Normal).GetSafeNormal();
		Point.MaterialID = MaterialID;
		Point.BiomeID = BiomeID;
		Point.FaceType = FaceType;
		Point.AmbientOcclusion = AO;

		// Add to output and mark cell as occupied
		const int32 NewIndex = OutSurfaceData.SurfacePoints.Add(Point);
		OccupiedCells.Add(Cell, NewIndex);
	}

	// Calculate surface area estimate
	// Each point represents approximately CellSize^2 area
	const int32 PointCount = OutSurfaceData.SurfacePoints.Num();
	OutSurfaceData.SurfaceAreaEstimate = PointCount * CellSize * CellSize;
	OutSurfaceData.bIsValid = true;

	UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): Extracted %d surface points from %d vertices (spacing=%.1f)"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z,
		PointCount, VertexCount, TargetPointSpacing);
}

void FVoxelSurfaceExtractor::DecodeUV1Data(const FVector2f& UV1, uint8& OutMaterialID, EVoxelFaceType& OutFaceType)
{
	// UV1.x = MaterialID as float
	// UV1.y = FaceType as float (0=Top, 1=Side, 2=Bottom)
	OutMaterialID = static_cast<uint8>(FMath::RoundToInt(UV1.X));

	const int32 FaceTypeInt = FMath::RoundToInt(UV1.Y);
	switch (FaceTypeInt)
	{
	case 0:
		OutFaceType = EVoxelFaceType::Top;
		break;
	case 1:
		OutFaceType = EVoxelFaceType::Side;
		break;
	case 2:
		OutFaceType = EVoxelFaceType::Bottom;
		break;
	default:
		OutFaceType = EVoxelFaceType::Top;
		break;
	}
}

void FVoxelSurfaceExtractor::DecodeColorData(const FColor& Color, uint8& OutBiomeID, uint8& OutAO)
{
	// Color layout: R=MaterialID (legacy), G=BiomeID, B=AO (2 bits in lower bits)
	OutBiomeID = Color.G;
	OutAO = Color.B & 0x03; // Lower 2 bits
}

uint32 FVoxelSurfaceExtractor::ComputeSpatialHash(const FVector& Position, float CellSize)
{
	const FIntVector Cell = GetGridCell(Position, CellSize);
	// FNV-1a hash
	uint32 Hash = 2166136261u;
	Hash ^= static_cast<uint32>(Cell.X);
	Hash *= 16777619u;
	Hash ^= static_cast<uint32>(Cell.Y);
	Hash *= 16777619u;
	Hash ^= static_cast<uint32>(Cell.Z);
	Hash *= 16777619u;
	return Hash;
}

FIntVector FVoxelSurfaceExtractor::GetGridCell(const FVector& Position, float CellSize)
{
	return FIntVector(
		FMath::FloorToInt(Position.X / CellSize),
		FMath::FloorToInt(Position.Y / CellSize),
		FMath::FloorToInt(Position.Z / CellSize)
	);
}
