// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelScatterManager.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelScatterRenderer.h"
#include "VoxelScatterConfiguration.h"
#include "VoxelScatterExclusionSubsystem.h"
#include "VoxelScatterPlacement.h"
#include "VoxelWorldConfiguration.h"
#include "IVoxelWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelCubeSphere.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelCaveConfiguration.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoise.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelBiomeRegistry.h"
#include "VoxelScatter.h"
#include "DrawDebugHelpers.h"
#include "Algo/BinarySearch.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectIterator.h"

static TAutoConsoleVariable<int32> CVarVoxelScatterRegionChunks(
	TEXT("voxel.Scatter.RegionChunks"),
	-1,
	TEXT("Chunks along the edge of a scatter HISM's region: each scatter type gets one HISM per region.\n")
	TEXT("-1 = 4 in every world mode; 0 = the render regions\n")
	TEXT("(voxel.Render.RegionChunks); N > 0 = regions of N chunks. Read when the scatter manager initializes."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarVoxelScatterFirstExtractionAtHorizon(
	TEXT("voxel.Scatter.FirstExtractionAtHorizon"),
	1,
	TEXT("World modes review G79: 1 = a chunk handed off with every scatter type out of range (it arrived at the window's front)\n")
	TEXT("is handed off again by the distance pass once it comes within the scatter horizon (the largest spawn distance), and\n")
	TEXT("types with SpawnDistance 0 stream at ScatterRadius like the others; 0 = such a chunk waits for its next mesh, the LOD\n")
	TEXT("refine near the viewer, where every type lands at once (before). Read each distance pass."),
	ECVF_Default);

/** The scatter HISM regions by default (voxel.Scatter.RegionChunks -1), every world mode: a quarter of the render regions' edge. */
static constexpr int32 DefaultScatterRegionChunks = 4;

namespace
{
	/**
	 * voxel.Scatter.Digest [file]: log every game world's scatter digest (UVoxelScatterManager::ComputeScatterDigest). With
	 * "file", also write one line per chunk (X,Y,Z,count,crc) to Saved/VoxelAudit/ScatterDigest_<world>_<time>.csv, to
	 * see which chunks two builds disagree on. A second line checks the caches against each other and the renderer
	 * (ComputeCacheResidency, world modes review G30), with up to five chunks whose instances differ from their spawn
	 * points; "file" writes all of them to ScatterCaches_<world>_<time>.csv.
	 */
	FAutoConsoleCommand GVoxelScatterDigestCmd(
		TEXT("voxel.Scatter.Digest"),
		TEXT("Log a digest of the cached scatter spawn points (counts and a CRC per type) for every game world, to compare builds. ")
		TEXT("'voxel.Scatter.Digest file' also writes a per-chunk CSV to Saved/VoxelAudit."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const bool bFile = Args.Num() > 0 && Args[0].Equals(TEXT("file"), ESearchCase::IgnoreCase);
			int32 Managers = 0;
			for (TObjectIterator<UVoxelScatterManager> It; It; ++It)
			{
				const UWorld* World = It->GetWorld();
				if (!World || !World->IsGameWorld())
				{
					continue;
				}
				++Managers;
				TArray<FString> ChunkLines;
				UE_LOG(LogVoxelScatter, Display, TEXT("voxel.Scatter.Digest [%s]: %s"), *World->GetName(),
					*It->ComputeScatterDigest(bFile ? &ChunkLines : nullptr));
				TArray<FString> MismatchLines;
				UE_LOG(LogVoxelScatter, Display, TEXT("voxel.Scatter.Digest [%s] caches: %s"), *World->GetName(), *It->ComputeCacheResidency(&MismatchLines));
				for (int32 i = 0; i < FMath::Min(MismatchLines.Num(), 5); ++i)
				{
					UE_LOG(LogVoxelScatter, Display, TEXT("voxel.Scatter.Digest [%s]   mismatch %s"), *World->GetName(), *MismatchLines[i]);
				}
				if (bFile && MismatchLines.Num() > 0)
				{
					const FString Path = FPaths::ProjectSavedDir() / TEXT("VoxelAudit") / FString::Printf(TEXT("ScatterCaches_%s_%s.csv"),
						*World->GetName(), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
					MismatchLines.Insert(TEXT("X,Y,Z,spawnPoints,instances,surface,types"), 0);
					FFileHelper::SaveStringArrayToFile(MismatchLines, *Path);
				}
				if (bFile)
				{
					const FString Path = FPaths::ProjectSavedDir() / TEXT("VoxelAudit") / FString::Printf(TEXT("ScatterDigest_%s_%s.csv"),
						*World->GetName(), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
					ChunkLines.Insert(TEXT("X,Y,Z,count,crc"), 0);
					const bool bSaved = FFileHelper::SaveStringArrayToFile(ChunkLines, *Path);
					UE_LOG(LogVoxelScatter, Display, TEXT("voxel.Scatter.Digest [%s]: %d chunk line(s) %s %s"), *World->GetName(),
						ChunkLines.Num() - 1, bSaved ? TEXT("written to") : TEXT("NOT written to"), *FPaths::ConvertRelativePathToFull(Path));
				}
			}
			if (Managers == 0)
			{
				UE_LOG(LogVoxelScatter, Display, TEXT("voxel.Scatter.Digest: no scatter manager in a game world"));
			}
		}));

	/**
	 * A planet chunk's density for ExtractSurfacePointsByRay's radial rays (world modes review G31), on the voxel lattice
	 * 0..ChunkSize on each axis: its own voxels, and past its +X, +Y and +Z faces (index ChunkSize) the voxel the planet
	 * generator makes there with the neighbour's edit (FScatterExtractionContext::GetVoxelOutsideChunk), computed the
	 * first time a ray needs it. Cell (I, J, K) spans voxels I..I+1 on each axis; the chunk owns cells 0..ChunkSize-1, so
	 * its last cell on each axis reaches one voxel into the neighbour.
	 */
	struct FPlanetRayField
	{
		FPlanetRayField(const TArray<FVoxelData>& InVoxels, int32 InChunkSize, const FVector& InOrigin, float InVoxelSize,
			const FScatterExtractionContext& InContext)
			: Voxels(InVoxels)
			, Context(InContext)
			, Origin(InOrigin)
			, VoxelSize(InVoxelSize)
			, CS(InChunkSize)
			, Side(InChunkSize + 1)
		{
			Shell.Init(0xFFFF, Side * Side * Side);
			CellState.Init(0, CS * CS * CS);
		}

		bool IsOwn(int32 X, int32 Y, int32 Z) const { return X < CS && Y < CS && Z < CS; }

		const FVoxelData& Own(int32 X, int32 Y, int32 Z) const { return Voxels[X + Y * CS + Z * CS * CS]; }

		/** The density at a lattice voxel (each coordinate in 0..ChunkSize). */
		uint8 Density(int32 X, int32 Y, int32 Z)
		{
			if (IsOwn(X, Y, Z))
			{
				return Own(X, Y, Z).Density;
			}
			uint16& Cached = Shell[X + Y * Side + Z * Side * Side];
			if (Cached == 0xFFFF)
			{
				const FIntVector Local(X, Y, Z);
				FVoxelData Given;
				Cached = (Context.OutsideVoxelSource && Context.OutsideVoxelSource(Local, Given))
					? Given.Density
					: Context.GetVoxelOutsideChunk(Local, Origin + FVector(Local) * VoxelSize, VoxelSize).Density;
			}
			return static_cast<uint8>(Cached);
		}

		/** Whether a cell's corners are all air (1), all solid (2) or both (3). */
		uint8 GetCellState(const FIntVector& C)
		{
			uint8& State = CellState[C.X + C.Y * CS + C.Z * CS * CS];
			if (State == 0)
			{
				bool bAir = false;
				bool bSolid = false;
				for (int32 I = 0; I < 8; ++I)
				{
					const bool bCornerSolid = Density(C.X + (I & 1), C.Y + ((I >> 1) & 1), C.Z + ((I >> 2) & 1)) >= VOXEL_SURFACE_THRESHOLD;
					bSolid |= bCornerSolid;
					bAir |= !bCornerSolid;
				}
				State = (bAir ? 1 : 0) | (bSolid ? 2 : 0);
			}
			return State;
		}

		/**
		 * Where in a cell a point lies, 0..1 on each axis, exactly 0 or 1 within 1e-7 of a face: both chunks sharing a face
		 * then read a point on it from the same four voxels (a voxel at the threshold on a face otherwise read 126.9999 from
		 * one side and 127 from the other, and neither kept the crossing).
		 */
		static FVector CellFraction(const FVector& Local, const FIntVector& C)
		{
			FVector F = Local - FVector(C);
			for (int32 A = 0; A < 3; ++A)
			{
				F[A] = F[A] < 1e-7 ? 0.0 : (F[A] > 1.0 - 1e-7 ? 1.0 : F[A]);
			}
			return F;
		}

		/** A cell's corner densities, corner I at (I & 1, (I >> 1) & 1, (I >> 2) & 1). */
		void GetCorners(const FIntVector& C, double V[8])
		{
			for (int32 I = 0; I < 8; ++I)
			{
				V[I] = Density(C.X + (I & 1), C.Y + ((I >> 1) & 1), C.Z + ((I >> 2) & 1));
			}
		}

		/**
		 * The trilinear density at a point of a cell, and optionally its gradient (per voxel).
		 * @param V The cell's corner densities (GetCorners)
		 * @param F Where in it, 0..1 on each axis
		 * @param OutGradient Receives the gradient (density per voxel) when not null
		 * @return The density there
		 */
		static double Sample(const double V[8], const FVector& F, FVector* OutGradient = nullptr)
		{
			// Along X on the cell's four X edges, then Y, then Z.
			const double C00 = V[0] + (V[1] - V[0]) * F.X;
			const double C10 = V[2] + (V[3] - V[2]) * F.X;
			const double C01 = V[4] + (V[5] - V[4]) * F.X;
			const double C11 = V[6] + (V[7] - V[6]) * F.X;
			const double C0 = C00 + (C10 - C00) * F.Y;
			const double C1 = C01 + (C11 - C01) * F.Y;
			if (OutGradient)
			{
				const double DX0 = (V[1] - V[0]) + ((V[3] - V[2]) - (V[1] - V[0])) * F.Y;
				const double DX1 = (V[5] - V[4]) + ((V[7] - V[6]) - (V[5] - V[4])) * F.Y;
				*OutGradient = FVector(DX0 + (DX1 - DX0) * F.Z, (C10 - C00) + ((C11 - C01) - (C10 - C00)) * F.Z, C1 - C0);
			}
			return C0 + (C1 - C0) * F.Z;
		}

		/**
		 * The cell's own corner nearest a point of it that is solid (bSolid) or air: where a crossing takes its material,
		 * biome and flags. A cell always has its own corner (0, 0, 0); when no own corner has the wanted state, the nearest
		 * own corner.
		 */
		const FVoxelData& NearestOwnCorner(const FIntVector& C, const FVector& F, bool bSolid) const
		{
			int32 Best = 0;
			double BestDistance = TNumericLimits<double>::Max();
			int32 Fallback = 0;
			double FallbackDistance = TNumericLimits<double>::Max();
			for (int32 I = 0; I < 8; ++I)
			{
				const FIntVector Corner(C.X + (I & 1), C.Y + ((I >> 1) & 1), C.Z + ((I >> 2) & 1));
				if (!IsOwn(Corner.X, Corner.Y, Corner.Z))
				{
					continue;
				}
				const double Distance = (FVector(I & 1, (I >> 1) & 1, (I >> 2) & 1) - F).SizeSquared();
				if (Distance < FallbackDistance)
				{
					FallbackDistance = Distance;
					Fallback = I;
				}
				if (Own(Corner.X, Corner.Y, Corner.Z).IsSolid() == bSolid && Distance < BestDistance)
				{
					BestDistance = Distance;
					Best = I;
				}
			}
			const int32 I = BestDistance < TNumericLimits<double>::Max() ? Best : Fallback;
			return Own(C.X + (I & 1), C.Y + ((I >> 1) & 1), C.Z + ((I >> 2) & 1));
		}

		const TArray<FVoxelData>& Voxels;
		const FScatterExtractionContext& Context;
		FVector Origin;
		float VoxelSize;
		int32 CS;
		int32 Side;
		/** The shell's densities once computed ((ChunkSize + 1)^3, 0xFFFF = not yet). */
		TArray<uint16> Shell;
		/** Per cell: 0 not yet classified, else GetCellState's answer. */
		TArray<uint8> CellState;
	};
}

UVoxelScatterManager::UVoxelScatterManager()
{
}

// ==================== Lifecycle ====================

void UVoxelScatterManager::Initialize(UVoxelWorldConfiguration* Config, UWorld* World)
{
	if (bIsInitialized)
	{
		UE_LOG(LogVoxelScatter, Warning, TEXT("VoxelScatterManager::Initialize called when already initialized"));
		Shutdown();
	}

	if (!Config)
	{
		UE_LOG(LogVoxelScatter, Error, TEXT("VoxelScatterManager::Initialize called with null configuration"));
		return;
	}

	if (!World)
	{
		UE_LOG(LogVoxelScatter, Error, TEXT("VoxelScatterManager::Initialize called with null world"));
		return;
	}

	Configuration = Config;
	CachedWorld = World;

	// Apply configuration
	ScatterRadius = Config->ScatterRadius;
	bDebugVisualization = Config->bScatterDebugVisualization;
	WorldSeed = Config->WorldSeed;

	// Clear any existing data
	SurfaceCache.Empty();
	SpawnCache.Empty();
	ScatterDefinitions.Empty();

	// Load scatter definitions from configuration asset if available
	bool bLoadedFromConfig = false;
	if (Config->ScatterConfiguration)
	{
		UVoxelScatterConfiguration* ScatterConfig = Config->ScatterConfiguration;
		if (ScatterConfig)
		{
			// Copy definitions from configuration
			for (const FScatterDefinition& Def : ScatterConfig->ScatterDefinitions)
			{
				ScatterDefinitions.Add(Def);
			}

			// Apply surface point spacing from config
			if (ScatterConfig->SurfacePointSpacing > 0.0f)
			{
				SurfacePointSpacing = ScatterConfig->SurfacePointSpacing;
			}

			bLoadedFromConfig = ScatterDefinitions.Num() > 0;
			if (bLoadedFromConfig)
			{
				UE_LOG(LogVoxelScatter, Log, TEXT("Loaded %d scatter definitions from configuration asset: %s"),
					ScatterDefinitions.Num(), *ScatterConfig->GetPathName());
			}
			else
			{
				UE_LOG(LogVoxelScatter, Log, TEXT("ScatterConfiguration %s holds no definitions: no scatter"), *ScatterConfig->GetPathName());
			}
		}
	}
	else
	{
		UE_LOG(LogVoxelScatter, Warning, TEXT("No ScatterConfiguration asset assigned in VoxelWorldConfiguration"));
	}

	// Auto-override scatter definitions for cubic mode
	if (Config->bAutoCubicScatterDefaults && Config->MeshingMode == EMeshingMode::Cubic)
	{
		for (FScatterDefinition& Def : ScatterDefinitions)
		{
			if (Def.PlacementMode != EScatterPlacementMode::BlockFaceSnap)
			{
				Def.PlacementMode = EScatterPlacementMode::BlockFaceSnap;
			}
		}
		UE_LOG(LogVoxelScatter, Log, TEXT("Cubic mode: auto-overriding %d definitions to BlockFaceSnap"), ScatterDefinitions.Num());
	}

	// Async scatter configuration
	MaxAsyncScatterTasks = FMath::Clamp(Config->MaxAsyncScatterTasks, 1, 4);

	// Reset statistics
	TotalChunksProcessed = 0;
	TotalExtractionsLaunched = 0;
	TotalRegenerations = 0;
	TotalDeferredFirstExtractions = 0;
	ChunksAwaitingFirstExtraction.Empty();
	TotalSurfacePointsExtracted = 0;
	TotalSpawnPointsGenerated = 0;

	// Create scatter renderer for HISM instance management, its HISMs per render region (WM-BP7; every world mode since WM-MP5)
	ScatterRenderer = NewObject<UVoxelScatterRenderer>(this);
	// Its HISMs per region (world modes review G42): every change to a HISM costs all its instances (UVoxelScatterRenderer's
	// batched writes), so scatter is kept in regions a quarter of the render regions' edge; precision only gains.
	FVoxelRenderRegions ScatterRegions = FVoxelRenderRegions::FromConfig(*Config);
	const int32 ScatterRegionSetting = CVarVoxelScatterRegionChunks.GetValueOnGameThread();
	if (ScatterRegionSetting > 0)
	{
		ScatterRegions.RegionChunks = ScatterRegionSetting;
	}
	else if (ScatterRegionSetting < 0)
	{
		ScatterRegions.RegionChunks = DefaultScatterRegionChunks;
	}
	ScatterRenderer->SetRenderRegions(ScatterRegions);
	UE_LOG(LogVoxelScatter, Log, TEXT("Scatter HISM regions: %d chunks along an edge (0 = one region)"), ScatterRegions.RegionChunks);
	ScatterRenderer->Initialize(this, World);

	bIsInitialized = true;

	// Register with the world's exclusion subsystem, which replays any volumes registered before
	// this manager existed (e.g. claims placed during world startup) — initialization order between
	// volume producers and the voxel world never matters.
	if (UVoxelScatterExclusionSubsystem* ExclusionSubsystem = World->GetSubsystem<UVoxelScatterExclusionSubsystem>())
	{
		ExclusionSubsystem->NotifyManagerInitialized(this);
	}

	ValidateScatterDistances();

	UE_LOG(LogVoxelScatter, Log, TEXT("VoxelScatterManager initialized (Radius=%.0f, PointSpacing=%.0f, Definitions=%d)"),
		ScatterRadius, SurfacePointSpacing, ScatterDefinitions.Num());
}

void UVoxelScatterManager::ValidateScatterDistances() const
{
	// Two distance relationships have to hold per definition or instances pop instead of fading.
	// Neither is enforced anywhere, both are freely editable per definition in the config asset,
	// and a violation is invisible in the editor — it only shows up as popping while moving.
	for (const FScatterDefinition& Def : ScatterDefinitions)
	{
		if (!Def.bEnabled)
		{
			continue;
		}

		// A definition with no explicit spawn distance falls back to the manager-wide radius.
		const float EffectiveSpawnDistance = GetEffectiveSpawnDistance(Def);

		// 1. HISM fades instances between LODStartDistance and CullDistance. Collapsing that
		//    range to zero leaves no fade at all, so instances vanish/appear in a hard step.
		if (Def.LODStartDistance >= Def.CullDistance)
		{
			UE_LOG(LogVoxelScatter, Warning,
				TEXT("Scatter '%s': LODStartDistance (%.0f) >= CullDistance (%.0f) — no fade range, ")
				TEXT("instances will pop at the cull edge. Set LODStartDistance below CullDistance."),
				*Def.Name, Def.LODStartDistance, Def.CullDistance);
		}

		// 2. Instances only exist out to the spawn distance. If they are still being drawn at
		//    that radius, the spawn ring itself is visible and any streaming latency shows up
		//    directly at the visible edge, with no already-generated margin hiding it.
		if (Def.CullDistance >= EffectiveSpawnDistance)
		{
			UE_LOG(LogVoxelScatter, Warning,
				TEXT("Scatter '%s': CullDistance (%.0f) >= effective SpawnDistance (%.0f) — instances are ")
				TEXT("drawn out to the radius where they stop being generated, so the spawn ring is visible. ")
				TEXT("Keep CullDistance below SpawnDistance."),
				*Def.Name, Def.CullDistance, EffectiveSpawnDistance);
		}
	}
}

void UVoxelScatterManager::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	// Detach from the exclusion subsystem while the world reference is still valid
	if (CachedWorld)
	{
		if (UVoxelScatterExclusionSubsystem* ExclusionSubsystem = CachedWorld->GetSubsystem<UVoxelScatterExclusionSubsystem>())
		{
			ExclusionSubsystem->NotifyManagerShutdown(this);
		}
	}

	// Shutdown scatter renderer first
	if (ScatterRenderer)
	{
		ScatterRenderer->Shutdown();
		ScatterRenderer = nullptr;
	}

	// Drain completed async scatter queue
	{
		FAsyncScatterResult DiscardedResult;
		while (CompletedScatterQueue.Dequeue(DiscardedResult)) {}
	}
	AsyncScatterInProgress.Empty();

	// Drain distance stream queue
	{
		FDistanceStreamResult DiscardedResult;
		while (DistanceStreamQueue.Dequeue(DiscardedResult)) {}
	}
	DistanceStreamInProgress.Empty();

	// Clear pending queue and deferred upgrades
	PendingGenerationQueue.Empty();
	PendingQueueSet.Empty();
	DeferredSupplementalPasses.Empty();

	// Clear all cached data
	SurfaceCache.Empty();
	SpawnCache.Empty();
	CompletedScatterTypes.Empty();
	ScatterDefinitions.Empty();
	ChunksAwaitingFirstExtraction.Empty();
	ClearedVolumesPerChunk.Empty();
	ExclusionVolumes.Empty();
	PendingExclusionRegrow.Empty();

	Configuration = nullptr;
	CachedWorld = nullptr;
	bIsInitialized = false;

	UE_LOG(LogVoxelScatter, Log, TEXT("VoxelScatterManager shutdown. Stats: Chunks=%lld, SurfacePoints=%lld, SpawnPoints=%lld"),
		TotalChunksProcessed, TotalSurfacePointsExtracted, TotalSpawnPointsGenerated);
}

// ==================== Per-Frame Update ====================

void UVoxelScatterManager::Update(const FVector& ViewerPosition, float DeltaTime)
{
	if (!bIsInitialized)
	{
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelScatter_Update);
	LastViewerPosition = ViewerPosition;
	FUpdateTimings Timings;
	const int64 ProcessedBefore = TotalChunksProcessed;
	const int64 LaunchedBefore = TotalExtractionsLaunched;
	double StepStart = FPlatformTime::Seconds();
	auto EndStep = [&StepStart](float& OutMs)
	{
		const double Now = FPlatformTime::Seconds();
		OutMs = static_cast<float>((Now - StepStart) * 1000.0);
		StepStart = Now;
	};

	// Process completed async scatter results (game thread only: cache + HISM update)
	ProcessCompletedAsyncScatter();
	EndStep(Timings.ApplyMs);

	// Launch regrow placements for chunks uncovered by an unregistered exclusion volume
	// (throttled; runs right after completions so freed async slots are usable this tick)
	ProcessPendingExclusionRegrow();
	EndStep(Timings.RegrowMs);

	// Launch new async scatter tasks from pending queue (throttled)
	ProcessPendingGenerationQueue();
	EndStep(Timings.LaunchMs);

	// Distance-based scatter streaming — fully decoupled from chunk generation.
	// Uses its own async pipeline (DistanceStreamQueue / DistanceStreamInProgress).
	// Process completed results every frame; launch new tasks periodically.
	ProcessCompletedDistanceStream();

	TimeSinceLastDistanceCheck += DeltaTime;
	if (TimeSinceLastDistanceCheck >= DistanceStreamingInterval)
	{
		TimeSinceLastDistanceCheck = 0.0f;
		PerformDistanceSpawn();
	}
	EndStep(Timings.DistanceMs);

	// Tick the scatter renderer to flush pending HISM rebuilds
	// Rebuilds are deferred while viewer is moving to prevent flicker
	if (ScatterRenderer && ScatterRenderer->IsInitialized())
	{
		ScatterRenderer->Tick(ViewerPosition, DeltaTime);
	}
	EndStep(Timings.FlushMs);
	Timings.Applied = static_cast<int32>(TotalChunksProcessed - ProcessedBefore);
	Timings.Launched = static_cast<int32>(TotalExtractionsLaunched - LaunchedBefore);
	LastUpdateTimings = Timings;

	// Debug visualization is drawn separately via DrawDebugVisualization()
}

// ==================== Scatter Definitions ====================

void UVoxelScatterManager::AddScatterDefinition(const FScatterDefinition& Definition)
{
	// Check for duplicate ID
	for (int32 i = 0; i < ScatterDefinitions.Num(); ++i)
	{
		if (ScatterDefinitions[i].ScatterID == Definition.ScatterID)
		{
			// Replace existing
			ScatterDefinitions[i] = Definition;
			UE_LOG(LogVoxelScatter, Log, TEXT("Replaced scatter definition: %s (ID=%d)"),
				*Definition.Name, Definition.ScatterID);
			return;
		}
	}

	// Add new
	ScatterDefinitions.Add(Definition);
	UE_LOG(LogVoxelScatter, Log, TEXT("Added scatter definition: %s (ID=%d)"),
		*Definition.Name, Definition.ScatterID);
}

const FScatterDefinition* UVoxelScatterManager::GetScatterDefinition(int32 ScatterID) const
{
	for (const FScatterDefinition& Def : ScatterDefinitions)
	{
		if (Def.ScatterID == ScatterID)
		{
			return &Def;
		}
	}
	return nullptr;
}

// ==================== Scatter Data Access ====================

const FChunkScatterData* UVoxelScatterManager::GetChunkScatterData(const FIntVector& ChunkCoord) const
{
	return SpawnCache.Find(ChunkCoord);
}

const FChunkSurfaceData* UVoxelScatterManager::GetChunkSurfaceData(const FIntVector& ChunkCoord) const
{
	return SurfaceCache.Find(ChunkCoord);
}

const UVoxelScatterRenderer* UVoxelScatterManager::GetScatterRenderer() const
{
	return ScatterRenderer;
}

bool UVoxelScatterManager::HasScatterData(const FIntVector& ChunkCoord) const
{
	const FChunkScatterData* Data = SpawnCache.Find(ChunkCoord);
	return Data && Data->bIsValid;
}

// ==================== Mesh Data Callback ====================

void UVoxelScatterManager::HandOffChunkVoxels(const FIntVector& ChunkCoord, const TArray<FVoxelData>& VoxelData,
	int32 ChunkSize, float VoxelSize)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelScatter_OnChunkMeshDataReady);
	if (!bIsInitialized || !Configuration)
	{
		return;
	}

	// Extraction reads the chunk's voxels (always full resolution, LOD-independent)
	if (VoxelData.Num() != ChunkSize * ChunkSize * ChunkSize)
	{
		UE_LOG(LogVoxelScatter, Warning, TEXT("Chunk (%d,%d,%d): No voxel data for scatter extraction (expected %d, got %d)"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, ChunkSize * ChunkSize * ChunkSize, VoxelData.Num());
		return;
	}

	const FVector ChunkCenter = GetChunkWorldOrigin(ChunkCoord) +
		FVector(Configuration->GetChunkWorldSize() * 0.5f);
	const float ChunkDistance = FVector::Dist(ChunkCenter, LastViewerPosition);

	// Get the set of scatter types already generated for this chunk (never regenerated)
	const TSet<int32>* CompletedTypes = CompletedScatterTypes.Find(ChunkCoord);

	// Tree mode filtering
	const EVoxelTreeMode TreeMode = Configuration->TreeMode;
	const float VoxelTreeMaxDist = Configuration->VoxelTreeMaxDistance;

	// Build definitions to generate:
	//  - All enabled defs within their SpawnDistance/ScatterRadius range
	//  - Exclude types already completed for this chunk
	//  - No LOD-based filtering: voxel data is always full resolution
	//  - Tree mode filtering: VoxelInjection defs handled based on TreeMode
	TArray<FScatterDefinition> DefsToGenerate;
	for (const FScatterDefinition& Def : ScatterDefinitions)
	{
		// The one eligibility rule (code review CR2, SCAT-17): enabled, the tree-mode rule, within its spawn distance.
		if (!IsDefinitionEligible(Def, ChunkDistance))
		{
			if (Def.bEnabled && Def.MeshType == EScatterMeshType::VoxelInjection && Configuration->TreeMode == EVoxelTreeMode::VoxelData)
			{
				UE_LOG(LogVoxelScatter, Warning, TEXT("  Scatter %d (%s): SKIPPED — VoxelInjection type with TreeMode=VoxelData (trees only in terrain, no HISM). Set TreeMode to HISM or Both to see trees as HISM instances."),
					Def.ScatterID, *Def.Name);
			}
			else
			{
				UE_LOG(LogVoxelScatter, Verbose, TEXT("  Scatter %d (%s): SKIPPED — disabled, a VoxelInjection type within the voxel trees' reach, or beyond its spawn distance %.0f at %.0f"),
					Def.ScatterID, *Def.Name, GetEffectiveSpawnDistance(Def), ChunkDistance);
			}
			continue;
		}

		// Skip types already generated for this chunk
		if (CompletedTypes && CompletedTypes->Contains(Def.ScatterID))
		{
			UE_LOG(LogVoxelScatter, Verbose, TEXT("  Scatter %d (%s): SKIPPED — already completed for this chunk"),
				Def.ScatterID, *Def.Name);
			continue;
		}

		DefsToGenerate.Add(Def);
	}

	if (DefsToGenerate.Num() == 0)
	{
		// Every type out of range and nothing cached: the chunk arrived at the window's front (G79). The distance pass hands
		// it off again once it comes within the scatter horizon, so its scatter does not wait for its next mesh.
		if (CVarVoxelScatterFirstExtractionAtHorizon.GetValueOnGameThread() != 0 && !SurfaceCache.Contains(ChunkCoord)
			&& !SpawnCache.Contains(ChunkCoord) && !AsyncScatterInProgress.Contains(ChunkCoord) && !PendingQueueSet.Contains(ChunkCoord)
			&& ChunkDistance > GetScatterHorizon())
		{
			ChunksAwaitingFirstExtraction.Add(ChunkCoord);
			UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): every type out of range at %.0f, awaiting its first extraction"),
				ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, ChunkDistance);
		}
		return;
	}
	ChunksAwaitingFirstExtraction.Remove(ChunkCoord);

	// Build pending request
	auto MakePendingRequest = [&]() -> FPendingScatterGeneration
	{
		FPendingScatterGeneration Req;
		Req.ChunkCoord = ChunkCoord;
		Req.DistanceToViewer = ChunkDistance;
		Req.CapturedDefinitions = DefsToGenerate;
		Req.ChunkVoxelData = VoxelData;
		Req.ChunkSize = ChunkSize;
		Req.VoxelSize = VoxelSize;
		return Req;
	};

	// If already in pending queue: merge new definitions into pending entry
	if (PendingQueueSet.Contains(ChunkCoord))
	{
		for (FPendingScatterGeneration& Pending : PendingGenerationQueue)
		{
			if (Pending.ChunkCoord == ChunkCoord)
			{
				// Build set of already-pending type IDs
				TSet<int32> PendingTypeIDs;
				for (const FScatterDefinition& Def : Pending.CapturedDefinitions)
				{
					PendingTypeIDs.Add(Def.ScatterID);
				}

				// Add any new definitions not already pending
				for (const FScatterDefinition& Def : DefsToGenerate)
				{
					if (!PendingTypeIDs.Contains(Def.ScatterID))
					{
						Pending.CapturedDefinitions.Add(Def);
					}
				}
				break;
			}
		}
		return;
	}

	// If async scatter is in-flight: defer as supplemental pass
	if (AsyncScatterInProgress.Contains(ChunkCoord))
	{
		DeferredSupplementalPasses.Add(ChunkCoord, MakePendingRequest());
		UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): Deferred supplemental scatter (%d defs) — async in-flight"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, DefsToGenerate.Num());
		return;
	}

	// Queue new scatter generation
	FPendingScatterGeneration PendingRequest = MakePendingRequest();
	int32 InsertIndex = Algo::LowerBound(PendingGenerationQueue, PendingRequest);
	PendingGenerationQueue.Insert(MoveTemp(PendingRequest), InsertIndex);
	PendingQueueSet.Add(ChunkCoord);

	UE_LOG(LogVoxelScatter, Verbose, TEXT("Queued scatter for chunk (%d,%d,%d) %d defs, dist %.0f (queue: %d)"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, DefsToGenerate.Num(),
		ChunkDistance, PendingGenerationQueue.Num());
}

void UVoxelScatterManager::OnChunkUnloaded(const FIntVector& ChunkCoord)
{
	// Remove from pending queue if present
	if (PendingQueueSet.Remove(ChunkCoord) > 0)
	{
		PendingGenerationQueue.RemoveAll([&ChunkCoord](const FPendingScatterGeneration& Request)
		{
			return Request.ChunkCoord == ChunkCoord;
		});
	}

	// Remove from async in-progress tracking (stale result will be discarded on arrival)
	AsyncScatterInProgress.Remove(ChunkCoord);

	// Remove from distance stream tracking (stale result will be discarded on arrival)
	DistanceStreamInProgress.Remove(ChunkCoord);

	// Remove deferred supplemental passes
	DeferredSupplementalPasses.Remove(ChunkCoord);

	// Clear cleared volumes and completed type tracking - allow full regeneration on reload
	ClearedVolumesPerChunk.Remove(ChunkCoord);
	CompletedScatterTypes.Remove(ChunkCoord);

	// A queued exclusion regrow is obsolete too: the reload's first pass reads the current volume set
	PendingExclusionRegrow.Remove(ChunkCoord);

	// An unloaded chunk waits for nothing (G79)
	ChunksAwaitingFirstExtraction.Remove(ChunkCoord);

	RemoveChunkScatter(ChunkCoord);
}

void UVoxelScatterManager::RegenerateChunkScatter(const FIntVector& ChunkCoord)
{
	if (!bIsInitialized)
	{
		return;
	}
	++TotalRegenerations;

	// Remove from pending queue if present
	if (PendingQueueSet.Remove(ChunkCoord) > 0)
	{
		PendingGenerationQueue.RemoveAll([&ChunkCoord](const FPendingScatterGeneration& Request)
		{
			return Request.ChunkCoord == ChunkCoord;
		});
	}

	// Remove from async in-progress (stale result will be discarded on arrival)
	AsyncScatterInProgress.Remove(ChunkCoord);

	// Remove deferred supplemental passes
	DeferredSupplementalPasses.Remove(ChunkCoord);

	// Clear cleared volumes and completed types so scatter can fully regenerate
	ClearedVolumesPerChunk.Remove(ChunkCoord);
	CompletedScatterTypes.Remove(ChunkCoord);

	// The hand-off that follows a regeneration decides again whether the chunk waits (G79)
	ChunksAwaitingFirstExtraction.Remove(ChunkCoord);

	// Drop cached surface/scatter data so the re-extraction is treated as a fresh first pass (a full
	// replace, not a supplemental append). Deliberately keep the rendered HISM instances: the completed
	// re-extraction replaces this chunk's instances per-type via the smooth UpdateChunkInstances path
	// (release + budgeted re-add), so an edit reclassifies scatter with no visible gap — unlike
	// RemoveChunkScatter (used on unload), which blinks the chunk's scatter out until async
	// re-extraction repopulates it. Stale in-flight results are dropped by the completion paths'
	// AsyncScatterInProgress / cache checks (both cleared above).
	SurfaceCache.Remove(ChunkCoord);
	SpawnCache.Remove(ChunkCoord);

	// Regeneration requires new mesh data - the caller should provide it via HandOffChunkVoxels
	UE_LOG(LogVoxelScatter, Verbose, TEXT("Regenerate scatter requested for chunk (%d,%d,%d) - awaiting new mesh data"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
}

void UVoxelScatterManager::ClearScatterInRadius(const FVector& WorldPosition, float Radius)
{
	if (!bIsInitialized || !Configuration)
	{
		return;
	}

	// Find which chunks are affected by this edit
	const float RadiusSq = Radius * Radius;
	int32 TotalRemoved = 0;

	ForEachChunkInBounds(FBox(WorldPosition - FVector(Radius), WorldPosition + FVector(Radius)), [&](const FIntVector& ChunkCoord)
	{
		// Add cleared volume to this chunk
		TArray<FClearedScatterVolume>& ClearedVolumes = ClearedVolumesPerChunk.FindOrAdd(ChunkCoord);
		ClearedVolumes.Add(FClearedScatterVolume(WorldPosition, Radius));

		// Scrub cached surface points inside the radius so distance streaming
		// can't resurrect scatter here from pre-edit surface data (placement
		// never consults cleared volumes; the cache must not contain them)
		if (FChunkSurfaceData* SurfaceData = SurfaceCache.Find(ChunkCoord))
		{
			SurfaceData->SurfacePoints.RemoveAll([&WorldPosition, RadiusSq](const FVoxelSurfacePoint& Point)
			{
				return FVector::DistSquared(Point.Position, WorldPosition) <= RadiusSq;
			});
		}

		// Remove spawn points that fall within the radius
		TSet<int32> AffectedTypes;
		FChunkScatterData* ScatterData = SpawnCache.Find(ChunkCoord);
		if (ScatterData && ScatterData->bIsValid)
		{
			for (int32 i = ScatterData->SpawnPoints.Num() - 1; i >= 0; --i)
			{
				const FScatterSpawnPoint& Point = ScatterData->SpawnPoints[i];
				if (FVector::DistSquared(Point.Position, WorldPosition) <= RadiusSq)
				{
					AffectedTypes.Add(Point.ScatterTypeID);
					ScatterData->SpawnPoints.RemoveAt(i);
					++TotalRemoved;
				}
			}
		}

		// Replace this chunk's instances for the affected types with the surviving
		// points — immediate release + re-add via the per-frame add budget, which
		// runs every Tick. A full-type rebuild would wait for a stationary viewer
		// AND an idle generation pipeline, leaving cleared scatter visible
		// (floating/clipped) for as long as the player keeps moving.
		if (ScatterData)
		{
			ReplaceChunkTypeInstances(ChunkCoord, *ScatterData, AffectedTypes);
		}

		// Also remove from pending queue to prevent stale data
		if (PendingQueueSet.Contains(ChunkCoord))
		{
			PendingQueueSet.Remove(ChunkCoord);
			PendingGenerationQueue.RemoveAll([&ChunkCoord](const FPendingScatterGeneration& Request)
			{
				return Request.ChunkCoord == ChunkCoord;
			});
		}
	});

	if (TotalRemoved > 0)
	{
		UE_LOG(LogVoxelScatter, Verbose, TEXT("Cleared %d scatter instances at (%.0f, %.0f, %.0f) radius %.0f"),
			TotalRemoved, WorldPosition.X, WorldPosition.Y, WorldPosition.Z, Radius);
	}
}

bool UVoxelScatterManager::IsPointInClearedVolume(const FIntVector& ChunkCoord, const FVector& WorldPosition) const
{
	const TArray<FClearedScatterVolume>* ClearedVolumes = ClearedVolumesPerChunk.Find(ChunkCoord);
	if (!ClearedVolumes)
	{
		return false;
	}

	for (const FClearedScatterVolume& Volume : *ClearedVolumes)
	{
		if (Volume.ContainsPoint(WorldPosition))
		{
			return true;
		}
	}

	return false;
}

// ==================== Exclusion Volumes ====================

// ==================== Harvest ====================

FScatterExclusionVolume UVoxelScatterManager::MakeHarvestExclusionVolume(const FVector& InstanceBase, float TrunkRadius, float Height,
	const FVector& Up)
{
	FScatterExclusionVolume Volume;
	// Deterministic identity: the same base (to the centimetre) on every machine yields the same Id.
	const FIntVector Key(FMath::RoundToInt(InstanceBase.X), FMath::RoundToInt(InstanceBase.Y), FMath::RoundToInt(InstanceBase.Z));
	const uint32 A = GetTypeHash(Key);
	const uint32 B = HashCombine(static_cast<uint32>(Key.X), static_cast<uint32>(Key.Y));
	const uint32 C = HashCombine(static_cast<uint32>(Key.Z), 0x48A7Fu);
	Volume.Id = FGuid(0x48415256u /*HARV*/, A, B, C);
	Volume.HalfExtent = FVector(FMath::Max(TrunkRadius, 1.0f), FMath::Max(TrunkRadius, 1.0f), FMath::Max(Height, 1.0f) * 0.5f);
	if (Up == FVector::UpVector)
	{
		Volume.Frame = FTransform(InstanceBase + FVector(0.0f, 0.0f, Volume.HalfExtent.Z - 50.0f));
	}
	else
	{
		// On a planet the box stands along local up (spherical SP-G1).
		Volume.Frame = FTransform(FQuat::FindBetweenNormals(FVector::UpVector, Up), InstanceBase + Up * (Volume.HalfExtent.Z - 50.0f));
	}
	return Volume;
}

bool UVoxelScatterManager::HarvestScatterNear(const FVector& WorldLocation, float Radius, bool bRemove, FScatterHarvestResult& OutResult)
{
	OutResult = FScatterHarvestResult();
	if (!ScatterRenderer || Radius <= 0.0f)
	{
		return false;
	}
	float BestDist = Radius;
	for (const FScatterDefinition& Def : ScatterDefinitions)
	{
		if (Def.HarvestCategory.IsNone())
		{
			continue;
		}
		int32 Index = INDEX_NONE;
		FTransform Xf;
		if (!ScatterRenderer->FindNearestInstance(Def.ScatterID, WorldLocation, BestDist, Index, Xf))
		{
			continue;
		}
		const float Dist = FVector::Dist(Xf.GetLocation(), WorldLocation);
		if (Dist <= BestDist)
		{
			BestDist = Dist;
			OutResult.bValid = true;
			OutResult.ScatterID = Def.ScatterID;
			OutResult.Name = Def.Name;
			OutResult.HarvestCategory = Def.HarvestCategory;
			OutResult.InstanceTransform = Xf;
			OutResult.Distance = Dist;
		}
	}
	if (OutResult.bValid && bRemove)
	{
		// Through the world's exclusion subsystem when there is one: cached there, replayed to managers
		// created later, and exported by the save system. Standalone managers (tests) register directly.
		const FVector Base = OutResult.InstanceTransform.GetLocation();
		const FScatterExclusionVolume Volume = MakeHarvestExclusionVolume(Base, 60.0f, 400.0f,
			WorldMode ? WorldMode->GetUpVector(Base) : FVector::UpVector);
		if (UVoxelScatterExclusionSubsystem* Exclusions = UVoxelScatterExclusionSubsystem::Get(this))
		{
			Exclusions->RegisterVolume(Volume);
		}
		else
		{
			RegisterScatterExclusionVolume(Volume);
		}
		UE_LOG(LogVoxelScatter, Log, TEXT("Harvested scatter '%s' (%s) at %s"), *OutResult.Name, *OutResult.HarvestCategory.ToString(),
			*OutResult.InstanceTransform.GetLocation().ToCompactString());
	}
	return OutResult.bValid;
}

bool UVoxelScatterManager::RegisterScatterExclusionVolume(const FScatterExclusionVolume& Volume)
{
	if (!Volume.IsUsable())
	{
		UE_LOG(LogVoxelScatter, Warning, TEXT("RegisterScatterExclusionVolume rejected: invalid Id or non-positive extent"));
		return false;
	}

	// Replacing a volume: the old footprint may extend beyond the new one, so queue it for regrow.
	// Regrow placement is filtered against the post-replace volume set at consumption, so any area
	// still covered stays bare and only the newly uncovered part regrows.
	if (const FScatterExclusionVolume* Existing = ExclusionVolumes.Find(Volume.Id))
	{
		QueueExclusionRegrowForBounds(Existing->GetWorldBounds());
	}

	ExclusionVolumes.Add(Volume.Id, Volume);

	// Clear existing foliage inside the volume right away via the smooth per-(chunk,type) replace
	// path. In-flight async results are filtered at consumption (ApplyClearedVolumesToResult), and
	// chunks that stream in later are born bare — so this immediate pass is the only catch-up needed.
	ClearSpawnPointsInExclusionVolume(Volume);

	UE_LOG(LogVoxelScatter, Log, TEXT("Registered scatter exclusion volume %s (half-extent %.0fx%.0fx%.0f at %s)"),
		*Volume.Id.ToString(), Volume.HalfExtent.X, Volume.HalfExtent.Y, Volume.HalfExtent.Z,
		*Volume.Frame.GetLocation().ToCompactString());
	return true;
}

bool UVoxelScatterManager::UnregisterScatterExclusionVolume(const FGuid& VolumeId)
{
	FScatterExclusionVolume Removed;
	if (!ExclusionVolumes.RemoveAndCopyValue(VolumeId, Removed))
	{
		return false;
	}

	// Regrow foliage where the volume used to be: overlapped chunks re-run placement from cached
	// surface data (throttled in Update). Deterministic chunk seeds reproduce the identical points
	// outside the removed volume, so the per-type replace is visually a no-op there.
	QueueExclusionRegrowForBounds(Removed.GetWorldBounds());

	UE_LOG(LogVoxelScatter, Log, TEXT("Unregistered scatter exclusion volume %s (%d chunk(s) queued to regrow)"),
		*VolumeId.ToString(), PendingExclusionRegrow.Num());
	return true;
}

bool UVoxelScatterManager::IsPointExcluded(const FVector& Position) const
{
	// Flat scan: exclusion volumes are few and long-lived (claim footprints), matching the
	// claim registry's own MVP storage judgment. Revisit with an AABB pre-filter if counts grow.
	for (const auto& Pair : ExclusionVolumes)
	{
		if (Pair.Value.ContainsPoint(Position))
		{
			return true;
		}
	}
	return false;
}

void UVoxelScatterManager::ClearSpawnPointsInExclusionVolume(const FScatterExclusionVolume& Volume)
{
	if (!bIsInitialized || !Configuration)
	{
		return;
	}

	int32 TotalRemoved = 0;

	ForEachChunkInBounds(Volume.GetWorldBounds(), [&](const FIntVector& ChunkCoord)
	{
		FChunkScatterData* ScatterData = SpawnCache.Find(ChunkCoord);
		if (!ScatterData || !ScatterData->bIsValid)
		{
			return;
		}

		// Remove spawn points inside the oriented box; note which types lost instances.
		// The surface cache is deliberately left whole — unregister regrows from it.
		TSet<int32> AffectedTypes;
		for (int32 i = ScatterData->SpawnPoints.Num() - 1; i >= 0; --i)
		{
			if (Volume.ContainsPoint(ScatterData->SpawnPoints[i].Position))
			{
				AffectedTypes.Add(ScatterData->SpawnPoints[i].ScatterTypeID);
				ScatterData->SpawnPoints.RemoveAt(i);
				++TotalRemoved;
			}
		}

		// Replace each affected type's instances with the survivors — immediate release +
		// budgeted re-add per (chunk,type), the same flash-free path player edits use.
		ReplaceChunkTypeInstances(ChunkCoord, *ScatterData, AffectedTypes);
	});

	if (TotalRemoved > 0)
	{
		UE_LOG(LogVoxelScatter, Verbose, TEXT("Exclusion volume %s cleared %d spawn point(s)"),
			*Volume.Id.ToString(), TotalRemoved);
	}
}

void UVoxelScatterManager::QueueExclusionRegrowForBounds(const FBox& Bounds)
{
	if (!bIsInitialized || !Configuration)
	{
		return;
	}

	ForEachChunkInBounds(Bounds, [&](const FIntVector& ChunkCoord)
	{
		// Only chunks with cached surface data can regrow in place. Anything else
		// (unloaded, awaiting re-mesh) regenerates naturally on its next fresh pass,
		// which consults the current volume set anyway.
		if (SurfaceCache.Contains(ChunkCoord))
		{
			PendingExclusionRegrow.Add(ChunkCoord);
		}
	});
}

void UVoxelScatterManager::ForEachChunkInBounds(const FBox& Bounds, TFunctionRef<void(const FIntVector&)> Visit) const
{
	// The chunks a world box touches, floored on the chunk grid (code review CR2, SCAT-18: the one loop).
	const float ChunkWorldSize = Configuration->GetChunkWorldSize();
	const FVector WorldOrigin = Configuration->WorldOrigin;
	const FIntVector MinChunk(
		FMath::FloorToInt((Bounds.Min.X - WorldOrigin.X) / ChunkWorldSize),
		FMath::FloorToInt((Bounds.Min.Y - WorldOrigin.Y) / ChunkWorldSize),
		FMath::FloorToInt((Bounds.Min.Z - WorldOrigin.Z) / ChunkWorldSize));
	const FIntVector MaxChunk(
		FMath::FloorToInt((Bounds.Max.X - WorldOrigin.X) / ChunkWorldSize),
		FMath::FloorToInt((Bounds.Max.Y - WorldOrigin.Y) / ChunkWorldSize),
		FMath::FloorToInt((Bounds.Max.Z - WorldOrigin.Z) / ChunkWorldSize));
	for (int32 CX = MinChunk.X; CX <= MaxChunk.X; ++CX)
	{
		for (int32 CY = MinChunk.Y; CY <= MaxChunk.Y; ++CY)
		{
			for (int32 CZ = MinChunk.Z; CZ <= MaxChunk.Z; ++CZ)
			{
				Visit(FIntVector(CX, CY, CZ));
			}
		}
	}
}

void UVoxelScatterManager::ReplaceChunkTypeInstances(const FIntVector& ChunkCoord, const FChunkScatterData& ScatterData, const TSet<int32>& AffectedTypes)
{
	// Each affected type's instances replaced with the survivors: immediate release + budgeted re-add per (chunk, type),
	// the flash-free path player edits use (code review CR2, SCAT-18: the one rebuild).
	if (AffectedTypes.Num() == 0 || !ScatterRenderer || !ScatterRenderer->IsInitialized())
	{
		return;
	}
	for (int32 TypeID : AffectedTypes)
	{
		const FScatterDefinition* Def = GetScatterDefinition(TypeID);
		if (!Def)
		{
			continue;
		}
		TArray<FTransform> RemainingTransforms;
		for (const FScatterSpawnPoint& Point : ScatterData.SpawnPoints)
		{
			if (Point.ScatterTypeID == TypeID)
			{
				RemainingTransforms.Add(Point.GetTransform(Def->bAlignToSurfaceNormal, Def->SurfaceOffset));
			}
		}
		ScatterRenderer->UpdateChunkTypeInstances(ChunkCoord, TypeID, MoveTemp(RemainingTransforms));
	}
}

void UVoxelScatterManager::ProcessPendingExclusionRegrow()
{
	if (PendingExclusionRegrow.Num() == 0)
	{
		return;
	}

	int32 Launched = 0;
	TArray<FIntVector> Processed;

	for (const FIntVector& ChunkCoord : PendingExclusionRegrow)
	{
		if (Launched >= MaxExclusionRegrowLaunchesPerTick || AsyncScatterInProgress.Num() >= MaxAsyncScatterTasks)
		{
			break;
		}

		// A busy chunk stays queued for a later tick: the replacement result must not
		// interleave with an in-flight (or queued) extraction/stream for the same chunk.
		if (AsyncScatterInProgress.Contains(ChunkCoord)
			|| DistanceStreamInProgress.Contains(ChunkCoord)
			|| PendingQueueSet.Contains(ChunkCoord))
		{
			continue;
		}

		const FChunkSurfaceData* Surface = SurfaceCache.Find(ChunkCoord);
		if (!Surface || !Surface->bIsValid || Surface->SurfacePoints.Num() == 0)
		{
			// Surface cache gone (chunk unloaded or a regenerate dropped it) — the next fresh
			// generation is volume-aware, so this entry is obsolete.
			Processed.Add(ChunkCoord);
			continue;
		}

		const FVector ChunkCenter = GetChunkWorldOrigin(ChunkCoord) + FVector(Configuration->GetChunkWorldSize() * 0.5f);
		const float ChunkDistance = FVector::Dist(ChunkCenter, LastViewerPosition);
		TArray<FScatterDefinition> Definitions = BuildEligibleDefinitionsForDistance(ChunkDistance);
		if (Definitions.Num() == 0)
		{
			Processed.Add(ChunkCoord);
			continue;
		}

		// Reset bookkeeping so the completed result goes through the FIRST-PASS route of
		// ProcessCompletedAsyncScatter (full replace via the smooth UpdateChunkInstances path)
		// instead of appending. Rendered instances stay up until the result swaps them per type.
		CompletedScatterTypes.Remove(ChunkCoord);
		SpawnCache.Remove(ChunkCoord);
		AsyncScatterInProgress.Add(ChunkCoord);

		FChunkSurfaceData SurfaceCopy = *Surface;
		const uint32 ChunkSeed = FVoxelScatterPlacement::ComputeChunkSeed(ChunkCoord, WorldSeed);
		TWeakObjectPtr<UVoxelScatterManager> WeakThis(this);

		Async(EAsyncExecution::ThreadPool,
			[WeakThis, ChunkCoord, ChunkSeed, SurfaceCopy = MoveTemp(SurfaceCopy),
			 Definitions = MoveTemp(Definitions)]() mutable
		{
			// === THREAD POOL: placement-only relaunch from cached surface data ===
			FAsyncScatterResult Result;
			Result.ChunkCoord = ChunkCoord;
			FVoxelScatterPlacement::GenerateSpawnPoints(SurfaceCopy, Definitions, ChunkSeed, Result.ScatterData);
			for (const FScatterDefinition& Def : Definitions)
			{
				Result.GeneratedTypeIDs.Add(Def.ScatterID);
			}
			Result.SurfaceData = MoveTemp(SurfaceCopy);
			Result.bSuccess = true;

			if (UVoxelScatterManager* This = WeakThis.Get())
			{
				This->CompletedScatterQueue.Enqueue(MoveTemp(Result));
			}
		});

		Processed.Add(ChunkCoord);
		++Launched;
	}

	for (const FIntVector& ChunkCoord : Processed)
	{
		PendingExclusionRegrow.Remove(ChunkCoord);
	}
}

TArray<FScatterDefinition> UVoxelScatterManager::BuildEligibleDefinitionsForDistance(float ChunkDistance) const
{
	TArray<FScatterDefinition> Result;
	if (!Configuration)
	{
		return Result;
	}

	// The hand-off's eligibility rule (minus the per-chunk completed-types check: regrow clears that set before launching).
	for (const FScatterDefinition& Def : ScatterDefinitions)
	{
		if (IsDefinitionEligible(Def, ChunkDistance))
		{
			Result.Add(Def);
		}
	}
	return Result;
}

bool UVoxelScatterManager::IsDefinitionEligible(const FScatterDefinition& Def, float ChunkDistance) const
{
	return Def.bEnabled && PassesTreeModeRule(Def, ChunkDistance) && ChunkDistance <= GetEffectiveSpawnDistance(Def);
}

// ==================== Configuration ====================

void UVoxelScatterManager::SetScatterRadius(float Radius)
{
	ScatterRadius = FMath::Max(Radius, 1000.0f);
	UE_LOG(LogVoxelScatter, Log, TEXT("Scatter radius set to %.0f"), ScatterRadius);
}

void UVoxelScatterManager::SetSurfacePointSpacing(float Spacing)
{
	SurfacePointSpacing = FMath::Max(Spacing, 10.0f);
	UE_LOG(LogVoxelScatter, Log, TEXT("Surface point spacing set to %.0f"), SurfacePointSpacing);
}

void UVoxelScatterManager::SetWorldSeed(uint32 Seed)
{
	WorldSeed = Seed;
	UE_LOG(LogVoxelScatter, Log, TEXT("World seed set to %u"), WorldSeed);
}

// ==================== Debug ====================

void UVoxelScatterManager::DrawDebugVisualization(UWorld* World) const
{
#if ENABLE_DRAW_DEBUG
	if (!World || !bDebugVisualization)
	{
		return;
	}

	// Draw spawn points for each chunk
	for (const auto& Pair : SpawnCache)
	{
		const FChunkScatterData& Data = Pair.Value;
		if (!Data.bIsValid)
		{
			continue;
		}

		for (const FScatterSpawnPoint& Point : Data.SpawnPoints)
		{
			// Get color from definition
			FColor Color = FColor::White;
			if (const FScatterDefinition* Def = GetScatterDefinition(Point.ScatterTypeID))
			{
				Color = Def->DebugColor;
			}

			// Draw sphere at spawn point
			const float Radius = 10.0f;
			DrawDebugSphere(World, Point.Position, Radius, 4, Color, false, -1.0f, 0, 1.0f);

			// Draw normal direction
			DrawDebugLine(World, Point.Position, Point.Position + Point.Normal * 30.0f, FColor::Blue, false, -1.0f, 0, 0.5f);
		}
	}

	// Draw scatter radius around viewer
	DrawDebugSphere(World, LastViewerPosition, ScatterRadius, 32, FColor::Yellow, false, -1.0f, 0, 1.0f);
#endif
}

int64 UVoxelScatterManager::GetTotalMemoryUsage() const
{
	int64 Total = sizeof(UVoxelScatterManager);

	// Surface data cache
	Total += SurfaceCache.GetAllocatedSize();
	for (const auto& Pair : SurfaceCache)
	{
		Total += Pair.Value.GetAllocatedSize();
	}

	// Scatter data cache
	Total += SpawnCache.GetAllocatedSize();
	for (const auto& Pair : SpawnCache)
	{
		Total += Pair.Value.GetAllocatedSize();
	}

	// Pending generation queue
	Total += PendingGenerationQueue.GetAllocatedSize();
	for (const auto& Pending : PendingGenerationQueue)
	{
		Total += Pending.ChunkVoxelData.GetAllocatedSize();
	}
	Total += PendingQueueSet.GetAllocatedSize();

	// Async in-progress sets
	Total += AsyncScatterInProgress.GetAllocatedSize();
	Total += DistanceStreamInProgress.GetAllocatedSize();

	// Cleared volumes
	Total += ClearedVolumesPerChunk.GetAllocatedSize();
	for (const auto& Pair : ClearedVolumesPerChunk)
	{
		Total += Pair.Value.GetAllocatedSize();
	}

	// Exclusion volumes + pending regrow queue
	Total += ExclusionVolumes.GetAllocatedSize();
	Total += PendingExclusionRegrow.GetAllocatedSize();

	// Scatter renderer
	if (ScatterRenderer)
	{
		Total += ScatterRenderer->GetTotalMemoryUsage();
	}

	return Total;
}

FScatterStatistics UVoxelScatterManager::GetStatistics() const
{
	FScatterStatistics Stats;
	Stats.ChunksWithScatter = SpawnCache.Num();
	Stats.TotalHISMInstances = (ScatterRenderer && ScatterRenderer->IsInitialized()) ? ScatterRenderer->GetTotalInstanceCount() : 0;
	Stats.TotalSurfacePoints = TotalSurfacePointsExtracted;
	Stats.TotalSpawnPoints = TotalSpawnPointsGenerated;

	// Calculate memory usage
	for (const auto& Pair : SurfaceCache)
	{
		Stats.SurfaceDataMemory += Pair.Value.GetAllocatedSize();
	}
	for (const auto& Pair : SpawnCache)
	{
		Stats.ScatterDataMemory += Pair.Value.GetAllocatedSize();
	}

	return Stats;
}

FString UVoxelScatterManager::GetDebugStats() const
{
	const FScatterStatistics Stats = GetStatistics();

	FString RendererStats = TEXT("Not initialized");
	if (ScatterRenderer && ScatterRenderer->IsInitialized())
	{
		RendererStats = ScatterRenderer->GetDebugStats();
	}

	return FString::Printf(
		TEXT("=== VoxelScatterManager ===\n")
		TEXT("Initialized: %s\n")
		TEXT("Definitions: %d\n")
		TEXT("Chunks with Scatter: %d\n")
		TEXT("Pending Queue: %d\n")
		TEXT("Async In-Flight: %d / %d\n")
		TEXT("Distance Stream In-Flight: %d / %d\n")
		TEXT("Awaiting First Extraction: %d (handed off by the distance pass: %lld, horizon %.0f)\n")
		TEXT("Total Surface Points: %lld\n")
		TEXT("Total Spawn Points: %lld\n")
		TEXT("Avg Surface/Chunk: %.1f\n")
		TEXT("Avg Spawn/Chunk: %.1f\n")
		TEXT("Surface Data Memory: %.2f KB\n")
		TEXT("Scatter Data Memory: %.2f KB\n")
		TEXT("Worker extraction since reset: %lld chunks, %lld surface points; extract mean %.3f ms (max %.2f), placement mean %.3f ms (max %.2f)\n")
		TEXT("%s\n"),
		bIsInitialized ? TEXT("Yes") : TEXT("No"),
		ScatterDefinitions.Num(),
		Stats.ChunksWithScatter,
		PendingGenerationQueue.Num(),
		AsyncScatterInProgress.Num(), MaxAsyncScatterTasks,
		DistanceStreamInProgress.Num(), MaxDistanceStreamTasks,
		ChunksAwaitingFirstExtraction.Num(), TotalDeferredFirstExtractions, GetScatterHorizon(),
		Stats.TotalSurfacePoints,
		Stats.TotalSpawnPoints,
		Stats.GetAverageSurfacePointsPerChunk(),
		Stats.GetAverageSpawnPointsPerChunk(),
		Stats.SurfaceDataMemory / 1024.0f,
		Stats.ScatterDataMemory / 1024.0f,
		ExtractionTiming.Chunks, ExtractionTiming.Points,
		ExtractionTiming.Chunks > 0 ? 1000.0 * ExtractionTiming.ExtractSeconds / ExtractionTiming.Chunks : 0.0, 1000.0 * ExtractionTiming.ExtractMaxSeconds,
		ExtractionTiming.Chunks > 0 ? 1000.0 * ExtractionTiming.PlaceSeconds / ExtractionTiming.Chunks : 0.0, 1000.0 * ExtractionTiming.PlaceMaxSeconds,
		*RendererStats
	);
}

void UVoxelScatterManager::ResetExtractionTiming()
{
	ExtractionTiming = FExtractionTiming();
}

// ==================== Internal Methods ====================

void UVoxelScatterManager::RemoveChunkScatter(const FIntVector& ChunkCoord)
{
	// Remove HISM instances first (before clearing cache data)
	if (ScatterRenderer && ScatterRenderer->IsInitialized())
	{
		ScatterRenderer->RemoveChunkInstances(ChunkCoord);
	}

	// Both caches, always (world modes review G30): with the two removals joined by a short-circuit ||, a chunk with
	// surface data kept its spawn points cached after unloading, and its next extraction was taken for a supplemental
	// pass (spawn points appended to the stale ones, the surface data not cached again).
	const bool bHadSurface = SurfaceCache.Remove(ChunkCoord) > 0;
	const bool bHadScatter = SpawnCache.Remove(ChunkCoord) > 0;
	if (bHadSurface || bHadScatter)
	{
		UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): Scatter data removed"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
	}
}

void UVoxelScatterManager::ApplyClearedVolumesToResult(const FIntVector& ChunkCoord, FChunkScatterData* ScatterData, FChunkSurfaceData* SurfaceData) const
{
	// Persistent exclusion volumes suppress SPAWN points on every async completion path (extraction,
	// distance stream) — this is what makes a chunk that streams in while a volume
	// is active born bare. Surface points are deliberately left whole: the surface cache must
	// survive so unregistering a volume can regrow foliage from it.
	if (ScatterData && ExclusionVolumes.Num() > 0)
	{
		ScatterData->SpawnPoints.RemoveAll([this](const FScatterSpawnPoint& Point)
		{
			return IsPointExcluded(Point.Position);
		});
	}

	const TArray<FClearedScatterVolume>* Volumes = ClearedVolumesPerChunk.Find(ChunkCoord);
	if (!Volumes || Volumes->Num() == 0)
	{
		return;
	}

	auto InAnyVolume = [Volumes](const FVector& Position)
	{
		for (const FClearedScatterVolume& Volume : *Volumes)
		{
			if (Volume.ContainsPoint(Position))
			{
				return true;
			}
		}
		return false;
	};

	if (ScatterData)
	{
		ScatterData->SpawnPoints.RemoveAll([&InAnyVolume](const FScatterSpawnPoint& Point)
		{
			return InAnyVolume(Point.Position);
		});
	}

	if (SurfaceData)
	{
		SurfaceData->SurfacePoints.RemoveAll([&InAnyVolume](const FVoxelSurfacePoint& Point)
		{
			return InAnyVolume(Point.Position);
		});
	}
}

void UVoxelScatterManager::ProcessPendingGenerationQueue()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelScatter_LaunchQueue);
	if (PendingGenerationQueue.Num() == 0)
	{
		return;
	}

	// Throttle by number of in-flight async tasks
	const int32 AvailableSlots = MaxAsyncScatterTasks - AsyncScatterInProgress.Num();
	if (AvailableSlots <= 0)
	{
		return;
	}

	// Also respect per-frame limit
	int32 NumToLaunch = FMath::Min(AvailableSlots, PendingGenerationQueue.Num());
	if (MaxScatterGenerationsPerFrame > 0)
	{
		NumToLaunch = FMath::Min(NumToLaunch, MaxScatterGenerationsPerFrame);
	}

	int32 LaunchedCount = 0;

	// Launch from back of queue (closest chunks are at back, due to reversed sort)
	for (int32 i = 0; i < NumToLaunch; ++i)
	{
		if (PendingGenerationQueue.Num() == 0)
		{
			break;
		}

		// Take last item (closest chunk — O(1) pop from back)
		FPendingScatterGeneration Request = MoveTemp(PendingGenerationQueue.Last());
		PendingGenerationQueue.Pop(EAllowShrinking::No);
		PendingQueueSet.Remove(Request.ChunkCoord);

		// Launch async scatter generation on thread pool
		LaunchAsyncScatterGeneration(MoveTemp(Request));
		++LaunchedCount;
	}

	if (LaunchedCount > 0)
	{
		UE_LOG(LogVoxelScatter, Verbose, TEXT("Launched %d async scatter tasks (%d queued, %d in-flight)"),
			LaunchedCount, PendingGenerationQueue.Num(), AsyncScatterInProgress.Num());
	}
}

bool UVoxelScatterManager::PassesTreeModeRule(const FScatterDefinition& Def, float ChunkDistance) const
{
	if (!Configuration || Def.MeshType != EScatterMeshType::VoxelInjection)
	{
		return true;
	}
	// As the hand-off: a VoxelInjection type is HISM scatter only in HISM mode, or in Both mode past the voxel trees' reach.
	if (Configuration->TreeMode == EVoxelTreeMode::VoxelData)
	{
		return false;
	}
	return Configuration->TreeMode != EVoxelTreeMode::Both || ChunkDistance > Configuration->VoxelTreeMaxDistance;
}

float UVoxelScatterManager::GetScatterHorizon() const
{
	float Horizon = 0.0f;
	for (const FScatterDefinition& Def : ScatterDefinitions)
	{
		if (Def.bEnabled && PassesTreeModeRule(Def, FLT_MAX))
		{
			Horizon = FMath::Max(Horizon, GetEffectiveSpawnDistance(Def));
		}
	}
	return Horizon;
}

void UVoxelScatterManager::ProcessDeferredFirstExtractions()
{
	if (ChunksAwaitingFirstExtraction.Num() == 0 || !Configuration)
	{
		return;
	}
	if (CVarVoxelScatterFirstExtractionAtHorizon.GetValueOnGameThread() == 0 || !ChunkHandOffRequester)
	{
		// The rule before: such a chunk waits for its next mesh. Drop the set so it does not grow for nothing.
		ChunksAwaitingFirstExtraction.Empty();
		return;
	}

	const float Horizon = GetScatterHorizon();
	const float ChunkWorldSize = Configuration->GetChunkWorldSize();

	// The chunks within the horizon, nearest first (the candidates are collected first: a hand-off edits the set)
	struct FCandidate
	{
		FIntVector ChunkCoord;
		float Distance;
	};
	TArray<FCandidate> Candidates;
	for (const FIntVector& ChunkCoord : ChunksAwaitingFirstExtraction)
	{
		const FVector ChunkCenter = GetChunkWorldOrigin(ChunkCoord) + FVector(ChunkWorldSize * 0.5f);
		const float ChunkDistance = FVector::Dist(ChunkCenter, LastViewerPosition);
		if (ChunkDistance <= Horizon)
		{
			Candidates.Add({ ChunkCoord, ChunkDistance });
		}
	}
	if (Candidates.Num() == 0)
	{
		return;
	}
	Candidates.Sort([](const FCandidate& A, const FCandidate& B) { return A.Distance < B.Distance; });

	int32 Launched = 0;
	for (const FCandidate& Candidate : Candidates)
	{
		if (Launched >= MaxDistanceSpawnChunksPerPass)
		{
			break;
		}
		// The hand-off runs HandOffChunkVoxels, which queues the extraction and takes the chunk out of the set; a chunk
		// without voxel data any more is dropped.
		ChunksAwaitingFirstExtraction.Remove(Candidate.ChunkCoord);
		++TotalDeferredFirstExtractions;
		if (ChunkHandOffRequester(Candidate.ChunkCoord))
		{
			++Launched;
		}
	}
	if (Launched > 0)
	{
		UE_LOG(LogVoxelScatter, Verbose, TEXT("Deferred first extraction: handed off %d of %d chunks within the scatter horizon (%.0f), %d still waiting"),
			Launched, Candidates.Num(), Horizon, ChunksAwaitingFirstExtraction.Num());
	}
}

void UVoxelScatterManager::PerformDistanceSpawn()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelScatter_DistanceSpawn);
	if (!Configuration)
	{
		return;
	}

	// G79: the chunks that arrived beyond every type's reach, once within the scatter horizon (before the cache check: they
	// have nothing cached yet)
	ProcessDeferredFirstExtractions();

	if (SurfaceCache.Num() == 0)
	{
		return;
	}

	// Collect the definitions the distance pass streams, each with its spawn distance: those with a SpawnDistance of their
	// own and, since G79, those at ScatterRadius too (their spawn is no longer tied to the hand-off that happened to find
	// them in range; the hand-off's tree-mode rule applies to them as it would there)
	const bool bRadiusTypesStream = CVarVoxelScatterFirstExtractionAtHorizon.GetValueOnGameThread() != 0;
	struct FDistanceLimitedDef
	{
		const FScatterDefinition* Def;
		float SpawnDistance;
		bool bTreeModeRule;
	};
	TArray<FDistanceLimitedDef> DistanceLimitedDefs;
	for (const FScatterDefinition& Def : ScatterDefinitions)
	{
		if (!Def.bEnabled)
		{
			continue;
		}
		if (Def.SpawnDistance > 0.0f)
		{
			DistanceLimitedDefs.Add({ &Def, Def.SpawnDistance, false });
		}
		else if (bRadiusTypesStream && PassesTreeModeRule(Def, FLT_MAX))
		{
			DistanceLimitedDefs.Add({ &Def, ScatterRadius, true });
		}
	}

	if (DistanceLimitedDefs.Num() == 0)
	{
		return;
	}

	const float ChunkWorldSize = Configuration->GetChunkWorldSize();

	// --- Out-of-range cleanup pass ---
	// For types beyond SpawnDistance * (1 + hysteresis):
	// 1. Clear from CompletedScatterTypes (allows regeneration on return)
	// 2. Remove spawn points from SpawnCache (prevents data bloat)
	// 3. Release instances in renderer (zero-scaled, returned to pool — no flicker)

	for (auto& CompletedPair : CompletedScatterTypes)
	{
		const FIntVector& ChunkCoord = CompletedPair.Key;
		TSet<int32>& CompletedTypeIDs = CompletedPair.Value;

		const FVector ChunkCenter = GetChunkWorldOrigin(ChunkCoord) +
			FVector(ChunkWorldSize * 0.5f);
		const float ChunkDistance = FVector::Dist(ChunkCenter, LastViewerPosition);

		TArray<int32> TypesToRemove;
		for (const FDistanceLimitedDef& Limited : DistanceLimitedDefs)
		{
			const float CleanupDistance = Limited.SpawnDistance * (1.0f + DistanceCleanupHysteresis);
			if (ChunkDistance > CleanupDistance && CompletedTypeIDs.Contains(Limited.Def->ScatterID))
			{
				TypesToRemove.Add(Limited.Def->ScatterID);
			}
		}

		if (TypesToRemove.Num() == 0)
		{
			continue;
		}

		// Clear from completed tracking (allows regeneration when player returns)
		for (int32 TypeID : TypesToRemove)
		{
			CompletedTypeIDs.Remove(TypeID);
		}

		// Release instances directly — zero-scaled, returned to pool
		if (ScatterRenderer && ScatterRenderer->IsInitialized())
		{
			for (int32 TypeID : TypesToRemove)
			{
				ScatterRenderer->ReleaseChunkScatterType(ChunkCoord, TypeID);
			}
		}

		// Remove spawn points for these types from the scatter data cache
		FChunkScatterData* ScatterData = SpawnCache.Find(ChunkCoord);
		if (ScatterData && ScatterData->bIsValid)
		{
			TSet<int32> TypeIDSet(TypesToRemove);
			ScatterData->SpawnPoints.RemoveAll([&TypeIDSet](const FScatterSpawnPoint& Point)
			{
				return TypeIDSet.Contains(Point.ScatterTypeID);
			});
		}
	}

	// --- Spawn pass ---
	// Don't launch more if our dedicated pipeline is saturated
	const int32 AvailableSlots = MaxDistanceStreamTasks - DistanceStreamInProgress.Num();
	if (AvailableSlots <= 0)
	{
		return;
	}

	// Collect candidates: chunks with cached surface data that need new scatter types
	struct FSpawnCandidate
	{
		FIntVector ChunkCoord;
		float Distance;
		TArray<FScatterDefinition> Definitions;
	};
	TArray<FSpawnCandidate> Candidates;

	for (const auto& Pair : SurfaceCache)
	{
		const FIntVector& ChunkCoord = Pair.Key;
		const FChunkSurfaceData& SurfaceData = Pair.Value;

		if (!SurfaceData.bIsValid || SurfaceData.SurfacePoints.Num() == 0)
		{
			continue;
		}

		// Skip chunks already being processed by distance streaming
		if (DistanceStreamInProgress.Contains(ChunkCoord))
		{
			continue;
		}

		// Skip chunks with a chunk-generation-pipeline task in flight. Matters for the exclusion
		// regrow relaunch, which clears CompletedScatterTypes while KEEPING the surface cache: a
		// distance-spawn here would see every type as un-generated and append a duplicate set on
		// top of the regrow's full replace. The skip just delays this chunk to a later pass.
		if (AsyncScatterInProgress.Contains(ChunkCoord))
		{
			continue;
		}

		const FVector ChunkCenter = GetChunkWorldOrigin(ChunkCoord) +
			FVector(ChunkWorldSize * 0.5f);
		const float ChunkDistance = FVector::Dist(ChunkCenter, LastViewerPosition);

		// Get completed types for this chunk
		const TSet<int32>* CompletedTypes = CompletedScatterTypes.Find(ChunkCoord);

		// Find definitions that are now in range but haven't been generated
		TArray<FScatterDefinition> NewlyInRangeDefs;
		for (const FDistanceLimitedDef& Limited : DistanceLimitedDefs)
		{
			if (CompletedTypes && CompletedTypes->Contains(Limited.Def->ScatterID))
			{
				continue;
			}
			if (Limited.bTreeModeRule && !PassesTreeModeRule(*Limited.Def, ChunkDistance))
			{
				continue;
			}

			if (ChunkDistance <= Limited.SpawnDistance)
			{
				NewlyInRangeDefs.Add(*Limited.Def);
			}
		}

		if (NewlyInRangeDefs.Num() > 0)
		{
			FSpawnCandidate Candidate;
			Candidate.ChunkCoord = ChunkCoord;
			Candidate.Distance = ChunkDistance;
			Candidate.Definitions = MoveTemp(NewlyInRangeDefs);
			Candidates.Add(MoveTemp(Candidate));
		}
	}

	if (Candidates.Num() == 0)
	{
		return;
	}

	// Sort closest first for best player experience
	Candidates.Sort([](const FSpawnCandidate& A, const FSpawnCandidate& B)
	{
		return A.Distance < B.Distance;
	});

	// Launch async tasks on thread pool — dedicated pipeline, no competition with chunk generation
	int32 ChunksLaunched = 0;
	const int32 MaxToLaunch = FMath::Min(MaxDistanceSpawnChunksPerPass, AvailableSlots);

	for (const FSpawnCandidate& Candidate : Candidates)
	{
		if (ChunksLaunched >= MaxToLaunch)
		{
			break;
		}

		const FChunkSurfaceData* SurfaceData = SurfaceCache.Find(Candidate.ChunkCoord);
		if (!SurfaceData)
		{
			continue;
		}

		const uint32 ChunkSeed = FVoxelScatterPlacement::ComputeChunkSeed(Candidate.ChunkCoord, WorldSeed);

		// Mark as in-progress in our dedicated tracking (not AsyncScatterInProgress)
		DistanceStreamInProgress.Add(Candidate.ChunkCoord);

		// Pre-mark types as completed to prevent duplicate launches on next pass
		TSet<int32>& Completed = CompletedScatterTypes.FindOrAdd(Candidate.ChunkCoord);
		TArray<int32> TypeIDs;
		for (const FScatterDefinition& Def : Candidate.Definitions)
		{
			Completed.Add(Def.ScatterID);
			TypeIDs.Add(Def.ScatterID);
		}

		// Capture data for thread pool — copy surface data, move definitions
		TWeakObjectPtr<UVoxelScatterManager> WeakThis(this);
		const FIntVector ChunkCoord = Candidate.ChunkCoord;
		FChunkSurfaceData CapturedSurface = *SurfaceData;
		TArray<FScatterDefinition> DefsToGenerate = Candidate.Definitions;

		Async(EAsyncExecution::ThreadPool,
			[WeakThis, ChunkCoord, ChunkSeed,
			 CapturedSurface = MoveTemp(CapturedSurface),
			 DefsToGenerate = MoveTemp(DefsToGenerate),
			 TypeIDs = MoveTemp(TypeIDs)]() mutable
		{
			FDistanceStreamResult Result;
			Result.ChunkCoord = ChunkCoord;

			FVoxelScatterPlacement::GenerateSpawnPoints(
				CapturedSurface,
				DefsToGenerate,
				ChunkSeed,
				Result.ScatterData
			);

			Result.GeneratedTypeIDs = MoveTemp(TypeIDs);
			Result.bSuccess = true;

			if (UVoxelScatterManager* This = WeakThis.Get())
			{
				This->DistanceStreamQueue.Enqueue(MoveTemp(Result));
			}
		});

		++ChunksLaunched;
	}

	if (ChunksLaunched > 0)
	{
		UE_LOG(LogVoxelScatter, Verbose, TEXT("Distance spawn: launched %d async tasks (%d candidates, %d in-flight)"),
			ChunksLaunched, Candidates.Num(), DistanceStreamInProgress.Num());
	}
}

void UVoxelScatterManager::ProcessCompletedDistanceStream()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelScatter_DistanceApply);
	FDistanceStreamResult Result;
	int32 ProcessedCount = 0;

	// Count check BEFORE dequeue — see ProcessCompletedAsyncScatter: dequeue-first
	// drops one result on the exiting iteration and leaks its in-progress entry.
	while (ProcessedCount < MaxDistanceStreamResultsPerFrame && DistanceStreamQueue.Dequeue(Result))
	{
		++ProcessedCount;

		// Remove from distance stream tracking
		DistanceStreamInProgress.Remove(Result.ChunkCoord);

		if (!Result.bSuccess)
		{
			continue;
		}

		// Discard if chunk was unloaded while async was in-flight
		if (!SurfaceCache.Contains(Result.ChunkCoord))
		{
			continue;
		}

		// Filter against cleared volumes registered AFTER the task launched (player
		// edits during flight) — the launch-time surface snapshot can't have seen them
		ApplyClearedVolumesToResult(Result.ChunkCoord, &Result.ScatterData, nullptr);

		const int32 SpawnCount = Result.ScatterData.SpawnPoints.Num();

		// Send to renderer for budget-limited HISM addition
		if (SpawnCount > 0 && ScatterRenderer && ScatterRenderer->IsInitialized())
		{
			ScatterRenderer->AddSupplementalInstances(Result.ChunkCoord, Result.ScatterData);
		}

		// Append to scatter data cache
		FChunkScatterData* ExistingScatter = SpawnCache.Find(Result.ChunkCoord);
		if (ExistingScatter && ExistingScatter->bIsValid)
		{
			ExistingScatter->SpawnPoints.Append(Result.ScatterData.SpawnPoints);
		}
		else
		{
			SpawnCache.Add(Result.ChunkCoord, MoveTemp(Result.ScatterData));
		}

		TotalSpawnPointsGenerated += SpawnCount;
	}
}

void UVoxelScatterManager::LaunchAsyncScatterGeneration(FPendingScatterGeneration PendingData)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelScatter_Launch);
	const FIntVector ChunkCoord = PendingData.ChunkCoord;

	// Mark as in-progress
	AsyncScatterInProgress.Add(ChunkCoord);
	++TotalExtractionsLaunched;

	// Capture all values needed by the background thread (no UObject access)
	const FVector ChunkWorldOrigin = GetChunkWorldOrigin(ChunkCoord);
	const float CapturedSurfacePointSpacing = SurfacePointSpacing;
	const uint32 CapturedWorldSeed = WorldSeed;

	// Use definitions captured at queue time (already filtered by distance)
	TArray<FScatterDefinition> FilteredDefinitions = MoveTemp(PendingData.CapturedDefinitions);

	if (FilteredDefinitions.Num() == 0)
	{
		AsyncScatterInProgress.Remove(ChunkCoord);
		return;
	}

	// === Async extraction from the chunk's voxels (LOD-independent) ===

	// Snapshot cleared volumes for this chunk (read-only on thread pool)
	TArray<FClearedScatterVolume> CapturedClearedVolumes;
	if (const TArray<FClearedScatterVolume>* Volumes = ClearedVolumesPerChunk.Find(ChunkCoord))
	{
		CapturedClearedVolumes = *Volumes;
	}

	const bool bCubicExtraction = Configuration && Configuration->MeshingMode == EMeshingMode::Cubic;
	// Build the classification context on the game thread and capture it by value. It holds a
	// non-owning world-mode pointer (same lifetime as the owning ChunkManager, per its async-gen
	// pattern) plus water level and noise params.
	const FScatterExtractionContext ExtractionContext = MakeExtractionContext(ChunkCoord);
	TWeakObjectPtr<UVoxelScatterManager> WeakThis(this);

	Async(EAsyncExecution::ThreadPool,
		[WeakThis, PendingData = MoveTemp(PendingData), ChunkWorldOrigin,
		 CapturedSurfacePointSpacing, CapturedWorldSeed, ChunkCoord,
		 FilteredDefinitions = MoveTemp(FilteredDefinitions),
		 CapturedClearedVolumes = MoveTemp(CapturedClearedVolumes),
		 ExtractionContext, bCubicExtraction]() mutable
	{
		// === THREAD POOL: Voxel-based surface extraction + placement ===

		FAsyncScatterResult Result;
		Result.ChunkCoord = ChunkCoord;
		Result.bSuccess = false;

		// Validate voxel data
		const int32 ExpectedVoxels = PendingData.ChunkSize * PendingData.ChunkSize * PendingData.ChunkSize;
		if (PendingData.ChunkVoxelData.Num() != ExpectedVoxels)
		{
			if (UVoxelScatterManager* This = WeakThis.Get())
			{
				This->CompletedScatterQueue.Enqueue(MoveTemp(Result));
			}
			return;
		}

		// Extract surface points from voxel data (LOD-independent)
		FChunkSurfaceData SurfaceData;
		const double ExtractStart = FPlatformTime::Seconds();
		ExtractChunkSurfacePoints(bCubicExtraction, PendingData.ChunkVoxelData, ChunkCoord, ChunkWorldOrigin, PendingData.ChunkSize,
			PendingData.VoxelSize, CapturedSurfacePointSpacing, CapturedClearedVolumes, ExtractionContext, SurfaceData);
		const double PlaceStart = FPlatformTime::Seconds();
		Result.ExtractSeconds = PlaceStart - ExtractStart;
		Result.ExtractedPoints = SurfaceData.SurfacePoints.Num();

		if (!SurfaceData.bIsValid || SurfaceData.SurfacePoints.Num() == 0)
		{
			if (UVoxelScatterManager* This = WeakThis.Get())
			{
				This->CompletedScatterQueue.Enqueue(MoveTemp(Result));
			}
			return;
		}

		// Scatter placement
		const uint32 ChunkSeed = FVoxelScatterPlacement::ComputeChunkSeed(ChunkCoord, CapturedWorldSeed);

		FChunkScatterData ScatterData;
		FVoxelScatterPlacement::GenerateSpawnPoints(
			SurfaceData,
			FilteredDefinitions,
			ChunkSeed,
			ScatterData
		);
		Result.PlaceSeconds = FPlatformTime::Seconds() - PlaceStart;

		Result.SurfaceData = MoveTemp(SurfaceData);
		Result.ScatterData = MoveTemp(ScatterData);
		Result.bSuccess = true;

		// Track which types were generated
		for (const FScatterDefinition& Def : FilteredDefinitions)
		{
			Result.GeneratedTypeIDs.Add(Def.ScatterID);
		}

		// Enqueue result for game thread consumption
		if (UVoxelScatterManager* This = WeakThis.Get())
		{
			This->CompletedScatterQueue.Enqueue(MoveTemp(Result));
		}
	});
}

void UVoxelScatterManager::ProcessCompletedAsyncScatter()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelScatter_Apply);
	FAsyncScatterResult Result;
	int32 ProcessedCount = 0;
	const int32 MaxProcessPerFrame = 2;

	// Count check BEFORE dequeue: dequeuing first would pop a result on the
	// iteration that exits the loop and silently drop it, leaving its chunk
	// stuck in AsyncScatterInProgress forever.
	while (ProcessedCount < MaxProcessPerFrame && CompletedScatterQueue.Dequeue(Result))
	{
		++ProcessedCount;
		++ExtractionTiming.Chunks;
		ExtractionTiming.Points += Result.ExtractedPoints;
		ExtractionTiming.ExtractSeconds += Result.ExtractSeconds;
		ExtractionTiming.ExtractMaxSeconds = FMath::Max(ExtractionTiming.ExtractMaxSeconds, Result.ExtractSeconds);
		ExtractionTiming.PlaceSeconds += Result.PlaceSeconds;
		ExtractionTiming.PlaceMaxSeconds = FMath::Max(ExtractionTiming.PlaceMaxSeconds, Result.PlaceSeconds);

		// Remove from in-progress tracking. If the chunk is no longer tracked,
		// it was unloaded or regeneration was requested while the task was in
		// flight — discard the stale result rather than resurrecting scatter
		// (cache entries + HISM instances) for a chunk that no longer exists.
		if (AsyncScatterInProgress.Remove(Result.ChunkCoord) == 0)
		{
			UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): Async scatter result discarded - chunk no longer tracked"),
				Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z);
			continue;
		}

		if (!Configuration)
		{
			continue;
		}

		if (!Result.bSuccess)
		{
			// No surface. A chunk without a scatter cache renders nothing of its own, except after RegenerateChunkScatter,
			// which keeps its instances until the re-extraction replaces them: an edit that left the chunk no surface
			// replaces them with none (world modes review G54). A supplemental pass (a cache exists) keeps what it has.
			if (!SpawnCache.Contains(Result.ChunkCoord) && ScatterRenderer && ScatterRenderer->IsInitialized())
			{
				ScatterRenderer->RemoveChunkInstances(Result.ChunkCoord);
			}
			continue;
		}

		// Filter against cleared volumes registered AFTER the task launched (player
		// edits during flight) — the launch-time snapshot can't have seen them
		ApplyClearedVolumesToResult(Result.ChunkCoord, &Result.ScatterData, &Result.SurfaceData);

		const int32 SurfacePointCount = Result.SurfaceData.SurfacePoints.Num();
		const int32 SpawnCount = Result.ScatterData.SpawnPoints.Num();

		// Track which scatter types were generated (never regenerated)
		TSet<int32>& Completed = CompletedScatterTypes.FindOrAdd(Result.ChunkCoord);
		Completed.Append(Result.GeneratedTypeIDs);

		// Append or create scatter/surface data
		FChunkScatterData* ExistingScatter = SpawnCache.Find(Result.ChunkCoord);
		const bool bSupplementalPass = (ExistingScatter && ExistingScatter->bIsValid);
		if (bSupplementalPass)
		{
			// Supplemental pass: append new spawn points to existing data. CompletedScatterTypes
			// guarantees this pass only generated types NOT already on the chunk, so the render
			// update below is append-only and never disturbs existing instances.
			ExistingScatter->SpawnPoints.Append(Result.ScatterData.SpawnPoints);

			UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): Supplemental scatter appended (+%d spawn, total %d)"),
				Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z,
				SpawnCount, ExistingScatter->SpawnPoints.Num());
		}
		else
		{
			// First pass: cache new data
			SurfaceCache.Add(Result.ChunkCoord, MoveTemp(Result.SurfaceData));
			SpawnCache.Add(Result.ChunkCoord, MoveTemp(Result.ScatterData));
		}

		TotalSurfacePointsExtracted += SurfacePointCount;
		TotalSpawnPointsGenerated += SpawnCount;
		++TotalChunksProcessed;

		// Update HISM instances. A first pass adds the whole chunk via the deferred-add budget;
		// a supplemental pass appends ONLY the newly generated points, leaving every existing
		// instance (this chunk and all others) in place. Routing supplemental updates through
		// AddSupplementalInstances instead of UpdateChunkInstances is what prevents a per-chunk
		// streaming update from triggering a world-wide per-type rebuild — the cause of the
		// "everything blinks out and streams back" refresh flash.
		if (ScatterRenderer && ScatterRenderer->IsInitialized())
		{
			if (bSupplementalPass)
			{
				ScatterRenderer->AddSupplementalInstances(Result.ChunkCoord, Result.ScatterData);
			}
			else
			{
				ScatterRenderer->UpdateChunkInstances(Result.ChunkCoord, SpawnCache[Result.ChunkCoord]);
			}
		}

		OnChunkScatterReady.Broadcast(Result.ChunkCoord, SpawnCount);

		// Check for deferred supplemental pass (e.g., LOD 0 defs arrived while LOD > 0 was in-flight)
		FPendingScatterGeneration DeferredPass;
		if (DeferredSupplementalPasses.RemoveAndCopyValue(Result.ChunkCoord, DeferredPass))
		{
			// Remove any types that were just completed
			DeferredPass.CapturedDefinitions.RemoveAll([&Completed](const FScatterDefinition& Def)
			{
				return Completed.Contains(Def.ScatterID);
			});

			if (DeferredPass.CapturedDefinitions.Num() > 0)
			{
				UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): Re-queuing deferred supplemental (%d defs)"),
					Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z,
					DeferredPass.CapturedDefinitions.Num());

				int32 InsertIndex = Algo::LowerBound(PendingGenerationQueue, DeferredPass);
				PendingGenerationQueue.Insert(MoveTemp(DeferredPass), InsertIndex);
				PendingQueueSet.Add(Result.ChunkCoord);
			}
		}
	}
}

void UVoxelScatterManager::ExtractSurfacePointsByColumn(
	const TArray<FVoxelData>& VoxelData,
	const FIntVector& ChunkCoord,
	const FVector& ChunkWorldOrigin,
	int32 ChunkSize,
	float VoxelSize,
	float SurfacePointSpacing,
	const TArray<FClearedScatterVolume>& ClearedVolumes,
	const FScatterExtractionContext& Context,
	FChunkSurfaceData& OutSurfaceData)
{
	OutSurfaceData = FChunkSurfaceData(ChunkCoord);
	OutSurfaceData.AveragePointSpacing = SurfacePointSpacing;

	const int32 ExpectedVoxels = ChunkSize * ChunkSize * ChunkSize;
	if (VoxelData.Num() != ExpectedVoxels)
	{
		OutSurfaceData.bIsValid = false;
		return;
	}

	// Stride to match SurfacePointSpacing (e.g., 100cm spacing with 100cm voxels = stride 1)
	const int32 Stride = FMath::Max(1, FMath::RoundToInt(SurfacePointSpacing / VoxelSize));
	const int32 ColumnsPerAxis = (ChunkSize + Stride - 1) / Stride;
	OutSurfaceData.SurfacePoints.Reserve(ColumnsPerAxis * ColumnsPerAxis);

	// A chunk whose top sits more than this far below the analytic terrain surface is treated as
	// buried (its surface points are covered by terrain in the chunk(s) above). ~1.5 voxels absorbs
	// isosurface-interpolation jitter around the real surface.
	const float CoverThreshold = VoxelSize * 1.5f;

	// World Z of this chunk's top. The cross-chunk coverage test below compares the CHUNK top (not the
	// per-point Z) against the analytic surface, so it only fires when the whole chunk is buried under a
	// neighbor. Comparing the chunk top keeps the test edit-independent: when the player carves into a
	// surface chunk (exposing a cave floor to sky), the now-topmost points reclassify to surface via
	// bFoundColumnTop instead of staying underground against an edit-blind analytic height.
	const float ChunkTopWorldZ = ChunkWorldOrigin.Z + ChunkSize * VoxelSize;

	// Helper lambda: get density at voxel position with bounds clamping
	auto GetDensity = [&](int32 X, int32 Y, int32 Z) -> float
	{
		X = FMath::Clamp(X, 0, ChunkSize - 1);
		Y = FMath::Clamp(Y, 0, ChunkSize - 1);
		Z = FMath::Clamp(Z, 0, ChunkSize - 1);
		const int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
		return static_cast<float>(VoxelData[Index].Density);
	};

	// Scan each column at stride intervals, finding all solid-air transitions.
	// Underground classification uses the voxel underground flag (set by
	// ApplyUndergroundClassificationPass) rather than a column-based heuristic,
	// which avoids falsely classifying exposed slope faces as underground.
	for (int32 X = 0; X < ChunkSize; X += Stride)
	{
		for (int32 Y = 0; Y < ChunkSize; Y += Stride)
		{
			// The topmost solid-air transition in a column is the open-sky surface; any
			// transition below it necessarily has solid terrain above (a cave floor, or the
			// ground under an overhang), so it must be classified underground. This closes the
			// gap where ApplyUndergroundClassificationPass leaves a covered floor unflagged
			// (ceiling < MinSolidThickness, or a cave spanning a chunk boundary) — without it,
			// tall surface scatter (trees) lands on those floors and pokes up through the roof.
			// Slope-safe: on a slope each column's terrain surface IS the topmost transition.
			bool bFoundColumnTop = false;

			// Analytic terrain-surface height at this column (computed once) for the cross-chunk
			// coverage check below: a point well below this has solid terrain above it even when that
			// terrain lives in a neighboring chunk. Only heightmap-based world modes provide a height. It is
			// the generated height, conditioning zones included (G23): against the natural height a POI's
			// lowered pad or ring buried the surface chunk holding its ground.
			float ColumnTerrainHeight = 0.0f;
			bool bHaveColumnHeight = false;
			if (Context.WorldMode != nullptr && Context.WorldMode->IsHeightmapBased())
			{
				ColumnTerrainHeight = Context.GetGeneratedTerrainHeight(
					ChunkWorldOrigin.X + X * VoxelSize,
					ChunkWorldOrigin.Y + Y * VoxelSize);
				bHaveColumnHeight = true;
			}

			// The voxel over the top layer is the chunk above's bottom voxel, which this chunk's data cannot show (world
			// modes review G26, G28): the voxel the generator made there, the cave carve included, with the chunk above's
			// edits applied. Solid, the ground continues into the chunk above, which extracts the real surface: taking it for
			// air made a copy of that surface on this chunk's top face, up to 1.5 voxels under the ground (G26), and under
			// deeper ground a copy of the rock over the whole top face of a buried chunk, which underground scatter took for
			// cave floors (G28). Air, it is the surface or a cave floor at the boundary. Decided at any depth.
			const bool bKnowVoxelAbove = bHaveColumnHeight;

			for (int32 Z = ChunkSize - 1; Z >= 0; --Z)
			{
				const int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
				const FVoxelData& Voxel = VoxelData[Index];

				if (!Voxel.IsSolid())
				{
					continue;
				}

				// Found solid voxel — check if there's air above
				float AirDensity = 0.0f; // Above chunk boundary = air (unless known, below)
				bool bAirAboveIsUnderground = false;
				bool bAirAboveIsWater = false;
				if (Z + 1 < ChunkSize)
				{
					const int32 AboveIndex = X + Y * ChunkSize + (Z + 1) * ChunkSize * ChunkSize;
					const FVoxelData& AboveVoxel = VoxelData[AboveIndex];
					if (AboveVoxel.IsSolid())
					{
						continue; // Not a surface transition
					}
					AirDensity = static_cast<float>(AboveVoxel.Density);
					bAirAboveIsUnderground = AboveVoxel.HasUndergroundFlag();
					bAirAboveIsWater = AboveVoxel.HasWaterFlag();
				}
				else if (bKnowVoxelAbove)
				{
					const FVoxelData AboveVoxel = Context.GetVoxelOutsideChunk(FIntVector(X, Y, ChunkSize),
						ChunkWorldOrigin + FVector(X * VoxelSize, Y * VoxelSize, ChunkSize * VoxelSize), VoxelSize, Voxel.BiomeID, &ColumnTerrainHeight);
					if (AboveVoxel.IsSolid())
					{
						bFoundColumnTop = true; // the ground above covers the rest of the column
						continue;
					}
					// The crossing lies between this voxel and the one above: interpolate against its density, as the mesher does.
					AirDensity = static_cast<float>(AboveVoxel.Density);
				}

				// Interpolate exact Z position (same formula as Marching Cubes edge interpolation)
				const float SolidDensity = static_cast<float>(Voxel.Density);
				float Fraction = 0.5f;
				const float DensityRange = AirDensity - SolidDensity;
				if (FMath::Abs(DensityRange) > SMALL_NUMBER)
				{
					Fraction = (static_cast<float>(VOXEL_SURFACE_THRESHOLD) - SolidDensity) / DensityRange;
					Fraction = FMath::Clamp(Fraction, 0.0f, 1.0f);
				}

				// World position (voxel grid positions, matching mesher convention)
				const FVector WorldPos(
					ChunkWorldOrigin.X + X * VoxelSize,
					ChunkWorldOrigin.Y + Y * VoxelSize,
					ChunkWorldOrigin.Z + (Z + Fraction) * VoxelSize
				);

				// Check cleared volumes
				bool bInClearedVolume = false;
				for (const FClearedScatterVolume& Volume : ClearedVolumes)
				{
					if (Volume.ContainsPoint(WorldPos))
					{
						bInClearedVolume = true;
						break;
					}
				}
				if (bInClearedVolume)
				{
					break; // Skip rest of this column
				}

				// Compute normal from density gradient (central differences)
				const float GradX = GetDensity(X + 1, Y, Z) - GetDensity(X - 1, Y, Z);
				const float GradY = GetDensity(X, Y + 1, Z) - GetDensity(X, Y - 1, Z);
				const float GradZ = GetDensity(X, Y, Z + 1) - GetDensity(X, Y, Z - 1);

				// Normal points from solid toward air (negative gradient direction)
				FVector Normal(-GradX, -GradY, -GradZ);
				if (!Normal.Normalize())
				{
					Normal = FVector::UpVector; // Fallback for flat areas
				}

				// Determine face type from normal direction
				EVoxelFaceType FaceType;
				if (Normal.Z > 0.5f)
				{
					FaceType = EVoxelFaceType::Top;
				}
				else if (Normal.Z < -0.5f)
				{
					FaceType = EVoxelFaceType::Bottom;
				}
				else
				{
					FaceType = EVoxelFaceType::Side;
				}

				// Create surface point
				FVoxelSurfacePoint Point;
				Point.Position = WorldPos;
				Point.Normal = Normal;
				Point.MaterialID = Voxel.MaterialID;
				Point.BiomeID = Voxel.BiomeID;
				Point.FaceType = FaceType;
				Point.AmbientOcclusion = Voxel.GetAO() & 0x03;
				Point.ComputeSlopeAngle();

				// Underground if: the cave/air flag says so; OR this is not the topmost surface in the
				// column (a lower transition is always covered by solid above); OR the whole chunk is
				// buried beneath the analytic terrain surface — solid terrain covers it from a NEIGHBORING
				// chunk (the cross-chunk cave floor the flag and column scan miss). The buried-chunk test is
				// edit-independent (see ChunkTopWorldZ), so carving into a surface chunk reclassifies
				// exposed floors to surface instead of pinning them underground.
				const bool bCoveredByTerrain = bHaveColumnHeight && (ChunkTopWorldZ < ColumnTerrainHeight - CoverThreshold);
				Point.bIsUnderground = bAirAboveIsUnderground || Voxel.HasUndergroundFlag() || bFoundColumnTop || bCoveredByTerrain;

				// Submerged: open water above it (water flag on the air voxel above) OR the point is below
				// the world water level. The water-level test catches submerged seabeds even where the
				// per-voxel water flag did not propagate. Underground takes precedence.
				const bool bBelowWater = Context.Sea.bEnabled && Context.Sea.GetDepth(WorldPos) > 0.0;
				Point.bIsUnderwater = (bAirAboveIsWater || bBelowWater) && !Point.bIsUnderground;

				OutSurfaceData.SurfacePoints.Add(Point);
				bFoundColumnTop = true;
			}
		}
	}

	OutSurfaceData.SurfaceAreaEstimate = OutSurfaceData.SurfacePoints.Num() * SurfacePointSpacing * SurfacePointSpacing;
	OutSurfaceData.bIsValid = true;

	// Diagnostic: log underground classification results
	int32 UGCount = 0;
	float MinUGZ = TNumericLimits<float>::Max();
	float MaxSurfZ = TNumericLimits<float>::Lowest();
	for (const FVoxelSurfacePoint& Pt : OutSurfaceData.SurfacePoints)
	{
		if (Pt.bIsUnderground)
		{
			++UGCount;
			MinUGZ = FMath::Min(MinUGZ, Pt.Position.Z);
		}
		else
		{
			MaxSurfZ = FMath::Max(MaxSurfZ, Pt.Position.Z);
		}
	}
	if (UGCount > 0)
	{
		UE_LOG(LogVoxelScatter, Verbose, TEXT("DIAG Extract chunk(%d,%d,%d): %d/%d underground. SurfMaxZ=%.0f, UGMinZ=%.0f (gap=%.0f)"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z,
			UGCount, OutSurfaceData.SurfacePoints.Num(),
			MaxSurfZ, MinUGZ, MaxSurfZ - MinUGZ);
	}
}

float FScatterExtractionContext::GetGeneratedTerrainHeight(double WorldX, double WorldY) const
{
	// The ladder's generated rung (code review CR2, SCAT-19): the natural height with the zones layered on as generation does.
	check(WorldMode != nullptr);
	return static_cast<float>(FVoxelSurfaceQuery::GetGeneratedSurface(*WorldMode, FVector(WorldX, WorldY, 0.0), NoiseParams,
		ConditioningZones.IsValid() ? ConditioningZones.Get() : nullptr).Position.Z);
}

FVoxelData FScatterExtractionContext::GetVoxelOutsideChunk(const FIntVector& LocalCoord, const FVector& WorldPos, float VoxelSize, int32 BiomeHint,
	const float* GeneratedHeight) const
{
	check(WorldMode != nullptr);
	// The voxel as generation makes it (the one single-voxel entry), on the surface the caller sampled for its column
	// where it has one (a heightmap scan judges a column by that height).
	FVoxelTerrainSurfacePoint ColumnSurface;
	const FVoxelTerrainSurfacePoint* Surface = nullptr;
	if (GeneratedHeight)
	{
		ColumnSurface.Position = FVector(WorldPos.X, WorldPos.Y, *GeneratedHeight);
		Surface = &ColumnSurface;
	}
	const FVoxelData Generated = FVoxelSurfaceQuery::GenerateVoxelAt(*WorldMode, WorldPos, VoxelSize, NoiseParams,
		ConditioningZones.IsValid() ? ConditioningZones.Get() : nullptr, CaveConfiguration, Sea, bEnableBiomes, BiomeHint, Surface);
	if (OutsideEdits.IsValid())
	{
		if (const FVoxelEdit* Edit = OutsideEdits->Find(LocalCoord))
		{
			return Edit->ApplyToProceduralData(Generated);
		}
	}
	return Generated;
}

void UVoxelScatterManager::ExtractChunkSurfacePoints(
	bool bCubic,
	const TArray<FVoxelData>& VoxelData,
	const FIntVector& ChunkCoord,
	const FVector& ChunkWorldOrigin,
	int32 ChunkSize,
	float VoxelSize,
	float PointSpacing,
	const TArray<FClearedScatterVolume>& ClearedVolumes,
	const FScatterExtractionContext& Context,
	FChunkSurfaceData& OutSurfaceData)
{
	if (bCubic)
	{
		ExtractSurfacePointsByBlockTop(VoxelData, ChunkCoord, ChunkWorldOrigin, ChunkSize, VoxelSize, ClearedVolumes, Context, OutSurfaceData);
	}
	else if (Context.WorldMode && Context.WorldMode->GetWorldModeType() == EWorldMode::SphericalPlanet)
	{
		// A planet scans along local up (spherical SP-G1).
		ExtractSurfacePointsByRay(VoxelData, ChunkCoord, ChunkWorldOrigin, ChunkSize, VoxelSize, PointSpacing, ClearedVolumes, Context,
			OutSurfaceData);
	}
	else
	{
		ExtractSurfacePointsByColumn(VoxelData, ChunkCoord, ChunkWorldOrigin, ChunkSize, VoxelSize, PointSpacing, ClearedVolumes,
			Context, OutSurfaceData);
	}
}

void UVoxelScatterManager::ExtractSurfacePointsByRay(
	const TArray<FVoxelData>& VoxelData,
	const FIntVector& ChunkCoord,
	const FVector& ChunkWorldOrigin,
	int32 ChunkSize,
	float VoxelSize,
	float PointSpacing,
	const TArray<FClearedScatterVolume>& ClearedVolumes,
	const FScatterExtractionContext& Context,
	FChunkSurfaceData& OutSurfaceData)
{
	OutSurfaceData = FChunkSurfaceData(ChunkCoord);
	OutSurfaceData.AveragePointSpacing = PointSpacing;

	const int32 CS = ChunkSize;
	if (VoxelData.Num() != CS * CS * CS || Context.WorldMode == nullptr || Context.WorldMode->GetWorldModeType() != EWorldMode::SphericalPlanet)
	{
		OutSurfaceData.bIsValid = false;
		return;
	}
	OutSurfaceData.bIsValid = true;
	const FSphericalPlanetParams& Params = static_cast<const FSphericalPlanetWorldMode*>(Context.WorldMode)->GetPlanetParams();
	const FVector Centre = Params.PlanetCenter;
	const double ChunkWorld = static_cast<double>(CS) * VoxelSize;

	// A chunk wholly under or over the planet's shell has no surface (and the grid's span below needs a chunk a few
	// degrees wide at most, which every chunk the shell reaches is).
	{
		float MinAltitude = 0.0f;
		float MaxAltitude = 0.0f;
		Context.WorldMode->GetAltitudeBounds(MinAltitude, MaxAltitude);
		const FVector Nearest(FMath::Clamp(Centre.X, ChunkWorldOrigin.X, ChunkWorldOrigin.X + ChunkWorld),
			FMath::Clamp(Centre.Y, ChunkWorldOrigin.Y, ChunkWorldOrigin.Y + ChunkWorld), FMath::Clamp(Centre.Z, ChunkWorldOrigin.Z, ChunkWorldOrigin.Z + ChunkWorld));
		double Farthest = 0.0;
		for (int32 I = 0; I < 8; ++I)
		{
			const FVector Corner = ChunkWorldOrigin + FVector(I & 1, (I >> 1) & 1, (I >> 2) & 1) * ChunkWorld;
			Farthest = FMath::Max(Farthest, FVector::Dist(Corner, Centre));
		}
		const double Margin = 2.0 * VoxelSize;
		if (FVector::Dist(Nearest, Centre) > Params.PlanetRadius + MaxAltitude + Margin || Farthest < Params.PlanetRadius + MinAltitude - Margin)
		{
			return;
		}
	}

	// One ray per cell of the cube-sphere grid at the stride (the column scan's columns at a planet's scale), the same
	// lattice for every chunk: neighbouring chunks' rays are one family, each crossing on a ray lies in exactly one chunk.
	const int32 Stride = FMath::Max(1, FMath::RoundToInt(PointSpacing / VoxelSize));
	const double CellSize = Stride * VoxelSize;
	const FVoxelCubeSphereGrid Grid(FVoxelCubeSphereGrid::CellsPerEdgeFor(Params.PlanetRadius, CellSize));
	FVector CornerDirections[8];
	for (int32 I = 0; I < 8; ++I)
	{
		CornerDirections[I] = (ChunkWorldOrigin + FVector(I & 1, (I >> 1) & 1, (I >> 2) & 1) * ChunkWorld - Centre).GetSafeNormal();
	}
	TArray<FIntPoint> Cells;
	Grid.GatherSpanned(MakeArrayView(CornerDirections, 8), Cells);

	FPlanetRayField Field(VoxelData, CS, ChunkWorldOrigin, VoxelSize, Context);
	// The ray through a cell from the planet's centre, in the chunk's voxel units: L(T) = LocalCentre + Direction * T.
	const FVector LocalCentre = (Centre - ChunkWorldOrigin) / VoxelSize;
	constexpr double OwnEpsilon = 1e-6;
	const double Threshold = static_cast<double>(VOXEL_SURFACE_THRESHOLD);
	OutSurfaceData.SurfacePoints.Reserve(Cells.Num() / 2);

	for (const FIntPoint& Cell : Cells)
	{
		const FVector Direction = Grid.CellCentreDirection(Cell);

		// The ray's span in the box [0, ChunkSize]^3.
		double TNear = 0.0;
		double TFar = TNumericLimits<double>::Max();
		bool bHit = true;
		for (int32 A = 0; A < 3 && bHit; ++A)
		{
			if (FMath::Abs(Direction[A]) < 1e-12)
			{
				bHit = LocalCentre[A] >= 0.0 && LocalCentre[A] <= CS;
				continue;
			}
			double T1 = -LocalCentre[A] / Direction[A];
			double T2 = (CS - LocalCentre[A]) / Direction[A];
			if (T1 > T2)
			{
				Swap(T1, T2);
			}
			TNear = FMath::Max(TNear, T1);
			TFar = FMath::Min(TFar, T2);
			bHit = TFar > TNear;
		}
		if (!bHit)
		{
			continue;
		}

		// Down the ray from where it enters the box (its outer end) cell by cell, a 3D DDA. S is the distance down from
		// there, in voxels.
		const FVector Down = -Direction;
		const FVector Start = LocalCentre + Direction * TFar;
		const double Length = TFar - TNear;
		FIntVector C(FMath::Clamp(FMath::FloorToInt32(Start.X), 0, CS - 1), FMath::Clamp(FMath::FloorToInt32(Start.Y), 0, CS - 1),
			FMath::Clamp(FMath::FloorToInt32(Start.Z), 0, CS - 1));
		int32 StepAxis[3];
		double SNext[3];
		double SDelta[3];
		for (int32 A = 0; A < 3; ++A)
		{
			if (Down[A] > 1e-12)
			{
				StepAxis[A] = 1;
				SNext[A] = (C[A] + 1 - Start[A]) / Down[A];
				SDelta[A] = 1.0 / Down[A];
			}
			else if (Down[A] < -1e-12)
			{
				StepAxis[A] = -1;
				SNext[A] = (C[A] - Start[A]) / Down[A];
				SDelta[A] = -1.0 / Down[A];
			}
			else
			{
				StepAxis[A] = 0;
				SNext[A] = TNumericLimits<double>::Max();
				SDelta[A] = TNumericLimits<double>::Max();
			}
		}

		// bSolid: whether the ray is in the ground at S. bCovered: whether a floor lies above S on the ray (or the ray
		// entered the chunk in the ground), so a lower crossing is underground.
		bool bFirst = true;
		bool bSolid = false;
		bool bCovered = false;
		bool bStopRay = false;
		double Omega = -1.0;
		double S = 0.0;
		double V[8];

		// A floor at SCross in cell C (corners V): the chunk's point when it lies in its box. Stops the ray at a cleared volume.
		auto EmitFloor = [&](double SCross)
		{
			const FVector LCross = Start + Down * SCross;
			const bool bOwned = LCross.X >= -OwnEpsilon && LCross.Y >= -OwnEpsilon && LCross.Z >= -OwnEpsilon
				&& LCross.X < CS - OwnEpsilon && LCross.Y < CS - OwnEpsilon && LCross.Z < CS - OwnEpsilon;
			const bool bUnder = bCovered;
			bCovered = true;
			if (!bOwned)
			{
				return;
			}
			const FVector WorldPos = ChunkWorldOrigin + LCross * VoxelSize;
			for (const FClearedScatterVolume& Volume : ClearedVolumes)
			{
				if (Volume.ContainsPoint(WorldPos))
				{
					bStopRay = true;
					return;
				}
			}

			const FVector CrossFraction = FPlanetRayField::CellFraction(LCross, C);
			FVector Gradient;
			FPlanetRayField::Sample(V, CrossFraction, &Gradient);
			FVector Normal = -Gradient;
			if (!Normal.Normalize())
			{
				Normal = Direction;
			}
			const FVoxelData& SolidVoxel = Field.NearestOwnCorner(C, CrossFraction, true);
			const FVoxelData& AirVoxel = Field.NearestOwnCorner(C, CrossFraction, false);
			const bool bAirIsAir = !AirVoxel.IsSolid();
			if (Omega < 0.0)
			{
				// The cell's solid angle: the quad of its corner directions (cells are small).
				const FVector D00 = Grid.DirectionAt(Cell, 0.0, 0.0);
				const FVector D11 = Grid.DirectionAt(Cell, 1.0, 1.0);
				const FVector D10 = Grid.DirectionAt(Cell, 1.0, 0.0);
				const FVector D01 = Grid.DirectionAt(Cell, 0.0, 1.0);
				Omega = 0.5 * FVector::CrossProduct(D11 - D00, D01 - D10).Size();
			}
			const double Radius = FVector::Dist(WorldPos, Centre);
			const double Along = FVector::DotProduct(Normal, Direction);

			FVoxelSurfacePoint Point;
			Point.Position = WorldPos;
			Point.Normal = Normal;
			Point.MaterialID = SolidVoxel.MaterialID;
			Point.BiomeID = SolidVoxel.BiomeID;
			Point.FaceType = Along > 0.5 ? EVoxelFaceType::Top : (Along < -0.5 ? EVoxelFaceType::Bottom : EVoxelFaceType::Side);
			Point.AmbientOcclusion = SolidVoxel.GetAO() & 0x03;
			Point.LocalUp = Direction;
			Point.ComputeSlopeAngle(Direction);
			Point.Altitude = Context.WorldMode->GetAltitude(WorldPos);
			Point.bHasAltitude = true;
			// The cell's area at the point over the column scan's area per point: the weighted density per area is the
			// column scan's on any part of the grid.
			Point.SpawnWeight = static_cast<float>(Omega * Radius * Radius / (CellSize * CellSize));
			Point.bIsUnderground = bUnder || (bAirIsAir && AirVoxel.HasUndergroundFlag()) || SolidVoxel.HasUndergroundFlag();
			const bool bBelowWater = Context.Sea.bEnabled && Context.Sea.GetDepth(WorldPos) > 0.0;
			Point.bIsUnderwater = ((bAirIsAir && AirVoxel.HasWaterFlag()) || bBelowWater) && !Point.bIsUnderground;
			OutSurfaceData.SurfacePoints.Add(Point);
		};

		while (!bStopRay)
		{
			const double SEnd = FMath::Min(FMath::Min3(SNext[0], SNext[1], SNext[2]), Length);
			const uint8 State = Field.GetCellState(C);
			if (State != 3)
			{
				// One sign over the whole cell (a trilinear interpolation stays between its corners). Entering it solid from
				// air (the previous cell's last sample a hair under the threshold, the surface through a voxel plane) is a
				// floor at the boundary.
				const bool bCellSolid = State == 2;
				if (bFirst)
				{
					bCovered = bCellSolid; // entering the chunk in the ground: the ray's surface lies above it
					bFirst = false;
				}
				else if (bCellSolid && !bSolid)
				{
					Field.GetCorners(C, V);
					EmitFloor(S);
				}
				bSolid = bCellSolid;
			}
			else
			{
				// Five samples over the cell's span; a rise through the threshold going down is a floor (the surface, or a
				// cave's floor under it).
				Field.GetCorners(C, V);
				double PrevF = 0.0;
				double PrevS = S;
				for (int32 J = 0; J <= 4 && !bStopRay; ++J)
				{
					const double SJ = S + (SEnd - S) * J / 4.0;
					const FVector Fraction = FPlanetRayField::CellFraction(Start + Down * SJ, C);
					const double F = FPlanetRayField::Sample(V, Fraction);
					const bool bJSolid = F >= Threshold;
					if (bFirst)
					{
						bCovered = bJSolid; // entering the chunk in the ground: the ray's surface lies above it
						bFirst = false;
					}
					else if (bJSolid && !bSolid)
					{
						// The crossing between the last sample and this one (at this sample when the last was in another cell).
						EmitFloor((J > 0 && FMath::Abs(F - PrevF) > UE_DOUBLE_SMALL_NUMBER)
							? PrevS + (Threshold - PrevF) / (F - PrevF) * (SJ - PrevS)
							: SJ);
					}
					bSolid = bJSolid;
					PrevF = F;
					PrevS = SJ;
				}
			}

			if (SEnd >= Length)
			{
				break;
			}
			// Into the next cell, across the boundary the ray reaches first.
			const int32 A = SNext[0] <= SNext[1] ? (SNext[0] <= SNext[2] ? 0 : 2) : (SNext[1] <= SNext[2] ? 1 : 2);
			C[A] += StepAxis[A];
			if (C[A] < 0 || C[A] >= CS)
			{
				break;
			}
			S = SNext[A];
			SNext[A] += SDelta[A];
		}
	}

	// The cells' area at the points (the rays are denser than the column scan's spacing away from the face centres).
	double Area = 0.0;
	for (const FVoxelSurfacePoint& Point : OutSurfaceData.SurfacePoints)
	{
		Area += Point.SpawnWeight;
	}
	OutSurfaceData.SurfaceAreaEstimate = static_cast<float>(Area * CellSize * CellSize);
}

void UVoxelScatterManager::ExtractSurfacePointsByBlockTop(
	const TArray<FVoxelData>& VoxelData,
	const FIntVector& ChunkCoord,
	const FVector& ChunkWorldOrigin,
	int32 ChunkSize,
	float VoxelSize,
	const TArray<FClearedScatterVolume>& ClearedVolumes,
	const FScatterExtractionContext& Context,
	FChunkSurfaceData& OutSurfaceData)
{
	OutSurfaceData = FChunkSurfaceData(ChunkCoord);
	OutSurfaceData.AveragePointSpacing = VoxelSize;

	const int32 ExpectedVoxels = ChunkSize * ChunkSize * ChunkSize;
	if (VoxelData.Num() != ExpectedVoxels)
	{
		OutSurfaceData.bIsValid = false;
		return;
	}

	// Surface points per exposed top face — scans all solid-air transitions per column.
	OutSurfaceData.SurfacePoints.Reserve(ChunkSize * ChunkSize);

	// A chunk whose top sits more than this far below the analytic terrain surface is buried
	// (its surface points are covered by terrain above). See ExtractSurfacePointsByColumn.
	const float CoverThreshold = VoxelSize * 1.5f;
	const float ChunkTopWorldZ = ChunkWorldOrigin.Z + ChunkSize * VoxelSize;

	for (int32 X = 0; X < ChunkSize; ++X)
	{
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			// Topmost transition in the column = open-sky surface; anything below it is
			// covered by solid above (cave floor / under an overhang) → underground. Closes
			// the same covered-floor gap as the smooth extractor (see ExtractSurfacePointsByColumn).
			bool bFoundColumnTop = false;

			// Generated terrain height at this column (conditioning zones included, G23) for the cross-chunk
			// coverage check (below).
			float ColumnTerrainHeight = 0.0f;
			bool bHaveColumnHeight = false;
			if (Context.WorldMode != nullptr && Context.WorldMode->IsHeightmapBased())
			{
				ColumnTerrainHeight = Context.GetGeneratedTerrainHeight(
					ChunkWorldOrigin.X + (X + 0.5f) * VoxelSize,
					ChunkWorldOrigin.Y + (Y + 0.5f) * VoxelSize);
				bHaveColumnHeight = true;
			}

			for (int32 Z = ChunkSize - 1; Z >= 0; --Z)
			{
				const int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
				const FVoxelData& Voxel = VoxelData[Index];

				if (!Voxel.IsSolid())
				{
					continue;
				}

				// Check air above
				bool bAirAboveIsUnderground = false;
				bool bAirAboveIsWater = false;
				if (Z + 1 < ChunkSize)
				{
					const int32 AboveIndex = X + Y * ChunkSize + (Z + 1) * ChunkSize * ChunkSize;
					const FVoxelData& AboveVoxel = VoxelData[AboveIndex];
					if (AboveVoxel.IsSolid())
					{
						continue; // Not a surface
					}
					bAirAboveIsUnderground = AboveVoxel.HasUndergroundFlag();
					bAirAboveIsWater = AboveVoxel.HasWaterFlag();
				}
				else if (bHaveColumnHeight)
				{
					// The block over the top layer is the chunk above's, sampled at this column's corner (G26, G28, see
					// ExtractSurfacePointsByColumn): solid where the ground continues above, caves included, decided at
					// any depth.
					const float SampleHeight = Context.GetGeneratedTerrainHeight(ChunkWorldOrigin.X + X * VoxelSize, ChunkWorldOrigin.Y + Y * VoxelSize);
					if (Context.GetVoxelOutsideChunk(FIntVector(X, Y, ChunkSize), ChunkWorldOrigin + FVector(X * VoxelSize, Y * VoxelSize, ChunkSize * VoxelSize),
							VoxelSize, Voxel.BiomeID, &SampleHeight).IsSolid())
					{
						bFoundColumnTop = true; // the ground above covers the rest of the column
						continue;
					}
				}

				// Block face center: center of block XY, top of block Z
				const FVector WorldPos(
					ChunkWorldOrigin.X + (X + 0.5f) * VoxelSize,
					ChunkWorldOrigin.Y + (Y + 0.5f) * VoxelSize,
					ChunkWorldOrigin.Z + (Z + 1.0f) * VoxelSize
				);

				// Check cleared volumes
				bool bInClearedVolume = false;
				for (const FClearedScatterVolume& Volume : ClearedVolumes)
				{
					if (Volume.ContainsPoint(WorldPos))
					{
						bInClearedVolume = true;
						break;
					}
				}
				if (bInClearedVolume)
				{
					break; // Skip rest of this column
				}

				// Cubic: normal is always up, face type is always Top
				FVoxelSurfacePoint Point;
				Point.Position = WorldPos;
				Point.Normal = FVector::UpVector;
				Point.MaterialID = Voxel.MaterialID;
				Point.BiomeID = Voxel.BiomeID;
				Point.FaceType = EVoxelFaceType::Top;
				Point.AmbientOcclusion = Voxel.GetAO() & 0x03;
				Point.SlopeAngle = 0.0f; // Flat top face

				// Underground if flagged, OR not the topmost surface in the column (covered within this
				// chunk), OR the whole chunk is buried below the analytic terrain surface (covered by a
				// neighboring chunk). The chunk-level test is edit-independent (see smooth extractor).
				const bool bCoveredByTerrain = bHaveColumnHeight && (ChunkTopWorldZ < ColumnTerrainHeight - CoverThreshold);
				Point.bIsUnderground = bAirAboveIsUnderground || Voxel.HasUndergroundFlag() || bFoundColumnTop || bCoveredByTerrain;

				// Submerged: open water above (water flag) OR below the world water level (catches
				// seabeds the water flag missed). Underground takes precedence.
				const bool bBelowWater = Context.Sea.bEnabled && Context.Sea.GetDepth(WorldPos) > 0.0;
				Point.bIsUnderwater = (bAirAboveIsWater || bBelowWater) && !Point.bIsUnderground;

				OutSurfaceData.SurfacePoints.Add(Point);
				bFoundColumnTop = true;
			}
		}
	}

	OutSurfaceData.SurfaceAreaEstimate = OutSurfaceData.SurfacePoints.Num() * VoxelSize * VoxelSize;
	OutSurfaceData.bIsValid = true;
}

FString UVoxelScatterManager::ComputeScatterDigest(TArray<FString>* OutChunkLines) const
{
	TArray<FIntVector> Chunks;
	SpawnCache.GetKeys(Chunks);
	Chunks.Sort([](const FIntVector& A, const FIntVector& B)
	{
		return A.X != B.X ? A.X < B.X : (A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z);
	});
	TMap<int32, TPair<int32, uint32>> PerType; // type -> (count, crc)
	int32 Points = 0;
	for (const FIntVector& Chunk : Chunks)
	{
		const TArray<FScatterSpawnPoint>& SpawnPoints = SpawnCache.FindChecked(Chunk).SpawnPoints;
		uint32 ChunkCrc = 0u;
		for (const FScatterSpawnPoint& Point : SpawnPoints)
		{
			const int32 Key[9] = {
				Point.ScatterTypeID,
				FMath::RoundToInt32(Point.Position.X * 10.0), FMath::RoundToInt32(Point.Position.Y * 10.0), FMath::RoundToInt32(Point.Position.Z * 10.0),
				FMath::RoundToInt32(Point.RotationYaw * 100.0f), FMath::RoundToInt32(Point.Scale * 1000.0f),
				FMath::RoundToInt32(Point.LocalUp.X * 10000.0), FMath::RoundToInt32(Point.LocalUp.Y * 10000.0), FMath::RoundToInt32(Point.LocalUp.Z * 10000.0) };
			TPair<int32, uint32>& Entry = PerType.FindOrAdd(Point.ScatterTypeID, TPair<int32, uint32>(0, 0u));
			++Entry.Key;
			Entry.Value = FCrc::MemCrc32(Key, sizeof(Key), Entry.Value);
			ChunkCrc = FCrc::MemCrc32(Key, sizeof(Key), ChunkCrc);
			++Points;
		}
		if (OutChunkLines)
		{
			OutChunkLines->Add(FString::Printf(TEXT("%d,%d,%d,%d,%08x"), Chunk.X, Chunk.Y, Chunk.Z, SpawnPoints.Num(), ChunkCrc));
		}
	}
	TArray<int32> Types;
	PerType.GetKeys(Types);
	Types.Sort();
	FString Out = FString::Printf(TEXT("types=%d chunks=%d points=%d"), Types.Num(), Chunks.Num(), Points);
	for (const int32 Type : Types)
	{
		const TPair<int32, uint32>& Entry = PerType.FindChecked(Type);
		Out += FString::Printf(TEXT(" | type %d: %d crc %08x"), Type, Entry.Key, Entry.Value);
	}
	return Out;
}

FString UVoxelScatterManager::ComputeCacheResidency(TArray<FString>* OutMismatchLines) const
{
	const bool bRenderer = ScatterRenderer && ScatterRenderer->IsInitialized();
	int32 ScatterOnly = 0, Mismatch = 0, DuplicateChunks = 0;
	int64 SpawnPoints = 0, Instances = 0, Duplicates = 0;
	for (const TPair<FIntVector, FChunkScatterData>& Pair : SpawnCache)
	{
		ScatterOnly += SurfaceCache.Contains(Pair.Key) ? 0 : 1;
		const int32 ChunkInstances = bRenderer ? ScatterRenderer->GetChunkInstanceCount(Pair.Key) : 0;
		SpawnPoints += Pair.Value.SpawnPoints.Num();
		Instances += ChunkInstances;

		// Two spawn points of one type at one position (to 0.1 uu, as the digest keys them): a pass appended twice.
		TSet<TTuple<int32, int64, int64, int64>> Seen;
		Seen.Reserve(Pair.Value.SpawnPoints.Num());
		int32 ChunkDuplicates = 0;
		for (const FScatterSpawnPoint& Point : Pair.Value.SpawnPoints)
		{
			bool bAlready = false;
			Seen.Add(MakeTuple(Point.ScatterTypeID, FMath::RoundToInt64(Point.Position.X * 10.0), FMath::RoundToInt64(Point.Position.Y * 10.0),
				FMath::RoundToInt64(Point.Position.Z * 10.0)), &bAlready);
			ChunkDuplicates += bAlready ? 1 : 0;
		}
		Duplicates += ChunkDuplicates;
		DuplicateChunks += ChunkDuplicates > 0 ? 1 : 0;
		if (ChunkInstances == Pair.Value.SpawnPoints.Num())
		{
			continue;
		}
		++Mismatch;
		if (OutMismatchLines && bRenderer)
		{
			// Per type: spawn points / instances, and '!' where the renderer does not track the type for the chunk (its
			// instances would survive the chunk's unload).
			const TArray<int32> Tracked = ScatterRenderer->GetChunkScatterTypes(Pair.Key);
			FString Types;
			for (const FScatterDefinition& Def : ScatterDefinitions)
			{
				int32 TypeSpawns = 0;
				for (const FScatterSpawnPoint& Point : Pair.Value.SpawnPoints)
				{
					TypeSpawns += Point.ScatterTypeID == Def.ScatterID ? 1 : 0;
				}
				const int32 TypeInstances = ScatterRenderer->GetChunkInstanceCount(Pair.Key, Def.ScatterID);
				if (TypeSpawns != TypeInstances)
				{
					Types += FString::Printf(TEXT(" %d:%d/%d%s"), Def.ScatterID, TypeSpawns, TypeInstances,
						Tracked.Contains(Def.ScatterID) || TypeInstances == 0 ? TEXT("") : TEXT("!"));
				}
			}
			OutMismatchLines->Add(FString::Printf(TEXT("%d,%d,%d,%d,%d,%s,%s"), Pair.Key.X, Pair.Key.Y, Pair.Key.Z, Pair.Value.SpawnPoints.Num(),
				ChunkInstances, SurfaceCache.Contains(Pair.Key) ? TEXT("surface") : TEXT("noSurface"), *Types.TrimStart()));
		}
	}
	return FString::Printf(TEXT("scatterChunks=%d surfaceChunks=%d scatterOnly=%d spawnPoints=%lld instances=%lld instanceMismatch=%d duplicates=%lld (%d chunks)"),
		SpawnCache.Num(), SurfaceCache.Num(), ScatterOnly, SpawnPoints, Instances, Mismatch, Duplicates, DuplicateChunks);
}

FVector UVoxelScatterManager::GetChunkWorldOrigin(const FIntVector& ChunkCoord) const
{
	if (!Configuration)
	{
		return FVector::ZeroVector;
	}

	const float ChunkWorldSize = Configuration->GetChunkWorldSize();
	return Configuration->WorldOrigin + FVector(ChunkCoord) * ChunkWorldSize;
}

FScatterExtractionContext UVoxelScatterManager::MakeExtractionContext(const FIntVector& ChunkCoord) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelScatter_MakeExtractionContext);
	FScatterExtractionContext Context;
	Context.WorldMode = WorldMode;
	if (Configuration)
	{
		Context.NoiseParams = Configuration->GetNoiseParams();
		Context.Sea = FVoxelSeaSurface::FromConfig(*Configuration); // bEnabled follows bEnableWaterLevel
		// The voxels a chunk's extraction reads past its own data are carved as generation carves them (G28, G72).
		Context.CaveConfiguration = Configuration->bEnableCaves ? Configuration->CaveConfiguration.Get() : nullptr;
		Context.bEnableBiomes = Configuration->bEnableBiomes;
		// The conditioning zones over the chunk, so the analytic surface is the generated one: the buried-chunk test's
		// height on a heightmap (G23), the voxels past a planet chunk's faces (spherical SP-B10).
		if (ConditioningGatherer)
		{
			const FVector Min = GetChunkWorldOrigin(ChunkCoord);
			TArray<FVoxelConditioningZone> Zones;
			ConditioningGatherer(FBox(Min, Min + FVector(Configuration->GetChunkWorldSize())), Zones);
			if (Zones.Num() > 0)
			{
				Context.ConditioningZones = MakeShared<const TArray<FVoxelConditioningZone>>(MoveTemp(Zones));
			}
		}
		// The edits in the voxels the extraction reads past the chunk: the chunk above's bottom layer on a heightmap world
		// (the voxels over this chunk's top layer, G26); on a planet the layers of the neighbours past the chunk's +X, +Y
		// and +Z faces, which its last cell on each axis reaches (G29, G31).
		if (EditLayerSource && WorldMode != nullptr)
		{
			Context.OutsideEdits = WorldMode->GetWorldModeType() == EWorldMode::SphericalPlanet
				? GatherPlanetTouchingEdits(ChunkCoord, Configuration->ChunkSize, EditLayerSource)
				: GatherAboveLayerEdits(ChunkCoord, Configuration->ChunkSize, EditLayerSource);
		}
	}
	return Context;
}

void UVoxelScatterManager::GetPlanetShellNeighbours(TArray<FIntVector>& OutOffsets)
{
	for (int32 I = 1; I < 8; ++I)
	{
		OutOffsets.AddUnique(FIntVector(I & 1, (I >> 1) & 1, (I >> 2) & 1));
	}
}

TSharedPtr<const TMap<FIntVector, FVoxelEdit>> UVoxelScatterManager::GatherAboveLayerEdits(const FIntVector& ChunkCoord, int32 ChunkSize,
	TFunctionRef<const FChunkEditLayer*(const FIntVector&)> EditLayerSource)
{
	const FChunkEditLayer* AboveLayer = EditLayerSource(ChunkCoord + FIntVector(0, 0, 1));
	if (!AboveLayer)
	{
		return nullptr;
	}
	TMap<FIntVector, FVoxelEdit> BottomEdits;
	for (const TPair<int32, FVoxelEdit>& Pair : AboveLayer->Edits)
	{
		const FIntVector& Local = Pair.Value.LocalPosition;
		if (Local.Z == 0 && Local.X >= 0 && Local.X < ChunkSize && Local.Y >= 0 && Local.Y < ChunkSize)
		{
			BottomEdits.Add(FIntVector(Local.X, Local.Y, ChunkSize), Pair.Value);
		}
	}
	if (BottomEdits.Num() == 0)
	{
		return nullptr;
	}
	return MakeShared<const TMap<FIntVector, FVoxelEdit>>(MoveTemp(BottomEdits));
}

TSharedPtr<const TMap<FIntVector, FVoxelEdit>> UVoxelScatterManager::GatherPlanetTouchingEdits(const FIntVector& ChunkCoord, int32 ChunkSize,
	TFunctionRef<const FChunkEditLayer*(const FIntVector&)> EditLayerSource)
{
	TArray<FIntVector> Neighbours;
	GetPlanetShellNeighbours(Neighbours);
	TMap<FIntVector, FVoxelEdit> Touching;
	for (const FIntVector& Offset : Neighbours)
	{
		const FChunkEditLayer* Layer = EditLayerSource(ChunkCoord + Offset);
		if (!Layer)
		{
			continue;
		}
		for (const TPair<int32, FVoxelEdit>& Pair : Layer->Edits)
		{
			// The neighbour's voxel in this chunk's frame; its layer touching the chunk lies one voxel outside it.
			const FIntVector Local = Pair.Value.LocalPosition + Offset * ChunkSize;
			if (Local.GetMin() >= -1 && Local.GetMax() <= ChunkSize)
			{
				Touching.Add(Local, Pair.Value);
			}
		}
	}
	if (Touching.Num() == 0)
	{
		return nullptr;
	}
	return MakeShared<const TMap<FIntVector, FVoxelEdit>>(MoveTemp(Touching));
}

void UVoxelScatterManager::GetEditReaderOffsets(bool bPlanet, const FIntVector* EditMin, TArray<FIntVector>& OutOffsets)
{
	// A heightmap chunk reads the voxels over its top layer: the bottom layer (local Z 0) of the chunk above (G26).
	if (!bPlanet)
	{
		if (!EditMin || EditMin->Z == 0)
		{
			OutOffsets.AddUnique(FIntVector(0, 0, -1));
		}
		return;
	}
	// A planet chunk reads one layer past its +X, +Y and +Z faces, in the neighbours at the offsets D of
	// GetPlanetShellNeighbours: the neighbour's local 0 on every axis where D is 1 (G29, G31). So the chunk at -D reads this
	// one's voxels there.
	{
		TArray<FIntVector> Shell;
		GetPlanetShellNeighbours(Shell);
		for (const FIntVector& D : Shell)
		{
			const bool bReached = !EditMin
				|| ((D.X == 0 || EditMin->X == 0) && (D.Y == 0 || EditMin->Y == 0) && (D.Z == 0 || EditMin->Z == 0));
			if (bReached)
			{
				OutOffsets.AddUnique(FIntVector::ZeroValue - D);
			}
		}
	}
}

void UVoxelScatterManager::GetEditReaders(const FIntVector& EditedChunk, const FIntVector* EditMin, TArray<FIntVector>& OutChunks) const
{
	// The same conditions under which MakeExtractionContext reads a neighbour's edits.
	if (!EditLayerSource || WorldMode == nullptr)
	{
		return;
	}
	TArray<FIntVector> Offsets;
	GetEditReaderOffsets(WorldMode->GetWorldModeType() == EWorldMode::SphericalPlanet, EditMin, Offsets);
	for (const FIntVector& Offset : Offsets)
	{
		OutChunks.Add(EditedChunk + Offset);
	}
}

UVoxelScatterManager::FFreshExtractionStats UVoxelScatterManager::CompareWithFreshExtraction(
	TFunctionRef<TSharedPtr<const TArray<FVoxelData>>(const FIntVector&)> VoxelSource, TArray<FFreshExtractionComparison>& OutStale) const
{
	FFreshExtractionStats Stats;
	if (!bIsInitialized || !Configuration)
	{
		return Stats;
	}
	const int32 CS = Configuration->ChunkSize;
	const float VoxelSize = Configuration->VoxelSize;
	const bool bCubic = Configuration->MeshingMode == EMeshingMode::Cubic;
	const FVector WorldOrigin = Configuration->WorldOrigin;
	const double ChunkWorldSize = Configuration->GetChunkWorldSize();

	TArray<FIntVector> Chunks;
	SurfaceCache.GetKeys(Chunks);
	Chunks.Sort([](const FIntVector& A, const FIntVector& B)
	{
		return A.X != B.X ? A.X < B.X : (A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z);
	});

	// One point as the comparison keys it: position to 0.1 uu, normal to 1e-4, material and classification.
	auto PointKey = [](const FVoxelSurfacePoint& P) -> FString
	{
		return FString::Printf(TEXT("%lld %lld %lld %d %d %d %d %d %d"),
			FMath::RoundToInt64(P.Position.X * 10.0), FMath::RoundToInt64(P.Position.Y * 10.0), FMath::RoundToInt64(P.Position.Z * 10.0),
			FMath::RoundToInt32(P.Normal.X * 10000.0), FMath::RoundToInt32(P.Normal.Y * 10000.0), FMath::RoundToInt32(P.Normal.Z * 10000.0),
			static_cast<int32>(P.MaterialID), P.bIsUnderground ? 1 : 0, P.bIsUnderwater ? 1 : 0);
	};

	// In batches, so only a batch's voxel snapshots and contexts are alive at once (a 64^3 chunk's voxels are a megabyte).
	constexpr int32 BatchSize = 64;
	struct FJob
	{
		FIntVector Coord = FIntVector::ZeroValue;
		TSharedPtr<const TArray<FVoxelData>> Voxels;
		FScatterExtractionContext Context;
		FChunkSurfaceData Fresh;
	};
	const TArray<FClearedScatterVolume> NoClearedVolumes;
	const float Spacing = SurfacePointSpacing;
	for (int32 Start = 0; Start < Chunks.Num(); Start += BatchSize)
	{
		TArray<FJob> Jobs;
		for (int32 I = Start; I < FMath::Min(Start + BatchSize, Chunks.Num()); ++I)
		{
			const FIntVector& Coord = Chunks[I];
			if (PendingQueueSet.Contains(Coord) || AsyncScatterInProgress.Contains(Coord) || DeferredSupplementalPasses.Contains(Coord))
			{
				++Stats.SkippedPending;
				continue;
			}
			if (ClearedVolumesPerChunk.Contains(Coord))
			{
				++Stats.SkippedCleared;
				continue;
			}
			TSharedPtr<const TArray<FVoxelData>> Voxels = VoxelSource(Coord);
			if (!Voxels.IsValid() || Voxels->Num() != CS * CS * CS)
			{
				++Stats.SkippedNoVoxels;
				continue;
			}
			FJob& Job = Jobs.AddDefaulted_GetRef();
			Job.Coord = Coord;
			Job.Voxels = MoveTemp(Voxels);
			Job.Context = MakeExtractionContext(Coord);
		}

		ParallelFor(Jobs.Num(), [&](int32 J)
		{
			FJob& Job = Jobs[J];
			const FVector Origin = WorldOrigin + FVector(Job.Coord) * ChunkWorldSize;
			ExtractChunkSurfacePoints(bCubic, *Job.Voxels, Job.Coord, Origin, CS, VoxelSize, Spacing, NoClearedVolumes, Job.Context, Job.Fresh);
		});

		for (const FJob& Job : Jobs)
		{
			++Stats.Compared;
			const FChunkSurfaceData& Cached = SurfaceCache.FindChecked(Job.Coord);
			// The fresh pass as the pipeline would cache it: no points when it found none (the cache then keeps no entry).
			const bool bFreshValid = Job.Fresh.bIsValid && Job.Fresh.SurfacePoints.Num() > 0;
			TMap<FString, int32> Count;
			for (const FVoxelSurfacePoint& P : Cached.SurfacePoints)
			{
				++Count.FindOrAdd(PointKey(P));
			}
			if (bFreshValid)
			{
				for (const FVoxelSurfacePoint& P : Job.Fresh.SurfacePoints)
				{
					--Count.FindOrAdd(PointKey(P));
				}
			}
			int32 Changed = 0;
			for (const TPair<FString, int32>& Pair : Count)
			{
				Changed += FMath::Abs(Pair.Value);
			}
			if (Changed > 0)
			{
				++Stats.Stale;
				FFreshExtractionComparison& Out = OutStale.AddDefaulted_GetRef();
				Out.ChunkCoord = Job.Coord;
				Out.CachedPoints = Cached.SurfacePoints.Num();
				Out.FreshPoints = bFreshValid ? Job.Fresh.SurfacePoints.Num() : 0;
				Out.ChangedPoints = Changed;
			}
		}
	}
	OutStale.Sort([](const FFreshExtractionComparison& A, const FFreshExtractionComparison& B) { return A.ChangedPoints > B.ChangedPoints; });
	return Stats;
}

