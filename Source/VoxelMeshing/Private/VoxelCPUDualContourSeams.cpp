// Copyright Daniel Raquel. All Rights Reserved.

// FVoxelCPUDualContourMesher's seam meshers: the face, edge and corner seams between chunks (SEAM_OWNERSHIP_ARCHITECTURE.md),
// a second translation unit of the class.

#include "VoxelCPUDualContourMesher.h"
#include "VoxelCPUDualContourShared.h"
#include "VoxelMeshing.h"
#include "QEFSolver.h"
#include "VoxelCrossingGradient.h"
#include "VoxelMeshingVoxelSampler.h"

// ============================================================================
// Face-Seam Meshing (SEAM_OWNERSHIP_ARCHITECTURE.md §2.2)
// ============================================================================

namespace VoxelDCFaceSeam
{
	/**
	 * What the three seam samplers share (code review CR2, MESH-13): a voxel's density, and the shading normal over the
	 * sampler's held box. TDerived gives GetVoxel (its view of the participants' voxels, Air outside it) and GetBox
	 * (the box of voxels that view holds, frame-local). One body, so every sampler's ring recompute runs the interior pass's
	 * op sequence.
	 */
	template <typename TDerived>
	struct TSeamSamplerBase
	{
		float GetDensity(int32 X, int32 Y, int32 Z) const
		{
			return static_cast<float>(static_cast<const TDerived*>(this)->GetVoxel(X, Y, Z).Density) / 255.0f;
		}

		/** The shading normal at a voxel-resolution crossing over the same box: the interior pass's CalculateShadingNormal view (G62). */
		FVector3f ShadingNormal(float X, float Y, float Z) const
		{
			int32 Lo[3];
			int32 Hi[3];
			static_cast<const TDerived*>(this)->GetBox(Lo, Hi);
			return VoxelDCGradient::CrossingShadingNormal(
				[this](int32 TX, int32 TY, int32 TZ) { return GetDensity(TX, TY, TZ); }, Lo, Hi, X, Y, Z);
		}
	};

	/**
	 * Voxel sampler over a face-seam request. Three views:
	 *   AOnly    — owner-local coords; Air outside A. Exactly A's Interior-pass view.
	 *   BOnly    — B-local coords; Air outside B. Exactly B's Interior-pass view.
	 *   Combined — owner-local coords; A for axis-coord [0, ChunkSize), B for
	 *              [ChunkSize, 2*ChunkSize) (shifted), Air otherwise (transverse
	 *              out-of-bounds is edge/corner-neighbour territory).
	 * Ring recomputes MUST use the per-side views so their results are bit-identical to the
	 * interior passes (same data, same Air clamp); only slab cells use the combined view.
	 */
	struct FSeamSampler : TSeamSamplerBase<FSeamSampler>
	{
		const FVoxelFaceSeamRequest& Req;
		enum class EMode : uint8 { AOnly, BOnly, Combined };
		EMode Mode;

		FSeamSampler(const FVoxelFaceSeamRequest& InReq, EMode InMode) : Req(InReq), Mode(InMode) {}

		FVoxelData GetVoxel(int32 X, int32 Y, int32 Z) const
		{
			const int32 CS = Req.ChunkSize;
			if (X >= 0 && X < CS && Y >= 0 && Y < CS && Z >= 0 && Z < CS)
			{
				const TArray<FVoxelData>& Data = (Mode == EMode::BOnly) ? *Req.VoxelDataB : *Req.VoxelDataA;
				return Data[X + Y * CS + Z * CS * CS];
			}
			if (Mode == EMode::Combined)
			{
				int32 C[3] = { X, Y, Z };
				const int32 U = Req.Axis;
				if (C[U] >= CS && C[U] < 2 * CS)
				{
					C[U] -= CS;
					if (C[0] >= 0 && C[0] < CS && C[1] >= 0 && C[1] < CS && C[2] >= 0 && C[2] < CS)
					{
						return (*Req.VoxelDataB)[C[0] + C[1] * CS + C[2] * CS * CS];
					}
				}
			}
			return FVoxelData::Air();
		}

		/** The sampler's valid data box (frame-local): own chunk only, or both chunks along the face axis for Combined. */
		void GetBox(int32 Lo[3], int32 Hi[3]) const
		{
			const int32 CS = Req.ChunkSize;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				Lo[Axis] = 0;
				Hi[Axis] = CS - 1;
			}
			if (Mode == EMode::Combined)
			{
				Hi[Req.Axis] = 2 * CS - 1;
			}
		}
	};

	/** Cell vertex for the seam pass (mirrors the mesher's private FDCCellVertex). */
	struct FSeamCellVertex
	{
		FVector3f Position;
		FVector3f Normal;
		uint8 EmittedMaterialID = 0;
		uint8 EmittedBiomeID = 0;
		/** The material blend code of the primary emission (VoxelMaterialBlend, VP2-O4) */
		uint32 EmittedBlend = 0;
		/** The cell's own material (VoxelDCMaterialBlend::FCellMaterialVote; valid when bValid) */
		uint8 CellMaterial = 0;
		int32 MeshVertexIndex = -1;
		bool bValid = false;
		/** The mixed meshers tried ComputeBoundaryFallbackVertex on this invalid cell (G17); never retried. */
		bool bFallbackTried = false;
	};

	/**
	 * SolveCellVertices' cell-edge table (the one table: same entries, same ORDER). QEF accumulation is order-sensitive in
	 * float, and the ring recompute must be bit-identical to the interior pass.
	 */
	using VoxelDCTables::FEdgeRef;
	const auto& GCellEdges = VoxelDCTables::CellEdges;

	/** MUST mirror GenerateQuads' AxisOffsets (winding-correct 4-cell order per edge axis). */
	struct FCellOffset { int32 DX, DY, DZ; };
	static const FCellOffset GAxisOffsets[3][4] = {
		{{0, 0, 0}, {0, -1, 0}, {0, -1, -1}, {0, 0, -1}},
		{{0, 0, 0}, {0, 0, -1}, {-1, 0, -1}, {-1, 0, 0}},
		{{0, 0, 0}, {-1, 0, 0}, {-1, -1, 0}, {0, -1, 0}},
	};

	/**
	 * Quadrant-visibility sampler over an edge-seam request.
	 *
	 * Works in a chosen participant's FRAME (FrameOffsetVoxels maps frame-local coords into the
	 * owner-frame quadrant space) with a 4-bit visibility mask over the (PerpA, PerpB) quadrants
	 * (bit = dv + dw*2, matching FVoxelEdgeSeamRequest::VoxelData order). Along the edge-parallel
	 * axis only [0, ChunkSize) is owned — outside is Air (corner-seam territory). With the
	 * right mask+frame this reproduces EXACTLY every earlier job's sampler: a participant's
	 * interior view (single quadrant) and a face job's pair view (two quadrants along one axis) —
	 * which is what makes the edge seam's ring recomputes bit-identical to those jobs' outputs.
	 */
	struct FQuadSampler : TSeamSamplerBase<FQuadSampler>
	{
		const FVoxelEdgeSeamRequest& Req;
		int32 UAxis;                  // edge-parallel axis
		int32 PerpA;                  // smaller perpendicular axis
		int32 PerpB;                  // larger perpendicular axis
		uint8 QuadrantMask;           // visible quadrants, bit = dv + dw*2
		FIntVector FrameOffsetVoxels; // frame-local -> owner-frame voxel offset

		FQuadSampler(const FVoxelEdgeSeamRequest& InReq, int32 InUAxis, int32 InPerpA, int32 InPerpB, uint8 InQuadrantMask,
			const FIntVector& InFrameOffsetVoxels)
			: Req(InReq), UAxis(InUAxis), PerpA(InPerpA), PerpB(InPerpB), QuadrantMask(InQuadrantMask), FrameOffsetVoxels(InFrameOffsetVoxels)
		{
		}

		FVoxelData GetVoxel(int32 X, int32 Y, int32 Z) const
		{
			const int32 CS = Req.ChunkSize;
			int32 C[3] = { X + FrameOffsetVoxels.X, Y + FrameOffsetVoxels.Y, Z + FrameOffsetVoxels.Z };
			if (C[UAxis] < 0 || C[UAxis] >= CS)
			{
				return FVoxelData::Air();
			}
			const int32 CA = C[PerpA];
			const int32 CB = C[PerpB];
			if (CA < 0 || CA >= 2 * CS || CB < 0 || CB >= 2 * CS)
			{
				return FVoxelData::Air();
			}
			const int32 Dv = (CA >= CS) ? 1 : 0;
			const int32 Dw = (CB >= CS) ? 1 : 0;
			if (!(QuadrantMask & (1 << (Dv + Dw * 2))))
			{
				return FVoxelData::Air();
			}
			int32 L[3];
			L[UAxis] = C[UAxis];
			L[PerpA] = CA - Dv * CS;
			L[PerpB] = CB - Dw * CS;
			const TArray<FVoxelData>& Data = *Req.VoxelData[Dv + Dw * 2];
			return Data[L[0] + L[1] * CS + L[2] * CS * CS];
		}

		/** The mask's valid box (frame-local): masks are boxes by construction (per-axis allowed halves). */
		void GetBox(int32 Lo[3], int32 Hi[3]) const
		{
			const int32 CS = Req.ChunkSize;
			Lo[UAxis] = -FrameOffsetVoxels[UAxis];
			Hi[UAxis] = CS - 1 - FrameOffsetVoxels[UAxis];
			const bool bDv0 = (QuadrantMask & 0b0101) != 0;
			const bool bDv1 = (QuadrantMask & 0b1010) != 0;
			const bool bDw0 = (QuadrantMask & 0b0011) != 0;
			const bool bDw1 = (QuadrantMask & 0b1100) != 0;
			Lo[PerpA] = (bDv0 ? 0 : CS) - FrameOffsetVoxels[PerpA];
			Hi[PerpA] = (bDv1 ? 2 * CS - 1 : CS - 1) - FrameOffsetVoxels[PerpA];
			Lo[PerpB] = (bDw0 ? 0 : CS) - FrameOffsetVoxels[PerpB];
			Hi[PerpB] = (bDw1 ? 2 * CS - 1 : CS - 1) - FrameOffsetVoxels[PerpB];
		}
	};

	/**
	 * Octant-visibility sampler over a corner-seam request.
	 *
	 * The 3D generalization of FQuadSampler: eight participants around the shared corner, an
	 * 8-bit visibility mask (bit = dx + dy*2 + dz*4, matching FVoxelCornerSeamRequest::VoxelData
	 * order), and a frame offset mapping frame-local coords into owner-frame octant space. With
	 * the right mask+frame this reproduces EXACTLY every earlier job's sampler — an interior view
	 * (1 octant), a face job's pair view (2 octants), and an edge job's quad view (4 octants,
	 * whose parallel-axis clamp falls out of the octant masking for free).
	 */
	struct FOctSampler : TSeamSamplerBase<FOctSampler>
	{
		const FVoxelCornerSeamRequest& Req;
		uint8 OctantMask;             // visible octants, bit = dx + dy*2 + dz*4
		FIntVector FrameOffsetVoxels; // frame-local -> owner-frame voxel offset

		FOctSampler(const FVoxelCornerSeamRequest& InReq, uint8 InOctantMask, const FIntVector& InFrameOffsetVoxels)
			: Req(InReq), OctantMask(InOctantMask), FrameOffsetVoxels(InFrameOffsetVoxels)
		{
		}

		FVoxelData GetVoxel(int32 X, int32 Y, int32 Z) const
		{
			const int32 CS = Req.ChunkSize;
			const int32 C[3] = { X + FrameOffsetVoxels.X, Y + FrameOffsetVoxels.Y, Z + FrameOffsetVoxels.Z };
			if (C[0] < 0 || C[0] >= 2 * CS || C[1] < 0 || C[1] >= 2 * CS || C[2] < 0 || C[2] >= 2 * CS)
			{
				return FVoxelData::Air();
			}
			const int32 Dx = (C[0] >= CS) ? 1 : 0;
			const int32 Dy = (C[1] >= CS) ? 1 : 0;
			const int32 Dz = (C[2] >= CS) ? 1 : 0;
			const int32 Octant = Dx + Dy * 2 + Dz * 4;
			if (!(OctantMask & (1 << Octant)))
			{
				return FVoxelData::Air();
			}
			const int32 LX = C[0] - Dx * CS;
			const int32 LY = C[1] - Dy * CS;
			const int32 LZ = C[2] - Dz * CS;
			return (*Req.VoxelData[Octant])[LX + LY * CS + LZ * CS * CS];
		}

		/** The octant mask's valid box (frame-local): octant masks are boxes by construction (per-axis allowed halves). */
		void GetBox(int32 Lo[3], int32 Hi[3]) const
		{
			const int32 CS = Req.ChunkSize;
			// Per-axis half presence: low/high octant-mask bits per axis (X, Y, Z).
			static const uint8 LowHalf[3] = { 0x55, 0x33, 0x0F };
			static const uint8 HighHalf[3] = { 0xAA, 0xCC, 0xF0 };
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				const bool bLow = (OctantMask & LowHalf[Axis]) != 0;
				const bool bHigh = (OctantMask & HighHalf[Axis]) != 0;
				Lo[Axis] = (bLow ? 0 : CS) - FrameOffsetVoxels[Axis];
				Hi[Axis] = (bHigh ? 2 * CS - 1 : CS - 1) - FrameOffsetVoxels[Axis];
			}
		}
	};

	/**
	 * Compute one DC cell vertex against a sampler, replicating DetectEdgeCrossings +
	 * SolveCellVertices op-for-op: per-edge crossing formula (t clamp, P0/P1 construction),
	 * gradient normal at the crossing, QEF.Add in GCellEdges order, the same Solve call, and
	 * the same averaged-normal fallback. With the per-side samplers this makes ring-cell
	 * results bit-identical to the corresponding Interior-domain pass (or, via FQuadSampler's
	 * restricted masks, to the corresponding face-seam job). Templated so every sampler type
	 * shares the single op sequence — the bit-exactness contract lives in ONE place.
	 */
	template <typename TSampler>
	static bool ComputeCellVertex(
		const TSampler& S,
		int32 CX, int32 CY, int32 CZ,
		int32 Stride, float VoxelSize, float IsoLevel, float SVDThreshold, float BiasStrength,
		FSeamCellVertex& Out)
	{
		FQEFSolver QEF;
		FVector3f AvgNormal = FVector3f::ZeroVector;
		// SolveCellVertices' vertex normal at a stride (G80) and at LOD 0 (G62), op for op.
		const bool bCrossingShading = Stride == 1;
		FVector3f ShadingSum = FVector3f::ZeroVector;
		auto DensityFn = [&S](int32 X, int32 Y, int32 Z) { return S.GetDensity(X, Y, Z); };
		VoxelDCMaterialBlend::FCellMaterialVote MaterialVote;

		for (const FEdgeRef& Edge : GCellEdges)
		{
			const int32 VX = (CX + Edge.DX) * Stride;
			const int32 VY = (CY + Edge.DY) * Stride;
			const int32 VZ = (CZ + Edge.DZ) * Stride;

			const float D0 = S.GetDensity(VX, VY, VZ);

			int32 NX = VX, NY = VY, NZ = VZ;
			if (Edge.Axis == 0) NX += Stride;
			else if (Edge.Axis == 1) NY += Stride;
			else NZ += Stride;

			const float D1 = S.GetDensity(NX, NY, NZ);

			const bool bSolid0 = (D0 >= IsoLevel);
			const bool bSolid1 = (D1 >= IsoLevel);
			if (bSolid0 == bSolid1)
			{
				continue;
			}

			// The cell's material vote: the solid endpoint's material (soft borders, VP2-O4; the interior pass's rule)
			if (bSolid0)
			{
				MaterialVote.Add(VoxelDCMaterialBlend::CornerIndex(Edge.DX, Edge.DY, Edge.DZ), S.GetVoxel(VX, VY, VZ).MaterialID);
			}
			else
			{
				MaterialVote.Add(VoxelDCMaterialBlend::CornerIndex(Edge.DX + (Edge.Axis == 0 ? 1 : 0),
					Edge.DY + (Edge.Axis == 1 ? 1 : 0), Edge.DZ + (Edge.Axis == 2 ? 1 : 0)), S.GetVoxel(NX, NY, NZ).MaterialID);
			}

			float t = (IsoLevel - D0) / (D1 - D0);
			t = FMath::Clamp(t, 0.0f, 1.0f);

			const FVector3f P0(static_cast<float>(VX) * VoxelSize,
				static_cast<float>(VY) * VoxelSize,
				static_cast<float>(VZ) * VoxelSize);
			const FVector3f P1(static_cast<float>(NX) * VoxelSize,
				static_cast<float>(NY) * VoxelSize,
				static_cast<float>(NZ) * VoxelSize);

			// DetectEdgeCrossings' crossing and QEF normal, op for op (G80, G81).
			const FVector3f CrossPos = (Stride > 1)
				? VoxelLODCrossing::FindVoxelCrossing(DensityFn, VX, VY, VZ, Edge.Axis, Stride, IsoLevel) * VoxelSize
				: P0 + (P1 - P0) * t;
			const FVector3f CrossNormal = S.ShadingNormal(CrossPos.X / VoxelSize, CrossPos.Y / VoxelSize, CrossPos.Z / VoxelSize);

			QEF.Add(CrossPos, CrossNormal);
			AvgNormal += CrossNormal;
			if (bCrossingShading)
			{
				ShadingSum += CrossNormal;
			}
		}

		if (QEF.Count == 0)
		{
			Out.bValid = false;
			return false;
		}

		const float CellWorldSize = static_cast<float>(Stride) * VoxelSize;
		const float MinX = static_cast<float>(CX * Stride) * VoxelSize;
		const float MinY = static_cast<float>(CY * Stride) * VoxelSize;
		const float MinZ = static_cast<float>(CZ * Stride) * VoxelSize;
		const FBox3f CellBounds(
			FVector3f(MinX, MinY, MinZ),
			FVector3f(MinX + CellWorldSize, MinY + CellWorldSize, MinZ + CellWorldSize)
		);

		Out.bValid = true;
		Out.MeshVertexIndex = -1;
		Out.Position = QEF.Solve(SVDThreshold, CellBounds, BiasStrength);
		Out.CellMaterial = MaterialVote.Result();

		if (!AvgNormal.Normalize())
		{
			AvgNormal = FVector3f(0.0f, 0.0f, 1.0f);
		}
		Out.Normal = (bCrossingShading && ShadingSum.Normalize()) ? ShadingSum : AvgNormal;
		return true;
	}

	/**
	 * The vertex of a coarse cell that has no crossing at its own stride but has a finer one on its boundary
	 * (G17, WORLD_MODES_REVIEW_PLAN.md §14.1). The mixed meshers stitch at the finest stride of their
	 * participants, so a fine crossing on the face plane (or a tuple's shared lines) can face a coarse cell
	 * whose corners all have one sign: the surface pokes through the coarse cell's face between its samples.
	 * That cell has no vertex at its own stride, and skipping the fine fan left a small hole bounded by the
	 * fine side's quads (two to six open edges). Here the cell gets one: the same QEF solve over every
	 * voxel-resolution (stride-1) crossing on the cell's six faces, normals at stride 1, positions in the
	 * coarse cell's bounds (surface-derived, inside its own cell: the #44 laws).
	 *
	 * Only for cells that ComputeCellVertex left invalid, so it never moves a vertex an interior pass or a
	 * same-LOD seam emits (they only emit valid cells). It is a pure function of the sampler view, the cell
	 * and its stride, not of the calling job's stitch stride, so every mixed job that fans onto the cell
	 * (face, edge, corner) solves the same vertex. Any finer crossing on the boundary implies a stride-1
	 * crossing there, so a cell a fan needs always gets a vertex.
	 */
	template <typename TSampler>
	static bool ComputeBoundaryFallbackVertex(
		const TSampler& S,
		int32 CX, int32 CY, int32 CZ,
		int32 Stride, float VoxelSize, float IsoLevel, float SVDThreshold, float BiasStrength,
		FSeamCellVertex& Out)
	{
		Out.bFallbackTried = true;
		if (Stride <= 1)
		{
			return false; // a stride-1 cell's boundary crossings are its own edges: ComputeCellVertex saw them all
		}

		const int32 Lo[3] = { CX * Stride, CY * Stride, CZ * Stride };
		const int32 Hi[3] = { Lo[0] + Stride, Lo[1] + Stride, Lo[2] + Stride };
		FQEFSolver QEF;
		FVector3f AvgNormal = FVector3f::ZeroVector;
		// The QEF's planes and the vertex normal from the crossings' shading normals (G62, G81).
		FVector3f ShadingSum = FVector3f::ZeroVector;
		uint8 FallbackMaterial = 0;
		bool bHaveMaterial = false;

		for (int32 Z = Lo[2]; Z <= Hi[2]; ++Z)
		{
			for (int32 Y = Lo[1]; Y <= Hi[1]; ++Y)
			{
				for (int32 X = Lo[0]; X <= Hi[0]; ++X)
				{
					const int32 P[3] = { X, Y, Z };
					const bool bOnMin[3] = { X == Lo[0], Y == Lo[1], Z == Lo[2] };
					const bool bOnMax[3] = { X == Hi[0], Y == Hi[1], Z == Hi[2] };
					for (int32 Axis = 0; Axis < 3; ++Axis)
					{
						if (P[Axis] == Hi[Axis])
						{
							continue; // the edge would leave the cell
						}
						// On the boundary: some other axis sits on the cell's min or max plane.
						const int32 A1 = (Axis + 1) % 3;
						const int32 A2 = (Axis + 2) % 3;
						if (!(bOnMin[A1] || bOnMax[A1] || bOnMin[A2] || bOnMax[A2]))
						{
							continue;
						}

						const float D0 = S.GetDensity(X, Y, Z);
						int32 N[3] = { X, Y, Z };
						++N[Axis];
						const float D1 = S.GetDensity(N[0], N[1], N[2]);
						if ((D0 >= IsoLevel) == (D1 >= IsoLevel))
						{
							continue;
						}
						if (!bHaveMaterial)
						{
							// The cell's material: its first fine crossing's solid endpoint (VP2-O4; the cell has no
							// crossing at its own stride to vote)
							FallbackMaterial = (D0 >= IsoLevel) ? S.GetVoxel(X, Y, Z).MaterialID : S.GetVoxel(N[0], N[1], N[2]).MaterialID;
							bHaveMaterial = true;
						}

						float t = (IsoLevel - D0) / (D1 - D0);
						t = FMath::Clamp(t, 0.0f, 1.0f);
						const FVector3f P0(static_cast<float>(X) * VoxelSize, static_cast<float>(Y) * VoxelSize, static_cast<float>(Z) * VoxelSize);
						const FVector3f P1(static_cast<float>(N[0]) * VoxelSize, static_cast<float>(N[1]) * VoxelSize, static_cast<float>(N[2]) * VoxelSize);
						const FVector3f CrossPos = P0 + (P1 - P0) * t;
						const FVector3f CrossNormal = S.ShadingNormal(CrossPos.X / VoxelSize, CrossPos.Y / VoxelSize, CrossPos.Z / VoxelSize);
						QEF.Add(CrossPos, CrossNormal);
						AvgNormal += CrossNormal;
						ShadingSum += CrossNormal;
					}
				}
			}
		}

		if (QEF.Count == 0)
		{
			return false;
		}

		const float CellWorldSize = static_cast<float>(Stride) * VoxelSize;
		const FVector3f Min(static_cast<float>(Lo[0]) * VoxelSize, static_cast<float>(Lo[1]) * VoxelSize, static_cast<float>(Lo[2]) * VoxelSize);
		const FBox3f CellBounds(Min, Min + FVector3f(CellWorldSize));

		Out.bValid = true;
		Out.MeshVertexIndex = -1;
		Out.Position = QEF.Solve(SVDThreshold, CellBounds, BiasStrength);
		Out.CellMaterial = FallbackMaterial;
		if (!AvgNormal.Normalize())
		{
			AvgNormal = FVector3f(0.0f, 0.0f, 1.0f);
		}
		Out.Normal = ShadingSum.Normalize() ? ShadingSum : AvgNormal;
		return true;
	}

	/**
	 * Mixed-LOD face seam (the §2.3 cross-resolution mesher).
	 *
	 * The slab is ALWAYS the owner's last cell layer at the OWNER's stride (the interior domain
	 * [0, Grid-1) covers the neighbour's first layer but not the owner's last — regardless of
	 * which side is finer). Rings recompute at each side's OWN stride with that side's sampler,
	 * bit-identical to its Interior pass. The face plane stitches at the FINER granularity:
	 * every fine crossing gets a fan onto the coarser side's ring vertices, quads collapsing to
	 * triangles when their two coarse-side slots fall in the same coarse cell (the T-junction).
	 * All positions are surface-derived QEF output clamped to their own cells (#44 laws hold by
	 * construction). A fine crossing facing a coarse cell with no crossing at its own stride fans
	 * onto that cell's boundary fallback vertex (G17, ComputeBoundaryFallbackVertex) instead of
	 * being skipped, which left a hole.
	 */
	static bool GenerateMixedFaceSeam(
		const FVoxelFaceSeamRequest& Req,
		const FVoxelMeshingConfig& Config,
		FChunkMeshData& Out)
	{
		const int32 ChunkSize = Req.ChunkSize;
		const int32 LODA = FMath::Clamp(Req.LODLevel, 0, 7);
		const int32 LODB = FMath::Clamp(Req.GetLODLevelB(), 0, 7);
		const int32 SA = 1 << LODA;
		const int32 SB = 1 << LODB;
		if ((ChunkSize % SA) != 0 || (ChunkSize % SB) != 0)
		{
			UE_LOG(LogVoxelMeshing, Error, TEXT("GenerateMixedFaceSeam: ChunkSize %d not divisible by strides %d/%d"), ChunkSize, SA, SB);
			return false;
		}
		const int32 GridA = ChunkSize / SA;
		const int32 GridB = ChunkSize / SB;
		if (GridA < 3 || GridB < 3)
		{
			return true; // degenerate resolution — valid empty seam
		}
		const int32 SLA = GridA - 1;
		const int32 SLB = GridB - 1;
		const int32 MinS = FMath::Min(SA, SB);
		const int32 RA = SA / MinS; // fine steps per owner-side cell (1 when the owner is finer)
		const int32 RB = SB / MinS; // fine steps per neighbour-side cell
		const int32 FaceGrid = ChunkSize / MinS;

		const int32 U = Req.Axis;
		const int32 V = (U + 1) % 3;
		const int32 W = (U + 2) % 3;
		const float VoxelSize = Req.VoxelSize;
		const float IsoLevel = Config.IsoLevel;
		const float SVDThreshold = Config.QEFSVDThreshold;
		const float BiasStrength = Config.QEFBiasStrength;
		const float FrameOffsetU = static_cast<float>(ChunkSize) * VoxelSize;

		const FSeamSampler SamplerA(Req, FSeamSampler::EMode::AOnly);
		const FSeamSampler SamplerB(Req, FSeamSampler::EMode::BOnly);
		const FSeamSampler SamplerC(Req, FSeamSampler::EMode::Combined);

		// ---- Cell layers: rings at their side's stride, slab at the owner's ----------------
		TArray<FSeamCellVertex> RingA; RingA.SetNum(SLA * SLA);
		TArray<FSeamCellVertex> Slab;  Slab.SetNum(SLA * SLA);
		TArray<FSeamCellVertex> RingB; RingB.SetNum(SLB * SLB);

		for (int32 w = 0; w < SLA; ++w)
		{
			for (int32 v = 0; v < SLA; ++v)
			{
				const int32 Idx = v + w * SLA;
				int32 C[3];
				C[V] = v;
				C[W] = w;
				C[U] = SLA - 1;
				ComputeCellVertex(SamplerA, C[0], C[1], C[2], SA, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, RingA[Idx]);
				C[U] = SLA;
				ComputeCellVertex(SamplerC, C[0], C[1], C[2], SA, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Slab[Idx]);
			}
		}
		for (int32 w = 0; w < SLB; ++w)
		{
			for (int32 v = 0; v < SLB; ++v)
			{
				const int32 Idx = v + w * SLB;
				int32 C[3];
				C[V] = v;
				C[W] = w;
				C[U] = 0;
				if (ComputeCellVertex(SamplerB, C[0], C[1], C[2], SB, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, RingB[Idx]))
				{
					RingB[Idx].Position[U] += FrameOffsetU;
				}
			}
		}

		// ---- Vertex emission (identical projection/material carry to the other seam paths) --
		const float UVScale = Config.bGenerateUVs ? Config.UVScale : 0.0f;
		auto EmitSeamVertex = [&Out, VoxelSize, UVScale](const FSeamCellVertex& Vertex, uint8 MaterialID, uint8 BiomeID, uint32 BlendCode) -> int32
		{
			const int32 Index = Out.Positions.Num();
			Out.Positions.Add(Vertex.Position);
			Out.Normals.Add(Vertex.Normal);

			const float AbsX = FMath::Abs(Vertex.Normal.X);
			const float AbsY = FMath::Abs(Vertex.Normal.Y);
			const float AbsZ = FMath::Abs(Vertex.Normal.Z);
			FVector2f UV;
			if (AbsZ >= AbsX && AbsZ >= AbsY)
			{
				UV = FVector2f(Vertex.Position.X * UVScale / VoxelSize, Vertex.Position.Y * UVScale / VoxelSize);
			}
			else if (AbsX >= AbsY)
			{
				UV = FVector2f(Vertex.Position.Y * UVScale / VoxelSize, Vertex.Position.Z * UVScale / VoxelSize);
			}
			else
			{
				UV = FVector2f(Vertex.Position.X * UVScale / VoxelSize, Vertex.Position.Z * UVScale / VoxelSize);
			}
			Out.UVs.Add(UV);
			Out.UV1s.Add(FVector2f(static_cast<float>(MaterialID), 0.0f));
			Out.Colors.Add(FColor(MaterialID, BiomeID, 0, 255));
			Out.MaterialBlends.Add(BlendCode);
			return Index;
		};

		constexpr int32 KeyStride = 1 << 20; // layer id * KeyStride + cell index (unique across layers)
		TMap<uint64, int32> DuplicateVertexCache;
		uint32 TriangleCount = 0;

		// Fan emitter over up to 4 cyclic cell refs: adjacent duplicates (the T-junction's
		// coincident coarse slots) collapse the quad to a triangle. Quad case keeps the
		// adaptive max-min-area diagonal.
		struct FRef { FSeamCellVertex* Cell = nullptr; int32 Key = 0; };
		auto EmitFan = [&](FRef (&Refs)[4], bool bFlip, uint8 QuadMaterial, uint8 QuadBiome)
		{
			FRef Uniq[4];
			int32 N = 0;
			for (int32 i = 0; i < 4; ++i)
			{
				if (N > 0 && Refs[i].Cell == Uniq[N - 1].Cell)
				{
					continue;
				}
				Uniq[N++] = Refs[i];
			}
			if (N > 1 && Uniq[N - 1].Cell == Uniq[0].Cell)
			{
				--N;
			}
			if (N < 3)
			{
				return; // fully degenerate (both sides coarse-coincident) — nothing to emit
			}

			int32 Indices[4];
			FSeamCellVertex* QuadCells[4];
			int32 QuadKeys[4];
			for (int32 i = 0; i < N; ++i)
			{
				QuadCells[i] = Uniq[i].Cell;
				QuadKeys[i] = Uniq[i].Key;
			}
			// The quad's material and its blend toward a neighbouring cell's material (VP2-O4); the interior pass's emission
			VoxelDCMaterialBlend::EmitQuadCells(QuadCells, QuadKeys, N, QuadMaterial, QuadBiome, DuplicateVertexCache,
				[&](const FSeamCellVertex& EmitCell, uint8 EmitMaterial, uint8 EmitBiome, uint32 EmitBlend)
				{
					return EmitSeamVertex(EmitCell, EmitMaterial, EmitBiome, EmitBlend);
				},
				Indices);

			auto AddTri = [&](int32 a, int32 b, int32 c)
			{
				Out.Indices.Add(Indices[a]);
				Out.Indices.Add(Indices[b]);
				Out.Indices.Add(Indices[c]);
			};

			if (N == 3)
			{
				if (bFlip) { AddTri(0, 1, 2); }
				else       { AddTri(0, 2, 1); }
				TriangleCount += 1;
				return;
			}

			auto TriArea = [](const FVector3f& A, const FVector3f& B, const FVector3f& C) -> float
			{
				return 0.5f * FVector3f::CrossProduct(B - A, C - A).Size();
			};
			const FVector3f& P0 = Uniq[0].Cell->Position;
			const FVector3f& P1 = Uniq[1].Cell->Position;
			const FVector3f& P2 = Uniq[2].Cell->Position;
			const FVector3f& P3 = Uniq[3].Cell->Position;
			const float MinArea02 = FMath::Min(TriArea(P0, P1, P2), TriArea(P0, P2, P3));
			const float MinArea13 = FMath::Min(TriArea(P1, P2, P3), TriArea(P1, P3, P0));
			const bool bUse13 = MinArea13 > MinArea02;

			if (bFlip)
			{
				if (bUse13) { AddTri(1, 2, 3); AddTri(1, 3, 0); }
				else        { AddTri(0, 1, 2); AddTri(0, 2, 3); }
			}
			else
			{
				if (bUse13) { AddTri(1, 3, 2); AddTri(1, 0, 3); }
				else        { AddTri(0, 2, 1); AddTri(0, 3, 2); }
			}
			TriangleCount += 2;
		};

		// ---- Owner-side groups at stride SA (identical structure to the same-LOD path) ------
		auto EmitAQuad = [&](const int32 EdgeCellA[3], int32 EdgeAbsAxis)
		{
			const int32 VX = EdgeCellA[0] * SA;
			const int32 VY = EdgeCellA[1] * SA;
			const int32 VZ = EdgeCellA[2] * SA;

			const float D0 = SamplerC.GetDensity(VX, VY, VZ);
			int32 NX = VX, NY = VY, NZ = VZ;
			if (EdgeAbsAxis == 0) NX += SA;
			else if (EdgeAbsAxis == 1) NY += SA;
			else NZ += SA;
			const float D1 = SamplerC.GetDensity(NX, NY, NZ);

			const bool bSolid0 = (D0 >= IsoLevel);
			const bool bSolid1 = (D1 >= IsoLevel);
			if (bSolid0 == bSolid1)
			{
				return;
			}

			FRef Refs[4];
			for (int32 i = 0; i < 4; ++i)
			{
				const FCellOffset& Off = GAxisOffsets[EdgeAbsAxis][i];
				const int32 CellC[3] = { EdgeCellA[0] + Off.DX, EdgeCellA[1] + Off.DY, EdgeCellA[2] + Off.DZ };
				const int32 CU = CellC[U];
				const int32 TV = CellC[V];
				const int32 TW = CellC[W];
				if (TV < 0 || TV >= SLA || TW < 0 || TW >= SLA)
				{
					return;
				}
				TArray<FSeamCellVertex>* Layer = (CU == SLA) ? &Slab : (CU == SLA - 1) ? &RingA : nullptr;
				if (!Layer)
				{
					return;
				}
				FSeamCellVertex& Cell = (*Layer)[TV + TW * SLA];
				if (!Cell.bValid)
				{
					return;
				}
				Refs[i] = { &Cell, ((CU == SLA) ? KeyStride : 0) + TV + TW * SLA };
			}

			const bool bFlip = (D0 < IsoLevel);
			int32 SolidX = VX, SolidY = VY, SolidZ = VZ;
			if (bFlip)
			{
				if (EdgeAbsAxis == 0) { SolidX += SA; }
				else if (EdgeAbsAxis == 1) { SolidY += SA; }
				else { SolidZ += SA; }
			}
			const FVoxelData SolidVoxel = SamplerC.GetVoxel(SolidX, SolidY, SolidZ);
			EmitFan(Refs, bFlip, SolidVoxel.MaterialID, SolidVoxel.BiomeID);
		};

		int32 EdgeCell[3];
		// Face-crossing U-edges (4 slab cells; G1).
		for (int32 w = 1; w <= SLA - 1; ++w)
		{
			for (int32 v = 1; v <= SLA - 1; ++v)
			{
				EdgeCell[U] = SLA;
				EdgeCell[V] = v;
				EdgeCell[W] = w;
				EmitAQuad(EdgeCell, U);
			}
		}
		// Slab <-> ring-A stitching at the owner's stride (G2/G3).
		for (int32 Sel = 1; Sel <= SLA - 1; ++Sel)
		{
			for (int32 Run = 0; Run <= SLA - 1; ++Run)
			{
				EdgeCell[U] = SLA;
				EdgeCell[V] = Run;
				EdgeCell[W] = Sel;
				EmitAQuad(EdgeCell, V);

				EdgeCell[V] = Sel;
				EdgeCell[W] = Run;
				EmitAQuad(EdgeCell, W);
			}
		}

		// ---- Face-plane stitching at the FINE granularity (the T-junction fans) -------------
		auto EmitFaceEdge = [&](int32 FV, int32 FW, int32 EdgeAbsAxis)
		{
			int32 BaseVox[3];
			BaseVox[U] = ChunkSize;
			BaseVox[V] = FV * MinS;
			BaseVox[W] = FW * MinS;
			int32 EndVox[3] = { BaseVox[0], BaseVox[1], BaseVox[2] };
			EndVox[EdgeAbsAxis] += MinS;

			const float D0 = SamplerC.GetDensity(BaseVox[0], BaseVox[1], BaseVox[2]);
			const float D1 = SamplerC.GetDensity(EndVox[0], EndVox[1], EndVox[2]);
			const bool bSolid0 = (D0 >= IsoLevel);
			const bool bSolid1 = (D1 >= IsoLevel);
			if (bSolid0 == bSolid1)
			{
				return;
			}

			FRef Refs[4];
			for (int32 i = 0; i < 4; ++i)
			{
				const FCellOffset& Off = GAxisOffsets[EdgeAbsAxis][i];
				const int32 OffXYZ[3] = { Off.DX, Off.DY, Off.DZ };
				const int32 OffU = OffXYZ[U];
				const int32 FineV = FV + OffXYZ[V];
				const int32 FineW = FW + OffXYZ[W];
				if (FineV < 0 || FineW < 0)
				{
					return;
				}
				if (OffU == 0)
				{
					// Neighbour-side slot: that side's ring at ITS stride.
					const int32 BV = FineV / RB;
					const int32 BW = FineW / RB;
					if (BV >= SLB || BW >= SLB)
					{
						return;
					}
					FSeamCellVertex& Cell = RingB[BV + BW * SLB];
					if (!Cell.bValid && !Cell.bFallbackTried)
					{
						// A coarser ring cell with no crossing at its stride (G17); B's interior never emits it.
						int32 C[3];
						C[U] = 0;
						C[V] = BV;
						C[W] = BW;
						if (ComputeBoundaryFallbackVertex(SamplerB, C[0], C[1], C[2], SB, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Cell))
						{
							Cell.Position[U] += FrameOffsetU;
						}
					}
					if (!Cell.bValid)
					{
						return; // no surface on the cell's boundary at all: nothing to fan onto
					}
					Refs[i] = { &Cell, 2 * KeyStride + BV + BW * SLB };
				}
				else
				{
					// Owner-side slot: the slab at the owner's stride.
					const int32 AV = FineV / RA;
					const int32 AW = FineW / RA;
					if (AV >= SLA || AW >= SLA)
					{
						return;
					}
					FSeamCellVertex& Cell = Slab[AV + AW * SLA];
					if (!Cell.bValid && !Cell.bFallbackTried)
					{
						// A coarser slab cell with no crossing at the owner's stride (G17).
						int32 C[3];
						C[U] = SLA;
						C[V] = AV;
						C[W] = AW;
						ComputeBoundaryFallbackVertex(SamplerC, C[0], C[1], C[2], SA, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Cell);
					}
					if (!Cell.bValid)
					{
						return;
					}
					Refs[i] = { &Cell, KeyStride + AV + AW * SLA };
				}
			}

			const bool bFlip = (D0 < IsoLevel);
			int32 SolidVox[3] = { BaseVox[0], BaseVox[1], BaseVox[2] };
			if (bFlip)
			{
				SolidVox[EdgeAbsAxis] += MinS;
			}
			const FVoxelData SolidVoxel = SamplerC.GetVoxel(SolidVox[0], SolidVox[1], SolidVox[2]);
			EmitFan(Refs, bFlip, SolidVoxel.MaterialID, SolidVoxel.BiomeID);
		};

		for (int32 FW = 0; FW < FaceGrid; ++FW)
		{
			for (int32 FV = 0; FV < FaceGrid; ++FV)
			{
				EmitFaceEdge(FV, FW, V);
				EmitFaceEdge(FV, FW, W);
			}
		}

		UE_LOG(LogVoxelMeshing, Verbose,
			TEXT("DC mixed face seam %s axis=%d LOD %d|%d: %d verts, %d tris"),
			*Req.OwnerChunkCoord.ToString(), U, LODA, LODB,
			Out.Positions.Num(), TriangleCount);

		return true;
	}

	// =========================================================================================
	// Shared emission helpers for the mixed-LOD edge/corner meshers.
	// =========================================================================================

	/** A cell reference for fan emission: the solved cell + a job-unique dedup key. */
	struct FSeamRef
	{
		FSeamCellVertex* Cell = nullptr;
		int32 Key = 0;
	};

	/**
	 * Vertex/fan emitter shared by the mixed-LOD edge and corner meshers: per-cell/per-material vertex dedup, cyclic
	 * duplicate collapse (quad -> triangle at T-junctions), and the adaptive max-min-area
	 * diagonal for full quads — the exact emission semantics of the earlier seam paths.
	 */
	struct FSeamEmitter
	{
		FChunkMeshData& Out;
		float VoxelSize = 100.0f;
		float UVScale = 0.0f;
		TMap<uint64, int32> DuplicateVertexCache;
		uint32 TriangleCount = 0;

		FSeamEmitter(FChunkMeshData& InOut, float InVoxelSize, float InUVScale)
			: Out(InOut), VoxelSize(InVoxelSize), UVScale(InUVScale)
		{
		}

		int32 EmitVertex(const FSeamCellVertex& Vertex, uint8 MaterialID, uint8 BiomeID, uint32 BlendCode)
		{
			const int32 Index = Out.Positions.Num();
			Out.Positions.Add(Vertex.Position);
			Out.Normals.Add(Vertex.Normal);

			const float AbsX = FMath::Abs(Vertex.Normal.X);
			const float AbsY = FMath::Abs(Vertex.Normal.Y);
			const float AbsZ = FMath::Abs(Vertex.Normal.Z);
			FVector2f UV;
			if (AbsZ >= AbsX && AbsZ >= AbsY)
			{
				UV = FVector2f(Vertex.Position.X * UVScale / VoxelSize, Vertex.Position.Y * UVScale / VoxelSize);
			}
			else if (AbsX >= AbsY)
			{
				UV = FVector2f(Vertex.Position.Y * UVScale / VoxelSize, Vertex.Position.Z * UVScale / VoxelSize);
			}
			else
			{
				UV = FVector2f(Vertex.Position.X * UVScale / VoxelSize, Vertex.Position.Z * UVScale / VoxelSize);
			}
			Out.UVs.Add(UV);
			Out.UV1s.Add(FVector2f(static_cast<float>(MaterialID), 0.0f));
			Out.Colors.Add(FColor(MaterialID, BiomeID, 0, 255));
			Out.MaterialBlends.Add(BlendCode);
			return Index;
		}

		void EmitFan(FSeamRef (&Refs)[4], bool bFlip, uint8 QuadMaterial, uint8 QuadBiome)
		{
			FSeamRef Uniq[4];
			int32 N = 0;
			for (int32 i = 0; i < 4; ++i)
			{
				if (N > 0 && Refs[i].Cell == Uniq[N - 1].Cell)
				{
					continue;
				}
				Uniq[N++] = Refs[i];
			}
			if (N > 1 && Uniq[N - 1].Cell == Uniq[0].Cell)
			{
				--N;
			}
			if (N < 3)
			{
				return;
			}

			int32 Indices[4];
			FSeamCellVertex* QuadCells[4];
			int32 QuadKeys[4];
			for (int32 i = 0; i < N; ++i)
			{
				QuadCells[i] = Uniq[i].Cell;
				QuadKeys[i] = Uniq[i].Key;
			}
			// The quad's material and its blend toward a neighbouring cell's material (VP2-O4); the interior pass's emission
			VoxelDCMaterialBlend::EmitQuadCells(QuadCells, QuadKeys, N, QuadMaterial, QuadBiome, DuplicateVertexCache,
				[&](const FSeamCellVertex& EmitCell, uint8 EmitMaterial, uint8 EmitBiome, uint32 EmitBlend)
				{
					return EmitVertex(EmitCell, EmitMaterial, EmitBiome, EmitBlend);
				},
				Indices);

			auto AddTri = [&](int32 a, int32 b, int32 c)
			{
				Out.Indices.Add(Indices[a]);
				Out.Indices.Add(Indices[b]);
				Out.Indices.Add(Indices[c]);
			};

			if (N == 3)
			{
				if (bFlip) { AddTri(0, 1, 2); }
				else       { AddTri(0, 2, 1); }
				TriangleCount += 1;
				return;
			}

			auto TriArea = [](const FVector3f& A, const FVector3f& B, const FVector3f& C) -> float
			{
				return 0.5f * FVector3f::CrossProduct(B - A, C - A).Size();
			};
			const FVector3f& P0 = Uniq[0].Cell->Position;
			const FVector3f& P1 = Uniq[1].Cell->Position;
			const FVector3f& P2 = Uniq[2].Cell->Position;
			const FVector3f& P3 = Uniq[3].Cell->Position;
			const float MinArea02 = FMath::Min(TriArea(P0, P1, P2), TriArea(P0, P2, P3));
			const float MinArea13 = FMath::Min(TriArea(P1, P2, P3), TriArea(P1, P3, P0));
			const bool bUse13 = MinArea13 > MinArea02;

			if (bFlip)
			{
				if (bUse13) { AddTri(1, 2, 3); AddTri(1, 3, 0); }
				else        { AddTri(0, 1, 2); AddTri(0, 2, 3); }
			}
			else
			{
				if (bUse13) { AddTri(1, 3, 2); AddTri(1, 0, 3); }
				else        { AddTri(0, 2, 1); AddTri(0, 3, 2); }
			}
			TriangleCount += 2;
		}
	};

	/**
	 * Mixed-LOD edge seam. Participants may render at up to four different
	 * LODs. Each of the 9 columns computes at its OWNING job's stride (column owner = the low
	 * quadrant per axis — the mixed-LOD face seam's ownership rule per column), reproducing that job's cells
	 * bit-exactly. Slot classification is by rectangle containment (mixed strides don't tile as
	 * a product grid), and ownership follows the participant-union rule: a fine edge belongs to
	 * this job iff its slots span >= 3 participants (pairs are the face jobs' territory,
	 * singles the interiors'). Fine edges whose slots exit the 9 columns along U are the
	 * corner jobs' territory.
	 */
	static bool GenerateMixedEdgeSeam(
		const FVoxelEdgeSeamRequest& Req,
		const FVoxelMeshingConfig& Config,
		FChunkMeshData& Out)
	{
		const int32 ChunkSize = Req.ChunkSize;
		int32 S[4];
		int32 MinS = TNumericLimits<int32>::Max();
		int32 MaxS = 1;
		for (int32 q = 0; q < 4; ++q)
		{
			const int32 L = FMath::Clamp(Req.GetLODLevelOf(q), 0, 7);
			S[q] = 1 << L;
			if ((ChunkSize % S[q]) != 0)
			{
				UE_LOG(LogVoxelMeshing, Error, TEXT("GenerateMixedEdgeSeam: ChunkSize %d not divisible by stride %d"), ChunkSize, S[q]);
				return false;
			}
			if (ChunkSize / S[q] < 3)
			{
				return true; // degenerate resolution — valid empty seam
			}
			MinS = FMath::Min(MinS, S[q]);
			MaxS = FMath::Max(MaxS, S[q]);
		}

		const int32 U = Req.EdgeAxis;
		const int32 PerpA = (U == 0) ? 1 : 0;
		const int32 PerpB = (U == 2) ? 1 : 2;
		const float VoxelSize = Req.VoxelSize;
		const float IsoLevel = Config.IsoLevel;
		const float SVDThreshold = Config.QEFSVDThreshold;
		const float BiasStrength = Config.QEFBiasStrength;
		const float ChunkWorldSpan = static_cast<float>(ChunkSize) * VoxelSize;

		// ---- The 9 regions, each at its owning job's stride --------------------------------
		// Inward (-1) states span a BAND of cells (width ~2*MaxS), not a single cell: at mixed
		// strides the smaller jobs skip T-junction fans across a band as wide as the coarser
		// stride, and those fans reference cells several fine-cells deep. The extra cells are
		// ordinary cells of the owning job (same mask/frame/stride) — recomputed bit-exactly;
		// unreferenced ones are harmless.
		const int32 Band = 2 * MaxS;
		struct FColumn
		{
			int32 StrideC = 1;
			int32 VLo = 0, WLo = 0;                   // owner-frame voxel rect min (transverse)
			int32 VCnt = 1, WCnt = 1;                 // cells per transverse axis
			int32 UValidVox = 0;                      // owned U extent: [0, UValidVox) voxels
			int32 UCells = 0;
			uint8 ParticipantMask = 0;                // quadrant bits (dv + dw*2)
			FIntVector FrameOffset = FIntVector::ZeroValue; // the owning participant's frame (cell solve)
			int32 CellABase = 0, CellBBase = 0;       // frame-local cell coords of (iv, iw) = (0, 0)
			TArray<FSeamCellVertex> Cells;            // indexed (iv + iw*VCnt)*UCells + u
		};
		FColumn Columns[9];
		auto ColumnIndex = [](int32 Dcv, int32 Dcw) { return (Dcv + 1) + (Dcw + 1) * 3; };

		for (int32 Dcw = -1; Dcw <= 1; ++Dcw)
		{
			for (int32 Dcv = -1; Dcv <= 1; ++Dcv)
			{
				FColumn& Col = Columns[ColumnIndex(Dcv, Dcw)];

				const int32 OwnerDv = (Dcv == 1) ? 1 : 0;
				const int32 OwnerDw = (Dcw == 1) ? 1 : 0;
				Col.StrideC = S[OwnerDv + OwnerDw * 2];
				const int32 GridC = ChunkSize / Col.StrideC;
				const int32 SLc = GridC - 1;

				uint8 Mask = 0;
				for (int32 dw = 0; dw <= 1; ++dw)
				{
					for (int32 dv = 0; dv <= 1; ++dv)
					{
						const bool bDvOK = (Dcv == 0) || ((Dcv == -1) == (dv == 0));
						const bool bDwOK = (Dcw == 0) || ((Dcw == -1) == (dw == 0));
						if (bDvOK && bDwOK)
						{
							Mask |= (1 << (dv + dw * 2));
						}
					}
				}
				Col.ParticipantMask = Mask;

				auto RectRange = [ChunkSize, Band](int32 Dc, int32 Sc, int32 SLcIn, int32& Lo, int32& Cnt)
				{
					if (Dc == -1)
					{
						Cnt = FMath::Clamp(Band / Sc, 1, SLcIn - 1);
						Lo = ChunkSize - Sc - Cnt * Sc;
					}
					else if (Dc == 0)
					{
						Cnt = 1;
						Lo = ChunkSize - Sc;
					}
					else
					{
						Cnt = 1;
						Lo = ChunkSize;
					}
				};
				RectRange(Dcv, Col.StrideC, SLc, Col.VLo, Col.VCnt);
				RectRange(Dcw, Col.StrideC, SLc, Col.WLo, Col.WCnt);
				Col.UCells = SLc;
				Col.UValidVox = SLc * Col.StrideC; // cells u in [0, SLc)

				FIntVector FrameOffset = FIntVector::ZeroValue;
				FrameOffset[PerpA] = OwnerDv * ChunkSize;
				FrameOffset[PerpB] = OwnerDw * ChunkSize;
				const int32 CellABase = (Col.VLo - FrameOffset[PerpA]) / Col.StrideC;
				const int32 CellBBase = (Col.WLo - FrameOffset[PerpB]) / Col.StrideC;
				Col.FrameOffset = FrameOffset;
				Col.CellABase = CellABase;
				Col.CellBBase = CellBBase;

				Col.Cells.SetNum(Col.VCnt * Col.WCnt * Col.UCells);
				const FQuadSampler Sampler{ Req, U, PerpA, PerpB, Mask, FrameOffset };
				for (int32 iw = 0; iw < Col.WCnt; ++iw)
				{
					for (int32 iv = 0; iv < Col.VCnt; ++iv)
					{
						for (int32 u = 0; u < Col.UCells; ++u)
						{
							int32 C[3];
							C[U] = u;
							C[PerpA] = CellABase + iv;
							C[PerpB] = CellBBase + iw;
							FSeamCellVertex& Cell = Col.Cells[(iv + iw * Col.VCnt) * Col.UCells + u];
							if (ComputeCellVertex(Sampler, C[0], C[1], C[2], Col.StrideC, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Cell))
							{
								Cell.Position[PerpA] += OwnerDv * ChunkWorldSpan;
								Cell.Position[PerpB] += OwnerDw * ChunkWorldSpan;
							}
						}
					}
				}
			}
		}

		FSeamEmitter Emitter(Out, VoxelSize, Config.bGenerateUVs ? Config.UVScale : 0.0f);
		const FQuadSampler FullSampler{ Req, U, PerpA, PerpB, 0b1111, FIntVector::ZeroValue };
		constexpr int32 ColKeyStride = 1 << 16;

		// Classify one slot cell (given by its min-corner voxel) into the 9 regions.
		auto ClassifySlot = [&](int32 UVox, int32 VVox, int32 WVox, FSeamRef& OutRef, uint8& UnionMask) -> bool
		{
			for (int32 ci = 0; ci < 9; ++ci)
			{
				FColumn& Col = Columns[ci];
				const int32 VHi = Col.VLo + Col.VCnt * Col.StrideC;
				const int32 WHi = Col.WLo + Col.WCnt * Col.StrideC;
				if (VVox >= Col.VLo && VVox < VHi && WVox >= Col.WLo && WVox < WHi)
				{
					if (UVox < 0 || UVox >= Col.UValidVox)
					{
						return false; // the region's u-rim — corner-seam territory
					}
					const int32 IV = (VVox - Col.VLo) / Col.StrideC;
					const int32 IW = (WVox - Col.WLo) / Col.StrideC;
					const int32 CellIdx = (IV + IW * Col.VCnt) * Col.UCells + UVox / Col.StrideC;
					FSeamCellVertex& Cell = Col.Cells[CellIdx];
					if (!Cell.bValid && !Cell.bFallbackTried)
					{
						// A coarser cell with no crossing at its stride, solved as every job solves it (G17).
						int32 C[3];
						C[U] = UVox / Col.StrideC;
						C[PerpA] = Col.CellABase + IV;
						C[PerpB] = Col.CellBBase + IW;
						const FQuadSampler Sampler{ Req, U, PerpA, PerpB, Col.ParticipantMask, Col.FrameOffset };
						if (ComputeBoundaryFallbackVertex(Sampler, C[0], C[1], C[2], Col.StrideC, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Cell))
						{
							Cell.Position[PerpA] += (Col.FrameOffset[PerpA] != 0 ? 1 : 0) * ChunkWorldSpan;
							Cell.Position[PerpB] += (Col.FrameOffset[PerpB] != 0 ? 1 : 0) * ChunkWorldSpan;
						}
					}
					if (!Cell.bValid)
					{
						return false;
					}
					OutRef = FSeamRef{ &Cell, ci * ColKeyStride + CellIdx };
					UnionMask |= Col.ParticipantMask;
					return true;
				}
			}
			return false; // outside the edge region — another job's territory
		};

		auto EmitMixedEdge = [&](int32 FU, int32 FV, int32 FW, int32 EdgeAbsAxis)
		{
			int32 BaseVox[3];
			BaseVox[U] = FU * MinS;
			BaseVox[PerpA] = FV * MinS;
			BaseVox[PerpB] = FW * MinS;
			int32 EndVox[3] = { BaseVox[0], BaseVox[1], BaseVox[2] };
			EndVox[EdgeAbsAxis] += MinS;

			const float D0 = FullSampler.GetDensity(BaseVox[0], BaseVox[1], BaseVox[2]);
			const float D1 = FullSampler.GetDensity(EndVox[0], EndVox[1], EndVox[2]);
			const bool bSolid0 = (D0 >= IsoLevel);
			const bool bSolid1 = (D1 >= IsoLevel);
			if (bSolid0 == bSolid1)
			{
				return;
			}

			FSeamRef Refs[4];
			uint8 UnionMask = 0;
			for (int32 i = 0; i < 4; ++i)
			{
				const FCellOffset& Off = GAxisOffsets[EdgeAbsAxis][i];
				const int32 OffXYZ[3] = { Off.DX, Off.DY, Off.DZ };
				const int32 SlotU = BaseVox[U] + OffXYZ[U] * MinS;
				const int32 SlotV = BaseVox[PerpA] + OffXYZ[PerpA] * MinS;
				const int32 SlotW = BaseVox[PerpB] + OffXYZ[PerpB] * MinS;
				if (!ClassifySlot(SlotU, SlotV, SlotW, Refs[i], UnionMask))
				{
					return;
				}
			}
			if (FMath::CountBits(UnionMask) < 3)
			{
				return; // computable by a single face pair (or interior) — that job owns it
			}

			const bool bFlip = (D0 < IsoLevel);
			int32 SolidVox[3] = { BaseVox[0], BaseVox[1], BaseVox[2] };
			if (bFlip)
			{
				SolidVox[EdgeAbsAxis] += MinS;
			}
			const FVoxelData SolidVoxel = FullSampler.GetVoxel(SolidVox[0], SolidVox[1], SolidVox[2]);
			Emitter.EmitFan(Refs, bFlip, SolidVoxel.MaterialID, SolidVoxel.BiomeID);
		};

		// Fine enumeration over the edge neighbourhood: U along the whole column, transverse a
		// band of +-2 coarse cells around the edge line (classification rejects the excess).
		const int32 FineU = ChunkSize / MinS;
		const int32 TLo = (ChunkSize - 2 * MaxS) / MinS;
		const int32 THi = (ChunkSize + 2 * MaxS) / MinS;
		for (int32 FW = TLo; FW < THi; ++FW)
		{
			for (int32 FV = TLo; FV < THi; ++FV)
			{
				for (int32 FU = 0; FU < FineU; ++FU)
				{
					EmitMixedEdge(FU, FV, FW, U);
					EmitMixedEdge(FU, FV, FW, PerpA);
					EmitMixedEdge(FU, FV, FW, PerpB);
				}
			}
		}

		UE_LOG(LogVoxelMeshing, Verbose,
			TEXT("DC mixed edge seam %s axis=%d: %d verts, %d tris"),
			*Req.OwnerChunkCoord.ToString(), U, Out.Positions.Num(), Emitter.TriangleCount);

		return true;
	}

	/**
	 * Mixed-LOD corner seam. The 27 corner-block cells each compute at
	 * their OWNING job's stride (owner = the low octant per axis), reproducing every
	 * neighbouring job — interiors, same/mixed face jobs, same/mixed edge jobs — bit-exactly.
	 * Ownership: a fine edge belongs to the corner job iff its slot participant-union is not
	 * contained in any single axis half (halves are exactly the edge tuples' octant sets, and
	 * every face pair / interior is inside some half).
	 */
	static bool GenerateMixedCornerSeam(
		const FVoxelCornerSeamRequest& Req,
		const FVoxelMeshingConfig& Config,
		FChunkMeshData& Out)
	{
		const int32 ChunkSize = Req.ChunkSize;
		int32 S[8];
		int32 MinS = TNumericLimits<int32>::Max();
		int32 MaxS = 1;
		for (int32 o = 0; o < 8; ++o)
		{
			const int32 L = FMath::Clamp(Req.GetLODLevelOf(o), 0, 7);
			S[o] = 1 << L;
			if ((ChunkSize % S[o]) != 0)
			{
				UE_LOG(LogVoxelMeshing, Error, TEXT("GenerateMixedCornerSeam: ChunkSize %d not divisible by stride %d"), ChunkSize, S[o]);
				return false;
			}
			if (ChunkSize / S[o] < 3)
			{
				return true;
			}
			MinS = FMath::Min(MinS, S[o]);
			MaxS = FMath::Max(MaxS, S[o]);
		}

		const float VoxelSize = Req.VoxelSize;
		const float IsoLevel = Config.IsoLevel;
		const float SVDThreshold = Config.QEFSVDThreshold;
		const float BiasStrength = Config.QEFBiasStrength;
		const float ChunkWorldSpan = static_cast<float>(ChunkSize) * VoxelSize;

		// ---- The 27 corner-block regions, each at its owning job's stride -------------------
		// Inward (-1) states span a BAND of cells (~2*MaxS wide) per axis, mirroring the mixed
		// edge mesher: T-junction fans skipped by the smaller jobs reference cells several
		// fine-cells deep at mixed strides.
		const int32 Band = 2 * MaxS;
		struct FCornerRegion
		{
			int32 StrideC = 1;
			int32 Lo[3] = { 0, 0, 0 };
			int32 Cnt[3] = { 1, 1, 1 };
			uint8 ParticipantMask = 0; // octant bits
			FIntVector FrameOffset = FIntVector::ZeroValue; // the owning participant's frame (cell solve)
			int32 CellBase[3] = { 0, 0, 0 }; // frame-local cell coords of index (0, 0, 0)
			TArray<FSeamCellVertex> Cells; // indexed (ix + iy*Cnt0)*... x-innermost
		};
		FCornerRegion Regions[27];
		auto RegionIndexOf = [](int32 SX, int32 SY, int32 SZ) { return (SX + 1) + (SY + 1) * 3 + (SZ + 1) * 9; };

		for (int32 SZ = -1; SZ <= 1; ++SZ)
		{
			for (int32 SY = -1; SY <= 1; ++SY)
			{
				for (int32 SX = -1; SX <= 1; ++SX)
				{
					const int32 St[3] = { SX, SY, SZ };
					FCornerRegion& Reg = Regions[RegionIndexOf(SX, SY, SZ)];

					const int32 OwnerOct = ((SX == 1) ? 1 : 0) + ((SY == 1) ? 2 : 0) + ((SZ == 1) ? 4 : 0);
					const int32 Sc = S[OwnerOct];
					const int32 GridC = ChunkSize / Sc;
					const int32 SLc = GridC - 1;
					Reg.StrideC = Sc;

					uint8 Mask = 0;
					for (int32 Oct = 0; Oct < 8; ++Oct)
					{
						const int32 D[3] = { Oct & 1, (Oct >> 1) & 1, (Oct >> 2) & 1 };
						bool bAllowed = true;
						for (int32 Axis = 0; Axis < 3; ++Axis)
						{
							if ((St[Axis] == -1 && D[Axis] != 0) || (St[Axis] == 1 && D[Axis] != 1))
							{
								bAllowed = false;
								break;
							}
						}
						if (bAllowed)
						{
							Mask |= (1 << Oct);
						}
					}
					Reg.ParticipantMask = Mask;

					FIntVector FrameOffset(
						(SX == 1) ? ChunkSize : 0,
						(SY == 1) ? ChunkSize : 0,
						(SZ == 1) ? ChunkSize : 0);
					int32 CellBase[3];
					for (int32 Axis = 0; Axis < 3; ++Axis)
					{
						if (St[Axis] == -1)
						{
							Reg.Cnt[Axis] = FMath::Clamp(Band / Sc, 1, SLc - 1);
							Reg.Lo[Axis] = ChunkSize - Sc - Reg.Cnt[Axis] * Sc;
						}
						else if (St[Axis] == 0)
						{
							Reg.Cnt[Axis] = 1;
							Reg.Lo[Axis] = ChunkSize - Sc;
						}
						else
						{
							Reg.Cnt[Axis] = 1;
							Reg.Lo[Axis] = ChunkSize;
						}
						CellBase[Axis] = (Reg.Lo[Axis] - FrameOffset[Axis]) / Sc;
						Reg.CellBase[Axis] = CellBase[Axis];
					}
					Reg.FrameOffset = FrameOffset;

					Reg.Cells.SetNum(Reg.Cnt[0] * Reg.Cnt[1] * Reg.Cnt[2]);
					const FOctSampler Sampler{ Req, Mask, FrameOffset };
					for (int32 iz = 0; iz < Reg.Cnt[2]; ++iz)
					{
						for (int32 iy = 0; iy < Reg.Cnt[1]; ++iy)
						{
							for (int32 ix = 0; ix < Reg.Cnt[0]; ++ix)
							{
								FSeamCellVertex& Cell = Reg.Cells[(iz * Reg.Cnt[1] + iy) * Reg.Cnt[0] + ix];
								if (ComputeCellVertex(Sampler, CellBase[0] + ix, CellBase[1] + iy, CellBase[2] + iz,
									Sc, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Cell))
								{
									Cell.Position.X += (SX == 1) ? ChunkWorldSpan : 0.0f;
									Cell.Position.Y += (SY == 1) ? ChunkWorldSpan : 0.0f;
									Cell.Position.Z += (SZ == 1) ? ChunkWorldSpan : 0.0f;
								}
							}
						}
					}
				}
			}
		}

		FSeamEmitter Emitter(Out, VoxelSize, Config.bGenerateUVs ? Config.UVScale : 0.0f);
		const FOctSampler FullSampler{ Req, 0xFF, FIntVector::ZeroValue };

		// The six axis halves — exactly the edge tuples' octant sets. A union contained in any
		// half is computable by a smaller job (edge tuple, face pair, or interior).
		const uint8 HalfMasks[6] = { 0x55, 0xAA, 0x33, 0xCC, 0x0F, 0xF0 };

		constexpr int32 RegKeyStride = 1 << 10;
		auto ClassifySlot = [&](const int32 SlotVox[3], FSeamRef& OutRef, uint8& UnionMask) -> bool
		{
			for (int32 ci = 0; ci < 27; ++ci)
			{
				FCornerRegion& Reg = Regions[ci];
				bool bInside = true;
				int32 Idx[3];
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					const int32 Hi = Reg.Lo[Axis] + Reg.Cnt[Axis] * Reg.StrideC;
					if (SlotVox[Axis] < Reg.Lo[Axis] || SlotVox[Axis] >= Hi)
					{
						bInside = false;
						break;
					}
					Idx[Axis] = (SlotVox[Axis] - Reg.Lo[Axis]) / Reg.StrideC;
				}
				if (!bInside)
				{
					continue;
				}
				const int32 CellIdx = (Idx[2] * Reg.Cnt[1] + Idx[1]) * Reg.Cnt[0] + Idx[0];
				FSeamCellVertex& Cell = Reg.Cells[CellIdx];
				if (!Cell.bValid && !Cell.bFallbackTried)
				{
					// A coarser cell with no crossing at its stride, solved as every job solves it (G17).
					const FOctSampler Sampler{ Req, Reg.ParticipantMask, Reg.FrameOffset };
					if (ComputeBoundaryFallbackVertex(Sampler, Reg.CellBase[0] + Idx[0], Reg.CellBase[1] + Idx[1], Reg.CellBase[2] + Idx[2],
						Reg.StrideC, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Cell))
					{
						Cell.Position.X += (Reg.FrameOffset.X != 0) ? ChunkWorldSpan : 0.0f;
						Cell.Position.Y += (Reg.FrameOffset.Y != 0) ? ChunkWorldSpan : 0.0f;
						Cell.Position.Z += (Reg.FrameOffset.Z != 0) ? ChunkWorldSpan : 0.0f;
					}
				}
				if (!Cell.bValid)
				{
					return false;
				}
				OutRef = FSeamRef{ &Cell, ci * RegKeyStride + CellIdx };
				UnionMask |= Reg.ParticipantMask;
				return true;
			}
			return false;
		};

		auto EmitCornerEdge = [&](int32 FX, int32 FY, int32 FZ, int32 EdgeAbsAxis)
		{
			const int32 BaseVox[3] = { FX * MinS, FY * MinS, FZ * MinS };
			int32 EndVox[3] = { BaseVox[0], BaseVox[1], BaseVox[2] };
			EndVox[EdgeAbsAxis] += MinS;

			const float D0 = FullSampler.GetDensity(BaseVox[0], BaseVox[1], BaseVox[2]);
			const float D1 = FullSampler.GetDensity(EndVox[0], EndVox[1], EndVox[2]);
			const bool bSolid0 = (D0 >= IsoLevel);
			const bool bSolid1 = (D1 >= IsoLevel);
			if (bSolid0 == bSolid1)
			{
				return;
			}

			FSeamRef Refs[4];
			uint8 UnionMask = 0;
			for (int32 i = 0; i < 4; ++i)
			{
				const FCellOffset& Off = GAxisOffsets[EdgeAbsAxis][i];
				const int32 SlotVox[3] = {
					BaseVox[0] + Off.DX * MinS,
					BaseVox[1] + Off.DY * MinS,
					BaseVox[2] + Off.DZ * MinS };
				if (!ClassifySlot(SlotVox, Refs[i], UnionMask))
				{
					return;
				}
			}
			for (int32 h = 0; h < 6; ++h)
			{
				if ((UnionMask & ~HalfMasks[h]) == 0)
				{
					return; // a smaller job (edge/face/interior) owns this
				}
			}

			const bool bFlip = (D0 < IsoLevel);
			int32 SolidVox[3] = { BaseVox[0], BaseVox[1], BaseVox[2] };
			if (bFlip)
			{
				SolidVox[EdgeAbsAxis] += MinS;
			}
			const FVoxelData SolidVoxel = FullSampler.GetVoxel(SolidVox[0], SolidVox[1], SolidVox[2]);
			Emitter.EmitFan(Refs, bFlip, SolidVoxel.MaterialID, SolidVoxel.BiomeID);
		};

		const int32 FLo = (ChunkSize - 2 * MaxS) / MinS;
		const int32 FHi = (ChunkSize + 2 * MaxS) / MinS;
		for (int32 FZ = FLo; FZ < FHi; ++FZ)
		{
			for (int32 FY = FLo; FY < FHi; ++FY)
			{
				for (int32 FX = FLo; FX < FHi; ++FX)
				{
					EmitCornerEdge(FX, FY, FZ, 0);
					EmitCornerEdge(FX, FY, FZ, 1);
					EmitCornerEdge(FX, FY, FZ, 2);
				}
			}
		}

		UE_LOG(LogVoxelMeshing, Verbose,
			TEXT("DC mixed corner seam %s: %d verts, %d tris"),
			*Req.OwnerChunkCoord.ToString(), Out.Positions.Num(), Emitter.TriangleCount);

		return true;
	}
}

bool FVoxelCPUDualContourMesher::GenerateFaceSeamMeshCPU(
	const FVoxelFaceSeamRequest& SeamRequest,
	FChunkMeshData& OutMeshData)
{
	using namespace VoxelDCFaceSeam;

	OutMeshData.Reset();

	if (!bIsInitialized)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("GenerateFaceSeamMeshCPU called before Initialize"));
		return false;
	}
	if (!SeamRequest.IsValid())
	{
		UE_LOG(LogVoxelMeshing, Error, TEXT("GenerateFaceSeamMeshCPU: invalid seam request (axis=%d, chunkSize=%d, A=%d, B=%d voxels)"),
			SeamRequest.Axis, SeamRequest.ChunkSize,
			SeamRequest.VoxelDataA.IsValid() ? SeamRequest.VoxelDataA->Num() : 0,
			SeamRequest.VoxelDataB.IsValid() ? SeamRequest.VoxelDataB->Num() : 0);
		return false;
	}

	// Mixed-LOD pair: a dedicated path — the same-LOD path below stays
	// bit-frozen (its ring identities are asserted by the SC tests and relied on by the edge and corner seams).
	if (SeamRequest.GetLODLevelB() != SeamRequest.LODLevel)
	{
		return GenerateMixedFaceSeam(SeamRequest, Config, OutMeshData);
	}

	const int32 ChunkSize = SeamRequest.ChunkSize;
	const int32 LODLevel = FMath::Clamp(SeamRequest.LODLevel, 0, 7);
	const int32 Stride = 1 << LODLevel;
	if ((ChunkSize % Stride) != 0)
	{
		UE_LOG(LogVoxelMeshing, Error, TEXT("GenerateFaceSeamMeshCPU: ChunkSize %d not divisible by stride %d"), ChunkSize, Stride);
		return false;
	}
	const int32 GridSize = ChunkSize / Stride;
	if (GridSize < 3)
	{
		// No exclusively-owned face cells at this resolution (everything is edge/corner-seam
		// territory) — an empty seam is the correct output, not an error.
		return true;
	}

	const int32 U = SeamRequest.Axis;          // face normal axis
	const int32 V = (U + 1) % 3;               // transverse axes
	const int32 W = (U + 2) % 3;
	const int32 SL = GridSize - 1;             // slab cell layer (owner frame): == neighbour's -1 layer
	const int32 TCount = SL;                   // exclusive transverse cell range per axis: [0, SL)
	const float VoxelSize = SeamRequest.VoxelSize;
	const float IsoLevel = Config.IsoLevel;
	const float SVDThreshold = Config.QEFSVDThreshold;
	const float BiasStrength = Config.QEFBiasStrength;
	const float FrameOffsetU = static_cast<float>(ChunkSize) * VoxelSize;

	const FSeamSampler SamplerA(SeamRequest, FSeamSampler::EMode::AOnly);
	const FSeamSampler SamplerB(SeamRequest, FSeamSampler::EMode::BOnly);
	const FSeamSampler SamplerC(SeamRequest, FSeamSampler::EMode::Combined);

	// ---- Cell layers -------------------------------------------------------------------------
	// Layer 0 = ring A  (owner cell U == SL-1; A-only view; owner frame)      — interior ring dup
	// Layer 1 = slab    (owner cell U == SL;   combined view; owner frame)    — the seam proper
	// Layer 2 = ring B  (B-frame cell U == 0;  B-only view; shifted to owner) — interior ring dup
	// Transverse extent: cells (v, w) in [0, SL)^2 — the exclusive face region (cells at
	// transverse -1/SL belong to edge/corner seams).
	TArray<FSeamCellVertex> Layers[3];
	for (int32 L = 0; L < 3; ++L)
	{
		Layers[L].SetNum(TCount * TCount);
	}

	for (int32 w = 0; w < TCount; ++w)
	{
		for (int32 v = 0; v < TCount; ++v)
		{
			const int32 Idx = v + w * TCount;

			int32 C[3];
			C[V] = v;
			C[W] = w;

			// Ring A: recompute exactly as A's Interior pass did (A data, Air clamp, A frame).
			C[U] = SL - 1;
			ComputeCellVertex(SamplerA, C[0], C[1], C[2], Stride, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Layers[0][Idx]);

			// Slab: both sides' data, full hermite reach across the face (the seam's whole point).
			C[U] = SL;
			ComputeCellVertex(SamplerC, C[0], C[1], C[2], Stride, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Layers[1][Idx]);

			// Ring B: recompute exactly as B's Interior pass did — in B's OWN frame (bit-identical
			// solve), then translate the result into the owner frame by one chunk along the axis.
			C[U] = 0;
			if (ComputeCellVertex(SamplerB, C[0], C[1], C[2], Stride, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Layers[2][Idx]))
			{
				Layers[2][Idx].Position[U] += FrameOffsetU;
			}
		}
	}

	// ---- Vertex emission (replicates EmitVertex: triplanar UV + UV1/color material carry) -----
	const float UVScale = Config.bGenerateUVs ? Config.UVScale : 0.0f;
	auto EmitSeamVertex = [&OutMeshData, VoxelSize, UVScale](const FSeamCellVertex& Vertex, uint8 MaterialID, uint8 BiomeID, uint32 BlendCode) -> int32
	{
		const int32 Index = OutMeshData.Positions.Num();
		OutMeshData.Positions.Add(Vertex.Position);
		OutMeshData.Normals.Add(Vertex.Normal);

		const float AbsX = FMath::Abs(Vertex.Normal.X);
		const float AbsY = FMath::Abs(Vertex.Normal.Y);
		const float AbsZ = FMath::Abs(Vertex.Normal.Z);
		FVector2f UV;
		if (AbsZ >= AbsX && AbsZ >= AbsY)
		{
			UV = FVector2f(Vertex.Position.X * UVScale / VoxelSize, Vertex.Position.Y * UVScale / VoxelSize);
		}
		else if (AbsX >= AbsY)
		{
			UV = FVector2f(Vertex.Position.Y * UVScale / VoxelSize, Vertex.Position.Z * UVScale / VoxelSize);
		}
		else
		{
			UV = FVector2f(Vertex.Position.X * UVScale / VoxelSize, Vertex.Position.Z * UVScale / VoxelSize);
		}
		OutMeshData.UVs.Add(UV);
		OutMeshData.UV1s.Add(FVector2f(static_cast<float>(MaterialID), 0.0f));
		OutMeshData.Colors.Add(FColor(MaterialID, BiomeID, 0, 255));
		OutMeshData.MaterialBlends.Add(BlendCode);
		return Index;
	};

	// Owner-frame cell U-layer -> layer array index (or -1 = not a seam-tracked layer).
	auto LayerOf = [SL](int32 CU) -> int32
	{
		return (CU == SL - 1) ? 0 : (CU == SL) ? 1 : (CU == SL + 1) ? 2 : -1;
	};

	TMap<uint64, int32> DuplicateVertexCache;
	uint32 TriangleCount = 0;

	// Emit the quad dual to one owned crossing edge. Mirrors GenerateQuads' flow: winding from
	// the edge-base density sign, per-quad material from the solid endpoint, per-cell/-material
	// vertex dedup, and the adaptive max-min-area diagonal split (the DT7 T-junction fix).
	auto EmitQuadForEdge = [&](const int32 EdgeCell[3], int32 EdgeAxis) -> void
	{
		const int32 VX = EdgeCell[0] * Stride;
		const int32 VY = EdgeCell[1] * Stride;
		const int32 VZ = EdgeCell[2] * Stride;

		const float D0 = SamplerC.GetDensity(VX, VY, VZ);
		int32 NX = VX, NY = VY, NZ = VZ;
		if (EdgeAxis == 0) NX += Stride;
		else if (EdgeAxis == 1) NY += Stride;
		else NZ += Stride;
		const float D1 = SamplerC.GetDensity(NX, NY, NZ);

		const bool bSolid0 = (D0 >= IsoLevel);
		const bool bSolid1 = (D1 >= IsoLevel);
		if (bSolid0 == bSolid1)
		{
			return; // no crossing on this edge
		}

		// Look up the 4 surrounding cell vertices across the three layers.
		FSeamCellVertex* Verts[4];
		int32 CellKeys[4];
		for (int32 i = 0; i < 4; ++i)
		{
			const FCellOffset& Off = GAxisOffsets[EdgeAxis][i];
			const int32 CellC[3] = { EdgeCell[0] + Off.DX, EdgeCell[1] + Off.DY, EdgeCell[2] + Off.DZ };
			const int32 Layer = LayerOf(CellC[U]);
			const int32 TV = CellC[V];
			const int32 TW = CellC[W];
			if (Layer < 0 || TV < 0 || TV >= TCount || TW < 0 || TW >= TCount)
			{
				return; // touches a cell outside the exclusive face region (P2 territory)
			}
			FSeamCellVertex& Cell = Layers[Layer][TV + TW * TCount];
			if (!Cell.bValid)
			{
				return; // matches the interior pass's all-cells-valid quad rule
			}
			Verts[i] = &Cell;
			CellKeys[i] = Layer * TCount * TCount + (TV + TW * TCount);
		}

		const bool bFlip = (D0 < IsoLevel);

		// Per-quad material from the solid endpoint (triangles must stay material-uniform).
		int32 SolidX = VX, SolidY = VY, SolidZ = VZ;
		if (bFlip)
		{
			if (EdgeAxis == 0) { SolidX += Stride; }
			else if (EdgeAxis == 1) { SolidY += Stride; }
			else { SolidZ += Stride; }
		}
		const FVoxelData SolidVoxel = SamplerC.GetVoxel(SolidX, SolidY, SolidZ);
		const uint8 QuadMaterial = SolidVoxel.MaterialID;
		const uint8 QuadBiome = SolidVoxel.BiomeID;

		int32 Indices[4];
		// The quad's material and its blend toward a neighbouring cell's material (VP2-O4); the interior pass's emission
		VoxelDCMaterialBlend::EmitQuadCells(Verts, CellKeys, 4, QuadMaterial, QuadBiome, DuplicateVertexCache,
			[&](const FSeamCellVertex& EmitCell, uint8 EmitMaterial, uint8 EmitBiome, uint32 EmitBlend)
			{
				return EmitSeamVertex(EmitCell, EmitMaterial, EmitBiome, EmitBlend);
			},
			Indices);

		// Adaptive max-min-area diagonal (mirrors GenerateQuads; preserves the DT7 sliver fix).
		auto TriArea = [](const FVector3f& A, const FVector3f& B, const FVector3f& C) -> float
		{
			return 0.5f * FVector3f::CrossProduct(B - A, C - A).Size();
		};
		const FVector3f& P0 = Verts[0]->Position;
		const FVector3f& P1 = Verts[1]->Position;
		const FVector3f& P2 = Verts[2]->Position;
		const FVector3f& P3 = Verts[3]->Position;
		const float MinArea02 = FMath::Min(TriArea(P0, P1, P2), TriArea(P0, P2, P3));
		const float MinArea13 = FMath::Min(TriArea(P1, P2, P3), TriArea(P1, P3, P0));
		const bool bUse13 = MinArea13 > MinArea02;

		auto AddTri = [&](int32 a, int32 b, int32 c)
		{
			OutMeshData.Indices.Add(Indices[a]);
			OutMeshData.Indices.Add(Indices[b]);
			OutMeshData.Indices.Add(Indices[c]);
		};

		if (bFlip)
		{
			if (bUse13) { AddTri(1, 2, 3); AddTri(1, 3, 0); }
			else        { AddTri(0, 1, 2); AddTri(0, 2, 3); }
		}
		else
		{
			if (bUse13) { AddTri(1, 3, 2); AddTri(1, 0, 3); }
			else        { AddTri(0, 2, 1); AddTri(0, 3, 2); }
		}

		TriangleCount += 2;
	};

	// ---- Owned edges -------------------------------------------------------------------------
	// The seam owns exactly the edges whose surrounding cells include >= 1 slab cell and lie
	// entirely within the exclusive face region. In the owner frame (SL = GridSize-1):
	//   group 1: U-axis edges at cell U == SL       (4 slab cells)          v,w in [1, SL-1]
	//   group 2: V-axis edges at cell U == SL       (slab + ring-A cells)   v in [0, SL-1], w in [1, SL-1]
	//   group 3: W-axis edges at cell U == SL       (slab + ring-A cells)   v in [1, SL-1], w in [0, SL-1]
	//   group 4: V-axis edges at cell U == SL + 1   (ring-B + slab cells)   v in [0, SL-1], w in [1, SL-1]
	//   group 5: W-axis edges at cell U == SL + 1   (ring-B + slab cells)   v in [1, SL-1], w in [0, SL-1]
	// Everything else is owned by an interior pass (all four cells interior to one chunk) or by
	// an edge/corner seam (cells spanning two+ slabs). This partition is exactly-once.
	int32 EdgeCell[3];

	// Group 1 — face-crossing edges.
	EdgeCell[U] = SL;
	for (int32 w = 1; w <= SL - 1; ++w)
	{
		for (int32 v = 1; v <= SL - 1; ++v)
		{
			EdgeCell[V] = v;
			EdgeCell[W] = w;
			EmitQuadForEdge(EdgeCell, U);
		}
	}

	// Groups 2-5 — stitching edges on both sides of the slab.
	for (int32 Side = 0; Side < 2; ++Side)
	{
		EdgeCell[U] = SL + Side; // SL, then SL+1

		// V-axis edges (groups 2 / 4)
		for (int32 w = 1; w <= SL - 1; ++w)
		{
			for (int32 v = 0; v <= SL - 1; ++v)
			{
				EdgeCell[V] = v;
				EdgeCell[W] = w;
				EmitQuadForEdge(EdgeCell, V);
			}
		}

		// W-axis edges (groups 3 / 5)
		for (int32 w = 0; w <= SL - 1; ++w)
		{
			for (int32 v = 1; v <= SL - 1; ++v)
			{
				EdgeCell[V] = v;
				EdgeCell[W] = w;
				EmitQuadForEdge(EdgeCell, W);
			}
		}
	}

	UE_LOG(LogVoxelMeshing, Verbose,
		TEXT("DC face seam %s axis=%d LOD=%d: %d verts, %d tris"),
		*SeamRequest.OwnerChunkCoord.ToString(), U, LODLevel,
		OutMeshData.Positions.Num(), TriangleCount);

	return true;
}

bool FVoxelCPUDualContourMesher::GenerateEdgeSeamMeshCPU(
	const FVoxelEdgeSeamRequest& SeamRequest,
	FChunkMeshData& OutMeshData)
{
	using namespace VoxelDCFaceSeam;

	OutMeshData.Reset();

	if (!bIsInitialized)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("GenerateEdgeSeamMeshCPU called before Initialize"));
		return false;
	}
	if (!SeamRequest.IsValid())
	{
		UE_LOG(LogVoxelMeshing, Error, TEXT("GenerateEdgeSeamMeshCPU: invalid seam request (axis=%d, chunkSize=%d)"),
			SeamRequest.EdgeAxis, SeamRequest.ChunkSize);
		return false;
	}

	// Mixed-LOD tuple: dedicated path — the same-LOD path below stays
	// bit-frozen (its ring identities are asserted by the ES tests).
	if (!SeamRequest.IsUniformLOD())
	{
		return GenerateMixedEdgeSeam(SeamRequest, Config, OutMeshData);
	}

	const int32 ChunkSize = SeamRequest.ChunkSize;
	const int32 LODLevel = FMath::Clamp(SeamRequest.LODLevel, 0, 7);
	const int32 Stride = 1 << LODLevel;
	if ((ChunkSize % Stride) != 0)
	{
		UE_LOG(LogVoxelMeshing, Error, TEXT("GenerateEdgeSeamMeshCPU: ChunkSize %d not divisible by stride %d"), ChunkSize, Stride);
		return false;
	}
	const int32 GridSize = ChunkSize / Stride;
	if (GridSize < 3)
	{
		return true; // no exclusively-owned edge cells at this resolution — valid empty seam
	}

	const int32 U = SeamRequest.EdgeAxis;                    // edge-parallel axis
	const int32 PerpA = (U == 0) ? 1 : 0;                    // perpendicular axes, ASCENDING
	const int32 PerpB = (U == 2) ? 1 : 2;                    // (matches registry participant order)
	const int32 SL = GridSize - 1;
	const int32 UCount = SL;                                 // edge-exclusive cells: u in [0, SL)
	const float VoxelSize = SeamRequest.VoxelSize;
	const float IsoLevel = Config.IsoLevel;
	const float SVDThreshold = Config.QEFSVDThreshold;
	const float BiasStrength = Config.QEFBiasStrength;
	const float ChunkWorldSpan = static_cast<float>(ChunkSize) * VoxelSize;

	// ---- The 9 cell columns around the edge (owner-frame transverse offsets dcv/dcw in [-1,1]).
	// Column (0,0) is the edge-exclusive column (all four quadrants visible, owner frame). The 8
	// ring columns recompute EXACTLY what their original jobs computed: participant interiors
	// (single quadrant, that participant's frame) and face slabs (the face job's pair mask, the
	// face OWNER's frame). Frame rule: a column computes in the quadrant frame of its minimum
	// participating side per axis — dc == +1 selects the far frame, else the owner-side frame.
	struct FColumnConfig
	{
		uint8 Mask = 0;
		FIntVector FrameOffsetVoxels = FIntVector::ZeroValue;
		int32 FrameCellA = 0;   // frame-local cell coord along PerpA
		int32 FrameCellB = 0;   // frame-local cell coord along PerpB
		float TranslateA = 0.f; // owner-frame position translation along PerpA
		float TranslateB = 0.f;
	};
	FColumnConfig Columns[9];
	TArray<FSeamCellVertex> Layers[9];

	auto ColumnIndex = [](int32 Dcv, int32 Dcw) { return (Dcv + 1) + (Dcw + 1) * 3; };

	for (int32 Dcw = -1; Dcw <= 1; ++Dcw)
	{
		for (int32 Dcv = -1; Dcv <= 1; ++Dcv)
		{
			FColumnConfig& Col = Columns[ColumnIndex(Dcv, Dcw)];

			// Per-axis visibility: 0 = pair (slab straddles), -1 = near quadrant only, +1 = far only.
			const int32 DvBitsAllowed[2] = { (Dcv <= 0) ? 1 : 0, (Dcv >= 0) ? 1 : 0 }; // [dv=0 allowed, dv=1 allowed]
			const int32 DwBitsAllowed[2] = { (Dcw <= 0) ? 1 : 0, (Dcw >= 0) ? 1 : 0 };
			for (int32 dw = 0; dw <= 1; ++dw)
			{
				for (int32 dv = 0; dv <= 1; ++dv)
				{
					if (DvBitsAllowed[dv] && DwBitsAllowed[dw])
					{
						Col.Mask |= (1 << (dv + dw * 2));
					}
				}
			}

			const int32 FrameDv = (Dcv == 1) ? 1 : 0;
			const int32 FrameDw = (Dcw == 1) ? 1 : 0;
			Col.FrameOffsetVoxels = FIntVector::ZeroValue;
			Col.FrameOffsetVoxels[PerpA] = FrameDv * ChunkSize;
			Col.FrameOffsetVoxels[PerpB] = FrameDw * ChunkSize;
			Col.FrameCellA = (Dcv == 1) ? 0 : (SL + Dcv); // -1 -> SL-1, 0 -> SL, +1 -> 0 (far frame)
			Col.FrameCellB = (Dcw == 1) ? 0 : (SL + Dcw);
			Col.TranslateA = FrameDv * ChunkWorldSpan;
			Col.TranslateB = FrameDw * ChunkWorldSpan;

			Layers[ColumnIndex(Dcv, Dcw)].SetNum(UCount);

			const FQuadSampler Sampler{ SeamRequest, U, PerpA, PerpB, Col.Mask, Col.FrameOffsetVoxels };
			for (int32 u = 0; u < UCount; ++u)
			{
				int32 C[3];
				C[U] = u;
				C[PerpA] = Col.FrameCellA;
				C[PerpB] = Col.FrameCellB;
				FSeamCellVertex& Cell = Layers[ColumnIndex(Dcv, Dcw)][u];
				if (ComputeCellVertex(Sampler, C[0], C[1], C[2], Stride, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Cell))
				{
					Cell.Position[PerpA] += Col.TranslateA;
					Cell.Position[PerpB] += Col.TranslateB;
				}
			}
		}
	}

	// ---- Vertex emission (identical to the face-seam path) ---------------------------------
	const float UVScale = Config.bGenerateUVs ? Config.UVScale : 0.0f;
	auto EmitSeamVertex = [&OutMeshData, VoxelSize, UVScale](const FSeamCellVertex& Vertex, uint8 MaterialID, uint8 BiomeID, uint32 BlendCode) -> int32
	{
		const int32 Index = OutMeshData.Positions.Num();
		OutMeshData.Positions.Add(Vertex.Position);
		OutMeshData.Normals.Add(Vertex.Normal);

		const float AbsX = FMath::Abs(Vertex.Normal.X);
		const float AbsY = FMath::Abs(Vertex.Normal.Y);
		const float AbsZ = FMath::Abs(Vertex.Normal.Z);
		FVector2f UV;
		if (AbsZ >= AbsX && AbsZ >= AbsY)
		{
			UV = FVector2f(Vertex.Position.X * UVScale / VoxelSize, Vertex.Position.Y * UVScale / VoxelSize);
		}
		else if (AbsX >= AbsY)
		{
			UV = FVector2f(Vertex.Position.Y * UVScale / VoxelSize, Vertex.Position.Z * UVScale / VoxelSize);
		}
		else
		{
			UV = FVector2f(Vertex.Position.X * UVScale / VoxelSize, Vertex.Position.Z * UVScale / VoxelSize);
		}
		OutMeshData.UVs.Add(UV);
		OutMeshData.UV1s.Add(FVector2f(static_cast<float>(MaterialID), 0.0f));
		OutMeshData.Colors.Add(FColor(MaterialID, BiomeID, 0, 255));
		OutMeshData.MaterialBlends.Add(BlendCode);
		return Index;
	};

	// Full-quad owner-frame sampler: crossing existence, winding, and material for owned edges.
	const FQuadSampler FullSampler{ SeamRequest, U, PerpA, PerpB, 0b1111, FIntVector::ZeroValue };

	TMap<uint64, int32> DuplicateVertexCache;
	uint32 TriangleCount = 0;

	auto EmitQuadForEdge = [&](const int32 EdgeCell[3], int32 EdgeAxis) -> void
	{
		const int32 VX = EdgeCell[0] * Stride;
		const int32 VY = EdgeCell[1] * Stride;
		const int32 VZ = EdgeCell[2] * Stride;

		const float D0 = FullSampler.GetDensity(VX, VY, VZ);
		int32 NX = VX, NY = VY, NZ = VZ;
		if (EdgeAxis == 0) NX += Stride;
		else if (EdgeAxis == 1) NY += Stride;
		else NZ += Stride;
		const float D1 = FullSampler.GetDensity(NX, NY, NZ);

		const bool bSolid0 = (D0 >= IsoLevel);
		const bool bSolid1 = (D1 >= IsoLevel);
		if (bSolid0 == bSolid1)
		{
			return;
		}

		FSeamCellVertex* Verts[4];
		int32 CellKeys[4];
		for (int32 i = 0; i < 4; ++i)
		{
			const FCellOffset& Off = GAxisOffsets[EdgeAxis][i];
			const int32 CellC[3] = { EdgeCell[0] + Off.DX, EdgeCell[1] + Off.DY, EdgeCell[2] + Off.DZ };
			const int32 Dcv = CellC[PerpA] - SL;
			const int32 Dcw = CellC[PerpB] - SL;
			const int32 CU = CellC[U];
			if (Dcv < -1 || Dcv > 1 || Dcw < -1 || Dcw > 1 || CU < 0 || CU >= UCount)
			{
				return; // outside the edge-seam region (interior / face / corner territory)
			}
			const int32 Layer = ColumnIndex(Dcv, Dcw);
			FSeamCellVertex& Cell = Layers[Layer][CU];
			if (!Cell.bValid)
			{
				return;
			}
			Verts[i] = &Cell;
			CellKeys[i] = Layer * UCount + CU;
		}

		const bool bFlip = (D0 < IsoLevel);

		int32 SolidX = VX, SolidY = VY, SolidZ = VZ;
		if (bFlip)
		{
			if (EdgeAxis == 0) { SolidX += Stride; }
			else if (EdgeAxis == 1) { SolidY += Stride; }
			else { SolidZ += Stride; }
		}
		const FVoxelData SolidVoxel = FullSampler.GetVoxel(SolidX, SolidY, SolidZ);
		const uint8 QuadMaterial = SolidVoxel.MaterialID;
		const uint8 QuadBiome = SolidVoxel.BiomeID;

		int32 Indices[4];
		// The quad's material and its blend toward a neighbouring cell's material (VP2-O4); the interior pass's emission
		VoxelDCMaterialBlend::EmitQuadCells(Verts, CellKeys, 4, QuadMaterial, QuadBiome, DuplicateVertexCache,
			[&](const FSeamCellVertex& EmitCell, uint8 EmitMaterial, uint8 EmitBiome, uint32 EmitBlend)
			{
				return EmitSeamVertex(EmitCell, EmitMaterial, EmitBiome, EmitBlend);
			},
			Indices);

		auto TriArea = [](const FVector3f& A, const FVector3f& B, const FVector3f& C) -> float
		{
			return 0.5f * FVector3f::CrossProduct(B - A, C - A).Size();
		};
		const FVector3f& P0 = Verts[0]->Position;
		const FVector3f& P1 = Verts[1]->Position;
		const FVector3f& P2 = Verts[2]->Position;
		const FVector3f& P3 = Verts[3]->Position;
		const float MinArea02 = FMath::Min(TriArea(P0, P1, P2), TriArea(P0, P2, P3));
		const float MinArea13 = FMath::Min(TriArea(P1, P2, P3), TriArea(P1, P3, P0));
		const bool bUse13 = MinArea13 > MinArea02;

		auto AddTri = [&](int32 a, int32 b, int32 c)
		{
			OutMeshData.Indices.Add(Indices[a]);
			OutMeshData.Indices.Add(Indices[b]);
			OutMeshData.Indices.Add(Indices[c]);
		};

		if (bFlip)
		{
			if (bUse13) { AddTri(1, 2, 3); AddTri(1, 3, 0); }
			else        { AddTri(0, 1, 2); AddTri(0, 2, 3); }
		}
		else
		{
			if (bUse13) { AddTri(1, 3, 2); AddTri(1, 0, 3); }
			else        { AddTri(0, 2, 1); AddTri(0, 3, 2); }
		}

		TriangleCount += 2;
	};

	// ---- Owned edges (exactly-once partition, owner frame) ---------------------------------
	// A DC edge belongs to the edge seam iff its 4 cells include >= 1 edge-column cell and no
	// corner cell (u outside [0, SL)). Enumerated directly:
	//   U-axis edges     at (u, cv, cw), cv,cw in {SL, SL+1}, u in [0, SL)
	//   PerpA-axis edges at (u, SL, cw), cw in {SL, SL+1},    u in [1, SL-1]
	//   PerpB-axis edges at (u, cv, SL), cv in {SL, SL+1},    u in [1, SL-1]
	int32 EdgeCell[3];

	for (int32 cw = SL; cw <= SL + 1; ++cw)
	{
		for (int32 cv = SL; cv <= SL + 1; ++cv)
		{
			for (int32 u = 0; u < SL; ++u)
			{
				EdgeCell[U] = u;
				EdgeCell[PerpA] = cv;
				EdgeCell[PerpB] = cw;
				EmitQuadForEdge(EdgeCell, U);
			}
		}
	}
	for (int32 Sel = SL; Sel <= SL + 1; ++Sel)
	{
		for (int32 u = 1; u <= SL - 1; ++u)
		{
			EdgeCell[U] = u;
			EdgeCell[PerpA] = SL;
			EdgeCell[PerpB] = Sel;
			EmitQuadForEdge(EdgeCell, PerpA);

			EdgeCell[PerpA] = Sel;
			EdgeCell[PerpB] = SL;
			EmitQuadForEdge(EdgeCell, PerpB);
		}
	}

	UE_LOG(LogVoxelMeshing, Verbose,
		TEXT("DC edge seam %s axis=%d LOD=%d: %d verts, %d tris"),
		*SeamRequest.OwnerChunkCoord.ToString(), U, LODLevel,
		OutMeshData.Positions.Num(), TriangleCount);

	return true;
}

bool FVoxelCPUDualContourMesher::GenerateCornerSeamMeshCPU(
	const FVoxelCornerSeamRequest& SeamRequest,
	FChunkMeshData& OutMeshData)
{
	using namespace VoxelDCFaceSeam;

	OutMeshData.Reset();

	if (!bIsInitialized)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("GenerateCornerSeamMeshCPU called before Initialize"));
		return false;
	}
	if (!SeamRequest.IsValid())
	{
		UE_LOG(LogVoxelMeshing, Error, TEXT("GenerateCornerSeamMeshCPU: invalid seam request (chunkSize=%d)"),
			SeamRequest.ChunkSize);
		return false;
	}

	// Mixed-LOD tuple: dedicated path — the same-LOD path below stays
	// bit-frozen (its ring identities are asserted by the CS tests).
	if (!SeamRequest.IsUniformLOD())
	{
		return GenerateMixedCornerSeam(SeamRequest, Config, OutMeshData);
	}

	const int32 ChunkSize = SeamRequest.ChunkSize;
	const int32 LODLevel = FMath::Clamp(SeamRequest.LODLevel, 0, 7);
	const int32 Stride = 1 << LODLevel;
	if ((ChunkSize % Stride) != 0)
	{
		UE_LOG(LogVoxelMeshing, Error, TEXT("GenerateCornerSeamMeshCPU: ChunkSize %d not divisible by stride %d"), ChunkSize, Stride);
		return false;
	}
	const int32 GridSize = ChunkSize / Stride;
	if (GridSize < 3)
	{
		return true; // degenerate resolution — valid empty seam
	}

	const int32 SL = GridSize - 1;
	const float VoxelSize = SeamRequest.VoxelSize;
	const float IsoLevel = Config.IsoLevel;
	const float SVDThreshold = Config.QEFSVDThreshold;
	const float BiasStrength = Config.QEFBiasStrength;
	const float ChunkWorldSpan = static_cast<float>(ChunkSize) * VoxelSize;

	// ---- The 3x3x3 cell block around the corner cell (per-axis state s in {-1,0,+1} relative
	// to SL). One unified rule generates every original job's exact configuration:
	//   mask:  s == 0 -> both halves visible on that axis; s == -1 -> lower only; +1 -> upper only
	//   frame: s == +1 computes in the far chunk's frame on that axis (offset +ChunkSize)
	//   cell:  frame-local coord = (s == +1) ? 0 : SL + s
	// This reproduces: the corner cell itself (all 8 octants, owner frame), the six edge-column
	// end cells (4-octant masks == the edge jobs' quad samplers), the twelve face-slab corner
	// cells (2-octant masks == the face jobs' pair samplers), and the participant-interior
	// corner cells (single octants == the Interior passes). Cells whose state has no zero
	// component are never referenced by an owned quad; they are computed harmlessly.
	FSeamCellVertex Cells[27];
	auto CellIndexOf = [](int32 SX, int32 SY, int32 SZ) { return (SX + 1) + (SY + 1) * 3 + (SZ + 1) * 9; };

	for (int32 SZ = -1; SZ <= 1; ++SZ)
	{
		for (int32 SY = -1; SY <= 1; ++SY)
		{
			for (int32 SX = -1; SX <= 1; ++SX)
			{
				const int32 S[3] = { SX, SY, SZ };
				uint8 Mask = 0;
				for (int32 Oct = 0; Oct < 8; ++Oct)
				{
					const int32 D[3] = { Oct & 1, (Oct >> 1) & 1, (Oct >> 2) & 1 };
					bool bAllowed = true;
					for (int32 Axis = 0; Axis < 3; ++Axis)
					{
						if ((S[Axis] == -1 && D[Axis] != 0) || (S[Axis] == 1 && D[Axis] != 1))
						{
							bAllowed = false;
							break;
						}
					}
					if (bAllowed)
					{
						Mask |= (1 << Oct);
					}
				}

				FIntVector FrameOffset(
					(SX == 1) ? ChunkSize : 0,
					(SY == 1) ? ChunkSize : 0,
					(SZ == 1) ? ChunkSize : 0);
				const int32 CellX = (SX == 1) ? 0 : (SL + SX);
				const int32 CellY = (SY == 1) ? 0 : (SL + SY);
				const int32 CellZ = (SZ == 1) ? 0 : (SL + SZ);

				const FOctSampler Sampler{ SeamRequest, Mask, FrameOffset };
				FSeamCellVertex& Cell = Cells[CellIndexOf(SX, SY, SZ)];
				if (ComputeCellVertex(Sampler, CellX, CellY, CellZ, Stride, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Cell))
				{
					Cell.Position.X += (SX == 1) ? ChunkWorldSpan : 0.0f;
					Cell.Position.Y += (SY == 1) ? ChunkWorldSpan : 0.0f;
					Cell.Position.Z += (SZ == 1) ? ChunkWorldSpan : 0.0f;
				}
			}
		}
	}

	// ---- Vertex emission (identical to the face/edge seam paths) ---------------------------
	const float UVScale = Config.bGenerateUVs ? Config.UVScale : 0.0f;
	auto EmitSeamVertex = [&OutMeshData, VoxelSize, UVScale](const FSeamCellVertex& Vertex, uint8 MaterialID, uint8 BiomeID, uint32 BlendCode) -> int32
	{
		const int32 Index = OutMeshData.Positions.Num();
		OutMeshData.Positions.Add(Vertex.Position);
		OutMeshData.Normals.Add(Vertex.Normal);

		const float AbsX = FMath::Abs(Vertex.Normal.X);
		const float AbsY = FMath::Abs(Vertex.Normal.Y);
		const float AbsZ = FMath::Abs(Vertex.Normal.Z);
		FVector2f UV;
		if (AbsZ >= AbsX && AbsZ >= AbsY)
		{
			UV = FVector2f(Vertex.Position.X * UVScale / VoxelSize, Vertex.Position.Y * UVScale / VoxelSize);
		}
		else if (AbsX >= AbsY)
		{
			UV = FVector2f(Vertex.Position.Y * UVScale / VoxelSize, Vertex.Position.Z * UVScale / VoxelSize);
		}
		else
		{
			UV = FVector2f(Vertex.Position.X * UVScale / VoxelSize, Vertex.Position.Z * UVScale / VoxelSize);
		}
		OutMeshData.UVs.Add(UV);
		OutMeshData.UV1s.Add(FVector2f(static_cast<float>(MaterialID), 0.0f));
		OutMeshData.Colors.Add(FColor(MaterialID, BiomeID, 0, 255));
		OutMeshData.MaterialBlends.Add(BlendCode);
		return Index;
	};

	const FOctSampler FullSampler{ SeamRequest, 0xFF, FIntVector::ZeroValue };

	TMap<uint64, int32> DuplicateVertexCache;
	uint32 TriangleCount = 0;

	auto EmitQuadForEdge = [&](const int32 EdgeCell[3], int32 EdgeAxis) -> void
	{
		const int32 VX = EdgeCell[0] * Stride;
		const int32 VY = EdgeCell[1] * Stride;
		const int32 VZ = EdgeCell[2] * Stride;

		const float D0 = FullSampler.GetDensity(VX, VY, VZ);
		int32 NX = VX, NY = VY, NZ = VZ;
		if (EdgeAxis == 0) NX += Stride;
		else if (EdgeAxis == 1) NY += Stride;
		else NZ += Stride;
		const float D1 = FullSampler.GetDensity(NX, NY, NZ);

		const bool bSolid0 = (D0 >= IsoLevel);
		const bool bSolid1 = (D1 >= IsoLevel);
		if (bSolid0 == bSolid1)
		{
			return;
		}

		FSeamCellVertex* Verts[4];
		int32 CellKeys[4];
		for (int32 i = 0; i < 4; ++i)
		{
			const FCellOffset& Off = GAxisOffsets[EdgeAxis][i];
			const int32 SX = (EdgeCell[0] + Off.DX) - SL;
			const int32 SY = (EdgeCell[1] + Off.DY) - SL;
			const int32 SZ = (EdgeCell[2] + Off.DZ) - SL;
			if (SX < -1 || SX > 1 || SY < -1 || SY > 1 || SZ < -1 || SZ > 1)
			{
				return; // outside the corner block (owned elsewhere)
			}
			const int32 Idx = CellIndexOf(SX, SY, SZ);
			FSeamCellVertex& Cell = Cells[Idx];
			if (!Cell.bValid)
			{
				return;
			}
			Verts[i] = &Cell;
			CellKeys[i] = Idx;
		}

		const bool bFlip = (D0 < IsoLevel);

		int32 SolidX = VX, SolidY = VY, SolidZ = VZ;
		if (bFlip)
		{
			if (EdgeAxis == 0) { SolidX += Stride; }
			else if (EdgeAxis == 1) { SolidY += Stride; }
			else { SolidZ += Stride; }
		}
		const FVoxelData SolidVoxel = FullSampler.GetVoxel(SolidX, SolidY, SolidZ);
		const uint8 QuadMaterial = SolidVoxel.MaterialID;
		const uint8 QuadBiome = SolidVoxel.BiomeID;

		int32 Indices[4];
		// The quad's material and its blend toward a neighbouring cell's material (VP2-O4); the interior pass's emission
		VoxelDCMaterialBlend::EmitQuadCells(Verts, CellKeys, 4, QuadMaterial, QuadBiome, DuplicateVertexCache,
			[&](const FSeamCellVertex& EmitCell, uint8 EmitMaterial, uint8 EmitBiome, uint32 EmitBlend)
			{
				return EmitSeamVertex(EmitCell, EmitMaterial, EmitBiome, EmitBlend);
			},
			Indices);

		auto TriArea = [](const FVector3f& A, const FVector3f& B, const FVector3f& C) -> float
		{
			return 0.5f * FVector3f::CrossProduct(B - A, C - A).Size();
		};
		const FVector3f& P0 = Verts[0]->Position;
		const FVector3f& P1 = Verts[1]->Position;
		const FVector3f& P2 = Verts[2]->Position;
		const FVector3f& P3 = Verts[3]->Position;
		const float MinArea02 = FMath::Min(TriArea(P0, P1, P2), TriArea(P0, P2, P3));
		const float MinArea13 = FMath::Min(TriArea(P1, P2, P3), TriArea(P1, P3, P0));
		const bool bUse13 = MinArea13 > MinArea02;

		auto AddTri = [&](int32 a, int32 b, int32 c)
		{
			OutMeshData.Indices.Add(Indices[a]);
			OutMeshData.Indices.Add(Indices[b]);
			OutMeshData.Indices.Add(Indices[c]);
		};

		if (bFlip)
		{
			if (bUse13) { AddTri(1, 2, 3); AddTri(1, 3, 0); }
			else        { AddTri(0, 1, 2); AddTri(0, 2, 3); }
		}
		else
		{
			if (bUse13) { AddTri(1, 3, 2); AddTri(1, 0, 3); }
			else        { AddTri(0, 2, 1); AddTri(0, 3, 2); }
		}

		TriangleCount += 2;
	};

	// ---- Owned edges: the 12 DC edges whose surrounding cells include the corner cell — for
	// each axis, the 2x2 fan at the corner (exactly-once: face jobs exclude multi-slab cells,
	// edge jobs exclude cells beyond their column's [0, SL) extent).
	for (int32 EdgeAxis = 0; EdgeAxis < 3; ++EdgeAxis)
	{
		const int32 A1 = (EdgeAxis + 1) % 3;
		const int32 A2 = (EdgeAxis + 2) % 3;
		for (int32 VB = SL; VB <= SL + 1; ++VB)
		{
			for (int32 VA = SL; VA <= SL + 1; ++VA)
			{
				int32 EdgeCell[3];
				EdgeCell[EdgeAxis] = SL;
				EdgeCell[A1] = VA;
				EdgeCell[A2] = VB;
				EmitQuadForEdge(EdgeCell, EdgeAxis);
			}
		}
	}

	UE_LOG(LogVoxelMeshing, Verbose,
		TEXT("DC corner seam %s LOD=%d: %d verts, %d tris"),
		*SeamRequest.OwnerChunkCoord.ToString(), LODLevel,
		OutMeshData.Positions.Num(), TriangleCount);

	return true;
}

// ============================================================================
// Configuration
