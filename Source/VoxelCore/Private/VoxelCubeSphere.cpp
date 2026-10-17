// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCubeSphere.h"

namespace VoxelCubeSpherePrivate
{
	/** Cube face frames: face normal N, U axis T, V axis B (a point on the face is N + U*T + V*B). */
	struct FCubeFace
	{
		FVector N;
		FVector T;
		FVector B;
	};

	const FCubeFace GCubeFaces[6] = {
		{ FVector( 1, 0, 0), FVector( 0, 1, 0), FVector(0, 0, 1) },  // 0: +X
		{ FVector(-1, 0, 0), FVector( 0,-1, 0), FVector(0, 0, 1) },  // 1: -X
		{ FVector( 0, 1, 0), FVector(-1, 0, 0), FVector(0, 0, 1) },  // 2: +Y
		{ FVector( 0,-1, 0), FVector( 1, 0, 0), FVector(0, 0, 1) },  // 3: -Y
		{ FVector( 0, 0, 1), FVector( 1, 0, 0), FVector(0, 1, 0) },  // 4: +Z
		{ FVector( 0, 0,-1), FVector( 1, 0, 0), FVector(0,-1, 0) },  // 5: -Z
	};

	int32 FaceOf(const FVector& Dir)
	{
		const FVector Abs = Dir.GetAbs();
		if (Abs.X >= Abs.Y && Abs.X >= Abs.Z)
		{
			return Dir.X >= 0.0 ? 0 : 1;
		}
		if (Abs.Y >= Abs.Z)
		{
			return Dir.Y >= 0.0 ? 2 : 3;
		}
		return Dir.Z >= 0.0 ? 4 : 5;
	}
}

// ==================== FVoxelCubeSphere ====================

void FVoxelCubeSphere::GetFaceAxes(int32 Face, FVector& OutN, FVector& OutT, FVector& OutB)
{
	const VoxelCubeSpherePrivate::FCubeFace& F = VoxelCubeSpherePrivate::GCubeFaces[FMath::Clamp(Face, 0, 5)];
	OutN = F.N;
	OutT = F.T;
	OutB = F.B;
}

// The tangent warp (WORLD_MODES_REVIEW_PLAN.md §15.1): u = (4 / pi) * atan(g) maps the gnomonic coordinate g in [-1, 1]
// to u in [-1, 1], uniform in angle along the face's centre lines; its inverse is g = tan(pi / 4 * u).
double FVoxelCubeSphere::WarpFromGnomonic(double G)
{
	return FMath::Atan(G) * (4.0 / UE_DOUBLE_PI);
}

double FVoxelCubeSphere::GnomonicFromWarp(double U)
{
	return FMath::Tan(U * (UE_DOUBLE_PI / 4.0));
}

FVoxelSurfaceCoord FVoxelCubeSphere::DirectionToSurface(const FVector& Dir)
{
	const int32 Face = VoxelCubeSpherePrivate::FaceOf(Dir);
	const VoxelCubeSpherePrivate::FCubeFace& F = VoxelCubeSpherePrivate::GCubeFaces[Face];
	// Scale the direction onto the face plane (N . P = 1), read its gnomonic coordinates along T and B, then warp them.
	const double Scale = 1.0 / FVector::DotProduct(Dir, F.N);
	return FVoxelSurfaceCoord(Face, FVector2D(
		WarpFromGnomonic(FVector::DotProduct(Dir, F.T) * Scale),
		WarpFromGnomonic(FVector::DotProduct(Dir, F.B) * Scale)));
}

FVector FVoxelCubeSphere::SurfaceToDirection(int32 Face, const FVector2D& UV)
{
	const VoxelCubeSpherePrivate::FCubeFace& F = VoxelCubeSpherePrivate::GCubeFaces[FMath::Clamp(Face, 0, 5)];
	return (F.N + GnomonicFromWarp(UV.X) * F.T + GnomonicFromWarp(UV.Y) * F.B).GetSafeNormal();
}

bool FVoxelCubeSphere::ProjectToFace(int32 Face, const FVector& Direction, FVector2D& OutUV, double MinDot)
{
	const VoxelCubeSpherePrivate::FCubeFace& F = VoxelCubeSpherePrivate::GCubeFaces[FMath::Clamp(Face, 0, 5)];
	const double Dot = FVector::DotProduct(Direction, F.N);
	if (Dot <= MinDot)
	{
		return false;
	}
	const double Scale = 1.0 / Dot;
	OutUV = FVector2D(
		WarpFromGnomonic(FVector::DotProduct(Direction, F.T) * Scale),
		WarpFromGnomonic(FVector::DotProduct(Direction, F.B) * Scale));
	return true;
}

void FVoxelCubeSphere::GetTangentFrame(const FVector& Up, FVector& OutEast, FVector& OutNorth)
{
	FVector East = FVector::CrossProduct(FVector::UpVector, Up);
	if (East.SizeSquared() < 1.0e-8)
	{
		// At a pole: +Y on the tangent plane.
		East = FVector::RightVector - Up * FVector::DotProduct(FVector::RightVector, Up);
	}
	OutEast = East.GetSafeNormal();
	OutNorth = FVector::CrossProduct(Up, OutEast).GetSafeNormal();
}

// ==================== FVoxelCubeSphereGrid ====================

FVoxelCubeSphereGrid::FVoxelCubeSphereGrid(int32 InCellsPerEdge)
	: N(FMath::Max(1, InCellsPerEdge))
{
}

int32 FVoxelCubeSphereGrid::CellsPerEdgeFor(double Radius, double CellSize)
{
	return FMath::Max(1, FMath::CeilToInt32(0.5 * UE_DOUBLE_PI * FMath::Max(Radius, 0.0) / FMath::Max(CellSize, 1.0)));
}

bool FVoxelCubeSphereGrid::FromAtlas(const FIntPoint& Cell, int32& OutFace, int32& OutIU, int32& OutIV) const
{
	if (!IsValidCell(Cell))
	{
		return false;
	}
	OutFace = Cell.X / N;
	OutIU = Cell.X - OutFace * N;
	OutIV = Cell.Y;
	return true;
}

bool FVoxelCubeSphereGrid::IsValidCell(const FIntPoint& Cell) const
{
	return N > 0 && Cell.X >= 0 && Cell.X < 6 * N && Cell.Y >= 0 && Cell.Y < N;
}

FIntPoint FVoxelCubeSphereGrid::CellOfDirection(const FVector& Direction, FVector2D* OutFraction) const
{
	const FVoxelSurfaceCoord Coord = FVoxelCubeSphere::DirectionToSurface(Direction);
	const double CellUV = GetCellUV();
	const double FU = (Coord.UV.X + 1.0) / CellUV;
	const double FV = (Coord.UV.Y + 1.0) / CellUV;
	const int32 IU = FMath::Clamp(FMath::FloorToInt32(FU), 0, N - 1);
	const int32 IV = FMath::Clamp(FMath::FloorToInt32(FV), 0, N - 1);
	if (OutFraction)
	{
		*OutFraction = FVector2D(FMath::Clamp(FU - IU, 0.0, 1.0), FMath::Clamp(FV - IV, 0.0, 1.0));
	}
	return ToAtlas(Coord.Face, IU, IV);
}

FVector FVoxelCubeSphereGrid::DirectionAt(const FIntPoint& Cell, double FU, double FV) const
{
	int32 Face = 0, IU = 0, IV = 0;
	FromAtlas(Cell, Face, IU, IV);
	const double CellUV = GetCellUV();
	return FVoxelCubeSphere::SurfaceToDirection(Face, FVector2D(-1.0 + (IU + FU) * CellUV, -1.0 + (IV + FV) * CellUV));
}

FIntPoint FVoxelCubeSphereGrid::Neighbour(const FIntPoint& Cell, int32 DX, int32 DY) const
{
	int32 Face = 0, IU = 0, IV = 0;
	if (!FromAtlas(Cell, Face, IU, IV))
	{
		return Cell;
	}
	const int32 NU = IU + DX;
	const int32 NV = IV + DY;
	if (NU >= 0 && NU < N && NV >= 0 && NV < N)
	{
		return ToAtlas(Face, NU, NV);
	}
	return CellOfDirection(DirectionAt(Cell, 0.5 + DX, 0.5 + DY));
}

bool FVoxelCubeSphereGrid::GatherWithinAngle(const FVector& Direction, double MaxAngle, int32 MaxCells, TArray<FIntPoint>& OutCells) const
{
	OutCells.Reset();
	if (!IsValid())
	{
		return true;
	}
	const double MinDot = FMath::Cos(FMath::Clamp(MaxAngle, 0.0, UE_DOUBLE_PI));
	TSet<FIntPoint> Visited;
	TArray<FIntPoint> Queue;
	const FIntPoint Start = CellOfDirection(Direction);
	Visited.Add(Start);
	Queue.Add(Start);
	for (int32 Head = 0; Head < Queue.Num(); ++Head)
	{
		const FIntPoint Cell = Queue[Head];
		if (Head > 0 && FVector::DotProduct(CellCentreDirection(Cell), Direction) < MinDot)
		{
			continue;
		}
		if (OutCells.Num() >= MaxCells)
		{
			return false;
		}
		OutCells.Add(Cell);
		const FIntPoint Steps[4] = { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) };
		for (const FIntPoint& Step : Steps)
		{
			const FIntPoint Next = Neighbour(Cell, Step.X, Step.Y);
			if (!Visited.Contains(Next))
			{
				Visited.Add(Next);
				Queue.Add(Next);
			}
		}
	}
	return true;
}

void FVoxelCubeSphereGrid::GatherSpanned(TConstArrayView<FVector> Directions, TArray<FIntPoint>& OutCells) const
{
	if (!IsValid() || Directions.Num() == 0)
	{
		return;
	}
	bool bFaces[6] = {};
	for (const FVector& Direction : Directions)
	{
		bFaces[VoxelCubeSpherePrivate::FaceOf(Direction)] = true;
	}
	const double CellUV = GetCellUV();
	const int32 FirstNew = OutCells.Num();
	for (int32 Face = 0; Face < 6; ++Face)
	{
		if (!bFaces[Face])
		{
			continue;
		}
		FBox2D Rect(ForceInit);
		for (const FVector& Direction : Directions)
		{
			FVector2D UV;
			if (FVoxelCubeSphere::ProjectToFace(Face, Direction, UV))
			{
				Rect += UV;
			}
		}
		if (!Rect.bIsValid)
		{
			continue;
		}
		const int32 U0 = FMath::Clamp(FMath::FloorToInt32((Rect.Min.X + 1.0) / CellUV), 0, N - 1);
		const int32 U1 = FMath::Clamp(FMath::FloorToInt32((Rect.Max.X + 1.0) / CellUV), 0, N - 1);
		const int32 V0 = FMath::Clamp(FMath::FloorToInt32((Rect.Min.Y + 1.0) / CellUV), 0, N - 1);
		const int32 V1 = FMath::Clamp(FMath::FloorToInt32((Rect.Max.Y + 1.0) / CellUV), 0, N - 1);
		for (int32 IV = V0; IV <= V1; ++IV)
		{
			for (int32 IU = U0; IU <= U1; ++IU)
			{
				const FIntPoint Cell = ToAtlas(Face, IU, IV);
				bool bSeen = false;
				for (int32 i = FirstNew; i < OutCells.Num() && !bSeen; ++i)
				{
					bSeen = OutCells[i] == Cell;
				}
				if (!bSeen)
				{
					OutCells.Add(Cell);
				}
			}
		}
	}
}
