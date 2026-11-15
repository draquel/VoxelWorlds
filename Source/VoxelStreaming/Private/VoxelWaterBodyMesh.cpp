// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWaterBodyMesh.h"

int32 FVoxelWaterBodyMesh::AddVertex(const FVector& WorldPosition, const FVector& Normal, const FVector& Tangent, const FVector2D& UV, float Speed)
{
	Top.Positions.Add(WorldPosition - Origin);
	Top.Normals.Add(Normal);
	Top.Tangents.Add(Tangent);
	Top.UV0.Add(UV);
	Top.UV1.Add(FVector2D(WaterMaterialID, Speed));
	Top.Colors.Add(FColor(WaterMaterialID, 0, 0, 255));
	return Top.Positions.Num() - 1;
}

void FVoxelWaterBodyMesh::AddQuad(int32 A, int32 B, int32 C, int32 D)
{
	// Counter-clockwise corners face up with (A, C, B), (A, D, C): the water mesher's winding, and the far seas'.
	Top.Triangles.Append({ A, C, B, A, D, C });
	++Quads;
}

void FVoxelWaterBodyMesh::FinishUnderside()
{
	// The same vertices (the top's normal: it shades from below as the water tiles' back faces do), every triangle
	// turned over.
	Underside.Positions = Top.Positions;
	Underside.Normals = Top.Normals;
	Underside.Tangents = Top.Tangents;
	Underside.UV0 = Top.UV0;
	Underside.UV1 = Top.UV1;
	Underside.Colors = Top.Colors;
	Underside.Triangles.Reset(Top.Triangles.Num());
	for (int32 i = 0; i + 2 < Top.Triangles.Num(); i += 3)
	{
		Underside.Triangles.Append({ Top.Triangles[i], Top.Triangles[i + 2], Top.Triangles[i + 1] });
	}
}

bool FVoxelWaterBodyMesh::Build(const FVoxelWaterBody& Body, const FVoxelWaterFrame& Frame, double VoxelSize, FVoxelWaterBodyMesh& Out)
{
	Out.Top.Reset();
	Out.Underside.Reset();
	Out.Quads = 0;
	Out.Origin = FVector::ZeroVector;
	const double Voxel = FMath::Max(VoxelSize, 1.0);
	if (Body.Kind == EVoxelWaterBodyKind::Lake && Body.Lake.IsValid() && Body.Lake->NumCells() > 0 && Body.Lake->IsRadial() == Frame.bRadial)
	{
		BuildLake(Body, Frame, Voxel, Out);
	}
	else if (Body.Kind == EVoxelWaterBodyKind::Reach && Body.Reach.IsValid() && Body.Reach->NumSegments() > 0 && Body.Reach->IsRadial() == Frame.bRadial)
	{
		BuildReach(Body, Frame, Voxel, Out);
	}
	if (Out.IsEmpty())
	{
		return false;
	}
	Out.FinishUnderside();
	return true;
}

void FVoxelWaterBodyMesh::BuildLake(const FVoxelWaterBody& Body, const FVoxelWaterFrame& Frame, double VoxelSize, FVoxelWaterBodyMesh& Out)
{
	const FVoxelWaterLake& Lake = *Body.Lake;
	const FIntPoint Size = Lake.GetSize();
	const double CellSize = Lake.GetCellSize();
	const double Level = Body.Level;
	// A grid point's UV: voxels along the grid's two axes from its minimum corner.
	const auto GridUV = [CellSize, VoxelSize](double GX, double GY) { return FVector2D(GX * CellSize / VoxelSize, GY * CellSize / VoxelSize); };

	if (!Frame.bRadial)
	{
		// A heightmap lake: flat, so the cells merge into rectangles (a row of cells as wide as it runs, then as many
		// rows down as hold the same run), each one quad at the level.
		const FBox2D& Bounds = Lake.GetBounds2D();
		Out.Origin = FVector(Bounds.GetCenter().X, Bounds.GetCenter().Y, Level);
		TBitArray<> Taken(false, Size.X * Size.Y);
		for (int32 Y = 0; Y < Size.Y; ++Y)
		{
			for (int32 X = 0; X < Size.X; ++X)
			{
				if (!Lake.HasCell(X, Y) || Taken[Y * Size.X + X])
				{
					continue;
				}
				int32 Width = 1;
				while (X + Width < Size.X && Lake.HasCell(X + Width, Y) && !Taken[Y * Size.X + X + Width])
				{
					++Width;
				}
				int32 Height = 1;
				for (; Y + Height < Size.Y; ++Height)
				{
					bool bRow = true;
					for (int32 I = 0; I < Width && bRow; ++I)
					{
						bRow = Lake.HasCell(X + I, Y + Height) && !Taken[(Y + Height) * Size.X + X + I];
					}
					if (!bRow)
					{
						break;
					}
				}
				for (int32 J = 0; J < Height; ++J)
				{
					for (int32 I = 0; I < Width; ++I)
					{
						Taken[(Y + J) * Size.X + X + I] = true;
					}
				}
				int32 Corners[4];
				const FIntPoint Grid[4] = { FIntPoint(X, Y), FIntPoint(X + Width, Y), FIntPoint(X + Width, Y + Height), FIntPoint(X, Y + Height) };
				for (int32 Corner = 0; Corner < 4; ++Corner)
				{
					Corners[Corner] = Out.AddVertex(Lake.GetGridPoint(Grid[Corner].X, Grid[Corner].Y, Level, Frame.Center), FVector::UpVector,
						FVector::ForwardVector, GridUV(Grid[Corner].X, Grid[Corner].Y), 0.0f);
				}
				Out.AddQuad(Corners[0], Corners[1], Corners[2], Corners[3]);
			}
		}
		return;
	}

	// A planet lake: the sheet lies on the sphere of the level's radius. A quad a cell, its corners the grid's corners
	// carried out along their own directions and shared between the cells that meet there, so the sheet has no seam.
	FVector AxisX, AxisY, Centre;
	Lake.GetTangentFrame(AxisX, AxisY, Centre);
	Out.Origin = Frame.Center + Centre * Level;
	TMap<FIntPoint, int32> Vertices;
	const auto Corner = [&](int32 GX, int32 GY)
	{
		const FIntPoint Key(GX, GY);
		if (const int32* Found = Vertices.Find(Key))
		{
			return *Found;
		}
		const FVector Position = Lake.GetGridPoint(GX, GY, Level, Frame.Center);
		const FVector Up = (Position - Frame.Center).GetSafeNormal(UE_SMALL_NUMBER, Centre);
		const FVector Tangent = (AxisX - Up * FVector::DotProduct(AxisX, Up)).GetSafeNormal(UE_SMALL_NUMBER, AxisX);
		return Vertices.Add(Key, Out.AddVertex(Position, Up, Tangent, GridUV(GX, GY), 0.0f));
	};
	for (int32 Y = 0; Y < Size.Y; ++Y)
	{
		for (int32 X = 0; X < Size.X; ++X)
		{
			if (Lake.HasCell(X, Y))
			{
				const int32 A = Corner(X, Y);
				const int32 B = Corner(X + 1, Y);
				const int32 C = Corner(X + 1, Y + 1);
				const int32 D = Corner(X, Y + 1);
				Out.AddQuad(A, B, C, D);
			}
		}
	}
}

void FVoxelWaterBodyMesh::BuildReach(const FVoxelWaterBody& Body, const FVoxelWaterFrame& Frame, double VoxelSize, FVoxelWaterBodyMesh& Out)
{
	const FVoxelWaterReach& Reach = *Body.Reach;
	const TArray<FVoxelWaterReachVertex>& Vertices = Reach.GetVertices();
	const int32 Num = Vertices.Num();
	const double Radius = FMath::Max(Frame.DatumRadius, 1.0);
	const float Speed = Reach.GetFlowSpeed();

	// A vertex's point on the datum (world XY at Z 0, or the datum sphere's point less the centre) and its up.
	const auto DatumPoint = [&](int32 Index)
	{
		const FVector& P = Vertices[Index].Position;
		return Frame.bRadial ? P * Radius : FVector(P.X, P.Y, 0.0);
	};
	const auto UpAt = [&](int32 Index) { return Frame.bRadial ? Vertices[Index].Position : FVector::UpVector; };
	// A point of the surface over a datum point: at the level along its own up.
	const auto SurfacePoint = [&](const FVector& Datum, double Level)
	{
		return Frame.bRadial ? Frame.Center + Datum.GetSafeNormal() * Level : FVector(Datum.X, Datum.Y, Level);
	};

	const int32 Middle = Num / 2;
	Out.Origin = SurfacePoint(DatumPoint(Middle), Vertices[Middle].SurfaceLevel);

	double Along = 0.0;
	TArray<int32> Right, Left;
	Right.Reserve(Num);
	Left.Reserve(Num);
	for (int32 Index = 0; Index < Num; ++Index)
	{
		const int32 Before = FMath::Max(Index - 1, 0);
		const int32 After = FMath::Min(Index + 1, Num - 1);
		const FVector Up = UpAt(Index);
		const FVector Here = DatumPoint(Index);
		if (Index > 0)
		{
			Along += FVector::Dist(DatumPoint(Index - 1), Here);
		}
		// The direction downstream along the surface: from the vertex before to the vertex after, across the up.
		FVector Span = DatumPoint(After) - DatumPoint(Before);
		Span -= Up * FVector::DotProduct(Span, Up);
		const double SpanLength = Span.Size();
		const FVector Downstream = SpanLength > UE_DOUBLE_SMALL_NUMBER ? Span / SpanLength : FVector::ForwardVector;
		const FVector Side = FVector::CrossProduct(Up, Downstream);
		// At a bend the two edges keep the half width from both segments: the offset along the bisector grows by
		// one over the cosine of half the turn, up to twice. Where a bend is tighter than the half width the inner
		// edge runs back on itself and the ribbon folds there: the folded triangles lie inside the quads before them
		// and face away, so the surface stays whole and they are not drawn.
		double Miter = 1.0;
		if (Index > 0 && Index < Num - 1)
		{
			FVector Leg = DatumPoint(After) - Here;
			Leg -= Up * FVector::DotProduct(Leg, Up);
			Miter = 1.0 / FMath::Clamp(FVector::DotProduct(Leg.GetSafeNormal(), Downstream), 0.5, 1.0);
		}
		const double HalfWidth = FMath::Max(Vertices[Index].HalfWidth, 0.0) * Miter;
		const double Level = Vertices[Index].SurfaceLevel;
		// Level across the river, sloped along it: the up tilted against the slope (the water levels' own normal).
		const double Slope = SpanLength > UE_DOUBLE_SMALL_NUMBER ? (Vertices[After].SurfaceLevel - Vertices[Before].SurfaceLevel) / SpanLength : 0.0;
		const FVector Normal = (Up - Downstream * Slope).GetSafeNormal(UE_SMALL_NUMBER, Up);
		const double U = Vertices[Index].HalfWidth / VoxelSize;
		const double V = Along / VoxelSize;
		Right.Add(Out.AddVertex(SurfacePoint(Here - Side * HalfWidth, Level), Normal, Downstream, FVector2D(-U, V), Speed));
		Left.Add(Out.AddVertex(SurfacePoint(Here + Side * HalfWidth, Level), Normal, Downstream, FVector2D(U, V), Speed));
	}
	for (int32 Index = 0; Index + 1 < Num; ++Index)
	{
		Out.AddQuad(Right[Index], Right[Index + 1], Left[Index + 1], Left[Index]);
	}
}
