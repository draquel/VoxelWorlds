// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelWorldModeTypes.h"
#include "ChunkRenderData.h"

/**
 * What the far ocean covers and how it looks (built by the chunk manager from the world configuration and the world
 * mode: UVoxelChunkManager::RebuildFarOcean).
 */
struct FVoxelFarOceanSettings
{
	/** The world's footprint (IVoxelWorldMode::GetWorldBounds): a circle or a rectangle */
	FVoxelWorldBounds Bounds;

	/** World X and Y of chunk column (0, 0)'s minimum corner (the configuration's WorldOrigin) */
	FVector2D GridOrigin = FVector2D::ZeroVector;

	/** Width of a chunk column (ChunkSize * VoxelSize) */
	double ColumnSize = 3200.0;

	/** Voxel size: the water's UVs are world X and Y over it, as the water tiles' are (seamless pattern) */
	double VoxelSize = 100.0;

	/** World Z of the sea surface: WaterLevel, where the water tiles lie */
	double WaterZ = 0.0;

	/** How far past the edge the sea and the seabed reach */
	double OuterDistance = 2.0e6;

	/** Past this distance from the edge the seabed is flat (only the slope needs fine rings) */
	double SeabedSlopeWidth = 0.0;

	/**
	 * How far below the seabed profile the drawn seabed sits, so the voxel terrain covers it wherever it is loaded.
	 * Keep it small: the loaded seabed shadows the drawn one along its edge in a strip about this wide.
	 */
	double SeabedOffset = 40.0;

/** Voxel material the seabed is drawn with (UV1.x and vertex colour R, as terrain vertices carry it) */
	uint8 SeabedMaterialID = 0;

	/** The seabed's height (world Z) at a distance past the edge: FIslandBowlWorldMode::GetSeabedHeight for an island */
	TFunction<double(double DistancePastEdge)> SeabedHeight;

/** Circles: how far inside the circle a ring's chord may run (the seabed's rings are circumscribed polygons) */
	double MaxChordError = 25.0;

	/** World position the meshes are relative to (their component sits here): the footprint's centre at Z 0 */
	FVector GetMeshOrigin() const { return FVector(Bounds.Center.X, Bounds.Center.Y, 0.0); }
};

/**
 * The far ocean of a bounded world (world modes review WM-E1, finding KI2): the sea from the edge of what is loaded out
 * to the horizon, and the seabed under it, so an island seen from outside its loaded ring sits in a sea instead of
 * floating in a void. Pure geometry; the chunk manager owns the mesh component (UVoxelChunkManager::RebuildFarOcean) and
 * it is its own primitive, so a recreated terrain scene proxy (G15) does not touch it.
 *
 * Two meshes, both static. Inside the footprint's box the far field draws the land and the sea (mode parity WM-MP1,
 * decision D37); the grid water and floor that once stood in for unloaded columns there went with voxel.World.FarField 0
 * becoming a development switch (code review CR1, D-CR13).
 *  - Open-sea water: from the grid's rectangle (the footprint's box plus GridMarginColumns) out to the box plus
 *    OuterDistance, in rings that widen geometrically.
 *  - Seabed: from the edge out to the open sea's corners, following SeabedHeight minus SeabedOffset: under the voxel
 *    seabed of the loaded columns that cross the edge, in their place where nothing is loaded.
 *
 * The water faces up. Its undersides (the water tiles are seen from below too: the scene proxy draws them without
 * back-face culling, QA R11) are separate meshes that the chunk manager shows only while the viewer is under the water
 * plane: drawn all the time, the undersides of far, nearly edge-on triangles won the winding test on some rows and
 * blended twice, which drew dark lines under the horizon.
 */
struct VOXELSTREAMING_API FVoxelFarOcean
{
	/** Columns of margin around the footprint's box in the grid (the old island cull reached a column and a half out) */
	static constexpr int32 GridMarginColumns = 2;

/** Rings of the open sea between the grid and the outer rectangle, and segments on each side of a ring */
	static constexpr int32 OpenSeaRings = 24;
	static constexpr int32 OpenSeaSegmentsPerSide = 8;

	/** Rings across the seabed's slope, and rings from its foot out to the open sea's corners */
	static constexpr int32 SeabedSlopeRings = 24;
	static constexpr int32 SeabedFlatRings = 16;

	/** A circle's seabed rings have between these many segments */
	static constexpr int32 MinCircleSegments = 64;
	static constexpr int32 MaxCircleSegments = 2048;

	/**
	 * The grid's columns: the footprint's box plus GridMarginColumns all round, inclusive (the open sea starts at their
	 * rectangle).
	 *
	 * @param Settings The far ocean's settings
	 * @param OutMin Lowest column X and Y
	 * @param OutMax Highest column X and Y
	 * @return False for an unbounded world (no grid)
	 */
	static bool GetGridColumns(const FVoxelFarOceanSettings& Settings, FIntPoint& OutMin, FIntPoint& OutMax);

	/**
	 * The open sea's outer rectangle: the footprint's box plus OuterDistance (world X and Y).
	 *
	 * @param Settings The far ocean's settings
	 * @return The rectangle; invalid for an unbounded world
	 */
	static FBox2D GetOuterBox(const FVoxelFarOceanSettings& Settings);

	/**
	 * Open-sea water: the frame between the grid's rectangle and the outer rectangle.
	 *
	 * @param Settings The far ocean's settings
	 * @param OutMesh Receives the mesh (reset first), relative to Settings.GetMeshOrigin()
	 * @param OutUnderside Optional: receives the same quads facing down (reset first)
	 * @return Number of quads
	 */
	static int32 BuildOpenSeaWater(const FVoxelFarOceanSettings& Settings, FChunkMeshData& OutMesh, FChunkMeshData* OutUnderside = nullptr);

	/**
	 * The seabed beyond the edge, up-facing, with terrain vertex data (UV1 = (SeabedMaterialID, top), colour R the
	 * same id, normals from the profile's slope).
	 *
	 * @param Settings The far ocean's settings (SeabedHeight must be set)
	 * @param OutMesh Receives the mesh (reset first), relative to Settings.GetMeshOrigin()
	 * @return Number of triangles
	 */
	static int32 BuildSeabed(const FVoxelFarOceanSettings& Settings, FChunkMeshData& OutMesh);

	/**
	 * How far (X, Y) lies past the footprint's edge, the way an island measures it (FIslandBowlWorldMode::
	 * GetDistancePastEdge): radially for a circle, the larger axis excess for a rectangle.
	 *
	 * @param Bounds The footprint
	 * @param X World X
	 * @param Y World Y
	 * @return Distance past the edge; zero or negative inside
	 */
	static double GetDistancePastEdge(const FVoxelWorldBounds& Bounds, double X, double Y);
};
