// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VertexFactory.h"
#include "RenderResource.h"

/**
 * Simple Voxel Vertex Factory - Phase 3 (Revised)
 *
 * A minimal custom vertex factory that inherits directly from FVertexFactory
 * (not FLocalVertexFactory) with its own custom shader.
 *
 * This avoids the complex SRV requirements of FLocalVertexFactory in UE5
 * and is closer to what the production voxel vertex factory will be.
 *
 * Vertex Format (simple, for testing):
 * - Position: float3 (12 bytes) - ATTRIBUTE0
 * - Normal: float3 (12 bytes) - ATTRIBUTE1
 * - TexCoord: float2 (8 bytes) - ATTRIBUTE2
 * - Color: FColor (4 bytes) - ATTRIBUTE3
 * Total: 36 bytes per vertex
 */

// Simple vertex structure for the test
struct FSimpleVoxelVertex
{
	FVector3f Position;
	FVector3f Normal;
	FVector2f TexCoord;
	FColor Color;

	FSimpleVoxelVertex() = default;

	FSimpleVoxelVertex(const FVector3f& InPos, const FVector3f& InNormal, const FVector2f& InUV, const FColor& InColor)
		: Position(InPos), Normal(InNormal), TexCoord(InUV), Color(InColor)
	{
	}
};

/**
 * Minimal custom vertex factory inheriting from FVertexFactory.
 * Uses custom shader at /Plugin/VoxelWorlds/Private/SimpleVoxelVertexFactory.ush
 */
class VOXELRENDERING_API FSimpleVoxelVertexFactory : public FVertexFactory
{
	DECLARE_VERTEX_FACTORY_TYPE(FSimpleVoxelVertexFactory);

public:
	FSimpleVoxelVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
		: FVertexFactory(InFeatureLevel)
	{
	}

	/** Initialize the vertex declaration */
	void Init(FRHICommandListBase& RHICmdList, const FVertexBuffer* VertexBuffer);

	// FRenderResource interface
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual void ReleaseRHI() override;

	// FVertexFactory interface
	static bool ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);

private:
	const FVertexBuffer* VertexBufferPtr = nullptr;
};

/**
 * Simple vertex buffer for FSimpleVoxelVertexFactory.
 */
class VOXELRENDERING_API FSimpleVoxelVertexBuffer : public FVertexBuffer
{
public:
	TArray<FSimpleVoxelVertex> Vertices;

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual FString GetFriendlyName() const override { return TEXT("FSimpleVoxelVertexBuffer"); }

	int32 GetNumVertices() const { return Vertices.Num(); }
};

/**
 * Simple index buffer.
 */
class VOXELRENDERING_API FSimpleVoxelIndexBuffer : public FIndexBuffer
{
public:
	TArray<uint32> Indices;

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual FString GetFriendlyName() const override { return TEXT("FSimpleVoxelIndexBuffer"); }

	int32 GetNumIndices() const { return Indices.Num(); }
};
