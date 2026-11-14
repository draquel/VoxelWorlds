// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelConditioningLine.h"
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
 * Where the terrain conditioning changed (UVoxelChunkManager::OnTerrainConditioningChangedIn): a world XY box on a
 * heightmap world, a cap on a planet, or everywhere (a conditioner was set). A reader that keeps something per place (a
 * map tile's image) draws again only where the region reaches; a line worked out late by a conditioner is announced with
 * its bounds, and many arrive one by one.
 */
struct FVoxelConditioningRegion
{
	/** The conditioning may have changed anywhere: Box and the cap are not read */
	bool bEverywhere = false;

	/** A cap on a planet (Direction, AngularRadius), else a heightmap world's Box */
	bool bCap = false;

	/** The cap's centre: a unit direction from the planet centre */
	FVector Direction = FVector::UpVector;

	/** The cap's half-angle (radians) */
	double AngularRadius = 0.0;

	/** A heightmap world's region (world XY). Invalid: a change that reaches nothing in this kind of world */
	FBox2D Box = FBox2D(ForceInit);

	static FVoxelConditioningRegion Everywhere()
	{
		FVoxelConditioningRegion Region;
		Region.bEverywhere = true;
		return Region;
	}

	static FVoxelConditioningRegion OfBox(const FBox2D& InBox)
	{
		FVoxelConditioningRegion Region;
		Region.Box = InBox;
		return Region;
	}

	static FVoxelConditioningRegion OfCap(const FVector& InDirection, double InAngularRadius)
	{
		FVoxelConditioningRegion Region;
		Region.bCap = true;
		Region.Direction = InDirection.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
		Region.AngularRadius = InAngularRadius;
		return Region;
	}
};

/**
 * The conditioning at a place: every shape that reshapes the generated terrain there, the lines and the zones, as one
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

	/**
	 * The conditioning lines there, in the order they are applied: by id (SortLines; the gathers leave them so). A line is
	 * applied before every zone, so a POI's pad wins where a road arrives at it.
	 */
	TArray<FVoxelConditioningLineRef> Lines;

	/**
	 * The least half width every line here is taken to have, for the height and for the band (world units; 0: the lines
	 * as they are). A reader that samples the surface every S units sets it to 0.71 S
	 * (FVoxelConditioningLine::CoarseHalfWidthPerSpacing) on the set it gathered for itself: a far-field patch, a map
	 * tile above level 0. A line narrower than its samples would otherwise fall between them and draw as dashes; with it
	 * a road is an unbroken line on the map and an unbroken band in the far field. The voxels are generated once at full
	 * resolution and cannot do this, so a generation request, and every reader that must agree with the voxels at a
	 * place, leaves it 0. A gather for a set that has it grows its region by it.
	 */
	float LeastHalfWidth = 0.0f;

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
		LeastHalfWidth = 0.0f;
	}

	/**
	 * Put the lines in the order they are applied in, by id, and drop a line gathered twice. The chunk manager's gathers
	 * end with it; a set filled by hand from more than one line calls it.
	 */
	void SortLines();

	/**
	 * A heightmap world's generated height: BaseHeight blended toward each heightmap line's target (one blend a line,
	 * from its nearest segment; in id order), then toward each heightmap zone's TargetHeight by the zone's weight at
	 * (X, Y). Zones layer in order (a later zone blends on top of the earlier ones); planet lines and zones are skipped.
	 * Every reader of the generated heightmap surface applies it to the natural height; the generators (CPU and GPU)
	 * apply the same lines from the chunk's segment list (FVoxelChunkLineSegments) and then ApplyZonesToHeight.
	 *
	 * @param X World X
	 * @param Y World Y
	 * @param BaseHeight The natural terrain's height at (X, Y)
	 * @return The conditioned height
	 */
	float ApplyToHeight(double X, double Y, float BaseHeight) const
	{
		return ApplyZonesToHeight(X, Y, Lines.Num() > 0 ? ApplyLinesToHeight(X, Y, BaseHeight) : BaseHeight);
	}

	/**
	 * Planets (spherical SP-B10): the terrain's generated altitude along a direction: BaseAltitude blended toward each
	 * planet line's target altitude, then toward each planet zone's TargetHeight (an altitude) by its weight there,
	 * layered in order like ApplyToHeight; heightmap lines and zones are skipped. Every reader of the generated planet
	 * surface applies it to the radial displacement; the generators apply the same lines from the chunk's segment list
	 * and then ApplyZonesToAltitude.
	 *
	 * @param Direction Unit direction from the planet centre
	 * @param PlanetRadius The datum radius (distances are measured on it)
	 * @param BaseAltitude The natural terrain's altitude along Direction
	 * @return The conditioned altitude
	 */
	float ApplyToAltitude(const FVector& Direction, double PlanetRadius, float BaseAltitude) const
	{
		return ApplyZonesToAltitude(Direction, PlanetRadius, Lines.Num() > 0 ? ApplyLinesToAltitude(Direction, PlanetRadius, BaseAltitude) : BaseAltitude);
	}

	/**
	 * The lines' part of ApplyToHeight: each heightmap line's blend, in order, evaluated from the lines themselves in
	 * double (the generators evaluate the chunk's segment list instead, in the chunk's frame), each line at least
	 * LeastHalfWidth wide.
	 * @param X World X
	 * @param Y World Y
	 * @param BaseHeight The height before the lines
	 * @return The height after them
	 */
	float ApplyLinesToHeight(double X, double Y, float BaseHeight) const;

	/**
	 * The lines' part of ApplyToAltitude, as ApplyLinesToHeight.
	 * @param Direction Unit direction from the planet centre
	 * @param PlanetRadius The datum radius
	 * @param BaseAltitude The altitude before the lines
	 * @return The altitude after them
	 */
	float ApplyLinesToAltitude(const FVector& Direction, double PlanetRadius, float BaseAltitude) const;

	// ==================== The surface under the lines (TERRAIN_LINES_PLAN.md, section 4.3) ====================
	// What the lines here make of the ground along them, for the readers of the generated surface. The generators (CPU
	// and GPU) read the same from the chunk's segment list (FVoxelChunkLineSegments). A set with no line answers at the
	// cost of a count test.

	/** @return Whether some line here has a band, a minimum cave depth or a clearing: a reader skips the tests when none has */
	bool HasLineSurfaces() const;

	/**
	 * The band at a heightmap (X, Y): the material the generated surface is there in place of the biome's. Where the
	 * bands of two lines overlap the later line's wins, as in the voxels. Honours LeastHalfWidth.
	 * @param X World X
	 * @param Y World Y
	 * @param OutMaterial The band's material (untouched when there is none)
	 * @param OutDepth How deep under the generated surface the band goes, in voxels (optional; untouched when there is none)
	 * @return Whether a band lies there
	 */
	bool GetBandAt(double X, double Y, uint8& OutMaterial, float* OutDepth = nullptr) const;

	/**
	 * The band along a planet direction, as GetBandAt.
	 * @param Direction Unit direction from the planet centre
	 * @param PlanetRadius The datum radius
	 * @param OutMaterial The band's material (untouched when there is none)
	 * @param OutDepth How deep the band goes, in voxels (optional; untouched when there is none)
	 * @return Whether a band lies there
	 */
	bool GetBandOnSphere(const FVector& Direction, double PlanetRadius, uint8& OutMaterial, float* OutDepth = nullptr) const;

	/**
	 * The shallowest depth a cave may open at under the lines at a heightmap (X, Y), in voxels: the largest of each
	 * line's minimum cave depth times its profile there (the whole depth on its core, easing to 0 across its falloff).
	 * A caller takes the larger of this and the cave layer's own minimum (FVoxelCPUNoiseGenerator::CalculateCaveDensity).
	 * @return The depth, 0 where no line asks for one
	 */
	float GetMinCaveDepthAt(double X, double Y) const;

	/** GetMinCaveDepthAt along a planet direction. */
	float GetMinCaveDepthOnSphere(const FVector& Direction, double PlanetRadius) const;

	/**
	 * Whether a heightmap (X, Y) lies inside some line's clear width: no scatter instance, PCG point or voxel tree is
	 * placed there.
	 */
	bool IsClearedAt(double X, double Y) const;

	/** IsClearedAt along a planet direction. */
	bool IsClearedOnSphere(const FVector& Direction, double PlanetRadius) const;

	/**
	 * The zones' part of ApplyToHeight: the height blended toward each heightmap zone's target, in order.
	 * @param X World X
	 * @param Y World Y
	 * @param BaseHeight The height before the zones (the natural height with the lines applied)
	 * @return The height after them
	 */
	float ApplyZonesToHeight(double X, double Y, float BaseHeight) const
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
	 * The zones' part of ApplyToAltitude: the altitude blended toward each planet zone's target, in order.
	 * @param Direction Unit direction from the planet centre
	 * @param PlanetRadius The datum radius
	 * @param BaseAltitude The altitude before the zones (the natural altitude with the lines applied)
	 * @return The altitude after them
	 */
	float ApplyZonesToAltitude(const FVector& Direction, double PlanetRadius, float BaseAltitude) const
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
