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
	/** The cards, relative to the mesh's space */
	TArray<FLumenCardBuildData> Cards;
	/** What the build found, and its time on the worker */
	FVoxelChunkMeshCardBuildStats Stats;
	double Seconds = 0.0;
};
