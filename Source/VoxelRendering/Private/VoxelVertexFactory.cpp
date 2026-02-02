// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelVertexFactory.h"
#include "VoxelRendering.h"
#include "MeshMaterialShader.h"
#include "ShaderParameterUtils.h"
#include "RHIStaticStates.h"
#include "PipelineStateCache.h"
#include "MeshDrawShaderBindings.h"

// Implement the uniform buffer struct metadata.
// This is REQUIRED for TUniformBufferRef::CreateUniformBufferImmediate() to work,
// as it generates the GetStructMetadata() function that the template needs.
// The shader-side cbuffer is manually declared in VoxelVertexFactory.ush as "cbuffer VoxelVF"
// and bound per-element in GetElementShaderBindings via FShaderUniformBufferParameter.
IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FVoxelVertexFactoryUniformShaderParameters, "VoxelVF");

// ==================== FVoxelVertexFactoryShaderParameters ====================

IMPLEMENT_TYPE_LAYOUT(FVoxelVertexFactoryShaderParameters);

void FVoxelVertexFactoryShaderParameters::Bind(const FShaderParameterMap& ParameterMap)
{
	VoxelUniformBuffer.Bind(ParameterMap, TEXT("VoxelVF"));
	UE_LOG(LogVoxelRendering, Log, TEXT("FVoxelVertexFactoryShaderParameters::Bind - VoxelUniformBuffer bound: %s"),
		VoxelUniformBuffer.IsBound() ? TEXT("YES") : TEXT("NO"));
}

void FVoxelVertexFactoryShaderParameters::GetElementShaderBindings(
	const FSceneInterface* Scene,
	const FSceneView* View,
	const FMeshMaterialShader* Shader,
	const EVertexInputStreamType InputStreamType,
	ERHIFeatureLevel::Type FeatureLevel,
	const FVertexFactory* VertexFactory,
	const FMeshBatchElement& BatchElement,
	FMeshDrawSingleShaderBindings& ShaderBindings,
	FVertexInputStreamArray& VertexStreams) const
{
	// Get per-chunk data from UserData
	const FVoxelMeshBatchUserData* UserData = static_cast<const FVoxelMeshBatchUserData*>(BatchElement.UserData);
	if (!UserData)
	{
		return;
	}

	// Bind the per-chunk uniform buffer
	if (VoxelUniformBuffer.IsBound() && UserData->UniformBuffer)
	{
		ShaderBindings.Add(VoxelUniformBuffer, UserData->UniformBuffer);
	}

	// Add vertex buffer stream
	if (UserData->VertexBuffer)
	{
		// Add vertex stream at slot 0
		VertexStreams.Add(FVertexInputStream(0, 0, UserData->VertexBuffer));
	}
}

// ==================== FVoxelVertexFactory ====================

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FVoxelVertexFactory, SF_Vertex, FVoxelVertexFactoryShaderParameters);

IMPLEMENT_VERTEX_FACTORY_TYPE(FVoxelVertexFactory, "/Plugin/VoxelWorlds/Private/VoxelVertexFactory.ush",
	EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsDynamicLighting
	| EVertexFactoryFlags::SupportsPrimitiveIdStream
	| EVertexFactoryFlags::SupportsPositionOnly
);

FVoxelVertexFactory::FVoxelVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
	: FVertexFactory(InFeatureLevel)
	, VertexCount(0)
{
}

FVoxelVertexFactory::~FVoxelVertexFactory()
{
}

void FVoxelVertexFactory::SetupVertexBuffer(FRHIBuffer* VertexBuffer, uint32 InVertexCount)
{
	check(IsInRenderingThread());

	CachedVertexBuffer = VertexBuffer;
	VertexCount = InVertexCount;

	// For dynamic meshes with raw RHI buffers, we don't set up FVertexStreamComponent
	// The vertex declaration defines the layout, and streams are bound at draw time
}

void FVoxelVertexFactory::SetUniformBuffer(const FVoxelVertexFactoryUniformBufferRef& InUniformBuffer)
{
	UniformBuffer = InUniformBuffer;
}

void FVoxelVertexFactory::InitRHI(FRHICommandListBase& RHICmdList)
{
	// Create vertex declaration for the 28-byte FVoxelVertex layout
	FVertexDeclarationElementList Elements;

	// Position - ATTRIBUTE0 (offset 0, 12 bytes)
	Elements.Add(FVertexElement(0, 0, VET_Float3, 0, sizeof(FVoxelVertex), false));

	// PackedNormalAndAO - ATTRIBUTE1 (offset 12, 4 bytes)
	Elements.Add(FVertexElement(0, 12, VET_UInt, 1, sizeof(FVoxelVertex), false));

	// UV - ATTRIBUTE2 (offset 16, 8 bytes)
	Elements.Add(FVertexElement(0, 16, VET_Float2, 2, sizeof(FVoxelVertex), false));

	// PackedMaterialData - ATTRIBUTE3 (offset 24, 4 bytes)
	Elements.Add(FVertexElement(0, 24, VET_UInt, 3, sizeof(FVoxelVertex), false));

	// Initialize the base class declaration - this is critical for mesh draw commands
	InitDeclaration(Elements);

	FVertexFactory::InitRHI(RHICmdList);
}

void FVoxelVertexFactory::ReleaseRHI()
{
	CachedVertexBuffer.SafeRelease();
	UniformBuffer.SafeRelease();

	// Base class handles Declaration cleanup
	FVertexFactory::ReleaseRHI();
}

bool FVoxelVertexFactory::ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters)
{
	// IMPORTANT: Skip default and special engine materials
	// These require additional VF functions (VertexFactoryGetViewIndex, VertexFactoryGetInstanceIdLoadIndex, etc.)
	// that we haven't implemented. Users must create a custom material for voxel rendering.
	if (Parameters.MaterialParameters.bIsDefaultMaterial || Parameters.MaterialParameters.bIsSpecialEngineMaterial)
	{
		return false;
	}

	// Skip volumetric cloud materials
	if (Parameters.MaterialParameters.bIsUsedWithVolumetricCloud)
	{
		return false;
	}

	// Only compile for surface materials
	if (Parameters.MaterialParameters.MaterialDomain != MD_Surface)
	{
		return false;
	}

	return true;
}

void FVoxelVertexFactory::ModifyCompilationEnvironment(
	const FVertexFactoryShaderPermutationParameters& Parameters,
	FShaderCompilerEnvironment& OutEnvironment)
{
	FVertexFactory::ModifyCompilationEnvironment(Parameters, OutEnvironment);

	// Define VOXEL_VERTEX_FACTORY to enable voxel-specific shader paths
	OutEnvironment.SetDefine(TEXT("VOXEL_VERTEX_FACTORY"), 1);
	OutEnvironment.SetDefine(TEXT("MANUAL_VERTEX_FETCH"), 0);

	// Disable features we don't support yet
	OutEnvironment.SetDefine(TEXT("INSTANCED_STEREO"), 0);
	OutEnvironment.SetDefine(TEXT("MULTI_VIEW"), 0);
}

FVertexDeclarationRHIRef FVoxelVertexFactory::CreateVertexDeclaration(FRHICommandListBase& RHICmdList)
{
	FVertexDeclarationElementList Elements;

	// Position - ATTRIBUTE0
	Elements.Add(FVertexElement(0, 0, VET_Float3, 0, sizeof(FVoxelVertex), false));

	// PackedNormalAndAO - ATTRIBUTE1
	Elements.Add(FVertexElement(0, 12, VET_UInt, 1, sizeof(FVoxelVertex), false));

	// UV - ATTRIBUTE2
	Elements.Add(FVertexElement(0, 16, VET_Float2, 2, sizeof(FVoxelVertex), false));

	// PackedMaterialData - ATTRIBUTE3
	Elements.Add(FVertexElement(0, 24, VET_UInt, 3, sizeof(FVoxelVertex), false));

	return PipelineStateCache::GetOrCreateVertexDeclaration(Elements);
}

void FVoxelVertexFactory::SetData(FRHICommandListBase& RHICmdList, const FDataType& InData)
{
	Data = InData;

	// Update the vertex declaration if needed
	if (!VertexDeclaration.IsValid())
	{
		VertexDeclaration = CreateVertexDeclaration(RHICmdList);
	}
}
