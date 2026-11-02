// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCrossingGradient.h"

// What the Dual Contouring mesher's interior pass (VoxelCPUDualContourMesher.cpp) and its seam meshers
// (VoxelCPUDualContourSeams.cpp) share: the gradient and shading normals and the cell's edge table. Private to
// VoxelMeshing.

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
	 * face's axis against the sun (G25). Where no tap clamps the result is the plain central difference, bit for bit.
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
			if (Span > 0 && Span < 2 * Stride)
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
	 * or, where the fit is ill-conditioned, ClampedGradientNormal at stride 1 about the voxel the point floors to. Every
	 * view (the interior pass, each seam sampler) passes its own density and box, so a ring recompute runs the interior
	 * pass's op sequence.
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
