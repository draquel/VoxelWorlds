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
 * 40 bytes per vertex.
 *
 * Uses FPackedNormal for tangent basis which is what FLocalVertexFactory expects.
 *
 * TexCoord (UV0): Face UVs for texture tiling within atlas tiles
 * TexCoord1 (UV1): Material data passed as floats to avoid sRGB conversion
 *   UV1.x = MaterialID (0-255 as float)
 *   UV1.y = FaceType (0=Top, 1=Side, 2=Bottom as float)
 *
 * Color channel encoding (for AO and BiomeID):
 *   R: Reserved (legacy MaterialID for compatibility)
 *   G: BiomeID (0-255)
 *   B: AO in top 2 bits - (VertexColor.B * 255) >> 6 gives AO 0-3
 *   A: Reserved (1.0)
 *
 * In material graph, use TexCoord[1] for MaterialID/FaceType:
 *   MaterialID = floor(TexCoord[1].x + 0.5)  // Round to nearest int
 *   FaceType = floor(TexCoord[1].y + 0.5)    // Round to nearest int
 *   BiomeID = round(VertexColor.G * 255)
 *   AO = floor(VertexColor.B * 4)  // 0-3 range
 *   AOFactor = 1.0 - (AO * 0.25)   // For darkening
 *
 * LOD Morphing via Material Parameter Collection (MPC):
 *   Create an MPC with scalar parameters:
 *     - LODStartDistance: Distance where MorphFactor = 0
 *     - LODEndDistance: Distance where MorphFactor = 1
 *     - LODInvRange: 1.0 / (End - Start) for efficient calculation
 *   Assign MPC to UVoxelWorldComponent::LODParameterCollection
 *
 * In material graph, use CollectionParameter nodes to calculate per-pixel MorphFactor:
 *   Distance = length(WorldPosition - CameraPosition)
 *   MorphFactor = saturate((Distance - LODStartDistance) * LODInvRange)
 */
struct VOXELRENDERING_API FVoxelLocalVertex
{
	FVector3f Position;       // 12 bytes, offset 0
	FPackedNormal TangentX;   // 4 bytes, offset 12
	FPackedNormal TangentZ;   // 4 bytes, offset 16 (TangentZ.W contains sign for binormal)
	FVector2f TexCoord;       // 8 bytes, offset 20 (UV0: face UVs for tiling)
	FVector2f TexCoord1;      // 8 bytes, offset 28 (UV1: MaterialID, FaceType as floats)
	FColor Color;             // 4 bytes, offset 36 (AO, BiomeID)
	// Total: 40 bytes

	FVoxelLocalVertex() = default;

	FVoxelLocalVertex(const FVector3f& InPos, const FVector3f& InNormal, const FVector2f& InUV, const FVector2f& InUV1, const FColor& InColor)
		: Position(InPos)
		, TexCoord(InUV)
		, TexCoord1(InUV1)
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

	// Legacy constructor for compatibility
	FVoxelLocalVertex(const FVector3f& InPos, const FVector3f& InNormal, const FVector2f& InUV, const FColor& InColor)
		: Position(InPos)
		, TexCoord(InUV)
		, TexCoord1(FVector2f::ZeroVector)
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
	 * UV1 encoding (avoids sRGB issues):
	 *   UV1.x = MaterialID (0-255 as float)
	 *   UV1.y = FaceType (0=Top, 1=Side, 2=Bottom as float)
	 *
	 * Vertex color encoding:
	 *   R: Reserved (legacy MaterialID for compatibility)
	 *   G: BiomeID (0-255)
	 *   B: AO << 6 (top 2 bits encode AO 0-3)
	 *   A: 255
	 *
	 * In material, access via TexCoord[1] for MaterialID/FaceType:
	 *   - MaterialID = floor(TexCoord[1].x + 0.5)
	 *   - FaceType = floor(TexCoord[1].y + 0.5)
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

		// Defensive: If normal is zero or invalid, default to up vector
		if (Normal.IsNearlyZero(KINDA_SMALL_NUMBER))
		{
			Normal = FVector3f::UpVector;
		}
		else
		{
			Normal = Normal.GetSafeNormal();
		}

		// Compute tangent perpendicular to normal
		FVector3f RefVector = FMath::Abs(Normal.Z) < 0.999f ? FVector3f(0, 0, 1) : FVector3f(1, 0, 0);
		FVector3f TangentVec = FVector3f::CrossProduct(RefVector, Normal).GetSafeNormal();

		Result.TangentX = FPackedNormal(TangentVec);
		Result.TangentZ = FPackedNormal(Normal);
		Result.TangentZ.Vector.W = 127; // Positive binormal sign

		// Get vertex attributes
		uint8 AOValue = VoxelVert.GetAO();
		uint8 MaterialID = VoxelVert.GetMaterialID();
		uint8 BiomeID = VoxelVert.GetBiomeID();

		// Determine face type from normal (for GPU meshing path)
		// Top = +Z (0), Side = X/Y faces (1), Bottom = -Z (2)
		uint8 FaceType = 1; // Default to side
		if (Normal.Z > 0.7f)
		{
			FaceType = 0; // Top
		}
		else if (Normal.Z < -0.7f)
		{
			FaceType = 2; // Bottom
		}

		// Store MaterialID and FaceType in UV1 as exact floats (no interpolation issues)
		Result.TexCoord1 = FVector2f(static_cast<float>(MaterialID), static_cast<float>(FaceType));

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
			// R = Reserved (legacy MaterialID for compatibility)
			// G = BiomeID (0-255)
			// B = AO << 6 (top 2 bits: 0, 64, 128, 192 for AO 0-3)
			// A = 255
			Result.Color = FColor(MaterialID, BiomeID, AOValue << 6, 255);
		}

		return Result;
	}
};

// Verify struct layout at compile time
static_assert(sizeof(FVoxelLocalVertex) == 40, "FVoxelLocalVertex size mismatch");
static_assert(STRUCT_OFFSET(FVoxelLocalVertex, Position) == 0, "Position offset mismatch");
static_assert(STRUCT_OFFSET(FVoxelLocalVertex, TangentX) == 12, "TangentX offset mismatch");
static_assert(STRUCT_OFFSET(FVoxelLocalVertex, TangentZ) == 16, "TangentZ offset mismatch");
static_assert(STRUCT_OFFSET(FVoxelLocalVertex, TexCoord) == 20, "TexCoord offset mismatch");
static_assert(STRUCT_OFFSET(FVoxelLocalVertex, TexCoord1) == 28, "TexCoord1 offset mismatch");
static_assert(STRUCT_OFFSET(FVoxelLocalVertex, Color) == 36, "Color offset mismatch");

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

	/** Tangent buffer for SRV (interleaved TangentX + TangentZ, 8 bytes per vertex) */
	FBufferRHIRef TangentBufferRHI;

	/** Tangent SRV for FLocalVertexFactory (needed for GPUScene manual vertex fetch) */
	FShaderResourceViewRHIRef TangentsSRV;

	/** TexCoord buffer for SRV */
	FBufferRHIRef TexCoordBufferRHI;

	/** TexCoord SRV for FLocalVertexFactory (needed for GPUScene manual vertex fetch) */
	FShaderResourceViewRHIRef TexCoordSRV;

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
		SIZE_T TangentSize = VertexCount * 8; // 2 x FPackedNormal (4 bytes each)
		SIZE_T TexCoordSize = VertexCount * sizeof(FVector2f);
		return VertexSize + IndexSize + ColorSize + TangentSize + TexCoordSize;
	}

	/** Release GPU resources */
	void ReleaseResources()
	{
		TexCoordSRV.SafeRelease();
		TexCoordBufferRHI.SafeRelease();
		TangentsSRV.SafeRelease();
		TangentBufferRHI.SafeRelease();
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
 *
 * @param RHICmdList RHI command list
 * @param VertexFactory Vertex factory to initialize
 * @param VertexBuffer Vertex buffer containing FVoxelLocalVertex data
 * @param ColorSRV SRV for vertex colors (manual vertex fetch)
 * @param TangentsSRV Optional SRV for tangents (manual vertex fetch, used by GPUScene)
 * @param TexCoordSRV Optional SRV for texture coordinates (manual vertex fetch, used by GPUScene)
 */
void VOXELRENDERING_API InitVoxelLocalVertexFactory(
	FRHICommandListBase& RHICmdList,
	FLocalVertexFactory* VertexFactory,
	const FVertexBuffer* VertexBuffer,
	FRHIShaderResourceView* ColorSRV,
	FRHIShaderResourceView* TangentsSRV = nullptr,
	FRHIShaderResourceView* TexCoordSRV = nullptr);
