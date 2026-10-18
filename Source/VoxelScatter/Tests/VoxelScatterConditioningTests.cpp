// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "VoxelScatterManager.h"
#include "VoxelScatterTypes.h"
#include "VoxelWorldConfiguration.h"
#include "IVoxelWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelData.h"
#include "ChunkRenderData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// World modes review G23 (WORLD_MODES_REVIEW_PLAN.md §14.1): heightmap scatter compares a chunk's top with the
// GENERATED height (the natural height plus the chunk's conditioning zones), not the natural one. A hillside rising
// 0.6 uu per uu along +X with two POI-style zones: a pad cut into the slope (Cut: its uphill pad and ring are lowered by
// up to 2,400 uu) and a pad raised 3,000-5,400 uu over it (Fill). The chunks are made from the generated height with the
// generator's SDF (no caves, biomes or water), and every heightmap path classifies them: the column scan, the cubic
// block-face scan and the GPU path's classifier (over the column scan's points, their classification cleared).
//
// - CutPadAndRing: over every chunk of the cut zone's footprint, no surface point on the generated ground (within 1.5
//   voxels of it) is underground. Against the natural height (no zones in the context: the old test) the chunk holding
//   the cut ground was called buried, and its ground points were underground (no surface scatter).
// - FilledPad: a cave pocket at the top of the chunk under the raised pad (its roof the chunk above): its floor and the
//   chunk's top face are underground. Against the natural height that chunk was a surface chunk, so its floor and the
//   sheet of points on its top face (inside the rock) were open surface.
// - ManagerWiring: through the scatter manager's own pipeline (OnChunkMeshDataReady -> the extraction context built
//   with the conditioning gatherer), a cut chunk keeps its ground as surface; with no gatherer it is buried.
// ---------------------------------------------------------------------------

namespace VoxelScatterConditioningTestUtils
{
	constexpr int32 ChunkSize = 32;
	constexpr float VoxelSize = 100.0f;
	constexpr float ChunkWorldSize = ChunkSize * VoxelSize;
	constexpr float Slope = 0.6f;

	/** A heightmap world mode whose natural terrain is a plane rising along +X: H = Slope * X. */
	struct FSlopeWorldMode : public IVoxelWorldMode
	{
		virtual float GetTerrainHeightAt(float X, float, const FVoxelNoiseParams&) const override { return Slope * X; }
		virtual bool IsHeightmapBased() const override { return true; }
		virtual float GetDensityAt(const FVector&, int32, float) const override { return 0.0f; }
		virtual FIntVector WorldToChunkCoord(const FVector&, int32, float) const override { return FIntVector::ZeroValue; }
		virtual FVector ChunkCoordToWorld(const FIntVector&, int32, float, int32) const override { return FVector::ZeroVector; }
		virtual int32 GetMinZ() const override { return -100000; }
		virtual int32 GetMaxZ() const override { return 100000; }
		virtual EWorldMode GetWorldModeType() const override { return EWorldMode::InfinitePlane; }
		virtual uint8 GetMaterialAtDepth(const FVector&, float, float) const override { return 0; }
	};

	/** A pad at the natural height of its centre, cut into the slope uphill and filled downhill. */
	FVoxelConditioningZone CutZone() { return FVoxelConditioningZone(FVector2D(5000.0, 0.0), 4000.0f, 6000.0f, Slope * 5000.0f, 1.0f); }

	/** A pad raised over the slope (natural -20,400..-15,600 under it) to -15,000, inside chunk layer -5. */
	FVoxelConditioningZone FillZone() { return FVoxelConditioningZone(FVector2D(-30000.0, 0.0), 4000.0f, 6000.0f, -15000.0f, 1.0f); }

	float GeneratedHeight(double X, double Y, const TArray<FVoxelConditioningZone>& Zones)
	{
		return FVoxelTerrainConditioning::ApplyToHeight(X, Y, Slope * static_cast<float>(X), Zones);
	}

	FVector ChunkOrigin(const FIntVector& Coord) { return FVector(Coord) * ChunkWorldSize; }

	/** A chunk generated from the generated height, as the heightmap generator's SDF makes it (grass, no caves). */
	TArray<FVoxelData> MakeChunk(const FIntVector& Coord, const TArray<FVoxelConditioningZone>& Zones)
	{
		TArray<FVoxelData> Voxels;
		Voxels.SetNumUninitialized(ChunkSize * ChunkSize * ChunkSize);
		const FVector Origin = ChunkOrigin(Coord);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				const float Height = GeneratedHeight(Origin.X + X * VoxelSize, Origin.Y + Y * VoxelSize, Zones);
				for (int32 Z = 0; Z < ChunkSize; ++Z)
				{
					const float SignedDistance = FInfinitePlaneWorldMode::CalculateSignedDistance(static_cast<float>(Origin.Z) + Z * VoxelSize, Height);
					Voxels[X + Y * ChunkSize + Z * ChunkSize * ChunkSize] =
						FVoxelData(EVoxelMaterial::Grass, FInfinitePlaneWorldMode::SignedDistanceToDensity(SignedDistance, VoxelSize));
				}
			}
		}
		return Voxels;
	}

	FScatterExtractionContext MakeContext(const IVoxelWorldMode& Mode, const TArray<FVoxelConditioningZone>* Zones)
	{
		FScatterExtractionContext Context;
		Context.WorldMode = &Mode;
		if (Zones)
		{
			Context.ConditioningZones = MakeShared<const TArray<FVoxelConditioningZone>>(*Zones);
		}
		return Context;
	}

	enum class EPath : uint8 { Column, Cubic, GPUClassifier };
	const TCHAR* PathName(EPath Path)
	{
		return Path == EPath::Column ? TEXT("column scan") : (Path == EPath::Cubic ? TEXT("cubic scan") : TEXT("GPU classifier"));
	}

	/** One heightmap path's surface points for a chunk. */
	TArray<FVoxelSurfacePoint> Extract(EPath Path, const TArray<FVoxelData>& Voxels, const FIntVector& Coord, const FScatterExtractionContext& Context)
	{
		FChunkSurfaceData Surface;
		UVoxelScatterManager::ExtractHeightmapSurfacePoints(Voxels, Coord, ChunkOrigin(Coord), ChunkSize, VoxelSize, VoxelSize,
			Path == EPath::Cubic, Context, Surface);
		if (Path == EPath::GPUClassifier)
		{
			for (FVoxelSurfacePoint& Point : Surface.SurfacePoints)
			{
				Point.bIsUnderground = false;
				Point.bIsUnderwater = false;
			}
			UVoxelScatterManager::ClassifySurfacePointsUnderground(Surface.SurfacePoints, Voxels, ChunkOrigin(Coord), ChunkSize, VoxelSize, Context);
		}
		return Surface.SurfacePoints;
	}

	/** The chunks of a zone's footprint: every column under it, the layers from below the lower of the two heights to above the higher. */
	TArray<FIntVector> FootprintChunks(const FVoxelConditioningZone& Zone, const TArray<FVoxelConditioningZone>& Zones)
	{
		const double Reach = Zone.InnerRadius + Zone.FalloffWidth;
		const int32 MinX = FMath::FloorToInt((Zone.Center.X - Reach) / ChunkWorldSize);
		const int32 MaxX = FMath::FloorToInt((Zone.Center.X + Reach) / ChunkWorldSize);
		const int32 MinY = FMath::FloorToInt((Zone.Center.Y - Reach) / ChunkWorldSize);
		const int32 MaxY = FMath::FloorToInt((Zone.Center.Y + Reach) / ChunkWorldSize);
		TArray<FIntVector> Chunks;
		for (int32 CY = MinY; CY <= MaxY; ++CY)
		{
			for (int32 CX = MinX; CX <= MaxX; ++CX)
			{
				float Low = TNumericLimits<float>::Max(), High = TNumericLimits<float>::Lowest();
				for (int32 I = 0; I <= 4; ++I)
				{
					for (int32 J = 0; J <= 4; ++J)
					{
						const double X = (CX + I * 0.25) * ChunkWorldSize;
						const double Y = (CY + J * 0.25) * ChunkWorldSize;
						const float Natural = Slope * static_cast<float>(X);
						const float Generated = GeneratedHeight(X, Y, Zones);
						Low = FMath::Min3(Low, Natural, Generated);
						High = FMath::Max3(High, Natural, Generated);
					}
				}
				for (int32 CZ = FMath::FloorToInt(Low / ChunkWorldSize) - 1; CZ <= FMath::FloorToInt(High / ChunkWorldSize) + 1; ++CZ)
				{
					Chunks.Add(FIntVector(CX, CY, CZ));
				}
			}
		}
		return Chunks;
	}

	/** Private game world + initialized scatter manager (CPU extraction), torn down on destruction. */
	struct FHarness
	{
		UWorld* World = nullptr;
		UVoxelWorldConfiguration* Config = nullptr;
		UVoxelScatterManager* Manager = nullptr;

		FHarness()
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelScatterConditioningTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			Config = NewObject<UVoxelWorldConfiguration>();
			Config->ChunkSize = ChunkSize;
			Config->VoxelSize = VoxelSize;
			Config->ScatterRadius = 100000.0f;
			Config->bUseGPUScatterExtraction = false;
			Manager = NewObject<UVoxelScatterManager>();
			Manager->AddToRoot();
			Manager->Initialize(Config, World);
		}

		~FHarness()
		{
			if (Manager)
			{
				Manager->Shutdown();
				Manager->RemoveFromRoot();
			}
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
		}

		/** Submit a chunk as if its mesh just arrived and pump until its surface data is cached. */
		const FChunkSurfaceData* Run(const FIntVector& Coord, const TArray<FVoxelData>& Voxels)
		{
			const FChunkMeshData EmptyMesh; // CPU voxel-based extraction ignores mesh data
			Manager->OnChunkMeshDataReady(Coord, 0, EmptyMesh, Voxels, ChunkSize, VoxelSize);
			const double Deadline = FPlatformTime::Seconds() + 15.0;
			while (FPlatformTime::Seconds() < Deadline)
			{
				Manager->Update(FVector::ZeroVector, 0.05f);
				if (Manager->GetPendingGenerationCount() == 0 && Manager->GetChunkSurfaceData(Coord))
				{
					break;
				}
			}
			return Manager->GetChunkSurfaceData(Coord);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterConditionedCutTest,
	"VoxelWorlds.Scatter.Conditioning.CutPadAndRing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterConditionedCutTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterConditioningTestUtils;
	const FSlopeWorldMode Mode;
	const TArray<FVoxelConditioningZone> Zones = { CutZone() };
	const FScatterExtractionContext Generated = MakeContext(Mode, &Zones);
	const FScatterExtractionContext Natural = MakeContext(Mode, nullptr);
	const TArray<FIntVector> Chunks = FootprintChunks(Zones[0], Zones);

	for (const EPath Path : { EPath::Column, EPath::Cubic, EPath::GPUClassifier })
	{
		int32 Ground = 0, GroundUnder = 0, NaturalGround = 0, NaturalGroundUnder = 0, CutGround = 0;
		TSet<FIntVector> NaturalBuriedChunks;
		for (const FIntVector& Coord : Chunks)
		{
			const TArray<FVoxelData> Voxels = MakeChunk(Coord, Zones);
			for (const bool bGenerated : { true, false })
			{
				for (const FVoxelSurfacePoint& Point : Extract(Path, Voxels, Coord, bGenerated ? Generated : Natural))
				{
					const float GroundZ = GeneratedHeight(Point.Position.X, Point.Position.Y, Zones);
					if (FMath::Abs(static_cast<float>(Point.Position.Z) - GroundZ) > VoxelSize * 1.5f)
					{
						continue;
					}
					if (bGenerated)
					{
						++Ground;
						GroundUnder += Point.bIsUnderground ? 1 : 0;
						CutGround += Slope * Point.Position.X - GroundZ > VoxelSize * 1.5f ? 1 : 0;
					}
					else
					{
						++NaturalGround;
						if (Point.bIsUnderground)
						{
							++NaturalGroundUnder;
							NaturalBuriedChunks.Add(Coord);
						}
					}
				}
			}
		}
		AddInfo(FString::Printf(TEXT("%s: %d chunks; generated height: %d ground points (%d where the ground is cut below the natural height), %d underground; natural height: %d of %d underground in %d chunks"),
			PathName(Path), Chunks.Num(), Ground, CutGround, GroundUnder, NaturalGroundUnder, NaturalGround, NaturalBuriedChunks.Num()));
		TestTrue(*FString::Printf(TEXT("%s: the footprint has ground points where the ground is cut (non-vacuous)"), PathName(Path)), CutGround > 500);
		TestEqual(*FString::Printf(TEXT("%s: no point on the generated ground is underground"), PathName(Path)), GroundUnder, 0);
		TestTrue(*FString::Printf(TEXT("%s: against the natural height the cut ground was buried (the test reproduces G23)"), PathName(Path)),
			NaturalGroundUnder > 100);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterConditionedFillTest,
	"VoxelWorlds.Scatter.Conditioning.FilledPad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterConditionedFillTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterConditioningTestUtils;
	const FSlopeWorldMode Mode;
	const TArray<FVoxelConditioningZone> Zones = { FillZone() };
	const FScatterExtractionContext Generated = MakeContext(Mode, &Zones);
	const FScatterExtractionContext Natural = MakeContext(Mode, nullptr);

	// The chunk under the pad's ground (-15,000 lies in layer -5): layer -6, [-19,200, -16,000], under the pad's inner
	// radius (X -32,000 to -28,800, Y 0 to 3,200) and so fully solid, with a pocket at its top (local Z 27-31) over voxels
	// X 22-28, Y 10-16 whose roof is the chunk above. The natural height under it is -19,200 to -17,280.
	const FIntVector Coord(-10, 0, -6);
	TArray<FVoxelData> Voxels = MakeChunk(Coord, Zones);
	int32 Solid = 0;
	for (const FVoxelData& Voxel : Voxels)
	{
		Solid += Voxel.IsSolid() ? 1 : 0;
	}
	TestEqual(TEXT("The chunk under the raised pad is solid"), Solid, ChunkSize * ChunkSize * ChunkSize);
	for (int32 Z = 27; Z < ChunkSize; ++Z)
	{
		for (int32 Y = 10; Y <= 16; ++Y)
		{
			for (int32 X = 22; X <= 28; ++X)
			{
				Voxels[X + Y * ChunkSize + Z * ChunkSize * ChunkSize] = FVoxelData::Air();
			}
		}
	}

	const float ChunkTop = static_cast<float>(ChunkOrigin(Coord).Z) + ChunkWorldSize;
	for (const EPath Path : { EPath::Column, EPath::Cubic, EPath::GPUClassifier })
	{
		for (const bool bGenerated : { true, false })
		{
			int32 Floor = 0, FloorUnder = 0, Top = 0, TopUnder = 0;
			for (const FVoxelSurfacePoint& Point : Extract(Path, Voxels, Coord, bGenerated ? Generated : Natural))
			{
				const bool bTop = Point.Position.Z > ChunkTop - VoxelSize;
				(bTop ? Top : Floor) += 1;
				(bTop ? TopUnder : FloorUnder) += Point.bIsUnderground ? 1 : 0;
			}
			AddInfo(FString::Printf(TEXT("%s, %s height: pocket floor %d points (%d underground), chunk top face %d points (%d underground)"),
				PathName(Path), bGenerated ? TEXT("generated") : TEXT("natural"), Floor, FloorUnder, Top, TopUnder));
			TestTrue(*FString::Printf(TEXT("%s: the pocket has a floor (non-vacuous)"), PathName(Path)), Floor > 0);
			if (bGenerated)
			{
				TestEqual(*FString::Printf(TEXT("%s: the pocket floor under the raised pad is underground"), PathName(Path)), FloorUnder, Floor);
				TestEqual(*FString::Printf(TEXT("%s: the chunk's top face under the raised pad is underground"), PathName(Path)), TopUnder, Top);
			}
			else
			{
				TestEqual(*FString::Printf(TEXT("%s: against the natural height the pocket floor was open surface (the test reproduces G23)"), PathName(Path)),
					FloorUnder, 0);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterConditioningWiringTest,
	"VoxelWorlds.Scatter.Conditioning.ManagerWiring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterConditioningWiringTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterConditioningTestUtils;
	const FSlopeWorldMode Mode;
	const TArray<FVoxelConditioningZone> Zones = { CutZone() };

	// X 6,400-9,600 under the cut: the pad (X <= 9,000, ground 3,000) and the ring's first 600 uu (ground <= ~3,080) lie in
	// layer 0 ([0, 3,200]); the natural height, 3,840-5,760, puts its top 490-2,410 uu below the old test's line.
	const FIntVector Coord(2, 0, 0);
	const TArray<FVoxelData> Voxels = MakeChunk(Coord, Zones);

	for (const bool bGatherer : { true, false })
	{
		FHarness Harness;
		Harness.Manager->SetWorldMode(&Mode);
		if (bGatherer)
		{
			// What the chunk manager installs (its region gather over the chunk's XY footprint).
			Harness.Manager->SetConditioningGatherer([&Zones](const FBox& ChunkBox, TArray<FVoxelConditioningZone>& OutZones)
			{
				for (const FVoxelConditioningZone& Zone : Zones)
				{
					if (Zone.GetBounds2D().Intersect(FBox2D(FVector2D(ChunkBox.Min.X, ChunkBox.Min.Y), FVector2D(ChunkBox.Max.X, ChunkBox.Max.Y))))
					{
						OutZones.Add(Zone);
					}
				}
			});
		}
		const FChunkSurfaceData* Surface = Harness.Run(Coord, Voxels);
		if (!TestNotNull(TEXT("Surface data extracted"), Surface))
		{
			Harness.Manager->SetWorldMode(nullptr);
			return true;
		}
		int32 Ground = 0, GroundUnder = 0;
		for (const FVoxelSurfacePoint& Point : Surface->SurfacePoints)
		{
			if (FMath::Abs(static_cast<float>(Point.Position.Z) - GeneratedHeight(Point.Position.X, Point.Position.Y, Zones)) <= VoxelSize * 1.5f)
			{
				++Ground;
				GroundUnder += Point.bIsUnderground ? 1 : 0;
			}
		}
		AddInfo(FString::Printf(TEXT("%s: %d ground points, %d underground"), bGatherer ? TEXT("with the gatherer") : TEXT("without"), Ground, GroundUnder));
		TestTrue(TEXT("The chunk holds the cut ground (non-vacuous)"), Ground >= ChunkSize * ChunkSize / 2);
		if (bGatherer)
		{
			TestEqual(TEXT("With the conditioning gatherer the cut ground is surface"), GroundUnder, 0);
		}
		else
		{
			TestEqual(TEXT("Without it (the natural height) the chunk is buried"), GroundUnder, Ground);
		}
		Harness.Manager->SetWorldMode(nullptr);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
