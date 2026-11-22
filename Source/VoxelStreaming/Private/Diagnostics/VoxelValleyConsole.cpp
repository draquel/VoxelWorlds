// Copyright Daniel Raquel. All Rights Reserved.

// voxel.World.Valleys.Survey: the valley network over a window of every play world that has valleys
// (Documentation/VALLEYS_PLAN.md, section 6.1; the spike's measures, kept as a diagnostic).

#include "VoxelChunkManager.h"
#include "VoxelStreaming.h"
#include "Diagnostics/VoxelConsoleHelpers.h"
#include "VoxelValleyNetwork.h"
#include "VoxelWorldConfiguration.h"
#include "Async/ParallelFor.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if !UE_BUILD_SHIPPING

namespace VoxelValleyConsole
{
	/** The network over a window: its counts, the valley length, the longest valley, the cost of a tile; logged and written to Saved/Valleys/. */
	void Survey(const UVoxelWorldConfiguration& Config, const FVector2D& Centre, double Half, const FString& Tag)
	{
		const TSharedPtr<FVoxelValleyNetwork> Network = FVoxelValleyNetwork::Create(Config);
		if (!Network.IsValid())
		{
			UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.World.Valleys.Survey [%s]: no valleys (no valley configuration, or not a smooth plane or island)"), *Config.GetName());
			return;
		}
		const FBox2D Window(Centre - FVector2D(Half), Centre + FVector2D(Half));
		const FIntPoint Low = Network->TileOf(Network->CellOf(Window.Min));
		const FIntPoint High = Network->TileOf(Network->CellOf(Window.Max));
		TArray<FIntPoint> TileCoords;
		for (int32 TY = Low.Y; TY <= High.Y; ++TY)
		{
			for (int32 TX = Low.X; TX <= High.X; ++TX)
			{
				TileCoords.Add(FIntPoint(TX, TY));
			}
		}
		// The tiles, built on the workers (the network keeps them), and the cost of one from nothing on this thread
		const double T0 = FPlatformTime::Seconds();
		ParallelFor(TileCoords.Num(), [&](int32 Index) { Network->GetTile(TileCoords[Index]); });
		const double BuildSeconds = FPlatformTime::Seconds() - T0;
		const double T1 = FPlatformTime::Seconds();
		FVoxelValleyNetwork::BuildTile(Network->GetSettings(), Network->GetGround(), TileCoords[0]);
		const double TileMs = (FPlatformTime::Seconds() - T1) * 1.0e3;

		int32 Land = 0, Sea = 0, Sinks = 0, SinksLeft = 0, BreachSteps = 0, Links = 0, Heads = 0, FloorRises = 0;
		double ValleyLength = 0.0;
		TMap<FIntPoint, double> LinkLength;
		for (const FIntPoint& Coord : TileCoords)
		{
			const TSharedRef<const FVoxelValleyTile> Tile = Network->GetTile(Coord);
			for (const FVoxelValleySegment& Segment : Tile->Segments)
			{
				LinkLength.FindOrAdd(Segment.Link) += FVector2D::Distance(Segment.A, Segment.B);
				FloorRises += Segment.FloorB > Segment.FloorA + 0.01 ? 1 : 0;
			}
			for (const FVoxelValleyNode& Node : Tile->Nodes)
			{
				if (!Window.IsInside(Node.Position))
				{
					continue;
				}
				Node.bSea ? ++Sea : ++Land;
				Sinks += Node.bSink ? 1 : 0;
				SinksLeft += (Node.bSink && !Node.bHasLink) ? 1 : 0;
				BreachSteps += Node.bBreach ? 1 : 0;
				if (Node.bValley)
				{
					++Links;
					Heads += Node.bHead ? 1 : 0;
					ValleyLength += LinkLength.FindRef(Node.Cell);
				}
			}
		}
		// The longest valley: from each head in the window down its valley links (across tiles) to its end
		double Longest = 0.0;
		FVector2D LongestHead = FVector2D::ZeroVector;
		int32 ToSea = 0, ToSink = 0;
		for (const FIntPoint& Coord : TileCoords)
		{
			for (const FVoxelValleyNode& Head : Network->GetTile(Coord)->Nodes)
			{
				if (!Head.bHead || !Window.IsInside(Head.Position))
				{
					continue;
				}
				double Length = 0.0;
				FVoxelValleyNode Node = Head;
				for (int32 Guard = 0; Node.bValley && Guard < 4096; ++Guard)
				{
					const double* Known = LinkLength.Find(Node.Cell);
					Length += Known ? *Known : FVector2D::Distance(Node.Position, Network->GetNode(Node.Down).Position);
					Node = Network->GetNode(Node.Down);
				}
				Node.bSea ? ++ToSea : ++ToSink;
				if (Length > Longest)
				{
					Longest = Length;
					LongestHead = Head.Position;
				}
			}
		}

		const FString Json = FString::Printf(
			TEXT("{\n \"world\": \"%s\", \"tag\": \"%s\", \"version\": %d, \"settings\": \"%s\", \"hash\": \"%08X\",\n \"centre\": [%.0f, %.0f], \"half\": %.0f, \"tiles\": %d, \"build_ms\": %.1f, \"tile_ms\": %.2f,\n")
			TEXT(" \"nodes_land\": %d, \"nodes_sea\": %d, \"sinks\": %d, \"sinks_left\": %d, \"breach_steps\": %d, \"valley_links\": %d, \"heads\": %d, \"valley_km\": %.2f,\n")
			TEXT(" \"heads_to_sea\": %d, \"heads_to_sink\": %d, \"longest_km\": %.2f, \"longest_head\": [%.0f, %.0f], \"floor_rises\": %d\n}\n"),
			*Config.GetName(), *Tag, FVoxelValleyNetwork::Version, *Network->GetSettings().DescribeGeneration(), Network->GetSettings().GetGenerationHash(),
			Centre.X, Centre.Y, Half, TileCoords.Num(), BuildSeconds * 1.0e3, TileMs,
			Land, Sea, Sinks, SinksLeft, BreachSteps, Links, Heads, ValleyLength / 1.0e5, ToSea, ToSink, Longest / 1.0e5, LongestHead.X, LongestHead.Y, FloorRises);
		const FString Folder = FPaths::ProjectSavedDir() / TEXT("Valleys");
		IFileManager::Get().MakeDirectory(*Folder, true);
		const FString Path = Folder / FString::Printf(TEXT("%s_%s.json"), *Config.GetName(), *Tag);
		FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		UE_LOG(LogVoxelStreaming, Log,
			TEXT("voxel.World.Valleys.Survey [%s]: (%.0f, %.0f) +- %.0f uu, %d tiles in %.0f ms (one tile %.2f ms): %d land nodes, %d sea; %d sinks (%d left), %d breach steps; %d valley links, %d heads (%d to the sea, %d to a sink), %.1f km of valley, the longest %.2f km; floor rises %d -> %s"),
			*Config.GetName(), Centre.X, Centre.Y, Half, TileCoords.Num(), BuildSeconds * 1.0e3, TileMs, Land, Sea, Sinks, SinksLeft, BreachSteps, Links, Heads, ToSea, ToSink,
			ValleyLength / 1.0e5, Longest / 1.0e5, FloorRises, *Path);
	}
}

// Console: voxel.World.Valleys.Survey [x y | pawn] [half] [tag]
static FAutoConsoleCommandWithWorldAndArgs GVoxelValleySurveyCmd(
	TEXT("voxel.World.Valleys.Survey"),
	TEXT("The valley network over a window of every play world that has valleys (the valleys plan, section 6.1): voxel.World.Valleys.Survey [x y | pawn] [half=1200000] [tag]. ")
	TEXT("Builds the window's tiles and logs the nodes, the sinks and breaches, the valley links and their length, the longest valley and the cost of a tile; ")
	TEXT("the numbers go to Saved/Valleys/<configuration>_<tag>.json."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		UWorld* TargetWorld = VoxelEditAuditHelpers::FindGameWorld(World);
		if (!TargetWorld)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.World.Valleys.Survey: no PIE/Game world found (start PIE first)"));
			return;
		}
		FVector2D Centre = FVector2D::ZeroVector;
		int32 Next = 0;
		if (Args.Num() >= 1 && Args[0].Equals(TEXT("pawn"), ESearchCase::IgnoreCase))
		{
			const APlayerController* PC = TargetWorld->GetFirstPlayerController();
			if (PC && PC->GetPawn())
			{
				Centre = FVector2D(PC->GetPawn()->GetActorLocation());
			}
			Next = 1;
		}
		else if (Args.Num() >= 2)
		{
			Centre = FVector2D(FCString::Atod(*Args[0]), FCString::Atod(*Args[1]));
			Next = 2;
		}
		const double Half = Args.Num() > Next ? FMath::Max(1000.0, FCString::Atod(*Args[Next])) : 1200000.0;
		const FString Tag = Args.Num() > Next + 1 ? Args[Next + 1] : TEXT("survey");
		for (UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(TargetWorld))
		{
			if (CM && CM->GetConfiguration())
			{
				VoxelValleyConsole::Survey(*CM->GetConfiguration(), Centre, Half, Tag);
			}
		}
	}));

#endif // !UE_BUILD_SHIPPING
