// Copyright Daniel Raquel. All Rights Reserved.

// FVoxelCPUNoiseGenerator's the post-readback passes (the water fill, plane and radial, and the underground classification; GPU twins in VoxelPostPasses.usf): a translation unit of the class.

#include "VoxelCPUNoiseGenerator.h"
#include "VoxelHash.h"
#include "VoxelGeneration.h"
#include "VoxelTerrainConditioning.h"
#include "InfinitePlaneWorldMode.h"
#include "IslandBowlWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelBiomeRegistry.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelCaveConfiguration.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelSeaSurface.h"
#include "VoxelDownLines.h"
#include "VoxelNoise.h"
#include "Async/Async.h"

// ==================== Water Fill Pass ====================

void FVoxelCPUNoiseGenerator::ApplyPostReadbackPasses(
	const FVoxelNoiseGenerationRequest& Request,
	TArray<FVoxelData>& OutVoxelData)
{
	// Same order as GenerateChunkCPU: water fill first (uses the per-voxel cave flags as barriers and
	// clears them), then underground classification.
	ApplyWaterFillPass(Request, OutVoxelData);
	ApplyUndergroundClassificationPass(Request, OutVoxelData);
}

void FVoxelCPUNoiseGenerator::ApplyWaterFillPass(
	const FVoxelNoiseGenerationRequest& Request,
	TArray<FVoxelData>& OutVoxelData)
{
	if (!Request.bEnableWaterLevel)
		return;

	// A planet's sea is the sphere of WaterRadius (SP-B8): the same three phases with the radius in place of the plane.
	if (Request.WorldMode == EWorldMode::SphericalPlanet)
	{
		ApplyRadialWaterFillPass(Request, OutVoxelData);
		return;
	}

	const int32 ChunkSize = Request.ChunkSize;
	const float VoxelSize = Request.VoxelSize;
	const FVector ChunkWorldPos = Request.GetChunkWorldPosition();
	const float WaterLevel = Request.WaterLevel;
	const int32 SliceSize = ChunkSize * ChunkSize;
	const int32 VolumeSize = SliceSize * ChunkSize;

	// === Phase 1: Column scan — seed water in open ocean ===
	// Cave-flagged voxels are treated as solid barriers so the column scan
	// doesn't flood through cave openings in the seabed.
	// A chunk the water level passes through scans each column from the level down. A chunk wholly under it scans from its
	// top layer (WaterZ clamps to ChunkSize - 1, so no openness check): a heightmap column is solid below its surface and
	// air above it but for caves, which are barriers here, so air at the top of the column is open sea. Until G129 such a
	// chunk had no water of its own and the chunk manager flooded it on the game thread when its mesh landed
	// (PropagateWaterFromNeighbors: a flood over the whole chunk, up to 17 ms a chunk under SmoothTerrain_Island's lakes).
	const bool bChunkAtOrUnderWaterLevel = WaterLevel >= ChunkWorldPos.Z;

	if (bChunkAtOrUnderWaterLevel)
	{
		const int32 WaterZ = FMath::Clamp(
			FMath::FloorToInt32((WaterLevel - ChunkWorldPos.Z) / VoxelSize),
			0, ChunkSize - 1);

		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				// When WaterZ < ChunkSize - 1, verify the column is open from
				// the chunk top down to the water level. Solid or cave-flagged
				// voxels above water level mean this column is underground.
				if (WaterZ < ChunkSize - 1)
				{
					bool bColumnOpen = true;
					for (int32 Z = ChunkSize - 1; Z > WaterZ; --Z)
					{
						int32 Index = X + Y * ChunkSize + Z * SliceSize;
						if (OutVoxelData[Index].IsSolid() || OutVoxelData[Index].HasCaveFlag())
						{
							bColumnOpen = false;
							break;
						}
					}
					if (!bColumnOpen)
						continue;
				}

				// Scan from water level downward: flag air, stop at solid or cave barrier
				for (int32 Z = WaterZ; Z >= 0; --Z)
				{
					int32 Index = X + Y * ChunkSize + Z * SliceSize;
					FVoxelData& Voxel = OutVoxelData[Index];

					if (Voxel.IsSolid() || Voxel.HasCaveFlag())
						break;

					Voxel.SetWaterFlag(true);
				}
			}
		}
	}

	// === Phase 2: BFS flood fill — propagate water through non-cave air ===
	// Cave-flagged voxels act as barriers so water stays in the open ocean
	// and doesn't leak into cave interiors.
	TArray<int32> BFSQueue;
	BFSQueue.Reserve(SliceSize);

	for (int32 i = 0; i < VolumeSize; ++i)
	{
		if (OutVoxelData[i].HasWaterFlag())
		{
			BFSQueue.Add(i);
		}
	}

	static constexpr int32 DX[6] = { 1, -1,  0,  0,  0,  0 };
	static constexpr int32 DY[6] = { 0,  0,  1, -1,  0,  0 };
	static constexpr int32 DZ[6] = { 0,  0,  0,  0,  1, -1 };

	int32 QueueHead = 0;
	while (QueueHead < BFSQueue.Num())
	{
		const int32 CurrentIdx = BFSQueue[QueueHead++];
		const int32 CZ = CurrentIdx / SliceSize;
		const int32 CY = (CurrentIdx - CZ * SliceSize) / ChunkSize;
		const int32 CX = CurrentIdx - CZ * SliceSize - CY * ChunkSize;

		for (int32 D = 0; D < 6; ++D)
		{
			const int32 NX = CX + DX[D];
			const int32 NY = CY + DY[D];
			const int32 NZ = CZ + DZ[D];

			if (NX < 0 || NX >= ChunkSize ||
				NY < 0 || NY >= ChunkSize ||
				NZ < 0 || NZ >= ChunkSize)
			{
				continue;
			}

			const int32 NeighborIdx = NX + NY * ChunkSize + NZ * SliceSize;
			FVoxelData& Neighbor = OutVoxelData[NeighborIdx];

			// Skip solid, already-flagged, and cave-carved voxels
			if (Neighbor.IsSolid() || Neighbor.HasWaterFlag() || Neighbor.HasCaveFlag())
				continue;

			const float NeighborWorldZ = ChunkWorldPos.Z + NZ * VoxelSize;
			if (NeighborWorldZ > WaterLevel)
				continue;

			Neighbor.SetWaterFlag(true);
			BFSQueue.Add(NeighborIdx);
		}
	}

	// === Phase 3: Clear cave flags ===
	// Cave flags were temporary barriers to prevent ocean water from flooding
	// cave interiors during the column scan and initial BFS. Now clear them
	// so cave voids become normal air.
	// Note: Phase 4 (cave re-flooding BFS) was removed — caves below water
	// level are intentionally kept dry so players can explore them on foot.
	for (int32 i = 0; i < VolumeSize; ++i)
	{
		if (OutVoxelData[i].HasCaveFlag())
		{
			OutVoxelData[i].SetCaveFlag(false);
		}
	}
}

void FVoxelCPUNoiseGenerator::ApplyRadialWaterFillPass(
	const FVoxelNoiseGenerationRequest& Request,
	TArray<FVoxelData>& OutVoxelData)
{
	const int32 ChunkSize = Request.ChunkSize;
	const float VoxelSize = Request.VoxelSize;
	const FVector ChunkWorldPos = Request.GetChunkWorldPosition();
	const int32 SliceSize = ChunkSize * ChunkSize;
	const int32 VolumeSize = SliceSize * ChunkSize;
	const FVoxelSeaSurface Sea = FVoxelSeaSurface::MakeSphere(Request.SphericalPlanetParams.PlanetCenter, Request.WaterRadius);
	auto VoxelWorldPos = [&](const FIntVector& Local)
	{
		return ChunkWorldPos + FVector(Local) * VoxelSize;
	};

	// === Phase 1: seed the open sea, in a chunk the sea surface passes through ===
	// As the plane's column scan: along each line in local down (FVoxelDownLines, the chunk centre's down), the part above
	// the sea must be clear of solid and cave voxels from where the line enters the chunk (the column is open from the
	// chunk top); from the sea surface down, air takes the water flag until the first solid or cave voxel. A line that
	// enters the chunk already below the sea counts as open, as a column does when the water plane is the chunk's top
	// layer. Chunks wholly under the sea fill from their neighbours (UVoxelChunkManager::PropagateWaterFromNeighbors). The
	// GPU's WaterLineSeedCS walks the same lines.
	const FBox VoxelBox(ChunkWorldPos, ChunkWorldPos + FVector((ChunkSize - 1) * VoxelSize));
	if (Sea.Crosses(VoxelBox))
	{
		struct FSeaLine
		{
			bool bBlocked = false; // solid or cave above the sea, or the flood met its floor
		};
		const FVoxelDownLines DownLines = FVoxelDownLines::Make(ChunkSize, FVoxelDownLines::PlanetChunkDown(Request));
		TArray<FSeaLine> Lines;
		Lines.SetNum(DownLines.NumLines());
		DownLines.ForEach([&](int32 LineId, int32 Index, const FIntVector& Local)
		{
			FSeaLine& Line = Lines[LineId];
			if (Line.bBlocked)
			{
				return;
			}
			FVoxelData& Voxel = OutVoxelData[Index];
			if (Voxel.IsSolid() || Voxel.HasCaveFlag())
			{
				Line.bBlocked = true;
				return;
			}
			if (Sea.IsBelow(VoxelWorldPos(Local)))
			{
				Voxel.SetWaterFlag(true);
			}
		});
	}

	// === Phase 2: BFS flood fill through non-cave air inside the sea radius ===
	TArray<int32> BFSQueue;
	BFSQueue.Reserve(SliceSize);
	for (int32 i = 0; i < VolumeSize; ++i)
	{
		if (OutVoxelData[i].HasWaterFlag())
		{
			BFSQueue.Add(i);
		}
	}

	static constexpr int32 DX[6] = { 1, -1,  0,  0,  0,  0 };
	static constexpr int32 DY[6] = { 0,  0,  1, -1,  0,  0 };
	static constexpr int32 DZ[6] = { 0,  0,  0,  0,  1, -1 };

	int32 QueueHead = 0;
	while (QueueHead < BFSQueue.Num())
	{
		const int32 CurrentIdx = BFSQueue[QueueHead++];
		const int32 CZ = CurrentIdx / SliceSize;
		const int32 CY = (CurrentIdx - CZ * SliceSize) / ChunkSize;
		const int32 CX = CurrentIdx - CZ * SliceSize - CY * ChunkSize;

		for (int32 D = 0; D < 6; ++D)
		{
			const FIntVector N(CX + DX[D], CY + DY[D], CZ + DZ[D]);
			if (N.X < 0 || N.X >= ChunkSize || N.Y < 0 || N.Y >= ChunkSize || N.Z < 0 || N.Z >= ChunkSize)
			{
				continue;
			}

			const int32 NeighborIdx = N.X + N.Y * ChunkSize + N.Z * SliceSize;
			FVoxelData& Neighbor = OutVoxelData[NeighborIdx];
			if (Neighbor.IsSolid() || Neighbor.HasWaterFlag() || Neighbor.HasCaveFlag() || !Sea.IsBelow(VoxelWorldPos(N)))
			{
				continue;
			}

			Neighbor.SetWaterFlag(true);
			BFSQueue.Add(NeighborIdx);
		}
	}

	// === Phase 3: clear the cave flags (temporary barriers), as the plane's pass does ===
	for (int32 i = 0; i < VolumeSize; ++i)
	{
		if (OutVoxelData[i].HasCaveFlag())
		{
			OutVoxelData[i].SetCaveFlag(false);
		}
	}
}

// ==================== Underground Classification Pass ====================

void FVoxelCPUNoiseGenerator::ApplyUndergroundClassificationPass(
	const FVoxelNoiseGenerationRequest& Request,
	TArray<FVoxelData>& OutVoxelData)
{
	const int32 ChunkSize = Request.ChunkSize;
	const int32 SliceSize = ChunkSize * ChunkSize;

	// Pass 1: Column scan to flag underground air not already flagged by cave carving.
	// State machine: scan top-down, require seeing open air BEFORE counting solid.
	// Solid voxels at the column top (before any air) are terrain surface geometry
	// (peaks, ridges, chunk-boundary terrain), NOT cave ceilings. Only begin
	// counting solid thickness after the first air gap, so the pattern must be:
	//   [optional solid at top] → air → solid (>= MinSolidThickness) → air = underground
	// This avoids falsely flagging surface air below ridges/overhangs as underground.
	const int32 MinSolidThickness = 3;

	// One scan line's state, top-down.
	struct FLineState
	{
		bool bSeenAir = false;
		bool bUnderground = false;
		int32 ConsecutiveSolid = 0;
	};
	auto Step = [MinSolidThickness](FLineState& Line, FVoxelData& Voxel)
	{
		if (Voxel.IsSolid())
		{
			// Only count solid after we've seen air (sky/surface)
			if (Line.bSeenAir)
			{
				++Line.ConsecutiveSolid;
			}
			return;
		}

		Line.bSeenAir = true;

		// Air voxel — decide if it's underground
		if (!Line.bUnderground && Line.ConsecutiveSolid >= MinSolidThickness)
		{
			// Passed through thick terrain ceiling — everything below is underground
			Line.bUnderground = true;
		}

		if (Line.bUnderground && !Voxel.HasWaterFlag())
		{
			Voxel.SetUndergroundFlag(true);
		}

		// Reset for next solid sequence (only matters while still above ground)
		if (!Line.bUnderground)
		{
			Line.ConsecutiveSolid = 0;
		}
	};

	if (Request.WorldMode == EWorldMode::SphericalPlanet)
	{
		// A planet scans along local down (the chunk centre's), not world -Z (SP-B7): at mid latitudes down is up to 55
		// degrees off every axis, and a world-Z scan reads a hillside as a cave ceiling, or misses a cave whose column
		// runs into the ground to the chunk's top. The same state machine runs on each digital line along down
		// (FVoxelDownLines) from where it enters the chunk; the GPU's UndergroundLinesCS walks the same lines.
		const FVoxelDownLines DownLines = FVoxelDownLines::Make(ChunkSize, FVoxelDownLines::PlanetChunkDown(Request));
		TArray<FLineState> Lines;
		Lines.SetNum(DownLines.NumLines());
		DownLines.ForEach([&](int32 LineId, int32 Index, const FIntVector&)
		{
			Step(Lines[LineId], OutVoxelData[Index]);
		});
	}
	else
	{
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				FLineState Line;
				for (int32 Z = ChunkSize - 1; Z >= 0; --Z)
				{
					Step(Line, OutVoxelData[X + Y * ChunkSize + Z * SliceSize]);
				}
			}
		}
	}

	// Pass 2: Propagate underground flag to solid boundary voxels.
	// Scatter surface points sit at solid-air boundaries. Flagging the solid
	// side ensures the scatter extractors find the flag whichever side of
	// the crossing they read.
	// Two iterations extend the flag deeper into solid geometry for corners
	// and edges where mesh interpolation places surface points further
	// from the cave air.
	// Guard: never flag a solid voxel that has a non-underground air neighbor.
	// Such voxels are surface-exposed (terrain face, cliff, overhang) and
	// flagging them would cause surface scatter to be classified underground.
	//
	// Each iteration is two-phase (collect, then apply) so it reads a consistent
	// pre-iteration snapshot: in-place flagging would let flags set earlier in the
	// same iteration cascade along the scan direction, making the effective depth
	// scan-order dependent — and impossible for the parallel GPU port
	// (VoxelPostPasses.usf UndergroundMark/Apply) to reproduce.
	const int32 PropagationDepth = 2;
	TArray<int32> ToFlag;
	for (int32 Iteration = 0; Iteration < PropagationDepth; ++Iteration)
	{
		ToFlag.Reset();
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < ChunkSize; ++Y)
			{
				for (int32 X = 0; X < ChunkSize; ++X)
				{
					const int32 Index = X + Y * ChunkSize + Z * SliceSize;
					const FVoxelData& Voxel = OutVoxelData[Index];

					if (!Voxel.IsSolid() || Voxel.HasUndergroundFlag())
						continue;

					// Check 6-connected neighbors
					const int32 Neighbors[6] = {
						(X > 0)              ? Index - 1 : -1,
						(X < ChunkSize - 1)  ? Index + 1 : -1,
						(Y > 0)              ? Index - ChunkSize : -1,
						(Y < ChunkSize - 1)  ? Index + ChunkSize : -1,
						(Z > 0)              ? Index - SliceSize : -1,
						(Z < ChunkSize - 1)  ? Index + SliceSize : -1,
					};

					bool bAdjacentToUnderground = false;
					bool bAdjacentToSurfaceAir = false;
					for (int32 N = 0; N < 6; ++N)
					{
						if (Neighbors[N] >= 0)
						{
							const FVoxelData& Nbr = OutVoxelData[Neighbors[N]];
							if (Nbr.HasUndergroundFlag())
							{
								bAdjacentToUnderground = true;
							}
							else if (!Nbr.IsSolid())
							{
								// Non-underground air = surface/sky exposure
								bAdjacentToSurfaceAir = true;
							}
						}
					}

					if (bAdjacentToUnderground && !bAdjacentToSurfaceAir)
					{
						ToFlag.Add(Index);
					}
				}
			}
		}

		for (const int32 Index : ToFlag)
		{
			OutVoxelData[Index].SetUndergroundFlag(true);
		}
	}
}
