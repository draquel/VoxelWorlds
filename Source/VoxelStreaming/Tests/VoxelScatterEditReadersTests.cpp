// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "Testing/VoxelScatterTestDefinitions.h"
#include "VoxelChunkManager.h"
#include "VoxelEditManager.h"
#include "VoxelEditTypes.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelScatterManager.h"
#include "VoxelScatterRenderer.h"
#include "Engine/StaticMesh.h"
#include "VoxelWorldConfiguration.h"
#include "IVoxelWorldMode.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G33 (WORLD_MODES_REVIEW_PLAN.md section 14.1, section 19.11): a system or editor edit (a POI's terraform) re-extracted
// the scatter of the edited chunk and its six face neighbours. The chunks whose extraction reads an edited chunk's voxels
// are, on a planet, the seven chunks at -X / -Y / -Z (the voxels past their +X / +Y / +Z faces, G29, G31): a planet's
// three edge and one corner readers kept a stale view until they reloaded, and its other three face neighbours
// re-extracted for nothing. On a heightmap a cubic world's block tops read the voxel over the top layer (the chunk below
// reads the bottom layer, G26); a smooth world's column scan reads the layer past every face (its density gradient,
// G109), so the face neighbours whose facing layer the edit touched read it.
//
// - Scatter.EditReaders.Rule: UVoxelScatterManager::GetEditReaderOffsets by the layers an edit reached, and what
//   UVoxelEditManager reports while it broadcasts OnChunkEdited: the voxels each chunk had changed (GetBroadcastEditBounds)
//   for a batch across two chunks, a brush, an undo and a clear, and no centre or radius for a batch of single-voxel edits
//   after a brush (G53: it reported the brush's).
// - Scatter.EditReaders.Planet: a streamed small planet with scatter, at the equator (local up +X, so the rays run along
//   X), the rays one voxel apart. A system edit digs the three min-face layers of a chunk the surface passes through: its
//   seven readers' last cells read them, and no other chunk is edited. The -Y -Z edge reader beside it is on the surface
//   too, and its rays through its last cells along the dug column cross the ground there. Which readers re-extract after
//   the edit (OnChunkScatterReady), and every cached chunk against a fresh extraction now (CompareWithFreshExtraction).
//   Every reader re-extracts and no chunk is stale (before G33 no edge or corner reader re-extracted and the edge reader
//   kept stale points). Then an edit inside the chunk: re-extractions per edit.
// - Scatter.EditReaders.Plane: a streamed smooth plane; a system edit digs a chunk's bottom layer where the ground lies
//   within G26's band over the chunk below's top. It reaches every face but the top, so the readers are the chunk below and
//   the four side neighbours (G109; before G33 the six face neighbours re-extracted, between G33 and G109 the chunk below
//   alone); every reader with points re-extracts and no cached chunk is stale after it. Then an edit inside the chunk, and
//   one that digs the whole chunk: its re-extraction finds no surface, and the instances RegenerateChunkScatter kept for the
//   replacement must not stay (they stood on the dug terrain).
// ---------------------------------------------------------------------------

namespace VoxelScatterEditReadersTest
{
	/** Tick until streaming and scatter have both been idle for 90 ticks; false on timeout. */
	bool WaitForScatterIdle(VoxelTesting::FTestChunkWorld& W, double TimeoutSeconds = 180.0)
	{
		UVoxelScatterManager* Scatter = W.Manager->GetScatterManager();
		if (!Scatter)
		{
			return false;
		}
		int32 IdleTicks = 0;
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (IdleTicks < 90 && FPlatformTime::Seconds() < Deadline)
		{
			W.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			const bool bIdle = W.Manager->GetPendingGenerationCount() == 0 && W.Manager->GetPendingMeshingCount() == 0
				&& W.Manager->GetAsyncGenerationInProgressCount() == 0 && W.Manager->GetAsyncMeshingInProgressCount() == 0
				&& W.Manager->GetPendingMeshUploadCount() == 0 && W.Manager->GetSeamJobsInFlightCount() == 0
				&& Scatter->GetPendingGenerationCount() == 0;
			IdleTicks = bIdle ? IdleTicks + 1 : 0;
			FPlatformProcess::Sleep(0.002f);
		}
		return IdleTicks >= 90;
	}

	/**
	 * A chunk's cached surface points as comparable keys, sorted: position to 0.1 uu, normal to 1e-4, material and
	 * classification (CompareWithFreshExtraction's key).
	 */
	TArray<FString> SurfaceKeys(const UVoxelScatterManager& Scatter, const FIntVector& Chunk)
	{
		TArray<FString> Keys;
		if (const FChunkSurfaceData* Surface = Scatter.GetChunkSurfaceData(Chunk))
		{
			for (const FVoxelSurfacePoint& P : Surface->SurfacePoints)
			{
				Keys.Add(FString::Printf(TEXT("%lld %lld %lld %d %d %d %d %d %d"), FMath::RoundToInt64(P.Position.X * 10.0), FMath::RoundToInt64(P.Position.Y * 10.0),
					FMath::RoundToInt64(P.Position.Z * 10.0), FMath::RoundToInt32(P.Normal.X * 10000.0), FMath::RoundToInt32(P.Normal.Y * 10000.0),
					FMath::RoundToInt32(P.Normal.Z * 10000.0), static_cast<int32>(P.MaterialID), P.bIsUnderground ? 1 : 0, P.bIsUnderwater ? 1 : 0));
			}
		}
		Keys.Sort();
		return Keys;
	}

	/** Every cached chunk against a fresh extraction now; the stale chunks. */
	TArray<UVoxelScatterManager::FFreshExtractionComparison> Audit(VoxelTesting::FTestChunkWorld& W, UVoxelScatterManager::FFreshExtractionStats* OutStats = nullptr)
	{
		TArray<UVoxelScatterManager::FFreshExtractionComparison> Stale;
		UVoxelChunkManager* Manager = W.Manager;
		const UVoxelScatterManager::FFreshExtractionStats Stats = W.Manager->GetScatterManager()->CompareWithFreshExtraction(
			[Manager](const FIntVector& Coord) { return Manager->GetVoxelSnapshot(Coord); }, Stale);
		if (OutStats)
		{
			*OutStats = Stats;
		}
		return Stale;
	}

	/** One system edit, as one operation: every voxel of Chunk that Pick selects set to NewVoxel (of its current one). */
	int32 SystemEdit(VoxelTesting::FTestChunkWorld& W, const FIntVector& Chunk, TFunctionRef<bool(const FIntVector&)> Pick,
		TFunctionRef<FVoxelData(const FVoxelData&)> NewVoxel)
	{
		UVoxelEditManager* Edits = W.Manager->GetEditManager();
		const int32 CS = W.Config->ChunkSize;
		const float VS = W.Config->VoxelSize;
		const FVector ChunkMin = W.Config->WorldOrigin + FVector(Chunk) * W.Config->GetChunkWorldSize();
		const EEditSource Previous = Edits->GetEditSource();
		Edits->SetEditSource(EEditSource::System);
		Edits->BeginEditOperation(TEXT("G33 test"));
		int32 Count = 0;
		for (int32 Z = 0; Z < CS; ++Z)
		for (int32 Y = 0; Y < CS; ++Y)
		for (int32 X = 0; X < CS; ++X)
		{
			if (!Pick(FIntVector(X, Y, Z)))
			{
				continue;
			}
			const FVector Pos = ChunkMin + (FVector(X, Y, Z) + FVector(0.5)) * VS;
			Count += Edits->ApplyEdit(Pos, NewVoxel(W.Manager->GetVoxelAt(Pos)), EEditMode::Set) ? 1 : 0;
		}
		Edits->EndEditOperation();
		Edits->SetEditSource(Previous);
		return Count;
	}

	/** An edit inside a chunk (local 10..12 on each axis): no layer another chunk's extraction reads. */
	bool IsInside(const FIntVector& L) { return L.X >= 10 && L.X < 13 && L.Y >= 10 && L.Y < 13 && L.Z >= 10 && L.Z < 13; }

	/** After a pass's two edits (G60): both undone, then both redone, each settled; the stale cached chunks after each, and the undo steps left after the undos. */
	struct FUndoRedoAudit
	{
		bool bSettled = true;
		int32 StaleAfterUndo = -1;
		int32 StaleAfterRedo = -1;
		int32 UndoStepsAfterUndo = -1;
	};

	FUndoRedoAudit UndoRedoPass(VoxelTesting::FTestChunkWorld& W)
	{
		FUndoRedoAudit A;
		UVoxelEditManager* Edits = W.Manager->GetEditManager();
		Edits->Undo();
		Edits->Undo();
		A.UndoStepsAfterUndo = Edits->GetUndoCount();
		A.bSettled = W.Settle() && WaitForScatterIdle(W);
		A.StaleAfterUndo = Audit(W).Num();
		Edits->Redo();
		Edits->Redo();
		A.bSettled = W.Settle() && WaitForScatterIdle(W) && A.bSettled;
		A.StaleAfterRedo = Audit(W).Num();
		return A;
	}

	/** What one streamed pass measured. */
	struct FPassResult
	{
		bool bSettled = false;
		bool bFound = false;
		FIntVector Edited = FIntVector::ZeroValue;
		int32 EditedVoxels = 0;
		int32 Readers = 0;                // GetEditReaders for the edit's bounds
		int32 ReadersWithPoints = 0;      // readers with surface points before the edit
		int32 ReadersReextracted = 0;     // of those, re-extracted after it (OnChunkScatterReady)
		int32 EdgeCornerWithPoints = 0;   // the same for the edge and corner readers
		int32 EdgeCornerReextracted = 0;
		int32 ReadersChanged = 0;         // readers whose cached points differ from before the edit
		int32 EdgeCornerChanged = 0;      // of them, edge or corner readers
		int32 StaleBefore = 0;            // the audit before the edit
		int32 Compared = 0;
		int32 Stale = 0;                  // after the edit
		int32 StaleReaders = 0;           // of them, readers of the edited chunk
		int32 StaleEdgeCorner = 0;        // of them, edge or corner readers
		int64 Regenerations = 0;          // RegenerateChunkScatter calls the edit caused
		int64 Extractions = 0;            // extractions it launched
		int64 MidRegenerations = 0;       // the same for an edit inside the chunk
		int64 MidExtractions = 0;
		UVoxelChunkManager::FEditScatterStats Stats;
	};

	/**
	 * One streamed pass: settle and audit; Find picks the chunk to edit (false: none qualifies) and Edit edits it
	 * (returning the voxels changed) with BoundsMin and BoundsMax the lowest and highest local coordinates it reaches;
	 * settle and audit again; then an edit inside the chunk.
	 */
	FPassResult RunPass(FAutomationTestBase& Test, VoxelTesting::FTestChunkWorld& W, const FIntVector& BoundsMin, const FIntVector& BoundsMax,
		TFunctionRef<bool(VoxelTesting::FTestChunkWorld&, FIntVector&)> Find,
		TFunctionRef<int32(VoxelTesting::FTestChunkWorld&, const FIntVector&)> Edit)
	{
		FPassResult R;
		UVoxelScatterManager* Scatter = W.Manager->GetScatterManager();
		if (!Scatter)
		{
			Test.AddError(TEXT("The chunk manager has a scatter manager"));
			return R;
		}
		R.bSettled = W.Settle() && WaitForScatterIdle(W);
		R.StaleBefore = Audit(W).Num();
		R.bFound = Find(W, R.Edited);
		if (!R.bFound)
		{
			return R;
		}

		TArray<FIntVector> Readers;
		Scatter->GetEditReaders(R.Edited, &BoundsMin, &BoundsMax, Readers);
		R.Readers = Readers.Num();
		auto IsEdgeOrCorner = [&R](const FIntVector& Reader)
		{
			const FIntVector D = R.Edited - Reader;
			return (D.X != 0) + (D.Y != 0) + (D.Z != 0) >= 2;
		};
		TMap<FIntVector, TArray<FString>> Before;
		for (const FIntVector& Reader : Readers)
		{
			if (Scatter->GetChunkSurfaceData(Reader))
			{
				Before.Add(Reader, SurfaceKeys(*Scatter, Reader));
			}
		}
		// Which chunks finish an extraction after the edit.
		TSet<FIntVector> ScatterReady;
		const FDelegateHandle Handle = Scatter->OnChunkScatterReady.AddLambda([&ScatterReady](const FIntVector& Chunk, int32)
		{
			ScatterReady.Add(Chunk);
		});
		const int64 Regen0 = Scatter->GetTotalRegenerations();
		const int64 Extract0 = Scatter->GetTotalExtractionsLaunched();
		R.EditedVoxels = Edit(W, R.Edited);
		R.bSettled = WaitForScatterIdle(W) && R.bSettled;
		R.Regenerations = Scatter->GetTotalRegenerations() - Regen0;
		R.Extractions = Scatter->GetTotalExtractionsLaunched() - Extract0;
		Scatter->OnChunkScatterReady.Remove(Handle);

		for (const TPair<FIntVector, TArray<FString>>& Pair : Before)
		{
			const bool bReextracted = ScatterReady.Contains(Pair.Key);
			++R.ReadersWithPoints;
			R.ReadersReextracted += bReextracted ? 1 : 0;
			if (IsEdgeOrCorner(Pair.Key))
			{
				++R.EdgeCornerWithPoints;
				R.EdgeCornerReextracted += bReextracted ? 1 : 0;
			}
			const bool bChanged = Pair.Value != SurfaceKeys(*Scatter, Pair.Key);
			R.ReadersChanged += bChanged ? 1 : 0;
			R.EdgeCornerChanged += (bChanged && IsEdgeOrCorner(Pair.Key)) ? 1 : 0;
		}

		UVoxelScatterManager::FFreshExtractionStats Stats;
		const TArray<UVoxelScatterManager::FFreshExtractionComparison> Stale = Audit(W, &Stats);
		R.Compared = Stats.Compared;
		R.Stale = Stale.Num();
		for (const UVoxelScatterManager::FFreshExtractionComparison& C : Stale)
		{
			if (Readers.Contains(C.ChunkCoord))
			{
				++R.StaleReaders;
				R.StaleEdgeCorner += IsEdgeOrCorner(C.ChunkCoord) ? 1 : 0;
			}
		}
		R.Stats = W.Manager->GetEditScatterStats();

		// An edit inside the chunk: the re-extractions it causes.
		const int64 Regen1 = Scatter->GetTotalRegenerations();
		const int64 Extract1 = Scatter->GetTotalExtractionsLaunched();
		SystemEdit(W, R.Edited, IsInside, [](const FVoxelData&) { return FVoxelData::Solid(EVoxelMaterial::Stone); });
		R.bSettled = WaitForScatterIdle(W) && R.bSettled;
		R.MidRegenerations = Scatter->GetTotalRegenerations() - Regen1;
		R.MidExtractions = Scatter->GetTotalExtractionsLaunched() - Extract1;
		return R;
	}

	FString Describe(const TCHAR* Name, const FPassResult& R)
	{
		return FString::Printf(TEXT("%s: chunk (%d,%d,%d), %d voxels edited, %d readers, %d with points before (%d edge or corner); re-extracted after the edit: %d (%d edge or corner); points changed: %d (%d edge or corner); audit before the edit %d stale, after: %d of %d cached chunks stale (%d readers, %d edge or corner); the edit: %lld regenerations, %lld extractions (readers re-extracted %lld, folded %lld, without data %lld); an edit inside the chunk: %lld regenerations, %lld extractions"),
			Name, R.Edited.X, R.Edited.Y, R.Edited.Z, R.EditedVoxels, R.Readers, R.ReadersWithPoints, R.EdgeCornerWithPoints, R.ReadersReextracted,
			R.EdgeCornerReextracted, R.ReadersChanged, R.EdgeCornerChanged, R.StaleBefore, R.Stale, R.Compared, R.StaleReaders, R.StaleEdgeCorner, R.Regenerations,
			R.Extractions, R.Stats.ReadersReextracted, R.Stats.ReadersFolded, R.Stats.ReadersWithoutData, R.MidRegenerations, R.MidExtractions);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterEditReadersRuleTest, "VoxelWorlds.Scatter.EditReaders.Rule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelScatterEditReadersRuleTest::RunTest(const FString& Parameters)
{
	// --- The reader rule by the layers an edit reached ---
	constexpr int32 CS = 16;
	auto Offsets = [](EVoxelScatterOutsideReads Reads, const FIntVector* Min, const FIntVector* Max)
	{
		TArray<FIntVector> Out;
		UVoxelScatterManager::GetEditReaderOffsets(Reads, CS, Min, Max, Out);
		Out.Sort([](const FIntVector& A, const FIntVector& B) { return A.X != B.X ? A.X < B.X : (A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z); });
		return Out;
	};
	using enum EVoxelScatterOutsideReads;
	const FIntVector Mid(5, 5, 5), BottomZ(5, 5, 0), FaceX(0, 5, 5), EdgeXY(0, 0, 5), Corner(0, 0, 0);
	const FIntVector TopZ(5, 5, CS - 1), FaceMaxX(CS - 1, 5, 5), MaxCorner(CS - 1, CS - 1, CS - 1);
	TestEqual(TEXT("Cubic heightmap: an edit inside the chunk is read by no other chunk"), Offsets(TopLayer, &Mid, &Mid).Num(), 0);
	TestEqual(TEXT("Cubic heightmap: an edit on the bottom layer is read by the chunk below"), Offsets(TopLayer, &BottomZ, &BottomZ),
		TArray<FIntVector>{ FIntVector(0, 0, -1) });
	TestEqual(TEXT("Cubic heightmap: an edit on the -X layer only is read by no other chunk"), Offsets(TopLayer, &FaceX, &FaceX).Num(), 0);
	TestEqual(TEXT("Cubic heightmap: unknown bounds give the chunk below"), Offsets(TopLayer, nullptr, nullptr), TArray<FIntVector>{ FIntVector(0, 0, -1) });
	TestEqual(TEXT("Smooth heightmap: an edit inside the chunk is read by no other chunk"), Offsets(FaceLayers, &Mid, &Mid).Num(), 0);
	TestEqual(TEXT("Smooth heightmap: an edit on the bottom layer is read by the chunk below"), Offsets(FaceLayers, &BottomZ, &BottomZ),
		TArray<FIntVector>{ FIntVector(0, 0, -1) });
	TestEqual(TEXT("Smooth heightmap: an edit on the top layer is read by the chunk above (G109)"), Offsets(FaceLayers, &TopZ, &TopZ),
		TArray<FIntVector>{ FIntVector(0, 0, 1) });
	TestEqual(TEXT("Smooth heightmap: an edit on the -X layer is read by the -X neighbour (G109)"), Offsets(FaceLayers, &FaceX, &FaceX),
		TArray<FIntVector>{ FIntVector(-1, 0, 0) });
	TestEqual(TEXT("Smooth heightmap: an edit on the +X layer is read by the +X neighbour (G109)"), Offsets(FaceLayers, &FaceMaxX, &FaceMaxX),
		TArray<FIntVector>{ FIntVector(1, 0, 0) });
	TestEqual(TEXT("Smooth heightmap: an edit on the -X -Y edge is read by the -X and -Y neighbours, no edge neighbour"), Offsets(FaceLayers, &EdgeXY, &EdgeXY),
		TArray<FIntVector>{ FIntVector(-1, 0, 0), FIntVector(0, -1, 0) });
	TestEqual(TEXT("Smooth heightmap: an edit across the whole chunk is read by its six face neighbours"), Offsets(FaceLayers, &Corner, &MaxCorner).Num(), 6);
	TestEqual(TEXT("Smooth heightmap: unknown bounds give the six"), Offsets(FaceLayers, nullptr, nullptr).Num(), 6);
	TestEqual(TEXT("Planet: an edit inside the chunk is read by no other chunk"), Offsets(PlanetShell, &Mid, &Mid).Num(), 0);
	TestEqual(TEXT("Planet: an edit on the -X layer is read by the -X neighbour"), Offsets(PlanetShell, &FaceX, &FaceX), TArray<FIntVector>{ FIntVector(-1, 0, 0) });
	TestEqual(TEXT("Planet: an edit on the +X layer is read by no other chunk"), Offsets(PlanetShell, &FaceMaxX, &FaceMaxX).Num(), 0);
	TestEqual(TEXT("Planet: an edit on the -X -Y edge is read by the -X, -Y and -X -Y neighbours"), Offsets(PlanetShell, &EdgeXY, &EdgeXY),
		TArray<FIntVector>{ FIntVector(-1, -1, 0), FIntVector(-1, 0, 0), FIntVector(0, -1, 0) });
	TestEqual(TEXT("Planet: an edit on the min corner is read by the seven neighbours at -X / -Y / -Z"), Offsets(PlanetShell, &Corner, &Corner).Num(), 7);
	TestEqual(TEXT("Planet: unknown bounds give the seven"), Offsets(PlanetShell, nullptr, nullptr).Num(), 7);

	// --- What the edit manager reports while it broadcasts ---
	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
	Config->AddToRoot();
	Config->WorldMode = EWorldMode::InfinitePlane;
	Config->WorldOrigin = FVector::ZeroVector;
	Config->ChunkSize = 16;
	Config->VoxelSize = 100.0f;
	UVoxelEditManager* Edits = NewObject<UVoxelEditManager>();
	Edits->AddToRoot();
	Edits->Initialize(Config);

	struct FBroadcast
	{
		FIntVector Chunk = FIntVector::ZeroValue;
		EEditSource Source = EEditSource::Player;
		FVector Center = FVector::ZeroVector;
		float Radius = 0.0f;
		bool bBounds = false;
		FIntVector Min = FIntVector::ZeroValue;
		FIntVector Max = FIntVector::ZeroValue;
	};
	TArray<FBroadcast> Seen;
	const FDelegateHandle Handle = Edits->OnChunkEdited.AddLambda([&Seen, Edits](const FIntVector& Chunk, EEditSource Source, const FVector& Center, float Radius)
	{
		FBroadcast& B = Seen.AddDefaulted_GetRef();
		B.Chunk = Chunk;
		B.Source = Source;
		B.Center = Center;
		B.Radius = Radius;
		B.bBounds = Edits->GetBroadcastEditBounds(B.Min, B.Max);
	});
	auto Voxel = [](int32 X, int32 Y, int32 Z) { return (FVector(X, Y, Z) + FVector(0.5)) * 100.0; };
	auto Find = [&Seen](const FIntVector& Chunk) -> const FBroadcast*
	{
		return Seen.FindByPredicate([&Chunk](const FBroadcast& B) { return B.Chunk == Chunk; });
	};

	FIntVector OutMin, OutMax;
	TestFalse(TEXT("No bounds outside a broadcast"), Edits->GetBroadcastEditBounds(OutMin, OutMax));

	// A batch across two chunks: chunk (0,0,0) voxels (15,3,4) and (15,6,2); chunk (1,0,0) voxels (0,3,4) and (0,9,0).
	Edits->SetEditSource(EEditSource::System);
	Edits->BeginEditOperation(TEXT("Batch"));
	Edits->ApplyEdit(Voxel(15, 3, 4), FVoxelData::Air(), EEditMode::Set);
	Edits->ApplyEdit(Voxel(15, 6, 2), FVoxelData::Air(), EEditMode::Set);
	Edits->ApplyEdit(Voxel(16, 3, 4), FVoxelData::Air(), EEditMode::Set);
	Edits->ApplyEdit(Voxel(16, 9, 0), FVoxelData::Air(), EEditMode::Set);
	Edits->EndEditOperation();
	{
		TestEqual(TEXT("Batch: one broadcast per chunk"), Seen.Num(), 2);
		const FBroadcast* A = Find(FIntVector(0, 0, 0));
		const FBroadcast* B = Find(FIntVector(1, 0, 0));
		if (TestNotNull(TEXT("Batch: chunk (0,0,0) broadcast"), A) && TestNotNull(TEXT("Batch: chunk (1,0,0) broadcast"), B))
		{
			TestTrue(TEXT("Batch: bounds during the broadcast"), A->bBounds && B->bBounds);
			TestEqual(TEXT("Batch: chunk (0,0,0) bounds min"), A->Min, FIntVector(15, 3, 2));
			TestEqual(TEXT("Batch: chunk (0,0,0) bounds max"), A->Max, FIntVector(15, 6, 4));
			TestEqual(TEXT("Batch: chunk (1,0,0) bounds min"), B->Min, FIntVector(0, 3, 0));
			TestEqual(TEXT("Batch: chunk (1,0,0) bounds max"), B->Max, FIntVector(0, 9, 4));
			TestEqual(TEXT("Batch: a batch of single-voxel edits has no radius"), A->Radius, 0.0f);
		}
	}

	// A brush, then a batch of single-voxel edits: the batch reports no centre or radius (G53: it reported the brush's).
	Seen.Reset();
	Edits->SetEditSource(EEditSource::Player);
	FVoxelBrushParams Brush;
	Brush.Radius = 150.0f;
	Brush.DensityDelta = 255;
	Edits->ApplyBrushEdit(Voxel(40, 40, 8), Brush, EEditMode::Subtract);
	TestTrue(TEXT("Brush: its broadcasts carry its centre and radius"), Seen.Num() > 0 && Seen[0].Radius == 150.0f && Seen[0].Center.Equals(Voxel(40, 40, 8)));
	TestTrue(TEXT("Brush: and its bounds"), Seen.Num() > 0 && Seen[0].bBounds);
	Seen.Reset();
	Edits->SetEditSource(EEditSource::System);
	Edits->BeginEditOperation(TEXT("Terraform after a brush"));
	Edits->ApplyEdit(Voxel(70, 2, 2), FVoxelData::Solid(EVoxelMaterial::Stone), EEditMode::Set);
	Edits->EndEditOperation();
	if (TestEqual(TEXT("Terraform: one broadcast"), Seen.Num(), 1))
	{
		AddInfo(FString::Printf(TEXT("A batch of single-voxel edits after a brush broadcasts centre (%.0f, %.0f, %.0f), radius %.0f"), Seen[0].Center.X,
			Seen[0].Center.Y, Seen[0].Center.Z, Seen[0].Radius));
		TestEqual(TEXT("Terraform after a brush: no radius (G53)"), Seen[0].Radius, 0.0f);
		TestTrue(TEXT("Terraform after a brush: no centre (G53)"), Seen[0].Center.IsZero());
		TestTrue(TEXT("Terraform: the bounds of its one voxel"), Seen[0].bBounds && Seen[0].Min == FIntVector(6, 2, 2) && Seen[0].Max == FIntVector(6, 2, 2));
	}

	// Undo the terraform: the bounds of the voxel it reverts.
	Seen.Reset();
	Edits->Undo();
	if (TestEqual(TEXT("Undo: one broadcast"), Seen.Num(), 1))
	{
		TestTrue(TEXT("Undo: the reverted voxel's bounds"), Seen[0].bBounds && Seen[0].Min == FIntVector(6, 2, 2) && Seen[0].Max == FIntVector(6, 2, 2));
	}

	// Clear a chunk's edits: the bounds of every edit it had.
	Seen.Reset();
	Edits->ClearChunkEdits(FIntVector(1, 0, 0));
	if (TestEqual(TEXT("Clear: one broadcast"), Seen.Num(), 1))
	{
		TestTrue(TEXT("Clear: the cleared edits' bounds"), Seen[0].bBounds && Seen[0].Min == FIntVector(0, 3, 0) && Seen[0].Max == FIntVector(0, 9, 4));
	}
	TestFalse(TEXT("No bounds after the broadcasts"), Edits->GetBroadcastEditBounds(OutMin, OutMax));

	Edits->OnChunkEdited.Remove(Handle);
	Edits->Shutdown();
	Edits->RemoveFromRoot();
	Config->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterEditReadersPlanetTest, "VoxelWorlds.Scatter.EditReaders.Planet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelScatterEditReadersPlanetTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterEditReadersTest;

	// A chunk the surface passes through whose -Y, -Z and -Y -Z neighbours (beside it, along local up) carry surface points
	// too, nearest the site.
	auto Find = [](VoxelTesting::FTestChunkWorld& W, FIntVector& OutChunk) -> bool
	{
		UVoxelScatterManager* Scatter = W.Manager->GetScatterManager();
		TArray<FIntVector> Loaded;
		W.Manager->GetLoadedChunks(Loaded);
		double BestDistance = 1.0e30;
		bool bFound = false;
		for (const FIntVector& Chunk : Loaded)
		{
			const FIntVector Beside[4] = { Chunk, Chunk - FIntVector(0, 1, 0), Chunk - FIntVector(0, 0, 1), Chunk - FIntVector(0, 1, 1) };
			bool bAll = true;
			for (const FIntVector& B : Beside)
			{
				bAll &= Scatter->GetChunkSurfaceData(B) != nullptr;
			}
			const double Distance = FVector(Chunk.Y, Chunk.Z, 0).Size();
			if (bAll && Distance < BestDistance)
			{
				BestDistance = Distance;
				OutChunk = Chunk;
				bFound = true;
			}
		}
		return bFound;
	};
	// Dig the three min-face layers: air walls (and floor) the seven readers' last cells reach.
	auto Edit = [](VoxelTesting::FTestChunkWorld& W, const FIntVector& Chunk) -> int32
	{
		return SystemEdit(W, Chunk, [](const FIntVector& L) { return L.X == 0 || L.Y == 0 || L.Z == 0; }, [](const FVoxelData&) { return FVoxelData::Air(); });
	};

	{
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("ScatterEditReadersPlanet"));
		W.ConfigureSmallPlanet();
		W.Config->bEnableScatter = true;
		W.Config->ScatterRadius = 100000.0f;
		W.StartManager();
		VoxelScatterTesting::AddSmoothDefinitions(*W.Manager->GetScatterManager());
		// Rays one voxel apart, so the edge reader's last cell column has a ray across it.
		W.Manager->GetScatterManager()->SetSurfacePointSpacing(W.Config->VoxelSize);
		// The equator on the +X face: local up +X, so the rays run along X, beside the chunk's walls.
		const FVector Up(1, 0, 0);
		const FVoxelTerrainSurfacePoint Site = W.Manager->GetSurfacePointAt(W.Config->WorldOrigin + Up);
		W.SetViewer(Site.Position + Up * 300.0);
		const FPassResult R = RunPass(*this, W, FIntVector(0, 0, 0), FIntVector(W.Config->ChunkSize - 1), Find, Edit);
		// Undo and redo are edits too (G60): the readers re-extract after them, so every cached chunk equals a fresh extraction.
		FUndoRedoAudit U;
		if (R.bFound)
		{
			U = UndoRedoPass(W);
		}
		W.Stop();

		const TCHAR* Name = TEXT("Edit readers");
		AddInfo(Describe(Name, R));
		TestTrue(FString::Printf(TEXT("%s: streaming and scatter settle"), Name), R.bSettled);
		if (!TestTrue(FString::Printf(TEXT("%s: a surface chunk whose -Y, -Z and -Y -Z neighbours carry points (non-vacuous)"), Name), R.bFound))
		{
			return true;
		}
		AddInfo(FString::Printf(TEXT("%s: both edits undone: %d cached chunks stale (%d undo steps left); redone: %d stale"), Name, U.StaleAfterUndo,
			U.UndoStepsAfterUndo, U.StaleAfterRedo));
		TestTrue(FString::Printf(TEXT("%s: streaming and scatter settle after the undos and redos"), Name), U.bSettled);
		TestEqual(FString::Printf(TEXT("%s: the two edits undone"), Name), U.UndoStepsAfterUndo, 0);
		TestEqual(FString::Printf(TEXT("%s: no cached chunk is stale after the undos"), Name), U.StaleAfterUndo, 0);
		TestEqual(FString::Printf(TEXT("%s: no cached chunk is stale after the redos"), Name), U.StaleAfterRedo, 0);
		TestEqual(FString::Printf(TEXT("%s: every cached chunk equals a fresh extraction before the edit"), Name), R.StaleBefore, 0);
		TestEqual(FString::Printf(TEXT("%s: the rule gives seven readers for an edit on the min corner"), Name), R.Readers, 7);
		TestTrue(FString::Printf(TEXT("%s: edge or corner readers with points (non-vacuous)"), Name), R.EdgeCornerWithPoints >= 1);
		TestEqual(TEXT("Every reader with points re-extracts after the edit"), R.ReadersReextracted, R.ReadersWithPoints);
		TestEqual(TEXT("No cached chunk is stale after the edit"), R.Stale, 0);
		TestTrue(TEXT("The re-extraction changed edge or corner readers' points"), R.EdgeCornerChanged > 0);
		TestEqual(TEXT("Every reader is re-extracted once or has no data yet"), R.Stats.ReadersReextracted + R.Stats.ReadersWithoutData, int64(7));
		TestEqual(TEXT("The edit regenerates the edited chunk and the readers re-extracted"), R.Regenerations, int64(1) + R.Stats.ReadersReextracted);
		TestEqual(TEXT("An edit inside the chunk regenerates it alone"), R.MidRegenerations, int64(1));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterEditReadersPlaneTest, "VoxelWorlds.Scatter.EditReaders.Plane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelScatterEditReadersPlaneTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterEditReadersTest;

	// The chunk over the most columns whose ground lies within G26's band (1.5 voxels) over its bottom face, with surface
	// points in the chunk below.
	auto Find = [](VoxelTesting::FTestChunkWorld& W, FIntVector& OutChunk) -> bool
	{
		UVoxelScatterManager* Scatter = W.Manager->GetScatterManager();
		const int32 CS = W.Config->ChunkSize;
		const float VS = W.Config->VoxelSize;
		const double ChunkWorld = W.Config->GetChunkWorldSize();
		TArray<FIntVector> Loaded;
		W.Manager->GetLoadedChunks(Loaded);
		int32 Best = 0;
		for (const FIntVector& Chunk : Loaded)
		{
			if (!Scatter->GetChunkSurfaceData(Chunk - FIntVector(0, 0, 1)))
			{
				continue;
			}
			const FVector Min = W.Config->WorldOrigin + FVector(Chunk) * ChunkWorld;
			int32 InEdge = 0;
			for (int32 Y = 0; Y < CS; ++Y)
			for (int32 X = 0; X < CS; ++X)
			{
				const double H = W.Manager->GetGeneratedSurfacePoint(FVector(Min.X + (X + 0.5) * VS, Min.Y + (Y + 0.5) * VS, 0.0)).Z;
				InEdge += (H > Min.Z + 0.1 * VS && H <= Min.Z + 1.4 * VS) ? 1 : 0;
			}
			if (InEdge > Best)
			{
				Best = InEdge;
				OutChunk = Chunk;
			}
		}
		return Best >= 16;
	};
	// Dig the bottom layer: the chunk below's top face is open surface there now (G26's dig case).
	auto Edit = [](VoxelTesting::FTestChunkWorld& W, const FIntVector& Chunk) -> int32
	{
		return SystemEdit(W, Chunk, [](const FIntVector& L) { return L.Z == 0; }, [](const FVoxelData&) { return FVoxelData::Air(); });
	};

	{
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("ScatterEditReadersPlane"));
		W.ConfigureSmallPlane();
		W.Config->bEnableScatter = true;
		W.Config->ScatterRadius = 100000.0f;
		W.StartManager();
		// The test definitions have no mesh, so the renderer would make no instance: give each the engine cube.
		UVoxelScatterManager* Scatter = W.Manager->GetScatterManager();
		VoxelScatterTesting::AddSmoothDefinitions(*Scatter);
		for (FScatterDefinition Def : TArray<FScatterDefinition>(Scatter->GetScatterDefinitions()))
		{
			Def.Mesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")));
			Scatter->AddScatterDefinition(Def);
		}
		const double ChunkWorld = W.Config->GetChunkWorldSize();
		W.SetViewer(FVector(ChunkWorld * 0.5, ChunkWorld * 0.5, W.Manager->GetGeneratedSurfacePoint(FVector(ChunkWorld * 0.5, ChunkWorld * 0.5, 0.0)).Z + 300.0));
		const FPassResult R = RunPass(*this, W, FIntVector(0, 0, 0), FIntVector(W.Config->ChunkSize - 1, W.Config->ChunkSize - 1, 0), Find, Edit);
		// Undo and redo are edits too (G60): the chunk below re-extracts after them, so every cached chunk equals a fresh extraction.
		FUndoRedoAudit U;
		if (R.bFound)
		{
			U = UndoRedoPass(W);
		}
		// A system edit that leaves the edited chunk no surface: its re-extraction finds no point, and the instances
		// RegenerateChunkScatter kept for the replacement must go (they stood on terrain the edit removed).
		int32 InstancesBefore = -1, InstancesAfter = -1;
		bool bClearSettled = false;
		if (R.bFound)
		{
			InstancesBefore = Scatter->GetScatterRenderer()->GetChunkInstanceCount(R.Edited);
			SystemEdit(W, R.Edited, [](const FIntVector&) { return true; }, [](const FVoxelData&) { return FVoxelData::Air(); });
			bClearSettled = WaitForScatterIdle(W);
			InstancesAfter = Scatter->GetScatterRenderer()->GetChunkInstanceCount(R.Edited);
		}
		W.Stop();

		const TCHAR* Name = TEXT("Edit readers");
		AddInfo(Describe(Name, R));
		TestTrue(FString::Printf(TEXT("%s: streaming and scatter settle"), Name), R.bSettled);
		if (!TestTrue(FString::Printf(TEXT("%s: a chunk over ground within G26's band of its bottom face (non-vacuous)"), Name), R.bFound))
		{
			return true;
		}
		TestEqual(FString::Printf(TEXT("%s: every cached chunk equals a fresh extraction before the edit"), Name), R.StaleBefore, 0);
		TestEqual(FString::Printf(TEXT("%s: a bottom-layer edit across the chunk is read by the chunk below and the four side neighbours (G109)"), Name),
			R.Readers, 5);
		TestEqual(FString::Printf(TEXT("%s: every reader with points re-extracts after the edit"), Name), R.ReadersReextracted, R.ReadersWithPoints);
		TestTrue(FString::Printf(TEXT("%s: readers' points changed (non-vacuous)"), Name), R.ReadersChanged >= 1);
		TestEqual(FString::Printf(TEXT("%s: no cached chunk is stale after the edit"), Name), R.Stale, 0);
		AddInfo(FString::Printf(TEXT("%s: both edits undone: %d cached chunks stale (%d undo steps left); redone: %d stale"), Name, U.StaleAfterUndo,
			U.UndoStepsAfterUndo, U.StaleAfterRedo));
		TestTrue(FString::Printf(TEXT("%s: streaming and scatter settle after the undos and redos"), Name), U.bSettled);
		TestEqual(FString::Printf(TEXT("%s: the two edits undone"), Name), U.UndoStepsAfterUndo, 0);
		TestEqual(FString::Printf(TEXT("%s: no cached chunk is stale after the undos"), Name), U.StaleAfterUndo, 0);
		TestEqual(FString::Printf(TEXT("%s: no cached chunk is stale after the redos"), Name), U.StaleAfterRedo, 0);
		AddInfo(FString::Printf(TEXT("%s: the whole chunk dug: its instances %d before, %d after"), Name, InstancesBefore, InstancesAfter));
		TestTrue(FString::Printf(TEXT("%s: the dug chunk had instances (non-vacuous)"), Name), InstancesBefore > 0);
		TestTrue(FString::Printf(TEXT("%s: scatter settles after the dig"), Name), bClearSettled);
		TestEqual(FString::Printf(TEXT("%s: a re-extraction that finds no surface leaves the chunk no instances"), Name), InstancesAfter, 0);
		TestEqual(TEXT("The edit regenerates the edited chunk and the readers re-extracted"), R.Regenerations, int64(1) + R.Stats.ReadersReextracted);
		TestEqual(TEXT("An edit inside the chunk regenerates it alone"), R.MidRegenerations, int64(1));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
