// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCrossingGradient.h"
#include "ChunkRenderData.h"

// What the Dual Contouring mesher's interior pass (VoxelCPUDualContourMesher.cpp) and its seam meshers
// (VoxelCPUDualContourSeams.cpp) share: the gradient and shading normals, the cell's edge table, and the soft material
// borders (each cell's own material, a quad's blend and the vertex emission that carries it). Private to VoxelMeshing.

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

namespace VoxelDCMaterialBlend
{
	/** The most cells one quad or fan of the meshers emits */
	inline constexpr int32 MaxQuadCells = 4;

	/** A cube cell's corner (0-7) from its offset from the cell's min corner: X in bit 0, Y in bit 1, Z in bit 2 */
	FORCEINLINE int32 CornerIndex(int32 DX, int32 DY, int32 DZ)
	{
		return DX | (DY << 1) | (DZ << 2);
	}

	/**
	 * A cell's own material: the most common material among the solid endpoints of its sign-change edges, each corner
	 * counted once, a tie to the corner added first (the edges go in VoxelDCTables::CellEdges order). These are the voxels
	 * the cell's surface bounds, the ones a quad takes its material from (its edge's solid endpoint), so a cell inside one
	 * material gets that material.
	 */
	struct FCellMaterialVote
	{
		uint8 CornerMask = 0;
		uint8 Materials[8] = {};
		int32 Count = 0;

		/** Count a solid corner's material (a corner already counted is skipped) */
		void Add(int32 Corner, uint8 Material)
		{
			const uint8 Bit = static_cast<uint8>(1 << Corner);
			if ((CornerMask & Bit) == 0)
			{
				CornerMask |= Bit;
				Materials[Count++] = Material;
			}
		}

		/** @return The cell's material (0 when no corner was added: a cell without a crossing has no vertex) */
		uint8 Result() const
		{
			uint8 Best = Count > 0 ? Materials[0] : 0;
			int32 BestVotes = 0;
			for (int32 i = 0; i < Count; ++i)
			{
				int32 Votes = 0;
				for (int32 j = 0; j < Count; ++j)
				{
					Votes += Materials[j] == Materials[i] ? 1 : 0;
				}
				if (Votes > BestVotes)
				{
					Best = Materials[i];
					BestVotes = Votes;
				}
			}
			return Best;
		}
	};

	/**
	 * The blend codes (VoxelMaterialBlend) of one quad or fan's vertices. The second material is the most common cell
	 * material that differs from the quad's, the third the most common of the rest (ties to the first such cell); a vertex
	 * whose cell has one of them shows it, the others the quad's own, and the weights interpolate across the triangles.
	 * Every vertex of the quad gets the same second and third materials, so the IDs stay flat per triangle. A vertex then
	 * shows its cell's material whichever quad it is emitted for, so the material changes smoothly from cell to cell
	 * across quads. A quad whose cells all have its material gets no blend (code 0); a fourth material (all four cells
	 * different from each other and from the quad's) shows the quad's.
	 *
	 * @param QuadMaterial The quad's material (its edge's solid endpoint)
	 * @param CellMaterials Each cell's own material
	 * @param N The cells (at most MaxQuadCells)
	 * @param OutCodes Each cell's blend code
	 */
	inline void QuadBlendCodes(uint8 QuadMaterial, const uint8* CellMaterials, int32 N, uint32* OutCodes)
	{
		// The most common cell material other than the quad's and Excluded (BestVotes 0: none)
		auto MostCommon = [&](int32 Excluded, int32& OutVotes) -> uint8
		{
			OutVotes = 0;
			uint8 Best = QuadMaterial;
			for (int32 i = 0; i < N; ++i)
			{
				if (CellMaterials[i] == QuadMaterial || CellMaterials[i] == Excluded)
				{
					continue;
				}
				int32 Votes = 0;
				for (int32 j = 0; j < N; ++j)
				{
					Votes += CellMaterials[j] == CellMaterials[i] ? 1 : 0;
				}
				if (Votes > OutVotes)
				{
					Best = CellMaterials[i];
					OutVotes = Votes;
				}
			}
			return Best;
		};
		int32 SecondVotes = 0;
		int32 ThirdVotes = 0;
		const uint8 Secondary = MostCommon(-1, SecondVotes);
		const uint8 Third = SecondVotes > 0 ? MostCommon(Secondary, ThirdVotes) : 0;
		for (int32 i = 0; i < N; ++i)
		{
			OutCodes[i] = SecondVotes > 0
				? VoxelMaterialBlend::Encode(Secondary, CellMaterials[i] == Secondary,
					ThirdVotes > 0, Third, ThirdVotes > 0 && CellMaterials[i] == Third)
				: 0;
		}
	}

	/**
	 * Emit one quad or fan's cell vertices with the quad's material and their blend codes, reusing a cell's vertex where
	 * it was emitted with the same material, biome and code, and otherwise a duplicate kept per (cell key, code, material,
	 * biome): the duplicate has the same position and normal, so a border cannot crack. The cache key packs the cell key
	 * (below 2^24), the code (24 bits), the material and the biome into 64 bits. The interior pass and every seam
	 * path share this, so a ring recompute emits the vertices the interior pass would.
	 *
	 * @param Cells The cells (FDCCellVertex or a seam cell: MeshVertexIndex, EmittedMaterialID, EmittedBiomeID,
	 *              EmittedBlend, CellMaterial)
	 * @param Keys A key per cell, unique within the cache's mesh (below 2^24)
	 * @param N The cells (at most MaxQuadCells)
	 * @param QuadMaterial, QuadBiome The quad's material and biome
	 * @param Cache The mesh's duplicate vertices
	 * @param Emit (Cell, MaterialID, BiomeID, BlendCode) -> the new vertex's index
	 * @param OutIndices Each cell's vertex index
	 */
	template <typename TCell, typename TEmitFn>
	void EmitQuadCells(TCell* const* Cells, const int32* Keys, int32 N, uint8 QuadMaterial, uint8 QuadBiome,
		TMap<uint64, int32>& Cache, const TEmitFn& Emit, int32* OutIndices)
	{
		uint8 CellMaterials[MaxQuadCells];
		for (int32 i = 0; i < N; ++i)
		{
			CellMaterials[i] = Cells[i]->CellMaterial;
		}
		uint32 Codes[MaxQuadCells];
		QuadBlendCodes(QuadMaterial, CellMaterials, N, Codes);

		for (int32 i = 0; i < N; ++i)
		{
			TCell& Cell = *Cells[i];
			const uint32 Code = Codes[i];
			if (Cell.MeshVertexIndex >= 0 && Cell.EmittedMaterialID == QuadMaterial && Cell.EmittedBiomeID == QuadBiome
				&& Cell.EmittedBlend == Code)
			{
				OutIndices[i] = Cell.MeshVertexIndex;
			}
			else if (Cell.MeshVertexIndex < 0)
			{
				OutIndices[i] = Emit(Cell, QuadMaterial, QuadBiome, Code);
				Cell.MeshVertexIndex = OutIndices[i];
				Cell.EmittedMaterialID = QuadMaterial;
				Cell.EmittedBiomeID = QuadBiome;
				Cell.EmittedBlend = Code;
			}
			else
			{
				checkSlow(Keys[i] >= 0 && Keys[i] < (1 << 24));
				const uint64 DupKey = (static_cast<uint64>(Keys[i]) << 40)
					| (static_cast<uint64>(Code & 0xFFFFFF) << 16)
					| (static_cast<uint64>(QuadMaterial) << 8)
					| static_cast<uint64>(QuadBiome);
				if (const int32* Found = Cache.Find(DupKey))
				{
					OutIndices[i] = *Found;
				}
				else
				{
					OutIndices[i] = Emit(Cell, QuadMaterial, QuadBiome, Code);
					Cache.Add(DupKey, OutIndices[i]);
				}
			}
		}
	}
}
