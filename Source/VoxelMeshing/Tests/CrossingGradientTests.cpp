// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelMeshingTestFixtures.h"
#include "VoxelCPUMarchingCubesMesher.h"
#include "VoxelMeshingTypes.h"
#include "VoxelData.h"
#include "ChunkRenderData.h"
#include "QEFSolver.h"       // G81: the solve itself (VoxelMeshing/Private)

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G62 (WORLD_MODES_REVIEW_PLAN.md §14.1): the smooth meshers' shading normals at voxel resolution against exact planes
// whose density is the generators' one-voxel ramp of the signed distance along local up (SignedDistanceToDensity: a
// heightmap's vertical distance, a planet's radial one), saturating beyond it. The central difference about the voxel a
// crossing floors to read the saturated rock under the surface, so a plane's normals tilted by 1.0-1.8 times its slope
// with the surface's phase in the voxel grid: bands. The fitted gradient is exact for a plane at
// any phase. Pure CPU.
// ---------------------------------------------------------------------------

namespace VoxelCrossingGradientTestUtils
{
	using namespace VoxelMeshingTestFixtures;

	constexpr int32 ChunkSize = 32;
	constexpr float VoxelSize = 100.0f;

	/** One test plane: the density ramps along Up, the surface tilted by SlopeDeg toward Tangent, at a phase Offset. */
	struct FRampPlane
	{
		FVector3f Up;
		FVector3f Tangent;
		float SlopeDeg = 0.0f;
		float Offset = 0.0f;

		/** g: the signed distance falls by g . p per voxel (Up minus the slope along Tangent); the normal is g's direction. */
		FVector3f Gradient() const { return Up - Tangent * FMath::Tan(FMath::DegreesToRadians(SlopeDeg)); }
		FVector3f Normal() const { return Gradient().GetSafeNormal(); }
	};

	/** Every test plane: heightmap planes at 5, 15 and 30 degrees, a planet's tangent planes at 45N 20E (flat and 15). */
	TArray<FRampPlane> MakePlanes()
	{
		TArray<FRampPlane> Planes;
		const FVector3f Z(0.0f, 0.0f, 1.0f);
		const float Offsets[3] = { 0.13f, 0.47f, 0.81f };
		const float Slopes[3] = { 5.0f, 15.0f, 30.0f };
		for (int32 S = 0; S < 3; ++S)
		{
			for (int32 O = 0; O < 3; ++O)
			{
				const float Azimuth = FMath::DegreesToRadians(17.0f + 47.0f * (S * 3 + O));
				Planes.Add({ Z, FVector3f(FMath::Cos(Azimuth), FMath::Sin(Azimuth), 0.0f), Slopes[S], Offsets[O] });
			}
		}
		const float Lat = FMath::DegreesToRadians(45.0f);
		const float Lon = FMath::DegreesToRadians(20.0f);
		const FVector3f Up(FMath::Cos(Lat) * FMath::Cos(Lon), FMath::Cos(Lat) * FMath::Sin(Lon), FMath::Sin(Lat));
		const FVector3f East = FVector3f::CrossProduct(Z, Up).GetSafeNormal();
		const FVector3f North = FVector3f::CrossProduct(Up, East);
		for (int32 O = 0; O < 3; ++O)
		{
			Planes.Add({ Up, East, 0.0f, Offsets[O] });
			Planes.Add({ Up, (East + North).GetSafeNormal(), 15.0f, Offsets[O] });
		}
		return Planes;
	}

	/** A chunk of the plane's ramp density (uint8 truncation as SignedDistanceToDensity), surface through the centre. */
	FVoxelMeshingRequest MakeRequest(const FRampPlane& Plane, int32 LODLevel)
	{
		FVoxelMeshingRequest Request;
		Request.ChunkCoord = FIntVector::ZeroValue;
		Request.ChunkSize = ChunkSize;
		Request.VoxelSize = VoxelSize;
		Request.LODLevel = LODLevel;
		Request.VoxelData.SetNum(ChunkSize * ChunkSize * ChunkSize);
		const FVector3f G = Plane.Gradient();
		const FVector3f Centre(ChunkSize * 0.5f, ChunkSize * 0.5f, ChunkSize * 0.5f);
		const float C = FVector3f::DotProduct(G, Centre) + Plane.Offset;
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < ChunkSize; ++Y)
			{
				for (int32 X = 0; X < ChunkSize; ++X)
				{
					const float SignedDistance = C - FVector3f::DotProduct(G, FVector3f(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z)));
					const float Density = (FMath::Clamp(SignedDistance, -1.0f, 1.0f) + 1.0f) * 127.5f;
					Request.VoxelData[X + Y * ChunkSize + Z * ChunkSize * ChunkSize] =
						FVoxelData(1, static_cast<uint8>(FMath::Clamp(Density, 0.0f, 255.0f)));
				}
			}
		}
		return Request;
	}

	/**
	 * Vertex normals against the plane's, over vertices more than a margin inside the chunk (no neighbour data here): the
	 * mean and largest angle, and the band, the spread of the mean angle over eight bins of the vertex's phase in the voxel
	 * grid along the normal's dominant axis (G62 made the error a function of that phase).
	 */
	struct FPlaneNormalStats
	{
		int32 Count = 0;
		double Sum = 0.0;
		double Max = 0.0;
		double BinSum[8] = {};
		int32 BinCount[8] = {};

		void Add(const FChunkMeshData& Mesh, const FVector3f& Normal, float Margin)
		{
			int32 Dominant = 0;
			for (int32 Axis = 1; Axis < 3; ++Axis)
			{
				if (FMath::Abs(Normal[Axis]) > FMath::Abs(Normal[Dominant]))
				{
					Dominant = Axis;
				}
			}
			for (int32 i = 0; i < Mesh.Positions.Num() && i < Mesh.Normals.Num(); ++i)
			{
				const FVector3f V = Mesh.Positions[i] / VoxelSize;
				if (V.GetMin() < Margin || V.GetMax() > ChunkSize - Margin)
				{
					continue;
				}
				const double Angle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
					static_cast<double>(FVector3f::DotProduct(Mesh.Normals[i].GetSafeNormal(), Normal)), -1.0, 1.0)));
				const double Phase = V[Dominant] - FMath::FloorToDouble(V[Dominant]);
				const int32 Bin = FMath::Clamp(static_cast<int32>(Phase * 8.0), 0, 7);
				++Count;
				Sum += Angle;
				Max = FMath::Max(Max, Angle);
				BinSum[Bin] += Angle;
				++BinCount[Bin];
			}
		}

		double Mean() const { return Count > 0 ? Sum / Count : 0.0; }

		/** Largest minus smallest bin mean over bins holding at least 20 vertices (degrees). */
		double Band() const
		{
			double Lo = TNumericLimits<double>::Max();
			double Hi = -TNumericLimits<double>::Max();
			for (int32 Bin = 0; Bin < 8; ++Bin)
			{
				if (BinCount[Bin] >= 20)
				{
					const double M = BinSum[Bin] / BinCount[Bin];
					Lo = FMath::Min(Lo, M);
					Hi = FMath::Max(Hi, M);
				}
			}
			return Hi >= Lo ? Hi - Lo : 0.0;
		}

		FString Describe() const
		{
			return FString::Printf(TEXT("%d vertices, mean %.3f deg, max %.2f, band %.3f"), Count, Mean(), Max, Band());
		}
	};
}

/**
 * G62 on exact planes: with the fitted gradient Dual Contouring's and Marching Cubes' vertex normals at LOD 0, 1 and 2
 * (LOD 1 and 2 take LOD 0's estimator at a voxel crossing: G61, G80) are within a quarter of a degree of the plane's on
 * average with no band over the surface's phase in the voxel grid (the central difference before G62 banded LOD 0 by
 * more than a degree in both meshers).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCrossingGradientPlanesTest, "VoxelWorlds.Meshing.CrossingGradient.Planes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelCrossingGradientPlanesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCrossingGradientTestUtils;

	FVoxelCPUDualContourMesher DCMesher;
	DCMesher.Initialize();
	DCMesher.SetConfig(MakeDCConfig());
	FVoxelCPUMarchingCubesMesher MCMesher;
	MCMesher.Initialize();
	MCMesher.SetConfig(MakeDCConfig());
	const TArray<FRampPlane> Planes = MakePlanes();
	// [Mesher: 0 DC, 1 MC][LOD]
	FPlaneNormalStats Stats[2][3];
	for (int32 LOD = 0; LOD < 3; ++LOD)
	{
		const float Margin = 1.5f * (1 << LOD) + 1.0f;
		for (const FRampPlane& Plane : Planes)
		{
			const FVoxelMeshingRequest Request = MakeRequest(Plane, LOD);
			FChunkMeshData Meshes[2];
			TestTrue(TEXT("DC meshing succeeds"), DCMesher.GenerateMeshCPU(Request, Meshes[0]));
			TestTrue(TEXT("MC meshing succeeds"), MCMesher.GenerateMeshCPU(Request, Meshes[1]));
			for (int32 Mesher = 0; Mesher < 2; ++Mesher)
			{
				Stats[Mesher][LOD].Add(Meshes[Mesher], Plane.Normal(), Margin);
			}
		}
	}

	static const TCHAR* Names[2] = { TEXT("DC"), TEXT("MC") };
	for (int32 Mesher = 0; Mesher < 2; ++Mesher)
	{
		for (int32 LOD = 0; LOD < 3; ++LOD)
		{
			const FPlaneNormalStats& After = Stats[Mesher][LOD];
			AddInfo(FString::Printf(TEXT("%s LOD %d: %s"), Names[Mesher], LOD, *After.Describe()));
			TestTrue(*FString::Printf(TEXT("%s LOD %d: vertices measured (%d)"), Names[Mesher], LOD, After.Count), After.Count > (1000 >> (2 * LOD)));
			TestTrue(*FString::Printf(TEXT("%s LOD %d: normals within a quarter degree of the plane's (mean %.3f)"), Names[Mesher], LOD, After.Mean()),
				After.Mean() < 0.25);
			TestTrue(*FString::Printf(TEXT("%s LOD %d: no band over the voxel phase (%.3f deg)"), Names[Mesher], LOD, After.Band()),
				After.Band() < 0.25);
		}
	}
	MCMesher.Shutdown();
	DCMesher.Shutdown();
	return true;
}

// ---------------------------------------------------------------------------
// G80 (VISUAL_POLISH_PLAN.md VP5, the LOD ribs): the smooth meshers' vertex positions at LOD 1 and 2 against the same
// exact planes. A strided edge interpolated between its two samples a stride apart reads them saturated (the ramp is a
// voxel either side of the surface), so its crossing lands toward the edge's middle by where the plane crosses the
// strided grid: the vertices' distance from the plane runs with that phase (terraces a stride apart). The crossing is
// LOD 0's on the voxel step along the edge that changes sign, which lies on a plane to the density's 8-bit step.
// ---------------------------------------------------------------------------

namespace VoxelCrossingGradientTestUtils
{
	/**
	 * Vertex distances from the plane (voxels, along its normal), over vertices more than a margin inside the chunk: the
	 * mean and largest magnitude, and the band, the spread of the mean signed distance over eight bins of the vertex's
	 * phase in its LOD's cell along the normal's dominant axis.
	 */
	struct FPlanePositionStats
	{
		int32 Count = 0;
		double AbsSum = 0.0;
		double Max = 0.0;
		double BinSum[8] = {};
		int32 BinCount[8] = {};

		void Add(const FChunkMeshData& Mesh, const FRampPlane& Plane, int32 LODLevel, float Margin)
		{
			const FVector3f G = Plane.Gradient();
			const float GLength = G.Size();
			const FVector3f Normal = Plane.Normal();
			const FVector3f Centre(ChunkSize * 0.5f, ChunkSize * 0.5f, ChunkSize * 0.5f);
			// MakeRequest's surface: SignedDistance = C - G . p = 0.
			const float C = FVector3f::DotProduct(G, Centre) + Plane.Offset;
			int32 Dominant = 0;
			for (int32 Axis = 1; Axis < 3; ++Axis)
			{
				if (FMath::Abs(Normal[Axis]) > FMath::Abs(Normal[Dominant]))
				{
					Dominant = Axis;
				}
			}
			const double Cell = static_cast<double>(1 << LODLevel);
			for (const FVector3f& Position : Mesh.Positions)
			{
				const FVector3f V = Position / VoxelSize;
				if (V.GetMin() < Margin || V.GetMax() > ChunkSize - Margin)
				{
					continue;
				}
				const double Distance = static_cast<double>(FVector3f::DotProduct(G, V) - C) / GLength;
				const double Local = static_cast<double>(V[Dominant]) / Cell;
				const int32 Bin = FMath::Clamp(static_cast<int32>((Local - FMath::FloorToDouble(Local)) * 8.0), 0, 7);
				++Count;
				AbsSum += FMath::Abs(Distance);
				Max = FMath::Max(Max, FMath::Abs(Distance));
				BinSum[Bin] += Distance;
				++BinCount[Bin];
			}
		}

		double Mean() const { return Count > 0 ? AbsSum / Count : 0.0; }

		/** Largest minus smallest bin mean over bins holding at least 10 vertices (voxels). */
		double Band() const
		{
			double Lo = TNumericLimits<double>::Max();
			double Hi = -TNumericLimits<double>::Max();
			for (int32 Bin = 0; Bin < 8; ++Bin)
			{
				if (BinCount[Bin] >= 10)
				{
					const double M = BinSum[Bin] / BinCount[Bin];
					Lo = FMath::Min(Lo, M);
					Hi = FMath::Max(Hi, M);
				}
			}
			return Hi >= Lo ? Hi - Lo : 0.0;
		}

		FString Describe() const
		{
			return FString::Printf(TEXT("%d vertices, mean %.4f voxels from the plane, max %.4f, band %.4f"), Count, Mean(), Max, Band());
		}
	};
}

/**
 * G80 on exact planes: Dual Contouring's and Marching Cubes' vertices at LOD 1 and 2 against the plane. Voxel crossings
 * put Marching Cubes' vertices on the plane (the crossing itself) and Dual Contouring's as close as LOD 0's, with no band
 * (the strided crossings before G80 banded by more than a quarter voxel at LOD 2 in both meshers).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelLODCrossingPlanesTest, "VoxelWorlds.Meshing.LODCrossings.Planes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelLODCrossingPlanesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCrossingGradientTestUtils;

	FVoxelCPUDualContourMesher DCMesher;
	DCMesher.Initialize();
	DCMesher.SetConfig(MakeDCConfig());
	FVoxelCPUMarchingCubesMesher MCMesher;
	MCMesher.Initialize();
	MCMesher.SetConfig(MakeDCConfig());
	const TArray<FRampPlane> Planes = MakePlanes();
	// [Mesher: 0 DC, 1 MC][LOD]
	FPlanePositionStats Stats[2][3];
	for (int32 LOD = 0; LOD < 3; ++LOD)
	{
		const float Margin = 1.5f * (1 << LOD) + 1.0f;
		for (const FRampPlane& Plane : Planes)
		{
			const FVoxelMeshingRequest Request = MakeRequest(Plane, LOD);
			FChunkMeshData Meshes[2];
			TestTrue(TEXT("DC meshing succeeds"), DCMesher.GenerateMeshCPU(Request, Meshes[0]));
			TestTrue(TEXT("MC meshing succeeds"), MCMesher.GenerateMeshCPU(Request, Meshes[1]));
			for (int32 Mesher = 0; Mesher < 2; ++Mesher)
			{
				Stats[Mesher][LOD].Add(Meshes[Mesher], Plane, LOD, Margin);
			}
		}
	}

	static const TCHAR* Names[2] = { TEXT("DC"), TEXT("MC") };
	for (int32 Mesher = 0; Mesher < 2; ++Mesher)
	{
		for (int32 LOD = 0; LOD < 3; ++LOD)
		{
			const FPlanePositionStats& After = Stats[Mesher][LOD];
			AddInfo(FString::Printf(TEXT("%s LOD %d: %s"), Names[Mesher], LOD, *After.Describe()));
			TestTrue(*FString::Printf(TEXT("%s LOD %d: vertices measured (%d)"), Names[Mesher], LOD, After.Count), After.Count > (1000 >> (2 * LOD)));
		}
		for (int32 LOD = 1; LOD < 3; ++LOD)
		{
			const FPlanePositionStats& After = Stats[Mesher][LOD];
			const FPlanePositionStats& LOD0 = Stats[Mesher][0];
			TestTrue(*FString::Printf(TEXT("%s LOD %d: no farther from the plane than LOD 0 (mean %.4f against %.4f voxels)"),
				Names[Mesher], LOD, After.Mean(), LOD0.Mean()), After.Mean() <= LOD0.Mean() * 1.5 + 0.01);
			TestTrue(*FString::Printf(TEXT("%s LOD %d: band under 0.05 voxels (%.4f)"), Names[Mesher], LOD, After.Band()),
				After.Band() < 0.05);
		}
		if (Mesher == 1)
		{
			for (int32 LOD = 1; LOD < 3; ++LOD)
			{
				TestTrue(*FString::Printf(TEXT("MC LOD %d: vertices on the plane (max %.4f voxels)"), LOD, Stats[1][LOD].Max),
					Stats[1][LOD].Max < 0.02);
			}
		}
	}
	MCMesher.Shutdown();
	DCMesher.Shutdown();
	return true;
}

// ---------------------------------------------------------------------------
// G81 (WORLD_MODES_REVIEW_PLAN.md §23.16): Dual Contouring's QEF solve. Its Jacobi decomposition zeroed each rotation's
// pivot above the diagonal only, so later rotations read the stale element below and converged to wrong eigenpairs, and
// the pseudoinverse solved for the position itself, so a direction the SVD threshold dropped went to the frame's origin.
// Found measuring G80: a stride-2 cell on a planet's flat tangent plane put its vertex two voxels off it.
// ---------------------------------------------------------------------------

/**
 * G81 on single cells: the cell G80's planes test found (three crossings on a planet's flat tangent plane at 45N 20E, a
 * stride-2 cell of 100 uu voxels) solves onto the plane, at its mass point (the solve before G81 put it more than a voxel
 * off the plane). A corner (three orthogonal planes meeting inside the cell) solves to the corner, and a cell far from
 * the origin solves onto its plane wherever it is.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelQEFSolveTest, "VoxelWorlds.Meshing.QEF.Solve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelQEFSolveTest::RunTest(const FString& Parameters)
{
	constexpr float VoxelSize = 100.0f;
	constexpr float SVDThreshold = 0.1f;
	constexpr float BiasStrength = 0.5f;

	// The planet cell: crossings and the fitted normal as G80's planes test logged them (voxels).
	{
		const FVector3f Normal = FVector3f(0.664f, 0.242f, 0.708f).GetSafeNormal();
		const FVector3f Points[3] = { FVector3f(16.0f, 12.0f, 17.55f), FVector3f(16.0f, 10.661f, 18.0f), FVector3f(15.518f, 12.0f, 18.0f) };
		FQEFSolver QEF;
		for (const FVector3f& P : Points)
		{
			// The plane through the first crossing with this normal: snap the others onto it, so the plane is exact.
			const FVector3f OnPlane = P - Normal * FVector3f::DotProduct(Normal, P - Points[0]);
			QEF.Add(OnPlane * VoxelSize, Normal);
		}
		const FBox3f Cell(FVector3f(14.0f, 10.0f, 16.0f) * VoxelSize, FVector3f(16.0f, 12.0f, 18.0f) * VoxelSize);
		const FVector3f MassPoint = QEF.MassPoint / static_cast<float>(QEF.Count);
		const FVector3f After = QEF.Solve(SVDThreshold, Cell, BiasStrength);
		const float AfterOff = FMath::Abs(FVector3f::DotProduct(Normal, After - Points[0] * VoxelSize)) / VoxelSize;
		AddInfo(FString::Printf(TEXT("Planet cell: %.4f voxels off the plane, %.4f from the mass point"),
			AfterOff, FVector3f::Dist(After, MassPoint) / VoxelSize));
		TestTrue(*FString::Printf(TEXT("Planet cell: on the plane (%.4f voxels)"), AfterOff), AfterOff < 0.001f);
		TestTrue(TEXT("Planet cell: at its mass point"), FVector3f::Dist(After, MassPoint) < 0.001f * VoxelSize);
	}

	// A corner: three orthogonal planes through (5.3, 6.2, 7.7) voxels, inside the cell [4, 8]^3.
	{
		const FVector3f Corner = FVector3f(5.3f, 6.2f, 7.7f) * VoxelSize;
		FQEFSolver QEF;
		QEF.Add(Corner + FVector3f(0.0f, 0.7f, -0.9f) * VoxelSize, FVector3f(1.0f, 0.0f, 0.0f));
		QEF.Add(Corner + FVector3f(-0.8f, 0.0f, -0.5f) * VoxelSize, FVector3f(0.0f, 1.0f, 0.0f));
		QEF.Add(Corner + FVector3f(-0.6f, 0.4f, 0.0f) * VoxelSize, FVector3f(0.0f, 0.0f, 1.0f));
		const FBox3f Cell(FVector3f(4.0f) * VoxelSize, FVector3f(8.0f) * VoxelSize);
		const FVector3f V = QEF.Solve(SVDThreshold, Cell, BiasStrength);
		TestTrue(*FString::Printf(TEXT("Corner: at the corner (%.4f voxels)"), FVector3f::Dist(V, Corner) / VoxelSize),
			FVector3f::Dist(V, Corner) < 0.001f * VoxelSize);
	}

	// A tilted plane in cells along a line away from the origin: the solve is on the plane in every one.
	{
		const FVector3f Normal = FVector3f(0.3f, -0.5f, 0.81f).GetSafeNormal();
		float Worst = 0.0f;
		for (int32 Step = 0; Step < 16; ++Step)
		{
			const FVector3f Base = FVector3f(3.0f, 5.0f, 2.0f) * static_cast<float>(Step) * 4.0f;
			const FVector3f Centre = Base + FVector3f(2.0f);
			FQEFSolver QEF;
			const FVector3f Tangent1 = FVector3f::CrossProduct(Normal, FVector3f(1.0f, 0.0f, 0.0f)).GetSafeNormal();
			const FVector3f Tangent2 = FVector3f::CrossProduct(Normal, Tangent1);
			const FVector2f Offsets[4] = { FVector2f(1.1f, 0.2f), FVector2f(-0.7f, 0.9f), FVector2f(-0.4f, -1.2f), FVector2f(0.8f, -0.6f) };
			for (const FVector2f& O : Offsets)
			{
				QEF.Add((Centre + Tangent1 * O.X + Tangent2 * O.Y) * VoxelSize, Normal);
			}
			const FBox3f Cell(Base * VoxelSize, (Base + FVector3f(4.0f)) * VoxelSize);
			const FVector3f V = QEF.Solve(SVDThreshold, Cell, BiasStrength);
			Worst = FMath::Max(Worst, FMath::Abs(FVector3f::DotProduct(Normal, V - Centre * VoxelSize)) / VoxelSize);
		}
		TestTrue(*FString::Printf(TEXT("A plane in 16 cells to 1,900 voxels out: on it in every one (worst %.4f voxels)"), Worst), Worst < 0.01f);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
