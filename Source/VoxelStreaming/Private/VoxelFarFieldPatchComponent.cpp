// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelFarFieldPatchComponent.h"
#include "VoxelFarField.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveViewRelevance.h"
#include "PrimitiveDrawingUtils.h"
#include "PrimitiveUniformShaderParametersBuilder.h"
#include "SceneInterface.h"
#include "SceneView.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "Engine/Engine.h"
#include "StaticMeshResources.h"
#include "RenderUtils.h"
#include "HAL/IConsoleManager.h"
#include "RayTracingInstance.h"

namespace VoxelFarFieldPatchPrivate
{
	/** Texture coordinates a vertex carries: UV0, UV1 and two zero channels, as a procedural mesh section's (the frame stays the same) */
	constexpr uint32 NumTexCoords = 4;

	/** @return Whether Lumen traces the engine's far-field layer (r.LumenScene.FarField; its change recreates every proxy) */
	bool IsLumenFarFieldOn()
	{
		static const TConsoleVariableData<int32>* LumenFarField = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.LumenScene.FarField"));
		return LumenFarField && LumenFarField->GetValueOnAnyThread() != 0;
	}

	/** One section on the render thread: its buffers, vertex factory and material, and its ray tracing geometry when traced */
	struct FProxySection
	{
		UMaterialInterface* Material = nullptr;
		FStaticMeshVertexBuffers VertexBuffers;
		FDynamicMeshIndexBuffer32 IndexBuffer;
		FLocalVertexFactory VertexFactory;
		bool bVisible = true;
#if RHI_RAYTRACING
		FRayTracingGeometry RayTracingGeometry;
		bool bRayTracing = false;
#endif

		explicit FProxySection(ERHIFeatureLevel::Type FeatureLevel)
			: VertexFactory(FeatureLevel, "FVoxelFarFieldPatchSection")
		{
		}
	};
}

/**
 * The far-field patch's scene proxy: the procedural mesh proxy's drawing (dynamic mesh elements, one batch a visible
 * section), with ray tracing geometry only for the sections the component marked, and only when it can be traced.
 */
class FVoxelFarFieldPatchSceneProxy final : public FPrimitiveSceneProxy
{
public:
	SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	FVoxelFarFieldPatchSceneProxy(UVoxelFarFieldPatchComponent* Component, bool bCanBeRayTraced)
		: FPrimitiveSceneProxy(Component)
		, MaterialRelevance(Component->GetMaterialRelevance(GetScene().GetShaderPlatform()))
	{
		using namespace VoxelFarFieldPatchPrivate;
		const int32 NumSections = Component->GetNumSections();
		Sections.AddZeroed(NumSections);
		for (int32 SectionIndex = 0; SectionIndex < NumSections; ++SectionIndex)
		{
			const FVoxelFarFieldPatchSection& Source = *Component->GetSection(SectionIndex);
			if (Source.IsEmpty())
			{
				continue;
			}
			FProxySection* Section = new FProxySection(GetScene().GetFeatureLevel());
			Section->IndexBuffer.Indices = Source.Indices;
			TArray<FDynamicMeshVertex> Vertices = Source.Vertices;
			Section->VertexBuffers.InitFromDynamicVertex(&Section->VertexFactory, Vertices, NumTexCoords);
			BeginInitResource(&Section->VertexBuffers.PositionVertexBuffer);
			BeginInitResource(&Section->VertexBuffers.StaticMeshVertexBuffer);
			BeginInitResource(&Section->VertexBuffers.ColorVertexBuffer);
			BeginInitResource(&Section->IndexBuffer);
			BeginInitResource(&Section->VertexFactory);
			Section->Material = Component->GetMaterial(SectionIndex);
			if (!Section->Material)
			{
				Section->Material = UMaterial::GetDefaultMaterial(MD_Surface);
			}
			Section->bVisible = Source.bVisible;
			Sections[SectionIndex] = Section;

#if RHI_RAYTRACING
			// G121: geometry for the marked sections of a component something can trace, none for the rest.
			if (bCanBeRayTraced && Source.bRayTracing)
			{
				Section->bRayTracing = true;
				++NumRayTracingSections;
				ENQUEUE_RENDER_COMMAND(InitVoxelFarFieldPatchRayTracingGeometry)(
					[DebugName = Component->GetFName(), Section](FRHICommandListImmediate& RHICmdList)
				{
					FRayTracingGeometryInitializer Initializer;
					Initializer.DebugName = DebugName;
					Initializer.IndexBuffer = Section->IndexBuffer.IndexBufferRHI;
					Initializer.TotalPrimitiveCount = Section->IndexBuffer.Indices.Num() / 3;
					Initializer.GeometryType = RTGT_Triangles;
					Initializer.bFastBuild = true;
					Initializer.bAllowUpdate = false;

					FRayTracingGeometrySegment Segment;
					Segment.VertexBuffer = Section->VertexBuffers.PositionVertexBuffer.VertexBufferRHI;
					Segment.MaxVertices = Section->VertexBuffers.PositionVertexBuffer.GetNumVertices();
					Segment.NumPrimitives = Initializer.TotalPrimitiveCount;
					Initializer.Segments.Add(Segment);

					Section->RayTracingGeometry.SetInitializer(Initializer);
					Section->RayTracingGeometry.InitResource(RHICmdList);
				});
			}
#endif
		}
	}

	virtual ~FVoxelFarFieldPatchSceneProxy()
	{
		for (VoxelFarFieldPatchPrivate::FProxySection* Section : Sections)
		{
			if (!Section)
			{
				continue;
			}
			Section->VertexBuffers.PositionVertexBuffer.ReleaseResource();
			Section->VertexBuffers.StaticMeshVertexBuffer.ReleaseResource();
			Section->VertexBuffers.ColorVertexBuffer.ReleaseResource();
			Section->IndexBuffer.ReleaseResource();
			Section->VertexFactory.ReleaseResource();
#if RHI_RAYTRACING
			if (Section->bRayTracing)
			{
				Section->RayTracingGeometry.ReleaseResource();
			}
#endif
			delete Section;
		}
	}

	/** Show or hide a section (render thread). */
	void SetSectionVisible_RenderThread(int32 SectionIndex, bool bVisible)
	{
		check(IsInRenderingThread());
		if (Sections.IsValidIndex(SectionIndex) && Sections[SectionIndex])
		{
			Sections[SectionIndex]->bVisible = bVisible;
		}
	}

	/** @return The sections whose ray tracing geometry is valid (render thread state: flush rendering commands first) */
	int32 CountValidRayTracingGeometries() const
	{
		int32 Count = 0;
#if RHI_RAYTRACING
		for (const VoxelFarFieldPatchPrivate::FProxySection* Section : Sections)
		{
			Count += (Section && Section->bRayTracing && Section->RayTracingGeometry.IsValid()) ? 1 : 0;
		}
#endif
		return Count;
	}

	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override
	{
		const bool bWireframe = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;
		FColoredMaterialRenderProxy* WireframeMaterial = nullptr;
		if (bWireframe)
		{
			WireframeMaterial = new FColoredMaterialRenderProxy(GEngine->WireframeMaterial ? GEngine->WireframeMaterial->GetRenderProxy() : nullptr,
				FLinearColor(0, 0.5f, 1.f));
			Collector.RegisterOneFrameMaterialProxy(WireframeMaterial);
		}

		for (const VoxelFarFieldPatchPrivate::FProxySection* Section : Sections)
		{
			if (!Section || !Section->bVisible)
			{
				continue;
			}
			FMaterialRenderProxy* MaterialProxy = bWireframe ? WireframeMaterial : Section->Material->GetRenderProxy();
			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
			{
				if ((VisibilityMap & (1 << ViewIndex)) == 0)
				{
					continue;
				}
				FMeshBatch& Mesh = Collector.AllocateMesh();
				Mesh.bWireframe = bWireframe;
				Mesh.MaterialRenderProxy = MaterialProxy;
				FillBatch(Mesh, *Section, Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>(), Collector.GetRHICommandList());
				Collector.AddMesh(ViewIndex, Mesh);
			}
		}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			if (VisibilityMap & (1 << ViewIndex))
			{
				RenderBounds(Collector.GetPDI(ViewIndex), ViewFamily.EngineShowFlags, GetBounds(), IsSelected());
			}
		}
#endif
	}

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		Result.bDrawRelevance = IsShown(View);
		Result.bShadowRelevance = IsShadowCast(View);
		Result.bDynamicRelevance = true;
		Result.bRenderInMainPass = ShouldRenderInMainPass();
		Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
		Result.bRenderCustomDepth = ShouldRenderCustomDepth();
		Result.bTranslucentSelfShadow = bCastVolumetricTranslucentShadow;
		MaterialRelevance.SetPrimitiveViewRelevance(Result);
		Result.bVelocityRelevance = DrawsVelocity() && Result.bOpaque && Result.bRenderInMainPass;
		return Result;
	}

	virtual bool CanBeOccluded() const override
	{
		return !MaterialRelevance.bDisableDepthTest;
	}

	virtual uint32 GetMemoryFootprint() const override
	{
		return sizeof(*this) + GetAllocatedSize();
	}

#if RHI_RAYTRACING
	// Without traced sections the scene leaves the proxy out of ray tracing altogether.
	virtual bool IsRayTracingRelevant() const override { return NumRayTracingSections > 0; }

	virtual bool HasRayTracingRepresentation() const override { return NumRayTracingSections > 0; }

	virtual void GetDynamicRayTracingInstances(FRayTracingInstanceCollector& Collector) override final
	{
		TConstArrayView<const FSceneView*> Views = Collector.GetViews();
		const uint32 VisibilityMap = Collector.GetVisibilityMap();
		const int32 FirstActiveViewIndex = FMath::CountTrailingZeros(VisibilityMap);
		if (!Views.IsValidIndex(FirstActiveViewIndex))
		{
			return;
		}
		const FSceneView* FirstActiveView = Views[FirstActiveViewIndex];

		for (const VoxelFarFieldPatchPrivate::FProxySection* Section : Sections)
		{
			if (!Section || !Section->bVisible || !Section->bRayTracing || !Section->RayTracingGeometry.IsValid())
			{
				continue;
			}
			FRayTracingInstance Instance;
			Instance.Geometry = &Section->RayTracingGeometry;
			Instance.InstanceTransforms.Add(GetLocalToWorld());

			FMeshBatch MeshBatch;
			MeshBatch.SegmentIndex = 0;
			MeshBatch.MaterialRenderProxy = Section->Material->GetRenderProxy();
			MeshBatch.CastRayTracedShadow = IsShadowCast(FirstActiveView);
			FillBatch(MeshBatch, *Section, Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>(), Collector.GetRHICommandList());
			Instance.Materials.Add(MeshBatch);

			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
			{
				if (VisibilityMap & (1 << ViewIndex))
				{
					Collector.AddRayTracingInstance(ViewIndex, Instance);
				}
			}
		}
	}
#endif

private:
	/** Fill a batch's vertex factory, index range and primitive uniform buffer for a section (the drawn and the traced alike). */
	void FillBatch(FMeshBatch& Mesh, const VoxelFarFieldPatchPrivate::FProxySection& Section, FDynamicPrimitiveUniformBuffer& PrimitiveUniformBuffer,
		FRHICommandListBase& RHICmdList) const
	{
		Mesh.VertexFactory = &Section.VertexFactory;
		Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
		Mesh.Type = PT_TriangleList;
		Mesh.DepthPriorityGroup = SDPG_World;
		Mesh.bCanApplyViewModeOverrides = false;

		FPrimitiveUniformShaderParametersBuilder Builder;
		BuildUniformShaderParameters(Builder);
		PrimitiveUniformBuffer.Set(RHICmdList, Builder);

		FMeshBatchElement& Element = Mesh.Elements[0];
		Element.IndexBuffer = &Section.IndexBuffer;
		Element.PrimitiveUniformBufferResource = &PrimitiveUniformBuffer.UniformBuffer;
		Element.FirstIndex = 0;
		Element.NumPrimitives = Section.IndexBuffer.Indices.Num() / 3;
		Element.MinVertexIndex = 0;
		Element.MaxVertexIndex = Section.VertexBuffers.PositionVertexBuffer.GetNumVertices() - 1;
	}

	TArray<VoxelFarFieldPatchPrivate::FProxySection*> Sections;
	FMaterialRelevance MaterialRelevance;
	/** The sections given ray tracing geometry */
	int32 NumRayTracingSections = 0;
};

// ==================== UVoxelFarFieldPatchComponent ====================

UVoxelFarFieldPatchComponent::UVoxelFarFieldPatchComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UVoxelFarFieldPatchComponent::SetSection(int32 SectionIndex, const FVoxelFarFieldSection& Mesh, bool bReverse, bool bRayTracing)
{
	if (SectionIndex < 0)
	{
		return;
	}
	if (SectionIndex >= Sections.Num())
	{
		Sections.SetNum(SectionIndex + 1);
	}
	// A filled section shows, as a procedural mesh section does once created.
	FVoxelFarFieldPatchSection& Section = Sections[SectionIndex];
	Section = FVoxelFarFieldPatchSection();
	Section.bRayTracing = bRayTracing;
	if (!Mesh.IsEmpty())
	{
		// As a procedural mesh section: missing normals face up, missing colours are white, missing tangents run along +X.
		const int32 N = Mesh.Positions.Num();
		const bool bNormals = Mesh.Normals.Num() == N;
		const bool bTangents = Mesh.Tangents.Num() == N;
		const bool bUV0 = Mesh.UV0.Num() == N;
		const bool bUV1 = Mesh.UV1.Num() == N;
		const bool bColors = Mesh.Colors.Num() == N;
		Section.Vertices.SetNumUninitialized(N);
		for (int32 i = 0; i < N; ++i)
		{
			FDynamicMeshVertex& V = Section.Vertices[i];
			V.Position = Mesh.Positions[i];
			V.TangentX = bTangents ? Mesh.Tangents[i] : FVector3f(1.0f, 0.0f, 0.0f);
			V.TangentZ = bNormals ? Mesh.Normals[i] : FVector3f(0.0f, 0.0f, 1.0f);
			V.TangentZ.Vector.W = 127;
			V.TextureCoordinate[0] = bUV0 ? Mesh.UV0[i] : FVector2f::ZeroVector;
			V.TextureCoordinate[1] = bUV1 ? Mesh.UV1[i] : FVector2f::ZeroVector;
			V.TextureCoordinate[2] = FVector2f::ZeroVector;
			V.TextureCoordinate[3] = FVector2f::ZeroVector;
			V.Color = bColors ? Mesh.Colors[i] : FColor(255, 255, 255);
			Section.LocalBox += FVector(Mesh.Positions[i]);
		}
		const int32 NumIndices = (Mesh.Triangles.Num() / 3) * 3;
		Section.Indices.SetNumUninitialized(NumIndices);
		for (int32 i = 0; i < NumIndices; i += 3)
		{
			const uint32 A = static_cast<uint32>(FMath::Clamp(Mesh.Triangles[i], 0, N - 1));
			const uint32 B = static_cast<uint32>(FMath::Clamp(Mesh.Triangles[i + 1], 0, N - 1));
			const uint32 C = static_cast<uint32>(FMath::Clamp(Mesh.Triangles[i + 2], 0, N - 1));
			Section.Indices[i] = A;
			Section.Indices[i + 1] = bReverse ? C : B;
			Section.Indices[i + 2] = bReverse ? B : C;
		}
	}
	UpdateLocalBounds();
	MarkRenderStateDirty();
}

void UVoxelFarFieldPatchComponent::ClearSection(int32 SectionIndex)
{
	if (Sections.IsValidIndex(SectionIndex))
	{
		Sections[SectionIndex] = FVoxelFarFieldPatchSection();
		UpdateLocalBounds();
		MarkRenderStateDirty();
	}
}

void UVoxelFarFieldPatchComponent::ClearAllSections()
{
	Sections.Empty();
	UpdateLocalBounds();
	MarkRenderStateDirty();
}

void UVoxelFarFieldPatchComponent::SetSectionVisible(int32 SectionIndex, bool bNewVisible)
{
	if (!Sections.IsValidIndex(SectionIndex))
	{
		return;
	}
	Sections[SectionIndex].bVisible = bNewVisible;
	// The component's own proxy type: a pending recreation copies the flag from the section instead.
	if (FVoxelFarFieldPatchSceneProxy* Proxy = static_cast<FVoxelFarFieldPatchSceneProxy*>(SceneProxy))
	{
		ENQUEUE_RENDER_COMMAND(VoxelFarFieldPatchSectionVisibility)(
			[Proxy, SectionIndex, bNewVisible](FRHICommandListImmediate&)
		{
			Proxy->SetSectionVisible_RenderThread(SectionIndex, bNewVisible);
		});
	}
}

bool UVoxelFarFieldPatchComponent::IsSectionVisible(int32 SectionIndex) const
{
	return Sections.IsValidIndex(SectionIndex) && Sections[SectionIndex].bVisible;
}

const FVoxelFarFieldPatchSection* UVoxelFarFieldPatchComponent::GetSection(int32 SectionIndex) const
{
	return Sections.IsValidIndex(SectionIndex) ? &Sections[SectionIndex] : nullptr;
}

bool UVoxelFarFieldPatchComponent::IsInTracedLayer() const
{
	return bVisibleInRayTracing && (!bRayTracingFarField || VoxelFarFieldPatchPrivate::IsLumenFarFieldOn());
}

bool UVoxelFarFieldPatchComponent::CanBeRayTraced() const
{
#if RHI_RAYTRACING
	return IsRayTracingEnabled() && IsInTracedLayer();
#else
	return false;
#endif
}

int32 UVoxelFarFieldPatchComponent::GetRayTracingSectionCount() const
{
	if (!IsInTracedLayer())
	{
		return 0;
	}
	int32 Count = 0;
	for (const FVoxelFarFieldPatchSection& Section : Sections)
	{
		Count += (Section.bRayTracing && !Section.IsEmpty()) ? 1 : 0;
	}
	return Count;
}

int32 UVoxelFarFieldPatchComponent::GetProxyRayTracingGeometryCount() const
{
	const FVoxelFarFieldPatchSceneProxy* Proxy = static_cast<const FVoxelFarFieldPatchSceneProxy*>(SceneProxy);
	return Proxy ? Proxy->CountValidRayTracingGeometries() : 0;
}

FPrimitiveSceneProxy* UVoxelFarFieldPatchComponent::CreateSceneProxy()
{
	for (const FVoxelFarFieldPatchSection& Section : Sections)
	{
		if (!Section.IsEmpty())
		{
			return new FVoxelFarFieldPatchSceneProxy(this, CanBeRayTraced());
		}
	}
	return nullptr;
}

FBoxSphereBounds UVoxelFarFieldPatchComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	FBoxSphereBounds WorldBounds(LocalBounds.TransformBy(LocalToWorld));
	WorldBounds.BoxExtent *= BoundsScale;
	WorldBounds.SphereRadius *= BoundsScale;
	return WorldBounds;
}

int32 UVoxelFarFieldPatchComponent::GetNumMaterials() const
{
	return Sections.Num();
}

void UVoxelFarFieldPatchComponent::UpdateLocalBounds()
{
	FBox Box(ForceInit);
	for (const FVoxelFarFieldPatchSection& Section : Sections)
	{
		Box += Section.LocalBox;
	}
	LocalBounds = Box.IsValid ? FBoxSphereBounds(Box) : FBoxSphereBounds(FVector::ZeroVector, FVector::ZeroVector, 0.0);
	UpdateBounds();
	MarkRenderTransformDirty();
}
