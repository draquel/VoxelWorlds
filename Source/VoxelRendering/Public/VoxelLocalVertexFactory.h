// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"
#include "LocalVertexFactory.h"
#include "VoxelVertex.h"
#include "RenderUtils.h"  // For FPackedNormal

/**
 * Vertex format compatible with FLocalVertexFactory.
 * 32 bytes per vertex (vs 28 for FVoxelVertex).
 *
 * Uses FPackedNormal for tangent basis which is what FLocalVertexFactory expects.
 */
struct VOXELRENDERING_API FVoxelLocalVertex
{
	FVector3f Position;       // 12 bytes, offset 0
	FPackedNormal TangentX;   // 4 bytes, offset 12
	FPackedNormal TangentZ;   // 4 bytes, offset 16 (TangentZ.W contains sign for binormal)
	FVector2f TexCoord;       // 8 bytes, offset 20
	FColor Color;             // 4 bytes, offset 28
	// Total: 32 bytes

	FVoxelLocalVertex() = default;

	FVoxelLocalVertex(const FVector3f& InPos, const FVector3f& InNormal, const FVector2f& InUV, const FColor& InColor)
		: Position(InPos)
		, TexCoord(InUV)
		, Color(InColor)
	{
		// Compute tangent from normal
		FVector3f RefVector = FMath::Abs(InNormal.Z) < 0.999f ? FVector3f(0, 0, 1) : FVector3f(1, 0, 0);
		FVector3f TangentVec = FVector3f::CrossProduct(RefVector, InNormal).GetSafeNormal();

		TangentX = FPackedNormal(TangentVec);
		TangentZ = FPackedNormal(InNormal);
		// Set W component of TangentZ to encode binormal sign (127 = positive/no flip)
		TangentZ.Vector.W = 127;
	}

	/** Convert from FVoxelVertex */
	static FVoxelLocalVertex FromVoxelVertex(const FVoxelVertex& VoxelVert)
	{
		FVoxelLocalVertex Result;
		Result.Position = VoxelVert.Position;
		Result.TexCoord = VoxelVert.UV;

		// Get normal and compute tangent
		FVector3f Normal = VoxelVert.GetNormal();
		FVector3f RefVector = FMath::Abs(Normal.Z) < 0.999f ? FVector3f(0, 0, 1) : FVector3f(1, 0, 0);
		FVector3f TangentVec = FVector3f::CrossProduct(RefVector, Normal).GetSafeNormal();

		Result.TangentX = FPackedNormal(TangentVec);
		Result.TangentZ = FPackedNormal(Normal);
		Result.TangentZ.Vector.W = 127; // Positive binormal sign

		// Convert AO to grayscale color (0=occluded/dark, 3=unoccluded/bright)
		// AO 0 = fully occluded = dark, AO 3 = unoccluded = bright
		uint8 AOValue = VoxelVert.GetAO();
		uint8 Brightness = static_cast<uint8>(255 - (AOValue * 64)); // 0->255, 1->191, 2->127, 3->63
		// Actually, AO 0 = unoccluded, 3 = fully occluded (from the mesher)
		// So: AO 0 = bright (255), AO 3 = dark (64)
		Brightness = static_cast<uint8>(255 - (AOValue * 64));
		Result.Color = FColor(Brightness, Brightness, Brightness, 255);

		return Result;
	}
};

// Verify struct layout at compile time
static_assert(sizeof(FVoxelLocalVertex) == 32, "FVoxelLocalVertex size mismatch");
static_assert(STRUCT_OFFSET(FVoxelLocalVertex, Position) == 0, "Position offset mismatch");
static_assert(STRUCT_OFFSET(FVoxelLocalVertex, TangentX) == 12, "TangentX offset mismatch");
static_assert(STRUCT_OFFSET(FVoxelLocalVertex, TangentZ) == 16, "TangentZ offset mismatch");
static_assert(STRUCT_OFFSET(FVoxelLocalVertex, TexCoord) == 20, "TexCoord offset mismatch");
static_assert(STRUCT_OFFSET(FVoxelLocalVertex, Color) == 28, "Color offset mismatch");

/**
 * Per-chunk GPU data for use with FLocalVertexFactory.
 */
struct VOXELRENDERING_API FVoxelChunkRenderData
{
	/** Chunk coordinate */
	FIntVector ChunkCoord = FIntVector::ZeroValue;

	/** LOD level */
	int32 LODLevel = 0;

	/** Number of vertices */
	uint32 VertexCount = 0;

	/** Number of indices */
	uint32 IndexCount = 0;

	/** Local-space bounding box (in absolute world space since positions are world space) */
	FBox WorldBounds = FBox(ForceInit);

	/** World position of chunk origin */
	FVector ChunkWorldPosition = FVector::ZeroVector;

	/** LOD morph factor for smooth transitions */
	float MorphFactor = 0.0f;

	/** Visibility flag */
	bool bIsVisible = true;

	/** Interleaved vertex buffer (FVoxelLocalVertex format) */
	FBufferRHIRef VertexBufferRHI;

	/** Index buffer */
	FBufferRHIRef IndexBufferRHI;

	/** Separate color buffer for SRV (FLocalVertexFactory needs this) */
	FBufferRHIRef ColorBufferRHI;

	/** Color SRV for FLocalVertexFactory */
	FShaderResourceViewRHIRef ColorSRV;

	/** Check if GPU buffers are valid */
	FORCEINLINE bool HasValidBuffers() const
	{
		return VertexBufferRHI.IsValid() && IndexBufferRHI.IsValid() && VertexCount > 0 && IndexCount > 0;
	}

	/** Get approximate GPU memory usage in bytes */
	FORCEINLINE SIZE_T GetGPUMemoryUsage() const
	{
		SIZE_T VertexSize = VertexCount * sizeof(FVoxelLocalVertex);
		SIZE_T IndexSize = IndexCount * sizeof(uint32);
		SIZE_T ColorSize = VertexCount * sizeof(FColor);
		return VertexSize + IndexSize + ColorSize;
	}

	/** Release GPU resources */
	void ReleaseResources()
	{
		ColorSRV.SafeRelease();
		ColorBufferRHI.SafeRelease();
		VertexBufferRHI.SafeRelease();
		IndexBufferRHI.SafeRelease();
		VertexCount = 0;
		IndexCount = 0;
	}
};

/**
 * Vertex buffer wrapper for FLocalVertexFactory compatibility.
 * Wraps an existing RHI buffer.
 */
class VOXELRENDERING_API FVoxelLocalVertexBuffer : public FVertexBuffer
{
public:
	void InitWithRHIBuffer(FBufferRHIRef InVertexBufferRHI)
	{
		PendingBuffer = InVertexBufferRHI;
	}

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		VertexBufferRHI = PendingBuffer;
	}

	virtual void ReleaseRHI() override
	{
		VertexBufferRHI.SafeRelease();
		PendingBuffer.SafeRelease();
	}

	virtual FString GetFriendlyName() const override { return TEXT("FVoxelLocalVertexBuffer"); }

private:
	FBufferRHIRef PendingBuffer;
};

/**
 * Index buffer wrapper for FLocalVertexFactory compatibility.
 */
class VOXELRENDERING_API FVoxelLocalIndexBuffer : public FIndexBuffer
{
public:
	FVoxelLocalIndexBuffer() : NumIndices(0) {}

	void InitWithRHIBuffer(FBufferRHIRef InIndexBufferRHI, uint32 InNumIndices)
	{
		PendingBuffer = InIndexBufferRHI;
		NumIndices = InNumIndices;
	}

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		IndexBufferRHI = PendingBuffer;
	}

	virtual void ReleaseRHI() override
	{
		IndexBufferRHI.SafeRelease();
		PendingBuffer.SafeRelease();
		NumIndices = 0;
	}

	virtual FString GetFriendlyName() const override { return TEXT("FVoxelLocalIndexBuffer"); }

	uint32 GetNumIndices() const { return NumIndices; }

private:
	FBufferRHIRef PendingBuffer;
	uint32 NumIndices;
};

/**
 * Helper function to initialize FLocalVertexFactory with voxel vertex data.
 * Uses the same pattern as LocalVFTestComponent.
 */
void VOXELRENDERING_API InitVoxelLocalVertexFactory(
	FRHICommandListBase& RHICmdList,
	FLocalVertexFactory* VertexFactory,
	const FVertexBuffer* VertexBuffer,
	FRHIShaderResourceView* ColorSRV);
