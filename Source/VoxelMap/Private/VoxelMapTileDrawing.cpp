// Copyright Daniel Raquel. All Rights Reserved.

// UVoxelMapSubsystem's tile drawing: a tile's pixels made on a worker and handed back to the game thread, a translation
// unit of the class.

#include "VoxelMapSubsystem.h"
#include "VoxelMap.h"
#include "VoxelChunkManager.h"
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
#include "ProfilingDebugging/CpuProfilerTrace.h"

void UVoxelMapSubsystem::GenerateTileAsync(const FVoxelMapTileId& Id, uint32 Generation)
{
	// The level-0 key of the tile (edit overlays and their rebuilds are level 0's; a coarse tile reads them).
	const FIntPoint TileCoord = Id.Coord;
	const uint64 TileKey = PackTileKey(TileCoord);
	const bool bLevel0 = Id.Level == 0;

	// Capture all values needed on the background thread by value. The world mode is OUR standalone
	// instance shared into the task: the task keeps it alive for however long it runs, so PIE
	// teardown order cannot invalidate it. (Capturing the chunk manager's raw pointer here is what
	// crashed on PIE stop — EndPlay->Shutdown() freed it under running tasks.)
	TSharedPtr<const IVoxelWorldMode> WorldMode = CachedWorldMode;
	if (!WorldMode || !TileFrame.CanDrawTile(Id))
	{
		// Drop the request cleanly; a view asks for the tile again.
		TileQueue.Cancel(Id);
		return;
	}

	// The tile frame (code review CR2, REND-15: one drawer for the sphere and the plane, and for every level of the
	// pyramid) and the world's values, by value.
	const FVoxelMapTileFrame Frame = TileFrame;
	const FVoxelNoiseParams NoiseParams = CachedNoiseParams;
	const bool bUseBiomes = bBiomesEnabled;

	// Atlas-derived map palette (plain data, captured by value).
	TArray<FColor> MaterialPalette = CachedMaterialPalette;

	// Value snapshot of the biome configuration — the background thread never touches the UObject.
	// Carries everything the shared surface-material pipeline needs (biome defs, blend width,
	// underwater + priority-sorted height rules, climate noise settings).
	const FVoxelBiomeSnapshot BiomeSnapshot = FVoxelBiomeSnapshot::FromConfig(CachedBiomeConfig);

	// Fallback climate noise for "biomes on, but no valid config" — matches the CPU generators'
	// hardcoded fallback (static registry biomes with default temperature/moisture fields).
	FVoxelNoiseParams FallbackTempNoiseParams;
	FallbackTempNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	FallbackTempNoiseParams.Octaves = 2;
	FallbackTempNoiseParams.Persistence = 0.5f;
	FallbackTempNoiseParams.Lacunarity = 2.0f;
	FallbackTempNoiseParams.Seed = NoiseParams.Seed + 1234;
	FallbackTempNoiseParams.Frequency = 0.00005f;

	FVoxelNoiseParams FallbackMoistureNoiseParams = FallbackTempNoiseParams;
	FallbackMoistureNoiseParams.Seed = NoiseParams.Seed + 5678;
	FallbackMoistureNoiseParams.Frequency = 0.00007f;

	// Terrain conditioning over the tile's area and its apron (POI pads, campsites): generation layers these onto the
	// base height, so the map must too (QA W-05). Gathered here on the game thread; plain data.
	FVoxelConditioning Conditioning;
	if (UVoxelChunkManager* ChunkMgr = CachedChunkManagerWeak.Get())
	{
		Frame.GatherConditioning(*ChunkMgr, Id, Conditioning);
	}

	// Edit overlays (R22), all level 0's. A level-0 tile: its own and its neighbours' (the hillshade apron reads their
	// border pixels; across a planet's face edge the neighbour is turned, so apron pixels are found by direction), plus,
	// when the tile is due an overlay rebuild, its chunks gathered now. A coarse tile: the overlays of the level-0 tiles
	// under it and one around it (sparse: edits are few), whichever is cheaper to look up.
	TSharedPtr<const FVoxelMapEditOverlay> OwnOverlay = bLevel0 ? EditOverlays.FindRef(TileKey) : nullptr;
	TArray<TPair<FIntPoint, TSharedPtr<const FVoxelMapEditOverlay>>> SourceOverlays;
	if (bLevel0)
	{
		for (int32 DY = -1; DY <= 1; ++DY)
		{
			for (int32 DX = -1; DX <= 1; ++DX)
			{
				const FIntPoint Neighbour = GetNeighbourTile(TileCoord, DX, DY);
				if ((DX != 0 || DY != 0) && Neighbour != TileCoord)
				{
					if (TSharedPtr<const FVoxelMapEditOverlay> Found = EditOverlays.FindRef(PackTileKey(Neighbour)))
					{
						SourceOverlays.Add({ Neighbour, Found });
					}
				}
			}
		}
	}
	else if (EditOverlays.Num() > 0)
	{
		const int64 Span = (1ll << Id.Level) + 2;
		if (Span * Span <= EditOverlays.Num())
		{
			TArray<FIntPoint> Under;
			Frame.Level0TilesOf(Id, 1, Under);
			for (const FIntPoint& Tile0 : Under)
			{
				if (TSharedPtr<const FVoxelMapEditOverlay> Found = EditOverlays.FindRef(PackTileKey(Tile0)))
				{
					SourceOverlays.Add({ Tile0, Found });
				}
			}
		}
		else
		{
			for (const TPair<uint64, TSharedPtr<const FVoxelMapEditOverlay>>& Pair : EditOverlays)
			{
				if (Pair.Value.IsValid() && Frame.CoversLevel0Tile(Id, UnpackTileKey(Pair.Key), 1))
				{
					SourceOverlays.Add({ UnpackTileKey(Pair.Key), Pair.Value });
				}
			}
		}
	}
	FVoxelMapOverlayInputs RebuildInput;
	if (bLevel0 && OverlayRebuildTiles.Remove(TileKey) > 0)
	{
		const double GatherStart = FPlatformTime::Seconds();
		LastOverlayGatherFrame = GFrameCounter;
		RebuildInput = GatherOverlayInputs(TileCoord);
		UE_LOG(LogVoxelMap, Verbose, TEXT("Overlay gather (%d,%d): %d chunk(s) in %.3f ms on the game thread."),
			TileCoord.X, TileCoord.Y, RebuildInput.NumChunks(), (FPlatformTime::Seconds() - GatherStart) * 1000.0);
	}
	const bool bRebuild = RebuildInput.IsValid();

	// Use a weak pointer to safely call back to the subsystem
	TWeakObjectPtr<UVoxelMapSubsystem> WeakThis(this);

	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
		[WeakThis, Id, Generation, WorldMode, Frame, NoiseParams, bUseBiomes, BiomeSnapshot,
		 FallbackTempNoiseParams, FallbackMoistureNoiseParams, MaterialPalette = MoveTemp(MaterialPalette),
		 Conditioning = MoveTemp(Conditioning), OwnOverlay, SourceOverlays = MoveTemp(SourceOverlays), RebuildInput, bRebuild]() mutable
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(VoxelMap_DrawTile);
		const double DrawStart = FPlatformTime::Seconds();
		// WorldMode is a TSharedPtr capture, validated non-null before launch — this task co-owns
		// the instance, so it stays valid even if the subsystem and world are torn down while we run.
		const bool bLevel0 = Id.Level == 0;
		const int32 Resolution = Frame.ChunkSize;
		const int32 GridSize = Resolution + 2;
		const int32 NumPixels = Resolution * Resolution;

		// --- Pass 1: the sample at each grid point and the generated surface's altitude there, with a 1-pixel apron on
		// every side. The apron lets the hillshade gradient use real neighbour altitudes at the tile border, so shading
		// is continuous across tile seams instead of flattening at the edges. A coarse tile's points are 2^L voxel
		// columns apart, each a point sample.
		TArray<FVoxelMapTileSample> Samples;
		TArray<float> Altitudes;
		Samples.SetNum(GridSize * GridSize);
		Altitudes.SetNumUninitialized(GridSize * GridSize);
		for (int32 GY = 0; GY < GridSize; ++GY)
		{
			for (int32 GX = 0; GX < GridSize; ++GX)
			{
				const int32 G = GY * GridSize + GX;
				Samples[G] = Frame.SampleAt(Id, GX, GY);
				Altitudes[G] = Frame.Altitude(*WorldMode, Samples[G], NoiseParams, Conditioning);
			}
		}

		// Surface material as the generators write it at the surface voxel: the SAME shared pipeline as chunk generation,
		// tree placement and PCG (FVoxelSurfaceQuery::QueryMapSurfaceMaterial).
		auto AnalyticMaterialAt = [&](const FVoxelMapTileSample& Sample, float Altitude) -> uint8
		{
			return FVoxelSurfaceQuery::QueryMapSurfaceMaterial(*WorldMode, Frame.PositionAt(Sample, Altitude), Sample.Direction, Altitude,
				Frame.VoxelSize, NoiseParams, BiomeSnapshot, bUseBiomes, Frame.Water, &Conditioning, FallbackTempNoiseParams, FallbackMoistureNoiseParams);
		};

		// The tile's own pixels: the overlay build compares against them, and the game thread keeps a level-0 tile's
		// altitude range to decide which edited chunks an overlay rebuild needs loaded.
		TArray<FVoxelMapTileSample> PixelSamples;
		TArray<FVector> PixelDirections;
		TArray<float> AnalyticAltitudes;
		PixelSamples.SetNum(NumPixels);
		PixelDirections.SetNumUninitialized(NumPixels);
		AnalyticAltitudes.SetNumUninitialized(NumPixels);
		float AnalyticMin = TNumericLimits<float>::Max();
		float AnalyticMax = TNumericLimits<float>::Lowest();
		for (int32 PY = 0; PY < Resolution; ++PY)
		{
			for (int32 PX = 0; PX < Resolution; ++PX)
			{
				const int32 G = (PY + 1) * GridSize + PX + 1;
				const int32 Index = PY * Resolution + PX;
				PixelSamples[Index] = Samples[G];
				PixelDirections[Index] = Samples[G].Direction;
				AnalyticAltitudes[Index] = Altitudes[G];
				AnalyticMin = FMath::Min(AnalyticMin, Altitudes[G]);
				AnalyticMax = FMath::Max(AnalyticMax, Altitudes[G]);
			}
		}

		// --- Edit overlay (R22) ---
		// Rebuild from the gathered chunks when due (level 0 only); columns that wait on an unloaded chunk keep their
		// previous value. Then draw edited columns at their edited altitude, material and water instead of the analytic
		// ones, including the apron cells that belong to neighbouring tiles' overlays.
		bool bOverlayPending = false;
		if (RebuildInput.IsValid())
		{
			const FVoxelMapEditOverlay Built = FVoxelMapEditOverlay::Build(RebuildInput, PixelDirections, AnalyticAltitudes,
				[&](int32 PX, int32 PY)
				{
					const int32 Index = PY * Resolution + PX;
					return AnalyticMaterialAt(PixelSamples[Index], AnalyticAltitudes[Index]);
				});
			bOverlayPending = Built.PendingColumns.Num() > 0;
			FVoxelMapEditOverlay Merged = FVoxelMapEditOverlay::MergePending(Built, OwnOverlay.Get());
			Merged.PendingColumns.Empty();
			OwnOverlay.Reset();
			if (!Merged.IsEmpty())
			{
				OwnOverlay = MakeShared<const FVoxelMapEditOverlay>(MoveTemp(Merged));
			}
			RebuildInput.Reset(); // release the snapshots before drawing
		}
		const TSharedPtr<const FVoxelMapEditOverlay> Overlay = OwnOverlay;
		auto UsableOverlay = [Resolution, bPlanetKind = Frame.bPlanet](const FVoxelMapEditOverlay* Candidate)
		{
			return Candidate && Candidate->Resolution == Resolution && Candidate->bAltitudeHeights == bPlanetKind;
		};

		// Edited altitudes into the grid. A level-0 tile's own pixels come from its overlay, the apron's from the
		// neighbour holding the apron point's pixel; a coarse tile's points each from the level-0 overlay its point falls
		// in (FVoxelMapTileFrame::SourcePixel). Without overlays nearby (most tiles) this is skipped.
		TArray<int32> OverlaySlot;
		TArray<const FVoxelMapEditOverlay*> PixelOverlay;
		OverlaySlot.Init(INDEX_NONE, NumPixels);
		PixelOverlay.Init(nullptr, NumPixels);
		if (UsableOverlay(Overlay.Get()) || SourceOverlays.Num() > 0)
		{
			TMap<FIntPoint, const FVoxelMapEditOverlay*> SourceByTile;
			for (const TPair<FIntPoint, TSharedPtr<const FVoxelMapEditOverlay>>& Pair : SourceOverlays)
			{
				if (UsableOverlay(Pair.Value.Get()))
				{
					SourceByTile.Add(Pair.Key, Pair.Value.Get());
				}
			}
			FIntPoint LastTile(MAX_int32, MAX_int32);
			const FVoxelMapEditOverlay* LastSource = nullptr;
			for (int32 GY = 0; GY < GridSize; ++GY)
			{
				for (int32 GX = 0; GX < GridSize; ++GX)
				{
					const int32 G = GY * GridSize + GX;
					const bool bInterior = GX >= 1 && GY >= 1 && GX <= Resolution && GY <= Resolution;
					FIntPoint SourceTile;
					int32 PX = 0;
					int32 PY = 0;
					Frame.SourcePixel(Id, GX, GY, Samples[G], SourceTile, PX, PY);
					const FVoxelMapEditOverlay* Source = nullptr;
					if (bLevel0 && bInterior)
					{
						Source = UsableOverlay(Overlay.Get()) ? Overlay.Get() : nullptr;
					}
					else
					{
						if (SourceTile != LastTile)
						{
							LastTile = SourceTile;
							LastSource = SourceByTile.FindRef(SourceTile);
						}
						Source = LastSource;
					}
					if (!Source)
					{
						continue;
					}
					const int32 Slot = Source->FindColumn(PX, PY);
					if (Slot != INDEX_NONE)
					{
						Altitudes[G] = Source->Heights[Slot];
						if (bInterior)
						{
							const int32 Index = (GY - 1) * Resolution + (GX - 1);
							OverlaySlot[Index] = Slot;
							PixelOverlay[Index] = Source;
						}
					}
				}
			}
		}

		// Shading ranges from the world's own height bounds (see ResolveChunkManager): land from the water (or the lowest
		// terrain) to the highest, water depth from the water down to the lowest.
		// The tile's water: the levels' range over the tile's own bounds, one level a tile.
		const FVoxelWaterLevelRange TileWater = Frame.GetTileWater(Id);
		float LandBase = 0.0f, LandSpan = 1.0f, DepthSpan = 1.0f;
		Frame.GetShadingSpans(TileWater, LandBase, LandSpan, DepthSpan);
		const float WaterAltitude = Frame.GetWaterAltitude(TileWater);

		// --- Pass 2: colour ---
		TArray<FColor> PixelData;
		PixelData.SetNumUninitialized(NumPixels);
		for (int32 PY = 0; PY < Resolution; ++PY)
		{
			for (int32 PX = 0; PX < Resolution; ++PX)
			{
				const int32 GX = PX + 1;
				const int32 GY = PY + 1;
				const int32 G = GY * GridSize + GX;
				const float Altitude = Altitudes[G];

				// Edited columns carry their own material and water: water there comes from the voxels (a pit dug
				// below the water line stays land until water floods it), not from the analytic altitude test.
				uint8 MaterialID = 0;
				bool bWater = false;
				const int32 Index = PY * Resolution + PX;
				const int32 Slot = OverlaySlot[Index];
				if (Slot != INDEX_NONE)
				{
					const FVoxelMapEditOverlay* Source = PixelOverlay[Index];
					MaterialID = Source->Materials[Slot];
					bWater = (Source->Flags[Slot] & FVoxelMapEditOverlay::FlagWater) != 0;
				}
				else
				{
					MaterialID = AnalyticMaterialAt(Samples[G], Altitude);
					bWater = Frame.IsWater(TileWater, Altitude);
				}

				FColor Color;
				if (bWater)
				{
					// Submerged terrain — render as water, deeper = darker blue. Depth is normalized against the world's
					// own terrain floor rather than a fixed constant, so shallow-water configs still get a readable gradient.
					const float Depth = WaterAltitude - Altitude;
					const float DepthFactor = FMath::Lerp(1.0f, 0.3f, FMath::Clamp(Depth / DepthSpan, 0.0f, 1.0f));
					Color.R = FMath::Clamp(static_cast<int32>(20 * DepthFactor), 0, 255);
					Color.G = FMath::Clamp(static_cast<int32>(80 * DepthFactor), 0, 255);
					Color.B = FMath::Clamp(static_cast<int32>(180 * DepthFactor), 0, 255);
					Color.A = 255;
				}
				else
				{
					// Atlas-baked average albedo of the real terrain texture (registry palette where a material has no
					// baked colour).
					Color = MaterialPalette[MaterialID];

					// Elevation tint — subtle darkening of low land toward the reference level, spanning the world's
					// actual height range.
					const float ElevationFactor = FMath::Clamp((Altitude - LandBase) / LandSpan, 0.0f, 1.0f);
					const float Elevation = FMath::Lerp(0.75f, 1.0f, ElevationFactor);

					// Hillshade from the neighbouring grid points (FVoxelMapTileFrame::ShadeAt), at the level's spacing.
					const float Shade = Frame.ShadeAt(G, GridSize, Altitudes, Samples, Id.Level);

					const float Brightness = Elevation * Shade;
					Color.R = FMath::Clamp(static_cast<int32>(Color.R * Brightness), 0, 255);
					Color.G = FMath::Clamp(static_cast<int32>(Color.G * Brightness), 0, 255);
					Color.B = FMath::Clamp(static_cast<int32>(Color.B * Brightness), 0, 255);
					Color.A = 255;
				}

				PixelData[Index] = Color;
			}
		}

		// Marshal back to game thread, with the worker time the drawing took (voxel.Map.Stats).
		const double DrawSeconds = FPlatformTime::Seconds() - DrawStart;
		AsyncTask(ENamedThreads::GameThread,
			[WeakThis, Id, PixelData = MoveTemp(PixelData), Resolution, Generation, bRebuild, Overlay,
			 bOverlayPending, AnalyticMin, AnalyticMax, DrawSeconds]() mutable
		{
			if (UVoxelMapSubsystem* Self = WeakThis.Get())
			{
				Self->DrawStats.DrawSeconds += DrawSeconds;
				Self->FinishTileDrawing(Id, MoveTemp(PixelData), Resolution, Generation, bRebuild, Overlay, bOverlayPending,
					AnalyticMin, AnalyticMax);
			}
		});
	});
}

void UVoxelMapSubsystem::FinishTileDrawing(const FVoxelMapTileId& Id, TArray<FColor>&& PixelData, int32 Resolution, uint32 Generation,
	bool bRebuild, const TSharedPtr<const FVoxelMapEditOverlay>& Overlay, bool bOverlayPending, float AnalyticMin, float AnalyticMax)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelMap_FinishTileDrawing);
	if (bShuttingDown)
	{
		// Deinitialize() already ran (PIE stop / world teardown): don't resurrect cache
		// state, don't broadcast into a dying world, and don't start new tasks.
		UE_LOG(LogVoxelMap, Verbose, TEXT("Discarded tile %s completion after shutdown"), *Id.ToString());
		return;
	}

	const FIntPoint TileCoord = Id.Coord;
	const uint64 Key = PackTileKey(TileCoord);

	if (!TileQueue.Complete(Id, Generation))
	{
		// Invalidated while drawing (an edit refresh or a conditioning change): this result is stale. Its image stays
		// until the redraw lands (queued again for each view showing it); a discarded overlay rebuild runs again.
		if (bRebuild)
		{
			OverlayRebuildTiles.Add(Key);
			TileQueue.Add(EVoxelMapRequester::Edits, Id);
		}
	}
	else
	{
		if (bRebuild)
		{
			const TSharedPtr<const FVoxelMapEditOverlay> Previous = EditOverlays.FindRef(Key);
			if (Overlay.IsValid())
			{
				EditOverlays.Add(Key, Overlay);
			}
			else
			{
				EditOverlays.Remove(Key);
			}
			if (bOverlayPending)
			{
				OverlayAwaitingTiles.Add(Key);
			}

			// Neighbours draw this tile's border columns into their hillshade apron: their images go stale when those
			// changed (redrawn now where a view shows them), so shading stays continuous across the tile edge.
			if (!FVoxelMapEditOverlay::BordersEqual(Previous.Get(), Overlay.Get()))
			{
				for (int32 DY = -1; DY <= 1; ++DY)
				{
					for (int32 DX = -1; DX <= 1; ++DX)
					{
						const FIntPoint Neighbour = GetNeighbourTile(TileCoord, DX, DY);
						if ((DX != 0 || DY != 0) && Neighbour != TileCoord)
						{
							InvalidateTile(FVoxelMapTileId(Neighbour));
						}
					}
				}
			}

			// The coarser tiles over this one read its overlay under their pixels (MAP_PYRAMID_PLAN.md P1).
			const bool bOverlayChanged = Previous.IsValid() != Overlay.IsValid() || (Overlay.IsValid() && !(*Previous == *Overlay));
			if (bOverlayChanged)
			{
				InvalidateAncestors(TileCoord);
			}
		}

		// A level-0 tile's analytic range decides which edited chunks a rebuild needs; an overlay that waited without it
		// may be able to rebuild now.
		if (Id.Level == 0)
		{
			const bool bRangeWasKnown = TileAnalyticRange.Contains(Key);
			TileAnalyticRange.Add(Key, FVector2f(AnalyticMin, AnalyticMax));
			if (!bRangeWasKnown && OverlayAwaitingTiles.Contains(Key))
			{
				MarkOverlayDirty(Key);
			}
		}

		// The store trims to its budget, least recently used first, at every level alike.
		++DrawStats.Drawn;
		DrawStats.Replaced += TileStore.Contains(Id) ? 1 : 0;
		{
			FScopeLock Lock(&TileMutex);
			TileStore.Store(Id, MoveTemp(PixelData), Resolution);
		}

		OnMapTileReady.Broadcast(Id);
	}

	// Every completion drains the queue, so tiles queued while all slots were busy start now.
	LaunchWaitingTiles();
}
