// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"
#include "LocalVertexFactory.h"
#include "VoxelVertex.h"
#include "RenderUtils.h"  // For FPackedNormal

#include <atomic>

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
 *   R: the far field's first material of a two-material quad (G45); 0 on chunk meshes
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

	/**
	 * Convert from FVoxelVertex.
	 *
	 * UV1 encoding (avoids sRGB issues):
	 *   UV1.x = MaterialID (0-255 as float)
	 *   UV1.y = FaceType (0=Top, 1=Side, 2=Bottom as float)
	 *
	 * Vertex color encoding:
	 *   R: the far field's first material of a two-material quad (G45); 0 on chunk meshes
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
	/**
	 * FromVoxelVertex with the face type (top, side, bottom) measured against a local up instead of world +Z (spherical
	 * SP-D3): a planet's chunks pass the radial direction at the vertex, so a hilltop at the equator is a top face.
	 *
	 * @param VoxelVert The mesher's vertex
	 * @param FaceUp Unit local up at the vertex
	 * @return The converted vertex
	 */
	static FVoxelLocalVertex FromVoxelVertex(const FVoxelVertex& VoxelVert, const FVector3f& FaceUp)
	{
		FVoxelLocalVertex Result = FromVoxelVertex(VoxelVert);
		FVector3f Normal = VoxelVert.GetNormal();
		Normal = Normal.IsNearlyZero(KINDA_SMALL_NUMBER) ? FaceUp : Normal.GetSafeNormal();
		const float Along = FVector3f::DotProduct(Normal, FaceUp);
		const uint8 FaceType = Along > 0.7f ? 0 : (Along < -0.7f ? 2 : 1);
		Result.TexCoord1.Y = static_cast<float>(FaceType);
		return Result;
	}

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

		// Encode data for the material graph:
		// R = MaterialID (0-255)
		// G = BiomeID (0-255)
		// B = AO << 6 (top 2 bits: 0, 64, 128, 192 for AO 0-3)
		// A = 255
		Result.Color = FColor(MaterialID, BiomeID, AOValue << 6, 255);

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

	/**
	 * The vertex stream: the positions alone (FVector3f) where FLocalVertexFactory fetches the other attributes from the
	 * shader resource views below (manual vertex fetch), else interleaved FVoxelLocalVertex (G120). The position is at
	 * offset 0 in both, VertexStride bytes apart.
	 */
	FBufferRHIRef VertexBufferRHI;

	/** VertexBufferRHI's bytes a vertex: sizeof(FVector3f) for the positions alone, else sizeof(FVoxelLocalVertex) */
	uint32 VertexStride = sizeof(FVoxelLocalVertex);

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

	/**
	 * The scene proxy's count of the mesh buffer bytes it holds (FVoxelSceneProxy::GetMeshBufferBytes): set when the buffers
	 * are created, which add GetGPUMemoryUsage to it, and ReleaseResources takes them off again. Null on data that never held
	 * buffers.
	 */
	std::atomic<int64>* LiveBytesCounter = nullptr;

	/** Check if GPU buffers are valid */
	FORCEINLINE bool HasValidBuffers() const
	{
		return VertexBufferRHI.IsValid() && IndexBufferRHI.IsValid() && VertexCount > 0 && IndexCount > 0;
	}

	/**
	 * The bytes of one mesh's buffers as the scene proxy creates them: the vertex stream, the colour (4 bytes a vertex),
	 * tangent (8) and texcoord (UV0 and UV1, 16: G119 counted 8) buffers of the shader resource views, and the indices.
	 *
	 * @param InVertexCount Vertices
	 * @param InIndexCount Indices
	 * @param InVertexStride The vertex stream's bytes a vertex (VertexStride: 12 for the positions alone, 40 interleaved)
	 * @return The bytes
	 */
	static SIZE_T CalcGPUMemoryUsage(uint32 InVertexCount, uint32 InIndexCount, uint32 InVertexStride)
	{
		const SIZE_T PerVertex = InVertexStride + sizeof(FColor) + 2 * sizeof(FPackedNormal) + sizeof(FVector4f);
		return static_cast<SIZE_T>(InVertexCount) * PerVertex + static_cast<SIZE_T>(InIndexCount) * sizeof(uint32);
	}

	/** @return The GPU memory of this mesh's buffers in bytes (CalcGPUMemoryUsage) */
	FORCEINLINE SIZE_T GetGPUMemoryUsage() const
	{
		return CalcGPUMemoryUsage(VertexCount, IndexCount, VertexStride);
	}

	/** Release GPU resources (and take their bytes off the proxy's count) */
	void ReleaseResources()
	{
		if (LiveBytesCounter && VertexBufferRHI.IsValid())
		{
			LiveBytesCounter->fetch_sub(static_cast<int64>(GetGPUMemoryUsage()), std::memory_order_relaxed);
		}
		LiveBytesCounter = nullptr;
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
 *
 * @param RHICmdList RHI command list
 * @param VertexFactory Vertex factory to initialize
 * @param VertexBuffer Vertex buffer: FVoxelLocalVertex data, or the positions alone (bPositionOnly)
 * @param bPositionOnly The buffer holds the positions alone (FVector3f): only for a vertex factory with manual vertex
 *        fetch, which reads the tangents, texcoords and colours from the SRVs (G120)
 * @param ColorSRV SRV for vertex colors (manual vertex fetch)
 * @param TangentsSRV Optional SRV for tangents (manual vertex fetch, used by GPUScene)
 * @param TexCoordSRV Optional SRV for texture coordinates (manual vertex fetch, used by GPUScene)
 */
void VOXELRENDERING_API InitVoxelLocalVertexFactory(
	FRHICommandListBase& RHICmdList,
	FLocalVertexFactory* VertexFactory,
	const FVertexBuffer* VertexBuffer,
	bool bPositionOnly,
	FRHIShaderResourceView* ColorSRV,
	FRHIShaderResourceView* TangentsSRV = nullptr,
	FRHIShaderResourceView* TexCoordSRV = nullptr);
