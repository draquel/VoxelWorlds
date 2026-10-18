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
 * POI claims) and applied by the generator before the SDF/density is derived. Pure data —
 * safe to copy onto a generation request and read on background generation threads.
 *
 * Two kinds (spherical SP-B10, decision D15 in WORLD_MODES_REVIEW_PLAN.md §19.1):
 *  - A heightmap zone (Direction zero): an XY disc around Center, TargetHeight a world Z; the
 *    heightmap generators blend the column height (ApplyToHeight).
 *  - A planet zone (Direction set, IsRadial): a cap on the planet's datum sphere around Direction,
 *    InnerRadius and FalloffWidth measured along the surface (great-circle distance at the planet
 *    radius), TargetHeight an altitude above the datum; the planet generators blend the terrain's
 *    altitude along each direction (ApplyToAltitude), so a pad is level in gravity. Center is unused.
 * Each kind is ignored by the other's functions, so a zone of the wrong kind contributes nothing.
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

	/** World Z the terrain is blended toward inside the footprint (a planet zone: the altitude above the datum). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning")
	float TargetHeight = 0.0f;

	/** Overall strength [0,1]: 1 = fully reach TargetHeight at the center, 0 = no effect. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Strength = 1.0f;

	/**
	 * Planet zones only: the unit direction from the planet centre to the zone's centre (MakeRadial). Zero (the
	 * default) makes a heightmap zone, which uses Center.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning")
	FVector Direction = FVector::ZeroVector;

	FVoxelConditioningZone() = default;

	FVoxelConditioningZone(const FVector2D& InCenter, float InInnerRadius, float InFalloffWidth, float InTargetHeight, float InStrength = 1.0f)
		: Center(InCenter), InnerRadius(InInnerRadius), FalloffWidth(InFalloffWidth), TargetHeight(InTargetHeight), Strength(InStrength)
	{
	}

	/**
	 * A planet zone (spherical SP-B10): a cap on the datum sphere.
	 *
	 * @param InDirection Direction from the planet centre to the zone's centre (normalized here)
	 * @param InInnerRadius Fully blended radius, along the surface (uu)
	 * @param InFalloffWidth Falloff ring width, along the surface (uu)
	 * @param InTargetAltitude Altitude above the datum the terrain is blended toward
	 * @param InStrength Overall strength [0, 1]
	 * @return The zone
	 */
	static FVoxelConditioningZone MakeRadial(const FVector& InDirection, float InInnerRadius, float InFalloffWidth,
		float InTargetAltitude, float InStrength = 1.0f)
	{
		FVoxelConditioningZone Zone(FVector2D::ZeroVector, InInnerRadius, InFalloffWidth, InTargetAltitude, InStrength);
		Zone.Direction = InDirection.GetSafeNormal();
		return Zone;
	}

	/** Whether this is a planet zone (a direction is set). */
	bool IsRadial() const { return !Direction.IsNearlyZero(); }

	/**
	 * The great-circle distance between two unit directions on a sphere: R * atan2(|A x B|, A . B), well conditioned at
	 * small angles (unlike acos), in float on the GPU too.
	 *
	 * @param A Unit direction
	 * @param B Unit direction
	 * @param Radius Sphere radius
	 * @return Distance along the sphere's surface
	 */
	static double GreatCircleDistance(const FVector& A, const FVector& B, double Radius)
	{
		return Radius * FMath::Atan2(FVector::CrossProduct(A, B).Size(), FVector::DotProduct(A, B));
	}

	/**
	 * A planet zone's weight along a direction: GetWeightAt's profile over the great-circle distance from the zone's
	 * centre on the datum sphere.
	 *
	 * @param InDirection Unit direction from the planet centre
	 * @param PlanetRadius The datum radius
	 * @return The weight (0 for a heightmap zone)
	 */
	float GetWeightAtDirection(const FVector& InDirection, double PlanetRadius) const
	{
		return IsRadial() ? GetWeightAtDistance(GreatCircleDistance(InDirection, Direction, PlanetRadius)) : 0.0f;
	}

	/** A planet zone's influence as an angle from its direction: (InnerRadius + FalloffWidth) / PlanetRadius. */
	double GetAngularRadius(double PlanetRadius) const
	{
		return (static_cast<double>(InnerRadius) + FMath::Max(0.0, static_cast<double>(FalloffWidth))) / FMath::Max(PlanetRadius, 1.0);
	}

	/**
	 * Whether a planet zone's influence reaches a cap (a direction and a half-angle): the angle between their centres is
	 * at most the cap's angle plus the zone's angular radius.
	 *
	 * @param CapDirection Unit direction of the cap's centre
	 * @param CapAngle The cap's half-angle (radians; pi or more covers the sphere)
	 * @param PlanetRadius The datum radius
	 * @return False for a heightmap zone
	 */
	bool MeetsCap(const FVector& CapDirection, double CapAngle, double PlanetRadius) const
	{
		if (!IsRadial())
		{
			return false;
		}
		const double Angle = FMath::Atan2(FVector::CrossProduct(CapDirection, Direction).Size(), FVector::DotProduct(CapDirection, Direction));
		return Angle <= CapAngle + GetAngularRadius(PlanetRadius);
	}

	/** Conditioning weight at (X,Y): Strength inside InnerRadius, smoothstep to 0 across FalloffWidth, 0 beyond. */
	float GetWeightAt(double X, double Y) const
	{
		const double Dx = X - Center.X;
		const double Dy = Y - Center.Y;
		return GetWeightAtDistance(FMath::Sqrt(Dx * Dx + Dy * Dy));
	}

	/** The weight at a distance from the centre: Strength inside InnerRadius, smoothstep to 0 across FalloffWidth, 0 beyond. */
	float GetWeightAtDistance(double Dist) const
	{
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

	/**
	 * Planets (spherical SP-B10): append the planet zones (FVoxelConditioningZone::MakeRadial) whose influence reaches a
	 * cap on the sphere. The default supplies none, so a heightmap-only conditioner conditions nothing on a planet.
	 * @param Direction     Unit direction from the planet centre to the cap's centre
	 * @param AngularRadius The cap's half-angle in radians (a chunk's, or a query's; pi or more is the whole sphere)
	 * @param OutZones      Append overlapping zones here
	 */
	virtual void GatherConditioningOnSphere(const FVector& Direction, double AngularRadius, TArray<FVoxelConditioningZone>& OutZones) const
	{
	}
};

/**
 * Stateless helper for applying conditioning zones to a generated terrain height.
 */
struct FVoxelTerrainConditioning
{
	/**
	 * Blend BaseHeight toward each zone's TargetHeight by the zone's weight at (X,Y). Zones layer in
	 * order (a later zone blends on top of earlier ones). Returns the conditioned height. Planet zones are skipped.
	 */
	static float ApplyToHeight(double X, double Y, float BaseHeight, const TArray<FVoxelConditioningZone>& Zones)
	{
		float H = BaseHeight;
		for (const FVoxelConditioningZone& Zone : Zones)
		{
			if (Zone.IsRadial())
			{
				continue;
			}
			const float W = Zone.GetWeightAt(X, Y);
			if (W > 0.0f)
			{
				H = FMath::Lerp(H, Zone.TargetHeight, W);
			}
		}
		return H;
	}

	/**
	 * Planets (spherical SP-B10): blend the terrain's altitude along a direction toward each planet zone's TargetHeight
	 * (an altitude) by its weight there, layered in order like ApplyToHeight; heightmap zones are skipped. The planet
	 * generators (CPU and GPU) and every reader of the generated planet surface apply it to the radial displacement.
	 *
	 * @param Direction Unit direction from the planet centre
	 * @param PlanetRadius The datum radius (zone distances are measured on it)
	 * @param BaseAltitude The natural terrain's altitude along Direction
	 * @param Zones Zones (planet zones apply)
	 * @return The conditioned altitude
	 */
	static float ApplyToAltitude(const FVector& Direction, double PlanetRadius, float BaseAltitude, const TArray<FVoxelConditioningZone>& Zones)
	{
		float A = BaseAltitude;
		for (const FVoxelConditioningZone& Zone : Zones)
		{
			const float W = Zone.GetWeightAtDirection(Direction, PlanetRadius);
			if (W > 0.0f)
			{
				A = FMath::Lerp(A, Zone.TargetHeight, W);
			}
		}
		return A;
	}
};
