// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelFarOcean.h"

namespace VoxelFarOceanPrivate
{
	/** The water tiles' material id (FVoxelWaterMesher::WATER_MATERIAL_ID): the water material may read it */
	constexpr uint8 WaterMaterialID = 254;

	/** Builds water quads (two-sided) and terrain quads into one FChunkMeshData */
	struct FQuadWriter
	{
		FChunkMeshData& Mesh;
		FVector Origin;
		double VoxelSize;

		FQuadWriter(FChunkMeshData& InMesh, const FVector& InOrigin, double InVoxelSize)
			: Mesh(InMesh), Origin(InOrigin), VoxelSize(FMath::Max(InVoxelSize, 1.0))
		{
		}

		int32 AddVertex(const FVector& World, const FVector3f& Normal, uint8 MaterialID)
		{
			const int32 Index = Mesh.Positions.Num();
			Mesh.Positions.Add(FVector3f(World - Origin));
			Mesh.Normals.Add(Normal);
			// World X and Y over the voxel size, as the water tiles' UVs (a seamless pattern across the two).
			Mesh.UVs.Add(FVector2f(static_cast<float>(World.X / VoxelSize), static_cast<float>(World.Y / VoxelSize)));
			Mesh.UV1s.Add(FVector2f(static_cast<float>(MaterialID), 0.0f));
			Mesh.Colors.Add(FColor(MaterialID, 0, 0, 255));
			return Index;
		}

		/**
		 * A flat water quad at Z with corners A, B, C, D counter-clockwise in X and Y, facing up, or facing down for an
		 * underside (with the up normal: it then shades from below as the water tiles do, which the terrain proxy draws
		 * without back-face culling).
		 */
		void AddWaterQuad(const FVector2D& A, const FVector2D& B, const FVector2D& C, const FVector2D& D, double Z, bool bDown = false)
		{
			const FVector2D Corners[4] = { A, B, C, D };
			int32 V[4];
			for (int32 i = 0; i < 4; ++i)
			{
				V[i] = AddVertex(FVector(Corners[i].X, Corners[i].Y, Z), FVector3f(0, 0, 1), WaterMaterialID);
			}
			// Counter-clockwise corners face up with (A, C, B), (A, D, C): the water mesher's winding.
			if (bDown)
			{
				Mesh.Indices.Append({ (uint32)V[0], (uint32)V[1], (uint32)V[2], (uint32)V[0], (uint32)V[2], (uint32)V[3] });
			}
			else
			{
				Mesh.Indices.Append({ (uint32)V[0], (uint32)V[2], (uint32)V[1], (uint32)V[0], (uint32)V[3], (uint32)V[2] });
			}
		}

		/** An up-facing triangle from vertex indices given counter-clockwise in X and Y. */
		void AddUpTriangle(int32 A, int32 B, int32 C)
		{
			Mesh.Indices.Append({ (uint32)A, (uint32)C, (uint32)B });
		}
	};

	/** Corner I (0-3, counter-clockwise from the minimum) of a rectangle, and a point along side I from corner I to I+1. */
	FVector2D Corner(const FBox2D& Box, int32 I)
	{
		switch (I & 3)
		{
		case 0: return FVector2D(Box.Min.X, Box.Min.Y);
		case 1: return FVector2D(Box.Max.X, Box.Min.Y);
		case 2: return FVector2D(Box.Max.X, Box.Max.Y);
		default: return FVector2D(Box.Min.X, Box.Max.Y);
		}
	}

	FVector2D AlongSide(const FBox2D& Box, int32 Side, double S)
	{
		return FMath::Lerp(Corner(Box, Side), Corner(Box, Side + 1), S);
	}

	FBox2D LerpBox(const FBox2D& Inner, const FBox2D& Outer, double T)
	{
		return FBox2D(FMath::Lerp(Inner.Min, Outer.Min, T), FMath::Lerp(Inner.Max, Outer.Max, T));
	}

	/** Ring parameters from 0 to 1 that widen geometrically (each ring Growth times the one inside it). */
	TArray<double> GeometricSteps(int32 Count, double Growth)
	{
		TArray<double> Steps;
		Steps.Reserve(Count + 1);
		const double Total = (FMath::Pow(Growth, Count) - 1.0) / (Growth - 1.0);
		double Sum = 0.0;
		Steps.Add(0.0);
		for (int32 i = 0; i < Count; ++i)
		{
			Sum += FMath::Pow(Growth, i);
			Steps.Add(i == Count - 1 ? 1.0 : Sum / Total);
		}
		return Steps;
	}
}

bool FVoxelFarOcean::GetGridColumns(const FVoxelFarOceanSettings& Settings, FIntPoint& OutMin, FIntPoint& OutMax)
{
	const FBox2D Box = Settings.Bounds.GetBox2D();
	if (!Settings.Bounds.IsBounded() || !Box.bIsValid || Settings.ColumnSize <= 0.0)
	{
		return false;
	}
	OutMin = FIntPoint(
		FMath::FloorToInt32((Box.Min.X - Settings.GridOrigin.X) / Settings.ColumnSize) - GridMarginColumns,
		FMath::FloorToInt32((Box.Min.Y - Settings.GridOrigin.Y) / Settings.ColumnSize) - GridMarginColumns);
	OutMax = FIntPoint(
		FMath::FloorToInt32((Box.Max.X - Settings.GridOrigin.X) / Settings.ColumnSize) + GridMarginColumns,
		FMath::FloorToInt32((Box.Max.Y - Settings.GridOrigin.Y) / Settings.ColumnSize) + GridMarginColumns);
	return true;
}

bool FVoxelFarOcean::GetPartColumns(const FVoxelFarOceanSettings& Settings, EVoxelFarOceanGridPart Part, const FIntRect& Window,
	FIntPoint& OutMin, FIntPoint& OutMax)
{
	if (!GetGridColumns(Settings, OutMin, OutMax))
	{
		return false;
	}
	if (Part == EVoxelFarOceanGridPart::Window)
	{
		OutMin = FIntPoint(FMath::Max(OutMin.X, Window.Min.X), FMath::Max(OutMin.Y, Window.Min.Y));
		OutMax = FIntPoint(FMath::Min(OutMax.X, Window.Max.X - 1), FMath::Min(OutMax.Y, Window.Max.Y - 1));
	}
	return OutMin.X <= OutMax.X && OutMin.Y <= OutMax.Y;
}

FBox2D FVoxelFarOcean::GetOuterBox(const FVoxelFarOceanSettings& Settings)
{
	FBox2D Box = Settings.Bounds.GetBox2D();
	if (!Box.bIsValid)
	{
		return FBox2D(ForceInit);
	}
	return Box.ExpandBy(FMath::Max(Settings.OuterDistance, 0.0));
}

int32 FVoxelFarOcean::BuildGridWater(const FVoxelFarOceanSettings& Settings, const TSet<FIntPoint>& OpenColumns, FChunkMeshData& OutMesh,
	FChunkMeshData* OutUnderside, EVoxelFarOceanGridPart Part, const FIntRect& Window)
{
	using namespace VoxelFarOceanPrivate;
	OutMesh.Reset();
	if (OutUnderside)
	{
		OutUnderside->Reset();
	}
	FIntPoint Min, Max;
	if (!GetPartColumns(Settings, Part, Window, Min, Max))
	{
		return 0;
	}
	const int32 W = Max.X - Min.X + 1;
	const int32 H = Max.Y - Min.Y + 1;

	// Covered cells: every grid column the streaming has not opened (and, with InlandCoverage, not deep inland).
	TBitArray<> Covered(true, W * H);
	if (Settings.InlandCoverage >= 0.0)
	{
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const double X0 = Settings.GridOrigin.X + (Min.X + X) * Settings.ColumnSize;
				const double Y0 = Settings.GridOrigin.Y + (Min.Y + Y) * Settings.ColumnSize;
				double Farthest = -TNumericLimits<double>::Max();
				for (const FVector2D& P : { FVector2D(X0, Y0), FVector2D(X0 + Settings.ColumnSize, Y0),
					FVector2D(X0, Y0 + Settings.ColumnSize), FVector2D(X0 + Settings.ColumnSize, Y0 + Settings.ColumnSize) })
				{
					Farthest = FMath::Max(Farthest, GetDistancePastEdge(Settings.Bounds, P.X, P.Y));
				}
				Covered[Y * W + X] = Farthest >= -Settings.InlandCoverage;
			}
		}
	}
	for (const FIntPoint& Open : OpenColumns)
	{
		if (Open.X >= Min.X && Open.X <= Max.X && Open.Y >= Min.Y && Open.Y <= Max.Y)
		{
			Covered[(Open.Y - Min.Y) * W + (Open.X - Min.X)] = false;
		}
	}
	if (Part == EVoxelFarOceanGridPart::Frame)
	{
		// The window's columns belong to the other part.
		for (int32 Y = FMath::Max(Window.Min.Y, Min.Y); Y < FMath::Min(Window.Max.Y, Max.Y + 1); ++Y)
		{
			for (int32 X = FMath::Max(Window.Min.X, Min.X); X < FMath::Min(Window.Max.X, Max.X + 1); ++X)
			{
				Covered[(Y - Min.Y) * W + (X - Min.X)] = false;
			}
		}
	}

	// Greedy rectangles (as the water mesher merges its cells), at most MaxRunColumns a side.
	FQuadWriter Writer(OutMesh, Settings.GetMeshOrigin(), Settings.VoxelSize);
	FChunkMeshData Unused;
	FQuadWriter Under(OutUnderside ? *OutUnderside : Unused, Settings.GetMeshOrigin(), Settings.VoxelSize);
	TBitArray<> Done(false, W * H);
	int32 Quads = 0;
	for (int32 Y = 0; Y < H; ++Y)
	{
		for (int32 X = 0; X < W; ++X)
		{
			const int32 Index = Y * W + X;
			if (!Covered[Index] || Done[Index])
			{
				continue;
			}
			int32 RunX = 1;
			while (X + RunX < W && RunX < MaxRunColumns && Covered[Index + RunX] && !Done[Index + RunX])
			{
				++RunX;
			}
			int32 RunY = 1;
			while (Y + RunY < H && RunY < MaxRunColumns)
			{
				bool bRowFull = true;
				for (int32 DX = 0; DX < RunX && bRowFull; ++DX)
				{
					const int32 Next = (Y + RunY) * W + X + DX;
					bRowFull = Covered[Next] && !Done[Next];
				}
				if (!bRowFull)
				{
					break;
				}
				++RunY;
			}
			for (int32 DY = 0; DY < RunY; ++DY)
			{
				for (int32 DX = 0; DX < RunX; ++DX)
				{
					Done[(Y + DY) * W + X + DX] = true;
				}
			}
			const double X0 = Settings.GridOrigin.X + (Min.X + X) * Settings.ColumnSize;
			const double Y0 = Settings.GridOrigin.Y + (Min.Y + Y) * Settings.ColumnSize;
			const double X1 = X0 + RunX * Settings.ColumnSize;
			const double Y1 = Y0 + RunY * Settings.ColumnSize;
			Writer.AddWaterQuad(FVector2D(X0, Y0), FVector2D(X1, Y0), FVector2D(X1, Y1), FVector2D(X0, Y1), Settings.WaterZ);
			if (OutUnderside)
			{
				Under.AddWaterQuad(FVector2D(X0, Y0), FVector2D(X1, Y0), FVector2D(X1, Y1), FVector2D(X0, Y1), Settings.WaterZ, /*bDown*/ true);
			}
			++Quads;
		}
	}
	return Quads;
}

int32 FVoxelFarOcean::BuildGridFloor(const FVoxelFarOceanSettings& Settings, const TSet<FIntPoint>& OpenColumns, FChunkMeshData& OutMesh,
	EVoxelFarOceanGridPart Part, const FIntRect& Window)
{
	using namespace VoxelFarOceanPrivate;
	OutMesh.Reset();
	FIntPoint Min, Max;
	if (!GetPartColumns(Settings, Part, Window, Min, Max) || !Settings.SeabedHeight)
	{
		return 0;
	}
	const int32 W = Max.X - Min.X + 1;
	const int32 H = Max.Y - Min.Y + 1;
	const double Size = Settings.ColumnSize;
	const double Drop = Settings.SeabedOffset + Settings.FloorDrop;
	auto ColumnMin = [&](int32 X, int32 Y) { return FVector2D(Settings.GridOrigin.X + (Min.X + X) * Size, Settings.GridOrigin.Y + (Min.Y + Y) * Size); };
	auto HeightAt = [&](const FVector2D& P)
	{
		const double D = GetDistancePastEdge(Settings.Bounds, P.X, P.Y);
		return (D <= 0.0 ? Settings.EdgeZ : Settings.SeabedHeight(D)) - Drop;
	};

	// 0 = no floor, 1 = flat (wholly inside the edge), 2 = crosses the edge.
	TArray<uint8> Kind;
	Kind.SetNumZeroed(W * H);
	for (int32 Y = 0; Y < H; ++Y)
	{
		for (int32 X = 0; X < W; ++X)
		{
			const FIntPoint Column(Min.X + X, Min.Y + Y);
			if (OpenColumns.Contains(Column) || (Part == EVoxelFarOceanGridPart::Frame && Window.Contains(Column)))
			{
				continue;
			}
			const FVector2D Lo = ColumnMin(X, Y);
			if (!Settings.Bounds.IntersectsBox2D(FBox2D(Lo, Lo + FVector2D(Size))))
			{
				continue; // wholly outside: the static seabed is there
			}
			double Farthest = -TNumericLimits<double>::Max();
			for (const FVector2D& P : { Lo, Lo + FVector2D(Size, 0.0), Lo + FVector2D(0.0, Size), Lo + FVector2D(Size) })
			{
				Farthest = FMath::Max(Farthest, GetDistancePastEdge(Settings.Bounds, P.X, P.Y));
			}
			if (Settings.InlandCoverage >= 0.0 && Farthest < -Settings.InlandCoverage)
			{
				continue; // the grid water leaves it empty too (InlandCoverage): no floor without water over it
			}
			Kind[Y * W + X] = Farthest <= 0.0 ? 1 : 2;
		}
	}

	FQuadWriter Writer(OutMesh, Settings.GetMeshOrigin(), Settings.VoxelSize);
	const uint8 Material = Settings.SeabedMaterialID;
	auto AddFloorQuad = [&](const FVector2D& Lo, const FVector2D& Hi)
	{
		const FVector2D C[4] = { Lo, FVector2D(Hi.X, Lo.Y), Hi, FVector2D(Lo.X, Hi.Y) };
		int32 V[4];
		for (int32 i = 0; i < 4; ++i)
		{
			V[i] = Writer.AddVertex(FVector(C[i].X, C[i].Y, HeightAt(C[i])), FVector3f(0, 0, 1), Material);
		}
		Writer.AddUpTriangle(V[0], V[1], V[2]);
		Writer.AddUpTriangle(V[0], V[2], V[3]);
	};

	// Flat cells merge (greedy, at most MaxRunColumns a side); cells across the edge are split 2 x 2. Within a column of
	// the edge the profile's smoothstep is still concave (its first half), so the quads stay under it.
	TBitArray<> Done(false, W * H);
	int32 Triangles = 0;
	for (int32 Y = 0; Y < H; ++Y)
	{
		for (int32 X = 0; X < W; ++X)
		{
			const int32 Index = Y * W + X;
			if (Kind[Index] == 0 || Done[Index])
			{
				continue;
			}
			if (Kind[Index] == 2)
			{
				Done[Index] = true;
				const FVector2D Lo = ColumnMin(X, Y);
				constexpr int32 Split = 2;
				for (int32 SY = 0; SY < Split; ++SY)
				{
					for (int32 SX = 0; SX < Split; ++SX)
					{
						const FVector2D A = Lo + FVector2D(SX, SY) * (Size / Split);
						AddFloorQuad(A, A + FVector2D(Size / Split));
						Triangles += 2;
					}
				}
				continue;
			}
			int32 RunX = 1;
			while (X + RunX < W && RunX < MaxRunColumns && Kind[Index + RunX] == 1 && !Done[Index + RunX])
			{
				++RunX;
			}
			int32 RunY = 1;
			while (Y + RunY < H && RunY < MaxRunColumns)
			{
				bool bRowFull = true;
				for (int32 DX = 0; DX < RunX && bRowFull; ++DX)
				{
					const int32 Next = (Y + RunY) * W + X + DX;
					bRowFull = Kind[Next] == 1 && !Done[Next];
				}
				if (!bRowFull)
				{
					break;
				}
				++RunY;
			}
			for (int32 DY = 0; DY < RunY; ++DY)
			{
				for (int32 DX = 0; DX < RunX; ++DX)
				{
					Done[(Y + DY) * W + X + DX] = true;
				}
			}
			AddFloorQuad(ColumnMin(X, Y), ColumnMin(X + RunX, Y + RunY));
			Triangles += 2;
		}
	}
	return Triangles;
}

int32 FVoxelFarOcean::BuildOpenSeaWater(const FVoxelFarOceanSettings& Settings, FChunkMeshData& OutMesh, FChunkMeshData* OutUnderside)
{
	using namespace VoxelFarOceanPrivate;
	OutMesh.Reset();
	if (OutUnderside)
	{
		OutUnderside->Reset();
	}
	FIntPoint Min, Max;
	if (!GetGridColumns(Settings, Min, Max))
	{
		return 0;
	}
	const FBox2D Inner(
		FVector2D(Settings.GridOrigin.X + Min.X * Settings.ColumnSize, Settings.GridOrigin.Y + Min.Y * Settings.ColumnSize),
		FVector2D(Settings.GridOrigin.X + (Max.X + 1) * Settings.ColumnSize, Settings.GridOrigin.Y + (Max.Y + 1) * Settings.ColumnSize));
	const FBox2D Outer = GetOuterBox(Settings);
	if (!Outer.bIsValid || !Outer.IsInside(Inner))
	{
		return 0;
	}

	// The frame between the two rectangles, split along the corner diagonals into four trapezoids per ring; every
	// ring has the same segments per side, so neighbouring rings share their vertices.
	FQuadWriter Writer(OutMesh, Settings.GetMeshOrigin(), Settings.VoxelSize);
	FChunkMeshData Unused;
	FQuadWriter Under(OutUnderside ? *OutUnderside : Unused, Settings.GetMeshOrigin(), Settings.VoxelSize);
	const TArray<double> Steps = GeometricSteps(OpenSeaRings, 1.25);
	int32 Quads = 0;
	for (int32 Ring = 0; Ring < OpenSeaRings; ++Ring)
	{
		const FBox2D In = LerpBox(Inner, Outer, Steps[Ring]);
		const FBox2D Out = LerpBox(Inner, Outer, Steps[Ring + 1]);
		for (int32 Side = 0; Side < 4; ++Side)
		{
			for (int32 Seg = 0; Seg < OpenSeaSegmentsPerSide; ++Seg)
			{
				const double S0 = static_cast<double>(Seg) / OpenSeaSegmentsPerSide;
				const double S1 = static_cast<double>(Seg + 1) / OpenSeaSegmentsPerSide;
				// Counter-clockwise: inner S0, inner S1, outer S1, outer S0 runs the right way on every side because
				// the sides themselves run counter-clockwise and "outer" is to their right.
				const FVector2D A = AlongSide(In, Side, S0);
				const FVector2D B = AlongSide(Out, Side, S0);
				const FVector2D C = AlongSide(Out, Side, S1);
				const FVector2D D = AlongSide(In, Side, S1);
				Writer.AddWaterQuad(A, B, C, D, Settings.WaterZ);
				if (OutUnderside)
				{
					Under.AddWaterQuad(A, B, C, D, Settings.WaterZ, /*bDown*/ true);
				}
				++Quads;
			}
		}
	}
	return Quads;
}

double FVoxelFarOcean::GetDistancePastEdge(const FVoxelWorldBounds& Bounds, double X, double Y)
{
	const double DX = FMath::Abs(X - Bounds.Center.X);
	const double DY = FMath::Abs(Y - Bounds.Center.Y);
	if (Bounds.Shape == EVoxelWorldBoundsShape::Rectangle)
	{
		return FMath::Max(DX - Bounds.HalfExtent.X, DY - Bounds.HalfExtent.Y);
	}
	return FMath::Sqrt(DX * DX + DY * DY) - Bounds.OuterRadius;
}

int32 FVoxelFarOcean::BuildSeabed(const FVoxelFarOceanSettings& Settings, FChunkMeshData& OutMesh)
{
	using namespace VoxelFarOceanPrivate;
	OutMesh.Reset();
	const FBox2D Outer = GetOuterBox(Settings);
	if (!Settings.SeabedHeight || !Outer.bIsValid
		|| (Settings.Bounds.Shape != EVoxelWorldBoundsShape::Circle && Settings.Bounds.Shape != EVoxelWorldBoundsShape::Rectangle))
	{
		return 0;
	}
	// Distances past the edge of each ring: across the slope evenly, then widening out to the open sea's corners.
	const FVector2D C(Settings.Bounds.Center.X, Settings.Bounds.Center.Y);
	const double FarthestCorner = FMath::Max(
		FMath::Max(FVector2D::Distance(C, Outer.Min), FVector2D::Distance(C, Outer.Max)),
		FMath::Max(FVector2D::Distance(C, FVector2D(Outer.Min.X, Outer.Max.Y)), FVector2D::Distance(C, FVector2D(Outer.Max.X, Outer.Min.Y))));
	const bool bCircle = Settings.Bounds.Shape == EVoxelWorldBoundsShape::Circle;
	// A circle's rings reach the outer box's corners; a rectangle's contours are the outer box itself at OuterDistance.
	const double MaxDistance = FMath::Max(bCircle ? FarthestCorner - Settings.Bounds.OuterRadius : Settings.OuterDistance, 1.0);
	const double Slope = FMath::Clamp(Settings.SeabedSlopeWidth, 0.0, MaxDistance);
	TArray<double> Distances;
	if (Slope > 0.0)
	{
		for (int32 i = 0; i <= SeabedSlopeRings; ++i)
		{
			Distances.Add(Slope * i / SeabedSlopeRings);
		}
	}
	else
	{
		Distances.Add(0.0);
	}
	const TArray<double> Far = GeometricSteps(SeabedFlatRings, 1.35);
	for (int32 i = 1; i < Far.Num(); ++i)
	{
		Distances.Add(FMath::Lerp(Slope, MaxDistance, Far[i]));
	}

	// The profile's slope, for normals.
	auto HeightAt = [&Settings](double D) { return Settings.SeabedHeight(FMath::Max(D, 0.0)) - Settings.SeabedOffset; };
	auto SlopeAt = [&HeightAt](double D) { return (HeightAt(D + 25.0) - HeightAt(FMath::Max(D - 25.0, 0.0))) / (D + 25.0 - FMath::Max(D - 25.0, 0.0)); };

	FQuadWriter Writer(OutMesh, Settings.GetMeshOrigin(), Settings.VoxelSize);
	const uint8 Material = Settings.SeabedMaterialID;
	auto AddSeabedVertex = [&](const FVector2D& P, double D, const FVector2D& Outward)
	{
		const double Dh = SlopeAt(D);
		const FVector Normal = FVector(-Dh * Outward.X, -Dh * Outward.Y, 1.0).GetSafeNormal();
		return Writer.AddVertex(FVector(P.X, P.Y, HeightAt(D)), FVector3f(Normal), Material);
	};

	int32 Triangles = 0;
	if (bCircle)
	{
		// Circumscribed polygons: every chord lies outside its circle, so no seabed is drawn inside the edge (where the
		// falloff's terrain may dip below EdgeHeight).
		const double Radius = Settings.Bounds.OuterRadius;
		const double Chord = FMath::Max(Settings.MaxChordError, 1.0);
		const int32 Segments = FMath::Clamp(
			FMath::CeilToInt32(UE_DOUBLE_PI / FMath::Acos(FMath::Max(1.0 - Chord / FMath::Max(Radius, Chord), -1.0))),
			MinCircleSegments, MaxCircleSegments);
		const double Circumscribe = 1.0 / FMath::Cos(UE_DOUBLE_PI / Segments);
		TArray<int32> Previous;
		TArray<int32> Current;
		for (int32 Ring = 0; Ring < Distances.Num(); ++Ring)
		{
			Current.Reset();
			const double R = (Radius + Distances[Ring]) * Circumscribe;
			for (int32 i = 0; i < Segments; ++i)
			{
				const double Angle = UE_DOUBLE_TWO_PI * i / Segments;
				const FVector2D Dir(FMath::Cos(Angle), FMath::Sin(Angle));
				const FVector2D P = C + Dir * R;
				// Heights from each vertex's own distance past the edge (a polygon vertex lies a little farther out).
				Current.Add(AddSeabedVertex(P, GetDistancePastEdge(Settings.Bounds, P.X, P.Y), Dir));
			}
			if (Ring > 0)
			{
				for (int32 i = 0; i < Segments; ++i)
				{
					const int32 Next = (i + 1) % Segments;
					// Counter-clockwise in X and Y: inner i, outer i, outer next, inner next.
					Writer.AddUpTriangle(Previous[i], Current[i], Current[Next]);
					Writer.AddUpTriangle(Previous[i], Current[Next], Previous[Next]);
					Triangles += 2;
				}
			}
			Swap(Previous, Current);
		}
	}
	else
	{
		// Rectangle: the contours are rectangles grown by the distance on both axes (the falloff's larger-axis metric).
		constexpr int32 SegmentsPerSide = 16;
		const FBox2D Edge = Settings.Bounds.GetBox2D();
		TArray<int32> Previous;
		TArray<int32> Current;
		for (int32 Ring = 0; Ring < Distances.Num(); ++Ring)
		{
			Current.Reset();
			const FBox2D Box = Edge.ExpandBy(Distances[Ring]);
			for (int32 Side = 0; Side < 4; ++Side)
			{
				const FVector2D Outward = Side == 0 ? FVector2D(0, -1) : Side == 1 ? FVector2D(1, 0) : Side == 2 ? FVector2D(0, 1) : FVector2D(-1, 0);
				for (int32 Seg = 0; Seg < SegmentsPerSide; ++Seg)
				{
					const FVector2D P = AlongSide(Box, Side, static_cast<double>(Seg) / SegmentsPerSide);
					Current.Add(AddSeabedVertex(P, Distances[Ring], Outward));
				}
			}
			if (Ring > 0)
			{
				const int32 N = Current.Num();
				for (int32 i = 0; i < N; ++i)
				{
					const int32 Next = (i + 1) % N;
					// The ring's vertices run counter-clockwise, as the circle's do: inner i, outer i, outer next.
					Writer.AddUpTriangle(Previous[i], Current[i], Current[Next]);
					Writer.AddUpTriangle(Previous[i], Current[Next], Previous[Next]);
					Triangles += 2;
				}
			}
			Swap(Previous, Current);
		}
	}
	return Triangles;
}
