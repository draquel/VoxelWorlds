// Copyright Daniel Raquel. All Rights Reserved.

// UVoxelMapSubsystem's save data: the explored tiles and the edit overlays, encoded for the world save, a translation
// unit of the class.

#include "VoxelMapSubsystem.h"
#include "VoxelMap.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelBiomeRegistry.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelMaterialAtlas.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelCoordinates.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelTerrainConditioning.h"
#include "IVoxelWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelWorldModeFactory.h"
#include "VoxelEditManager.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "VoxelFarField.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "Misc/Base64.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "VoxelResidencyStats.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

FString UVoxelMapSubsystem::EncodeTileCoords(const TArray<FIntPoint>& Tiles)
{
	if (Tiles.Num() == 0)
	{
		return FString();
	}
	TArray<int32> Raw;
	Raw.Reserve(Tiles.Num() * 2);
	for (const FIntPoint& Tile : Tiles)
	{
		Raw.Add(Tile.X);
		Raw.Add(Tile.Y);
	}
	const int32 RawBytes = Raw.Num() * sizeof(int32);
	TArray<uint8> Compressed;
	int32 CompressedBytes = FCompression::CompressMemoryBound(NAME_Zlib, RawBytes);
	Compressed.SetNumUninitialized(CompressedBytes);
	if (!FCompression::CompressMemory(NAME_Zlib, Compressed.GetData(), CompressedBytes, Raw.GetData(), RawBytes))
	{
		UE_LOG(LogVoxelMap, Warning, TEXT("EncodeTileCoords: compressing %d tiles failed."), Tiles.Num());
		return FString();
	}
	Compressed.SetNum(CompressedBytes);
	return FBase64::Encode(Compressed);
}

bool UVoxelMapSubsystem::DecodeTileCoords(const FString& Encoded, int32 Count, TArray<FIntPoint>& OutTiles)
{
	OutTiles.Reset();
	if (Encoded.IsEmpty() || Count <= 0)
	{
		return Encoded.IsEmpty() && Count <= 0;
	}
	TArray<uint8> Compressed;
	if (!FBase64::Decode(Encoded, Compressed))
	{
		return false;
	}
	TArray<int32> Raw;
	Raw.SetNumUninitialized(Count * 2);
	if (!FCompression::UncompressMemory(NAME_Zlib, Raw.GetData(), Raw.Num() * sizeof(int32), Compressed.GetData(), Compressed.Num()))
	{
		return false;
	}
	OutTiles.Reserve(Count);
	for (int32 i = 0; i < Count; ++i)
	{
		OutTiles.Add(FIntPoint(Raw[i * 2], Raw[i * 2 + 1]));
	}
	return true;
}

FString UVoxelMapSubsystem::ExportExploredTiles(int32& OutCount) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelMap_ExportExploredTiles);
	TArray<FIntPoint> Tiles;
	Tiles.Reserve(ExploredTiles.Num());
	for (const uint64 Key : ExploredTiles)
	{
		Tiles.Add(UnpackTileKey(Key));
	}
	// Row-major order: neighbouring tiles become near-identical byte runs, which is what makes the blob small.
	Tiles.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.Y != B.Y ? A.Y < B.Y : A.X < B.X; });
	FString Encoded = EncodeTileCoords(Tiles);
	OutCount = Encoded.IsEmpty() ? 0 : Tiles.Num();
	return Encoded;
}

int32 UVoxelMapSubsystem::ImportExploredTiles(const FString& Encoded, int32 Count)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelMap_ImportExploredTiles);
	TArray<FIntPoint> Tiles;
	if (!DecodeTileCoords(Encoded, Count, Tiles))
	{
		UE_LOG(LogVoxelMap, Warning, TEXT("ImportExploredTiles: malformed explored-tile data (%d tiles claimed); map exploration not restored."), Count);
		return 0;
	}
	for (const FIntPoint& Tile : Tiles)
	{
		ExploredTiles.Add(PackTileKey(Tile));
	}

	// Explored tiles draw from the tile cache: queue the images (four run at a time; the rest wait in the queue).
	// Tiles outside the world (explored before exploration stopped at the edge, WM-I2) are dropped first; without a
	// resolved world they are dropped when it resolves.
	if (ResolveChunkManager())
	{
		PruneExploredOutsideWorld();
		for (const FIntPoint& Tile : Tiles)
		{
			const uint64 Key = PackTileKey(Tile);
			if (!ExploredTiles.Contains(Key))
			{
				continue;
			}
			// Restored tiles keep only their overview (WM-E2): the minimap asks for detail where the player is.
			if (!TileStore.Contains(Key) && !TileQueue.IsQueued(Key))
			{
				QueueTileGeneration(Tile);
			}
		}
	}
	else
	{
		bGenerateExploredWhenReady = true;
	}

	UE_LOG(LogVoxelMap, Log, TEXT("Restored %d explored map tile(s) (%d explored now)."), Tiles.Num(), ExploredTiles.Num());
	return Tiles.Num();
}

namespace
{
	/** 'VMEO': map edit overlays. */
	constexpr uint32 EditOverlayBlobMagic = 0x4F454D56;

	/** Bump when the blob layout around the per-overlay data changes. */
	constexpr uint8 EditOverlayBlobVersion = 1;

	/** Refuse to inflate a claimed payload beyond this (a corrupt size field). */
	constexpr uint32 MaxEditOverlayBlobBytes = 256u * 1024u * 1024u;
}

FString UVoxelMapSubsystem::EncodeEditOverlays(const TArray<FVoxelMapTileOverlay>& Overlays)
{
	if (Overlays.Num() == 0)
	{
		return FString();
	}
	TArray<uint8> Raw;
	{
		FMemoryWriter Writer(Raw);
		uint32 Magic = EditOverlayBlobMagic;
		uint8 Version = EditOverlayBlobVersion;
		int32 Count = Overlays.Num();
		Writer << Magic << Version << Count;
		for (const FVoxelMapTileOverlay& Entry : Overlays)
		{
			int32 X = Entry.Tile.X;
			int32 Y = Entry.Tile.Y;
			Writer << X << Y;
			FVoxelMapEditOverlay Copy = Entry.Overlay;
			Copy.Serialize(Writer);
		}
	}

	int32 CompressedBytes = FCompression::CompressMemoryBound(NAME_Zlib, Raw.Num());
	TArray<uint8> Compressed;
	Compressed.SetNumUninitialized(CompressedBytes);
	if (!FCompression::CompressMemory(NAME_Zlib, Compressed.GetData(), CompressedBytes, Raw.GetData(), Raw.Num()))
	{
		UE_LOG(LogVoxelMap, Warning, TEXT("EncodeEditOverlays: compressing %d tile overlay(s) failed."), Overlays.Num());
		return FString();
	}

	// The uncompressed size travels in front of the zlib stream: decoding needs it.
	TArray<uint8> Blob;
	{
		FMemoryWriter Writer(Blob);
		uint32 RawSize = static_cast<uint32>(Raw.Num());
		Writer << RawSize;
		Writer.Serialize(Compressed.GetData(), CompressedBytes);
	}
	return FBase64::Encode(Blob);
}

bool UVoxelMapSubsystem::DecodeEditOverlays(const FString& Encoded, TArray<FVoxelMapTileOverlay>& OutOverlays)
{
	OutOverlays.Reset();
	if (Encoded.IsEmpty())
	{
		return true;
	}
	TArray<uint8> Blob;
	if (!FBase64::Decode(Encoded, Blob) || Blob.Num() <= static_cast<int32>(sizeof(uint32)))
	{
		return false;
	}
	uint32 RawSize = 0;
	{
		FMemoryReader Header(Blob);
		Header << RawSize;
	}
	if (RawSize == 0 || RawSize > MaxEditOverlayBlobBytes)
	{
		return false;
	}
	TArray<uint8> Raw;
	Raw.SetNumUninitialized(RawSize);
	if (!FCompression::UncompressMemory(NAME_Zlib, Raw.GetData(), RawSize, Blob.GetData() + sizeof(uint32), Blob.Num() - sizeof(uint32)))
	{
		return false;
	}

	FMemoryReader Reader(Raw);
	uint32 Magic = 0;
	uint8 Version = 0;
	int32 Count = 0;
	Reader << Magic << Version << Count;
	// Each tile takes at least its coordinates plus an overlay header, so Count cannot exceed the payload.
	if (Reader.IsError() || Magic != EditOverlayBlobMagic || Version != EditOverlayBlobVersion || Count < 0
		|| Count > static_cast<int32>(RawSize / 8))
	{
		return false;
	}
	OutOverlays.Reserve(Count);
	for (int32 i = 0; i < Count; ++i)
	{
		FVoxelMapTileOverlay& Entry = OutOverlays.AddDefaulted_GetRef();
		Reader << Entry.Tile.X << Entry.Tile.Y;
		if (Reader.IsError() || !Entry.Overlay.Serialize(Reader))
		{
			OutOverlays.Reset();
			return false;
		}
	}
	return true;
}

FString UVoxelMapSubsystem::ExportEditOverlays(int32& OutCount) const
{
	TArray<FVoxelMapTileOverlay> Tiles;
	Tiles.Reserve(EditOverlays.Num());
	for (const TPair<uint64, TSharedPtr<const FVoxelMapEditOverlay>>& Pair : EditOverlays)
	{
		if (Pair.Value.IsValid() && !Pair.Value->IsEmpty())
		{
			FVoxelMapTileOverlay& Entry = Tiles.AddDefaulted_GetRef();
			Entry.Tile = UnpackTileKey(Pair.Key);
			Entry.Overlay = *Pair.Value;
		}
	}
	// Row-major order keeps the blob deterministic for an unchanged world.
	Tiles.Sort([](const FVoxelMapTileOverlay& A, const FVoxelMapTileOverlay& B)
	{
		return A.Tile.Y != B.Tile.Y ? A.Tile.Y < B.Tile.Y : A.Tile.X < B.Tile.X;
	});
	FString Encoded = EncodeEditOverlays(Tiles);
	OutCount = Encoded.IsEmpty() ? 0 : Tiles.Num();
	return Encoded;
}

int32 UVoxelMapSubsystem::ImportEditOverlays(const FString& Encoded)
{
	if (bShuttingDown)
	{
		return 0;
	}
	TArray<FVoxelMapTileOverlay> Tiles;
	if (!DecodeEditOverlays(Encoded, Tiles))
	{
		UE_LOG(LogVoxelMap, Warning, TEXT("ImportEditOverlays: malformed map edit overlay data; edits show on the map as their areas load."));
		Tiles.Reset();
	}

	// The save replaces the overlays. Every tile whose overlay changes is redrawn, and refreshed once the edits that
	// load next are in: rebuilt where its edited chunks are loaded, cleared where no edits remain, kept otherwise.
	TSet<uint64> Touched;
	for (const TPair<uint64, TSharedPtr<const FVoxelMapEditOverlay>>& Pair : EditOverlays)
	{
		Touched.Add(Pair.Key);
	}
	EditOverlays.Reset();
	int32 Restored = 0;
	for (FVoxelMapTileOverlay& Entry : Tiles)
	{
		const bool bResolutionMatches = !bCacheResolved || Entry.Overlay.Resolution == CachedChunkSize;
		const bool bKindMatches = !bCacheResolved || Entry.Overlay.bAltitudeHeights == TileFrame.bPlanet;
		if (Entry.Overlay.IsEmpty() || !bResolutionMatches || !bKindMatches)
		{
			continue;
		}
		const uint64 Key = PackTileKey(Entry.Tile);
		EditOverlays.Add(Key, MakeShared<const FVoxelMapEditOverlay>(MoveTemp(Entry.Overlay)));
		Touched.Add(Key);
		++Restored;
	}
	for (const uint64 Key : Touched)
	{
		MarkOverlayDirty(Key);
		if (bCacheResolved && (TileStore.Contains(Key) || TileQueue.IsQueued(Key)))
		{
			QueueTileGeneration(UnpackTileKey(Key), /*bInvalidate*/ true);
		}
	}
	UE_LOG(LogVoxelMap, Log, TEXT("Restored %d map edit overlay(s) (%d tile(s) refreshed)."), Restored, Touched.Num());
	return Restored;
}
