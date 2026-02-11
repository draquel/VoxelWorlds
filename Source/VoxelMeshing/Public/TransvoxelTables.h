// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Transvoxel lookup tables for seamless LOD transitions in Marching Cubes terrain.
 *
 * The Transvoxel algorithm (Eric Lengyel, 2010) solves the problem of seams between
 * adjacent chunks at different LOD levels. When a high-resolution chunk borders a
 * low-resolution chunk, their surface meshes have different vertex positions along
 * the shared boundary, creating visible gaps.
 *
 * Transvoxel uses special "transition cells" at LOD boundaries:
 * - Interior cells use standard Marching Cubes (8 corners, 256 configurations)
 * - Boundary cells use transition cells (9 high-res samples on one face, connecting
 *   to the 4 low-res corners that the neighbor chunk will produce)
 *
 * Transition Cell Layout (looking at the high-res face):
 *
 *     6---7---8
 *     |   |   |
 *     3---4---5
 *     |   |   |
 *     0---1---2
 *
 * The 9 samples form a 3x3 grid on the high-resolution side. These connect to
 * the 4 corner samples (0, 2, 6, 8) which correspond to what the low-resolution
 * neighbor will generate.
 *
 * Each transition cell has 512 possible configurations (2^9 samples).
 * Due to symmetry, these reduce to 56 equivalence classes.
 *
 * Tables derived from Eric Lengyel's official Transvoxel implementation (MIT License).
 * Source: https://github.com/EricLengyel/Transvoxel
 *
 * @see FVoxelCPUSmoothMesher
 * @see https://transvoxel.org/
 */
namespace TransvoxelTables
{
	/**
	 * Transition cell class for each of 512 configurations.
	 * - High bit (0x80): If set, triangle winding order must be reversed
	 * - Low 7 bits (0x7F): Equivalence class (0-55)
	 */
	extern const uint8 TransitionCellClass[512];

	/**
	 * Transition cell data: vertex count and triangle count for each equivalence class.
	 * Format: (vertexCount << 4) | triangleCount
	 */
	extern const uint8 TransitionCellData[56];

	/**
	 * Transition vertex data for each of the 512 possible cases.
	 * IMPORTANT: This is indexed by CASE (0-511), NOT by equivalence class!
	 * Each case has pre-transformed vertex positions based on the case's
	 * relationship to its equivalence class.
	 *
	 * Each uint16 encodes vertex position:
	 * - High byte: Vertex reuse information (can be ignored for basic implementation)
	 * - Low byte: Edge endpoint indices
	 *   - High nibble (bits 4-7): First endpoint sample index
	 *   - Low nibble (bits 0-3): Second endpoint sample index
	 *
	 * Endpoint indices (as hex nibbles):
	 *   0-8: The 9 samples on the transition face
	 *   9 (0x9): Interior corner mapping to face sample 0
	 *   A (0xA): Interior corner mapping to face sample 2
	 *   B (0xB): Interior corner mapping to face sample 6
	 *   C (0xC): Interior corner mapping to face sample 8
	 *
	 * If both endpoints are equal, vertex is at that exact sample point.
	 * If different, interpolate between the two sample points.
	 */
	extern const uint16 TransitionVertexData[512][12];

	/**
	 * Transition cell triangles for each equivalence class.
	 * Each triangle is 3 consecutive vertex indices into the TransitionVertexData.
	 * Terminated by 0xFF.
	 */
	extern const uint8 TransitionCellTriangles[56][37];

	/**
	 * Sample point offsets for the 9-point transition cell face.
	 * These are in the local 2D coordinate system of the face.
	 * Point 0 is at (0,0), Point 8 is at (1,1).
	 */
	extern const FVector2f TransitionSampleOffsets[9];

	/**
	 * Maps the 13 sample points to their corresponding 3D positions
	 * for each of the 6 possible face orientations.
	 * Index: [FaceIndex][SampleIndex] -> FVector3f offset from cell origin
	 *
	 * Sample indices:
	 *   0-8: The 9 samples on the transition face (3x3 grid)
	 *   9-12: The 4 interior corners of the cell (at the opposite side from the face)
	 *
	 * Face indices:
	 * 0: -X face (looking from -X toward +X)
	 * 1: +X face (looking from +X toward -X)
	 * 2: -Y face
	 * 3: +Y face
	 * 4: -Z face
	 * 5: +Z face
	 */
	extern const FVector3f TransitionCellSampleOffsets[6][13];

	/**
	 * The 4 corner indices of the transition cell that correspond to
	 * the low-resolution neighbor's vertices.
	 * These are sample points 0, 2, 6, 8 (the corners of the 3x3 grid).
	 */
	constexpr int32 LowResCorners[4] = { 0, 2, 6, 8 };

	/**
	 * Check if a transition cell case has inverted winding.
	 */
	FORCEINLINE bool IsTransitionCaseInverted(uint16 CellCase)
	{
		return (TransitionCellClass[CellCase] & 0x80) != 0;
	}

	/**
	 * Get the equivalence class for a transition cell case.
	 */
	FORCEINLINE uint8 GetTransitionCellClass(uint16 CellCase)
	{
		return TransitionCellClass[CellCase] & 0x7F;
	}

	/**
	 * Get vertex count for a transition cell configuration.
	 */
	FORCEINLINE int32 GetTransitionVertexCount(uint16 CellCase)
	{
		const uint8 CellClass = TransitionCellClass[CellCase] & 0x7F;
		return TransitionCellData[CellClass] >> 4;
	}

	/**
	 * Get triangle count for a transition cell configuration.
	 */
	FORCEINLINE int32 GetTransitionTriangleCount(uint16 CellCase)
	{
		const uint8 CellClass = TransitionCellClass[CellCase] & 0x7F;
		return TransitionCellData[CellClass] & 0x0F;
	}

	/**
	 * Map an endpoint index to its corresponding sample index.
	 * Endpoints 0-8 map directly to face samples (indices 0-8).
	 * Endpoints 9-C map to interior corner samples (indices 9-12).
	 */
	FORCEINLINE int32 MapEndpointToSample(int32 Endpoint)
	{
		if (Endpoint <= 8)
		{
			return Endpoint;  // Face samples 0-8
		}
		// Interior corners: 0x9->9, 0xA->10, 0xB->11, 0xC->12
		switch (Endpoint)
		{
			case 0x9: return 9;   // Interior corner 0
			case 0xA: return 10;  // Interior corner 1
			case 0xB: return 11;  // Interior corner 2
			case 0xC: return 12;  // Interior corner 3
			default:  return 0;   // Fallback
		}
	}

	// =========================================================================
	// Lengyel's Regular Marching Cubes Tables
	// =========================================================================
	//
	// These are the modified Marching Cubes tables from Eric Lengyel's Transvoxel
	// implementation. Using these instead of classic Lorensen & Cline tables ensures
	// that the regular MC triangulation is compatible with the Transvoxel transition
	// cells, preventing gaps at LOD boundaries due to ambiguous case resolution.
	//
	// Corner ordering (differs from classic MC):
	//   0=(0,0,0), 1=(1,0,0), 2=(0,1,0), 3=(1,1,0),
	//   4=(0,0,1), 5=(1,0,1), 6=(0,1,1), 7=(1,1,1)
	//
	// Source: https://github.com/EricLengyel/Transvoxel (MIT License)

	/**
	 * Cell data for one of the 16 regular MC equivalence classes.
	 * GeometryCounts: high nibble = vertex count, low nibble = triangle count.
	 * VertexIndex: groups of 3 indices giving the triangulation.
	 */
	struct FRegularCellData
	{
		uint8 GeometryCounts;
		uint8 VertexIndex[15];

		FORCEINLINE int32 GetVertexCount() const { return GeometryCounts >> 4; }
		FORCEINLINE int32 GetTriangleCount() const { return GeometryCounts & 0x0F; }
	};

	/**
	 * Maps an 8-bit regular MC case index to an equivalence class (0-15).
	 * Uses Lengyel's corner ordering.
	 */
	extern const uint8 RegularCellClass[256];

	/**
	 * Triangulation data for each of the 16 equivalence classes.
	 */
	extern const FRegularCellData RegularCellData[16];

	/**
	 * Vertex data for each of the 256 MC cases.
	 * Each uint16 encodes an edge:
	 * - Low byte, low nibble: first corner index (0-7)
	 * - Low byte, high nibble: second corner index (0-7)
	 * - High byte: vertex reuse information (can be ignored)
	 */
	extern const uint16 RegularVertexData[256][12];
}
