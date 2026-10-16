#include "VoxelStreamingBenchmark.h"
#include "VoxelChunkManager.h"
#include "VoxelCollisionManager.h"
#include "VoxelWorldConfiguration.h"
#include "IVoxelMesher.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "HAL/PlatformMemory.h"
#include "HAL/IConsoleManager.h"
#include "Engine/GameViewportClient.h"
#include "UnrealClient.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformFileManager.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelBench, Log, All);

FVoxelStreamingBenchmark::FVoxelStreamingBenchmark(const FVoxelBenchConfig& InConfig, UVoxelChunkManager* InChunkManager)
	: Config(InConfig)
	, ChunkManager(InChunkManager)
{
	Config.Direction = Config.Direction.GetSafeNormal();
	if (Config.Direction.IsNearlyZero())
	{
		Config.Direction = FVector::ForwardVector;
	}

	// Route: StartPosition is the first vertex; cache cumulative arc lengths for EvaluatePath.
	if (Config.PathType == EVoxelBenchPath::Route)
	{
		RouteVertices.Add(Config.StartPosition);
		RouteVertices.Append(Config.RoutePoints);
		RouteCumulative.Add(0.0);
		for (int32 i = 1; i < RouteVertices.Num(); ++i)
		{
			RouteCumulative.Add(RouteCumulative.Last() + FVector::Dist(RouteVertices[i - 1], RouteVertices[i]));
		}
		if (RouteVertices.Num() < 2)
		{
			UE_LOG(LogVoxelBench, Warning, TEXT("Benchmark '%s': route has no waypoints, falling back to a straight path"), *Config.Tag);
			Config.PathType = EVoxelBenchPath::Straight;
		}
	}
	// Orbit: start at the circle point nearest the start position.
	if (Config.PathType == EVoxelBenchPath::Orbit)
	{
		if (Config.OrbitRadius <= 0.0f)
		{
			UE_LOG(LogVoxelBench, Warning, TEXT("Benchmark '%s': orbit radius <= 0, falling back to a straight path"), *Config.Tag);
			Config.PathType = EVoxelBenchPath::Straight;
		}
		else
		{
			OrbitStartAngle = FMath::Atan2(Config.StartPosition.Y - Config.OrbitCenter.Y, Config.StartPosition.X - Config.OrbitCenter.X);
		}
	}
	// Route and orbit default to the whole path (one lap); a straight path needs an explicit length.
	if (Config.TraverseDistance <= 0.0f)
	{
		Config.TraverseDistance = static_cast<float>(GetPathLength());
	}

	CurrentPos = ApplyAltitude(EvaluatePath(0.0));

	if (ChunkManager)
	{
		ChunkManager->ResetBenchCounters();
		ChunkManager->SetBenchmarkView(true, CurrentPos);
	}
	Samples.Reserve(4096);
	SetupFlyPawn();
	SampleMemory();
	UE_LOG(LogVoxelBench, Log, TEXT("Benchmark '%s' started: path=%d start=(%.0f,%.0f,%.0f) vel=%.0f dist=%.0f follow=%.0f"),
		*Config.Tag, static_cast<int32>(Config.PathType), CurrentPos.X, CurrentPos.Y, CurrentPos.Z, Config.VelocityUU,
		Config.TraverseDistance, Config.FollowTerrainAltitude);
}

double FVoxelStreamingBenchmark::GetPathLength() const
{
	switch (Config.PathType)
	{
	case EVoxelBenchPath::Route:
		return RouteCumulative.Num() > 0 ? RouteCumulative.Last() : 0.0;
	case EVoxelBenchPath::Orbit:
		return 2.0 * UE_DOUBLE_PI * Config.OrbitRadius;
	default:
		return Config.TraverseDistance;
	}
}

FVector FVoxelStreamingBenchmark::EvaluatePath(double S) const
{
	switch (Config.PathType)
	{
	case EVoxelBenchPath::Route:
	{
		// Past the last vertex the viewer holds at the end of the route.
		const int32 Last = RouteVertices.Num() - 1;
		if (S >= RouteCumulative[Last])
		{
			return RouteVertices[Last];
		}
		for (int32 i = 1; i <= Last; ++i)
		{
			if (S <= RouteCumulative[i])
			{
				const double SegLen = RouteCumulative[i] - RouteCumulative[i - 1];
				const double Alpha = SegLen > UE_KINDA_SMALL_NUMBER ? (S - RouteCumulative[i - 1]) / SegLen : 1.0;
				return FMath::Lerp(RouteVertices[i - 1], RouteVertices[i], Alpha);
			}
		}
		return RouteVertices[Last];
	}
	case EVoxelBenchPath::Orbit:
	{
		const double Angle = OrbitStartAngle + S / Config.OrbitRadius;
		return FVector(
			Config.OrbitCenter.X + Config.OrbitRadius * FMath::Cos(Angle),
			Config.OrbitCenter.Y + Config.OrbitRadius * FMath::Sin(Angle),
			Config.StartPosition.Z);
	}
	default:
		return Config.StartPosition + Config.Direction * S;
	}
}

FVector FVoxelStreamingBenchmark::ApplyAltitude(const FVector& Pos) const
{
	if (Config.FollowTerrainAltitude < 0.0f || !ChunkManager)
	{
		return Pos;
	}
	return FVector(Pos.X, Pos.Y, ChunkManager->GetGeneratedSurfaceHeight(Pos.X, Pos.Y) + Config.FollowTerrainAltitude);
}

void FVoxelStreamingBenchmark::SetupFlyPawn()
{
	if (!ChunkManager) { return; }
	UWorld* World = ChunkManager->GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn) { return; }
	FlyPawn = Pawn;
	OriginalPawnLocation = Pawn->GetActorLocation();
	bPawnConfigured = true;
	// A route over the sea or a follow= run over a lake keeps the pawn underwater; with damage on it
	// drowns and respawns mid-run, which moves the streaming origin (G4, G8).
	bOriginalCanBeDamaged = Pawn->CanBeDamaged();
	Pawn->SetCanBeDamaged(false);
	// A character would walk + fall through unloaded terrain; switch it to flying so it tracks
	// the bench path cleanly. Non-character pawns just get teleported each tick.
	if (ACharacter* Char = Cast<ACharacter>(Pawn))
	{
		if (UCharacterMovementComponent* Move = Char->GetCharacterMovement())
		{
			OriginalMovementMode = static_cast<uint8>(Move->MovementMode.GetValue());
			Move->SetMovementMode(MOVE_Flying);
			Move->StopMovementImmediately();
		}
	}
}

void FVoxelStreamingBenchmark::UpdateFlyPawn()
{
	if (APawn* Pawn = FlyPawn.Get())
	{
		Pawn->SetActorLocation(CurrentPos, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void FVoxelStreamingBenchmark::BeginRestoreHold()
{
	APawn* Pawn = FlyPawn.Get();
	if (!Pawn)
	{
		Phase = EPhase::Done;
		return;
	}

	// Back to the start, held without gravity. The run streamed the start out and back in, so its
	// collision may still be cooking: restoring the movement mode at once dropped the pawn through (G9).
	Pawn->SetActorLocation(OriginalPawnLocation, false, nullptr, ETeleportType::TeleportPhysics);
	if (ACharacter* Char = Cast<ACharacter>(Pawn))
	{
		if (UCharacterMovementComponent* Move = Char->GetCharacterMovement())
		{
			Move->StopMovementImmediately();
			Move->SetMovementMode(MOVE_None);
		}
	}
	RestoreElapsed = 0.0f;
	RestoreNextNudge = 0.0f;
	Phase = EPhase::Restore;
}

void FVoxelStreamingBenchmark::TickRestoreHold(float DeltaTime)
{
	APawn* Pawn = FlyPawn.Get();
	if (!Pawn || !ChunkManager)
	{
		Phase = EPhase::Done;
		return;
	}
	RestoreElapsed += DeltaTime;

	// The chunk under the pawn's feet: ask for its collision every half second (a request made while the
	// chunk is still meshing is dropped), and release once it is cooked and a trace finds the ground.
	const float HalfHeight = Pawn->GetSimpleCollisionHalfHeight();
	const FVector Feet = Pawn->GetActorLocation() - FVector(0.0, 0.0, HalfHeight);
	const FIntVector FeetChunk = ChunkManager->WorldToChunkCoord(Feet - FVector(0.0, 0.0, 1.0));
	UVoxelCollisionManager* Collision = ChunkManager->GetCollisionManager();
	if (Collision && RestoreElapsed >= RestoreNextNudge)
	{
		Collision->RequestCollision(FeetChunk, /*Priority*/ 1000.0f);
		RestoreNextNudge = RestoreElapsed + 0.5f;
	}

	bool bGrounded = false;
	if (Collision && Collision->HasCollision(FeetChunk))
	{
		const UWorld* World = Pawn->GetWorld();
		FHitResult Hit;
		FCollisionQueryParams Params(FName(TEXT("VoxelBenchRestore")), /*bTraceComplex*/ true, Pawn);
		bGrounded = World && World->LineTraceSingleByChannel(Hit, Pawn->GetActorLocation(),
			Feet - FVector(0.0, 0.0, 300.0), ECC_WorldStatic, Params);
	}

	if (bGrounded || RestoreElapsed >= Config.RestoreTimeoutSec)
	{
		ReleasePawn(bGrounded);
		Phase = EPhase::Done;
	}
}

void FVoxelStreamingBenchmark::ReleasePawn(bool bGrounded)
{
	APawn* Pawn = FlyPawn.Get();
	FlyPawn = nullptr;
	if (!IsValid(Pawn))
	{
		return;
	}
	if (ACharacter* Char = Cast<ACharacter>(Pawn))
	{
		if (UCharacterMovementComponent* Move = Char->GetCharacterMovement())
		{
			Move->StopMovementImmediately();
			Move->SetMovementMode(static_cast<EMovementMode>(OriginalMovementMode));
		}
	}
	Pawn->SetCanBeDamaged(bOriginalCanBeDamaged);

	if (Phase != EPhase::Restore)
	{
		UE_LOG(LogVoxelBench, Warning, TEXT("Benchmark '%s': stopped before it finished; pawn released where it is"), *Config.Tag);
	}
	else if (bGrounded)
	{
		UE_LOG(LogVoxelBench, Log, TEXT("Benchmark '%s': pawn restored at its start (ground ready after %.1fs)"),
			*Config.Tag, RestoreElapsed);
	}
	else
	{
		UE_LOG(LogVoxelBench, Warning, TEXT("Benchmark '%s': pawn released at its start after %.1fs without collision under it"),
			*Config.Tag, RestoreElapsed);
	}
}

FVoxelStreamingBenchmark::~FVoxelStreamingBenchmark()
{
	// A run replaced mid-hold, or the world going away: never leave the pawn frozen or invulnerable.
	if (FlyPawn.IsValid())
	{
		ReleasePawn(/*bGrounded*/ false);
	}
}

bool FVoxelStreamingBenchmark::QueuesDrained() const
{
	if (!ChunkManager) { return true; }
	// "Caught up" = no pending mesh-producing work. Unload backlog is tracked separately
	// (lazy deletion) so a slow unloader doesn't hold the catch-up metric hostage.
	const int32 Backlog =
		ChunkManager->GetPendingGenerationCount() +
		ChunkManager->GetPendingMeshingCount() +
		ChunkManager->GetAsyncGenerationInProgressCount() +
		ChunkManager->GetPendingMeshUploadCount();
	return Backlog <= Config.DrainedThreshold;
}

void FVoxelStreamingBenchmark::TakeSample(float DeltaTime)
{
	if (!ChunkManager) { return; }
	const auto& T = ChunkManager->GetTimingStats();

	FSample S;
	S.SimTime = SimTime;
	S.Phase = static_cast<uint8>(Phase);
	S.PosX = CurrentPos.X; S.PosY = CurrentPos.Y; S.PosZ = CurrentPos.Z;
	S.GenQueue = ChunkManager->GetPendingGenerationCount();
	S.MeshQueue = ChunkManager->GetPendingMeshingCount();
	S.UnloadQueue = ChunkManager->GetPendingUnloadCount();
	S.PendingUpload = ChunkManager->GetPendingMeshUploadCount();
	S.GenInFlight = ChunkManager->GetAsyncGenerationInProgressCount();
	S.LoadedChunks = ChunkManager->GetLoadedChunkCount();
	S.TotalChunks = ChunkManager->GetTotalChunkCount();
	S.FrameMs = DeltaTime * 1000.0f;
	S.GenMs = T.GenerationMs;
	S.MeshMs = T.MeshingMs;
	S.LODMs = T.LODMs;
	S.LODUpdateMs = T.LODUpdateMs;
	S.StreamMs = T.StreamingMs;
	S.TotalMs = T.TotalMs;
	S.RenderMs = T.RenderSubmitMs;
	S.CollMs = T.CollisionMs;
	S.ScatMs = T.ScatterMs;
	S.GenLaunchMs = T.GenLaunchMs;
	S.GenPollMs = T.GenPollMs;
	S.GenApplyMs = T.GenApplyMs;
	S.GenStoreMs = T.GenStoreMs;
	S.GenNotifyMs = T.GenNotifyMs;
	S.GenApplyCount = T.GenApplyCount;
	S.MeshTickMs = T.MeshTickMs;
	S.MeshLaunchMs = T.MeshLaunchMs;
	S.MeshApplyMs = T.MeshApplyMs;
	S.MeshSnapMs = T.MeshSnapshotMs;
	S.MeshDispMs = T.MeshDispatchMs;
	S.MeshLaunchCount = T.MeshLaunchCount;
	S.RendMeshMs = T.RenderMeshMs;
	S.RendSubRendMs = T.RenderSubRendererMs;
	S.RendSubScatMs = T.RenderSubScatterMs;
	S.RendSubWatMs = T.RenderSubWaterMs;
	S.RendUnloadMs = T.RenderUnloadMs;
	S.RendWTileMs = T.RenderWaterTileMs;
	S.RendFlushMs = T.RenderFlushMs;
	S.RendSubmitCount = T.RenderSubmitCount;
	S.RemeshCount = ChunkManager->GetBenchRemeshCount();
	S.SeamMs = T.SeamMs;
	S.CollPrepMs = T.CollisionPrepMs;
	S.CollApplyMs = T.CollisionApplyMs;
	S.FarCompMs = T.FarCompressMs;
	S.FarCompDispatchN = T.FarCompressDispatchCount;
	S.FarCompApplyN = T.FarCompressApplyCount;
	S.FarOceanMs = T.FarOceanMs;
	S.FarOceanRebuildN = T.FarOceanRebuildCount;
	S.LODCand = T.LODEvalCandidates;
	S.LODQueued = T.LODEvalQueued;
	ChunkManager->GetBenchLODBacklog(S.LODStale, S.LODStaleNear);

	MemorySampleTimer += DeltaTime;
	if (MemorySampleTimer >= Config.MemorySampleIntervalSec)
	{
		MemorySampleTimer = 0.0f;
		SampleMemory();
		if (Phase == EPhase::Traverse)
		{
			TMap<int32, int32> ByLayer;
			ChunkManager->GetBenchLoadedByLayer(ByLayer);
			for (const TPair<int32, int32>& Layer : ByLayer)
			{
				LayerLoadedSum.FindOrAdd(Layer.Key) += Layer.Value;
				int32& Max = LayerLoadedMax.FindOrAdd(Layer.Key);
				Max = FMath::Max(Max, Layer.Value);
			}
			++LayerSamples;
		}
	}
	S.VoxelMB = LastMemory.VoxelMB;
	S.EditMB = LastMemory.EditMB;
	S.RendCPUMB = LastMemory.RendCPUMB;
	S.RendGPUMB = LastMemory.RendGPUMB;
	S.CollMB = LastMemory.CollMB;
	S.ScatMB = LastMemory.ScatMB;
	S.ProcessMB = LastMemory.ProcessMB;
	S.ResidentN = LastMemory.ResidentN;
	S.UniformN = LastMemory.UniformN;
	S.CompressedN = LastMemory.CompressedN;
	S.EmptyN = LastMemory.EmptyN;
	Samples.Add(S);
}

void FVoxelStreamingBenchmark::SampleMemory()
{
	if (!ChunkManager) { return; }
	constexpr double ToMB = 1.0 / (1024.0 * 1024.0);
	const UVoxelChunkManager::FVoxelMemoryStats M = ChunkManager->GetVoxelMemoryStats();
	LastMemory.VoxelMB = static_cast<float>(M.VoxelDataBytes * ToMB);
	LastMemory.EditMB = static_cast<float>(M.EditDataBytes * ToMB);
	LastMemory.RendCPUMB = static_cast<float>(M.RendererCPUBytes * ToMB);
	LastMemory.RendGPUMB = static_cast<float>(M.RendererGPUBytes * ToMB);
	LastMemory.CollMB = static_cast<float>(M.CollisionBytes * ToMB);
	LastMemory.ScatMB = static_cast<float>(M.ScatterBytes * ToMB);
	LastMemory.ProcessMB = static_cast<float>(FPlatformMemory::GetStats().UsedPhysical * ToMB);
	LastMemory.ResidentN = M.ResidentChunks;
	LastMemory.UniformN = M.UniformChunks;
	LastMemory.CompressedN = M.CompressedChunks;
	LastMemory.EmptyN = M.EmptyChunks;
}

void FVoxelStreamingBenchmark::Tick(float DeltaTime)
{
	if (Phase == EPhase::Done || !ChunkManager || DeltaTime <= 0.0f) { return; }

	// After the report: no more samples and no bench view, only the pawn's way back to the ground.
	if (Phase == EPhase::Restore)
	{
		TickRestoreHold(DeltaTime);
		return;
	}

	SimTime += DeltaTime;
	PhaseElapsed += DeltaTime;

	// Advance the streaming origin before sampling so the sample reflects the current position.
	if (Phase == EPhase::Traverse)
	{
		DistanceTraversed = FMath::Min(DistanceTraversed + Config.VelocityUU * DeltaTime, Config.TraverseDistance);
		CurrentPos = ApplyAltitude(EvaluatePath(DistanceTraversed));
	}
	ChunkManager->SetBenchmarkView(true, CurrentPos);
	UpdateFlyPawn();

	TakeSample(DeltaTime);

	switch (Phase)
	{
	case EPhase::Warmup:
		DrainedStreak = QueuesDrained() ? (DrainedStreak + 1) : 0;
		if (DrainedStreak >= Config.EquilibriumStreak || PhaseElapsed >= Config.WarmupTimeoutSec)
		{
			bWarmupDrained = DrainedStreak >= Config.EquilibriumStreak;
			Phase = EPhase::Traverse;
			PhaseElapsed = 0.0f;
			DistanceTraversed = 0.0f;
			DrainedStreak = 0;
			TraverseStartSimTime = SimTime;
			UE_LOG(LogVoxelBench, Log, TEXT("Benchmark '%s': warmup done at t=%.1fs, traversing %.0f uu"),
				*Config.Tag, SimTime, Config.TraverseDistance);
		}
		break;

	case EPhase::Traverse:
		if (DistanceTraversed >= Config.TraverseDistance)
		{
			Phase = EPhase::CatchUp;
			PhaseElapsed = 0.0f;
			DrainedStreak = 0;
			CatchUpStartSimTime = SimTime;
			UE_LOG(LogVoxelBench, Log, TEXT("Benchmark '%s': traverse done at t=%.1fs (%.0f uu), measuring catch-up"),
				*Config.Tag, SimTime, DistanceTraversed);
		}
		break;

	case EPhase::CatchUp:
		DrainedStreak = QueuesDrained() ? (DrainedStreak + 1) : 0;
		if (DrainedStreak >= Config.EquilibriumStreak)
		{
			Finish(true);
		}
		else if (PhaseElapsed >= Config.CatchUpTimeoutSec)
		{
			Finish(false);
		}
		break;

	default:
		break;
	}
}

void FVoxelStreamingBenchmark::Finish(bool bReachedEquilibrium)
{
	CatchUpDurationSec = bReachedEquilibrium ? (SimTime - CatchUpStartSimTime) : -1.0;
	if (ChunkManager)
	{
		// Audit while the benchmark view is still the streaming origin, so "beyond the unload
		// radius" is measured from where the run ended rather than from the restored pawn.
		AuditFindings = ChunkManager->AuditResidency(AuditReport);
		SampleMemory();
		ChunkManager->SetBenchmarkView(false, FVector::ZeroVector);
	}
	WriteReport();
	BeginRestoreHold();
}

// ---- percentile over an arbitrary float projection of the traverse-phase samples ----
static float Percentile(TArray<float>& Values, float P)
{
	if (Values.Num() == 0) { return 0.0f; }
	Values.Sort();
	const int32 Idx = FMath::Clamp(FMath::RoundToInt(P * (Values.Num() - 1)), 0, Values.Num() - 1);
	return Values[Idx];
}

// Minimal JSON string escaping for paths, tags and the multi-line audit report.
static FString JsonEscape(const FString& In)
{
	FString Out;
	Out.Reserve(In.Len() + 8);
	for (const TCHAR C : In)
	{
		switch (C)
		{
		case TEXT('"'): Out += TEXT("\\\""); break;
		case TEXT('\\'): Out += TEXT("\\\\"); break;
		case TEXT('\n'): Out += TEXT("\\n"); break;
		case TEXT('\r'): break;
		case TEXT('\t'): Out += TEXT("\\t"); break;
		default: Out.AppendChar(C); break;
		}
	}
	return Out;
}

FString FVoxelStreamingBenchmark::BuildRunMetadataJson() const
{
	TArray<FString> Fields;
	auto Str = [&Fields](const TCHAR* Key, const FString& Value) { Fields.Add(FString::Printf(TEXT("    \"%s\": \"%s\""), Key, *JsonEscape(Value))); };
	auto Num = [&Fields](const TCHAR* Key, double Value) { Fields.Add(FString::Printf(TEXT("    \"%s\": %.3f"), Key, Value)); };
	auto Int = [&Fields](const TCHAR* Key, int64 Value) { Fields.Add(FString::Printf(TEXT("    \"%s\": %lld"), Key, Value)); };
	auto Bool = [&Fields](const TCHAR* Key, bool bValue) { Fields.Add(FString::Printf(TEXT("    \"%s\": %s"), Key, bValue ? TEXT("true") : TEXT("false"))); };
	auto Vec = [&Fields](const TCHAR* Key, const FVector& V) { Fields.Add(FString::Printf(TEXT("    \"%s\": [%.1f, %.1f, %.1f]"), Key, V.X, V.Y, V.Z)); };

	const UWorld* World = ChunkManager ? ChunkManager->GetWorld() : nullptr;
	FString MapName = World ? World->GetMapName() : FString();
	if (World) { MapName.RemoveFromStart(World->StreamingLevelsPrefix); }
	Str(TEXT("map"), MapName);
	Str(TEXT("worldType"), World ? (World->WorldType == EWorldType::PIE ? TEXT("PIE") : World->WorldType == EWorldType::Game ? TEXT("Game") : TEXT("Other")) : TEXT("none"));
	Str(TEXT("build"), Config.BuildLabel);
	Str(TEXT("engine"), FEngineVersion::Current().ToString());
	Str(TEXT("rhi"), FApp::CanEverRender() ? FApp::GetGraphicsRHI() : FString(TEXT("null")));
	if (const UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr)
	{
		FVector2D Size = FVector2D::ZeroVector;
		Viewport->GetViewportSize(Size);
		Fields.Add(FString::Printf(TEXT("    \"viewport\": [%.0f, %.0f]"), Size.X, Size.Y));
	}

	// The -Voxel* switches on the command line (scheduler / LOD / force-CPU / config overrides).
	FString VoxelSwitches;
	{
		TArray<FString> Tokens, Switches;
		FCommandLine::Parse(FCommandLine::Get(), Tokens, Switches);
		for (const FString& Switch : Switches)
		{
			if (Switch.StartsWith(TEXT("Voxel"), ESearchCase::IgnoreCase))
			{
				VoxelSwitches += (VoxelSwitches.IsEmpty() ? TEXT("-") : TEXT(" -")) + Switch;
			}
		}
	}
	Str(TEXT("voxelSwitches"), VoxelSwitches);
	Str(TEXT("configOverride"), UVoxelWorldConfiguration::GetOverridePath());

	if (const UVoxelWorldConfiguration* Cfg = ChunkManager ? ChunkManager->GetConfiguration() : nullptr)
	{
		Str(TEXT("config"), Cfg->GetPathName());
		Str(TEXT("worldMode"), StaticEnum<EWorldMode>()->GetNameStringByValue(static_cast<int64>(Cfg->WorldMode)));
		Str(TEXT("meshingMode"), StaticEnum<EMeshingMode>()->GetNameStringByValue(static_cast<int64>(Cfg->MeshingMode)));
		const IVoxelMesher* Mesher = ChunkManager->GetMesherPtr();
		Str(TEXT("mesher"), Mesher ? Mesher->GetMesherTypeName() : FString(TEXT("none")));
		Bool(TEXT("gpuGenerationConfigured"), Cfg->bUseGPUGeneration);
		Bool(TEXT("gpuGenerationActive"), ChunkManager->IsUsingGPUGeneration());
		Bool(TEXT("gpuMeshingConfigured"), Cfg->bUseGPUMeshing);
		Bool(TEXT("gpuScatterExtraction"), Cfg->bUseGPUScatterExtraction);
		Bool(TEXT("forceCPU"), FParse::Param(FCommandLine::Get(), TEXT("VoxelForceCPU")));
		Int(TEXT("worldSeed"), Cfg->WorldSeed);
		Num(TEXT("voxelSize"), Cfg->VoxelSize);
		Int(TEXT("chunkSize"), Cfg->ChunkSize);
		Num(TEXT("chunkWorldSize"), Cfg->GetChunkWorldSize());
		Num(TEXT("viewDistance"), Cfg->ViewDistance);
		Int(TEXT("lodBands"), Cfg->LODBands.Num());
		Bool(TEXT("lodEnabled"), Cfg->IsLODEnabled()); // effective: off for cubic whatever bEnableLOD says
		// Far-compression mode (G6 A/B): sweep on/off and whether encodes run on workers.
		auto CVarInt = [](const TCHAR* Name) -> int32
		{
			const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
			return Var ? Var->GetInt() : -1;
		};
		Int(TEXT("farCompression"), CVarInt(TEXT("voxel.FarCompression")));
		Int(TEXT("farCompressionAsync"), CVarInt(TEXT("voxel.FarCompression.Async")));
		Vec(TEXT("worldOrigin"), Cfg->WorldOrigin);
		if (Cfg->WorldMode == EWorldMode::IslandBowl)
		{
			Int(TEXT("islandShape"), Cfg->IslandShape);
			Num(TEXT("islandRadius"), Cfg->IslandRadius);
			Num(TEXT("islandSizeY"), Cfg->IslandSizeY);
			Num(TEXT("islandFalloffWidth"), Cfg->IslandFalloffWidth);
			Int(TEXT("islandFalloffType"), Cfg->IslandFalloffType);
			Fields.Add(FString::Printf(TEXT("    \"islandCenter\": [%.1f, %.1f]"), Cfg->IslandCenterX, Cfg->IslandCenterY));
			Num(TEXT("islandEdgeHeight"), Cfg->IslandEdgeHeight);
			Num(TEXT("islandSeabedDepth"), Cfg->IslandSeabedDepth);
			Num(TEXT("islandSeabedSlopeWidth"), Cfg->IslandSeabedSlopeWidth);
			Bool(TEXT("islandFarOcean"), Cfg->bIslandFarOcean && CVarInt(TEXT("voxel.World.FarOcean")) != 0);
			Bool(TEXT("islandFarOceanBuilt"), ChunkManager->HasFarOcean());
		}
		else if (Cfg->WorldMode == EWorldMode::SphericalPlanet)
		{
			Num(TEXT("worldRadius"), Cfg->WorldRadius);
		}
	}

	static const TCHAR* PathNames[] = { TEXT("straight"), TEXT("route"), TEXT("orbit") };
	Str(TEXT("path"), PathNames[static_cast<int32>(Config.PathType)]);
	Vec(TEXT("start"), Config.StartPosition);
	if (Config.PathType == EVoxelBenchPath::Straight)
	{
		Vec(TEXT("direction"), Config.Direction);
	}
	else if (Config.PathType == EVoxelBenchPath::Route)
	{
		FString Points;
		for (const FVector& P : Config.RoutePoints)
		{
			Points += FString::Printf(TEXT("%s[%.0f, %.0f, %.0f]"), Points.IsEmpty() ? TEXT("") : TEXT(", "), P.X, P.Y, P.Z);
		}
		Fields.Add(FString::Printf(TEXT("    \"route\": [%s]"), *Points));
	}
	else
	{
		Fields.Add(FString::Printf(TEXT("    \"orbitCenter\": [%.1f, %.1f]"), Config.OrbitCenter.X, Config.OrbitCenter.Y));
		Num(TEXT("orbitRadius"), Config.OrbitRadius);
	}
	Num(TEXT("pathLength"), GetPathLength());
	Num(TEXT("followTerrainAltitude"), Config.FollowTerrainAltitude);
	Num(TEXT("warmupTimeoutSec"), Config.WarmupTimeoutSec);
	Num(TEXT("catchUpTimeoutSec"), Config.CatchUpTimeoutSec);

	return FString::Join(Fields, TEXT(",\n"));
}

void FVoxelStreamingBenchmark::WriteReport()
{
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("VoxelBench");
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	PF.CreateDirectoryTree(*Dir);
	const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	const FString Base = Dir / FString::Printf(TEXT("%s_%s"), *Stamp, *Config.Tag);
	ReportCsvPath = Base + TEXT(".csv");

	// ---- CSV time-series ----
	FString Csv = TEXT("SimTime,Phase,PosX,PosY,PosZ,GenQ,MeshQ,UnloadQ,UploadQ,GenInFlight,Loaded,Total,FrameMs,GenMs,MeshMs,LODMs,StreamMs,TotalMs,RenderMs,CollMs,ScatMs,GenLaunchMs,GenPollMs,GenApplyMs,GenStoreMs,GenNotifyMs,GenApplyN,MeshTickMs,MeshLaunchMs,MeshApplyMs,MeshSnapMs,MeshDispMs,MeshLaunchN,RendMeshMs,RendSubRendMs,RendSubScatMs,RendSubWatMs,RendUnloadMs,RendWTileMs,RendFlushMs,RendSubmitN,Remesh,SeamMs,CollPrepMs,CollApplyMs,VoxelMB,EditMB,RendCPUMB,RendGPUMB,CollMB,ScatMB,ProcessMB,ResidentN,UniformN,CompressedN,EmptyN,FarCompMs,FarCompDispN,FarCompApplyN,LODCand,LODQueued,LODStale,LODStaleNear,LODUpdMs,FarOceanMs,FarOceanRebuildN\n");
	for (const FSample& S : Samples)
	{
		Csv += FString::Printf(TEXT("%.3f,%d,%.0f,%.0f,%.0f,%d,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%lld,%.3f,%.3f,%.3f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.0f,%d,%d,%d,%d,%.3f,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%d\n"),
			S.SimTime, S.Phase, S.PosX, S.PosY, S.PosZ, S.GenQueue, S.MeshQueue, S.UnloadQueue, S.PendingUpload,
			S.GenInFlight, S.LoadedChunks, S.TotalChunks, S.FrameMs, S.GenMs, S.MeshMs, S.LODMs, S.StreamMs, S.TotalMs,
			S.RenderMs, S.CollMs, S.ScatMs,
			S.GenLaunchMs, S.GenPollMs, S.GenApplyMs, S.GenStoreMs, S.GenNotifyMs, S.GenApplyCount,
			S.MeshTickMs, S.MeshLaunchMs, S.MeshApplyMs,
			S.MeshSnapMs, S.MeshDispMs, S.MeshLaunchCount,
			S.RendMeshMs, S.RendSubRendMs, S.RendSubScatMs, S.RendSubWatMs,
			S.RendUnloadMs, S.RendWTileMs, S.RendFlushMs, S.RendSubmitCount,
			S.RemeshCount, S.SeamMs, S.CollPrepMs, S.CollApplyMs,
			S.VoxelMB, S.EditMB, S.RendCPUMB, S.RendGPUMB, S.CollMB, S.ScatMB, S.ProcessMB,
			S.ResidentN, S.UniformN, S.CompressedN, S.EmptyN,
			S.FarCompMs, S.FarCompDispatchN, S.FarCompApplyN,
			S.LODCand, S.LODQueued, S.LODStale, S.LODStaleNear, S.LODUpdateMs, S.FarOceanMs, S.FarOceanRebuildN);
	}
	FFileHelper::SaveStringToFile(Csv, *ReportCsvPath);

	// ---- summary over the traverse phase (the loaded window) ----
	int32 PeakGen = 0, PeakMesh = 0, PeakUnload = 0, MaxLoaded = 0;
	float PeakVoxelMB = 0.0f, PeakRendGPUMB = 0.0f, PeakRendCPUMB = 0.0f, PeakCollMB = 0.0f, PeakScatMB = 0.0f, PeakProcessMB = 0.0f;
	float PeakVoxelTotalMB = 0.0f;
	TArray<float> FrameMsArr, TotalMsArr, FarCompMsArr, LODStaleArr, LODStaleNearArr, LODUpdateMsArr, MeshMsArr, SeamMsArr, FarOceanMsArr;
	int64 FarOceanRebuilds = 0;
	int32 LODEvalFrames = 0;
	// Chunk mesh jobs launched over the whole run (every phase).
	int64 MeshJobsLaunched = 0;
	for (const FSample& S : Samples)
	{
		MaxLoaded = FMath::Max(MaxLoaded, S.LoadedChunks); // retention peak across all phases
		PeakVoxelMB = FMath::Max(PeakVoxelMB, S.VoxelMB);
		PeakRendGPUMB = FMath::Max(PeakRendGPUMB, S.RendGPUMB);
		PeakRendCPUMB = FMath::Max(PeakRendCPUMB, S.RendCPUMB);
		PeakCollMB = FMath::Max(PeakCollMB, S.CollMB);
		PeakScatMB = FMath::Max(PeakScatMB, S.ScatMB);
		PeakProcessMB = FMath::Max(PeakProcessMB, S.ProcessMB);
		PeakVoxelTotalMB = FMath::Max(PeakVoxelTotalMB, S.VoxelMB + S.EditMB + S.RendCPUMB + S.RendGPUMB + S.CollMB + S.ScatMB);
		MeshJobsLaunched += S.MeshLaunchCount;
		if (S.Phase != static_cast<uint8>(EPhase::Traverse)) { continue; }
		PeakGen = FMath::Max(PeakGen, S.GenQueue);
		PeakMesh = FMath::Max(PeakMesh, S.MeshQueue);
		PeakUnload = FMath::Max(PeakUnload, S.UnloadQueue);
		FrameMsArr.Add(S.FrameMs);
		TotalMsArr.Add(S.TotalMs);
		FarCompMsArr.Add(S.FarCompMs);
		LODStaleArr.Add(static_cast<float>(S.LODStale));
		LODStaleNearArr.Add(static_cast<float>(S.LODStaleNear));
		LODUpdateMsArr.Add(S.LODUpdateMs);
		MeshMsArr.Add(S.MeshMs);
		SeamMsArr.Add(S.SeamMs);
		FarOceanMsArr.Add(S.FarOceanMs);
		FarOceanRebuilds += S.FarOceanRebuildN;
		LODEvalFrames += S.LODCand >= 0 ? 1 : 0;
	}
	auto Mean = [](const TArray<float>& Values)
	{
		double Sum = 0.0;
		for (const float V : Values) { Sum += V; }
		return Values.Num() > 0 ? static_cast<float>(Sum / Values.Num()) : 0.0f;
	};
	const float LODStaleMean = Mean(LODStaleArr);
	const float LODStaleNearMean = Mean(LODStaleNearArr);
	const float LODEvalFraction = FrameMsArr.Num() > 0 ? static_cast<float>(LODEvalFrames) / FrameMsArr.Num() : 0.0f;
	int64 FarCompEncodes = 0, FarCompInstalled = 0, FarCompStale = 0;
	ChunkManager->GetBenchFarCompressionCounts(FarCompEncodes, FarCompInstalled, FarCompStale);

	double UnloadLagMean = 0.0, UnloadLagMax = 0.0; int64 UnloadLagCount = 0;
	ChunkManager->GetBenchUnloadLagStats(UnloadLagMean, UnloadLagMax, UnloadLagCount);
	double UnloadDistMean = 0.0, UnloadDistMax = 0.0;
	ChunkManager->GetBenchUnloadDistStats(UnloadDistMean, UnloadDistMax);
	const int64 Thrash = ChunkManager->GetBenchRemeshCount();
	const int64 ThrashLOD = ChunkManager->GetBenchRemeshByReason(EVoxelRemeshReason::LODTransition);
	const int64 ThrashDirty = ChunkManager->GetBenchRemeshByReason(EVoxelRemeshReason::Dirty);
	const int64 ThrashOther = ChunkManager->GetBenchRemeshByReason(EVoxelRemeshReason::Other);
	const double TraverseDur = CatchUpStartSimTime - TraverseStartSimTime;

	FString Json;
	Json += TEXT("{\n");
	Json += FString::Printf(TEXT("  \"tag\": \"%s\",\n"), *JsonEscape(Config.Tag));
	Json += FString::Printf(TEXT("  \"run\": {\n%s\n  },\n"), *BuildRunMetadataJson());
	Json += FString::Printf(TEXT("  \"velocityUU\": %.1f,\n"), Config.VelocityUU);
	Json += FString::Printf(TEXT("  \"traverseDistance\": %.1f,\n"), Config.TraverseDistance);
	Json += FString::Printf(TEXT("  \"samples\": %d,\n"), Samples.Num());
	Json += FString::Printf(TEXT("  \"warmupSec\": %.2f,\n"), TraverseStartSimTime);
	Json += FString::Printf(TEXT("  \"warmupDrained\": %s,\n"), bWarmupDrained ? TEXT("true") : TEXT("false"));
	Json += FString::Printf(TEXT("  \"traverseDurationSec\": %.2f,\n"), TraverseDur);
	Json += FString::Printf(TEXT("  \"catchUpSec\": %.2f,\n"), CatchUpDurationSec);
	Json += FString::Printf(TEXT("  \"reachedEquilibrium\": %s,\n"), CatchUpDurationSec >= 0.0 ? TEXT("true") : TEXT("false"));
	Json += FString::Printf(TEXT("  \"peakGenQueue\": %d,\n"), PeakGen);
	Json += FString::Printf(TEXT("  \"peakMeshQueue\": %d,\n"), PeakMesh);
	Json += FString::Printf(TEXT("  \"peakUnloadQueue\": %d,\n"), PeakUnload);
	Json += FString::Printf(TEXT("  \"peakLoadedChunks\": %d,\n"), MaxLoaded);
	Json += FString::Printf(TEXT("  \"frameMsP50\": %.3f,\n"), Percentile(FrameMsArr, 0.50f));
	Json += FString::Printf(TEXT("  \"frameMsP95\": %.3f,\n"), Percentile(FrameMsArr, 0.95f));
	Json += FString::Printf(TEXT("  \"frameMsP99\": %.3f,\n"), Percentile(FrameMsArr, 0.99f));
	Json += FString::Printf(TEXT("  \"totalMsP50\": %.3f,\n"), Percentile(TotalMsArr, 0.50f));
	Json += FString::Printf(TEXT("  \"totalMsP95\": %.3f,\n"), Percentile(TotalMsArr, 0.95f));
	Json += FString::Printf(TEXT("  \"totalMsP99\": %.3f,\n"), Percentile(TotalMsArr, 0.99f));
	// Far-compression sweep (a subset of totalMs) and its session encode counts (G6).
	Json += FString::Printf(TEXT("  \"farCompressMsP50\": %.3f,\n"), Percentile(FarCompMsArr, 0.50f));
	Json += FString::Printf(TEXT("  \"farCompressMsP95\": %.3f,\n"), Percentile(FarCompMsArr, 0.95f));
	Json += FString::Printf(TEXT("  \"farCompressMsP99\": %.3f,\n"), Percentile(FarCompMsArr, 0.99f));
	Json += FString::Printf(TEXT("  \"farCompressEncodes\": %lld,\n"), FarCompEncodes);
	Json += FString::Printf(TEXT("  \"farCompressInstalled\": %lld,\n"), FarCompInstalled);
	Json += FString::Printf(TEXT("  \"farCompressStale\": %lld,\n"), FarCompStale);
	// Far ocean (WM-E1, a subset of totalMs): open-column scan and grid water rebuilds over the traverse.
	Json += FString::Printf(TEXT("  \"farOceanMsP50\": %.3f,\n"), Percentile(FarOceanMsArr, 0.50f));
	Json += FString::Printf(TEXT("  \"farOceanMsP95\": %.3f,\n"), Percentile(FarOceanMsArr, 0.95f));
	Json += FString::Printf(TEXT("  \"farOceanMsMax\": %.3f,\n"), Percentile(FarOceanMsArr, 1.0f));
	Json += FString::Printf(TEXT("  \"farOceanRebuilds\": %lld,\n"), FarOceanRebuilds);
	// Meshing and seam sections of totalMs over the traverse, and the run's chunk-mesh and seam jobs (M5: a
	// cubic world moved its boundary work from chunk re-meshes to seam jobs).
	Json += FString::Printf(TEXT("  \"meshMsP95\": %.3f,\n"), Percentile(MeshMsArr, 0.95f));
	Json += FString::Printf(TEXT("  \"seamMsP50\": %.3f,\n"), Percentile(SeamMsArr, 0.50f));
	Json += FString::Printf(TEXT("  \"seamMsP95\": %.3f,\n"), Percentile(SeamMsArr, 0.95f));
	Json += FString::Printf(TEXT("  \"meshJobsLaunched\": %lld,\n"), MeshJobsLaunched);
	Json += FString::Printf(TEXT("  \"seamJobsCompleted\": %lld,\n"), ChunkManager->GetBenchSeamJobsCompleted());
	Json += FString::Printf(TEXT("  \"thrashRemeshCount\": %lld,\n"), Thrash);
	Json += FString::Printf(TEXT("  \"thrashLOD\": %lld,\n"), ThrashLOD);
	Json += FString::Printf(TEXT("  \"thrashDirty\": %lld,\n"), ThrashDirty);
	Json += FString::Printf(TEXT("  \"thrashOther\": %lld,\n"), ThrashOther);
	// LOD evaluation over the traverse (KI12): share of frames that evaluated LOD levels, and the LOD
	// backlog (Loaded chunks whose LOD change is not queued; "near" = the strategy puts them at LOD 0).
	Json += FString::Printf(TEXT("  \"lodEvalFrameFraction\": %.3f,\n"), LODEvalFraction);
	Json += FString::Printf(TEXT("  \"lodStaleMean\": %.1f,\n"), LODStaleMean);
	Json += FString::Printf(TEXT("  \"lodStaleP95\": %.0f,\n"), Percentile(LODStaleArr, 0.95f));
	Json += FString::Printf(TEXT("  \"lodStaleMax\": %.0f,\n"), Percentile(LODStaleArr, 1.0f));
	Json += FString::Printf(TEXT("  \"lodStaleNearMean\": %.1f,\n"), LODStaleNearMean);
	Json += FString::Printf(TEXT("  \"lodStaleNearMax\": %.0f,\n"), Percentile(LODStaleNearArr, 1.0f));
	// LOD strategy Update (balanced-LOD cache rebuild), a subset of totalMs timed since §13.11.
	Json += FString::Printf(TEXT("  \"lodUpdateMsP50\": %.3f,\n"), Percentile(LODUpdateMsArr, 0.50f));
	Json += FString::Printf(TEXT("  \"lodUpdateMsP95\": %.3f,\n"), Percentile(LODUpdateMsArr, 0.95f));
	// Loaded chunks per layer over the traverse (memory-sample cadence): mean and max.
	{
		TArray<int32> Layers;
		LayerLoadedSum.GetKeys(Layers);
		Layers.Sort();
		FString LayerJson;
		for (const int32 Layer : Layers)
		{
			const double LayerMean = LayerSamples > 0 ? static_cast<double>(LayerLoadedSum[Layer]) / LayerSamples : 0.0;
			LayerJson += FString::Printf(TEXT("%s\"%d\": [%.1f, %d]"), LayerJson.IsEmpty() ? TEXT("") : TEXT(", "), Layer, LayerMean, LayerLoadedMax.FindRef(Layer));
		}
		Json += FString::Printf(TEXT("  \"loadedByLayerMeanMax\": {%s},\n"), *LayerJson);
	}
	Json += FString::Printf(TEXT("  \"unloadLagMeanMs\": %.1f,\n"), UnloadLagMean);
	Json += FString::Printf(TEXT("  \"unloadLagMaxMs\": %.1f,\n"), UnloadLagMax);
	Json += FString::Printf(TEXT("  \"unloadCount\": %lld,\n"), UnloadLagCount);
	Json += FString::Printf(TEXT("  \"unloadDistMeanUU\": %.0f,\n"), UnloadDistMean);
	Json += FString::Printf(TEXT("  \"unloadDistMaxUU\": %.0f,\n"), UnloadDistMax);
	Json += FString::Printf(TEXT("  \"effMaxAsyncGen\": %d,\n"), ChunkManager->GetEffectiveMaxAsyncGenerationTasks());
	Json += FString::Printf(TEXT("  \"effMaxAsyncMesh\": %d,\n"), ChunkManager->GetEffectiveMaxAsyncMeshTasks());
	Json += FString::Printf(TEXT("  \"effMaxLODRemeshPerFrame\": %d,\n"), ChunkManager->GetEffectiveMaxLODRemeshPerFrame());
	Json += FString::Printf(TEXT("  \"effMaxPendingMeshes\": %d,\n"), ChunkManager->GetEffectiveMaxPendingMeshes());
	// Memory: peaks over the whole run and the values when it finished (after catch-up).
	Json += TEXT("  \"memory\": {\n");
	Json += FString::Printf(TEXT("    \"peakVoxelMB\": %.1f, \"endVoxelMB\": %.1f,\n"), PeakVoxelMB, LastMemory.VoxelMB);
	Json += FString::Printf(TEXT("    \"peakRendererGPUMB\": %.1f, \"endRendererGPUMB\": %.1f,\n"), PeakRendGPUMB, LastMemory.RendGPUMB);
	Json += FString::Printf(TEXT("    \"peakRendererCPUMB\": %.1f, \"endRendererCPUMB\": %.1f,\n"), PeakRendCPUMB, LastMemory.RendCPUMB);
	Json += FString::Printf(TEXT("    \"peakCollisionMB\": %.1f, \"endCollisionMB\": %.1f,\n"), PeakCollMB, LastMemory.CollMB);
	Json += FString::Printf(TEXT("    \"peakScatterMB\": %.1f, \"endScatterMB\": %.1f,\n"), PeakScatMB, LastMemory.ScatMB);
	Json += FString::Printf(TEXT("    \"endEditMB\": %.1f,\n"), LastMemory.EditMB);
	Json += FString::Printf(TEXT("    \"peakVoxelSystemsMB\": %.1f,\n"), PeakVoxelTotalMB);
	Json += FString::Printf(TEXT("    \"peakProcessMB\": %.0f, \"endProcessMB\": %.0f,\n"), PeakProcessMB, LastMemory.ProcessMB);
	Json += FString::Printf(TEXT("    \"endResidentChunks\": %d, \"endUniformChunks\": %d, \"endCompressedChunks\": %d, \"endEmptyChunks\": %d\n"),
		LastMemory.ResidentN, LastMemory.UniformN, LastMemory.CompressedN, LastMemory.EmptyN);
	Json += TEXT("  },\n");
	// Residency audit taken at the end of catch-up (0 findings = healthy).
	Json += FString::Printf(TEXT("  \"auditFindings\": %d,\n"), AuditFindings);
	Json += FString::Printf(TEXT("  \"auditReport\": \"%s\"\n"), *JsonEscape(AuditReport));
	Json += TEXT("}\n");
	FFileHelper::SaveStringToFile(Json, *(Base + TEXT(".json")));

	UE_LOG(LogVoxelBench, Warning,
		TEXT("Benchmark '%s' memory: peakVoxelSystems=%.0fMB (voxel %.0f, rendGPU %.0f, rendCPU %.0f, coll %.0f, scatter %.0f) peakProcess=%.0fMB; audit findings=%d"),
		*Config.Tag, PeakVoxelTotalMB, PeakVoxelMB, PeakRendGPUMB, PeakRendCPUMB, PeakCollMB, PeakScatMB, PeakProcessMB, AuditFindings);
	UE_LOG(LogVoxelBench, Warning,
		TEXT("Benchmark '%s' DONE: traverse=%.1fs catchUp=%.1fs peakMeshQ=%d peakGenQ=%d peakUnloadQ=%d frameP95=%.1fms totalP95=%.1fms thrash=%lld(lod=%lld dirty=%lld other=%lld) unloadLag(mean/max)=%.0f/%.0fms unloadDist(mean/max)=%.0f/%.0fuu -> %s"),
		*Config.Tag, TraverseDur, CatchUpDurationSec, PeakMesh, PeakGen, PeakUnload,
		Percentile(FrameMsArr, 0.95f), Percentile(TotalMsArr, 0.95f), Thrash, ThrashLOD, ThrashDirty, ThrashOther, UnloadLagMean, UnloadLagMax, UnloadDistMean, UnloadDistMax, *ReportCsvPath);
}
