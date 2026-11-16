// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWaterBodyComponent.h"
#include "VoxelStreaming.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "IVoxelWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "Components/SplineComponent.h"
#include "GameFramework/Actor.h"
#include "Misc/Crc.h"
#include "UObject/UObjectIterator.h"

UVoxelWaterBodyComponent::UVoxelWaterBodyComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

USplineComponent* UVoxelWaterBodyComponent::GetSpline() const
{
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return nullptr;
	}
	TInlineComponentArray<USplineComponent*> Splines(Owner);
	for (USplineComponent* Candidate : Splines)
	{
		if (SplineName.IsNone() || Candidate->GetFName() == SplineName)
		{
			return Candidate;
		}
	}
	return nullptr;
}

FString UVoxelWaterBodyComponent::GetBodyName() const
{
	return GetOwner() ? GetOwner()->GetName() : GetName();
}

FString UVoxelWaterBodyComponent::GetBodyKey() const
{
	// The names, not the paths: a PIE world's package is renamed, its level's actors are not, and a client's copy of a
	// level actor carries the server's name (UVoxelTerrainLineComponent::GetLineId's rule).
	const AActor* Owner = GetOwner();
	return (Owner ? Owner->GetName() : FString()) + TEXT(".") + GetName();
}

uint32 UVoxelWaterBodyComponent::GetWaterBodyId() const
{
	const uint32 Hash = FCrc::StrCrc32(*GetBodyKey());
	return Hash != FVoxelWaterBody::SeaId ? Hash : 1u;
}

double UVoxelWaterBodyComponent::GetNaturalHeight(const UVoxelChunkManager& Manager, const FVector& Place)
{
	const UVoxelWorldConfiguration* Configuration = Manager.GetConfiguration();
	const IVoxelWorldMode* WorldMode = Manager.GetWorldMode();
	if (!Configuration || !WorldMode)
	{
		return 0.0;
	}
	const FVoxelNoiseParams NoiseParams = Configuration->GetNoiseParams();
	return Configuration->WorldMode == EWorldMode::SphericalPlanet
		? static_cast<double>(static_cast<const FSphericalPlanetWorldMode*>(WorldMode)->GetTerrainAltitude(Place, NoiseParams))
		: WorldMode->GetSurfaceOnUpLine(FVector(Place.X, Place.Y, 0.0), NoiseParams).Position.Z;
}

bool UVoxelWaterBodyComponent::ComesBefore(const UVoxelWaterBodyComponent& Other) const
{
	if (GetRegistrationRank() != Other.GetRegistrationRank())
	{
		return GetRegistrationRank() < Other.GetRegistrationRank();
	}
	if (GetRegistrationOrder() != Other.GetRegistrationOrder())
	{
		return GetRegistrationOrder() > Other.GetRegistrationOrder();
	}
	return GetBodyKey().Compare(Other.GetBodyKey(), ESearchCase::CaseSensitive) < 0;
}

void UVoxelWaterBodyComponent::RegisterBody(UVoxelChunkManager& Manager)
{
	if (bRegistered || bRegistering)
	{
		return;
	}
	bRegistering = true;

	// The bodies of this world that come before this one, in their order: a river is made against the lakes and the
	// rivers it meets, whichever of the level's actors began play first.
	TArray<UVoxelWaterBodyComponent*> Earlier;
	for (TObjectIterator<UVoxelWaterBodyComponent> It; It; ++It)
	{
		UVoxelWaterBodyComponent* Other = *It;
		if (Other != this && IsValid(Other) && !Other->IsTemplate() && Other->GetOwner() && Other->GetWorld() == GetWorld()
			&& !Other->bRegistered && Other->ComesBefore(*this))
		{
			Earlier.Add(Other);
		}
	}
	Earlier.Sort([](const UVoxelWaterBodyComponent& A, const UVoxelWaterBodyComponent& B) { return A.ComesBefore(B); });
	for (UVoxelWaterBodyComponent* Other : Earlier)
	{
		Other->RegisterBody(Manager);
	}

	BodyWarnings.Reset();
	RegisteredBody = FVoxelWaterBody();
	if (AddBody(Manager, RegisteredBody, BodyWarnings))
	{
		bRegistered = true;
		for (const FString& Warning : BodyWarnings)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("Water body %s: %s."), *GetBodyName(), *Warning);
		}
	}
	bRegistering = false;
}

void UVoxelWaterBodyComponent::BeginPlay()
{
	Super::BeginPlay();

	// The voxel world started before this actor began play: register now (every level actor begins play before the
	// world's first tick, so before the chunk manager's first generation request). Else wait for it to start.
	if (UVoxelChunkManager* Manager = UVoxelChunkManager::Find(GetWorld()))
	{
		RegisterBody(*Manager);
	}
	else if (!bRegistered)
	{
		InitializedHandle = UVoxelChunkManager::OnInitialized.AddUObject(this, &UVoxelWaterBodyComponent::HandleChunkManagerInitialized);
	}
}

void UVoxelWaterBodyComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (InitializedHandle.IsValid())
	{
		UVoxelChunkManager::OnInitialized.Remove(InitializedHandle);
		InitializedHandle.Reset();
	}
	Super::EndPlay(EndPlayReason);
}

void UVoxelWaterBodyComponent::HandleChunkManagerInitialized(UVoxelChunkManager& Manager)
{
	if (Manager.GetWorld() != GetWorld())
	{
		return;
	}
	UVoxelChunkManager::OnInitialized.Remove(InitializedHandle);
	InitializedHandle.Reset();
	RegisterBody(Manager);
}
