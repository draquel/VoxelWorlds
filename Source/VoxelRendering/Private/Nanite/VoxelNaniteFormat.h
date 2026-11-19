// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/NaniteResources.h"

/**
 * The Nanite page layouts a runtime encoder writes and the plugin cannot include (runtime Nanite spike, N1;
 * Documentation/RUNTIME_NANITE_SPIKE_PLAN.md section 6, "What the plugin includes and what it copies").
 *
 * Copied from UE 5.8.4:
 * - the page GPU header, the page and cluster disk headers: NaniteBuilder's Encode/NaniteEncode.cpp (a Developer module's
 *   private file, absent from Shipping and Test targets);
 * - the page section sizes and the UV header: NaniteBuilder's Encode/NaniteEncodeShared.h;
 * - the fixup chunk's records: Engine/Internal/Nanite/NaniteFixupChunk.h (an Internal header, which the build tool gives
 *   only to modules of the engine's scope).
 *
 * The public layouts (FPackedCluster, FPackedHierarchyNode, FPageStreamingState, FPageRangeKey, the NANITE_* constants)
 * come from the engine's own headers. Every copy here is checked by size; an engine upgrade compares the copies with the
 * new engine's files before anything else.
 */
namespace VoxelNanite
{
	/** NaniteEncode.cpp FPageGPUHeader: the first 16 bytes of a page on the GPU */
	struct FPageGPUHeader
	{
		/** NumClusters: 16, MaxClusterBoneInfluences: 8, MaxVoxelBoneInfluences: 8 */
		uint32 NumClusters_MaxClusterBoneInfluences_MaxVoxelBoneInfluences = 0;
		uint32 Pad[3] = { 0, 0, 0 };
	};
	static_assert(sizeof(FPageGPUHeader) == NANITE_GPU_PAGE_HEADER_SIZE, "FPageGPUHeader out of sync with the engine");

	/** NaniteEncode.cpp FPageDiskHeader: the start of a page's stored data, after its fixup chunk */
	struct FPageDiskHeader
	{
		uint32 NumClusters = 0;
		uint32 NumRawFloat4s = 0;
		uint32 NumVertexRefs = 0;
		uint32 StripBitmaskOffset = 0;
		uint32 VertexRefBitmaskOffset = 0;
	};
	static_assert(sizeof(FPageDiskHeader) == 5 * 4, "FPageDiskHeader out of sync with the engine");

	/** NaniteEncode.cpp FClusterDiskHeader: one per cluster, after the page disk header; offsets from the page's start */
	struct FClusterDiskHeader
	{
		uint32 DecodeInfoOffset = 0;
		uint32 IndexDataOffset = 0;
		uint32 PageClusterMapOffset = 0;
		uint32 VertexRefDataOffset = 0;
		uint32 LowBytesOffset = 0;
		uint32 MidBytesOffset = 0;
		uint32 HighBytesOffset = 0;
		uint32 NumVertexRefs = 0;
		uint32 NumPrevRefVerticesBeforeDwords = 0;
		uint32 NumPrevNewVerticesBeforeDwords = 0;
	};
	static_assert(sizeof(FClusterDiskHeader) == 10 * 4, "FClusterDiskHeader out of sync with the engine");

	/** NaniteEncodeShared.h FPackedUVHeader: per cluster and UV set, (Min << 5) | NumBits for U and V */
	struct FPackedUVHeader
	{
		uint32 Data[2] = { 0, 0 };
	};
	static_assert(sizeof(FPackedUVHeader) == 8, "FPackedUVHeader out of sync with the engine");

	/**
	 * NaniteEncodeShared.h FPageSections: the size in bytes of each GPU section of a page (a cluster's own, or a page's
	 * sum), and the offsets they give in the order the transcode lays a page out.
	 */
	struct FPageSections
	{
		uint32 Cluster = 0;
		uint32 ClusterBoneInfluence = 0;
		uint32 VoxelBoneInfluence = 0;
		uint32 MaterialTable = 0;
		uint32 VertReuseBatchInfo = 0;
		uint32 BoneInfluence = 0;
		uint32 BrickData = 0;
		uint32 ExtendedData = 0;
		uint32 DecodeInfo = 0;
		uint32 Index = 0;
		uint32 Position = 0;
		uint32 Attribute = 0;

		uint32 GetClusterBoneInfluenceSize() const { return Align(ClusterBoneInfluence, 16); }
		uint32 GetVoxelBoneInfluenceSize() const { return Align(VoxelBoneInfluence, 16); }
		uint32 GetMaterialTableSize() const { return Align(MaterialTable, 16); }
		uint32 GetVertReuseBatchInfoSize() const { return Align(VertReuseBatchInfo, 16); }
		uint32 GetBoneInfluenceSize() const { return Align(BoneInfluence, 16); }
		uint32 GetBrickDataSize() const { return Align(BrickData, 16); }
		uint32 GetExtendedDataSize() const { return Align(ExtendedData, 16); }
		uint32 GetDecodeInfoSize() const { return Align(DecodeInfo, 16); }

		uint32 GetClusterOffset() const { return NANITE_GPU_PAGE_HEADER_SIZE; }
		uint32 GetClusterBoneInfluenceOffset() const { return GetClusterOffset() + Cluster; }
		uint32 GetVoxelBoneInfluenceOffset() const { return GetClusterBoneInfluenceOffset() + GetClusterBoneInfluenceSize(); }
		uint32 GetMaterialTableOffset() const { return GetVoxelBoneInfluenceOffset() + GetVoxelBoneInfluenceSize(); }
		uint32 GetVertReuseBatchInfoOffset() const { return GetMaterialTableOffset() + GetMaterialTableSize(); }
		uint32 GetBoneInfluenceOffset() const { return GetVertReuseBatchInfoOffset() + GetVertReuseBatchInfoSize(); }
		uint32 GetBrickDataOffset() const { return GetBoneInfluenceOffset() + GetBoneInfluenceSize(); }
		uint32 GetExtendedDataOffset() const { return GetBrickDataOffset() + GetBrickDataSize(); }
		uint32 GetDecodeInfoOffset() const { return GetExtendedDataOffset() + GetExtendedDataSize(); }
		uint32 GetIndexOffset() const { return GetDecodeInfoOffset() + GetDecodeInfoSize(); }
		uint32 GetPositionOffset() const { return GetIndexOffset() + Index; }
		uint32 GetAttributeOffset() const { return GetPositionOffset() + Position; }
		uint32 GetTotal() const { return GetAttributeOffset() + Attribute; }

		/** @return The sections' offsets in a page laid out with these sizes */
		FPageSections GetOffsets() const
		{
			FPageSections Offsets;
			Offsets.Cluster = GetClusterOffset();
			Offsets.ClusterBoneInfluence = GetClusterBoneInfluenceOffset();
			Offsets.VoxelBoneInfluence = GetVoxelBoneInfluenceOffset();
			Offsets.MaterialTable = GetMaterialTableOffset();
			Offsets.VertReuseBatchInfo = GetVertReuseBatchInfoOffset();
			Offsets.BoneInfluence = GetBoneInfluenceOffset();
			Offsets.BrickData = GetBrickDataOffset();
			Offsets.ExtendedData = GetExtendedDataOffset();
			Offsets.DecodeInfo = GetDecodeInfoOffset();
			Offsets.Index = GetIndexOffset();
			Offsets.Position = GetPositionOffset();
			Offsets.Attribute = GetAttributeOffset();
			return Offsets;
		}

		void operator+=(const FPageSections& Other)
		{
			Cluster += Other.Cluster;
			ClusterBoneInfluence += Other.ClusterBoneInfluence;
			VoxelBoneInfluence += Other.VoxelBoneInfluence;
			MaterialTable += Other.MaterialTable;
			VertReuseBatchInfo += Other.VertReuseBatchInfo;
			BoneInfluence += Other.BoneInfluence;
			BrickData += Other.BrickData;
			ExtendedData += Other.ExtendedData;
			DecodeInfo += Other.DecodeInfo;
			Index += Other.Index;
			Position += Other.Position;
			Attribute += Other.Attribute;
		}
	};

	/** NaniteFixupChunk.h FFixupChunk::FHeader; NANITE_FIXUP_MAGIC in Magic */
	struct FFixupHeader
	{
		uint16 Magic = 0;
		uint16 NumGroupFixups = 0;
		uint16 NumPartFixups = 0;
		uint16 NumClusters = 0;
		/** Pages to reconsider for fixup when this page is installed or removed (none with root pages only) */
		uint16 NumReconsiderPages = 0;
		uint16 Pad = 0;
		uint32 NumParentFixups = 0;
		uint32 NumHierarchyLocations = 0;
		uint32 NumClusterIndices = 0;
	};
	static_assert(sizeof(FFixupHeader) == 24, "FFixupChunk::FHeader out of sync with the engine");

	/** NaniteFixupChunk.h FFixupChunk::FGroupFixup: the pages a group needs before its parts are installed */
	struct FFixupGroup
	{
		/** FPageRangeKey::Value */
		uint32 PageDependencies = NANITE_PAGE_RANGE_KEY_EMPTY_RANGE;
		/** Runtime state (NANITE_FIXUP_FLAG_INSTALLED); 0 on disk */
		uint32 Flags = 0;
		uint16 FirstPartFixup = 0;
		uint16 NumPartFixups = 0;
		uint16 FirstParentFixup = 0;
		uint16 NumParentFixups = 0;
	};
	static_assert(sizeof(FFixupGroup) == 16, "FFixupChunk::FGroupFixup out of sync with the engine");

	/** NaniteFixupChunk.h FFixupChunk::FPartFixup: a run of a page's clusters and the hierarchy slots pointing at it */
	struct FFixupPart
	{
		uint16 PageIndex = 0;
		uint8 StartClusterIndex = 0;
		uint8 LeafCounter = 0;
		uint32 FirstHierarchyLocation = 0;
		uint32 NumHierarchyLocations = 0;
	};
	static_assert(sizeof(FFixupPart) == 12, "FFixupChunk::FPartFixup out of sync with the engine");

	/** NaniteFixupChunk.h FFixupChunk::FParentFixup (none with root pages only; kept for the size check) */
	struct FFixupParent
	{
		uint16 PageIndex = 0;
		uint16 PartFixupPageIndex = 0;
		uint16 PartFixupIndex = 0;
		uint16 NumClusterIndices = 0;
		uint16 FirstClusterIndex = 0;
	};
	static_assert(sizeof(FFixupParent) == 10, "FFixupChunk::FParentFixup out of sync with the engine");

	/**
	 * NaniteFixupChunk.h FFixupChunk::FHierarchyLocation: a hierarchy node's child slot, ChildIndex in the low
	 * NANITE_MAX_BVH_NODE_FANOUT_BITS bits and NodeIndex above
	 *
	 * @param NodeIndex  The node
	 * @param ChildIndex The slot (0 to NANITE_MAX_BVH_NODE_FANOUT - 1)
	 * @return The packed location
	 */
	inline uint32 PackHierarchyLocation(uint32 NodeIndex, uint32 ChildIndex)
	{
		return (ChildIndex & NANITE_MAX_BVH_NODE_FANOUT_MASK) | (NodeIndex << NANITE_MAX_BVH_NODE_FANOUT_BITS);
	}

	/**
	 * NaniteFixupChunk.h FFixupChunk::GetSize: a fixup chunk's bytes
	 *
	 * @return The size of a chunk with these record counts
	 */
	constexpr uint32 GetFixupChunkSize(uint32 NumGroupFixups, uint32 NumPartFixups, uint32 NumParentFixups, uint32 NumHierarchyLocations,
		uint32 NumReconsiderPages, uint32 NumClusterIndices)
	{
		return sizeof(FFixupHeader) + NumGroupFixups * sizeof(FFixupGroup) + NumPartFixups * sizeof(FFixupPart)
			+ NumParentFixups * sizeof(FFixupParent) + NumHierarchyLocations * sizeof(uint32) + NumReconsiderPages * sizeof(uint16)
			+ NumClusterIndices * sizeof(uint8);
	}

	/** The engine's packed cluster, 8 float4s (NANITE_NUM_PACKED_CLUSTER_FLOAT4S), stored as columns in a page */
	static_assert(sizeof(Nanite::FPackedCluster) == NANITE_NUM_PACKED_CLUSTER_FLOAT4S * 16, "FPackedCluster changed size");
	/** The engine's hierarchy node: NANITE_HIERARCHY_NODE_SLICE_SIZE_DWORDS (60) for its NANITE_MAX_BVH_NODE_FANOUT slots */
	static_assert(sizeof(Nanite::FPackedHierarchyNode) == NANITE_HIERARCHY_NODE_SLICE_SIZE_DWORDS * 4, "FPackedHierarchyNode changed size");
}
