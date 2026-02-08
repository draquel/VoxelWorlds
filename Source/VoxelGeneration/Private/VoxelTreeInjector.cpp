// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelTreeInjector.h"
#include "VoxelCoreTypes.h"
#include "IVoxelWorldMode.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelCPUNoiseGenerator.h"

void FVoxelTreeInjector::InjectTrees(
	const FIntVector& ChunkCoord,
	int32 ChunkSize, float VoxelSize,
	const FVector& WorldOrigin, int32 WorldSeed,
	const TArray<FVoxelTreeTemplate>& Templates,
	const FVoxelNoiseParams& NoiseParams,
	const IVoxelWorldMode& WorldMode,
	float TreeDensity,
	const UVoxelBiomeConfiguration* BiomeConfig,
	bool bEnableWaterLevel, float WaterLevel,
	TArray<FVoxelData>& InOutVoxelData)
{
	if (Templates.Num() == 0 || TreeDensity <= 0.0f)
	{
		return;
	}

	// Compute max tree extent to determine neighbor search radius
	int32 MaxExtent = 0;
	int32 MaxHeight = 0;
	for (const FVoxelTreeTemplate& Tmpl : Templates)
	{
		MaxExtent = FMath::Max(MaxExtent, Tmpl.GetMaxHorizontalExtent());
		MaxHeight = FMath::Max(MaxHeight, Tmpl.GetMaxHeight());
	}

	// Neighbor search radius in chunks (how far a tree from a neighbor could reach into this chunk)
	const int32 SearchRadiusChunks = FMath::Max(1, FMath::CeilToInt(static_cast<float>(MaxExtent) / ChunkSize));

	// Iterate over this chunk and neighbor chunks
	for (int32 DX = -SearchRadiusChunks; DX <= SearchRadiusChunks; ++DX)
	{
		for (int32 DY = -SearchRadiusChunks; DY <= SearchRadiusChunks; ++DY)
		{
			// Only check same Z level and one above/below (trees grow upward)
			const int32 MaxZSearch = FMath::CeilToInt(static_cast<float>(MaxHeight) / ChunkSize);
			for (int32 DZ = -MaxZSearch; DZ <= 0; ++DZ)
			{
				const FIntVector SourceChunk = ChunkCoord + FIntVector(DX, DY, DZ);

				TArray<FIntVector> TreePositions;
				TArray<int32> TemplateIDs;
				TArray<uint32> TreeSeeds;

				ComputeTreePositionsForChunk(
					SourceChunk,
					ChunkSize, VoxelSize,
					WorldOrigin, WorldSeed,
					NoiseParams, WorldMode,
					TreeDensity, Templates,
					BiomeConfig, bEnableWaterLevel, WaterLevel,
					TreePositions, TemplateIDs, TreeSeeds);

				// Stamp each tree that could overlap this chunk
				for (int32 i = 0; i < TreePositions.Num(); ++i)
				{
					const int32 TmplIdx = TemplateIDs[i];
					if (TmplIdx < 0 || TmplIdx >= Templates.Num())
					{
						continue;
					}

					const FVoxelTreeTemplate& Tmpl = Templates[TmplIdx];
					const FIntVector& BasePos = TreePositions[i];

					// Quick bounding box check: does tree potentially overlap this chunk?
					const int32 Extent = Tmpl.GetMaxHorizontalExtent();
					const int32 Height = Tmpl.GetMaxHeight();
					const FIntVector ChunkMin = ChunkCoord * ChunkSize;
					const FIntVector ChunkMax = ChunkMin + FIntVector(ChunkSize, ChunkSize, ChunkSize);

					if (BasePos.X + Extent < ChunkMin.X || BasePos.X - Extent >= ChunkMax.X)
					{
						continue;
					}
					if (BasePos.Y + Extent < ChunkMin.Y || BasePos.Y - Extent >= ChunkMax.Y)
					{
						continue;
					}
					if (BasePos.Z >= ChunkMax.Z || BasePos.Z + Height < ChunkMin.Z)
					{
						continue;
					}

					StampTree(BasePos, Tmpl, TreeSeeds[i], ChunkCoord, ChunkSize, InOutVoxelData);
				}
			}
		}
	}
}

void FVoxelTreeInjector::ComputeTreePositionsForChunk(
	const FIntVector& SourceChunkCoord,
	int32 ChunkSize, float VoxelSize,
	const FVector& WorldOrigin, int32 WorldSeed,
	const FVoxelNoiseParams& NoiseParams,
	const IVoxelWorldMode& WorldMode,
	float TreeDensity,
	const TArray<FVoxelTreeTemplate>& Templates,
	const UVoxelBiomeConfiguration* BiomeConfig,
	bool bEnableWaterLevel, float WaterLevel,
	TArray<FIntVector>& OutGlobalVoxelPositions,
	TArray<int32>& OutTemplateIDs,
	TArray<uint32>& OutSeeds)
{
	if (Templates.Num() == 0 || TreeDensity <= 0.0f)
	{
		return;
	}

	uint32 ChunkSeed = ComputeTreeChunkSeed(SourceChunkCoord, WorldSeed);

	// Determine number of trees: integer part guaranteed, fractional part is probability
	const int32 GuaranteedTrees = FMath::FloorToInt(TreeDensity);
	const float FractionalChance = TreeDensity - GuaranteedTrees;

	int32 NumTrees = GuaranteedTrees;
	if (FractionalChance > 0.0f && RandomFromSeed(ChunkSeed) < FractionalChance)
	{
		++NumTrees;
	}

	if (NumTrees <= 0)
	{
		return;
	}

	// Chunk origin in global voxel coordinates
	const FIntVector ChunkVoxelOrigin = SourceChunkCoord * ChunkSize;

	for (int32 TreeIdx = 0; TreeIdx < NumTrees; ++TreeIdx)
	{
		// Random X,Y within source chunk (in local voxel coords)
		const int32 LocalX = RandomIntFromSeed(ChunkSeed, 0, ChunkSize - 1);
		const int32 LocalY = RandomIntFromSeed(ChunkSeed, 0, ChunkSize - 1);

		// Random template selection
		const int32 TemplateIdx = RandomIntFromSeed(ChunkSeed, 0, Templates.Num() - 1);

		// Per-tree seed (for height/radius variation in StampTree)
		const uint32 TreeSeed = ChunkSeed ^ (TreeIdx * 2654435761u);

		// Sample terrain height at this XY position using world mode
		const float WorldX = WorldOrigin.X + (ChunkVoxelOrigin.X + LocalX) * VoxelSize;
		const float WorldY = WorldOrigin.Y + (ChunkVoxelOrigin.Y + LocalY) * VoxelSize;

		const float TerrainHeight = WorldMode.GetTerrainHeightAt(WorldX, WorldY, NoiseParams);

		// Advance seed state to maintain determinism (must happen before any continue)
		RandomFromSeed(ChunkSeed);

		// ==================== Placement Filtering ====================
		const FVoxelTreeTemplate& Tmpl = Templates[TemplateIdx];

		// Water level check: skip trees below water
		if (bEnableWaterLevel && TerrainHeight < WaterLevel)
		{
			continue;
		}

		// Compute slope at tree position
		const float SlopeAngle = ComputeSlopeAt(WorldX, WorldY, VoxelSize, WorldMode, NoiseParams);

		// Query surface material and biome
		uint8 SurfaceMaterial = 0;
		uint8 BiomeID = 0;
		QuerySurfaceConditions(WorldX, WorldY, TerrainHeight, VoxelSize,
			BiomeConfig, WorldSeed, bEnableWaterLevel, WaterLevel,
			SurfaceMaterial, BiomeID);

		// Check per-template placement rules
		if (!Tmpl.CanSpawnAt(TerrainHeight, SlopeAngle, SurfaceMaterial, BiomeID))
		{
			continue;
		}

		// Convert world height to global voxel Z
		const float LocalZ = (TerrainHeight - WorldOrigin.Z) / VoxelSize;
		const int32 GlobalZ = FMath::FloorToInt(LocalZ);

		// Verify the tree base is on solid ground (Z is the last solid voxel, tree starts on top)
		const FIntVector GlobalVoxelPos(ChunkVoxelOrigin.X + LocalX, ChunkVoxelOrigin.Y + LocalY, GlobalZ);

		OutGlobalVoxelPositions.Add(GlobalVoxelPos);
		OutTemplateIDs.Add(TemplateIdx);
		OutSeeds.Add(TreeSeed);
	}
}

float FVoxelTreeInjector::ComputeSlopeAt(
	float WorldX, float WorldY, float VoxelSize,
	const IVoxelWorldMode& WorldMode,
	const FVoxelNoiseParams& NoiseParams)
{
	// Sample terrain height at 4 neighboring positions to compute gradient
	const float Step = VoxelSize;

	const float HX0 = WorldMode.GetTerrainHeightAt(WorldX - Step, WorldY, NoiseParams);
	const float HX1 = WorldMode.GetTerrainHeightAt(WorldX + Step, WorldY, NoiseParams);
	const float HY0 = WorldMode.GetTerrainHeightAt(WorldX, WorldY - Step, NoiseParams);
	const float HY1 = WorldMode.GetTerrainHeightAt(WorldX, WorldY + Step, NoiseParams);

	// Central difference gradient
	const float DX = (HX1 - HX0) / (2.0f * Step);
	const float DY = (HY1 - HY0) / (2.0f * Step);

	// Slope angle from gradient magnitude
	const float GradientMag = FMath::Sqrt(DX * DX + DY * DY);
	return FMath::RadiansToDegrees(FMath::Atan(GradientMag));
}

void FVoxelTreeInjector::QuerySurfaceConditions(
	float WorldX, float WorldY, float TerrainHeight, float VoxelSize,
	const UVoxelBiomeConfiguration* BiomeConfig,
	int32 WorldSeed,
	bool bEnableWaterLevel, float WaterLevel,
	uint8& OutSurfaceMaterial, uint8& OutBiomeID)
{
	OutSurfaceMaterial = 0;
	OutBiomeID = 0;

	if (!BiomeConfig || !BiomeConfig->IsValid())
	{
		return;
	}

	// Sample temperature and moisture noise (same as VoxelCPUNoiseGenerator)
	FVoxelNoiseParams TempNoiseParams;
	TempNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	TempNoiseParams.Octaves = 2;
	TempNoiseParams.Persistence = 0.5f;
	TempNoiseParams.Lacunarity = 2.0f;
	TempNoiseParams.Amplitude = 1.0f;
	TempNoiseParams.Seed = WorldSeed + BiomeConfig->TemperatureSeedOffset;
	TempNoiseParams.Frequency = BiomeConfig->TemperatureNoiseFrequency;

	FVoxelNoiseParams MoistureNoiseParams;
	MoistureNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	MoistureNoiseParams.Octaves = 2;
	MoistureNoiseParams.Persistence = 0.5f;
	MoistureNoiseParams.Lacunarity = 2.0f;
	MoistureNoiseParams.Amplitude = 1.0f;
	MoistureNoiseParams.Seed = WorldSeed + BiomeConfig->MoistureSeedOffset;
	MoistureNoiseParams.Frequency = BiomeConfig->MoistureNoiseFrequency;

	// Sample at this world position (Z=0 for 2D biome sampling)
	const FVector BiomeSamplePos(WorldX, WorldY, 0.0f);
	const float Temperature = FVoxelCPUNoiseGenerator::FBM3D(BiomeSamplePos, TempNoiseParams);
	const float Moisture = FVoxelCPUNoiseGenerator::FBM3D(BiomeSamplePos, MoistureNoiseParams);

	// Select biome
	FBiomeBlend Blend = BiomeConfig->GetBiomeBlend(Temperature, Moisture);
	OutBiomeID = Blend.GetDominantBiome();

	// Get surface material (depth = 0 for surface)
	const bool bIsUnderwater = bEnableWaterLevel && TerrainHeight < WaterLevel;
	if (bIsUnderwater)
	{
		OutSurfaceMaterial = BiomeConfig->GetBlendedMaterialWithWater(Blend, 0.0f, TerrainHeight, WaterLevel);
	}
	else
	{
		OutSurfaceMaterial = BiomeConfig->GetBlendedMaterial(Blend, 0.0f);
	}

	// Apply height material rules (snow on peaks, etc.)
	if (BiomeConfig->bEnableHeightMaterials)
	{
		OutSurfaceMaterial = BiomeConfig->ApplyHeightMaterialRules(OutSurfaceMaterial, TerrainHeight, 0.0f);
	}
}

void FVoxelTreeInjector::StampTree(
	const FIntVector& BaseGlobalVoxel,
	const FVoxelTreeTemplate& Template,
	uint32 TreeSeed,
	const FIntVector& ChunkCoord,
	int32 ChunkSize,
	TArray<FVoxelData>& InOutVoxelData)
{
	const FIntVector ChunkMin = ChunkCoord * ChunkSize;

	// Helper: write a voxel if it's within this chunk's bounds
	auto SetVoxel = [&](int32 GX, int32 GY, int32 GZ, uint8 MaterialID, bool bOnlyReplaceAir)
	{
		const int32 LX = GX - ChunkMin.X;
		const int32 LY = GY - ChunkMin.Y;
		const int32 LZ = GZ - ChunkMin.Z;

		if (LX < 0 || LX >= ChunkSize || LY < 0 || LY >= ChunkSize || LZ < 0 || LZ >= ChunkSize)
		{
			return;
		}

		const int32 Index = LX + LY * ChunkSize + LZ * ChunkSize * ChunkSize;
		FVoxelData& Voxel = InOutVoxelData[Index];

		if (bOnlyReplaceAir && Voxel.IsSolid())
		{
			return; // Don't overwrite existing terrain
		}

		Voxel.Density = 255; // Fully solid
		Voxel.MaterialID = MaterialID;
	};

	// Compute actual tree dimensions from template + variance
	uint32 VarianceSeed = TreeSeed;
	const int32 ActualTrunkHeight = Template.TrunkHeight +
		RandomIntFromSeed(VarianceSeed, -Template.TrunkHeightVariance, Template.TrunkHeightVariance);
	const int32 ActualCanopyRadius = Template.CanopyRadius +
		RandomIntFromSeed(VarianceSeed, -Template.CanopyRadiusVariance, Template.CanopyRadiusVariance);

	const int32 TrunkH = FMath::Max(1, ActualTrunkHeight);
	const int32 CanopyR = FMath::Max(1, ActualCanopyRadius);

	// ==================== Trunk ====================
	for (int32 Z = 0; Z < TrunkH; ++Z)
	{
		const int32 GZ = BaseGlobalVoxel.Z + Z;

		if (Template.TrunkRadius == 0)
		{
			// 1x1 trunk
			SetVoxel(BaseGlobalVoxel.X, BaseGlobalVoxel.Y, GZ, Template.TrunkMaterialID, false);
		}
		else
		{
			// Cross pattern (3x3 minus corners for radius 1)
			for (int32 DX = -Template.TrunkRadius; DX <= Template.TrunkRadius; ++DX)
			{
				for (int32 DY = -Template.TrunkRadius; DY <= Template.TrunkRadius; ++DY)
				{
					// Skip corners for cross pattern
					if (FMath::Abs(DX) + FMath::Abs(DY) > Template.TrunkRadius)
					{
						continue;
					}
					SetVoxel(BaseGlobalVoxel.X + DX, BaseGlobalVoxel.Y + DY, GZ, Template.TrunkMaterialID, false);
				}
			}
		}
	}

	// ==================== Canopy ====================
	const FIntVector CanopyCenter(
		BaseGlobalVoxel.X,
		BaseGlobalVoxel.Y,
		BaseGlobalVoxel.Z + TrunkH + Template.CanopyVerticalOffset
	);

	const float CanopyRSq = static_cast<float>(CanopyR * CanopyR);

	for (int32 DX = -CanopyR; DX <= CanopyR; ++DX)
	{
		for (int32 DY = -CanopyR; DY <= CanopyR; ++DY)
		{
			for (int32 DZ = -CanopyR; DZ <= CanopyR; ++DZ)
			{
				bool bInside = false;

				switch (Template.CanopyShape)
				{
				case ETreeCanopyShape::Sphere:
				{
					const float DistSq = static_cast<float>(DX * DX + DY * DY + DZ * DZ);
					bInside = DistSq <= CanopyRSq;
					break;
				}
				case ETreeCanopyShape::Cone:
				{
					// Cone: wider at bottom (DZ = -CanopyR), narrows at top (DZ = +CanopyR)
					const float T = static_cast<float>(DZ + CanopyR) / static_cast<float>(2 * CanopyR);
					const float RadiusAtZ = CanopyR * (1.0f - T * 0.8f); // 80% taper
					const float DistXY = FMath::Sqrt(static_cast<float>(DX * DX + DY * DY));
					bInside = DistXY <= RadiusAtZ;
					break;
				}
				case ETreeCanopyShape::FlatDisc:
				{
					const float DistXY = FMath::Sqrt(static_cast<float>(DX * DX + DY * DY));
					bInside = DistXY <= CanopyR && FMath::Abs(DZ) <= 1;
					break;
				}
				case ETreeCanopyShape::RoundedCube:
				{
					// Manhattan distance check with slight rounding
					const int32 Manhattan = FMath::Abs(DX) + FMath::Abs(DY) + FMath::Abs(DZ);
					bInside = FMath::Abs(DX) <= CanopyR && FMath::Abs(DY) <= CanopyR &&
						FMath::Abs(DZ) <= CanopyR && Manhattan <= CanopyR + (CanopyR / 2);
					break;
				}
				}

				if (bInside)
				{
					SetVoxel(
						CanopyCenter.X + DX,
						CanopyCenter.Y + DY,
						CanopyCenter.Z + DZ,
						Template.LeafMaterialID,
						true // Only replace air
					);
				}
			}
		}
	}
}

uint32 FVoxelTreeInjector::ComputeTreeChunkSeed(const FIntVector& ChunkCoord, int32 WorldSeed)
{
	// FNV-1a hash with a different salt than scatter placement to avoid correlation
	uint32 Seed = static_cast<uint32>(WorldSeed) ^ 0xDEADBEEFu;
	Seed ^= static_cast<uint32>(ChunkCoord.X);
	Seed *= 16777619u;
	Seed ^= static_cast<uint32>(ChunkCoord.Y);
	Seed *= 16777619u;
	Seed ^= static_cast<uint32>(ChunkCoord.Z);
	Seed *= 16777619u;
	return Seed;
}

float FVoxelTreeInjector::RandomFromSeed(uint32& Seed)
{
	Seed = Seed * 1664525u + 1013904223u;
	return static_cast<float>(Seed & 0x7FFFFFFF) / static_cast<float>(0x80000000u);
}

int32 FVoxelTreeInjector::RandomIntFromSeed(uint32& Seed, int32 Min, int32 Max)
{
	if (Min >= Max)
	{
		return Min;
	}
	const float T = RandomFromSeed(Seed);
	return Min + FMath::FloorToInt(T * (Max - Min + 1));
}
