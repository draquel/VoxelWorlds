// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"
#include "LocalVertexFactory.h"
#include "VoxelVertex.h"
#include "VoxelMaterialRegistry.h"
#include "RenderUtils.h"  // For FPackedNormal

/**
 * Debug mode for vertex color output.
 * When enabled, material colors are baked directly into vertex RGB for visual debugging.
 * When disabled, encoded data (AO, MaterialID, BiomeID) is stored for material graph use.
 */
namespace EVoxelVertexColorDebugMode
{
	/** Normal mode: R=AO, G=MaterialID/255, B=BiomeID/255, A=1 */
	constexpr int32 Disabled = 0;

	/** Debug mode: RGB=MaterialColor * AO, A=1 (visual debugging) */
	constexpr int32 MaterialColors = 1;

	/** Debug mode: RGB=BiomeID as hue, A=1 */
	constexpr int32 BiomeColors = 2;
}

/** Current debug mode - change this to switch modes (defined in VoxelSceneProxy.cpp) */
extern VOXELRENDERING_API int32 GVoxelVertexColorDebugMode;

/**
 * Vertex format compatible with FLocalVertexFactory.
 * 32 bytes per vertex (vs 28 for FVoxelVertex).
 *
 * Uses FPackedNormal for tangent basis which is what FLocalVertexFactory expects.
 *
 * Color channel encoding (ALIGNED with CPU mesher for shared materials):
 *   R: MaterialID (0-255) - VertexColor.R * 255 for texture array index
 *   G: BiomeID (0-255) - VertexColor.G * 255 for biome blending
 *   B: AO in top 2 bits - (VertexColor.B * 255) >> 6 gives AO 0-3
 *   A: Reserved (1.0)
 *
 * In material graph:
 *   MaterialID = round(VertexColor.R * 255)
 *   BiomeID = round(VertexColor.G * 255)
 *   AO = floor(VertexColor.B * 4)  // 0-3 range
 *   AOFactor = 1.0 - (AO * 0.25)   // For darkening
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

	/**
	 * Convert from FVoxelVertex.
	 *
	 * Vertex color encoding (aligned with CPU mesher for shared materials):
	 *   R: MaterialID (0-255)
	 *   G: BiomeID (0-255)
	 *   B: AO << 6 (top 2 bits encode AO 0-3)
	 *   A: 255
	 *
	 * In material, access via VertexColor node:
	 *   - MaterialID = round(VertexColor.R * 255)
	 *   - BiomeID = round(VertexColor.G * 255)
	 *   - AO = floor(VertexColor.B * 4)
	 *   - AOFactor = 1.0 - (AO * 0.25)
	 */
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

		// Get vertex attributes
		uint8 AOValue = VoxelVert.GetAO();
		uint8 MaterialID = VoxelVert.GetMaterialID();
		uint8 BiomeID = VoxelVert.GetBiomeID();

		// Apply debug mode
		if (GVoxelVertexColorDebugMode == EVoxelVertexColorDebugMode::MaterialColors)
		{
			// Debug: Bake material color * AO directly into vertex color
			float AOFactor = 1.0f - (AOValue * 0.25f); // 0->1.0, 1->0.75, 2->0.5, 3->0.25
			FColor MatColor = FVoxelMaterialRegistry::GetMaterialColor(MaterialID);
			Result.Color = FColor(
				static_cast<uint8>(MatColor.R * AOFactor),
				static_cast<uint8>(MatColor.G * AOFactor),
				static_cast<uint8>(MatColor.B * AOFactor),
				255
			);
		}
		else if (GVoxelVertexColorDebugMode == EVoxelVertexColorDebugMode::BiomeColors)
		{
			// Debug: Show BiomeID as distinct color using lookup table
			// Each BiomeID gets a unique, easily distinguishable color
			static const FColor BiomeDebugColors[] = {
				FColor(255, 0, 0, 255),     // 0 = Red
				FColor(0, 255, 0, 255),     // 1 = Green
				FColor(0, 0, 255, 255),     // 2 = Blue
				FColor(255, 255, 0, 255),   // 3 = Yellow
				FColor(255, 0, 255, 255),   // 4 = Magenta
				FColor(0, 255, 255, 255),   // 5 = Cyan
				FColor(255, 128, 0, 255),   // 6 = Orange
				FColor(128, 0, 255, 255),   // 7 = Purple
			};
			constexpr int32 NumBiomeColors = sizeof(BiomeDebugColors) / sizeof(BiomeDebugColors[0]);

			float AOFactor = 1.0f - (AOValue * 0.25f);
			FColor BaseColor = BiomeDebugColors[BiomeID % NumBiomeColors];
			Result.Color = FColor(
				static_cast<uint8>(BaseColor.R * AOFactor),
				static_cast<uint8>(BaseColor.G * AOFactor),
				static_cast<uint8>(BaseColor.B * AOFactor),
				255
			);
		}
		else
		{
			// Normal mode: Encode data for material graph
			// ALIGNED with CPU mesher (FChunkMeshData.Colors) for shared materials:
			// R = MaterialID (0-255)
			// G = BiomeID (0-255)
			// B = AO << 6 (top 2 bits: 0, 64, 128, 192 for AO 0-3)
			// A = 255
			Result.Color = FColor(MaterialID, BiomeID, AOValue << 6, 255);
		}

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
