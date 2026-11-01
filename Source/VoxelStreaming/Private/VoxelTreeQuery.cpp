// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelTreeQuery.h"
#include "VoxelTreeInjector.h"
#include "VoxelWorldConfiguration.h"
#include "IVoxelWorldMode.h"

FVector FVoxelTreeQuery::GlobalVoxelToWorldCentre(const UVoxelWorldConfiguration* Config, const FIntVector& GlobalVoxel)
{
	const float VoxelSize = Config ? Config->VoxelSize : 100.0f;
	const FVector Origin = Config ? Config->WorldOrigin : FVector::ZeroVector;
	return Origin + (FVector(GlobalVoxel) + FVector(0.5)) * VoxelSize;
}

bool FVoxelTreeQuery::FindAt(const UVoxelWorldConfiguration& Config, const IVoxelWorldMode& WorldMode, const FVector& WorldPos,
	FReadVoxel ReadVoxel, FVoxelTreeInstance& OutTree)
{
	const float VoxelSize = Config.VoxelSize;
	const FVector Relative = (WorldPos - Config.WorldOrigin) / VoxelSize;
	const FIntVector GlobalVoxel(FMath::FloorToInt32(Relative.X), FMath::FloorToInt32(Relative.Y), FMath::FloorToInt32(Relative.Z));

	TArray<FVoxelTreeInstance> Candidates;
	FVoxelTreeInjector::FindTreesWithTrunkAt(GlobalVoxel, Config.ChunkSize, VoxelSize, Config.WorldOrigin,
		Config.WorldSeed, Config.TreeTemplates, Config.GetNoiseParams(), WorldMode, Config.TreeDensity,
		Config.BiomeConfiguration, Config.bEnableWaterLevel, Config.WaterLevel, Candidates);

	// The voxel must still be that tree's trunk block; where two trunks touch, the tree with more of its trunk left wins.
	const FVoxelData Voxel = ReadVoxel(GlobalVoxelToWorldCentre(&Config, GlobalVoxel));
	int32 BestTrunk = 0;
	TArray<FVoxelTreeVoxel> Blocks;
	for (const FVoxelTreeInstance& Candidate : Candidates)
	{
		const FVoxelTreeTemplate& Template = Config.TreeTemplates[Candidate.TemplateIndex];
		if (!Voxel.IsSolid() || Voxel.MaterialID != Template.TrunkMaterialID)
		{
			continue;
		}
		GetBlocks(Config, Candidate, ReadVoxel, Blocks);
		const int32 Trunk = Blocks.FilterByPredicate([](const FVoxelTreeVoxel& B) { return B.bTrunk; }).Num();
		if (Trunk > BestTrunk)
		{
			BestTrunk = Trunk;
			OutTree = Candidate;
		}
	}
	return BestTrunk > 0;
}

void FVoxelTreeQuery::FindNear(const UVoxelWorldConfiguration& Config, const IVoxelWorldMode& WorldMode, const FVector& WorldPos, float Radius,
	FReadVoxel ReadVoxel, TArray<FVoxelTreeInstance>& OutTrees)
{
	OutTrees.Reset();
	const float VoxelSize = Config.VoxelSize;
	const int32 ChunkSize = Config.ChunkSize;
	const double ChunkWorldSize = static_cast<double>(ChunkSize) * VoxelSize;
	const FVector Relative = WorldPos - Config.WorldOrigin;
	const int32 CX0 = FMath::FloorToInt32((Relative.X - Radius) / ChunkWorldSize);
	const int32 CX1 = FMath::FloorToInt32((Relative.X + Radius) / ChunkWorldSize);
	const int32 CY0 = FMath::FloorToInt32((Relative.Y - Radius) / ChunkWorldSize);
	const int32 CY1 = FMath::FloorToInt32((Relative.Y + Radius) / ChunkWorldSize);
	TArray<TPair<double, FVoxelTreeInstance>> Found;
	TArray<FVoxelTreeVoxel> Blocks;
	for (int32 CX = CX0; CX <= CX1; ++CX)
	{
		for (int32 CY = CY0; CY <= CY1; ++CY)
		{
			TArray<FIntVector> Positions;
			TArray<int32> Templates;
			TArray<uint32> Seeds;
			FVoxelTreeInjector::ComputeTreePositionsForChunk(FIntVector(CX, CY, 0), ChunkSize, VoxelSize, Config.WorldOrigin,
				Config.WorldSeed, Config.GetNoiseParams(), WorldMode, Config.TreeDensity, Config.TreeTemplates,
				Config.BiomeConfiguration, Config.bEnableWaterLevel, Config.WaterLevel, Positions, Templates, Seeds);
			for (int32 i = 0; i < Positions.Num(); ++i)
			{
				FVoxelTreeInstance Tree;
				Tree.BaseVoxel = Positions[i];
				Tree.TemplateIndex = Templates[i];
				Tree.Seed = Seeds[i];
				const double Distance = FVector::Dist2D(GlobalVoxelToWorldCentre(&Config, Tree.BaseVoxel), WorldPos);
				if (Distance > Radius)
				{
					continue;
				}
				GetBlocks(Config, Tree, ReadVoxel, Blocks);
				if (Blocks.ContainsByPredicate([&Tree](const FVoxelTreeVoxel& B) { return B.bTrunk && B.GlobalVoxel.Z > Tree.BaseVoxel.Z; }))
				{
					Found.Emplace(Distance, Tree);
				}
			}
		}
	}
	Found.Sort([](const TPair<double, FVoxelTreeInstance>& A, const TPair<double, FVoxelTreeInstance>& B) { return A.Key < B.Key; });
	for (const TPair<double, FVoxelTreeInstance>& Entry : Found)
	{
		OutTrees.Add(Entry.Value);
	}
}

void FVoxelTreeQuery::GetBlocks(const UVoxelWorldConfiguration& Config, const FVoxelTreeInstance& Tree, FReadVoxel ReadVoxel,
	TArray<FVoxelTreeVoxel>& OutBlocks)
{
	OutBlocks.Reset();
	if (!Config.TreeTemplates.IsValidIndex(Tree.TemplateIndex))
	{
		return;
	}
	TArray<FVoxelTreeVoxel> Stamp;
	FVoxelTreeInjector::CollectTreeVoxels(Tree.BaseVoxel, Config.TreeTemplates[Tree.TemplateIndex], Tree.Seed, Stamp);
	TSet<FIntVector> Seen;
	// Later entries win (the trunk is stamped after the canopy and punches through it), so walk the stamp backwards.
	for (int32 i = Stamp.Num() - 1; i >= 0; --i)
	{
		const FVoxelTreeVoxel& Block = Stamp[i];
		bool bAlreadySeen = false;
		Seen.Add(Block.GlobalVoxel, &bAlreadySeen);
		if (bAlreadySeen)
		{
			continue;
		}
		const FVoxelData Voxel = ReadVoxel(GlobalVoxelToWorldCentre(&Config, Block.GlobalVoxel));
		if (Voxel.IsSolid() && Voxel.MaterialID == Block.MaterialID)
		{
			OutBlocks.Add(Block);
		}
	}
}
