// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelChunkLineSegments.h"
#include "VoxelNoiseTypes.h"
#include "SphericalPlanetWorldMode.h"

void FVoxelChunkLineSegments::Build(const FVoxelNoiseGenerationRequest& Request, FVoxelChunkLineSegments& Out)
{
	Out.Data.Reset();
	Out.Bounds = FBox3f(ForceInit);
	Out.ArcRadius = 0.0f;
	const TArray<FVoxelConditioningLineRef>& Lines = Request.Conditioning.Lines;
	if (Lines.Num() == 0)
	{
		return;
	}
	const double ChunkWorldSize = static_cast<double>(Request.ChunkSize) * Request.VoxelSize;

	if (Request.WorldMode == EWorldMode::SphericalPlanet)
	{
		// The chunk's cap from the planet centre, as a chord radius around the anchor's datum point: every voxel's
		// direction lies within asin(half diagonal / |anchor|) of the anchor's. A chunk that holds the centre takes all.
		const double PlanetRadius = Request.SphericalPlanetParams.PlanetRadius;
		const FVector AnchorRelative = Request.GetPlanetAnchorRelative();
		const double AnchorDist = AnchorRelative.Size();
		const FVector AnchorDir = FSphericalPlanetWorldMode::GetDirectionFromCenter(AnchorRelative, FVector::ZeroVector);
		const double HalfDiagonal = 0.5 * ChunkWorldSize * UE_DOUBLE_SQRT_3;
		const double CapChord = AnchorDist <= HalfDiagonal
			? -1.0
			: 2.0 * PlanetRadius * FMath::Sin(0.5 * FMath::Asin(HalfDiagonal / AnchorDist));
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			Lines[Index]->AppendChunkSegmentsOnSphere(AnchorDir, PlanetRadius, CapChord, Index, Out.Data, Out.Bounds);
		}
		Out.ArcRadius = static_cast<float>(PlanetRadius);
		return;
	}

	const FVector ChunkOrigin = Request.GetChunkWorldPosition();
	const FVector2D Origin(ChunkOrigin.X, ChunkOrigin.Y);
	const FBox2D ChunkBox(Origin, Origin + FVector2D(ChunkWorldSize, ChunkWorldSize));
	for (int32 Index = 0; Index < Lines.Num(); ++Index)
	{
		Lines[Index]->AppendChunkSegments(ChunkBox, Origin, Index, Out.Data, Out.Bounds);
	}
}

void FVoxelChunkLineSegments::EvaluateColumns(int32 ChunkSize, float VoxelSize, FVoxelChunkLineColumns& Out) const
{
	Out.First.Reset();
	Out.Blends.Reset();
	if (Num() == 0)
	{
		return;
	}
	Out.First.SetNumUninitialized(ChunkSize * ChunkSize + 1);
	for (int32 Y = 0; Y < ChunkSize; ++Y)
	{
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			Out.First[X + Y * ChunkSize] = Out.Blends.Num();
			// The column's offset from the origin corner, as the shader forms it (float3(VoxelCoord) x VoxelSize).
			const FVector3f Point(static_cast<float>(X) * VoxelSize, static_cast<float>(Y) * VoxelSize, 0.0f);
			if (!MayReach(Point))
			{
				continue;
			}
			ForEachLine(Point, [&Out](EVoxelConditioningLineBlend Blend, float Target, float Weight)
			{
				FVoxelChunkLineColumns::FColumnBlend& Added = Out.Blends.AddDefaulted_GetRef();
				Added.Target = Target;
				Added.Weight = Weight;
				Added.Blend = Blend;
			});
		}
	}
	Out.First[ChunkSize * ChunkSize] = Out.Blends.Num();
}
