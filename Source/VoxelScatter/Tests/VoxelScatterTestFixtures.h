// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "IVoxelWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelData.h"
#include "VoxelScatterManager.h"
#include "VoxelWorldConfiguration.h"
#include "Testing/VoxelScatterTestDefinitions.h"

/**
 * The scatter suites' shared fixtures (code review CR2 row 17, SCAT-34): a game world with a configuration and a scatter
 * manager (the harness every suite derived its own from), a tilted plane as a world mode, a heightmap chunk's voxels from a
 * height and a material per column, and the two planet suites' configuration. Compiled only with WITH_DEV_AUTOMATION_TESTS.
 */
namespace VoxelScatterTesting
{
	/**
	 * A transient game world, a configuration and a scatter manager on it. Derive, set Config in the constructor, then
	 * StartManager with the smooth or the cubic test definitions; the destructor shuts the manager down and destroys the
	 * world (a world mode set through UseWorldMode is cleared first, as the suites did).
	 */
	struct FScatterTestWorld
	{
		UWorld* World = nullptr;
		UVoxelWorldConfiguration* Config = nullptr;
		UVoxelScatterManager* Manager = nullptr;

		/** A new world and a fresh configuration (not rooted: the manager holds it). */
		explicit FScatterTestWorld(const TCHAR* WorldName)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, WorldName);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			Config = NewObject<UVoxelWorldConfiguration>();
		}

		/** A new world over a configuration the caller made (and roots). */
		FScatterTestWorld(const TCHAR* WorldName, UVoxelWorldConfiguration* InConfig)
			: Config(InConfig)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, WorldName);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
		}

		FScatterTestWorld(const FScatterTestWorld&) = delete;
		FScatterTestWorld& operator=(const FScatterTestWorld&) = delete;

		~FScatterTestWorld()
		{
			if (Manager)
			{
				if (bWorldModeSet)
				{
					Manager->SetWorldMode(nullptr);
				}
				Manager->Shutdown();
				Manager->RemoveFromRoot();
			}
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
		}

		/**
		 * The scatter manager on Config (rooted), with the test definitions.
		 * @param bSmoothDefinitions The smooth definitions (IDs 0, 1, 2) or the cubic ones (100, 101, 102, 10)
		 */
		void StartManager(bool bSmoothDefinitions)
		{
			Manager = NewObject<UVoxelScatterManager>();
			Manager->AddToRoot();
			Manager->Initialize(Config, World);
			if (bSmoothDefinitions)
			{
				AddSmoothDefinitions(*Manager);
			}
			else
			{
				AddCubicDefinitions(*Manager);
			}
		}

		/** Give the manager a world mode for the test (cleared before the manager shuts down). */
		void UseWorldMode(const IVoxelWorldMode* Mode)
		{
			Manager->SetWorldMode(Mode);
			bWorldModeSet = true;
		}

	private:
		bool bWorldModeSet = false;
	};

	/** A heightmap world mode whose terrain is the plane H = Offset + SlopeX * X + SlopeY * Y, for tests of the heightmap scatter paths. */
	struct FTiltedPlaneWorldMode : public IVoxelWorldMode
	{
		float Offset = 0.0f;
		float SlopeX = 0.0f;
		float SlopeY = 0.0f;
		FTiltedPlaneWorldMode(float InOffset, float InSlopeX, float InSlopeY) : Offset(InOffset), SlopeX(InSlopeX), SlopeY(InSlopeY) {}
		float GetTerrainHeightAt(float X, float Y, const FVoxelNoiseParams&) const { return Offset + SlopeX * X + SlopeY * Y; }
		virtual FVoxelTerrainSurfacePoint GetSurfaceOnUpLine(const FVector& P, const FVoxelNoiseParams& N) const override
		{
			FVoxelTerrainSurfacePoint S;
			S.Position = FVector(P.X, P.Y, GetTerrainHeightAt(static_cast<float>(P.X), static_cast<float>(P.Y), N));
			S.Altitude = static_cast<float>(S.Position.Z);
			return S;
		}
		virtual bool IsHeightmapBased() const override { return true; }
		virtual float GetDensityAt(const FVector&, int32, float) const override { return 0.0f; }
		virtual FIntVector WorldToChunkCoord(const FVector&, int32, float) const override { return FIntVector::ZeroValue; }
		virtual FVector ChunkCoordToWorld(const FIntVector&, int32, float, int32) const override { return FVector::ZeroVector; }
		virtual int32 GetMinZ() const override { return -100000; }
		virtual int32 GetMaxZ() const override { return 100000; }
		virtual EWorldMode GetWorldModeType() const override { return EWorldMode::InfinitePlane; }
		virtual uint8 GetMaterialAtDepth(const FVector&, float, float) const override { return 0; }

		float HeightAt(double X, double Y) const { return GetTerrainHeightAt(static_cast<float>(X), static_cast<float>(Y), FVoxelNoiseParams()); }
	};

	/** The plane H = 300.37 + 0.13 X + 0.11 Y (300 to 1,068 over two chunks each way, inside chunk layer 0, about 10 degrees of slope). */
	inline const FTiltedPlaneWorldMode& TiltedPlane()
	{
		static const FTiltedPlaneWorldMode Mode(300.37f, 0.13f, 0.11f);
		return Mode;
	}

	/**
	 * A heightmap chunk's voxels as the heightmap generator makes them (the signed distance to the column's height, as
	 * density), from a height per column and a material per column.
	 * @param Origin The chunk's world origin (its minimum corner)
	 * @param HeightAt The terrain height at a world (X, Y)
	 * @param MaterialAt The material of the column at local (X, Y)
	 */
	inline TArray<FVoxelData> MakeHeightmapChunk(const FVector& Origin, int32 ChunkSize, float VoxelSize,
		TFunctionRef<float(double X, double Y)> HeightAt, TFunctionRef<uint8(int32 X, int32 Y)> MaterialAt)
	{
		TArray<FVoxelData> Voxels;
		Voxels.SetNumUninitialized(ChunkSize * ChunkSize * ChunkSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				const float Height = HeightAt(Origin.X + X * VoxelSize, Origin.Y + Y * VoxelSize);
				const uint8 Material = MaterialAt(X, Y);
				for (int32 Z = 0; Z < ChunkSize; ++Z)
				{
					const float SignedDistance = FInfinitePlaneWorldMode::CalculateSignedDistance(static_cast<float>(Origin.Z) + Z * VoxelSize, Height);
					Voxels[X + Y * ChunkSize + Z * ChunkSize * ChunkSize] =
						FVoxelData(Material, FInfinitePlaneWorldMode::SignedDistanceToDensity(SignedDistance, VoxelSize));
				}
			}
		}
		return Voxels;
	}

	/**
	 * The planet scatter suites' planet (rooted; the caller removes it from the root): radius 250,000 with 3,000 of relief
	 * on 75 uu voxels in 32-voxel chunks, origin (1000, -2000, 500), seed 248675; at scale (bAtScale) radius 1e8 with
	 * 192,000 of relief and nine octaves.
	 */
	inline UVoxelWorldConfiguration* MakePlanetScatterConfig(bool bAtScale = false)
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::SphericalPlanet;
		Config->WorldOrigin = FVector(1000.0, -2000.0, 500.0);
		Config->WorldRadius = bAtScale ? 1.0e8f : 250000.0f;
		Config->PlanetHeightScale = bAtScale ? 192000.0f : 3000.0f;
		Config->BaseHeight = 0.0f;
		Config->PlanetMaxTerrainHeight = bAtScale ? 250000.0f : 4000.0f;
		Config->PlanetMaxTerrainDepth = bAtScale ? 250000.0f : 4000.0f;
		Config->VoxelSize = 75.0f;
		Config->ChunkSize = 32;
		Config->bEnableBiomes = false;
		Config->bEnableCaves = false;
		Config->bEnableWaterLevel = false;
		Config->NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
		Config->NoiseParams.Seed = 248675;
		Config->NoiseParams.Octaves = bAtScale ? 9 : 3;
		Config->NoiseParams.Frequency = bAtScale ? 0.000025f / 64.0f : 0.000025f;
		Config->NoiseParams.Lacunarity = 1.681f;
		Config->NoiseParams.Persistence = 0.5f;
		return Config;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
