// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelEditManager.h"
#include "VoxelCoreTypes.h"
#include "VoxelBlockSurface.h"
#include "VoxelCoordinates.h"
#include "VoxelTreeInjector.h"
#include "VoxelSurfaceQuery.h"
#include "IVoxelWorldMode.h"
#include "DistanceBandLODStrategy.h"
#include "IVoxelMeshRenderer.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Cubic as a game world (world modes review §21): the block-top surface (WM-C5, KC6) and voxel trees found and felled
// (WM-C8, KC9), on a real chunk manager streaming a small world with CPU generation.
//
// A cubic block at voxel layer k spans [k V, (k + 1) V) above WorldOrigin and the top face of the topmost solid block lies
// up to a block above the density isosurface. The surface queries must return that top face, exactly what the streamed
// voxels hold, at any WorldOrigin and inside a conditioning zone; a smooth world's queries must be what they were.
// ---------------------------------------------------------------------------

namespace VoxelCubicGameplayTestUtils
{
	/** Two tree templates: a 1x1 trunk under a sphere and a cross trunk under a cone (both the cubic demo's materials). */
	TArray<FVoxelTreeTemplate> MakeTreeTemplates()
	{
		TArray<FVoxelTreeTemplate> Templates;
		FVoxelTreeTemplate Oak;
		Oak.TemplateID = 0;
		Oak.TrunkHeight = 5;
		Oak.TrunkHeightVariance = 1;
		Oak.CanopyRadius = 2;
		Oak.CanopyRadiusVariance = 1;
		Oak.MaxSlopeDegrees = 90.0f;
		Templates.Add(Oak);
		FVoxelTreeTemplate Spruce = Oak;
		Spruce.TemplateID = 1;
		Spruce.TrunkRadius = 1;
		Spruce.CanopyShape = ETreeCanopyShape::Cone;
		Spruce.CanopyRadius = 3;
		Templates.Add(Spruce);
		return Templates;
	}

	/** A small world streamed around a fixed viewer by a real chunk manager (CPU generation, no caves or water). */
	struct FGameplayWorld : VoxelTesting::FTestChunkWorld
	{

		/**
		 * @param MeshingMode Cubic or a smooth mesher
		 * @param bTrees Inject voxel trees (cubic only)
		 * @param Zone A conditioning zone to add before streaming (InnerRadius <= 0: none)
		 */
		void Start(const TCHAR* WorldName, EMeshingMode MeshingMode, bool bTrees, const FVoxelConditioningZone& Zone)
		{
			Create(WorldName);
			Renderer.bKeepMeshes = false; // the chunk set is enough here
			Config->WorldMode = EWorldMode::InfinitePlane;
			Config->MeshingMode = MeshingMode;
			Config->bUseGreedyMeshing = true;
			Config->bEnableLOD = MeshingMode != EMeshingMode::Cubic;
			Config->ChunkSize = 16;
			Config->VoxelSize = 75.0f;
			// Off the voxel grid's natural zero on purpose, as the demo's WorldOrigin is (12345, 54321, 0).
			Config->WorldOrigin = FVector(1234.5, -543.25, 37.5);
			Config->ViewDistance = 3000.0f;
			Config->bUseGPUGeneration = false;
			Config->bGenerateCollision = false;
			Config->bEnableScatter = false;
			Config->bEnableWaterLevel = false;
			Config->bEnableCaves = false;
			// Hills of a few hundred uu over a few thousand: every column of the sampled area streams its surface.
			Config->HeightScale = 900.0f;
			Config->BaseHeight = 0.0f;
			Config->NoiseParams.Seed = 9173;
			Config->NoiseParams.Frequency = 0.0005f;
			Config->NoiseParams.Octaves = 3;
			Config->WorldSeed = 9173;
			Config->TreeTemplates = bTrees ? MakeTreeTemplates() : TArray<FVoxelTreeTemplate>();
			Config->TreeDensity = bTrees ? 4.0f : 0.0f;
			Config->TreeMode = EVoxelTreeMode::VoxelData;
			ClampLODBandsToViewDistance();

			SpawnManager();
			if (Zone.InnerRadius > 0.0f)
			{
				Manager->AddConditioningZone(Zone);
			}
			InitializeManager();

			const FVector Viewer = Config->WorldOrigin;
			const float SurfaceZ = Manager->GetGeneratedSurfacePoint(FVector(Viewer.X, Viewer.Y, 0.0)).Z;
			SetViewer(FVector(Viewer.X, Viewer.Y, SurfaceZ + 200.0));
		}

		/** Tick until the pipeline has been idle for 60 ticks; false if it does not settle within 90 s. */
		bool Settle()
		{
			SettleIdleTicks = 60;
			return FTestChunkWorld::Settle(90.0);
		}

		/** Centre of a global voxel's cell. */
		FVector VoxelCentre(int32 I, int32 J, int64 K) const
		{
			return Config->WorldOrigin + FVector(I + 0.5, J + 0.5, static_cast<double>(K) + 0.5) * Config->VoxelSize;
		}

		/** The global voxel column (i, j) containing a world position. */
		FIntPoint ColumnOf(double X, double Y) const
		{
			return FIntPoint(FMath::FloorToInt((X - Config->WorldOrigin.X) / Config->VoxelSize), FMath::FloorToInt((Y - Config->WorldOrigin.Y) / Config->VoxelSize));
		}

		/**
		 * The reference: the topmost voxel of a column that is solid with air above it, read from the streamed voxels
		 * (generated, or edit-merged), scanning layers [Low, High]. False when the column holds no such voxel there.
		 */
		bool ScanTopLayer(const FIntPoint& Column, int64 Low, int64 High, bool bEditMerged, int64& OutLayer) const
		{
			bool bAbove = false;
			for (int64 K = High; K >= Low; --K)
			{
				const FVector P = VoxelCentre(Column.X, Column.Y, K);
				const FIntVector Chunk = FVoxelCoordinates::WorldToChunk(P - Config->WorldOrigin, Config->ChunkSize, Config->VoxelSize);
				if (!Manager->HasChunkVoxelData(Chunk))
				{
					return false; // an unloaded chunk in the window: no reference here
				}
				const FVoxelData Voxel = bEditMerged ? Manager->GetEditMergedVoxelAtWorldPosition(P) : Manager->GetVoxelAtWorldPosition(P);
				if (Voxel.IsSolid() && !bAbove)
				{
					OutLayer = K;
					return true;
				}
				bAbove = Voxel.IsSolid();
			}
			return false;
		}
	};

	/** The test's pad: centre, radii and a target off the voxel grid. */
	FVoxelConditioningZone MakePad(const FVector& Origin)
	{
		return FVoxelConditioningZone(FVector2D(Origin.X + 600.0, Origin.Y - 450.0), 400.0f, 500.0f, static_cast<float>(Origin.Z) + 213.7f, 1.0f);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCubicBlockSurfaceGeneratedTest,
	"VoxelWorlds.Cubic.BlockSurface.Generated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelCubicBlockSurfaceGeneratedTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCubicGameplayTestUtils;

	FGameplayWorld Test;
	const FVoxelConditioningZone Pad = MakePad(FVector(1234.5, -543.25, 37.5));
	Test.Start(TEXT("VoxelCubicBlockSurfaceWorld"), EMeshingMode::Cubic, false, Pad);
	TestTrue(TEXT("The pipeline settled"), Test.Settle());
	UVoxelChunkManager* Manager = Test.Manager;
	const UVoxelWorldConfiguration* Config = Test.Config;
	const float V = Config->VoxelSize;
	const FVector& Origin = Config->WorldOrigin;
	TestTrue(TEXT("A cubic heightmap world has a block surface"), Manager->IsBlockSurface());

	// Random columns around the viewer, every one compared with a scan of the voxels streamed there.
	FRandomStream Rng(77);
	int32 Compared = 0, GeneratedOk = 0, EditMergedOk = 0, IsoBelowTop = 0, InPad = 0, InPadOk = 0;
	float WorstIsoGap = 0.0f, LeastIsoGap = TNumericLimits<float>::Max();
	for (int32 Sample = 0; Sample < 600; ++Sample)
	{
		const double X = Origin.X + Rng.FRandRange(-1500.0f, 1500.0f);
		const double Y = Origin.Y + Rng.FRandRange(-1500.0f, 1500.0f);
		const float Generated = Manager->GetGeneratedSurfacePoint(FVector(X, Y, 0.0)).Z;
		const FIntPoint Column = Test.ColumnOf(X, Y);
		const int64 Guess = static_cast<int64>(FMath::FloorToDouble((Generated - Origin.Z) / V));
		int64 TopLayer = 0;
		if (!Test.ScanTopLayer(Column, Guess - 24, Guess + 24, /*bEditMerged=*/false, TopLayer))
		{
			continue;
		}
		++Compared;
		const float Expected = static_cast<float>(FVoxelBlockSurface::LayerTop(TopLayer, Origin.Z, V));
		GeneratedOk += FMath::IsNearlyEqual(Generated, Expected, 0.01f) ? 1 : 0;

		FVoxelSurfaceSample Hit;
		const bool bLoaded = Manager->QueryEditMergedSurfacePoint(FVector(X, Y, 0.0), Hit);
		const float Height = static_cast<float>(Hit.Position.Z);
		const bool bEditMergedOk = bLoaded && FMath::IsNearlyEqual(Height, Expected, 0.01f);
		EditMergedOk += bEditMergedOk ? 1 : 0;
		if (!bEditMergedOk && Compared - EditMergedOk <= 5)
		{
			AddInfo(FString::Printf(TEXT("Edit-merged mismatch at (%.1f, %.1f): loaded %d, height %.2f, expected %.2f, generated %.2f"),
				X, Y, bLoaded ? 1 : 0, Height, Expected, Generated));
		}

		// What the isosurface (the query before WM-C5) gave for the column, at its sample point: up to a block under the top.
		const float Iso = static_cast<float>(Manager->GetWorldMode()->GetSurfaceOnUpLine(
			FVector(FVoxelBlockSurface::ColumnCorner(X, Origin.X, V), FVoxelBlockSurface::ColumnCorner(Y, Origin.Y, V), 0.0),
			Config->NoiseParams).Position.Z);
		if (FVector2D::Distance(FVector2D(X, Y), Pad.Center) > Pad.InnerRadius + Pad.FalloffWidth)
		{
			IsoBelowTop += Expected - Iso > 1.0f ? 1 : 0;
			WorstIsoGap = FMath::Max(WorstIsoGap, Expected - Iso);
			LeastIsoGap = FMath::Min(LeastIsoGap, Expected - Iso);
		}

		// Inside the pad's core every column tops out where the pad's height does.
		if (FVector2D::Distance(FVector2D(X, Y), Pad.Center) < Pad.InnerRadius - 2.0f * V)
		{
			++InPad;
			InPadOk += FMath::IsNearlyEqual(Expected, Manager->SnapToBlockTop(Pad.TargetHeight), 0.01f) ? 1 : 0;
		}
	}
	AddInfo(FString::Printf(TEXT("%d columns compared: generated %d, edit-merged %d match the streamed voxels; the isosurface sat under "
		"the block top at %d (by %.1f to %.1f uu); pad core %d of %d at SnapToBlockTop(target)"),
		Compared, GeneratedOk, EditMergedOk, IsoBelowTop, LeastIsoGap, WorstIsoGap, InPadOk, InPad));
	TestTrue(TEXT("Enough columns compared"), Compared >= 400);
	TestEqual(TEXT("GetGeneratedSurfacePoint is the streamed top face at every column"), GeneratedOk, Compared);
	TestEqual(TEXT("QueryEditMergedSurface is the streamed top face at every column"), EditMergedOk, Compared);
	TestTrue(TEXT("The isosurface lay under the block tops (the KC6 gap)"), IsoBelowTop > Compared / 4);
	TestTrue(TEXT("Each block top lies above its column's isosurface, within a block"), LeastIsoGap > -0.01f && WorstIsoGap <= V + 0.01f);
	TestTrue(TEXT("Some columns in the pad's core"), InPad > 0);
	TestEqual(TEXT("The pad's core tops out at SnapToBlockTop of its height"), InPadOk, InPad);
	TestTrue(TEXT("SnapToBlockTop lies above its height, within a block"),
		Manager->SnapToBlockTop(Pad.TargetHeight) >= Pad.TargetHeight - 0.01f && Manager->SnapToBlockTop(Pad.TargetHeight) <= Pad.TargetHeight + V + 0.01f);

	// Exactly at the density's rounding: a height a hair under a voxel's bottom face still fills that voxel.
	const double Face = Origin.Z + 12.0 * V;
	TestEqual(TEXT("A height on a voxel face fills the voxel above it"), FVoxelBlockSurface::BlockTopForHeight(static_cast<float>(Face), Origin.Z, V), Face + V);
	TestEqual(TEXT("A height mid-voxel tops out at that voxel"), FVoxelBlockSurface::BlockTopForHeight(static_cast<float>(Face - 0.5 * V), Origin.Z, V), Face);

	Test.Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCubicBlockSurfaceEditMergedTest,
	"VoxelWorlds.Cubic.BlockSurface.EditMerged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelCubicBlockSurfaceEditMergedTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCubicGameplayTestUtils;

	FGameplayWorld Test;
	Test.Start(TEXT("VoxelCubicBlockSurfaceEditWorld"), EMeshingMode::Cubic, true, FVoxelConditioningZone(FVector2D::ZeroVector, 0.0f, 0.0f, 0.0f));
	TestTrue(TEXT("The pipeline settled"), Test.Settle());
	UVoxelChunkManager* Manager = Test.Manager;
	UVoxelEditManager* Edits = Manager->GetEditManager();
	const float V = Test.Config->VoxelSize;
	const FVector& Origin = Test.Config->WorldOrigin;
	const uint8 Leaf = Test.Config->TreeTemplates[0].LeafMaterialID;
	const uint8 Trunk = Test.Config->TreeTemplates[0].TrunkMaterialID;

	auto Query = [Manager](double X, double Y, float& OutHeight)
	{
		FVoxelSurfaceSample Hit;
		if (!Manager->QueryEditMergedSurfacePoint(FVector(X, Y, 0.0), Hit))
		{
			return false;
		}
		OutHeight = static_cast<float>(Hit.Position.Z);
		return true;
	};

	// Columns away from every tree (no tree block within the scanned window), so the edits below decide the surface.
	FRandomStream Rng(5);
	int32 Checked = 0;
	for (int32 Try = 0; Try < 200 && Checked < 6; ++Try)
	{
		const double X = Origin.X + Rng.FRandRange(-1200.0f, 1200.0f);
		const double Y = Origin.Y + Rng.FRandRange(-1200.0f, 1200.0f);
		const FIntPoint Column = Test.ColumnOf(X, Y);
		float Top = 0.0f;
		if (!Query(X, Y, Top))
		{
			continue;
		}
		const int64 TopLayer = static_cast<int64>(FMath::RoundToDouble((Top - Origin.Z) / V)) - 1;
		bool bTreeNear = false;
		for (int64 K = TopLayer - 2; K <= TopLayer + 16 && !bTreeNear; ++K)
		{
			const uint8 M = Manager->GetVoxelAtWorldPosition(Test.VoxelCentre(Column.X, Column.Y, K)).MaterialID;
			bTreeNear = (M == Leaf || M == Trunk) && Manager->GetVoxelAtWorldPosition(Test.VoxelCentre(Column.X, Column.Y, K)).IsSolid();
		}
		if (bTreeNear)
		{
			continue;
		}
		++Checked;
		const double CX = Origin.X + (Column.X + 0.5) * V;
		const double CY = Origin.Y + (Column.Y + 0.5) * V;
		auto Describe = [&](const TCHAR* Step)
		{
			FString Layers;
			for (int64 K = TopLayer - 2; K <= TopLayer + 8; ++K)
			{
				const FVector P = Test.VoxelCentre(Column.X, Column.Y, K);
				const FIntVector Chunk = FVoxelCoordinates::WorldToChunk(P - Origin, Test.Config->ChunkSize, V);
				const FVoxelData Voxel = Manager->GetEditMergedVoxelAtWorldPosition(P);
				Layers += FString::Printf(TEXT(" %lld:%d/%d%s%s"), K, Voxel.Density, Voxel.MaterialID, Manager->HasChunkVoxelData(Chunk) ? TEXT("") : TEXT("(nodata)"),
					Manager->IsChunkLoaded(Chunk) ? TEXT("") : TEXT("(unloaded)"));
			}
			AddInfo(FString::Printf(TEXT("%s at column (%d, %d), top layer %lld:%s"), Step, Column.X, Column.Y, TopLayer, *Layers));
		};

		// A block placed on top raises the surface by one block.
		Edits->BeginEditOperation(TEXT("Test place"), false);
		Edits->ApplyEdit(Test.VoxelCentre(Column.X, Column.Y, TopLayer + 1), FVoxelData(3, 255), EEditMode::Set);
		Edits->EndEditOperation();
		float Height = 0.0f;
		if (!TestTrue(TEXT("Place: loaded"), Query(X, Y, Height)) || !TestEqual(TEXT("Place: one block higher"), Height, Top + V, 0.01f))
		{
			Describe(TEXT("Place"));
		}

		// Leaves floating above are not ground (a canopy is walked under); a trunk block is.
		Edits->BeginEditOperation(TEXT("Test leaves"), false);
		Edits->ApplyEdit(Test.VoxelCentre(Column.X, Column.Y, TopLayer + 4), FVoxelData(Leaf, 255), EEditMode::Set);
		Edits->EndEditOperation();
		TestTrue(TEXT("Leaves: loaded"), Query(CX, CY, Height));
		TestEqual(TEXT("Leaves above do not count as ground"), Height, Top + V, 0.01f);
		Edits->BeginEditOperation(TEXT("Test trunk"), false);
		Edits->ApplyEdit(Test.VoxelCentre(Column.X, Column.Y, TopLayer + 6), FVoxelData(Trunk, 255), EEditMode::Set);
		Edits->EndEditOperation();
		if (!TestTrue(TEXT("Trunk: loaded"), Query(CX, CY, Height))
			|| !TestEqual(TEXT("A trunk block is ground"), Height, static_cast<float>(FVoxelBlockSurface::LayerTop(TopLayer + 6, Origin.Z, V)), 0.01f))
		{
			Describe(TEXT("Trunk"));
		}

		// Dig the trunk, the placed block and the top two natural blocks: two blocks under the original top.
		Edits->BeginEditOperation(TEXT("Test dig"), false);
		for (int64 K = TopLayer - 1; K <= TopLayer + 6; ++K)
		{
			if (K != TopLayer + 4)
			{
				Edits->ApplyEdit(Test.VoxelCentre(Column.X, Column.Y, K), FVoxelData::Air(), EEditMode::Set);
			}
		}
		Edits->EndEditOperation();
		TestTrue(TEXT("Dig: loaded"), Query(X, Y, Height));
		TestEqual(TEXT("Dig: two blocks under the original top"), Height, Top - 2.0f * V, 0.01f);
		int64 Reference = 0;
		TestTrue(TEXT("Dig: the reference scan finds the leaf over the pit as the top solid voxel"),
			Test.ScanTopLayer(Column, TopLayer - 12, TopLayer + 12, /*bEditMerged=*/true, Reference) && Reference == TopLayer + 4);
	}
	TestTrue(TEXT("Columns checked"), Checked >= 4);

	Test.Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCubicBlockSurfaceSmoothUnchangedTest,
	"VoxelWorlds.Cubic.BlockSurface.SmoothUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelCubicBlockSurfaceSmoothUnchangedTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCubicGameplayTestUtils;

	// The same world meshed smooth: no block surface, and the queries are the isosurface as they were before WM-C5.
	FGameplayWorld Test;
	const FVoxelConditioningZone Pad = MakePad(FVector(1234.5, -543.25, 37.5));
	Test.Start(TEXT("VoxelSmoothSurfaceWorld"), EMeshingMode::DualContouring, false, Pad);
	TestTrue(TEXT("The pipeline settled"), Test.Settle());
	UVoxelChunkManager* Manager = Test.Manager;
	const UVoxelWorldConfiguration* Config = Test.Config;
	const IVoxelWorldMode* WM = Manager->GetWorldMode();
	TestFalse(TEXT("A smooth world has no block surface"), Manager->IsBlockSurface());
	TestEqual(TEXT("SnapToBlockTop is the identity in a smooth world"), Manager->SnapToBlockTop(-123.456f), -123.456f);

	FRandomStream Rng(31);
	int32 Generated = 0, EditMerged = 0, Loaded = 0;
	for (int32 Sample = 0; Sample < 300; ++Sample)
	{
		const double X = Config->WorldOrigin.X + Rng.FRandRange(-1500.0f, 1500.0f);
		const double Y = Config->WorldOrigin.Y + Rng.FRandRange(-1500.0f, 1500.0f);

		// GetGeneratedSurfacePoint: the world mode's height with the zones blended in, bit for bit.
		const float Natural = static_cast<float>(WM->GetSurfaceOnUpLine(FVector(X, Y, 0.0), Config->NoiseParams).Position.Z);
		const float Expected = FVoxelTerrainConditioning::ApplyToHeight(X, Y, Natural, { Pad });
		Generated += Manager->GetGeneratedSurfacePoint(FVector(X, Y, 0.0)).Z == Expected ? 1 : 0;

		// QueryEditMergedSurfacePoint: the column at (X, Y) from one chunk under the estimate to one above, interpolated
		// where the density crosses the threshold (FVoxelSurfaceQuery::ExtractSurfaceFromColumn), as before.
		FVoxelSurfaceSample Hit;
		if (!Manager->QueryEditMergedSurfacePoint(FVector(X, Y, 0.0), Hit))
		{
			continue;
		}
		const float Height = static_cast<float>(Hit.Position.Z);
		const uint8 Material = Hit.MaterialID;
		++Loaded;
		const int32 Window = Config->ChunkSize;
		const float BaseZ = Natural - Window * Config->VoxelSize;
		TArray<FVoxelData> Column;
		for (int32 i = 0; i < 2 * Window + 1; ++i)
		{
			Column.Add(Manager->GetEditMergedVoxelAtWorldPosition(FVector(X, Y, BaseZ + i * Config->VoxelSize)));
		}
		float RefHeight = 0.0f;
		uint8 RefMaterial = 0, RefBiome = 0;
		const bool bRef = FVoxelSurfaceQuery::ExtractSurfaceFromColumn(Column, BaseZ, Config->VoxelSize, RefHeight, RefMaterial, RefBiome);
		EditMerged += bRef && Height == RefHeight && Material == RefMaterial ? 1 : 0;
	}
	AddInfo(FString::Printf(TEXT("generated %d / 300, edit-merged %d / %d as before"), Generated, EditMerged, Loaded));
	TestEqual(TEXT("GetGeneratedSurfacePoint unchanged in a smooth world"), Generated, 300);
	TestTrue(TEXT("Loaded columns sampled"), Loaded >= 200);
	TestEqual(TEXT("QueryEditMergedSurfacePoint unchanged in a smooth world"), EditMerged, Loaded);

	Test.Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCubicVoxelTreesFindAndFellTest,
	"VoxelWorlds.Cubic.VoxelTrees.FindAndFell",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelCubicVoxelTreesFindAndFellTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCubicGameplayTestUtils;

	FGameplayWorld Test;
	Test.Start(TEXT("VoxelCubicTreesWorld"), EMeshingMode::Cubic, true, FVoxelConditioningZone(FVector2D::ZeroVector, 0.0f, 0.0f, 0.0f));
	TestTrue(TEXT("The pipeline settled"), Test.Settle());
	UVoxelChunkManager* Manager = Test.Manager;
	const UVoxelWorldConfiguration* Config = Test.Config;
	TestTrue(TEXT("The world injects voxel trees"), Manager->HasVoxelTrees());

	// The trees the injector places on the chunks around the viewer, as it computes them.
	struct FPlaced { FVoxelTreeInstance Tree; TArray<FVoxelTreeVoxel> Stamp; };
	TArray<FPlaced> Placed;
	const int32 N = Config->ChunkSize;
	for (int32 CX = -1; CX <= 0; ++CX)
	{
		for (int32 CY = -1; CY <= 0; ++CY)
		{
			TArray<FIntVector> Positions;
			TArray<int32> Templates;
			TArray<uint32> Seeds;
			FVoxelTreeInjector::ComputeTreePositionsForChunk(FIntVector(CX, CY, 0), N, Config->VoxelSize, Config->WorldOrigin, Config->WorldSeed,
				Config->NoiseParams, *Manager->GetWorldMode(), Config->TreeDensity, Config->TreeTemplates, nullptr, false, 0.0f,
				Positions, Templates, Seeds);
			for (int32 i = 0; i < Positions.Num(); ++i)
			{
				FPlaced& P = Placed.AddDefaulted_GetRef();
				P.Tree.BaseVoxel = Positions[i];
				P.Tree.TemplateIndex = Templates[i];
				P.Tree.Seed = Seeds[i];
				FVoxelTreeInjector::CollectTreeVoxels(P.Tree.BaseVoxel, Config->TreeTemplates[P.Tree.TemplateIndex], P.Tree.Seed, P.Stamp);
			}
		}
	}
	TestTrue(TEXT("Trees placed around the viewer"), Placed.Num() >= 8);

	// Every trunk block a tree still has finds that tree; its blocks are all in the world as stamped.
	int32 Present = 0, TrunkBlocks = 0, Found = 0, Leaves = 0;
	FPlaced* ToFell = nullptr;
	for (FPlaced& P : Placed)
	{
		TArray<FVoxelTreeVoxel> Blocks;
		Manager->GetVoxelTreeBlocks(P.Tree, Blocks);
		const TArray<FVoxelTreeVoxel> Trunk = Blocks.FilterByPredicate([](const FVoxelTreeVoxel& B) { return B.bTrunk; });
		if (Trunk.Num() < 3)
		{
			continue; // skipped by the stamp (under an overhang) or crowded out by a neighbour
		}
		++Present;
		Leaves += Blocks.Num() - Trunk.Num();
		for (const FVoxelTreeVoxel& B : Trunk)
		{
			++TrunkBlocks;
			FVoxelTreeInstance Hit;
			if (Manager->FindVoxelTreeAt(Manager->GlobalVoxelToWorldCentre(B.GlobalVoxel), Hit) && Hit.BaseVoxel == P.Tree.BaseVoxel
				&& Hit.TemplateIndex == P.Tree.TemplateIndex)
			{
				++Found;
			}
		}
		if (!ToFell && Trunk.Num() >= 4 && Blocks.Num() > Trunk.Num())
		{
			ToFell = &P;
		}
	}
	AddInfo(FString::Printf(TEXT("%d of %d placed trees present; %d of %d trunk blocks found their tree; %d leaf blocks"),
		Present, Placed.Num(), Found, TrunkBlocks, Leaves));
	TestTrue(TEXT("Trees present in the streamed voxels"), Present >= 4);
	TestTrue(TEXT("Leaves present"), Leaves > Present * 4);
	TestTrue(TEXT("Nearly every trunk block finds its own tree (touching trunks may resolve to the neighbour)"), Found >= TrunkBlocks * 95 / 100);

	// Ground and leaves are no tree's trunk.
	if (ToFell)
	{
		const FVoxelTreeVoxel* Leaf = ToFell->Stamp.FindByPredicate([](const FVoxelTreeVoxel& B) { return !B.bTrunk; });
		FVoxelTreeInstance None;
		const FIntVector Under = ToFell->Tree.BaseVoxel - FIntVector(0, 0, 2);
		TestFalse(TEXT("The ground under a tree is no trunk"), Manager->FindVoxelTreeAt(Manager->GlobalVoxelToWorldCentre(Under), None));
		if (Leaf && Manager->GetEditMergedVoxelAtWorldPosition(Manager->GlobalVoxelToWorldCentre(Leaf->GlobalVoxel)).MaterialID == Leaf->MaterialID)
		{
			TestFalse(TEXT("A leaf block is no trunk"), Manager->FindVoxelTreeAt(Manager->GlobalVoxelToWorldCentre(Leaf->GlobalVoxel), None));
		}
	}

	// Fell one tree: its blocks go but the stump; every other tree's trunk stays.
	if (TestNotNull(TEXT("A tree to fell"), ToFell))
	{
		TArray<FVoxelTreeVoxel> Before;
		Manager->GetVoxelTreeBlocks(ToFell->Tree, Before);
		TMap<FIntVector, int32> OtherTrunks;
		for (const FPlaced& P : Placed)
		{
			if (&P != ToFell)
			{
				TArray<FVoxelTreeVoxel> Blocks;
				Manager->GetVoxelTreeBlocks(P.Tree, Blocks);
				for (const FVoxelTreeVoxel& B : Blocks)
				{
					if (B.bTrunk && !Before.ContainsByPredicate([&B](const FVoxelTreeVoxel& Mine) { return Mine.GlobalVoxel == B.GlobalVoxel; }))
					{
						OtherTrunks.Add(B.GlobalVoxel, B.MaterialID);
					}
				}
			}
		}
		const int32 Stump = Before.FilterByPredicate([ToFell](const FVoxelTreeVoxel& B) { return B.bTrunk && B.GlobalVoxel.Z == ToFell->Tree.BaseVoxel.Z; }).Num();
		const int32 Removed = Manager->FellVoxelTree(ToFell->Tree, /*bKeepStump=*/true);
		TestEqual(TEXT("Fell: every block but the stump removed"), Removed, Before.Num() - Stump);
		TestTrue(TEXT("Fell: a stump is kept"), Stump >= 1);
		TArray<FVoxelTreeVoxel> After;
		Manager->GetVoxelTreeBlocks(ToFell->Tree, After);
		TestEqual(TEXT("Fell: only the stump is left"), After.Num(), Stump);
		for (const FVoxelTreeVoxel& B : Before)
		{
			const FVoxelData Voxel = Manager->GetEditMergedVoxelAtWorldPosition(Manager->GlobalVoxelToWorldCentre(B.GlobalVoxel));
			const bool bStump = B.bTrunk && B.GlobalVoxel.Z == ToFell->Tree.BaseVoxel.Z;
			if (bStump != Voxel.IsSolid())
			{
				AddError(FString::Printf(TEXT("Fell: block %s is %s"), *B.GlobalVoxel.ToString(), Voxel.IsSolid() ? TEXT("still solid") : TEXT("gone (the stump)")));
			}
		}
		int32 OthersIntact = 0;
		for (const TPair<FIntVector, int32>& Other : OtherTrunks)
		{
			const FVoxelData Voxel = Manager->GetEditMergedVoxelAtWorldPosition(Manager->GlobalVoxelToWorldCentre(Other.Key));
			OthersIntact += Voxel.IsSolid() && Voxel.MaterialID == Other.Value ? 1 : 0;
		}
		TestEqual(TEXT("Fell: every other tree's trunk is intact"), OthersIntact, OtherTrunks.Num());
		TestEqual(TEXT("Fell: felling it again removes nothing"), Manager->FellVoxelTree(ToFell->Tree, true), 0);
		TestEqual(TEXT("Fell: outside the undo history"), Manager->GetEditManager()->GetUndoCount(), 0);
	}

	Test.Stop();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
