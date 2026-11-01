// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "VoxelCPUDualContourMesher.h"
#include "VoxelMeshingTypes.h"
#include "ChunkRenderData.h"
#include "VoxelData.h"

/**
 * The meshing suites' shared fixtures (code review CR2 row 17, MESH-18): the test chunk, the two meshers' configurations,
 * a chunk's voxels from a solid predicate, the interior mesh of such a chunk, and the position and edge keys the
 * closed-surface checks weld with. The Dual Contouring seam suites (corner, edge, face, mixed, tuple, interior domain), the
 * crossing-gradient and digest tests and the Marching Cubes LOD and planet tests had a copy each.
 */
namespace VoxelMeshingTestFixtures
{
	constexpr int32 TestChunkSize = 32;
	constexpr float TestVoxelSize = 100.0f;

	/** Dual Contouring as the suites run it: iso 0.5, UVs, no AO, the QEF's SVD threshold 0.1. */
	inline FVoxelMeshingConfig MakeDCConfig()
	{
		FVoxelMeshingConfig Config;
		Config.IsoLevel = 0.5f;
		Config.bGenerateUVs = true;
		Config.bCalculateAO = false;
		Config.QEFSVDThreshold = 0.1f;
		return Config;
	}

	/**
	 * Marching Cubes as the LOD-boundary and planet suites run it: iso 0.5, UVs, no AO, Transvoxel as asked. No skirts: they
	 * would drape extra geometry over the boundary plane and pollute a watertightness measurement.
	 */
	inline FVoxelMeshingConfig MakeMCConfig(bool bUseTransvoxel)
	{
		FVoxelMeshingConfig Config;
		Config.IsoLevel = 0.5f;
		Config.bGenerateUVs = true;
		Config.bCalculateAO = false;
		Config.bUseTransvoxel = bUseTransvoxel;
		return Config;
	}

	/** A test chunk's voxels (solid material 1 or air) from a predicate over global voxel coordinates. */
	inline TArray<FVoxelData> FillVoxels(const FIntVector& ChunkOffsetVoxels,
		TFunctionRef<bool(int32, int32, int32)> SolidFn)
	{
		TArray<FVoxelData> Data;
		Data.SetNumUninitialized(TestChunkSize * TestChunkSize * TestChunkSize);
		for (int32 Z = 0; Z < TestChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < TestChunkSize; ++Y)
			{
				for (int32 X = 0; X < TestChunkSize; ++X)
				{
					const int32 Index = X + Y * TestChunkSize + Z * TestChunkSize * TestChunkSize;
					const bool bSolid = SolidFn(X + ChunkOffsetVoxels.X, Y + ChunkOffsetVoxels.Y, Z + ChunkOffsetVoxels.Z);
					Data[Index] = bSolid ? FVoxelData::Solid(1, 0) : FVoxelData::Air();
				}
			}
		}
		return Data;
	}

	/** FillVoxels as a shared snapshot (a seam request's participant). */
	inline TSharedPtr<const TArray<FVoxelData>> SharedVoxels(const FIntVector& ChunkOffsetVoxels,
		TFunctionRef<bool(int32, int32, int32)> SolidFn)
	{
		return MakeShared<TArray<FVoxelData>>(FillVoxels(ChunkOffsetVoxels, SolidFn));
	}

	/** Flat ground: solid up to voxel layer 12. */
	inline bool FlatGround(int32 X, int32 Y, int32 Z)
	{
		return Z <= 12;
	}

	/** A position's bytes as hex: the exact-position weld key. */
	inline FString PosKey(const FVector3f& P)
	{
		return BytesToHex(reinterpret_cast<const uint8*>(&P), sizeof(FVector3f));
	}

	/** Count each triangle edge of a mesh (offset into a shared frame) by its two positions' keys; a degenerate edge is skipped. */
	inline void AccumulateEdges(const FChunkMeshData& M, const FVector3f& Offset, TMap<FString, int32>& EdgeUse)
	{
		const int32 TriCount = M.Indices.Num() / 3;
		for (int32 T = 0; T < TriCount; ++T)
		{
			FVector3f P[3];
			for (int32 i = 0; i < 3; ++i)
			{
				P[i] = M.Positions[M.Indices[T * 3 + i]] + Offset;
			}
			for (int32 i = 0; i < 3; ++i)
			{
				const FString KA = PosKey(P[i]);
				const FString KB = PosKey(P[(i + 1) % 3]);
				if (KA == KB)
				{
					continue;
				}
				const FString Key = (KA < KB) ? (KA + KB) : (KB + KA);
				EdgeUse.FindOrAdd(Key)++;
			}
		}
	}

	/** The interior Dual Contouring mesh of a test chunk filled from a predicate, at a LOD. */
	inline bool MeshInterior(const FIntVector& ChunkOffsetVoxels, int32 LOD,
		TFunctionRef<bool(int32, int32, int32)> SolidFn, FChunkMeshData& Out)
	{
		FVoxelMeshingRequest Request;
		Request.LODLevel = LOD;
		Request.ChunkSize = TestChunkSize;
		Request.VoxelSize = TestVoxelSize;
		Request.MeshCellDomain = EVoxelMeshCellDomain::Interior;
		Request.VoxelData = FillVoxels(ChunkOffsetVoxels, SolidFn);

		FVoxelCPUDualContourMesher Mesher;
		Mesher.Initialize();
		Mesher.SetConfig(MakeDCConfig());
		const bool bOk = Mesher.GenerateMeshCPU(Request, Out);
		Mesher.Shutdown();
		return bOk;
	}

	/** Run a function over an initialized Dual Contouring mesher with the suites' configuration. */
	template <typename TFn>
	bool RunMesher(TFn&& Fn)
	{
		FVoxelCPUDualContourMesher Mesher;
		Mesher.Initialize();
		Mesher.SetConfig(MakeDCConfig());
		const bool bOk = Fn(Mesher);
		Mesher.Shutdown();
		return bOk;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
