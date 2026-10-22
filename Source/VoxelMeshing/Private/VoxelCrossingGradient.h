// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

// voxel.Mesh.CrossingGradientNormals (VoxelCPUDualContourMesher.cpp): the smooth meshers' voxel-resolution shading normals.
extern TAutoConsoleVariable<int32> CVarVoxelCrossingGradientNormals;

/**
 * The density gradient at a point of the surface, fitted to the voxels around it (G62, WORLD_MODES_REVIEW_PLAN.md §14.1).
 *
 * Generated density is a one-voxel ramp of the signed distance that saturates beyond it (SignedDistanceToDensity: 0 and 255
 * a voxel either side of the surface along local up). The central difference both smooth meshers took at voxel resolution
 * floored the point to a voxel and read the voxels one either side of that voxel, and the one below a surface a fraction f
 * above it lies in the saturated rock: the difference along up was 1 - f/2 of the ramp's, so a slope's normal tilted by
 * 1.0-1.8 times the slope depending on where the surface crossed the voxel layer. That drew bands a voxel layer apart (LOD
 * 0 of both meshers; Dual Contouring averages a cell's crossings, which share the cell's layer, so it banded about as much),
 * and on a planet, where every grid axis crosses the ramp, errors of a degree or more on flat ground.
 *
 * Here the gradient is the weighted least-squares fit of a linear function to the voxels of the 3x3x3 block around the
 * point's nearest voxel that are on the ramp (0 < density < 255; a saturated voxel only bounds the ramp) and held by the
 * caller's view, with tent weights centred on the point (the product over the axes of 1.5 minus the distance, voxels) and
 * a free offset (exact for any plane through or near the point, so a geomorphed vertex off the surface is fitted too). A
 * plane's normal comes out exact whatever the surface's phase in the voxel grid. When the voxels on the ramp do not span
 * three directions (the surface passing through a voxel layer leaves one layer on the ramp, where the floored central
 * difference is exact; or too few voxels held) the fit reports failure and the caller keeps its old estimate.
 *
 * Every read is a voxel of the block, in one fixed order, so a caller that passes the same view gets the same bits.
 */
namespace VoxelCrossingGradient
{
	/** voxel.Mesh.CrossingGradientNormals: 1 = the fit (default), 0 = the floored central difference, as before. */
	inline bool UseCrossingGradientNormals()
	{
		return CVarVoxelCrossingGradientNormals.GetValueOnAnyThread() != 0;
	}

	/** det(M) / (trace(M) / 3)^3 below this: the voxels on the ramp do not span three directions (planes give 0.2-0.8). */
	constexpr float MinConditioning = 0.02f;

	/**
	 * The outward normal (minus the fitted density gradient) at a point.
	 *
	 * @param Density (X, Y, Z) -> density in [0, 1] at a voxel
	 * @param IsHeld (X, Y, Z) -> whether the caller's view holds that voxel (reads outside it are skipped, never clamped)
	 * @param X, Y, Z The point (voxels, the view's frame)
	 * @param OutNormal The unit normal, written only on success
	 * @return False when the fit is ill-conditioned (the caller falls back to its central difference)
	 */
	template <typename TDensityFn, typename THeldFn>
	bool FitNormal(const TDensityFn& Density, const THeldFn& IsHeld, float X, float Y, float Z, FVector3f& OutNormal)
	{
		const float P[3] = { X, Y, Z };
		int32 Base[3];
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Base[Axis] = FMath::FloorToInt(P[Axis] + 0.5f) - 1;
		}

		// Weighted sums: W, W*d, W*D, W*d*d^T, W*D*d (d = voxel - point, D = density).
		float SW = 0.0f;
		float SD = 0.0f;
		float SX = 0.0f, SY = 0.0f, SZ = 0.0f;
		float XX = 0.0f, XY = 0.0f, XZ = 0.0f, YY = 0.0f, YZ = 0.0f, ZZ = 0.0f;
		float DX = 0.0f, DY = 0.0f, DZ = 0.0f;
		for (int32 K = 0; K < 3; ++K)
		{
			const int32 VZ = Base[2] + K;
			const float OZ = static_cast<float>(VZ) - Z;
			const float WZ = 1.5f - FMath::Abs(OZ);
			if (WZ <= 0.0f)
			{
				continue;
			}
			for (int32 J = 0; J < 3; ++J)
			{
				const int32 VY = Base[1] + J;
				const float OY = static_cast<float>(VY) - Y;
				const float WY = 1.5f - FMath::Abs(OY);
				if (WY <= 0.0f)
				{
					continue;
				}
				for (int32 I = 0; I < 3; ++I)
				{
					const int32 VX = Base[0] + I;
					const float OX = static_cast<float>(VX) - X;
					const float WX = 1.5f - FMath::Abs(OX);
					if (WX <= 0.0f || !IsHeld(VX, VY, VZ))
					{
						continue;
					}
					const float D = Density(VX, VY, VZ);
					if (D <= 0.0f || D >= 1.0f)
					{
						continue;
					}
					const float W = WX * WY * WZ;
					SW += W;
					SD += W * D;
					SX += W * OX;
					SY += W * OY;
					SZ += W * OZ;
					XX += W * OX * OX;
					XY += W * OX * OY;
					XZ += W * OX * OZ;
					YY += W * OY * OY;
					YZ += W * OY * OZ;
					ZZ += W * OZ * OZ;
					DX += W * D * OX;
					DY += W * D * OY;
					DZ += W * D * OZ;
				}
			}
		}
		if (SW <= 0.0f)
		{
			return false;
		}

		// Centre on the weighted means (the free offset), then solve M g = r by the adjugate.
		const float InvW = 1.0f / SW;
		const float M00 = XX - SX * SX * InvW, M01 = XY - SX * SY * InvW, M02 = XZ - SX * SZ * InvW;
		const float M11 = YY - SY * SY * InvW, M12 = YZ - SY * SZ * InvW, M22 = ZZ - SZ * SZ * InvW;
		const float R0 = DX - SD * SX * InvW, R1 = DY - SD * SY * InvW, R2 = DZ - SD * SZ * InvW;
		const float C00 = M11 * M22 - M12 * M12;
		const float C01 = M02 * M12 - M01 * M22;
		const float C02 = M01 * M12 - M02 * M11;
		const float C11 = M00 * M22 - M02 * M02;
		const float C12 = M01 * M02 - M00 * M12;
		const float C22 = M00 * M11 - M01 * M01;
		const float Det = M00 * C00 + M01 * C01 + M02 * C02;
		const float Third = (M00 + M11 + M22) / 3.0f;
		if (!(Third > 0.0f) || !(Det > MinConditioning * Third * Third * Third))
		{
			return false;
		}
		// g = adj(M) r / det(M); det > 0 here, so the direction of adj(M) r is the gradient's.
		FVector3f Normal(
			-(C00 * R0 + C01 * R1 + C02 * R2),
			-(C01 * R0 + C11 * R1 + C12 * R2),
			-(C02 * R0 + C12 * R1 + C22 * R2));
		if (!Normal.Normalize())
		{
			return false;
		}
		OutNormal = Normal;
		return true;
	}
}
