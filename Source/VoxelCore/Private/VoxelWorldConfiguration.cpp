// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWorldConfiguration.h"
#include "VoxelCore.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelTreeTypes.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

/** Editor-side half of UVoxelWorldConfiguration::GetOverridePath (the command line wins). */
static TAutoConsoleVariable<FString> CVarVoxelConfigOverride(
	TEXT("voxel.ConfigOverride"),
	TEXT(""),
	TEXT("Object path of a UVoxelWorldConfiguration that replaces the voxel world actor's own on the next ")
	TEXT("initialize (e.g. /Game/PluginTesting/Review/Review_Cubic). Empty = use the actor's config. ")
	TEXT("The -VoxelConfig=<path> command-line switch takes priority."),
	ECVF_Default);

UVoxelWorldConfiguration::UVoxelWorldConfiguration()
{
	// Setup default LOD bands matched to default ViewDistance (10000 units)
	// Default: ChunkSize=32, VoxelSize=100 -> ChunkWorldSize=3200 units
	//
	// Design principles:
	// - Bands should cover up to ViewDistance (not far beyond)
	// - MorphRange ~25% of band width for smooth transitions
	// - Wider bands at distance for stability (less popping)
	// - Last band extends to ViewDistance
	//
	// When changing ViewDistance, adjust bands proportionally or extend the last band.
	LODBands.Empty();
	LODBands.Add(FLODBand(0.0f, 4000.0f, 0, 1, 32));      // LOD 0: 0-4000, full detail
	LODBands.Add(FLODBand(4000.0f, 7000.0f, 1, 2, 32));   // LOD 1: 4000-7000, half detail
	LODBands.Add(FLODBand(7000.0f, 10000.0f, 2, 4, 32));  // LOD 2: 7000-10000, quarter detail

	// Set morph ranges to 25% of band width for smooth transitions
	for (FLODBand& Band : LODBands)
	{
		Band.MorphRange = (Band.MaxDistance - Band.MinDistance) * 0.25f;
	}

	// Biome configuration (BiomeConfiguration data asset) contains:
	// - Biome definitions (Plains, Desert, Tundra, etc.)
	// - Biome blending parameters
	// - Height material rules (snow at peaks, rock at altitude)
	// If BiomeConfiguration is null, a default one will be created at runtime

	// Default tree templates
	{
		FVoxelTreeTemplate Oak;
		Oak.TemplateID = 0;
		Oak.Name = TEXT("Oak");
		Oak.TrunkHeight = 5;
		Oak.TrunkHeightVariance = 3;
		Oak.TrunkRadius = 0;
		Oak.TrunkMaterialID = 20; // Wood
		Oak.CanopyShape = ETreeCanopyShape::Sphere;
		Oak.CanopyRadius = 3;
		Oak.CanopyRadiusVariance = 1;
		Oak.LeafMaterialID = 21; // Leaves
		Oak.CanopyVerticalOffset = 0;
		Oak.AllowedMaterials = { 0 }; // Grass only
		Oak.MaxSlopeDegrees = 30.0f;
		TreeTemplates.Add(Oak);

		FVoxelTreeTemplate Birch;
		Birch.TemplateID = 1;
		Birch.Name = TEXT("Birch");
		Birch.TrunkHeight = 7;
		Birch.TrunkHeightVariance = 4;
		Birch.TrunkRadius = 0;
		Birch.TrunkMaterialID = 20;
		Birch.CanopyShape = ETreeCanopyShape::Sphere;
		Birch.CanopyRadius = 2;
		Birch.CanopyRadiusVariance = 1;
		Birch.LeafMaterialID = 21;
		Birch.CanopyVerticalOffset = 0;
		Birch.AllowedMaterials = { 0 }; // Grass only
		Birch.MaxSlopeDegrees = 25.0f;
		TreeTemplates.Add(Birch);

		FVoxelTreeTemplate Bush;
		Bush.TemplateID = 2;
		Bush.Name = TEXT("Bush");
		Bush.TrunkHeight = 1;
		Bush.TrunkHeightVariance = 1;
		Bush.TrunkRadius = 0;
		Bush.TrunkMaterialID = 20;
		Bush.CanopyShape = ETreeCanopyShape::Sphere;
		Bush.CanopyRadius = 2;
		Bush.CanopyRadiusVariance = 1;
		Bush.LeafMaterialID = 21;
		Bush.CanopyVerticalOffset = -1;
		Bush.AllowedMaterials = { 0, 1 }; // Grass and Dirt
		Bush.MaxSlopeDegrees = 40.0f;
		TreeTemplates.Add(Bush);
	}
}

float UVoxelWorldConfiguration::GetChunkWorldSizeAtLOD(int32 LODLevel) const
{
	for (const FLODBand& Band : LODBands)
	{
		if (Band.LODLevel == LODLevel)
		{
			return Band.ChunkSize * Band.VoxelStride * VoxelSize;
		}
	}

	// Fallback to base chunk size
	return GetChunkWorldSize();
}

const FLODBand* UVoxelWorldConfiguration::GetLODBandForDistance(float Distance) const
{
	for (const FLODBand& Band : LODBands)
	{
		if (Band.ContainsDistance(Distance))
		{
			return &Band;
		}
	}

	return nullptr;
}

int32 UVoxelWorldConfiguration::GetLODLevelForDistance(float Distance) const
{
	const FLODBand* Band = GetLODBandForDistance(Distance);
	if (Band)
	{
		return Band->LODLevel;
	}

	// Beyond all bands - return coarsest LOD
	if (LODBands.Num() > 0)
	{
		return LODBands.Last().LODLevel;
	}

	return 0;
}

bool UVoxelWorldConfiguration::ValidateConfiguration() const
{
	bool bValid = true;

	// Check voxel size
	if (VoxelSize <= 0.0f)
	{
		UE_LOG(LogVoxelCore, Error, TEXT("VoxelWorldConfiguration: VoxelSize must be positive"));
		bValid = false;
	}

	// Check chunk size
	if (ChunkSize < 8 || ChunkSize > 128)
	{
		UE_LOG(LogVoxelCore, Warning, TEXT("VoxelWorldConfiguration: ChunkSize %d is outside recommended range [8, 128]"), ChunkSize);
	}

	// Check if chunk size is power of 2
	if ((ChunkSize & (ChunkSize - 1)) != 0)
	{
		UE_LOG(LogVoxelCore, Warning, TEXT("VoxelWorldConfiguration: ChunkSize %d is not a power of 2, may cause issues"), ChunkSize);
	}

	// Check LOD bands
	if (LODBands.Num() == 0)
	{
		UE_LOG(LogVoxelCore, Error, TEXT("VoxelWorldConfiguration: No LOD bands configured"));
		bValid = false;
	}
	else
	{
		// Check for gaps or overlaps in LOD bands
		for (int32 i = 0; i < LODBands.Num(); ++i)
		{
			const FLODBand& Band = LODBands[i];

			if (Band.MinDistance >= Band.MaxDistance)
			{
				UE_LOG(LogVoxelCore, Error, TEXT("VoxelWorldConfiguration: LOD band %d has invalid range [%f, %f]"),
					i, Band.MinDistance, Band.MaxDistance);
				bValid = false;
			}

			if (i > 0)
			{
				const FLODBand& PrevBand = LODBands[i - 1];
				if (FMath::Abs(Band.MinDistance - PrevBand.MaxDistance) > KINDA_SMALL_NUMBER)
				{
					UE_LOG(LogVoxelCore, Warning, TEXT("VoxelWorldConfiguration: Gap between LOD bands %d and %d"), i - 1, i);
				}
			}
		}

		// Check first band starts at 0
		if (LODBands[0].MinDistance > KINDA_SMALL_NUMBER)
		{
			UE_LOG(LogVoxelCore, Warning, TEXT("VoxelWorldConfiguration: First LOD band doesn't start at 0"));
		}
	}

	// Check view distance
	if (ViewDistance <= 0.0f)
	{
		UE_LOG(LogVoxelCore, Error, TEXT("VoxelWorldConfiguration: ViewDistance must be positive"));
		bValid = false;
	}

	// Check streaming settings
	if (MaxChunksToLoadPerFrame < 1)
	{
		UE_LOG(LogVoxelCore, Error, TEXT("VoxelWorldConfiguration: MaxChunksToLoadPerFrame must be at least 1"));
		bValid = false;
	}

	// Settings that run but give wrong results: warnings, not failures.
	for (const FString& Warning : GetSettingsWarnings())
	{
		UE_LOG(LogVoxelCore, Warning, TEXT("VoxelWorldConfiguration %s: %s"), *GetName(), *Warning);
	}

	return bValid;
}

TArray<FString> UVoxelWorldConfiguration::GetSettingsWarnings() const
{
	TArray<FString> Warnings;

	// Tree templates: an id the registry does not define renders with whatever material slot it lands on
	// and is not treated as leaves (not masked, not non-occluding). DemoSmoothWorldConfig shipped leaves
	// 22 and 23 and a trunk of 21 (Leaves), which showed as clay and brick canopies when meshed cubic (KC10).
	const int32 MaterialCount = FVoxelMaterialRegistry::GetMaterialCount();
	auto CheckTreeMaterial = [&Warnings, MaterialCount](const FVoxelTreeTemplate& Template, const TCHAR* Field, uint8 MaterialID)
	{
		if (!FVoxelMaterialRegistry::GetMaterial(MaterialID))
		{
			Warnings.Add(FString::Printf(
				TEXT("tree template %d (%s): %s %d is not a registered voxel material (ids 0-%d; Wood is %d, Leaves %d)"),
				Template.TemplateID, *Template.Name, Field, MaterialID, MaterialCount - 1,
				EVoxelMaterial::Wood, EVoxelMaterial::Leaves));
		}
	};
	for (const FVoxelTreeTemplate& Template : TreeTemplates)
	{
		CheckTreeMaterial(Template, TEXT("TrunkMaterialID"), Template.TrunkMaterialID);
		CheckTreeMaterial(Template, TEXT("LeafMaterialID"), Template.LeafMaterialID);
		for (const uint8 Allowed : Template.AllowedMaterials)
		{
			CheckTreeMaterial(Template, TEXT("AllowedMaterials entry"), Allowed);
		}
	}

	if (WorldMode == EWorldMode::SphericalPlanet)
	{
		// The surface sits at WorldRadius + BaseHeight + offset + noise * PlanetHeightScale * multiplier / 2, noise in
		// [-1, 1], where continentalness gives the offset and the multiplier as in the heightmap modes (SP-B5; none
		// without a biome configuration or with continentalness off). The LOD strategy streams only chunks that meet the
		// shell [WorldRadius - Depth, WorldRadius + Height], and the density query treats points well outside it as air
		// or core, so terrain past the shell is cut off. The bounds are FSphericalPlanetWorldMode::GetDisplacementBounds'.
		float MinOffset = 0.0f, MaxOffset = 0.0f, MaxScaleMult = 1.0f;
		FVoxelBiomeSnapshot::FromConfig(BiomeConfiguration).GetContinentalnessExtremes(MinOffset, MaxOffset, MaxScaleMult);
		const float Amplitude = 0.5f * PlanetHeightScale * MaxScaleMult;
		const float Highest = BaseHeight + MaxOffset + Amplitude;
		const float Deepest = Amplitude - BaseHeight - MinOffset;
		if (Highest > PlanetMaxTerrainHeight)
		{
			Warnings.Add(FString::Printf(
				TEXT("planet terrain reaches %.0f uu above WorldRadius (BaseHeight %.0f + continentalness offset up to %.0f + PlanetHeightScale / 2 x %.2f) but PlanetMaxTerrainHeight is %.0f: peaks above the shell are cut off"),
				Highest, BaseHeight, MaxOffset, MaxScaleMult, PlanetMaxTerrainHeight));
		}
		if (Deepest > PlanetMaxTerrainDepth)
		{
			Warnings.Add(FString::Printf(
				TEXT("planet terrain reaches %.0f uu below WorldRadius (PlanetHeightScale / 2 x %.2f - BaseHeight %.0f - continentalness offset down to %.0f) but PlanetMaxTerrainDepth is %.0f: valleys below the shell are cut off"),
				Deepest, MaxScaleMult, BaseHeight, MinOffset, PlanetMaxTerrainDepth));
		}

		// Generation fills a planet's oceans by WaterRadius; everything else that asks "is this under water"
		// (scatter, navigation, NPC spawning, swimming) still compares against the WaterLevel Z plane, which
		// on a planet splits it into a wet and a dry hemisphere (SP-E1).
		if (bEnableWaterLevel)
		{
			Warnings.Add(FString::Printf(
				TEXT("water is enabled on a planet: generation uses WaterRadius (%.0f), but scatter, navigation, NPC spawning and swimming still read the Z plane WaterLevel (%.0f) until SP-E1"),
				WaterRadius, WaterLevel));
		}
	}

	// The far ocean (WM-E1) is a flat sea at WaterLevel around an island whose edge lies under it. An edge above the
	// water leaves a dry rim at R + falloff that the far ocean meets with no shore.
	if (WorldMode == EWorldMode::IslandBowl && bEnableWaterLevel && bIslandFarOcean && IslandEdgeHeight >= WaterLevel)
	{
		Warnings.Add(FString::Printf(
			TEXT("the island's edge (IslandEdgeHeight %.0f) is not under the water (WaterLevel %.0f): the far ocean meets a dry rim at the world's edge"),
			IslandEdgeHeight, WaterLevel));
	}

	return Warnings;
}

void UVoxelWorldConfiguration::PostLoad()
{
	Super::PostLoad();

	if (!HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		for (const FString& Warning : GetSettingsWarnings())
		{
			UE_LOG(LogVoxelCore, Warning, TEXT("VoxelWorldConfiguration %s: %s"), *GetPathName(), *Warning);
		}
	}
}

float UVoxelWorldConfiguration::GetMaterialLODStartDistance() const
{
	if (LODBands.Num() == 0)
	{
		return 0.0f;
	}

	// Start morphing at first band's morph start point
	const FLODBand& FirstBand = LODBands[0];
	return FMath::Max(0.0f, FirstBand.MaxDistance - FirstBand.MorphRange);
}

float UVoxelWorldConfiguration::GetMaterialLODEndDistance() const
{
	if (LODBands.Num() == 0)
	{
		return ViewDistance;
	}

	// End at the last band's max distance, clamped to ViewDistance
	return FMath::Min(LODBands.Last().MaxDistance, ViewDistance);
}

FVector UVoxelWorldConfiguration::GetPlanetSpawnPosition() const
{
	// Determine spawn direction based on PlanetSpawnLocation setting
	FVector SpawnDirection;
	switch (PlanetSpawnLocation)
	{
	case 0:  // +X (Equator East)
		SpawnDirection = FVector(1.0f, 0.0f, 0.0f);
		break;
	case 1:  // +Y (Equator North)
		SpawnDirection = FVector(0.0f, 1.0f, 0.0f);
		break;
	case 2:  // +Z (North Pole) - Default
		SpawnDirection = FVector(0.0f, 0.0f, 1.0f);
		break;
	case 3:  // -Z (South Pole)
		SpawnDirection = FVector(0.0f, 0.0f, -1.0f);
		break;
	default:
		SpawnDirection = FVector(0.0f, 0.0f, 1.0f);
		break;
	}

	// Calculate spawn position: center + direction * (radius + altitude)
	const float SpawnRadius = WorldRadius + PlanetSpawnAltitude;
	return WorldOrigin + SpawnDirection * SpawnRadius;
}

FString UVoxelWorldConfiguration::GetOverridePath()
{
	FString Path;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelConfig="), Path) && !Path.IsEmpty())
	{
		return Path;
	}
	return CVarVoxelConfigOverride.GetValueOnGameThread().TrimStartAndEnd();
}

#if WITH_EDITOR
void UVoxelWorldConfiguration::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Validate when properties change in editor
	ValidateConfiguration();
}
#endif
