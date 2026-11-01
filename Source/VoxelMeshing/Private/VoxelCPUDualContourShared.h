// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "VoxelCrossingGradient.h"

// What the Dual Contouring mesher's interior pass (VoxelCPUDualContourMesher.cpp) and its seam meshers
// (VoxelCPUDualContourSeams.cpp) share: the console variables both read, the gradient and shading normals, and the cell's
// edge table. Private to VoxelMeshing.

/** voxel.Mesh.QEFSolve (defined in VoxelCPUDualContourMesher.cpp) */
extern TAutoConsoleVariable<int32> CVarDCQEFSolve;

/** voxel.Mesh.ScaleClampedGradients (defined in VoxelCPUDualContourMesher.cpp) */
extern TAutoConsoleVariable<int32> CVarDCScaleClampedGradients;

namespace VoxelDCTables
{
	/** One edge of a cube cell: its min corner's offset from the cell and its axis */
	struct FEdgeRef { int32 DX, DY, DZ, Axis; };

	/** The 12 edges of a cube cell, 4 along each axis, in the order the QEF accumulates them (the interior and the seams) */
	inline constexpr FEdgeRef CellEdges[12] = {
		{0, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 0, 2},
		{1, 0, 0, 1}, {1, 0, 0, 2},
		{0, 1, 0, 0}, {0, 1, 0, 2},
		{0, 0, 1, 0}, {0, 0, 1, 1},
		{1, 1, 0, 2}, {1, 0, 1, 1}, {0, 1, 1, 0},
	};
}

namespace VoxelDCGradient
{
	/**
	 * The central-difference gradient normal (taps Stride voxels either side) with every tap clamped into the box [Lo, Hi]:
	 * "the terrain continues" past the data a view holds, so no mesh depends on a neighbour's voxels (seam ownership).
	 *
	 * A clamped tap shortens its axis's difference (one-sided at a chunk face), so that component is scaled back to the full
	 * span. Unscaled, the axis a chunk face cut lost up to half its weight and the normals of the cells along the face leaned
	 * away from it: lines of shading along chunk faces wherever the surface crosses them obliquely, darker or brighter by the
	 * face's axis against the sun (G25; on a planet at stride 4, after G44, the Z faces' strips). Where no tap clamps the
	 * result is the plain central difference, bit for bit.
	 *
	 * @param Density (X, Y, Z) -> density at a voxel inside the box
	 * @param Lo, Hi The box (inclusive voxel coordinates per axis)
	 * @param IX, IY, IZ The voxel the gradient is taken at
	 * @param Stride The taps' distance (voxels)
	 * @return The outward normal (minus the gradient), +Z where it vanishes
	 */
	template <typename TDensityFn>
	FVector3f ClampedGradientNormal(const TDensityFn& Density, const int32 Lo[3], const int32 Hi[3],
		int32 IX, int32 IY, int32 IZ, int32 Stride)
	{
		const int32 I[3] = { IX, IY, IZ };
		float G[3];
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			int32 Plus[3];
			int32 Minus[3];
			for (int32 A = 0; A < 3; ++A)
			{
				const int32 Offset = (A == Axis) ? Stride : 0;
				Plus[A] = FMath::Clamp(I[A] + Offset, Lo[A], Hi[A]);
				Minus[A] = FMath::Clamp(I[A] - Offset, Lo[A], Hi[A]);
			}
			G[Axis] = Density(Plus[0], Plus[1], Plus[2]) - Density(Minus[0], Minus[1], Minus[2]);
			const int32 Span = Plus[Axis] - Minus[Axis];
			if (Span > 0 && Span < 2 * Stride && CVarDCScaleClampedGradients.GetValueOnAnyThread() != 0)
			{
				G[Axis] *= static_cast<float>(2 * Stride) / static_cast<float>(Span);
			}
		}
		FVector3f Normal(-G[0], -G[1], -G[2]);
		if (!Normal.Normalize())
		{
			return FVector3f(0.0f, 0.0f, 1.0f);
		}
		return Normal;
	}

	/**
	 * The shading normal at a point of the surface at voxel resolution (G62): the density gradient fitted to the voxels
	 * of the box [Lo, Hi] around it (VoxelCrossingGradient::FitNormal; voxels outside the box are skipped, not clamped),
	 * or, where the fit is ill-conditioned or voxel.Mesh.CrossingGradientNormals is 0, ClampedGradientNormal at stride 1
	 * about the voxel the point floors to, as before. Every view (the interior pass, each seam sampler) passes its own
	 * density and box, so a ring recompute runs the interior pass's op sequence.
	 *
	 * @param Density (X, Y, Z) -> density at a voxel inside the box
	 * @param Lo, Hi The box (inclusive voxel coordinates per axis)
	 * @param X, Y, Z The point (voxels)
	 * @return The outward unit normal
	 */
	template <typename TDensityFn>
	FVector3f CrossingShadingNormal(const TDensityFn& Density, const int32 Lo[3], const int32 Hi[3], float X, float Y, float Z)
	{
		const VoxelCrossingGradient::FBoxHeld IsHeld{ { Lo[0], Lo[1], Lo[2] }, { Hi[0], Hi[1], Hi[2] } };
		return VoxelCrossingGradient::ShadingNormal(Density, IsHeld, X, Y, Z,
			[&]() { return ClampedGradientNormal(Density, Lo, Hi, FMath::FloorToInt(X), FMath::FloorToInt(Y), FMath::FloorToInt(Z), 1); });
	}
}

/**
 * Vertex normals of strided cells (G44, WORLD_MODES_REVIEW_PLAN.md §14.1 and §20.8).
 *
 * Generated density saturates a voxel either side of the surface (SignedDistanceToDensity), so a gradient whose taps lie
 * Stride voxels apart reads 0 or 255 on every axis the surface crosses within them: at stride 4 each component becomes a
 * sign and the normal snaps toward the nearest of the 26 cube directions. On a planet, where local up is diagonal, LOD 2
 * leaned toward the cube diagonals (the ring brighter than the LOD 0/1 centre and the far field); on a heightmap its
 * slopes snapped. Every LOD's data is at voxel resolution, so a strided cell's vertex normal is the normal LOD 0 gives the
 * voxel cell where the surface crosses the strided cell's edges nearest the vertex: LOD 0's estimator with LOD 0's
 * footprint. Averaging voxel-scale normals over the whole strided cell instead smooths them (brighter on rough terrain),
 * and a single gradient sample is noisier than LOD 0's cell average (darker).
 *
 * Edge crossings at LOD > 0 (G80, WORLD_MODES_REVIEW_PLAN.md §23.16, VISUAL_POLISH_PLAN.md VP5): interpolated between the two samples a stride
 * apart, the same saturation put a strided edge's crossing toward the edge's middle by where the surface crosses the
 * strided grid, so LOD 1 and 2 were terraced a stride apart (ribs in the terrain's own shadows under a low sun). With
 * voxel.Mesh.VoxelLODCrossings the crossing is LOD 0's on the first voxel step along the edge that changes sign
 * (VoxelLODCrossing::FindVoxelCrossing; Marching Cubes' rule beside it) and its QEF normal the fitted gradient there
 * (G62's shading normal), so every LOD's vertices lie on LOD 0's surface; LOD 2 still drops the detail its cells cannot hold.
 *
 * Density and Gradient are the caller's views (the mesher's request, or a seam sampler, each with its clamp rule), so the
 * interior pass and the seam rings run one op sequence and stay bit-identical. Every voxel read lies inside the strided
 * cell, so no view reads past the data a cell's own vertex reads. Gradient is LOD 0's shading normal: the fitted gradient
 * of G62 (CalculateShadingNormal, the samplers' ShadingNormal), which reads the voxels within a voxel and a half of the
 * crossing that its view holds.
 */
namespace VoxelDCShading
{
	/**
	 * Whether the QEF's planes take the fitted gradient's normal (G62's shading normal, exact for a plane) at a crossing of
	 * this stride: at a stride with G80's voxel crossings, at stride 1 with G81's solve (which uses the normals: before it,
	 * a flat cell's vertex fell back to its mass point whatever they said) and the fitted gradient on.
	 */
	inline bool UseFittedQEFNormals(int32 Stride)
	{
		return (Stride > 1)
			? VoxelCrossingGradient::UseVoxelLODCrossings(Stride)
			: (CVarDCQEFSolve.GetValueOnAnyThread() != 0 && VoxelCrossingGradient::UseCrossingGradientNormals());
	}

	using VoxelLODCrossing::FVoxelCrossing;

	/**
	 * LOD 0's vertex normal of the voxel cell with min corner Cell: LOD 0's gradient normals at its crossings, summed in
	 * the cell's edge order (SolveCellVertices' CellEdges) and normalized, with SolveCellVertices' +Z fallback.
	 */
	template <typename TDensityFn, typename TGradientFn>
	FVector3f VoxelCellNormal(const TDensityFn& Density, const TGradientFn& Gradient, const FIntVector& Cell, float IsoLevel)
	{
		static const int32 Edges[12][4] = {
			{0, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 0, 2},
			{1, 0, 0, 1}, {1, 0, 0, 2},
			{0, 1, 0, 0}, {0, 1, 0, 2},
			{0, 0, 1, 0}, {0, 0, 1, 1},
			{1, 1, 0, 2}, {1, 0, 1, 1}, {0, 1, 1, 0},
		};
		float Corner[8];
		for (int32 i = 0; i < 8; ++i)
		{
			Corner[i] = Density(Cell.X + (i & 1), Cell.Y + ((i >> 1) & 1), Cell.Z + ((i >> 2) & 1));
		}
		FVector3f Sum = FVector3f::ZeroVector;
		for (const int32* E : Edges)
		{
			const int32 A[3] = { E[0], E[1], E[2] };
			int32 B[3] = { E[0], E[1], E[2] };
			++B[E[3]];
			const float D0 = Corner[A[0] + A[1] * 2 + A[2] * 4];
			const float D1 = Corner[B[0] + B[1] * 2 + B[2] * 4];
			if ((D0 >= IsoLevel) == (D1 >= IsoLevel))
			{
				continue;
			}
			const float T = FMath::Clamp((IsoLevel - D0) / (D1 - D0), 0.0f, 1.0f);
			float C[3] = { static_cast<float>(Cell.X + A[0]), static_cast<float>(Cell.Y + A[1]), static_cast<float>(Cell.Z + A[2]) };
			C[E[3]] += T;
			Sum += Gradient(C[0], C[1], C[2]);
		}
		if (!Sum.Normalize())
		{
			Sum = FVector3f(0.0f, 0.0f, 1.0f);
		}
		return Sum;
	}

	/** A strided cell's voxel crossings in the cell's edge order, and the vertex normal they give. */
	struct FCellVoxelCrossings
	{
		FVoxelCrossing Crossings[12];
		int32 Num = 0;

		void Add(const FVoxelCrossing& Crossing)
		{
			if (Num < 12)
			{
				Crossings[Num++] = Crossing;
			}
		}

		/**
		 * LOD 0's normal of the voxel cell at the crossing nearest the vertex (the first in edge order on a tie).
		 *
		 * @param VertexPosition The solved vertex (cell frame, uu)
		 * @param VoxelSize Voxel size (uu)
		 * @param Density, Gradient, IsoLevel As for VoxelCellNormal
		 * @param Fallback The normal when the cell has no crossing (none reaches here: the QEF needs one)
		 */
		template <typename TDensityFn, typename TGradientFn>
		FVector3f VertexNormal(const FVector3f& VertexPosition, float VoxelSize, const TDensityFn& Density,
			const TGradientFn& Gradient, float IsoLevel, const FVector3f& Fallback) const
		{
			if (Num == 0)
			{
				return Fallback;
			}
			int32 Best = 0;
			float BestDistSq = TNumericLimits<float>::Max();
			for (int32 i = 0; i < Num; ++i)
			{
				const float DistSq = FVector3f::DistSquared(Crossings[i].VoxelPosition * VoxelSize, VertexPosition);
				if (DistSq < BestDistSq)
				{
					BestDistSq = DistSq;
					Best = i;
				}
			}
			return VoxelCellNormal(Density, Gradient, Crossings[Best].VoxelCell, IsoLevel);
		}
	};
}
