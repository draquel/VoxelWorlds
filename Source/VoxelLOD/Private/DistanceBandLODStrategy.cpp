// Copyright Daniel Raquel. All Rights Reserved.

#include "DistanceBandLODStrategy.h"
#include "VoxelLOD.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelBiomeConfiguration.h"
#include "InfinitePlaneWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelWorldModeFactory.h"
#include "VoxelCoordinates.h"
#include "DrawDebugHelpers.h"
#include "HAL/IConsoleManager.h"

// Spherical Phase 6, WM-BP10 (D18): the voxel cut-over altitude over the surface under the viewer, above which a planet's
// voxel window unloads and the far field alone draws the planet.
static TAutoConsoleVariable<float> CVarVoxelCutover(
	TEXT("voxel.Stream.VoxelCutover"),
	-1.0f,
	TEXT("The voxel cut-over altitude (uu over the terrain or sea under the streaming viewer), in every world mode with a far\n")
	TEXT("field. Above it the voxel window unloads and the far field alone draws the world; below 0.85 of it the window streams back. -1 = the configuration's\n")
	TEXT("VoxelCutoverAltitude (0 there: three times the view distance), 0 = never (the window keeps its ground disk at any altitude)."),
	ECVF_Default);

// G49 (WORLD_MODES_REVIEW_PLAN.md §20.28): a suspended planet window resumes by the descent prefetch's lead only once the
// camera itself is under the cut-over, so a fast descent that stops above the cut-over does not resume it for a moment.
static TAutoConsoleVariable<int32> CVarPlanetCutoverResumeGuard(
	TEXT("voxel.World.CutoverResumeGuard"),
	1,
	TEXT("Planets: 1 (default) = a suspended voxel window resumes only with the streaming viewer under the resume altitude\n")
	TEXT("(0.85 of the cut-over) AND the camera under the cut-over (G49: the descent prefetch's lead resumes it from the\n")
	TEXT("cut-over at most). 0 = wherever the lead takes the streaming viewer (the rule before: a descent stopping above the\n")
	TEXT("cut-over resumed the window for a moment)."),
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
	// (Min/Max) by N while leaving MaxViewDistance (the field size /
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
		const FVoxelWorldBounds HeightBounds = TerrainWorldMode->GetWorldBounds();
		const float BoundsMin = HeightBounds.MinZ;
		const float BoundsMax = HeightBounds.MaxZ;

		// One chunk of buffer on each side for terrain variation / meshing / edit slack.
		TerrainMinHeight = BoundsMin - ChunkWorldSize;
		TerrainMaxHeight = BoundsMax + ChunkWorldSize;

		UE_LOG(LogVoxelLOD, Log, TEXT("  Terrain bounds culling: Height range [%.0f - %.0f]"),
			TerrainMinHeight, TerrainMaxHeight);
	}

	// The vertical window comes from the terrain in heightmap worlds (D8, WM-S1): every column streams its own surface
	// slab, sampled from the world mode (WM-A3) so it follows the height generation produces, islands included (KI5).
	// Near the viewer the viewer's layer ± NearLayers is added (GetColumnLayerRange).
	bSurfaceSlabCulling = WorldConfig->bSurfaceSlabCulling && IsHeightmapWorld();
	ColumnSurfaceCache.Reset();
	if (bSurfaceSlabCulling)
	{
		SlabNoiseParams = WorldConfig->GetNoiseParams();
		bSlabHasWater = WorldConfig->bEnableWaterLevel;
		SlabWaterLevel = WorldConfig->WaterLevel; // world Z, as the terrain's heights and generation's water are (G19)
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
	PlanetWorldMode = nullptr;
	bPlanetSlab = false;
	PlanetCellSurfaceCache.Reset();
	PlanetChunkSlabCache.Reset();
	LoadEnvelopeMemo = FSurfaceEnvelopeMemo();
	NearEnvelopeMemo = FSurfaceEnvelopeMemo();
	PlanetNearBandAngle = 0.0;
	GroundRadiusViewer = FVector(TNumericLimits<double>::Max());
	CutoverConfigured = 0.0;
	PlanetSeaRadius = 0.0;
	bWindowSuspended = false;
	ViewerSurfaceAltitude = 0.0;
	CameraSurfaceAltitude = 0.0;
	bCutoverResumeHeld = false;
	bCutoverAllowed = false;
	HeightmapSeaZ = TNumericLimits<double>::Lowest();
	HeightmapGroundViewer = FVector(TNumericLimits<double>::Max());
	HeightmapGroundZ = 0.0;
	if (IsHeightmapWorld())
	{
		// The voxel cut-over on a heightmap world (world modes review WM-MP2, D38): the same altitude rule as a planet's,
		// over the generated ground under the viewer or the sea where higher, in force only while a far field draws the
		// world beyond the window (FLODQueryContext::bFarFieldActive, read in Update; the same gate on a planet since G78).
		CutoverConfigured = WorldConfig->VoxelCutoverAltitude > 0.0f ? WorldConfig->VoxelCutoverAltitude
			: DefaultCutoverViewDistances * MaxViewDistance;
		HeightmapSeaZ = WorldConfig->bEnableWaterLevel ? static_cast<double>(WorldConfig->WaterLevel) : TNumericLimits<double>::Lowest();
		UE_LOG(LogVoxelLOD, Log, TEXT("  Voxel cut-over (with a far field): %.0f uu over the surface under the viewer (resumes below %.0f)%s"),
			CutoverConfigured, CutoverConfigured * CutoverResumeFraction,
			CVarVoxelCutover.GetValueOnAnyThread() >= 0.0f ? TEXT(" (voxel.Stream.VoxelCutover)") : TEXT(""));
	}
	if (WorldMode == EWorldMode::SphericalPlanet)
	{
		const FSphericalPlanetParams Planet = FVoxelWorldModeFactory::MakePlanetParams(*WorldConfig);
		PlanetRadius = Planet.PlanetRadius;
		PlanetCenter = Planet.PlanetCenter;
		PlanetWorldMode = static_cast<const FSphericalPlanetWorldMode*>(TerrainWorldMode.Get());

		// The terrain's real bounds (spherical Phase 6, WM-BP8): what generation can reach (the displacement bounds, noise at
		// continentalness's extremes), within the configured shell, past which terrain is cut off. M's and L's shells are
		// their extents plus 10%; the shell window and the horizon's peaks use these bounds.
		float DisplacementMin = -Planet.MaxTerrainDepth;
		float DisplacementMax = Planet.MaxTerrainHeight;
		FSphericalPlanetWorldMode::GetDisplacementBounds(PlanetWorldMode->GetTerrainParams(), &PlanetWorldMode->GetBiomeSnapshot(),
			DisplacementMin, DisplacementMax);
		PlanetMaxTerrainHeight = FMath::Min(DisplacementMax, Planet.MaxTerrainHeight);
		PlanetMaxTerrainDepth = FMath::Min(-DisplacementMin, Planet.MaxTerrainDepth);

		// The shell window (SP-C1): every chunk within ShellSlackChunks of the terrain's bounds, at any latitude.
		ShellWindowInnerRadius = FMath::Max(0.0, PlanetRadius - PlanetMaxTerrainDepth - ShellSlackChunks * ChunkWorldSize);
		ShellWindowOuterRadius = PlanetRadius + PlanetMaxTerrainHeight + ShellSlackChunks * ChunkWorldSize;

		// The per-chunk slab (WM-BP8): the surface's altitude range over the cells a chunk's corners span, cells about a
		// chunk across on the datum sphere.
		bPlanetSlab = WorldConfig->bSurfaceSlabCulling;
		SlabNoiseParams = WorldConfig->GetNoiseParams();
		PlanetSlabGrid = FVoxelCubeSphereGrid(FVoxelCubeSphereGrid::CellsPerEdgeFor(PlanetRadius, ChunkWorldSize));

		UE_LOG(LogVoxelLOD, Log, TEXT("  Shell culling enabled: Radius=%.0f, terrain %.0f to +%.0f (configured shell -%.0f to +%.0f)"),
			PlanetRadius, -PlanetMaxTerrainDepth, PlanetMaxTerrainHeight, Planet.MaxTerrainDepth, Planet.MaxTerrainHeight);
		UE_LOG(LogVoxelLOD, Log, TEXT("  Shell window: radius %.0f to %.0f (the terrain + %.1f chunk), horizon + %.0f for the highest peaks"),
			ShellWindowInnerRadius, ShellWindowOuterRadius, ShellSlackChunks,
			FMath::Sqrt(2.0 * PlanetRadius * FMath::Max(0.0f, PlanetMaxTerrainHeight) + FMath::Square(static_cast<double>(FMath::Max(0.0f, PlanetMaxTerrainHeight)))));
		UE_LOG(LogVoxelLOD, Log, TEXT("  Planet window: %s; horizon along the ground from the viewer's ground point; the viewer's altitude ±%d chunks within %.0f uu, up to the top of the surface slab there (or the sea), none above it"),
			bPlanetSlab ? *FString::Printf(TEXT("per-chunk surface slab ±%.1f chunks (%d cells a face edge)"), SlabSlackChunks, PlanetSlabGrid.GetCellsPerEdge())
				: TEXT("the whole shell (slab culling off)"),
			NearLayers, (LODBands.Num() > 0) ? LODBands[0].MaxDistance + 2.0f * ChunkWorldSize : 0.0f);

		// The voxel cut-over (WM-BP10): over the surface under the viewer, the sea where it lies above the terrain.
		CutoverConfigured = WorldConfig->VoxelCutoverAltitude > 0.0f ? WorldConfig->VoxelCutoverAltitude
			: DefaultCutoverViewDistances * MaxViewDistance;
		PlanetSeaRadius = WorldConfig->bEnableWaterLevel ? static_cast<double>(WorldConfig->WaterRadius) : 0.0;
		UE_LOG(LogVoxelLOD, Log, TEXT("  Voxel cut-over (with a far field): %.0f uu over the surface under the viewer (resumes below %.0f)%s"),
			GetConfiguredCutoverAltitude(), GetConfiguredCutoverAltitude() * CutoverResumeFraction,
			CVarVoxelCutover.GetValueOnAnyThread() >= 0.0f ? TEXT(" (voxel.Stream.VoxelCutover)") : TEXT(""));
	}

	// The near window (heightmap worlds): band 0's outer edge plus two chunk widths, measured horizontally.
	NearLayersRadius = (LODBands.Num() > 0) ? LODBands[0].MaxDistance + 2.0f * ChunkWorldSize : 0.0f;
	if (PlanetWorldMode)
	{
		// The near disk's cone (GetPlanetNearLayersCeiling), at its widest: along the ground at the shell's inner radius.
		const double InnerRadius = FMath::Max(ShellWindowInnerRadius, static_cast<double>(ChunkWorldSize));
		const double HalfDiagonal = 0.5 * ChunkWorldSize * UE_DOUBLE_SQRT_3;
		PlanetNearBandAngle = FMath::Min(UE_DOUBLE_PI, NearLayersRadius / InnerRadius + FMath::Asin(FMath::Min(1.0, HalfDiagonal / InnerRadius)));
	}
	if (IsHeightmapWorld())
	{
		UE_LOG(LogVoxelLOD, Log, TEXT("  Vertical window from the terrain: the viewer's layer ±%d within %.0f uu, horizon measured horizontally"),
			NearLayers, NearLayersRadius);
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
	if (!Context.WorldOrigin.Equals(CachedWorldOrigin, 0.0))
	{
		// The planet's chunk facts are keyed by chunk coordinate, so they hold for one grid origin only.
		PlanetChunkSlabCache.Reset();
	}
	CachedWorldOrigin = Context.WorldOrigin;
	CachedViewerChunk = WorldPosToChunkCoord(Context.ViewerPosition);

	// A planet viewer's ground point (WM-BP8): one terrain sample when the viewer moves; the horizon is measured from it.
	if (PlanetWorldMode && Context.ViewerPosition != GroundRadiusViewer)
	{
		GroundRadiusViewer = FVector(TNumericLimits<double>::Max()); // GetPlanetGroundRadius computes rather than reads
		GroundRadiusCached = GetPlanetGroundRadius(Context.ViewerPosition);
		GroundRadiusViewer = Context.ViewerPosition;
	}

	// The voxel cut-over (WM-BP10; heightmap worlds WM-MP2): above it the window holds nothing (the far field draws the
	// world); below a fraction of it the window streams back. Measured over the terrain under the viewer, or the sea where
	// that is higher. In force only while the chunk manager has a far field (D38), in every mode since G78: without one a
	// suspended window would draw nothing at all, on a planet as on a plane (voxel.World.FarField 0 is a debug switch).
	bCutoverAllowed = Context.bFarFieldActive;
	if (PlanetWorldMode || (IsHeightmapWorld() && TerrainWorldMode.IsValid()))
	{
		ViewerSurfaceAltitude = ComputeSurfaceAltitude(Context.ViewerPosition);
		// The camera is the viewer moved back up by the prefetch's lead, along the same direction (the same surface).
		CameraSurfaceAltitude = ViewerSurfaceAltitude + FMath::Max(Context.ViewerLead, 0.0);
		const bool bWasSuspended = bWindowSuspended;
		const double Cutover = GetCutoverAltitude(); // 0 without a far field: never suspends, and a suspended window resumes
		const bool bByViewer = ComputeCutoverState(bWasSuspended, ViewerSurfaceAltitude, Cutover);
		bWindowSuspended = CVarPlanetCutoverResumeGuard.GetValueOnAnyThread() != 0
			? ComputeCutoverState(bWasSuspended, ViewerSurfaceAltitude, Cutover, CameraSurfaceAltitude)
			: bByViewer;
		const bool bWasResumeHeld = bCutoverResumeHeld;
		bCutoverResumeHeld = bWindowSuspended && !bByViewer;
		if (bCutoverResumeHeld && !bWasResumeHeld)
		{
			UE_LOG(LogVoxelLOD, Log, TEXT("Planet voxel window resume held (G49): the streaming viewer %.0f uu over the surface (under the resume altitude %.0f), the camera %.0f over it (over the cut-over %.0f)."),
				ViewerSurfaceAltitude, Cutover * CutoverResumeFraction, CameraSurfaceAltitude, Cutover);
		}
		if (bWindowSuspended != bWasSuspended)
		{
			bBalancedLODCacheDirty = true;
			UE_LOG(LogVoxelLOD, Log, TEXT("%s voxel window %s at %.0f uu over the surface, the camera at %.0f (cut-over %.0f, resumes below %.0f)."),
				PlanetWorldMode ? TEXT("Planet") : TEXT("Heightmap"),
				bWindowSuspended ? TEXT("SUSPENDED: the far field draws the world") : TEXT("RESUMED"),
				ViewerSurfaceAltitude, CameraSurfaceAltitude, Cutover, Cutover * CutoverResumeFraction);
		}
	}

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
		|| !Context.WorldOrigin.Equals(LastRebuildWorldOrigin);
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

TArray<FChunkLODRequest> FDistanceBandLODStrategy::GetVisibleChunks(
	const FLODQueryContext& Context) const
{
	TArray<FChunkLODRequest> Requests;

	if (LODBands.Num() == 0)
	{
		return Requests;
	}

	// A chunk in the window and inside the horizon: the world-mode culls, its LOD and its request.
	auto AddRequest = [&](const FIntVector& ChunkCoord, const FVector& ChunkCenter)
	{
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

		if (bEnableLOD)
		{
			// Use the balanced LOD (2:1 adjacency + hysteresis) so loaded/
			// meshed chunks never differ from a neighbor by more than one
			// level.
			LODLevel = GetLODForChunk(ChunkCoord, Context);
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

		Requests.Add(Request);
	};

	// The window's candidates in any mode (the enumeration keeps each mode's order; the sort is by priority).
	ForEachWindowCandidate(Context, AddRequest);

	// Sort by priority (highest first)
	Requests.Sort();

	return Requests;
}

bool FDistanceBandLODStrategy::IsChunkInWindow(const FIntVector& ChunkCoord, const FLODQueryContext& Context, EWindowUse Use) const
{
	return IsHeightmapWorld()
		? GetColumnLayerRange(ChunkCoord.X, ChunkCoord.Y, Context, Use).Contains(ChunkCoord.Z)
		: IsInPlanetWindow(ChunkCoord, Context, Use);
}

void FDistanceBandLODStrategy::ForEachWindowCandidate(const FLODQueryContext& Context, TFunctionRef<void(const FIntVector&, const FVector&)> Visit) const
{
	// Above the voxel cut-over (WM-BP10, WM-MP2) the window is empty: the far field draws the world.
	if (bWindowSuspended)
	{
		return;
	}
	if (!IsHeightmapWorld())
	{
		// A planet (WM-BP8): the chunks of its window by chunk (IsInPlanetWindow) within the horizon's cone, measured along
		// the ground from the viewer's ground point.
		const double GroundRadius = GetPlanetGroundRadius(Context.ViewerPosition);
		ForEachPlanetCandidate(Context, EWindowUse::Load, [&](const FIntVector& ChunkCoord)
		{
			const FVector ChunkCenter = ChunkCoordToWorldCenter(ChunkCoord);
			if (GetPlanetGroundDistance(PlanetCenter, GroundRadius, Context.ViewerPosition, ChunkCenter) > MaxViewDistance
				|| !IsInPlanetWindow(ChunkCoord, Context, EWindowUse::Load))
			{
				return;
			}
			Visit(ChunkCoord, ChunkCenter);
		});
		return;
	}

	// Every column within the horizon, and in each column the layers of its vertical window (absolute layers, the
	// terrain's).
	const FIntVector ViewerChunk = WorldPosToChunkCoord(Context.ViewerPosition);
	const float ChunkWorldSize = BaseChunkSize * VoxelSize;
	const int32 MaxChunkRadius = FMath::CeilToInt(MaxViewDistance / ChunkWorldSize) + 1;
	for (int32 X = -MaxChunkRadius; X <= MaxChunkRadius; ++X)
	{
		for (int32 Y = -MaxChunkRadius; Y <= MaxChunkRadius; ++Y)
		{
			const int32 ColumnX = ViewerChunk.X + X;
			const int32 ColumnY = ViewerChunk.Y + Y;

			// The horizon is horizontal, so a whole column is in range or not.
			if (GetStreamingHorizon(ChunkCoordToWorldCenter(FIntVector(ColumnX, ColumnY, 0)), Context.ViewerPosition) > MaxViewDistance)
			{
				continue;
			}

			const FColumnLayerRange Window = GetColumnLayerRange(ColumnX, ColumnY, Context, EWindowUse::Load);
			Window.ForEachLayer([&](int32 Z)
			{
				const FIntVector ChunkCoord(ColumnX, ColumnY, Z);
				Visit(ChunkCoord, ChunkCoordToWorldCenter(ChunkCoord));
			});
		}
	}
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

		// Above the voxel cut-over (WM-BP10) nothing is kept.
		if (bWindowSuspended)
		{
			Candidates.Emplace(GetDistanceToViewer(ChunkCenter, Context), ChunkCoord);
			continue;
		}

		// Beyond the unload horizon; outside the keep window (heightmap worlds: the column's slab, or the height range
		// near the viewer; a planet: the chunk's slab, or the shell below it near the viewer, WM-BP8); or culled by the
		// world mode, regardless of distance.
		const bool bUnload = GetStreamingHorizon(ChunkCenter, Context.ViewerPosition) > UnloadDistance
			|| !IsChunkInWindow(ChunkCoord, Context, EWindowUse::Keep)
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

FString FDistanceBandLODStrategy::GetDebugInfo() const
{
	FString Info = FString::Printf(TEXT("DistanceBandLODStrategy\n"));
	Info += FString::Printf(TEXT("  Initialized: %s\n"), bIsInitialized ? TEXT("Yes") : TEXT("No"));
	Info += FString::Printf(TEXT("  LOD Bands: %d\n"), LODBands.Num());
	Info += FString::Printf(TEXT("  Max View Distance: %.0f\n"), MaxViewDistance);
	Info += FString::Printf(TEXT("  Frustum Culling: %s\n"), bEnableFrustumCulling ? TEXT("Enabled") : TEXT("Disabled"));
	Info += FString::Printf(TEXT("  Viewer Chunk: (%d, %d, %d)\n"),
		CachedViewerChunk.X, CachedViewerChunk.Y, CachedViewerChunk.Z);

	// World-mode-specific culling info
	if (WorldMode == EWorldMode::InfinitePlane || WorldMode == EWorldMode::IslandBowl)
	{
		Info += FString::Printf(TEXT("  Terrain Culling: Height=[%.0f - %.0f]\n"),
			TerrainMinHeight, TerrainMaxHeight);
		Info += FString::Printf(TEXT("  Vertical Window: %s, + viewer layer ±%d within %.0f uu (horizon measured horizontally)\n"),
			bSurfaceSlabCulling ? TEXT("surface slab ±1.5 chunks") : TEXT("whole height range"), NearLayers, NearLayersRadius);
	}
	if (WorldMode == EWorldMode::IslandBowl && IslandTotalExtentX > 0.0f)
	{
		Info += FString::Printf(TEXT("  Island Boundary Culling: Shape=%s, ExtentX=%.0f, ExtentY=%.0f, Center=(%.0f, %.0f)\n"),
			IslandShape == 1 ? TEXT("Rectangle") : TEXT("Circular"),
			IslandTotalExtentX, IslandTotalExtentY,
			IslandCenter.X, IslandCenter.Y);
	}
	if (WorldMode == EWorldMode::SphericalPlanet && PlanetRadius > 0.0)
	{
		const double InnerRadius = PlanetRadius - PlanetMaxTerrainDepth;
		const double OuterRadius = PlanetRadius + PlanetMaxTerrainHeight;
		Info += FString::Printf(TEXT("  Shell Culling: Radius=%.0f, Terrain=[%.0f - %.0f]\n"),
			PlanetRadius, InnerRadius, OuterRadius);
		Info += FString::Printf(TEXT("  Window: %s in radius [%.0f - %.0f], + the viewer's altitude ±%d chunks within %.0f uu; horizon along the ground from the viewer's ground point; + %.0f for peaks\n"),
			bPlanetSlab ? TEXT("each chunk's surface slab ±1.5 chunks") : TEXT("the whole shell"),
			ShellWindowInnerRadius, ShellWindowOuterRadius, NearLayers, NearLayersRadius,
			GetVisibilityHorizon(PlanetRadius, 0.0, PlanetMaxTerrainHeight));
		Info += FString::Printf(TEXT("  Near band: %s (viewer %.0f over the datum, the band up to %.0f)\n"),
			IsPlanetNearBandActive(CachedViewerPosition) ? TEXT("on") : TEXT("off, above the surface slab"),
			FVector::Dist(CachedViewerPosition, PlanetCenter) - PlanetRadius, GetPlanetNearLayersCeiling(CachedViewerPosition));
		Info += FString::Printf(TEXT("  Slab caches: %d cells, %d chunks\n"), PlanetCellSurfaceCache.Num(), PlanetChunkSlabCache.Num());
		Info += FString::Printf(TEXT("  Voxel cut-over: %s (the camera under the cut-over: %s); viewer %.0f over it, camera %.0f, window %s\n"),
			bCutoverAllowed
				? *FString::Printf(TEXT("%.0f over the surface (resumes below %.0f)"), GetConfiguredCutoverAltitude(), GetConfiguredCutoverAltitude() * CutoverResumeFraction)
				: *FString::Printf(TEXT("off (no far field; %.0f configured)"), GetConfiguredCutoverAltitude()),
			CVarPlanetCutoverResumeGuard.GetValueOnAnyThread() != 0 ? TEXT("on") : TEXT("off"), ViewerSurfaceAltitude, CameraSurfaceAltitude,
			bWindowSuspended ? (bCutoverResumeHeld ? TEXT("SUSPENDED (resume held: the camera over the cut-over)") : TEXT("SUSPENDED")) : TEXT("active"));
	}
	else if (IsHeightmapWorld())
	{
		Info += FString::Printf(TEXT("  Voxel cut-over (WM-MP2): %s; viewer %.0f over the surface under it, camera %.0f, window %s\n"),
			bCutoverAllowed
				? *FString::Printf(TEXT("%.0f over the surface (resumes below %.0f)"), GetConfiguredCutoverAltitude(), GetConfiguredCutoverAltitude() * CutoverResumeFraction)
				: *FString::Printf(TEXT("off (no far field; %.0f configured)"), GetConfiguredCutoverAltitude()),
			ViewerSurfaceAltitude, CameraSurfaceAltitude,
			bWindowSuspended ? (bCutoverResumeHeld ? TEXT("SUSPENDED (resume held: the camera over the cut-over)") : TEXT("SUSPENDED")) : TEXT("active"));
	}

	Info += TEXT("\n  Bands:\n");
	for (int32 i = 0; i < LODBands.Num(); ++i)
	{
		const FLODBand& Band = LODBands[i];
		Info += FString::Printf(TEXT("    [%d] LOD%d: %.0f - %.0f (stride: %d)\n"),
			i, Band.LODLevel, Band.MinDistance, Band.MaxDistance, Band.VoxelStride);
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
	NearLayersRadius = (LODBands.Num() > 0) ? LODBands[0].MaxDistance + 2.0f * BaseChunkSize * VoxelSize : 0.0f;

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

float FDistanceBandLODStrategy::GetStreamingHorizon(const FVector& ChunkCenter, const FVector& ViewerPosition) const
{
	// Heightmap worlds: a cylinder. The column's window decides its vertical extent (D8), so the viewer's altitude
	// never takes the ground out of range; with a 3D horizon a viewer 40,000 uu up would load no terrain at all.
	if (IsHeightmapWorld())
	{
		return static_cast<float>(FVector::Dist2D(ChunkCenter, ViewerPosition));
	}
	// A planet: the same rule on a sphere (WM-BP8). Along the ground from the viewer's ground point, a cone around its
	// up-line; each chunk's slab decides its radial extent. A 3D distance from the viewer (P6.1) emptied the window from
	// about the view distance up and lost the ground under a climbing viewer (the planet's G13).
	if (PlanetRadius <= 0.0)
	{
		return static_cast<float>(FVector::Dist(ChunkCenter, ViewerPosition));
	}
	return static_cast<float>(GetPlanetGroundDistance(PlanetCenter, GetPlanetGroundRadius(ViewerPosition), ViewerPosition, ChunkCenter));
}

double FDistanceBandLODStrategy::GetPlanetGroundDistance(const FVector& Centre, double GroundRadius, const FVector& Viewer, const FVector& Point)
{
	// The angle between the two directions from the centre, from atan2 of the cross and dot products (accurate at every
	// angle, in double: both vectors are of size R).
	const FVector A = Viewer - Centre;
	const FVector B = Point - Centre;
	const double Cross = FVector::CrossProduct(A, B).Size();
	const double Dot = FVector::DotProduct(A, B);
	if (Cross <= 0.0 && Dot <= 0.0)
	{
		return Cross == 0.0 && Dot == 0.0 ? 0.0 : GroundRadius * UE_DOUBLE_PI; // one of them at the centre, or antipodal
	}
	return GroundRadius * FMath::Atan2(Cross, Dot);
}

bool FDistanceBandLODStrategy::ComputeCutoverState(bool bWasSuspended, double AltitudeOverSurface, double CutoverAltitude)
{
	if (CutoverAltitude <= 0.0)
	{
		return false;
	}
	return AltitudeOverSurface > (bWasSuspended ? CutoverAltitude * CutoverResumeFraction : CutoverAltitude);
}

bool FDistanceBandLODStrategy::ComputeCutoverState(bool bWasSuspended, double AltitudeOverSurface, double CutoverAltitude,
	double CameraAltitudeOverSurface)
{
	if (CutoverAltitude <= 0.0)
	{
		return false;
	}
	if (bWasSuspended)
	{
		return AltitudeOverSurface > CutoverAltitude * CutoverResumeFraction || CameraAltitudeOverSurface > CutoverAltitude;
	}
	return AltitudeOverSurface > CutoverAltitude;
}

double FDistanceBandLODStrategy::ComputeSurfaceAltitude(const FVector& Position) const
{
	if (PlanetWorldMode)
	{
		const double SurfaceRadius = FMath::Max(GetPlanetGroundRadius(Position), PlanetSeaRadius);
		return FVector::Dist(Position, PlanetCenter) - SurfaceRadius;
	}
	// One terrain sample when the position moves (as the planet's ground point).
	if (Position != HeightmapGroundViewer)
	{
		HeightmapGroundZ = static_cast<float>(TerrainWorldMode->GetSurfaceOnUpLine(Position, SlabNoiseParams).Position.Z);
		HeightmapGroundViewer = Position;
	}
	return Position.Z - FMath::Max(HeightmapGroundZ, HeightmapSeaZ);
}

double FDistanceBandLODStrategy::GetConfiguredCutoverAltitude() const
{
	// The configured altitude in any mode with a surface under the viewer; GetCutoverAltitude applies the far-field gate.
	if (!PlanetWorldMode && !IsHeightmapWorld())
	{
		return 0.0;
	}
	const float Override = CVarVoxelCutover.GetValueOnAnyThread();
	return Override >= 0.0f ? static_cast<double>(Override) : CutoverConfigured;
}

double FDistanceBandLODStrategy::GetPlanetGroundRadius(const FVector& ViewerPosition) const
{
	if (ViewerPosition == GroundRadiusViewer)
	{
		return GroundRadiusCached;
	}
	if (!PlanetWorldMode)
	{
		return PlanetRadius;
	}
	const FVector Direction = FSphericalPlanetWorldMode::GetDirectionFromCenter(ViewerPosition, PlanetCenter);
	return PlanetRadius + PlanetWorldMode->GetTerrainAltitude(Direction, SlabNoiseParams);
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

	// LOD disabled or no bands: nothing to balance; consumers fall back to raw (0).
	if (!bEnableLOD || LODBands.Num() == 0)
	{
		CommittedLODCache.Reset();
		return;
	}

	// Enumerate every chunk the strategy keeps within the load horizon: each column's keep window (GetColumnLayerRange),
	// which contains its load window, without the other world-mode culls. The resulting map is a superset of the chunks
	// that will actually be meshed, so every meshed chunk has a balanced entry, including chunks the near window loaded
	// at an earlier viewer layer; culled chunks are harmless extras.
	const FIntVector ViewerChunk = WorldPosToChunkCoord(Context.ViewerPosition);
	const float ChunkWorldSize = BaseChunkSize * VoxelSize;
	const int32 MaxChunkRadius = FMath::CeilToInt(MaxViewDistance / FMath::Max(ChunkWorldSize, 1.0f)) + 1;

	// Pass 1: raw band LOD + hysteresis (vs last frame's committed LOD).
	BalancedLODCache.Reserve((2 * MaxChunkRadius + 1) * (2 * MaxChunkRadius + 1) * 4);
	auto AddRaw = [&](const FIntVector& ChunkCoord, const FVector& ChunkCenter)
	{
		const float Distance = GetDistanceToViewer(ChunkCenter, Context);
		const int32 RawLOD = GetRawLODForDistance(Distance);

		int32 Committed = RawLOD;
		if (const int32* Prev = CommittedLODCache.Find(ChunkCoord))
		{
			Committed = ApplyLODHysteresis(*Prev, RawLOD, Distance);
		}

		BalancedLODCache.Add(ChunkCoord, Committed);
	};
	if (!IsHeightmapWorld() && !bWindowSuspended)
	{
		// A planet (WM-BP8): every chunk of the keep window within the load horizon (none above the voxel cut-over, WM-BP10).
		const double GroundRadius = GetPlanetGroundRadius(Context.ViewerPosition);
		ForEachPlanetCandidate(Context, EWindowUse::Keep, [&](const FIntVector& ChunkCoord)
		{
			const FVector ChunkCenter = ChunkCoordToWorldCenter(ChunkCoord);
			if (GetPlanetGroundDistance(PlanetCenter, GroundRadius, Context.ViewerPosition, ChunkCenter) <= MaxViewDistance
				&& IsInPlanetWindow(ChunkCoord, Context, EWindowUse::Keep))
			{
				AddRaw(ChunkCoord, ChunkCenter);
			}
		});
	}
	for (int32 X = -MaxChunkRadius; X <= MaxChunkRadius && IsHeightmapWorld() && !bWindowSuspended; ++X)
	{
		for (int32 Y = -MaxChunkRadius; Y <= MaxChunkRadius; ++Y)
		{
			const int32 ColumnX = ViewerChunk.X + X;
			const int32 ColumnY = ViewerChunk.Y + Y;
			if (GetStreamingHorizon(ChunkCoordToWorldCenter(FIntVector(ColumnX, ColumnY, 0)), Context.ViewerPosition) > MaxViewDistance)
			{
				continue;
			}

			GetColumnLayerRange(ColumnX, ColumnY, Context, EWindowUse::Keep).ForEachLayer([&](int32 Z)
			{
				const FIntVector ChunkCoord(ColumnX, ColumnY, Z);
				AddRaw(ChunkCoord, ChunkCoordToWorldCenter(ChunkCoord));
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
	// at any corner — no 3-LOD junction, regardless of band widths.
	{
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

		const FIntVector* Offsets = AllOffsets.GetData();
		const int32 NumOffsets = AllOffsets.Num();

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

FDistanceBandLODStrategy::FColumnLayerRange FDistanceBandLODStrategy::GetColumnLayerRange(
	int32 ChunkX,
	int32 ChunkY,
	const FLODQueryContext& Context,
	EWindowUse Use) const
{
	FColumnLayerRange Window;
	const FIntVector ViewerChunk = WorldPosToChunkCoord(Context.ViewerPosition);

	if (!IsHeightmapWorld())
	{
		GetPlanetShellWindow(ChunkX, ChunkY, Context, Window);
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
	const bool bNear = HorizontalDistance <= NearLayersRadius;

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

	// Load, near the viewer: also the viewer's own layer ± NearLayers, for caves, dungeons and digging below or
	// above the surface. The viewer's altitude only adds layers here; the slab keeps the ground.
	if (bNear)
	{
		Window.NearMinZ = FMath::Max(ViewerChunk.Z - NearLayers, BoundsMinZ);
		Window.NearMaxZ = FMath::Min(ViewerChunk.Z + NearLayers, BoundsMaxZ);
	}
	return Window;
}

void FDistanceBandLODStrategy::GetPlanetShellWindow(int32 ChunkX, int32 ChunkY, const FLODQueryContext& Context, FColumnLayerRange& OutWindow) const
{
	GetShellCrossing(2, ChunkX, ChunkY, ShellWindowInnerRadius, ShellWindowOuterRadius, Context.WorldOrigin, OutWindow);
}

void FDistanceBandLODStrategy::GetShellCrossing(int32 Axis, int32 ColumnA, int32 ColumnB, double InnerRadius, double OuterRadius,
	const FVector& WorldOrigin, FColumnLayerRange& OutWindow) const
{
	// The column's footprint relative to the line along Axis through the centre: its nearest and farthest points bound
	// the distance Rho from that line of every point in the column.
	const int32 AxisA = (Axis + 1) % 3;
	const int32 AxisB = (Axis + 2) % 3;
	const double ChunkWorldSize = static_cast<double>(BaseChunkSize) * VoxelSize;
	const double MinA = WorldOrigin[AxisA] + ColumnA * ChunkWorldSize - PlanetCenter[AxisA];
	const double MinB = WorldOrigin[AxisB] + ColumnB * ChunkWorldSize - PlanetCenter[AxisB];
	const double MaxA = MinA + ChunkWorldSize;
	const double MaxB = MinB + ChunkWorldSize;
	const double NearA = FMath::Clamp(0.0, MinA, MaxA);
	const double NearB = FMath::Clamp(0.0, MinB, MaxB);
	const double FarA = FMath::Max(FMath::Abs(MinA), FMath::Abs(MaxA));
	const double FarB = FMath::Max(FMath::Abs(MinB), FMath::Abs(MaxB));
	const double RhoMin2 = NearA * NearA + NearB * NearB;
	const double RhoMax2 = FarA * FarA + FarB * FarB;

	const double Outer2 = OuterRadius * OuterRadius;
	if (RhoMin2 > Outer2)
	{
		return; // the column passes outside the shell
	}

	// A point at Rho from the line is in the shell for |Z| (along Axis, from the centre) from sqrt(Inner^2 - Rho^2) (0 past
	// the inner radius) to sqrt(Outer^2 - Rho^2); over the footprint the union is [sqrt(Inner^2 - RhoMax^2),
	// sqrt(Outer^2 - RhoMin^2)]. One crossing on each side of the centre, which meet when the column reaches past the
	// inner radius. Not clipped to the viewer's altitude (WM-BP8): the horizon and the slab bound the window.
	const double Inner2 = InnerRadius * InnerRadius;
	const double ZHigh = FMath::Sqrt(Outer2 - RhoMin2);
	const double ZLow = RhoMax2 < Inner2 ? FMath::Sqrt(Inner2 - RhoMax2) : 0.0;

	const double CenterZ = PlanetCenter[Axis];
	auto AddCrossing = [&](double Lo, double Hi, int32& OutMinZ, int32& OutMaxZ)
	{
		if (Lo <= Hi)
		{
			GetLayersOverlapping(Lo, Hi, WorldOrigin[Axis], OutMinZ, OutMaxZ);
		}
	};
	if (ZLow <= 0.0)
	{
		AddCrossing(CenterZ - ZHigh, CenterZ + ZHigh, OutWindow.SlabMinZ, OutWindow.SlabMaxZ);
		return;
	}
	AddCrossing(CenterZ + ZLow, CenterZ + ZHigh, OutWindow.SlabMinZ, OutWindow.SlabMaxZ);
	AddCrossing(CenterZ - ZHigh, CenterZ - ZLow, OutWindow.NearMinZ, OutWindow.NearMaxZ);
	if (OutWindow.SlabMinZ <= OutWindow.NearMaxZ && OutWindow.NearMinZ <= OutWindow.SlabMaxZ && OutWindow.SlabMinZ <= OutWindow.SlabMaxZ
		&& OutWindow.NearMinZ <= OutWindow.NearMaxZ)
	{
		// The two crossings share layers (a small planet): one range, so ForEachLayer visits each layer once.
		OutWindow.SlabMinZ = FMath::Min(OutWindow.SlabMinZ, OutWindow.NearMinZ);
		OutWindow.SlabMaxZ = FMath::Max(OutWindow.SlabMaxZ, OutWindow.NearMaxZ);
		OutWindow.NearMinZ = 0;
		OutWindow.NearMaxZ = -1;
	}
}

void FDistanceBandLODStrategy::GetLayersOverlapping(double Lo, double Hi, double OriginZ, int32& OutMin, int32& OutMax) const
{
	// Layer Z spans [OriginZ + Z * W, OriginZ + (Z + 1) * W]. It overlaps [Lo, Hi] when its top is not below Lo and its
	// bottom is not above Hi (touching counts, as the old per-chunk bounds cull did).
	const double ChunkWorldSize = FMath::Max(static_cast<double>(BaseChunkSize) * VoxelSize, 1.0);
	OutMin = FMath::CeilToInt32((Lo - OriginZ) / ChunkWorldSize) - 1;
	OutMax = FMath::FloorToInt32((Hi - OriginZ) / ChunkWorldSize);
}

bool FDistanceBandLODStrategy::IsInPlanetWindow(const FIntVector& ChunkCoord, const FLODQueryContext& Context, EWindowUse Use) const
{
	const FPlanetChunkSlab Chunk = GetPlanetChunkSlab(ChunkCoord);

	// The terrain's shell: nothing outside it holds terrain.
	const double ShellLo = ShellWindowInnerRadius - PlanetRadius;
	const double ShellHi = ShellWindowOuterRadius - PlanetRadius;
	if (Chunk.AltitudeMax < ShellLo || Chunk.AltitudeMin > ShellHi)
	{
		return false;
	}

	// Its slab, at every distance (with slab culling off the slab is the shell).
	if (Chunk.AltitudeMax >= Chunk.SlabMin && Chunk.AltitudeMin <= Chunk.SlabMax)
	{
		return true;
	}

	// Off its slab, near the viewer along the ground: caves, dungeons and digging under the surface, and the chunks around
	// a viewer near the ground or the sea (the spawn's terrain wait and the settle hold wait for them to cook), as a
	// heightmap world streams the viewer's layer +- NearLayers within its height bounds.
	const FVector ChunkCenter = ChunkCoordToWorldCenter(ChunkCoord);
	if (GetPlanetGroundDistance(PlanetCenter, GetPlanetGroundRadius(Context.ViewerPosition), Context.ViewerPosition, ChunkCenter) > NearLayersRadius)
	{
		return false;
	}
	if (Use == EWindowUse::Keep)
	{
		// The whole shell within the unload distance, so a viewer changing altitude unloads nothing near it.
		return FVector::Dist(ChunkCenter, Context.ViewerPosition) <= GetUnloadDistance();
	}
	// Up to the top of the surface slab over the near disk, the band's ceiling (G52): above it every chunk the band adds off
	// its slab is air. A descending viewer streamed a new layer of them every second, taking the generation its ground disk
	// needed; one on the ground kept a layer of them loaded around it, which a descent loaded in its last second.
	const double ViewerAltitude = FVector::Dist(Context.ViewerPosition, PlanetCenter) - PlanetRadius;
	const double Ceiling = GetPlanetNearLayersCeiling(Context.ViewerPosition);
	if (ViewerAltitude > Ceiling)
	{
		return false;
	}
	const double Reach = NearLayers * static_cast<double>(BaseChunkSize) * VoxelSize;
	return Chunk.AltitudeMax >= ViewerAltitude - Reach && Chunk.AltitudeMin <= FMath::Min(ViewerAltitude + Reach, Ceiling);
}

FDistanceBandLODStrategy::FPlanetChunkSlab FDistanceBandLODStrategy::GetPlanetChunkSlab(const FIntVector& ChunkCoord) const
{
	if (const FPlanetChunkSlab* Found = PlanetChunkSlabCache.Find(ChunkCoord))
	{
		return *Found;
	}
	if (PlanetChunkSlabCache.Num() >= PlanetSlabCacheLimit)
	{
		PlanetChunkSlabCache.Reset();
	}

	// The chunk box relative to the planet centre: its nearest point and farthest corner bound its radial extent.
	const double ChunkWorldSize = static_cast<double>(BaseChunkSize) * VoxelSize;
	const FVector Min = CachedWorldOrigin + FVector(ChunkCoord) * ChunkWorldSize - PlanetCenter;
	const FVector Max = Min + FVector(ChunkWorldSize);
	const double Near = FMath::Sqrt(FBox(Min, Max).ComputeSquaredDistanceToPoint(FVector::ZeroVector));
	double Far2 = 0.0;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		Far2 += FMath::Square(FMath::Max(FMath::Abs(Min[Axis]), FMath::Abs(Max[Axis])));
	}

	FPlanetChunkSlab Slab;
	Slab.AltitudeMin = static_cast<float>(Near - PlanetRadius);
	Slab.AltitudeMax = static_cast<float>(FMath::Sqrt(Far2) - PlanetRadius);
	Slab.SlabMin = static_cast<float>(ShellWindowInnerRadius - PlanetRadius);
	Slab.SlabMax = static_cast<float>(ShellWindowOuterRadius - PlanetRadius);

	// The slab: the surface over the cells the chunk's corners span, ± SlabSlackChunks. Only for a chunk in the shell (a
	// chunk outside it is out of the window whatever its slab), off the centre (whose corners span every direction).
	const bool bInShell = Slab.AltitudeMax >= Slab.SlabMin && Slab.AltitudeMin <= Slab.SlabMax;
	if (bPlanetSlab && PlanetWorldMode && PlanetSlabGrid.IsValid() && bInShell && Near > ChunkWorldSize)
	{
		FVector Corners[8];
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Corners[Index] = FVector((Index & 1) ? Max.X : Min.X, (Index & 2) ? Max.Y : Min.Y, (Index & 4) ? Max.Z : Min.Z).GetSafeNormal();
		}
		TArray<FIntPoint> Cells;
		Cells.Reserve(16);
		PlanetSlabGrid.GatherSpanned(Corners, Cells);
		FFloatInterval Surface(TNumericLimits<float>::Max(), TNumericLimits<float>::Lowest());
		for (const FIntPoint& Cell : Cells)
		{
			const FFloatInterval Range = GetPlanetCellSurfaceRange(Cell);
			Surface.Min = FMath::Min(Surface.Min, Range.Min);
			Surface.Max = FMath::Max(Surface.Max, Range.Max);
		}
		if (Surface.Min <= Surface.Max)
		{
			const float Slack = static_cast<float>(SlabSlackChunks * ChunkWorldSize);
			Slab.SlabMin = Surface.Min - Slack;
			Slab.SlabMax = Surface.Max + Slack;
		}
	}
	PlanetChunkSlabCache.Add(ChunkCoord, Slab);
	return Slab;
}

FFloatInterval FDistanceBandLODStrategy::GetPlanetCellSurfaceRange(const FIntPoint& Cell) const
{
	if (const FFloatInterval* Found = PlanetCellSurfaceCache.Find(Cell))
	{
		return *Found;
	}
	if (PlanetCellSurfaceCache.Num() >= PlanetSlabCacheLimit)
	{
		PlanetCellSurfaceCache.Reset();
	}

	// Five samples, as a heightmap column's GetSurfaceRange takes: the corners and the centre. SlabSlackChunks covers the
	// surface between them (and conditioning's deltas from the natural surface), as on a heightmap.
	static constexpr double Fractions[5][2] = { { 0.0, 0.0 }, { 1.0, 0.0 }, { 0.0, 1.0 }, { 1.0, 1.0 }, { 0.5, 0.5 } };
	FFloatInterval Range(TNumericLimits<float>::Max(), TNumericLimits<float>::Lowest());
	if (PlanetWorldMode && PlanetSlabGrid.IsValidCell(Cell))
	{
		for (const double* Fraction : Fractions)
		{
			const float Altitude = PlanetWorldMode->GetTerrainAltitude(PlanetSlabGrid.DirectionAt(Cell, Fraction[0], Fraction[1]), SlabNoiseParams);
			Range.Min = FMath::Min(Range.Min, Altitude);
			Range.Max = FMath::Max(Range.Max, Altitude);
		}
	}
	else
	{
		Range = FFloatInterval(-PlanetMaxTerrainDepth, PlanetMaxTerrainHeight);
	}
	PlanetCellSurfaceCache.Add(Cell, Range);
	return Range;
}

FFloatInterval FDistanceBandLODStrategy::GetPlanetSurfaceEnvelope(const FVector& Direction, double Angle, FSurfaceEnvelopeMemo& Memo) const
{
	// Memoized by the direction's cell: the disk is gathered around that cell's centre, one cell wider each way, so it
	// holds the disk around any direction inside the cell.
	const FIntPoint Cell = PlanetSlabGrid.CellOfDirection(Direction);
	if (Cell == Memo.Cell && Angle == Memo.Angle)
	{
		return Memo.Range;
	}

	const double CellAngle = 2.0 * (0.5 * UE_DOUBLE_PI) / FMath::Max(1, PlanetSlabGrid.GetCellsPerEdge());
	TArray<FIntPoint> Cells;
	constexpr int32 MaxCells = 40000;
	FFloatInterval Envelope(-PlanetMaxTerrainDepth, PlanetMaxTerrainHeight);
	if (PlanetSlabGrid.GatherWithinAngle(PlanetSlabGrid.CellCentreDirection(Cell), Angle + 2.0 * CellAngle, MaxCells, Cells))
	{
		Envelope = FFloatInterval(TNumericLimits<float>::Max(), TNumericLimits<float>::Lowest());
		for (const FIntPoint& DiskCell : Cells)
		{
			const FFloatInterval Range = GetPlanetCellSurfaceRange(DiskCell);
			Envelope.Min = FMath::Min(Envelope.Min, Range.Min);
			Envelope.Max = FMath::Max(Envelope.Max, Range.Max);
		}
	}
	// else: a disk of more cells than that (a view distance of a large part of the planet): the terrain's bounds.

	Memo.Cell = Cell;
	Memo.Angle = Angle;
	Memo.Range = Envelope;
	return Envelope;
}

double FDistanceBandLODStrategy::GetPlanetNearLayersCeiling(const FVector& ViewerPosition) const
{
	const double ShellTop = ShellWindowOuterRadius - PlanetRadius;
	if (!PlanetWorldMode || !bPlanetSlab || !PlanetSlabGrid.IsValid())
	{
		return ShellTop; // without the slab every chunk in the shell is in the window anyway
	}

	// The highest surface the near disk's chunks read for their slabs, or the sea's surface where that is higher; the
	// slab's slack over it, as every slab has.
	const FVector Relative = ViewerPosition - PlanetCenter;
	const FVector Up = Relative.IsNearlyZero() ? FVector::UpVector : Relative.GetUnsafeNormal();
	double Top = GetPlanetSurfaceEnvelope(Up, PlanetNearBandAngle, NearEnvelopeMemo).Max;
	if (PlanetSeaRadius > 0.0)
	{
		Top = FMath::Max(Top, PlanetSeaRadius - PlanetRadius);
	}
	return Top + SlabSlackChunks * static_cast<double>(BaseChunkSize) * VoxelSize;
}

bool FDistanceBandLODStrategy::IsPlanetNearBandActive(const FVector& ViewerPosition) const
{
	return FVector::Dist(ViewerPosition, PlanetCenter) - PlanetRadius <= GetPlanetNearLayersCeiling(ViewerPosition);
}

void FDistanceBandLODStrategy::ForEachPlanetCandidate(const FLODQueryContext& Context, EWindowUse Use,
	TFunctionRef<void(const FIntVector&)> Visit) const
{
	if (PlanetRadius <= 0.0)
	{
		return;
	}
	const double ChunkWorldSize = static_cast<double>(BaseChunkSize) * VoxelSize;
	const FVector ViewerRelative = Context.ViewerPosition - PlanetCenter;
	const double ViewerRadius = ViewerRelative.Size();
	const FVector Up = ViewerRadius > UE_SMALL_NUMBER ? ViewerRelative / ViewerRadius : FVector::UpVector;
	const double GroundRadius = FMath::Max(GetPlanetGroundRadius(Context.ViewerPosition), ChunkWorldSize);
	const double ShellLo = ShellWindowInnerRadius - PlanetRadius;
	const double ShellHi = ShellWindowOuterRadius - PlanetRadius;

	// A chunk is in range by its centre's ground distance, and its corners (which pick its slab's cells) lie within half a
	// diagonal of the centre: the angle that subtends at the shell's inner radius widens every cone below.
	const double HalfDiagonal = 0.5 * ChunkWorldSize * UE_DOUBLE_SQRT_3;
	const double ChunkAngle = FMath::Asin(FMath::Min(1.0, HalfDiagonal / FMath::Max(ShellWindowInnerRadius, HalfDiagonal)));
	const double LoadAngle = FMath::Min(UE_DOUBLE_PI, MaxViewDistance / GroundRadius + ChunkAngle);

	TSet<FIntVector> Seen;
	Seen.Reserve(4096);

	// The chunks crossing the spherical shell [R + AltLo, R + AltHi] whose columns lie in the box around the cone of
	// directions within Angle of Up. Columns run along the grid axis closest to Up, so each crosses the shell steeply.
	auto Enumerate = [&](double Angle, double AltLo, double AltHi)
	{
		const double R0 = FMath::Max(0.0, PlanetRadius + AltLo);
		const double R1 = PlanetRadius + AltHi;
		if (R1 < R0)
		{
			return;
		}
		// The cone segment's bounding box, relative to the centre: along an axis at angle Phi from Up, the directions within
		// Angle reach cos(max(0, Phi - Angle)) at most and cos(min(pi, Phi + Angle)) at least.
		FVector BoxMin, BoxMax;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double Phi = FMath::Acos(FMath::Clamp(Up[Axis], -1.0, 1.0));
			const double MaxDot = FMath::Cos(FMath::Max(0.0, Phi - Angle));
			const double MinDot = FMath::Cos(FMath::Min(UE_DOUBLE_PI, Phi + Angle));
			BoxMin[Axis] = FMath::Min(R0 * MinDot, R1 * MinDot);
			BoxMax[Axis] = FMath::Max(R0 * MaxDot, R1 * MaxDot);
		}
		const int32 Axis = (FMath::Abs(Up.X) >= FMath::Abs(Up.Y) && FMath::Abs(Up.X) >= FMath::Abs(Up.Z)) ? 0
			: (FMath::Abs(Up.Y) >= FMath::Abs(Up.Z) ? 1 : 2);
		const int32 AxisA = (Axis + 1) % 3;
		const int32 AxisB = (Axis + 2) % 3;
		const FVector GridOffset = CachedWorldOrigin - PlanetCenter; // the grid origin relative to the centre
		auto ToLayer = [&](double Relative, int32 OnAxis) { return FMath::FloorToInt32((Relative - GridOffset[OnAxis]) / ChunkWorldSize); };
		const int32 AxisLo = ToLayer(BoxMin[Axis], Axis);
		const int32 AxisHi = ToLayer(BoxMax[Axis], Axis);
		for (int32 IA = ToLayer(BoxMin[AxisA], AxisA); IA <= ToLayer(BoxMax[AxisA], AxisA); ++IA)
		{
			for (int32 IB = ToLayer(BoxMin[AxisB], AxisB); IB <= ToLayer(BoxMax[AxisB], AxisB); ++IB)
			{
				FColumnLayerRange Crossing;
				GetShellCrossing(Axis, IA, IB, R0, R1, CachedWorldOrigin, Crossing);
				auto VisitRange = [&](int32 Lo, int32 Hi)
				{
					for (int32 K = FMath::Max(Lo, AxisLo); K <= FMath::Min(Hi, AxisHi); ++K)
					{
						FIntVector Coord;
						Coord[Axis] = K;
						Coord[AxisA] = IA;
						Coord[AxisB] = IB;
						bool bAlreadySeen = false;
						Seen.Add(Coord, &bAlreadySeen);
						if (!bAlreadySeen)
						{
							Visit(Coord);
						}
					}
				};
				VisitRange(Crossing.SlabMinZ, Crossing.SlabMaxZ);
				VisitRange(Crossing.NearMinZ, Crossing.NearMaxZ);
			}
		}
	};

	// The slab over the disk of the load horizon (the surface's envelope there ± slack), or the whole shell.
	double EnvelopeLo = ShellLo;
	double EnvelopeHi = ShellHi;
	if (bPlanetSlab && PlanetWorldMode)
	{
		const FFloatInterval Surface = GetPlanetSurfaceEnvelope(Up, LoadAngle, LoadEnvelopeMemo);
		const double Slack = SlabSlackChunks * ChunkWorldSize;
		EnvelopeLo = FMath::Max(ShellLo, Surface.Min - Slack);
		EnvelopeHi = FMath::Min(ShellHi, Surface.Max + Slack);
	}
	Enumerate(LoadAngle, EnvelopeLo, EnvelopeHi);

	// The near window, off the slab near the viewer (IsInPlanetWindow): Load, the viewer's altitude ± NearLayers up
	// to the band's ceiling, none above it (G52); Keep, the shell within the unload distance.
	const double NearCeiling = (Use == EWindowUse::Load) ? GetPlanetNearLayersCeiling(Context.ViewerPosition) : TNumericLimits<double>::Max();
	if (bPlanetSlab && NearLayersRadius > 0.0f && ViewerRadius - PlanetRadius <= NearCeiling)
	{
		const double ViewerAltitude = ViewerRadius - PlanetRadius;
		const double Reach = (Use == EWindowUse::Load) ? NearLayers * ChunkWorldSize : GetUnloadDistance();
		const double NearLo = FMath::Max(ShellLo, ViewerAltitude - Reach);
		const double NearHi = FMath::Min3(ShellHi, ViewerAltitude + Reach, NearCeiling);
		if (NearLo <= NearHi)
		{
			Enumerate(FMath::Min(UE_DOUBLE_PI, NearLayersRadius / GroundRadius + ChunkAngle), NearLo, NearHi);
		}
	}
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
	// included, as generation produces it). The slack GetColumnLayerRange applies covers variation between the samples.
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
	if (WorldMode != EWorldMode::SphericalPlanet || PlanetRadius <= 0.0)
	{
		return false;
	}

	// Distances from a chunk's centre bound every point in it within half its diagonal.
	const double ChunkHalfDiagonal = 0.5 * BaseChunkSize * VoxelSize * UE_SQRT_3;
	const FVector ChunkCenter = ChunkCoordToWorldCenter(ChunkCoord);
	const double ChunkDistanceFromCenter = FVector::Dist(ChunkCenter, PlanetCenter);

	// SHELL: cull a chunk wholly inside the window's inner radius (the core) or wholly outside its outer radius (space).
	// The window (GetPlanetShellWindow) uses the same radii, exactly per column; this bound keeps a superset of it, so
	// nothing the window loads is unloaded by the cull.
	if (ChunkDistanceFromCenter + ChunkHalfDiagonal < ShellWindowInnerRadius
		|| ChunkDistanceFromCenter - ChunkHalfDiagonal > ShellWindowOuterRadius)
	{
		return true;
	}

	// HORIZON (SP-C2): a point at height H above the datum shows over the horizon of a viewer at height h while their
	// distance is under sqrt(2Rh + h^2) + sqrt(2RH + H^2), the two tangent distances to the datum sphere. With H the
	// shell's top, a chunk whose nearest point is farther than that holds nothing the viewer can see. A viewer below the
	// datum (a valley, the sea floor) sees at least the peaks; a viewer below the shell (inside the planet) is not culled.
	const double ViewerAltitude = FVector::Dist(Context.ViewerPosition, PlanetCenter) - PlanetRadius;
	if (ViewerAltitude < -PlanetMaxTerrainDepth)
	{
		return false;
	}
	const double Horizon = GetVisibilityHorizon(PlanetRadius, ViewerAltitude, PlanetMaxTerrainHeight);
	return FVector::Dist(Context.ViewerPosition, ChunkCenter) - ChunkHalfDiagonal > Horizon;
}

double FDistanceBandLODStrategy::GetVisibilityHorizon(double PlanetRadius, double ViewerAltitude, double MaxTerrainHeight)
{
	const double Viewer = FMath::Max(0.0, ViewerAltitude);
	const double Peaks = FMath::Max(0.0, MaxTerrainHeight);
	return FMath::Sqrt(2.0 * PlanetRadius * Viewer + Viewer * Viewer) + FMath::Sqrt(2.0 * PlanetRadius * Peaks + Peaks * Peaks);
}
