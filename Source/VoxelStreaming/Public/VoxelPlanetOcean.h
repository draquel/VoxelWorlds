// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * What a planet's sea surface covers and how finely (built by the chunk manager from the world configuration:
 * UVoxelChunkManager::RebuildPlanetOcean).
 */
struct FVoxelPlanetOceanSettings
{
	/** The planet centre: the mesh's positions are relative to it (its component sits there) */
	FVector Center = FVector::ZeroVector;

	/** The sea radius (UVoxelWorldConfiguration::WaterRadius) */
	double Radius = 0.0;

	/** Voxel size: the UVs count voxels of arc along each cube face, as the water tiles' count voxels in X and Y */
	double VoxelSize = 100.0;

	/**
	 * How far a quad may sag under the sphere at its diagonal's middle, its deepest point; sets the resolution
	 * (ChooseSegments). Water flags and swimming use the true sphere, so this is only how much lower the drawn sea can be.
	 */
	double MaxSag = 8.0;

	/** Fewest and most quads along a cube face's edge */
	int32 MinSegments = 32;
	int32 MaxSegments = 160;
};

/** One section of the planet ocean, ready for a procedural mesh section (positions relative to the planet centre). */
struct FVoxelPlanetOceanMesh
{
	TArray<FVector> Positions;
	TArray<FVector> Normals;
	TArray<FVector> Tangents;
	TArray<FVector2D> UV0;
	TArray<FVector2D> UV1;
	TArray<FColor> Colors;
	TArray<int32> Triangles;

	void Reset()
	{
		Positions.Reset();
		Normals.Reset();
		Tangents.Reset();
		UV0.Reset();
		UV1.Reset();
		Colors.Reset();
		Triangles.Reset();
	}
};

/**
 * A planet's sea surface (spherical requirement SP-E2; WORLD_MODES_REVIEW_PLAN.md §17, WM-W8): the sphere of WaterRadius
 * around the planet centre as a cube sphere, six faces of Segments x Segments quads on the tangent warp (g = tan(pi/4 u),
 * the planet's surface coordinate, FSphericalPlanetWorldMode::WorldToSurface), so the quads are near equal in size.
 * Pure geometry; the chunk manager owns the mesh component, its own primitive like the island's far ocean (FVoxelFarOcean).
 *
 * Two sections:
 *  - The top: triangles facing away from the centre. The water material is one-sided, so seen from outside the far
 *    hemisphere is culled and no ray meets the near one twice (the old test actor drew a two-sided sphere, whose far side
 *    blended over its near side: the light and dark halves of review plan §15.2).
 *  - The underside: the same quads facing the centre, with the outward normal (it shades from below as the water tiles
 *    and the far ocean's undersides do). The chunk manager shows it only while the viewer is inside the sea radius: from
 *    inside a sphere every ray meets it once.
 *
 * Vertex attributes match the water tiles (FVoxelWaterMesher): UV0 counts voxels of arc along the face's U and V axes from
 * the face centre (the pattern restarts at the cube's twelve edges), UV1 = (water material id, 0), colour R the id; normals
 * are radial, tangents along the face's U axis. Each face has its own vertices, so its UVs do not blend into the next face's.
 */
struct VOXELSTREAMING_API FVoxelPlanetOcean
{
	/** The water tiles' material id (FVoxelWaterMesher::WATER_MATERIAL_ID) */
	static constexpr uint8 WaterMaterialID = 254;

	/**
	 * The quads along a face's edge for a sag budget: the fewest whose largest quad (at the face's corners, where the warp's
	 * cells are biggest) sags at most MaxSag in the middle, clamped to [MinSegments, MaxSegments].
	 *
	 * @param Radius Sphere radius
	 * @param MaxSag Sag budget (world units)
	 * @param MinSegments Fewest quads along an edge
	 * @param MaxSegments Most quads along an edge
	 * @return Quads along a face's edge
	 */
	static int32 ChooseSegments(double Radius, double MaxSag, int32 MinSegments, int32 MaxSegments);

	/**
	 * The direction from the centre to grid vertex (I, J) of a face: the face normal plus the gnomonic offsets
	 * tan(pi/4 u) and tan(pi/4 v) along its U and V axes, normalised. u and v run from -1 to 1 over the face.
	 *
	 * @param Face Cube face 0..5 (+X, -X, +Y, -Y, +Z, -Z, as FSphericalPlanetWorldMode's surface coordinate)
	 * @param I Column 0..Segments
	 * @param J Row 0..Segments
	 * @param Segments Quads along the face's edge
	 * @return Unit direction
	 */
	static FVector GetGridDirection(int32 Face, int32 I, int32 J, int32 Segments);

	/**
	 * A face's frame: normal N, U axis T and V axis B, with T x B = N (FSphericalPlanetWorldMode's faces).
	 *
	 * @param Face Cube face 0..5
	 * @param OutN Face normal
	 * @param OutT U axis
	 * @param OutB V axis
	 */
	static void GetFaceFrame(int32 Face, FVector& OutN, FVector& OutT, FVector& OutB);

	/**
	 * The largest sag of any quad's middle under the sphere at this resolution (measured: the quad's corner average
	 * against the radius, over one face's quads; the six faces are alike).
	 *
	 * @param Radius Sphere radius
	 * @param Segments Quads along a face's edge
	 * @return Largest sag (world units)
	 */
	static double MeasureMaxSag(double Radius, int32 Segments);

	/**
	 * Build the sea surface.
	 *
	 * @param Settings Centre, radius, UV scale and resolution
	 * @param OutTop The top section (faces outward)
	 * @param OutUnderside The underside section (faces inward), or null for none
	 * @param OutSegments Receives the quads along a face's edge
	 * @return Quads in the top section (6 x Segments^2)
	 */
	static int32 Build(const FVoxelPlanetOceanSettings& Settings, FVoxelPlanetOceanMesh& OutTop, FVoxelPlanetOceanMesh* OutUnderside,
		int32& OutSegments);
};
