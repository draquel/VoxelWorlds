// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelChunkValleySegments.h"
#include "VoxelNoiseTypes.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelValleyNetwork.h"

void VoxelValleyCarve::AppendRows(const FVoxelValleySegment& Segment, const FVector& Origin, TArray<FVector4f>& Out)
{
	const FVector A = Segment.A - Origin, B = Segment.B - Origin;
	Out.Add(FVector4f(static_cast<float>(A.X), static_cast<float>(A.Y), static_cast<float>(A.Z), static_cast<float>(Segment.FloorA)));
	Out.Add(FVector4f(static_cast<float>(B.X), static_cast<float>(B.Y), static_cast<float>(B.Z), static_cast<float>(Segment.FloorB)));
	Out.Add(FVector4f(Segment.FloorHalfWidth, Segment.ValleyHalfWidth, Segment.TaperA, Segment.TaperB));
}

void FVoxelChunkValleySegments::Build(const FVoxelNoiseGenerationRequest& Request, FVoxelChunkValleySegments& Out)
{
	Out.Data.Reset();
	Out.SeaLevel = -MAX_flt;
	Out.Bounds = FBox3f(ForceInit);
	if (!Request.Valleys.IsValid() || Request.Valleys->IsRadial() != (Request.WorldMode == EWorldMode::SphericalPlanet))
	{
		return;
	}
	const double ChunkWorldSize = static_cast<double>(Request.ChunkSize) * Request.VoxelSize;
	if (Request.WorldMode == EWorldMode::SphericalPlanet)
	{
		// The chunk's cap from the planet centre, as a chord radius around the anchor's datum point (as the conditioning
		// lines' list takes it, FVoxelChunkLineSegments::Build): every voxel's direction lies within asin(half diagonal /
		// |anchor|) of the anchor's. A chunk that holds the centre takes all.
		const double PlanetRadius = Request.SphericalPlanetParams.PlanetRadius;
		const FVector AnchorRelative = Request.GetPlanetAnchorRelative();
		const double AnchorDist = AnchorRelative.Size();
		const FVector AnchorDir = FSphericalPlanetWorldMode::GetDirectionFromCenter(AnchorRelative, FVector::ZeroVector);
		const double HalfDiagonal = 0.5 * ChunkWorldSize * UE_DOUBLE_SQRT_3;
		const double CapChord = AnchorDist <= HalfDiagonal
			? -1.0
			: 2.0 * PlanetRadius * FMath::Sin(0.5 * FMath::Asin(HalfDiagonal / AnchorDist));
		BuildOnSphere(*Request.Valleys, AnchorDir, PlanetRadius, CapChord, Out);
		return;
	}
	const FVoxelValleyGround& Ground = Request.Valleys->GetGround();
	Out.SeaLevel = Ground.bSea ? static_cast<float>(Ground.SeaLevel) : -MAX_flt;
	const FVector ChunkOrigin = Request.GetChunkWorldPosition();
	const FVector Origin(ChunkOrigin.X, ChunkOrigin.Y, 0.0);
	TArray<FVoxelValleySegment> Segments;
	Request.Valleys->GatherSegments(FBox2D(FVector2D(Origin), FVector2D(Origin) + FVector2D(ChunkWorldSize)), Segments);
	Out.Data.Reserve(Segments.Num() * VoxelValleyCarve::SegmentStride);
	for (const FVoxelValleySegment& Segment : Segments)
	{
		VoxelValleyCarve::AppendRows(Segment, Origin, Out.Data);
	}
}

void FVoxelChunkValleySegments::BuildOnSphere(const FVoxelValleyNetwork& Network, const FVector& AnchorDirection, double PlanetRadius, double CapChord,
	FVoxelChunkValleySegments& Out)
{
	Out.Data.Reset();
	Out.Bounds = FBox3f(ForceInit);
	const FVoxelValleyGround& Ground = Network.GetGround();
	Out.SeaLevel = Ground.bSea ? static_cast<float>(Ground.SeaLevel) : -MAX_flt;
	// The anchor's datum point on the network's sphere (the datum radius is the planet's)
	const FVector AnchorPoint = AnchorDirection * PlanetRadius;
	TArray<FVoxelValleySegment> Segments;
	Network.GatherSegmentsNear(AnchorPoint, CapChord, Segments);
	Out.Data.Reserve(Segments.Num() * VoxelValleyCarve::SegmentStride);
	for (const FVoxelValleySegment& Segment : Segments)
	{
		VoxelValleyCarve::AppendRows(Segment, AnchorPoint, Out.Data);
		// The datum points less the anchor's, in double, then narrowed: the shader measures short chords (WM-BP6)
		const FVector3f A(Segment.A - AnchorPoint), B(Segment.B - AnchorPoint);
		const FVector3f Extent(Segment.ValleyHalfWidth);
		Out.Bounds += A.ComponentMin(B) - Extent;
		Out.Bounds += A.ComponentMax(B) + Extent;
	}
}

float FVoxelChunkValleySegments::Apply(const FVector3f& P, float Natural) const
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
		VoxelValleyCarve::SegmentAt(P, Rows[0], Rows[1], Rows[2], Weight, Floor);
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
			const FVector3f P(static_cast<float>(X) * VoxelSize, static_cast<float>(Y) * VoxelSize, 0.0f);
			for (int32 Segment = 0; Segment < Num(); ++Segment)
			{
				float Weight = 0.0f, Floor = 0.0f;
				const FVector4f* Rows = &Data[Segment * VoxelValleyCarve::SegmentStride];
				VoxelValleyCarve::SegmentAt(P, Rows[0], Rows[1], Rows[2], Weight, Floor);
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
