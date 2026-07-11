// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.h"   // FVoxelNoiseParams, EVoxelNoiseType, EWorldMode

class IVoxelWorldMode;
class UVoxelWorldConfiguration;
class UVoxelBiomeConfiguration;
class UVoxelCaveConfiguration;

/**
 * How a field is sampled and how its samples map to display.
 */
enum class EVoxelFieldKind : uint8
{
	/** Continuous value sampled at a world (X,Y); colored via a heatmap ramp. */
	Scalar2D,
	/** Integer id (material/biome) sampled at a world (X,Y); colored via a palette. */
	Categorical,
	/** Continuous value sampled at a world (X,Y,SampleZ); colored via a heatmap ramp. */
	Scalar3D,
};

/**
 * Which color mapping a field uses.
 */
enum class EVoxelFieldColor : uint8
{
	/** Scalar value in [RangeMin,RangeMax] -> lerp(RampLow, RampHigh). */
	HeatmapRamp,
	/** Integer material id -> FVoxelMaterialRegistry color. */
	MaterialPalette,
	/** Integer biome id -> distinct hashed hue. */
	BiomePalette,
};

/**
 * Immutable snapshot of a world configuration, ready for chunk-independent field sampling.
 *
 * Built once from a UVoxelWorldConfiguration (FromConfiguration) — instantiates the matching
 * IVoxelWorldMode (with biome context, exactly as UVoxelChunkManager::Initialize does) and resolves
 * the biome-climate noise params (matching FVoxelCPUNoiseGenerator / UVoxelMapSubsystem). The world
 * mode is shared (TSharedPtr) and sampled read-only; copies share the same instance.
 */
struct FVoxelFieldSampleContext
{
	/** Analytic world mode instance (owns terrain height/density math + biome context). */
	TSharedPtr<IVoxelWorldMode> WorldMode;

	/** Terrain noise params (its Seed is the base seed for all noise, mirroring generation). */
	FVoxelNoiseParams NoiseParams;

	float VoxelSize = 100.0f;
	FVector WorldOrigin = FVector::ZeroVector;

	/** Biome config (non-owning; kept alive by the source config asset). Null when biomes disabled. */
	const UVoxelBiomeConfiguration* BiomeConfig = nullptr;
	bool bEnableBiomes = false;

	/** Cave config (non-owning). Null when caves disabled. */
	const UVoxelCaveConfiguration* CaveConfig = nullptr;
	bool bEnableCaves = false;

	bool bEnableWaterLevel = false;
	float WaterLevel = 0.0f;

	/** Derived biome-climate noise params (2-octave Simplex + config seed offsets/frequencies). */
	FVoxelNoiseParams TempNoiseParams;
	FVoxelNoiseParams MoistureNoiseParams;
	FVoxelNoiseParams ContinentalnessNoiseParams;
	bool bUseContinentalness = false;

	bool bValid = false;

	bool IsValid() const { return bValid && WorldMode.IsValid(); }

	/** Base seed for all noise (terrain + biome-climate), mirroring generation (NoiseParams.Seed). */
	int32 GetSeed() const { return NoiseParams.Seed; }

	/** Build a context from a world configuration asset. Game thread (instantiates the world mode). */
	static FVoxelFieldSampleContext FromConfiguration(const UVoxelWorldConfiguration* Config);
};

/**
 * A registered, sampleable field (terrain height, biome, cave presence, ...).
 *
 * This is the extension point: adding a new visualization = registering one FVoxelEditorField
 * with a Sample function. The Sample delegate returns a raw float — the continuous value for
 * scalar fields, or an integer id cast to float for categorical fields. Color mapping is derived
 * from Kind/ColorMode by the baker.
 */
struct FVoxelEditorField
{
	/** Stable identifier (used by UI combos, the actor's ActiveField, etc.). */
	FName Id;

	/** Human-readable label. */
	FText DisplayName;

	EVoxelFieldKind Kind = EVoxelFieldKind::Scalar2D;
	EVoxelFieldColor ColorMode = EVoxelFieldColor::HeatmapRamp;

	/** Heatmap ramp: use a fixed [DisplayMin,DisplayMax] range unless bAutoRange (then the baker
	 *  uses the sampled region's min/max, for automatic contrast). Ignored for categorical fields. */
	bool bAutoRange = false;
	float DisplayMin = 0.0f;
	float DisplayMax = 1.0f;
	FLinearColor RampLow = FLinearColor(0.02f, 0.02f, 0.05f);
	FLinearColor RampHigh = FLinearColor(1.0f, 1.0f, 1.0f);

	/** Optional unit label for a legend (e.g. "cm", "deg", "[-1,1]"). */
	FText Units;

	/** Sample the field. Returns the raw scalar value, or (categorical) the integer id as a float. */
	TFunction<float(const FVoxelFieldSampleContext& /*Ctx*/, double /*X*/, double /*Y*/, double /*Z*/)> Sample;

	/** Map a raw scalar sample to a display color using this field's ramp and the given range. */
	FColor MapScalarToColor(float Value, float RangeMin, float RangeMax) const;

	/** Map a raw categorical sample (integer id as float) to a display color via this field's palette. */
	FColor MapCategoricalToColor(float Value) const;

	/** Human-readable label for a category id (biome / material name) — for the legend. Uses this
	 *  field's palette to decide the source and the context for the biome-name lookup. */
	FText GetCategoryLabel(const FVoxelFieldSampleContext& Ctx, int32 CategoryId) const;
};

/**
 * Static registry of built-in + custom fields.
 *
 * Built-ins (Height, Slope, SurfaceMaterial, Biome, Temperature, Moisture, Continentalness,
 * CavePresence) are registered once from module startup. New tools/phases register more via
 * RegisterField — that is the whole cost of adding a new visualization.
 */
class FVoxelFieldRegistry
{
public:
	/** Add or replace a field by Id. */
	static void RegisterField(const FVoxelEditorField& Field);

	/** Find a field by Id, or nullptr. */
	static const FVoxelEditorField* FindField(FName Id);

	/** All registered fields (built-ins first, in registration order). */
	static const TArray<FVoxelEditorField>& GetFields();

	/** Field ids, for populating UI combos / GetOptions. */
	static TArray<FName> GetFieldIds();

	/** Register the built-in fields once (idempotent). Called from FVoxelWorldsEditorModule::StartupModule. */
	static void EnsureBuiltinsRegistered();
};
