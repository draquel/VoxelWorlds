// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Marching Cubes lookup tables for smooth mesh generation.
 *
 * The Marching Cubes algorithm processes voxels in 2x2x2 cubes. Each cube has 8 corners,
 * and each corner can be inside (solid) or outside (air) the isosurface. This creates
 * 256 possible configurations (2^8).
 *
 * For each configuration:
 * - EdgeTable indicates which of the 12 cube edges are intersected by the isosurface
 * - TriTable provides the triangles to generate, specified as sequences of edge indices
 *
 * Cube Corner Layout (matching standard Marching Cubes convention):
 *
 *          7-------6
 *         /|      /|
 *        4-------5 |
 *        | |     | |
 *        | 3-----|-2
 *        |/      |/
 *        0-------1
 *
 * Corner 0: (0,0,0)  Corner 4: (0,0,1)
 * Corner 1: (1,0,0)  Corner 5: (1,0,1)
 * Corner 2: (1,1,0)  Corner 6: (1,1,1)
 * Corner 3: (0,1,0)  Corner 7: (0,1,1)
 *
 * Edge numbering:
 * Edge 0:  0-1  (bottom front)
 * Edge 1:  1-2  (bottom right)
 * Edge 2:  2-3  (bottom back)
 * Edge 3:  3-0  (bottom left)
 * Edge 4:  4-5  (top front)
 * Edge 5:  5-6  (top right)
 * Edge 6:  6-7  (top back)
 * Edge 7:  7-4  (top left)
 * Edge 8:  0-4  (left front)
 * Edge 9:  1-5  (right front)
 * Edge 10: 2-6  (right back)
 * Edge 11: 3-7  (left back)
 *
 * @see FVoxelCPUMarchingCubesMesher
 * @see FVoxelGPUMarchingCubesMesher
 */
namespace MarchingCubesTables
{
	/**
	 * Edge table: for each of the 256 cube configurations, a 12-bit mask
	 * indicating which edges are intersected by the isosurface.
	 *
	 * Bit N corresponds to Edge N. If bit is set, the edge crosses the isosurface.
	 */
	extern const uint16 EdgeTable[256];

	/**
	 * Triangle table: for each configuration, up to 5 triangles (15 edge indices).
	 * Each triangle is specified as 3 consecutive edge indices.
	 * Sequences are terminated by -1.
	 *
	 * Example: TriTable[N] = {0, 8, 3, 1, 9, 4, -1, ...} defines two triangles:
	 *   Triangle 1: edges 0, 8, 3
	 *   Triangle 2: edges 1, 9, 4
	 */
	extern const int8 TriTable[256][16];

	/**
	 * Corner offsets: local position offset for each of the 8 cube corners.
	 * Used to calculate corner world positions.
	 */
	extern const FIntVector CornerOffsets[8];

	/**
	 * Edge vertex pairs: which two corners each edge connects.
	 * EdgeVertexPairs[EdgeIndex][0] = first corner
	 * EdgeVertexPairs[EdgeIndex][1] = second corner
	 */
	extern const int32 EdgeVertexPairs[12][2];

	/**
	 * Get the number of triangles for a given cube configuration.
	 *
	 * @param CubeIndex The 8-bit cube configuration (0-255)
	 * @return Number of triangles (0-5)
	 */
	FORCEINLINE int32 GetTriangleCount(uint8 CubeIndex)
	{
		int32 Count = 0;
		for (int32 i = 0; TriTable[CubeIndex][i] != -1; i += 3)
		{
			Count++;
		}
		return Count;
	}
}
