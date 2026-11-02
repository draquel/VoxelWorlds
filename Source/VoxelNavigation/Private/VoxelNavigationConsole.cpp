// Copyright Daniel Raquel. All Rights Reserved.

// Voxel surface navigation's console command (code review CR4, FW-30: a plugin registers its own diagnostics):
// voxel.Nav.Path. The game module's vox.NavPath runs it.

#include "VoxelNavigation.h"
#include "VoxelSurfaceNavigationSubsystem.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldModeTypes.h"
#include "CollisionQueryParams.h"
#include "Engine/Engine.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

#if !UE_BUILD_SHIPPING

namespace VoxelNavigationConsole
{
	/** The PIE or Game world: the console's own when it is one, else the first PIE/Game world context. */
	UWorld* FindPlayWorld(UWorld* World)
	{
		if (World && (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game))
		{
			return World;
		}
		if (GEngine)
		{
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				if (Ctx.World() && (Ctx.WorldType == EWorldType::PIE || Ctx.WorldType == EWorldType::Game))
				{
					return Ctx.World();
				}
			}
		}
		return nullptr;
	}

	/** A vector's part across local up, normalised. */
	FVector Across(const FVector& V, const FVector& Up)
	{
		return (V - Up * FVector::DotProduct(V, Up)).GetSafeNormal();
	}

	void Path(const TArray<FString>& Args, UWorld* InWorld)
	{
		UWorld* World = FindPlayWorld(InWorld);
		UVoxelSurfaceNavigationSubsystem* Nav = World ? World->GetSubsystem<UVoxelSurfaceNavigationSubsystem>() : nullptr;
		if (!Nav)
		{
			UE_LOG(LogVoxelNavigation, Warning, TEXT("voxel.Nav.Path: no PIE/Game world surface navigation subsystem."));
			return;
		}
		APlayerController* PC = World->GetFirstPlayerController();
		const APawn* Player = PC ? PC->GetPawn() : nullptr;
		if (!Player)
		{
			UE_LOG(LogVoxelNavigation, Warning, TEXT("voxel.Nav.Path: no player pawn."));
			return;
		}
		// The world's frame (a planet's local up), from the chunk manager.
		const UVoxelChunkManager* ChunkMgr = UVoxelChunkManager::Find(World);
		const bool bPlanet = ChunkMgr && ChunkMgr->GetWorldBounds().Shape == EVoxelWorldBoundsShape::Shell;
		FVector Goal;
		if (Args.Num() >= 3)
		{
			Goal = FVector(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]), FCString::Atof(*Args[2]));
		}
		else
		{
			// Where the camera looks: the point under the crosshair, or 60 m ahead along the view.
			FVector ViewLoc;
			FRotator ViewRot;
			PC->GetPlayerViewPoint(ViewLoc, ViewRot);
			FHitResult Hit;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(VoxNavPath), false, Player);
			if (World->LineTraceSingleByChannel(Hit, ViewLoc, ViewLoc + ViewRot.Vector() * 20000.f, ECC_Visibility, Params))
			{
				Goal = Hit.ImpactPoint;
			}
			else if (bPlanet)
			{
				Goal = Player->GetActorLocation() + Across(ViewRot.Vector(), ChunkMgr->GetUpVectorAt(Player->GetActorLocation())) * 6000.f;
			}
			else
			{
				Goal = Player->GetActorLocation() + FVector(ViewRot.Vector().X, ViewRot.Vector().Y, 0.f).GetSafeNormal() * 6000.f;
			}
		}
		TArray<FVector> Points;
		FVoxelSurfacePathStats Stats;
		{
			FVector OnSurface;
			const bool bProjected = Nav->ProjectToSurface(Player->GetActorLocation(), OnSurface);
			const FVoxelSurfaceNavParams P = Nav->GetParams();
			UE_LOG(LogVoxelNavigation, Display, TEXT("voxel.Nav.Path: player %s -> surface %s (%s), walkable %d | cell %.0f, step %.0f, water %s %.0f (wade %.0f), ready %d"),
				*Player->GetActorLocation().ToCompactString(), *OnSurface.ToCompactString(), bProjected ? TEXT("ok") : TEXT("no world"),
				Nav->IsWalkableAt(Player->GetActorLocation()) ? 1 : 0, P.CellSize, P.MaxStepRise, P.bHasWater ? TEXT("on") : TEXT("off"), P.WaterLevel, P.MaxWadeDepth, Nav->IsReady() ? 1 : 0);
		}
		const double T0 = FPlatformTime::Seconds();
		const bool bOk = Nav->FindPathWithStats(Player->GetActorLocation(), Goal, Points, Stats);
		const double Ms = (FPlatformTime::Seconds() - T0) * 1000.0;
		if (bOk)
		{
			TArray<FVector> Drawn = Points;
			Drawn.Insert(Player->GetActorLocation(), 0);
			Nav->DrawPath(Drawn, 10.f, Stats.bPartial ? FLinearColor::Yellow : FLinearColor(0.2f, 1.f, 0.2f));
		}
		float Length = 0.f;
		FVector Prev = Player->GetActorLocation();
		for (const FVector& P : Points)
		{
			Length += bPlanet ? FVector::Dist(Prev, P) : FVector::Dist2D(Prev, P);
			Prev = P;
		}
		UE_LOG(LogVoxelNavigation, Display, TEXT("voxel.Nav.Path to %s: %s — %d waypoint(s), %.0f uu long, %d nodes, %d samples, %d raw cells, %.2f ms%s%s (cached cells %d)"),
			*Goal.ToCompactString(), bOk ? TEXT("OK") : TEXT("NO PATH"), Points.Num(), Length, Stats.NodesExpanded, Stats.HeightSamples, Stats.RawCells, Ms,
			Stats.bPartial ? TEXT(", partial") : TEXT(""), Stats.bGoalSnapped ? TEXT(", goal snapped") : TEXT(""), Nav->GetCachedCellCount());
	}
}

static FAutoConsoleCommandWithWorldAndArgs GVoxelNavPathCmd(
	TEXT("voxel.Nav.Path"),
	TEXT("Find and draw a surface path from the player to the crosshair point (or voxel.Nav.Path x y z) for 10 s; logs its nodes, samples and time"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) { VoxelNavigationConsole::Path(Args, World); }));

#endif // !UE_BUILD_SHIPPING
