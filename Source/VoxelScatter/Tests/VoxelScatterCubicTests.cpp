// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "VoxelScatterManager.h"
#include "VoxelScatterPlacement.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelData.h"
#include "VoxelMaterialRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Cubic scatter (world modes review WM-C3, KC4): cubic worlds extract block-face
// centres from voxels (every world does since G27 retired the GPU extractor), and a
// BlockFaceSnap definition keeps each instance on the block face it was placed on.
// ---------------------------------------------------------------------------

namespace VoxelScatterCubicTestUtils
{
	constexpr int32 TestChunkSize = 16;
	constexpr float TestVoxelSize = 75.0f;

	/** Flat grass slab filling the bottom half of the chunk. */
	static TArray<FVoxelData> MakeFlatGrassChunk()
	{
		TArray<FVoxelData> Voxels;
		Voxels.Init(FVoxelData::Air(), TestChunkSize * TestChunkSize * TestChunkSize);
		for (int32 Z = 0; Z < TestChunkSize / 2; ++Z)
		{
			for (int32 Y = 0; Y < TestChunkSize; ++Y)
			{
				for (int32 X = 0; X < TestChunkSize; ++X)
				{
					Voxels[X + Y * TestChunkSize + Z * TestChunkSize * TestChunkSize] = FVoxelData::Solid(EVoxelMaterial::Grass);
				}
			}
		}
		return Voxels;
	}

	/** A grid of top-face centres, spaced one voxel apart, as the cubic extractor reports them. */
	static FChunkSurfaceData MakeFaceCentreSurface(int32 Count)
	{
		FChunkSurfaceData Surface(FIntVector::ZeroValue);
		Surface.AveragePointSpacing = TestVoxelSize;
		Surface.bIsValid = true;
		for (int32 Y = 0; Y < Count; ++Y)
		{
			for (int32 X = 0; X < Count; ++X)
			{
				FVoxelSurfacePoint Point;
				Point.Position = FVector((X + 0.5) * TestVoxelSize, (Y + 0.5) * TestVoxelSize, 600.0);
				Point.Normal = FVector::UpVector;
				Point.MaterialID = EVoxelMaterial::Grass;
				Point.FaceType = EVoxelFaceType::Top;
				Point.SlopeAngle = 0.0f;
				Surface.SurfacePoints.Add(Point);
			}
		}
		return Surface;
	}

	/** Largest XY distance of a spawn point from the face centre it came from (the nearest one). */
	static double MaxOffsetFromFaceCentre(const TArray<FScatterSpawnPoint>& Points)
	{
		double MaxOffset = 0.0;
		for (const FScatterSpawnPoint& P : Points)
		{
			const double CX = (FMath::FloorToDouble(P.Position.X / TestVoxelSize) + 0.5) * TestVoxelSize;
			const double CY = (FMath::FloorToDouble(P.Position.Y / TestVoxelSize) + 0.5) * TestVoxelSize;
			MaxOffset = FMath::Max(MaxOffset, FMath::Max(FMath::Abs(P.Position.X - CX), FMath::Abs(P.Position.Y - CY)));
		}
		return MaxOffset;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterCubicCPUExtractionTest,
	"VoxelWorlds.Scatter.Cubic.CPUExtractionOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterCubicCPUExtractionTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterCubicTestUtils;

	// A cubic chunk whose interior mesh would be empty (its only faces lie on its boundary planes, which the
	// seam jobs mesh) gets scatter: extraction reads voxels, never the mesh. bUseGPUScatterExtraction is set,
	// and ignored (G27).
	UVoxelWorldConfiguration* Cubic = NewObject<UVoxelWorldConfiguration>();
	Cubic->MeshingMode = EMeshingMode::Cubic;
	Cubic->bUseGPUScatterExtraction = true;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelScatterCubicTestWorld"));
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);

	Cubic->ChunkSize = TestChunkSize;
	Cubic->VoxelSize = TestVoxelSize;
	Cubic->ScatterRadius = 100000.0f;
	Cubic->MaxAsyncScatterTasks = 2;
	UVoxelScatterManager* Manager = NewObject<UVoxelScatterManager>();
	Manager->AddToRoot();
	Manager->Initialize(Cubic, World);

	const FIntVector Coord(0, 0, 0);
	Manager->OnChunkMeshDataReady(Coord, MakeFlatGrassChunk(), TestChunkSize, TestVoxelSize);

	const double Deadline = FPlatformTime::Seconds() + 15.0;
	while (FPlatformTime::Seconds() < Deadline && Manager->GetChunkSurfaceData(Coord) == nullptr)
	{
		Manager->Update(FVector::ZeroVector, 0.05f);
		FPlatformProcess::Sleep(0.02f);
	}

	const FChunkSurfaceData* Surface = Manager->GetChunkSurfaceData(Coord);
	TestNotNull(TEXT("Cubic chunk with an empty interior mesh got surface data"), Surface);
	if (Surface)
	{
		TestEqual(TEXT("One point per top face"), Surface->SurfacePoints.Num(), TestChunkSize * TestChunkSize);
		bool bAllOnFaceCentres = true;
		for (const FVoxelSurfacePoint& P : Surface->SurfacePoints)
		{
			const double FX = FMath::Fmod(P.Position.X, TestVoxelSize);
			const double FY = FMath::Fmod(P.Position.Y, TestVoxelSize);
			bAllOnFaceCentres &= FMath::IsNearlyEqual(FX, 0.5 * TestVoxelSize, 0.01) && FMath::IsNearlyEqual(FY, 0.5 * TestVoxelSize, 0.01);
		}
		TestTrue(TEXT("Every point sits on a block-face centre"), bAllOnFaceCentres);
	}

	Manager->Shutdown();
	Manager->RemoveFromRoot();
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterBlockFaceSnapTest,
	"VoxelWorlds.Scatter.Cubic.BlockFaceSnapStaysOnFace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterBlockFaceSnapTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterCubicTestUtils;

	const FChunkSurfaceData Surface = MakeFaceCentreSurface(20);

	FScatterDefinition Def;
	Def.ScatterID = 1;
	Def.Name = TEXT("BlockFaceSnapTest");
	Def.bEnabled = true;
	Def.Density = 1.0f;
	Def.PositionJitter = 100.0f; // wider than the 37.5 uu half face
	Def.SurfaceOffset = 0.0f;

	// Interpolated placement keeps the full jitter: instances leave their block.
	Def.PlacementMode = EScatterPlacementMode::SurfaceInterpolated;
	TArray<FScatterSpawnPoint> Interpolated;
	FVoxelScatterPlacement::GenerateSpawnPointsForType(Surface, Def, 1234u, Interpolated);
	TestEqual(TEXT("Density 1 places on every point (interpolated)"), Interpolated.Num(), Surface.SurfacePoints.Num());
	double MaxInterpolated = 0.0;
	for (int32 i = 0; i < Interpolated.Num(); ++i)
	{
		const FVector Offset = Interpolated[i].Position - Surface.SurfacePoints[i].Position;
		MaxInterpolated = FMath::Max(MaxInterpolated, FMath::Max(FMath::Abs(Offset.X), FMath::Abs(Offset.Y)));
	}
	TestTrue(TEXT("Without the snap some instance leaves its face (control)"), MaxInterpolated > 0.5 * TestVoxelSize);

	// Block-face snap: the same seed and jitter, every instance stays within half a face of its centre.
	Def.PlacementMode = EScatterPlacementMode::BlockFaceSnap;
	TArray<FScatterSpawnPoint> Snapped;
	FVoxelScatterPlacement::GenerateSpawnPointsForType(Surface, Def, 1234u, Snapped);
	TestEqual(TEXT("Density 1 places on every point (snapped)"), Snapped.Num(), Surface.SurfacePoints.Num());
	double MaxSnapped = 0.0;
	bool bOnTop = true;
	for (int32 i = 0; i < Snapped.Num(); ++i)
	{
		const FVector Offset = Snapped[i].Position - Surface.SurfacePoints[i].Position;
		MaxSnapped = FMath::Max(MaxSnapped, FMath::Max(FMath::Abs(Offset.X), FMath::Abs(Offset.Y)));
		bOnTop &= FMath::IsNearlyEqual(Offset.Z, 0.0, 0.001);
	}
	TestTrue(TEXT("Every snapped instance stays on its block face"), MaxSnapped <= 0.5 * TestVoxelSize + 0.001);
	TestTrue(TEXT("Snapped instances keep some jitter"), MaxSnapped > 0.25 * TestVoxelSize);
	TestTrue(TEXT("Snapped instances sit on the face (no vertical offset)"), bOnTop);
	TestTrue(TEXT("Nearest face centre of every snapped instance is its own"), MaxOffsetFromFaceCentre(Snapped) <= 0.5 * TestVoxelSize + 0.001);

	// A jitter that already fits inside the face is left as it is.
	Def.PositionJitter = 20.0f;
	TArray<FScatterSpawnPoint> Small;
	FVoxelScatterPlacement::GenerateSpawnPointsForType(Surface, Def, 1234u, Small);
	Def.PlacementMode = EScatterPlacementMode::SurfaceInterpolated;
	TArray<FScatterSpawnPoint> SmallControl;
	FVoxelScatterPlacement::GenerateSpawnPointsForType(Surface, Def, 1234u, SmallControl);
	bool bSame = Small.Num() == SmallControl.Num();
	for (int32 i = 0; bSame && i < Small.Num(); ++i)
	{
		bSame = Small[i].Position.Equals(SmallControl[i].Position, 0.001);
	}
	TestTrue(TEXT("A jitter within half a face is unchanged by the snap"), bSame);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
