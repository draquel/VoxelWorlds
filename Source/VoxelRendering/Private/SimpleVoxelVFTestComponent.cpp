// Copyright Daniel Raquel. All Rights Reserved.

#include "SimpleVoxelVFTestComponent.h"
#include "SimpleVoxelVertexFactory.h"
#include "VoxelRendering.h"
#include "PrimitiveSceneProxy.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "MaterialShared.h"
#include "Engine/Engine.h"
#include "SceneManagement.h"
#include "RenderResource.h"

// ============================================================================
// Scene Proxy using FSimpleVoxelVertexFactory with custom shader
// ============================================================================

class FSimpleVoxelVFTestSceneProxy : public FPrimitiveSceneProxy
{
public:
	FSimpleVoxelVFTestSceneProxy(USimpleVoxelVFTestComponent* Component)
		: FPrimitiveSceneProxy(Component)
		, MaterialInterface(Component->GetMaterial(0))
		, VertexFactory(GetScene().GetFeatureLevel())
	{
		// Get material (use default if none set)
		if (MaterialInterface == nullptr)
		{
			MaterialInterface = UMaterial::GetDefaultMaterial(MD_Surface);
		}

		// Cache material relevance for GetViewRelevance
		MaterialRelevance = MaterialInterface->GetRelevance(GetScene().GetFeatureLevel());

		// Build quad geometry in LOCAL space
		const float HalfSize = Component->QuadSize * 0.5f;
		const FVector3f Normal(0, 0, 1);

		// Add 4 vertices for the quad
		VertexBuffer.Vertices.Add(FSimpleVoxelVertex(
			FVector3f(-HalfSize, -HalfSize, 0), Normal, FVector2f(0, 0), FColor::Red));
		VertexBuffer.Vertices.Add(FSimpleVoxelVertex(
			FVector3f(HalfSize, -HalfSize, 0), Normal, FVector2f(1, 0), FColor::Green));
		VertexBuffer.Vertices.Add(FSimpleVoxelVertex(
			FVector3f(HalfSize, HalfSize, 0), Normal, FVector2f(1, 1), FColor::Blue));
		VertexBuffer.Vertices.Add(FSimpleVoxelVertex(
			FVector3f(-HalfSize, HalfSize, 0), Normal, FVector2f(0, 1), FColor::Yellow));

		// Two triangles (CCW winding when viewed from +Z)
		IndexBuffer.Indices = { 0, 1, 2, 0, 2, 3 };

		UE_LOG(LogVoxelRendering, Log, TEXT("FSimpleVoxelVFTestSceneProxy: Created with %d vertices, %d indices"),
			VertexBuffer.GetNumVertices(), IndexBuffer.GetNumIndices());
	}

	virtual ~FSimpleVoxelVFTestSceneProxy()
	{
		VertexBuffer.ReleaseResource();
		IndexBuffer.ReleaseResource();
		VertexFactory.ReleaseResource();
	}

	virtual SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override
	{
		// Initialize buffers first
		VertexBuffer.InitResource(RHICmdList);
		IndexBuffer.InitResource(RHICmdList);

		// Initialize vertex factory with the vertex buffer
		VertexFactory.Init(RHICmdList, &VertexBuffer);
		VertexFactory.InitResource(RHICmdList);

		UE_LOG(LogVoxelRendering, Log, TEXT("FSimpleVoxelVFTestSceneProxy: Render resources created"));
	}

	virtual void GetDynamicMeshElements(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_SimpleVoxelVFTestSceneProxy_GetDynamicMeshElements);

		// Check we have valid resources
		if (VertexBuffer.GetNumVertices() == 0 || IndexBuffer.GetNumIndices() == 0)
		{
			return;
		}

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
				// Allocate mesh batch
				FMeshBatch& Mesh = Collector.AllocateMesh();

				// Fill in mesh batch
				Mesh.VertexFactory = &VertexFactory;
				Mesh.MaterialRenderProxy = MaterialProxy;
				Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
				Mesh.Type = PT_TriangleList;
				Mesh.DepthPriorityGroup = SDPG_World;
				Mesh.bCanApplyViewModeOverrides = true;
				Mesh.bUseWireframeSelectionColoring = IsSelected();

				// Fill in batch element
				FMeshBatchElement& BatchElement = Mesh.Elements[0];
				BatchElement.IndexBuffer = &IndexBuffer;
				BatchElement.FirstIndex = 0;
				BatchElement.NumPrimitives = IndexBuffer.GetNumIndices() / 3;
				BatchElement.MinVertexIndex = 0;
				BatchElement.MaxVertexIndex = VertexBuffer.GetNumVertices() - 1;

				// Provide primitive uniform buffer (contains LocalToWorld transform)
				BatchElement.PrimitiveUniformBuffer = GetUniformBuffer();

				Collector.AddMesh(ViewIndex, Mesh);
			}
		}
	}

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		Result.bDrawRelevance = IsShown(View);
		Result.bShadowRelevance = IsShadowCast(View);
		Result.bDynamicRelevance = true;
		Result.bStaticRelevance = false;
		Result.bRenderInMainPass = ShouldRenderInMainPass();
		Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
		Result.bRenderCustomDepth = ShouldRenderCustomDepth();
		MaterialRelevance.SetPrimitiveViewRelevance(Result);
		return Result;
	}

	virtual uint32 GetMemoryFootprint() const override
	{
		return sizeof(*this) + GetAllocatedSize();
	}

	SIZE_T GetAllocatedSize() const
	{
		return VertexBuffer.Vertices.GetAllocatedSize() +
		       IndexBuffer.Indices.GetAllocatedSize();
	}

private:
	UMaterialInterface* MaterialInterface;
	FMaterialRelevance MaterialRelevance;

	FSimpleVoxelVertexBuffer VertexBuffer;
	FSimpleVoxelIndexBuffer IndexBuffer;
	FSimpleVoxelVertexFactory VertexFactory;
};

// ============================================================================
// Component Implementation
// ============================================================================

USimpleVoxelVFTestComponent::USimpleVoxelVFTestComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	bWantsOnUpdateTransform = false;
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	SetCastShadow(true);
}

FPrimitiveSceneProxy* USimpleVoxelVFTestComponent::CreateSceneProxy()
{
	if (QuadSize <= 0)
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("USimpleVoxelVFTestComponent: QuadSize <= 0, not creating scene proxy"));
		return nullptr;
	}

	UE_LOG(LogVoxelRendering, Log, TEXT("USimpleVoxelVFTestComponent: Creating scene proxy with Material=%s, QuadSize=%.1f"),
		Material ? *Material->GetName() : TEXT("nullptr (using default)"), QuadSize);

	return new FSimpleVoxelVFTestSceneProxy(this);
}

FBoxSphereBounds USimpleVoxelVFTestComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	const float HalfSize = QuadSize * 0.5f;
	FBox LocalBox(
		FVector(-HalfSize, -HalfSize, -1.0f),
		FVector(HalfSize, HalfSize, 1.0f)
	);
	return FBoxSphereBounds(LocalBox).TransformBy(LocalToWorld);
}

UMaterialInterface* USimpleVoxelVFTestComponent::GetMaterial(int32 ElementIndex) const
{
	return Material;
}

void USimpleVoxelVFTestComponent::SetMaterial(int32 ElementIndex, UMaterialInterface* InMaterial)
{
	Material = InMaterial;
	MarkRenderStateDirty();
}

void USimpleVoxelVFTestComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	if (Material != nullptr)
	{
		OutMaterials.Add(Material);
	}
	else
	{
		OutMaterials.Add(UMaterial::GetDefaultMaterial(MD_Surface));
	}
}

void USimpleVoxelVFTestComponent::RefreshMesh()
{
	MarkRenderStateDirty();
}
