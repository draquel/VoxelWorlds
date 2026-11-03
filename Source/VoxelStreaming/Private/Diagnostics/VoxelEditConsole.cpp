// Copyright Daniel Raquel. All Rights Reserved.

// The edit pipeline's console commands: debug system edits and the meshing work they cause.

#include "VoxelChunkManager.h"
#include "VoxelStreaming.h"
#include "Diagnostics/VoxelConsoleHelpers.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelEditManager.h"
#include "VoxelScatterManager.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"

// Console: voxel.Edit.System <x y z | pawn> <radius> [dig|fill=dig] [none|edge|corner]
//          voxel.Edit.SystemLayers <x y z | pawn> [dig|fill=dig] [axes=xyz] [depth=1]
// Debug system edits (world modes review G33), as a POI's terraform makes them (EEditSource::System, one operation); `pawn`
// stands for the player pawn's location:
// - voxel.Edit.System: a sphere brush at (x, y, z) (ApplyBrushEdit, density 255 a voxel at full strength); `edge` moves the
//   centre onto the nearest chunk edge running closest to local up (the edit spans the four chunks around it), `corner`
//   onto the nearest chunk corner (eight).
// - voxel.Edit.SystemLayers: the chunk holding (x, y, z), every voxel of its first `depth` layers on the given min faces
//   (x: local X < depth, and so on) set to air (dig) or to solid (fill, the voxel's material, stone where it was air). Its
//   min corner is a chunk corner, so on a planet the chunks at -X / -Y / -Z read it (G29, G31) though none of them is
//   edited: the edit G33's rule before missed for the edge and corner neighbours.
// Both log the edited chunks and, for each, the chunks whose extraction reads its edited voxels (GetEditReaders).
static void RunDebugSystemEdit(const TArray<FString>& InArgs, UWorld* World, bool bLayers)
{
	const TCHAR* Name = bLayers ? TEXT("voxel.Edit.SystemLayers") : TEXT("voxel.Edit.System");
	UWorld* TargetWorld = VoxelEditAuditHelpers::FindGameWorld(World);
	// "pawn" in place of x y z: the player pawn's location.
	TArray<FString> Args = InArgs;
	if (TargetWorld && Args.Num() > 0 && Args[0].Equals(TEXT("pawn"), ESearchCase::IgnoreCase))
	{
		const APlayerController* PC = TargetWorld->GetFirstPlayerController();
		const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		if (!Pawn)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("%s: no player pawn"), Name);
			return;
		}
		const FVector L = Pawn->GetActorLocation();
		Args.RemoveAt(0);
		Args.Insert(FString::SanitizeFloat(L.Z), 0);
		Args.Insert(FString::SanitizeFloat(L.Y), 0);
		Args.Insert(FString::SanitizeFloat(L.X), 0);
	}
	if (!TargetWorld || Args.Num() < (bLayers ? 3 : 4))
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("%s: %s"), Name, !TargetWorld ? TEXT("no PIE/Game world found (start PIE first)")
			: (bLayers ? TEXT("usage: <x y z | pawn> [dig|fill] [axes=xyz] [depth=1]") : TEXT("usage: <x y z | pawn> radius [dig|fill] [none|edge|corner]")));
		return;
	}
	FVector P(FCString::Atod(*Args[0]), FCString::Atod(*Args[1]), FCString::Atod(*Args[2]));
	const int32 ModeArg = bLayers ? 3 : 4;
	const bool bFill = Args.Num() > ModeArg && Args[ModeArg].Equals(TEXT("fill"), ESearchCase::IgnoreCase);
	// voxel.Edit.System's snap: "edge" moves the centre onto the chunk edge nearest it that runs closest to local up (the two
	// other axes onto chunk boundaries), "corner" onto the nearest chunk corner.
	const FString Snap = (!bLayers && Args.Num() > 5) ? Args[5].ToLower() : FString(TEXT("none"));
	for (UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(TargetWorld))
	{
		UVoxelEditManager* EditMgr = CM ? CM->GetEditManager() : nullptr;
		const UVoxelWorldConfiguration* Config = CM ? CM->GetConfiguration() : nullptr;
		if (!EditMgr || !Config)
		{
			continue;
		}
		TMap<FIntVector, FVoxelEditedVoxelBounds> Edited;
		FDelegateHandle Handle = EditMgr->OnChunkEdited.AddLambda([&Edited, EditMgr](const FIntVector& Chunk, EEditSource, const FVector&, float)
		{
			FIntVector Min, Max;
			FVoxelEditedVoxelBounds& B = Edited.FindOrAdd(Chunk);
			if (EditMgr->GetBroadcastEditBounds(Min, Max))
			{
				B.Add(Min);
				B.Add(Max);
			}
		});
		const EEditSource PrevSource = EditMgr->GetEditSource();
		EditMgr->SetEditSource(EEditSource::System);
		int32 Voxels = 0;
		if (bLayers)
		{
			const FString Axes = Args.Num() > 4 ? Args[4].ToLower() : FString(TEXT("xyz"));
			const int32 Depth = Args.Num() > 5 ? FMath::Max(1, FCString::Atoi(*Args[5])) : 1;
			const int32 CS = Config->ChunkSize;
			const float VS = Config->VoxelSize;
			const FIntVector Chunk = CM->WorldToChunkCoord(P);
			const FVector ChunkMin = Config->WorldOrigin + FVector(Chunk) * Config->GetChunkWorldSize();
			EditMgr->BeginEditOperation(TEXT("Debug system layers"));
			for (int32 Z = 0; Z < CS; ++Z)
			for (int32 Y = 0; Y < CS; ++Y)
			for (int32 X = 0; X < CS; ++X)
			{
				const bool bOnLayer = (Axes.Contains(TEXT("x")) && X < Depth) || (Axes.Contains(TEXT("y")) && Y < Depth) || (Axes.Contains(TEXT("z")) && Z < Depth);
				if (!bOnLayer)
				{
					continue;
				}
				const FVector VoxelPos = ChunkMin + (FVector(X, Y, Z) + FVector(0.5)) * VS;
				const FVoxelData Current = CM->GetVoxelAt(VoxelPos);
				const FVoxelData NewData = bFill ? FVoxelData::Solid(Current.IsSolid() ? Current.MaterialID : EVoxelMaterial::Stone, Current.BiomeID) : FVoxelData::Air();
				Voxels += EditMgr->ApplyEdit(VoxelPos, NewData, EEditMode::Set) ? 1 : 0;
			}
			EditMgr->EndEditOperation();
		}
		else
		{
			if (Snap == TEXT("edge") || Snap == TEXT("corner"))
			{
				const double CW = Config->GetChunkWorldSize();
				const FVector Rel = P - Config->WorldOrigin;
				const FVector Up = Config->WorldMode == EWorldMode::SphericalPlanet ? Rel.GetSafeNormal() : FVector::UpVector;
				// The axis along the edge: the one closest to local up (kept for an edge, snapped too for a corner).
				const int32 Along = FMath::Abs(Up.X) >= FMath::Abs(Up.Y) ? (FMath::Abs(Up.X) >= FMath::Abs(Up.Z) ? 0 : 2) : (FMath::Abs(Up.Y) >= FMath::Abs(Up.Z) ? 1 : 2);
				FVector Snapped = Rel;
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					if (Axis != Along || Snap == TEXT("corner"))
					{
						Snapped[Axis] = FMath::RoundToDouble(Rel[Axis] / CW) * CW;
					}
				}
				P = Config->WorldOrigin + Snapped;
			}
			FVoxelBrushParams Brush;
			Brush.Shape = EVoxelBrushShape::Sphere;
			Brush.Radius = FMath::Max(1.0f, FCString::Atof(*Args[3]));
			Brush.Strength = 1.0f;
			Brush.FalloffType = EVoxelBrushFalloff::Sharp;
			Brush.DensityDelta = 255;
			Brush.MaterialID = EVoxelMaterial::Stone;
			Voxels = EditMgr->ApplyBrushEdit(P, Brush, bFill ? EEditMode::Add : EEditMode::Subtract);
		}
		EditMgr->SetEditSource(PrevSource);
		EditMgr->OnChunkEdited.Remove(Handle);

		UE_LOG(LogVoxelStreaming, Warning, TEXT("%s (%.0f, %.0f, %.0f) %s: %d voxel(s) in %d chunk(s)"), Name, P.X, P.Y, P.Z,
			bFill ? TEXT("fill") : TEXT("dig"), Voxels, Edited.Num());
		const UVoxelScatterManager* ScatterMgr = CM->GetScatterManager();
		for (const TPair<FIntVector, FVoxelEditedVoxelBounds>& Pair : Edited)
		{
			FString ReaderText;
			if (ScatterMgr)
			{
				TArray<FIntVector> Readers;
				ScatterMgr->GetEditReaders(Pair.Key, Pair.Value.IsValid() ? &Pair.Value.Min : nullptr, Pair.Value.IsValid() ? &Pair.Value.Max : nullptr, Readers);
				for (const FIntVector& R : Readers)
				{
					ReaderText += FString::Printf(TEXT(" (%d,%d,%d)%s"), R.X, R.Y, R.Z, Edited.Contains(R) ? TEXT("*") : TEXT(""));
				}
			}
			UE_LOG(LogVoxelStreaming, Warning, TEXT("  chunk (%d,%d,%d) local (%d,%d,%d)-(%d,%d,%d); read by:%s"), Pair.Key.X, Pair.Key.Y, Pair.Key.Z,
				Pair.Value.Min.X, Pair.Value.Min.Y, Pair.Value.Min.Z, Pair.Value.Max.X, Pair.Value.Max.Y, Pair.Value.Max.Z,
				ReaderText.IsEmpty() ? TEXT(" none") : *ReaderText);
		}
	}
}

static FAutoConsoleCommandWithWorldAndArgs GVoxelEditSystemCmd(
	TEXT("voxel.Edit.System"),
	TEXT("Debug: a system edit (as a POI terraform) with a sphere brush. Args: <x y z | pawn> radius [dig|fill] [none|edge|corner]. Logs the edited chunks ")
	TEXT("and the chunks whose scatter reads them."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) { RunDebugSystemEdit(Args, World, false); }));

static FAutoConsoleCommandWithWorldAndArgs GVoxelEditSystemLayersCmd(
	TEXT("voxel.Edit.SystemLayers"),
	TEXT("Debug: a system edit of the min-face layers of the chunk holding a point. Args: <x y z | pawn> [dig|fill] [axes=xyz] [depth=1]. ")
	TEXT("Logs the chunks whose scatter reads it."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) { RunDebugSystemEdit(Args, World, true); }));

// Console: voxel.Edit.MeshWork [reset]
// World modes review G56 / G57: the meshing work since the last reset (UVoxelChunkManager::GetMeshWorkStats): chunk mesh jobs
// and seam jobs with their worker time, streaming's as well as edits', and what edits asked of the meshing pipeline. To
// measure an edit, hold the viewer still (vox.Freeze 1), reset, edit (voxel.Edit.System), wait for the pipeline to settle and
// log.
static TMap<TWeakObjectPtr<UVoxelChunkManager>, TPair<UVoxelChunkManager::FMeshWorkStats, double>> GVoxelMeshWorkBaselines;

static FAutoConsoleCommandWithWorldAndArgs GVoxelEditMeshWorkCmd(
	TEXT("voxel.Edit.MeshWork"),
	TEXT("Log the meshing work since the last 'voxel.Edit.MeshWork reset': chunk meshes and seam jobs ")
	TEXT("with their worker time, and what edits asked of the meshing pipeline. Arg: reset."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		UWorld* TargetWorld = VoxelEditAuditHelpers::FindGameWorld(World);
		if (!TargetWorld)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Edit.MeshWork: no PIE/Game world found (start PIE first)"));
			return;
		}
		const bool bReset = Args.Num() > 0 && Args[0].Equals(TEXT("reset"), ESearchCase::IgnoreCase);
		for (UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(TargetWorld))
		{
			if (!CM)
			{
				continue;
			}
			const UVoxelChunkManager::FMeshWorkStats& Now = CM->GetMeshWorkStats();
			if (bReset)
			{
				GVoxelMeshWorkBaselines.Add(CM, TPair<UVoxelChunkManager::FMeshWorkStats, double>(Now, FPlatformTime::Seconds()));
				UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Edit.MeshWork [%s]: reset"), *TargetWorld->GetName());
				continue;
			}
			const TPair<UVoxelChunkManager::FMeshWorkStats, double>* Base = GVoxelMeshWorkBaselines.Find(CM);
			const UVoxelChunkManager::FMeshWorkStats B = Base ? Base->Key : UVoxelChunkManager::FMeshWorkStats();
			const double Since = Base ? FPlatformTime::Seconds() - Base->Value : 0.0;
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Edit.MeshWork [%s]: %s %.1f s: chunk meshes %lld (worker %.1f ms), seam jobs %lld (worker %.1f ms); edits: %lld broadcasts, %lld edited chunks re-meshed, %lld on their way to a mesh (%lld re-meshed after it landed); now: %d to generate, %d to mesh, %d seam jobs in flight"),
				*TargetWorld->GetName(), Base ? TEXT("since reset") : TEXT("since Initialize (no reset),"), Since,
				Now.ChunkMeshJobs - B.ChunkMeshJobs, Now.ChunkMeshWorkerMs - B.ChunkMeshWorkerMs, Now.SeamJobs - B.SeamJobs,
				Now.SeamWorkerMs - B.SeamWorkerMs, Now.EditBroadcasts - B.EditBroadcasts, Now.EditedRemeshes - B.EditedRemeshes,
				Now.EditedOnTheWay - B.EditedOnTheWay, Now.RemeshesAfterMeshing - B.RemeshesAfterMeshing, CM->GetPendingGenerationCount(),
				CM->GetPendingMeshingCount(), CM->GetSeamJobsInFlightCount());
		}
	}));
