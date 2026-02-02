// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "SimpleVoxelVFTestComponent.generated.h"

/**
 * Test component that renders using FSimpleVoxelVertexFactory - Phase 3
 *
 * This component demonstrates using a custom vertex factory class
 * (FSimpleVoxelVertexFactory) which extends FLocalVertexFactory.
 *
 * Key differences from SimpleVoxelTestComponent:
 * - Uses custom vertex factory class instead of FDynamicMeshBuilder
 * - Manages vertex/index buffers directly
 * - Sets up FLocalVertexFactory::FDataType with proper SRVs
 *
 * Vertices are in LOCAL space - the component's transform (LocalToWorld)
 * is applied by the shader.
 *
 * Usage:
 * 1. Add this component to an Actor
 * 2. Optionally assign a material (uses default if none)
 * 3. The quad should appear with vertex colors (red/green/blue/yellow corners)
 */
UCLASS(ClassGroup=(Rendering), meta=(BlueprintSpawnableComponent))
class VOXELRENDERING_API USimpleVoxelVFTestComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	USimpleVoxelVFTestComponent();

	//~ Begin UPrimitiveComponent Interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual int32 GetNumMaterials() const override { return 1; }
	virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;
	virtual void SetMaterial(int32 ElementIndex, UMaterialInterface* InMaterial) override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;
	//~ End UPrimitiveComponent Interface

	/** Material to render with. If null, uses default material. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Simple Voxel VF Test")
	TObjectPtr<UMaterialInterface> Material;

	/** Size of the test quad in cm */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Simple Voxel VF Test", meta = (ClampMin = "1.0"))
	float QuadSize = 100.0f;

	/** Refresh the mesh (call after changing properties at runtime) */
	UFUNCTION(BlueprintCallable, Category = "Simple Voxel VF Test")
	void RefreshMesh();
};
