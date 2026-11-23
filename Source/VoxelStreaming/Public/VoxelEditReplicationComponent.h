// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "VoxelEditWire.h"
#include "VoxelEditReplicationComponent.generated.h"

class UVoxelEditManager;
class UVoxelChunkManager;
struct FVoxelReplicatedChange;

/**
 * What the server has sent one client of the edit layer (G273, D188), and the choice of what to send it next. Plain C++,
 * so the send rules are tested without a network: a live change goes as a delta for each chunk the client holds at the
 * change's previous revision; every other chunk whose revision the client lacks goes whole, nearest the client first.
 */
struct VOXELSTREAMING_API FVoxelEditSendState
{
	/** The revision of each chunk the client has been sent. */
	TMap<FIntVector, uint32> Sent;

	/**
	 * The deltas of a live change this client can take, as messages of at most MaxVoxelsPerMessage voxels; each chunk
	 * taken is recorded at its new revision. A chunk the client does not hold at the previous revision is left for the
	 * catch-up (PickCatchUp), which sends it whole.
	 * @param Change The server's change
	 * @param MaxVoxelsPerMessage The most voxels a message holds (at least 1)
	 * @param OutMessages Receives the messages (appended)
	 * @return The number of chunks taken as deltas
	 */
	int32 TakeLive(const FVoxelReplicatedChange& Change, int32 MaxVoxelsPerMessage, TArray<FVoxelEditWireMessage>& OutMessages);

	/**
	 * The whole chunks the client lacks, nearest first: up to MaxChunks chunks, packed into messages of at most
	 * MaxVoxelsPerMessage voxels (a chunk with more goes in parts). Each chunk picked is recorded at its revision. A chunk
	 * the client was never sent and that holds no replicated voxel is recorded without a message.
	 * @param Manager The server's edit manager
	 * @param ViewChunk Where the client is, in chunk units (the chunk grid's coordinates, fractional)
	 * @param MaxChunks The most chunks to pick
	 * @param MaxVoxelsPerMessage The most voxels a message holds (at least 1)
	 * @param OutMessages Receives the messages (appended)
	 * @return The number of chunks picked, sent or recorded
	 */
	int32 PickCatchUp(const UVoxelEditManager& Manager, const FVector& ViewChunk, int32 MaxChunks, int32 MaxVoxelsPerMessage,
		TArray<FVoxelEditWireMessage>& OutMessages);

	/**
	 * How many of the manager's chunks the client does not hold at their revision.
	 * @param Manager The server's edit manager
	 * @return The chunks behind
	 */
	int32 CountBehind(const UVoxelEditManager& Manager) const;
};

/**
 * Sends the server's voxel edits to one client and writes them into the client's edit layer (G273, D187-D191). The game
 * layer puts one on its player controller (VoxelWorlds knows no controller class). On the server, for a remote player's
 * controller: each replicated change goes to the client as it happens (a reliable client RPC per message, ordered with
 * the controller's other RPCs), and the chunks the client lacks (a late join, a restart, a chunk changed while it caught
 * up) go whole, nearest the player first, under voxel.Edit.Net.CatchUpKBps. On the client: each message is decoded and
 * written with UVoxelEditManager::ApplyReplicatedMessage, queued until the voxel world is up. A standalone game, a listen
 * host's own controller and a client's other copies do nothing.
 */
UCLASS(ClassGroup = (Voxel), meta = (BlueprintSpawnableComponent))
class VOXELSTREAMING_API UVoxelEditReplicationComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UVoxelEditReplicationComponent();

	//~ UActorComponent
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** @return The send state of the client this component serves (server side) */
	const FVoxelEditSendState& GetSendState() const { return SendState; }

	/**
	 * Server: whether this remote player's client holds every replicated chunk at the server's revision (its catch-up is
	 * done). What follows on the controller's channel then reaches a client whose edit layer is the server's (the map's
	 * overlays, G273).
	 * @return True when serving a remote client that is bound and has nothing behind
	 */
	bool IsClientInStep() const;

	/**
	 * One line on this component for voxel.Edit.Net: the side, the chunks in step and behind, the messages and bytes.
	 * @return The line
	 */
	FString DescribeForConsole() const;

protected:
	/**
	 * Client: an edit message from the server.
	 * @param Bytes The message, as FVoxelEditWire::Encode wrote it
	 */
	UFUNCTION(Client, Reliable)
	void Client_ReceiveEdits(const TArray<uint8>& Bytes);

private:
	/** Server: whether this controller is a remote player's (the only case that sends). */
	bool IsServingRemoteClient() const;

	/** Server: bind to the edit manager once the voxel world is up. @return True when bound */
	bool BindServer();

	/** Server: a replicated change on the server's edit manager. */
	void HandleReplicatedChange(const FVoxelReplicatedChange& Change);

	/** Server: send the chunks the client lacks, within the budget. */
	void SendCatchUp(float DeltaTime);

	/** Server: encode and send messages. @return The bytes sent */
	int32 SendMessages(const TArray<FVoxelEditWireMessage>& Messages);

	/** Client: write the queued messages once the voxel world is up. */
	void ApplyPending();

	/** Where the client's player is, in chunk units, for the catch-up's order. */
	FVector GetViewChunk(const UVoxelChunkManager& ChunkManager) const;

	FVoxelEditSendState SendState;

	TWeakObjectPtr<UVoxelEditManager> BoundManager;
	FDelegateHandle ChangeHandle;

	/** Client: messages received before the voxel world was up. */
	TArray<TArray<uint8>> PendingMessages;

	/** Server: the catch-up's budget, in bytes (refilled at voxel.Edit.Net.CatchUpKBps). */
	double CatchUpTokens = 0.0;

	/** The counters voxel.Edit.Net reports. */
	int32 MessagesSent = 0;
	int64 BytesSent = 0;
	int32 LiveChunksSent = 0;
	int32 WholeChunksSent = 0;
	int32 MessagesReceived = 0;
	int64 BytesReceived = 0;
	int32 VoxelsApplied = 0;
	int32 BadMessages = 0;
};
