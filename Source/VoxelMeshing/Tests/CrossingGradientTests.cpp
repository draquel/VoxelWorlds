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
 * bands by more than a degree in both meshers. Vertex positions are the same either way (Dual Contouring's QEF keeps the
 * central difference). With a real RHI the GPU Marching Cubes mesher's normals match the CPU's in both settings.
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
			TestTrue(*FString::Printf(TEXT("%s LOD %d: positions the same in either setting"), Names[Mesher], LOD), bPositionsSame[Mesher][LOD]);
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

#endif // WITH_DEV_AUTOMATION_TESTS
