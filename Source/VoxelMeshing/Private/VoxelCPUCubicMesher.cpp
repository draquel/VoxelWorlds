// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCPUCubicMesher.h"
#include "VoxelMeshing.h"

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

	const double StartTime = FPlatformTime::Seconds();

	// Reset output
	OutMeshData.Reset();
	OutStats = FVoxelMeshingStats();

	// Pre-allocate arrays (estimate based on typical terrain)
	const int32 EstimatedFaces = Request.ChunkSize * Request.ChunkSize * 6;
	OutMeshData.Positions.Reserve(EstimatedFaces * 4);
	OutMeshData.Normals.Reserve(EstimatedFaces * 4);
	OutMeshData.UVs.Reserve(EstimatedFaces * 4);
	OutMeshData.Colors.Reserve(EstimatedFaces * 4);
	OutMeshData.Indices.Reserve(EstimatedFaces * 6);

	const int32 ChunkSize = Request.ChunkSize;
	uint32 SolidVoxels = 0;
	uint32 CulledFaces = 0;
	uint32 GeneratedFaces = 0;

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

				// Check each face
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
		TEXT("CPU meshing complete: %d verts, %d tris, %d faces (%d culled), %.2fms"),
		OutStats.VertexCount, OutStats.GetTriangleCount(), GeneratedFaces, CulledFaces,
		OutStats.GenerationTimeMs);

	return true;
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
	const int32 SliceSize = ChunkSize * ChunkSize;

	// +X neighbor
	if (X >= ChunkSize && Request.NeighborXPos.Num() == SliceSize)
	{
		const int32 Index = Y + Z * ChunkSize;
		return Request.NeighborXPos[Index];
	}

	// -X neighbor
	if (X < 0 && Request.NeighborXNeg.Num() == SliceSize)
	{
		const int32 Index = Y + Z * ChunkSize;
		return Request.NeighborXNeg[Index];
	}

	// +Y neighbor
	if (Y >= ChunkSize && Request.NeighborYPos.Num() == SliceSize)
	{
		const int32 Index = X + Z * ChunkSize;
		return Request.NeighborYPos[Index];
	}

	// -Y neighbor
	if (Y < 0 && Request.NeighborYNeg.Num() == SliceSize)
	{
		const int32 Index = X + Z * ChunkSize;
		return Request.NeighborYNeg[Index];
	}

	// +Z neighbor
	if (Z >= ChunkSize && Request.NeighborZPos.Num() == SliceSize)
	{
		const int32 Index = X + Y * ChunkSize;
		return Request.NeighborZPos[Index];
	}

	// -Z neighbor
	if (Z < 0 && Request.NeighborZNeg.Num() == SliceSize)
	{
		const int32 Index = X + Y * ChunkSize;
		return Request.NeighborZNeg[Index];
	}

	// No neighbor data available - treat as air (render boundary faces)
	return FVoxelData::Air();
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

		// Pack color: R=MaterialID, G=BiomeID, B=AO, A=Flags
		const uint8 AO = Config.bCalculateAO ? Voxel.GetAO() : 0;
		MeshData.Colors.Add(FColor(Voxel.MaterialID, Voxel.BiomeID, AO * 85, 255));
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
