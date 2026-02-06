// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCPUSmoothMesher.h"
#include "VoxelMeshing.h"
#include "MarchingCubesTables.h"
#include "TransvoxelTables.h"

// Note: Smooth meshing uses triplanar blending, so FaceType is not needed.
// UV1.x stores MaterialID, UV1.y is reserved (set to 0).

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

	// Don't clear debug data automatically - let it accumulate across chunks
	// so visualization can show data from multiple chunks. The caller should
	// call ClearDebugData() when they want to reset the collection.
	// Limit the size to prevent unbounded growth.
	if (bCollectDebugVisualization && TransitionCellDebugData.Num() > 10000)
	{
		// Keep only the most recent entries
		TransitionCellDebugData.RemoveAt(0, TransitionCellDebugData.Num() - 5000);
	}

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
	OutMeshData.UV1s.Reserve(EstimatedTriangles * 3);
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

	// Get transition face mask for Transvoxel
	const uint8 TransitionMask = Config.bUseTransvoxel ? GetTransitionFaces(Request) : 0;
	const bool bHasTransitions = TransitionMask != 0;

	// Debug logging for transition cell processing
	if (bDebugLogTransitionCells)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("=== MESH GENERATION START ==="));
		UE_LOG(LogVoxelMeshing, Warning, TEXT("  Chunk: (%d,%d,%d) LOD: %d, Stride: %d"),
			Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z,
			Request.LODLevel, Stride);
		UE_LOG(LogVoxelMeshing, Warning, TEXT("  Transvoxel: %s, TransitionMask: 0x%02X, HasTransitions: %s"),
			Config.bUseTransvoxel ? TEXT("ON") : TEXT("OFF"),
			TransitionMask,
			bHasTransitions ? TEXT("Yes") : TEXT("No"));
		UE_LOG(LogVoxelMeshing, Warning, TEXT("  Neighbor LODs: [-X:%d +X:%d -Y:%d +Y:%d -Z:%d +Z:%d]"),
			Request.NeighborLODLevels[0], Request.NeighborLODLevels[1],
			Request.NeighborLODLevels[2], Request.NeighborLODLevels[3],
			Request.NeighborLODLevels[4], Request.NeighborLODLevels[5]);
		UE_LOG(LogVoxelMeshing, Warning, TEXT("  Debug flags: Logging=%s, Visualization=%s"),
			bDebugLogTransitionCells ? TEXT("ON") : TEXT("OFF"),
			bCollectDebugVisualization ? TEXT("ON") : TEXT("OFF"));
	}

	if (bHasTransitions)
	{
		UE_LOG(LogVoxelMeshing, Verbose, TEXT("Chunk has transition faces: 0x%02X"), TransitionMask);
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
				if (Config.bUseTransvoxel)
				{
					// Check if this cell needs transition geometry (Transvoxel)
					int32 TransitionFaceIndex = -1;
					if (bHasTransitions && IsTransitionCell(X, Y, Z, ChunkSize, Stride, TransitionMask, TransitionFaceIndex))
					{
						// Get the transition cell stride (should match neighbor's stride for alignment)
						const int32 TransitionStride = GetTransitionCellStride(Request, TransitionFaceIndex, Stride);

						// When transition stride > current stride, we need to:
						// 1. Only process transition cells at positions aligned to the transition stride
						// 2. Skip positions that fall within a larger transition cell
						//
						// For example, at LOD 0 (Stride=1) with LOD 1 neighbor (TransitionStride=2):
						// - Process transition cell at X=0 (covers X=[0,2))
						// - Skip X=1 (it's inside the X=0 transition cell)

						bool bShouldProcessTransition = true;

						// For transition cells, we need to determine the correct base position.
						// For negative faces (-X, -Y, -Z), the cell is at 0 in the face-normal direction.
						// For positive faces (+X, +Y, +Z), the cell must be at ChunkSize - TransitionStride
						// so that face samples (at offset 1.0) land exactly at the chunk boundary.
						int32 TransitionX = X;
						int32 TransitionY = Y;
						int32 TransitionZ = Z;

						if (TransitionStride > Stride)
						{
							// Check alignment and calculate correct base position
							switch (TransitionFaceIndex)
							{
							case 0: // -X: cell at X=0, must be aligned in Y and Z
								bShouldProcessTransition = (Y % TransitionStride == 0) && (Z % TransitionStride == 0);
								TransitionY = (Y / TransitionStride) * TransitionStride;
								TransitionZ = (Z / TransitionStride) * TransitionStride;
								break;
							case 1: // +X: cell at ChunkSize-TransitionStride, aligned in Y and Z
								bShouldProcessTransition = (Y % TransitionStride == 0) && (Z % TransitionStride == 0);
								TransitionX = ChunkSize - TransitionStride;
								TransitionY = (Y / TransitionStride) * TransitionStride;
								TransitionZ = (Z / TransitionStride) * TransitionStride;
								break;
							case 2: // -Y: cell at Y=0, must be aligned in X and Z
								bShouldProcessTransition = (X % TransitionStride == 0) && (Z % TransitionStride == 0);
								TransitionX = (X / TransitionStride) * TransitionStride;
								TransitionZ = (Z / TransitionStride) * TransitionStride;
								break;
							case 3: // +Y: cell at ChunkSize-TransitionStride, aligned in X and Z
								bShouldProcessTransition = (X % TransitionStride == 0) && (Z % TransitionStride == 0);
								TransitionX = (X / TransitionStride) * TransitionStride;
								TransitionY = ChunkSize - TransitionStride;
								TransitionZ = (Z / TransitionStride) * TransitionStride;
								break;
							case 4: // -Z: cell at Z=0, must be aligned in X and Y
								bShouldProcessTransition = (X % TransitionStride == 0) && (Y % TransitionStride == 0);
								TransitionX = (X / TransitionStride) * TransitionStride;
								TransitionY = (Y / TransitionStride) * TransitionStride;
								break;
							case 5: // +Z: cell at ChunkSize-TransitionStride, aligned in X and Y
								bShouldProcessTransition = (X % TransitionStride == 0) && (Y % TransitionStride == 0);
								TransitionX = (X / TransitionStride) * TransitionStride;
								TransitionY = (Y / TransitionStride) * TransitionStride;
								TransitionZ = ChunkSize - TransitionStride;
								break;
							}
						}

						if (bShouldProcessTransition)
						{
							// Process as transition cell for seamless LOD connection
							// The face-level check in VoxelChunkManager ensures edge data is available.
							// Per-cell check is kept as safety for corner cases, but we still attempt
							// transition cells even if corner data is missing - the non-crossing edge
							// handling will minimize artifacts.
							if (!HasRequiredNeighborData(Request, TransitionX, TransitionY, TransitionZ, TransitionStride, TransitionFaceIndex))
							{
								if (bDebugLogTransitionCells)
								{
									UE_LOG(LogVoxelMeshing, Verbose, TEXT("Transition cell at (%d,%d,%d) face %d: some corner data may be missing"),
										TransitionX, TransitionY, TransitionZ, TransitionFaceIndex);
								}
							}
							ProcessTransitionCell(Request, TransitionX, TransitionY, TransitionZ, TransitionStride, TransitionFaceIndex, OutMeshData, TriangleCount);
						}
						// else: This cell is covered by an earlier aligned transition cell, skip it
					}
					else
					{
						// Check if this cell falls within a transition cell's footprint.
						// If so, skip it - the transition cell will cover this region.
						if (!bHasTransitions || !IsInTransitionRegion(Request, X, Y, Z, ChunkSize, Stride, TransitionMask))
						{
							// Regular Marching Cubes cell
							ProcessCubeLOD(Request, X, Y, Z, Stride, OutMeshData, TriangleCount);
						}
					}
				}
				else
				{
					// Regular Marching Cubes - no transition handling
					ProcessCubeLOD(Request, X, Y, Z, Stride, OutMeshData, TriangleCount);
				}
			}
		}
	}

	// Generate skirts as fallback when Transvoxel is disabled
	if (!Config.bUseTransvoxel && Config.bGenerateSkirts)
	{
		GenerateSkirts(Request, Stride, OutMeshData, TriangleCount);
	}

	// Log debug summary for transition cells
	if (bDebugLogTransitionCells)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("=== MESH GENERATION COMPLETE ==="));
		UE_LOG(LogVoxelMeshing, Warning, TEXT("  Chunk (%d,%d,%d) LOD %d: %d transition cells in debug data"),
			Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z,
			Request.LODLevel, TransitionCellDebugData.Num());

		if (TransitionCellDebugData.Num() > 0)
		{
			// Count by face
			int32 FaceCounts[6] = {0, 0, 0, 0, 0, 0};
			for (const auto& Cell : TransitionCellDebugData)
			{
				if (Cell.FaceIndex >= 0 && Cell.FaceIndex < 6)
				{
					FaceCounts[Cell.FaceIndex]++;
				}
			}
			UE_LOG(LogVoxelMeshing, Warning, TEXT("  By face: -X:%d +X:%d -Y:%d +Y:%d -Z:%d +Z:%d"),
				FaceCounts[0], FaceCounts[1], FaceCounts[2], FaceCounts[3], FaceCounts[4], FaceCounts[5]);
		}
		else
		{
			UE_LOG(LogVoxelMeshing, Warning, TEXT("  No transition cells collected (TransitionMask was 0x%02X, bCollectViz=%s)"),
				TransitionMask, bCollectDebugVisualization ? TEXT("ON") : TEXT("OFF"));
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

		// Vertex color: MaterialID (legacy), BiomeID, AO (smooth meshing doesn't compute per-vertex AO)
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

		// UV1: MaterialID only (smooth meshing uses triplanar, no FaceType needed)
		const FVector2f MaterialUV(static_cast<float>(MaterialID), 0.0f);
		OutMeshData.UV1s.Add(MaterialUV);
		OutMeshData.UV1s.Add(MaterialUV);
		OutMeshData.UV1s.Add(MaterialUV);

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

		// Vertex color: MaterialID (legacy), BiomeID, AO
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

		// UV1: MaterialID only (smooth meshing uses triplanar, no FaceType needed)
		const FVector2f MaterialUV(static_cast<float>(MaterialID), 0.0f);
		OutMeshData.UV1s.Add(MaterialUV);
		OutMeshData.UV1s.Add(MaterialUV);
		OutMeshData.UV1s.Add(MaterialUV);

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

float FVoxelCPUSmoothMesher::GetDensityAtTrilinear(
	const FVoxelMeshingRequest& Request,
	float X, float Y, float Z) const
{
	// Trilinear interpolation for fractional voxel positions.
	// This is essential for Transvoxel mid-point samples that fall between voxels.

	// Get integer coordinates and fractional parts
	const int32 X0 = FMath::FloorToInt(X);
	const int32 Y0 = FMath::FloorToInt(Y);
	const int32 Z0 = FMath::FloorToInt(Z);

	const float FracX = X - static_cast<float>(X0);
	const float FracY = Y - static_cast<float>(Y0);
	const float FracZ = Z - static_cast<float>(Z0);

	// If on integer coordinates, skip interpolation
	if (FracX < KINDA_SMALL_NUMBER && FracY < KINDA_SMALL_NUMBER && FracZ < KINDA_SMALL_NUMBER)
	{
		return GetDensityAt(Request, X0, Y0, Z0);
	}

	// Sample the 8 corners of the cell containing this point
	const float D000 = GetDensityAt(Request, X0,     Y0,     Z0);
	const float D100 = GetDensityAt(Request, X0 + 1, Y0,     Z0);
	const float D010 = GetDensityAt(Request, X0,     Y0 + 1, Z0);
	const float D110 = GetDensityAt(Request, X0 + 1, Y0 + 1, Z0);
	const float D001 = GetDensityAt(Request, X0,     Y0,     Z0 + 1);
	const float D101 = GetDensityAt(Request, X0 + 1, Y0,     Z0 + 1);
	const float D011 = GetDensityAt(Request, X0,     Y0 + 1, Z0 + 1);
	const float D111 = GetDensityAt(Request, X0 + 1, Y0 + 1, Z0 + 1);

	// Trilinear interpolation
	// First interpolate along X
	const float D00 = FMath::Lerp(D000, D100, FracX);
	const float D10 = FMath::Lerp(D010, D110, FracX);
	const float D01 = FMath::Lerp(D001, D101, FracX);
	const float D11 = FMath::Lerp(D011, D111, FracX);

	// Then along Y
	const float D0 = FMath::Lerp(D00, D10, FracY);
	const float D1 = FMath::Lerp(D01, D11, FracY);

	// Finally along Z
	return FMath::Lerp(D0, D1, FracZ);
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
	// Select material from the solid corner closest to the isosurface (density nearest 0.5).
	// This ensures consistent surface material selection across all LOD levels.

	constexpr uint8 IsosurfaceThreshold = 128; // 0.5 in uint8 density
	uint8 SurfaceMaterial = 0;
	int32 ClosestDistance = INT32_MAX;

	for (int32 i = 0; i < 8; i++)
	{
		// Check if this corner is inside (solid)
		if (CubeIndex & (1 << i))
		{
			const FIntVector& Offset = MarchingCubesTables::CornerOffsets[i];
			const FVoxelData Voxel = GetVoxelAt(Request, X + Offset.X, Y + Offset.Y, Z + Offset.Z);

			// Calculate distance from isosurface (how close density is to 0.5)
			const int32 DistanceFromSurface = FMath::Abs(static_cast<int32>(Voxel.Density) - IsosurfaceThreshold);

			if (DistanceFromSurface < ClosestDistance)
			{
				ClosestDistance = DistanceFromSurface;
				SurfaceMaterial = Voxel.MaterialID;
			}
		}
	}

	return SurfaceMaterial;
}

uint8 FVoxelCPUSmoothMesher::GetDominantBiome(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	uint8 CubeIndex) const
{
	// Select biome from the solid corner closest to the isosurface (density nearest 0.5).
	// Consistent with GetDominantMaterial for uniform surface selection.

	constexpr uint8 IsosurfaceThreshold = 128; // 0.5 in uint8 density
	uint8 SurfaceBiome = 0;
	int32 ClosestDistance = INT32_MAX;

	for (int32 i = 0; i < 8; i++)
	{
		// Check if this corner is inside (solid)
		if (CubeIndex & (1 << i))
		{
			const FIntVector& Offset = MarchingCubesTables::CornerOffsets[i];
			const FVoxelData Voxel = GetVoxelAt(Request, X + Offset.X, Y + Offset.Y, Z + Offset.Z);

			// Calculate distance from isosurface (how close density is to 0.5)
			const int32 DistanceFromSurface = FMath::Abs(static_cast<int32>(Voxel.Density) - IsosurfaceThreshold);

			if (DistanceFromSurface < ClosestDistance)
			{
				ClosestDistance = DistanceFromSurface;
				SurfaceBiome = Voxel.BiomeID;
			}
		}
	}

	return SurfaceBiome;
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
	// For LOD > 0, find the surface material by scanning upward from solid corners.
	// On slopes, the surface is at different Z levels across the cube, so we need
	// to find the actual surface transition (solid→air) for each corner and use
	// the material from just below that transition.
	//
	// Strategy: For each solid strided corner, scan upward to find where density
	// drops below threshold (the surface), then use the last solid voxel's material.

	constexpr int32 MaxScanDistance = 8; // Don't scan too far up
	uint8 SurfaceMaterial = 0;
	int32 HighestSurfaceZ = INT32_MIN;

	for (int32 i = 0; i < 8; i++)
	{
		if (CubeIndex & (1 << i))
		{
			const FIntVector& Offset = MarchingCubesTables::CornerOffsets[i];
			const int32 CornerX = X + Offset.X * Stride;
			const int32 CornerY = Y + Offset.Y * Stride;
			const int32 CornerZ = Z + Offset.Z * Stride;

			// Scan upward from this corner to find the surface
			uint8 LastSolidMaterial = 0;
			int32 SurfaceZ = CornerZ;

			for (int32 dz = 0; dz <= MaxScanDistance; dz++)
			{
				const FVoxelData Voxel = GetVoxelAt(Request, CornerX, CornerY, CornerZ + dz);

				if (Voxel.IsSolid())
				{
					LastSolidMaterial = Voxel.MaterialID;
					SurfaceZ = CornerZ + dz;
				}
				else
				{
					// Found air - the surface is at the previous solid voxel
					break;
				}
			}

			// Use the material from the highest surface found (prefer grass over dirt)
			if (SurfaceZ > HighestSurfaceZ)
			{
				HighestSurfaceZ = SurfaceZ;
				SurfaceMaterial = LastSolidMaterial;
			}
		}
	}

	return SurfaceMaterial;
}

uint8 FVoxelCPUSmoothMesher::GetDominantBiomeLOD(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 Stride,
	uint8 CubeIndex) const
{
	// For LOD > 0, find the surface biome by scanning upward from solid corners.
	// Consistent with GetDominantMaterialLOD approach.

	constexpr int32 MaxScanDistance = 8;
	uint8 SurfaceBiome = 0;
	int32 HighestSurfaceZ = INT32_MIN;

	for (int32 i = 0; i < 8; i++)
	{
		if (CubeIndex & (1 << i))
		{
			const FIntVector& Offset = MarchingCubesTables::CornerOffsets[i];
			const int32 CornerX = X + Offset.X * Stride;
			const int32 CornerY = Y + Offset.Y * Stride;
			const int32 CornerZ = Z + Offset.Z * Stride;

			// Scan upward from this corner to find the surface
			uint8 LastSolidBiome = 0;
			int32 SurfaceZ = CornerZ;

			for (int32 dz = 0; dz <= MaxScanDistance; dz++)
			{
				const FVoxelData Voxel = GetVoxelAt(Request, CornerX, CornerY, CornerZ + dz);

				if (Voxel.IsSolid())
				{
					LastSolidBiome = Voxel.BiomeID;
					SurfaceZ = CornerZ + dz;
				}
				else
				{
					break;
				}
			}

			// Use the biome from the highest surface found
			if (SurfaceZ > HighestSurfaceZ)
			{
				HighestSurfaceZ = SurfaceZ;
				SurfaceBiome = LastSolidBiome;
			}
		}
	}

	return SurfaceBiome;
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

// ============================================================================
// Skirt Generation (LOD Seam Hiding)
// ============================================================================

void FVoxelCPUSmoothMesher::GenerateSkirts(
	const FVoxelMeshingRequest& Request,
	int32 Stride,
	FChunkMeshData& OutMeshData,
	uint32& OutTriangleCount)
{
	const uint32 TrisBefore = OutTriangleCount;
	const int32 ChunkSize = Request.ChunkSize;
	const float VoxelSize = Request.VoxelSize;
	const float SkirtDepth = Config.SkirtDepth * VoxelSize * Stride;
	const float ChunkWorldSize = static_cast<float>(ChunkSize) * VoxelSize;

	// Tolerance for boundary detection - use larger tolerance to catch interpolated vertices
	const float BoundaryTolerance = VoxelSize * Stride * 0.6f;

	// LOD seams occur because adjacent chunks at different LOD levels have mismatched
	// vertex positions along their shared boundary. Skirts extend boundary vertices
	// downward to create vertical strips that hide gaps.

	const int32 OriginalVertexCount = OutMeshData.Positions.Num();
	const int32 OriginalIndexCount = OutMeshData.Indices.Num();

	// Only generate skirts on faces that have LOD transitions
	const uint8 TransitionMask = Request.TransitionFaces;

	// For each face, collect boundary edges
	// Face 0: -X (X near 0), Face 1: +X (X near ChunkWorldSize)
	// Face 2: -Y (Y near 0), Face 3: +Y (Y near ChunkWorldSize)

	for (int32 Face = 0; Face < 4; Face++)
	{
		// Check if this face has a LOD transition
		static const uint8 FaceTransitionFlags[4] = {
			FVoxelMeshingRequest::TRANSITION_XNEG,  // Face 0: -X
			FVoxelMeshingRequest::TRANSITION_XPOS,  // Face 1: +X
			FVoxelMeshingRequest::TRANSITION_YNEG,  // Face 2: -Y
			FVoxelMeshingRequest::TRANSITION_YPOS,  // Face 3: +Y
		};

		// Only generate skirts where there's an actual LOD transition
		if ((TransitionMask & FaceTransitionFlags[Face]) == 0)
		{
			continue;
		}

		// Collect edges on this boundary
		struct FBoundaryEdge
		{
			int32 V0, V1;  // Original vertex indices
		};
		TArray<FBoundaryEdge> BoundaryEdges;

		// Determine boundary parameters
		const bool bIsXFace = (Face == 0 || Face == 1);
		const bool bIsPositiveFace = (Face == 1 || Face == 3);
		const float BoundaryValue = bIsPositiveFace ? ChunkWorldSize : 0.0f;

		// Direction to extend skirt (DOWNWARD for vertical seam coverage)
		// Skirts drop straight down to cover vertical gaps between LOD levels
		const FVector3f SkirtDir = FVector3f(0.0f, 0.0f, -1.0f);

		// Find edges that lie on this boundary (only from original mesh)
		const int32 NumTriangles = OriginalIndexCount / 3;
		for (int32 TriIdx = 0; TriIdx < NumTriangles; TriIdx++)
		{
			const int32 BaseIdx = TriIdx * 3;
			const int32 Idx0 = OutMeshData.Indices[BaseIdx];
			const int32 Idx1 = OutMeshData.Indices[BaseIdx + 1];
			const int32 Idx2 = OutMeshData.Indices[BaseIdx + 2];

			// Skip if any index is out of range
			if (Idx0 >= OriginalVertexCount || Idx1 >= OriginalVertexCount || Idx2 >= OriginalVertexCount)
			{
				continue;
			}

			const FVector3f& P0 = OutMeshData.Positions[Idx0];
			const FVector3f& P1 = OutMeshData.Positions[Idx1];
			const FVector3f& P2 = OutMeshData.Positions[Idx2];

			// Check which vertices are on the boundary
			auto IsOnBoundary = [&](const FVector3f& P) -> bool
			{
				const float Coord = bIsXFace ? P.X : P.Y;
				return FMath::Abs(Coord - BoundaryValue) < BoundaryTolerance;
			};

			const bool b0 = IsOnBoundary(P0);
			const bool b1 = IsOnBoundary(P1);
			const bool b2 = IsOnBoundary(P2);

			// Add edges where both vertices are on the boundary
			auto AddEdgeIfOnBoundary = [&](int32 IdxA, int32 IdxB, bool bA, bool bB)
			{
				if (bA && bB)
				{
					FBoundaryEdge Edge;
					Edge.V0 = IdxA;
					Edge.V1 = IdxB;
					BoundaryEdges.Add(Edge);
				}
			};

			AddEdgeIfOnBoundary(Idx0, Idx1, b0, b1);
			AddEdgeIfOnBoundary(Idx1, Idx2, b1, b2);
			AddEdgeIfOnBoundary(Idx2, Idx0, b2, b0);
		}

		const float UVScale = Config.bGenerateUVs ? Config.UVScale : 0.0f;

		// Generate skirt geometry for each boundary edge
		for (const FBoundaryEdge& Edge : BoundaryEdges)
		{
			// IMPORTANT: Make copies, not references. Adding to arrays can cause reallocation.
			const FVector3f P0 = OutMeshData.Positions[Edge.V0];
			const FVector3f P1 = OutMeshData.Positions[Edge.V1];
			const FVector3f N0 = OutMeshData.Normals[Edge.V0];
			const FVector3f N1 = OutMeshData.Normals[Edge.V1];
			const FColor C0 = OutMeshData.Colors[Edge.V0];
			const FColor C1 = OutMeshData.Colors[Edge.V1];
			const FVector2f MatUV0 = OutMeshData.UV1s.IsValidIndex(Edge.V0) ? OutMeshData.UV1s[Edge.V0] : FVector2f::ZeroVector;
			const FVector2f MatUV1 = OutMeshData.UV1s.IsValidIndex(Edge.V1) ? OutMeshData.UV1s[Edge.V1] : FVector2f::ZeroVector;

			// Create skirt vertices that extend straight down
			// This creates a vertical curtain that hides gaps between LOD levels
			const FVector3f Bottom0 = P0 + SkirtDir * SkirtDepth;
			const FVector3f Bottom1 = P1 + SkirtDir * SkirtDepth;

			// Skirt normal faces outward from the boundary (perpendicular to the face)
			FVector3f SkirtNormal;
			if (bIsXFace)
			{
				SkirtNormal = bIsPositiveFace ? FVector3f(1, 0, 0) : FVector3f(-1, 0, 0);
			}
			else
			{
				SkirtNormal = bIsPositiveFace ? FVector3f(0, 1, 0) : FVector3f(0, -1, 0);
			}

			// UV coordinates based on world position
			auto CalcUV = [&](const FVector3f& Pos) -> FVector2f
			{
				if (bIsXFace)
				{
					return FVector2f(Pos.Y * UVScale / VoxelSize, Pos.Z * UVScale / VoxelSize);
				}
				else
				{
					return FVector2f(Pos.X * UVScale / VoxelSize, Pos.Z * UVScale / VoxelSize);
				}
			};

			const FVector2f UV0 = CalcUV(P0);
			const FVector2f UV1 = CalcUV(P1);
			const FVector2f UVBottom0 = CalcUV(Bottom0);
			const FVector2f UVBottom1 = CalcUV(Bottom1);

			// Add 4 vertices for the skirt quad
			const uint32 BaseVertex = OutMeshData.Positions.Num();

			// Vertices: 0=P0 (top), 1=Bottom0, 2=P1 (top), 3=Bottom1
			OutMeshData.Positions.Add(P0);
			OutMeshData.Positions.Add(Bottom0);
			OutMeshData.Positions.Add(P1);
			OutMeshData.Positions.Add(Bottom1);

			// All skirt vertices use the outward-facing normal
			OutMeshData.Normals.Add(SkirtNormal);
			OutMeshData.Normals.Add(SkirtNormal);
			OutMeshData.Normals.Add(SkirtNormal);
			OutMeshData.Normals.Add(SkirtNormal);

			OutMeshData.UVs.Add(UV0);
			OutMeshData.UVs.Add(UVBottom0);
			OutMeshData.UVs.Add(UV1);
			OutMeshData.UVs.Add(UVBottom1);

			// UV1: MaterialID only (smooth meshing uses triplanar, no FaceType needed)
			OutMeshData.UV1s.Add(FVector2f(MatUV0.X, 0.0f));
			OutMeshData.UV1s.Add(FVector2f(MatUV0.X, 0.0f));
			OutMeshData.UV1s.Add(FVector2f(MatUV1.X, 0.0f));
			OutMeshData.UV1s.Add(FVector2f(MatUV1.X, 0.0f));

			OutMeshData.Colors.Add(C0);
			OutMeshData.Colors.Add(C0);
			OutMeshData.Colors.Add(C1);
			OutMeshData.Colors.Add(C1);

			// Generate triangles with correct winding based on face direction
			// Skirt is a vertical quad: P0-P1 at top, Bottom0-Bottom1 at bottom
			// For positive faces (+X, +Y), normal faces outward
			if (bIsPositiveFace)
			{
				// Triangle 1: P0 -> Bottom0 -> P1
				OutMeshData.Indices.Add(BaseVertex + 0);
				OutMeshData.Indices.Add(BaseVertex + 1);
				OutMeshData.Indices.Add(BaseVertex + 2);

				// Triangle 2: P1 -> Bottom0 -> Bottom1
				OutMeshData.Indices.Add(BaseVertex + 2);
				OutMeshData.Indices.Add(BaseVertex + 1);
				OutMeshData.Indices.Add(BaseVertex + 3);
			}
			else
			{
				// Reverse winding for negative faces
				// Triangle 1: P0 -> P1 -> Bottom0
				OutMeshData.Indices.Add(BaseVertex + 0);
				OutMeshData.Indices.Add(BaseVertex + 2);
				OutMeshData.Indices.Add(BaseVertex + 1);

				// Triangle 2: P1 -> Bottom1 -> Bottom0
				OutMeshData.Indices.Add(BaseVertex + 2);
				OutMeshData.Indices.Add(BaseVertex + 3);
				OutMeshData.Indices.Add(BaseVertex + 1);
			}

			OutTriangleCount += 2;
		}
	}

	const uint32 SkirtTris = OutTriangleCount - TrisBefore;
	if (SkirtTris > 0)
	{
		UE_LOG(LogVoxelMeshing, Log, TEXT("Generated %d skirt triangles for chunk (%d,%d,%d) at LOD %d"),
			SkirtTris, Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z, Request.LODLevel);
	}
}

// ============================================================================
// Transvoxel Implementation
// ============================================================================

uint8 FVoxelCPUSmoothMesher::GetTransitionFaces(const FVoxelMeshingRequest& Request) const
{
	// The TransitionFaces field is set by the chunk manager based on neighbor LOD levels.
	// A face needs transition cells if the neighbor chunk is at a lower LOD (higher LOD level number).
	return Request.TransitionFaces;
}

bool FVoxelCPUSmoothMesher::IsTransitionCell(
	int32 X, int32 Y, int32 Z,
	int32 ChunkSize, int32 Stride,
	uint8 TransitionMask,
	int32& OutFaceIndex) const
{
	// A cell is a transition cell if it's on the edge of the chunk
	// and that edge borders a lower-LOD neighbor.
	// We only need transition cells on the last row of cells on each face.

	// Check each face
	// -X face (X == 0)
	if ((TransitionMask & TransitionXNeg) && X == 0)
	{
		OutFaceIndex = 0;
		return true;
	}
	// +X face (X == ChunkSize - Stride)
	if ((TransitionMask & TransitionXPos) && X == ChunkSize - Stride)
	{
		OutFaceIndex = 1;
		return true;
	}
	// -Y face (Y == 0)
	if ((TransitionMask & TransitionYNeg) && Y == 0)
	{
		OutFaceIndex = 2;
		return true;
	}
	// +Y face (Y == ChunkSize - Stride)
	if ((TransitionMask & TransitionYPos) && Y == ChunkSize - Stride)
	{
		OutFaceIndex = 3;
		return true;
	}
	// -Z face (Z == 0)
	if ((TransitionMask & TransitionZNeg) && Z == 0)
	{
		OutFaceIndex = 4;
		return true;
	}
	// +Z face (Z == ChunkSize - Stride)
	if ((TransitionMask & TransitionZPos) && Z == ChunkSize - Stride)
	{
		OutFaceIndex = 5;
		return true;
	}

	OutFaceIndex = -1;
	return false;
}

int32 FVoxelCPUSmoothMesher::GetTransitionCellStride(
	const FVoxelMeshingRequest& Request,
	int32 FaceIndex,
	int32 CurrentStride) const
{
	// Get the neighbor's LOD level for this face
	const int32 NeighborLOD = Request.NeighborLODLevels[FaceIndex];

	if (NeighborLOD < 0)
	{
		// No neighbor, use current stride
		return CurrentStride;
	}

	// Calculate neighbor's stride: 2^LODLevel
	const int32 NeighborStride = 1 << NeighborLOD;

	// The transition cell should use the LARGER stride (coarser neighbor's stride)
	// to ensure the transition cell spans the same area as one of the neighbor's cells.
	// This guarantees the corner vertices will align with the neighbor's grid.
	return FMath::Max(CurrentStride, NeighborStride);
}

bool FVoxelCPUSmoothMesher::IsInTransitionRegion(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 ChunkSize, int32 CurrentStride,
	uint8 TransitionMask) const
{
	// Check each face for transition regions.
	// A cell is in a transition region if:
	// 1. That face has a transition (bit set in TransitionMask)
	// 2. The cell is within TransitionStride of the boundary
	// 3. The cell is NOT the boundary row itself (that's handled by IsTransitionCell)

	// For negative faces, transition region is [0, TransitionStride)
	// For positive faces, transition region is [ChunkSize - TransitionStride, ChunkSize - CurrentStride)

	// -X face
	if (TransitionMask & TransitionXNeg)
	{
		const int32 TransitionStride = GetTransitionCellStride(Request, 0, CurrentStride);
		if (TransitionStride > CurrentStride && X >= 0 && X < TransitionStride && X != 0)
		{
			return true; // In -X transition region but not the boundary row
		}
	}

	// +X face
	if (TransitionMask & TransitionXPos)
	{
		const int32 TransitionStride = GetTransitionCellStride(Request, 1, CurrentStride);
		if (TransitionStride > CurrentStride && X >= ChunkSize - TransitionStride && X < ChunkSize - CurrentStride)
		{
			return true; // In +X transition region but not the boundary row
		}
	}

	// -Y face
	if (TransitionMask & TransitionYNeg)
	{
		const int32 TransitionStride = GetTransitionCellStride(Request, 2, CurrentStride);
		if (TransitionStride > CurrentStride && Y >= 0 && Y < TransitionStride && Y != 0)
		{
			return true;
		}
	}

	// +Y face
	if (TransitionMask & TransitionYPos)
	{
		const int32 TransitionStride = GetTransitionCellStride(Request, 3, CurrentStride);
		if (TransitionStride > CurrentStride && Y >= ChunkSize - TransitionStride && Y < ChunkSize - CurrentStride)
		{
			return true;
		}
	}

	// -Z face
	if (TransitionMask & TransitionZNeg)
	{
		const int32 TransitionStride = GetTransitionCellStride(Request, 4, CurrentStride);
		if (TransitionStride > CurrentStride && Z >= 0 && Z < TransitionStride && Z != 0)
		{
			return true;
		}
	}

	// +Z face
	if (TransitionMask & TransitionZPos)
	{
		const int32 TransitionStride = GetTransitionCellStride(Request, 5, CurrentStride);
		if (TransitionStride > CurrentStride && Z >= ChunkSize - TransitionStride && Z < ChunkSize - CurrentStride)
		{
			return true;
		}
	}

	return false;
}

bool FVoxelCPUSmoothMesher::HasRequiredNeighborData(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 Stride,
	int32 FaceIndex) const
{
	// Check if all sample positions for this transition cell have valid neighbor data.
	// Transition cells at chunk edges/corners may require edge/corner neighbor data,
	// not just face neighbor data.

	const int32 ChunkSize = Request.ChunkSize;
	const int32 SliceSize = ChunkSize * ChunkSize;

	// Check all 13 sample positions
	for (int32 i = 0; i < 13; i++)
	{
		const FVector3f& Offset = TransvoxelTables::TransitionCellSampleOffsets[FaceIndex][i];

		// Calculate sample position (need to check integer bounds for trilinear corners too)
		const float SampleX = static_cast<float>(X) + Offset.X * static_cast<float>(Stride);
		const float SampleY = static_cast<float>(Y) + Offset.Y * static_cast<float>(Stride);
		const float SampleZ = static_cast<float>(Z) + Offset.Z * static_cast<float>(Stride);

		// Check all 8 corners that trilinear interpolation would sample
		// (or just the integer position if it's exact)
		const int32 X0 = FMath::FloorToInt(SampleX);
		const int32 Y0 = FMath::FloorToInt(SampleY);
		const int32 Z0 = FMath::FloorToInt(SampleZ);

		// Check both the floor and ceil positions for trilinear interpolation
		for (int32 dx = 0; dx <= 1; dx++)
		{
			for (int32 dy = 0; dy <= 1; dy++)
			{
				for (int32 dz = 0; dz <= 1; dz++)
				{
					const int32 CheckX = X0 + dx;
					const int32 CheckY = Y0 + dy;
					const int32 CheckZ = Z0 + dz;

					// Skip if within chunk bounds
					if (CheckX >= 0 && CheckX < ChunkSize &&
						CheckY >= 0 && CheckY < ChunkSize &&
						CheckZ >= 0 && CheckZ < ChunkSize)
					{
						continue;
					}

					// Determine what neighbor data is required
					const bool bXPos = (CheckX >= ChunkSize);
					const bool bXNeg = (CheckX < 0);
					const bool bYPos = (CheckY >= ChunkSize);
					const bool bYNeg = (CheckY < 0);
					const bool bZPos = (CheckZ >= ChunkSize);
					const bool bZNeg = (CheckZ < 0);

					const int32 OutCount = (bXPos || bXNeg ? 1 : 0) +
					                       (bYPos || bYNeg ? 1 : 0) +
					                       (bZPos || bZNeg ? 1 : 0);

					// Check face neighbors
					if (OutCount == 1)
					{
						if (bXPos && Request.NeighborXPos.Num() != SliceSize) return false;
						if (bXNeg && Request.NeighborXNeg.Num() != SliceSize) return false;
						if (bYPos && Request.NeighborYPos.Num() != SliceSize) return false;
						if (bYNeg && Request.NeighborYNeg.Num() != SliceSize) return false;
						if (bZPos && Request.NeighborZPos.Num() != SliceSize) return false;
						if (bZNeg && Request.NeighborZNeg.Num() != SliceSize) return false;
					}
					// Check edge neighbors
					else if (OutCount == 2)
					{
						if (bXPos && bYPos && !Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_YPOS)) return false;
						if (bXPos && bYNeg && !Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_YNEG)) return false;
						if (bXNeg && bYPos && !Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_YPOS)) return false;
						if (bXNeg && bYNeg && !Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_YNEG)) return false;
						if (bXPos && bZPos && !Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_ZPOS)) return false;
						if (bXPos && bZNeg && !Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_ZNEG)) return false;
						if (bXNeg && bZPos && !Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_ZPOS)) return false;
						if (bXNeg && bZNeg && !Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_ZNEG)) return false;
						if (bYPos && bZPos && !Request.HasEdge(FVoxelMeshingRequest::EDGE_YPOS_ZPOS)) return false;
						if (bYPos && bZNeg && !Request.HasEdge(FVoxelMeshingRequest::EDGE_YPOS_ZNEG)) return false;
						if (bYNeg && bZPos && !Request.HasEdge(FVoxelMeshingRequest::EDGE_YNEG_ZPOS)) return false;
						if (bYNeg && bZNeg && !Request.HasEdge(FVoxelMeshingRequest::EDGE_YNEG_ZNEG)) return false;
					}
					// Check corner neighbors
					else if (OutCount == 3)
					{
						if (bXPos && bYPos && bZPos && !Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZPOS)) return false;
						if (bXPos && bYPos && bZNeg && !Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZNEG)) return false;
						if (bXPos && bYNeg && bZPos && !Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZPOS)) return false;
						if (bXPos && bYNeg && bZNeg && !Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZNEG)) return false;
						if (bXNeg && bYPos && bZPos && !Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZPOS)) return false;
						if (bXNeg && bYPos && bZNeg && !Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZNEG)) return false;
						if (bXNeg && bYNeg && bZPos && !Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZPOS)) return false;
						if (bXNeg && bYNeg && bZNeg && !Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZNEG)) return false;
					}
				}
			}
		}
	}

	return true;
}

void FVoxelCPUSmoothMesher::GetTransitionCellDensities(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 Stride,
	int32 FaceIndex,
	float OutDensities[13]) const
{
	// Sample 13 points for the transition cell:
	// - Points 0-8: 3x3 grid on the transition face (high-res detail)
	// - Points 9-12: Interior corners of the cell (low-res corners)
	//
	// Points 0, 2, 6, 8 are face corners (what the low-LOD neighbor sees on the face)
	// Points 1, 3, 4, 5, 7 are mid-edge and center points (high-res detail)
	// Points 9-12 are at the opposite side of the cell from the face
	//
	// IMPORTANT: At low LOD levels (especially LOD 0 with Stride=1), the mid-point
	// samples (1, 3, 4, 5, 7) fall at fractional voxel positions (e.g., 0.5).
	// We must use trilinear interpolation to get correct density values,
	// otherwise the transition geometry will be misaligned.

	const int32 ChunkSize = Request.ChunkSize;

	// Sample all 13 points using float positions
	for (int32 i = 0; i < 13; i++)
	{
		const FVector3f& Offset = TransvoxelTables::TransitionCellSampleOffsets[FaceIndex][i];

		// Convert offset (0-1) to voxel coordinates as FLOATS to preserve fractional positions
		const float SampleX = static_cast<float>(X) + Offset.X * static_cast<float>(Stride);
		const float SampleY = static_cast<float>(Y) + Offset.Y * static_cast<float>(Stride);
		const float SampleZ = static_cast<float>(Z) + Offset.Z * static_cast<float>(Stride);

		// Use trilinear interpolation for fractional positions
		OutDensities[i] = GetDensityAtTrilinear(Request, SampleX, SampleY, SampleZ);
	}
}

void FVoxelCPUSmoothMesher::ProcessTransitionCell(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 Stride,
	int32 FaceIndex,
	FChunkMeshData& OutMeshData,
	uint32& OutTriangleCount)
{
	static const TCHAR* FaceNames[6] = { TEXT("-X"), TEXT("+X"), TEXT("-Y"), TEXT("+Y"), TEXT("-Z"), TEXT("+Z") };

	// Get all 13 density samples for this transition cell
	// Samples 0-8: face samples (3x3 grid on transition face)
	// Samples 9-12: interior corners (at the opposite side of the cell)
	float Densities[13];
	GetTransitionCellDensities(Request, X, Y, Z, Stride, FaceIndex, Densities);

	const float IsoLevel = Config.IsoLevel;
	const float VoxelSize = Request.VoxelSize;

	// Build the case index from the 9 FACE samples only
	// The case index determines the triangulation pattern
	uint16 CaseIndex = 0;
	for (int32 i = 0; i < 9; i++)
	{
		if (Densities[i] >= IsoLevel)
		{
			CaseIndex |= (1 << i);
		}
	}

	// Look up the equivalence class
	// The high bit (0x80) indicates inverted winding order
	// The low 7 bits contain the equivalence class (0-55)
	const uint8 CellClassData = TransvoxelTables::TransitionCellClass[CaseIndex];
	const bool bInverted = (CellClassData & 0x80) != 0;
	const uint8 CellClass = CellClassData & 0x7F;

	// Debug logging for transition cells
	if (bDebugLogTransitionCells)
	{
		UE_LOG(LogVoxelMeshing, Log, TEXT("=== TRANSITION CELL ==="));
		UE_LOG(LogVoxelMeshing, Log, TEXT("  Chunk: (%d,%d,%d) LOD: %d"),
			Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z, Request.LODLevel);
		UE_LOG(LogVoxelMeshing, Log, TEXT("  Cell: (%d,%d,%d) Face: %s Stride: %d"),
			X, Y, Z, FaceNames[FaceIndex], Stride);
		UE_LOG(LogVoxelMeshing, Log, TEXT("  Neighbor LOD: %d"), Request.NeighborLODLevels[FaceIndex]);
		UE_LOG(LogVoxelMeshing, Log, TEXT("  Case: %d (0x%03X) Class: %d Inverted: %s"),
			CaseIndex, CaseIndex, CellClass, bInverted ? TEXT("Yes") : TEXT("No"));
		UE_LOG(LogVoxelMeshing, Log, TEXT("  Face Densities: [%.3f, %.3f, %.3f, %.3f, %.3f, %.3f, %.3f, %.3f, %.3f]"),
			Densities[0], Densities[1], Densities[2],
			Densities[3], Densities[4], Densities[5],
			Densities[6], Densities[7], Densities[8]);
		UE_LOG(LogVoxelMeshing, Log, TEXT("  Interior Densities: [%.3f, %.3f, %.3f, %.3f]"),
			Densities[9], Densities[10], Densities[11], Densities[12]);
	}

	// Early out for empty cases (class 0)
	if (CellClass == 0)
	{
		if (bDebugLogTransitionCells)
		{
			UE_LOG(LogVoxelMeshing, Log, TEXT("  Result: Empty (class 0)"));
		}
		return;
	}

	// Bounds check
	if (CellClass >= 56)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("Invalid transition cell class %d for case %d"), CellClass, CaseIndex);
		return;
	}

	const uint8 CellData = TransvoxelTables::TransitionCellData[CellClass];
	const int32 VertexCount = CellData >> 4;
	const int32 TriangleCount = CellData & 0x0F;

	if (VertexCount == 0 || TriangleCount == 0)
	{
		return;
	}

	// Bounds check vertex count
	if (VertexCount > 12)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("Invalid vertex count %d for class %d"), VertexCount, CellClass);
		return;
	}

	if (bDebugLogTransitionCells)
	{
		UE_LOG(LogVoxelMeshing, Log, TEXT("  Generating: %d vertices, %d triangles"), VertexCount, TriangleCount);
	}

	// Get base position in world coordinates
	const FVector3f BasePos = FVector3f(
		static_cast<float>(X) * VoxelSize,
		static_cast<float>(Y) * VoxelSize,
		static_cast<float>(Z) * VoxelSize
	);

	const float CellSize = static_cast<float>(Stride) * VoxelSize;

	// Collect debug visualization data if enabled
	FTransitionCellDebugData* DebugData = nullptr;
	if (bCollectDebugVisualization)
	{
		DebugData = &TransitionCellDebugData.AddDefaulted_GetRef();
		DebugData->ChunkCoord = Request.ChunkCoord;
		DebugData->CellBasePos = BasePos;
		DebugData->FaceIndex = FaceIndex;
		DebugData->Stride = Stride;
		DebugData->CurrentLOD = Request.LODLevel;
		DebugData->NeighborLOD = Request.NeighborLODLevels[FaceIndex];
		DebugData->CaseIndex = CaseIndex;
		DebugData->CellClass = CellClass;
		DebugData->bInverted = bInverted;
		FMemory::Memcpy(DebugData->SampleDensities, Densities, sizeof(Densities));

		// Store sample positions
		DebugData->SamplePositions.Reserve(13);
		for (int32 i = 0; i < 13; i++)
		{
			const FVector3f& Offset = TransvoxelTables::TransitionCellSampleOffsets[FaceIndex][i];
			DebugData->SamplePositions.Add(BasePos + Offset * CellSize);
		}
	}

	// Generate vertices
	// The vertex data encoding (from Eric Lengyel's Transvoxel):
	// - High byte: vertex reuse info (ignored for basic implementation)
	// - Low byte: edge endpoints
	//   - High nibble (bits 4-7): first endpoint index
	//   - Low nibble (bits 0-3): second endpoint index
	//
	// Endpoint indices:
	//   0-8: The 9 samples on the transition face
	//   9 (0x9): Interior corner 0 (sample index 9)
	//   A (0xA): Interior corner 1 (sample index 10)
	//   B (0xB): Interior corner 2 (sample index 11)
	//   C (0xC): Interior corner 3 (sample index 12)
	TArray<FVector3f> CellVertices;
	CellVertices.Reserve(VertexCount);

	// IMPORTANT: Index by CASE, not by class! The vertex data is pre-transformed per case.
	const uint16* VertexData = TransvoxelTables::TransitionVertexData[CaseIndex];
	for (int32 i = 0; i < VertexCount; i++)
	{
		const uint16 VData = VertexData[i];

		// Extract endpoints from LOW byte only
		const uint8 LowByte = VData & 0xFF;
		const int32 EndpointA = (LowByte >> 4) & 0x0F;  // High nibble of low byte
		const int32 EndpointB = LowByte & 0x0F;         // Low nibble of low byte

		// Map endpoint indices to sample indices
		// 0-8 map directly to face samples (indices 0-8 in Densities array)
		// 9-C (0x9-0xC) map to interior corners (indices 9-12 in Densities array)
		auto MapEndpointToSample = [](int32 Endpoint) -> int32
		{
			if (Endpoint <= 8)
			{
				return Endpoint;  // Face samples 0-8
			}
			// Interior corners: 0x9->9, 0xA->10, 0xB->11, 0xC->12
			switch (Endpoint)
			{
				case 0x9: return 9;   // Interior corner 0
				case 0xA: return 10;  // Interior corner 1
				case 0xB: return 11;  // Interior corner 2
				case 0xC: return 12;  // Interior corner 3
				default:
					return 0;  // Fallback
			}
		};

		const int32 SampleA = MapEndpointToSample(EndpointA);
		const int32 SampleB = MapEndpointToSample(EndpointB);

		FVector3f VertexPos;

		if (SampleA == SampleB)
		{
			// Vertex is exactly at this sample point
			const FVector3f& Offset = TransvoxelTables::TransitionCellSampleOffsets[FaceIndex][SampleA];
			VertexPos = BasePos + Offset * CellSize;
		}
		else
		{
			// Interpolate between two sample points
			const float DensityA = Densities[SampleA];
			const float DensityB = Densities[SampleB];

			// CRITICAL: Validate that the isosurface actually crosses this edge
			// The Transvoxel case index only considers face samples (0-8), but vertex data
			// can reference edges involving interior samples (9-12). If the interior samples
			// don't match the expected configuration, we may get edges that don't cross.
			const bool bAInside = (DensityA >= IsoLevel);
			const bool bBInside = (DensityB >= IsoLevel);
			if (bAInside == bBInside)
			{
				// Edge doesn't cross the isosurface - this vertex is invalid
				// Place it at the midpoint to minimize visual artifacts, but mark for potential skip
				const FVector3f& OffsetA = TransvoxelTables::TransitionCellSampleOffsets[FaceIndex][SampleA];
				const FVector3f& OffsetB = TransvoxelTables::TransitionCellSampleOffsets[FaceIndex][SampleB];
				const FVector3f PosA = BasePos + OffsetA * CellSize;
				const FVector3f PosB = BasePos + OffsetB * CellSize;

				// For edges that don't cross, snap to the endpoint that's closest to the surface
				// This prevents wild interpolation values and reduces visual artifacts
				if (bAInside)
				{
					// Both inside - snap to the one closer to the surface (lower density means closer to surface from inside)
					VertexPos = (DensityA <= DensityB) ? PosA : PosB;
				}
				else
				{
					// Both outside - snap to the one closer to the surface (higher density means closer to surface from outside)
					VertexPos = (DensityA >= DensityB) ? PosA : PosB;
				}

				// Only log occasionally to avoid spam
				static int32 NonCrossingEdgeCount = 0;
				if (++NonCrossingEdgeCount <= 10)
				{
					UE_LOG(LogVoxelMeshing, Verbose, TEXT("Transition cell: edge %d-%d doesn't cross isosurface (d0=%.3f, d1=%.3f)"),
						SampleA, SampleB, DensityA, DensityB);
				}
			}
			else
			{
				// Normal case - edge crosses the isosurface, interpolate correctly
				const FVector3f& OffsetA = TransvoxelTables::TransitionCellSampleOffsets[FaceIndex][SampleA];
				const FVector3f& OffsetB = TransvoxelTables::TransitionCellSampleOffsets[FaceIndex][SampleB];
				const FVector3f PosA = BasePos + OffsetA * CellSize;
				const FVector3f PosB = BasePos + OffsetB * CellSize;

				VertexPos = InterpolateEdge(DensityA, DensityB, PosA, PosB, IsoLevel);
			}
		}

		// Validate vertex position is finite
		if (!FMath::IsFinite(VertexPos.X) || !FMath::IsFinite(VertexPos.Y) || !FMath::IsFinite(VertexPos.Z))
		{
			UE_LOG(LogVoxelMeshing, Error, TEXT("Transition cell: NaN/Inf vertex %d - skipping cell"), i);
			return;
		}

		CellVertices.Add(VertexPos);

		if (bDebugLogTransitionCells)
		{
			UE_LOG(LogVoxelMeshing, Log, TEXT("    Vertex %d: (%.1f, %.1f, %.1f) from samples %d-%d (d=%.3f,%.3f)"),
				i, VertexPos.X, VertexPos.Y, VertexPos.Z, SampleA, SampleB,
				Densities[SampleA], Densities[SampleB]);
		}
	}

	// Store generated vertices for debug visualization
	if (DebugData)
	{
		DebugData->GeneratedVertices = CellVertices;
	}

	// Get material and biome info
	const uint8 MaterialID = GetDominantMaterialLOD(Request, X, Y, Z, Stride, static_cast<uint8>(CaseIndex & 0xFF));
	const uint8 BiomeID = GetDominantBiomeLOD(Request, X, Y, Z, Stride, static_cast<uint8>(CaseIndex & 0xFF));
	const FColor VertexColor = FColor(MaterialID, BiomeID, 0, 255);

	// Add vertices to mesh
	const uint32 BaseIndex = OutMeshData.Positions.Num();

	for (int32 i = 0; i < CellVertices.Num(); i++)
	{
		const FVector3f& Pos = CellVertices[i];
		OutMeshData.Positions.Add(Pos);

		// Calculate normal using gradient
		const FVector3f Normal = CalculateGradientNormalLOD(Request,
			Pos.X / VoxelSize, Pos.Y / VoxelSize, Pos.Z / VoxelSize, Stride);
		OutMeshData.Normals.Add(Normal);

		// UV mapping (triplanar-style based on normal)
		const FVector3f AbsNormal = FVector3f(FMath::Abs(Normal.X), FMath::Abs(Normal.Y), FMath::Abs(Normal.Z));
		FVector2f UV;
		if (AbsNormal.Z >= AbsNormal.X && AbsNormal.Z >= AbsNormal.Y)
		{
			UV = FVector2f(Pos.X, Pos.Y) * Config.UVScale / VoxelSize;
		}
		else if (AbsNormal.X >= AbsNormal.Y)
		{
			UV = FVector2f(Pos.Y, Pos.Z) * Config.UVScale / VoxelSize;
		}
		else
		{
			UV = FVector2f(Pos.X, Pos.Z) * Config.UVScale / VoxelSize;
		}
		OutMeshData.UVs.Add(UV);

		// UV1: MaterialID only (smooth meshing uses triplanar, no FaceType needed)
		OutMeshData.UV1s.Add(FVector2f(static_cast<float>(MaterialID), 0.0f));

		OutMeshData.Colors.Add(VertexColor);
	}

	// Add triangles with proper winding order
	// Lengyel's tables use opposite winding from Unreal's convention, so we reverse by default
	// The inverted flag (0x80) then indicates we should use the original (non-reversed) order
	const uint8* Triangles = TransvoxelTables::TransitionCellTriangles[CellClass];
	for (int32 t = 0; t < TriangleCount; t++)
	{
		const int32 BaseT = t * 3;
		if (Triangles[BaseT] == 0xFF)
		{
			break;
		}

		// Validate triangle indices
		const uint8 Idx0 = Triangles[BaseT];
		const uint8 Idx1 = Triangles[BaseT + 1];
		const uint8 Idx2 = Triangles[BaseT + 2];

		if (Idx0 >= VertexCount || Idx1 >= VertexCount || Idx2 >= VertexCount)
		{
			UE_LOG(LogVoxelMeshing, Warning, TEXT("Invalid triangle index in class %d: %d,%d,%d (vertex count: %d)"),
				CellClass, Idx0, Idx1, Idx2, VertexCount);
			continue;
		}

		if (bInverted)
		{
			// Inverted cases: use original table winding
			OutMeshData.Indices.Add(BaseIndex + Idx0);
			OutMeshData.Indices.Add(BaseIndex + Idx1);
			OutMeshData.Indices.Add(BaseIndex + Idx2);
		}
		else
		{
			// Normal cases: reverse winding for Unreal's coordinate system
			OutMeshData.Indices.Add(BaseIndex + Idx2);
			OutMeshData.Indices.Add(BaseIndex + Idx1);
			OutMeshData.Indices.Add(BaseIndex + Idx0);
		}
	}

	OutTriangleCount += TriangleCount;
}
