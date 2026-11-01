// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMapTileFrame.h"
#include "IVoxelWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelChunkManager.h"

bool FVoxelMapTileFrame::HasWater() const
{
	if (bPlanet)
	{
		return Sea.bEnabled && Sea.bRadial;
	}
	return Sea.bEnabled;
}

float FVoxelMapTileFrame::GetWaterAltitude() const
{
	if (bPlanet)
	{
		return static_cast<float>(Sea.Radius - Radius);
	}
	return static_cast<float>(Sea.PlaneZ);
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
	const TArray<FVoxelConditioningZone>& Zones) const
{
	if (bPlanet)
	{
		return static_cast<const FSphericalPlanetWorldMode&>(WorldMode).GetConditionedTerrainAltitude(Sample.Direction, NoiseParams, Zones);
	}
	// TRUE generated surface height: the standalone mode carries a value-captured biome snapshot, so this applies the same
	// continentalness modulation (offset + height-scale) and per-mode composition (e.g. IslandBowl falloff) as chunk
	// generation.
	float Height = static_cast<float>(WorldMode.GetSurfaceOnUpLine(FVector(Sample.X, Sample.Y, 0.0), NoiseParams).Position.Z);
	if (Zones.Num() > 0)
	{
		Height = FVoxelTerrainConditioning::ApplyToHeight(Sample.X, Sample.Y, Height, Zones);
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

bool FVoxelMapTileFrame::IsWater(float InAltitude) const
{
	if (bPlanet)
	{
		return HasWater() && Radius + InAltitude < Sea.Radius;
	}
	return HasWater() && InAltitude < static_cast<float>(Sea.PlaneZ);
}

void FVoxelMapTileFrame::GetShadingSpans(float& OutLandBase, float& OutLandSpan, float& OutDepthSpan) const
{
	// Land spans [the water (or the lowest terrain) .. the highest], water depth [the water .. the lowest]; both guard
	// against degenerate configurations where the bounds collapse.
	OutLandBase = HasWater() ? GetWaterAltitude() : MinAltitude;
	OutLandSpan = FMath::Max(MaxAltitude - OutLandBase, 1.0f);
	OutDepthSpan = FMath::Max(OutLandBase - MinAltitude, 1.0f);
}

float FVoxelMapTileFrame::ShadeAt(int32 G, int32 GridSize, TConstArrayView<float> Altitudes, TConstArrayView<FVoxelMapTileSample> Samples) const
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
		const float DX = (HR - HL) / (2.0f * VoxelSize);
		const float DY = (HU - HD) / (2.0f * VoxelSize);
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

void FVoxelMapTileFrame::GatherZones(const UVoxelChunkManager& ChunkManager, FIntPoint Tile, TArray<FVoxelConditioningZone>& OutZones) const
{
	if (bPlanet)
	{
		// The tile's cap: its centre's direction and 1.5 tile widths of angle (spherical SP-B10).
		ChunkManager.GatherConditioningZonesOnSphere(Grid.CellCentreDirection(Tile), 1.5 * GetTileWorldSize() / FMath::Max(Radius, 1.0), OutZones);
		return;
	}
	// The tile and its 1-voxel apron.
	const FVector2D TileMin(
		(Tile.X * ChunkSize - 1) * VoxelSize + WorldOrigin.X,
		(Tile.Y * ChunkSize - 1) * VoxelSize + WorldOrigin.Y);
	const FVector2D TileMax = TileMin + FVector2D((ChunkSize + 2) * VoxelSize);
	ChunkManager.GatherConditioningZones(FBox2D(TileMin, TileMax), OutZones);
}
