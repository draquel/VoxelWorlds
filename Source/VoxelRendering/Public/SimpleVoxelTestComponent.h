// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "SimpleVoxelTestComponent.generated.h"

/**
 * Test component that renders a simple colored quad using FLocalVertexFactory.
 *
 * This is Phase 2 of the vertex factory implementation:
 * - Uses Epic's built-in FLocalVertexFactory (battle-tested)
 * - Verifies the entire rendering pipeline works
 * - Eliminates custom shader as a variable
 *
 * If this renders correctly, we know the C++ rendering pipeline is solid
 * and can proceed to custom vertex factory implementation.
 *
 * Usage:
 * 1. Add this component to an Actor
 * 2. Optionally assign a material (uses default if none)
 * 3. The quad should appear with vertex colors (red/green/blue/yellow corners)
 */
UCLASS(ClassGroup=(Rendering), meta=(BlueprintSpawnableComponent))
class VOXELRENDERING_API USimpleVoxelTestComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	USimpleVoxelTestComponent();

	//~ Begin UPrimitiveComponent Interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual int32 GetNumMaterials() const override { return 1; }
	virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;
	virtual void SetMaterial(int32 ElementIndex, UMaterialInterface* InMaterial) override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;
	//~ End UPrimitiveComponent Interface

	/** Material to render with. If null, uses default material. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Simple Voxel Test")
	TObjectPtr<UMaterialInterface> Material;

	/** Size of the test quad in cm */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Simple Voxel Test", meta = (ClampMin = "1.0"))
	float QuadSize = 100.0f;

	/** Refresh the mesh (call after changing properties at runtime) */
	UFUNCTION(BlueprintCallable, Category = "Simple Voxel Test")
	void RefreshMesh();
};
