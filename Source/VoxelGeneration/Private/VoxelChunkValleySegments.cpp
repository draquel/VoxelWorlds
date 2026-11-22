// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelChunkValleySegments.h"
#include "VoxelNoiseTypes.h"
#include "VoxelValleyNetwork.h"

void VoxelValleyCarve::AppendRows(const FVoxelValleySegment& Segment, const FVector2D& Origin, TArray<FVector4f>& Out)
{
	const FVector2D A = Segment.A - Origin, B = Segment.B - Origin;
	Out.Add(FVector4f(static_cast<float>(A.X), static_cast<float>(A.Y), static_cast<float>(B.X), static_cast<float>(B.Y)));
	Out.Add(FVector4f(static_cast<float>(Segment.FloorA), static_cast<float>(Segment.FloorB), Segment.FloorHalfWidth, Segment.ValleyHalfWidth));
	Out.Add(FVector4f(Segment.TaperA, Segment.TaperB, 0.0f, 0.0f));
}

void FVoxelChunkValleySegments::Build(const FVoxelNoiseGenerationRequest& Request, FVoxelChunkValleySegments& Out)
{
	Out.Data.Reset();
	Out.SeaLevel = -MAX_flt;
	if (!Request.Valleys.IsValid() || Request.WorldMode == EWorldMode::SphericalPlanet)
	{
		return;
	}
	const FVoxelValleyGround& Ground = Request.Valleys->GetGround();
	Out.SeaLevel = Ground.bSea ? static_cast<float>(Ground.SeaLevel) : -MAX_flt;
	const double ChunkWorldSize = static_cast<double>(Request.ChunkSize) * Request.VoxelSize;
	const FVector ChunkOrigin = Request.GetChunkWorldPosition();
	const FVector2D Origin(ChunkOrigin.X, ChunkOrigin.Y);
	TArray<FVoxelValleySegment> Segments;
	Request.Valleys->GatherSegments(FBox2D(Origin, Origin + FVector2D(ChunkWorldSize)), Segments);
	Out.Data.Reserve(Segments.Num() * VoxelValleyCarve::SegmentStride);
	for (const FVoxelValleySegment& Segment : Segments)
	{
		VoxelValleyCarve::AppendRows(Segment, Origin, Out.Data);
	}
}

float FVoxelChunkValleySegments::Apply(float Px, float Py, float Natural) const
{
	if (Natural < SeaLevel)
	{
		return Natural;
	}
	float Cut = 0.0f, Fill = 0.0f;
	for (int32 Segment = 0; Segment < Num(); ++Segment)
	{
		float Weight = 0.0f, Floor = 0.0f;
		const FVector4f* Rows = &Data[Segment * VoxelValleyCarve::SegmentStride];
		VoxelValleyCarve::SegmentAt(Px, Py, Rows[0], Rows[1], Rows[2], Weight, Floor);
		if (Weight > 0.0f)
		{
			VoxelValleyCarve::Fold(Weight, Floor, Natural, Cut, Fill);
		}
	}
	return VoxelValleyCarve::Finish(Natural, Cut, Fill);
}

void FVoxelChunkValleySegments::EvaluateColumns(int32 ChunkSize, float VoxelSize, FVoxelChunkValleyColumns& Out) const
{
	Out.First.Reset();
	Out.Entries.Reset();
	Out.SeaLevel = SeaLevel;
	if (Num() == 0)
	{
		return;
	}
	Out.First.SetNumUninitialized(ChunkSize * ChunkSize + 1);
	for (int32 Y = 0; Y < ChunkSize; ++Y)
	{
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			const int32 Column = X + Y * ChunkSize;
			Out.First[Column] = Out.Entries.Num();
			// The column's offset from the origin corner, as the shader forms it (float3(VoxelCoord) x VoxelSize)
			const float Px = static_cast<float>(X) * VoxelSize, Py = static_cast<float>(Y) * VoxelSize;
			for (int32 Segment = 0; Segment < Num(); ++Segment)
			{
				float Weight = 0.0f, Floor = 0.0f;
				const FVector4f* Rows = &Data[Segment * VoxelValleyCarve::SegmentStride];
				VoxelValleyCarve::SegmentAt(Px, Py, Rows[0], Rows[1], Rows[2], Weight, Floor);
				if (Weight > 0.0f)
				{
					Out.Entries.Add(FVector2f(Weight, Floor));
				}
			}
		}
	}
	Out.First[ChunkSize * ChunkSize] = Out.Entries.Num();
	if (Out.Entries.Num() == 0)
	{
		Out.First.Reset();
	}
}
