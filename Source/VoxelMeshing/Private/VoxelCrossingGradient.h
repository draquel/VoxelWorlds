// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

// voxel.Mesh.CrossingGradientNormals (VoxelCPUDualContourMesher.cpp): the smooth meshers' voxel-resolution shading normals.
extern TAutoConsoleVariable<int32> CVarVoxelCrossingGradientNormals;
// voxel.Mesh.VoxelShadingNormals (VoxelCPUDualContourMesher.cpp): both smooth meshers' vertex normals at LOD > 0 (G44, G61).
extern TAutoConsoleVariable<int32> CVarVoxelShadingNormals;
// voxel.Mesh.VoxelLODCrossings (VoxelCPUDualContourMesher.cpp): both smooth meshers' edge crossings at LOD > 0 (G80).
extern TAutoConsoleVariable<int32> CVarVoxelLODCrossings;

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

	/** voxel.Mesh.VoxelShadingNormals for a vertex or cell of this stride (always false at stride 1: LOD 0 is the reference). */
	inline bool UseVoxelShadingNormals(int32 Stride)
	{
		return Stride > 1 && CVarVoxelShadingNormals.GetValueOnAnyThread() != 0;
	}

	/**
	 * voxel.Mesh.VoxelLODCrossings for an edge of this stride, or this many voxels long (always false at 1: a one-voxel
	 * edge's crossing is LOD 0's, so LOD 0, and a ribbon's fine side against LOD 0, stay bit for bit as they were).
	 */
	inline bool UseVoxelLODCrossings(int32 StrideOrLength)
	{
		return StrideOrLength > 1 && CVarVoxelLODCrossings.GetValueOnAnyThread() != 0;
	}

	/** The voxels of the box [Lo, Hi] (inclusive per axis) as FitNormal's IsHeld. */
	struct FBoxHeld
	{
		int32 Lo[3];
		int32 Hi[3];
		bool operator()(int32 VX, int32 VY, int32 VZ) const
		{
			return VX >= Lo[0] && VX <= Hi[0] && VY >= Lo[1] && VY <= Hi[1] && VZ >= Lo[2] && VZ <= Hi[2];
		}
	};

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

	/**
	 * The shading normal at a point of the surface (G62): the fitted gradient over the voxels the caller's view holds
	 * (voxel.Mesh.CrossingGradientNormals 1), else, or where the fit is ill-conditioned, the caller's own estimate (its
	 * central difference at stride 1, as before G62). One body behind both meshers' CalculateShadingNormal and Dual
	 * Contouring's seam samplers (code review CR2, MESH-10).
	 *
	 * @param Density (X, Y, Z) -> density in [0, 1] at a voxel
	 * @param IsHeld (X, Y, Z) -> whether the caller's view holds that voxel
	 * @param X, Y, Z The point (voxels, the view's frame)
	 * @param Fallback () -> the caller's estimate where the fit is off or fails
	 * @return The outward unit normal
	 */
	template <typename TDensityFn, typename THeldFn, typename TFallbackFn>
	FVector3f ShadingNormal(const TDensityFn& Density, const THeldFn& IsHeld, float X, float Y, float Z, const TFallbackFn& Fallback)
	{
		if (UseCrossingGradientNormals())
		{
			FVector3f Normal;
			if (FitNormal(Density, IsHeld, X, Y, Z, Normal))
			{
				return Normal;
			}
		}
		return Fallback();
	}
}

/**
 * Where a strided edge's surface crosses at voxel resolution (G80, voxel.Mesh.VoxelLODCrossings). Every LOD's data is at
 * voxel resolution, so a strided edge's vertex lies on LOD 0's surface when it takes one voxel step's crossing instead of
 * the interpolation between the edge's two saturated ends (which put the vertex toward the edge's middle: the LOD ribs).
 * The two smooth meshers pick the step by two rules, kept apart on purpose (code review CR2, MESH-11):
 *   - Dual Contouring takes the first voxel step along the edge that changes sign (FindVoxelCrossing): its QEF takes one
 *     crossing per edge in the edge's walking order, and the voxel cell beside that step is where G44 reads the vertex's
 *     normal;
 *   - Marching Cubes takes the step whose crossing lies nearest the place InterpolateEdge gave the vertex
 *     (FindEdgeVoxelCrossing): the vertex moves the least, and the transition ribbon's geomorph target follows it.
 * Each is a pure function of the edge, walked from its min end, so every cell, seam frame and ribbon sharing an edge finds
 * the same point. Changing either rule moves that mesher's LOD 1 and 2 vertices (the Streaming.*.LODPositions baselines).
 */
namespace VoxelLODCrossing
{
	/** Where a strided edge's surface crosses at voxel resolution, and the voxel cell inside the strided cell there. */
	struct FVoxelCrossing
	{
		FVector3f VoxelPosition; // voxels, the strided cell's frame
		FIntVector VoxelCell;    // min corner of the voxel cell inside the strided cell next to the crossing
	};

	/**
	 * Dual Contouring's rule: the first of a strided cell edge's Stride voxel steps whose ends differ in sign.
	 *
	 * @param Density (X, Y, Z) -> density, the view the edge's endpoints were read through
	 * @param VX, VY, VZ The edge's first endpoint (voxels)
	 * @param Axis The edge's axis
	 * @param Stride The edge's length (voxels)
	 * @param IsoLevel The surface threshold
	 * @param EdgeOffset The edge's corner offset in the cell (FEdgeRef's DX, DY, DZ): 1 = the edge runs along the cell's
	 *                   max face on that axis
	 * @return The crossing (the edge's first step if none changes sign, which a crossing on the edge excludes)
	 */
	template <typename TDensityFn>
	FVoxelCrossing FindVoxelCrossing(const TDensityFn& Density, int32 VX, int32 VY, int32 VZ, int32 Axis, int32 Stride,
		float IsoLevel, const int32 EdgeOffset[3])
	{
		int32 P[3] = { VX, VY, VZ };
		float T = 0.0f;
		float D0 = Density(P[0], P[1], P[2]);
		for (int32 Step = 0; Step < Stride; ++Step)
		{
			int32 N[3] = { P[0], P[1], P[2] };
			++N[Axis];
			const float D1 = Density(N[0], N[1], N[2]);
			if ((D0 >= IsoLevel) != (D1 >= IsoLevel))
			{
				T = FMath::Clamp((IsoLevel - D0) / (D1 - D0), 0.0f, 1.0f);
				break;
			}
			if (Step + 1 < Stride)
			{
				P[Axis] = N[Axis];
				D0 = D1;
			}
		}
		FVoxelCrossing Out;
		float C[3] = { static_cast<float>(P[0]), static_cast<float>(P[1]), static_cast<float>(P[2]) };
		C[Axis] += T;
		Out.VoxelPosition = FVector3f(C[0], C[1], C[2]);
		// Across the edge, the voxel cell inside the strided cell starts a voxel below the edge on each axis where the
		// edge runs along the cell's max face.
		int32 Cell[3] = { P[0], P[1], P[2] };
		for (int32 A = 0; A < 3; ++A)
		{
			if (A != Axis)
			{
				Cell[A] -= EdgeOffset[A];
			}
		}
		Out.VoxelCell = FIntVector(Cell[0], Cell[1], Cell[2]);
		return Out;
	}

	/**
	 * Marching Cubes' rule: of the voxel steps along the edge whose ends differ in sign, the one whose crossing lies nearest
	 * the place InterpolateEdge gives the vertex (the first on a tie). Reads only the voxels on the edge.
	 *
	 * @param Density (X, Y, Z) -> density, the view the edge's endpoints were read through
	 * @param EndA, EndB The edge's endpoints (voxels); they differ along one axis
	 * @param DensityA, DensityB The densities at EndA and EndB, which the vertex was interpolated from
	 * @param IsoLevel The surface threshold
	 * @param OutCrossing Out: the crossing (voxels)
	 * @return false for an edge that is not axis-aligned or has no voxel crossing
	 */
	template <typename TDensityFn>
	bool FindEdgeVoxelCrossing(const TDensityFn& Density, const FIntVector& EndA, const FIntVector& EndB,
		float DensityA, float DensityB, float IsoLevel, FVector3f& OutCrossing)
	{
		const FIntVector Delta = EndB - EndA;
		const int32 Axis = (Delta.X != 0) ? 0 : (Delta.Y != 0) ? 1 : 2;
		const int32 Length = FMath::Abs(Delta[Axis]);
		const int32 OffAxes = (Delta.X != 0 ? 1 : 0) + (Delta.Y != 0 ? 1 : 0) + (Delta.Z != 0 ? 1 : 0);
		if (Length == 0 || OffAxes != 1)
		{
			return false;
		}

		// Walk from the edge's min end, so every cell sharing the edge reads it in one order.
		const bool bAFirst = Delta[Axis] > 0;
		const FIntVector Lo = bAFirst ? EndA : EndB;
		const float DLo = bAFirst ? DensityA : DensityB;
		const float DHi = bAFirst ? DensityB : DensityA;

		// The vertex's place on the edge (voxels from Lo), as InterpolateEdge puts it.
		const float VertexT = (FMath::Abs(DHi - DLo) < KINDA_SMALL_NUMBER)
			? 0.5f : FMath::Clamp((IsoLevel - DLo) / (DHi - DLo), 0.0f, 1.0f);
		const float VertexAlong = VertexT * static_cast<float>(Length);

		// The voxel step whose crossing lies nearest the vertex (the first on a tie): LOD 0's vertex on that step.
		FIntVector P = Lo;
		float D0 = DLo;
		float BestAlong = -1.0f;
		float BestDist = TNumericLimits<float>::Max();
		for (int32 Step = 0; Step < Length; ++Step)
		{
			FIntVector N = P;
			++N[Axis];
			const float D1 = (Step + 1 == Length) ? DHi : Density(N.X, N.Y, N.Z);
			if ((D0 >= IsoLevel) != (D1 >= IsoLevel))
			{
				const float T = FMath::Clamp((IsoLevel - D0) / (D1 - D0), 0.0f, 1.0f);
				const float Along = static_cast<float>(Step) + T;
				const float Dist = FMath::Abs(Along - VertexAlong);
				if (Dist < BestDist)
				{
					BestDist = Dist;
					BestAlong = Along;
				}
			}
			P = N;
			D0 = D1;
		}
		if (BestAlong < 0.0f)
		{
			return false;
		}

		OutCrossing = FVector3f(static_cast<float>(Lo.X), static_cast<float>(Lo.Y), static_cast<float>(Lo.Z));
		OutCrossing[Axis] += BestAlong;
		return true;
	}
}
