// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelPlanetOcean.h"
#include "VoxelWaterLevels.h"

/**
 * The drawn surface of one lake or river reach (the hydrology plan's section 5.6, D147): one mesh a body, in every world
 * mode, inside the voxel window and beyond it. Pure geometry from the body's record; the chunk manager owns the
 * components (FVoxelWaterBodyMeshOwner), as with the island's far ocean and a planet's ocean shell.
 *
 * - A lake is a sheet over its footprint's cells at its level. On a heightmap world the cells are merged into
 *   rectangles, each a quad at world Z = the level. On a planet the sheet lies on the sphere of the level's radius: a
 *   quad a cell, its corners the grid's corners carried out along their own directions, shared between cells. When
 *   the footprint carries its ground (FVoxelWaterLake::WithGround, G229), a cell whose corners and centre all lie
 *   under the level and its margin (ShoreMarginVoxels) is drawn so; a cell of the shore is drawn as four triangles,
 *   corner, corner and centre, each cut where the ground, linear between those three points, rises over the level
 *   and its margin, the cut points shared by the cells beside it. So the sheet's edge follows the shore, not the
 *   cells' steps, and from beyond the voxel window, where the far field's coarser ground does not hide a cell of
 *   shore, no square shows.
 * - A river reach is a ribbon along its polyline, two vertices a vertex of the reach, at the vertex's surface level and
 *   to its footprint's half width on either side (the bed's half width and its falloff: the banks rise over the surface
 *   across the falloff, so the ribbon's edges lie tucked under them). Level across the river, sloped along it.
 *
 * Two sections each, as the far seas have: the top, and the underside, the same triangles turned over with the top's
 * normal, which the owner shows only while the viewer is under that body's surface.
 *
 * Vertex attributes follow the water tiles': positions relative to Origin (the component sits there, so no float is of
 * the size of a world position or of a planet's radius), normals the local up (tilted along a river reach's slope), UV1.x
 * the water material id, colour R the id. A lake's UV0 counts voxels along its grid's axes and its tangent runs along
 * the first. A river reach's UV0 is (across, along) in voxels, the second running downstream from its head; its tangent
 * is the direction downstream and UV1.y its speed (uu a second), which the river's material moves its ripples by.
 */
struct VOXELSTREAMING_API FVoxelWaterBodyMesh
{
	/** The water tiles' material id (FVoxelWaterMesher::WATER_MATERIAL_ID) */
	static constexpr uint8 WaterMaterialID = 254;

	/** Where the mesh's positions are measured from: the component's world location */
	FVector Origin = FVector::ZeroVector;

	/** The surface seen from above */
	FVoxelPlanetOceanMesh Top;

	/** The same triangles turned over, with the top's normals: the surface seen from under it */
	FVoxelPlanetOceanMesh Underside;

	/** How many quads the surface has: whole cells, or on a heightmap world rectangles of them */
	int32 Quads = 0;

	/** How many triangles the cut cells of a lake's shore add (a lake whose footprint carries its ground) */
	int32 ShoreTriangles = 0;

	/**
	 * How far over the level, in voxels, the ground under a lake's sheet may stand where its footprint carries the
	 * ground: up close the ground hides the sheet where it stands over the level, so the margin costs nothing there and
	 * covers the voxels' surface lying a little off the ground read at the cells' corners; from far it is how far up
	 * the shore the sheet's smooth edge may show. A corner on the footprint's outline takes none (BuildLake)
	 */
	static constexpr double ShoreMarginVoxels = 0.1;

	/** @return Whether nothing was built (a body with no footprint) */
	bool IsEmpty() const { return Top.Triangles.Num() == 0; }

	/** @return A vertex's world position */
	FVector GetWorldPosition(int32 Vertex) const { return Origin + Top.Positions[Vertex]; }

	/**
	 * Build a body's surface.
	 *
	 * @param Body The lake or river reach (the sea builds nothing)
	 * @param Frame The world's frame: plain Z, or a planet's centre and datum radius
	 * @param VoxelSize The voxel size: UV0 counts voxels
	 * @param Out Reset and filled
	 * @return Whether a surface was built
	 */
	static bool Build(const FVoxelWaterBody& Body, const FVoxelWaterFrame& Frame, double VoxelSize, FVoxelWaterBodyMesh& Out);

private:
	static void BuildLake(const FVoxelWaterBody& Body, const FVoxelWaterFrame& Frame, double VoxelSize, FVoxelWaterBodyMesh& Out);
	static void BuildReach(const FVoxelWaterBody& Body, const FVoxelWaterFrame& Frame, double VoxelSize, FVoxelWaterBodyMesh& Out);

	/** Add a vertex to the top section. */
	int32 AddVertex(const FVector& WorldPosition, const FVector& Normal, const FVector& Tangent, const FVector2D& UV, float Speed);

	/** Add a quad of four top vertices, counter-clockwise seen from above (A, B, C, D). */
	void AddQuad(int32 A, int32 B, int32 C, int32 D);

	/** Add a triangle of three top vertices, counter-clockwise seen from above (a cut cell of a lake's shore). */
	void AddTriangle(int32 A, int32 B, int32 C);

	/** Make the underside from the top: the same vertices, each triangle turned over. */
	void FinishUnderside();
};
