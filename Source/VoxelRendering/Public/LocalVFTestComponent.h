// Copyright Daniel Raquel. All Rights Reserved.

/**
 * LocalVFTestComponent - Reference implementation for using FLocalVertexFactory
 *
 * This file demonstrates the CORRECT way to use FLocalVertexFactory in UE5:
 *
 * KEY LEARNINGS:
 * 1. Use FLocalVertexFactory DIRECTLY - don't extend it with IMPLEMENT_VERTEX_FACTORY_TYPE
 *    (extending creates a new shader type without compiled shaders)
 *
 * 2. For interleaved vertex buffers:
 *    - Vertex stream components work correctly (specify offset and stride)
 *    - SRVs cannot read interleaved data correctly
 *    - Use GNullColorVertexBuffer.VertexBufferSRV for Position/Tangent/TexCoord SRVs
 *    - Create SEPARATE contiguous buffer for ColorComponentsSRV
 *
 * 3. FColor uses BGRA memory layout - use PF_B8G8R8A8 for color SRVs
 *
 * 4. Use FPackedNormal (VET_PackedNormal) for tangent basis
 *
 * See VoxelVertexFactoryImplementationPlan.md Appendix A for full documentation.
 */

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "LocalVertexFactory.h"
#include "LocalVFTestComponent.generated.h"

/**
 * Helper function to initialize FLocalVertexFactory with interleaved vertex data.
 *
 * Uses FLocalVertexFactory directly (not extended) to leverage Epic's proven shaders.
 */
void InitLocalVertexFactoryStreams(
	FRHICommandListBase& RHICmdList,
	FLocalVertexFactory* VertexFactory,
	const FVertexBuffer* PositionBuffer,
	uint32 Stride,
	uint32 PositionOffset,
	uint32 TangentXOffset,
	uint32 TangentZOffset,
	uint32 TexCoordOffset,
	uint32 ColorOffset,
	FRHIShaderResourceView* PositionSRV,
	FRHIShaderResourceView* TangentsSRV,
	FRHIShaderResourceView* TexCoordSRV = nullptr,
	FRHIShaderResourceView* ColorSRV = nullptr
);

/**
 * Simple vertex structure compatible with FLocalVertexFactory.
 * Uses FPackedNormal for tangent basis (same format FLocalVertexFactory expects).
 *
 * 32 bytes per vertex (vs 28 for FVoxelVertex, but compatible with FLocalVertexFactory)
 */
struct FLocalVFTestVertex
{
	FVector3f Position;       // 12 bytes, offset 0
	FPackedNormal TangentX;   // 4 bytes, offset 12
	FPackedNormal TangentZ;   // 4 bytes, offset 16 (TangentZ.W contains sign for binormal)
	FVector2f TexCoord;       // 8 bytes, offset 20
	FColor Color;             // 4 bytes, offset 28
	// Total: 32 bytes

	FLocalVFTestVertex() = default;

	FLocalVFTestVertex(const FVector3f& InPos, const FVector3f& InNormal, const FVector3f& InTangent, const FVector2f& InUV, const FColor& InColor)
		: Position(InPos)
		, TangentX(InTangent)
		, TangentZ(InNormal)
		, TexCoord(InUV)
		, Color(InColor)
	{
		// Set W component of TangentZ to encode binormal sign (1 = no flip)
		TangentZ.Vector.W = 127;
	}
};

// Verify struct layout at compile time
static_assert(sizeof(FLocalVFTestVertex) == 32, "FLocalVFTestVertex size mismatch");
static_assert(STRUCT_OFFSET(FLocalVFTestVertex, Position) == 0, "Position offset mismatch");
static_assert(STRUCT_OFFSET(FLocalVFTestVertex, TangentX) == 12, "TangentX offset mismatch");
static_assert(STRUCT_OFFSET(FLocalVFTestVertex, TangentZ) == 16, "TangentZ offset mismatch");
static_assert(STRUCT_OFFSET(FLocalVFTestVertex, TexCoord) == 20, "TexCoord offset mismatch");
static_assert(STRUCT_OFFSET(FLocalVFTestVertex, Color) == 28, "Color offset mismatch");

/**
 * Vertex buffer for FLocalVFTestVertex.
 *
 * For FLocalVertexFactory compatibility, we need:
 * - Interleaved vertex buffer for vertex stream components (position, tangent, texcoord, color)
 * - Separate color buffer for ColorComponentsSRV (FLocalVertexFactory uses SRV for color fetch)
 */
class VOXELRENDERING_API FLocalVFTestVertexBuffer : public FVertexBuffer
{
public:
	TArray<FLocalVFTestVertex> Vertices;

	/** Separate buffer containing just color data for SRV access */
	FBufferRHIRef ColorBufferRHI;

	/** SRV for vertex colors - reads from separate color buffer */
	FShaderResourceViewRHIRef ColorSRV;

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual void ReleaseRHI() override;
	virtual FString GetFriendlyName() const override { return TEXT("FLocalVFTestVertexBuffer"); }

	int32 GetNumVertices() const { return Vertices.Num(); }
};

/**
 * Index buffer for the test.
 */
class VOXELRENDERING_API FLocalVFTestIndexBuffer : public FIndexBuffer
{
public:
	TArray<uint32> Indices;

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual FString GetFriendlyName() const override { return TEXT("FLocalVFTestIndexBuffer"); }

	int32 GetNumIndices() const { return Indices.Num(); }
};

/**
 * Test component using FLocalVertexFactory directly.
 *
 * This approach:
 * - Uses FLocalVertexFactory directly (not extended) to leverage Epic's proven shaders
 * - No custom shader needed - uses LocalVertexFactory.ush
 * - Sets up FDataType with proper vertex stream offsets and SRVs
 *
 * If this renders correctly, we can refactor FVoxelVertexFactory to use this pattern.
 *
 * Usage:
 * 1. Add this component to an Actor
 * 2. Create a material with VertexColor node connected to Base Color
 * 3. Assign that material to the component's Material property
 * 4. The quad should appear with vertex colors (red/green/blue/yellow corners)
 *
 * Note: The default material does NOT display vertex colors. You must assign
 * a vertex color material to see the test colors.
 */
UCLASS(ClassGroup=(Rendering), meta=(BlueprintSpawnableComponent))
class VOXELRENDERING_API ULocalVFTestComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	ULocalVFTestComponent();

	//~ Begin UPrimitiveComponent Interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual int32 GetNumMaterials() const override { return 1; }
	virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;
	virtual void SetMaterial(int32 ElementIndex, UMaterialInterface* InMaterial) override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;
	//~ End UPrimitiveComponent Interface

	/** Material to render with. If null, uses default material. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local VF Test")
	TObjectPtr<UMaterialInterface> Material;

	/** Size of the test quad in cm */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local VF Test", meta = (ClampMin = "1.0"))
	float QuadSize = 100.0f;

	/** Refresh the mesh (call after changing properties at runtime) */
	UFUNCTION(BlueprintCallable, Category = "Local VF Test")
	void RefreshMesh();
};
