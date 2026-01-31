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
	bEnableMorphing = WorldConfig->bEnableLODMorphing;
	bEnableFrustumCulling = WorldConfig->bEnableFrustumCulling;

	// Copy LOD bands
	LODBands = WorldConfig->LODBands;

	// Sort bands by distance
	LODBands.Sort([](const FLODBand& A, const FLODBand& B)
	{
		return A.MinDistance < B.MinDistance;
	});

	// Calculate max view distance
	MaxViewDistance = 0.0f;
	for (const FLODBand& Band : LODBands)
	{
		MaxViewDistance = FMath::Max(MaxViewDistance, Band.MaxDistance);
	}

	// Set vertical range based on world mode
	switch (WorldMode)
	{
		case EWorldMode::InfinitePlane:
			MinVerticalChunks = -2;
			MaxVerticalChunks = 8;
			break;

		case EWorldMode::SphericalPlanet:
			// Spherical worlds need full 3D range
			MinVerticalChunks = -32;
			MaxVerticalChunks = 32;
			break;

		case EWorldMode::IslandBowl:
			MinVerticalChunks = -4;
			MaxVerticalChunks = 8;
			break;
	}

	bIsInitialized = true;

	UE_LOG(LogVoxelLOD, Log, TEXT("FDistanceBandLODStrategy initialized with %d LOD bands, max distance: %.0f"),
		LODBands.Num(), MaxViewDistance);
}

void FDistanceBandLODStrategy::Update(const FLODQueryContext& Context, float DeltaTime)
{
	// Cache viewer position for quick access
	CachedViewerPosition = Context.ViewerPosition;
	CachedViewerChunk = WorldPosToChunkCoord(Context.ViewerPosition);
}

int32 FDistanceBandLODStrategy::GetLODForChunk(
	const FIntVector& ChunkCoord,
	const FLODQueryContext& Context) const
{
	const FVector ChunkCenter = ChunkCoordToWorldCenter(ChunkCoord);
	const float Distance = GetDistanceToViewer(ChunkCenter, Context);

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

				// Find appropriate LOD band
				const FLODBand* Band = FindBandForDistance(Distance);
				if (!Band)
				{
					continue;
				}

				// Frustum culling (optional)
				if (bEnableFrustumCulling && !IsChunkInFrustum(ChunkCoord, Context))
				{
					continue;
				}

				// Create request
				FChunkLODRequest Request;
				Request.ChunkCoord = ChunkCoord;
				Request.LODLevel = Band->LODLevel;
				Request.Priority = CalculatePriority(ChunkCoord, Context);
				Request.MorphFactor = bEnableMorphing ? Band->GetMorphFactor(Distance) : 0.0f;

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
		const FVector ChunkCenter = ChunkCoordToWorldCenter(ChunkCoord);
		const float Distance = GetDistanceToViewer(ChunkCenter, Context);

		// Unload if beyond unload distance
		if (Distance > UnloadDistance)
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

	// Recalculate max view distance
	MaxViewDistance = 0.0f;
	for (const FLODBand& Band : LODBands)
	{
		MaxViewDistance = FMath::Max(MaxViewDistance, Band.MaxDistance);
	}
}

// ==================== Internal Helpers ====================

FVector FDistanceBandLODStrategy::ChunkCoordToWorldCenter(const FIntVector& ChunkCoord) const
{
	return FVoxelCoordinates::ChunkToWorldCenter(ChunkCoord, BaseChunkSize, VoxelSize);
}

FIntVector FDistanceBandLODStrategy::WorldPosToChunkCoord(const FVector& WorldPos) const
{
	return FVoxelCoordinates::WorldToChunk(WorldPos, BaseChunkSize, VoxelSize);
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

	// Get chunk bounding box
	const FBox ChunkBounds = FVoxelCoordinates::ChunkToWorldBounds(ChunkCoord, BaseChunkSize, VoxelSize);
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
