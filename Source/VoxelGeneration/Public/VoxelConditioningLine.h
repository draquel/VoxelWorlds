// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelConditioningLine.generated.h"

struct FVoxelConditioningLine;

/** A reference to a shared, immutable conditioning line. */
using FVoxelConditioningLineRef = TSharedRef<const FVoxelConditioningLine>;

/** How a conditioning line moves the terrain toward its target. */
UENUM(BlueprintType)
enum class EVoxelConditioningLineBlend : uint8
{
	/** Toward the target: cut where the ground is higher, fill where it is lower (a road) */
	Level,

	/** Toward the lower of the target and the ground: a cutting, a river bed. Ground under the target is left alone */
	LowerOnly,

	/** Toward the higher of the target and the ground: an embankment. Ground over the target is left alone */
	RaiseOnly
};

/** One vertex of a conditioning line. */
USTRUCT(BlueprintType)
struct VOXELGENERATION_API FVoxelConditioningLineVertex
{
	GENERATED_BODY()

	/**
	 * Where the vertex is: world X and Y on a heightmap world (Z is not read), the direction from the planet centre on a
	 * planet (normalized when the line is made).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning")
	FVector Position = FVector::ZeroVector;

	/** The height the terrain is blended toward here: a world Z, or an altitude above the datum on a planet */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning")
	float TargetHeight = 0.0f;

	/** Half the width of the fully blended core here (world units, along the surface on a planet) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning", meta = (ClampMin = "0.0"))
	float HalfWidth = 225.0f;

	FVoxelConditioningLineVertex() = default;

	FVoxelConditioningLineVertex(const FVector& InPosition, float InTargetHeight, float InHalfWidth)
		: Position(InPosition), TargetHeight(InTargetHeight), HalfWidth(InHalfWidth)
	{
	}
};

/**
 * The ground along a conditioning line (Documentation/TERRAIN_LINES_PLAN.md, section 4.3): a band of material, a minimum
 * depth for caves under it and a width that is kept clear of scatter, PCG points and voxel trees. Each part is off at
 * its default, so a line with a default surface only moves the height.
 */
USTRUCT(BlueprintType)
struct VOXELGENERATION_API FVoxelConditioningLineSurface
{
	GENERATED_BODY()

	/** The band's material: an index into the world's material table, or -1 for no band */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning", meta = (ClampMin = "-1", ClampMax = "255"))
	int32 BandMaterialID = -1;

	/**
	 * Half the band's width (world units, along the surface on a planet). The band is never wider than the core: it
	 * reaches to the lesser of this and the line's half width at the place. 0: the core's width.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning", meta = (ClampMin = "0.0"))
	float BandHalfWidth = 0.0f;

	/**
	 * How deep under the generated surface the band's material goes, in voxels. Four or more: Dual Contouring at stride 4
	 * reads a quad's material from the solid voxel on its lattice, up to three voxels under the surface.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning", meta = (ClampMin = "0.0"))
	float BandDepth = 4.0f;

	/**
	 * No cave opens within this many voxels under the surface on the line's core; across the falloff the depth eases back
	 * to the caves' own. Applied as the cave configuration's underwater minimum depth is, and never makes a cave
	 * shallower. 0: caves as elsewhere.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning", meta = (ClampMin = "0.0"))
	float MinCaveDepth = 0.0f;

	/**
	 * Half the width (world units, along the surface on a planet) inside which no scatter instance, PCG point or voxel
	 * tree is placed. It may be wider than the core (a verge): the line's reach grows to hold it. 0: nothing is cleared.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning", meta = (ClampMin = "0.0"))
	float ClearHalfWidth = 0.0f;

	/** @return Whether the line paints a band */
	bool HasBand() const { return BandMaterialID >= 0 && BandMaterialID <= 255; }

	/** @return Whether any part of the surface is on */
	bool IsSet() const { return HasBand() || MinCaveDepth > 0.0f || ClearHalfWidth > 0.0f; }
};

/** One line at a place (FVoxelConditioningLine::Sample, SampleOnSphere): what its nearest segment gives there. */
struct FVoxelConditioningLineSample
{
	/** The target height (a world Z, or an altitude on a planet) */
	float Target = 0.0f;

	/** The weight the height is blended by: the strength times Profile */
	float Weight = 0.0f;

	/** The weight's profile for a strength of 1: 1 on the core, a smoothstep to 0 across the falloff */
	float Profile = 0.0f;

	/** The distance to the line (world units; the arc on a planet) */
	float Distance = 0.0f;

	/** The core's half width at the nearest place (not less than the least half width the sample was asked with) */
	float HalfWidth = 0.0f;

	/** Whether the place lies on the line's band */
	bool bOnBand = false;
};

/**
 * What a conditioning line is made from (FVoxelConditioningLine::Make, UVoxelChunkManager::AddConditioningLine): its
 * vertices and the values of the whole line.
 */
USTRUCT(BlueprintType)
struct VOXELGENERATION_API FVoxelConditioningLineParams
{
	GENERATED_BODY()

	/** The polyline, in order. Two or more; a vertex every 2 voxels on a line that bends (the bend bound) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning")
	TArray<FVoxelConditioningLineVertex> Vertices;

	/** Width (world units) of the falloff outside the core, easing back to the natural terrain */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning", meta = (ClampMin = "0.0"))
	float FalloffWidth = 300.0f;

	/** Overall strength [0, 1]: 1 reaches the target on the core, 0 leaves the height alone (a line that only paints) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Strength = 1.0f;

	/** How the terrain moves toward the target */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning")
	EVoxelConditioningLineBlend Blend = EVoxelConditioningLineBlend::Level;

	/**
	 * The line's place in the order lines are applied in (ascending), stable for the same world: every machine and every
	 * reader then blends two lines that meet in the same order.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning")
	int64 Id = 0;

	/** A planet line: the vertices' positions are directions from the planet centre and the targets altitudes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning")
	bool bRadial = false;

	/** The ground along the line: a band of material, the caves' minimum depth under it, the width kept clear */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conditioning")
	FVoxelConditioningLineSurface Surface;
};

/**
 * What a line is checked against where it is made (FVoxelConditioningLine::GetWarnings; TERRAIN_LINES_PLAN.md section
 * 4.1): the world's values the limits are stated in.
 */
struct VOXELGENERATION_API FVoxelConditioningLineLimits
{
	/** The world's voxel size (the bend's step, the widths and a planet segment's sag are bounded in voxels) */
	float VoxelSize = 100.0f;

	/** A block world (cubic meshing on a heightmap): the target may change by one block between adjacent columns */
	bool bBlockWorld = false;

	/** The datum radius, for a planet line (0 on a heightmap world) */
	double PlanetRadius = 0.0;

	/**
	 * The cut and fill cap: how far a target may lie from the natural surface (world units; 0 skips the check). The LOD
	 * strategy's surface slab does not look at lines and assumes conditioning stays within 1.5 chunks of the natural
	 * surface, so the cap is well inside that.
	 */
	float MaxCutFill = 0.0f;

	/** The natural surface at a vertex's position: a world Z, or an altitude on a planet. Unset skips the cap's check */
	TFunction<float(const FVector& Position)> NaturalHeight;
};

/**
 * A conditioning line: a polyline that levels, lowers or raises the generated terrain along it, the second conditioning
 * shape beside the zone's disc (Documentation/TERRAIN_LINES_PLAN.md, section 4.1).
 *
 * Two kinds, as the zones: a heightmap line (vertices at world X, Y, targets a world Z) and a planet line (vertices as
 * directions on the datum sphere, targets altitudes, widths measured along the surface). Each kind is ignored by the
 * other's functions.
 *
 * At a point, the nearest segment alone answers: the target height and the half width are interpolated between its two
 * vertices at the nearest place on it, and the weight is the zone's profile over the distance (the strength inside the
 * half width, a smoothstep to zero across the falloff). One blend per line, never one per segment. On a planet a segment
 * is the chord between its two vertices' points on the datum sphere, and the distance is the arc of the chord from the
 * point to the segment, as a planet zone measures it.
 *
 * A line is a shared, immutable object, made once (Make): a gather appends references (FVoxelConditioningLineRef), never
 * copies, and a reference is safe to carry on a generation request and to read on a worker. The segments are grouped in
 * blocks with bounds, so a point far from a long line costs a test per block.
 *
 * A line also carries the ground along it (FVoxelConditioningLineSurface): a band of material the generators write and
 * the analytic readers name, a minimum depth for caves under it, and a width kept clear of scatter, PCG points and voxel
 * trees (FVoxelConditioning's GetBandAt, GetMinCaveDepthAt, IsClearedAt and their planet twins).
 */
struct VOXELGENERATION_API FVoxelConditioningLine
{
	/** Segments in a block of the line's bounds hierarchy */
	static constexpr int32 BlockSegments = 16;

	/** Floats per segment in a chunk's segment list (FVoxelChunkLineSegments), in float4s */
	static constexpr int32 SegmentStride = 5;

	/**
	 * What a reader that samples every S units takes as a line's least half width, over S: a band 1.42 S wide is sampled
	 * by a lattice of spacing S whatever its direction (the plan's section 4.4), so a road draws unbroken where a point
	 * sample would give dashes (FVoxelConditioning::LeastHalfWidth).
	 */
	static constexpr double CoarseHalfWidthPerSpacing = 0.71;

	/**
	 * A reader whose region is wider than this many chunks takes no lines: at that scale a road is far under one of its
	 * samples, and its gather would ask a conditioner for every line over many placement cells. The far field's patches
	 * (FVoxelFarFieldSettings::ZoneGatherMax, the same width), a map tile above the level this wide, and the PCG sampler
	 * over bounds this wide (which then takes only the lines at hand).
	 */
	static constexpr int32 GatherLimitChunks = 64;

	/**
	 * Make a line. Consecutive vertices at the same place are merged, a planet line's directions are normalized, the
	 * strength is clamped to [0, 1] and negative widths to 0. A line with fewer than two distinct vertices has no
	 * segment and conditions nothing. The limits of the plan's section 4.1 are not enforced here: GetWarnings names the
	 * ones a line breaks.
	 *
	 * @param Params The vertices and the line's values
	 * @return The line
	 */
	static FVoxelConditioningLineRef Make(const FVoxelConditioningLineParams& Params);

	/** @return The line's vertices (a planet line's positions are unit directions) */
	const TArray<FVoxelConditioningLineVertex>& GetVertices() const { return Vertices; }

	/** @return How many segments the line has (the vertices less one, 0 for an empty line) */
	int32 NumSegments() const { return FMath::Max(Vertices.Num() - 1, 0); }

	/** @return The falloff's width outside the core */
	float GetFalloffWidth() const { return FalloffWidth; }

	/** @return The strength [0, 1] */
	float GetStrength() const { return Strength; }

	/** @return The blend */
	EVoxelConditioningLineBlend GetBlend() const { return Blend; }

	/** @return The id lines are ordered by */
	int64 GetId() const { return Id; }

	/** @return Whether this is a planet line */
	bool IsRadial() const { return bRadial; }

	/** @return The ground along the line (the band's half width as given: 0 is the core's) */
	const FVoxelConditioningLineSurface& GetSurface() const { return Surface; }

	/**
	 * @return How far from its segments the line changes anything: the largest half width plus the falloff, or the clear
	 *         half width where that is wider (world units, along the surface on a planet). Every bounds test grows by it,
	 *         so the nearest segment of a point the line reaches is never culled.
	 */
	double GetReach() const { return Reach; }

	/** @return How far from its segments the line moves the height: the largest half width plus the falloff */
	double GetHeightReach() const { return HeightReach; }

	/**
	 * @return The band's half width as the generators read it: the surface's, or a value no core exceeds where the
	 *         surface gives 0 (the core's width); negative when the line has no band
	 */
	float GetBandHalfWidthLimit() const { return BandHalfWidthLimit; }

	/** @return A heightmap line's XY bounds, grown by its reach (invalid for a planet line or an empty one) */
	FBox2D GetBounds2D() const;

	/**
	 * A planet line's bounds as a cap on the sphere, grown by its reach.
	 * @param PlanetRadius The datum radius
	 * @param OutDirection The cap's centre direction
	 * @param OutAngle The cap's half-angle (radians)
	 * @return False for a heightmap line or an empty one
	 */
	bool GetCap(double PlanetRadius, FVector& OutDirection, double& OutAngle) const;

	/**
	 * Whether a heightmap line's reach meets a world XY region: some block of its segments, grown by the reach, overlaps
	 * it. Conservative (a block's box, not each segment).
	 * @param Region The region (a point region is a point query)
	 * @return False for a planet line
	 */
	bool MeetsRegion(const FBox2D& Region) const;

	/**
	 * Whether a planet line's reach meets a cap on the sphere. Conservative, as MeetsRegion.
	 * @param CapDirection Unit direction of the cap's centre
	 * @param CapAngle The cap's half-angle (radians; pi or more covers the sphere)
	 * @param PlanetRadius The datum radius
	 * @return False for a heightmap line
	 */
	bool MeetsCap(const FVector& CapDirection, double CapAngle, double PlanetRadius) const;

	/**
	 * A heightmap line at (X, Y): the target and the weight from the nearest segment.
	 * @param X World X
	 * @param Y World Y
	 * @param OutTarget The target height there
	 * @param OutWeight The weight there
	 * @return Whether the weight is over zero (false for a planet line)
	 */
	bool Evaluate(double X, double Y, float& OutTarget, float& OutWeight) const;

	/**
	 * A planet line along a direction: the target altitude and the weight from the nearest segment.
	 * @param Direction Unit direction from the planet centre
	 * @param PlanetRadius The datum radius
	 * @param OutTarget The target altitude there
	 * @param OutWeight The weight there
	 * @return Whether the weight is over zero (false for a heightmap line)
	 */
	bool EvaluateOnSphere(const FVector& Direction, double PlanetRadius, float& OutTarget, float& OutWeight) const;

	/**
	 * A heightmap line at (X, Y): everything its nearest segment gives there.
	 * @param X World X
	 * @param Y World Y
	 * @param LeastHalfWidth The least half width the line is taken to have, for the height and for the band (a reader
	 *                       that samples coarsely: FVoxelConditioning::LeastHalfWidth); 0 for the line as it is
	 * @param Out The sample
	 * @return Whether the place is within the line's reach (false for a planet line)
	 */
	bool Sample(double X, double Y, double LeastHalfWidth, FVoxelConditioningLineSample& Out) const;

	/**
	 * A planet line along a direction, as Sample.
	 * @param Direction Unit direction from the planet centre
	 * @param PlanetRadius The datum radius
	 * @param LeastHalfWidth As Sample's
	 * @param Out The sample
	 * @return Whether the place is within the line's reach (false for a heightmap line)
	 */
	bool SampleOnSphere(const FVector& Direction, double PlanetRadius, double LeastHalfWidth, FVoxelConditioningLineSample& Out) const;

	/**
	 * Whether a distance from the line lies on its band: within the lesser of the band's half width and the core's
	 * there. In float, as the generators test it (FVoxelChunkLineSegments, the shader's GpuBlendConditioningLine).
	 * @param Dist The distance to the line
	 * @param HalfWidth The core's half width at the nearest place
	 * @param InBandHalfWidth The band's half width as a chunk's segment list carries it (GetBandHalfWidthLimit)
	 */
	static bool IsOnBand(float Dist, float HalfWidth, float InBandHalfWidth)
	{
		return Dist <= FMath::Min(InBandHalfWidth, HalfWidth);
	}

	/**
	 * One line's blend of a height: toward the target (Level), toward the lower of the target and the height (LowerOnly)
	 * or toward the higher (RaiseOnly), by the weight. The generators' segment loop (FVoxelChunkLineSegments, the
	 * shader's GpuBlendConditioningLine) is this arithmetic in the same order.
	 * @param InBlend The line's blend
	 * @param Height The height so far
	 * @param Target The line's target at the place
	 * @param Weight The line's weight at the place
	 * @return The blended height
	 */
	static float BlendHeight(EVoxelConditioningLineBlend InBlend, float Height, float Target, float Weight)
	{
		float Toward = Target;
		if (InBlend == EVoxelConditioningLineBlend::LowerOnly)
		{
			Toward = FMath::Min(Height, Target);
		}
		else if (InBlend == EVoxelConditioningLineBlend::RaiseOnly)
		{
			Toward = FMath::Max(Height, Target);
		}
		return Height + Weight * (Toward - Height);
	}

	/**
	 * The weight at a distance from a line: the zone's profile (the strength inside the half width, a smoothstep to zero
	 * across the falloff, zero beyond). In float, as the generators compute it.
	 */
	static float WeightAtDistance(float Dist, float HalfWidth, float InFalloffWidth, float InStrength)
	{
		if (Dist <= HalfWidth)
		{
			return InStrength;
		}
		const float Falloff = FMath::Max(1.0f, InFalloffWidth);
		if (Dist >= HalfWidth + Falloff)
		{
			return 0.0f;
		}
		const float T = (Dist - HalfWidth) / Falloff;
		const float S = 1.0f - (T * T * (3.0f - 2.0f * T));
		return InStrength * S;
	}

	/**
	 * The largest of distance x weight over the falloff, for a strength of 1: the distance at which a change of target
	 * across a bend's bisector moves the terrain most (the bend bound).
	 * @param HalfWidth The core's half width
	 * @param InFalloffWidth The falloff's width
	 * @return The peak (world units)
	 */
	static float PeakOfDistanceTimesWeight(float HalfWidth, float InFalloffWidth);

	/**
	 * The limits of the plan's section 4.1 this line breaks, one sentence each, for the place a line is made to log (as
	 * UVoxelWorldConfiguration::GetSettingsWarnings): fewer than two vertices; the cut and fill cap; a levelled core or
	 * a falloff under two voxels; the bend bound (grade x turn x the peak of distance x weight over a tenth of a voxel,
	 * or a turn whose bisector meets its neighbour's inside the reach); a part of the line within twice its reach of
	 * another part; a planet segment whose chord sags more than a tenth of a voxel; on a block world a target steeper
	 * than one block a column; a band under four voxels deep or under two voxels wide.
	 *
	 * @param Limits The world's values
	 * @return The warnings (empty when the line keeps every limit)
	 */
	TArray<FString> GetWarnings(const FVoxelConditioningLineLimits& Limits) const;

	/**
	 * A heightmap chunk's segments of this line: every segment whose box, grown by the line's reach, meets the chunk's XY
	 * box, in order along the line, appended as SegmentStride float4s each, positions relative to the chunk's origin:
	 * [A.x, A.y, 0, target at A] [B.x, B.y, 0, target at B] [half width at A, at B, falloff, strength]
	 * [line index, blend, band material or -1, band depth] [band half width, minimum cave depth, 0, 0].
	 *
	 * @param ChunkBox The chunk's XY box
	 * @param Origin The chunk's origin corner (positions are stored less it)
	 * @param LineIndex The line's index in the chunk's list (segments of one line carry the same)
	 * @param Out Appended to
	 * @param OutBounds Grown by each appended segment's box, grown by the reach, relative to the origin
	 */
	void AppendChunkSegments(const FBox2D& ChunkBox, const FVector2D& Origin, int32 LineIndex, TArray<FVector4f>& Out, FBox3f& OutBounds) const;

	/**
	 * A planet chunk's segments of this line, as AppendChunkSegments: every segment that comes within the chunk's cap
	 * (as a chord radius around the anchor's datum point) plus the line's reach, positions as the vertices' points on
	 * the datum sphere less the anchor's (in double, stored in float): [A.xyz, target at A] [B.xyz, target at B] [...].
	 *
	 * @param AnchorDirection The chunk anchor's direction from the planet centre
	 * @param PlanetRadius The datum radius
	 * @param CapChord The chord radius, around the anchor's datum point, of the datum points under the chunk's voxels
	 *                 (negative: the chunk holds the planet centre, every segment is taken)
	 * @param LineIndex The line's index in the chunk's list
	 * @param Out Appended to
	 * @param OutBounds Grown by each appended segment's box, grown by the reach, relative to the anchor's datum point
	 */
	void AppendChunkSegmentsOnSphere(const FVector& AnchorDirection, double PlanetRadius, double CapChord, int32 LineIndex,
		TArray<FVector4f>& Out, FBox3f& OutBounds) const;

private:
	/**
	 * The nearest segment of a heightmap line to (X, Y), among the blocks within SearchReach of it. False when none is
	 * within SearchReach (the line's reach; more for a reader that widens the line).
	 */
	bool FindNearest(double X, double Y, double SearchReach, int32& OutSegment, double& OutT, double& OutDistSq) const;

	/** The nearest segment of a planet line to a point on the datum sphere, as FindNearest. */
	bool FindNearestOnSphere(const FVector& Point, double PlanetRadius, double SearchReach, int32& OutSegment, double& OutT, double& OutDistSq) const;

	/** A sample from a segment FindNearest or FindNearestOnSphere found, at a distance (the arc on a planet). */
	void FillSample(int32 Segment, double T, double Distance, double LeastHalfWidth, FVoxelConditioningLineSample& Out) const;

	/** The float4s of one segment (SegmentStride of them), positions given relative to a chunk's frame. */
	void AppendSegment(int32 Segment, const FVector3f& A, const FVector3f& B, int32 LineIndex, TArray<FVector4f>& Out) const;

	/** Build Reach, the bounds and the blocks from the vertices. */
	void BuildBounds();

	TArray<FVoxelConditioningLineVertex> Vertices;
	float FalloffWidth = 0.0f;
	float Strength = 1.0f;
	EVoxelConditioningLineBlend Blend = EVoxelConditioningLineBlend::Level;
	int64 Id = 0;
	bool bRadial = false;
	FVoxelConditioningLineSurface Surface;

	/** The band's half width as the segment list carries it (GetBandHalfWidthLimit) */
	float BandHalfWidthLimit = -1.0f;

	/** The largest half width plus the falloff (at least 1, as the weight's profile) */
	double HeightReach = 0.0;

	/** HeightReach, or the clear half width where that is wider */
	double Reach = 0.0;

	/** A heightmap line: the vertices' XY box (not grown) and one per block of BlockSegments segments */
	FBox2D Bounds = FBox2D(ForceInit);
	TArray<FBox2D> BlockBounds;

	/**
	 * A planet line: a ball around the vertices on the unit sphere (a centre direction and the largest chord from it to
	 * a vertex), and one per block. Chords, not angles: a point's chord to the centre is at most the ball's radius plus
	 * its chord to the nearest segment, which lies in the vertices' hull.
	 */
	FVector CapCentre = FVector::UpVector;
	double CapChordRadius = 0.0;
	TArray<FVector4d> BlockBalls;
};
