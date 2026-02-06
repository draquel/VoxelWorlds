// Copyright Daniel Raquel. All Rights Reserved.

#include "DistanceBandLODStrategy.h"
#include "VoxelLOD.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelCoordinates.h"
#include "DrawDebugHelpers.h"

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
	bEnableLOD = WorldConfig->bEnableLOD;
	bEnableMorphing = WorldConfig->bEnableLODMorphing;
	bEnableFrustumCulling = WorldConfig->bEnableFrustumCulling;

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

	// Cache world-mode-specific parameters first (needed for vertical range calculation)
	const float ChunkWorldSize = BaseChunkSize * VoxelSize;

	// Cache Infinite Plane terrain bounds for vertical culling
	if (WorldMode == EWorldMode::InfinitePlane)
	{
		// Terrain extends from SeaLevel + BaseHeight (minimum) to SeaLevel + BaseHeight + HeightScale (maximum)
		// Add one chunk as buffer for terrain variation and meshing
		const float TerrainBase = WorldConfig->SeaLevel + WorldConfig->BaseHeight;
		TerrainMinHeight = TerrainBase - ChunkWorldSize; // One chunk below base for safety
		TerrainMaxHeight = TerrainBase + WorldConfig->HeightScale + ChunkWorldSize; // One chunk above max

		UE_LOG(LogVoxelLOD, Log, TEXT("  Terrain bounds culling: Height range [%.0f - %.0f]"),
			TerrainMinHeight, TerrainMaxHeight);
	}

	// Cache Island mode parameters for boundary culling
	if (WorldMode == EWorldMode::IslandBowl)
	{
		IslandTotalExtent = WorldConfig->IslandRadius + WorldConfig->IslandFalloffWidth;
		IslandCenterOffset = FVector2D(WorldConfig->IslandCenterX, WorldConfig->IslandCenterY);

		UE_LOG(LogVoxelLOD, Log, TEXT("  Island culling enabled: TotalExtent=%.0f, CenterOffset=(%.0f, %.0f)"),
			IslandTotalExtent, IslandCenterOffset.X, IslandCenterOffset.Y);
	}

	// Cache Spherical Planet parameters for shell culling
	if (WorldMode == EWorldMode::SphericalPlanet)
	{
		PlanetRadius = WorldConfig->WorldRadius;
		PlanetMaxTerrainHeight = WorldConfig->PlanetMaxTerrainHeight;
		PlanetMaxTerrainDepth = WorldConfig->PlanetMaxTerrainDepth;

		UE_LOG(LogVoxelLOD, Log, TEXT("  Shell culling enabled: Radius=%.0f, Height=%.0f, Depth=%.0f"),
			PlanetRadius, PlanetMaxTerrainHeight, PlanetMaxTerrainDepth);
	}

	// Set vertical range based on world mode
	switch (WorldMode)
	{
		case EWorldMode::InfinitePlane:
			MinVerticalChunks = -2;
			MaxVerticalChunks = 8;
			break;

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

		case EWorldMode::IslandBowl:
			MinVerticalChunks = -4;
			MaxVerticalChunks = 8;
			break;
	}

	bIsInitialized = true;

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

	const FVector ChunkCenter = ChunkCoordToWorldCenter(ChunkCoord);
	const float Distance = GetDistanceToViewer(ChunkCenter, Context);

	// Debug: Log LOD calculation for chunk at origin periodically
	static int32 LODDebugCounter = 0;
	if (++LODDebugCounter % 600 == 0 && ChunkCoord == FIntVector(0, 0, 0))
	{
		UE_LOG(LogVoxelLOD, Warning, TEXT("GetLODForChunk(0,0,0): ChunkCenter=(%.0f,%.0f,%.0f), ViewerPos=(%.0f,%.0f,%.0f), Distance=%.0f"),
			ChunkCenter.X, ChunkCenter.Y, ChunkCenter.Z,
			Context.ViewerPosition.X, Context.ViewerPosition.Y, Context.ViewerPosition.Z,
			Distance);
	}

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

	int32 MinZ, MaxZ;
	GetVerticalChunkRange(Context, MinZ, MaxZ);

	// Iterate over potential chunk positions
	for (int32 X = -MaxChunkRadius; X <= MaxChunkRadius; ++X)
	{
		for (int32 Y = -MaxChunkRadius; Y <= MaxChunkRadius; ++Y)
		{
			for (int32 Z = MinZ; Z <= MaxZ; ++Z)
			{
				const FIntVector ChunkCoord = ViewerChunk + FIntVector(X, Y, Z);
				const FVector ChunkCenter = ChunkCoordToWorldCenter(ChunkCoord);
				const float Distance = GetDistanceToViewer(ChunkCenter, Context);

				// Skip if beyond max view distance
				if (Distance > MaxViewDistance)
				{
					continue;
				}

				// World-mode-specific culling
				// Infinite plane: skip chunks outside terrain height bounds
				if (ShouldCullOutsideTerrainBounds(ChunkCoord, Context))
				{
					continue;
				}

				// Island mode: skip chunks outside island boundary
				if (ShouldCullIslandBoundary(ChunkCoord, Context))
				{
					continue;
				}

				// Spherical planet mode: skip chunks beyond horizon/shell
				if (ShouldCullBeyondHorizon(ChunkCoord, Context))
				{
					continue;
				}

				// Find appropriate LOD band
				const FLODBand* Band = FindBandForDistance(Distance);

				// Determine LOD level: use band if found, otherwise fallback
				int32 LODLevel = 0;
				float MorphFactor = 0.0f;

				if (Band)
				{
					LODLevel = Band->LODLevel;
					MorphFactor = bEnableMorphing ? Band->GetMorphFactor(Distance) : 0.0f;
				}
				else if (bEnableLOD && LODBands.Num() > 0)
				{
					// Beyond all bands but within ViewDistance - use coarsest LOD
					LODLevel = LODBands.Last().LODLevel;
				}
				// else: LOD disabled or no bands - use LOD 0

				// Frustum culling (optional)
				if (bEnableFrustumCulling && !IsChunkInFrustum(ChunkCoord, Context))
				{
					continue;
				}

				// Create request
				FChunkLODRequest Request;
				Request.ChunkCoord = ChunkCoord;
				Request.LODLevel = LODLevel;
				Request.Priority = CalculatePriority(ChunkCoord, Context);
				Request.MorphFactor = MorphFactor;

				Requests.Add(Request);
			}
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

	// Check each loaded chunk
	for (const FIntVector& ChunkCoord : LoadedChunks)
	{
		bool bShouldUnload = false;

		// Check world-mode-specific culling first (these should be unloaded immediately)
		if (ShouldCullOutsideTerrainBounds(ChunkCoord, Context) ||
			ShouldCullIslandBoundary(ChunkCoord, Context) ||
			ShouldCullBeyondHorizon(ChunkCoord, Context))
		{
			bShouldUnload = true;
		}
		else
		{
			// Standard distance-based unloading
			const FVector ChunkCenter = ChunkCoordToWorldCenter(ChunkCoord);
			const float Distance = GetDistanceToViewer(ChunkCenter, Context);

			if (Distance > UnloadDistance)
			{
				bShouldUnload = true;
			}
		}

		if (bShouldUnload)
		{
			OutUnload.Add(ChunkCoord);

			// Respect per-frame limit
			if (OutUnload.Num() >= Context.MaxChunksToUnloadPerFrame)
			{
				break;
			}
		}
	}

	// Sort by distance (farthest first for unloading)
	OutUnload.Sort([this, &Context](const FIntVector& A, const FIntVector& B)
	{
		const float DistA = GetDistanceToViewer(ChunkCoordToWorldCenter(A), Context);
		const float DistB = GetDistanceToViewer(ChunkCoordToWorldCenter(B), Context);
		return DistA > DistB;
	});
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
	if (WorldMode == EWorldMode::InfinitePlane)
	{
		Info += FString::Printf(TEXT("  Terrain Culling: Height=[%.0f - %.0f]\n"),
			TerrainMinHeight, TerrainMaxHeight);
	}
	if (WorldMode == EWorldMode::IslandBowl && IslandTotalExtent > 0.0f)
	{
		Info += FString::Printf(TEXT("  Island Culling: Extent=%.0f, Center=(%.0f, %.0f)\n"),
			IslandTotalExtent, IslandCenterOffset.X, IslandCenterOffset.Y);
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

	// Sort by distance
	LODBands.Sort([](const FLODBand& A, const FLODBand& B)
	{
		return A.MinDistance < B.MinDistance;
	});

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
	switch (Context.WorldMode)
	{
		case EWorldMode::SphericalPlanet:
		{
			// For spherical planets, use geodesic distance along surface
			// Simplified: use 3D distance for now, can be improved later
			return FVector::Distance(Position, Context.ViewerPosition);
		}

		case EWorldMode::IslandBowl:
		{
			// For islands, use 2D distance (ignore height difference)
			return FVector::Dist2D(Position, Context.ViewerPosition);
		}

		default: // InfinitePlane
		{
			return FVector::Distance(Position, Context.ViewerPosition);
		}
	}
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

void FDistanceBandLODStrategy::GetVerticalChunkRange(
	const FLODQueryContext& Context,
	int32& OutMinZ,
	int32& OutMaxZ) const
{
	const FIntVector ViewerChunk = WorldPosToChunkCoord(Context.ViewerPosition);

	OutMinZ = ViewerChunk.Z + MinVerticalChunks;
	OutMaxZ = ViewerChunk.Z + MaxVerticalChunks;
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

bool FDistanceBandLODStrategy::ShouldCullOutsideTerrainBounds(
	const FIntVector& ChunkCoord,
	const FLODQueryContext& Context) const
{
	if (WorldMode != EWorldMode::InfinitePlane)
	{
		return false;
	}

	// Get chunk's Z bounds in world space
	const float ChunkWorldSize = BaseChunkSize * VoxelSize;
	const float ChunkMinZ = Context.WorldOrigin.Z + (ChunkCoord.Z * ChunkWorldSize);
	const float ChunkMaxZ = ChunkMinZ + ChunkWorldSize;

	// Cull if chunk is entirely below terrain minimum
	if (ChunkMaxZ < TerrainMinHeight)
	{
		return true;
	}

	// Cull if chunk is entirely above terrain maximum
	if (ChunkMinZ > TerrainMaxHeight)
	{
		return true;
	}

	return false;
}

bool FDistanceBandLODStrategy::ShouldCullIslandBoundary(
	const FIntVector& ChunkCoord,
	const FLODQueryContext& Context) const
{
	if (WorldMode != EWorldMode::IslandBowl || IslandTotalExtent <= 0.0f)
	{
		return false;
	}

	// Get chunk center in world space
	const FVector ChunkCenter = ChunkCoordToWorldCenter(ChunkCoord);

	// Calculate 2D distance from island center (WorldOrigin + IslandCenterOffset)
	const FVector2D IslandCenter(
		Context.WorldOrigin.X + IslandCenterOffset.X,
		Context.WorldOrigin.Y + IslandCenterOffset.Y
	);
	const FVector2D ChunkCenter2D(ChunkCenter.X, ChunkCenter.Y);
	const float Distance2D = FVector2D::Distance(ChunkCenter2D, IslandCenter);

	// Add chunk diagonal as buffer (chunk could overlap island boundary)
	const float ChunkWorldSize = BaseChunkSize * VoxelSize;
	const float ChunkDiagonal = ChunkWorldSize * UE_SQRT_2;

	// Cull if chunk center is beyond island extent + buffer
	return Distance2D > (IslandTotalExtent + ChunkDiagonal);
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
	const FVector ToChunkFromPlanet = ChunkCenter - Context.WorldOrigin;
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
	const FVector ToViewerFromPlanet = Context.ViewerPosition - Context.WorldOrigin;
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
