// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelChunkWaterColumns.h"
#include "VoxelNoiseTypes.h"

FVoxelChunkWaterFrame FVoxelChunkWaterFrame::FromRequest(const FVoxelNoiseGenerationRequest& Request)
{
	FVoxelChunkWaterFrame Frame;
	Frame.ChunkWorldPos = Request.GetChunkWorldPosition();
	Frame.ChunkSize = Request.ChunkSize;
	Frame.VoxelSize = Request.VoxelSize;
	Frame.bPlanet = Request.WorldMode == EWorldMode::SphericalPlanet;
	Frame.PlanetCenter = Request.SphericalPlanetParams.PlanetCenter;
	Frame.PlanetRadius = Request.SphericalPlanetParams.PlanetRadius;
	return Frame;
}

FVoxelChunkWaterFrame FVoxelChunkWaterFrame::Make(const FIntVector& ChunkCoord, int32 InChunkSize, float InVoxelSize, const FVector& WorldOrigin,
	bool bInPlanet, const FVector& InPlanetCenter, double InPlanetRadius)
{
	FVoxelChunkWaterFrame Frame;
	// FVoxelNoiseGenerationRequest::GetChunkWorldPosition's own arithmetic: the chunk's world size in float.
	const float ChunkWorldSize = InChunkSize * InVoxelSize;
	Frame.ChunkWorldPos = WorldOrigin + FVector(ChunkCoord) * ChunkWorldSize;
	Frame.ChunkSize = InChunkSize;
	Frame.VoxelSize = InVoxelSize;
	Frame.bPlanet = bInPlanet;
	Frame.PlanetCenter = InPlanetCenter;
	Frame.PlanetRadius = InPlanetRadius;
	return Frame;
}

void FVoxelChunkWaterColumns::SetFrame(const FVoxelChunkWaterFrame& InFrame)
{
	Frame = InFrame;
	Data.Reset();
	FirstColumn = 0;
	const int32 N = Frame.ChunkSize;
	if (!Frame.bPlanet)
	{
		Data.Init(FVector4f(NoWater, NoWater, 0.0f, 0.0f), N * N);
		return;
	}

	// A planet: the lines the post-passes walk, and the anchor the planet shaders work from (the chunk's centre,
	// FVoxelNoiseGenerationRequest::GetPlanetAnchorRelative's arithmetic).
	Lines = FVoxelDownLines::Make(N, FVoxelDownLines::PlanetChunkDown(Frame.ChunkWorldPos, N, Frame.VoxelSize, Frame.PlanetCenter));
	ChunkPlanetRelative = Frame.ChunkWorldPos - Frame.PlanetCenter;
	const float Half = static_cast<float>(N / 2) * Frame.VoxelSize;
	AnchorDist = (ChunkPlanetRelative + FVector(FVector3f(Half, Half, Half))).Size();

	// The shift rows first: the density pass finds a voxel's line from them.
	FirstColumn = N;
	Data.Init(FVector4f(NoWater, NoWater, 0.0f, 0.0f), N + Lines.NumLines());
	for (int32 K = 0; K < N; ++K)
	{
		Data[K] = FVector4f(static_cast<float>(Lines.Shifts[K].X), static_cast<float>(Lines.Shifts[K].Y), 0.0f, 0.0f);
	}
}

FVector4f FVoxelChunkWaterColumns::MakeRow(const FVoxelWaterLevels& Water, const FVector& ReferenceWorldPos) const
{
	FVoxelWaterSample Sample;
	if (!Water.Sample(ReferenceWorldPos, Sample))
	{
		return FVector4f(NoWater, NoWater, 0.0f, 0.0f);
	}
	if (!Frame.bPlanet)
	{
		const float Level = static_cast<float>(Sample.SurfaceLevel);
		return FVector4f(Level, Level, Sample.MinCaveDepth, 0.0f);
	}
	// Formed in double: the level as an altitude over the datum, and over the anchor's distance from the centre.
	return FVector4f(static_cast<float>(Sample.SurfaceLevel - Frame.PlanetRadius), static_cast<float>(Sample.SurfaceLevel - AnchorDist),
		Sample.MinCaveDepth, 0.0f);
}

bool FVoxelChunkWaterColumns::FindLineFirstVoxel(int32 LineId, FIntVector& OutLocal) const
{
	const int32 N = Frame.ChunkSize;
	const int32 Span = 3 * N;
	const int32 LineB = LineId % Span - N;
	const int32 LineC = LineId / Span - N;
	for (int32 K = 0; K < N; ++K)
	{
		const int32 B = LineB + Lines.Shifts[K].X;
		const int32 C = LineC + Lines.Shifts[K].Y;
		if (B >= 0 && B < N && C >= 0 && C < N)
		{
			OutLocal[Lines.AxisA] = Lines.bForward ? K : N - 1 - K;
			OutLocal[(Lines.AxisA + 1) % 3] = B;
			OutLocal[(Lines.AxisA + 2) % 3] = C;
			return true;
		}
	}
	return false;
}

void FVoxelChunkWaterColumns::Build(const FVoxelWaterLevels& Water, const FVoxelChunkWaterFrame& InFrame, FVoxelChunkWaterColumns& Out)
{
	Out.Data.Reset();
	Out.FirstColumn = 0;
	if (Water.IsSeaAlone() || InFrame.ChunkSize <= 0)
	{
		return;
	}
	Out.SetFrame(InFrame);
	const int32 N = InFrame.ChunkSize;
	if (!InFrame.bPlanet)
	{
		// A column's level is the level over its voxels' sample point in the plane.
		for (int32 Y = 0; Y < N; ++Y)
		{
			for (int32 X = 0; X < N; ++X)
			{
				Out.Data[X + Y * N] = Out.MakeRow(Water, InFrame.VoxelWorldPos(FIntVector(X, Y, 0)));
			}
		}
		return;
	}

	// A planet: each line that crosses the chunk takes the level over its first voxel there (ForEach visits a line's
	// voxels in order from its upper side, so the first visit of a line is that voxel).
	TBitArray<> Seen(false, Out.Lines.NumLines());
	Out.Lines.ForEach([&](int32 LineId, int32, const FIntVector& Local)
	{
		if (!Seen[LineId])
		{
			Seen[LineId] = true;
			Out.Data[Out.FirstColumn + LineId] = Out.MakeRow(Water, InFrame.VoxelWorldPos(Local));
		}
	});
}

void FVoxelChunkWaterColumns::Build(const FVoxelNoiseGenerationRequest& Request, FVoxelChunkWaterColumns& Out)
{
	if (!Request.WaterBodies.IsValid())
	{
		Out.Data.Reset();
		Out.FirstColumn = 0;
		return;
	}
	Build(*Request.WaterBodies, FVoxelChunkWaterFrame::FromRequest(Request), Out);
}

FVoxelChunkWaterReader::FVoxelChunkWaterReader(const FVoxelWaterLevels& InWater, const FVoxelChunkWaterFrame& InFrame)
	: Water(InWater)
{
	Columns.SetFrame(InFrame);
	Known.Init(false, Columns.NumColumns());
}

const FVector4f& FVoxelChunkWaterReader::GetRowOf(const FIntVector& Local)
{
	const int32 Column = Columns.ColumnOf(Local);
	FVector4f& Row = Columns.Data[Columns.FirstColumn + Column];
	if (!Known[Column])
	{
		Known[Column] = true;
		++Evaluated;
		const FVoxelChunkWaterFrame& Frame = Columns.GetFrame();
		if (!Frame.bPlanet)
		{
			Row = Columns.MakeRow(Water, Frame.VoxelWorldPos(FIntVector(Local.X, Local.Y, 0)));
		}
		else
		{
			// The line's first voxel in the chunk, as Build takes it; the voxel asked about is on the line, so there is one.
			FIntVector First = Local;
			Columns.FindLineFirstVoxel(Column, First);
			Row = Columns.MakeRow(Water, Frame.VoxelWorldPos(First));
		}
	}
	return Row;
}

bool FVoxelChunkWaterReader::IsVoxelUnder(const FIntVector& Local)
{
	return Columns.IsVoxelUnderLevel(Local, GetRowOf(Local).Y);
}
