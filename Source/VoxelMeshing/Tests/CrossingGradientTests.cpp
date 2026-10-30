// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelCPUMarchingCubesMesher.h"
#include "VoxelGPUMarchingCubesMesher.h"
#include "VoxelMeshingTypes.h"
#include "VoxelData.h"
#include "ChunkRenderData.h"
#include "RenderingThread.h"
#include "RHI.h"             // GUsingNullRHI: the GPU half skips under -nullrhi
#include "HAL/IConsoleManager.h"
#include "QEFSolver.h"       // G81: the solve itself (VoxelMeshing/Private)

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G62 (WORLD_MODES_REVIEW_PLAN.md §14.1): the smooth meshers' shading normals at voxel resolution against exact planes
// whose density is the generators' one-voxel ramp of the signed distance along local up (SignedDistanceToDensity: a
// heightmap's vertical distance, a planet's radial one), saturating beyond it. The central difference about the voxel a
// crossing floors to read the saturated rock under the surface, so a plane's normals tilted by 1.0-1.8 times its slope
// with the surface's phase in the voxel grid: bands. The fitted gradient (voxel.Mesh.CrossingGradientNormals 1) is exact
// for a plane at any phase. Pure CPU but for the GPU Marching Cubes half (a real RHI).
// ---------------------------------------------------------------------------

namespace VoxelCrossingGradientTestUtils
{
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

	FVoxelMeshingConfig MakeConfig()
	{
		FVoxelMeshingConfig Config;
		Config.bUseSmoothMeshing = true;
		Config.IsoLevel = 0.5f;
		Config.bGenerateUVs = true;
		Config.bCalculateAO = false;
		Config.bGenerateSkirts = false;
		Config.QEFSVDThreshold = 0.1f;
		return Config;
	}

	/** Sets voxel.Mesh.CrossingGradientNormals at console priority for a scope. */
	struct FScopedCrossingGradient
	{
		IConsoleVariable* Var = nullptr;
		int32 Previous = 1;

		explicit FScopedCrossingGradient(int32 Value)
		{
			Var = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Mesh.CrossingGradientNormals"));
			if (Var)
			{
				Previous = Var->GetInt();
				Var->Set(Value, ECVF_SetByConsole);
			}
		}

		~FScopedCrossingGradient()
		{
			if (Var)
			{
				Var->Set(Previous, ECVF_SetByConsole);
			}
		}
	};

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

	bool PositionsIdentical(const FChunkMeshData& A, const FChunkMeshData& B)
	{
		return A.Positions.Num() == B.Positions.Num() && A.Indices.Num() == B.Indices.Num()
			&& (A.Positions.Num() == 0 || FMemory::Memcmp(A.Positions.GetData(), B.Positions.GetData(), A.Positions.Num() * sizeof(FVector3f)) == 0)
			&& (A.Indices.Num() == 0 || FMemory::Memcmp(A.Indices.GetData(), B.Indices.GetData(), A.Indices.Num() * sizeof(uint32)) == 0);
	}

	bool MeshGPU(FVoxelGPUMarchingCubesMesher& GPUMesher, const FVoxelMeshingRequest& Request, FChunkMeshData& Out)
	{
		volatile bool bCompleted = false;
		FVoxelMeshingHandle Handle = GPUMesher.GenerateMeshAsync(Request,
			FOnVoxelMeshingComplete::CreateLambda([&bCompleted](FVoxelMeshingHandle, bool) { bCompleted = true; }));
		const double StartTime = FPlatformTime::Seconds();
		while (!bCompleted && (FPlatformTime::Seconds() - StartTime) < 30.0)
		{
			FlushRenderingCommands();
			GPUMesher.Tick(0.0f);
			FPlatformProcess::Sleep(0.002f);
		}
		const bool bOk = bCompleted && GPUMesher.ReadbackToCPU(Handle, Out);
		GPUMesher.ReleaseHandle(Handle);
		return bOk;
	}
}

/**
 * G62 on exact planes: with the fitted gradient Dual Contouring's and Marching Cubes' vertex normals at LOD 0, 1 and 2
 * (LOD 1 and 2 take LOD 0's estimator at a voxel crossing: G44, G61) are within a quarter of a degree of the plane's on
 * average with no band over the surface's phase in the voxel grid; with the central difference before G62 (0) LOD 0
 * bands by more than a degree in both meshers. Marching Cubes' vertex positions are the same either way (Dual
 * Contouring's QEF takes the fitted gradient since G80 and G81). With a real RHI the GPU Marching Cubes mesher's normals
 * match the CPU's in both settings.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCrossingGradientPlanesTest, "VoxelWorlds.Meshing.CrossingGradient.Planes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelCrossingGradientPlanesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCrossingGradientTestUtils;

	FVoxelCPUDualContourMesher DCMesher;
	DCMesher.Initialize();
	DCMesher.SetConfig(MakeConfig());
	FVoxelCPUMarchingCubesMesher MCMesher;
	MCMesher.Initialize();
	MCMesher.SetConfig(MakeConfig());
	const bool bGPU = !GUsingNullRHI;
	FVoxelGPUMarchingCubesMesher GPUMesher;
	if (bGPU)
	{
		GPUMesher.Initialize();
		GPUMesher.SetConfig(MakeConfig());
	}
	else
	{
		AddInfo(TEXT("GPU half skipped: it needs a real RHI (run without -nullrhi)"));
	}

	const TArray<FRampPlane> Planes = MakePlanes();
	// [Mode][Mesher: 0 DC, 1 MC][LOD]
	FPlaneNormalStats Stats[2][2][3];
	bool bPositionsSame[2][3] = { { true, true, true }, { true, true, true } };
	double GPUSum[2][3] = {}, GPUMax[2][3] = {};
	int32 GPUMatched[2][3] = {}, GPUVertices[2][3] = {};
	for (int32 LOD = 0; LOD < 3; ++LOD)
	{
		const float Margin = 1.5f * (1 << LOD) + 1.0f;
		for (const FRampPlane& Plane : Planes)
		{
			const FVoxelMeshingRequest Request = MakeRequest(Plane, LOD);
			FChunkMeshData Meshes[2][2];
			for (int32 Mode = 0; Mode < 2; ++Mode)
			{
				FScopedCrossingGradient Scoped(Mode);
				TestTrue(TEXT("DC meshing succeeds"), DCMesher.GenerateMeshCPU(Request, Meshes[Mode][0]));
				TestTrue(TEXT("MC meshing succeeds"), MCMesher.GenerateMeshCPU(Request, Meshes[Mode][1]));
				for (int32 Mesher = 0; Mesher < 2; ++Mesher)
				{
					Stats[Mode][Mesher][LOD].Add(Meshes[Mode][Mesher], Plane.Normal(), Margin);
				}
				if (!bGPU)
				{
					continue;
				}
				FChunkMeshData GPUMesh;
				TestTrue(TEXT("GPU MC meshing and readback succeed"), MeshGPU(GPUMesher, Request, GPUMesh));
				// Each GPU vertex against the CPU vertex at its position (triangle soup: every match carries the cell's normal).
				TMap<FIntVector, FVector3f> CPUNormals;
				const FChunkMeshData& CPUMesh = Meshes[Mode][1];
				for (int32 i = 0; i < CPUMesh.Positions.Num(); ++i)
				{
					const FVector3f& P = CPUMesh.Positions[i];
					CPUNormals.Add(FIntVector(FMath::RoundToInt32(P.X), FMath::RoundToInt32(P.Y), FMath::RoundToInt32(P.Z)), CPUMesh.Normals[i]);
				}
				GPUVertices[Mode][LOD] += GPUMesh.Positions.Num();
				for (int32 i = 0; i < GPUMesh.Positions.Num() && i < GPUMesh.Normals.Num(); ++i)
				{
					const FVector3f& P = GPUMesh.Positions[i];
					const FVector3f* N = CPUNormals.Find(FIntVector(FMath::RoundToInt32(P.X), FMath::RoundToInt32(P.Y), FMath::RoundToInt32(P.Z)));
					if (!N)
					{
						continue;
					}
					const double Diff = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
						static_cast<double>(FVector3f::DotProduct(N->GetSafeNormal(), GPUMesh.Normals[i].GetSafeNormal())), -1.0, 1.0)));
					GPUSum[Mode][LOD] += Diff;
					GPUMax[Mode][LOD] = FMath::Max(GPUMax[Mode][LOD], Diff);
					++GPUMatched[Mode][LOD];
				}
			}
			for (int32 Mesher = 0; Mesher < 2; ++Mesher)
			{
				bPositionsSame[Mesher][LOD] &= PositionsIdentical(Meshes[0][Mesher], Meshes[1][Mesher]);
			}
		}
	}

	static const TCHAR* Names[2] = { TEXT("DC"), TEXT("MC") };
	for (int32 Mesher = 0; Mesher < 2; ++Mesher)
	{
		for (int32 LOD = 0; LOD < 3; ++LOD)
		{
			const FPlaneNormalStats& Before = Stats[0][Mesher][LOD];
			const FPlaneNormalStats& After = Stats[1][Mesher][LOD];
			AddInfo(FString::Printf(TEXT("%s LOD %d: before G62 %s; after %s"), Names[Mesher], LOD, *Before.Describe(), *After.Describe()));
			TestTrue(*FString::Printf(TEXT("%s LOD %d: vertices measured (%d)"), Names[Mesher], LOD, After.Count), After.Count > (1000 >> (2 * LOD)));
			// Dual Contouring's QEF takes the fitted gradient since G80 (a stride) and G81 (stride 1), so its vertices move
			// with the setting (onto the plane: VoxelWorlds.Meshing.LODCrossings.Planes); Marching Cubes' do not.
			if (Mesher == 1)
			{
				TestTrue(*FString::Printf(TEXT("%s LOD %d: positions the same in either setting"), Names[Mesher], LOD), bPositionsSame[Mesher][LOD]);
			}
			TestTrue(*FString::Printf(TEXT("%s LOD %d: normals within a quarter degree of the plane's (mean %.3f)"), Names[Mesher], LOD, After.Mean()),
				After.Mean() < 0.25);
			TestTrue(*FString::Printf(TEXT("%s LOD %d: no band over the voxel phase (%.3f deg)"), Names[Mesher], LOD, After.Band()),
				After.Band() < 0.25);
		}
		TestTrue(*FString::Printf(TEXT("%s LOD 0: the central difference before G62 bands (%.2f deg)"), Names[Mesher], Stats[0][Mesher][0].Band()),
			Stats[0][Mesher][0].Band() > 1.0);
	}
	if (bGPU)
	{
		for (int32 Mode = 0; Mode < 2; ++Mode)
		{
			for (int32 LOD = 0; LOD < 3; ++LOD)
			{
				const double Mean = GPUMatched[Mode][LOD] > 0 ? GPUSum[Mode][LOD] / GPUMatched[Mode][LOD] : 0.0;
				AddInfo(FString::Printf(TEXT("GPU MC, setting %d LOD %d: %d of %d vertices at a CPU vertex, normals %.3f deg apart on average, %.3f at most"),
					Mode, LOD, GPUMatched[Mode][LOD], GPUVertices[Mode][LOD], Mean, GPUMax[Mode][LOD]));
				TestTrue(*FString::Printf(TEXT("GPU MC, setting %d LOD %d: most GPU vertices at a CPU vertex"), Mode, LOD),
					GPUMatched[Mode][LOD] * 10 >= GPUVertices[Mode][LOD] * 9);
				TestTrue(*FString::Printf(TEXT("GPU MC, setting %d LOD %d: GPU normals match the CPU's (mean %.3f deg)"), Mode, LOD, Mean),
					GPUMatched[Mode][LOD] > 0 && Mean < 0.1);
			}
		}
		GPUMesher.Shutdown();
	}
	MCMesher.Shutdown();
	DCMesher.Shutdown();
	return true;
}

// ---------------------------------------------------------------------------
// G80 (VISUAL_POLISH_PLAN.md VP5, the LOD ribs): the smooth meshers' vertex positions at LOD 1 and 2 against the same
// exact planes. A strided edge interpolated between its two samples a stride apart reads them saturated (the ramp is a
// voxel either side of the surface), so its crossing lands toward the edge's middle by where the plane crosses the
// strided grid: the vertices' distance from the plane runs with that phase (terraces a stride apart). With
// voxel.Mesh.VoxelLODCrossings the crossing is LOD 0's on the voxel step along the edge that changes sign, which lies
// on a plane to the density's 8-bit step.
// ---------------------------------------------------------------------------

namespace VoxelCrossingGradientTestUtils
{
	/** Sets voxel.Mesh.VoxelLODCrossings at console priority for a scope. */
	struct FScopedLODCrossings
	{
		IConsoleVariable* Var = nullptr;
		int32 Previous = 1;

		explicit FScopedLODCrossings(int32 Value)
		{
			Var = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Mesh.VoxelLODCrossings"));
			if (Var)
			{
				Previous = Var->GetInt();
				Var->Set(Value, ECVF_SetByConsole);
			}
		}

		~FScopedLODCrossings()
		{
			if (Var)
			{
				Var->Set(Previous, ECVF_SetByConsole);
			}
		}
	};

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
 * G80 on exact planes: Dual Contouring's and Marching Cubes' vertices at LOD 1 and 2 against the plane, with the strided
 * crossings (voxel.Mesh.VoxelLODCrossings 0) and voxel crossings (1). The strided crossings band by more than a quarter
 * voxel at LOD 2 in both meshers; voxel crossings put Marching Cubes' vertices on the plane (the crossing itself) and
 * Dual Contouring's as close as LOD 0's, with no band. LOD 0 is the same in either setting. With a real RHI the GPU
 * Marching Cubes mesher's vertices are the CPU's in both settings.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelLODCrossingPlanesTest, "VoxelWorlds.Meshing.LODCrossings.Planes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelLODCrossingPlanesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCrossingGradientTestUtils;

	FVoxelCPUDualContourMesher DCMesher;
	DCMesher.Initialize();
	DCMesher.SetConfig(MakeConfig());
	FVoxelCPUMarchingCubesMesher MCMesher;
	MCMesher.Initialize();
	MCMesher.SetConfig(MakeConfig());
	const bool bGPU = !GUsingNullRHI;
	FVoxelGPUMarchingCubesMesher GPUMesher;
	if (bGPU)
	{
		GPUMesher.Initialize();
		GPUMesher.SetConfig(MakeConfig());
	}
	else
	{
		AddInfo(TEXT("GPU half skipped: it needs a real RHI (run without -nullrhi)"));
	}

	const TArray<FRampPlane> Planes = MakePlanes();
	// [Mode][Mesher: 0 DC, 1 MC][LOD]
	FPlanePositionStats Stats[2][2][3];
	bool bLOD0Same[2] = { true, true };
	double GPUMax[2][3] = {};
	int32 GPUMatched[2][3] = {}, GPUVertices[2][3] = {};
	for (int32 LOD = 0; LOD < 3; ++LOD)
	{
		const float Margin = 1.5f * (1 << LOD) + 1.0f;
		for (const FRampPlane& Plane : Planes)
		{
			const FVoxelMeshingRequest Request = MakeRequest(Plane, LOD);
			FChunkMeshData Meshes[2][2];
			for (int32 Mode = 0; Mode < 2; ++Mode)
			{
				FScopedLODCrossings Scoped(Mode);
				TestTrue(TEXT("DC meshing succeeds"), DCMesher.GenerateMeshCPU(Request, Meshes[Mode][0]));
				TestTrue(TEXT("MC meshing succeeds"), MCMesher.GenerateMeshCPU(Request, Meshes[Mode][1]));
				for (int32 Mesher = 0; Mesher < 2; ++Mesher)
				{
					Stats[Mode][Mesher][LOD].Add(Meshes[Mode][Mesher], Plane, LOD, Margin);
				}
				if (!bGPU)
				{
					continue;
				}
				FChunkMeshData GPUMesh;
				TestTrue(TEXT("GPU MC meshing and readback succeed"), MeshGPU(GPUMesher, Request, GPUMesh));
				// Each GPU vertex against the nearest CPU vertex in its 1 uu cell or the cells around it.
				TMultiMap<FIntVector, FVector3f> CPUPositions;
				for (const FVector3f& P : Meshes[Mode][1].Positions)
				{
					CPUPositions.Add(FIntVector(FMath::FloorToInt32(P.X), FMath::FloorToInt32(P.Y), FMath::FloorToInt32(P.Z)), P);
				}
				GPUVertices[Mode][LOD] += GPUMesh.Positions.Num();
				for (const FVector3f& P : GPUMesh.Positions)
				{
					const FIntVector Key(FMath::FloorToInt32(P.X), FMath::FloorToInt32(P.Y), FMath::FloorToInt32(P.Z));
					double Best = TNumericLimits<double>::Max();
					for (int32 DZ = -1; DZ <= 1; ++DZ)
					{
						for (int32 DY = -1; DY <= 1; ++DY)
						{
							for (int32 DX = -1; DX <= 1; ++DX)
							{
								TArray<FVector3f> Found;
								CPUPositions.MultiFind(Key + FIntVector(DX, DY, DZ), Found);
								for (const FVector3f& Q : Found)
								{
									Best = FMath::Min(Best, static_cast<double>(FVector3f::Dist(P, Q)));
								}
							}
						}
					}
					if (Best < 1.0)
					{
						++GPUMatched[Mode][LOD];
						GPUMax[Mode][LOD] = FMath::Max(GPUMax[Mode][LOD], Best);
					}
				}
			}
			if (LOD == 0)
			{
				for (int32 Mesher = 0; Mesher < 2; ++Mesher)
				{
					bLOD0Same[Mesher] &= PositionsIdentical(Meshes[0][Mesher], Meshes[1][Mesher]);
				}
			}
		}
	}

	static const TCHAR* Names[2] = { TEXT("DC"), TEXT("MC") };
	for (int32 Mesher = 0; Mesher < 2; ++Mesher)
	{
		TestTrue(*FString::Printf(TEXT("%s LOD 0: positions the same in either setting"), Names[Mesher]), bLOD0Same[Mesher]);
		for (int32 LOD = 0; LOD < 3; ++LOD)
		{
			const FPlanePositionStats& Before = Stats[0][Mesher][LOD];
			const FPlanePositionStats& After = Stats[1][Mesher][LOD];
			AddInfo(FString::Printf(TEXT("%s LOD %d: strided crossings %s; voxel crossings %s"), Names[Mesher], LOD, *Before.Describe(), *After.Describe()));
			TestTrue(*FString::Printf(TEXT("%s LOD %d: vertices measured (%d)"), Names[Mesher], LOD, After.Count), After.Count > (1000 >> (2 * LOD)));
		}
		for (int32 LOD = 1; LOD < 3; ++LOD)
		{
			const FPlanePositionStats& Before = Stats[0][Mesher][LOD];
			const FPlanePositionStats& After = Stats[1][Mesher][LOD];
			const FPlanePositionStats& LOD0 = Stats[1][Mesher][0];
			TestTrue(*FString::Printf(TEXT("%s LOD %d: voxel crossings no farther from the plane than LOD 0 (mean %.4f against %.4f voxels)"),
				Names[Mesher], LOD, After.Mean(), LOD0.Mean()), After.Mean() <= LOD0.Mean() * 1.5 + 0.01);
			TestTrue(*FString::Printf(TEXT("%s LOD %d: voxel crossings band under 0.05 voxels (%.4f)"), Names[Mesher], LOD, After.Band()),
				After.Band() < 0.05);
			TestTrue(*FString::Printf(TEXT("%s LOD %d: voxel crossings closer to the plane than the strided (mean %.4f against %.4f)"),
				Names[Mesher], LOD, After.Mean(), Before.Mean()), After.Mean() < Before.Mean());
		}
		TestTrue(*FString::Printf(TEXT("%s LOD 2: the strided crossings band (%.3f voxels)"), Names[Mesher], Stats[0][Mesher][2].Band()),
			Stats[0][Mesher][2].Band() > 0.25);
		if (Mesher == 1)
		{
			for (int32 LOD = 1; LOD < 3; ++LOD)
			{
				TestTrue(*FString::Printf(TEXT("MC LOD %d: voxel crossings on the plane (max %.4f voxels)"), LOD, Stats[1][1][LOD].Max),
					Stats[1][1][LOD].Max < 0.02);
			}
		}
	}
	if (bGPU)
	{
		for (int32 Mode = 0; Mode < 2; ++Mode)
		{
			for (int32 LOD = 0; LOD < 3; ++LOD)
			{
				AddInfo(FString::Printf(TEXT("GPU MC, setting %d LOD %d: %d of %d vertices within 1 uu of a CPU vertex, %.4f uu at most"),
					Mode, LOD, GPUMatched[Mode][LOD], GPUVertices[Mode][LOD], GPUMax[Mode][LOD]));
				TestTrue(*FString::Printf(TEXT("GPU MC, setting %d LOD %d: every GPU vertex at a CPU vertex"), Mode, LOD),
					GPUVertices[Mode][LOD] > 0 && GPUMatched[Mode][LOD] == GPUVertices[Mode][LOD] && GPUMax[Mode][LOD] < 0.05);
			}
		}
		GPUMesher.Shutdown();
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
 * stride-2 cell of 100 uu voxels) solves onto the plane, at its mass point, with the solve about the mass point; the
 * solve before put it more than a voxel off the plane. A corner (three orthogonal planes meeting inside the cell) solves
 * to the corner either way, and a cell far from the origin solves onto its plane with the new solve wherever it is.
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
		const FVector3f After = QEF.Solve(SVDThreshold, Cell, BiasStrength, true);
		const FVector3f Before = QEF.Solve(SVDThreshold, Cell, BiasStrength, false);
		const float AfterOff = FMath::Abs(FVector3f::DotProduct(Normal, After - Points[0] * VoxelSize)) / VoxelSize;
		const float BeforeOff = FMath::Abs(FVector3f::DotProduct(Normal, Before - Points[0] * VoxelSize)) / VoxelSize;
		AddInfo(FString::Printf(TEXT("Planet cell: the solve about the mass point %.4f voxels off the plane, %.4f from the mass point; before %.4f off"),
			AfterOff, FVector3f::Dist(After, MassPoint) / VoxelSize, BeforeOff));
		TestTrue(*FString::Printf(TEXT("Planet cell: on the plane (%.4f voxels)"), AfterOff), AfterOff < 0.001f);
		TestTrue(TEXT("Planet cell: at its mass point"), FVector3f::Dist(After, MassPoint) < 0.001f * VoxelSize);
		TestTrue(*FString::Printf(TEXT("Planet cell: the solve before was off the plane (%.4f voxels)"), BeforeOff), BeforeOff > 0.5f);
	}

	// A corner: three orthogonal planes through (5.3, 6.2, 7.7) voxels, inside the cell [4, 8]^3.
	{
		const FVector3f Corner = FVector3f(5.3f, 6.2f, 7.7f) * VoxelSize;
		FQEFSolver QEF;
		QEF.Add(Corner + FVector3f(0.0f, 0.7f, -0.9f) * VoxelSize, FVector3f(1.0f, 0.0f, 0.0f));
		QEF.Add(Corner + FVector3f(-0.8f, 0.0f, -0.5f) * VoxelSize, FVector3f(0.0f, 1.0f, 0.0f));
		QEF.Add(Corner + FVector3f(-0.6f, 0.4f, 0.0f) * VoxelSize, FVector3f(0.0f, 0.0f, 1.0f));
		const FBox3f Cell(FVector3f(4.0f) * VoxelSize, FVector3f(8.0f) * VoxelSize);
		for (const bool bAboutMassPoint : { false, true })
		{
			const FVector3f V = QEF.Solve(SVDThreshold, Cell, BiasStrength, bAboutMassPoint);
			TestTrue(*FString::Printf(TEXT("Corner (solve %d): at the corner (%.4f voxels)"), bAboutMassPoint ? 1 : 0, FVector3f::Dist(V, Corner) / VoxelSize),
				FVector3f::Dist(V, Corner) < 0.001f * VoxelSize);
		}
	}

	// A tilted plane in cells along a line away from the origin: the new solve is on the plane in every one.
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
			const FVector3f V = QEF.Solve(SVDThreshold, Cell, BiasStrength, true);
			Worst = FMath::Max(Worst, FMath::Abs(FVector3f::DotProduct(Normal, V - Centre * VoxelSize)) / VoxelSize);
		}
		TestTrue(*FString::Printf(TEXT("A plane in 16 cells to 1,900 voxels out: on it in every one (worst %.4f voxels)"), Worst), Worst < 0.01f);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
