// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FVoxelNoiseGenerationRequest;
struct FVoxelValleySegment;
class FVoxelValleyNetwork;

/**
 * The valleys' change to the natural height (Documentation/VALLEYS_PLAN.md, section 6.2), one float evaluation the CPU
 * generator, the shader (GpuApplyValleys, Valleys.ush) and the world modes' point query share, operation for operation.
 *
 * Each valley segment that reaches a point gives a weight there: 1 on the floor (within its half width of the centre
 * curve), a smoothstep down to 0 across the side to the valley's half width, scaled by the head's taper; and a floor:
 * the floor's height interpolated along the segment. Its change to the height is Weight x (Floor - Natural). The
 * segments combine as the deepest cut (the least change under 0) plus the highest fill (the greatest over 0): each is
 * continuous where segments overlap (a junction, two valleys side by side), so the height has no step. Ground under the
 * sea is left as it is: a valley that reaches the sea has reached it.
 *
 * A segment is ValleySegmentStride float4s: [A.xyz, floor at A] [B.xyz, floor at B] [floor half width, valley half width,
 * taper at A, taper at B], positions in the frame of the reader: on a heightmap world a chunk's origin corner for the
 * generators and a tile's corner for the point query, every z 0; on a planet (the plan's V5) points on the datum sphere
 * less the chunk anchor's datum point for the generators (the conditioning lines' frame) and less a tile's middle for
 * the point query. The distance is the chord, the same on the CPU and the GPU.
 */
namespace VoxelValleyCarve
{
	static constexpr int32 SegmentStride = 3;

	/**
	 * A segment's weight and floor at a point.
	 * @param P The point, in the segment's frame (z 0 on a heightmap world)
	 * @param R0, R1, R2 The segment's three rows
	 * @param OutWeight 0 outside the valley, 1 on its floor
	 * @param OutFloor The floor's height at the point's nearest place along the segment
	 */
	FORCEINLINE void SegmentAt(const FVector3f& P, const FVector4f& R0, const FVector4f& R1, const FVector4f& R2, float& OutWeight, float& OutFloor)
	{
		const float ABx = R1.X - R0.X;
		const float ABy = R1.Y - R0.Y;
		const float ABz = R1.Z - R0.Z;
		const float Length2 = ABx * ABx + ABy * ABy + ABz * ABz;
		const float T = Length2 > 0.0f ? FMath::Clamp(((P.X - R0.X) * ABx + (P.Y - R0.Y) * ABy + (P.Z - R0.Z) * ABz) / Length2, 0.0f, 1.0f) : 0.0f;
		const float Dx = P.X - (R0.X + ABx * T);
		const float Dy = P.Y - (R0.Y + ABy * T);
		const float Dz = P.Z - (R0.Z + ABz * T);
		const float D = FMath::Sqrt(Dx * Dx + Dy * Dy + Dz * Dz);
		OutFloor = R0.W + (R1.W - R0.W) * T;
		if (D >= R2.Y)
		{
			OutWeight = 0.0f;
			return;
		}
		const float U = FMath::Clamp((D - R2.X) / FMath::Max(1.0f, R2.Y - R2.X), 0.0f, 1.0f);
		OutWeight = (1.0f - U * U * (3.0f - 2.0f * U)) * (R2.Z + (R2.W - R2.Z) * T);
	}

	/** The cut and the fill so far, folded with one segment's change at a point. */
	FORCEINLINE void Fold(float Weight, float Floor, float Natural, float& InOutCut, float& InOutFill)
	{
		const float Change = Weight * (Floor - Natural);
		InOutCut = FMath::Min(InOutCut, Change);
		InOutFill = FMath::Max(InOutFill, Change);
	}

	/** @return The height with the cut and the fill */
	FORCEINLINE float Finish(float Natural, float Cut, float Fill)
	{
		return (Natural + Cut) + Fill;
	}

	/** Appends a network segment as its three rows, its positions less an origin (the reader's frame) */
	VOXELGENERATION_API void AppendRows(const FVoxelValleySegment& Segment, const FVector& Origin, TArray<FVector4f>& Out);
}

/**
 * The valleys over each XY column of a chunk on a heightmap world: per column, the weight and the floor of every valley
 * segment that reaches it. The CPU generator works it out once per column before its voxel loop (EvaluateColumns), as
 * the conditioning lines' columns, and applies it to the column's natural height (Apply).
 */
struct VOXELGENERATION_API FVoxelChunkValleyColumns
{
	/** Per column (X + Y x ChunkSize) the index of its first entry, and one past the last column's end */
	TArray<int32> First;

	/** (weight, floor) per segment that reaches a column, column after column */
	TArray<FVector2f> Entries;

	/** The sea's level: a natural height under it is left as it is. -MAX_flt with no sea */
	float SeaLevel = -MAX_flt;

	/** @return Whether no column has a valley */
	bool IsEmpty() const { return Entries.Num() == 0; }

	/**
	 * A column's height with the valleys applied.
	 * @param Column The column's index, X + Y x ChunkSize
	 * @param Natural The natural height there
	 * @return The height with the valleys
	 */
	float Apply(int32 Column, float Natural) const
	{
		if (Natural < SeaLevel || First[Column] == First[Column + 1])
		{
			return Natural;
		}
		float Cut = 0.0f, Fill = 0.0f;
		for (int32 Index = First[Column]; Index < First[Column + 1]; ++Index)
		{
			VoxelValleyCarve::Fold(Entries[Index].X, Entries[Index].Y, Natural, Cut, Fill);
		}
		return VoxelValleyCarve::Finish(Natural, Cut, Fill);
	}
};

/**
 * A chunk's valley segment list: the segments whose valley reaches the chunk, in the chunk's frame (relative to its
 * origin corner on a heightmap world; to its anchor's point on the datum sphere on a planet, the conditioning lines'
 * frame), in floats. One function builds it (Build) and both generators read it: the CPU generator evaluates it
 * (EvaluateColumns on a heightmap world, Apply at each voxel's datum point on a planet) and the GPU uploads the same
 * float4s and walks them (GpuApplyValleys, Valleys.ush).
 */
struct VOXELGENERATION_API FVoxelChunkValleySegments
{
	/** The segments, VoxelValleyCarve::SegmentStride float4s each: what the GPU uploads as ValleyData */
	TArray<FVector4f> Data;

	/** The sea's level (a planet: its altitude), -MAX_flt with no sea */
	float SeaLevel = -MAX_flt;

	/** The segments' bounds in the chunk's frame, widened by their valleys' half widths (a planet's voxels test it first: MayReach) */
	FBox3f Bounds = FBox3f(ForceInit);

	/** @return How many segments the list holds */
	int32 Num() const { return Data.Num() / VoxelValleyCarve::SegmentStride; }

	/**
	 * Build a chunk's list from its request's valley network (a request with none gives an empty list).
	 * @param Request The chunk's generation request
	 * @param Out Reset and filled
	 */
	static void Build(const FVoxelNoiseGenerationRequest& Request, FVoxelChunkValleySegments& Out);

	/**
	 * A planet chunk's list: the segments within a chord of the anchor's datum point (the conditioning lines' cap).
	 * @param Network The network
	 * @param AnchorDirection The chunk anchor's direction from the planet's centre
	 * @param PlanetRadius The datum radius
	 * @param CapChord How far from the anchor's datum point the chunk's voxels' datum points lie; negative: all of them
	 * @param Out Reset and filled
	 */
	static void BuildOnSphere(const FVoxelValleyNetwork& Network, const FVector& AnchorDirection, double PlanetRadius, double CapChord, FVoxelChunkValleySegments& Out);

	/**
	 * The list at a point of the chunk's frame: the height with the valleys. The shader's loop, operation for operation.
	 * @param P The point, in the chunk's frame (z 0 on a heightmap world)
	 * @param Natural The natural height there
	 */
	float Apply(const FVector3f& P, float Natural) const;

	/** @return Whether a point of the chunk's frame lies inside the list's bounds (a planet's voxel: the walk is skipped outside) */
	bool MayReach(const FVector3f& P) const { return Bounds.IsValid && Bounds.IsInsideOrOn(P); }

	/**
	 * Each column's (weight, floor) entries, at the columns' offsets as the shader forms them (X x VoxelSize, Y x VoxelSize).
	 * @param ChunkSize Voxels along the chunk's edge
	 * @param VoxelSize The request's voxel size
	 * @param Out Reset and filled
	 */
	void EvaluateColumns(int32 ChunkSize, float VoxelSize, FVoxelChunkValleyColumns& Out) const;
};
