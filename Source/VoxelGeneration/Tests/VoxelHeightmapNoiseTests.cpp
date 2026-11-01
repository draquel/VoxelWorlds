// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/IConsoleManager.h"
#include "Testing/VoxelTestScopedCVar.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoise.h"
#include "VoxelNoiseTypes.h"
#include "IVoxelWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelClassicNoiseControl.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The heightmap noise since the world modes review's MP4 (WM-MP6, WM-MP7, decision D39; GenerationVersion 3): the
// heightmap entry point FVoxelCPUNoiseGenerator::FBM3D samples the planet's noise (FVoxelNoise): a hashed integer
// lattice with no 256-unit period, split-origin sampling exact to float precision at any distance from the origin, and the
// continuous Simplex kernel (G43). The classic table noise (VoxelClassicNoiseControl::ClassicFBM3D) is kept as the control.
//
// - Noise.HeightmapNoise: FBM3D is the planet noise bit for bit; at 2D points up to 1e9 uu from the origin it is within
//   1e-5 of the double reference (the classic noise's float positions were 64 uu coarse there); it repeats nowhere over
//   1e6 noise units along X where the classic noise repeats every 256 / frequency uu (non-vacuous: the control is shown to
//   repeat); its spread is the classic noise's.
// - WorldModes.Plane.NoiseCreases: the generated terrain height across the line X = Y, where every octave's simplex
//   boundary lies, steps no more and its slope changes no more than at a control of points off the line; with the classic
//   kernel (voxel.Noise.ContinuousSimplex 0) the height steps there (G43's creases, about a quarter voxel).
// ---------------------------------------------------------------------------

namespace VoxelHeightmapNoiseTestUtils
{
	/** VoxelDemo-like terrain params: Simplex, 4 octaves at 0.0004 (a 2,500 uu wavelength). */
	FVoxelNoiseParams MakeTerrainParams(EVoxelNoiseType Type = EVoxelNoiseType::Simplex)
	{
		FVoxelNoiseParams Params;
		Params.NoiseType = Type;
		Params.Seed = 1337;
		Params.Frequency = 0.0004f;
		Params.Octaves = 4;
		Params.Lacunarity = 2.0f;
		Params.Persistence = 0.5f;
		Params.Amplitude = 1.0f;
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHeightmapNoiseTest,
	"VoxelWorlds.Noise.HeightmapNoise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHeightmapNoiseTest::RunTest(const FString& Parameters)
{
	using namespace VoxelHeightmapNoiseTestUtils;
	FRandomStream Random(0x4E01);
	const EVoxelNoiseType Types[] = { EVoxelNoiseType::Simplex, EVoxelNoiseType::Perlin, EVoxelNoiseType::Cellular, EVoxelNoiseType::Voronoi };
	const TCHAR* TypeNames[] = { TEXT("Simplex"), TEXT("Perlin"), TEXT("Cellular"), TEXT("Voronoi") };

	// 1. The heightmap entry point is the planet noise, bit for bit, at 2D and 3D points.
	for (int32 t = 0; t < 4; ++t)
	{
		const FVoxelNoiseParams Params = MakeTerrainParams(Types[t]);
		int32 Differ = 0;
		for (int32 i = 0; i < 2000; ++i)
		{
			const double Z = (i & 1) ? Random.FRandRange(-12000.0, 12000.0) : 0.0;
			const FVector P(Random.FRandRange(-1.0e6, 1.0e6), Random.FRandRange(-1.0e6, 1.0e6), Z);
			Differ += FVoxelCPUNoiseGenerator::FBM3D(P, Params) == FVoxelNoise::FBM3D(P, Params) ? 0 : 1;
		}
		TestEqual(*FString::Printf(TEXT("%s: FBM3D is FVoxelNoise::FBM3D bit for bit (%d of 2000 differ)"), TypeNames[t], Differ), Differ, 0);
	}

	// 2. Exact far from the origin: 2D points (the heightmap fields' (X, Y, 0)) at 1e3 to 1e9 uu against the double
	// reference, per point (the CPU) and as a chunk origin + the voxel's offset (the GPU's algebra); the classic noise
	// for comparison at the same points (a float position is 64 uu coarse at 1e9).
	for (int32 t = 0; t < 4; ++t)
	{
		const FVoxelNoiseParams Params = MakeTerrainParams(Types[t]);
		for (const double Magnitude : { 1.0e3, 1.0e6, 1.0e8, 1.0e9 })
		{
			double MaxPoint = 0.0, MaxOffset = 0.0;
			for (int32 i = 0; i < 200; ++i)
			{
				const double Angle = Random.FRandRange(0.0, UE_DOUBLE_TWO_PI);
				const FVector Origin(FMath::Cos(Angle) * Magnitude, FMath::Sin(Angle) * Magnitude, 0.0);
				const FVector3f Offset(Random.FRandRange(0.0f, 4800.0f), Random.FRandRange(0.0f, 4800.0f), 0.0f);
				const FVector Point = Origin + FVector(Offset);
				const double Reference = FVoxelNoise::ReferenceFBM3D(Point, Params);
				MaxPoint = FMath::Max(MaxPoint, FMath::Abs(FVoxelCPUNoiseGenerator::FBM3D(Point, Params) - Reference));
				MaxOffset = FMath::Max(MaxOffset, FMath::Abs(FVoxelNoise::FBM3D(Origin, Offset, Params) - Reference));
			}
			AddInfo(FString::Printf(TEXT("%s at %.0e uu: per point off the double reference by up to %.2e, chunk origin + offset by %.2e"), TypeNames[t], Magnitude, MaxPoint, MaxOffset));
			TestTrue(*FString::Printf(TEXT("%s at %.0e: per point within 1e-5 (%.2e)"), TypeNames[t], Magnitude, MaxPoint), MaxPoint <= 1.0e-5);
			TestTrue(*FString::Printf(TEXT("%s at %.0e: origin + offset within 1e-5 (%.2e)"), TypeNames[t], Magnitude, MaxOffset), MaxOffset <= 1.0e-5);
		}
	}

	// 3. No period. The classic table noise repeats along X every 256 lattice units for Perlin and every 768 for Simplex
	// (a shift of 768 moves the skewed lattice coordinates by 1,024, 256 and 256, each a multiple of the table's 256);
	// every octave's period divides it with an integer lacunarity: the controls. The heightmap noise at the same shifts,
	// and at every multiple of 256 / frequency up to 1e6 noise units, is as unrelated as two random samples.
	for (const EVoxelNoiseType Type : { EVoxelNoiseType::Perlin, EVoxelNoiseType::Simplex })
	{
		const FVoxelNoiseParams Params = MakeTerrainParams(Type);
		const double PeriodUnits = Type == EVoxelNoiseType::Perlin ? 256.0 : 768.0;
		const double PeriodUU = PeriodUnits / Params.Frequency;
		const TCHAR* Name = Type == EVoxelNoiseType::Perlin ? TEXT("Perlin") : TEXT("Simplex");
		double ClassicMean = 0.0, NewMean = 0.0;
		const int32 Count = 256;
		for (int32 i = 0; i < Count; ++i)
		{
			const FVector P(Random.FRandRange(0.0, PeriodUU), Random.FRandRange(0.0, PeriodUU), 0.0);
			const FVector Q = P + FVector(PeriodUU, 0.0, 0.0);
			ClassicMean += FMath::Abs(VoxelClassicNoiseControl::ClassicFBM3D(Q, Params) - VoxelClassicNoiseControl::ClassicFBM3D(P, Params)) / Count;
			NewMean += FMath::Abs(FVoxelCPUNoiseGenerator::FBM3D(Q, Params) - FVoxelCPUNoiseGenerator::FBM3D(P, Params)) / Count;
		}
		AddInfo(FString::Printf(TEXT("%s: a shift of %.0f / f = %.0f uu along X: the classic noise differs by %.2e on average, the heightmap noise by %.3f"), Name, PeriodUnits, PeriodUU, ClassicMean, NewMean));
		TestTrue(*FString::Printf(TEXT("Control: the classic %s noise repeats every %.0f / frequency uu (%.2e)"), Name, PeriodUnits, ClassicMean), ClassicMean < 1.0e-3);
		TestTrue(*FString::Printf(TEXT("The heightmap %s noise does not repeat there (%.3f)"), Name, NewMean), NewMean > 0.05);
	}
	{
		const FVoxelNoiseParams Params = MakeTerrainParams(EVoxelNoiseType::Simplex);
		const double PeriodUU = 256.0 / Params.Frequency;

		double WorstMean = TNumericLimits<double>::Max();
		double WorstShift = 0.0;
		const int32 MaxK = FMath::FloorToInt32(1.0e6 / 256.0);
		for (int32 k = 1; k <= MaxK; ++k)
		{
			const double Shift = PeriodUU * k;
			double Mean = 0.0;
			for (int32 i = 0; i < 16; ++i)
			{
				const FVector P(Random.FRandRange(0.0, 1.0e6 / Params.Frequency), Random.FRandRange(0.0, 1.0e6 / Params.Frequency), 0.0);
				Mean += FMath::Abs(FVoxelCPUNoiseGenerator::FBM3D(P + FVector(Shift, 0.0, 0.0), Params) - FVoxelCPUNoiseGenerator::FBM3D(P, Params)) / 16.0;
			}
			if (Mean < WorstMean)
			{
				WorstMean = Mean;
				WorstShift = Shift;
			}
		}
		AddInfo(FString::Printf(TEXT("The most alike shift of k x 256 / f up to 1e6 noise units (%.0f uu) still differs by %.3f on average"), WorstShift, WorstMean));
		TestTrue(*FString::Printf(TEXT("The heightmap noise repeats nowhere within 1e6 noise units (worst mean difference %.3f)"), WorstMean), WorstMean > 0.05);
	}

	// 4. The spread: the heightmap noise's standard deviation within 5% of the classic noise's over the same 2D points, so
	// the demo's height scales mean what they did (the continuous kernel is scaled to the classic spread, G43).
	{
		const FVoxelNoiseParams Params = MakeTerrainParams(EVoxelNoiseType::Simplex);
		double SumC = 0.0, SumCC = 0.0, SumN = 0.0, SumNN = 0.0, MaxAbsN = 0.0;
		const int32 Count = 20000;
		for (int32 i = 0; i < Count; ++i)
		{
			const FVector P(Random.FRandRange(-1.0e6, 1.0e6), Random.FRandRange(-1.0e6, 1.0e6), 0.0);
			const double C = VoxelClassicNoiseControl::ClassicFBM3D(P, Params);
			const double N = FVoxelCPUNoiseGenerator::FBM3D(P, Params);
			SumC += C; SumCC += C * C; SumN += N; SumNN += N * N;
			MaxAbsN = FMath::Max(MaxAbsN, FMath::Abs(N));
		}
		const double StdC = FMath::Sqrt(FMath::Max(0.0, SumCC / Count - FMath::Square(SumC / Count)));
		const double StdN = FMath::Sqrt(FMath::Max(0.0, SumNN / Count - FMath::Square(SumN / Count)));
		AddInfo(FString::Printf(TEXT("Spread over +-1e6 uu: classic std %.4f, heightmap noise std %.4f (max |value| %.3f)"), StdC, StdN, MaxAbsN));
		TestTrue(TEXT("The heightmap noise keeps the classic spread (std within 5%)"), FMath::Abs(StdN / StdC - 1.0) < 0.05);
		TestTrue(TEXT("The heightmap noise stays in [-1, 1]"), MaxAbsN <= 1.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlaneNoiseCreasesTest,
	"VoxelWorlds.WorldModes.Plane.NoiseCreases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlaneNoiseCreasesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelHeightmapNoiseTestUtils;

	// The terrain: VoxelDemo-like noise and an 800 uu height scale (a gentle slope at a 2,500 uu wavelength).
	const FVoxelNoiseParams Params = MakeTerrainParams(EVoxelNoiseType::Simplex);
	FWorldModeTerrainParams Terrain;
	Terrain.SeaLevel = 0.0f;
	Terrain.BaseHeight = 0.0f;
	Terrain.HeightScale = 800.0f;
	auto HeightAt = [&](const FVector2D& XY)
	{
		return static_cast<double>(FInfinitePlaneWorldMode::NoiseToTerrainHeight(FInfinitePlaneWorldMode::SampleTerrainNoise2D(XY.X, XY.Y, Params), Terrain));
	};

	// Across the line X = Y (every octave's simplex boundary x - y = 0 in noise space, the plane the cube sphere's face
	// edges lie on for a planet), and across the same step at a control of points off the line: the height's step over
	// 2e-3 uu and the change of its slope either side.
	struct FStats { double MaxStep = 0.0; double MaxSlopeJump = 0.0; };
	auto Measure = [&](bool bOnLine, int32 Seed)
	{
		FRandomStream Random(Seed);
		FStats Out;
		const FVector2D Normal(UE_DOUBLE_INV_SQRT_2, -UE_DOUBLE_INV_SQRT_2);
		constexpr double Eps = 1.0e-3;
		for (int32 i = 0; i < 4000; ++i)
		{
			const double A = Random.FRandRange(-1.0e6, 1.0e6);
			// Off the line: a random offset of a quarter to three quarters of a wavelength across it
			const double Off = bOnLine ? 0.0 : Random.FRandRange(625.0, 1875.0) * (Random.FRand() < 0.5 ? 1.0 : -1.0);
			const FVector2D P = FVector2D(A, A) + Normal * Off;
			auto H = [&](double T) { return HeightAt(P + Normal * T); };
			Out.MaxStep = FMath::Max(Out.MaxStep, FMath::Abs(H(Eps) - H(-Eps)));
			const double SlopePlus = (H(2.0 * Eps) - H(Eps)) / Eps;
			const double SlopeMinus = (H(-Eps) - H(-2.0 * Eps)) / Eps;
			Out.MaxSlopeJump = FMath::Max(Out.MaxSlopeJump, FMath::Abs(SlopePlus - SlopeMinus));
		}
		return Out;
	};

	FStats OnLine[2], Control[2]; // [kernel: 0 classic, 1 continuous]
	for (int32 Kernel = 0; Kernel < 2; ++Kernel)
	{
		VoxelTesting::FScopedCVar Scoped(TEXT("voxel.Noise.ContinuousSimplex"), Kernel);
		OnLine[Kernel] = Measure(true, 0x11 + Kernel);
		Control[Kernel] = Measure(false, 0x21 + Kernel);
		AddInfo(FString::Printf(TEXT("%s kernel: across X = Y the height steps up to %.4f uu (control off the line %.4f), the slope changes by up to %.4f (control %.4f)"),
			Kernel == 1 ? TEXT("Continuous") : TEXT("Classic"), OnLine[Kernel].MaxStep, Control[Kernel].MaxStep, OnLine[Kernel].MaxSlopeJump, Control[Kernel].MaxSlopeJump));
	}
	// The step over 2e-3 uu of a smooth 800 uu slope is at most a few thousandths of a uu; a crease is a step of a
	// fraction of a voxel (the classic kernel's, non-vacuous), and no larger than the control for the continuous kernel.
	TestTrue(*FString::Printf(TEXT("Classic kernel: the height steps across X = Y (%.4f uu, non-vacuous)"), OnLine[0].MaxStep), OnLine[0].MaxStep > 10.0 * Control[0].MaxStep);
	TestTrue(*FString::Printf(TEXT("Continuous kernel: no step across X = Y (%.4f against the control's %.4f)"), OnLine[1].MaxStep, Control[1].MaxStep),
		OnLine[1].MaxStep <= 2.0 * Control[1].MaxStep + 1.0e-3);
	TestTrue(*FString::Printf(TEXT("Continuous kernel: the slope is as continuous across X = Y as off it (%.4f against %.4f)"), OnLine[1].MaxSlopeJump, Control[1].MaxSlopeJump),
		OnLine[1].MaxSlopeJump <= 2.0 * Control[1].MaxSlopeJump + 1.0e-2);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
