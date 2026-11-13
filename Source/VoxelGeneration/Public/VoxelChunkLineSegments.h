// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelConditioningLine.h"

struct FVoxelNoiseGenerationRequest;

/**
 * What a chunk's conditioning lines do to each of its XY columns on a heightmap world: per column, the lines that have a
 * weight there with their target, weight and blend, in the order they are applied. The heightmap generator works it out
 * once per column before its voxel loop (FVoxelChunkLineSegments::EvaluateColumns), since the lines depend on (X, Y)
 * alone and the loop computes the height for every voxel.
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

	/** @return Whether no column has a blend (the chunk's lines reach none of its columns) */
	bool IsEmpty() const { return Blends.Num() == 0; }

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
 * [A.xyz, target at A] [B.xyz, target at B] [half width at A, half width at B, falloff, strength] [line index, blend, 0, 0].
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
	 * Walk the list at a point: for each line, the nearest of its segments, then Func(Blend, Target, Weight) if the
	 * weight there is over zero. The shader's loop, operation for operation, in float.
	 *
	 * @param Point The point in the chunk's frame: (X, Y, 0) from the origin corner on a heightmap world, the point on
	 *              the datum sphere less the anchor's on a planet
	 * @param Func Called once per line with a weight, in the order the lines are applied
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
	 * A height with the chunk's lines applied at a point (the planet generator's per-voxel form; the heightmap generator
	 * uses EvaluateColumns).
	 * @param Point The point in the chunk's frame, as ForEachLine
	 * @param Height The natural height or altitude there
	 * @return The height after the lines
	 */
	float Apply(const FVector3f& Point, float Height) const
	{
		ForEachLine(Point, [&Height](EVoxelConditioningLineBlend Blend, float Target, float Weight)
		{
			Height = FVoxelConditioningLine::BlendHeight(Blend, Height, Target, Weight);
		});
		return Height;
	}

	/**
	 * A heightmap chunk's columns: each column's blends, at the positions the generators give a column's voxels
	 * (X x VoxelSize, Y x VoxelSize from the origin corner, in float).
	 * @param ChunkSize Voxels along a chunk's edge
	 * @param VoxelSize The base voxel size
	 * @param Out Reset and filled
	 */
	void EvaluateColumns(int32 ChunkSize, float VoxelSize, FVoxelChunkLineColumns& Out) const;

private:
	/** One line's result at its nearest segment: the distance (an arc on a planet), the half width and target there, the weight. */
	template <typename FuncType>
	void FinishLine(int32 Segment, float T, float DistSq, FuncType&& Func) const
	{
		const FVector4f& S0 = Data[Segment * FVoxelConditioningLine::SegmentStride + 0];
		const FVector4f& S1 = Data[Segment * FVoxelConditioningLine::SegmentStride + 1];
		const FVector4f& S2 = Data[Segment * FVoxelConditioningLine::SegmentStride + 2];
		const FVector4f& S3 = Data[Segment * FVoxelConditioningLine::SegmentStride + 3];
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
		const float Weight = FVoxelConditioningLine::WeightAtDistance(Dist, HalfWidth, S2.Z, S2.W);
		if (Weight > 0.0f)
		{
			Func(static_cast<EVoxelConditioningLineBlend>(static_cast<uint8>(S3.Y)), Target, Weight);
		}
	}
};
