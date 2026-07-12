// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelFieldPreviewActor.h"

#include "VoxelFieldTypes.h"
#include "VoxelFieldImageBaker.h"
#include "VoxelWorldConfiguration.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "UObject/ConstructorHelpers.h"

#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"

static const FName FieldTextureParamName(TEXT("FieldTexture"));

AVoxelFieldPreviewActor::AVoxelFieldPreviewActor()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;

	Plane = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PreviewPlane"));
	Plane->SetupAttachment(SceneRoot);
	Plane->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Plane->SetCastShadow(false);

	// Engine unit plane (100x100 uu, facing +Z) — scaled to the region at bake time.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneMeshFinder(TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (PlaneMeshFinder.Succeeded())
	{
		Plane->SetStaticMesh(PlaneMeshFinder.Object);
	}
}

void AVoxelFieldPreviewActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Rebake();
}

#if WITH_EDITOR
void AVoxelFieldPreviewActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	Rebake();
}

void AVoxelFieldPreviewActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	// Re-bake continuously while dragging so the region follows the actor.
	Rebake();
}
#endif

void AVoxelFieldPreviewActor::Regenerate()
{
	Rebake();
}

TArray<FString> AVoxelFieldPreviewActor::GetFieldOptions() const
{
	FVoxelFieldRegistry::EnsureBuiltinsRegistered();

	TArray<FString> Options;
	for (const FVoxelEditorField& F : FVoxelFieldRegistry::GetFields())
	{
		Options.Add(F.Id.ToString());
	}
	return Options;
}

void AVoxelFieldPreviewActor::EnsureMaterial()
{
	if (BaseMaterial)
	{
		return;
	}

#if WITH_EDITOR
	// Build a minimal unlit material: TextureSampleParameter2D("FieldTexture") -> Emissive.
	// Constructed in code so the tool carries no content-asset dependency.
	UMaterial* Material = NewObject<UMaterial>(this, NAME_None, RF_Transient);
	Material->MaterialDomain = MD_Surface;
	Material->BlendMode = BLEND_Opaque;
	Material->SetShadingModel(MSM_Unlit);
	Material->TwoSided = true;

	UMaterialExpressionTextureSampleParameter2D* TexParam =
		NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
	TexParam->ParameterName = FieldTextureParamName;
	TexParam->SamplerType = SAMPLERTYPE_Color;
	if (GEngine)
	{
		TexParam->Texture = GEngine->DefaultTexture; // valid default so the base material compiles
	}

	Material->GetExpressionCollection().AddExpression(TexParam);
	Material->GetEditorOnlyData()->EmissiveColor.Connect(0, TexParam);
	Material->PostEditChange();

	BaseMaterial = Material;
#endif
}

void AVoxelFieldPreviewActor::Rebake()
{
	if (!Plane)
	{
		return;
	}

	// Always keep the plane sized to the region, even before a successful bake.
	const float PlaneScale = FMath::Max(RegionSize, 1.0f) / 100.0f; // engine plane is 100 uu
	Plane->SetRelativeScale3D(FVector(PlaneScale, PlaneScale, 1.0f));

	if (!Configuration)
	{
		return;
	}

	FVoxelFieldRegistry::EnsureBuiltinsRegistered();
	const FVoxelEditorField* FieldDef = FVoxelFieldRegistry::FindField(Field);
	if (!FieldDef)
	{
		return;
	}

	FVoxelFieldSampleContext Ctx = FVoxelFieldSampleContext::FromConfiguration(Configuration);
	if (!Ctx.IsValid())
	{
		return;
	}

	const FVector Location = GetActorLocation();
	FVoxelFieldBakeParams BakeParams;
	BakeParams.Center = FVector2D(Location.X, Location.Y);
	BakeParams.RegionSize = RegionSize;
	BakeParams.Resolution = Resolution;
	BakeParams.SampleZ = SampleZ;

	float RangeMin = 0.0f, RangeMax = 0.0f;
	UTexture2D* Tex = FVoxelFieldImageBaker::BakeToTexture(*FieldDef, Ctx, BakeParams, PreviewTexture, RangeMin, RangeMax);
	PreviewTexture = Tex;

	EnsureMaterial();
	if (BaseMaterial && !DynMaterial)
	{
		DynMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, this);
	}
	if (DynMaterial)
	{
		DynMaterial->SetTextureParameterValue(FieldTextureParamName, PreviewTexture);
		Plane->SetMaterial(0, DynMaterial);
	}
}
