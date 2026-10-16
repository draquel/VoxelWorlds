// Copyright Daniel Raquel. All Rights Reserved.

#include "DistanceBandLODStrategy.h"
#include "VoxelLOD.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelBiomeConfiguration.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelWorldModeFactory.h"
#include "VoxelCoordinates.h"
#include "DrawDebugHelpers.h"
#include "HAL/IConsoleManager.h"

// P2-B: toggle the LOD adjacency balance + hysteresis at runtime, so the
// >=2-level-gap count (see voxel.LogBoundaryResidency) can be compared on/off in
// a single live session. 1 = balanced (default), 0 = raw distance bands (legacy).
static TAutoConsoleVariable<int32> CVarLODBalance(
	TEXT("voxel.LODBalance"),
	1,
	TEXT("Enforce the 2:1 LOD adjacency balance + hysteresis in FDistanceBandLODStrategy. 1=on (default), 0=raw distance bands."),
	ECVF_Default);

// When the 2:1 balance runs, balance across all 26 neighbours (6 faces + 12 edges + 8 corners)
// instead of just the 6 faces. Face-only balancing leaves diagonal (edge/corner) neighbours able
// to differ by 2 LOD levels, so three LODs can meet at a chunk corner (LOD0/1/2 junction) and the
// per-face transvoxel/morph can't stitch it. Full 26-neighbour balancing guarantees every pair of
// vertex-sharing chunks differs by <=1, so at most two LOD levels ever meet at any corner — no
// 3-LOD junction, independent of band widths. 1=on (default), 0=face-only (legacy).
static TAutoConsoleVariable<int32> CVarLODBalanceDiagonal(
	TEXT("voxel.LODBalanceDiagonal"),
	1,
	TEXT("2:1 LOD balance across all 26 neighbours (edges+corners), not just 6 faces. Prevents 3-LOD corner junctions. 1=on (default), 0=face-only."),
	ECVF_Default);

FDistanceBandLODStrategy::FDistanceBandLODStrategy()
{
	// Default LOD bands will be set during Initialize()
}

void FDistanceBandLODStrategy::Initialize(const UVoxelWorldConfiguration* WorldConfig)
{
	if (!WorldConfig)
	{
		UE_LOG(LogVoxelLOD, Error, TEXT("FDistanceBandLODStrategy::Initialize called with null config"));
		return;
	}

	// Cache configuration values
	VoxelSize = WorldConfig->VoxelSize;
	BaseChunkSize = WorldConfig->ChunkSize;
	WorldMode = WorldConfig->WorldMode;
	bEnableLOD = WorldConfig->IsLODEnabled(); // off for cubic regardless of bEnableLOD
	bEnableMorphing = WorldConfig->bEnableLODMorphing;
	bEnableFrustumCulling = WorldConfig->bEnableFrustumCulling;

	// Asymmetric LOD hysteresis (eager refine / damped coarsen). Read from config, then
	// allow -VoxelLODRefineHyst / -VoxelLODCoarsenHyst command-line overrides for A/B.
	RefineHysteresisFraction = FMath::Max(0.0f, WorldConfig->LODRefineHysteresis);
	CoarsenHysteresisFraction = FMath::Max(0.0f, WorldConfig->LODCoarsenHysteresis);
	float HystOverride;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelLODRefineHyst="), HystOverride)) { RefineHysteresisFraction = FMath::Max(0.0f, HystOverride); }
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelLODCoarsenHyst="), HystOverride)) { CoarsenHysteresisFraction = FMath::Max(0.0f, HystOverride); }

	// Stability invariant. Adjacent bands share an edge E, so ApplyLODHysteresis refines at
	// E + Refine*ChunkW and coarsens at E + Coarsen*ChunkW. Refine > Coarsen would order those
	// thresholds the wrong way round and a chunk sitting between them would flip LOD every
	// frame. Nothing else enforces this, and both values are freely editable on the config.
	if (RefineHysteresisFraction > CoarsenHysteresisFraction)
	{
		UE_LOG(LogVoxelLOD, Warning,
			TEXT("LODRefineHysteresis (%.2f) exceeds LODCoarsenHysteresis (%.2f); clamping refine to coarsen. ")
			TEXT("Refine must not exceed coarsen or LOD transitions oscillate at band edges."),
			RefineHysteresisFraction, CoarsenHysteresisFraction);
		RefineHysteresisFraction = CoarsenHysteresisFraction;
	}

	// Copy LOD bands
	LODBands = WorldConfig->LODBands;

	// Sort bands by distance
	LODBands.Sort([](const FLODBand& A, const FLODBand& B)
	{
		return A.MinDistance < B.MinDistance;
	});

	// Use ViewDistance from configuration as the authoritative max distance
	// This allows easy control over render distance independent of LOD band setup
	MaxViewDistance = WorldConfig->ViewDistance;

	// Benchmark/iteration override: -VoxelLODScale=N scales every band boundary
	// (Min/Max/MorphRange) by N while leaving MaxViewDistance (the field size /
	// chunk budget) fixed. N<1 pulls the fine-LOD reach inward (coarser sooner =
	// less re-mesh thrash, lower visual detail); N>1 pushes it outward (clamped to
	// view distance). Lets us A/B the thrash-vs-detail tradeoff without re-authoring
	// the config. Total LOD-transition thrash scales ~linearly with the sum of band
	// radii for a planar traverse, so this is the primary band-tuning thrash lever.
	float LODScale = 1.0f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelLODScale="), LODScale) && LODScale > 0.0f && !FMath::IsNearlyEqual(LODScale, 1.0f))
	{
		for (FLODBand& Band : LODBands)
		{
			Band.MinDistance = FMath::Min(Band.MinDistance * LODScale, MaxViewDistance);
			Band.MaxDistance = FMath::Min(Band.MaxDistance * LODScale, MaxViewDistance);
			Band.MorphRange *= LODScale;
		}
		UE_LOG(LogVoxelLOD, Warning, TEXT("  -VoxelLODScale=%.3f applied: band boundaries scaled (view distance fixed at %.0f)"),
			LODScale, MaxViewDistance);
	}

	// Cache world-mode-specific parameters first (needed for vertical range calculation)
	const float ChunkWorldSize = BaseChunkSize * VoxelSize;

	// The configured world mode, from the builder generation uses (WM-A1). Height for the culls below comes from it
	// (WM-A3), not from InfinitePlane's static math.
	TerrainWorldMode = FVoxelWorldModeFactory::Create(*WorldConfig);

	// Cache terrain bounds for vertical culling (applies to Infinite Plane and Island modes — both use
	// 2D heightmap terrain). The height EXTENT is owned by the world mode (IVoxelWorldMode::GetTerrainHeightBounds,
	// the authority for vertical culling), which accounts for the full noise amplitude AND continentalness — and for
	// an island, the floor beyond its edge — so these culling bounds can never drift from what the generator
	// actually produces. (A former inline estimate omitted the -HeightScale valley depth on the min side and could
	// cull chunks holding real terrain.)
	if (WorldMode == EWorldMode::InfinitePlane || WorldMode == EWorldMode::IslandBowl)
	{
		float BoundsMin = 0.0f;
		float BoundsMax = 0.0f;
		TerrainWorldMode->GetTerrainHeightBounds(BoundsMin, BoundsMax);

		// One chunk of buffer on each side for terrain variation / meshing / edit slack.
		TerrainMinHeight = BoundsMin - ChunkWorldSize;
		TerrainMaxHeight = BoundsMax + ChunkWorldSize;

		UE_LOG(LogVoxelLOD, Log, TEXT("  Terrain bounds culling: Height range [%.0f - %.0f]"),
			TerrainMinHeight, TerrainMaxHeight);
	}

	// The vertical window comes from the terrain in heightmap worlds (D8, WM-S1): every column streams its own surface
	// slab, sampled from the world mode (WM-A3) so it follows the height generation produces, islands included (KI5).
	// Near the viewer the viewer's layer ± NearWindowLayers is added (GetColumnWindow).
	bSurfaceSlabCulling = WorldConfig->bFarBandSurfaceSlabCulling && IsHeightmapWorld();
	ColumnSurfaceCache.Reset();
	if (bSurfaceSlabCulling)
	{
		SlabNoiseParams = WorldConfig->NoiseParams;
		bSlabHasWater = WorldConfig->bEnableWaterLevel;
		SlabWaterLevel = WorldConfig->WaterLevel + WorldConfig->WorldOrigin.Z; // the water plane is relative to the origin
		UE_LOG(LogVoxelLOD, Log, TEXT("  Surface slab ENABLED: every column keeps its surface ±%.1f chunks%s at every distance"),
			SlabSlackChunks, bSlabHasWater ? TEXT(" (up to the water plane)") : TEXT(""));
	}

	// Cache Island mode parameters for boundary culling, from the builder generation uses (WM-A1): the centre is
	// absolute, so the cull and the generated island can no longer disagree when WorldOrigin is not zero (KI3).
	if (WorldMode == EWorldMode::IslandBowl)
	{
		const FIslandBowlParams Island = FVoxelWorldModeFactory::MakeIslandParams(*WorldConfig);
		IslandShape = static_cast<int32>(Island.Shape);
		IslandTotalExtentX = Island.GetTotalExtentX();
		IslandTotalExtentY = Island.GetTotalExtentY();
		IslandCenter = FVector2D(Island.CenterX, Island.CenterY);
		IslandBounds = TerrainWorldMode->GetWorldBounds();

		UE_LOG(LogVoxelLOD, Log, TEXT("  Island culling enabled: Shape=%s, ExtentX=%.0f, ExtentY=%.0f, Center=(%.0f, %.0f)"),
			IslandShape == 1 ? TEXT("Rectangle") : TEXT("Circular"),
			IslandTotalExtentX, IslandTotalExtentY,
			IslandCenter.X, IslandCenter.Y);
	}

	// Cache Spherical Planet parameters for shell culling (SP-A6: the same builder)
	if (WorldMode == EWorldMode::SphericalPlanet)
	{
		const FSphericalPlanetParams Planet = FVoxelWorldModeFactory::MakePlanetParams(*WorldConfig);
		PlanetRadius = Planet.PlanetRadius;
		PlanetMaxTerrainHeight = Planet.MaxTerrainHeight;
		PlanetMaxTerrainDepth = Planet.MaxTerrainDepth;
		PlanetCenter = Planet.PlanetCenter;

		UE_LOG(LogVoxelLOD, Log, TEXT("  Shell culling enabled: Radius=%.0f, Height=%.0f, Depth=%.0f"),
			PlanetRadius, PlanetMaxTerrainHeight, PlanetMaxTerrainDepth);
	}

	// The near window (heightmap worlds): band 0's outer edge plus two chunk widths, measured horizontally.
	NearWindowRadius = (LODBands.Num() > 0) ? LODBands[0].MaxDistance + 2.0f * ChunkWorldSize : 0.0f;
	if (IsHeightmapWorld())
	{
		UE_LOG(LogVoxelLOD, Log, TEXT("  Vertical window from the terrain: the viewer's layer ±%d within %.0f uu, horizon measured horizontally"),
			NearWindowLayers, NearWindowRadius);
	}

	// The viewer-relative vertical window: SphericalPlanet only (heightmap worlds use GetColumnWindow's terrain window)
	switch (WorldMode)
	{
		case EWorldMode::SphericalPlanet:
		{
			// For spherical planets, calculate vertical range based on terrain shell thickness
			// The terrain shell extends from (Radius - Depth) to (Radius + Height)
			// We need chunks that can intersect this shell within view distance
			const float ShellThickness = PlanetMaxTerrainHeight + PlanetMaxTerrainDepth;
			const float MaxVerticalExtent = FMath::Min(MaxViewDistance, PlanetRadius + PlanetMaxTerrainHeight);

			// Calculate chunk range needed to cover the vertical extent
			// Use a reasonable range based on view distance, not the full planet
			const int32 VerticalChunkRange = FMath::CeilToInt(MaxVerticalExtent / ChunkWorldSize) + 1;

			// Clamp to reasonable limits
			MinVerticalChunks = -FMath::Min(VerticalChunkRange, 16);
			MaxVerticalChunks = FMath::Min(VerticalChunkRange, 16);

			UE_LOG(LogVoxelLOD, Log, TEXT("  Spherical vertical range: %d to %d chunks (shell thickness=%.0f)"),
				MinVerticalChunks, MaxVerticalChunks, ShellThickness);
			break;
		}

		default:
			break;
	}
	bIsInitialized = true;
	bBalancedLODCacheDirty = true; // bands, view distance or bounds may have changed

	// Calculate expected chunk radius for reference (ChunkWorldSize already defined above)
	const int32 ExpectedChunkRadius = FMath::CeilToInt(MaxViewDistance / ChunkWorldSize) + 1;

	UE_LOG(LogVoxelLOD, Log, TEXT("FDistanceBandLODStrategy initialized:"));
	UE_LOG(LogVoxelLOD, Log, TEXT("  ViewDistance: %.0f, LOD Bands: %d, LOD Enabled: %s"),
		MaxViewDistance, LODBands.Num(), bEnableLOD ? TEXT("Yes") : TEXT("No"));
	UE_LOG(LogVoxelLOD, Log, TEXT("  ChunkWorldSize: %.0f, ExpectedChunkRadius: %d (~%d chunks per Z level)"),
		ChunkWorldSize, ExpectedChunkRadius, (ExpectedChunkRadius * 2 + 1) * (ExpectedChunkRadius * 2 + 1));

	if (LODBands.Num() > 0)
	{
		UE_LOG(LogVoxelLOD, Log, TEXT("  LOD Band range: 0 - %.0f (last band max)"),
			LODBands.Last().MaxDistance);
		if (MaxViewDistance > LODBands.Last().MaxDistance)
		{
			UE_LOG(LogVoxelLOD, Log, TEXT("  Note: ViewDistance extends %.0f beyond last LOD band"),
				MaxViewDistance - LODBands.Last().MaxDistance);
		}
	}
}

void FDistanceBandLODStrategy::Update(const FLODQueryContext& Context, float DeltaTime)
{
	// Cache viewer position and world origin for quick access
	CachedViewerPosition = Context.ViewerPosition;
	CachedWorldOrigin = Context.WorldOrigin;
	CachedViewerChunk = WorldPosToChunkCoord(Context.ViewerPosition);

	// Recompute the balanced LOD assignment when the viewer has moved. Must run before any
	// GetLODForChunk / GetVisibleChunks query (the interface guarantees Update is called
	// first each frame), so all consumers read a consistent, balanced map.
	if (ShouldRebuildBalancedLODCache(Context))
	{
		RebuildBalancedLODCache(Context);
	}
}

bool FDistanceBandLODStrategy::ShouldRebuildBalancedLODCache(const FLODQueryContext& Context) const
{
	return bBalancedLODCacheDirty
		|| CachedViewerChunk != LastRebuildViewerChunk
		|| FVector::DistSquared(Context.ViewerPosition, LastRebuildViewerPosition) >= FMath::Square(BalancedLODCacheMoveThreshold)
		|| !Context.WorldOrigin.Equals(LastRebuildWorldOrigin)
		|| CVarLODBalance.GetValueOnGameThread() != LastRebuildBalanceCVar
		|| CVarLODBalanceDiagonal.GetValueOnGameThread() != LastRebuildDiagonalCVar;
}

int32 FDistanceBandLODStrategy::GetRawLODForDistance(float Distance) const
{
	const FLODBand* Band = FindBandForDistance(Distance);
	if (Band)
	{
		return Band->LODLevel;
	}

	// Beyond all bands - return coarsest LOD
	if (LODBands.Num() > 0)
	{
		return LODBands.Last().LODLevel;
	}

	return 0;
}

int32 FDistanceBandLODStrategy::GetLODForChunk(
	const FIntVector& ChunkCoord,
	const FLODQueryContext& Context) const
{
	// When LOD is disabled, always return LOD 0 (full detail)
	if (!bEnableLOD)
	{
		return 0;
	}

	// Prefer the balanced cache (2:1 adjacency + hysteresis). Chunks outside the
	// candidate volume (e.g. transient queue entries beyond view distance) fall
	// back to the raw distance-band LOD.
	if (const int32* Balanced = BalancedLODCache.Find(ChunkCoord))
	{
		return *Balanced;
	}

	const FVector ChunkCenter = ChunkCoordToWorldCenter(ChunkCoord);
	const float Distance = GetDistanceToViewer(ChunkCenter, Context);
	return GetRawLODForDistance(Distance);
}

int32 FDistanceBandLODStrategy::GetDistanceBandLOD(
	const FIntVector& ChunkCoord,
	const FLODQueryContext& Context) const
{
	if (bEnableLOD)
	{
		return GetLODForChunk(ChunkCoord, Context);
	}

	// LOD off: no balance or hysteresis runs, so read the bands as a plain distance table.
	return GetRawLODForDistance(GetDistanceToViewer(ChunkCoordToWorldCenter(ChunkCoord), Context));
}

float FDistanceBandLODStrategy::GetLODMorphFactor(
	const FIntVector& ChunkCoord,
	const FLODQueryContext& Context) const
{
	if (!bEnableMorphing)
	{
		return 0.0f;
	}

	const FVector ChunkCenter = ChunkCoordToWorldCenter(ChunkCoord);
	const float Distance = GetDistanceToViewer(ChunkCenter, Context);

	const FLODBand* Band = FindBandForDistance(Distance);
	if (Band)
	{
		return Band->GetMorphFactor(Distance);
	}

	return 0.0f;
}

TArray<FChunkLODRequest> FDistanceBandLODStrategy::GetVisibleChunks(
	const FLODQueryContext& Context) const
{
	TArray<FChunkLODRequest> Requests;

	if (LODBands.Num() == 0)
	{
		return Requests;
	}

	const FIntVector ViewerChunk = WorldPosToChunkCoord(Context.ViewerPosition);

	// Calculate the maximum chunk radius needed
	const float ChunkWorldSize = BaseChunkSize * VoxelSize;
	const int32 MaxChunkRadius = FMath::CeilToInt(MaxViewDistance / ChunkWorldSize) + 1;

	// Every column within the horizon, and in each column the layers of its vertical window (absolute layers: the
	// terrain's in heightmap worlds, the viewer's on a planet).
	for (int32 X = -MaxChunkRadius; X <= MaxChunkRadius; ++X)
	{
		for (int32 Y = -MaxChunkRadius; Y <= MaxChunkRadius; ++Y)
		{
			const int32 ColumnX = ViewerChunk.X + X;
			const int32 ColumnY = ViewerChunk.Y + Y;

			// Heightmap worlds: the horizon is horizontal, so a whole column is in range or not.
			if (IsHeightmapWorld() && GetHorizonDistance(ChunkCoordToWorldCenter(FIntVector(ColumnX, ColumnY, 0)), Context.ViewerPosition) > MaxViewDistance)
			{
				continue;
			}

			const FColumnWindow Window = GetColumnWindow(ColumnX, ColumnY, Context, EWindowUse::Load);
			Window.ForEachLayer([&](int32 Z)
			{
				const FIntVector ChunkCoord(ColumnX, ColumnY, Z);
				const FVector ChunkCenter = ChunkCoordToWorldCenter(ChunkCoord);

				// Skip if beyond the load horizon (per chunk on a planet)
				if (GetHorizonDistance(ChunkCenter, Context.ViewerPosition) > MaxViewDistance)
				{
					return;
				}

				// Island mode: skip chunks outside island boundary
				if (ShouldCullIslandBoundary(ChunkCoord, Context))
				{
					return;
				}

				// Spherical planet mode: skip chunks beyond horizon/shell
				if (ShouldCullBeyondHorizon(ChunkCoord, Context))
				{
					return;
				}

				// Determine LOD level
				int32 LODLevel = 0;
				float MorphFactor = 0.0f;

				if (bEnableLOD)
				{
					// Use the balanced LOD (2:1 adjacency + hysteresis) so loaded/
					// meshed chunks never differ from a neighbor by more than one
					// level. Morph still derives from the raw band the chunk sits in.
					LODLevel = GetLODForChunk(ChunkCoord, Context);
					if (bEnableMorphing)
					{
						const float Distance = GetDistanceToViewer(ChunkCenter, Context);
						if (const FLODBand* Band = FindBandForDistance(Distance))
						{
							MorphFactor = Band->GetMorphFactor(Distance);
						}
					}
				}
				// When LOD disabled: LODLevel stays 0 (full detail for all chunks)

				// Frustum culling (optional)
				if (bEnableFrustumCulling && !IsChunkInFrustum(ChunkCoord, Context))
				{
					return;
				}

				// Create request
				FChunkLODRequest Request;
				Request.ChunkCoord = ChunkCoord;
				Request.LODLevel = LODLevel;
				Request.Priority = CalculatePriority(ChunkCoord, Context);
				Request.MorphFactor = MorphFactor;

				Requests.Add(Request);
			});
		}
	}

	// Sort by priority (highest first)
	Requests.Sort();

	return Requests;
}

void FDistanceBandLODStrategy::GetChunksToLoad(
	TArray<FChunkLODRequest>& OutLoad,
	const TSet<FIntVector>& LoadedChunks,
	const FLODQueryContext& Context) const
{
	OutLoad.Reset();

	// Get all visible chunks
	TArray<FChunkLODRequest> VisibleChunks = GetVisibleChunks(Context);

	// Filter to only chunks that aren't loaded
	// Note: Rate limiting is handled by the chunk manager's ProcessGenerationQueue,
	// not here. We return all visible unloaded chunks so the manager can track them.
	for (const FChunkLODRequest& Request : VisibleChunks)
	{
		if (!LoadedChunks.Contains(Request.ChunkCoord))
		{
			OutLoad.Add(Request);
		}
	}
}

void FDistanceBandLODStrategy::GetChunksToUnload(
	TArray<FIntVector>& OutUnload,
	const TSet<FIntVector>& LoadedChunks,
	const FLODQueryContext& Context) const
{
	OutUnload.Reset();

	const float UnloadDistance = MaxViewDistance * UnloadDistanceMultiplier;

	// Every candidate, no per-frame cap: the chunk manager vetoes some (mid-pipeline, edit-pinned)
	// and applies MaxChunksToUnloadPerFrame after that (IVoxelLODStrategy::SelectUnloadCandidates).
	// Capping here let vetoed chunks early in the set's stable iteration order use up the budget.
	TArray<TPair<float, FIntVector>> Candidates;
	for (const FIntVector& ChunkCoord : LoadedChunks)
	{
		const FVector ChunkCenter = ChunkCoordToWorldCenter(ChunkCoord);

		// Beyond the unload horizon; outside the column's keep window (heightmap worlds: the slab, or the height range
		// near the viewer); or culled by the world mode, regardless of distance.
		const bool bUnload = GetHorizonDistance(ChunkCenter, Context.ViewerPosition) > UnloadDistance
			|| (IsHeightmapWorld() && !GetColumnWindow(ChunkCoord.X, ChunkCoord.Y, Context, EWindowUse::Keep).Contains(ChunkCoord.Z))
			|| ShouldCullIslandBoundary(ChunkCoord, Context)
			|| ShouldCullBeyondHorizon(ChunkCoord, Context);
		if (bUnload)
		{
			Candidates.Emplace(GetDistanceToViewer(ChunkCenter, Context), ChunkCoord);
		}
	}

	// Farthest first. Distances are computed once: after a teleport every loaded chunk is a
	// candidate for several frames, so the comparator must not recompute them.
	Candidates.Sort([](const TPair<float, FIntVector>& A, const TPair<float, FIntVector>& B)
	{
		return A.Key > B.Key;
	});
	OutUnload.Reserve(Candidates.Num());
	for (const TPair<float, FIntVector>& Candidate : Candidates)
	{
		OutUnload.Add(Candidate.Value);
	}
}

float FDistanceBandLODStrategy::GetChunkPriority(
	const FIntVector& ChunkCoord,
	const FLODQueryContext& Context) const
{
	return CalculatePriority(ChunkCoord, Context);
}

FString FDistanceBandLODStrategy::GetDebugInfo() const
{
	FString Info = FString::Printf(TEXT("DistanceBandLODStrategy\n"));
	Info += FString::Printf(TEXT("  Initialized: %s\n"), bIsInitialized ? TEXT("Yes") : TEXT("No"));
	Info += FString::Printf(TEXT("  LOD Bands: %d\n"), LODBands.Num());
	Info += FString::Printf(TEXT("  Max View Distance: %.0f\n"), MaxViewDistance);
	Info += FString::Printf(TEXT("  Morphing: %s\n"), bEnableMorphing ? TEXT("Enabled") : TEXT("Disabled"));
	Info += FString::Printf(TEXT("  Frustum Culling: %s\n"), bEnableFrustumCulling ? TEXT("Enabled") : TEXT("Disabled"));
	Info += FString::Printf(TEXT("  Viewer Chunk: (%d, %d, %d)\n"),
		CachedViewerChunk.X, CachedViewerChunk.Y, CachedViewerChunk.Z);

	// World-mode-specific culling info
	if (WorldMode == EWorldMode::InfinitePlane || WorldMode == EWorldMode::IslandBowl)
	{
		Info += FString::Printf(TEXT("  Terrain Culling: Height=[%.0f - %.0f]\n"),
			TerrainMinHeight, TerrainMaxHeight);
		Info += FString::Printf(TEXT("  Vertical Window: %s, + viewer layer ±%d within %.0f uu (horizon measured horizontally)\n"),
			bSurfaceSlabCulling ? TEXT("surface slab ±1.5 chunks") : TEXT("whole height range"), NearWindowLayers, NearWindowRadius);
	}
	if (WorldMode == EWorldMode::IslandBowl && IslandTotalExtentX > 0.0f)
	{
		Info += FString::Printf(TEXT("  Island Boundary Culling: Shape=%s, ExtentX=%.0f, ExtentY=%.0f, Center=(%.0f, %.0f)\n"),
			IslandShape == 1 ? TEXT("Rectangle") : TEXT("Circular"),
			IslandTotalExtentX, IslandTotalExtentY,
			IslandCenter.X, IslandCenter.Y);
	}
	if (WorldMode == EWorldMode::SphericalPlanet && PlanetRadius > 0.0f)
	{
		const float InnerRadius = PlanetRadius - PlanetMaxTerrainDepth;
		const float OuterRadius = PlanetRadius + PlanetMaxTerrainHeight;
		Info += FString::Printf(TEXT("  Shell Culling: Radius=%.0f, Shell=[%.0f - %.0f]\n"),
			PlanetRadius, InnerRadius, OuterRadius);
	}

	Info += TEXT("\n  Bands:\n");
	for (int32 i = 0; i < LODBands.Num(); ++i)
	{
		const FLODBand& Band = LODBands[i];
		Info += FString::Printf(TEXT("    [%d] LOD%d: %.0f - %.0f (stride: %d, morph: %.0f)\n"),
			i, Band.LODLevel, Band.MinDistance, Band.MaxDistance, Band.VoxelStride, Band.MorphRange);
	}

	return Info;
}

void FDistanceBandLODStrategy::DrawDebugVisualization(
	UWorld* World,
	const FLODQueryContext& Context) const
{
	if (!World)
	{
		return;
	}

#if ENABLE_DRAW_DEBUG
	const FVector ViewerPos = Context.ViewerPosition;

	// Draw LOD band rings (horizontal plane at viewer height)
	for (const FLODBand& Band : LODBands)
	{
		const FColor Color = GetLODDebugColor(Band.LODLevel);

		// Draw max distance circle
		DrawDebugCircle(
			World,
			ViewerPos,
			Band.MaxDistance,
			64,
			Color,
			false,
			-1.0f,
			0,
			5.0f,
			FVector::RightVector,
			FVector::ForwardVector,
			false
		);

		// Draw morph start circle if morphing enabled
		if (bEnableMorphing && Band.MorphRange > 0.0f)
		{
			const float MorphStart = Band.MaxDistance - Band.MorphRange;
			DrawDebugCircle(
				World,
				ViewerPos,
				MorphStart,
				64,
				FColor(Color.R / 2, Color.G / 2, Color.B / 2),
				false,
				-1.0f,
				0,
				2.0f,
				FVector::RightVector,
				FVector::ForwardVector,
				false
			);
		}
	}

	// Draw viewer position
	DrawDebugSphere(World, ViewerPos, 50.0f, 8, FColor::White, false, -1.0f, 0, 3.0f);
#endif
}

void FDistanceBandLODStrategy::SetLODBands(const TArray<FLODBand>& InBands)
{
	LODBands = InBands;
	bBalancedLODCacheDirty = true;

	// Sort by distance
	LODBands.Sort([](const FLODBand& A, const FLODBand& B)
	{
		return A.MinDistance < B.MinDistance;
	});

	// The near window follows band 0 (see Initialize).
	NearWindowRadius = (LODBands.Num() > 0) ? LODBands[0].MaxDistance + 2.0f * BaseChunkSize * VoxelSize : 0.0f;

	// Note: MaxViewDistance is set from Configuration->ViewDistance during Initialize()
	// and should not be overridden here. Call SetViewDistance() if needed.
}

// ==================== Internal Helpers ====================

FVector FDistanceBandLODStrategy::ChunkCoordToWorldCenter(const FIntVector& ChunkCoord) const
{
	// Include CachedWorldOrigin for correct world-space position
	return CachedWorldOrigin + FVoxelCoordinates::ChunkToWorldCenter(ChunkCoord, BaseChunkSize, VoxelSize);
}

FIntVector FDistanceBandLODStrategy::WorldPosToChunkCoord(const FVector& WorldPos) const
{
	// Subtract WorldOrigin to get position relative to chunk coordinate system
	return FVoxelCoordinates::WorldToChunk(WorldPos - CachedWorldOrigin, BaseChunkSize, VoxelSize);
}

float FDistanceBandLODStrategy::GetDistanceToViewer(
	const FVector& Position,
	const FLODQueryContext& Context) const
{
	// 3D Euclidean distance in every world mode. On a planet, near the viewer it matches the distance along the surface,
	// which is all surface play needs (SPHERICAL_PLANET_REQUIREMENTS.md SP-C3). Islands used 2D distance until WM-S2,
	// which kept whole columns in the near bands, uncompressed (KI13).
	return FVector::Distance(Position, Context.ViewerPosition);
}

float FDistanceBandLODStrategy::GetHorizonDistance(const FVector& ChunkCenter, const FVector& ViewerPosition) const
{
	// Heightmap worlds: a cylinder. The column's window decides its vertical extent (D8), so the viewer's altitude
	// never takes the ground out of range; with a 3D horizon a viewer 40,000 uu up would load no terrain at all.
	return IsHeightmapWorld()
		? static_cast<float>(FVector::Dist2D(ChunkCenter, ViewerPosition))
		: static_cast<float>(FVector::Dist(ChunkCenter, ViewerPosition));
}

const FLODBand* FDistanceBandLODStrategy::FindBandForDistance(float Distance) const
{
	for (const FLODBand& Band : LODBands)
	{
		if (Band.ContainsDistance(Distance))
		{
			return &Band;
		}
	}

	return nullptr;
}

const FLODBand* FDistanceBandLODStrategy::FindBandByLOD(int32 LODLevel) const
{
	for (const FLODBand& Band : LODBands)
	{
		if (Band.LODLevel == LODLevel)
		{
			return &Band;
		}
	}

	return nullptr;
}

int32 FDistanceBandLODStrategy::ApplyLODHysteresis(int32 CommittedLOD, int32 RawLOD, float Distance) const
{
	if (RawLOD == CommittedLOD)
	{
		return CommittedLOD;
	}

	// Decisive (multi-level) changes — e.g. a teleport — are accepted immediately.
	if (FMath::Abs(RawLOD - CommittedLOD) >= 2)
	{
		return RawLOD;
	}

	// Single-level change: require the distance to cross the committed band's edge
	// by a margin before switching, so a chunk hovering on a band boundary doesn't
	// flip every frame (the bistability seen at the LOD ring).
	const float ChunkW = BaseChunkSize * VoxelSize;
	const FLODBand* CommittedBand = FindBandByLOD(CommittedLOD);
	if (!CommittedBand)
	{
		return RawLOD;
	}

	if (RawLOD > CommittedLOD)
	{
		// Coarsening (moving away): only accept once clearly past the outer edge. Damped
		// (larger margin) so terrain just left behind doesn't flicker back to detail.
		return (Distance > CommittedBand->MaxDistance + CoarsenHysteresisFraction * ChunkW) ? RawLOD : CommittedLOD;
	}

	// Refining (moving closer): accept slightly BEFORE the inner edge, so a chunk reaches full
	// detail just before you enter it rather than after you are already standing on it.
	//
	// The margin is ADDED here, unlike the coarsen case which adds it on the far side. Both
	// margins used to subtract from / add to the same direction, which made refine trail the
	// band edge instead of anticipating it: with a 4800uu chunk and the shipped 0.25/0.5
	// fractions, a chunk coarsened at edge+2400 but only refined at edge-1200, a 3600uu band
	// in which the committed LOD never changed and detail arrived 1200uu after it was due.
	// Anticipating shrinks that to (Coarsen - Refine) * ChunkW and matches the documented
	// eager-refine / damped-coarsen intent. Initialize() enforces Refine <= Coarsen, which is
	// what keeps the two thresholds ordered and the transition stable.
	return (Distance < CommittedBand->MinDistance + RefineHysteresisFraction * ChunkW) ? RawLOD : CommittedLOD;
}

void FDistanceBandLODStrategy::RebuildBalancedLODCache(const FLODQueryContext& Context)
{
	BalancedLODCache.Reset();

	// Record what this map is computed for (ShouldRebuildBalancedLODCache).
	bBalancedLODCacheDirty = false;
	++BalancedLODCacheRebuildCount;
	LastRebuildViewerPosition = Context.ViewerPosition;
	LastRebuildViewerChunk = WorldPosToChunkCoord(Context.ViewerPosition);
	LastRebuildWorldOrigin = Context.WorldOrigin;
	LastRebuildBalanceCVar = CVarLODBalance.GetValueOnGameThread();
	LastRebuildDiagonalCVar = CVarLODBalanceDiagonal.GetValueOnGameThread();

	// LOD disabled or no bands: nothing to balance; consumers fall back to raw (0).
	if (!bEnableLOD || LODBands.Num() == 0)
	{
		CommittedLODCache.Reset();
		return;
	}

	const bool bBalance = LastRebuildBalanceCVar != 0;

	// Enumerate every chunk the strategy keeps within the load horizon: each column's keep window (GetColumnWindow),
	// which contains its load window, without the other world-mode culls. The resulting map is a superset of the chunks
	// that will actually be meshed, so every meshed chunk has a balanced entry, including chunks the near window loaded
	// at an earlier viewer layer; culled chunks are harmless extras.
	const FIntVector ViewerChunk = WorldPosToChunkCoord(Context.ViewerPosition);
	const float ChunkWorldSize = BaseChunkSize * VoxelSize;
	const int32 MaxChunkRadius = FMath::CeilToInt(MaxViewDistance / FMath::Max(ChunkWorldSize, 1.0f)) + 1;

	// Pass 1: raw band LOD + hysteresis (vs last frame's committed LOD).
	BalancedLODCache.Reserve((2 * MaxChunkRadius + 1) * (2 * MaxChunkRadius + 1) * 4);
	for (int32 X = -MaxChunkRadius; X <= MaxChunkRadius; ++X)
	{
		for (int32 Y = -MaxChunkRadius; Y <= MaxChunkRadius; ++Y)
		{
			const int32 ColumnX = ViewerChunk.X + X;
			const int32 ColumnY = ViewerChunk.Y + Y;
			if (IsHeightmapWorld() && GetHorizonDistance(ChunkCoordToWorldCenter(FIntVector(ColumnX, ColumnY, 0)), Context.ViewerPosition) > MaxViewDistance)
			{
				continue;
			}

			GetColumnWindow(ColumnX, ColumnY, Context, EWindowUse::Keep).ForEachLayer([&](int32 Z)
			{
				const FIntVector ChunkCoord(ColumnX, ColumnY, Z);
				const FVector ChunkCenter = ChunkCoordToWorldCenter(ChunkCoord);
				if (GetHorizonDistance(ChunkCenter, Context.ViewerPosition) > MaxViewDistance)
				{
					return;
				}

				const float Distance = GetDistanceToViewer(ChunkCenter, Context);
				const int32 RawLOD = GetRawLODForDistance(Distance);

				int32 Committed = RawLOD;
				if (bBalance)
				{
					if (const int32* Prev = CommittedLODCache.Find(ChunkCoord))
					{
						Committed = ApplyLODHysteresis(*Prev, RawLOD, Distance);
					}
				}

				BalancedLODCache.Add(ChunkCoord, Committed);
			});
		}
	}

	// Pass 2: 2:1 adjacency balance. Refine any chunk that is more than
	// MaxNeighborLODDelta coarser than its finest neighbor, iterating to a fixpoint.
	// Refine-only (LOD numbers only decrease), so it always converges.
	//
	// Neighbour set: face-only (6) leaves diagonal (edge/corner) neighbours able to differ by
	// 2 LOD levels, so three LODs can meet at a chunk corner (LOD0/1/2 junction) that the
	// per-face transvoxel/morph can't stitch. The full 26-neighbour set (6 faces + 12 edges +
	// 8 corners) makes every vertex-sharing pair differ by <=1, so at most two LOD levels meet
	// at any corner — no 3-LOD junction, regardless of band widths. Toggle: voxel.LODBalanceDiagonal.
	if (bBalance)
	{
		static const FIntVector FaceOffsets[6] = {
			FIntVector(-1, 0, 0), FIntVector(1, 0, 0),
			FIntVector(0, -1, 0), FIntVector(0, 1, 0),
			FIntVector(0, 0, -1), FIntVector(0, 0, 1),
		};
		// All 26 neighbours (every dx,dy,dz in {-1,0,1} except the origin), built once.
		static const TArray<FIntVector> AllOffsets = []()
		{
			TArray<FIntVector> Offs;
			Offs.Reserve(26);
			for (int32 dz = -1; dz <= 1; ++dz)
			{
				for (int32 dy = -1; dy <= 1; ++dy)
				{
					for (int32 dx = -1; dx <= 1; ++dx)
					{
						if (dx != 0 || dy != 0 || dz != 0)
						{
							Offs.Add(FIntVector(dx, dy, dz));
						}
					}
				}
			}
			return Offs;
		}();

		const bool bDiagonal = LastRebuildDiagonalCVar != 0;
		const FIntVector* Offsets = bDiagonal ? AllOffsets.GetData() : FaceOffsets;
		const int32 NumOffsets = bDiagonal ? AllOffsets.Num() : 6;

		bool bChanged = true;
		int32 Guard = 0;
		while (bChanged && Guard++ < 16)
		{
			bChanged = false;
			for (TPair<FIntVector, int32>& Pair : BalancedLODCache)
			{
				int32 MinNeighbor = Pair.Value;
				for (int32 i = 0; i < NumOffsets; ++i)
				{
					if (const int32* NLOD = BalancedLODCache.Find(Pair.Key + Offsets[i]))
					{
						MinNeighbor = FMath::Min(MinNeighbor, *NLOD);
					}
				}

				const int32 MaxAllowed = MinNeighbor + MaxNeighborLODDelta;
				if (Pair.Value > MaxAllowed)
				{
					Pair.Value = MaxAllowed;
					bChanged = true;
				}
			}
		}
	}

	// Commit for the next rebuild's hysteresis.
	CommittedLODCache = BalancedLODCache;
}

bool FDistanceBandLODStrategy::IsChunkInFrustum(
	const FIntVector& ChunkCoord,
	const FLODQueryContext& Context) const
{
	// If no frustum planes provided, assume visible
	if (Context.FrustumPlanes.Num() < 6)
	{
		return true;
	}

	// Get chunk bounding box (includes WorldOrigin offset)
	const FBox LocalBounds = FVoxelCoordinates::ChunkToWorldBounds(ChunkCoord, BaseChunkSize, VoxelSize);
	const FBox ChunkBounds(LocalBounds.Min + CachedWorldOrigin, LocalBounds.Max + CachedWorldOrigin);
	const FVector BoxCenter = ChunkBounds.GetCenter();
	const FVector BoxExtent = ChunkBounds.GetExtent();

	// Test against each frustum plane
	for (const FPlane& Plane : Context.FrustumPlanes)
	{
		// Calculate effective radius for box-plane test
		const float Radius =
			FMath::Abs(BoxExtent.X * Plane.X) +
			FMath::Abs(BoxExtent.Y * Plane.Y) +
			FMath::Abs(BoxExtent.Z * Plane.Z);

		// If box is completely behind plane, it's outside frustum
		if (Plane.PlaneDot(BoxCenter) < -Radius)
		{
			return false;
		}
	}

	return true;
}

float FDistanceBandLODStrategy::CalculatePriority(
	const FIntVector& ChunkCoord,
	const FLODQueryContext& Context) const
{
	const FVector ChunkCenter = ChunkCoordToWorldCenter(ChunkCoord);
	const float Distance = GetDistanceToViewer(ChunkCenter, Context);

	// Base priority: inverse distance (closer = higher)
	float Priority = 1.0f / FMath::Max(Distance, 1.0f);

	// Boost for chunks in view direction
	const FVector ToChunk = (ChunkCenter - Context.ViewerPosition).GetSafeNormal();
	const float DotProduct = FVector::DotProduct(ToChunk, Context.ViewerForward);

	if (DotProduct > 0.0f)
	{
		// Forward chunks get up to 2x priority boost
		Priority *= (1.0f + DotProduct);
	}

	return Priority;
}

FDistanceBandLODStrategy::FColumnWindow FDistanceBandLODStrategy::GetColumnWindow(
	int32 ChunkX,
	int32 ChunkY,
	const FLODQueryContext& Context,
	EWindowUse Use) const
{
	FColumnWindow Window;
	const FIntVector ViewerChunk = WorldPosToChunkCoord(Context.ViewerPosition);

	// Planet: the viewer's layer + the mode's offsets, absolute (callers no longer add the viewer's layer, §13.11).
	if (!IsHeightmapWorld())
	{
		Window.SlabMinZ = ViewerChunk.Z + MinVerticalChunks;
		Window.SlabMaxZ = ViewerChunk.Z + MaxVerticalChunks;
		return Window;
	}

	// Heightmap worlds (D8): everything is clipped to the terrain's height bounds.
	const double OriginZ = Context.WorldOrigin.Z;
	int32 BoundsMinZ = 0, BoundsMaxZ = -1;
	GetLayersOverlapping(TerrainMinHeight, TerrainMaxHeight, OriginZ, BoundsMinZ, BoundsMaxZ);

	// Near the viewer: the column centre's horizontal distance. Load and keep share the radius, as the old slab gate
	// did, so the extra layers unload as soon as their column leaves it; the radius sits two chunks past band 0, which
	// keeps them from churning while the viewer wanders around band 0's edge.
	const float ChunkWorldSize = BaseChunkSize * VoxelSize;
	const FVector2D ColumnCentre(
		Context.WorldOrigin.X + (ChunkX + 0.5) * ChunkWorldSize,
		Context.WorldOrigin.Y + (ChunkY + 0.5) * ChunkWorldSize);
	const double HorizontalDistance = FVector2D::Distance(ColumnCentre, FVector2D(Context.ViewerPosition.X, Context.ViewerPosition.Y));
	const bool bNear = HorizontalDistance <= NearWindowRadius;

	// Keep, near the viewer: the whole height range, whatever the viewer's layer. A viewer moving up or down a layer
	// (walking across a layer boundary, flying up) unloads nothing it loaded nearby.
	if (bNear && Use == EWindowUse::Keep)
	{
		Window.SlabMinZ = BoundsMinZ;
		Window.SlabMaxZ = BoundsMaxZ;
		return Window;
	}

	// The column's own surface ± slack (up to the water plane), at every distance; the whole range with the slab off.
	if (bSurfaceSlabCulling)
	{
		float SurfMin = 0.0f, SurfMax = 0.0f;
		GetColumnSurfaceRange(ChunkX, ChunkY, Context.WorldOrigin, SurfMin, SurfMax);
		const double Slack = SlabSlackChunks * ChunkWorldSize;
		GetLayersOverlapping(SurfMin - Slack, SurfMax + Slack, OriginZ, Window.SlabMinZ, Window.SlabMaxZ);
		Window.SlabMinZ = FMath::Max(Window.SlabMinZ, BoundsMinZ);
		Window.SlabMaxZ = FMath::Min(Window.SlabMaxZ, BoundsMaxZ);
	}
	else
	{
		Window.SlabMinZ = BoundsMinZ;
		Window.SlabMaxZ = BoundsMaxZ;
	}

	// Load, near the viewer: also the viewer's own layer ± NearWindowLayers, for caves, dungeons and digging below or
	// above the surface. The viewer's altitude only adds layers here; the slab keeps the ground.
	if (bNear)
	{
		Window.NearMinZ = FMath::Max(ViewerChunk.Z - NearWindowLayers, BoundsMinZ);
		Window.NearMaxZ = FMath::Min(ViewerChunk.Z + NearWindowLayers, BoundsMaxZ);
	}
	return Window;
}

void FDistanceBandLODStrategy::GetLayersOverlapping(double Lo, double Hi, double OriginZ, int32& OutMin, int32& OutMax) const
{
	// Layer Z spans [OriginZ + Z * W, OriginZ + (Z + 1) * W]. It overlaps [Lo, Hi] when its top is not below Lo and its
	// bottom is not above Hi (touching counts, as the old per-chunk bounds cull did).
	const double ChunkWorldSize = FMath::Max(static_cast<double>(BaseChunkSize) * VoxelSize, 1.0);
	OutMin = FMath::CeilToInt32((Lo - OriginZ) / ChunkWorldSize) - 1;
	OutMax = FMath::FloorToInt32((Hi - OriginZ) / ChunkWorldSize);
}

FColor FDistanceBandLODStrategy::GetLODDebugColor(int32 LODLevel) const
{
	// Color gradient from green (LOD0) to red (high LOD)
	static const FColor LODColors[] = {
		FColor::Green,      // LOD 0
		FColor::Cyan,       // LOD 1
		FColor::Blue,       // LOD 2
		FColor::Magenta,    // LOD 3
		FColor::Yellow,     // LOD 4
		FColor::Orange,     // LOD 5
		FColor::Red,        // LOD 6
		FColor(128, 0, 0),  // LOD 7
	};

	const int32 ColorIndex = FMath::Clamp(LODLevel, 0, 7);
	return LODColors[ColorIndex];
}

void FDistanceBandLODStrategy::GetColumnSurfaceRange(int32 ChunkX, int32 ChunkY,
	const FVector& WorldOrigin, float& OutMin, float& OutMax) const
{
	const FIntPoint Key(ChunkX, ChunkY);
	if (const FFloatInterval* Cached = ColumnSurfaceCache.Find(Key))
	{
		OutMin = Cached->Min;
		OutMax = Cached->Max;
		return;
	}

	// The configured world mode's surface over the chunk footprint (a 5-point sample in heightmap modes, continentalness
	// included, as generation produces it). The slack GetColumnWindow applies covers variation between the samples.
	// Generation params are immutable per world, so memoize for the strategy's lifetime.
	const float ChunkWorldSize = BaseChunkSize * VoxelSize;
	const FVector2D MinXY(WorldOrigin.X + ChunkX * ChunkWorldSize, WorldOrigin.Y + ChunkY * ChunkWorldSize);
	const FVector2D MaxXY = MinXY + FVector2D(ChunkWorldSize, ChunkWorldSize);
	const FFloatInterval Surface = TerrainWorldMode->GetSurfaceRange(MinXY, MaxXY, SlabNoiseParams);
	float SurfMin = Surface.Min;
	float SurfMax = Surface.Max;

	// Underwater columns reach up to the water plane: the far ocean's SURFACE (and the water
	// volume above its floor) must stay loaded, not just the seabed.
	if (bSlabHasWater)
	{
		SurfMax = FMath::Max(SurfMax, SlabWaterLevel);
	}

	ColumnSurfaceCache.Add(Key, FFloatInterval(SurfMin, SurfMax));
	OutMin = SurfMin;
	OutMax = SurfMax;
}

bool FDistanceBandLODStrategy::ShouldCullIslandBoundary(
	const FIntVector& ChunkCoord,
	const FLODQueryContext& Context) const
{
	if (WorldMode != EWorldMode::IslandBowl || IslandTotalExtentX <= 0.0f)
	{
		return false;
	}

	// The column streams when it holds any of the island's footprint. Beyond R + F nothing is reachable (the walls,
	// WM-I5) and the far ocean draws the sea and the seabed (WM-E1); a circle's cull used to keep a ring of columns
	// wholly outside, out to a chunk diagonal past the edge (6,788 uu on the demo).
	const FVector ChunkCenter = ChunkCoordToWorldCenter(ChunkCoord);
	const float ChunkHalfSize = BaseChunkSize * VoxelSize * 0.5f;
	const FBox2D Column(FVector2D(ChunkCenter.X - ChunkHalfSize, ChunkCenter.Y - ChunkHalfSize),
		FVector2D(ChunkCenter.X + ChunkHalfSize, ChunkCenter.Y + ChunkHalfSize));
	return !IslandBounds.IntersectsBox2D(Column);
}

bool FDistanceBandLODStrategy::ShouldCullBeyondHorizon(
	const FIntVector& ChunkCoord,
	const FLODQueryContext& Context) const
{
	if (WorldMode != EWorldMode::SphericalPlanet || PlanetRadius <= 0.0f)
	{
		return false;
	}

	const float ChunkWorldSize = BaseChunkSize * VoxelSize;
	const float ChunkDiagonal = ChunkWorldSize * UE_SQRT_3; // 3D diagonal for spherical

	// Get chunk bounds for shell intersection tests
	const FVector ChunkCenter = ChunkCoordToWorldCenter(ChunkCoord);
	const FVector ToChunkFromPlanet = ChunkCenter - PlanetCenter;
	const float ChunkDistanceFromCenter = ToChunkFromPlanet.Size();

	// Calculate inner and outer shell radii
	const float InnerShellRadius = PlanetRadius - PlanetMaxTerrainDepth;
	const float OuterShellRadius = PlanetRadius + PlanetMaxTerrainHeight;

	// INNER SHELL CULLING: Cull chunks entirely inside the planet core
	// If the chunk's farthest point from planet center is still inside the inner shell, cull it
	const float ChunkMaxRadius = ChunkDistanceFromCenter + ChunkDiagonal;
	if (ChunkMaxRadius < InnerShellRadius)
	{
		return true; // Chunk is entirely inside planet core
	}

	// OUTER SHELL CULLING: Cull chunks entirely outside the terrain shell
	// If the chunk's closest point to planet center is outside the outer shell, cull it
	const float ChunkMinRadius = FMath::Max(0.0f, ChunkDistanceFromCenter - ChunkDiagonal);
	if (ChunkMinRadius > OuterShellRadius)
	{
		return true; // Chunk is entirely outside terrain shell
	}

	// HORIZON CULLING: For chunks that intersect the shell, check if they're beyond the horizon
	// Calculate viewer's altitude above planet surface
	const FVector ToViewerFromPlanet = Context.ViewerPosition - PlanetCenter;
	const float ViewerDistanceFromCenter = ToViewerFromPlanet.Size();
	const float ViewerAltitude = ViewerDistanceFromCenter - PlanetRadius;

	// If viewer is deep underground, skip horizon culling (they're inside the planet)
	if (ViewerAltitude < -PlanetMaxTerrainDepth)
	{
		return false;
	}

	// Calculate horizon distance using viewer's effective altitude (clamped to surface)
	const float EffectiveAltitude = FMath::Max(0.0f, ViewerAltitude);
	if (EffectiveAltitude > 0.0f)
	{
		// Horizon distance formula: sqrt(2*R*h + h^2)
		const float HorizonDistance = FMath::Sqrt(2.0f * PlanetRadius * EffectiveAltitude + EffectiveAltitude * EffectiveAltitude);

		// Add buffer for terrain variation (but smaller than before - just terrain height, not full diagonal)
		const float HorizonBuffer = HorizonDistance + PlanetMaxTerrainHeight;

		// Distance from viewer to chunk
		const float DistanceToChunk = FVector::Distance(Context.ViewerPosition, ChunkCenter);

		// Cull if chunk center is beyond horizon + buffer
		if (DistanceToChunk > HorizonBuffer + ChunkDiagonal)
		{
			return true;
		}
	}

	return false;
}
