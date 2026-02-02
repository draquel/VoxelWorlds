// Copyright Daniel Raquel. All Rights Reserved.

#include "LocalVFTestComponent.h"
#include "VoxelRendering.h"
#include "PrimitiveSceneProxy.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "MaterialShared.h"
#include "Engine/Engine.h"
#include "SceneManagement.h"
#include "RenderResource.h"
#include "StaticMeshResources.h"
#include "RenderUtils.h"
#include "RHI.h"
#include "GlobalRenderResources.h"

// ============================================================================
// Helper Function to Initialize FLocalVertexFactory
// ============================================================================

/**
 * Initialize an FLocalVertexFactory with interleaved vertex data.
 * Uses FLocalVertexFactory directly (no custom type) to leverage Epic's proven shaders.
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
	FRHIShaderResourceView* TexCoordSRV,
	FRHIShaderResourceView* ColorSRV)
{
	check(IsInRenderingThread());
	check(VertexFactory != nullptr);
	check(PositionBuffer != nullptr);

	// Build FDataType for FLocalVertexFactory
	FLocalVertexFactory::FDataType NewData;

	// Position stream component - reads from interleaved buffer via vertex input
	NewData.PositionComponent = FVertexStreamComponent(
		PositionBuffer,
		PositionOffset,
		Stride,
		VET_Float3
	);

	// For interleaved buffers, the SRVs we create are incorrectly formatted for manual vertex fetch.
	// Use global null buffer SRVs to satisfy uniform buffer requirements.
	// The actual vertex data comes from the vertex stream components which read correctly from interleaved data.
	NewData.PositionComponentSRV = GNullColorVertexBuffer.VertexBufferSRV;

	// Tangent basis stream components - read from interleaved buffer via vertex input
	NewData.TangentBasisComponents[0] = FVertexStreamComponent(
		PositionBuffer,
		TangentXOffset,
		Stride,
		VET_PackedNormal
	);
	NewData.TangentBasisComponents[1] = FVertexStreamComponent(
		PositionBuffer,
		TangentZOffset,
		Stride,
		VET_PackedNormal
	);

	// Tangents SRV - use global null buffer
	NewData.TangentsSRV = GNullColorVertexBuffer.VertexBufferSRV;

	// Texture coordinates - read from interleaved buffer via vertex input
	NewData.TextureCoordinates.Empty();
	NewData.TextureCoordinates.Add(FVertexStreamComponent(
		PositionBuffer,
		TexCoordOffset,
		Stride,
		VET_Float2
	));
	NewData.TextureCoordinatesSRV = GNullColorVertexBuffer.VertexBufferSRV;

	// Vertex color - read from interleaved buffer via vertex input
	NewData.ColorComponent = FVertexStreamComponent(
		PositionBuffer,
		ColorOffset,
		Stride,
		VET_Color
	);
	// Use provided ColorSRV (should point to separate color buffer for correct SRV access)
	NewData.ColorComponentsSRV = ColorSRV ? ColorSRV : GNullColorVertexBuffer.VertexBufferSRV.GetReference();

	// Light map coordinate index
	NewData.LightMapCoordinateIndex = 0;
	NewData.NumTexCoords = 1;

	// Set the data on the vertex factory
	VertexFactory->SetData(RHICmdList, NewData);

	UE_LOG(LogVoxelRendering, Log, TEXT("InitLocalVertexFactoryStreams - Configured with Stride=%d"), Stride);
}

// ============================================================================
// FLocalVFTestVertexBuffer Implementation
// ============================================================================

void FLocalVFTestVertexBuffer::InitRHI(FRHICommandListBase& RHICmdList)
{
	if (Vertices.Num() == 0)
	{
		return;
	}

	const uint32 NumVertices = Vertices.Num();
	const uint32 SizeInBytes = NumVertices * sizeof(FLocalVFTestVertex);

	// Create interleaved vertex buffer for vertex stream components
	FRHIResourceCreateInfo CreateInfo(TEXT("LocalVFTestVertexBuffer"));

	VertexBufferRHI = RHICmdList.CreateBuffer(
		SizeInBytes,
		BUF_Static | BUF_VertexBuffer,
		sizeof(FLocalVFTestVertex),
		ERHIAccess::VertexOrIndexBuffer,
		CreateInfo
	);

	void* Data = RHICmdList.LockBuffer(VertexBufferRHI, 0, SizeInBytes, RLM_WriteOnly);
	FMemory::Memcpy(Data, Vertices.GetData(), SizeInBytes);
	RHICmdList.UnlockBuffer(VertexBufferRHI);

	// Create separate color buffer for SRV access
	// FLocalVertexFactory uses ColorComponentsSRV for manual vertex fetch of colors
	const uint32 ColorBufferSize = NumVertices * sizeof(FColor);
	FRHIResourceCreateInfo ColorCreateInfo(TEXT("LocalVFTestColorBuffer"));

	ColorBufferRHI = RHICmdList.CreateBuffer(
		ColorBufferSize,
		BUF_Static | BUF_ShaderResource,
		sizeof(FColor),
		ERHIAccess::SRVMask,
		ColorCreateInfo
	);

	// Copy just the color data to the separate buffer
	FColor* ColorData = (FColor*)RHICmdList.LockBuffer(ColorBufferRHI, 0, ColorBufferSize, RLM_WriteOnly);
	for (int32 i = 0; i < Vertices.Num(); i++)
	{
		ColorData[i] = Vertices[i].Color;
	}
	RHICmdList.UnlockBuffer(ColorBufferRHI);

	// Create SRV for colors from the separate color buffer
	// PF_B8G8R8A8 matches FColor's BGRA memory layout
	ColorSRV = RHICmdList.CreateShaderResourceView(ColorBufferRHI, sizeof(FColor), PF_B8G8R8A8);

	UE_LOG(LogVoxelRendering, Log, TEXT("FLocalVFTestVertexBuffer: Created with %d vertices (%d bytes), ColorSRV: %s"),
		NumVertices, SizeInBytes, ColorSRV.IsValid() ? TEXT("OK") : TEXT("NULL"));
}

void FLocalVFTestVertexBuffer::ReleaseRHI()
{
	ColorSRV.SafeRelease();
	ColorBufferRHI.SafeRelease();
	FVertexBuffer::ReleaseRHI();
}

// ============================================================================
// FLocalVFTestIndexBuffer Implementation
// ============================================================================

void FLocalVFTestIndexBuffer::InitRHI(FRHICommandListBase& RHICmdList)
{
	if (Indices.Num() == 0)
	{
		return;
	}

	const uint32 NumIndices = Indices.Num();
	const uint32 SizeInBytes = NumIndices * sizeof(uint32);

	FRHIResourceCreateInfo CreateInfo(TEXT("LocalVFTestIndexBuffer"));

	IndexBufferRHI = RHICmdList.CreateBuffer(
		SizeInBytes,
		BUF_Static | BUF_IndexBuffer,
		sizeof(uint32),
		ERHIAccess::VertexOrIndexBuffer,
		CreateInfo
	);

	void* Data = RHICmdList.LockBuffer(IndexBufferRHI, 0, SizeInBytes, RLM_WriteOnly);
	FMemory::Memcpy(Data, Indices.GetData(), SizeInBytes);
	RHICmdList.UnlockBuffer(IndexBufferRHI);

	UE_LOG(LogVoxelRendering, Log, TEXT("FLocalVFTestIndexBuffer: Created with %d indices (%d bytes)"),
		NumIndices, SizeInBytes);
}

// ============================================================================
// Scene Proxy using FLocalVoxelVertexFactory
// ============================================================================

class FLocalVFTestSceneProxy : public FPrimitiveSceneProxy
{
public:
	FLocalVFTestSceneProxy(ULocalVFTestComponent* Component)
		: FPrimitiveSceneProxy(Component)
		, MaterialInterface(Component->GetMaterial(0))
		, VertexFactory(GetScene().GetFeatureLevel(), "FLocalVFTestSceneProxy")
	{
		// Get material (use default if none set)
		if (MaterialInterface == nullptr)
		{
			MaterialInterface = UMaterial::GetDefaultMaterial(MD_Surface);
			UE_LOG(LogVoxelRendering, Warning,
				TEXT("FLocalVFTestSceneProxy: No material set - using default material which does NOT display vertex colors. "
				     "Assign a material with VertexColor node connected to BaseColor to see the test colors."));
		}

		// Cache material relevance for GetViewRelevance
		MaterialRelevance = MaterialInterface->GetRelevance(GetScene().GetFeatureLevel());

		// Build quad geometry in LOCAL space
		const float HalfSize = Component->QuadSize * 0.5f;

		// Normal facing +Z, Tangent facing +X
		const FVector3f Normal(0, 0, 1);
		const FVector3f Tangent(1, 0, 0);

		// Add 4 vertices for the quad using FLocalVFTestVertex format
		VertexBuffer.Vertices.Add(FLocalVFTestVertex(
			FVector3f(-HalfSize, -HalfSize, 0), Normal, Tangent, FVector2f(0, 0), FColor::Red));
		VertexBuffer.Vertices.Add(FLocalVFTestVertex(
			FVector3f(HalfSize, -HalfSize, 0), Normal, Tangent, FVector2f(1, 0), FColor::Green));
		VertexBuffer.Vertices.Add(FLocalVFTestVertex(
			FVector3f(HalfSize, HalfSize, 0), Normal, Tangent, FVector2f(1, 1), FColor::Blue));
		VertexBuffer.Vertices.Add(FLocalVFTestVertex(
			FVector3f(-HalfSize, HalfSize, 0), Normal, Tangent, FVector2f(0, 1), FColor::Yellow));

		// Two triangles (CCW winding when viewed from +Z)
		IndexBuffer.Indices = { 0, 1, 2, 0, 2, 3 };

		UE_LOG(LogVoxelRendering, Log, TEXT("FLocalVFTestSceneProxy: Created with %d vertices, %d indices"),
			VertexBuffer.GetNumVertices(), IndexBuffer.GetNumIndices());
	}

	virtual ~FLocalVFTestSceneProxy()
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
		// Initialize buffers first - this creates the SRVs
		VertexBuffer.InitResource(RHICmdList);
		IndexBuffer.InitResource(RHICmdList);

		// Initialize vertex factory with proper stream configuration
		// Uses FLocalVertexFactory directly (no custom type) to leverage Epic's proven shaders
		// Position/Tangent/TexCoord SRVs use global null buffer; ColorSRV uses our separate color buffer
		InitLocalVertexFactoryStreams(
			RHICmdList,
			&VertexFactory,                                   // Vertex factory to configure
			&VertexBuffer,                                    // Position buffer (interleaved)
			sizeof(FLocalVFTestVertex),                       // Stride
			STRUCT_OFFSET(FLocalVFTestVertex, Position),      // Position offset
			STRUCT_OFFSET(FLocalVFTestVertex, TangentX),      // TangentX offset
			STRUCT_OFFSET(FLocalVFTestVertex, TangentZ),      // TangentZ offset
			STRUCT_OFFSET(FLocalVFTestVertex, TexCoord),      // TexCoord offset
			STRUCT_OFFSET(FLocalVFTestVertex, Color),         // Color offset
			nullptr,                                          // Position SRV (use null buffer)
			nullptr,                                          // Tangents SRV (use null buffer)
			nullptr,                                          // TexCoord SRV (use null buffer)
			VertexBuffer.ColorSRV                             // Color SRV (from separate color buffer)
		);

		VertexFactory.InitResource(RHICmdList);

		UE_LOG(LogVoxelRendering, Log, TEXT("FLocalVFTestSceneProxy: Render resources created, VF initialized=%d"),
			VertexFactory.IsInitialized() ? 1 : 0);
	}

	virtual void GetDynamicMeshElements(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_LocalVFTestSceneProxy_GetDynamicMeshElements);

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
				Mesh.bDisableBackfaceCulling = true;  // Show both sides for debugging

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

	FLocalVFTestVertexBuffer VertexBuffer;
	FLocalVFTestIndexBuffer IndexBuffer;
	FLocalVertexFactory VertexFactory;
};

// ============================================================================
// Component Implementation
// ============================================================================

ULocalVFTestComponent::ULocalVFTestComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	bWantsOnUpdateTransform = false;
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	SetCastShadow(true);
}

FPrimitiveSceneProxy* ULocalVFTestComponent::CreateSceneProxy()
{
	if (QuadSize <= 0)
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("ULocalVFTestComponent: QuadSize <= 0, not creating scene proxy"));
		return nullptr;
	}

	UE_LOG(LogVoxelRendering, Log, TEXT("ULocalVFTestComponent: Creating scene proxy with Material=%s, QuadSize=%.1f"),
		Material ? *Material->GetName() : TEXT("nullptr (using default)"), QuadSize);

	return new FLocalVFTestSceneProxy(this);
}

FBoxSphereBounds ULocalVFTestComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	const float HalfSize = QuadSize * 0.5f;
	FBox LocalBox(
		FVector(-HalfSize, -HalfSize, -1.0f),
		FVector(HalfSize, HalfSize, 1.0f)
	);
	return FBoxSphereBounds(LocalBox).TransformBy(LocalToWorld);
}

UMaterialInterface* ULocalVFTestComponent::GetMaterial(int32 ElementIndex) const
{
	return Material;
}

void ULocalVFTestComponent::SetMaterial(int32 ElementIndex, UMaterialInterface* InMaterial)
{
	Material = InMaterial;
	MarkRenderStateDirty();
}

void ULocalVFTestComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
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

void ULocalVFTestComponent::RefreshMesh()
{
	MarkRenderStateDirty();
}
