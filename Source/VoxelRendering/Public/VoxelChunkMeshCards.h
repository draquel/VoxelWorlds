// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelVertex.h"

class FMeshCardsBuildData;
struct FChunkMeshData;
struct FVoxelPrebuiltChunkMeshCards;

/** How a chunk mesh's Lumen cards are built (G116, voxel.Render.LumenCards) */
enum class EVoxelChunkMeshCards : uint8
{
	/** No cards: the primitive stays out of Lumen's scene */
	None,
	/** Axis-aligned cards over the mesh's box, two per slice of each axis (step 4, BuildVoxelChunkMeshCards) */
	Axis,
	/** Cards fitted to the mesh's surface: one per cluster of surface facing one way at one depth (BuildVoxelChunkMeshFittedCards) */
	Fitted,
};

/**
 * What a chunk mesh's Lumen cards take (G116): the mode and its tuning, read from the voxel.Render.LumenCards cvars by the
 * region component and fixed for the meshes it creates; a change re-sends the meshes.
 */
struct FVoxelChunkMeshCardSettings
{
	EVoxelChunkMeshCards Mode = EVoxelChunkMeshCards::None;

	/** Axis: parts per axis (1..MaxVoxelChunkMeshCardSlices) */
	int32 Slices = 1;

	/** Fitted: cells along the box's longest side of the grid the surface is sampled on (4..64) */
	int32 Resolution = 16;

	/** Fitted: the most cards a mesh keeps (1..32, Lumen's limit a mesh); the clusters covering least go first */
	int32 MaxCards = 16;

	/** Fitted: the fewest surface cells a card under an outer layer covers (an outer card takes half the face's cells at most) */
	int32 MinCells = 4;

	bool HasCards() const { return Mode != EVoxelChunkMeshCards::None; }

	bool operator==(const FVoxelChunkMeshCardSettings& Other) const
	{
		return Mode == Other.Mode && (Mode == EVoxelChunkMeshCards::None
			|| (Mode == EVoxelChunkMeshCards::Axis ? Slices == Other.Slices
				: Resolution == Other.Resolution && MaxCards == Other.MaxCards && MinCells == Other.MinCells));
	}
	bool operator!=(const FVoxelChunkMeshCardSettings& Other) const { return !(*this == Other); }
};

/** The most slices per axis a chunk mesh's axis cards take: Lumen looks up at most 32 cards per mesh (6 a slice) */
constexpr int32 MaxVoxelChunkMeshCardSlices = 5;

/** What one fitted build found (BuildVoxelChunkMeshFittedCards): the surface it sampled and how much of it the cards cover */
struct FVoxelChunkMeshCardBuildStats
{
	/** Surface elements: cells of the grid where a run of surface faces a card direction, summed over the six directions */
	int32 Surfels = 0;
	/** Of them, the ones a kept card captures */
	int32 CoveredSurfels = 0;
	/** Clusters found before MaxCards cut them */
	int32 Clusters = 0;
};

/**
 * Cards fitted to a chunk mesh's surface (G116), any thread: the engine's mesh card build (MeshCardRepresentationUtilities,
 * editor-only and ray traced with Embree) redone over the mesh's own triangles. The triangles are sampled into a grid of
 * cells over the box; for each of the six directions, every column of cells along it is walked from the box's face on
 * that side, and a cell of surface facing that way that follows empty space starts a surface element, which a card can
 * capture from any near plane between the last surface before it and itself. Clusters are then grown greedily, the
 * largest first: the near plane that captures most uncovered elements becomes a card over their cells. So the outer layer
 * gets a card fitted to it, and every layer below it (a cave's ceiling under an overhang's underside, a cave's floor under
 * the ground, a cave's walls) gets its own when it covers at least MinCells.
 *
 * @param Vertices    The mesh's vertices (their positions and normals), in the box's space
 * @param Indices     Triangle indices into Positions
 * @param LocalBounds The mesh's box
 * @param Settings    Resolution, MaxCards and MinCells (the mode is the caller's)
 * @param OutCardData Receives the cards (its Bounds and two-sidedness are the caller's)
 * @param OutStats    Optional: the surface sampled and covered
 */
VOXELRENDERING_API void BuildVoxelChunkMeshFittedCards(TConstArrayView<FVoxelVertex> Vertices, TConstArrayView<uint32> Indices, const FBox& LocalBounds,
	const FVoxelChunkMeshCardSettings& Settings, FMeshCardsBuildData& OutCardData, FVoxelChunkMeshCardBuildStats* OutStats = nullptr);

/**
 * The Lumen cards the chunk meshes take now (the voxel.Render.LumenCards cvars), any thread.
 *
 * @return The mode and its tuning (none when off)
 */
VOXELRENDERING_API FVoxelChunkMeshCardSettings GetVoxelChunkMeshCardSettings();

/**
 * A chunk mesh's fitted cards, built on its mesh worker (any thread) so the upload takes them as they are; the region
 * component uses them when they were fitted with the settings it creates meshes with, and builds anew otherwise.
 *
 * @param MeshData The mesher's mesh (its positions' bounds are the renderer's LocalBounds)
 * @return The cards and the settings they were fitted with; null unless the fitted cards are on (voxel.Render.LumenCards 2,
 *         chunk primitives) and the mesh has triangles
 */
VOXELRENDERING_API TSharedPtr<const FVoxelPrebuiltChunkMeshCards, ESPMode::ThreadSafe> PrebuildVoxelChunkMeshCards(const FChunkMeshData& MeshData);

/**
 * Whether a kept mesh's worker-fitted cards no longer fit the settings (a voxel.Render.LumenCards change since), so its
 * owner fits them again before merging (a seam bucket re-sends its seams' meshes without meshing them again). Any thread.
 *
 * @param MeshData The kept mesh
 * @return True when the fitted cards are on and the mesh has none or cards fitted with other settings
 */
VOXELRENDERING_API bool AreVoxelPrebuiltChunkMeshCardsStale(const FChunkMeshData& MeshData);

/**
 * A mesh made of parts that each had their cards fitted (a seam bucket's seams, each on its seam worker), any thread: the
 * parts' cards together, the MaxCards whose surface is largest, in the union of their boxes.
 *
 * @param Parts The parts' cards (in the merged mesh's space)
 * @return The merged cards; null when a part has none or the parts were fitted with different settings (the upload fits
 *         the merged mesh then)
 */
VOXELRENDERING_API TSharedPtr<const FVoxelPrebuiltChunkMeshCards, ESPMode::ThreadSafe> MergeVoxelPrebuiltChunkMeshCards(
	TConstArrayView<TSharedPtr<const FVoxelPrebuiltChunkMeshCards, ESPMode::ThreadSafe>> Parts);

/**
 * The chunk meshes' Lumen cards (G116), summed over every mesh built since the start or the last reset
 * (voxel.Render.LumenCardStats): any thread adds, any thread reads.
 */
struct VOXELRENDERING_API FVoxelChunkMeshCardCounters
{
	struct FCounts
	{
		/** Meshes built with cards, the cards they got, and for the fitted ones the surface elements sampled and covered */
		int64 Meshes = 0;
		int64 Cards = 0;
		int64 FittedMeshes = 0;
		int64 Surfels = 0;
		int64 CoveredSurfels = 0;
		/** Fitted meshes whose clusters MaxCards cut */
		int64 Capped = 0;
		/** Time spent building the cards on the game thread, and the longest one build took there */
		double Seconds = 0.0;
		double MaxSeconds = 0.0;
		/** Of the meshes, those that took cards a worker fitted; the workers' builds and their time */
		int64 WorkerMeshes = 0;
		int64 WorkerBuilds = 0;
		double WorkerSeconds = 0.0;
	};

	/** @return The counts now */
	static FCounts Get();

	/** Clear every count. */
	static void Reset();

	/**
	 * A mesh's cards were built.
	 *
	 * @param NumCards Its cards
	 * @param Seconds  The build's time
	 * @param Stats    The fitted build's stats (null for axis cards)
	 * @param MaxCards The fitted build's card limit
	 * @param bOnWorker A worker built them (PrebuildVoxelChunkMeshCards, counted by AddWorkerBuild): Seconds is unused
	 */
	static void AddBuild(int32 NumCards, double Seconds, const FVoxelChunkMeshCardBuildStats* Stats, int32 MaxCards, bool bOnWorker = false);

	/**
	 * A worker fitted a mesh's cards (PrebuildVoxelChunkMeshCards).
	 *
	 * @param Seconds The build's time
	 */
	static void AddWorkerBuild(double Seconds);
};
