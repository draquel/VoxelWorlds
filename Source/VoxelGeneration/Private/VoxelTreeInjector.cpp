// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelTreeInjector.h"
#include "VoxelCoreTypes.h"
#include "IVoxelWorldMode.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelSurfaceQuery.h"

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

	// Compute max tree horizontal extent to determine XY neighbor search radius
	int32 MaxExtent = 0;
	for (const FVoxelTreeTemplate& Tmpl : Templates)
	{
		MaxExtent = FMath::Max(MaxExtent, Tmpl.GetMaxHorizontalExtent());
	}

	// Neighbor search radius in chunks (how far a tree from a neighbor could reach into this chunk)
	const int32 SearchRadiusChunks = FMath::Max(1, FMath::CeilToInt(static_cast<float>(MaxExtent) / ChunkSize));

	// Iterate over this chunk and XY neighbor chunks
	// Tree placement is 2D (terrain height determines Z), so no DZ loop needed
	const FIntVector ChunkMin = ChunkCoord * ChunkSize;
	const FIntVector ChunkMax = ChunkMin + FIntVector(ChunkSize, ChunkSize, ChunkSize);

	for (int32 DX = -SearchRadiusChunks; DX <= SearchRadiusChunks; ++DX)
	{
		for (int32 DY = -SearchRadiusChunks; DY <= SearchRadiusChunks; ++DY)
		{
			// Source chunk uses target's Z (irrelevant since seed is 2D)
			const FIntVector SourceChunk(ChunkCoord.X + DX, ChunkCoord.Y + DY, ChunkCoord.Z);

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

				// Underground check: skip trees whose base is under an overhang or cave roof.
				// Scan upward from the air voxel above the base looking for any solid.
				{
					const FIntVector LocalBase = FIntVector(BasePos.X, BasePos.Y, BasePos.Z + 1) - ChunkMin;
					if (LocalBase.X >= 0 && LocalBase.X < ChunkSize &&
						LocalBase.Y >= 0 && LocalBase.Y < ChunkSize &&
						LocalBase.Z >= 0 && LocalBase.Z < ChunkSize)
					{
						bool bIsUnderground = false;
						const int32 BaseIdx = LocalBase.X + LocalBase.Y * ChunkSize + LocalBase.Z * ChunkSize * ChunkSize;

						// Fast path: check generation-time underground flag
						if (InOutVoxelData[BaseIdx].HasUndergroundFlag())
						{
							bIsUnderground = true;
						}
						else
						{
							// Column scan: look for solid above within this chunk
							for (int32 ScanZ = LocalBase.Z + 1; ScanZ < ChunkSize; ++ScanZ)
							{
								const int32 ScanIdx = LocalBase.X + LocalBase.Y * ChunkSize + ScanZ * ChunkSize * ChunkSize;
								if (InOutVoxelData[ScanIdx].IsSolid())
								{
									bIsUnderground = true;
									break;
								}
							}
						}

						if (bIsUnderground)
						{
							continue;
						}
					}
				}

				StampTree(BasePos, Tmpl, TreeSeeds[i], ChunkCoord, ChunkSize, InOutVoxelData);
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

	// Hoisted biome snapshot for the per-tree surface queries below (plain data; same lifetime
	// contract as the generator's own use of the config — it outlives generation).
	const FVoxelBiomeSnapshot BiomeSnapshot = FVoxelBiomeSnapshot::FromConfig(BiomeConfig);

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

		// Compute slope at tree position (shared surface-query utility)
		const float SlopeAngle = FVoxelSurfaceQuery::ComputeSlopeDegrees(WorldMode, WorldX, WorldY, VoxelSize, NoiseParams);

		// Query surface material and biome (shared surface-query utility)
		uint8 SurfaceMaterial = 0;
		uint8 BiomeID = 0;
		FVoxelSurfaceQuery::QuerySurfaceConditions(WorldX, WorldY, TerrainHeight, VoxelSize,
			BiomeSnapshot, WorldSeed, bEnableWaterLevel, WaterLevel,
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
	// bOnlyReplaceAir: true = skip if existing voxel is solid terrain (but allow overwriting other tree materials)
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
			// Allow overwriting other tree materials (leaves from neighboring trees)
			// but don't overwrite natural terrain
			const bool bIsTreeMaterial = (Voxel.MaterialID == Template.TrunkMaterialID || Voxel.MaterialID == Template.LeafMaterialID);
			if (!bIsTreeMaterial)
			{
				return; // Don't overwrite existing terrain
			}
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

	// ==================== Canopy (stamped first so trunk can punch through) ====================
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

	// ==================== Trunk (stamped second to punch through canopy) ====================
	// Trunk extends from ground up to the canopy center so it pokes into the leaf ball
	const int32 TotalTrunkH = TrunkH + Template.CanopyVerticalOffset + 1;
	for (int32 Z = 0; Z < TotalTrunkH; ++Z)
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
}

uint32 FVoxelTreeInjector::ComputeTreeChunkSeed(const FIntVector& ChunkCoord, int32 WorldSeed)
{
	// FNV-1a hash using only X,Y — tree placement is 2D (determined by terrain height)
	// Including Z would create different tree sets per Z-level chunk, causing phantom trees
	uint32 Seed = static_cast<uint32>(WorldSeed) ^ 0xDEADBEEFu;
	Seed ^= static_cast<uint32>(ChunkCoord.X);
	Seed *= 16777619u;
	Seed ^= static_cast<uint32>(ChunkCoord.Y);
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
