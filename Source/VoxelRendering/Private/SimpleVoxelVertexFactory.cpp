// Copyright Daniel Raquel. All Rights Reserved.

#include "SimpleVoxelVertexFactory.h"
#include "VoxelRendering.h"
#include "MeshMaterialShader.h"
#include "ShaderParameterUtils.h"

// ============================================================================
// Vertex Factory Type Implementation
// ============================================================================

/**
 * Implement the vertex factory type with custom shader.
 */
IMPLEMENT_VERTEX_FACTORY_TYPE(
	FSimpleVoxelVertexFactory,
	"/Plugin/VoxelWorlds/Private/SimpleVoxelVertexFactory.ush",
	EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsDynamicLighting
);

bool FSimpleVoxelVertexFactory::ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters)
{
	// DISABLED: This vertex factory has shader errors with UE5 Large World Coordinates.
	// We're testing the FLocalVertexFactory-based approach instead (FLocalVoxelVertexFactory).
	// TODO: Remove this vertex factory once FLocalVoxelVertexFactory is verified working.
	return false;
}

void FSimpleVoxelVertexFactory::ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	// Add any custom defines here if needed
	OutEnvironment.SetDefine(TEXT("SIMPLE_VOXEL_VERTEX_FACTORY"), 1);
}

// ============================================================================
// Vertex Factory Implementation
// ============================================================================

void FSimpleVoxelVertexFactory::Init(FRHICommandListBase& RHICmdList, const FVertexBuffer* VertexBuffer)
{
	VertexBufferPtr = VertexBuffer;

	// Define the vertex declaration
	FVertexDeclarationElementList Elements;

	// Position - float3 at offset 0
	Elements.Add(FVertexElement(
		0,  // Stream index
		STRUCT_OFFSET(FSimpleVoxelVertex, Position),
		VET_Float3,
		0,  // Attribute index (ATTRIBUTE0)
		sizeof(FSimpleVoxelVertex),
		false  // Not instanced
	));

	// Normal - float3 at offset 12
	Elements.Add(FVertexElement(
		0,
		STRUCT_OFFSET(FSimpleVoxelVertex, Normal),
		VET_Float3,
		1,  // ATTRIBUTE1
		sizeof(FSimpleVoxelVertex),
		false
	));

	// TexCoord - float2 at offset 24
	Elements.Add(FVertexElement(
		0,
		STRUCT_OFFSET(FSimpleVoxelVertex, TexCoord),
		VET_Float2,
		2,  // ATTRIBUTE2
		sizeof(FSimpleVoxelVertex),
		false
	));

	// Color - RGBA8 at offset 32
	Elements.Add(FVertexElement(
		0,
		STRUCT_OFFSET(FSimpleVoxelVertex, Color),
		VET_Color,
		3,  // ATTRIBUTE3
		sizeof(FSimpleVoxelVertex),
		false
	));

	InitDeclaration(Elements);
}

void FSimpleVoxelVertexFactory::InitRHI(FRHICommandListBase& RHICmdList)
{
	// Set up vertex streams
	if (VertexBufferPtr && VertexBufferPtr->VertexBufferRHI.IsValid())
	{
		FVertexStream VertexStream;
		VertexStream.VertexBuffer = VertexBufferPtr;
		VertexStream.Stride = sizeof(FSimpleVoxelVertex);
		VertexStream.Offset = 0;

		Streams.Empty();
		Streams.Add(VertexStream);
	}

	UE_LOG(LogVoxelRendering, Log, TEXT("FSimpleVoxelVertexFactory::InitRHI - Streams: %d"), Streams.Num());
}

void FSimpleVoxelVertexFactory::ReleaseRHI()
{
	Streams.Empty();
	FVertexFactory::ReleaseRHI();
}

// ============================================================================
// Vertex Buffer Implementation
// ============================================================================

void FSimpleVoxelVertexBuffer::InitRHI(FRHICommandListBase& RHICmdList)
{
	if (Vertices.Num() == 0)
	{
		return;
	}

	const uint32 NumVertices = Vertices.Num();
	const uint32 SizeInBytes = NumVertices * sizeof(FSimpleVoxelVertex);

	FRHIResourceCreateInfo CreateInfo(TEXT("SimpleVoxelVertexBuffer"));

	VertexBufferRHI = RHICmdList.CreateBuffer(
		SizeInBytes,
		BUF_Static | BUF_VertexBuffer,
		sizeof(FSimpleVoxelVertex),
		ERHIAccess::VertexOrIndexBuffer,
		CreateInfo
	);

	void* Data = RHICmdList.LockBuffer(VertexBufferRHI, 0, SizeInBytes, RLM_WriteOnly);
	FMemory::Memcpy(Data, Vertices.GetData(), SizeInBytes);
	RHICmdList.UnlockBuffer(VertexBufferRHI);

	UE_LOG(LogVoxelRendering, Log, TEXT("FSimpleVoxelVertexBuffer: Created with %d vertices (%d bytes)"),
		NumVertices, SizeInBytes);
}

// ============================================================================
// Index Buffer Implementation
// ============================================================================

void FSimpleVoxelIndexBuffer::InitRHI(FRHICommandListBase& RHICmdList)
{
	if (Indices.Num() == 0)
	{
		return;
	}

	const uint32 NumIndices = Indices.Num();
	const uint32 SizeInBytes = NumIndices * sizeof(uint32);

	FRHIResourceCreateInfo CreateInfo(TEXT("SimpleVoxelIndexBuffer"));

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

	UE_LOG(LogVoxelRendering, Log, TEXT("FSimpleVoxelIndexBuffer: Created with %d indices (%d bytes)"),
		NumIndices, SizeInBytes);
}
