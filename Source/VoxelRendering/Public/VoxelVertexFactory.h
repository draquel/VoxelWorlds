// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"
#include "VertexFactory.h"
#include "LocalVertexFactory.h"
#include "ShaderParameterMacros.h"
#include "RHIResources.h"
#include "VoxelVertex.h"

/**
 * Per-chunk data passed through BatchElement.UserData to GetElementShaderBindings.
 * This allows each chunk to have its own uniform buffer and vertex buffer.
 */
struct FVoxelMeshBatchUserData
{
	FRHIBuffer* VertexBuffer = nullptr;
	FRHIUniformBuffer* UniformBuffer = nullptr;
};

/**
 * Index buffer for voxel chunks.
 * Wrapper around FIndexBuffer that can use a pre-created RHI buffer.
 * Must call InitResource() after InitWithRHIBuffer() to mark as initialized.
 */
class VOXELRENDERING_API FVoxelIndexBuffer : public FIndexBuffer
{
public:
	FVoxelIndexBuffer() : NumIndices(0) {}

	/** Set up with a pre-created RHI buffer. Call InitResource() after this. */
	void InitWithRHIBuffer(FBufferRHIRef InIndexBufferRHI, uint32 InNumIndices)
	{
		PendingBuffer = InIndexBufferRHI;
		NumIndices = InNumIndices;
	}

	/** Override InitRHI to use our pre-created buffer */
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		IndexBufferRHI = PendingBuffer;
	}

	/** Override ReleaseRHI to clean up */
	virtual void ReleaseRHI() override
	{
		IndexBufferRHI.SafeRelease();
		PendingBuffer.SafeRelease();
		NumIndices = 0;
	}

	/** Release the RHI buffer reference */
	void ReleaseRHIBuffer()
	{
		ReleaseResource();
		PendingBuffer.SafeRelease();
		NumIndices = 0;
	}

	uint32 GetNumIndices() const { return NumIndices; }

private:
	FBufferRHIRef PendingBuffer;
	uint32 NumIndices;
};

/**
 * Per-chunk GPU buffer container.
 *
 * Holds all GPU resources needed to render a single voxel chunk.
 * Managed by FVoxelSceneProxy on the render thread.
 *
 * Thread Safety: Must only be accessed on render thread
 */
struct VOXELRENDERING_API FVoxelChunkGPUData
{
	/** Chunk coordinate */
	FIntVector ChunkCoord = FIntVector::ZeroValue;

	/** LOD level */
	int32 LODLevel = 0;

	/** Number of vertices */
	uint32 VertexCount = 0;

	/** Number of indices */
	uint32 IndexCount = 0;

	/** Local-space bounding box */
	FBox LocalBounds = FBox(ForceInit);

	/** World position of chunk origin */
	FVector ChunkWorldPosition = FVector::ZeroVector;

	/** LOD morph factor for smooth transitions */
	float MorphFactor = 0.0f;

	/** Visibility flag */
	bool bIsVisible = true;

	/** Vertex buffer RHI resource */
	FBufferRHIRef VertexBufferRHI;

	/** Index buffer RHI resource */
	FBufferRHIRef IndexBufferRHI;

	/** Index buffer wrapper for mesh batch rendering */
	TSharedPtr<FVoxelIndexBuffer> IndexBuffer;

	/** Shader resource view for vertex buffer */
	FShaderResourceViewRHIRef VertexBufferSRV;

	/** Check if GPU buffers are valid */
	FORCEINLINE bool HasValidBuffers() const
	{
		return VertexBufferRHI.IsValid() && IndexBufferRHI.IsValid() && VertexCount > 0 && IndexCount > 0;
	}

	/** Get approximate GPU memory usage in bytes */
	FORCEINLINE SIZE_T GetGPUMemoryUsage() const
	{
		return (VertexCount * sizeof(FVoxelVertex)) + (IndexCount * sizeof(uint32));
	}

	/** Initialize index buffer wrapper from RHI buffer. Must be called on render thread. */
	void InitIndexBufferWrapper(FRHICommandListBase& RHICmdList)
	{
		if (IndexBufferRHI.IsValid() && IndexCount > 0)
		{
			IndexBuffer = MakeShared<FVoxelIndexBuffer>();
			IndexBuffer->InitWithRHIBuffer(IndexBufferRHI, IndexCount);
			// InitResource() will call InitRHI() which assigns our pre-created buffer
			// and marks the resource as initialized
			IndexBuffer->InitResource(RHICmdList);
		}
	}

	/** Release GPU resources. Can be called from any thread. */
	void ReleaseResources()
	{
		if (IndexBuffer.IsValid())
		{
			// ReleaseRHIBuffer calls ReleaseResource internally
			IndexBuffer->ReleaseRHIBuffer();
			IndexBuffer.Reset();
		}
		VertexBufferRHI.SafeRelease();
		IndexBufferRHI.SafeRelease();
		VertexBufferSRV.SafeRelease();
		VertexCount = 0;
		IndexCount = 0;
	}
};

/**
 * Uniform shader parameters for voxel vertex factory.
 */
BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FVoxelVertexFactoryUniformShaderParameters, VOXELRENDERING_API)
	/** Local to world transform matrix */
	SHADER_PARAMETER(FMatrix44f, LocalToWorld)
	/** World to local transform matrix (inverse) */
	SHADER_PARAMETER(FMatrix44f, WorldToLocal)
	/** LOD morph factor (0-1) */
	SHADER_PARAMETER(float, LODMorphFactor)
	/** LOD grid size for morphing */
	SHADER_PARAMETER(float, LODGridSize)
	/** Voxel size in world units */
	SHADER_PARAMETER(float, VoxelSize)
	/** Padding */
	SHADER_PARAMETER(float, Padding)
END_GLOBAL_SHADER_PARAMETER_STRUCT()

typedef TUniformBufferRef<FVoxelVertexFactoryUniformShaderParameters> FVoxelVertexFactoryUniformBufferRef;

/**
 * Vertex factory declaration for voxel meshes.
 */
class VOXELRENDERING_API FVoxelVertexFactoryShaderParameters : public FVertexFactoryShaderParameters
{
	DECLARE_TYPE_LAYOUT(FVoxelVertexFactoryShaderParameters, NonVirtual);

public:
	void Bind(const FShaderParameterMap& ParameterMap);
	void GetElementShaderBindings(
		const class FSceneInterface* Scene,
		const class FSceneView* View,
		const class FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const class FVertexFactory* VertexFactory,
		const struct FMeshBatchElement& BatchElement,
		class FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams
	) const;

	LAYOUT_FIELD(FShaderUniformBufferParameter, VoxelUniformBuffer);
};

/**
 * Custom vertex factory for voxel mesh rendering.
 *
 * Provides efficient GPU rendering using the packed 28-byte FVoxelVertex format.
 * Supports LOD morphing for smooth transitions between detail levels.
 *
 * Thread Safety: Must be initialized/released on render thread
 *
 * @see FVoxelVertex
 * @see Documentation/RENDERING_SYSTEM.md
 */
class VOXELRENDERING_API FVoxelVertexFactory : public FVertexFactory
{
	DECLARE_VERTEX_FACTORY_TYPE(FVoxelVertexFactory);

public:
	FVoxelVertexFactory(ERHIFeatureLevel::Type InFeatureLevel);
	virtual ~FVoxelVertexFactory();

	/**
	 * Initialize the vertex factory with a vertex buffer.
	 * Must be called on render thread.
	 */
	void SetupVertexBuffer(FRHIBuffer* VertexBuffer, uint32 InVertexCount);

	/**
	 * Set uniform buffer parameters.
	 */
	void SetUniformBuffer(const FVoxelVertexFactoryUniformBufferRef& InUniformBuffer);

	/**
	 * Get the uniform buffer.
	 */
	FRHIUniformBuffer* GetUniformBuffer() const { return UniformBuffer.GetReference(); }

	// FRenderResource interface
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual void ReleaseRHI() override;

	// FVertexFactory interface
	static bool ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);

	/** Get vertex declaration for the voxel vertex format */
	static FVertexDeclarationRHIRef CreateVertexDeclaration(FRHICommandListBase& RHICmdList);

	/** Data passed to shaders */
	struct FDataType
	{
		/** Position stream */
		FVertexStreamComponent PositionComponent;

		/** Packed normal and AO stream */
		FVertexStreamComponent PackedNormalAndAOComponent;

		/** UV stream */
		FVertexStreamComponent UVComponent;

		/** Packed material data stream */
		FVertexStreamComponent PackedMaterialDataComponent;
	};

	/** Set vertex data */
	void SetData(FRHICommandListBase& RHICmdList, const FDataType& InData);

	/** Get current data */
	const FDataType& GetData() const { return Data; }

private:
	/** Cached vertex buffer reference */
	FBufferRHIRef CachedVertexBuffer;

	/** Number of vertices */
	uint32 VertexCount = 0;

	/** Uniform buffer reference */
	FVoxelVertexFactoryUniformBufferRef UniformBuffer;

	/** Vertex declaration */
	FVertexDeclarationRHIRef VertexDeclaration;

	FDataType Data;
};
