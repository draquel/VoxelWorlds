// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelPlanetOcean.h"

namespace VoxelPlanetOceanPrivate
{
	/** Face frames as FSphericalPlanetWorldMode's surface coordinate: normal N, U axis T, V axis B (T x B = N). */
	const FVector GFaceFrames[6][3] = {
		{ FVector( 1, 0, 0), FVector( 0, 1, 0), FVector(0, 0, 1) },  // 0: +X
		{ FVector(-1, 0, 0), FVector( 0,-1, 0), FVector(0, 0, 1) },  // 1: -X
		{ FVector( 0, 1, 0), FVector(-1, 0, 0), FVector(0, 0, 1) },  // 2: +Y
		{ FVector( 0,-1, 0), FVector( 1, 0, 0), FVector(0, 0, 1) },  // 3: -Y
		{ FVector( 0, 0, 1), FVector( 1, 0, 0), FVector(0, 1, 0) },  // 4: +Z
		{ FVector( 0, 0,-1), FVector( 1, 0, 0), FVector(0,-1, 0) },  // 5: -Z
	};
}

void FVoxelPlanetOcean::GetFaceFrame(int32 Face, FVector& OutN, FVector& OutT, FVector& OutB)
{
	const int32 F = FMath::Clamp(Face, 0, 5);
	OutN = VoxelPlanetOceanPrivate::GFaceFrames[F][0];
	OutT = VoxelPlanetOceanPrivate::GFaceFrames[F][1];
	OutB = VoxelPlanetOceanPrivate::GFaceFrames[F][2];
}

FVector FVoxelPlanetOcean::GetGridDirection(int32 Face, int32 I, int32 J, int32 Segments)
{
	FVector N, T, B;
	GetFaceFrame(Face, N, T, B);
	const double S = FMath::Max(Segments, 1);
	const double U = -1.0 + 2.0 * I / S;
	const double V = -1.0 + 2.0 * J / S;
	return (N + FMath::Tan(U * UE_DOUBLE_PI / 4.0) * T + FMath::Tan(V * UE_DOUBLE_PI / 4.0) * B).GetSafeNormal();
}

double FVoxelPlanetOcean::MeasureMaxSag(double Radius, int32 Segments)
{
	// A quad is drawn as two triangles split along its A-D diagonal, its longest chord: the diagonal's middle is the
	// deepest point under the sphere, R (1 - cos(theta / 2)) for the angle theta between the corners' directions.
	double MaxSag = 0.0;
	for (int32 J = 0; J < Segments; ++J)
	{
		for (int32 I = 0; I < Segments; ++I)
		{
			const FVector A = GetGridDirection(0, I, J, Segments);
			const FVector D = GetGridDirection(0, I + 1, J + 1, Segments);
			MaxSag = FMath::Max(MaxSag, Radius * (1.0 - ((A + D) * 0.5).Size()));
		}
	}
	return MaxSag;
}

int32 FVoxelPlanetOcean::ChooseSegments(double Radius, double MaxSag, int32 MinSegments, int32 MaxSegments)
{
	const int32 Lo = FMath::Max(MinSegments, 1);
	const int32 Hi = FMath::Max(MaxSegments, Lo);
	if (Radius <= 0.0 || MaxSag <= 0.0)
	{
		return Hi;
	}
	// The sag falls as 1 / Segments^2: start from the uniform estimate (an edge of pi/2 R in Segments chords, each
	// sagging chord^2 / 8R) and step up until the measured worst quad fits.
	const double Chord = FMath::Sqrt(8.0 * Radius * MaxSag);
	int32 Segments = FMath::Clamp(FMath::CeilToInt32(UE_DOUBLE_HALF_PI * Radius / Chord), Lo, Hi);
	while (Segments < Hi && MeasureMaxSag(Radius, Segments) > MaxSag)
	{
		++Segments;
	}
	return Segments;
}

int32 FVoxelPlanetOcean::Build(const FVoxelPlanetOceanSettings& Settings, FVoxelPlanetOceanMesh& OutTop, FVoxelPlanetOceanMesh* OutUnderside,
	int32& OutSegments)
{
	OutTop.Reset();
	if (OutUnderside)
	{
		OutUnderside->Reset();
	}
	OutSegments = 0;
	if (Settings.Radius <= 0.0)
	{
		return 0;
	}

	const int32 Segments = ChooseSegments(Settings.Radius, Settings.MaxSag, Settings.MinSegments, Settings.MaxSegments);
	OutSegments = Segments;
	const int32 Row = Segments + 1;
	const int32 VertexCount = 6 * Row * Row;
	const int32 QuadCount = 6 * Segments * Segments;
	// Voxels of arc per unit of the face coordinate: along a face's centre lines the warp is uniform in angle, pi/4 per unit.
	const double UVScale = UE_DOUBLE_PI / 4.0 * Settings.Radius / FMath::Max(Settings.VoxelSize, 1.0);

	OutTop.Positions.Reserve(VertexCount);
	OutTop.Normals.Reserve(VertexCount);
	OutTop.Tangents.Reserve(VertexCount);
	OutTop.UV0.Reserve(VertexCount);
	OutTop.UV1.Reserve(VertexCount);
	OutTop.Colors.Reserve(VertexCount);
	OutTop.Triangles.Reserve(QuadCount * 6);

	for (int32 Face = 0; Face < 6; ++Face)
	{
		FVector N, T, B;
		GetFaceFrame(Face, N, T, B);
		const int32 Base = OutTop.Positions.Num();
		for (int32 J = 0; J < Row; ++J)
		{
			for (int32 I = 0; I < Row; ++I)
			{
				const FVector Dir = GetGridDirection(Face, I, J, Segments);
				OutTop.Positions.Add(Dir * Settings.Radius);
				OutTop.Normals.Add(Dir);
				OutTop.Tangents.Add((T - FVector::DotProduct(T, Dir) * Dir).GetSafeNormal());
				const double U = -1.0 + 2.0 * I / Segments;
				const double V = -1.0 + 2.0 * J / Segments;
				OutTop.UV0.Add(FVector2D(U * UVScale, V * UVScale));
				OutTop.UV1.Add(FVector2D(WaterMaterialID, 0.0));
				OutTop.Colors.Add(FColor(WaterMaterialID, 0, 0, 255));
			}
		}
		// Corners A (i, j), B (i+1, j), D (i+1, j+1), C (i, j+1) run counter-clockwise seen from outside (T x B = N);
		// UE's front faces take them as (A, D, B), (A, C, D), as the far ocean's and the water mesher's quads do.
		for (int32 J = 0; J < Segments; ++J)
		{
			for (int32 I = 0; I < Segments; ++I)
			{
				const int32 A = Base + I + J * Row;
				const int32 Bi = A + 1;
				const int32 C = A + Row;
				const int32 D = C + 1;
				OutTop.Triangles.Append({ A, D, Bi, A, C, D });
			}
		}
	}

	if (OutUnderside)
	{
		// The same vertices (the outward normal: it shades from below as the water tiles do), every triangle reversed.
		OutUnderside->Positions = OutTop.Positions;
		OutUnderside->Normals = OutTop.Normals;
		OutUnderside->Tangents = OutTop.Tangents;
		OutUnderside->UV0 = OutTop.UV0;
		OutUnderside->UV1 = OutTop.UV1;
		OutUnderside->Colors = OutTop.Colors;
		OutUnderside->Triangles.Reserve(OutTop.Triangles.Num());
		for (int32 i = 0; i + 2 < OutTop.Triangles.Num(); i += 3)
		{
			OutUnderside->Triangles.Append({ OutTop.Triangles[i], OutTop.Triangles[i + 2], OutTop.Triangles[i + 1] });
		}
	}
	return QuadCount;
}
