// Copyright Daniel Raquel. All Rights Reserved.

// The skeletal scatter definitions' diagnostics (the foliage and bolt plan's D68, F3; kept from the F2 spike): the skinned
// components the scatter made (voxel.Scatter.Skinned), what their tick would cost the game thread were it on
// (voxel.Scatter.Skinned.TimeRefresh), and stat unit's times sampled over a few seconds (voxel.Scatter.Skinned.Sample).
// They live in VoxelStreaming because VoxelScatter does not depend on it.

#include "VoxelChunkManager.h"
#include "Diagnostics/VoxelConsoleHelpers.h"
#include "VoxelScatterManager.h"
#include "VoxelScatterRenderer.h"
#include "VoxelStreaming.h"
#include "Animation/TransformProviderData.h"
#include "Components/InstancedSkinnedMeshComponent.h"
#include "Containers/Ticker.h"
#include "DynamicRHI.h"
#include "RenderCore.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"

namespace VoxelSkinnedScatter
{
	/** Every skinned component of every scatter in the world. */
	static void CollectComponents(UWorld* World, TArray<TPair<const FScatterDefinition*, UInstancedSkinnedMeshComponent*>>& Out)
	{
		for (UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(World))
		{
			const UVoxelScatterManager* Scatter = CM ? CM->GetScatterManager() : nullptr;
			const UVoxelScatterRenderer* Renderer = Scatter ? Scatter->GetScatterRenderer() : nullptr;
			if (!Renderer)
			{
				continue;
			}
			for (const FScatterDefinition& Definition : Scatter->GetScatterDefinitions())
			{
				if (!Definition.IsSkeletal())
				{
					continue;
				}
				TArray<TPair<FIntVector, UInstancedSkinnedMeshComponent*>> Components;
				Renderer->GetTypeSkinnedComponents(Definition.ScatterID, Components);
				for (const TPair<FIntVector, UInstancedSkinnedMeshComponent*>& Pair : Components)
				{
					Out.Emplace(&Definition, Pair.Value);
				}
			}
		}
	}

	/** One sampling run of voxel.Scatter.Skinned.Sample: per frame the frame, game thread, render thread and GPU ms */
	struct FSampleRun
	{
		FString Label;
		double EndSeconds = 0.0;
		TArray<float> Frame, Game, Render, GPU;
	};
	static TUniquePtr<FSampleRun> Sampling;

	static FString Summary(TArray<float>& Values)
	{
		if (Values.Num() == 0)
		{
			return TEXT("-");
		}
		Values.Sort();
		double Sum = 0.0;
		for (const float V : Values)
		{
			Sum += V;
		}
		return FString::Printf(TEXT("%.2f/%.2f"), Sum / Values.Num(), Values[FMath::Min(Values.Num() - 1, Values.Num() * 95 / 100)]);
	}

	static bool SampleTick(float DeltaTime)
	{
		FSampleRun& Run = *Sampling;
		Run.Frame.Add(DeltaTime * 1000.0f);
		Run.Game.Add(static_cast<float>(FPlatformTime::ToMilliseconds(GGameThreadTime)));
		Run.Render.Add(static_cast<float>(FPlatformTime::ToMilliseconds(GRenderThreadTime)));
		Run.GPU.Add(static_cast<float>(FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles())));
		if (FPlatformTime::Seconds() < Run.EndSeconds)
		{
			return true;
		}
		UE_LOG(LogVoxelStreaming, Display, TEXT("voxel.Scatter.Skinned.Sample [%s]: %d frames, mean/P95 ms: frame %s, game %s, render %s, GPU %s"),
			*Run.Label, Run.Frame.Num(), *Summary(Run.Frame), *Summary(Run.Game), *Summary(Run.Render), *Summary(Run.GPU));
		Sampling.Reset();
		return false;
	}
}

// Console: voxel.Scatter.Skinned
// The skeletal definitions' skinned components: per definition its components, instances, mesh, transform provider and
// whether any component ticks (none should: the scatter makes them without a tick, F2's game-thread premium)
static FAutoConsoleCommandWithWorldAndArgs GVoxelScatterSkinnedCmd(
	TEXT("voxel.Scatter.Skinned"),
	TEXT("Log the skeletal scatter definitions' skinned components: per definition the components, instances, mesh, transform provider and ticking components."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& /*Args*/, UWorld* World)
	{
		UWorld* TargetWorld = VoxelEditAuditHelpers::FindGameWorld(World);
		if (!TargetWorld)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Scatter.Skinned: no PIE/Game world found (start PIE first)"));
			return;
		}
		TArray<TPair<const FScatterDefinition*, UInstancedSkinnedMeshComponent*>> Components;
		VoxelSkinnedScatter::CollectComponents(TargetWorld, Components);
		struct FRow { int32 Components = 0, Instances = 0, Ticking = 0; FString Mesh, Provider; };
		TMap<FString, FRow> Rows;
		int32 Total = 0, Ticking = 0;
		for (const TPair<const FScatterDefinition*, UInstancedSkinnedMeshComponent*>& Pair : Components)
		{
			FRow& Row = Rows.FindOrAdd(FString::Printf(TEXT("%d %s"), Pair.Key->ScatterID, *Pair.Key->Name));
			++Row.Components;
			Row.Instances += Pair.Value->GetInstanceCount();
			Row.Ticking += Pair.Value->IsComponentTickEnabled() ? 1 : 0;
			Row.Mesh = Pair.Value->GetSkinnedAsset() ? Pair.Value->GetSkinnedAsset()->GetName() : TEXT("none");
			Row.Provider = Pair.Value->GetTransformProvider() ? Pair.Value->GetTransformProvider()->GetClass()->GetName() : TEXT("none (at rest)");
			Total += Pair.Value->GetInstanceCount();
			Ticking += Pair.Value->IsComponentTickEnabled() ? 1 : 0;
		}
		FString Out = FString::Printf(TEXT("voxel.Scatter.Skinned: %d components, %d instances, %d ticking; provider class %s"), Components.Num(),
			Total, Ticking, UVoxelScatterRenderer::GetSkinnedTransformProviderClass() ? TEXT("loaded") : TEXT("NOT loaded (the Dynamic Wind plugin)"));
		for (const TPair<FString, FRow>& Row : Rows)
		{
			Out += FString::Printf(TEXT("\n  type %s: %d components, %d instances, %d ticking, mesh %s, provider %s"), *Row.Key,
				Row.Value.Components, Row.Value.Instances, Row.Value.Ticking, *Row.Value.Mesh, *Row.Value.Provider);
		}
		UE_LOG(LogVoxelStreaming, Display, TEXT("%s"), *Out);
	}));

// Console: voxel.Scatter.Skinned.TimeRefresh [Iterations=50]
// What the skinned components' tick would cost the game thread per frame were it on: their RefreshBoneTransforms (the
// bounds over every instance, the render transform and dynamic data marked dirty), timed directly over Iterations
static FAutoConsoleCommandWithWorldAndArgs GVoxelScatterSkinnedTimeRefreshCmd(
	TEXT("voxel.Scatter.Skinned.TimeRefresh"),
	TEXT("Time the scatter's skinned components' RefreshBoneTransforms (their tick's per-frame work, off by design) over Iterations and log the ms per frame."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		UWorld* TargetWorld = VoxelEditAuditHelpers::FindGameWorld(World);
		if (!TargetWorld)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Scatter.Skinned.TimeRefresh: no PIE/Game world found (start PIE first)"));
			return;
		}
		const int32 Iterations = Args.Num() > 0 ? FMath::Max(1, FCString::Atoi(*Args[0])) : 50;
		TArray<TPair<const FScatterDefinition*, UInstancedSkinnedMeshComponent*>> Components;
		VoxelSkinnedScatter::CollectComponents(TargetWorld, Components);
		int32 Instances = 0;
		double Seconds = 0.0;
		for (const TPair<const FScatterDefinition*, UInstancedSkinnedMeshComponent*>& Pair : Components)
		{
			Instances += Pair.Value->GetInstanceCount();
			const double Start = FPlatformTime::Seconds();
			for (int32 i = 0; i < Iterations; ++i)
			{
				Pair.Value->RefreshBoneTransforms(nullptr);
			}
			Seconds += FPlatformTime::Seconds() - Start;
		}
		UE_LOG(LogVoxelStreaming, Display, TEXT("voxel.Scatter.Skinned.TimeRefresh: %d components, %d instances: %.3f ms per frame (%d iterations)"),
			Components.Num(), Instances, Seconds * 1000.0 / Iterations, Iterations);
	}));

// Console: voxel.Scatter.Skinned.Sample <Seconds> [Label]
// The frame, game thread, render thread and GPU times (stat unit's sources) per frame over Seconds, logged as mean / P95
static FAutoConsoleCommand GVoxelScatterSkinnedSampleCmd(
	TEXT("voxel.Scatter.Skinned.Sample"),
	TEXT("Sample stat unit's frame, game, render and GPU times for Seconds and log their mean and P95 under Label."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (VoxelSkinnedScatter::Sampling)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Scatter.Skinned.Sample: a run is in progress"));
			return;
		}
		VoxelSkinnedScatter::Sampling = MakeUnique<VoxelSkinnedScatter::FSampleRun>();
		VoxelSkinnedScatter::Sampling->Label = Args.Num() > 1 ? Args[1] : TEXT("sample");
		VoxelSkinnedScatter::Sampling->EndSeconds = FPlatformTime::Seconds() + (Args.Num() > 0 ? FCString::Atof(*Args[0]) : 5.0);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&VoxelSkinnedScatter::SampleTick));
	}));
