// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelLocalVertexFactory.h"

namespace Nanite
{
	struct FResources;
}

/**
 * How a chunk mesh is encoded for Nanite (runtime Nanite; Documentation/RUNTIME_NANITE_SPIKE_PLAN.md).
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

	/**
	 * The material IDs drawn masked (alpha cutout: the leaves; FVoxelMaterialDefinition::bIsMasked), one bit each (D162). A
	 * triangle whose material (UV1.x of its first corner) is set here goes to material section 1, drawn with the masked
	 * material; every other triangle to section 0, drawn opaque. All clear: one section, the triangles in input order.
	 */
	uint64 MaskedMaterials[4] = { 0, 0, 0, 0 };

	/** @return Whether any material is drawn masked */
	bool HasMaskedMaterials() const { return (MaskedMaterials[0] | MaskedMaterials[1] | MaskedMaterials[2] | MaskedMaterials[3]) != 0; }

	/** @return Whether MaterialID is drawn masked */
	bool IsMaskedMaterial(int32 MaterialID) const
	{
		return MaterialID >= 0 && MaterialID < 256 && (MaskedMaterials[MaterialID >> 6] >> (MaterialID & 63) & 1) != 0;
	}
};

/** What an encode made (counts and sizes for voxel.Render.NaniteStats) */
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
	/** Triangles in material section 1 (masked, D162), and the sections the resource uses (1 or 2) */
	int32 NumMaskedTriangles = 0;
	int32 NumMaterialSections = 1;
	double EncodeSeconds = 0.0;
};

/**
 * Encode a chunk mesh as a Nanite resource whose pages are all root pages (nothing streams), under a flat hierarchy whose
 * clusters are always drawn: Nanite as a rasterizer, no level of detail inside the mesh. Clusters are cut in index
 * order (at most 128 triangles and 256 vertices), the masked materials' triangles (Settings.MaskedMaterials) after the
 * others, each cluster in one material section (0 opaque, 1 masked). Degenerate triangles (two equal indices) are
 * dropped. Any thread.
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

/**
 * The triangles an encode keeps, in the order it stores them: degenerate ones dropped, then (with masked materials) the
 * opaque ones before the masked ones, each group in input order (D162). Any thread.
 *
 * @param Vertices    The mesh's vertices (UV1.x is the material)
 * @param Indices     Triangle indices into Vertices
 * @param Settings    The masked materials
 * @param OutIndices  Receives three indices a triangle, in the encode's order
 * @param OutSections Receives each triangle's material section (0 opaque, 1 masked)
 */
VOXELRENDERING_API void OrderVoxelNaniteTriangles(TConstArrayView<FVoxelLocalVertex> Vertices, TConstArrayView<uint32> Indices,
	const FVoxelNaniteEncodeSettings& Settings, TArray<uint32>& OutIndices, TArray<uint8>& OutSections);

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
 * triangles in the same order (OrderVoxelNaniteTriangles) and winding, each in its cluster's material section, positions within half a quantization step, normals and tangents within their
 * bits, colours and UV1 (material and face type) exact, UV0 within the UV encoding's precision. Also checks each page's
 * GPU size against a root page, and that every cluster is reached by exactly one hierarchy slot. Any thread.
 *
 * A format slip shows on the GPU as garbage or a hang, not an error; this check runs first (voxel.Render.Nanite.Validate).
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
