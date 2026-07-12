// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelFieldPreviewActor.generated.h"

class UVoxelWorldConfiguration;
class UStaticMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UTexture2D;

/**
 * Editor-only actor that visualizes a procedural field over the region it frames.
 *
 * Drop it into a level, assign a UVoxelWorldConfiguration, pick a Field, and it bakes that field
 * (via the shared FVoxelFieldSampleContext + FVoxelFieldImageBaker) onto a flat plane sized to
 * RegionSize and centered on the actor — so moving the actor re-frames the region. Lives in the
 * VoxelWorldsEditor module (Type "Editor"), so it never cooks into a shipping build; do not leave
 * one saved in a level you intend to ship.
 *
 * Shares its entire sampling/baking core with the "Voxel Field Preview" dock panel.
 */
UCLASS(meta = (DisplayName = "Voxel Field Preview"))
class VOXELWORLDSEDITOR_API AVoxelFieldPreviewActor : public AActor
{
	GENERATED_BODY()

public:
	AVoxelFieldPreviewActor();

	/** World configuration whose procedural fields are visualized. */
	UPROPERTY(EditAnywhere, Category = "Voxel Field Preview")
	TObjectPtr<UVoxelWorldConfiguration> Configuration;

	/** Which field to display (Height, Biome, CavePresence, ...). Options come from the field registry. */
	UPROPERTY(EditAnywhere, Category = "Voxel Field Preview", meta = (GetOptions = "GetFieldOptions"))
	FName Field = TEXT("Height");

	/** Full side length of the visualized square region, in world units (centered on the actor). */
	UPROPERTY(EditAnywhere, Category = "Voxel Field Preview", meta = (ClampMin = "100"))
	float RegionSize = 51200.0f;

	/** Pixels per side of the baked image (higher = sharper, slower). */
	UPROPERTY(EditAnywhere, Category = "Voxel Field Preview", meta = (ClampMin = "1", ClampMax = "4096"))
	int32 Resolution = 256;

	/** World Z at which 3D fields (e.g. cave presence) are sampled. Ignored by 2D fields. */
	UPROPERTY(EditAnywhere, Category = "Voxel Field Preview")
	float SampleZ = 0.0f;

	/** Re-bake the field image now (also runs automatically when a property changes or the actor moves). */
	UFUNCTION(CallInEditor, Category = "Voxel Field Preview")
	void Regenerate();

	virtual void OnConstruction(const FTransform& Transform) override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
#endif

private:
	/** Field id options for the Field dropdown. */
	UFUNCTION()
	TArray<FString> GetFieldOptions() const;

	/** Bake the current field into the preview texture and update the plane. */
	void Rebake();

	/** Lazily build the self-contained unlit texture material (editor-only, no content asset). */
	void EnsureMaterial();

	UPROPERTY()
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> Plane;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> BaseMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> DynMaterial;

	UPROPERTY()
	TObjectPtr<UTexture2D> PreviewTexture;
};
