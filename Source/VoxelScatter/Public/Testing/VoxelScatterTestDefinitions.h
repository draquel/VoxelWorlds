// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelScatterManager.h"
#include "VoxelScatterTypes.h"

/**
 * Scatter definitions for automation tests. Until code review CR1 (D-CR18) the scatter manager made these for itself
 * when a world had no scatter configuration (three for a smooth world, four for a cubic one; no project world had
 * none); the tests that ran on them keep them here. No mesh: a test that wants instances gives each one (the engine
 * cube, say) through AddScatterDefinition.
 */
namespace VoxelScatterTesting
{
	/** Grass (ID 0) on grass, rocks (ID 1) on stone and dirt, trees (ID 2) on grass with a SpawnDistance of 20,000. */
	inline TArray<FScatterDefinition> SmoothDefinitions()
	{
		TArray<FScatterDefinition> Definitions;

		FScatterDefinition Grass;
		Grass.ScatterID = 0;
		Grass.Name = TEXT("Grass");
		Grass.DebugColor = FColor::Green;
		Grass.bEnabled = true;
		Grass.Density = 0.5f;
		Grass.MinSlopeDegrees = 0.0f;
		Grass.MaxSlopeDegrees = 30.0f;
		Grass.AllowedMaterials = { EVoxelMaterial::Grass };
		Grass.bTopFacesOnly = true;
		Grass.ScaleRange = FVector2D(0.7f, 1.3f);
		Grass.bRandomYawRotation = true;
		Grass.bAlignToSurfaceNormal = true;
		Grass.SurfaceOffset = 0.0f;
		Grass.PositionJitter = 25.0f;
		Grass.LODStartDistance = 3000.0f;
		Grass.CullDistance = 8000.0f;
		Grass.MinScreenSize = 0.005f;
		Grass.bCastShadows = false;
		Definitions.Add(Grass);

		FScatterDefinition Rocks;
		Rocks.ScatterID = 1;
		Rocks.Name = TEXT("Rocks");
		Rocks.DebugColor = FColor(128, 128, 128);
		Rocks.bEnabled = true;
		Rocks.Density = 0.05f;
		Rocks.MinSlopeDegrees = 0.0f;
		Rocks.MaxSlopeDegrees = 60.0f;
		Rocks.AllowedMaterials = { EVoxelMaterial::Stone, EVoxelMaterial::Dirt };
		Rocks.bTopFacesOnly = false;
		Rocks.ScaleRange = FVector2D(0.5f, 2.0f);
		Rocks.bRandomYawRotation = true;
		Rocks.bAlignToSurfaceNormal = false;
		Rocks.SurfaceOffset = 0.0f;
		Rocks.PositionJitter = 50.0f;
		Rocks.LODStartDistance = 8000.0f;
		Rocks.CullDistance = 20000.0f;
		Rocks.MinScreenSize = 0.002f;
		Rocks.bCastShadows = true;
		Definitions.Add(Rocks);

		FScatterDefinition Trees;
		Trees.ScatterID = 2;
		Trees.Name = TEXT("Trees");
		Trees.DebugColor = FColor(34, 139, 34);
		Trees.bEnabled = true;
		Trees.Density = 0.02f;
		Trees.MinSlopeDegrees = 0.0f;
		Trees.MaxSlopeDegrees = 20.0f;
		Trees.AllowedMaterials = { EVoxelMaterial::Grass };
		Trees.bTopFacesOnly = true;
		Trees.ScaleRange = FVector2D(0.8f, 1.5f);
		Trees.bRandomYawRotation = true;
		Trees.bAlignToSurfaceNormal = false;
		Trees.SurfaceOffset = 0.0f;
		Trees.PositionJitter = 100.0f;
		Trees.LODStartDistance = 15000.0f;
		Trees.CullDistance = 50000.0f;
		Trees.MinScreenSize = 0.001f;
		Trees.bCastShadows = true;
		Trees.SpawnDistance = 20000.0f;
		Definitions.Add(Trees);

		return Definitions;
	}

	/**
	 * Cubic grass (ID 100) and flowers (101) as cross billboards on grass blocks, rocks (102) on stone and dirt, and the
	 * HISM block tree (10, a VoxelInjection type the TreeMode filters), all block-face snapped.
	 */
	inline TArray<FScatterDefinition> CubicDefinitions()
	{
		TArray<FScatterDefinition> Definitions;

		FScatterDefinition CubicGrass;
		CubicGrass.ScatterID = 100;
		CubicGrass.Name = TEXT("CubicGrass");
		CubicGrass.DebugColor = FColor::Green;
		CubicGrass.bEnabled = true;
		CubicGrass.Density = 0.4f;
		CubicGrass.MinSlopeDegrees = 0.0f;
		CubicGrass.MaxSlopeDegrees = 10.0f;
		CubicGrass.AllowedMaterials = { EVoxelMaterial::Grass };
		CubicGrass.bTopFacesOnly = true;
		CubicGrass.ScaleRange = FVector2D(0.7f, 1.2f);
		CubicGrass.bRandomYawRotation = true;
		CubicGrass.bAlignToSurfaceNormal = false;
		CubicGrass.SurfaceOffset = 0.0f;
		CubicGrass.PositionJitter = 0.0f;
		CubicGrass.MeshType = EScatterMeshType::CrossBillboard;
		CubicGrass.PlacementMode = EScatterPlacementMode::BlockFaceSnap;
		CubicGrass.BillboardWidth = 80.0f;
		CubicGrass.BillboardHeight = 80.0f;
		CubicGrass.bUseBillboardAtlas = true;
		CubicGrass.BillboardAtlasColumn = 0;
		CubicGrass.BillboardAtlasRow = 0;
		CubicGrass.BillboardAtlasColumns = 4;
		CubicGrass.BillboardAtlasRows = 4;
		CubicGrass.LODStartDistance = 3000.0f;
		CubicGrass.CullDistance = 5000.0f;
		CubicGrass.MinScreenSize = 0.005f;
		CubicGrass.bCastShadows = false;
		Definitions.Add(CubicGrass);

		FScatterDefinition CubicFlowers;
		CubicFlowers.ScatterID = 101;
		CubicFlowers.Name = TEXT("CubicFlowers");
		CubicFlowers.DebugColor = FColor::Yellow;
		CubicFlowers.bEnabled = true;
		CubicFlowers.Density = 0.08f;
		CubicFlowers.MinSlopeDegrees = 0.0f;
		CubicFlowers.MaxSlopeDegrees = 10.0f;
		CubicFlowers.AllowedMaterials = { EVoxelMaterial::Grass };
		CubicFlowers.bTopFacesOnly = true;
		CubicFlowers.ScaleRange = FVector2D(0.6f, 1.0f);
		CubicFlowers.bRandomYawRotation = true;
		CubicFlowers.bAlignToSurfaceNormal = false;
		CubicFlowers.SurfaceOffset = 0.0f;
		CubicFlowers.PositionJitter = 0.0f;
		CubicFlowers.MeshType = EScatterMeshType::CrossBillboard;
		CubicFlowers.PlacementMode = EScatterPlacementMode::BlockFaceSnap;
		CubicFlowers.BillboardWidth = 60.0f;
		CubicFlowers.BillboardHeight = 60.0f;
		CubicFlowers.bUseBillboardAtlas = true;
		CubicFlowers.BillboardAtlasColumn = 1;
		CubicFlowers.BillboardAtlasRow = 0;
		CubicFlowers.BillboardAtlasColumns = 4;
		CubicFlowers.BillboardAtlasRows = 4;
		CubicFlowers.LODStartDistance = 2000.0f;
		CubicFlowers.CullDistance = 4000.0f;
		CubicFlowers.MinScreenSize = 0.005f;
		CubicFlowers.bCastShadows = false;
		Definitions.Add(CubicFlowers);

		FScatterDefinition CubicRocks;
		CubicRocks.ScatterID = 102;
		CubicRocks.Name = TEXT("CubicRocks");
		CubicRocks.DebugColor = FColor(128, 128, 128);
		CubicRocks.bEnabled = true;
		CubicRocks.Density = 0.05f;
		CubicRocks.MinSlopeDegrees = 0.0f;
		CubicRocks.MaxSlopeDegrees = 45.0f;
		CubicRocks.AllowedMaterials = { EVoxelMaterial::Stone, EVoxelMaterial::Dirt };
		CubicRocks.bTopFacesOnly = true;
		CubicRocks.ScaleRange = FVector2D(0.5f, 1.5f);
		CubicRocks.bRandomYawRotation = true;
		CubicRocks.bAlignToSurfaceNormal = false;
		CubicRocks.SurfaceOffset = 0.0f;
		CubicRocks.PositionJitter = 0.0f;
		CubicRocks.MeshType = EScatterMeshType::StaticMesh;
		CubicRocks.PlacementMode = EScatterPlacementMode::BlockFaceSnap;
		CubicRocks.LODStartDistance = 8000.0f;
		CubicRocks.CullDistance = 20000.0f;
		CubicRocks.MinScreenSize = 0.002f;
		CubicRocks.bCastShadows = true;
		Definitions.Add(CubicRocks);

		FScatterDefinition HISMTree;
		HISMTree.ScatterID = 10;
		HISMTree.Name = TEXT("HISMBlockTree");
		HISMTree.DebugColor = FColor(34, 139, 34);
		HISMTree.bEnabled = true;
		HISMTree.Density = 0.02f;
		HISMTree.MinSlopeDegrees = 0.0f;
		HISMTree.MaxSlopeDegrees = 20.0f;
		HISMTree.AllowedMaterials = { EVoxelMaterial::Grass };
		HISMTree.bTopFacesOnly = true;
		HISMTree.ScaleRange = FVector2D(0.8f, 1.3f);
		HISMTree.bRandomYawRotation = true;
		HISMTree.bAlignToSurfaceNormal = false;
		HISMTree.SurfaceOffset = 0.0f;
		HISMTree.PositionJitter = 0.0f;
		HISMTree.MeshType = EScatterMeshType::VoxelInjection;
		HISMTree.PlacementMode = EScatterPlacementMode::BlockFaceSnap;
		HISMTree.TreeTemplateID = 0;
		HISMTree.LODStartDistance = 15000.0f;
		HISMTree.CullDistance = 50000.0f;
		HISMTree.MinScreenSize = 0.001f;
		HISMTree.bCastShadows = true;
		HISMTree.SpawnDistance = 20000.0f;
		Definitions.Add(HISMTree);

		return Definitions;
	}

	/** Add SmoothDefinitions() to an initialized manager, before any chunk is handed to it. */
	inline void AddSmoothDefinitions(UVoxelScatterManager& Manager)
	{
		for (const FScatterDefinition& Definition : SmoothDefinitions())
		{
			Manager.AddScatterDefinition(Definition);
		}
	}

	/** Add CubicDefinitions() to an initialized manager of a cubic world, before any chunk is handed to it. */
	inline void AddCubicDefinitions(UVoxelScatterManager& Manager)
	{
		for (const FScatterDefinition& Definition : CubicDefinitions())
		{
			Manager.AddScatterDefinition(Definition);
		}
	}
}
