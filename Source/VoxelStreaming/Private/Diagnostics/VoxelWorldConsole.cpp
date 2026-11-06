// Copyright Daniel Raquel. All Rights Reserved.

// The voxel world's console commands: the world, far-field and far-ocean reports, the biome coverage, the residency
// audit and the in-place re-mesh. Each reads a chunk manager through its public API; none holds state of its own.

#include "VoxelChunkManager.h"
#include "VoxelStreaming.h"
#include "Diagnostics/VoxelConsoleHelpers.h"
#include "VoxelTreeInjector.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelWorldConfiguration.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "UObject/UObjectIterator.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"

static FAutoConsoleCommandWithWorld GVoxelFarFieldStatsCmd(
	TEXT("voxel.World.FarFieldStats"),
	TEXT("Log the far field's state for every live chunk manager (patches, drawn, hidden by the voxel window, builds, memory)."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld*)
	{
		for (TObjectIterator<UVoxelChunkManager> It; It; ++It)
		{
			if (It->GetWorld() && It->GetWorld()->IsGameWorld())
			{
				UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.World.FarFieldStats: %s"), *It->GetFarFieldDebugString());
			}
		}
	}));

static FAutoConsoleCommandWithWorld GVoxelWorldStatsCmd(
	TEXT("voxel.World.Stats"),
	TEXT("Log the voxel world's report for every live chunk manager: the world's bounds and sea (a planet's radius, shell, orbit radius and sea sphere; a heightmap world's mode, bounds, terrain Z and far ocean), the voxel window's reach from the streaming viewer with the far field, and the cut-over and descent prefetch. vox.Planet / vox.World print the same lines and add the pawn's."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld*)
	{
		for (TObjectIterator<UVoxelChunkManager> It; It; ++It)
		{
			if (It->GetWorld() && It->GetWorld()->IsGameWorld())
			{
				TArray<FString> Lines;
				It->GetWorldReport(FVoxelWorldReportNotes(), Lines);
				for (const FString& Line : Lines)
				{
					UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.World.Stats: %s"), *Line);
				}
			}
		}
	}));

static FAutoConsoleCommandWithWorld GVoxelFarOceanStatsCmd(
	TEXT("voxel.World.FarOceanStats"),
	TEXT("Log the far ocean's state for every live chunk manager (open columns, grid quads, rebuilds, extent)."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld*)
	{
		for (TObjectIterator<UVoxelChunkManager> It; It; ++It)
		{
			if (It->GetWorld() && It->GetWorld()->IsGameWorld())
			{
				UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.World.FarOceanStats: %s"), *It->GetFarOceanDebugString());
			}
		}
	}));

// Console: voxel.World.BiomeCoverage
// Each biome's share of the land: the natural surface (QuerySurfacePoint, the analytic rung, nothing loaded needed) at
// samples x samples points over the world (an island's footprint box, the points inside it; a plane's square of the
// radius around the streaming viewer; a planet's sphere on a Fibonacci lattice, equal areas), above the sea counted as
// land. One line per biome: its share of the land and of the samples, its mean and highest surface. The tool the biome
// boxes and the climate options are tuned with (SMOOTH_TERRAIN_BIOMES_PLAN.md: every biome 5-20 % of the land).
static FAutoConsoleCommandWithWorldAndArgs GVoxelBiomeCoverageCmd(
	TEXT("voxel.World.BiomeCoverage"),
	TEXT("Log each biome's share of the land over the world: voxel.World.BiomeCoverage [samples=160] [radius=200000 (a plane's square around the viewer)]."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		UWorld* TargetWorld = VoxelEditAuditHelpers::FindGameWorld(World);
		if (!TargetWorld)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.World.BiomeCoverage: no PIE/Game world found (start PIE first)"));
			return;
		}
		const int32 Samples = FMath::Clamp(Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 160, 8, 1024);
		const double PlaneRadius = Args.Num() > 1 ? FCString::Atod(*Args[1]) : 200000.0;
		for (UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(TargetWorld))
		{
			const UVoxelWorldConfiguration* Config = CM ? CM->GetConfiguration() : nullptr;
			if (!Config || !CM->GetWorldMode())
			{
				continue;
			}
			const FVoxelWorldBounds Bounds = CM->GetWorldBounds();
			const FVoxelSeaSurface Sea = CM->GetSeaSurface();
			const bool bPlanet = Bounds.Shape == EVoxelWorldBoundsShape::Shell;

			// The sample points on the up-lines: the lattice on the sphere, else a grid over the footprint's box
			TArray<FVector> Points;
			Points.Reserve(Samples * Samples);
			if (bPlanet)
			{
				const int32 Count = Samples * Samples;
				const double Golden = UE_DOUBLE_PI * (3.0 - FMath::Sqrt(5.0));
				const double Radius = Config->WorldRadius;
				for (int32 i = 0; i < Count; ++i)
				{
					const double Z = 1.0 - (2.0 * i + 1.0) / Count;
					const double R = FMath::Sqrt(FMath::Max(0.0, 1.0 - Z * Z));
					Points.Add(Bounds.Center + FVector(FMath::Cos(Golden * i) * R, FMath::Sin(Golden * i) * R, Z) * Radius);
				}
			}
			else
			{
				const FVector Viewer = CM->GetStreamingViewerPosition();
				const FVector2D Centre = Bounds.IsBounded() ? FVector2D(Bounds.Center) : FVector2D(Viewer);
				const FVector2D Half = Bounds.IsBounded() ? Bounds.HalfExtent : FVector2D(PlaneRadius, PlaneRadius);
				for (int32 j = 0; j < Samples; ++j)
				{
					for (int32 i = 0; i < Samples; ++i)
					{
						const double X = Centre.X - Half.X + (i + 0.5) * 2.0 * Half.X / Samples;
						const double Y = Centre.Y - Half.Y + (j + 0.5) * 2.0 * Half.Y / Samples;
						if (Bounds.ContainsXY(X, Y))
						{
							Points.Add(FVector(X, Y, 0.0));
						}
					}
				}
			}

			struct FBiomeCount { int32 Land = 0; int32 All = 0; double AltitudeSum = 0.0; float AltitudeMax = -MAX_FLT; };
			TMap<int32, FBiomeCount> Counts;
			int32 Land = 0;
			int32 Sampled = 0;
			for (const FVector& Point : Points)
			{
				FVoxelSurfaceSample Surface;
				if (!CM->QuerySurfacePoint(Point, EVoxelSurfaceLayer::Natural, Surface))
				{
					continue;
				}
				++Sampled;
				FBiomeCount& C = Counts.FindOrAdd(Surface.BiomeID);
				++C.All;
				if (Sea.bEnabled && Sea.IsBelow(Surface.Position))
				{
					continue;
				}
				++Land;
				++C.Land;
				const float Altitude = bPlanet ? Surface.Altitude : static_cast<float>(Surface.Position.Z);
				C.AltitudeSum += Altitude;
				C.AltitudeMax = FMath::Max(C.AltitudeMax, Altitude);
			}

			const UVoxelBiomeConfiguration* Biomes = Config->BiomeConfiguration;
			UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.World.BiomeCoverage: %s, %d samples, land %.1f%%"), *Config->GetName(), Sampled,
				Sampled > 0 ? 100.0 * Land / Sampled : 0.0);
			Counts.KeySort(TLess<int32>());
			for (const TPair<int32, FBiomeCount>& Pair : Counts)
			{
				FString Name = TEXT("?");
				if (Biomes)
				{
					for (const FBiomeDefinition& Biome : Biomes->Biomes)
					{
						if (Biome.BiomeID == Pair.Key)
						{
							Name = Biome.Name;
						}
					}
				}
				const FBiomeCount& C = Pair.Value;
				UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.World.BiomeCoverage:   %2d %-18s land %5.1f%%  all %5.1f%%  surface mean %7.1f m  max %7.1f m"),
					Pair.Key, *Name, Land > 0 ? 100.0 * C.Land / Land : 0.0, Sampled > 0 ? 100.0 * C.All / Sampled : 0.0,
					C.Land > 0 ? C.AltitudeSum / C.Land / 100.0 : 0.0, C.Land > 0 ? C.AltitudeMax / 100.0 : 0.0);
			}
		}
	}));

// Console: voxel.World.HeightProfile
// The natural surface along a line (QuerySurfacePoint, the analytic rung): its height (world Z, or the altitude on a
// planet), biome and material at each of n points from (x0, y0) to (x1, y1). For tuning a world's shape and for
// looking at a crease or a step numerically rather than in a screenshot.
static FAutoConsoleCommandWithWorldAndArgs GVoxelHeightProfileCmd(
	TEXT("voxel.World.HeightProfile"),
	TEXT("Log the natural surface along a line: voxel.World.HeightProfile x0 y0 x1 y1 [points=41] (a planet: the up-lines through those points at z 0)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		UWorld* TargetWorld = VoxelEditAuditHelpers::FindGameWorld(World);
		if (!TargetWorld || Args.Num() < 4)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.World.HeightProfile: start PIE and pass x0 y0 x1 y1 [points]"));
			return;
		}
		const FVector2D A(FCString::Atod(*Args[0]), FCString::Atod(*Args[1]));
		const FVector2D B(FCString::Atod(*Args[2]), FCString::Atod(*Args[3]));
		const int32 Points = FMath::Clamp(Args.Num() > 4 ? FCString::Atoi(*Args[4]) : 41, 2, 2001);
		for (UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(TargetWorld))
		{
			if (!CM || !CM->GetWorldMode())
			{
				continue;
			}
			const bool bPlanet = CM->GetWorldBounds().Shape == EVoxelWorldBoundsShape::Shell;
			for (int32 i = 0; i < Points; ++i)
			{
				const FVector2D P = FMath::Lerp(A, B, static_cast<double>(i) / (Points - 1));
				FVoxelSurfaceSample Surface;
				if (CM->QuerySurfacePoint(FVector(P.X, P.Y, 0.0), EVoxelSurfaceLayer::Natural, Surface))
				{
					UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.World.HeightProfile: %4d (%10.1f, %10.1f) height %9.1f biome %3d material %3d slope %5.1f"),
						i, P.X, P.Y, bPlanet ? Surface.Altitude : Surface.Position.Z, Surface.BiomeID, Surface.MaterialID, Surface.SlopeDegrees);
				}
			}
		}
	}));

// Console: voxel.Mesh.RemeshAll
// Re-mesh every loaded chunk in place and rebuild every seam (UVoxelChunkManager::RemeshAllLoadedChunks), so a mesher
// cvar changed mid-session (the meshers read theirs per job) shows while the LOD stays where it is; with streaming paused
// too, the usual way to hold a scene for an A/B (G115).
static FAutoConsoleCommandWithWorldAndArgs GVoxelRemeshAllCmd(
	TEXT("voxel.Mesh.RemeshAll"),
	TEXT("Re-mesh all loaded voxel chunks in place (apply a live mesher CVar change without moving LOD)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		UWorld* TargetWorld = (World && (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game)) ? World : nullptr;
		if (!TargetWorld && GEngine)
		{
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				if (Ctx.World() && (Ctx.WorldType == EWorldType::PIE || Ctx.WorldType == EWorldType::Game))
				{
					TargetWorld = Ctx.World();
					break;
				}
			}
		}
		if (!TargetWorld)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Mesh.RemeshAll: no PIE/Game world found (start PIE first)"));
			return;
		}

		int32 Managers = 0;
		int32 Paused = 0;
		int32 TotalDirtied = 0;
		for (UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(TargetWorld))
		{
			if (!CM) { continue; }
			++Managers;
			Paused += CM->IsStreamingEnabled() ? 0 : 1;
			TotalDirtied += CM->RemeshAllLoadedChunks();
		}
		UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Mesh.RemeshAll: re-meshing %d chunk(s) across %d manager(s)%s"), TotalDirtied, Managers,
			Paused > 0 ? TEXT(" (streaming paused: these chunks and their seams alone re-mesh)") : TEXT(""));
	}));

// Console: voxel.Stream.AuditResidency
// One-line QA check for "a distant chunk that should have unloaded is still visible" (R23): chunks
// tracked beyond the unload radius (by state), renderer chunks and seam buckets with no tracked
// owner, and async jobs stuck past voxel.Stream.MeshStuckSeconds. findings=0 after streaming has
// settled = healthy. Logged at Warning on LogVoxelStreaming; read it with unreal-mcp
// LogsToolset.GetLogEntries (Claudius get_output_log fails while PIE holds the log file open).
static FAutoConsoleCommandWithWorldAndArgs GVoxelAuditResidencyCmd(
	TEXT("voxel.Stream.AuditResidency"),
	TEXT("Audit voxel chunk residency: tracked chunks beyond the unload radius by state, renderer chunks and seam buckets ")
	TEXT("with no tracked owner, and stuck async jobs. findings=0 after streaming settles = healthy."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		UWorld* TargetWorld = (World && (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game)) ? World : nullptr;
		if (!TargetWorld && GEngine)
		{
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				if (Ctx.World() && (Ctx.WorldType == EWorldType::PIE || Ctx.WorldType == EWorldType::Game))
				{
					TargetWorld = Ctx.World();
					break;
				}
			}
		}
		if (!TargetWorld)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Stream.AuditResidency: no PIE/Game world found (start PIE first)"));
			return;
		}

		int32 Managers = 0;
		for (const UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(TargetWorld))
		{
			if (!CM) { continue; }
			++Managers;
			FString Report;
			CM->AuditResidency(Report);
			TArray<FString> Lines;
			Report.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Stream.AuditResidency [%s] %s"), *GetNameSafe(CM->GetOwner()), *Line);
			}
		}
		if (Managers == 0)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Stream.AuditResidency: no UVoxelChunkManager found"));
		}
	}));

// Console: voxel.Trees.List [metres=40]
// A cubic world's standing voxel trees near the player (code review CR4, FW-30: was the game module's vox.Trees, which runs
// this now): each tree's template, base and trunk / leaf blocks left.
static FAutoConsoleCommandWithWorldAndArgs GVoxelTreesListCmd(
	TEXT("voxel.Trees.List"),
	TEXT("List a cubic world's standing voxel trees near the player: voxel.Trees.List [metres=40] (template, base, trunk and leaf blocks)"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* InWorld)
	{
		UWorld* World = VoxelEditAuditHelpers::FindGameWorld(InWorld);
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		APawn* Player = PC ? PC->GetPawn() : nullptr;
		UVoxelChunkManager* ChunkMgr = World ? UVoxelChunkManager::Find(World) : nullptr;
		if (!Player || !ChunkMgr)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Trees.List: need a player pawn and a chunk manager."));
			return;
		}
		if (!ChunkMgr->HasVoxelTrees())
		{
			UE_LOG(LogVoxelStreaming, Display, TEXT("voxel.Trees.List: this world has no voxel trees (cubic meshing, TreeMode VoxelData or Both, templates)."));
			return;
		}
		const float Metres = Args.Num() >= 1 ? FCString::Atof(*Args[0]) : 40.f;
		TArray<FVoxelTreeInstance> Trees;
		ChunkMgr->FindVoxelTreesNear(Player->GetActorLocation(), Metres * 100.f, Trees);
		UE_LOG(LogVoxelStreaming, Display, TEXT("voxel.Trees.List: %d standing voxel tree(s) within %.0f m:"), Trees.Num(), Metres);
		TArray<FVoxelTreeVoxel> Blocks;
		for (int32 i = 0; i < FMath::Min(Trees.Num(), 20); ++i)
		{
			ChunkMgr->GetVoxelTreeBlocks(Trees[i], Blocks);
			const int32 Trunk = Blocks.FilterByPredicate([](const FVoxelTreeVoxel& B) { return B.bTrunk; }).Num();
			const FVector Base = ChunkMgr->GlobalVoxelToWorldCentre(Trees[i].BaseVoxel);
			UE_LOG(LogVoxelStreaming, Display, TEXT("  %d. template %d at %s (base voxel %s), %.0f m: %d trunk, %d leaf blocks"), i + 1,
				Trees[i].TemplateIndex, *Base.ToCompactString(), *Trees[i].BaseVoxel.ToString(), FVector::Dist2D(Base, Player->GetActorLocation()) / 100.f,
				Trunk, Blocks.Num() - Trunk);
		}
	}));

// Console: voxel.Stream.Enabled 0|1
// Pause or resume a game world's chunk streaming (UVoxelChunkManager::SetStreamingEnabled; code review CR4, FW-30: the
// plugin's own switch for what vox.Freeze's streaming flag does).
static FAutoConsoleCommandWithWorldAndArgs GVoxelStreamEnabledCmd(
	TEXT("voxel.Stream.Enabled"),
	TEXT("Pause (0) or resume (1) the game world's chunk streaming: voxel.Stream.Enabled 0|1"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* InWorld)
	{
		UWorld* World = VoxelEditAuditHelpers::FindGameWorld(InWorld);
		UVoxelChunkManager* ChunkMgr = World ? UVoxelChunkManager::Find(World) : nullptr;
		if (!ChunkMgr || Args.Num() < 1)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Stream.Enabled: usage voxel.Stream.Enabled 0|1 (needs a game world with a chunk manager)"));
			return;
		}
		const bool bEnabled = FCString::Atoi(*Args[0]) != 0;
		ChunkMgr->SetStreamingEnabled(bEnabled);
		UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.Stream.Enabled: streaming %s."), bEnabled ? TEXT("on") : TEXT("paused"));
	}));
