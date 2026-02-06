# Ore Veins

**Module**: VoxelCore, VoxelGeneration
**Dependencies**: VoxelBiomeConfiguration, VoxelCPUNoiseGenerator
**Last Updated**: 2026-02-06

## Overview

The ore vein system provides procedural generation of underground ore deposits using 3D noise patterns. Ores can be configured globally (appearing in all biomes) or per-biome (for specialized deposits like desert gold or tundra ice crystals). The system supports two vein shapes and integrates seamlessly with the existing biome and material systems.

## FOreVeinConfig Struct

The `FOreVeinConfig` struct defines all parameters for a single ore type. Located in `VoxelBiomeDefinition.h`.

```cpp
USTRUCT(BlueprintType)
struct VOXELCORE_API FOreVeinConfig
{
    GENERATED_BODY()

    /** Display name for this ore type */
    FString Name;

    /** Material ID for this ore (index into material atlas) */
    uint8 MaterialID = 0;

    /** Minimum depth below surface for ore to spawn (in voxels) */
    float MinDepth = 3.0f;

    /** Maximum depth below surface for ore to spawn (0 = no limit) */
    float MaxDepth = 0.0f;

    /** Shape of ore deposits (Blob or Streak) */
    EOreVeinShape Shape = EOreVeinShape::Blob;

    /** Frequency of ore noise (lower = larger deposits) */
    float Frequency = 0.05f;

    /** Noise threshold for ore placement (higher = rarer ore) */
    float Threshold = 0.85f;

    /** Seed offset for this ore type (added to world seed) */
    int32 SeedOffset = 0;

    /** Rarity multiplier (0-1, lower = rarer) */
    float Rarity = 1.0f;

    /** Stretch factor for streak-shaped veins (only used when Shape == Streak) */
    float StreakStretch = 4.0f;

    /** Priority for ore placement (higher = checked first) */
    int32 Priority = 0;
};
```

### Configuration Fields Reference

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `Name` | FString | "" | Display name for debugging and editor |
| `MaterialID` | uint8 | 0 | Index into material atlas (EVoxelMaterial enum) |
| `MinDepth` | float | 3.0 | Minimum depth below surface in voxels |
| `MaxDepth` | float | 0.0 | Maximum depth (0 = unlimited) |
| `Shape` | EOreVeinShape | Blob | Deposit shape type |
| `Frequency` | float | 0.05 | Noise frequency (0.001-1.0) |
| `Threshold` | float | 0.85 | Placement threshold (0.0-1.0) |
| `SeedOffset` | int32 | 0 | Added to world seed for unique patterns |
| `Rarity` | float | 1.0 | Additional rarity filter (0.0-1.0) |
| `StreakStretch` | float | 4.0 | Elongation factor for streak shapes (1.0-10.0) |
| `Priority` | int32 | 0 | Evaluation order (higher first) |

---

## Vein Shapes

### EOreVeinShape Enum

```cpp
UENUM(BlueprintType)
enum class EOreVeinShape : uint8
{
    /** Blobby, rounded clusters using 3D noise threshold */
    Blob,

    /** Elongated, streak-like veins using anisotropic/directional noise */
    Streak
};
```

### Blob Shape

Blob-shaped deposits create rounded, irregular clusters of ore. This is the most common shape for materials like coal and gold.

**Characteristics:**
- Roughly spherical clusters
- Size controlled by `Frequency` (lower = larger blobs)
- Rarity controlled by `Threshold` (higher = fewer, smaller deposits)
- Natural-looking random distribution

**Best For:**
- Coal deposits (common, shallow)
- Gold nuggets (rare, deep)
- Generic ore clusters

### Streak Shape

Streak-shaped deposits create elongated, vein-like formations. Ideal for iron and copper deposits that naturally occur in linear veins.

**Characteristics:**
- Elongated along a pseudo-random direction
- Direction varies smoothly across the world
- `StreakStretch` controls elongation (4.0 = 4x longer than wide)
- Creates more realistic mining vein patterns

**Implementation:**
```cpp
if (OreConfig.Shape == EOreVeinShape::Streak)
{
    // Create direction vector based on position (varies smoothly across world)
    FVector StreakDir;
    StreakDir.X = FMath::Sin(WorldPos.Y * 0.0001f + StreakSeed);
    StreakDir.Y = FMath::Cos(WorldPos.X * 0.0001f + StreakSeed * 1.5f);
    StreakDir.Z = FMath::Sin(WorldPos.Z * 0.0002f + StreakSeed * 2.0f);
    StreakDir.Normalize();

    // Project position onto perpendicular plane to stretch
    FVector Projected = WorldPos - StreakDir * FVector::DotProduct(WorldPos, StreakDir);
    SamplePos = Projected + StreakDir * (FVector::DotProduct(WorldPos, StreakDir) / OreConfig.StreakStretch);
}
```

**Best For:**
- Iron veins
- Copper deposits
- Any mineral that forms in geological veins

---

## Global vs Biome-Specific Ores

### Global Ore Veins

Global ores are defined in `UVoxelBiomeConfiguration::GlobalOreVeins` and spawn in all biomes by default.

```cpp
/** Global ore vein configurations */
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ore Veins")
TArray<FOreVeinConfig> GlobalOreVeins;
```

### Biome-Specific Ore Veins

Each biome can define its own ore veins in `FBiomeDefinition::BiomeOreVeins`.

```cpp
/** Biome-specific ore veins (optional) */
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Biome|Ore")
TArray<FOreVeinConfig> BiomeOreVeins;

/** If true, biome ores ADD to global ores. If false, biome ores REPLACE global ores. */
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Biome|Ore")
bool bAddToGlobalOres = false;
```

### Ore Resolution Logic

The system resolves which ores to use via `GetOreVeinsForBiome()`:

```cpp
void UVoxelBiomeConfiguration::GetOreVeinsForBiome(uint8 BiomeID, TArray<FOreVeinConfig>& OutOres) const
{
    // If biome has its own ores...
    if (Biome && Biome->BiomeOreVeins.Num() > 0)
    {
        if (Biome->bAddToGlobalOres)
        {
            // Combine biome ores with global ores
            OutOres = SortedGlobalOres;
            OutOres.Append(Biome->BiomeOreVeins);
            // Re-sort by priority
        }
        else
        {
            // Biome ores REPLACE global ores
            OutOres = Biome->BiomeOreVeins;
        }
    }
    else
    {
        // Use global ores
        OutOres = SortedGlobalOres;
    }
}
```

### Configuration Scenarios

| Biome Config | bAddToGlobalOres | Result |
|--------------|------------------|--------|
| Empty BiomeOreVeins | N/A | Uses GlobalOreVeins |
| Has BiomeOreVeins | false | Uses ONLY BiomeOreVeins |
| Has BiomeOreVeins | true | Combines both, sorted by priority |

---

## Default Ore Types

The system initializes with three default ore types:

### Coal (Priority: 10)

```cpp
FOreVeinConfig(
    TEXT("Coal"),
    EVoxelMaterial::Coal,    // MaterialID
    5.0f,                    // MinDepth
    50.0f,                   // MaxDepth
    EOreVeinShape::Blob,     // Shape
    0.08f,                   // Frequency
    0.82f,                   // Threshold
    100,                     // SeedOffset
    10                       // Priority
)
```

**Characteristics:**
- Most common ore
- Shallow to medium depth (5-50 voxels)
- Blob-shaped clusters
- Low threshold (0.82) = more frequent
- Lowest priority (checked last)

### Iron (Priority: 20)

```cpp
FOreVeinConfig(
    TEXT("Iron"),
    EVoxelMaterial::Iron,    // MaterialID
    10.0f,                   // MinDepth
    100.0f,                  // MaxDepth
    EOreVeinShape::Streak,   // Shape (elongated veins)
    0.06f,                   // Frequency
    0.87f,                   // Threshold
    200,                     // SeedOffset
    20                       // Priority
)
```

**Characteristics:**
- Moderate rarity
- Medium depth (10-100 voxels)
- Streak-shaped elongated veins
- Medium threshold (0.87)
- Medium priority (overrides coal)

### Gold (Priority: 30)

```cpp
FOreVeinConfig(
    TEXT("Gold"),
    EVoxelMaterial::Gold,    // MaterialID
    25.0f,                   // MinDepth
    0.0f,                    // MaxDepth (no limit)
    EOreVeinShape::Blob,     // Shape
    0.04f,                   // Frequency
    0.93f,                   // Threshold (rare)
    300,                     // SeedOffset
    30                       // Priority (highest)
)
```

**Characteristics:**
- Rare ore
- Deep only (25+ voxels, no maximum)
- Small blob clusters (low frequency)
- High threshold (0.93) = very rare
- Highest priority (checked first, can override others)

---

## Integration Points

### VoxelBiomeConfiguration

The biome configuration asset contains the master toggle and global ore definitions:

```cpp
class VOXELCORE_API UVoxelBiomeConfiguration : public UDataAsset
{
    /** Enable ore vein generation */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ore Veins")
    bool bEnableOreVeins = true;

    /** Global ore vein configurations */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ore Veins")
    TArray<FOreVeinConfig> GlobalOreVeins;

    /** Get applicable ores for a biome */
    void GetOreVeinsForBiome(uint8 BiomeID, TArray<FOreVeinConfig>& OutOres) const;

    /** Check if ore veins are enabled and configured */
    bool HasOreVeins() const { return bEnableOreVeins && GlobalOreVeins.Num() > 0; }
};
```

### VoxelCPUNoiseGenerator

The CPU noise generator applies ore placement during chunk generation:

```cpp
class VOXELGENERATION_API FVoxelCPUNoiseGenerator
{
    /** Sample ore vein noise at a position */
    static float SampleOreVeinNoise(
        const FVector& WorldPos,
        const FOreVeinConfig& OreConfig,
        int32 WorldSeed);

    /** Check if ore should be placed here */
    static bool CheckOreVeinPlacement(
        const FVector& WorldPos,
        float DepthBelowSurface,
        const TArray<FOreVeinConfig>& OreConfigs,
        int32 WorldSeed,
        uint8& OutMaterialID);
};
```

### Ore Placement in Generation Pipeline

Ore veins are applied during voxel generation, after biome and height material rules:

```cpp
// In GenerateChunkInfinitePlane (and other world mode generators):

// Apply ore vein overrides (only for solid voxels below surface)
if (Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.5f)
{
    TArray<FOreVeinConfig> ApplicableOres;
    BiomeConfig->GetOreVeinsForBiome(BiomeID, ApplicableOres);

    uint8 OreMaterial = 0;
    if (CheckOreVeinPlacement(WorldPos, DepthBelowSurface, ApplicableOres, Request.NoiseParams.Seed, OreMaterial))
    {
        MaterialID = OreMaterial;
    }
}
```

---

## Usage Examples

### Example 1: Adding a New Global Ore

To add a new ore type that spawns in all biomes:

```cpp
// In your configuration setup or blueprint
UVoxelBiomeConfiguration* Config = /* your config */;

FOreVeinConfig Diamond;
Diamond.Name = TEXT("Diamond");
Diamond.MaterialID = EVoxelMaterial::Diamond;  // Assuming you've added this
Diamond.MinDepth = 50.0f;   // Very deep only
Diamond.MaxDepth = 0.0f;    // No limit
Diamond.Shape = EOreVeinShape::Blob;
Diamond.Frequency = 0.02f;  // Small clusters
Diamond.Threshold = 0.96f;  // Very rare
Diamond.SeedOffset = 400;
Diamond.Priority = 40;      // Highest priority

Config->GlobalOreVeins.Add(Diamond);
```

### Example 2: Desert-Specific Ore

To add an ore that only appears in the desert biome:

```cpp
// Get the desert biome definition
FBiomeDefinition& Desert = Config->Biomes[1];  // Assuming index 1

FOreVeinConfig DesertGold;
DesertGold.Name = TEXT("Desert Gold");
DesertGold.MaterialID = EVoxelMaterial::Gold;
DesertGold.MinDepth = 15.0f;  // Shallower in desert
DesertGold.MaxDepth = 40.0f;
DesertGold.Shape = EOreVeinShape::Blob;
DesertGold.Frequency = 0.05f;
DesertGold.Threshold = 0.88f;  // More common than normal gold
DesertGold.SeedOffset = 350;
DesertGold.Priority = 25;

Desert.BiomeOreVeins.Add(DesertGold);
Desert.bAddToGlobalOres = false;  // REPLACE global ores (no coal/iron in desert)
```

### Example 3: Supplementing Global Ores

To add extra ores to a biome while keeping global ores:

```cpp
// Get the tundra biome definition
FBiomeDefinition& Tundra = Config->Biomes[2];

FOreVeinConfig IceCrystal;
IceCrystal.Name = TEXT("Ice Crystal");
IceCrystal.MaterialID = EVoxelMaterial::IceCrystal;
IceCrystal.MinDepth = 3.0f;
IceCrystal.MaxDepth = 30.0f;
IceCrystal.Shape = EOreVeinShape::Streak;  // Crystalline veins
IceCrystal.Frequency = 0.07f;
IceCrystal.Threshold = 0.84f;
IceCrystal.StreakStretch = 6.0f;  // Extra elongated
IceCrystal.SeedOffset = 500;
IceCrystal.Priority = 15;

Tundra.BiomeOreVeins.Add(IceCrystal);
Tundra.bAddToGlobalOres = true;  // ADD to coal/iron/gold
```

### Example 4: Blueprint Configuration

In the Unreal Editor, you can configure ores directly in the `UVoxelBiomeConfiguration` data asset:

1. Create a new `VoxelBiomeConfiguration` asset in Content Browser
2. Expand "Ore Veins" category
3. Set `bEnableOreVeins` to true
4. Click `+` on `GlobalOreVeins` to add new ore types
5. Configure each ore's properties in the Details panel
6. For biome-specific ores, expand a biome in the `Biomes` array
7. Add ores to `BiomeOreVeins` and set `bAddToGlobalOres` as needed

---

## Tuning Guide

### Making Ores Rarer

- Increase `Threshold` (0.85 -> 0.95)
- Decrease `Rarity` (1.0 -> 0.5)
- Increase `MinDepth` to restrict to deeper areas

### Making Deposits Larger

- Decrease `Frequency` (0.05 -> 0.02)
- Decrease `Threshold` slightly (0.90 -> 0.85)

### Making Deposits Longer (Streak)

- Use `EOreVeinShape::Streak`
- Increase `StreakStretch` (4.0 -> 8.0)

### Controlling Overlap

- Higher `Priority` ores are checked first
- Gold (priority 30) can appear where coal (priority 10) would otherwise be
- Use different `SeedOffset` values to ensure unique noise patterns

---

## Performance Notes

- Ore placement uses 2-octave Simplex noise (same as biome sampling)
- Each ore config adds one noise sample per solid voxel
- Ores are only checked for voxels with `Density >= VOXEL_SURFACE_THRESHOLD`
- Depth check (`IsValidDepth`) is performed before noise sampling
- Priority sorting is cached in `UVoxelBiomeConfiguration`

**Typical Performance:**
- 3 ore types: ~5% overhead on chunk generation
- 10 ore types: ~15% overhead on chunk generation
- Recommend limiting to 8-12 ore types per biome

---

## Constraints

- Maximum of `MAX_ORE_VEINS` (16) ore configurations per resolution
- Ore material IDs must be valid indices in the material atlas
- MinDepth must be >= 0
- Threshold should be in range [0.7, 0.98] for reasonable results
- Frequency should be in range [0.01, 0.2] for visible deposits

---

**Status**: Fully implemented for CPU generation. GPU compute shader integration planned for future phase.
