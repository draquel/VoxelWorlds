// Copyright Daniel Raquel. All Rights Reserved.

#include "PCGVoxelPlanetSurfaceGrid.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelCubeSphere.h"

int32 FPCGVoxelPlanetSurfaceGrid::CellsPerFaceEdge(double PlanetRadius, double Spacing)
{
	return FVoxelCubeSphereGrid::CellsPerEdgeFor(PlanetRadius, Spacing);
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
	const FVoxelCubeSphereGrid Grid(CellsPerFaceEdge(Params.PlanetRadius, Spacing));

	auto CrossesBox = [&](const FVector& Direction)
	{
		FVector Hit, HitNormal;
		float HitTime = 0.0f;
		const FVector Start = Center + Direction * Inner;
		const FVector End = Center + Direction * Outer;
		return Bounds.IsInsideOrOn(Start) || Bounds.IsInsideOrOn(End)
			|| FMath::LineExtentBoxIntersection(Bounds, Start, End, FVector::ZeroVector, Hit, HitNormal, HitTime);
	};

	TSet<FIntPoint> Visited;
	TArray<FIntPoint> Queue;
	auto Visit = [&](const FIntPoint& Cell)
	{
		if (!Visited.Contains(Cell))
		{
			Visited.Add(Cell);
			Queue.Add(Cell);
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
			Visit(Grid.CellOfDirection(Direction));
		}
	}

	for (int32 Head = 0; Head < Queue.Num(); ++Head)
	{
		const FIntPoint Cell = Queue[Head];
		const FVector Direction = Grid.CellCentreDirection(Cell);
		if (!CrossesBox(Direction))
		{
			continue;
		}
		if (OutCells.Num() >= MaxCells)
		{
			return false;
		}
		FPCGVoxelPlanetSurfaceCell& Out = OutCells.AddDefaulted_GetRef();
		Grid.FromAtlas(Cell, Out.Face, Out.IU, Out.IV);
		Out.Direction = Direction;

		// The four neighbours: on the face by index, across an edge through the direction one cell beyond it (the grid's).
		const FIntPoint Steps[4] = { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) };
		for (const FIntPoint& Step : Steps)
		{
			Visit(Grid.Neighbour(Cell, Step.X, Step.Y));
		}
	}
	return true;
}
