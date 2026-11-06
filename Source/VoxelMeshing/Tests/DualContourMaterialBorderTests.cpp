// Copyright Daniel Raquel. All Rights Reserved.

// Material-border tests for the Dual Contouring mesher.
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
//   MB5  LOD0 soft border (VP2-O4) -> the blend is flat per triangle, only at the border, and every vertex at one
//        position shows one material (the border fades across quads without a step)
//   MB6  LOD1 soft border          -> the same at stride 2
//   MB7  one material              -> no vertex blends
//
// MB3/MB4, the same invariants on the GPU Dual Contouring mesher, went with that mesher
// (code review CR1, MESH-01/16).

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelMeshingTestFixtures.h"
#include "VoxelMeshingTypes.h"
#include "VoxelData.h"
#include "ChunkRenderData.h"

namespace DualContourMaterialBorderTestHelpers
{
	using namespace VoxelMeshingTestFixtures;

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
		Config.IsoLevel = 0.5f;
		Config.bGenerateUVs = true;
		Config.bCalculateAO = false;
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

	/** The material a vertex shows: its blend's second material where it has the weight, else its own (UV1.x) */
	uint8 ShownMaterial(const FChunkMeshData& MeshData, int32 Vertex)
	{
		const uint16 Code = MeshData.MaterialBlends[Vertex];
		return VoxelMaterialBlend::IsBlend(Code) && VoxelMaterialBlend::ShowsSecondary(Code)
			? VoxelMaterialBlend::Secondary(Code)
			: static_cast<uint8>(FMath::RoundToInt(MeshData.UV1s[Vertex].X));
	}

	/** Mesh the two-material chunk and check the soft border's blend (VP2-O4). */
	bool RunSoftBorderInvariants(FAutomationTestBase& Test, int32 LODLevel)
	{
		const FVoxelMeshingRequest Request = MakeRequest(LODLevel);
		FChunkMeshData MeshData;
		const bool bSuccess = MeshChunkCPU(Request, MeshData);
		Test.TestTrue(TEXT("Meshing succeeded"), bSuccess);
		Test.TestEqual(TEXT("A blend code per vertex"), MeshData.MaterialBlends.Num(), MeshData.Positions.Num());
		if (!bSuccess || MeshData.Positions.Num() == 0 || MeshData.MaterialBlends.Num() != MeshData.Positions.Num())
		{
			return false;
		}

		// 1. The blend is flat per triangle: all three corners in it or none, with one second material, which differs
		//    from the triangle's own. Only the weight may differ between the corners (it interpolates).
		int32 MixedBlendTriangles = 0;
		int32 SelfBlendTriangles = 0;
		int32 BlendTriangles = 0;
		int32 RampTriangles = 0;
		const int32 NumTriangles = MeshData.Indices.Num() / 3;
		for (int32 Tri = 0; Tri < NumTriangles; ++Tri)
		{
			const uint16 C0 = MeshData.MaterialBlends[MeshData.Indices[Tri * 3 + 0]];
			const uint16 C1 = MeshData.MaterialBlends[MeshData.Indices[Tri * 3 + 1]];
			const uint16 C2 = MeshData.MaterialBlends[MeshData.Indices[Tri * 3 + 2]];
			const uint16 Flat = VoxelMaterialBlend::WeightFlag;
			if ((C0 & ~Flat) != (C1 & ~Flat) || (C1 & ~Flat) != (C2 & ~Flat))
			{
				MixedBlendTriangles++;
				continue;
			}
			if (VoxelMaterialBlend::IsBlend(C0))
			{
				BlendTriangles++;
				const uint8 Own = static_cast<uint8>(FMath::RoundToInt(MeshData.UV1s[MeshData.Indices[Tri * 3]].X));
				SelfBlendTriangles += VoxelMaterialBlend::Secondary(C0) == Own ? 1 : 0;
				RampTriangles += (C0 != C1 || C1 != C2) ? 1 : 0;
			}
		}
		Test.TestEqual(TEXT("Every triangle's blend is flat (one second material, all corners or none)"), MixedBlendTriangles, 0);
		Test.TestEqual(TEXT("No triangle blends toward its own material"), SelfBlendTriangles, 0);
		Test.TestTrue(TEXT("The border has blended triangles"), BlendTriangles > 0);
		Test.TestTrue(TEXT("Some blended triangles ramp (their corners' weights differ)"), RampTriangles > 0);

		// 2. Blends only near the border: a vertex's cell is within one cell of the split plane
		const int32 Stride = 1 << LODLevel;
		int32 FarBlendVertices = 0;
		for (int32 i = 0; i < MeshData.Positions.Num(); ++i)
		{
			const float XVoxels = MeshData.Positions[i].X / TestVoxelSize;
			if (VoxelMaterialBlend::IsBlend(MeshData.MaterialBlends[i])
				&& FMath::Abs(XVoxels - static_cast<float>(MaterialSplitX)) > 2.0f * static_cast<float>(Stride))
			{
				FarBlendVertices++;
			}
		}
		Test.TestEqual(TEXT("No vertex blends away from the border"), FarBlendVertices, 0);

		// 3. Every vertex at one position shows one material, whichever quad emitted it: the material changes from cell
		//    to cell, never at a quad's edge
		TMap<FVector3f, uint8> ShownAt;
		int32 Disagreements = 0;
		for (int32 i = 0; i < MeshData.Positions.Num(); ++i)
		{
			const uint8 Shown = ShownMaterial(MeshData, i);
			if (const uint8* Seen = ShownAt.Find(MeshData.Positions[i]))
			{
				Disagreements += *Seen != Shown ? 1 : 0;
			}
			else
			{
				ShownAt.Add(MeshData.Positions[i], Shown);
			}
		}
		Test.TestEqual(TEXT("Vertices at one position show one material"), Disagreements, 0);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCMaterialBorderMB5SoftLOD0Test, "VoxelWorlds.Meshing.DualContour.MaterialBorder.MB5_LOD0_SoftBorder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCMaterialBorderMB5SoftLOD0Test::RunTest(const FString& Parameters)
{
	using namespace DualContourMaterialBorderTestHelpers;
	RunSoftBorderInvariants(*this, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCMaterialBorderMB6SoftLOD1Test, "VoxelWorlds.Meshing.DualContour.MaterialBorder.MB6_LOD1_SoftBorder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCMaterialBorderMB6SoftLOD1Test::RunTest(const FString& Parameters)
{
	using namespace DualContourMaterialBorderTestHelpers;
	RunSoftBorderInvariants(*this, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDCMaterialBorderMB7UniformTest, "VoxelWorlds.Meshing.DualContour.MaterialBorder.MB7_OneMaterialNoBlend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDCMaterialBorderMB7UniformTest::RunTest(const FString& Parameters)
{
	using namespace DualContourMaterialBorderTestHelpers;
	FVoxelMeshingRequest Request = MakeRequest(0);
	for (FVoxelData& Voxel : Request.VoxelData)
	{
		Voxel.MaterialID = MaterialLeft;
	}
	FChunkMeshData MeshData;
	TestTrue(TEXT("Meshing succeeded"), MeshChunkCPU(Request, MeshData));
	TestEqual(TEXT("A blend code per vertex"), MeshData.MaterialBlends.Num(), MeshData.Positions.Num());
	int32 Blended = 0;
	for (const uint16 Code : MeshData.MaterialBlends)
	{
		Blended += VoxelMaterialBlend::IsBlend(Code) ? 1 : 0;
	}
	TestEqual(TEXT("No vertex blends in a one-material chunk"), Blended, 0);
	return true;
}
