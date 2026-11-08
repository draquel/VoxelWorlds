// Copyright Daniel Raquel. All Rights Reserved.

// The foliage and bolt plan's F2 spike (D68), spike-only: one Megaplant species' scatter drawn through instanced skinned
// meshes with the Dynamic Wind transform provider, beside its static bakes at the same placements and count, so the two
// can be measured like for like. Nothing in the scatter's pipeline changes: the commands read a definition's live HISM
// instances (placement, extraction, regions and claims as they are), hide those HISMs and draw the same transforms
// through spike components at the same cull and fade distances. A snapshot: it does not follow streaming, so freeze it
// first (vox.Freeze 1 streaming). The Dynamic Wind plugin is reached by class name (no build dependency): its runtime
// module loads with the Procedural Vegetation plugin the project enables.

#include "VoxelChunkManager.h"
#include "Diagnostics/VoxelConsoleHelpers.h"
#include "VoxelScatterManager.h"
#include "VoxelScatterRenderer.h"
#include "VoxelStreaming.h"
#include "Animation/TransformProviderData.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedSkinnedMeshComponent.h"
#include "Containers/Ticker.h"
#include "DynamicRHI.h"
#include "RenderCore.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UnrealType.h"

namespace VoxelSkinnedSpike
{
	/** The spike components made (destroyed by mode off or the next build) */
	static TArray<TWeakObjectPtr<UPrimitiveComponent>> SpikeComponents;
	/** The scatter HISMs the spike hid (shown again by mode off or the next build) */
	static TArray<TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>> HiddenHISMs;

	static void Reset()
	{
		for (const TWeakObjectPtr<UPrimitiveComponent>& Component : SpikeComponents)
		{
			if (Component.IsValid())
			{
				Component->DestroyComponent();
			}
		}
		SpikeComponents.Reset();
		for (const TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>& HISM : HiddenHISMs)
		{
			if (HISM.IsValid())
			{
				HISM->SetVisibility(true);
			}
		}
		HiddenHISMs.Reset();
	}

	/**
	 * A Megaplant bake's skeletal source by Scripts/bake_megaplants.py's convention:
	 * <Species>/Static/SM_<Plant> was baked from <Species>/<Plant without its stage letter>/<Plant>.
	 */
	static USkeletalMesh* FindSkeletalSource(const FSoftObjectPath& StaticPath)
	{
		const FString Package = StaticPath.GetLongPackageName();
		const FString Asset = StaticPath.GetAssetName();
		FString SpeciesDir;
		FString StaticDir;
		if (!Package.Split(TEXT("/"), &StaticDir, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd)
			|| !StaticDir.EndsWith(TEXT("/Static")) || !Asset.StartsWith(TEXT("SM_")))
		{
			return nullptr;
		}
		SpeciesDir = StaticDir.LeftChop(7);
		const FString Plant = Asset.RightChop(3);
		FString PlantDir;
		if (!Plant.Split(TEXT("_"), &PlantDir, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
		{
			return nullptr;
		}
		const FString Path = FString::Printf(TEXT("%s/%s/%s.%s"), *SpeciesDir, *PlantDir, *Plant, *Plant);
		return LoadObject<USkeletalMesh>(nullptr, *Path);
	}

	/**
	 * The definition's live instances (not pooled) in its HISM's space, each repeated Copies times: copy k > 0 offset
	 * along the instance's own X and Y axes (its tangent plane) within Spread and turned, deterministic per instance.
	 */
	static void CollectTransforms(const UHierarchicalInstancedStaticMeshComponent* HISM, int32 Copies, float Spread,
		TArray<FTransform>& OutTransforms, int32& OutLive)
	{
		OutLive = 0;
		for (int32 i = 0; i < HISM->GetInstanceCount(); ++i)
		{
			FTransform T;
			if (!HISM->GetInstanceTransform(i, T, false) || T.GetScale3D().GetAbsMin() < UE_KINDA_SMALL_NUMBER)
			{
				continue;
			}
			++OutLive;
			OutTransforms.Add(T);
			for (int32 k = 1; k < Copies; ++k)
			{
				const uint32 Hash = HashCombine(GetTypeHash(i), GetTypeHash(k));
				const float Angle = (Hash & 0xFFFF) / 65535.0f * UE_TWO_PI;
				const float Radius = Spread * (0.5f + 0.5f * ((Hash >> 16) & 0xFFFF) / 65535.0f);
				const FVector Offset = T.GetRotation().RotateVector(FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f) * Radius);
				FTransform Copy = T;
				Copy.SetLocation(T.GetLocation() + Offset);
				Copy.SetRotation(T.GetRotation() * FQuat(FVector::UpVector, FMath::DegreesToRadians(97.0f * k)));
				OutTransforms.Add(Copy);
			}
		}
	}

	/** A spike component beside the HISM: the same parent, transform, cull and fade distances and shadow setting. */
	static void Place(UPrimitiveComponent* Component, const UHierarchicalInstancedStaticMeshComponent* HISM)
	{
		Component->SetupAttachment(HISM->GetAttachParent());
		Component->SetRelativeTransform(HISM->GetRelativeTransform());
		Component->SetCastShadow(HISM->CastShadow);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetCanEverAffectNavigation(false);
	}

	static void Build(UWorld* World, const FString& Filter, int32 Mode, int32 Copies, float Spread, bool bTick)
	{
		Reset();
		UClass* WindDataClass = FindObject<UClass>(nullptr, TEXT("/Script/DynamicWind.DynamicWindData"));
		UClass* WindSkeletalClass = FindObject<UClass>(nullptr, TEXT("/Script/DynamicWind.DynamicWindSkeletalData"));
		if (Mode == 1 && !WindDataClass)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Scatter.SkinnedSpike: the Dynamic Wind plugin is not loaded (no DynamicWindData class)"));
			return;
		}
		int32 Definitions = 0, Components = 0, Live = 0, Drawn = 0;
		for (UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(World))
		{
			UVoxelScatterManager* Scatter = CM ? CM->GetScatterManager() : nullptr;
			const UVoxelScatterRenderer* Renderer = Scatter ? Scatter->GetScatterRenderer() : nullptr;
			if (!Renderer)
			{
				continue;
			}
			// The held writes applied, so the HISMs hold what the scatter placed
			const_cast<UVoxelScatterRenderer*>(Renderer)->FlushHISMWrites();
			for (const FScatterDefinition& Definition : Scatter->GetScatterDefinitions())
			{
				if (!Definition.Name.Contains(Filter))
				{
					continue;
				}
				USkeletalMesh* Skeletal = Mode == 1 ? FindSkeletalSource(Definition.Mesh.ToSoftObjectPath()) : nullptr;
				if (Mode == 1 && !Skeletal)
				{
					UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Scatter.SkinnedSpike: %s (%d): no skeletal source for %s"),
						*Definition.Name, Definition.ScatterID, *Definition.Mesh.ToString());
					continue;
				}
				TArray<TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>> HISMs;
				Renderer->GetTypeHISMs(Definition.ScatterID, HISMs);
				int32 DefinitionLive = 0, DefinitionDrawn = 0;
				for (const TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>& Pair : HISMs)
				{
					UHierarchicalInstancedStaticMeshComponent* HISM = Pair.Value;
					if (!HISM || !HISM->GetOwner())
					{
						continue;
					}
					TArray<FTransform> Transforms;
					int32 HISMLive = 0;
					CollectTransforms(HISM, Copies, Spread, Transforms, HISMLive);
					HISM->SetVisibility(false);
					HiddenHISMs.Add(HISM);
					DefinitionLive += HISMLive;
					if (Transforms.Num() == 0)
					{
						continue;
					}
					int32 StartCull = 0, EndCull = 0;
					HISM->GetCullDistances(StartCull, EndCull);
					UPrimitiveComponent* Made = nullptr;
					if (Mode == 1)
					{
						UInstancedSkinnedMeshComponent* Skinned = NewObject<UInstancedSkinnedMeshComponent>(HISM->GetOwner(), NAME_None, RF_Transient);
						Place(Skinned, HISM);
						Skinned->SetSkinnedAsset(Skeletal);
						Skinned->SetTransformProvider(NewObject<UTransformProviderData>(Skinned, WindDataClass, NAME_None, RF_Transient));
						Skinned->SetCullDistances(StartCull, EndCull);
						// The component's tick refreshes its bounds over every instance and marks its render transform dirty
						// each frame (RefreshBoneTransforms); the Dynamic Wind provider animates on the GPU without it
						Skinned->SetComponentTickEnabled(bTick);
						TArray<int32> Animations;
						Animations.SetNumZeroed(Transforms.Num());
						Skinned->AddInstances(Transforms, Animations, false, false);
						Made = Skinned;
					}
					else
					{
						UHierarchicalInstancedStaticMeshComponent* Baked = NewObject<UHierarchicalInstancedStaticMeshComponent>(HISM->GetOwner(), NAME_None, RF_Transient);
						Place(Baked, HISM);
						Baked->SetStaticMesh(HISM->GetStaticMesh());
						for (int32 Slot = 0; Slot < HISM->GetNumMaterials(); ++Slot)
						{
							Baked->SetMaterial(Slot, HISM->GetMaterial(Slot));
						}
						Baked->SetMobility(EComponentMobility::Static);
						Baked->bEnableDensityScaling = false;
						Baked->SetCullDistances(StartCull, EndCull);
						Baked->AddInstances(Transforms, false);
						Made = Baked;
					}
					Made->RegisterComponent();
					SpikeComponents.Add(Made);
					DefinitionDrawn += Transforms.Num();
					++Components;
				}
				const bool bWindData = Skeletal && WindSkeletalClass && Skeletal->GetAssetUserDataOfClass(WindSkeletalClass);
				UE_LOG(LogVoxelStreaming, Display, TEXT("voxel.Scatter.SkinnedSpike: %s (%d): %d HISMs, %d live instances, %d drawn %s%s%s"),
					*Definition.Name, Definition.ScatterID, HISMs.Num(), DefinitionLive, DefinitionDrawn,
					Mode == 1 ? TEXT("skinned from ") : TEXT("baked"), Skeletal ? *Skeletal->GetPathName() : TEXT(""),
					Mode == 1 ? (bWindData ? TEXT(" (wind data)") : TEXT(" (NO wind data)")) : TEXT(""));
				Live += DefinitionLive;
				Drawn += DefinitionDrawn;
				++Definitions;
			}
		}
		UE_LOG(LogVoxelStreaming, Display, TEXT("voxel.Scatter.SkinnedSpike: '%s' mode %s x%d%s: %d definitions, %d components, %d live instances, %d drawn"),
			*Filter, Mode == 1 ? TEXT("skinned") : TEXT("baked"), Copies, (Mode == 1 && !bTick) ? TEXT(" (no tick)") : TEXT(""), Definitions, Components, Live, Drawn);
	}

	/** Set a float or FVector field of a reflected struct by name. */
	static void SetField(const UScriptStruct* Struct, void* Data, const TCHAR* Name, const FVector& Vector, float Scalar)
	{
		const FProperty* Property = Struct->FindPropertyByName(Name);
		if (const FFloatProperty* Float = CastField<FFloatProperty>(Property))
		{
			Float->SetPropertyValue_InContainer(Data, Scalar);
		}
		else if (const FStructProperty* Inner = CastField<FStructProperty>(Property))
		{
			*Inner->ContainerPtrToValuePtr<FVector>(Data) = Vector;
		}
	}

	/** UDynamicWindSubsystem::UpdateWindParameters by reflection, the simulation centred on the local viewer. */
	static void FeedWind(UWorld* World, float Speed, float Amplitude, const FVector& Direction)
	{
		UClass* SubsystemClass = FindObject<UClass>(nullptr, TEXT("/Script/DynamicWind.DynamicWindSubsystem"));
		UWorldSubsystem* Subsystem = SubsystemClass ? World->GetSubsystemBase(SubsystemClass) : nullptr;
		UFunction* Update = Subsystem ? Subsystem->FindFunction(TEXT("UpdateWindParameters")) : nullptr;
		const FStructProperty* ParamsProperty = Update ? CastField<FStructProperty>(Update->FindPropertyByName(TEXT("Parameters"))) : nullptr;
		if (!ParamsProperty)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Scatter.SkinnedSpike.Wind: no UDynamicWindSubsystem in this world"));
			return;
		}
		FVector Center = FVector::ZeroVector;
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			FRotator Rotation;
			PC->GetPlayerViewPoint(Center, Rotation);
		}
		uint8* Params = static_cast<uint8*>(FMemory::Malloc(Update->ParmsSize, 16));
		FMemory::Memzero(Params, Update->ParmsSize);
		Update->InitializeStruct(Params);
		void* Wind = ParamsProperty->ContainerPtrToValuePtr<void>(Params);
		SetField(ParamsProperty->Struct, Wind, TEXT("SimulationCenter"), Center, 0.0f);
		SetField(ParamsProperty->Struct, Wind, TEXT("SimulationExtents"), FVector::ZeroVector, 20000.0f);
		SetField(ParamsProperty->Struct, Wind, TEXT("WindDirection"), Direction.GetSafeNormal(), 0.0f);
		SetField(ParamsProperty->Struct, Wind, TEXT("WindSpeed"), FVector::ZeroVector, Speed);
		SetField(ParamsProperty->Struct, Wind, TEXT("WindAmplitude"), FVector::ZeroVector, Amplitude);
		Subsystem->ProcessEvent(Update, Params);
		Update->DestroyStruct(Params);
		FMemory::Free(Params);
		UE_LOG(LogVoxelStreaming, Display, TEXT("voxel.Scatter.SkinnedSpike.Wind: speed %.2f, amplitude %.2f, direction %s, centre %s"),
			Speed, Amplitude, *Direction.GetSafeNormal().ToString(), *Center.ToString());
	}
}

namespace VoxelSkinnedSpike
{
	/** One sampling run of voxel.Scatter.SkinnedSpike.Sample: per frame the frame, game thread, render thread and GPU ms */
	struct FSampleRun
	{
		FString Label;
		double EndSeconds = 0.0;
		TArray<float> Frame, Game, Render, GPU;
		FTSTicker::FDelegateHandle Handle;
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
		UE_LOG(LogVoxelStreaming, Display, TEXT("voxel.Scatter.SkinnedSpike.Sample [%s]: %d frames, mean/P95 ms: frame %s, game %s, render %s, GPU %s"),
			*Run.Label, Run.Frame.Num(), *Summary(Run.Frame), *Summary(Run.Game), *Summary(Run.Render), *Summary(Run.GPU));
		Sampling.Reset();
		return false;
	}
}

// Console: voxel.Scatter.SkinnedSpike.Sample <Seconds> [Label] (spike-only, F2): the frame, game thread, render thread
// and GPU times (stat unit's sources) per frame over Seconds, logged as mean / P95
static FAutoConsoleCommand GVoxelScatterSkinnedSpikeSampleCmd(
	TEXT("voxel.Scatter.SkinnedSpike.Sample"),
	TEXT("Spike (the foliage and bolt plan's F2): sample stat unit's frame, game, render and GPU times for Seconds and log their mean and P95 under Label."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (VoxelSkinnedSpike::Sampling)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Scatter.SkinnedSpike.Sample: a run is in progress"));
			return;
		}
		VoxelSkinnedSpike::Sampling = MakeUnique<VoxelSkinnedSpike::FSampleRun>();
		VoxelSkinnedSpike::Sampling->Label = Args.Num() > 1 ? Args[1] : TEXT("sample");
		VoxelSkinnedSpike::Sampling->EndSeconds = FPlatformTime::Seconds() + (Args.Num() > 0 ? FCString::Atof(*Args[0]) : 5.0);
		VoxelSkinnedSpike::Sampling->Handle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateStatic(&VoxelSkinnedSpike::SampleTick));
	}));

// Console: voxel.Scatter.SkinnedSpike.TimeTick [Iterations=50] (spike-only, F2): the game thread's per-frame work of the
// skinned components' tick, timed directly (UInstancedSkinnedMeshComponent::RefreshBoneTransforms: the bounds over every
// instance, the render transform and dynamic data marked dirty), independent of the GPU and of other processes' load
static FAutoConsoleCommand GVoxelScatterSkinnedSpikeTimeTickCmd(
	TEXT("voxel.Scatter.SkinnedSpike.TimeTick"),
	TEXT("Spike (the foliage and bolt plan's F2): time the skinned spike components' per-frame tick work (RefreshBoneTransforms) over Iterations and log the ms per frame."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		const int32 Iterations = Args.Num() > 0 ? FMath::Max(1, FCString::Atoi(*Args[0])) : 50;
		int32 Components = 0, Instances = 0;
		double Seconds = 0.0;
		for (const TWeakObjectPtr<UPrimitiveComponent>& Component : VoxelSkinnedSpike::SpikeComponents)
		{
			UInstancedSkinnedMeshComponent* Skinned = Cast<UInstancedSkinnedMeshComponent>(Component.Get());
			if (!Skinned)
			{
				continue;
			}
			++Components;
			Instances += Skinned->GetInstanceCount();
			const double Start = FPlatformTime::Seconds();
			for (int32 i = 0; i < Iterations; ++i)
			{
				Skinned->RefreshBoneTransforms(nullptr);
			}
			Seconds += FPlatformTime::Seconds() - Start;
		}
		UE_LOG(LogVoxelStreaming, Display, TEXT("voxel.Scatter.SkinnedSpike.TimeTick: %d components, %d instances: %.3f ms per frame (%d iterations)"),
			Components, Instances, Seconds * 1000.0 / Iterations, Iterations);
	}));

// Console: voxel.Scatter.SkinnedSpike <NameFilter> <off|skinned|baked> [Copies=1] [Spread=150] [Tick=1] (spike-only, F2)
static FAutoConsoleCommandWithWorldAndArgs GVoxelScatterSkinnedSpikeCmd(
	TEXT("voxel.Scatter.SkinnedSpike"),
	TEXT("Spike (the foliage and bolt plan's F2): draw the scatter definitions whose name contains NameFilter through instanced skinned meshes with the Dynamic Wind provider (skinned) or a copy of their static bakes (baked), at their live HISM placements repeated Copies times within Spread uu, the HISMs hidden; Tick 0 turns the skinned components' tick off; off restores. Freeze streaming first."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		UWorld* TargetWorld = VoxelEditAuditHelpers::FindGameWorld(World);
		if (!TargetWorld)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Scatter.SkinnedSpike: no PIE/Game world found (start PIE first)"));
			return;
		}
		const FString ModeArg = Args.Num() > 1 ? Args[1] : TEXT("off");
		if (Args.Num() < 2 || ModeArg.Equals(TEXT("off"), ESearchCase::IgnoreCase) || ModeArg == TEXT("0"))
		{
			VoxelSkinnedSpike::Reset();
			UE_LOG(LogVoxelStreaming, Display, TEXT("voxel.Scatter.SkinnedSpike: off (the scatter's HISMs shown)"));
			return;
		}
		const int32 Mode = (ModeArg.Equals(TEXT("skinned"), ESearchCase::IgnoreCase) || ModeArg == TEXT("1")) ? 1 : 2;
		const int32 Copies = Args.Num() > 2 ? FMath::Max(1, FCString::Atoi(*Args[2])) : 1;
		const float Spread = Args.Num() > 3 ? FCString::Atof(*Args[3]) : 150.0f;
		const bool bTick = Args.Num() > 4 ? FCString::Atoi(*Args[4]) != 0 : true;
		VoxelSkinnedSpike::Build(TargetWorld, Args[0], Mode, Copies, Spread, bTick);
	}));

// Console: voxel.Scatter.SkinnedSpike.Wind <Speed> <Amplitude> [DirX DirY DirZ] (spike-only, F2)
static FAutoConsoleCommandWithWorldAndArgs GVoxelScatterSkinnedSpikeWindCmd(
	TEXT("voxel.Scatter.SkinnedSpike.Wind"),
	TEXT("Spike (the foliage and bolt plan's F2): feed UDynamicWindSubsystem by hand: speed, amplitude (0 = still) and direction (default +X), the simulation centred on the local viewer."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		UWorld* TargetWorld = VoxelEditAuditHelpers::FindGameWorld(World);
		if (!TargetWorld)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Scatter.SkinnedSpike.Wind: no PIE/Game world found (start PIE first)"));
			return;
		}
		const float Speed = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 15.0f;
		const float Amplitude = Args.Num() > 1 ? FCString::Atof(*Args[1]) : 1.0f;
		const FVector Direction = Args.Num() > 4
			? FVector(FCString::Atof(*Args[2]), FCString::Atof(*Args[3]), FCString::Atof(*Args[4]))
			: FVector::ForwardVector;
		VoxelSkinnedSpike::FeedWind(TargetWorld, Speed, Amplitude, Direction);
	}));
