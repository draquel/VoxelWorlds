// Copyright Daniel Raquel. All Rights Reserved.

// Material-border tests for the Dual Contouring meshers (CPU + GPU).
//
// Regression tests for the material-border striping bug: MaterialID travels to
// the pixel shader as a float in UV1.x, which the hardware interpolates across
// each triangle before the shader rounds it back to an integer atlas index. A
// triangle whose corners carry different MaterialIDs therefore sweeps through
// every intermediate index, rendering stripes of unrelated materials along the
// border. The fix assigns materials per-QUAD (from the owned edge's solid
// endpoint) and duplicates cell vertices shared by quads of different
// materials, so every emitted triangle is material-uniform. The GPU mesher
// mirrors this in DualContourMeshGeneration.usf Pass 3 (crossing-stored
// material + duplicate allocation).
//
//   MB1  CPU LOD0 two-material split  -> every triangle uniform, only source IDs emitted
//   MB2  CPU LOD1 (stride 2) split    -> same invariants under LOD striding
//   MB3  GPU LOD0 two-material split  -> GPU parity (requires real RHI)
//   MB4  GPU LOD1 (stride 2) split    -> GPU parity under LOD striding

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelGPUDualContourMesher.h"
#include "VoxelMeshingTypes.h"
#include "VoxelData.h"
#include "RenderingThread.h" // FlushRenderingCommands — driving the GPU mesher's async readback
#include "RHI.h"             // GUsingNullRHI — skip GPU tests under -nullrhi

namespace DualContourMaterialBorderTestHelpers
{
	constexpr int32 TestChunkSize = 32;
	constexpr float TestVoxelSize = 100.0f;

	// The two source materials meeting at the border plane X = 16 voxels.
	// Deliberately non-adjacent IDs so interpolation striping (2..7) would be
	// caught if it ever produced intermediate IDs in the emitted data.
	constexpr uint8 MaterialLeft = 2;
	constexpr uint8 MaterialRight = 7;
	constexpr int32 MaterialSplitX = TestChunkSize / 2;

	/** Gently sloped surface so the isosurface crosses cells at varying heights. */
	FVoxelData SampleLocal(int32 X, int32 Y, int32 Z)
	{
		const float H = 16.0f + 0.05f * static_cast<float>(X) + 3.0f * FMath::Sin(static_cast<float>(Y) * 0.3f);
		const float Normalized = FMath::Clamp(0.5f + (H - static_cast<float>(Z)) / 16.0f, 0.0f, 1.0f);

		FVoxelData Voxel;
		Voxel.Density = static_cast<uint8>(FMath::RoundToInt(Normalized * 255.0f));
		Voxel.MaterialID = (X < MaterialSplitX) ? MaterialLeft : MaterialRight;
		Voxel.BiomeID = 0;
		Voxel.Metadata = 0;
		return Voxel;
	}

	FVoxelMeshingRequest MakeRequest(int32 LODLevel)
	{
		FVoxelMeshingRequest Request;
		Request.ChunkCoord = FIntVector(0, 0, 0);
		Request.LODLevel = LODLevel;
		Request.ChunkSize = TestChunkSize;
		Request.VoxelSize = TestVoxelSize;

		Request.VoxelData.SetNumUninitialized(TestChunkSize * TestChunkSize * TestChunkSize);
		for (int32 Z = 0; Z < TestChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < TestChunkSize; ++Y)
			{
				for (int32 X = 0; X < TestChunkSize; ++X)
				{
					const int32 Index = X + Y * TestChunkSize + Z * TestChunkSize * TestChunkSize;
					Request.VoxelData[Index] = SampleLocal(X, Y, Z);
				}
			}
		}
		return Request;
	}

	FVoxelMeshingConfig MakeConfig()
	{
		FVoxelMeshingConfig Config;
		Config.bUseSmoothMeshing = true;
		Config.IsoLevel = 0.5f;
		Config.bGenerateUVs = true;
		Config.bCalculateAO = false;
		Config.bGenerateSkirts = false;
		Config.QEFSVDThreshold = 0.1f;
		Config.QEFBiasStrength = 0.5f;
		return Config;
	}

	/** Mesh via the CPU Dual Contouring mesher. */
	bool MeshChunkCPU(const FVoxelMeshingRequest& Request, FChunkMeshData& OutMesh)
	{
		FVoxelCPUDualContourMesher Mesher;
		Mesher.Initialize();
		Mesher.SetConfig(MakeConfig());
		const bool bOk = Mesher.GenerateMeshCPU(Request, OutMesh);
		Mesher.Shutdown();
		return bOk;
	}

	/**
	 * Mesh via the GPU Dual Contouring mesher and block until the async readback
	 * completes (same Sleep + FlushRenderingCommands + Tick pump as the GT tests
	 * in DualContourLODBoundaryTests). Requires a real RHI — callers gate on
	 * GUsingNullRHI before invoking this.
	 */
	bool MeshChunkGPU(const FVoxelMeshingRequest& Request, FChunkMeshData& OutMesh)
	{
		FVoxelGPUDualContourMesher Mesher;
		Mesher.Initialize();
		Mesher.SetConfig(MakeConfig());

		volatile bool bCompleted = false;
		volatile bool bSucceeded = false;
		FVoxelMeshingHandle Handle = Mesher.GenerateMeshAsync(Request,
			FOnVoxelMeshingComplete::CreateLambda([&bCompleted, &bSucceeded](FVoxelMeshingHandle, bool bSuccess)
			{
				bSucceeded = bSuccess;
				bCompleted = true;
			}));

		if (!Handle.IsValid())
		{
			Mesher.Shutdown();
			return false;
		}

		// 30s budget: the first GPU DC dispatch after a .usf change pays PSO/shader
		// cold-start costs; subsequent dispatches are sub-100ms.
		const double StartTime = FPlatformTime::Seconds();
		const double TimeoutSeconds = 30.0;
		while (!bCompleted && (FPlatformTime::Seconds() - StartTime) < TimeoutSeconds)
		{
			FlushRenderingCommands();
			Mesher.Tick(0.0f);
			FPlatformProcess::Sleep(0.002f);
		}

		const bool bOk = bCompleted && bSucceeded && Mesher.ReadbackToCPU(Handle, OutMesh);
		Mesher.ReleaseHandle(Handle);
		Mesher.Shutdown();
		return bOk;
	}

	using FDCMeshFn = bool(*)(const FVoxelMeshingRequest&, FChunkMeshData&);

	/** Mesh the two-material chunk and run all material-uniformity invariants. */
	bool RunMaterialBorderInvariants(FAutomationTestBase& Test, int32 LODLevel, FDCMeshFn MeshFn)
	{
		const FVoxelMeshingRequest Request = MakeRequest(LODLevel);
		FChunkMeshData MeshData;
		const bool bSuccess = MeshFn(Request, MeshData);

		Test.TestTrue(TEXT("Meshing succeeded"), bSuccess);
		Test.TestTrue(TEXT("Mesh has vertices"), MeshData.Positions.Num() > 0);
		Test.TestTrue(TEXT("Index count is a multiple of 3"), MeshData.Indices.Num() % 3 == 0);
		Test.TestEqual(TEXT("UV1 count matches vertex count"), MeshData.UV1s.Num(), MeshData.Positions.Num());
		Test.TestEqual(TEXT("Color count matches vertex count"), MeshData.Colors.Num(), MeshData.Positions.Num());
		if (!bSuccess || MeshData.Positions.Num() == 0)
		{
			return false;
		}

		// 1. Only the two source MaterialIDs may appear — never an intermediate.
		//    Colors.R must agree with UV1.x (both feed the shader's ID decode).
		bool bSawLeft = false;
		bool bSawRight = false;
		int32 BadIDCount = 0;
		int32 ColorMismatchCount = 0;
		for (int32 i = 0; i < MeshData.UV1s.Num(); ++i)
		{
			const uint8 MatID = static_cast<uint8>(FMath::RoundToInt(MeshData.UV1s[i].X));
			if (MatID == MaterialLeft) { bSawLeft = true; }
			else if (MatID == MaterialRight) { bSawRight = true; }
			else { BadIDCount++; }

			if (MeshData.Colors[i].R != MatID)
			{
				ColorMismatchCount++;
			}
		}
		Test.TestEqual(TEXT("No vertex carries a non-source MaterialID"), BadIDCount, 0);
		Test.TestEqual(TEXT("Vertex color R channel matches UV1.x MaterialID"), ColorMismatchCount, 0);
		Test.TestTrue(TEXT("Left material present in mesh"), bSawLeft);
		Test.TestTrue(TEXT("Right material present in mesh"), bSawRight);

		// 2. Every triangle is material-uniform: identical UV1.x at all 3 corners.
		//    This is the core anti-striping invariant — a mixed triangle would be
		//    interpolated by the hardware into intermediate material indices.
		int32 MixedTriangleCount = 0;
		const int32 NumTriangles = MeshData.Indices.Num() / 3;
		for (int32 Tri = 0; Tri < NumTriangles; ++Tri)
		{
			const float M0 = MeshData.UV1s[MeshData.Indices[Tri * 3 + 0]].X;
			const float M1 = MeshData.UV1s[MeshData.Indices[Tri * 3 + 1]].X;
			const float M2 = MeshData.UV1s[MeshData.Indices[Tri * 3 + 2]].X;
			if (M0 != M1 || M1 != M2)
			{
				MixedTriangleCount++;
			}
		}
		Test.TestEqual(TEXT("Every triangle is material-uniform (no mixed-ID triangles)"), MixedTriangleCount, 0);

		// 3. Duplicated border vertices must be position-identical to their
		//    primaries — duplication may not open cracks. Verify no triangle
		//    degenerated (zero-area from index aliasing) beyond what DC emits
		//    anyway: all three corner indices of a triangle must be distinct.
		int32 DegenerateIndexCount = 0;
		for (int32 Tri = 0; Tri < NumTriangles; ++Tri)
		{
			const int32 I0 = MeshData.Indices[Tri * 3 + 0];
			const int32 I1 = MeshData.Indices[Tri * 3 + 1];
			const int32 I2 = MeshData.Indices[Tri * 3 + 2];
			if (I0 == I1 || I1 == I2 || I0 == I2)
			{
				DegenerateIndexCount++;
			}
		}
		Test.TestEqual(TEXT("No index-degenerate triangles"), DegenerateIndexCount, 0);

		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCMaterialBorderMB1LOD0Test, "VoxelWorlds.Meshing.DualContour.MaterialBorder.MB1_LOD0_UniformTriangles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCMaterialBorderMB1LOD0Test::RunTest(const FString& Parameters)
{
	using namespace DualContourMaterialBorderTestHelpers;
	RunMaterialBorderInvariants(*this, 0, &MeshChunkCPU);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCMaterialBorderMB2LOD1Test, "VoxelWorlds.Meshing.DualContour.MaterialBorder.MB2_LOD1_UniformTriangles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCMaterialBorderMB2LOD1Test::RunTest(const FString& Parameters)
{
	using namespace DualContourMaterialBorderTestHelpers;
	RunMaterialBorderInvariants(*this, 1, &MeshChunkCPU);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCMaterialBorderMB3GPULOD0Test, "VoxelWorlds.Meshing.DualContour.MaterialBorder.MB3_GPU_LOD0_UniformTriangles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCMaterialBorderMB3GPULOD0Test::RunTest(const FString& Parameters)
{
	using namespace DualContourMaterialBorderTestHelpers;
	if (GUsingNullRHI) { AddInfo(TEXT("Skipped: GPU DC tests require a real RHI (run without -nullrhi)")); return true; }
	RunMaterialBorderInvariants(*this, 0, &MeshChunkGPU);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCMaterialBorderMB4GPULOD1Test, "VoxelWorlds.Meshing.DualContour.MaterialBorder.MB4_GPU_LOD1_UniformTriangles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCMaterialBorderMB4GPULOD1Test::RunTest(const FString& Parameters)
{
	using namespace DualContourMaterialBorderTestHelpers;
	if (GUsingNullRHI) { AddInfo(TEXT("Skipped: GPU DC tests require a real RHI (run without -nullrhi)")); return true; }
	RunMaterialBorderInvariants(*this, 1, &MeshChunkGPU);
	return true;
}
