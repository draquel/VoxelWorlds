// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelLocalVertexFactory.h"

namespace Nanite
{
	struct FResources;
}

/**
 * How a chunk mesh is encoded for Nanite (runtime Nanite spike, N1; Documentation/RUNTIME_NANITE_SPIKE_PLAN.md).
 */
struct VOXELRENDERING_API FVoxelNaniteEncodeSettings
{
	/** Position step 2^-PositionPrecision uu (4 = 1/16 uu); lowered for a cluster too large for 21 bits an axis */
	int32 PositionPrecision = 4;
	/** Octahedral normal bits an axis (the engine's default 8) */
	int32 NormalPrecision = 8;
	/** Tangent angle bits (the engine's default 7) */
	int32 TangentPrecision = 7;
	/**
	 * A triangle's corner refers back to a vertex the cluster already holds (5-bit references, at most 29 vertices back)
	 * instead of storing it again. Off: every triangle stores three new vertices (simplest, 85 triangles a cluster at most,
	 * and each vertex stored once per triangle using it).
	 */
	bool bVertexReuse = true;
};

/** What an encode made (counts and sizes for the spike's numbers) */
struct VOXELRENDERING_API FVoxelNaniteEncodeStats
{
	int32 NumTriangles = 0;
	int32 NumClusters = 0;
	int32 NumPages = 0;
	int32 NumHierarchyNodes = 0;
	/** Vertices stored in the clusters (a vertex stored again in another cluster, or past the reference window, counts again) */
	int32 NumStoredVertices = 0;
	/** The pages' bytes on the GPU (each page takes a 32 KB root page slot whatever its size) and as stored */
	int64 GpuBytes = 0;
	int64 RootDataBytes = 0;
	/** The position precision used (lower than asked when a cluster needed it) */
	int32 PositionPrecision = 0;
	double EncodeSeconds = 0.0;
};

/**
 * Encode a chunk mesh as a Nanite resource whose pages are all root pages (nothing streams), under a flat hierarchy whose
 * clusters are always drawn: Nanite as a rasterizer, no level of detail inside the mesh. Clusters are cut in index
 * order (at most 128 triangles and 256 vertices). Degenerate triangles (two equal indices) are dropped. Any thread.
 *
 * The resource is plain data until FResources::InitResources hands it to the engine's streaming manager.
 *
 * @param Vertices  The mesh's vertices as the custom vertex factory stores them (position, tangents, UV0, UV1, colour)
 * @param Indices   Triangle indices into Vertices
 * @param Settings  Precision and vertex reuse
 * @param OutStats  Receives counts and sizes (optional)
 * @return The resource, null for a mesh without triangles
 */
VOXELRENDERING_API TUniquePtr<Nanite::FResources> EncodeVoxelNaniteMesh(TConstArrayView<FVoxelLocalVertex> Vertices,
	TConstArrayView<uint32> Indices, const FVoxelNaniteEncodeSettings& Settings = FVoxelNaniteEncodeSettings(),
	FVoxelNaniteEncodeStats* OutStats = nullptr);

/** What a validation found (the largest differences between the decoded mesh and the input) */
struct VOXELRENDERING_API FVoxelNaniteValidation
{
	bool bValid = false;
	/** The first problem found, empty when valid */
	FString Error;
	int32 NumDecodedTriangles = 0;
	/** Largest position error (uu), smallest normal and tangent dot products, largest UV errors */
	float MaxPositionError = 0.0f;
	float MinNormalDot = 1.0f;
	float MinTangentDot = 1.0f;
	float MaxUV0Error = 0.0f;
	float MaxUV1Error = 0.0f;
	int32 ColorMismatches = 0;
};

/**
 * Decode a resource EncodeVoxelNaniteMesh made, on the CPU, as the GPU transcode would read it (the fixup chunks, the page
 * and cluster headers, the packed clusters, the strip indices, the byte streams), and compare it with the input: the same
 * triangles in the same order and winding, positions within half a quantization step, normals and tangents within their
 * bits, colours and UV1 (material and face type) exact, UV0 within the UV encoding's precision. Also checks each page's
 * GPU size against a root page, and that every cluster is reached by exactly one hierarchy slot. Any thread.
 *
 * A format slip shows on the GPU as garbage or a hang, not an error; this check runs first (the spike's validation mode).
 *
 * @param Resources The encoded resource (before InitResources: the streaming manager empties RootData after the upload)
 * @param Vertices  The input vertices
 * @param Indices   The input indices
 * @param Settings  The settings it was encoded with
 * @return What the comparison found
 */
VOXELRENDERING_API FVoxelNaniteValidation ValidateVoxelNaniteMesh(const Nanite::FResources& Resources,
	TConstArrayView<FVoxelLocalVertex> Vertices, TConstArrayView<uint32> Indices,
	const FVoxelNaniteEncodeSettings& Settings = FVoxelNaniteEncodeSettings());
