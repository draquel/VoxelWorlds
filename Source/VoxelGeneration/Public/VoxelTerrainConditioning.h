// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelTerrainConditioning.generated.h"

/**
 * A terrain conditioning zone — blends the generated terrain height toward a target within a
 * footprint, with a smooth falloff ring back to the natural terrain.
 *
 * Generation-time and deterministic: the base terrain IS flat here, consistent everywhere as
 * chunks stream, with no edit storage. Produced by IVoxelTerrainConditioner (game-side, e.g. from
 * POI claims) and applied by the heightmap generator before the SDF/density is derived. Pure data —
 * safe to copy onto a generation request and read on background generation threads.
 *
 * See WorldClaims/Documentation/CLAIMS_ARCHITECTURE.md "Terrain conditioning".
 */
USTRUCT(BlueprintType)
struct VOXELGENERATION_API FVoxelConditioningZone
{
	GENERATED_BODY()

	/** Footprint center (world X,Y). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning")
	FVector2D Center = FVector2D::ZeroVector;

	/** Radius (world units) fully blended to TargetHeight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning")
	float InnerRadius = 1000.0f;

	/** Width (world units) of the falloff ring outside InnerRadius, easing back to natural terrain. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning")
	float FalloffWidth = 1000.0f;

	/** World Z the terrain is blended toward inside the footprint. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning")
	float TargetHeight = 0.0f;

	/** Overall strength [0,1]: 1 = fully reach TargetHeight at the center, 0 = no effect. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Strength = 1.0f;

	FVoxelConditioningZone() = default;

	FVoxelConditioningZone(const FVector2D& InCenter, float InInnerRadius, float InFalloffWidth, float InTargetHeight, float InStrength = 1.0f)
		: Center(InCenter), InnerRadius(InInnerRadius), FalloffWidth(InFalloffWidth), TargetHeight(InTargetHeight), Strength(InStrength)
	{
	}

	/** Conditioning weight at (X,Y): Strength inside InnerRadius, smoothstep to 0 across FalloffWidth, 0 beyond. */
	float GetWeightAt(double X, double Y) const
	{
		const double Dx = X - Center.X;
		const double Dy = Y - Center.Y;
		const double Dist = FMath::Sqrt(Dx * Dx + Dy * Dy);
		if (Dist <= InnerRadius)
		{
			return Strength;
		}
		const double Falloff = FMath::Max(1.0, static_cast<double>(FalloffWidth));
		if (Dist >= static_cast<double>(InnerRadius) + Falloff)
		{
			return 0.0f;
		}
		const double T = (Dist - InnerRadius) / Falloff;        // 0 at inner edge, 1 at outer edge
		const double S = 1.0 - (T * T * (3.0 - 2.0 * T));       // smoothstep 1 -> 0
		return static_cast<float>(Strength * S);
	}

	/** XY world-space bounds of the zone's influence (center +/- (InnerRadius + FalloffWidth)). */
	FBox2D GetBounds2D() const
	{
		const float R = InnerRadius + FMath::Max(0.0f, FalloffWidth);
		return FBox2D(Center - FVector2D(R, R), Center + FVector2D(R, R));
	}
};

/**
 * Boundary-safe interface for supplying terrain conditioning zones at generation time.
 *
 * Implemented game-side (e.g. from POI / construction claims). VoxelWorlds stays POI-agnostic — it
 * only asks "what conditioning applies to this region?". Implementations must be pure and
 * thread-safe: the chunk manager queries them while building generation requests.
 */
class VOXELGENERATION_API IVoxelTerrainConditioner
{
public:
	virtual ~IVoxelTerrainConditioner() = default;

	/**
	 * Append conditioning zones overlapping the given world-space XY region.
	 * @param Region    Chunk (or query) XY bounds.
	 * @param OutZones  Append overlapping zones here.
	 */
	virtual void GatherConditioning(const FBox2D& Region, TArray<FVoxelConditioningZone>& OutZones) const = 0;
};

/**
 * Stateless helper for applying conditioning zones to a generated terrain height.
 */
struct FVoxelTerrainConditioning
{
	/**
	 * Blend BaseHeight toward each zone's TargetHeight by the zone's weight at (X,Y). Zones layer in
	 * order (a later zone blends on top of earlier ones). Returns the conditioned height.
	 */
	static float ApplyToHeight(double X, double Y, float BaseHeight, const TArray<FVoxelConditioningZone>& Zones)
	{
		float H = BaseHeight;
		for (const FVoxelConditioningZone& Zone : Zones)
		{
			const float W = Zone.GetWeightAt(X, Y);
			if (W > 0.0f)
			{
				H = FMath::Lerp(H, Zone.TargetHeight, W);
			}
		}
		return H;
	}
};
