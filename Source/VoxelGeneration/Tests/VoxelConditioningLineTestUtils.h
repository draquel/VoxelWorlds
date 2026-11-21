// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelConditioningLine.h"
#include "VoxelChunkLineSegments.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"
#include "VoxelData.h"
#include "VoxelBlockSurface.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "IVoxelWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "SphericalPlanetWorldMode.h"

#if WITH_DEV_AUTOMATION_TESTS

// What the conditioning line tests share (VoxelConditioningLineTests.cpp, VoxelConditioningLineSurfaceTests.cpp): lines
// to test with, a small world of each kind, a request as the chunk manager builds it, and a generated chunk checked
// against the analytic conditioned height.

namespace VoxelConditioningLineTestUtils
{
	constexpr float VoxelSize = 75.0f;
	constexpr int32 ChunkSize = 32;

	/** A straight heightmap line from A to B with a vertex every Spacing, targets from TargetA to TargetB. */
	inline FVoxelConditioningLineParams MakeStraight(const FVector2D& A, const FVector2D& B, float TargetA, float TargetB, float HalfWidth,
		float Falloff, EVoxelConditioningLineBlend Blend = EVoxelConditioningLineBlend::Level, int64 Id = 1, double Spacing = 150.0)
	{
		FVoxelConditioningLineParams Params;
		Params.FalloffWidth = Falloff;
		Params.Blend = Blend;
		Params.Id = Id;
		const int32 Steps = FMath::Max(1, FMath::CeilToInt32((B - A).Size() / Spacing));
		for (int32 Step = 0; Step <= Steps; ++Step)
		{
			const double T = static_cast<double>(Step) / Steps;
			const FVector2D P = FMath::Lerp(A, B, T);
			Params.Vertices.Add(FVoxelConditioningLineVertex(FVector(P.X, P.Y, 0.0), FMath::Lerp(TargetA, TargetB, static_cast<float>(T)), HalfWidth));
		}
		return Params;
	}

	/** A planet line along the great circle from A to B, a vertex every Spacing along the surface. */
	inline FVoxelConditioningLineParams MakeArc(const FVector& A, const FVector& B, double PlanetRadius, float TargetA, float TargetB, float HalfWidth,
		float Falloff, EVoxelConditioningLineBlend Blend = EVoxelConditioningLineBlend::Level, int64 Id = 1, double Spacing = 150.0)
	{
		FVoxelConditioningLineParams Params;
		Params.FalloffWidth = Falloff;
		Params.Blend = Blend;
		Params.Id = Id;
		Params.bRadial = true;
		const double Angle = FMath::Atan2(FVector::CrossProduct(A, B).Size(), FVector::DotProduct(A, B));
		const int32 Steps = FMath::Max(1, FMath::CeilToInt32(Angle * PlanetRadius / Spacing));
		for (int32 Step = 0; Step <= Steps; ++Step)
		{
			const double T = static_cast<double>(Step) / Steps;
			const FVector Direction = (A * FMath::Sin((1.0 - T) * Angle) + B * FMath::Sin(T * Angle)).GetSafeNormal();
			Params.Vertices.Add(FVoxelConditioningLineVertex(Direction, FMath::Lerp(TargetA, TargetB, static_cast<float>(T)), HalfWidth));
		}
		return Params;
	}

	/** The distance from a point to a polyline, every segment tried (no bounds, no blocks): the tests' own measure. */
	inline double BruteDistance(const TArray<FVoxelConditioningLineVertex>& Vertices, const FVector& P, double Scale, int32* OutSegment = nullptr, double* OutT = nullptr)
	{
		double Best = TNumericLimits<double>::Max();
		for (int32 Index = 0; Index + 1 < Vertices.Num(); ++Index)
		{
			const FVector A = Vertices[Index].Position * Scale;
			const FVector AB = Vertices[Index + 1].Position * Scale - A;
			const double T = FMath::Clamp(FVector::DotProduct(P - A, AB) / FMath::Max(AB.SizeSquared(), UE_DOUBLE_SMALL_NUMBER), 0.0, 1.0);
			const double Dist = (P - (A + AB * T)).Size();
			if (Dist < Best)
			{
				Best = Dist;
				if (OutSegment) { *OutSegment = Index; }
				if (OutT) { *OutT = T; }
			}
		}
		return Best;
	}

	/** A small world of each kind: no biomes, caves or water, so the height is all that decides a voxel. */
	inline UVoxelWorldConfiguration* MakeConfig(EWorldMode Mode)
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = Mode;
		Config->VoxelSize = VoxelSize;
		Config->ChunkSize = ChunkSize;
		Config->NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
		Config->NoiseParams.Seed = 7411;
		Config->NoiseParams.Octaves = 3;
		Config->NoiseParams.Frequency = 0.0002f;
		Config->NoiseParams.Lacunarity = 2.0f;
		Config->NoiseParams.Persistence = 0.5f;
		Config->bEnableBiomes = false;
		Config->bEnableCaves = false;
		Config->bEnableWaterLevel = false;
		Config->BaseHeight = 0.0f;
		if (Mode == EWorldMode::SphericalPlanet)
		{
			Config->WorldOrigin = FVector(12345.0, 54321.0, 0.0);
			Config->WorldRadius = 100000.0f;
			Config->PlanetHeightScale = 1200.0f;
			Config->PlanetMaxTerrainHeight = 1500.0f;
			Config->PlanetMaxTerrainDepth = 1500.0f;
		}
		else
		{
			Config->WorldOrigin = FVector(-37.5, 112.5, 0.0);
			Config->HeightScale = 900.0f;
			if (Mode == EWorldMode::IslandBowl)
			{
				Config->IslandRadius = 9000.0f;
				Config->IslandFalloffWidth = 5000.0f;
			}
		}
		return Config;
	}

	/** A request as UVoxelChunkManager builds it: the world-mode fields from the factory, the rest from the config. */
	inline FVoxelNoiseGenerationRequest MakeRequest(const UVoxelWorldConfiguration& Config, const FIntVector& ChunkCoord)
	{
		FVoxelNoiseGenerationRequest Request;
		Request.ChunkCoord = ChunkCoord;
		Request.ChunkSize = Config.ChunkSize;
		Request.VoxelSize = Config.VoxelSize;
		Request.NoiseParams = Config.NoiseParams;
		FVoxelWorldModeFactory::FillGenerationRequest(Config, Request);
		Request.bEnableBiomes = false;
		Request.bEnableCaves = false;
		Request.bEnableWaterLevel = false;
		return Request;
	}

	inline FIntVector ChunkAt(const UVoxelWorldConfiguration& Config, const FVector& WorldPos)
	{
		const FVector Local = (WorldPos - Config.WorldOrigin) / (static_cast<double>(Config.ChunkSize) * Config.VoxelSize);
		return FIntVector(FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), FMath::FloorToInt32(Local.Z));
	}

	/** What a generated chunk is checked against: the analytic conditioned height at each of its voxels. */
	struct FChunkCheck
	{
		int32 Voxels = 0;
		/** Voxels whose solidity differs from the analytic height's (away from the surface's own rounding) */
		int32 Wrong = 0;
		/** Columns (a heightmap) or voxels (a planet) the conditioning moves by more than a voxel */
		int32 Moved = 0;
		/** Voxels where no line or zone has a weight, and those of them that differ from the unconditioned chunk */
		int32 Untouched = 0;
		int32 UntouchedDiffering = 0;
		/** The CPU generator's time for the chunk without and with the conditioning (ms; the least of three runs each) */
		double NaturalMs = 0.0;
		double ConditionedMs = 0.0;
	};

	/**
	 * Generate a chunk with and without a conditioning and compare every voxel with the analytic conditioned height:
	 * solid exactly where the generators' own rule says for that height (FVoxelBlockSurface::IsGeneratedSolid), skipping
	 * the voxels within Slack of the surface (float against double), and unchanged where the conditioning has no weight.
	 */
	inline FChunkCheck CheckChunk(const UVoxelWorldConfiguration& Config, const IVoxelWorldMode& Mode, const FIntVector& ChunkCoord,
		const FVoxelConditioning& Conditioning, float Slack)
	{
		FChunkCheck Check;
		FVoxelCPUNoiseGenerator Generator;
		Generator.Initialize();
		FVoxelNoiseGenerationRequest Request = MakeRequest(Config, ChunkCoord);
		// Each side three times, the least time kept: what the lines cost the CPU generator on this chunk.
		const auto Generate = [&Generator, &Request](TArray<FVoxelData>& Out)
		{
			double Least = TNumericLimits<double>::Max();
			for (int32 Run = 0; Run < 3; ++Run)
			{
				const double Start = FPlatformTime::Seconds();
				Generator.GenerateChunkCPU(Request, Out);
				Least = FMath::Min(Least, 1000.0 * (FPlatformTime::Seconds() - Start));
			}
			return Least;
		};
		TArray<FVoxelData> Natural;
		Check.NaturalMs = Generate(Natural);
		Request.Conditioning = Conditioning;
		TArray<FVoxelData> Conditioned;
		Check.ConditionedMs = Generate(Conditioned);
		Generator.Shutdown();
		if (Natural.Num() != Conditioned.Num() || Natural.Num() == 0)
		{
			Check.Wrong = -1;
			return Check;
		}

		const bool bPlanet = Config.WorldMode == EWorldMode::SphericalPlanet;
		const FVector ChunkOrigin = Request.GetChunkWorldPosition();
		const FSphericalPlanetParams Planet = bPlanet ? FVoxelWorldModeFactory::MakePlanetParams(Config) : FSphericalPlanetParams();
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < ChunkSize; ++Y)
			{
				for (int32 X = 0; X < ChunkSize; ++X)
				{
					const int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
					const FVector WorldPos = ChunkOrigin + FVector(X, Y, Z) * VoxelSize;
					float NaturalHeight = 0.0f;
					float Height = 0.0f;
					double VoxelLevel = 0.0;
					if (bPlanet)
					{
						const FVector Relative = WorldPos - Planet.PlanetCenter;
						const FVector Direction = Relative.GetSafeNormal();
						NaturalHeight = static_cast<const FSphericalPlanetWorldMode&>(Mode).GetTerrainAltitude(Direction, Config.NoiseParams);
						Height = Conditioning.ApplyToAltitude(Direction, Planet.PlanetRadius, NaturalHeight);
						VoxelLevel = Relative.Size() - Planet.PlanetRadius;
					}
					else
					{
						NaturalHeight = static_cast<float>(Mode.GetSurfaceOnUpLine(FVector(WorldPos.X, WorldPos.Y, 0.0), Config.NoiseParams).Position.Z);
						Height = Conditioning.ApplyToHeight(WorldPos.X, WorldPos.Y, NaturalHeight);
						VoxelLevel = WorldPos.Z;
					}
					++Check.Voxels;
					if ((Z == 0 || bPlanet) && FMath::Abs(Height - NaturalHeight) > VoxelSize)
					{
						++Check.Moved;
					}
					if (Height == NaturalHeight)
					{
						++Check.Untouched;
						if (FMemory::Memcmp(&Natural[Index], &Conditioned[Index], sizeof(FVoxelData)) != 0)
						{
							++Check.UntouchedDiffering;
						}
					}
					if (FMath::Abs(static_cast<double>(Height) - VoxelLevel) < Slack)
					{
						continue;
					}
					const bool bSolid = Conditioned[Index].Density >= VOXEL_SURFACE_THRESHOLD;
					if (bSolid != FVoxelBlockSurface::IsGeneratedSolid(Height, VoxelLevel, VoxelSize))
					{
						++Check.Wrong;
					}
				}
			}
		}
		return Check;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
