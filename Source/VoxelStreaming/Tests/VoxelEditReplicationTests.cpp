// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Serialization/MemoryWriter.h"
#include "VoxelEditManager.h"
#include "VoxelEditReplicationComponent.h"
#include "VoxelEditWire.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G273 (Documentation/EDIT_REPLICATION_PLAN.md, D187-D191). Voxel edits replicated from the server to its clients: what
// travels is each changed voxel's edit, not the brush, so a client's edit layer built from the messages alone equals the
// server's. These tests run the server's edit manager, the per-client send rules (FVoxelEditSendState) and a client's edit
// manager in one process, every message through FVoxelEditWire's bytes, with no network:
//
// - Edit.Replication.Wire: every edit mode, a removal and a chunk in parts survive the bytes; cut, foreign or bad bytes
//   are refused.
// - Edit.Replication.Live.Plane / .Planet: digs and placements (a smooth density sphere across a chunk corner, a cubic
//   block's cube, a Set, a Paint), a fill cancelling a dig (the edit removed), an undo and a redo, each sent as it
//   happens: after each the client's layer equals the server's.
// - Edit.Replication.CatchUp: a client that joins after the edits (a late join, a restart) gets every chunk whole, nearest
//   first, a chunk too big for one message in parts, and a live change to a chunk it has not been sent yet waits for the
//   catch-up; a cleared chunk empties on the client.
// - Edit.Replication.LocalStamp: a Local operation (a dungeon stamp every machine makes) never overwrites a voxel a
//   replicated one changed, on the server or on a client, whichever came first; it is not sent.
// - Edit.Replication.SaveMarks: the edits file (version 4) keeps the replicated voxels, a load sends them to the clients;
//   a version 3 file loads with none marked.
// ---------------------------------------------------------------------------

namespace VoxelEditReplicationTestUtils
{
	constexpr int32 ChunkSize = 16;
	constexpr float VoxelSize = 100.0f;

	UVoxelEditManager* MakeManager(bool bPlanet)
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->ChunkSize = ChunkSize;
		Config->VoxelSize = VoxelSize;
		Config->WorldOrigin = bPlanet ? FVector(1000.0, -2000.0, 500.0) : FVector::ZeroVector;
		Config->WorldMode = bPlanet ? EWorldMode::SphericalPlanet : EWorldMode::InfinitePlane;
		UVoxelEditManager* Manager = NewObject<UVoxelEditManager>();
		Manager->Initialize(Config);
		Manager->SetEditSource(EEditSource::Player);
		return Manager;
	}

	FVector VoxelCentre(const UVoxelEditManager& Manager, int32 X, int32 Y, int32 Z, bool bPlanet)
	{
		return (bPlanet ? FVector(1000.0, -2000.0, 500.0) : FVector::ZeroVector) + (FVector(X, Y, Z) + 0.5) * VoxelSize;
	}

	/** Every chunk's edits (mode, delta, material, voxel) and marks, keyed by chunk and index, as text for a diff. */
	TMap<FString, FString> Snapshot(const UVoxelEditManager& Manager, bool bMarkedOnly = false)
	{
		TMap<FString, FString> Out;
		TArray<FIntVector> Chunks;
		Manager.GetEditedChunkCoords(Chunks);
		for (const FIntVector& Chunk : Chunks)
		{
			const FChunkEditLayer* Layer = Manager.GetEditLayer(Chunk);
			for (const TPair<int32, FVoxelEdit>& Pair : Layer->Edits)
			{
				if (bMarkedOnly && !Layer->IsReplicatedVoxel(Pair.Key))
				{
					continue; // a Local stamp, which each machine makes for itself
				}
				const FVoxelEdit& E = Pair.Value;
				Out.Add(FString::Printf(TEXT("%s#%d"), *Chunk.ToString(), Pair.Key),
					FString::Printf(TEXT("m%d d%d b%d v%d/%d/%d/%d%s"), (int32)E.EditMode, E.DensityDelta, E.BrushMaterialID,
						E.NewData.MaterialID, E.NewData.Density, E.NewData.BiomeID, E.NewData.Metadata,
						Layer->IsReplicatedVoxel(Pair.Key) ? TEXT(" R") : TEXT("")));
			}
		}
		return Out;
	}

	/** The marked voxels with no edit (a removal the server sent), which Snapshot does not list. */
	int32 MarkedWithoutEdit(const UVoxelEditManager& Manager, const TArray<FIntVector>& Chunks)
	{
		int32 Count = 0;
		for (const FIntVector& Chunk : Chunks)
		{
			if (const FChunkEditLayer* Layer = Manager.GetEditLayer(Chunk))
			{
				for (const int32 Index : Layer->ReplicatedVoxels)
				{
					Count += Layer->Edits.Contains(Index) ? 0 : 1;
				}
			}
		}
		return Count;
	}

	/** The client's layer against the server's; bMarkedOnly leaves out the server's Local stamps the client did not make. */
	bool SameLayers(FAutomationTestBase& Test, const TCHAR* What, const UVoxelEditManager& Server, const UVoxelEditManager& Client,
		bool bMarkedOnly = false)
	{
		const TMap<FString, FString> A = Snapshot(Server, bMarkedOnly);
		const TMap<FString, FString> B = Snapshot(Client, bMarkedOnly);
		bool bSame = A.Num() == B.Num();
		for (const TPair<FString, FString>& Pair : A)
		{
			const FString* Other = B.Find(Pair.Key);
			if (!Other || *Other != Pair.Value)
			{
				Test.AddError(FString::Printf(TEXT("%s: voxel %s is '%s' on the server and '%s' on the client"), What, *Pair.Key,
					*Pair.Value, Other ? **Other : TEXT("(none)")));
				bSame = false;
				break;
			}
		}
		if (A.Num() != B.Num())
		{
			Test.AddError(FString::Printf(TEXT("%s: %d edits on the server, %d on the client"), What, A.Num(), B.Num()));
		}
		return bSame;
	}

	/** One client: its send state on the server, its edit manager, and the messages on their way. */
	struct FTestClient
	{
		FVoxelEditSendState State;
		UVoxelEditManager* Manager = nullptr;
		TArray<FVoxelEditWireMessage> InFlight;
		int32 MaxVoxels = 2048;
		int32 Messages = 0;

		/** Each message through the bytes into the client's layer. */
		bool Deliver(FAutomationTestBase& Test)
		{
			bool bOk = true;
			for (const FVoxelEditWireMessage& Message : InFlight)
			{
				TArray<uint8> Bytes;
				FVoxelEditWire::Encode(Message, Bytes);
				FVoxelEditWireMessage Decoded;
				if (!FVoxelEditWire::Decode(Bytes, ChunkSize, Decoded))
				{
					Test.AddError(TEXT("A message did not decode"));
					bOk = false;
					continue;
				}
				Manager->ApplyReplicatedMessage(Decoded);
				++Messages;
			}
			InFlight.Reset();
			return bOk;
		}

		/** Catch up until the server has nothing more for this client. @return The rounds taken */
		int32 CatchUp(FAutomationTestBase& Test, const UVoxelEditManager& Server, const FVector& ViewChunk, int32 ChunksPerRound = 2)
		{
			int32 Rounds = 0;
			while (State.PickCatchUp(Server, ViewChunk, ChunksPerRound, MaxVoxels, InFlight) > 0 && Rounds < 1000)
			{
				Deliver(Test);
				++Rounds;
			}
			return Rounds;
		}
	};

	FDelegateHandle Listen(UVoxelEditManager& Server, FTestClient& Client)
	{
		return Server.OnReplicatedChange.AddLambda([&Client](const FVoxelReplicatedChange& Change)
		{
			Client.State.TakeLive(Change, Client.MaxVoxels, Client.InFlight);
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEditReplicationWireTest,
	"VoxelWorlds.Edit.Replication.Wire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelEditReplicationWireTest::RunTest(const FString& Parameters)
{
	using namespace VoxelEditReplicationTestUtils; // in the function: a file-scope directive leaks into the unity blob (G251)
	FVoxelEditWireMessage Message;
	Message.Source = EEditSource::Player;
	Message.EditCenter = FVector(123456.5, -98765.25, 4321.0);
	Message.EditRadius = 150.0f;
	FVoxelEditWireChunk& A = Message.Chunks.AddDefaulted_GetRef();
	A.Coord = FIntVector(-3, 7, 120);
	A.Kind = EVoxelEditWireKind::Delta;
	const EEditMode Modes[] = { EEditMode::Set, EEditMode::Add, EEditMode::Subtract, EEditMode::Paint };
	for (int32 I = 0; I < 4; ++I)
	{
		FVoxelEditWireVoxel& V = A.Voxels.AddDefaulted_GetRef();
		V.Index = I * 997 % (ChunkSize * ChunkSize * ChunkSize);
		V.bHasEdit = true;
		V.Edit.EditMode = Modes[I];
		V.Edit.DensityDelta = I == 2 ? 400 : 80 * I; // an accumulated dig passes 255
		V.Edit.BrushMaterialID = 3 + I;
		V.Edit.NewData = FVoxelData(7, 200 - I, 2, 9);
	}
	FVoxelEditWireVoxel& Removed = A.Voxels.AddDefaulted_GetRef();
	Removed.Index = ChunkSize * ChunkSize * ChunkSize - 1;
	Removed.bHasEdit = false;
	FVoxelEditWireChunk& B = Message.Chunks.AddDefaulted_GetRef();
	B.Coord = FIntVector(0, 0, 0);
	B.Kind = EVoxelEditWireKind::FullFirst;
	FVoxelEditWireChunk& C = Message.Chunks.AddDefaulted_GetRef();
	C.Coord = FIntVector(1, 0, 0);
	C.Kind = EVoxelEditWireKind::FullMore;
	for (int32 I = 0; I < 3000; ++I) // big enough to compress
	{
		FVoxelEditWireVoxel& V = C.Voxels.AddDefaulted_GetRef();
		V.Index = I;
		V.bHasEdit = true;
		V.Edit.EditMode = EEditMode::Subtract;
		V.Edit.DensityDelta = 80;
	}

	TArray<uint8> Bytes;
	FVoxelEditWire::Encode(Message, Bytes);
	FVoxelEditWireMessage Back;
	TestTrue(TEXT("Decodes"), FVoxelEditWire::Decode(Bytes, ChunkSize, Back));
	TestTrue(TEXT("Compressed: under 3 bytes a voxel"), Bytes.Num() < 3 * Message.GetVoxelCount());
	TestEqual(TEXT("Source"), (int32)Back.Source, (int32)Message.Source);
	TestTrue(TEXT("Centre within a float's precision"), Back.EditCenter.Equals(Message.EditCenter, 0.1));
	TestEqual(TEXT("Radius"), Back.EditRadius, Message.EditRadius);
	TestEqual(TEXT("Chunks"), Back.Chunks.Num(), 3);
	if (Back.Chunks.Num() == 3)
	{
		TestEqual(TEXT("Coord"), Back.Chunks[0].Coord, A.Coord);
		TestEqual(TEXT("Kinds"), (int32)Back.Chunks[1].Kind + 10 * (int32)Back.Chunks[2].Kind, 1 + 20);
		TestEqual(TEXT("An empty whole chunk stays empty"), Back.Chunks[1].Voxels.Num(), 0);
		TestEqual(TEXT("Voxels"), Back.Chunks[0].Voxels.Num(), 5);
		for (int32 I = 0; I < 4 && Back.Chunks[0].Voxels.Num() == 5; ++I)
		{
			const FVoxelEditWireVoxel& V = Back.Chunks[0].Voxels[I];
			const FVoxelEditWireVoxel& W = A.Voxels[I];
			TestTrue(FString::Printf(TEXT("Voxel %d"), I), V.Index == W.Index && V.bHasEdit && V.Edit.EditMode == W.Edit.EditMode
				&& V.Edit.DensityDelta == W.Edit.DensityDelta && V.Edit.BrushMaterialID == W.Edit.BrushMaterialID
				&& V.Edit.NewData.MaterialID == 7 && V.Edit.NewData.Density == 200 - I && V.Edit.NewData.BiomeID == 2
				&& V.Edit.NewData.Metadata == 9);
		}
		if (Back.Chunks[0].Voxels.Num() == 5)
		{
			TestFalse(TEXT("The removal has no edit"), Back.Chunks[0].Voxels[4].bHasEdit);
			const FIntVector Local = Back.Chunks[0].Voxels[0].Edit.LocalPosition;
			TestEqual(TEXT("Local position from the index"), Local.X + Local.Y * ChunkSize + Local.Z * ChunkSize * ChunkSize, A.Voxels[0].Index);
		}
	}

	// Refused: cut bytes, another version, an index outside the chunk.
	TArray<uint8> Cut = Bytes;
	Cut.SetNum(Bytes.Num() / 2);
	TestFalse(TEXT("Cut bytes are refused"), FVoxelEditWire::Decode(Cut, ChunkSize, Back));
	TArray<uint8> Foreign = Bytes;
	Foreign[0] = FVoxelEditWire::Version + 1;
	TestFalse(TEXT("Another version is refused"), FVoxelEditWire::Decode(Foreign, ChunkSize, Back));
	FVoxelEditWireMessage Small;
	Small.Chunks.AddDefaulted_GetRef().Voxels.AddDefaulted_GetRef().Index = ChunkSize * ChunkSize * ChunkSize;
	FVoxelEditWire::Encode(Small, Bytes);
	TestFalse(TEXT("An index outside the chunk is refused"), FVoxelEditWire::Decode(Bytes, ChunkSize, Back));
	TestFalse(TEXT("Empty bytes are refused"), FVoxelEditWire::Decode(TArray<uint8>(), ChunkSize, Back));
	return true;
}

static void RunLiveCase(FAutomationTestBase& Test, bool bPlanet)
{
	using namespace VoxelEditReplicationTestUtils; // in the function: a file-scope directive leaks into the unity blob (G251)
	UVoxelEditManager* Server = MakeManager(bPlanet);
	FTestClient Client;
	Client.Manager = MakeManager(bPlanet);
	Listen(*Server, Client);

	// The client's OnChunkEdited carries the server's source, centre and radius.
	int32 ClientPlayerBroadcasts = 0;
	float LastRadius = -1.0f;
	Client.Manager->OnChunkEdited.AddLambda([&](const FIntVector&, EEditSource Source, const FVector&, float Radius)
	{
		ClientPlayerBroadcasts += Source == EEditSource::Player ? 1 : 0;
		LastRadius = Radius;
	});

	// A smooth dig at a chunk corner (the chunks around it its falloff reaches).
	FVoxelBrushParams Sphere;
	Sphere.Shape = EVoxelBrushShape::Sphere;
	Sphere.Radius = VoxelSize * 1.5f;
	Sphere.FalloffType = EVoxelBrushFalloff::Smooth;
	Sphere.DensityDelta = 80;
	const FVector Corner = VoxelCentre(*Server, ChunkSize, ChunkSize, ChunkSize, bPlanet) - FVector(VoxelSize * 0.5f);
	Server->ApplyBrushEdit(Corner, Sphere, EEditMode::Subtract);
	Client.Deliver(Test);
	SameLayers(Test, TEXT("A smooth dig across a chunk corner"), *Server, *Client.Manager);
	Test.TestTrue(TEXT("The client's broadcasts carry the player source"), ClientPlayerBroadcasts >= 4);
	Test.TestEqual(TEXT("... and the brush radius"), LastRadius, Sphere.Radius);

	// The same dig again (relative edits accumulate past 255), a cubic block placed and a block dug.
	Server->ApplyBrushEdit(Corner, Sphere, EEditMode::Subtract);
	Server->ApplyBrushEdit(Corner, Sphere, EEditMode::Subtract);
	FVoxelBrushParams Block;
	Block.Shape = EVoxelBrushShape::Cube;
	Block.Radius = VoxelSize * 0.5f;
	Block.FalloffType = EVoxelBrushFalloff::Sharp;
	Block.DensityDelta = 255;
	Block.MaterialID = 5;
	const FVector BlockAt = VoxelCentre(*Server, 3, 4, 5, bPlanet);
	Server->ApplyBrushEdit(BlockAt, Block, EEditMode::Add);
	Server->ApplyBrushEdit(VoxelCentre(*Server, 9, 4, 5, bPlanet), Block, EEditMode::Subtract);
	Client.Deliver(Test);
	SameLayers(Test, TEXT("Accumulated digs and cubic blocks"), *Server, *Client.Manager);

	// A Set and a Paint (absolute and material-only), then a fill that cancels the dug block: its edit is removed.
	Server->ApplyEdit(VoxelCentre(*Server, 1, 1, 1, bPlanet), FVoxelData(4, 255), EEditMode::Set);
	FVoxelBrushParams Paint = Block;
	Paint.MaterialID = 8;
	Server->ApplyBrushEdit(VoxelCentre(*Server, 2, 1, 1, bPlanet), Paint, EEditMode::Paint);
	Server->ApplyBrushEdit(VoxelCentre(*Server, 9, 4, 5, bPlanet), Block, EEditMode::Add);
	Client.Deliver(Test);
	SameLayers(Test, TEXT("A Set, a Paint and a cancelling fill"), *Server, *Client.Manager);

	// Undo the last three operations and redo one.
	Server->Undo();
	Server->Undo();
	Server->Undo();
	Client.Deliver(Test);
	SameLayers(Test, TEXT("Three undos"), *Server, *Client.Manager);
	Server->Redo();
	Client.Deliver(Test);
	SameLayers(Test, TEXT("A redo"), *Server, *Client.Manager);

	// An operation outside the history (a flood, a felled tree) replicates too.
	Server->SetEditSource(EEditSource::System);
	Server->BeginEditOperation(TEXT("Flood"), /*bAddToUndoHistory=*/false);
	Server->ApplyEdit(VoxelCentre(*Server, 12, 12, 12, bPlanet), FVoxelData(1, 0), EEditMode::Set);
	Server->EndEditOperation();
	Client.Deliver(Test);
	SameLayers(Test, TEXT("An operation outside the history"), *Server, *Client.Manager);
	Test.TestTrue(TEXT("Every chunk the client was sent is at the server's revision"), Client.State.CountBehind(*Server) == 0);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEditReplicationLivePlaneTest,
	"VoxelWorlds.Edit.Replication.Live.Plane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelEditReplicationLivePlaneTest::RunTest(const FString& Parameters)
{
	using namespace VoxelEditReplicationTestUtils; // in the function: a file-scope directive leaks into the unity blob (G251)
	RunLiveCase(*this, /*bPlanet=*/false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEditReplicationLivePlanetTest,
	"VoxelWorlds.Edit.Replication.Live.Planet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelEditReplicationLivePlanetTest::RunTest(const FString& Parameters)
{
	using namespace VoxelEditReplicationTestUtils; // in the function: a file-scope directive leaks into the unity blob (G251)
	RunLiveCase(*this, /*bPlanet=*/true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEditReplicationCatchUpTest,
	"VoxelWorlds.Edit.Replication.CatchUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelEditReplicationCatchUpTest::RunTest(const FString& Parameters)
{
	using namespace VoxelEditReplicationTestUtils; // in the function: a file-scope directive leaks into the unity blob (G251)
	UVoxelEditManager* Server = MakeManager(false);
	FVoxelBrushParams Sphere;
	Sphere.Radius = VoxelSize * 3.0f;
	Sphere.DensityDelta = 80;
	// Edits in four places: near the origin, two chunks out, ten out, and one big fill across a chunk.
	Server->ApplyBrushEdit(VoxelCentre(*Server, 4, 4, 4, false), Sphere, EEditMode::Subtract);
	Server->ApplyBrushEdit(VoxelCentre(*Server, 2 * ChunkSize + 5, 4, 4, false), Sphere, EEditMode::Subtract);
	Server->ApplyBrushEdit(VoxelCentre(*Server, 10 * ChunkSize + 5, 4, 4, false), Sphere, EEditMode::Add);
	FVoxelBrushParams Big;
	Big.Shape = EVoxelBrushShape::Cube;
	Big.Radius = VoxelSize * 6.0f;
	Big.FalloffType = EVoxelBrushFalloff::Sharp;
	Big.DensityDelta = 255;
	Server->ApplyBrushEdit(VoxelCentre(*Server, 5 * ChunkSize + 8, 8, 8, false), Big, EEditMode::Add);
	// A Local stamp in a chunk of its own: nothing to send there.
	Server->SetEditReplication(EVoxelEditReplication::Local);
	Server->ApplyEdit(VoxelCentre(*Server, -40, -40, -40, false), FVoxelData(2, 255), EEditMode::Set);
	Server->SetEditReplication(EVoxelEditReplication::Replicated);

	// A client joining now (a late join, or any client after a restart's load): small messages, so a chunk goes in parts.
	FTestClient Late;
	Late.Manager = MakeManager(false);
	Late.MaxVoxels = 100;
	Listen(*Server, Late);
	TArray<FVoxelEditWireMessage> First;
	Late.State.PickCatchUp(*Server, FVector(10.5, 0.5, 0.5), 1, Late.MaxVoxels, First);
	TestTrue(TEXT("The first chunk sent is the one nearest the client (ten chunks out)"),
		First.Num() > 0 && First[0].Chunks.Num() > 0 && First[0].Chunks[0].Coord.X == 10);
	Late.InFlight = First;
	Late.Deliver(*this);

	// A live change to a chunk the client has not been sent yet waits for the catch-up; one to a chunk it has goes now.
	Server->ApplyBrushEdit(VoxelCentre(*Server, 4, 4, 4, false), Sphere, EEditMode::Add);
	TestEqual(TEXT("A live change to a chunk not yet sent is not sent as a delta"), Late.InFlight.Num(), 0);
	Server->ApplyBrushEdit(VoxelCentre(*Server, 10 * ChunkSize + 5, 4, 4, false), Sphere, EEditMode::Subtract);
	TestTrue(TEXT("A live change to a chunk already sent goes at once"), Late.InFlight.Num() > 0);
	Late.Deliver(*this);

	const int32 Before = Late.Messages;
	Late.CatchUp(*this, *Server, FVector(10.5, 0.5, 0.5));
	TestTrue(TEXT("The big chunk went in several messages"), Late.Messages - Before > 3);
	SameLayers(*this, TEXT("A late joiner after its catch-up"), *Server, *Late.Manager, /*bMarkedOnly=*/true);
	TestEqual(TEXT("Nothing behind"), Late.State.CountBehind(*Server), 0);
	TestFalse(TEXT("The Local stamp's chunk was not sent"), Late.Manager->GetEditLayer(FIntVector(-3, -3, -3)) != nullptr
		&& Late.Manager->GetEditLayer(FIntVector(-3, -3, -3))->GetEditCount() > 0);

	// A clear of a chunk reaches the client: its voxels there go.
	const FIntVector Cleared(2, 0, 0);
	Server->ClearChunkEdits(Cleared);
	Late.Deliver(*this);
	Late.CatchUp(*this, *Server, FVector(0.5));
	SameLayers(*this, TEXT("After a chunk clear"), *Server, *Late.Manager, /*bMarkedOnly=*/true);
	TestTrue(TEXT("The cleared chunk is empty on the client"), !Late.Manager->ChunkHasEdits(Cleared));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEditReplicationLocalStampTest,
	"VoxelWorlds.Edit.Replication.LocalStamp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelEditReplicationLocalStampTest::RunTest(const FString& Parameters)
{
	using namespace VoxelEditReplicationTestUtils; // in the function: a file-scope directive leaks into the unity blob (G251)
	auto Stamp = [](UVoxelEditManager& Manager)
	{
		// A wall of stone over a 4x4x4 block, as a dungeon stamp writes it.
		Manager.SetEditReplication(EVoxelEditReplication::Local);
		Manager.SetEditSource(EEditSource::System);
		Manager.BeginEditOperation(TEXT("Stamp"), /*bAddToUndoHistory=*/false);
		for (int32 X = 2; X < 6; ++X)
		for (int32 Y = 2; Y < 6; ++Y)
		for (int32 Z = 2; Z < 6; ++Z)
		{
			Manager.ApplyEdit(VoxelCentre(Manager, X, Y, Z, false), FVoxelData(6, 255), EEditMode::Set);
		}
		Manager.EndEditOperation();
		Manager.SetEditReplication(EVoxelEditReplication::Replicated);
		Manager.SetEditSource(EEditSource::Player);
	};
	FVoxelBrushParams Dig;
	Dig.Shape = EVoxelBrushShape::Cube;
	Dig.Radius = VoxelSize * 0.5f;
	Dig.FalloffType = EVoxelBrushFalloff::Sharp;
	Dig.DensityDelta = 255;

	// The server stamps, then a player digs into the wall: the dig composes with the stamp (a Set).
	UVoxelEditManager* Server = MakeManager(false);
	int32 Published = 0;
	Server->OnReplicatedChange.AddLambda([&Published](const FVoxelReplicatedChange&) { ++Published; });
	Stamp(*Server);
	TestEqual(TEXT("A Local stamp is not sent"), Published, 0);
	Server->ApplyBrushEdit(VoxelCentre(*Server, 3, 3, 3, false), Dig, EEditMode::Subtract);
	TestEqual(TEXT("The dig is sent"), Published, 1);

	// The server stamps again (a restart, a re-stream): the dug voxel stays dug.
	const FChunkEditLayer* Layer = Server->GetEditLayer(FIntVector::ZeroValue);
	const int32 Dug = Layer->ToIndex(FIntVector(3, 3, 3));
	const uint8 DugDensity = Layer->Edits.FindChecked(Dug).NewData.Density;
	Stamp(*Server);
	TestEqual(TEXT("A Local stamp leaves the dug voxel as it is on the server"), (int32)Layer->Edits.FindChecked(Dug).NewData.Density, (int32)DugDensity);
	TestTrue(TEXT("... and it is dug"), DugDensity < 128);

	// Client A stamps before the dig arrives, client B after: both end as the server.
	for (const bool bStampFirst : { true, false })
	{
		FTestClient Client;
		Client.Manager = MakeManager(false);
		if (bStampFirst)
		{
			Stamp(*Client.Manager);
		}
		Client.CatchUp(*this, *Server, FVector(0.5));
		if (!bStampFirst)
		{
			Stamp(*Client.Manager);
		}
		SameLayers(*this, bStampFirst ? TEXT("A client that stamped before the dig arrived")
			: TEXT("A client that stamped after the dig arrived"), *Server, *Client.Manager);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEditReplicationSaveMarksTest,
	"VoxelWorlds.Edit.Replication.SaveMarks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelEditReplicationSaveMarksTest::RunTest(const FString& Parameters)
{
	using namespace VoxelEditReplicationTestUtils; // in the function: a file-scope directive leaks into the unity blob (G251)
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("EditReplication");
	const FString V4 = Dir / TEXT("edits_v4.bin");
	const FString V3 = Dir / TEXT("edits_v3.bin");

	UVoxelEditManager* Server = MakeManager(false);
	FVoxelBrushParams Sphere;
	Sphere.Radius = VoxelSize * 2.0f;
	Server->ApplyBrushEdit(VoxelCentre(*Server, 8, 8, 8, false), Sphere, EEditMode::Subtract);
	// A dig back to the generated voxel: marked, with no edit.
	Server->ApplyBrushEdit(VoxelCentre(*Server, 30, 8, 8, false), Sphere, EEditMode::Add);
	Server->ApplyBrushEdit(VoxelCentre(*Server, 30, 8, 8, false), Sphere, EEditMode::Subtract);
	Server->SetEditReplication(EVoxelEditReplication::Local);
	Server->ApplyEdit(VoxelCentre(*Server, 40, 40, 40, false), FVoxelData(2, 255), EEditMode::Set);
	Server->SetEditReplication(EVoxelEditReplication::Replicated);
	TArray<FIntVector> Chunks;
	Server->GetEditedChunkCoords(Chunks);
	for (const TPair<FIntVector, uint32>& Pair : Server->GetReplicatedRevisions())
	{
		Chunks.AddUnique(Pair.Key);
	}
	const int32 MarkedEmpty = MarkedWithoutEdit(*Server, Chunks);
	TestTrue(TEXT("The dig and fill left marked voxels with no edit"), MarkedEmpty > 0);
	TestTrue(TEXT("Saved"), Server->SaveEditsToFile(V4));

	// A restarted server loads the file: the same edits and marks, and its clients get them whole.
	UVoxelEditManager* Restarted = MakeManager(false);
	FTestClient Client;
	Client.Manager = MakeManager(false);
	Listen(*Restarted, Client);
	TestTrue(TEXT("Loaded"), Restarted->LoadEditsFromFile(V4));
	TestTrue(TEXT("The same edits and marks after a load"), Snapshot(*Server).OrderIndependentCompareEqual(Snapshot(*Restarted)));
	TestEqual(TEXT("The same marked voxels with no edit"), MarkedWithoutEdit(*Restarted, Chunks), MarkedEmpty);
	TestEqual(TEXT("A load sends no delta: its chunks go whole"), Client.InFlight.Num(), 0);
	Client.CatchUp(*this, *Restarted, FVector(0.5));
	SameLayers(*this, TEXT("A client of a restarted server"), *Restarted, *Client.Manager, /*bMarkedOnly=*/true);

	// A version 3 file (before G273): its edit loads, unmarked, and nothing is sent.
	{
		TArray<uint8> Buffer;
		FMemoryWriter Writer(Buffer);
		uint32 Magic = 0x56455449;
		uint32 Version = 3;
		int32 ChunkCount = 1;
		FIntVector Coord(0, 0, 0);
		int32 EditCount = 1;
		FIntVector Local(1, 2, 3);
		uint8 Mode = static_cast<uint8>(EEditMode::Subtract);
		int32 Delta = 80;
		uint8 Bytes[5] = { 1, 0, 0, 0, 0 };
		Writer << Magic << Version << ChunkCount << Coord << EditCount << Local << Mode << Delta;
		Writer.Serialize(Bytes, 5);
		FFileHelper::SaveArrayToFile(Buffer, *V3);
	}
	UVoxelEditManager* Old = MakeManager(false);
	int32 Published = 0;
	Old->OnReplicatedChange.AddLambda([&Published](const FVoxelReplicatedChange&) { ++Published; });
	TestTrue(TEXT("A version 3 file loads"), Old->LoadEditsFromFile(V3));
	TestEqual(TEXT("... with its edit"), Old->GetTotalEditCount(), 1);
	const FChunkEditLayer* OldLayer = Old->GetEditLayer(FIntVector::ZeroValue);
	TestTrue(TEXT("... unmarked"), OldLayer && !OldLayer->HasReplicatedVoxels());
	TestEqual(TEXT("... and nothing sent"), Published, 0);

	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
