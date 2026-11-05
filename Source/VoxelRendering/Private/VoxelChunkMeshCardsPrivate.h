// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelChunkMeshCards.h"
#include "MeshCardBuild.h"

/** A chunk mesh's fitted cards from its mesh worker (PrebuildVoxelChunkMeshCards), fixed once built: any thread reads them */
struct FVoxelPrebuiltChunkMeshCards
{
	/** The settings they were fitted with */
	FVoxelChunkMeshCardSettings Settings;
	/** The box they were fitted in (the mesh's positions' bounds) */
	FBox LocalBounds = FBox(ForceInit);
	/** The cards, relative to the mesh's space, the surface elements each captures and their area (uu^2: the elements
	 *  times a cell's face, comparable across meshes) */
	TArray<FLumenCardBuildData> Cards;
	TArray<int32> CardCoverage;
	TArray<float> CardArea;
	/** What the build found */
	FVoxelChunkMeshCardBuildStats Stats;
};
