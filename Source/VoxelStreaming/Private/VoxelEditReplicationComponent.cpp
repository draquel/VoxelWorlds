// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelEditReplicationComponent.h"
#include "VoxelChunkManager.h"
#include "VoxelEditManager.h"
#include "VoxelStreaming.h"
#include "VoxelWorldConfiguration.h"
#include "Engine/Engine.h"
#include "Engine/NetConnection.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectIterator.h"

static TAutoConsoleVariable<float> CVarVoxelEditNetCatchUpKBps(
	TEXT("voxel.Edit.Net.CatchUpKBps"),
	64.0f,
	TEXT("G273: how many KB a second the server sends each client of the chunks it lacks (a late join, a restart), nearest the ")
	TEXT("player first. A live edit goes at once whatever this is. 0 = no catch-up."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarVoxelEditNetMaxVoxels(
	TEXT("voxel.Edit.Net.MaxVoxelsPerMessage"),
	2048,
	TEXT("G273: the most voxels one edit message to a client holds (a few bytes each); a larger change or chunk goes in parts."),
	ECVF_Default);

namespace
{
	/** Append a chunk's voxels to the messages, in pieces of at most MaxVoxels, a new message when the last is full. */
	void PackChunk(const FVoxelEditWireChunk& Chunk, EEditSource Source, const FVector& Centre, float Radius, int32 MaxVoxels,
		TArray<FVoxelEditWireMessage>& OutMessages, int32& InOutLastVoxels)
	{
		auto NewMessage = [&]() -> FVoxelEditWireMessage&
		{
			FVoxelEditWireMessage& Message = OutMessages.AddDefaulted_GetRef();
			Message.Source = Source;
			Message.EditCenter = Centre;
			Message.EditRadius = Radius;
			InOutLastVoxels = 0;
			return Message;
		};
		if (OutMessages.Num() == 0)
		{
			NewMessage();
		}
		if (Chunk.Voxels.Num() == 0)
		{
			OutMessages.Last().Chunks.Add(Chunk); // an empty whole chunk: the client removes what it marked there
			return;
		}
		int32 Start = 0;
		bool bFirstPiece = true;
		while (Start < Chunk.Voxels.Num())
		{
			if (InOutLastVoxels >= MaxVoxels)
			{
				NewMessage();
			}
			const int32 Take = FMath::Min(Chunk.Voxels.Num() - Start, MaxVoxels - InOutLastVoxels);
			FVoxelEditWireChunk& Piece = OutMessages.Last().Chunks.AddDefaulted_GetRef();
			Piece.Coord = Chunk.Coord;
			// Only the first piece of a whole chunk clears the client's marks there; the rest add to it.
			Piece.Kind = (Chunk.Kind == EVoxelEditWireKind::FullFirst && !bFirstPiece) ? EVoxelEditWireKind::FullMore : Chunk.Kind;
			Piece.Voxels.Append(Chunk.Voxels.GetData() + Start, Take);
			Start += Take;
			InOutLastVoxels += Take;
			bFirstPiece = false;
		}
	}
}

// ==================== FVoxelEditSendState ====================

int32 FVoxelEditSendState::TakeLive(const FVoxelReplicatedChange& Change, int32 MaxVoxelsPerMessage, TArray<FVoxelEditWireMessage>& OutMessages)
{
	MaxVoxelsPerMessage = FMath::Max(MaxVoxelsPerMessage, 1);
	TArray<FVoxelEditWireMessage> Messages;
	int32 LastVoxels = 0;
	int32 Taken = 0;
	for (const FVoxelReplicatedChunkChange& Chunk : Change.Chunks)
	{
		// A chunk the client holds at another revision (or not at all, behind the change) is the catch-up's to send whole.
		if (Chunk.bResendWhole || Sent.FindRef(Chunk.Wire.Coord) != Chunk.PreviousRevision)
		{
			continue;
		}
		PackChunk(Chunk.Wire, Change.Source, Change.EditCenter, Change.EditRadius, MaxVoxelsPerMessage, Messages, LastVoxels);
		Sent.Add(Chunk.Wire.Coord, Chunk.Revision);
		++Taken;
	}
	OutMessages.Append(MoveTemp(Messages));
	return Taken;
}

int32 FVoxelEditSendState::PickCatchUp(const UVoxelEditManager& Manager, const FVector& ViewChunk, int32 MaxChunks,
	int32 MaxVoxelsPerMessage, TArray<FVoxelEditWireMessage>& OutMessages)
{
	MaxVoxelsPerMessage = FMath::Max(MaxVoxelsPerMessage, 1);
	struct FCandidate
	{
		double DistSq;
		FIntVector Coord;
		uint32 Revision;
	};
	TArray<FCandidate> Candidates;
	int32 Recorded = 0;
	for (const TPair<FIntVector, uint32>& Pair : Manager.GetReplicatedRevisions())
	{
		const uint32* Have = Sent.Find(Pair.Key);
		if (Have && *Have == Pair.Value)
		{
			continue;
		}
		const FChunkEditLayer* Layer = Manager.GetEditLayer(Pair.Key);
		if (!Have && !(Layer && Layer->HasReplicatedVoxels()))
		{
			// Never sent and nothing in it to send: the client holds it as it is.
			Sent.Add(Pair.Key, Pair.Value);
			++Recorded;
			continue;
		}
		const FVector Centre = FVector(Pair.Key) + FVector(0.5);
		Candidates.Add({ FVector::DistSquared(Centre, ViewChunk), Pair.Key, Pair.Value });
	}
	Candidates.Sort([](const FCandidate& A, const FCandidate& B) { return A.DistSq < B.DistSq; });

	TArray<FVoxelEditWireMessage> Messages;
	int32 LastVoxels = 0;
	TArray<FVoxelEditWireChunk> Parts;
	const int32 Count = FMath::Min(Candidates.Num(), FMath::Max(MaxChunks, 0));
	for (int32 I = 0; I < Count; ++I)
	{
		Manager.GatherReplicatedChunk(Candidates[I].Coord, MaxVoxelsPerMessage, Parts);
		for (const FVoxelEditWireChunk& Part : Parts)
		{
			PackChunk(Part, EEditSource::System, FVector::ZeroVector, 0.0f, MaxVoxelsPerMessage, Messages, LastVoxels);
		}
		Sent.Add(Candidates[I].Coord, Candidates[I].Revision);
	}
	OutMessages.Append(MoveTemp(Messages));
	return Recorded + Count;
}

int32 FVoxelEditSendState::CountBehind(const UVoxelEditManager& Manager) const
{
	int32 Behind = 0;
	for (const TPair<FIntVector, uint32>& Pair : Manager.GetReplicatedRevisions())
	{
		if (Sent.FindRef(Pair.Key) != Pair.Value)
		{
			++Behind;
		}
	}
	return Behind;
}

// ==================== UVoxelEditReplicationComponent ====================

UVoxelEditReplicationComponent::UVoxelEditReplicationComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.1f;
	SetIsReplicatedByDefault(true);
}

bool UVoxelEditReplicationComponent::IsServingRemoteClient() const
{
	const APlayerController* PC = Cast<APlayerController>(GetOwner());
	const UWorld* World = GetWorld();
	if (!PC || !World)
	{
		return false;
	}
	const ENetMode NetMode = World->GetNetMode();
	return (NetMode == NM_DedicatedServer || NetMode == NM_ListenServer) && !PC->IsLocalController() && PC->GetNetConnection() != nullptr;
}

bool UVoxelEditReplicationComponent::IsClientInStep() const
{
	const UVoxelEditManager* EditManager = BoundManager.Get();
	return EditManager && IsServingRemoteClient() && SendState.CountBehind(*EditManager) == 0;
}

bool UVoxelEditReplicationComponent::BindServer()
{
	UVoxelChunkManager* ChunkManager = UVoxelChunkManager::Find(GetWorld());
	UVoxelEditManager* EditManager = ChunkManager ? ChunkManager->GetEditManager() : nullptr;
	if (!EditManager || !EditManager->IsInitialized())
	{
		return false;
	}
	BoundManager = EditManager;
	ChangeHandle = EditManager->OnReplicatedChange.AddUObject(this, &UVoxelEditReplicationComponent::HandleReplicatedChange);
	UE_LOG(LogVoxelStreaming, Log, TEXT("[EditNet] serving %s: %d chunk(s) with replicated edits to catch up"),
		*GetNameSafe(GetOwner()), SendState.CountBehind(*EditManager));
	return true;
}

void UVoxelEditReplicationComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (World->GetNetMode() == NM_Client)
	{
		ApplyPending();
		return;
	}
	if (!IsServingRemoteClient())
	{
		return;
	}
	if (!BoundManager.IsValid() && !BindServer())
	{
		return;
	}
	SendCatchUp(DeltaTime);
}

void UVoxelEditReplicationComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UVoxelEditManager* EditManager = BoundManager.Get())
	{
		EditManager->OnReplicatedChange.Remove(ChangeHandle);
	}
	BoundManager.Reset();
	ChangeHandle.Reset();
	Super::EndPlay(EndPlayReason);
}

void UVoxelEditReplicationComponent::HandleReplicatedChange(const FVoxelReplicatedChange& Change)
{
	if (!IsServingRemoteClient())
	{
		return;
	}
	TArray<FVoxelEditWireMessage> Messages;
	LiveChunksSent += SendState.TakeLive(Change, CVarVoxelEditNetMaxVoxels.GetValueOnGameThread(), Messages);
	SendMessages(Messages);
}

void UVoxelEditReplicationComponent::SendCatchUp(float DeltaTime)
{
	UVoxelEditManager* EditManager = BoundManager.Get();
	const UVoxelChunkManager* ChunkManager = UVoxelChunkManager::Find(GetWorld());
	const APlayerController* PC = Cast<APlayerController>(GetOwner());
	if (!EditManager || !ChunkManager || !PC)
	{
		return;
	}
	const double Rate = FMath::Max(CVarVoxelEditNetCatchUpKBps.GetValueOnGameThread(), 0.0f) * 1024.0;
	if (Rate <= 0.0)
	{
		return;
	}
	// A second's worth at most, so a client idle for a while does not get a burst.
	CatchUpTokens = FMath::Min(CatchUpTokens + Rate * DeltaTime, Rate);
	UNetConnection* Connection = PC->GetNetConnection();
	if (CatchUpTokens <= 0.0 || (Connection && !Connection->IsNetReady()))
	{
		return; // over the budget, or the connection is saturated: the next tick goes on
	}
	const FVector ViewChunk = GetViewChunk(*ChunkManager);
	const int32 MaxVoxels = CVarVoxelEditNetMaxVoxels.GetValueOnGameThread();
	for (int32 Round = 0; Round < 16 && CatchUpTokens > 0.0; ++Round)
	{
		TArray<FVoxelEditWireMessage> Messages;
		const int32 Picked = SendState.PickCatchUp(*EditManager, ViewChunk, /*MaxChunks=*/4, MaxVoxels, Messages);
		if (Picked == 0)
		{
			break;
		}
		for (const FVoxelEditWireMessage& Message : Messages)
		{
			WholeChunksSent += Message.Chunks.Num();
		}
		CatchUpTokens -= SendMessages(Messages);
	}
}

int32 UVoxelEditReplicationComponent::SendMessages(const TArray<FVoxelEditWireMessage>& Messages)
{
	int32 Bytes = 0;
	TArray<uint8> Encoded;
	for (const FVoxelEditWireMessage& Message : Messages)
	{
		if (Message.Chunks.Num() == 0)
		{
			continue;
		}
		FVoxelEditWire::Encode(Message, Encoded);
		Client_ReceiveEdits(Encoded);
		Bytes += Encoded.Num();
		++MessagesSent;
	}
	BytesSent += Bytes;
	return Bytes;
}

void UVoxelEditReplicationComponent::Client_ReceiveEdits_Implementation(const TArray<uint8>& Bytes)
{
	++MessagesReceived;
	BytesReceived += Bytes.Num();
	PendingMessages.Add(Bytes);
	ApplyPending();
}

void UVoxelEditReplicationComponent::ApplyPending()
{
	if (PendingMessages.Num() == 0)
	{
		return;
	}
	UVoxelChunkManager* ChunkManager = UVoxelChunkManager::Find(GetWorld());
	UVoxelEditManager* EditManager = ChunkManager ? ChunkManager->GetEditManager() : nullptr;
	const UVoxelWorldConfiguration* Config = ChunkManager ? ChunkManager->GetConfiguration() : nullptr;
	if (!EditManager || !EditManager->IsInitialized() || !Config)
	{
		return; // the voxel world is not up yet: the next tick tries again
	}
	FVoxelEditWireMessage Message;
	for (const TArray<uint8>& Bytes : PendingMessages)
	{
		if (!FVoxelEditWire::Decode(Bytes, Config->ChunkSize, Message))
		{
			++BadMessages;
			UE_LOG(LogVoxelStreaming, Warning, TEXT("[EditNet] an edit message from the server (%d bytes) did not decode; dropped"), Bytes.Num());
			continue;
		}
		VoxelsApplied += EditManager->ApplyReplicatedMessage(Message);
	}
	PendingMessages.Reset();
}

FVector UVoxelEditReplicationComponent::GetViewChunk(const UVoxelChunkManager& ChunkManager) const
{
	const APlayerController* PC = Cast<APlayerController>(GetOwner());
	const UVoxelWorldConfiguration* Config = ChunkManager.GetConfiguration();
	if (!PC || !Config)
	{
		return FVector::ZeroVector;
	}
	const APawn* Pawn = PC->GetPawn();
	const FVector Location = Pawn ? Pawn->GetActorLocation() : PC->GetFocalLocation();
	const double ChunkWorldSize = static_cast<double>(Config->ChunkSize) * Config->VoxelSize;
	return ChunkWorldSize > 0.0 ? (Location - Config->WorldOrigin) / ChunkWorldSize : FVector::ZeroVector;
}

FString UVoxelEditReplicationComponent::DescribeForConsole() const
{
	const UWorld* World = GetWorld();
	const bool bClient = World && World->GetNetMode() == NM_Client;
	if (bClient)
	{
		return FString::Printf(TEXT("side=client owner=%s received=%d msgs %lld bytes applied=%d voxels pending=%d bad=%d"),
			*GetNameSafe(GetOwner()), MessagesReceived, BytesReceived, VoxelsApplied, PendingMessages.Num(), BadMessages);
	}
	const UVoxelEditManager* EditManager = BoundManager.Get();
	return FString::Printf(TEXT("side=server owner=%s serving=%d bound=%d chunks=%d insync=%d behind=%d sent=%d msgs %lld bytes live=%d whole=%d"),
		*GetNameSafe(GetOwner()), IsServingRemoteClient() ? 1 : 0, EditManager ? 1 : 0,
		EditManager ? EditManager->GetReplicatedRevisions().Num() : 0,
		EditManager ? EditManager->GetReplicatedRevisions().Num() - SendState.CountBehind(*EditManager) : 0,
		EditManager ? SendState.CountBehind(*EditManager) : 0, MessagesSent, BytesSent, LiveChunksSent, WholeChunksSent);
}

// Console: voxel.Edit.Net [X,Y,Z ...]. Every play world's edit replication (G273): each world's replicated chunks and
// edits, and each controller's component, the server's side (chunks in step, behind, what it sent) and the client's (what
// it received and wrote); with places, each world's edit-merged voxel at each (material and density), so a client's
// terrain can be compared with the server's.
static FAutoConsoleCommand GVoxelEditNetCommand(
	TEXT("voxel.Edit.Net"),
	TEXT("G273: log each play world's edit replication: per world the replicated chunks and edits, per player controller what ")
	TEXT("the server sent that client (chunks in step, behind, messages, bytes) and what a client received and wrote. ")
	TEXT("voxel.Edit.Net X,Y,Z ... adds each world's voxel at each place (place<n>=material/density)."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		TArray<FVector> Places;
		for (const FString& Arg : Args)
		{
			TArray<FString> Parts;
			if (Arg.ParseIntoArray(Parts, TEXT(","), true) == 3)
			{
				Places.Emplace(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2]));
			}
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (!World || !World->IsGameWorld())
			{
				continue;
			}
			const ENetMode NetMode = World->GetNetMode();
			const TCHAR* Net = NetMode == NM_Client ? TEXT("Client") : NetMode == NM_DedicatedServer ? TEXT("Server")
				: NetMode == NM_ListenServer ? TEXT("Listen") : TEXT("Standalone");
			const UVoxelChunkManager* ChunkManager = UVoxelChunkManager::Find(World);
			const UVoxelEditManager* EditManager = ChunkManager ? ChunkManager->GetEditManager() : nullptr;
			// The voxels a replicated edit changed, over the chunks with edits (the server's marks, or what a client was sent).
			int32 Marked = 0;
			int32 MarkedChunks = 0;
			if (EditManager)
			{
				TArray<FIntVector> Chunks;
				EditManager->GetEditedChunkCoords(Chunks);
				for (const FIntVector& Coord : Chunks)
				{
					const FChunkEditLayer* Layer = EditManager->GetEditLayer(Coord);
					Marked += Layer ? Layer->ReplicatedVoxels.Num() : 0;
					MarkedChunks += (Layer && Layer->HasReplicatedVoxels()) ? 1 : 0;
				}
			}
			FString PlaceVoxels;
			for (int32 Index = 0; Index < Places.Num(); ++Index)
			{
				const FVoxelData Voxel = ChunkManager ? ChunkManager->GetVoxelAt(Places[Index]) : FVoxelData();
				PlaceVoxels += FString::Printf(TEXT(" place%d=%d/%d"), Index + 1, Voxel.MaterialID, Voxel.Density);
			}
			UE_LOG(LogVoxelStreaming, Log, TEXT("[EditNet] world=%d net=%s edits=%d markedchunks=%d marked=%d%s"),
				Context.PIEInstance, Net, EditManager ? EditManager->GetTotalEditCount() : -1, MarkedChunks, Marked, *PlaceVoxels);
			for (TObjectIterator<UVoxelEditReplicationComponent> It; It; ++It)
			{
				if (It->GetWorld() == World)
				{
					UE_LOG(LogVoxelStreaming, Log, TEXT("[EditNet] world=%d net=%s %s"), Context.PIEInstance, Net, *It->DescribeForConsole());
				}
			}
		}
	}));
