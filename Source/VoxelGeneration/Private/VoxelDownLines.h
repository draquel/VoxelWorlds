// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelNoiseTypes.h"
#include "SphericalPlanetWorldMode.h"

/**
 * The digital lines along a down direction that a planet chunk's post-passes scan: the radial water fill's seeding and
 * the underground classification (spherical SP-B7, SP-B8).
 *
 * The lines are sheared along the axis nearest to Down (AxisA). With K the step index along AxisA from the chunk's upper
 * side, voxel (a, b, c) is on line (b - Shifts[K].X, c - Shifts[K].Y), where Shifts[K] = (round(K * SlopeB),
 * round(K * SlopeC)): every voxel is on exactly one line, and each step moves at most one voxel sideways. With Down = -Z
 * the lines are the chunk's columns, top to bottom.
 *
 * One geometry for both generators: FVoxelCPUNoiseGenerator walks it with ForEach, and the GPU post-passes
 * (VoxelPostPasses.usf, WaterLineSeedCS / UndergroundLinesCS) upload AxisA, bForward and Shifts and walk the same lines,
 * one thread per line (spherical Phase 2, WORLD_MODES_REVIEW_PLAN.md §16). The shifts are rounded here, in double, so the
 * GPU never rounds K * slope the other way at a half.
 *
 * Plain data; build per chunk on any thread.
 */
struct FVoxelDownLines
{
	/** Voxels per chunk edge */
	int32 ChunkSize = 0;

	/** The axis nearest to Down (0 = X, 1 = Y, 2 = Z); lines step along it */
	int32 AxisA = 2;

	/** Down points along +AxisA (the lines start at index 0 on AxisA), else along -AxisA */
	bool bForward = false;

	/** Per step K in [0, ChunkSize): the line's sideways offset on AxisB = (AxisA + 1) % 3 (X) and AxisC = (AxisA + 2) % 3 (Y) */
	TArray<FIntPoint> Shifts;

	/**
	 * The lines along a down direction.
	 *
	 * @param InChunkSize Voxels per chunk edge
	 * @param Down Unit down direction (a planet chunk's: PlanetChunkDown)
	 * @return The line geometry
	 */
	static FVoxelDownLines Make(int32 InChunkSize, const FVector& Down)
	{
		FVoxelDownLines Lines;
		Lines.ChunkSize = InChunkSize;
		const FVector AbsDown = Down.GetAbs();
		Lines.AxisA = (AbsDown.X >= AbsDown.Y && AbsDown.X >= AbsDown.Z) ? 0 : (AbsDown.Y >= AbsDown.Z ? 1 : 2);
		const int32 AxisB = (Lines.AxisA + 1) % 3;
		const int32 AxisC = (Lines.AxisA + 2) % 3;
		Lines.bForward = Down[Lines.AxisA] > 0.0;
		const double SlopeB = Down[AxisB] / FMath::Max(AbsDown[Lines.AxisA], UE_DOUBLE_SMALL_NUMBER);
		const double SlopeC = Down[AxisC] / FMath::Max(AbsDown[Lines.AxisA], UE_DOUBLE_SMALL_NUMBER);
		Lines.Shifts.SetNum(InChunkSize);
		for (int32 K = 0; K < InChunkSize; ++K)
		{
			Lines.Shifts[K] = FIntPoint(FMath::RoundToInt32(K * SlopeB), FMath::RoundToInt32(K * SlopeC));
		}
		return Lines;
	}

	/**
	 * The local down direction at a planet chunk's centre: the direction the chunk's lines run.
	 *
	 * @param Request A planet chunk's generation request
	 * @return Unit down direction
	 */
	static FVector PlanetChunkDown(const FVoxelNoiseGenerationRequest& Request)
	{
		const FVector ChunkCenter = Request.GetChunkWorldPosition() + FVector(0.5 * Request.ChunkSize * Request.VoxelSize);
		return -FSphericalPlanetWorldMode::GetDirectionFromCenter(ChunkCenter, Request.SphericalPlanetParams.PlanetCenter);
	}

	/** Line ids ForEach hands out: [0, NumLines()). Ids span [-ChunkSize, 2 * ChunkSize) on each sheared axis. */
	int32 NumLines() const
	{
		return 9 * ChunkSize * ChunkSize;
	}

	/**
	 * Visits every voxel of the chunk once, line by line, each line from where it enters the chunk on its upper side to
	 * where it leaves.
	 *
	 * @param Visit Called as Visit(LineId, VoxelIndex, Local), in order along each line; LineId in [0, NumLines())
	 */
	template <typename FVisit>
	void ForEach(FVisit&& Visit) const
	{
		const int32 AxisB = (AxisA + 1) % 3;
		const int32 AxisC = (AxisA + 2) % 3;
		const int32 SliceSize = ChunkSize * ChunkSize;
		const int32 Span = 3 * ChunkSize;

		FIntVector Local;
		for (int32 K = 0; K < ChunkSize; ++K)
		{
			Local[AxisA] = bForward ? K : ChunkSize - 1 - K;
			const int32 ShiftB = Shifts[K].X;
			const int32 ShiftC = Shifts[K].Y;
			for (int32 C = 0; C < ChunkSize; ++C)
			{
				Local[AxisC] = C;
				for (int32 B = 0; B < ChunkSize; ++B)
				{
					Local[AxisB] = B;
					const int32 LineId = (B - ShiftB + ChunkSize) + (C - ShiftC + ChunkSize) * Span;
					Visit(LineId, Local.X + Local.Y * ChunkSize + Local.Z * SliceSize, Local);
				}
			}
		}
	}
};
