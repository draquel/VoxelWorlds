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

void UVoxelMapSubsystem::GenerateTileAsync(FIntPoint TileCoord, uint32 Generation)
{
	const uint64 TileKey = PackTileKey(TileCoord);

	// Capture all values needed on the background thread by value. The world mode is OUR standalone
	// instance shared into the task: the task keeps it alive for however long it runs, so PIE
	// teardown order cannot invalidate it. (Capturing the chunk manager's raw pointer here is what
	// crashed on PIE stop — EndPlay->Shutdown() freed it under running tasks.)
	TSharedPtr<const IVoxelWorldMode> WorldMode = CachedWorldMode;
	if (!WorldMode || !TileFrame.CanDrawTile(TileCoord))
	{
		// Drop the request cleanly; the tile is queued again by the next exploration or chunk event.
		TileQueue.Cancel(TileKey);
		return;
	}

	// The tile frame (code review CR2, REND-15: one drawer for the sphere and the plane) and the world's values, by value.
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

	// Terrain conditioning over the tile and its apron (POI pads, campsites): generation layers these onto the base
	// height, so the map must too (QA W-05). Gathered here on the game thread; plain data.
	TArray<FVoxelConditioningZone> Zones;
	if (UVoxelChunkManager* ChunkMgr = CachedChunkManagerWeak.Get())
	{
		Frame.GatherZones(*ChunkMgr, TileCoord, Zones);
	}

	// Edit overlays (R22): this tile's and its neighbours' (the hillshade apron reads their border pixels; across a
	// planet's face edge the neighbour is turned, so apron pixels are found by direction), plus, when the tile is due an
	// overlay rebuild, its chunks gathered now.
	TSharedPtr<const FVoxelMapEditOverlay> OwnOverlay = EditOverlays.FindRef(TileKey);
	TArray<TPair<FIntPoint, TSharedPtr<const FVoxelMapEditOverlay>>, TInlineAllocator<8>> NeighbourOverlays;
	for (int32 DY = -1; DY <= 1; ++DY)
	{
		for (int32 DX = -1; DX <= 1; ++DX)
		{
			const FIntPoint Neighbour = GetNeighbourTile(TileCoord, DX, DY);
			if ((DX != 0 || DY != 0) && Neighbour != TileCoord)
			{
				if (TSharedPtr<const FVoxelMapEditOverlay> Found = EditOverlays.FindRef(PackTileKey(Neighbour)))
				{
					NeighbourOverlays.Add({ Neighbour, Found });
				}
			}
		}
	}
	FVoxelMapOverlayInputs RebuildInput;
	if (OverlayRebuildTiles.Remove(TileKey) > 0)
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
		[WeakThis, TileCoord, Generation, WorldMode, Frame, NoiseParams, bUseBiomes, BiomeSnapshot,
		 FallbackTempNoiseParams, FallbackMoistureNoiseParams, MaterialPalette = MoveTemp(MaterialPalette),
		 Zones = MoveTemp(Zones), OwnOverlay, NeighbourOverlays, RebuildInput, bRebuild]() mutable
	{
		// WorldMode is a TSharedPtr capture, validated non-null before launch — this task co-owns
		// the instance, so it stays valid even if the subsystem and world are torn down while we run.
		const int32 Resolution = Frame.ChunkSize;
		const int32 GridSize = Resolution + 2;
		const int32 NumPixels = Resolution * Resolution;

		// --- Pass 1: the sample at each grid point and the generated surface's altitude there, with a 1-pixel apron on
		// every side. The apron lets the hillshade gradient use real neighbour altitudes at the tile border, so shading
		// is continuous across tile seams instead of flattening at the edges.
		TArray<FVoxelMapTileSample> Samples;
		TArray<float> Altitudes;
		Samples.SetNum(GridSize * GridSize);
		Altitudes.SetNumUninitialized(GridSize * GridSize);
		for (int32 GY = 0; GY < GridSize; ++GY)
		{
			for (int32 GX = 0; GX < GridSize; ++GX)
			{
				const int32 G = GY * GridSize + GX;
				Samples[G] = Frame.SampleAt(TileCoord, GX, GY);
				Altitudes[G] = Frame.Altitude(*WorldMode, Samples[G], NoiseParams, Zones);
			}
		}

		// Surface material as the generators write it at the surface voxel: the SAME shared pipeline as chunk generation,
		// tree placement and PCG (FVoxelSurfaceQuery::QueryMapSurfaceMaterial).
		auto AnalyticMaterialAt = [&](const FVoxelMapTileSample& Sample, float Altitude) -> uint8
		{
			return FVoxelSurfaceQuery::QueryMapSurfaceMaterial(*WorldMode, Frame.PositionAt(Sample, Altitude), Sample.Direction, Altitude,
				Frame.VoxelSize, NoiseParams, BiomeSnapshot, bUseBiomes, Frame.Sea, &Zones, FallbackTempNoiseParams, FallbackMoistureNoiseParams);
		};

		// The tile's own pixels: the overlay build compares against them, and the game thread keeps their altitude range to
		// decide which edited chunks an overlay rebuild needs loaded.
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
		// Rebuild from the gathered chunks when due; columns that wait on an unloaded chunk keep their previous value.
		// Then draw edited columns at their edited altitude, material and water instead of the analytic ones, including
		// the apron cells that belong to neighbouring tiles' overlays.
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

		// Edited altitudes into the grid: the tile's own pixels from its overlay, the apron's from the neighbour holding
		// the apron point's pixel.
		TArray<int32> OverlaySlot;
		OverlaySlot.Init(INDEX_NONE, NumPixels);
		for (int32 GY = 0; GY < GridSize; ++GY)
		{
			for (int32 GX = 0; GX < GridSize; ++GX)
			{
				const int32 G = GY * GridSize + GX;
				const bool bInterior = GX >= 1 && GY >= 1 && GX <= Resolution && GY <= Resolution;
				FIntPoint SourceTile;
				int32 PX = 0;
				int32 PY = 0;
				Frame.ApronPixel(TileCoord, GX, GY, Samples[G], SourceTile, PX, PY);
				const FVoxelMapEditOverlay* Source = nullptr;
				if (bInterior)
				{
					Source = UsableOverlay(Overlay.Get()) ? Overlay.Get() : nullptr;
				}
				else
				{
					for (const TPair<FIntPoint, TSharedPtr<const FVoxelMapEditOverlay>>& Pair : NeighbourOverlays)
					{
						if (Pair.Key == SourceTile && UsableOverlay(Pair.Value.Get()))
						{
							Source = Pair.Value.Get();
							break;
						}
					}
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
						OverlaySlot[PY * Resolution + PX] = Slot;
					}
				}
			}
		}

		// Shading ranges from the world's own height bounds (see ResolveChunkManager): land from the water (or the lowest
		// terrain) to the highest, water depth from the water down to the lowest.
		float LandBase = 0.0f, LandSpan = 1.0f, DepthSpan = 1.0f;
		Frame.GetShadingSpans(LandBase, LandSpan, DepthSpan);
		const float WaterAltitude = Frame.GetWaterAltitude();

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
				const int32 Slot = OverlaySlot[PY * Resolution + PX];
				if (Slot != INDEX_NONE)
				{
					MaterialID = Overlay->Materials[Slot];
					bWater = (Overlay->Flags[Slot] & FVoxelMapEditOverlay::FlagWater) != 0;
				}
				else
				{
					MaterialID = AnalyticMaterialAt(Samples[G], Altitude);
					bWater = Frame.IsWater(Altitude);
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

					// Hillshade from the neighbouring grid points (FVoxelMapTileFrame::ShadeAt).
					const float Shade = Frame.ShadeAt(G, GridSize, Altitudes, Samples);

					const float Brightness = Elevation * Shade;
					Color.R = FMath::Clamp(static_cast<int32>(Color.R * Brightness), 0, 255);
					Color.G = FMath::Clamp(static_cast<int32>(Color.G * Brightness), 0, 255);
					Color.B = FMath::Clamp(static_cast<int32>(Color.B * Brightness), 0, 255);
					Color.A = 255;
				}

				PixelData[PY * Resolution + PX] = Color;
			}
		}

		// Marshal back to game thread
		AsyncTask(ENamedThreads::GameThread,
			[WeakThis, TileCoord, PixelData = MoveTemp(PixelData), Resolution, Generation, bRebuild, Overlay,
			 bOverlayPending, AnalyticMin, AnalyticMax]() mutable
		{
			if (UVoxelMapSubsystem* Self = WeakThis.Get())
			{
				Self->FinishTileDrawing(TileCoord, MoveTemp(PixelData), Resolution, Generation, bRebuild, Overlay, bOverlayPending,
					AnalyticMin, AnalyticMax);
			}
		});
	});
}

void UVoxelMapSubsystem::FinishTileDrawing(FIntPoint TileCoord, TArray<FColor>&& PixelData, int32 Resolution, uint32 Generation,
	bool bRebuild, const TSharedPtr<const FVoxelMapEditOverlay>& Overlay, bool bOverlayPending, float AnalyticMin, float AnalyticMax)
{
	if (bShuttingDown)
	{
		// Deinitialize() already ran (PIE stop / world teardown): don't resurrect cache
		// state, don't broadcast into a dying world, and don't start new tasks.
		UE_LOG(LogVoxelMap, Verbose, TEXT("Discarded tile (%d,%d) completion after shutdown"),
			TileCoord.X, TileCoord.Y);
		return;
	}

	const uint64 Key = PackTileKey(TileCoord);

	if (!TileQueue.Complete(Key, Generation))
	{
		// Invalidated while drawing (an edit refresh or a conditioning change): this result is stale. The tile
		// is waiting again and its cached image stays until the redraw lands; a discarded overlay rebuild runs
		// again with it.
		if (bRebuild)
		{
			OverlayRebuildTiles.Add(Key);
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

			// Neighbours draw this tile's border columns into their hillshade apron: redraw the cached ones
			// when those changed, so shading stays continuous across the tile edge.
			if (!FVoxelMapEditOverlay::BordersEqual(Previous.Get(), Overlay.Get()))
			{
				for (int32 DY = -1; DY <= 1; ++DY)
				{
					for (int32 DX = -1; DX <= 1; ++DX)
					{
						const FIntPoint Neighbour = GetNeighbourTile(TileCoord, DX, DY);
						if ((DX != 0 || DY != 0) && Neighbour != TileCoord && TileStore.Contains(PackTileKey(Neighbour)))
						{
							TileQueue.Request(PackTileKey(Neighbour), /*bInvalidate*/ true, /*bPriority*/ true);
						}
					}
				}
			}
		}

		// The tile's analytic range decides which edited chunks a rebuild needs; an overlay that waited
		// without it may be able to rebuild now.
		const bool bRangeWasKnown = TileAnalyticRange.Contains(Key);
		TileAnalyticRange.Add(Key, FVector2f(AnalyticMin, AnalyticMax));
		if (!bRangeWasKnown && OverlayAwaitingTiles.Contains(Key))
		{
			MarkOverlayDirty(Key);
		}

		// Keep the detail image where something asked for it (the minimap's area, a zoomed-in world map) or the
		// tile already had it; elsewhere only the overview (WM-E2). The store trims to its budgets. An unlimited
		// detail budget (0) keeps every detail image, as the map did before WM-E2.
		const bool bKeepDetail = DetailWanted.Remove(Key) > 0 || TileStore.HasDetail(Key)
			|| TileStore.GetDetailBudget() == 0;
		{
			FScopeLock Lock(&TileMutex);
			TileStore.Store(Key, TileCoord, MoveTemp(PixelData), Resolution, bKeepDetail);
		}

		OnMapTileReady.Broadcast(TileCoord);
	}

	// Every completion drains the queue, so tiles queued while all slots were busy start now.
	LaunchWaitingTiles();
}
