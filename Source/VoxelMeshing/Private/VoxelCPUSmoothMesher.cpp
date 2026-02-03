// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCPUSmoothMesher.h"
#include "VoxelMeshing.h"
#include "MarchingCubesTables.h"

FVoxelCPUSmoothMesher::FVoxelCPUSmoothMesher()
{
}

FVoxelCPUSmoothMesher::~FVoxelCPUSmoothMesher()
{
	if (bIsInitialized)
	{
		Shutdown();
	}
}

void FVoxelCPUSmoothMesher::Initialize()
{
	if (bIsInitialized)
	{
		return;
	}

	UE_LOG(LogVoxelMeshing, Log, TEXT("CPU Smooth Mesher initialized"));
	bIsInitialized = true;
}

void FVoxelCPUSmoothMesher::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	ReleaseAllHandles();
	bIsInitialized = false;
	UE_LOG(LogVoxelMeshing, Log, TEXT("CPU Smooth Mesher shutdown"));
}

bool FVoxelCPUSmoothMesher::IsInitialized() const
{
	return bIsInitialized;
}

bool FVoxelCPUSmoothMesher::GenerateMeshCPU(
	const FVoxelMeshingRequest& Request,
	FChunkMeshData& OutMeshData)
{
	FVoxelMeshingStats Stats;
	return GenerateMeshCPU(Request, OutMeshData, Stats);
}

bool FVoxelCPUSmoothMesher::GenerateMeshCPU(
	const FVoxelMeshingRequest& Request,
	FChunkMeshData& OutMeshData,
	FVoxelMeshingStats& OutStats)
{
	if (!bIsInitialized)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("CPU Smooth Mesher not initialized"));
		return false;
	}

	if (!Request.IsValid())
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("Invalid meshing request"));
		return false;
	}

	const double StartTime = FPlatformTime::Seconds();

	// Reset output
	OutMeshData.Reset();
	OutStats = FVoxelMeshingStats();

	const int32 ChunkSize = Request.ChunkSize;

	// Calculate LOD stride - each LOD level doubles the stride
	// LOD 0 = stride 1 (full detail), LOD 1 = stride 2, LOD 2 = stride 4, etc.
	const int32 LODLevel = FMath::Clamp(Request.LODLevel, 0, 7);
	const int32 Stride = 1 << LODLevel;  // 2^LODLevel

	// Number of cubes to process at this LOD level
	const int32 LODChunkSize = ChunkSize / Stride;

	UE_LOG(LogVoxelMeshing, Log, TEXT("Smooth meshing chunk (%d,%d,%d) at LOD %d (stride %d, cubes %d^3)"),
		Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z,
		LODLevel, Stride, LODChunkSize);

	// Pre-allocate arrays (estimate based on typical terrain, scaled for LOD)
	const int32 EstimatedTriangles = LODChunkSize * LODChunkSize * 2;
	OutMeshData.Positions.Reserve(EstimatedTriangles * 3);
	OutMeshData.Normals.Reserve(EstimatedTriangles * 3);
	OutMeshData.UVs.Reserve(EstimatedTriangles * 3);
	OutMeshData.Colors.Reserve(EstimatedTriangles * 3);
	OutMeshData.Indices.Reserve(EstimatedTriangles * 3);

	uint32 TriangleCount = 0;
	uint32 SolidVoxels = 0;

	// Count solid voxels (sampled at LOD stride)
	for (int32 Z = 0; Z < ChunkSize; Z += Stride)
	{
		for (int32 Y = 0; Y < ChunkSize; Y += Stride)
		{
			for (int32 X = 0; X < ChunkSize; X += Stride)
			{
				if (!Request.GetVoxel(X, Y, Z).IsAir())
				{
					SolidVoxels++;
				}
			}
		}
	}

	// Process each cube in the chunk at LOD resolution
	// At higher LOD levels, we process fewer but larger cubes
	// Each cube at LOD level N covers a Stride x Stride x Stride region
	for (int32 Z = 0; Z < ChunkSize; Z += Stride)
	{
		for (int32 Y = 0; Y < ChunkSize; Y += Stride)
		{
			for (int32 X = 0; X < ChunkSize; X += Stride)
			{
				ProcessCubeLOD(Request, X, Y, Z, Stride, OutMeshData, TriangleCount);
			}
		}
	}

	// Calculate stats
	const double EndTime = FPlatformTime::Seconds();
	OutStats.VertexCount = OutMeshData.Positions.Num();
	OutStats.IndexCount = OutMeshData.Indices.Num();
	OutStats.FaceCount = TriangleCount;
	OutStats.SolidVoxelCount = SolidVoxels;
	OutStats.CulledFaceCount = 0;
	OutStats.GenerationTimeMs = static_cast<float>((EndTime - StartTime) * 1000.0);

	UE_LOG(LogVoxelMeshing, Verbose,
		TEXT("Smooth meshing complete: %d verts, %d tris, %.2fms"),
		OutStats.VertexCount, TriangleCount, OutStats.GenerationTimeMs);

	return true;
}

void FVoxelCPUSmoothMesher::ProcessCube(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	FChunkMeshData& OutMeshData,
	uint32& OutTriangleCount)
{
	const float VoxelSize = Request.VoxelSize;
	const float IsoLevel = Config.IsoLevel;

	// Sample density at 8 cube corners
	float CornerDensities[8];
	for (int32 i = 0; i < 8; i++)
	{
		const FIntVector& Offset = MarchingCubesTables::CornerOffsets[i];
		CornerDensities[i] = GetDensityAt(Request, X + Offset.X, Y + Offset.Y, Z + Offset.Z);
	}

	// Build cube index from corner inside/outside states
	uint8 CubeIndex = 0;
	for (int32 i = 0; i < 8; i++)
	{
		if (CornerDensities[i] >= IsoLevel)
		{
			CubeIndex |= (1 << i);
		}
	}

	// Early out if cube is entirely inside or outside
	if (MarchingCubesTables::EdgeTable[CubeIndex] == 0)
	{
		return;
	}

	// Get material and biome for this cube
	const uint8 MaterialID = GetDominantMaterial(Request, X, Y, Z, CubeIndex);
	const uint8 BiomeID = GetDominantBiome(Request, X, Y, Z, CubeIndex);

	// Calculate corner world positions
	FVector3f CornerPositions[8];
	for (int32 i = 0; i < 8; i++)
	{
		const FIntVector& Offset = MarchingCubesTables::CornerOffsets[i];
		CornerPositions[i] = FVector3f(
			static_cast<float>(X + Offset.X) * VoxelSize,
			static_cast<float>(Y + Offset.Y) * VoxelSize,
			static_cast<float>(Z + Offset.Z) * VoxelSize
		);
	}

	// Interpolate vertex positions along intersected edges
	FVector3f EdgeVertices[12];
	const uint16 EdgeMask = MarchingCubesTables::EdgeTable[CubeIndex];

	for (int32 Edge = 0; Edge < 12; Edge++)
	{
		if (EdgeMask & (1 << Edge))
		{
			const int32 V0 = MarchingCubesTables::EdgeVertexPairs[Edge][0];
			const int32 V1 = MarchingCubesTables::EdgeVertexPairs[Edge][1];

			EdgeVertices[Edge] = InterpolateEdge(
				CornerDensities[V0], CornerDensities[V1],
				CornerPositions[V0], CornerPositions[V1],
				IsoLevel
			);
		}
	}

	// Generate triangles from the lookup table
	const int8* TriIndices = MarchingCubesTables::TriTable[CubeIndex];

	for (int32 i = 0; TriIndices[i] != -1; i += 3)
	{
		const int32 Edge0 = TriIndices[i];
		const int32 Edge1 = TriIndices[i + 1];
		const int32 Edge2 = TriIndices[i + 2];

		const FVector3f& P0 = EdgeVertices[Edge0];
		const FVector3f& P1 = EdgeVertices[Edge1];
		const FVector3f& P2 = EdgeVertices[Edge2];

		// Calculate normals using gradient of density field at each vertex
		// Convert back to voxel coordinates for gradient calculation
		const FVector3f N0 = CalculateGradientNormal(Request,
			P0.X / VoxelSize, P0.Y / VoxelSize, P0.Z / VoxelSize);
		const FVector3f N1 = CalculateGradientNormal(Request,
			P1.X / VoxelSize, P1.Y / VoxelSize, P1.Z / VoxelSize);
		const FVector3f N2 = CalculateGradientNormal(Request,
			P2.X / VoxelSize, P2.Y / VoxelSize, P2.Z / VoxelSize);

		// Dominant-axis UV projection based on face normal
		// This reduces texture stretching on slopes by choosing the best projection plane
		const float UVScale = Config.bGenerateUVs ? Config.UVScale : 0.0f;

		// Calculate face normal from triangle edges
		const FVector3f FaceNormal = FVector3f::CrossProduct(P1 - P0, P2 - P0).GetSafeNormal();

		// Determine dominant axis based on face normal
		const float AbsX = FMath::Abs(FaceNormal.X);
		const float AbsY = FMath::Abs(FaceNormal.Y);
		const float AbsZ = FMath::Abs(FaceNormal.Z);

		FVector2f UV0, UV1, UV2;

		if (AbsZ >= AbsX && AbsZ >= AbsY)
		{
			// Z-dominant (horizontal surface): project onto XY plane
			UV0 = FVector2f(P0.X * UVScale / VoxelSize, P0.Y * UVScale / VoxelSize);
			UV1 = FVector2f(P1.X * UVScale / VoxelSize, P1.Y * UVScale / VoxelSize);
			UV2 = FVector2f(P2.X * UVScale / VoxelSize, P2.Y * UVScale / VoxelSize);
		}
		else if (AbsX >= AbsY)
		{
			// X-dominant (East/West facing): project onto YZ plane
			UV0 = FVector2f(P0.Y * UVScale / VoxelSize, P0.Z * UVScale / VoxelSize);
			UV1 = FVector2f(P1.Y * UVScale / VoxelSize, P1.Z * UVScale / VoxelSize);
			UV2 = FVector2f(P2.Y * UVScale / VoxelSize, P2.Z * UVScale / VoxelSize);
		}
		else
		{
			// Y-dominant (North/South facing): project onto XZ plane
			UV0 = FVector2f(P0.X * UVScale / VoxelSize, P0.Z * UVScale / VoxelSize);
			UV1 = FVector2f(P1.X * UVScale / VoxelSize, P1.Z * UVScale / VoxelSize);
			UV2 = FVector2f(P2.X * UVScale / VoxelSize, P2.Z * UVScale / VoxelSize);
		}

		// Vertex color: MaterialID, BiomeID, AO (smooth meshing doesn't compute per-vertex AO)
		const FColor VertexColor(MaterialID, BiomeID, 0, 255);

		// Get base index for this triangle
		const uint32 BaseVertex = OutMeshData.Positions.Num();

		// Add vertices
		OutMeshData.Positions.Add(P0);
		OutMeshData.Positions.Add(P1);
		OutMeshData.Positions.Add(P2);

		OutMeshData.Normals.Add(N0);
		OutMeshData.Normals.Add(N1);
		OutMeshData.Normals.Add(N2);

		OutMeshData.UVs.Add(UV0);
		OutMeshData.UVs.Add(UV1);
		OutMeshData.UVs.Add(UV2);

		OutMeshData.Colors.Add(VertexColor);
		OutMeshData.Colors.Add(VertexColor);
		OutMeshData.Colors.Add(VertexColor);

		// Add indices (already in correct winding order from table)
		OutMeshData.Indices.Add(BaseVertex + 0);
		OutMeshData.Indices.Add(BaseVertex + 1);
		OutMeshData.Indices.Add(BaseVertex + 2);

		OutTriangleCount++;
	}
}

void FVoxelCPUSmoothMesher::ProcessCubeLOD(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 Stride,
	FChunkMeshData& OutMeshData,
	uint32& OutTriangleCount)
{
	const float VoxelSize = Request.VoxelSize;
	const float IsoLevel = Config.IsoLevel;

	// At higher LOD levels, each cube covers Stride voxels
	// The effective cube size in world units is VoxelSize * Stride
	const float LODVoxelSize = VoxelSize * static_cast<float>(Stride);

	// Sample density at 8 cube corners (at strided positions)
	float CornerDensities[8];
	for (int32 i = 0; i < 8; i++)
	{
		const FIntVector& Offset = MarchingCubesTables::CornerOffsets[i];
		// Sample at strided positions
		const int32 SampleX = X + Offset.X * Stride;
		const int32 SampleY = Y + Offset.Y * Stride;
		const int32 SampleZ = Z + Offset.Z * Stride;
		CornerDensities[i] = GetDensityAt(Request, SampleX, SampleY, SampleZ);
	}

	// Build cube index from corner inside/outside states
	uint8 CubeIndex = 0;
	for (int32 i = 0; i < 8; i++)
	{
		if (CornerDensities[i] >= IsoLevel)
		{
			CubeIndex |= (1 << i);
		}
	}

	// Early out if cube is entirely inside or outside
	if (MarchingCubesTables::EdgeTable[CubeIndex] == 0)
	{
		return;
	}

	// Get material and biome for this cube (sample at base position)
	const uint8 MaterialID = GetDominantMaterialLOD(Request, X, Y, Z, Stride, CubeIndex);
	const uint8 BiomeID = GetDominantBiomeLOD(Request, X, Y, Z, Stride, CubeIndex);

	// Calculate corner world positions (in actual world units)
	FVector3f CornerPositions[8];
	for (int32 i = 0; i < 8; i++)
	{
		const FIntVector& Offset = MarchingCubesTables::CornerOffsets[i];
		// Position in world space (stride affects the spacing)
		CornerPositions[i] = FVector3f(
			static_cast<float>(X + Offset.X * Stride) * VoxelSize,
			static_cast<float>(Y + Offset.Y * Stride) * VoxelSize,
			static_cast<float>(Z + Offset.Z * Stride) * VoxelSize
		);
	}

	// Interpolate vertex positions along intersected edges
	FVector3f EdgeVertices[12];
	const uint16 EdgeMask = MarchingCubesTables::EdgeTable[CubeIndex];

	for (int32 Edge = 0; Edge < 12; Edge++)
	{
		if (EdgeMask & (1 << Edge))
		{
			const int32 V0 = MarchingCubesTables::EdgeVertexPairs[Edge][0];
			const int32 V1 = MarchingCubesTables::EdgeVertexPairs[Edge][1];

			EdgeVertices[Edge] = InterpolateEdge(
				CornerDensities[V0], CornerDensities[V1],
				CornerPositions[V0], CornerPositions[V1],
				IsoLevel
			);
		}
	}

	// Generate triangles from the lookup table
	const int8* TriIndices = MarchingCubesTables::TriTable[CubeIndex];

	for (int32 i = 0; TriIndices[i] != -1; i += 3)
	{
		const int32 Edge0 = TriIndices[i];
		const int32 Edge1 = TriIndices[i + 1];
		const int32 Edge2 = TriIndices[i + 2];

		const FVector3f& P0 = EdgeVertices[Edge0];
		const FVector3f& P1 = EdgeVertices[Edge1];
		const FVector3f& P2 = EdgeVertices[Edge2];

		// Calculate normals using gradient of density field at each vertex
		// For LOD, we sample gradients at the actual world positions
		const FVector3f N0 = CalculateGradientNormalLOD(Request,
			P0.X / VoxelSize, P0.Y / VoxelSize, P0.Z / VoxelSize, Stride);
		const FVector3f N1 = CalculateGradientNormalLOD(Request,
			P1.X / VoxelSize, P1.Y / VoxelSize, P1.Z / VoxelSize, Stride);
		const FVector3f N2 = CalculateGradientNormalLOD(Request,
			P2.X / VoxelSize, P2.Y / VoxelSize, P2.Z / VoxelSize, Stride);

		// Dominant-axis UV projection based on face normal
		const float UVScale = Config.bGenerateUVs ? Config.UVScale : 0.0f;

		// Calculate face normal from triangle edges
		const FVector3f FaceNormal = FVector3f::CrossProduct(P1 - P0, P2 - P0).GetSafeNormal();

		// Determine dominant axis based on face normal
		const float AbsX = FMath::Abs(FaceNormal.X);
		const float AbsY = FMath::Abs(FaceNormal.Y);
		const float AbsZ = FMath::Abs(FaceNormal.Z);

		FVector2f UV0, UV1, UV2;

		if (AbsZ >= AbsX && AbsZ >= AbsY)
		{
			// Z-dominant (horizontal surface): project onto XY plane
			UV0 = FVector2f(P0.X * UVScale / VoxelSize, P0.Y * UVScale / VoxelSize);
			UV1 = FVector2f(P1.X * UVScale / VoxelSize, P1.Y * UVScale / VoxelSize);
			UV2 = FVector2f(P2.X * UVScale / VoxelSize, P2.Y * UVScale / VoxelSize);
		}
		else if (AbsX >= AbsY)
		{
			// X-dominant (East/West facing): project onto YZ plane
			UV0 = FVector2f(P0.Y * UVScale / VoxelSize, P0.Z * UVScale / VoxelSize);
			UV1 = FVector2f(P1.Y * UVScale / VoxelSize, P1.Z * UVScale / VoxelSize);
			UV2 = FVector2f(P2.Y * UVScale / VoxelSize, P2.Z * UVScale / VoxelSize);
		}
		else
		{
			// Y-dominant (North/South facing): project onto XZ plane
			UV0 = FVector2f(P0.X * UVScale / VoxelSize, P0.Z * UVScale / VoxelSize);
			UV1 = FVector2f(P1.X * UVScale / VoxelSize, P1.Z * UVScale / VoxelSize);
			UV2 = FVector2f(P2.X * UVScale / VoxelSize, P2.Z * UVScale / VoxelSize);
		}

		// Vertex color: MaterialID, BiomeID, AO
		const FColor VertexColor(MaterialID, BiomeID, 0, 255);

		// Get base index for this triangle
		const uint32 BaseVertex = OutMeshData.Positions.Num();

		// Add vertices
		OutMeshData.Positions.Add(P0);
		OutMeshData.Positions.Add(P1);
		OutMeshData.Positions.Add(P2);

		OutMeshData.Normals.Add(N0);
		OutMeshData.Normals.Add(N1);
		OutMeshData.Normals.Add(N2);

		OutMeshData.UVs.Add(UV0);
		OutMeshData.UVs.Add(UV1);
		OutMeshData.UVs.Add(UV2);

		OutMeshData.Colors.Add(VertexColor);
		OutMeshData.Colors.Add(VertexColor);
		OutMeshData.Colors.Add(VertexColor);

		// Add indices
		OutMeshData.Indices.Add(BaseVertex + 0);
		OutMeshData.Indices.Add(BaseVertex + 1);
		OutMeshData.Indices.Add(BaseVertex + 2);

		OutTriangleCount++;
	}
}

float FVoxelCPUSmoothMesher::GetDensityAt(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z) const
{
	const FVoxelData Voxel = GetVoxelAt(Request, X, Y, Z);
	return static_cast<float>(Voxel.Density) / 255.0f;
}

FVoxelData FVoxelCPUSmoothMesher::GetVoxelAt(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z) const
{
	const int32 ChunkSize = Request.ChunkSize;

	// Check if within chunk bounds
	if (X >= 0 && X < ChunkSize && Y >= 0 && Y < ChunkSize && Z >= 0 && Z < ChunkSize)
	{
		return Request.GetVoxel(X, Y, Z);
	}

	// Handle neighbor chunk lookups
	const int32 SliceSize = ChunkSize * ChunkSize;

	// Clamp coordinates for fallback
	const int32 ClampedX = FMath::Clamp(X, 0, ChunkSize - 1);
	const int32 ClampedY = FMath::Clamp(Y, 0, ChunkSize - 1);
	const int32 ClampedZ = FMath::Clamp(Z, 0, ChunkSize - 1);

	// Determine which axes are out of bounds and in which direction
	const bool bXPos = (X >= ChunkSize);
	const bool bXNeg = (X < 0);
	const bool bYPos = (Y >= ChunkSize);
	const bool bYNeg = (Y < 0);
	const bool bZPos = (Z >= ChunkSize);
	const bool bZNeg = (Z < 0);

	const bool bOutX = bXPos || bXNeg;
	const bool bOutY = bYPos || bYNeg;
	const bool bOutZ = bZPos || bZNeg;
	const int32 OutCount = (bOutX ? 1 : 0) + (bOutY ? 1 : 0) + (bOutZ ? 1 : 0);

	// Single-axis out of bounds: use face neighbor data
	if (OutCount == 1)
	{
		if (bXPos && Request.NeighborXPos.Num() == SliceSize)
		{
			return Request.NeighborXPos[Y + Z * ChunkSize];
		}
		if (bXNeg && Request.NeighborXNeg.Num() == SliceSize)
		{
			return Request.NeighborXNeg[Y + Z * ChunkSize];
		}
		if (bYPos && Request.NeighborYPos.Num() == SliceSize)
		{
			return Request.NeighborYPos[X + Z * ChunkSize];
		}
		if (bYNeg && Request.NeighborYNeg.Num() == SliceSize)
		{
			return Request.NeighborYNeg[X + Z * ChunkSize];
		}
		if (bZPos && Request.NeighborZPos.Num() == SliceSize)
		{
			return Request.NeighborZPos[X + Y * ChunkSize];
		}
		if (bZNeg && Request.NeighborZNeg.Num() == SliceSize)
		{
			return Request.NeighborZNeg[X + Y * ChunkSize];
		}
		// Fallback to edge voxel
		return Request.GetVoxel(ClampedX, ClampedY, ClampedZ);
	}

	// Edge case (2 axes out of bounds): use edge neighbor data
	if (OutCount == 2)
	{
		// X+Y edge (Z varies)
		if (bXPos && bYPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_YPOS))
		{
			return Request.EdgeXPosYPos[Z];
		}
		if (bXPos && bYNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_YNEG))
		{
			return Request.EdgeXPosYNeg[Z];
		}
		if (bXNeg && bYPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_YPOS))
		{
			return Request.EdgeXNegYPos[Z];
		}
		if (bXNeg && bYNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_YNEG))
		{
			return Request.EdgeXNegYNeg[Z];
		}

		// X+Z edge (Y varies)
		if (bXPos && bZPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_ZPOS))
		{
			return Request.EdgeXPosZPos[Y];
		}
		if (bXPos && bZNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_ZNEG))
		{
			return Request.EdgeXPosZNeg[Y];
		}
		if (bXNeg && bZPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_ZPOS))
		{
			return Request.EdgeXNegZPos[Y];
		}
		if (bXNeg && bZNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_ZNEG))
		{
			return Request.EdgeXNegZNeg[Y];
		}

		// Y+Z edge (X varies)
		if (bYPos && bZPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_YPOS_ZPOS))
		{
			return Request.EdgeYPosZPos[X];
		}
		if (bYPos && bZNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_YPOS_ZNEG))
		{
			return Request.EdgeYPosZNeg[X];
		}
		if (bYNeg && bZPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_YNEG_ZPOS))
		{
			return Request.EdgeYNegZPos[X];
		}
		if (bYNeg && bZNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_YNEG_ZNEG))
		{
			return Request.EdgeYNegZNeg[X];
		}

		// Fallback to edge voxel
		return Request.GetVoxel(ClampedX, ClampedY, ClampedZ);
	}

	// Corner case (3 axes out of bounds): use corner neighbor data
	if (OutCount == 3)
	{
		if (bXPos && bYPos && bZPos && Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZPOS))
		{
			return Request.CornerXPosYPosZPos;
		}
		if (bXPos && bYPos && bZNeg && Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZNEG))
		{
			return Request.CornerXPosYPosZNeg;
		}
		if (bXPos && bYNeg && bZPos && Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZPOS))
		{
			return Request.CornerXPosYNegZPos;
		}
		if (bXPos && bYNeg && bZNeg && Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZNEG))
		{
			return Request.CornerXPosYNegZNeg;
		}
		if (bXNeg && bYPos && bZPos && Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZPOS))
		{
			return Request.CornerXNegYPosZPos;
		}
		if (bXNeg && bYPos && bZNeg && Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZNEG))
		{
			return Request.CornerXNegYPosZNeg;
		}
		if (bXNeg && bYNeg && bZPos && Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZPOS))
		{
			return Request.CornerXNegYNegZPos;
		}
		if (bXNeg && bYNeg && bZNeg && Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZNEG))
		{
			return Request.CornerXNegYNegZNeg;
		}

		// Fallback to corner voxel
		return Request.GetVoxel(ClampedX, ClampedY, ClampedZ);
	}

	// Out of bounds - treat as air
	return FVoxelData::Air();
}

FVector3f FVoxelCPUSmoothMesher::InterpolateEdge(
	float d0, float d1,
	const FVector3f& p0, const FVector3f& p1,
	float IsoLevel) const
{
	// Avoid division by zero
	if (FMath::Abs(d1 - d0) < KINDA_SMALL_NUMBER)
	{
		return (p0 + p1) * 0.5f;
	}

	// Linear interpolation factor
	float t = (IsoLevel - d0) / (d1 - d0);
	t = FMath::Clamp(t, 0.0f, 1.0f);

	return p0 + (p1 - p0) * t;
}

FVector3f FVoxelCPUSmoothMesher::CalculateGradientNormal(
	const FVoxelMeshingRequest& Request,
	float X, float Y, float Z) const
{
	// Use central difference for gradient approximation
	// Sample at half-voxel offsets for better accuracy
	const float Delta = 0.5f;

	// Round to integers for sampling (we can't truly interpolate here without trilinear,
	// so we use nearest neighbor with offsets)
	const int32 IX = FMath::FloorToInt(X);
	const int32 IY = FMath::FloorToInt(Y);
	const int32 IZ = FMath::FloorToInt(Z);

	// Central difference gradient
	float gx = GetDensityAt(Request, IX + 1, IY, IZ) - GetDensityAt(Request, IX - 1, IY, IZ);
	float gy = GetDensityAt(Request, IX, IY + 1, IZ) - GetDensityAt(Request, IX, IY - 1, IZ);
	float gz = GetDensityAt(Request, IX, IY, IZ + 1) - GetDensityAt(Request, IX, IY, IZ - 1);

	// Normal points away from solid (opposite to gradient direction)
	FVector3f Normal(-gx, -gy, -gz);

	// Normalize, with fallback for degenerate cases
	if (!Normal.Normalize())
	{
		return FVector3f(0.0f, 0.0f, 1.0f);
	}

	return Normal;
}

uint8 FVoxelCPUSmoothMesher::GetDominantMaterial(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	uint8 CubeIndex) const
{
	// Count materials from solid corners
	TMap<uint8, int32> MaterialCounts;

	for (int32 i = 0; i < 8; i++)
	{
		// Check if this corner is inside (solid)
		if (CubeIndex & (1 << i))
		{
			const FIntVector& Offset = MarchingCubesTables::CornerOffsets[i];
			const FVoxelData Voxel = GetVoxelAt(Request, X + Offset.X, Y + Offset.Y, Z + Offset.Z);
			MaterialCounts.FindOrAdd(Voxel.MaterialID)++;
		}
	}

	// Find the most common material
	uint8 DominantMaterial = 0;
	int32 MaxCount = 0;

	for (const auto& Pair : MaterialCounts)
	{
		if (Pair.Value > MaxCount)
		{
			MaxCount = Pair.Value;
			DominantMaterial = Pair.Key;
		}
	}

	return DominantMaterial;
}

uint8 FVoxelCPUSmoothMesher::GetDominantBiome(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	uint8 CubeIndex) const
{
	// Count biomes from solid corners
	TMap<uint8, int32> BiomeCounts;

	for (int32 i = 0; i < 8; i++)
	{
		// Check if this corner is inside (solid)
		if (CubeIndex & (1 << i))
		{
			const FIntVector& Offset = MarchingCubesTables::CornerOffsets[i];
			const FVoxelData Voxel = GetVoxelAt(Request, X + Offset.X, Y + Offset.Y, Z + Offset.Z);
			BiomeCounts.FindOrAdd(Voxel.BiomeID)++;
		}
	}

	// Find the most common biome
	uint8 DominantBiome = 0;
	int32 MaxCount = 0;

	for (const auto& Pair : BiomeCounts)
	{
		if (Pair.Value > MaxCount)
		{
			MaxCount = Pair.Value;
			DominantBiome = Pair.Key;
		}
	}

	return DominantBiome;
}

// ============================================================================
// LOD Helper Functions
// ============================================================================

FVector3f FVoxelCPUSmoothMesher::CalculateGradientNormalLOD(
	const FVoxelMeshingRequest& Request,
	float X, float Y, float Z,
	int32 Stride) const
{
	// Use stride-scaled central difference for gradient approximation
	// This gives smoother normals at higher LOD levels
	const float Delta = static_cast<float>(Stride);

	const int32 IX = FMath::FloorToInt(X);
	const int32 IY = FMath::FloorToInt(Y);
	const int32 IZ = FMath::FloorToInt(Z);

	// Central difference gradient with LOD-scaled sampling
	float gx = GetDensityAt(Request, IX + Stride, IY, IZ) - GetDensityAt(Request, IX - Stride, IY, IZ);
	float gy = GetDensityAt(Request, IX, IY + Stride, IZ) - GetDensityAt(Request, IX, IY - Stride, IZ);
	float gz = GetDensityAt(Request, IX, IY, IZ + Stride) - GetDensityAt(Request, IX, IY, IZ - Stride);

	// Normal points away from solid (opposite to gradient direction)
	FVector3f Normal(-gx, -gy, -gz);

	if (!Normal.Normalize())
	{
		return FVector3f(0.0f, 0.0f, 1.0f);
	}

	return Normal;
}

uint8 FVoxelCPUSmoothMesher::GetDominantMaterialLOD(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 Stride,
	uint8 CubeIndex) const
{
	// Count materials from solid corners at strided positions
	TMap<uint8, int32> MaterialCounts;

	for (int32 i = 0; i < 8; i++)
	{
		if (CubeIndex & (1 << i))
		{
			const FIntVector& Offset = MarchingCubesTables::CornerOffsets[i];
			const int32 SampleX = X + Offset.X * Stride;
			const int32 SampleY = Y + Offset.Y * Stride;
			const int32 SampleZ = Z + Offset.Z * Stride;
			const FVoxelData Voxel = GetVoxelAt(Request, SampleX, SampleY, SampleZ);
			MaterialCounts.FindOrAdd(Voxel.MaterialID)++;
		}
	}

	uint8 DominantMaterial = 0;
	int32 MaxCount = 0;

	for (const auto& Pair : MaterialCounts)
	{
		if (Pair.Value > MaxCount)
		{
			MaxCount = Pair.Value;
			DominantMaterial = Pair.Key;
		}
	}

	return DominantMaterial;
}

uint8 FVoxelCPUSmoothMesher::GetDominantBiomeLOD(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 Stride,
	uint8 CubeIndex) const
{
	// Count biomes from solid corners at strided positions
	TMap<uint8, int32> BiomeCounts;

	for (int32 i = 0; i < 8; i++)
	{
		if (CubeIndex & (1 << i))
		{
			const FIntVector& Offset = MarchingCubesTables::CornerOffsets[i];
			const int32 SampleX = X + Offset.X * Stride;
			const int32 SampleY = Y + Offset.Y * Stride;
			const int32 SampleZ = Z + Offset.Z * Stride;
			const FVoxelData Voxel = GetVoxelAt(Request, SampleX, SampleY, SampleZ);
			BiomeCounts.FindOrAdd(Voxel.BiomeID)++;
		}
	}

	uint8 DominantBiome = 0;
	int32 MaxCount = 0;

	for (const auto& Pair : BiomeCounts)
	{
		if (Pair.Value > MaxCount)
		{
			MaxCount = Pair.Value;
			DominantBiome = Pair.Key;
		}
	}

	return DominantBiome;
}

// ============================================================================
// Async Pattern (wraps sync for CPU mesher)
// ============================================================================

FVoxelMeshingHandle FVoxelCPUSmoothMesher::GenerateMeshAsync(
	const FVoxelMeshingRequest& Request,
	FOnVoxelMeshingComplete OnComplete)
{
	const uint64 RequestId = NextRequestId.fetch_add(1);
	FVoxelMeshingHandle Handle(RequestId, Request.ChunkCoord);

	// CPU mesher runs synchronously
	FCachedResult Result;
	Result.bSuccess = GenerateMeshCPU(Request, Result.MeshData, Result.Stats);

	// Cache the result
	{
		FScopeLock Lock(&CacheLock);
		CachedResults.Add(RequestId, MoveTemp(Result));
	}

	Handle.bIsComplete = true;
	Handle.bWasSuccessful = Result.bSuccess;

	// Call completion delegate
	if (OnComplete.IsBound())
	{
		OnComplete.Execute(Handle, Handle.bWasSuccessful);
	}

	return Handle;
}

bool FVoxelCPUSmoothMesher::IsComplete(const FVoxelMeshingHandle& Handle) const
{
	return Handle.bIsComplete;
}

bool FVoxelCPUSmoothMesher::WasSuccessful(const FVoxelMeshingHandle& Handle) const
{
	return Handle.bWasSuccessful;
}

FRHIBuffer* FVoxelCPUSmoothMesher::GetVertexBuffer(const FVoxelMeshingHandle& Handle)
{
	// CPU mesher doesn't create GPU buffers
	return nullptr;
}

FRHIBuffer* FVoxelCPUSmoothMesher::GetIndexBuffer(const FVoxelMeshingHandle& Handle)
{
	// CPU mesher doesn't create GPU buffers
	return nullptr;
}

bool FVoxelCPUSmoothMesher::GetBufferCounts(
	const FVoxelMeshingHandle& Handle,
	uint32& OutVertexCount,
	uint32& OutIndexCount) const
{
	FScopeLock Lock(&CacheLock);
	const FCachedResult* Result = CachedResults.Find(Handle.RequestId);
	if (Result && Result->bSuccess)
	{
		OutVertexCount = Result->MeshData.GetVertexCount();
		OutIndexCount = Result->MeshData.Indices.Num();
		return true;
	}
	return false;
}

bool FVoxelCPUSmoothMesher::GetRenderData(
	const FVoxelMeshingHandle& Handle,
	FChunkRenderData& OutRenderData)
{
	FScopeLock Lock(&CacheLock);
	const FCachedResult* Result = CachedResults.Find(Handle.RequestId);
	if (Result && Result->bSuccess)
	{
		OutRenderData.ChunkCoord = Handle.ChunkCoord;
		OutRenderData.VertexCount = Result->MeshData.GetVertexCount();
		OutRenderData.IndexCount = Result->MeshData.Indices.Num();
		return true;
	}
	return false;
}

bool FVoxelCPUSmoothMesher::ReadbackToCPU(
	const FVoxelMeshingHandle& Handle,
	FChunkMeshData& OutMeshData)
{
	FScopeLock Lock(&CacheLock);
	const FCachedResult* Result = CachedResults.Find(Handle.RequestId);
	if (Result && Result->bSuccess)
	{
		OutMeshData = Result->MeshData;
		return true;
	}
	return false;
}

void FVoxelCPUSmoothMesher::ReleaseHandle(const FVoxelMeshingHandle& Handle)
{
	FScopeLock Lock(&CacheLock);
	CachedResults.Remove(Handle.RequestId);
}

void FVoxelCPUSmoothMesher::ReleaseAllHandles()
{
	FScopeLock Lock(&CacheLock);
	CachedResults.Empty();
}

void FVoxelCPUSmoothMesher::SetConfig(const FVoxelMeshingConfig& InConfig)
{
	Config = InConfig;
}

const FVoxelMeshingConfig& FVoxelCPUSmoothMesher::GetConfig() const
{
	return Config;
}

bool FVoxelCPUSmoothMesher::GetStats(
	const FVoxelMeshingHandle& Handle,
	FVoxelMeshingStats& OutStats) const
{
	FScopeLock Lock(&CacheLock);
	const FCachedResult* Result = CachedResults.Find(Handle.RequestId);
	if (Result)
	{
		OutStats = Result->Stats;
		return true;
	}
	return false;
}
