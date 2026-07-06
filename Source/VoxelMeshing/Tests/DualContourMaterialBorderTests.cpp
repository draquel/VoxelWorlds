// Copyright Daniel Raquel. All Rights Reserved.

// Material-border tests for the CPU Dual Contouring mesher.
//
// Regression tests for the material-border striping bug: MaterialID travels to
// the pixel shader as a float in UV1.x, which the hardware interpolates across
// each triangle before the shader rounds it back to an integer atlas index. A
// triangle whose corners carry different MaterialIDs therefore sweeps through
// every intermediate index, rendering stripes of unrelated materials along the
// border. The fix assigns materials per-QUAD (from the owned edge's solid
// endpoint) and duplicates cell vertices shared by quads of different
// materials, so every emitted triangle is material-uniform.
//
//   MB1  LOD0 two-material split  -> every triangle uniform, only source IDs emitted
//   MB2  LOD1 (stride 2) split    -> same invariants under LOD striding

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelMeshingTypes.h"
#include "VoxelData.h"

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

	/** Mesh the chunk and run all material-uniformity invariants. */
	bool RunMaterialBorderInvariants(FAutomationTestBase& Test, int32 LODLevel)
	{
		FVoxelCPUDualContourMesher Mesher;
		Mesher.Initialize();

		const FVoxelMeshingRequest Request = MakeRequest(LODLevel);
		FChunkMeshData MeshData;
		const bool bSuccess = Mesher.GenerateMeshCPU(Request, MeshData);
		Mesher.Shutdown();

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
	RunMaterialBorderInvariants(*this, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCMaterialBorderMB2LOD1Test, "VoxelWorlds.Meshing.DualContour.MaterialBorder.MB2_LOD1_UniformTriangles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCMaterialBorderMB2LOD1Test::RunTest(const FString& Parameters)
{
	using namespace DualContourMaterialBorderTestHelpers;
	RunMaterialBorderInvariants(*this, 1);
	return true;
}
