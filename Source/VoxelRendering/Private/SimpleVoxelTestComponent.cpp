// Copyright Daniel Raquel. All Rights Reserved.

#include "SimpleVoxelTestComponent.h"
#include "VoxelRendering.h"
#include "PrimitiveSceneProxy.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "MaterialShared.h"
#include "Engine/Engine.h"
#include "DynamicMeshBuilder.h"
#include "SceneManagement.h"

// ============================================================================
// Scene Proxy using FDynamicMeshBuilder (handles FLocalVertexFactory internally)
// ============================================================================

class FSimpleVoxelTestSceneProxy : public FPrimitiveSceneProxy
{
public:
	FSimpleVoxelTestSceneProxy(USimpleVoxelTestComponent* Component)
		: FPrimitiveSceneProxy(Component)
		, MaterialInterface(Component->GetMaterial(0))
		, QuadSize(Component->QuadSize)
	{
		// Get material (use default if none set)
		if (MaterialInterface == nullptr)
		{
			MaterialInterface = UMaterial::GetDefaultMaterial(MD_Surface);
		}

		// Cache material relevance for GetViewRelevance
		MaterialRelevance = MaterialInterface->GetRelevance(GetScene().GetFeatureLevel());

		UE_LOG(LogVoxelRendering, Log, TEXT("FSimpleVoxelTestSceneProxy: Created with QuadSize=%.1f"), QuadSize);
	}

	virtual SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	virtual void GetDynamicMeshElements(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_SimpleVoxelTestSceneProxy_GetDynamicMeshElements);

		// Setup wireframe material if in wireframe mode
		const bool bWireframe = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;
		FMaterialRenderProxy* MaterialProxy = nullptr;

		if (bWireframe)
		{
			FColoredMaterialRenderProxy* WireframeMaterialInstance = new FColoredMaterialRenderProxy(
				GEngine->WireframeMaterial ? GEngine->WireframeMaterial->GetRenderProxy() : nullptr,
				FLinearColor(0, 0.5f, 1.f)
			);
			Collector.RegisterOneFrameMaterialProxy(WireframeMaterialInstance);
			MaterialProxy = WireframeMaterialInstance;
		}
		else
		{
			MaterialProxy = MaterialInterface->GetRenderProxy();
		}

		// For each view that can see us
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			if (VisibilityMap & (1 << ViewIndex))
			{
				const FSceneView* View = Views[ViewIndex];

				// Use FDynamicMeshBuilder which handles FLocalVertexFactory setup correctly
				FDynamicMeshBuilder MeshBuilder(View->GetFeatureLevel());

				const float HalfSize = QuadSize * 0.5f;
				const FVector3f Normal(0, 0, 1);
				const FVector3f Tangent(1, 0, 0);

				// Add 4 vertices for the quad (in local space)
				// Bottom-left (red)
				MeshBuilder.AddVertex(
					FDynamicMeshVertex(
						FVector3f(-HalfSize, -HalfSize, 0),
						Tangent,
						Normal,
						FVector2f(0, 0),
						FColor::Red
					)
				);
				// Bottom-right (green)
				MeshBuilder.AddVertex(
					FDynamicMeshVertex(
						FVector3f(HalfSize, -HalfSize, 0),
						Tangent,
						Normal,
						FVector2f(1, 0),
						FColor::Green
					)
				);
				// Top-right (blue)
				MeshBuilder.AddVertex(
					FDynamicMeshVertex(
						FVector3f(HalfSize, HalfSize, 0),
						Tangent,
						Normal,
						FVector2f(1, 1),
						FColor::Blue
					)
				);
				// Top-left (yellow)
				MeshBuilder.AddVertex(
					FDynamicMeshVertex(
						FVector3f(-HalfSize, HalfSize, 0),
						Tangent,
						Normal,
						FVector2f(0, 1),
						FColor::Yellow
					)
				);

				// Add 2 triangles (CCW winding when viewed from +Z)
				MeshBuilder.AddTriangle(0, 1, 2);
				MeshBuilder.AddTriangle(0, 2, 3);

				// Get the mesh batch from the builder
				MeshBuilder.GetMesh(
					GetLocalToWorld(),
					MaterialProxy,
					SDPG_World,
					true,  // bDisableBackfaceCulling
					false, // bReceivesDecals
					ViewIndex,
					Collector
				);
			}
		}
	}

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		Result.bDrawRelevance = IsShown(View);
		Result.bShadowRelevance = IsShadowCast(View);
		Result.bDynamicRelevance = true;  // We use GetDynamicMeshElements
		Result.bStaticRelevance = false;
		Result.bRenderInMainPass = ShouldRenderInMainPass();
		Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
		Result.bRenderCustomDepth = ShouldRenderCustomDepth();
		MaterialRelevance.SetPrimitiveViewRelevance(Result);
		return Result;
	}

	virtual uint32 GetMemoryFootprint() const override
	{
		return sizeof(*this);
	}

private:
	UMaterialInterface* MaterialInterface;
	FMaterialRelevance MaterialRelevance;
	float QuadSize;
};

// ============================================================================
// Component Implementation
// ============================================================================

USimpleVoxelTestComponent::USimpleVoxelTestComponent()
{
	PrimaryComponentTick.bCanEverTick = false;

	// Enable rendering
	bWantsOnUpdateTransform = false;
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);

	// Cast shadows
	SetCastShadow(true);
}

FPrimitiveSceneProxy* USimpleVoxelTestComponent::CreateSceneProxy()
{
	if (QuadSize <= 0)
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("USimpleVoxelTestComponent: QuadSize <= 0, not creating scene proxy"));
		return nullptr;
	}

	UE_LOG(LogVoxelRendering, Log, TEXT("USimpleVoxelTestComponent: Creating scene proxy with Material=%s, QuadSize=%.1f"),
		Material ? *Material->GetName() : TEXT("nullptr (using default)"), QuadSize);

	return new FSimpleVoxelTestSceneProxy(this);
}

FBoxSphereBounds USimpleVoxelTestComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	const float HalfSize = QuadSize * 0.5f;
	FBox LocalBox(
		FVector(-HalfSize, -HalfSize, -1.0f),
		FVector(HalfSize, HalfSize, 1.0f)
	);
	return FBoxSphereBounds(LocalBox).TransformBy(LocalToWorld);
}

UMaterialInterface* USimpleVoxelTestComponent::GetMaterial(int32 ElementIndex) const
{
	return Material;
}

void USimpleVoxelTestComponent::SetMaterial(int32 ElementIndex, UMaterialInterface* InMaterial)
{
	Material = InMaterial;
	MarkRenderStateDirty();
}

void USimpleVoxelTestComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	if (Material != nullptr)
	{
		OutMaterials.Add(Material);
	}
	else
	{
		// Report the default material that will be used
		OutMaterials.Add(UMaterial::GetDefaultMaterial(MD_Surface));
	}
}

void USimpleVoxelTestComponent::RefreshMesh()
{
	MarkRenderStateDirty();
}
