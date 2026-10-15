// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.h"
#include "VoxelWorldModeTypes.generated.h"

/**
 * Shape of a world's bounds (FVoxelWorldBounds). Part of the world-mode API (world modes review WM-A2).
 */
UENUM(BlueprintType)
enum class EVoxelWorldBoundsShape : uint8
{
	/** No edge: the world extends without limit in X and Y (InfinitePlane) */
	Unbounded,

	/** A disc in XY around Center with radius OuterRadius (a circular island's R + falloff) */
	Circle,

	/** An axis-aligned rectangle in XY around Center with half sizes HalfExtent (a rectangular island) */
	Rectangle,

	/** A spherical shell around Center, InnerRadius to OuterRadius (a planet's terrain shell) */
	Shell
};

/**
 * Where a world ends: IVoxelWorldMode::GetWorldBounds, Blueprint-callable through UVoxelChunkManager::GetWorldBounds.
 *
 * Consumers that place, explore, spawn or navigate use it to stay inside the world: an island's bounds are the
 * R + falloff footprint (beyond it generation only writes a flat floor), a plane has none, a planet's are its terrain
 * shell. Part of the world-mode API (world modes review WM-A2, findings KI1 and KI9).
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FVoxelWorldBounds
{
	GENERATED_BODY()

	/** Shape of the bounds */
	UPROPERTY(BlueprintReadOnly, Category = "Voxel|World")
	EVoxelWorldBoundsShape Shape = EVoxelWorldBoundsShape::Unbounded;

	/** Centre in world space: the island's centre at the datum height (heightmap modes) or the planet centre */
	UPROPERTY(BlueprintReadOnly, Category = "Voxel|World")
	FVector Center = FVector::ZeroVector;

	/** Half sizes in X and Y of the footprint's box: (R+F, R+F) for a circle, the half sizes for a rectangle, the outer radius for a shell */
	UPROPERTY(BlueprintReadOnly, Category = "Voxel|World")
	FVector2D HalfExtent = FVector2D::ZeroVector;

	/** Shell: the inner radius (the deepest terrain). 0 for the other shapes */
	UPROPERTY(BlueprintReadOnly, Category = "Voxel|World")
	float InnerRadius = 0.0f;

	/** Circle: the footprint radius. Shell: the outer radius (the highest terrain). 0 otherwise */
	UPROPERTY(BlueprintReadOnly, Category = "Voxel|World")
	float OuterRadius = 0.0f;

	/** Lowest world Z the terrain reaches (heightmap modes: the terrain height bounds; shell: Center.Z - OuterRadius) */
	UPROPERTY(BlueprintReadOnly, Category = "Voxel|World")
	float MinZ = 0.0f;

	/** Highest world Z the terrain reaches */
	UPROPERTY(BlueprintReadOnly, Category = "Voxel|World")
	float MaxZ = 0.0f;

	/** @return True unless the world is unbounded */
	bool IsBounded() const { return Shape != EVoxelWorldBoundsShape::Unbounded; }

	/**
	 * Whether the vertical line at (X, Y) passes through the world: always for an unbounded world, inside the
	 * footprint for an island, inside the shell's outer radius for a planet.
	 *
	 * @param X World X
	 * @param Y World Y
	 * @return True when (X, Y) is inside the footprint
	 */
	bool ContainsXY(double X, double Y) const;

	/**
	 * Whether a point is inside the world. Heightmap worlds are bounded in X and Y only (a point above or below the
	 * footprint is inside); a planet's world is everything within its outer radius.
	 *
	 * @param WorldPos World position
	 * @return True when the point is inside
	 */
	bool Contains(const FVector& WorldPos) const;

	/**
	 * The footprint's axis-aligned box in X and Y.
	 *
	 * @return The box; an invalid box (IsValid false) for an unbounded world
	 */
	FBox2D GetBox2D() const;

	// ==================== Footprint geometry (world modes review M3: WM-I1, WM-I2, WM-I5) ====================
	// All in X and Y; a shell is treated as the disc of its outer radius, like ContainsXY.

	/**
	 * Signed distance in X and Y from (X, Y) to the footprint's edge.
	 *
	 * @param X World X
	 * @param Y World Y
	 * @return Positive inside, negative outside; TNumericLimits<double>::Max() for an unbounded world
	 */
	double GetDistanceToEdgeXY(double X, double Y) const;

	/**
	 * Whether a disc lies wholly inside the footprint: a site, its pad and its falloff ring all stay in the world.
	 *
	 * @param X Disc centre X
	 * @param Y Disc centre Y
	 * @param Radius Disc radius
	 * @return True when the whole disc is inside (always for an unbounded world)
	 */
	bool ContainsCircleXY(double X, double Y, double Radius) const;

	/**
	 * Whether a box in X and Y overlaps the footprint: a map tile or a grid cell that holds some of the world.
	 *
	 * @param Box World-space box in X and Y
	 * @return True when any part of the box is inside (always for an unbounded world)
	 */
	bool IntersectsBox2D(const FBox2D& Box) const;

	/**
	 * The nearest point in X and Y at least Margin inside the edge: (X, Y) itself when it already is.
	 *
	 * @param X World X
	 * @param Y World Y
	 * @param Margin How far inside the edge the result stays (clamped so the footprint does not vanish)
	 * @return The clamped point; (X, Y) for an unbounded world
	 */
	FVector2D ClampXY(double X, double Y, double Margin = 0.0) const;

	/**
	 * The unit direction in X and Y out of the world across the nearest edge: radial for a circle, the nearest side's
	 * normal for a rectangle.
	 *
	 * @param X World X
	 * @param Y World Y
	 * @return Unit vector; zero for an unbounded world
	 */
	FVector2D GetOutwardNormalXY(double X, double Y) const;
};

/**
 * A point's position on the world's surface parameterisation: IVoxelWorldMode::WorldToSurface (spherical requirement
 * SP-A2). The key for every 2D consumer (map tiles, navigation grids, POI cells, explored tiles).
 *
 * Heightmap modes: Face 0, UV = world (X, Y). A planet: one of six cube faces and UV in [-1, 1] on it.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FVoxelSurfaceCoord
{
	GENERATED_BODY()

	/** Cube face (0-5) on a planet; always 0 in heightmap modes */
	UPROPERTY(BlueprintReadWrite, Category = "Voxel|World")
	int32 Face = 0;

	/** Position on the face: world X and Y in heightmap modes, [-1, 1] on a planet's cube face */
	UPROPERTY(BlueprintReadWrite, Category = "Voxel|World")
	FVector2D UV = FVector2D::ZeroVector;

	FVoxelSurfaceCoord() = default;
	FVoxelSurfaceCoord(int32 InFace, const FVector2D& InUV) : Face(InFace), UV(InUV) {}
};

/**
 * What generated a world, recorded with its save so a save is never loaded into a different world (world modes review
 * WM-A4, finding KI7; spherical requirement SP-G9). The save key holds the map and the seed; this holds the rest that
 * decides where terrain, edits and records are: the world mode, the mode's shape params, the voxel grid and the
 * generation version. Built by FVoxelWorldModeFactory::MakeFingerprint; read through
 * UVoxelChunkManager::GetWorldFingerprint.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FVoxelWorldFingerprint
{
	GENERATED_BODY()

	/** World mode */
	UPROPERTY(BlueprintReadOnly, Category = "Voxel|World")
	EWorldMode WorldMode = EWorldMode::InfinitePlane;

	/**
	 * The mode's shape params (island extent, falloff, centre and edge height; planet radius, shell and centre) and the
	 * voxel grid (voxel size, chunk size, world origin), as "Name=Value" pairs joined by ';'. Compared as text.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Voxel|World")
	FString Shape;

	/**
	 * Version of the generation code that produced the terrain (FVoxelWorldModeFactory::GenerationVersion). 0 means
	 * not recorded: a save written before fingerprints existed.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Voxel|World")
	int32 GenerationVersion = 0;

	/** @return True when the fingerprint was recorded (GenerationVersion > 0) */
	bool IsRecorded() const { return GenerationVersion > 0; }

	/**
	 * @param Other Fingerprint to compare with
	 * @return True when the world mode, the shape and the generation version are all equal
	 */
	bool Matches(const FVoxelWorldFingerprint& Other) const;

	/**
	 * What differs from another fingerprint, for logs.
	 *
	 * @param Other Fingerprint to compare with
	 * @return One clause per difference ("world mode IslandBowl vs InfinitePlane; IslandRadius=200000.0 vs 120000.0"),
	 *         empty when they match
	 */
	FString DescribeDifferences(const FVoxelWorldFingerprint& Other) const;

	/** @return "<WorldMode> [<Shape>] gen <GenerationVersion>" */
	FString ToString() const;
};
