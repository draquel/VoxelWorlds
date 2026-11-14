// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelTerrainLineComponent.h"
#include "VoxelStreaming.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "IVoxelWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "Components/SplineComponent.h"
#include "Misc/Crc.h"

UVoxelTerrainLineComponent::UVoxelTerrainLineComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

USplineComponent* UVoxelTerrainLineComponent::GetSpline() const
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

int64 UVoxelTerrainLineComponent::GetLineId() const
{
	// The names, not the paths: a PIE world's package is renamed, its level's actors are not, and a client's copy of a
	// level actor carries the server's name.
	const AActor* Owner = GetOwner();
	const FString Key = (Owner ? Owner->GetName() : FString()) + TEXT(".") + GetName();
	const uint32 Hash = FCrc::StrCrc32(*Key);
	return (static_cast<int64>(Order) << 32) | static_cast<int64>(Hash);
}

bool UVoxelTerrainLineComponent::BuildLineParams(const UVoxelChunkManager& Manager, FVoxelConditioningLineParams& OutParams) const
{
	const USplineComponent* Spline = GetSpline();
	const UVoxelWorldConfiguration* Configuration = Manager.GetConfiguration();
	const IVoxelWorldMode* WorldMode = Manager.GetWorldMode();
	if (!Spline || !Configuration || !WorldMode || !Manager.IsInitialized())
	{
		return false;
	}
	const double Length = Spline->GetSplineLength();
	if (Length <= UE_KINDA_SMALL_NUMBER)
	{
		return false;
	}

	const bool bPlanet = Configuration->WorldMode == EWorldMode::SphericalPlanet;
	const FSphericalPlanetParams PlanetParams = bPlanet ? FVoxelWorldModeFactory::MakePlanetParams(*Configuration) : FSphericalPlanetParams();
	const FVoxelNoiseParams NoiseParams = Configuration->GetNoiseParams();

	// A vertex every Spacing along the spline, the last one on its end. The spline's length is an approximation (a
	// straight run of 3,000 uu reads a fraction of a unit long), which must not add a step.
	const double Spacing = FMath::Max(VertexSpacingVoxels, 0.25f) * Configuration->VoxelSize;
	const int32 Steps = FMath::Max(1, FMath::CeilToInt32(Length / Spacing - 0.01));
	const double Step = Length / Steps;

	// Where each vertex is (world X, Y or a direction), the spline's own height there and the natural terrain's.
	TArray<FVector> Positions;
	TArray<float> SplineHeights;
	TArray<float> Natural;
	TArray<float> HalfWidths;
	Positions.Reserve(Steps + 1);
	SplineHeights.Reserve(Steps + 1);
	Natural.Reserve(Steps + 1);
	HalfWidths.Reserve(Steps + 1);
	for (int32 Index = 0; Index <= Steps; ++Index)
	{
		const float Distance = static_cast<float>(FMath::Min(Index * Step, Length));
		const FVector World = Spline->GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
		if (bPlanet)
		{
			const FVector FromCentre = World - PlanetParams.PlanetCenter;
			const FVector Direction = FromCentre.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
			Positions.Add(Direction);
			SplineHeights.Add(static_cast<float>(FromCentre.Size() - PlanetParams.PlanetRadius));
			Natural.Add(static_cast<const FSphericalPlanetWorldMode*>(WorldMode)->GetTerrainAltitude(Direction, NoiseParams));
		}
		else
		{
			Positions.Add(FVector(World.X, World.Y, 0.0));
			SplineHeights.Add(static_cast<float>(World.Z));
			Natural.Add(static_cast<float>(WorldMode->GetSurfaceOnUpLine(FVector(World.X, World.Y, 0.0), NoiseParams).Position.Z));
		}
		HalfWidths.Add(HalfWidth * FMath::Abs(static_cast<float>(Spline->GetScaleAtDistanceAlongSpline(Distance).Y)));
	}

	// The targets.
	TArray<float> Targets;
	if (HeightMode == EVoxelTerrainLineHeight::SplineHeight)
	{
		Targets = MoveTemp(SplineHeights);
	}
	else
	{
		// The natural height averaged over SmoothingDistance each way along the line, then held to the grade from both
		// ends, then the offset. The average reaches as far back as forward, so it narrows toward each end and the line
		// meets the ground there: an average of what lies ahead alone would start a road on a rise with a mound.
		const int32 Window = FMath::Max(0, FMath::RoundToInt32(SmoothingDistance / Step));
		Targets.SetNumUninitialized(Natural.Num());
		for (int32 Index = 0; Index < Natural.Num(); ++Index)
		{
			const int32 Reach = FMath::Min3(Window, Index, Natural.Num() - 1 - Index);
			double Sum = 0.0;
			for (int32 Other = Index - Reach; Other <= Index + Reach; ++Other)
			{
				Sum += Natural[Other];
			}
			Targets[Index] = static_cast<float>(Sum / (2 * Reach + 1));
		}
		if (MaxGrade > 0.0f)
		{
			const float MaxStep = static_cast<float>(MaxGrade * Step);
			for (int32 Index = 1; Index < Targets.Num(); ++Index)
			{
				Targets[Index] = FMath::Clamp(Targets[Index], Targets[Index - 1] - MaxStep, Targets[Index - 1] + MaxStep);
			}
			for (int32 Index = Targets.Num() - 2; Index >= 0; --Index)
			{
				Targets[Index] = FMath::Clamp(Targets[Index], Targets[Index + 1] - MaxStep, Targets[Index + 1] + MaxStep);
			}
		}
		for (float& Target : Targets)
		{
			Target += TerrainOffset;
		}
	}

	OutParams = FVoxelConditioningLineParams();
	OutParams.Vertices.Reserve(Positions.Num());
	for (int32 Index = 0; Index < Positions.Num(); ++Index)
	{
		OutParams.Vertices.Add(FVoxelConditioningLineVertex(Positions[Index], Targets[Index], HalfWidths[Index]));
	}
	OutParams.FalloffWidth = FalloffWidth;
	OutParams.Strength = Strength;
	OutParams.Blend = Blend;
	OutParams.Id = GetLineId();
	OutParams.bRadial = bPlanet;
	OutParams.Surface = Surface;
	return true;
}

void UVoxelTerrainLineComponent::BeginPlay()
{
	Super::BeginPlay();

	// The voxel world started before this actor began play: register now (every level actor begins play before the
	// world's first tick, so before the chunk manager's first generation request). Else wait for it to start.
	if (UVoxelChunkManager* Manager = UVoxelChunkManager::Find(GetWorld()))
	{
		RegisterLine(*Manager);
	}
	else
	{
		InitializedHandle = UVoxelChunkManager::OnInitialized.AddUObject(this, &UVoxelTerrainLineComponent::HandleChunkManagerInitialized);
	}
}

void UVoxelTerrainLineComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (InitializedHandle.IsValid())
	{
		UVoxelChunkManager::OnInitialized.Remove(InitializedHandle);
		InitializedHandle.Reset();
	}
	Super::EndPlay(EndPlayReason);
}

void UVoxelTerrainLineComponent::HandleChunkManagerInitialized(UVoxelChunkManager& Manager)
{
	if (Manager.GetWorld() != GetWorld())
	{
		return;
	}
	UVoxelChunkManager::OnInitialized.Remove(InitializedHandle);
	InitializedHandle.Reset();
	RegisterLine(Manager);
}

void UVoxelTerrainLineComponent::RegisterLine(UVoxelChunkManager& Manager)
{
	if (RegisteredLine.IsValid())
	{
		return;
	}
	const FString Name = GetOwner() ? GetOwner()->GetName() : GetName();
	FVoxelConditioningLineParams Params;
	if (!BuildLineParams(Manager, Params))
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("Terrain line %s: no line was made (%s)."), *Name,
			GetSpline() ? TEXT("its spline has no length") : TEXT("its actor has no spline component, or none with the name asked for"));
		return;
	}
	const FVoxelConditioningLineRef Line = FVoxelConditioningLine::Make(Params);
	RegisteredLine = Line;
	LineWarnings = Manager.GetConditioningLineWarnings(*Line);

	float MinHalfWidth = TNumericLimits<float>::Max(), MaxHalfWidth = 0.0f;
	float MinTarget = TNumericLimits<float>::Max(), MaxTarget = TNumericLimits<float>::Lowest();
	for (const FVoxelConditioningLineVertex& Vertex : Params.Vertices)
	{
		MinHalfWidth = FMath::Min(MinHalfWidth, Vertex.HalfWidth);
		MaxHalfWidth = FMath::Max(MaxHalfWidth, Vertex.HalfWidth);
		MinTarget = FMath::Min(MinTarget, Vertex.TargetHeight);
		MaxTarget = FMath::Max(MaxTarget, Vertex.TargetHeight);
	}
	UE_LOG(LogVoxelStreaming, Log, TEXT("Terrain line %s: %d vertices over %.0f uu of spline, %s, half width %.0f to %.0f, target %.0f to %.0f, id %lld."),
		*Name, Params.Vertices.Num(), GetSpline()->GetSplineLength(),
		HeightMode == EVoxelTerrainLineHeight::FollowTerrain ? TEXT("following the terrain") : TEXT("at the spline's height"),
		MinHalfWidth, MaxHalfWidth, MinTarget, MaxTarget, Params.Id);
	for (const FString& Warning : LineWarnings)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("Terrain line %s: %s."), *Name, *Warning);
	}
	Manager.AddConditioningLine(Line, false);
}

AVoxelTerrainLineActor::AVoxelTerrainLineActor()
{
	PrimaryActorTick.bCanEverTick = false;

	Spline = CreateDefaultSubobject<USplineComponent>(TEXT("Spline"));
	SetRootComponent(Spline);
	// A line long enough to see: the default spline is 100 uu.
	Spline->SetSplinePoints({ FVector::ZeroVector, FVector(3000.0, 0.0, 0.0) }, ESplineCoordinateSpace::Local);

	TerrainLine = CreateDefaultSubobject<UVoxelTerrainLineComponent>(TEXT("TerrainLine"));
}
