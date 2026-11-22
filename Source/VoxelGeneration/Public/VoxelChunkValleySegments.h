// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FVoxelNoiseGenerationRequest;
struct FVoxelValleySegment;

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
 * A segment is ValleySegmentStride float4s: [A.x, A.y, B.x, B.y] [floor at A, floor at B, floor half width, valley half
 * width] [taper at A, taper at B, 0, 0], positions in the frame of the reader (a chunk's origin corner for the
 * generators, the world for the point query).
 */
namespace VoxelValleyCarve
{
	static constexpr int32 SegmentStride = 3;

	/**
	 * A segment's weight and floor at a point.
	 * @param Px, Py The point, in the segment's frame
	 * @param R0, R1, R2 The segment's three rows
	 * @param OutWeight 0 outside the valley, 1 on its floor
	 * @param OutFloor The floor's height at the point's nearest place along the segment
	 */
	FORCEINLINE void SegmentAt(float Px, float Py, const FVector4f& R0, const FVector4f& R1, const FVector4f& R2, float& OutWeight, float& OutFloor)
	{
		const float ABx = R0.Z - R0.X;
		const float ABy = R0.W - R0.Y;
		const float Length2 = ABx * ABx + ABy * ABy;
		const float T = Length2 > 0.0f ? FMath::Clamp(((Px - R0.X) * ABx + (Py - R0.Y) * ABy) / Length2, 0.0f, 1.0f) : 0.0f;
		const float Dx = Px - (R0.X + ABx * T);
		const float Dy = Py - (R0.Y + ABy * T);
		const float D = FMath::Sqrt(Dx * Dx + Dy * Dy);
		OutFloor = R1.X + (R1.Y - R1.X) * T;
		if (D >= R1.W)
		{
			OutWeight = 0.0f;
			return;
		}
		const float U = FMath::Clamp((D - R1.Z) / FMath::Max(1.0f, R1.W - R1.Z), 0.0f, 1.0f);
		OutWeight = (1.0f - U * U * (3.0f - 2.0f * U)) * (R2.X + (R2.Y - R2.X) * T);
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
	VOXELGENERATION_API void AppendRows(const FVoxelValleySegment& Segment, const FVector2D& Origin, TArray<FVector4f>& Out);
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
 * origin corner), in floats. One function builds it (Build) and both generators read it: the CPU generator evaluates it
 * (EvaluateColumns) and the GPU uploads the same float4s and walks them (GpuApplyValleys, Valleys.ush).
 */
struct VOXELGENERATION_API FVoxelChunkValleySegments
{
	/** The segments, VoxelValleyCarve::SegmentStride float4s each: what the GPU uploads as ValleyData */
	TArray<FVector4f> Data;

	/** The sea's level, -MAX_flt with no sea */
	float SeaLevel = -MAX_flt;

	/** @return How many segments the list holds */
	int32 Num() const { return Data.Num() / VoxelValleyCarve::SegmentStride; }

	/**
	 * Build a chunk's list from its request's valley network (a request with none, or a planet's, gives an empty list).
	 * @param Request The chunk's generation request
	 * @param Out Reset and filled
	 */
	static void Build(const FVoxelNoiseGenerationRequest& Request, FVoxelChunkValleySegments& Out);

	/**
	 * The list at a point of the chunk's frame: the height with the valleys. The shader's loop, operation for operation.
	 * @param Px, Py The point, from the chunk's origin corner
	 * @param Natural The natural height there
	 */
	float Apply(float Px, float Py, float Natural) const;

	/**
	 * Each column's (weight, floor) entries, at the columns' offsets as the shader forms them (X x VoxelSize, Y x VoxelSize).
	 * @param ChunkSize Voxels along the chunk's edge
	 * @param VoxelSize The request's voxel size
	 * @param Out Reset and filled
	 */
	void EvaluateColumns(int32 ChunkSize, float VoxelSize, FVoxelChunkValleyColumns& Out) const;
};
