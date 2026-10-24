// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UVoxelWorldConfiguration;

/**
 * The walking surface of a block (cubic) world (world modes review WM-C5, KC6).
 *
 * A cubic block at voxel (i, j, k) is the cell [k * V, (k + 1) * V) above WorldOrigin, and the heightmap generators give
 * it the density of the column height H sampled at the cell's lower corner (i, j) (FInfinitePlaneWorldMode::
 * CalculateSignedDistance / SignedDistanceToDensity). The block is solid when that density reaches the surface threshold,
 * so the top face of the topmost solid block lies between H and H + V: the density isosurface that smooth meshing draws
 * (and that every surface query returned) is up to one block below the block tops a cubic world draws and collides with.
 * These helpers turn a generated column height into the block top the generator makes from it, exactly by the
 * generator's own density rule, and say where a position's block column is sampled.
 *
 * Smooth worlds and planets have no block surface (UsesBlockSurface is false there); cubic planets are out of scope.
 *
 * Thread safety: stateless and thread-safe (UsesBlockSurface reads a console variable).
 */
struct VOXELGENERATION_API FVoxelBlockSurface
{
	/**
	 * Whether a world's walking surface is block tops: cubic meshing in a heightmap world mode (InfinitePlane, IslandBowl),
	 * with voxel.Cubic.BlockSurface on (0 = the density isosurface, as before WM-C5).
	 *
	 * @param Config The world configuration
	 * @return True when surface queries and pads should use block tops
	 */
	static bool UsesBlockSurface(const UVoxelWorldConfiguration& Config);

	/**
	 * The lower corner, along one axis, of the block column containing a coordinate: where the generator samples the
	 * column's height (Origin + floor((Coord - Origin) / VoxelSize) * VoxelSize).
	 *
	 * @param Coord World coordinate (X or Y)
	 * @param Origin WorldOrigin's coordinate on the same axis
	 * @param VoxelSize World units per voxel
	 * @return The column's sample coordinate
	 */
	static double ColumnCorner(double Coord, double Origin, float VoxelSize);

	/**
	 * Whether the generator makes the voxel whose cell's lower face is at VoxelZ solid in a column of height Height (before
	 * caves): the same float arithmetic as the heightmap generators.
	 *
	 * @param Height The column's generated height (conditioning zones included)
	 * @param VoxelZ World Z of the voxel's cell bottom
	 * @param VoxelSize World units per voxel
	 * @return True when the voxel's density reaches the surface threshold
	 */
	static bool IsGeneratedSolid(float Height, double VoxelZ, float VoxelSize);

	/**
	 * The global voxel layer (above WorldOrigin) of the topmost voxel the generator makes solid in a column of height Height.
	 *
	 * @param Height The column's generated height
	 * @param OriginZ WorldOrigin.Z
	 * @param VoxelSize World units per voxel
	 * @return The voxel layer k; its block's top face is OriginZ + (k + 1) * VoxelSize
	 */
	static int64 TopSolidLayerForHeight(float Height, double OriginZ, float VoxelSize);

	/**
	 * The top face of the topmost block the generator makes solid in a column of height Height: the block-world surface for
	 * that height (between Height and Height + VoxelSize).
	 *
	 * @param Height The column's generated height
	 * @param OriginZ WorldOrigin.Z
	 * @param VoxelSize World units per voxel
	 * @return World Z of the block top
	 */
	static double BlockTopForHeight(float Height, double OriginZ, float VoxelSize);

	/**
	 * The top face of a voxel layer's blocks.
	 *
	 * @param Layer Global voxel layer above WorldOrigin
	 * @param OriginZ WorldOrigin.Z
	 * @param VoxelSize World units per voxel
	 * @return World Z of the layer's top face
	 */
	static double LayerTop(int64 Layer, double OriginZ, float VoxelSize)
	{
		return OriginZ + static_cast<double>(Layer + 1) * VoxelSize;
	}
};
