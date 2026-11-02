// Copyright Daniel Raquel. All Rights Reserved.

// UVoxelMapSubsystem's globe albedo: a planet's coarse colours for the zoomed-out world map (spherical Phase 6 P6.5),
// a translation unit of the class.

#include "VoxelMapSubsystem.h"
#include "VoxelMap.h"
#include "VoxelChunkManager.h"
#include "VoxelSeaSurface.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelBiomeRegistry.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelMaterialAtlas.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelCoordinates.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelTerrainConditioning.h"
#include "IVoxelWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelWorldModeFactory.h"
#include "VoxelEditManager.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "VoxelFarField.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "Misc/Base64.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "VoxelResidencyStats.h"

const FVoxelMapGlobeAlbedo* UVoxelMapSubsystem::GetGlobeAlbedo()
{
	if (GlobeAlbedo.IsValid())
	{
		return GlobeAlbedo.Get();
	}
	if (bGlobeAlbedoBuilding || bShuttingDown || !ResolveChunkManager() || !TileFrame.bPlanet)
	{
		return nullptr;
	}
	UVoxelChunkManager* ChunkMgr = CachedChunkManagerWeak.Get();
	const UVoxelWorldConfiguration* Config = ChunkMgr ? ChunkMgr->GetConfiguration() : nullptr;
	if (!Config)
	{
		return nullptr;
	}
	// The far field's analytic surface, over a world mode of our own (the task may outlive the chunk manager's).
	TSharedPtr<const FVoxelFarFieldSurface> Surface = FVoxelFarFieldSurface::Make(*Config, FVoxelWorldModeFactory::Create(*Config));
	if (!Surface.IsValid())
	{
		return nullptr;
	}
	bGlobeAlbedoBuilding = true;
	const int32 Cells = FMath::Min(GlobeAlbedoCellsPerEdge, TileFrame.Grid.GetCellsPerEdge());
	TWeakObjectPtr<UVoxelMapSubsystem> WeakThis(this);
	Async(EAsyncExecution::ThreadPool, [WeakThis, Surface, Cells, Palette = CachedMaterialPalette, Min = TileFrame.MinAltitude, Max = TileFrame.MaxAltitude]()
	{
		TSharedPtr<const FVoxelMapGlobeAlbedo> Built = BuildGlobeAlbedo(*Surface, Cells, Palette, Min, Max);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Built]()
		{
			UVoxelMapSubsystem* Self = WeakThis.Get();
			if (!Self || Self->bShuttingDown)
			{
				return;
			}
			Self->GlobeAlbedo = Built;
			Self->bGlobeAlbedoBuilding = false;
			Self->GlobeExploredFromCount = -1;
			UE_LOG(LogVoxelMap, Log, TEXT("Globe albedo: %d cells a face edge built in %.2f s."), Built->Grid.GetCellsPerEdge(), Built->BuildSeconds);
			Self->OnMapTileReady.Broadcast(FIntPoint(-1, -1));
		});
	});
	return nullptr;
}

bool UVoxelMapSubsystem::IsGlobeCellExplored(const FIntPoint& AlbedoCell)
{
	if (!GlobeAlbedo.IsValid() || !TileFrame.bPlanet)
	{
		return false;
	}
	// The albedo cells holding an explored tile, refreshed when the exploration has changed (at most once a second past
	// 100,000 explored tiles: a planet's exploration can hold a million).
	const double Now = FPlatformTime::Seconds();
	if (GlobeExploredFromCount != ExploredTiles.Num()
		&& (GlobeExploredFromCount < 0 || ExploredTiles.Num() < 100000 || Now - GlobeExploredRefreshedSeconds >= 1.0))
	{
		GlobeExploredCells.Reset();
		const int32 N = TileFrame.Grid.GetCellsPerEdge();
		const int32 Nc = GlobeAlbedo->Grid.GetCellsPerEdge();
		for (const uint64 Key : ExploredTiles)
		{
			int32 Face = 0, IU = 0, IV = 0;
			if (TileFrame.Grid.FromAtlas(UnpackTileKey(Key), Face, IU, IV))
			{
				GlobeExploredCells.Add(PackTileKey(GlobeAlbedo->Grid.ToAtlas(Face, static_cast<int32>(static_cast<int64>(IU) * Nc / N),
					static_cast<int32>(static_cast<int64>(IV) * Nc / N))));
			}
		}
		GlobeExploredFromCount = ExploredTiles.Num();
		GlobeExploredRefreshedSeconds = Now;
	}
	return GlobeExploredCells.Contains(PackTileKey(AlbedoCell));
}

TSharedPtr<FVoxelMapGlobeAlbedo> UVoxelMapSubsystem::BuildGlobeAlbedo(const FVoxelFarFieldSurface& Surface, int32 CellsPerEdge,
	const TArray<FColor>& MaterialPalette, float AltitudeMin, float AltitudeMax)
{
	const double StartTime = FPlatformTime::Seconds();
	TSharedPtr<FVoxelMapGlobeAlbedo> Out = MakeShared<FVoxelMapGlobeAlbedo>();
	const int32 N = FMath::Max(1, CellsPerEdge);
	Out->Grid = FVoxelCubeSphereGrid(N);
	const int32 Width = 6 * N;
	const FSphericalPlanetWorldMode& Planet = Surface.GetPlanet();
	const double Radius = Planet.GetPlanetParams().PlanetRadius;

	// Pass 1: each cell's altitude and surface material at its centre (no conditioning zones: a POI pad does not show
	// at this size).
	TArray<float> Altitudes;
	TArray<uint8> Materials;
	Altitudes.SetNumUninitialized(Width * N);
	Materials.SetNumUninitialized(Width * N);
	const TArray<FVoxelConditioningZone> NoZones;
	ParallelFor(N, [&](int32 Y)
	{
		for (int32 X = 0; X < Width; ++X)
		{
			const FVector Direction = Out->Grid.CellCentreDirection(FIntPoint(X, Y));
			float Continentalness = 0.0f;
			const float Altitude = Surface.SampleAltitude(FVoxelFarFieldSample::OfDirection(Direction), NoZones, &Continentalness);
			uint8 Material = 0, Biome = 0;
			Surface.SampleConditions(FVoxelFarFieldSample::OfDirection(Direction), Altitude, Continentalness, Material, Biome);
			Altitudes[Y * Width + X] = Altitude;
			Materials[Y * Width + X] = Material;
		}
	});

	// Pass 2: colour as a tile pixel (VoxelMapSubsystem's planet tiles): the sea by depth, land by its material shaded
	// by elevation and by a hillshade from the neighbouring cells on the face (one-sided at a face's edge).
	const FVoxelSeaSurface& Sea = Surface.Sea;
	const bool bSeaOn = Sea.bEnabled && Sea.bRadial;
	const float SeaAltitude = bSeaOn ? static_cast<float>(Sea.Radius - Radius) : AltitudeMin;
	const float LandBase = bSeaOn ? SeaAltitude : AltitudeMin;
	const float LandSpan = FMath::Max(AltitudeMax - LandBase, 1.0f);
	const float DepthSpan = FMath::Max(LandBase - AltitudeMin, 1.0f);
	Out->Colors.SetNumUninitialized(Width * N);
	ParallelFor(N, [&](int32 Y)
	{
		for (int32 X = 0; X < Width; ++X)
		{
			const int32 Index = Y * Width + X;
			const float Altitude = Altitudes[Index];
			FColor Color;
			if (bSeaOn && Altitude < SeaAltitude)
			{
				const float DepthFactor = FMath::Lerp(1.0f, 0.3f, FMath::Clamp((SeaAltitude - Altitude) / DepthSpan, 0.0f, 1.0f));
				Color = FColor(static_cast<uint8>(20 * DepthFactor), static_cast<uint8>(80 * DepthFactor), static_cast<uint8>(180 * DepthFactor), 255);
			}
			else
			{
				Color = MaterialPalette.IsValidIndex(Materials[Index]) ? MaterialPalette[Materials[Index]] : FColor(128, 128, 128, 255);
				const float Elevation = FMath::Lerp(0.75f, 1.0f, FMath::Clamp((Altitude - LandBase) / LandSpan, 0.0f, 1.0f));
				// The cell's neighbours on its face (X wraps within the face's span of the atlas).
				const int32 Face = X / N;
				const int32 IU = X % N;
				auto PointAt = [&](int32 U, int32 V)
				{
					const int32 CU = FMath::Clamp(U, 0, N - 1);
					const int32 CV = FMath::Clamp(V, 0, N - 1);
					const FIntPoint Cell(Face * N + CU, CV);
					return Out->Grid.CellCentreDirection(Cell) * (Radius + Altitudes[CV * Width + Face * N + CU]);
				};
				const FVector Up = Out->Grid.CellCentreDirection(FIntPoint(X, Y));
				FVector Normal = FVector::CrossProduct(PointAt(IU + 1, Y) - PointAt(IU - 1, Y), PointAt(IU, Y + 1) - PointAt(IU, Y - 1)).GetSafeNormal();
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
				const float Shade = FMath::Lerp(0.55f, 1.25f, FMath::Clamp(static_cast<float>(FVector::DotProduct(Normal, LightDir)), 0.0f, 1.0f));
				const float Brightness = Elevation * Shade;
				Color.R = static_cast<uint8>(FMath::Clamp(static_cast<int32>(Color.R * Brightness), 0, 255));
				Color.G = static_cast<uint8>(FMath::Clamp(static_cast<int32>(Color.G * Brightness), 0, 255));
				Color.B = static_cast<uint8>(FMath::Clamp(static_cast<int32>(Color.B * Brightness), 0, 255));
				Color.A = 255;
			}
			Out->Colors[Index] = Color;
		}
	});
	Out->BuildSeconds = FPlatformTime::Seconds() - StartTime;
	return Out;
}
