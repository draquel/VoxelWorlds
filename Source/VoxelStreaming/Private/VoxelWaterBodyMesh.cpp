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

void FVoxelWaterBodyMesh::AddTriangle(int32 A, int32 B, int32 C)
{
	// Counter-clockwise corners face up as (A, C, B), as AddQuad's.
	Top.Triangles.Append({ A, C, B });
	++ShoreTriangles;
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
	Out.ShoreTriangles = 0;
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

	FVector AxisX = FVector::ForwardVector, AxisY = FVector::RightVector, Centre = FVector::UpVector;
	if (Frame.bRadial)
	{
		Lake.GetTangentFrame(AxisX, AxisY, Centre);
		Out.Origin = Frame.Center + Centre * Level;
	}
	else
	{
		const FBox2D& Bounds = Lake.GetBounds2D();
		Out.Origin = FVector(Bounds.GetCenter().X, Bounds.GetCenter().Y, Level);
	}
	// A vertex at a point of the grid (in cells; may be a fraction): at the level, its normal the local up, its tangent
	// along the grid's first axis.
	const auto MakeVertex = [&](double GX, double GY)
	{
		const FVector Position = Lake.GetGridPoint(GX, GY, Level, Frame.Center);
		if (!Frame.bRadial)
		{
			return Out.AddVertex(Position, FVector::UpVector, FVector::ForwardVector, GridUV(GX, GY), 0.0f);
		}
		const FVector Up = (Position - Frame.Center).GetSafeNormal(UE_SMALL_NUMBER, Centre);
		const FVector Tangent = (AxisX - Up * FVector::DotProduct(AxisX, Up)).GetSafeNormal(UE_SMALL_NUMBER, AxisX);
		return Out.AddVertex(Position, Up, Tangent, GridUV(GX, GY), 0.0f);
	};

	// The shore (G229): with the ground at the cells' corners and centres, how far a point of the half grid (corners at
	// even coordinates, a cell's centre at odd ones) lies under the level and its margin. Positive is drawn. A corner on
	// the footprint's outline (a corner of a cell outside it) takes no margin: on a gentle shore the ground at the level
	// and its margin may lie past the footprint's cell of shore, and a sheet drawn to the outline there would show the
	// outline's steps again. So the sheet reaches the outline only where the ground there lies under the level itself
	// (the rim narrower than a cell a hand-placed lake warns about).
	const bool bShore = Lake.HasGround();
	const double Margin = ShoreMarginVoxels * VoxelSize;
	const auto Wetness = [&Lake, Level, Margin](const FIntPoint& Half)
	{
		if (Half.X & 1)
		{
			return Level + Margin - Lake.GetCentreGround(Half.X / 2, Half.Y / 2);
		}
		const int32 GX = Half.X / 2;
		const int32 GY = Half.Y / 2;
		const bool bOutline = !Lake.HasCell(GX - 1, GY - 1) || !Lake.HasCell(GX, GY - 1) || !Lake.HasCell(GX - 1, GY) || !Lake.HasCell(GX, GY);
		return Level + (bOutline ? 0.0 : Margin) - Lake.GetCornerGround(GX, GY);
	};
	// A cell drawn whole: a cell of the footprint, and with the ground every one of its corners and its centre wet.
	const auto IsWhole = [&](int32 X, int32 Y)
	{
		if (!Lake.HasCell(X, Y))
		{
			return false;
		}
		if (!bShore)
		{
			return true;
		}
		const FIntPoint Half(2 * X, 2 * Y);
		return Wetness(Half) > 0.0 && Wetness(Half + FIntPoint(2, 0)) > 0.0 && Wetness(Half + FIntPoint(2, 2)) > 0.0
			&& Wetness(Half + FIntPoint(0, 2)) > 0.0 && Wetness(Half + FIntPoint(1, 1)) > 0.0;
	};

	// Vertices shared between cells, by the points of the half grid they lie between and how far along: a corner is the
	// pair of itself, a cut point the pair of its edge's ends in order, so the two cells beside an edge make one vertex.
	TMap<TPair<FIntPoint, FIntPoint>, int32> Shared;
	// Each shared vertex's point of the grid, in cells: the shore's triangles are judged on the grid's plane.
	TMap<int32, FVector2D> GridOf;
	const auto SharedVertex = [&](FIntPoint A, FIntPoint B, double T)
	{
		if (B.X < A.X || (B.X == A.X && B.Y < A.Y))
		{
			Swap(A, B);
			T = 1.0 - T;
		}
		if (T <= UE_KINDA_SMALL_NUMBER)
		{
			B = A;
		}
		else if (T >= 1.0 - UE_KINDA_SMALL_NUMBER)
		{
			A = B;
		}
		const TPair<FIntPoint, FIntPoint> Key(A, B);
		if (const int32* Found = Shared.Find(Key))
		{
			return *Found;
		}
		const double Along = A == B ? 0.0 : T;
		const double GX = 0.5 * FMath::Lerp(static_cast<double>(A.X), static_cast<double>(B.X), Along);
		const double GY = 0.5 * FMath::Lerp(static_cast<double>(A.Y), static_cast<double>(B.Y), Along);
		const int32 Vertex = MakeVertex(GX, GY);
		Shared.Add(Key, Vertex);
		GridOf.Add(Vertex, FVector2D(GX, GY));
		return Vertex;
	};

	if (!Frame.bRadial)
	{
		// A heightmap lake: flat, so the whole cells merge into rectangles (a row of cells as wide as it runs, then as
		// many rows down as hold the same run), each one quad at the level.
		TBitArray<> Taken(false, Size.X * Size.Y);
		for (int32 Y = 0; Y < Size.Y; ++Y)
		{
			for (int32 X = 0; X < Size.X; ++X)
			{
				if (!IsWhole(X, Y) || Taken[Y * Size.X + X])
				{
					continue;
				}
				int32 Width = 1;
				while (X + Width < Size.X && IsWhole(X + Width, Y) && !Taken[Y * Size.X + X + Width])
				{
					++Width;
				}
				int32 Height = 1;
				for (; Y + Height < Size.Y; ++Height)
				{
					bool bRow = true;
					for (int32 I = 0; I < Width && bRow; ++I)
					{
						bRow = IsWhole(X + I, Y + Height) && !Taken[(Y + Height) * Size.X + X + I];
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
				const int32 A = MakeVertex(X, Y);
				const int32 B = MakeVertex(X + Width, Y);
				const int32 C = MakeVertex(X + Width, Y + Height);
				const int32 D = MakeVertex(X, Y + Height);
				Out.AddQuad(A, B, C, D);
			}
		}
	}
	else
	{
		// A planet lake: the sheet lies on the sphere of the level's radius. A quad a cell, its corners the grid's corners
		// carried out along their own directions and shared between the cells that meet there, so the sheet has no seam.
		for (int32 Y = 0; Y < Size.Y; ++Y)
		{
			for (int32 X = 0; X < Size.X; ++X)
			{
				if (IsWhole(X, Y))
				{
					const FIntPoint Half(2 * X, 2 * Y);
					const int32 A = SharedVertex(Half, Half, 0.0);
					const int32 B = SharedVertex(Half + FIntPoint(2, 0), Half + FIntPoint(2, 0), 0.0);
					const int32 C = SharedVertex(Half + FIntPoint(2, 2), Half + FIntPoint(2, 2), 0.0);
					const int32 D = SharedVertex(Half + FIntPoint(0, 2), Half + FIntPoint(0, 2), 0.0);
					Out.AddQuad(A, B, C, D);
				}
			}
		}
	}
	if (!bShore)
	{
		return;
	}

	// The cells of the shore: four triangles a cell (two corners and the centre, counter-clockwise as the quad's
	// corners), each cut to its wet part where the ground, linear along the triangle's edges, crosses the level and its
	// margin. A triangle cut by a line is a triangle or a quad, drawn as a fan.
	for (int32 Y = 0; Y < Size.Y; ++Y)
	{
		for (int32 X = 0; X < Size.X; ++X)
		{
			if (!Lake.HasCell(X, Y) || IsWhole(X, Y))
			{
				continue;
			}
			const FIntPoint Corners[4] = { FIntPoint(2 * X, 2 * Y), FIntPoint(2 * X + 2, 2 * Y), FIntPoint(2 * X + 2, 2 * Y + 2), FIntPoint(2 * X, 2 * Y + 2) };
			const FIntPoint Middle(2 * X + 1, 2 * Y + 1);
			for (int32 Side = 0; Side < 4; ++Side)
			{
				const FIntPoint Points[3] = { Corners[Side], Corners[(Side + 1) % 4], Middle };
				const double Wet[3] = { Wetness(Points[0]), Wetness(Points[1]), Wetness(Points[2]) };
				TArray<int32, TInlineAllocator<4>> Polygon;
				for (int32 I = 0; I < 3; ++I)
				{
					const int32 J = (I + 1) % 3;
					if (Wet[I] > 0.0)
					{
						Polygon.Add(SharedVertex(Points[I], Points[I], 0.0));
					}
					if ((Wet[I] > 0.0) != (Wet[J] > 0.0))
					{
						Polygon.Add(SharedVertex(Points[I], Points[J], Wet[I] / (Wet[I] - Wet[J])));
					}
				}
				// A sliver (a cut point next to a corner, three points nearly in line) is left out: on a planet the sheet
				// lies on a sphere, and three points nearly in line on the grid's plane make there a triangle whose
				// facing the sphere's curve decides. What it covered is under a ten-thousandth of a cell.
				for (int32 K = 1; K + 1 < Polygon.Num(); ++K)
				{
					const FVector2D A = GridOf.FindRef(Polygon[0]);
					const FVector2D B = GridOf.FindRef(Polygon[K]);
					const FVector2D C = GridOf.FindRef(Polygon[K + 1]);
					if (FVector2D::CrossProduct(B - A, C - A) > 2.0e-4)
					{
						Out.AddTriangle(Polygon[0], Polygon[K], Polygon[K + 1]);
					}
				}
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
