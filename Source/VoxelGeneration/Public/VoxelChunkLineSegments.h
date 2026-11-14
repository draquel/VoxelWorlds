// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelConditioningLine.h"

struct FVoxelNoiseGenerationRequest;

/**
 * What a chunk's conditioning lines make of the ground at a point (the plan's section 4.3): the band there and the
 * shallowest depth a cave may open at. The generators' form of FVoxelConditioning's GetBandAt and GetMinCaveDepthAt, in
 * float from the chunk's segment list; the shader carries the same three values (GpuApplyConditioningLines).
 */
struct FVoxelChunkLineSurface
{
	/** The band's material, or -1 where no band lies (where two lines' bands overlap, the later line's) */
	int32 BandMaterial = -1;

	/** How deep under the generated surface the band goes, in voxels */
	float BandDepth = 0.0f;

	/** The shallowest depth a cave may open at, in voxels: 0 leaves the caves their own */
	float MinCaveDepth = 0.0f;

	/** @return Whether anything is set: a band or a minimum cave depth */
	bool IsSet() const { return BandMaterial >= 0 || MinCaveDepth > 0.0f; }

	/**
	 * Whether a voxel at a depth under the generated surface carries the band's material. Air over the band does too (a
	 * negative depth), as it carries the surface's material elsewhere.
	 * @param DepthBelowSurface The voxel's depth under the generated surface, in voxels
	 */
	bool HasBandAt(float DepthBelowSurface) const { return BandMaterial >= 0 && DepthBelowSurface < BandDepth; }
};

/** One line's result at a point of a chunk (FVoxelChunkLineSegments::ForEachLine), from its nearest segment. */
struct FVoxelChunkLineSample
{
	/** The line's blend */
	EVoxelConditioningLineBlend Blend = EVoxelConditioningLineBlend::Level;

	/** The target height (or altitude) there */
	float Target = 0.0f;

	/** The weight the height is blended by: the strength times Profile (0 for a line that only paints) */
	float Weight = 0.0f;

	/** The weight's profile for a strength of 1: 1 on the core, a smoothstep to 0 across the falloff. Over 0 */
	float Profile = 0.0f;

	/** The line's band material where the point lies on its band, else -1 */
	int32 BandMaterial = -1;

	/** The line's band depth, in voxels */
	float BandDepth = 0.0f;

	/** The line's minimum cave depth times Profile, in voxels */
	float MinCaveDepth = 0.0f;

	/**
	 * Apply this line to what the lines before it left: the height's blend, the band (a later line's replaces an
	 * earlier one's), the deepest minimum cave depth. The shader's GpuBlendConditioningLine in the same order.
	 * @param InOutHeight The height so far
	 * @param InOutSurface The surface so far
	 */
	void ApplyTo(float& InOutHeight, FVoxelChunkLineSurface& InOutSurface) const
	{
		if (Weight > 0.0f)
		{
			InOutHeight = FVoxelConditioningLine::BlendHeight(Blend, InOutHeight, Target, Weight);
		}
		if (BandMaterial >= 0)
		{
			InOutSurface.BandMaterial = BandMaterial;
			InOutSurface.BandDepth = BandDepth;
		}
		InOutSurface.MinCaveDepth = FMath::Max(InOutSurface.MinCaveDepth, MinCaveDepth);
	}
};

/**
 * What a chunk's conditioning lines do to each of its XY columns on a heightmap world: per column, the lines that have a
 * weight there with their target, weight and blend, in the order they are applied, and the surface the lines leave there
 * (the band, the caves' minimum depth). The heightmap generator works it out once per column before its voxel loop
 * (FVoxelChunkLineSegments::EvaluateColumns), since the lines depend on (X, Y) alone and the loop computes the height
 * for every voxel.
 */
struct VOXELGENERATION_API FVoxelChunkLineColumns
{
	/** One line's blend at a column */
	struct FColumnBlend
	{
		float Target = 0.0f;
		float Weight = 0.0f;
		EVoxelConditioningLineBlend Blend = EVoxelConditioningLineBlend::Level;
	};

	/** Per column (X + Y x ChunkSize) the index of its first blend in Blends, and one past the last column's end */
	TArray<int32> First;

	/** The blends, column after column */
	TArray<FColumnBlend> Blends;

	/** Per column (X + Y x ChunkSize) the surface the lines leave there; empty when no column has one */
	TArray<FVoxelChunkLineSurface> Surfaces;

	/** @return Whether no column has a blend (the chunk's lines move the height of none of its columns) */
	bool IsEmpty() const { return Blends.Num() == 0; }

	/** @return Whether some column has a band or a minimum cave depth */
	bool HasSurfaces() const { return Surfaces.Num() > 0; }

	/**
	 * A column's height with the lines applied.
	 * @param Column The column's index, X + Y x ChunkSize
	 * @param Height The natural height there
	 * @return The height after the lines
	 */
	float Apply(int32 Column, float Height) const
	{
		for (int32 Index = First[Column]; Index < First[Column + 1]; ++Index)
		{
			const FColumnBlend& Blend = Blends[Index];
			Height = FVoxelConditioningLine::BlendHeight(Blend.Blend, Height, Blend.Target, Blend.Weight);
		}
		return Height;
	}
};

/**
 * A chunk's segment list: the segments of the conditioning lines whose reach meets the chunk, in the chunk's frame, in
 * floats, in a fixed order (the lines in the order they are applied, each line's segments in order along it). One
 * function builds it (Build) and both generators read it: the CPU generator evaluates it (Apply, EvaluateColumns) and
 * the GPU uploads the same float4s and walks them in one flat loop (GpuApplyConditioningLines, ConditioningLines.ush),
 * so both sides read the same numbers. Any change to the layout or to the loop is mirrored there.
 *
 * The frame: positions relative to the chunk's origin corner on a heightmap world (X, Y, 0), and on a planet the
 * vertices' points on the datum sphere relative to the chunk anchor's (the planet shaders' frame, PlanetFrame.ush), so
 * no float is of the size of a world position or of the planet's radius.
 *
 * FVoxelConditioningLine::SegmentStride float4s a segment:
 * [A.xyz, target at A] [B.xyz, target at B] [half width at A, half width at B, falloff, strength]
 * [line index, blend, band material or -1, band depth in voxels] [band half width, minimum cave depth in voxels, 0, 0].
 * The last two rows carry the line's surface (FVoxelConditioningLineSurface), the same on each of its segments.
 */
struct VOXELGENERATION_API FVoxelChunkLineSegments
{
	/** The segments, SegmentStride float4s each: what the GPU uploads as ConditioningLineData */
	TArray<FVector4f> Data;

	/** The box of the segments, each grown by its line's reach, in the chunk's frame: outside it no line has a weight */
	FBox3f Bounds = FBox3f(ForceInit);

	/**
	 * The datum radius on a planet, 0 on a heightmap world: on a planet the distance to a segment is the arc under the
	 * chord (as a planet zone's), on a heightmap world the distance in the plane.
	 */
	float ArcRadius = 0.0f;

	/** @return How many segments the list holds */
	int32 Num() const { return Data.Num() / FVoxelConditioningLine::SegmentStride; }

	/**
	 * Build a chunk's segment list from its request's conditioning (the lines of the request's world kind; the other
	 * kind's are skipped, as the zones'). A request with no line gives an empty list at the cost of one count test.
	 *
	 * @param Request The chunk's generation request
	 * @param Out Reset and filled
	 */
	static void Build(const FVoxelNoiseGenerationRequest& Request, FVoxelChunkLineSegments& Out);

	/** @return Whether a point of the chunk's frame may have a weight from some line (it lies in Bounds) */
	bool MayReach(const FVector3f& Point) const
	{
		return Point.X >= Bounds.Min.X && Point.X <= Bounds.Max.X && Point.Y >= Bounds.Min.Y && Point.Y <= Bounds.Max.Y
			&& Point.Z >= Bounds.Min.Z && Point.Z <= Bounds.Max.Z;
	}

	/**
	 * Walk the list at a point: for each line, the nearest of its segments, then Func(const FVoxelChunkLineSample&) if
	 * the point lies within the line's core or falloff (a profile over zero; a line that only paints has a weight of
	 * zero there and still a band). The shader's loop, operation for operation, in float.
	 *
	 * @param Point The point in the chunk's frame: (X, Y, 0) from the origin corner on a heightmap world, the point on
	 *              the datum sphere less the anchor's on a planet
	 * @param Func Called once per line that reaches the point, in the order the lines are applied
	 */
	template <typename FuncType>
	void ForEachLine(const FVector3f& Point, FuncType&& Func) const
	{
		const int32 Count = Num();
		int32 CurrentLine = -1;
		int32 BestSegment = 0;
		float BestT = 0.0f;
		float BestDistSq = TNumericLimits<float>::Max();
		for (int32 Segment = 0; Segment < Count; ++Segment)
		{
			const FVector4f& S0 = Data[Segment * FVoxelConditioningLine::SegmentStride + 0];
			const FVector4f& S1 = Data[Segment * FVoxelConditioningLine::SegmentStride + 1];
			const int32 Line = static_cast<int32>(Data[Segment * FVoxelConditioningLine::SegmentStride + 3].X);
			if (Line != CurrentLine)
			{
				// A new line begins: the one before it blends, from its nearest segment.
				if (CurrentLine >= 0)
				{
					FinishLine(BestSegment, BestT, BestDistSq, Func);
				}
				CurrentLine = Line;
				BestDistSq = TNumericLimits<float>::Max();
			}
			const FVector3f A(S0.X, S0.Y, S0.Z);
			const FVector3f AB = FVector3f(S1.X, S1.Y, S1.Z) - A;
			const FVector3f AP = Point - A;
			const float LengthSq = FVector3f::DotProduct(AB, AB);
			const float T = LengthSq > 1.0e-6f ? FMath::Clamp(FVector3f::DotProduct(AP, AB) / LengthSq, 0.0f, 1.0f) : 0.0f;
			const FVector3f D = AP - AB * T;
			const float DistSq = FVector3f::DotProduct(D, D);
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				BestT = T;
				BestSegment = Segment;
			}
		}
		if (CurrentLine >= 0)
		{
			FinishLine(BestSegment, BestT, BestDistSq, Func);
		}
	}

	/**
	 * A height with the chunk's lines applied at a point, and the surface they leave there (the planet generator's
	 * per-voxel form; the heightmap generator uses EvaluateColumns).
	 * @param Point The point in the chunk's frame, as ForEachLine
	 * @param Height The natural height or altitude there
	 * @param OutSurface The band and the caves' minimum depth there (reset first)
	 * @return The height after the lines
	 */
	float Apply(const FVector3f& Point, float Height, FVoxelChunkLineSurface& OutSurface) const
	{
		OutSurface = FVoxelChunkLineSurface();
		ForEachLine(Point, [&Height, &OutSurface](const FVoxelChunkLineSample& Sample)
		{
			Sample.ApplyTo(Height, OutSurface);
		});
		return Height;
	}

	/**
	 * A height with the chunk's lines applied at a point, for a caller that does not need the surface.
	 * @param Point The point in the chunk's frame, as ForEachLine
	 * @param Height The natural height or altitude there
	 * @return The height after the lines
	 */
	float Apply(const FVector3f& Point, float Height) const
	{
		FVoxelChunkLineSurface Surface;
		return Apply(Point, Height, Surface);
	}

	/**
	 * A heightmap chunk's columns: each column's blends and surface, at the positions the generators give a column's voxels
	 * (X x VoxelSize, Y x VoxelSize from the origin corner, in float).
	 * @param ChunkSize Voxels along a chunk's edge
	 * @param VoxelSize The base voxel size
	 * @param Out Reset and filled
	 */
	void EvaluateColumns(int32 ChunkSize, float VoxelSize, FVoxelChunkLineColumns& Out) const;

private:
	/**
	 * One line's result at its nearest segment: the distance (an arc on a planet), the half width and target there, the
	 * weight and its profile, the band and the minimum cave depth.
	 */
	template <typename FuncType>
	void FinishLine(int32 Segment, float T, float DistSq, FuncType&& Func) const
	{
		const FVector4f& S0 = Data[Segment * FVoxelConditioningLine::SegmentStride + 0];
		const FVector4f& S1 = Data[Segment * FVoxelConditioningLine::SegmentStride + 1];
		const FVector4f& S2 = Data[Segment * FVoxelConditioningLine::SegmentStride + 2];
		const FVector4f& S3 = Data[Segment * FVoxelConditioningLine::SegmentStride + 3];
		const FVector4f& S4 = Data[Segment * FVoxelConditioningLine::SegmentStride + 4];
		float Dist = FMath::Sqrt(DistSq);
		if (ArcRadius > 0.0f)
		{
			// The arc under the chord, 2 R asin(chord / 2R), asin by its series below 0.1 as the planet zones' shader
			// takes it (PlanetApplyConditioning: FXC's asin is a polynomial with uu-scale error at 2R).
			const float X = FMath::Min(1.0f, Dist / (2.0f * ArcRadius));
			const float X2 = X * X;
			const float AsinX = (X < 0.1f) ? X * (1.0f + X2 * (1.0f / 6.0f + X2 * (3.0f / 40.0f + X2 * (5.0f / 112.0f)))) : FMath::Asin(X);
			Dist = 2.0f * ArcRadius * AsinX;
		}
		const float HalfWidth = S2.X + T * (S2.Y - S2.X);
		const float Target = S0.W + T * (S1.W - S0.W);
		// The profile is the weight for a strength of 1: over zero on the core and across the falloff whatever the
		// strength, so a line that only paints (strength 0) is still handed on for its band.
		const float Profile = FVoxelConditioningLine::WeightAtDistance(Dist, HalfWidth, S2.Z, 1.0f);
		if (Profile > 0.0f)
		{
			FVoxelChunkLineSample Sample;
			Sample.Blend = static_cast<EVoxelConditioningLineBlend>(static_cast<uint8>(S3.Y));
			Sample.Target = Target;
			Sample.Weight = FVoxelConditioningLine::WeightAtDistance(Dist, HalfWidth, S2.Z, S2.W);
			Sample.Profile = Profile;
			Sample.BandMaterial = FVoxelConditioningLine::IsOnBand(Dist, HalfWidth, S4.X) ? static_cast<int32>(S3.Z) : -1;
			Sample.BandDepth = S3.W;
			Sample.MinCaveDepth = S4.Y * Profile;
			Func(Sample);
		}
	}
};
