// Copyright Daniel Raquel. All Rights Reserved.

#include "PCGVoxelPlanetSurfaceGrid.h"
#include "SphericalPlanetWorldMode.h"

int32 FPCGVoxelPlanetSurfaceGrid::CellsPerFaceEdge(double PlanetRadius, double Spacing)
{
	return FMath::Max(1, FMath::CeilToInt32(0.5 * UE_DOUBLE_PI * PlanetRadius / FMath::Max(Spacing, 1.0)));
}

bool FPCGVoxelPlanetSurfaceGrid::GatherCells(const FSphericalPlanetWorldMode& Planet, const FBox& Bounds, double Spacing, int64 MaxCells,
	TArray<FPCGVoxelPlanetSurfaceCell>& OutCells)
{
	OutCells.Reset();
	if (!Bounds.IsValid)
	{
		return true;
	}
	const FSphericalPlanetParams& Params = Planet.GetPlanetParams();
	const FVector& Center = Params.PlanetCenter;
	const double Inner = Params.GetInnerRadius();
	const double Outer = Params.GetOuterRadius();
	const int32 N = CellsPerFaceEdge(Params.PlanetRadius, Spacing);
	const double CellUV = 2.0 / N;

	auto CellOf = [&](const FVector& Direction, int32& OutFace, int32& OutIU, int32& OutIV)
	{
		const FVoxelSurfaceCoord Coord = Planet.WorldToSurface(Center + Direction);
		OutFace = Coord.Face;
		OutIU = FMath::Clamp(FMath::FloorToInt32((Coord.UV.X + 1.0) / CellUV), 0, N - 1);
		OutIV = FMath::Clamp(FMath::FloorToInt32((Coord.UV.Y + 1.0) / CellUV), 0, N - 1);
	};
	auto CellDirection = [&](int32 Face, double U, double V)
	{
		return (Planet.SurfaceToWorld(FVoxelSurfaceCoord(Face, FVector2D(U, V)), 0.0f) - Center).GetSafeNormal();
	};
	auto CrossesBox = [&](const FVector& Direction)
	{
		FVector Hit, HitNormal;
		float HitTime = 0.0f;
		const FVector Start = Center + Direction * Inner;
		const FVector End = Center + Direction * Outer;
		return Bounds.IsInsideOrOn(Start) || Bounds.IsInsideOrOn(End)
			|| FMath::LineExtentBoxIntersection(Bounds, Start, End, FVector::ZeroVector, Hit, HitNormal, HitTime);
	};
	auto Key = [N](int32 Face, int32 IU, int32 IV)
	{
		return (static_cast<int64>(Face) * N + IU) * N + IV;
	};

	TSet<int64> Visited;
	TArray<FIntVector> Queue; // (face, iu, iv)
	auto Visit = [&](int32 Face, int32 IU, int32 IV)
	{
		const int64 K = Key(Face, IU, IV);
		if (!Visited.Contains(K))
		{
			Visited.Add(K);
			Queue.Add(FIntVector(Face, IU, IV));
		}
	};

	// Seeds: the cells of the box's centre and corners.
	TArray<FVector> SeedPoints = { Bounds.GetCenter() };
	for (int32 Corner = 0; Corner < 8; ++Corner)
	{
		SeedPoints.Add(FVector(Corner & 1 ? Bounds.Max.X : Bounds.Min.X, Corner & 2 ? Bounds.Max.Y : Bounds.Min.Y, Corner & 4 ? Bounds.Max.Z : Bounds.Min.Z));
	}
	for (const FVector& Point : SeedPoints)
	{
		const FVector Direction = (Point - Center).GetSafeNormal();
		if (!Direction.IsNearlyZero())
		{
			int32 Face, IU, IV;
			CellOf(Direction, Face, IU, IV);
			Visit(Face, IU, IV);
		}
	}

	for (int32 Head = 0; Head < Queue.Num(); ++Head)
	{
		const FIntVector Cell = Queue[Head];
		const double U = -1.0 + (Cell.Y + 0.5) * CellUV;
		const double V = -1.0 + (Cell.Z + 0.5) * CellUV;
		const FVector Direction = CellDirection(Cell.X, U, V);
		if (!CrossesBox(Direction))
		{
			continue;
		}
		if (OutCells.Num() >= MaxCells)
		{
			return false;
		}
		FPCGVoxelPlanetSurfaceCell& Out = OutCells.AddDefaulted_GetRef();
		Out.Face = Cell.X;
		Out.IU = Cell.Y;
		Out.IV = Cell.Z;
		Out.Direction = Direction;

		// The four neighbours: on the face by index, across an edge through the direction one cell beyond it (the warp
		// extends past [-1, 1], and WorldToSurface puts the direction on the face it belongs to).
		const FIntPoint Steps[4] = { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) };
		for (const FIntPoint& Step : Steps)
		{
			const int32 NU = Cell.Y + Step.X;
			const int32 NV = Cell.Z + Step.Y;
			if (NU >= 0 && NU < N && NV >= 0 && NV < N)
			{
				Visit(Cell.X, NU, NV);
			}
			else
			{
				int32 Face, IU, IV;
				CellOf(CellDirection(Cell.X, U + Step.X * CellUV, V + Step.Y * CellUV), Face, IU, IV);
				Visit(Face, IU, IV);
			}
		}
	}
	return true;
}
