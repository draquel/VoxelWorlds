// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCPUCubicMesher.h"
#include "VoxelMeshing.h"
#include "VoxelMaterialRegistry.h"

// Face direction offsets: +X, -X, +Y, -Y, +Z, -Z
const FIntVector FVoxelCPUCubicMesher::FaceOffsets[6] = {
	FIntVector( 1,  0,  0),  // 0: +X (East)
	FIntVector(-1,  0,  0),  // 1: -X (West)
	FIntVector( 0,  1,  0),  // 2: +Y (North)
	FIntVector( 0, -1,  0),  // 3: -Y (South)
	FIntVector( 0,  0,  1),  // 4: +Z (Top)
	FIntVector( 0,  0, -1)   // 5: -Z (Bottom)
};

// Face normals
const FVector3f FVoxelCPUCubicMesher::FaceNormals[6] = {
	FVector3f( 1.0f,  0.0f,  0.0f),  // +X
	FVector3f(-1.0f,  0.0f,  0.0f),  // -X
	FVector3f( 0.0f,  1.0f,  0.0f),  // +Y
	FVector3f( 0.0f, -1.0f,  0.0f),  // -Y
	FVector3f( 0.0f,  0.0f,  1.0f),  // +Z
	FVector3f( 0.0f,  0.0f, -1.0f)   // -Z
};

// Quad vertex offsets for each face (CCW winding when viewed from outside)
const FVector3f FVoxelCPUCubicMesher::QuadVertices[6][4] = {
	// +X face (East)
	{
		FVector3f(1.0f, 0.0f, 0.0f),
		FVector3f(1.0f, 1.0f, 0.0f),
		FVector3f(1.0f, 1.0f, 1.0f),
		FVector3f(1.0f, 0.0f, 1.0f)
	},
	// -X face (West)
	{
		FVector3f(0.0f, 1.0f, 0.0f),
		FVector3f(0.0f, 0.0f, 0.0f),
		FVector3f(0.0f, 0.0f, 1.0f),
		FVector3f(0.0f, 1.0f, 1.0f)
	},
	// +Y face (North)
	{
		FVector3f(1.0f, 1.0f, 0.0f),
		FVector3f(0.0f, 1.0f, 0.0f),
		FVector3f(0.0f, 1.0f, 1.0f),
		FVector3f(1.0f, 1.0f, 1.0f)
	},
	// -Y face (South)
	{
		FVector3f(0.0f, 0.0f, 0.0f),
		FVector3f(1.0f, 0.0f, 0.0f),
		FVector3f(1.0f, 0.0f, 1.0f),
		FVector3f(0.0f, 0.0f, 1.0f)
	},
	// +Z face (Top)
	{
		FVector3f(0.0f, 0.0f, 1.0f),
		FVector3f(1.0f, 0.0f, 1.0f),
		FVector3f(1.0f, 1.0f, 1.0f),
		FVector3f(0.0f, 1.0f, 1.0f)
	},
	// -Z face (Bottom) - CCW when viewed from below
	{
		FVector3f(0.0f, 0.0f, 0.0f),
		FVector3f(1.0f, 0.0f, 0.0f),
		FVector3f(1.0f, 1.0f, 0.0f),
		FVector3f(0.0f, 1.0f, 0.0f)
	}
};

// UV coordinates for quad vertices
const FVector2f FVoxelCPUCubicMesher::QuadUVs[4] = {
	FVector2f(0.0f, 0.0f),
	FVector2f(1.0f, 0.0f),
	FVector2f(1.0f, 1.0f),
	FVector2f(0.0f, 1.0f)
};

// AO neighbor offsets for each face and vertex
// AONeighborOffsets[Face][Vertex][0=Side1, 1=Side2, 2=Corner]
// For each vertex, we check two edge-adjacent neighbors and one corner neighbor
// to determine ambient occlusion level (0-3)
const FIntVector FVoxelCPUCubicMesher::AONeighborOffsets[6][4][3] = {
	// Face 0: +X
	{
		{ FIntVector(1,-1,0), FIntVector(1,0,-1), FIntVector(1,-1,-1) },
		{ FIntVector(1,1,0),  FIntVector(1,0,-1), FIntVector(1,1,-1)  },
		{ FIntVector(1,1,0),  FIntVector(1,0,1),  FIntVector(1,1,1)   },
		{ FIntVector(1,-1,0), FIntVector(1,0,1),  FIntVector(1,-1,1)  }
	},
	// Face 1: -X
	{
		{ FIntVector(-1,1,0),  FIntVector(-1,0,-1), FIntVector(-1,1,-1)  },
		{ FIntVector(-1,-1,0), FIntVector(-1,0,-1), FIntVector(-1,-1,-1) },
		{ FIntVector(-1,-1,0), FIntVector(-1,0,1),  FIntVector(-1,-1,1)  },
		{ FIntVector(-1,1,0),  FIntVector(-1,0,1),  FIntVector(-1,1,1)   }
	},
	// Face 2: +Y
	{
		{ FIntVector(1,1,0),  FIntVector(0,1,-1), FIntVector(1,1,-1)  },
		{ FIntVector(-1,1,0), FIntVector(0,1,-1), FIntVector(-1,1,-1) },
		{ FIntVector(-1,1,0), FIntVector(0,1,1),  FIntVector(-1,1,1)  },
		{ FIntVector(1,1,0),  FIntVector(0,1,1),  FIntVector(1,1,1)   }
	},
	// Face 3: -Y
	{
		{ FIntVector(-1,-1,0), FIntVector(0,-1,-1), FIntVector(-1,-1,-1) },
		{ FIntVector(1,-1,0),  FIntVector(0,-1,-1), FIntVector(1,-1,-1)  },
		{ FIntVector(1,-1,0),  FIntVector(0,-1,1),  FIntVector(1,-1,1)   },
		{ FIntVector(-1,-1,0), FIntVector(0,-1,1),  FIntVector(-1,-1,1)  }
	},
	// Face 4: +Z
	{
		{ FIntVector(-1,0,1), FIntVector(0,-1,1), FIntVector(-1,-1,1) },
		{ FIntVector(1,0,1),  FIntVector(0,-1,1), FIntVector(1,-1,1)  },
		{ FIntVector(1,0,1),  FIntVector(0,1,1),  FIntVector(1,1,1)   },
		{ FIntVector(-1,0,1), FIntVector(0,1,1),  FIntVector(-1,1,1)  }
	},
	// Face 5: -Z
	{
		{ FIntVector(1,0,-1),  FIntVector(0,-1,-1), FIntVector(1,-1,-1)  },
		{ FIntVector(-1,0,-1), FIntVector(0,-1,-1), FIntVector(-1,-1,-1) },
		{ FIntVector(-1,0,-1), FIntVector(0,1,-1),  FIntVector(-1,1,-1)  },
		{ FIntVector(1,0,-1),  FIntVector(0,1,-1),  FIntVector(1,1,-1)   }
	}
};

FVoxelCPUCubicMesher::FVoxelCPUCubicMesher()
{
}

FVoxelCPUCubicMesher::~FVoxelCPUCubicMesher()
{
	if (bIsInitialized)
	{
		Shutdown();
	}
}

void FVoxelCPUCubicMesher::Initialize()
{
	if (bIsInitialized)
	{
		return;
	}

	UE_LOG(LogVoxelMeshing, Log, TEXT("CPU Cubic Mesher initialized"));
	bIsInitialized = true;
}

void FVoxelCPUCubicMesher::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	ReleaseAllHandles();
	bIsInitialized = false;
	UE_LOG(LogVoxelMeshing, Log, TEXT("CPU Cubic Mesher shutdown"));
}

bool FVoxelCPUCubicMesher::IsInitialized() const
{
	return bIsInitialized;
}

bool FVoxelCPUCubicMesher::GenerateMeshCPU(
	const FVoxelMeshingRequest& Request,
	FChunkMeshData& OutMeshData)
{
	FVoxelMeshingStats Stats;
	return GenerateMeshCPU(Request, OutMeshData, Stats);
}

bool FVoxelCPUCubicMesher::GenerateMeshCPU(
	const FVoxelMeshingRequest& Request,
	FChunkMeshData& OutMeshData,
	FVoxelMeshingStats& OutStats)
{
	if (!bIsInitialized)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("CPU Cubic Mesher not initialized"));
		return false;
	}

	if (!Request.IsValid())
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("Invalid meshing request"));
		return false;
	}

	if (Config.bUseGreedyMeshing)
	{
		// Use greedy meshing for optimized output (40-60% fewer triangles)
		GenerateMeshGreedy(Request, OutMeshData, OutStats);
	}
	else
	{
		// Use simple per-voxel meshing (useful for debugging or when per-face data needed)
		GenerateMeshSimple(Request, OutMeshData, OutStats);
	}

	return true;
}

void FVoxelCPUCubicMesher::GenerateMeshGreedy(
	const FVoxelMeshingRequest& Request,
	FChunkMeshData& OutMeshData,
	FVoxelMeshingStats& OutStats)
{
	const double StartTime = FPlatformTime::Seconds();

	// Reset output
	OutMeshData.Reset();
	OutStats = FVoxelMeshingStats();

	// Pre-allocate arrays (greedy meshing produces fewer faces, so estimate lower)
	const int32 EstimatedFaces = Request.ChunkSize * Request.ChunkSize * 2;
	OutMeshData.Positions.Reserve(EstimatedFaces * 4);
	OutMeshData.Normals.Reserve(EstimatedFaces * 4);
	OutMeshData.UVs.Reserve(EstimatedFaces * 4);
	OutMeshData.Colors.Reserve(EstimatedFaces * 4);
	OutMeshData.Indices.Reserve(EstimatedFaces * 6);

	uint32 GeneratedFaces = 0;

	// Process each face direction
	for (int32 Face = 0; Face < 6; Face++)
	{
		ProcessFaceDirectionGreedy(Face, Request, OutMeshData, GeneratedFaces);
	}

	// Count solid voxels for stats
	const int32 ChunkSize = Request.ChunkSize;
	uint32 SolidVoxels = 0;
	for (int32 Z = 0; Z < ChunkSize; Z++)
	{
		for (int32 Y = 0; Y < ChunkSize; Y++)
		{
			for (int32 X = 0; X < ChunkSize; X++)
			{
				if (!Request.GetVoxel(X, Y, Z).IsAir())
				{
					SolidVoxels++;
				}
			}
		}
	}

	// Calculate stats
	const double EndTime = FPlatformTime::Seconds();
	OutStats.VertexCount = OutMeshData.Positions.Num();
	OutStats.IndexCount = OutMeshData.Indices.Num();
	OutStats.FaceCount = GeneratedFaces;
	OutStats.SolidVoxelCount = SolidVoxels;
	OutStats.CulledFaceCount = 0; // Not tracked in greedy meshing
	OutStats.GenerationTimeMs = static_cast<float>((EndTime - StartTime) * 1000.0);

	UE_LOG(LogVoxelMeshing, Verbose,
		TEXT("Greedy meshing complete: %d verts, %d tris, %d merged faces, %.2fms"),
		OutStats.VertexCount, OutStats.GetTriangleCount(), GeneratedFaces,
		OutStats.GenerationTimeMs);
}

void FVoxelCPUCubicMesher::GenerateMeshSimple(
	const FVoxelMeshingRequest& Request,
	FChunkMeshData& OutMeshData,
	FVoxelMeshingStats& OutStats)
{
	const double StartTime = FPlatformTime::Seconds();

	// Reset output
	OutMeshData.Reset();
	OutStats = FVoxelMeshingStats();

	const int32 ChunkSize = Request.ChunkSize;

	// Pre-allocate arrays (estimate worst case: each voxel exposes some faces)
	const int32 EstimatedFaces = ChunkSize * ChunkSize * 6;
	OutMeshData.Positions.Reserve(EstimatedFaces * 4);
	OutMeshData.Normals.Reserve(EstimatedFaces * 4);
	OutMeshData.UVs.Reserve(EstimatedFaces * 4);
	OutMeshData.Colors.Reserve(EstimatedFaces * 4);
	OutMeshData.Indices.Reserve(EstimatedFaces * 6);

	uint32 SolidVoxels = 0;
	uint32 GeneratedFaces = 0;
	uint32 CulledFaces = 0;

	// Iterate through all voxels
	for (int32 Z = 0; Z < ChunkSize; Z++)
	{
		for (int32 Y = 0; Y < ChunkSize; Y++)
		{
			for (int32 X = 0; X < ChunkSize; X++)
			{
				const FVoxelData& Voxel = Request.GetVoxel(X, Y, Z);

				// Skip air voxels
				if (Voxel.IsAir())
				{
					continue;
				}

				SolidVoxels++;

				// Check all 6 faces
				for (int32 Face = 0; Face < 6; Face++)
				{
					if (ShouldRenderFace(Request, X, Y, Z, Face))
					{
						EmitQuad(OutMeshData, Request, X, Y, Z, Face, Voxel);
						GeneratedFaces++;
					}
					else
					{
						CulledFaces++;
					}
				}
			}
		}
	}

	// Calculate stats
	const double EndTime = FPlatformTime::Seconds();
	OutStats.VertexCount = OutMeshData.Positions.Num();
	OutStats.IndexCount = OutMeshData.Indices.Num();
	OutStats.FaceCount = GeneratedFaces;
	OutStats.SolidVoxelCount = SolidVoxels;
	OutStats.CulledFaceCount = CulledFaces;
	OutStats.GenerationTimeMs = static_cast<float>((EndTime - StartTime) * 1000.0);

	UE_LOG(LogVoxelMeshing, Verbose,
		TEXT("Simple meshing complete: %d verts, %d tris, %d faces (culled %d), %.2fms"),
		OutStats.VertexCount, OutStats.GetTriangleCount(), GeneratedFaces, CulledFaces,
		OutStats.GenerationTimeMs);
}

void FVoxelCPUCubicMesher::GetFaceAxes(int32 Face, int32& OutPrimaryAxis, int32& OutUAxis, int32& OutVAxis, bool& OutPositive)
{
	// Face 0: +X, Face 1: -X -> Primary=X(0), U=Y(1), V=Z(2)
	// Face 2: +Y, Face 3: -Y -> Primary=Y(1), U=X(0), V=Z(2)
	// Face 4: +Z, Face 5: -Z -> Primary=Z(2), U=X(0), V=Y(1)
	switch (Face)
	{
	case 0: // +X
		OutPrimaryAxis = 0; OutUAxis = 1; OutVAxis = 2; OutPositive = true;
		break;
	case 1: // -X
		OutPrimaryAxis = 0; OutUAxis = 1; OutVAxis = 2; OutPositive = false;
		break;
	case 2: // +Y
		OutPrimaryAxis = 1; OutUAxis = 0; OutVAxis = 2; OutPositive = true;
		break;
	case 3: // -Y
		OutPrimaryAxis = 1; OutUAxis = 0; OutVAxis = 2; OutPositive = false;
		break;
	case 4: // +Z
		OutPrimaryAxis = 2; OutUAxis = 0; OutVAxis = 1; OutPositive = true;
		break;
	case 5: // -Z
	default:
		OutPrimaryAxis = 2; OutUAxis = 0; OutVAxis = 1; OutPositive = false;
		break;
	}
}

void FVoxelCPUCubicMesher::ProcessFaceDirectionGreedy(
	int32 Face,
	const FVoxelMeshingRequest& Request,
	FChunkMeshData& OutMeshData,
	uint32& OutGeneratedFaces)
{
	const int32 ChunkSize = Request.ChunkSize;
	const int32 SliceSize = ChunkSize * ChunkSize;

	// Get axis mapping for this face
	int32 PrimaryAxis, UAxis, VAxis;
	bool bPositive;
	GetFaceAxes(Face, PrimaryAxis, UAxis, VAxis, bPositive);

	// Allocate mask and processed arrays
	TArray<uint16> FaceMask;
	TArray<bool> Processed;
	FaceMask.SetNumZeroed(SliceSize);
	Processed.SetNumZeroed(SliceSize);

	// Process each slice along the primary axis
	for (int32 SliceIndex = 0; SliceIndex < ChunkSize; SliceIndex++)
	{
		// Build face mask for this slice
		BuildFaceMask(Face, SliceIndex, Request, FaceMask);

		// Reset processed array
		FMemory::Memzero(Processed.GetData(), SliceSize * sizeof(bool));

		// Greedy merge algorithm
		for (int32 V = 0; V < ChunkSize; V++)
		{
			for (int32 U = 0; U < ChunkSize; U++)
			{
				const int32 Index = U + V * ChunkSize;

				// Skip if already processed or no face needed
				if (Processed[Index] || FaceMask[Index] == 0)
				{
					continue;
				}

				const uint16 CurrentMaterial = FaceMask[Index];

				// Find maximum width (extend along U axis)
				int32 Width = 1;
				while (U + Width < ChunkSize)
				{
					const int32 NextIndex = (U + Width) + V * ChunkSize;
					if (Processed[NextIndex] || FaceMask[NextIndex] != CurrentMaterial)
					{
						break;
					}
					Width++;
				}

				// Find maximum height (extend along V axis) for this width
				int32 Height = 1;
				bool bCanExtend = true;
				while (bCanExtend && V + Height < ChunkSize)
				{
					// Check if the entire row can be extended
					for (int32 DU = 0; DU < Width; DU++)
					{
						const int32 CheckIndex = (U + DU) + (V + Height) * ChunkSize;
						if (Processed[CheckIndex] || FaceMask[CheckIndex] != CurrentMaterial)
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
				for (int32 DV = 0; DV < Height; DV++)
				{
					for (int32 DU = 0; DU < Width; DU++)
					{
						const int32 MarkIndex = (U + DU) + (V + DV) * ChunkSize;
						Processed[MarkIndex] = true;
					}
				}

				// Emit merged quad
				const uint8 MaterialID = static_cast<uint8>(CurrentMaterial - 1);
				EmitMergedQuad(OutMeshData, Request, Face, SliceIndex, U, V, Width, Height, MaterialID);
				OutGeneratedFaces++;
			}
		}
	}
}

void FVoxelCPUCubicMesher::BuildFaceMask(
	int32 Face,
	int32 SliceIndex,
	const FVoxelMeshingRequest& Request,
	TArray<uint16>& OutMask) const
{
	const int32 ChunkSize = Request.ChunkSize;

	// Get axis mapping
	int32 PrimaryAxis, UAxis, VAxis;
	bool bPositive;
	GetFaceAxes(Face, PrimaryAxis, UAxis, VAxis, bPositive);

	// Clear mask
	FMemory::Memzero(OutMask.GetData(), ChunkSize * ChunkSize * sizeof(uint16));

	// Build coordinate array for iteration
	int32 Coords[3];

	for (int32 V = 0; V < ChunkSize; V++)
	{
		for (int32 U = 0; U < ChunkSize; U++)
		{
			// Map U, V to world coordinates
			Coords[PrimaryAxis] = SliceIndex;
			Coords[UAxis] = U;
			Coords[VAxis] = V;

			const int32 X = Coords[0];
			const int32 Y = Coords[1];
			const int32 Z = Coords[2];

			const FVoxelData& Voxel = Request.GetVoxel(X, Y, Z);

			// Skip air voxels
			if (Voxel.IsAir())
			{
				continue;
			}

			// Check if face should be rendered (neighbor is air)
			if (ShouldRenderFace(Request, X, Y, Z, Face))
			{
				// Store MaterialID + 1 (so 0 means no face)
				const int32 MaskIndex = U + V * ChunkSize;
				OutMask[MaskIndex] = static_cast<uint16>(Voxel.MaterialID) + 1;
			}
		}
	}
}

void FVoxelCPUCubicMesher::EmitMergedQuad(
	FChunkMeshData& MeshData,
	const FVoxelMeshingRequest& Request,
	int32 Face,
	int32 SliceIndex,
	int32 U, int32 V,
	int32 Width, int32 Height,
	uint8 MaterialID) const
{
	const float VoxelSize = Request.VoxelSize;
	const int32 ChunkSize = Request.ChunkSize;

	// Get axis mapping
	int32 PrimaryAxis, UAxis, VAxis;
	bool bPositive;
	GetFaceAxes(Face, PrimaryAxis, UAxis, VAxis, bPositive);

	// Calculate base position in world coordinates
	FVector3f BasePos;
	int32 Coords[3];
	Coords[PrimaryAxis] = SliceIndex;
	Coords[UAxis] = U;
	Coords[VAxis] = V;
	BasePos = FVector3f(
		static_cast<float>(Coords[0]) * VoxelSize,
		static_cast<float>(Coords[1]) * VoxelSize,
		static_cast<float>(Coords[2]) * VoxelSize
	);

	// Calculate the offset for the face (positive faces are offset by 1 voxel)
	FVector3f FaceOffset = FVector3f::ZeroVector;
	if (bPositive)
	{
		FaceOffset[PrimaryAxis] = VoxelSize;
	}

	// Calculate quad size
	const float QuadWidth = Width * VoxelSize;
	const float QuadHeight = Height * VoxelSize;

	// Get the axis vectors for U and V directions
	FVector3f UDir = FVector3f::ZeroVector;
	FVector3f VDir = FVector3f::ZeroVector;
	UDir[UAxis] = VoxelSize;
	VDir[VAxis] = VoxelSize;

	const FVector3f& Normal = FaceNormals[Face];
	const uint32 BaseVertex = MeshData.Positions.Num();

	// Calculate the 4 vertices of the merged quad
	// We need to maintain correct winding order for each face direction
	TArray<FVector3f, TInlineAllocator<4>> Vertices;
	Vertices.SetNum(4);

	// The vertex order depends on the face direction to maintain correct winding
	FVector3f Corner0 = BasePos + FaceOffset;  // Base corner
	FVector3f Corner1 = Corner0 + UDir * Width;  // +U
	FVector3f Corner2 = Corner0 + UDir * Width + VDir * Height;  // +U +V
	FVector3f Corner3 = Corner0 + VDir * Height;  // +V

	// Calculate per-vertex AO at the 4 corner voxels of the merged quad
	// Corner voxel positions in 2D (U,V) space:
	// Vertex 0: (U, V)
	// Vertex 1: (U+Width-1, V)
	// Vertex 2: (U+Width-1, V+Height-1)
	// Vertex 3: (U, V+Height-1)
	uint8 VertexAO[4] = {0, 0, 0, 0};
	if (Config.bCalculateAO)
	{
		// Map 2D corner positions to 3D voxel coordinates
		int32 CornerCoords[4][3];

		// Corner 0: (U, V)
		CornerCoords[0][PrimaryAxis] = SliceIndex;
		CornerCoords[0][UAxis] = U;
		CornerCoords[0][VAxis] = V;

		// Corner 1: (U+Width-1, V)
		CornerCoords[1][PrimaryAxis] = SliceIndex;
		CornerCoords[1][UAxis] = U + Width - 1;
		CornerCoords[1][VAxis] = V;

		// Corner 2: (U+Width-1, V+Height-1)
		CornerCoords[2][PrimaryAxis] = SliceIndex;
		CornerCoords[2][UAxis] = U + Width - 1;
		CornerCoords[2][VAxis] = V + Height - 1;

		// Corner 3: (U, V+Height-1)
		CornerCoords[3][PrimaryAxis] = SliceIndex;
		CornerCoords[3][UAxis] = U;
		CornerCoords[3][VAxis] = V + Height - 1;

		// Calculate AO for each corner vertex
		VertexAO[0] = CalculateVertexAO(Request, CornerCoords[0][0], CornerCoords[0][1], CornerCoords[0][2], Face, 0);
		VertexAO[1] = CalculateVertexAO(Request, CornerCoords[1][0], CornerCoords[1][1], CornerCoords[1][2], Face, 1);
		VertexAO[2] = CalculateVertexAO(Request, CornerCoords[2][0], CornerCoords[2][1], CornerCoords[2][2], Face, 2);
		VertexAO[3] = CalculateVertexAO(Request, CornerCoords[3][0], CornerCoords[3][1], CornerCoords[3][2], Face, 3);
	}

	// Adjust winding based on face direction to match the original QuadVertices patterns
	// Also track which AO value goes with which vertex after reordering
	int32 AOMapping[4] = {0, 1, 2, 3};
	switch (Face)
	{
	case 0: // +X: Y increases left-to-right, Z increases bottom-to-top (when viewed from +X)
		Vertices[0] = Corner0;
		Vertices[1] = Corner1;
		Vertices[2] = Corner2;
		Vertices[3] = Corner3;
		AOMapping[0] = 0; AOMapping[1] = 1; AOMapping[2] = 2; AOMapping[3] = 3;
		break;
	case 1: // -X: Need to reverse winding
		Vertices[0] = Corner1;
		Vertices[1] = Corner0;
		Vertices[2] = Corner3;
		Vertices[3] = Corner2;
		AOMapping[0] = 1; AOMapping[1] = 0; AOMapping[2] = 3; AOMapping[3] = 2;
		break;
	case 2: // +Y: X increases, Z increases
		Vertices[0] = Corner1;
		Vertices[1] = Corner0;
		Vertices[2] = Corner3;
		Vertices[3] = Corner2;
		AOMapping[0] = 1; AOMapping[1] = 0; AOMapping[2] = 3; AOMapping[3] = 2;
		break;
	case 3: // -Y: Need to reverse
		Vertices[0] = Corner0;
		Vertices[1] = Corner1;
		Vertices[2] = Corner2;
		Vertices[3] = Corner3;
		AOMapping[0] = 0; AOMapping[1] = 1; AOMapping[2] = 2; AOMapping[3] = 3;
		break;
	case 4: // +Z: X increases, Y increases
		Vertices[0] = Corner0;
		Vertices[1] = Corner1;
		Vertices[2] = Corner2;
		Vertices[3] = Corner3;
		AOMapping[0] = 0; AOMapping[1] = 1; AOMapping[2] = 2; AOMapping[3] = 3;
		break;
	case 5: // -Z: Need to reverse for correct winding when viewed from below
		Vertices[0] = Corner0;
		Vertices[1] = Corner1;
		Vertices[2] = Corner2;
		Vertices[3] = Corner3;
		AOMapping[0] = 0; AOMapping[1] = 1; AOMapping[2] = 2; AOMapping[3] = 3;
		break;
	}

	// Look up material color
	FColor MaterialColor = FVoxelMaterialRegistry::GetMaterialColor(MaterialID);

	// UV coordinates scaled by quad size for proper texture tiling
	const FVector2f UV0(0.0f, 0.0f);
	const FVector2f UV1(Width * Config.UVScale, 0.0f);
	const FVector2f UV2(Width * Config.UVScale, Height * Config.UVScale);
	const FVector2f UV3(0.0f, Height * Config.UVScale);

	// Emit 4 vertices with per-vertex AO
	for (int32 i = 0; i < 4; i++)
	{
		MeshData.Positions.Add(Vertices[i]);
		MeshData.Normals.Add(Normal);

		// Apply per-vertex AO: 0=unoccluded (full brightness), 3=fully occluded (25% brightness)
		const float AOFactor = Config.bCalculateAO ? (1.0f - VertexAO[AOMapping[i]] * 0.25f) : 1.0f;
		const FColor VertexColor(
			static_cast<uint8>(MaterialColor.R * AOFactor),
			static_cast<uint8>(MaterialColor.G * AOFactor),
			static_cast<uint8>(MaterialColor.B * AOFactor),
			255
		);
		MeshData.Colors.Add(VertexColor);
	}

	// Add UVs
	if (Config.bGenerateUVs)
	{
		MeshData.UVs.Add(UV0);
		MeshData.UVs.Add(UV1);
		MeshData.UVs.Add(UV2);
		MeshData.UVs.Add(UV3);
	}
	else
	{
		MeshData.UVs.Add(FVector2f::ZeroVector);
		MeshData.UVs.Add(FVector2f::ZeroVector);
		MeshData.UVs.Add(FVector2f::ZeroVector);
		MeshData.UVs.Add(FVector2f::ZeroVector);
	}

	// Emit 6 indices (2 triangles, CW winding)
	MeshData.Indices.Add(BaseVertex + 0);
	MeshData.Indices.Add(BaseVertex + 2);
	MeshData.Indices.Add(BaseVertex + 1);
	MeshData.Indices.Add(BaseVertex + 0);
	MeshData.Indices.Add(BaseVertex + 3);
	MeshData.Indices.Add(BaseVertex + 2);
}

bool FVoxelCPUCubicMesher::ShouldRenderFace(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 Face) const
{
	const FIntVector& Offset = FaceOffsets[Face];
	const int32 NX = X + Offset.X;
	const int32 NY = Y + Offset.Y;
	const int32 NZ = Z + Offset.Z;

	// Get neighbor voxel (handles chunk boundaries)
	const FVoxelData Neighbor = GetVoxelAt(Request, NX, NY, NZ);

	// Render face if neighbor is air
	return Neighbor.IsAir();
}

FVoxelData FVoxelCPUCubicMesher::GetVoxelAt(
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
	// Note: For AO calculations, we may need diagonal neighbors that span multiple chunks.
	// We only have face-adjacent neighbor data, so diagonal lookups return air.
	const int32 SliceSize = ChunkSize * ChunkSize;

	// +X neighbor (requires Y and Z to be in bounds)
	if (X >= ChunkSize && X < ChunkSize + 1 && Y >= 0 && Y < ChunkSize && Z >= 0 && Z < ChunkSize)
	{
		if (Request.NeighborXPos.Num() == SliceSize)
		{
			const int32 Index = Y + Z * ChunkSize;
			return Request.NeighborXPos[Index];
		}
		return FVoxelData::Air();
	}

	// -X neighbor (requires Y and Z to be in bounds)
	if (X < 0 && X >= -1 && Y >= 0 && Y < ChunkSize && Z >= 0 && Z < ChunkSize)
	{
		if (Request.NeighborXNeg.Num() == SliceSize)
		{
			const int32 Index = Y + Z * ChunkSize;
			return Request.NeighborXNeg[Index];
		}
		return FVoxelData::Air();
	}

	// +Y neighbor (requires X and Z to be in bounds)
	if (Y >= ChunkSize && Y < ChunkSize + 1 && X >= 0 && X < ChunkSize && Z >= 0 && Z < ChunkSize)
	{
		if (Request.NeighborYPos.Num() == SliceSize)
		{
			const int32 Index = X + Z * ChunkSize;
			return Request.NeighborYPos[Index];
		}
		return FVoxelData::Air();
	}

	// -Y neighbor (requires X and Z to be in bounds)
	if (Y < 0 && Y >= -1 && X >= 0 && X < ChunkSize && Z >= 0 && Z < ChunkSize)
	{
		if (Request.NeighborYNeg.Num() == SliceSize)
		{
			const int32 Index = X + Z * ChunkSize;
			return Request.NeighborYNeg[Index];
		}
		return FVoxelData::Air();
	}

	// +Z neighbor (requires X and Y to be in bounds)
	if (Z >= ChunkSize && Z < ChunkSize + 1 && X >= 0 && X < ChunkSize && Y >= 0 && Y < ChunkSize)
	{
		if (Request.NeighborZPos.Num() == SliceSize)
		{
			const int32 Index = X + Y * ChunkSize;
			return Request.NeighborZPos[Index];
		}
		return FVoxelData::Air();
	}

	// -Z neighbor (requires X and Y to be in bounds)
	if (Z < 0 && Z >= -1 && X >= 0 && X < ChunkSize && Y >= 0 && Y < ChunkSize)
	{
		if (Request.NeighborZNeg.Num() == SliceSize)
		{
			const int32 Index = X + Y * ChunkSize;
			return Request.NeighborZNeg[Index];
		}
		return FVoxelData::Air();
	}

	// Out of bounds in multiple directions (diagonal) or too far out - treat as air
	return FVoxelData::Air();
}

uint8 FVoxelCPUCubicMesher::CalculateVertexAO(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 Face, int32 VertexIndex) const
{
	const FIntVector& Side1 = AONeighborOffsets[Face][VertexIndex][0];
	const FIntVector& Side2 = AONeighborOffsets[Face][VertexIndex][1];
	const FIntVector& Corner = AONeighborOffsets[Face][VertexIndex][2];

	const bool bSide1 = !GetVoxelAt(Request, X + Side1.X, Y + Side1.Y, Z + Side1.Z).IsAir();
	const bool bSide2 = !GetVoxelAt(Request, X + Side2.X, Y + Side2.Y, Z + Side2.Z).IsAir();
	const bool bCorner = !GetVoxelAt(Request, X + Corner.X, Y + Corner.Y, Z + Corner.Z).IsAir();

	// Standard voxel AO formula: if both sides are solid, corner is fully occluded
	if (bSide1 && bSide2)
	{
		return 3;
	}
	return static_cast<uint8>(bSide1 + bSide2 + bCorner);
}

void FVoxelCPUCubicMesher::CalculateFaceAO(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 Face, uint8 OutAO[4]) const
{
	for (int32 V = 0; V < 4; V++)
	{
		OutAO[V] = CalculateVertexAO(Request, X, Y, Z, Face, V);
	}
}

void FVoxelCPUCubicMesher::EmitQuad(
	FChunkMeshData& MeshData,
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 Face,
	const FVoxelData& Voxel) const
{
	const float VoxelSize = Request.VoxelSize;
	const FVector3f VoxelPos(
		static_cast<float>(X) * VoxelSize,
		static_cast<float>(Y) * VoxelSize,
		static_cast<float>(Z) * VoxelSize
	);

	const FVector3f& Normal = FaceNormals[Face];
	const uint32 BaseVertex = MeshData.Positions.Num();

	// Calculate per-vertex AO if enabled
	uint8 VertexAO[4] = {0, 0, 0, 0};
	if (Config.bCalculateAO)
	{
		CalculateFaceAO(Request, X, Y, Z, Face, VertexAO);
	}

	// Look up material color from registry
	FColor MaterialColor = FVoxelMaterialRegistry::GetMaterialColor(Voxel.MaterialID);

	// Emit 4 vertices
	for (int32 V = 0; V < 4; V++)
	{
		const FVector3f Position = VoxelPos + QuadVertices[Face][V] * VoxelSize;
		MeshData.Positions.Add(Position);
		MeshData.Normals.Add(Normal);

		if (Config.bGenerateUVs)
		{
			MeshData.UVs.Add(QuadUVs[V] * Config.UVScale);
		}
		else
		{
			MeshData.UVs.Add(FVector2f::ZeroVector);
		}

		// Apply per-vertex AO: 0=unoccluded (full brightness), 3=fully occluded (25% brightness)
		// Formula: AOFactor = 1.0 - (AO * 0.25)
		const float AOFactor = Config.bCalculateAO ? (1.0f - VertexAO[V] * 0.25f) : 1.0f;
		MeshData.Colors.Add(FColor(
			static_cast<uint8>(MaterialColor.R * AOFactor),
			static_cast<uint8>(MaterialColor.G * AOFactor),
			static_cast<uint8>(MaterialColor.B * AOFactor),
			255));
	}

	// Emit 6 indices (2 triangles, CW winding for Unreal's left-handed coordinate system)
	MeshData.Indices.Add(BaseVertex + 0);
	MeshData.Indices.Add(BaseVertex + 2);
	MeshData.Indices.Add(BaseVertex + 1);
	MeshData.Indices.Add(BaseVertex + 0);
	MeshData.Indices.Add(BaseVertex + 3);
	MeshData.Indices.Add(BaseVertex + 2);
}

// ============================================================================
// Async Pattern (wraps sync for CPU mesher)
// ============================================================================

FVoxelMeshingHandle FVoxelCPUCubicMesher::GenerateMeshAsync(
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

bool FVoxelCPUCubicMesher::IsComplete(const FVoxelMeshingHandle& Handle) const
{
	// CPU mesher always completes synchronously
	return Handle.bIsComplete;
}

bool FVoxelCPUCubicMesher::WasSuccessful(const FVoxelMeshingHandle& Handle) const
{
	return Handle.bWasSuccessful;
}

FRHIBuffer* FVoxelCPUCubicMesher::GetVertexBuffer(const FVoxelMeshingHandle& Handle)
{
	// CPU mesher doesn't create GPU buffers
	return nullptr;
}

FRHIBuffer* FVoxelCPUCubicMesher::GetIndexBuffer(const FVoxelMeshingHandle& Handle)
{
	// CPU mesher doesn't create GPU buffers
	return nullptr;
}

bool FVoxelCPUCubicMesher::GetBufferCounts(
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

bool FVoxelCPUCubicMesher::GetRenderData(
	const FVoxelMeshingHandle& Handle,
	FChunkRenderData& OutRenderData)
{
	// CPU mesher doesn't create GPU render data
	// Caller should use ReadbackToCPU and upload manually
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

bool FVoxelCPUCubicMesher::ReadbackToCPU(
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

void FVoxelCPUCubicMesher::ReleaseHandle(const FVoxelMeshingHandle& Handle)
{
	FScopeLock Lock(&CacheLock);
	CachedResults.Remove(Handle.RequestId);
}

void FVoxelCPUCubicMesher::ReleaseAllHandles()
{
	FScopeLock Lock(&CacheLock);
	CachedResults.Empty();
}

void FVoxelCPUCubicMesher::SetConfig(const FVoxelMeshingConfig& InConfig)
{
	Config = InConfig;
}

const FVoxelMeshingConfig& FVoxelCPUCubicMesher::GetConfig() const
{
	return Config;
}

bool FVoxelCPUCubicMesher::GetStats(
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
