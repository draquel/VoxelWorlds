// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMapTileFrame.h"
#include "IVoxelWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelChunkManager.h"

FBox FVoxelMapTileFrame::GetTileBounds(const FVoxelMapTileId& Id) const
{
	FBox TileBounds(ForceInit);
	const int32 Last = ChunkSize + 1;
	for (const FIntPoint Corner : { FIntPoint(1, 1), FIntPoint(Last, 1), FIntPoint(1, Last), FIntPoint(Last, Last) })
	{
		const FVoxelMapTileSample Sample = SampleAt(Id, Corner.X, Corner.Y);
		TileBounds += PositionAt(Sample, MinAltitude);
		TileBounds += PositionAt(Sample, MaxAltitude);
	}
	return TileBounds;
}

bool FVoxelMapTileFrame::HasWater(const FVoxelWaterLevelRange& TileWater) const
{
	if (bPlanet)
	{
		return TileWater.bAny && Water.IsRadial();
	}
	return TileWater.bAny;
}

float FVoxelMapTileFrame::GetWaterAltitude(const FVoxelWaterLevelRange& TileWater) const
{
	if (bPlanet)
	{
		return static_cast<float>(TileWater.GetLevel() - Radius);
	}
	return static_cast<float>(TileWater.GetLevel());
}

double FVoxelMapTileFrame::GetTileWorldSize() const
{
	if (bPlanet)
	{
		// Along a face's centre lines on the datum sphere (the tangent warp keeps the rest within about 1.2x).
		return static_cast<float>(0.5 * UE_DOUBLE_PI * Radius / Grid.GetCellsPerEdge());
	}
	return ChunkSize * VoxelSize;
}

FIntPoint FVoxelMapTileFrame::TileOf(const FVector& WorldPos) const
{
	if (bPlanet)
	{
		return Grid.CellOfDirection(FSphericalPlanetWorldMode::GetDirectionFromCenter(WorldPos, Center));
	}
	const float ChunkWorldSize = ChunkSize * VoxelSize;
	if (ChunkWorldSize <= 0.f)
	{
		return FIntPoint(0, 0);
	}
	return FIntPoint(
		FMath::FloorToInt((WorldPos.X - WorldOrigin.X) / ChunkWorldSize),
		FMath::FloorToInt((WorldPos.Y - WorldOrigin.Y) / ChunkWorldSize));
}

FVector FVoxelMapTileFrame::TileOrigin(FIntPoint Tile) const
{
	if (bPlanet)
	{
		// A planet: the tile's (U, V) = (0, 0) corner on the datum sphere.
		return Grid.IsValidCell(Tile) ? Center + Grid.DirectionAt(Tile, 0.0, 0.0) * Radius : Center;
	}
	const float ChunkWorldSize = ChunkSize * VoxelSize;
	return FVector(
		Tile.X * ChunkWorldSize + WorldOrigin.X,
		Tile.Y * ChunkWorldSize + WorldOrigin.Y,
		0.0f);
}

FIntPoint FVoxelMapTileFrame::Neighbour(FIntPoint Tile, int32 DX, int32 DY) const
{
	return bPlanet ? Grid.Neighbour(Tile, DX, DY) : Tile + FIntPoint(DX, DY);
}

bool FVoxelMapTileFrame::IsValidTile(FIntPoint Tile) const
{
	if (bPlanet)
	{
		// A planet: the atlas (spherical Phase 4).
		return Grid.IsValidCell(Tile);
	}
	if (!Bounds.IsBounded())
	{
		return true;
	}
	const FVector Min = TileOrigin(Tile);
	const double Size = GetTileWorldSize();
	return Bounds.IntersectsBox2D(FBox2D(FVector2D(Min.X, Min.Y), FVector2D(Min.X + Size, Min.Y + Size)));
}

void FVoxelMapTileFrame::TilesOfChunk(const FIntVector& ChunkCoord, TArray<FIntPoint>& OutTiles) const
{
	if (!bPlanet)
	{
		OutTiles.Add(FIntPoint(ChunkCoord.X, ChunkCoord.Y));
		return;
	}
	// The cells the chunk's corners and centre span from the planet centre.
	const double ChunkWorldSize = static_cast<double>(ChunkSize) * VoxelSize;
	TArray<FVector, TInlineAllocator<9>> Directions;
	for (int32 Corner = 0; Corner < 9; ++Corner)
	{
		const FVector Offset = Corner < 8 ? FVector(Corner & 1, (Corner >> 1) & 1, (Corner >> 2) & 1) : FVector(0.5);
		Directions.Add(FSphericalPlanetWorldMode::GetDirectionFromCenter(
			WorldOrigin + (FVector(ChunkCoord) + Offset) * ChunkWorldSize, Center));
	}
	Grid.GatherSpanned(Directions, OutTiles);
}

void FVoxelMapTileFrame::TilesWithinRadius(const FVector& WorldPos, double InRadius, TArray<FIntPoint>& OutTiles) const
{
	if (bPlanet)
	{
		// A planet: the tiles whose centres lie within the radius (plus a tile) along the surface, the hemispheres apart.
		const double Angle = (InRadius + GetTileWorldSize()) / FMath::Max(Radius, 1.0);
		Grid.GatherWithinAngle(FSphericalPlanetWorldMode::GetDirectionFromCenter(WorldPos, Center), Angle, 16384, OutTiles);
		return;
	}
	const float ChunkWorldSize = ChunkSize * VoxelSize;
	const FIntPoint CenterTile = TileOf(WorldPos);
	const int32 TileRadius = FMath::CeilToInt(static_cast<float>(InRadius) / ChunkWorldSize);
	for (int32 TY = CenterTile.Y - TileRadius; TY <= CenterTile.Y + TileRadius; ++TY)
	{
		for (int32 TX = CenterTile.X - TileRadius; TX <= CenterTile.X + TileRadius; ++TX)
		{
			const FIntPoint TileCoord(TX, TY);
			// Exploration stops at the world's edge (WM-I2): the sea past an island's R + falloff is not mapped.
			if (IsValidTile(TileCoord))
			{
				OutTiles.Add(TileCoord);
			}
		}
	}
}

bool FVoxelMapTileFrame::DirectionToTilePixel(const FVector& Direction, FIntPoint& OutTile, int32& OutPX, int32& OutPY) const
{
	if (!bPlanet)
	{
		return false;
	}
	FVector2D Fraction;
	OutTile = Grid.CellOfDirection(Direction, &Fraction);
	const int32 Res = FMath::Max(1, ChunkSize);
	OutPX = FMath::Clamp(FMath::FloorToInt32(Fraction.X * Res), 0, Res - 1);
	OutPY = FMath::Clamp(FMath::FloorToInt32(Fraction.Y * Res), 0, Res - 1);
	return true;
}

bool FVoxelMapTileFrame::ChunkMayReachSurface(const FIntVector& ChunkCoord, float LowestAnalyticAltitude) const
{
	if (bPlanet)
	{
		// Edited chunks wholly more than a chunk below the tile's lowest analytic altitude cannot reach its surface.
		const double ChunkWorldSize = static_cast<double>(ChunkSize) * VoxelSize;
		double MaxRadius = 0.0;
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector Offset(Corner & 1, (Corner >> 1) & 1, (Corner >> 2) & 1);
			MaxRadius = FMath::Max(MaxRadius, FVector::Dist(WorldOrigin + (FVector(ChunkCoord) + Offset) * ChunkWorldSize, Center));
		}
		return !(MaxRadius - Radius < LowestAnalyticAltitude - ChunkWorldSize);
	}
	// Edits more than a chunk below the analytic surface cannot reach the top surface without the chunks above them
	// being edited too, so they need not be loaded (dungeon interiors under a loaded entrance).
	const float ChunkWorldSize = ChunkSize * VoxelSize;
	const int32 MinRequiredZ = FMath::FloorToInt((LowestAnalyticAltitude - WorldOrigin.Z) / ChunkWorldSize) - 1;
	return ChunkCoord.Z >= MinRequiredZ;
}

FVoxelMapTileSample FVoxelMapTileFrame::SampleAt(FIntPoint Tile, int32 GX, int32 GY) const
{
	FVoxelMapTileSample Sample;
	if (bPlanet)
	{
		Sample.Direction = Grid.DirectionAt(Tile, (GX - 0.5) / ChunkSize, (GY - 0.5) / ChunkSize);
		return Sample;
	}
	// Grid index (GX, GY) maps to pixel (GX - 1, GY - 1).
	Sample.X = (Tile.X * ChunkSize + GX - 1) * VoxelSize + WorldOrigin.X;
	Sample.Y = (Tile.Y * ChunkSize + GY - 1) * VoxelSize + WorldOrigin.Y;
	return Sample;
}

float FVoxelMapTileFrame::Altitude(const IVoxelWorldMode& WorldMode, const FVoxelMapTileSample& Sample, const FVoxelNoiseParams& NoiseParams,
	const FVoxelConditioning& Conditioning) const
{
	if (bPlanet)
	{
		return static_cast<const FSphericalPlanetWorldMode&>(WorldMode).GetConditionedTerrainAltitude(Sample.Direction, NoiseParams, Conditioning);
	}
	// TRUE generated surface height: the standalone mode carries a value-captured biome snapshot, so this applies the same
	// continentalness modulation (offset + height-scale) and per-mode composition (e.g. IslandBowl falloff) as chunk
	// generation.
	float Height = static_cast<float>(WorldMode.GetSurfaceOnUpLine(FVector(Sample.X, Sample.Y, 0.0), NoiseParams).Position.Z);
	if (!Conditioning.IsEmpty())
	{
		Height = Conditioning.ApplyToHeight(Sample.X, Sample.Y, Height);
	}
	return Height;
}

FVector FVoxelMapTileFrame::PositionAt(const FVoxelMapTileSample& Sample, float InAltitude) const
{
	if (bPlanet)
	{
		return Center + Sample.Direction * (Radius + InAltitude);
	}
	return FVector(Sample.X, Sample.Y, InAltitude);
}

bool FVoxelMapTileFrame::IsWater(const FVoxelWaterLevelRange& TileWater, float InAltitude) const
{
	if (bPlanet)
	{
		return HasWater(TileWater) && Radius + InAltitude < TileWater.GetLevel();
	}
	return HasWater(TileWater) && InAltitude < static_cast<float>(TileWater.GetLevel());
}

void FVoxelMapTileFrame::GetShadingSpans(const FVoxelWaterLevelRange& TileWater, float& OutLandBase, float& OutLandSpan, float& OutDepthSpan) const
{
	// Land spans [the water (or the lowest terrain) .. the highest], water depth [the water .. the lowest]; both guard
	// against degenerate configurations where the bounds collapse.
	OutLandBase = HasWater(TileWater) ? GetWaterAltitude(TileWater) : MinAltitude;
	OutLandSpan = FMath::Max(MaxAltitude - OutLandBase, 1.0f);
	OutDepthSpan = FMath::Max(OutLandBase - MinAltitude, 1.0f);
}

float FVoxelMapTileFrame::ShadeAt(int32 G, int32 GridSize, TConstArrayView<float> Altitudes, TConstArrayView<FVoxelMapTileSample> Samples, int32 Level) const
{
	float NdotL = 0.0f;
	if (bPlanet)
	{
		// The surface normal from the neighbouring pixels' ground points, lit by the plane's NW key light in the local
		// frame (north toward the +Z pole).
		auto PositionOf = [&](int32 Index)
		{
			return Center + Samples[Index].Direction * (Radius + Altitudes[Index]);
		};
		const FVector Up = Samples[G].Direction;
		FVector Normal = FVector::CrossProduct(PositionOf(G + 1) - PositionOf(G - 1), PositionOf(G + GridSize) - PositionOf(G - GridSize)).GetSafeNormal();
		if (Normal.IsNearlyZero())
		{
			Normal = Up;
		}
		else if (FVector::DotProduct(Normal, Up) < 0.0)
		{
			Normal = -Normal;
		}
		FVector East, North;
		FVoxelCubeSphere::GetTangentFrame(Up, East, North);
		const FVector LightDir = (North * -0.707 + East * -0.707 + Up).GetSafeNormal();
		NdotL = FMath::Clamp(static_cast<float>(FVector::DotProduct(Normal, LightDir)), 0.0f, 1.0f);
	}
	else
	{
		// Lambert N.L from the height gradient (central differences over the apron grid) under the classic cartographic
		// NW key light at about 45 degrees altitude. This is what makes ridges and valleys legible; the elevation tint
		// alone reads flat because terrain at one altitude is one flat colour.
		const FVector LightDir = FVector(-0.707f, -0.707f, 1.0f).GetSafeNormal();
		const float HL = Altitudes[G - 1];
		const float HR = Altitudes[G + 1];
		const float HD = Altitudes[G - GridSize];
		const float HU = Altitudes[G + GridSize];
		// The grid points are 2^Level voxels apart at a coarse level of the pyramid.
		const float Spacing = VoxelSize * static_cast<float>(1 << Level);
		const float DX = (HR - HL) / (2.0f * Spacing);
		const float DY = (HU - HD) / (2.0f * Spacing);
		const FVector Normal = FVector(-DX, -DY, 1.0f).GetSafeNormal();
		NdotL = FMath::Clamp(static_cast<float>(FVector::DotProduct(Normal, LightDir)), 0.0f, 1.0f);
	}
	// Remap N.L into a gentle range: flat ground stays near its base colour, slopes facing the light brighten and
	// away-facing slopes fall into shadow.
	return FMath::Lerp(0.55f, 1.25f, NdotL);
}

void FVoxelMapTileFrame::ApronPixel(FIntPoint Tile, int32 GX, int32 GY, const FVoxelMapTileSample& Sample, FIntPoint& OutTile, int32& OutPX, int32& OutPY) const
{
	const int32 Resolution = ChunkSize;
	const int32 PX = GX - 1;
	const int32 PY = GY - 1;
	if (bPlanet)
	{
		const bool bInterior = GX >= 1 && GY >= 1 && GX <= Resolution && GY <= Resolution;
		if (bInterior)
		{
			OutTile = Tile;
			OutPX = PX;
			OutPY = PY;
			return;
		}
		FVector2D Fraction;
		OutTile = Grid.CellOfDirection(Sample.Direction, &Fraction);
		OutPX = FMath::Clamp(FMath::FloorToInt32(Fraction.X * Resolution), 0, Resolution - 1);
		OutPY = FMath::Clamp(FMath::FloorToInt32(Fraction.Y * Resolution), 0, Resolution - 1);
		return;
	}
	const int32 DX = PX < 0 ? -1 : (PX >= Resolution ? 1 : 0);
	const int32 DY = PY < 0 ? -1 : (PY >= Resolution ? 1 : 0);
	OutTile = Tile + FIntPoint(DX, DY);
	OutPX = PX - DX * Resolution;
	OutPY = PY - DY * Resolution;
}

void FVoxelMapTileFrame::GatherConditioning(const UVoxelChunkManager& ChunkManager, FIntPoint Tile, FVoxelConditioning& Out) const
{
	if (bPlanet)
	{
		// The tile's cap: its centre's direction and 1.5 tile widths of angle (spherical SP-B10).
		ChunkManager.GatherConditioningOnSphere(Grid.CellCentreDirection(Tile), 1.5 * GetTileWorldSize() / FMath::Max(Radius, 1.0),
			EVoxelConditioningGather::Ready, Out);
		return;
	}
	// The tile and its 1-voxel apron.
	const FVector2D TileMin(
		(Tile.X * ChunkSize - 1) * VoxelSize + WorldOrigin.X,
		(Tile.Y * ChunkSize - 1) * VoxelSize + WorldOrigin.Y);
	const FVector2D TileMax = TileMin + FVector2D((ChunkSize + 2) * VoxelSize);
	ChunkManager.GatherConditioning(FBox2D(TileMin, TileMax), EVoxelConditioningGather::Ready, Out);
}

// ---------------------------------------------------------------------------
// The tile pyramid (MAP_PYRAMID_PLAN.md P1, D84)
// ---------------------------------------------------------------------------

namespace
{
	/** Floor division (the tile of a pixel index, negative on a plane). */
	int64 FloorDiv(int64 A, int64 B)
	{
		const int64 Q = A / B;
		return (A % B != 0 && ((A < 0) != (B < 0))) ? Q - 1 : Q;
	}
}

int32 FVoxelMapTileFrame::GetMaxLevel() const
{
	if (!bPlanet)
	{
		return MaxPlaneLevel;
	}
	// The level whose one tile holds a face: ceil(log2 N).
	const int32 N = Grid.GetCellsPerEdge();
	return N > 1 ? static_cast<int32>(FMath::CeilLogTwo(static_cast<uint32>(N))) : 0;
}

int32 FVoxelMapTileFrame::TilesPerFaceEdgeAt(int32 Level) const
{
	const int32 N = Grid.GetCellsPerEdge();
	return N > 0 ? ((N - 1) >> Level) + 1 : 0;
}

int32 FVoxelMapTileFrame::LevelForTexel(double TexelWorldSize) const
{
	const double Pixel = GetPixelWorldSize();
	if (Pixel <= 0.0 || TexelWorldSize <= Pixel)
	{
		return 0;
	}
	const int32 Level = FMath::FloorToInt32(FMath::Log2(TexelWorldSize / Pixel) + 1.0e-6);
	return FMath::Clamp(Level, 0, GetMaxLevel());
}

bool FVoxelMapTileFrame::FaceIndicesAt(const FVoxelMapTileId& Id, int32& OutFace, int32& OutU, int32& OutV) const
{
	const int32 NL = TilesPerFaceEdgeAt(Id.Level);
	if (!bPlanet || NL <= 0 || Id.Level < 0 || Id.Level > GetMaxLevel() || Id.Coord.X < 0 || Id.Coord.Y < 0 || Id.Coord.X >= 6 * NL || Id.Coord.Y >= NL)
	{
		return false;
	}
	OutFace = Id.Coord.X / NL;
	OutU = Id.Coord.X - OutFace * NL;
	OutV = Id.Coord.Y;
	return true;
}

FVoxelMapTileId FVoxelMapTileFrame::LevelTileOf(const FIntPoint& Tile0, int32 Level) const
{
	if (!bPlanet)
	{
		return FVoxelMapTileId(FIntPoint(Tile0.X >> Level, Tile0.Y >> Level), Level);
	}
	int32 Face = 0, IU = 0, IV = 0;
	if (!Grid.FromAtlas(Tile0, Face, IU, IV))
	{
		return FVoxelMapTileId(Tile0, Level);
	}
	return FVoxelMapTileId(FIntPoint(Face * TilesPerFaceEdgeAt(Level) + (IU >> Level), IV >> Level), Level);
}

FVoxelMapTileId FVoxelMapTileFrame::ParentOf(const FVoxelMapTileId& Id) const
{
	if (!bPlanet)
	{
		return FVoxelMapTileId(FIntPoint(Id.Coord.X >> 1, Id.Coord.Y >> 1), Id.Level + 1);
	}
	int32 Face = 0, U = 0, V = 0;
	if (!FaceIndicesAt(Id, Face, U, V))
	{
		return FVoxelMapTileId(Id.Coord, Id.Level + 1);
	}
	return FVoxelMapTileId(FIntPoint(Face * TilesPerFaceEdgeAt(Id.Level + 1) + (U >> 1), V >> 1), Id.Level + 1);
}

FIntPoint FVoxelMapTileFrame::FirstLevel0Tile(const FVoxelMapTileId& Id) const
{
	if (!bPlanet)
	{
		return FIntPoint(Id.Coord.X * (1 << Id.Level), Id.Coord.Y * (1 << Id.Level));
	}
	int32 Face = 0, U = 0, V = 0;
	if (!FaceIndicesAt(Id, Face, U, V))
	{
		return Id.Coord;
	}
	return Grid.ToAtlas(Face, U << Id.Level, V << Id.Level);
}

bool FVoxelMapTileFrame::IsValidTile(const FVoxelMapTileId& Id) const
{
	if (Id.Level == 0)
	{
		return IsValidTile(Id.Coord);
	}
	if (bPlanet)
	{
		int32 Face = 0, U = 0, V = 0;
		return FaceIndicesAt(Id, Face, U, V);
	}
	if (Id.Level < 0 || Id.Level > MaxPlaneLevel)
	{
		return false;
	}
	if (!Bounds.IsBounded())
	{
		return true;
	}
	const FVector Min = TileOrigin(FirstLevel0Tile(Id));
	const double Size = GetTileWorldSize() * static_cast<double>(1 << Id.Level);
	return Bounds.IntersectsBox2D(FBox2D(FVector2D(Min.X, Min.Y), FVector2D(Min.X + Size, Min.Y + Size)));
}

bool FVoxelMapTileFrame::CanDrawTile(const FVoxelMapTileId& Id) const
{
	if (!bPlanet)
	{
		return Id.Level >= 0 && Id.Level <= MaxPlaneLevel;
	}
	int32 Face = 0, U = 0, V = 0;
	return FaceIndicesAt(Id, Face, U, V);
}

bool FVoxelMapTileFrame::CoversLevel0Tile(const FVoxelMapTileId& Id, const FIntPoint& Tile0, int32 Ring) const
{
	const int32 Span = 1 << Id.Level;
	const FIntPoint First = FirstLevel0Tile(Id);
	if (!bPlanet)
	{
		return Tile0.X >= First.X - Ring && Tile0.X < First.X + Span + Ring && Tile0.Y >= First.Y - Ring && Tile0.Y < First.Y + Span + Ring;
	}
	int32 Face = 0, IU = 0, IV = 0, FirstFace = 0, FirstU = 0, FirstV = 0;
	if (!Grid.FromAtlas(Tile0, Face, IU, IV) || !Grid.FromAtlas(First, FirstFace, FirstU, FirstV) || Face != FirstFace)
	{
		return false;
	}
	return IU >= FirstU - Ring && IU < FirstU + Span + Ring && IV >= FirstV - Ring && IV < FirstV + Span + Ring;
}

void FVoxelMapTileFrame::Level0TilesOf(const FVoxelMapTileId& Id, int32 Ring, TArray<FIntPoint>& OutTiles) const
{
	const int32 Span = 1 << Id.Level;
	const FIntPoint First = FirstLevel0Tile(Id);
	if (!bPlanet)
	{
		for (int32 Y = First.Y - Ring; Y < First.Y + Span + Ring; ++Y)
		{
			for (int32 X = First.X - Ring; X < First.X + Span + Ring; ++X)
			{
				OutTiles.Add(FIntPoint(X, Y));
			}
		}
		return;
	}
	int32 Face = 0, U = 0, V = 0;
	if (!Grid.FromAtlas(First, Face, U, V))
	{
		return;
	}
	const int32 N = Grid.GetCellsPerEdge();
	for (int32 IV = FMath::Max(0, V - Ring); IV < FMath::Min(N, V + Span + Ring); ++IV)
	{
		for (int32 IU = FMath::Max(0, U - Ring); IU < FMath::Min(N, U + Span + Ring); ++IU)
		{
			OutTiles.Add(Grid.ToAtlas(Face, IU, IV));
		}
	}
}

FVoxelMapTileSample FVoxelMapTileFrame::SampleAt(const FVoxelMapTileId& Id, int32 GX, int32 GY) const
{
	if (Id.Level == 0)
	{
		return SampleAt(Id.Coord, GX, GY);
	}
	FVoxelMapTileSample Sample;
	const int32 Scale = 1 << Id.Level;
	if (bPlanet)
	{
		// The pixel's centre, 2^L level-0 pixels a pixel from the tile's first level-0 cell (past the face's last cell on
		// its edge tiles: the face's plane extended, pixels no view reads).
		Sample.Direction = Grid.DirectionAt(FirstLevel0Tile(Id), (GX - 0.5) / ChunkSize * Scale, (GY - 0.5) / ChunkSize * Scale);
		return Sample;
	}
	// The pixel's corner, as level 0's: grid index (GX, GY) is pixel (GX - 1, GY - 1), each 2^L voxels.
	Sample.X = static_cast<float>(static_cast<double>((static_cast<int64>(Id.Coord.X) * ChunkSize + GX - 1) * Scale) * VoxelSize + WorldOrigin.X);
	Sample.Y = static_cast<float>(static_cast<double>((static_cast<int64>(Id.Coord.Y) * ChunkSize + GY - 1) * Scale) * VoxelSize + WorldOrigin.Y);
	return Sample;
}

void FVoxelMapTileFrame::SourcePixel(const FVoxelMapTileId& Id, int32 GX, int32 GY, const FVoxelMapTileSample& Sample, FIntPoint& OutTile0,
	int32& OutPX, int32& OutPY) const
{
	if (Id.Level == 0)
	{
		ApronPixel(Id.Coord, GX, GY, Sample, OutTile0, OutPX, OutPY);
		return;
	}
	const int64 Scale = static_cast<int64>(1) << Id.Level;
	const int32 Res = FMath::Max(1, ChunkSize);
	if (bPlanet)
	{
		int32 Face = 0, U = 0, V = 0;
		const int64 N = Grid.GetCellsPerEdge();
		if (FaceIndicesAt(Id, Face, U, V))
		{
			// The point's level-0 pixel along the face: the pixel's centre lies 2^(L-1) level-0 pixels into it.
			const int64 PU = (static_cast<int64>(U) * Res + GX - 1) * Scale + Scale / 2;
			const int64 PV = (static_cast<int64>(V) * Res + GY - 1) * Scale + Scale / 2;
			if (PU >= 0 && PV >= 0 && PU < N * Res && PV < N * Res)
			{
				OutTile0 = Grid.ToAtlas(Face, static_cast<int32>(PU / Res), static_cast<int32>(PV / Res));
				OutPX = static_cast<int32>(PU % Res);
				OutPY = static_cast<int32>(PV % Res);
				return;
			}
		}
		// Across a face edge: the cell the direction falls in.
		FVector2D Fraction;
		OutTile0 = Grid.CellOfDirection(Sample.Direction, &Fraction);
		OutPX = FMath::Clamp(FMath::FloorToInt32(Fraction.X * Res), 0, Res - 1);
		OutPY = FMath::Clamp(FMath::FloorToInt32(Fraction.Y * Res), 0, Res - 1);
		return;
	}
	const int64 PX = (static_cast<int64>(Id.Coord.X) * Res + GX - 1) * Scale;
	const int64 PY = (static_cast<int64>(Id.Coord.Y) * Res + GY - 1) * Scale;
	const int64 TX = FloorDiv(PX, Res);
	const int64 TY = FloorDiv(PY, Res);
	OutTile0 = FIntPoint(static_cast<int32>(TX), static_cast<int32>(TY));
	OutPX = static_cast<int32>(PX - TX * Res);
	OutPY = static_cast<int32>(PY - TY * Res);
}

void FVoxelMapTileFrame::GatherConditioning(const UVoxelChunkManager& ChunkManager, const FVoxelMapTileId& Id, FVoxelConditioning& Out) const
{
	if (Id.Level == 0)
	{
		GatherConditioning(ChunkManager, Id.Coord, Out);
		return;
	}
	const double Scale = static_cast<double>(1 << Id.Level);
	// A pixel above level 0 is a point sample every 2^Level voxels: the tile takes every line as at least 0.71 of that
	// wide, for the height and for the band, so a road is an unbroken line on the map and not dashes, and gathers over
	// its area grown by as much. A tile wider than the gather limit takes no lines (a road is far under a pixel there).
	const bool bLines = TakesLines(Id.Level);
	const double Least = bLines ? FVoxelConditioningLine::CoarseHalfWidthPerSpacing * Scale * VoxelSize : 0.0;
	if (bPlanet)
	{
		// The tile's cap: its centre's direction and 1.5 of its widths of angle.
		const FVector Centre = Grid.DirectionAt(FirstLevel0Tile(Id), 0.5 * Scale, 0.5 * Scale);
		ChunkManager.GatherConditioningOnSphere(Centre, (1.5 * Scale * GetTileWorldSize() + Least) / FMath::Max(Radius, 1.0),
			EVoxelConditioningGather::Ready, Out);
	}
	else
	{
		// The tile and its one-pixel apron.
		const FVector2D TileMin(
			(static_cast<double>(Id.Coord.X) * ChunkSize - 1.0) * Scale * VoxelSize + WorldOrigin.X,
			(static_cast<double>(Id.Coord.Y) * ChunkSize - 1.0) * Scale * VoxelSize + WorldOrigin.Y);
		const FVector2D TileMax = TileMin + FVector2D((ChunkSize + 2) * Scale * VoxelSize);
		ChunkManager.GatherConditioning(FBox2D(TileMin, TileMax).ExpandBy(Least), EVoxelConditioningGather::Ready, Out);
	}
	if (bLines)
	{
		Out.LeastHalfWidth = static_cast<float>(Least);
	}
	else
	{
		Out.Lines.Reset();
	}
}

bool FVoxelMapTileFrame::TakesLines(int32 Level)
{
	// A tile at a level is 2^Level chunks wide.
	return Level >= 0 && Level < 31 && (1 << Level) <= FVoxelConditioningLine::GatherLimitChunks;
}
