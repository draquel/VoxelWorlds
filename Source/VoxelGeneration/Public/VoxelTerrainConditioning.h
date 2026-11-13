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
 * A conditioning line: a polyline that levels, lowers or raises the generated terrain and paints a band of material along
 * it, the second conditioning shape beside the zone's disc (Documentation/TERRAIN_LINES_PLAN.md, section 4.1).
 *
 * Declared here so the conditioning at a place (FVoxelConditioning) and the conditioner's line gathers can name it. It
 * has no vertices and no values yet, nothing makes one and nothing evaluates one: a world generates what it generates
 * without the type. A line is a shared, immutable object: a gather appends references (FVoxelConditioningLineRef), never
 * copies, and a reference is safe to carry on a generation request and to read on a worker.
 */
struct FVoxelConditioningLine
{
};

/** A reference to a shared, immutable conditioning line. */
using FVoxelConditioningLineRef = TSharedRef<const FVoxelConditioningLine>;

/**
 * What a gather of conditioning is for, which decides how a conditioner answers for its lines. Zones are always complete:
 * a conditioner has them at hand. A line may have to be computed first (a road's route), and a caller either waits for
 * it or does not.
 */
enum class EVoxelConditioningGather : uint8
{
	/**
	 * Every line that reaches the place, computed then and there if it is not at hand: a chunk's generation request (its
	 * voxels are generated once) and every reader that must agree with the generated voxels at a place (a surface query
	 * at a point, a chunk's scatter, the PCG sampler).
	 */
	Complete,

	/**
	 * The lines that are at hand now, without computing any: the readers whose regions are too wide to wait for (the far
	 * field, the map). The conditioner works out the rest later and announces them through the conditioning-changed
	 * path (UVoxelChunkManager::OnTerrainConditioningChanged), and the reader draws again.
	 */
	Ready
};

/**
 * The conditioning at a place: every shape that reshapes the generated terrain there, the zones and the lines, as one
 * value. The gathers fill it for a region, a cap or a chunk (UVoxelChunkManager::GatherConditioning); a generation
 * request carries it; every reader of the generated surface takes it and asks it for the height on a heightmap world
 * (ApplyToHeight) or the altitude on a planet (ApplyToAltitude), so a reader never names a shape. Pure data: safe to copy
 * onto a request and to read on background threads (the lines are shared and immutable).
 */
USTRUCT()
struct VOXELGENERATION_API FVoxelConditioning
{
	GENERATED_BODY()

	/** The conditioning zones there, in the order they are applied (a later zone blends on top of the earlier ones). */
	UPROPERTY()
	TArray<FVoxelConditioningZone> Zones;

	/** The conditioning lines there. Gathered and carried; nothing evaluates a line yet (FVoxelConditioningLine). */
	TArray<FVoxelConditioningLineRef> Lines;

	/**
	 * The conditioning of a list of zones and no line.
	 * @param InZones The zones, in the order they are applied
	 * @return The conditioning
	 */
	static FVoxelConditioning OfZones(TArray<FVoxelConditioningZone> InZones)
	{
		FVoxelConditioning Conditioning;
		Conditioning.Zones = MoveTemp(InZones);
		return Conditioning;
	}

	/** @return Whether nothing conditions the terrain here: no zone and no line (the natural surface) */
	bool IsEmpty() const { return Zones.Num() == 0 && Lines.Num() == 0; }

	/** Empty the set, keeping its allocations. */
	void Reset()
	{
		Zones.Reset();
		Lines.Reset();
	}

	/**
	 * A heightmap world's generated height: BaseHeight blended toward each heightmap zone's TargetHeight by the zone's
	 * weight at (X, Y). Zones layer in order (a later zone blends on top of the earlier ones); planet zones are skipped.
	 * The heightmap generators (CPU and GPU) and every reader of the generated heightmap surface apply it to the natural
	 * height.
	 *
	 * @param X World X
	 * @param Y World Y
	 * @param BaseHeight The natural terrain's height at (X, Y)
	 * @return The conditioned height
	 */
	float ApplyToHeight(double X, double Y, float BaseHeight) const
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
	 * Planets (spherical SP-B10): the terrain's generated altitude along a direction: BaseAltitude blended toward each
	 * planet zone's TargetHeight (an altitude) by its weight there, layered in order like ApplyToHeight; heightmap zones
	 * are skipped. The planet generators (CPU and GPU) and every reader of the generated planet surface apply it to the
	 * radial displacement.
	 *
	 * @param Direction Unit direction from the planet centre
	 * @param PlanetRadius The datum radius (zone distances are measured on it)
	 * @param BaseAltitude The natural terrain's altitude along Direction
	 * @return The conditioned altitude
	 */
	float ApplyToAltitude(const FVector& Direction, double PlanetRadius, float BaseAltitude) const
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

/**
 * Boundary-safe interface for supplying terrain conditioning at generation time: the zones, and the lines.
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

	/**
	 * Append the conditioning lines whose reach meets the given world-space XY region. The default supplies none.
	 * @param Region    Chunk (or query) XY bounds
	 * @param Need      Complete: every such line, computed here if it is not at hand. Ready: only the lines at hand; the
	 *                  conditioner announces the rest through the conditioning-changed path when it has them
	 * @param OutLines  Append references to the lines here (a line is shared, never copied)
	 */
	virtual void GatherConditioningLines(const FBox2D& Region, EVoxelConditioningGather Need, TArray<FVoxelConditioningLineRef>& OutLines) const
	{
	}

	/**
	 * Planets: append the conditioning lines whose reach meets a cap on the sphere. The default supplies none.
	 * @param Direction     Unit direction from the planet centre to the cap's centre
	 * @param AngularRadius The cap's half-angle in radians (a chunk's, or a query's; pi or more is the whole sphere)
	 * @param Need          Complete or Ready, as GatherConditioningLines
	 * @param OutLines      Append references to the lines here
	 */
	virtual void GatherConditioningLinesOnSphere(const FVector& Direction, double AngularRadius, EVoxelConditioningGather Need,
		TArray<FVoxelConditioningLineRef>& OutLines) const
	{
	}
};
