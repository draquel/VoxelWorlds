// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "VoxelScatterTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Testing/VoxelScatterTestDefinitions.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "VoxelScatterManager.h"
#include "VoxelScatterTypes.h"
#include "VoxelWorldConfiguration.h"
#include "IVoxelWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelEditTypes.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelData.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"
#include "VoxelWorldModeFactory.h"
#include "VoxelCaveConfiguration.h"
#include "VoxelCaveTypes.h"
#include "VoxelBiomeConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// World modes review G26 (WORLD_MODES_REVIEW_PLAN.md §14.1, §19.4): the voxel over a heightmap chunk's top layer is the
// chunk above's bottom voxel, which the chunk's own data cannot show. The column scan and the cubic scan took it for air,
// so where the ground continues into the chunk above within 1.5 voxels of the chunk's top (the band the buried-chunk test
// lets through) the chunk made a copy of the surface on its top face, up to 1.5 voxels under the ground, as open surface.
// Near the generated ground that voxel is now the one the generator made there (the generated height's density at the
// chunk's top), with the chunk above's edits on its bottom layer applied.
//
// - Contour: a plane rising through a chunk layer's top, from inside the lower chunk to past the band; over the lower chunk
//   and the one above, every column has exactly one open-surface point, on the generated surface, and the lower chunk's
//   top layer has none where the ground continues above it. Both heightmap paths (column scan, cubic scan; the GPU
//   classifier this also ran went with the GPU extractor, G27). With no analytic height (the voxel above taken for air,
//   as before) that top layer holds a row of open-surface points under the ground.
// - FlatGround: flat ground at heights around the layer's top, from inside the top voxel cell to past the band: one
//   open-surface point per column across the two chunks, within 2 uu of the ground; a crossing in the top voxel cell
//   interpolates against the generated voxel above, as the mesher does (against an assumed-air voxel it sat up to half a
//   voxel low).
// - EditsAbove: the chunk above's edits on its bottom layer decide that voxel. A dig there exposes the lower chunk's top
//   face (its points are open surface again); a build over a top face that was the surface covers it (no point). Through
//   the context, and through the scatter manager's own pipeline (SetEditLayerSource -> the extraction context).
// ---------------------------------------------------------------------------

namespace VoxelScatterTopFaceTestUtils
{
	using namespace VoxelScatterTesting;

	constexpr int32 ChunkSize = 32;
	constexpr float VoxelSize = 100.0f;
	constexpr float ChunkWorldSize = ChunkSize * VoxelSize;
	/** Layer 0's top: the lower chunk (0,0,0) holds Z [0, 3,200), the one above (0,0,1) Z [3,200, 6,400). */
	constexpr float LayerTop = ChunkWorldSize;
	/** The band the buried-chunk test lets through: the ground within this of the chunk's top. */
	constexpr float Band = 1.5f * VoxelSize;
	const FIntVector Lower(0, 0, 0);
	const FIntVector Upper(0, 0, 1);

	/** A heightmap world mode whose natural terrain is the plane H = Slope * X + Offset. */
	struct FPlaneWorldMode : public IVoxelWorldMode
	{
		float Slope = 0.0f;
		float Offset = 0.0f;
		FPlaneWorldMode(float InSlope, float InOffset) : Slope(InSlope), Offset(InOffset) {}
		float GetTerrainHeightAt(float X, float, const FVoxelNoiseParams&) const { return Slope * X + Offset; }
		virtual FVoxelTerrainSurfacePoint GetSurfaceOnUpLine(const FVector& P, const FVoxelNoiseParams& N) const override
		{
			FVoxelTerrainSurfacePoint S;
			S.Position = FVector(P.X, P.Y, GetTerrainHeightAt(static_cast<float>(P.X), static_cast<float>(P.Y), N));
			S.Altitude = static_cast<float>(S.Position.Z);
			return S;
		}
		virtual bool IsHeightmapBased() const override { return true; }
		virtual float GetDensityAt(const FVector&, int32, float) const override { return 0.0f; }
		virtual FIntVector WorldToChunkCoord(const FVector&, int32, float) const override { return FIntVector::ZeroValue; }
		virtual FVector ChunkCoordToWorld(const FIntVector&, int32, float, int32) const override { return FVector::ZeroVector; }
		virtual int32 GetMinZ() const override { return -100000; }
		virtual int32 GetMaxZ() const override { return 100000; }
		virtual EWorldMode GetWorldModeType() const override { return EWorldMode::InfinitePlane; }
		virtual uint8 GetMaterialAtDepth(const FVector&, float, float) const override { return 0; }
	};

	FVector ChunkOrigin(const FIntVector& Coord) { return FVector(Coord) * ChunkWorldSize; }

	float HeightAt(const IVoxelWorldMode& Mode, double X)
	{
		return static_cast<float>(Mode.GetSurfaceOnUpLine(FVector(X, 0.0, 0.0), FVoxelNoiseParams()).Position.Z);
	}

	/** A chunk generated from the world mode's height with the heightmap generator's SDF (grass, no caves). */
	TArray<FVoxelData> MakeChunk(const FIntVector& Coord, const IVoxelWorldMode& Mode)
	{
		// The height varies along X only (HeightAt), every column grass.
		return MakeHeightmapChunk(ChunkOrigin(Coord), ChunkSize, VoxelSize,
			[&Mode](double X, double Y) { return HeightAt(Mode, X); },
			[](int32, int32) { return static_cast<uint8>(EVoxelMaterial::Grass); });
	}

	/** Edits on the upper chunk's bottom layer (local Z 0) over columns X, Y in [Min, Max]: set to NewData. */
	FChunkEditLayer MakeBottomPatch(int32 Min, int32 Max, const FVoxelData& NewData)
	{
		FChunkEditLayer Layer(Upper, ChunkSize);
		for (int32 Y = Min; Y <= Max; ++Y)
		{
			for (int32 X = Min; X <= Max; ++X)
			{
				Layer.ApplyEdit(FVoxelEdit(FIntVector(X, Y, 0), NewData, EEditMode::Set));
			}
		}
		return Layer;
	}

	/** A chunk's voxels with an edit layer merged in, as the chunk manager hands them to scatter. */
	TArray<FVoxelData> ApplyLayer(TArray<FVoxelData> Voxels, const FChunkEditLayer& Layer)
	{
		for (const TPair<int32, FVoxelEdit>& Pair : Layer.Edits)
		{
			Voxels[Pair.Key] = Pair.Value.ApplyToProceduralData(Voxels[Pair.Key]);
		}
		return Voxels;
	}

	/**
	 * The context the scatter manager builds: the world mode (null = no analytic height) and the bottom-layer edits above,
	 * keyed in this chunk's frame (the layer over the top at Z = ChunkSize) as GatherAboveLayerEdits keys them.
	 */
	FScatterExtractionContext MakeContext(const IVoxelWorldMode* Mode, const FChunkEditLayer* AboveLayer = nullptr)
	{
		FScatterExtractionContext Context;
		Context.WorldMode = Mode;
		if (AboveLayer)
		{
			TMap<FIntVector, FVoxelEdit> Bottom;
			for (const TPair<int32, FVoxelEdit>& Pair : AboveLayer->Edits)
			{
				if (Pair.Value.LocalPosition.Z == 0)
				{
					Bottom.Add(FIntVector(Pair.Value.LocalPosition.X, Pair.Value.LocalPosition.Y, ChunkSize), Pair.Value);
				}
			}
			Context.OutsideEdits = MakeShared<const TMap<FIntVector, FVoxelEdit>>(MoveTemp(Bottom));
		}
		return Context;
	}

	enum class EPath : uint8 { Column, Cubic };
	const TCHAR* PathName(EPath Path)
	{
		return Path == EPath::Column ? TEXT("column scan") : TEXT("cubic scan");
	}

	/** One heightmap path's surface points for a chunk. */
	TArray<FVoxelSurfacePoint> Extract(EPath Path, const TArray<FVoxelData>& Voxels, const FIntVector& Coord, const FScatterExtractionContext& Context)
	{
		FChunkSurfaceData Surface;
		UVoxelScatterManager::ExtractChunkSurfacePoints(Path == EPath::Cubic, Voxels, Coord, ChunkOrigin(Coord), ChunkSize, VoxelSize, VoxelSize, {}, Context, Surface);
		return Surface.SurfacePoints;
	}

	/** The column a point stands in: smooth points sit on voxel columns, cubic ones at the block's centre. */
	FIntPoint ColumnOf(EPath Path, const FVector& Position)
	{
		const double LX = Position.X / VoxelSize, LY = Position.Y / VoxelSize;
		return Path == EPath::Cubic
			? FIntPoint(FMath::FloorToInt32(LX), FMath::FloorToInt32(LY))
			: FIntPoint(FMath::RoundToInt32(LX), FMath::RoundToInt32(LY));
	}

	/** Whether a point lies on a chunk's top layer: inside its top voxel cell (smooth) or on its top face (cubic). */
	bool OnTopLayer(EPath Path, const FVector& Position, float ChunkTop)
	{
		return Path == EPath::Cubic ? FMath::Abs(Position.Z - ChunkTop) < 1.0 : Position.Z > ChunkTop - VoxelSize + 0.01;
	}

	/**
	 * The surface each column has over the two chunks, from their (edit-merged) voxels: smooth, the crossing under the
	 * highest solid voxel's air neighbour interpolated as the mesher does; cubic, the top face of the highest solid block.
	 */
	float ExpectedSurfaceZ(EPath Path, const TArray<FVoxelData>& LowerVoxels, const TArray<FVoxelData>& UpperVoxels, int32 X, int32 Y)
	{
		auto VoxelAt = [&](int32 Z) -> const FVoxelData&
		{
			return Z < ChunkSize ? LowerVoxels[X + Y * ChunkSize + Z * ChunkSize * ChunkSize]
				: UpperVoxels[X + Y * ChunkSize + (Z - ChunkSize) * ChunkSize * ChunkSize];
		};
		for (int32 Z = 2 * ChunkSize - 2; Z >= 0; --Z)
		{
			if (VoxelAt(Z).IsSolid() && !VoxelAt(Z + 1).IsSolid())
			{
				if (Path == EPath::Cubic)
				{
					return (Z + 1) * VoxelSize;
				}
				const float S = VoxelAt(Z).Density, A = VoxelAt(Z + 1).Density;
				return (Z + FMath::Clamp((static_cast<float>(VOXEL_SURFACE_THRESHOLD) - S) / (A - S), 0.0f, 1.0f)) * VoxelSize;
			}
		}
		return TNumericLimits<float>::Lowest();
	}

	/** Over the two chunks: open-surface points per column, the worst distance from the expected surface, the lower chunk's top-layer points. */
	struct FTwoChunkResult
	{
		int32 Columns = 0;
		int32 ColumnsWithOne = 0;
		int32 MaxOpenInColumn = 0;
		float WorstError = 0.0f;
		/** Lower chunk's top-layer points (any class) in columns whose ground lies at or above the layer's top, within the band. */
		int32 BandTopLayer = 0;
		/** Of them, open surface. */
		int32 BandTopLayerOpen = 0;
	};

	FTwoChunkResult RunTwoChunks(EPath Path, const IVoxelWorldMode& Mode, const TArray<FVoxelData>& LowerVoxels, const TArray<FVoxelData>& UpperVoxels,
		const FScatterExtractionContext& LowerContext, const FScatterExtractionContext& UpperContext, const TSet<int32>* OnlyColumns = nullptr)
	{
		TMap<int32, TArray<float>> OpenZ;
		FTwoChunkResult Result;
		for (const bool bUpper : { false, true })
		{
			const FIntVector Coord = bUpper ? Upper : Lower;
			for (const FVoxelSurfacePoint& Point : Extract(Path, bUpper ? UpperVoxels : LowerVoxels, Coord, bUpper ? UpperContext : LowerContext))
			{
				const FIntPoint C = ColumnOf(Path, Point.Position);
				if (C.X < 0 || C.X >= ChunkSize || C.Y < 0 || C.Y >= ChunkSize)
				{
					continue;
				}
				const int32 Key = C.X + C.Y * ChunkSize;
				if (!Point.bIsUnderground)
				{
					OpenZ.FindOrAdd(Key).Add(static_cast<float>(Point.Position.Z));
				}
				const float Ground = HeightAt(Mode, C.X * VoxelSize);
				if (!bUpper && OnTopLayer(Path, Point.Position, LayerTop) && Ground >= LayerTop && Ground <= LayerTop + Band)
				{
					++Result.BandTopLayer;
					Result.BandTopLayerOpen += Point.bIsUnderground ? 0 : 1;
				}
			}
		}
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				const int32 Key = X + Y * ChunkSize;
				if (OnlyColumns && !OnlyColumns->Contains(Key))
				{
					continue;
				}
				++Result.Columns;
				const TArray<float>* Zs = OpenZ.Find(Key);
				const int32 Count = Zs ? Zs->Num() : 0;
				Result.ColumnsWithOne += Count == 1 ? 1 : 0;
				Result.MaxOpenInColumn = FMath::Max(Result.MaxOpenInColumn, Count);
				if (Zs)
				{
					const float Expected = ExpectedSurfaceZ(Path, LowerVoxels, UpperVoxels, X, Y);
					for (const float Z : *Zs)
					{
						Result.WorstError = FMath::Max(Result.WorstError, FMath::Abs(Z - Expected));
					}
				}
			}
		}
		return Result;
	}

	TSet<int32> PatchColumns(int32 Min, int32 Max, bool bInside)
	{
		TSet<int32> Keys;
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				const bool bIn = X >= Min && X <= Max && Y >= Min && Y <= Max;
				if (bIn == bInside)
				{
					Keys.Add(X + Y * ChunkSize);
				}
			}
		}
		return Keys;
	}

	/** Private game world + initialized scatter manager (CPU extraction), torn down on destruction. */
	struct FHarness : VoxelScatterTesting::FScatterTestWorld
	{

		FHarness()
			: FScatterTestWorld(TEXT("VoxelScatterTopFaceTestWorld"))
		{
			Config->ChunkSize = ChunkSize;
			Config->VoxelSize = VoxelSize;
			Config->ScatterRadius = 100000.0f;
			Config->MeshingMode = EMeshingMode::MarchingCubes; // smooth: the column scan
			StartManager(/*bSmoothDefinitions=*/true);
		}

		/** Submit a chunk as if its mesh just arrived and pump until its extraction is done; null when it found no point. */
		const FChunkSurfaceData* Run(const FIntVector& Coord, const TArray<FVoxelData>& Voxels)
		{
			Manager->HandOffChunkVoxels(Coord, Voxels, ChunkSize, VoxelSize);
			const double Deadline = FPlatformTime::Seconds() + 15.0;
			while (FPlatformTime::Seconds() < Deadline)
			{
				Manager->Update(FVector::ZeroVector, 0.05f);
				if (Manager->GetPendingGenerationCount() == 0)
				{
					break;
				}
			}
			return Manager->GetChunkSurfaceData(Coord);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterTopFaceContourTest,
	"VoxelWorlds.Scatter.TopFace.Contour",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterTopFaceContourTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterTopFaceTestUtils;
	// Rising: H = 3,040 + 0.12 X over the columns X 0-3,100: inside the lower chunk below its top voxel cell (X < 500), in
	// that cell (to X 1,300), in the band over the layer's top (X 1,400-2,500: the ground 8-140 uu over it) and past it
	// (X >= 2,600). Falling: H = 3,473 - 0.12 X, the same zones mirrored; at X 1,000 the column's corner (the block above's
	// sample) lies 3 uu past the band and its centre (the cubic buried test's height) 3 uu inside it.
	const FPlaneWorldMode Planes[] = { FPlaneWorldMode(0.12f, LayerTop - 160.0f), FPlaneWorldMode(-0.12f, LayerTop + Band + 3.0f + 120.0f) };
	for (const FPlaneWorldMode& Mode : Planes)
	{
		const TArray<FVoxelData> LowerVoxels = MakeChunk(Lower, Mode);
		const TArray<FVoxelData> UpperVoxels = MakeChunk(Upper, Mode);
		const FScatterExtractionContext Context = MakeContext(&Mode);
		const FScatterExtractionContext NoHeight = MakeContext(nullptr);
		const TCHAR* PlaneName = Mode.Slope > 0.0f ? TEXT("rising") : TEXT("falling");

		for (const EPath Path : { EPath::Column, EPath::Cubic })
		{
			const FTwoChunkResult R = RunTwoChunks(Path, Mode, LowerVoxels, UpperVoxels, Context, Context);
			const FTwoChunkResult Old = RunTwoChunks(Path, Mode, LowerVoxels, UpperVoxels, NoHeight, NoHeight);
			AddInfo(FString::Printf(TEXT("%s, %s: %d of %d columns with exactly one open-surface point (most %d), worst %.2f uu off the surface; lower chunk's top layer under the band's ground: %d points (%d open); with no analytic height %d (%d open), %d columns with one point"),
				PlaneName, PathName(Path), R.ColumnsWithOne, R.Columns, R.MaxOpenInColumn, R.WorstError, R.BandTopLayer, R.BandTopLayerOpen, Old.BandTopLayer, Old.BandTopLayerOpen, Old.ColumnsWithOne));
			TestEqual(*FString::Printf(TEXT("%s, %s: every column has exactly one open-surface point over the two chunks"), PlaneName, PathName(Path)), R.ColumnsWithOne, R.Columns);
			TestTrue(*FString::Printf(TEXT("%s, %s: on the surface"), PlaneName, PathName(Path)), R.WorstError <= 2.0f);
			TestEqual(*FString::Printf(TEXT("%s, %s: the lower chunk's top layer has no point where the ground continues above it"), PlaneName, PathName(Path)), R.BandTopLayer, 0);
			TestTrue(*FString::Printf(TEXT("%s, %s: taking the voxel above for air made a row of open surface under the ground (the test reproduces G26)"), PlaneName, PathName(Path)),
				Old.BandTopLayerOpen >= 12 * ChunkSize);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterTopFaceFlatGroundTest,
	"VoxelWorlds.Scatter.TopFace.FlatGround",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterTopFaceFlatGroundTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterTopFaceTestUtils;
	// The ground's height over the layer's top, in voxels: inside the top voxel cell, at the top, in the band, past it.
	for (const float Offset : { -0.9f, -0.5f, -0.1f, -0.01f, 0.0f, 0.01f, 0.3f, 0.7f, 1.0f, 1.4f, 1.6f, 3.0f })
	{
		const FPlaneWorldMode Mode(0.0f, LayerTop + Offset * VoxelSize);
		const TArray<FVoxelData> LowerVoxels = MakeChunk(Lower, Mode);
		const TArray<FVoxelData> UpperVoxels = MakeChunk(Upper, Mode);
		const FScatterExtractionContext Context = MakeContext(&Mode);
		for (const EPath Path : { EPath::Column, EPath::Cubic })
		{
			const FTwoChunkResult R = RunTwoChunks(Path, Mode, LowerVoxels, UpperVoxels, Context, Context);
			const FString Case = FString::Printf(TEXT("ground %+.2f voxels over the top, %s"), Offset, PathName(Path));
			TestEqual(*FString::Printf(TEXT("%s: one open-surface point per column"), *Case), R.ColumnsWithOne, R.Columns);
			TestTrue(*FString::Printf(TEXT("%s: on the surface (worst %.2f uu)"), *Case, R.WorstError), R.WorstError <= 2.0f);
			TestEqual(*FString::Printf(TEXT("%s: no point on the lower chunk's top layer under the ground"), *Case), R.BandTopLayer, 0);
		}
	}

	// The crossing in the top voxel cell: against the generated voxel above it lands on the ground; taking that voxel for air
	// put it up to half a voxel low.
	const FPlaneWorldMode Mode(0.0f, LayerTop - 0.1f * VoxelSize);
	const TArray<FVoxelData> LowerVoxels = MakeChunk(Lower, Mode);
	const TArray<FVoxelData> UpperVoxels = MakeChunk(Upper, Mode);
	const FTwoChunkResult Known = RunTwoChunks(EPath::Column, Mode, LowerVoxels, UpperVoxels, MakeContext(&Mode), MakeContext(&Mode));
	const FTwoChunkResult Old = RunTwoChunks(EPath::Column, Mode, LowerVoxels, UpperVoxels, MakeContext(nullptr), MakeContext(nullptr));
	AddInfo(FString::Printf(TEXT("Ground 0.1 voxels under the top: the top cell's crossing %.2f uu off against the generated voxel above, %.2f uu taking it for air"),
		Known.WorstError, Old.WorstError));
	TestTrue(TEXT("Taking the voxel above for air put the top cell's crossing more than 10 uu low (non-vacuous)"), Old.WorstError > 10.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterTopFaceEditsAboveTest,
	"VoxelWorlds.Scatter.TopFace.EditsAbove",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterTopFaceEditsAboveTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterTopFaceTestUtils;
	constexpr int32 PatchMin = 8, PatchMax = 15;
	const TSet<int32> Inside = PatchColumns(PatchMin, PatchMax, true);
	const TSet<int32> Outside = PatchColumns(PatchMin, PatchMax, false);

	// Dig: the ground half a voxel over the layer's top (the band); the player dug the upper chunk's bottom layer away over
	// the patch, exposing the lower chunk's top face there.
	{
		const FPlaneWorldMode Mode(0.0f, LayerTop + 0.5f * VoxelSize);
		const FChunkEditLayer Dig = MakeBottomPatch(PatchMin, PatchMax, FVoxelData::Air());
		const TArray<FVoxelData> LowerVoxels = MakeChunk(Lower, Mode);
		const TArray<FVoxelData> UpperVoxels = ApplyLayer(MakeChunk(Upper, Mode), Dig);
		for (const EPath Path : { EPath::Column, EPath::Cubic })
		{
			const FTwoChunkResult In = RunTwoChunks(Path, Mode, LowerVoxels, UpperVoxels, MakeContext(&Mode, &Dig), MakeContext(&Mode), &Inside);
			const FTwoChunkResult Out = RunTwoChunks(Path, Mode, LowerVoxels, UpperVoxels, MakeContext(&Mode, &Dig), MakeContext(&Mode), &Outside);
			const FTwoChunkResult Blind = RunTwoChunks(Path, Mode, LowerVoxels, UpperVoxels, MakeContext(&Mode), MakeContext(&Mode), &Inside);
			AddInfo(FString::Printf(TEXT("Dig, %s: dug columns %d of %d with one open point (worst %.2f uu), the rest %d of %d; without the edits %d of %d"),
				PathName(Path), In.ColumnsWithOne, In.Columns, In.WorstError, Out.ColumnsWithOne, Out.Columns, Blind.ColumnsWithOne, Blind.Columns));
			TestEqual(*FString::Printf(TEXT("Dig, %s: the exposed top face is open surface"), PathName(Path)), In.ColumnsWithOne, In.Columns);
			TestTrue(*FString::Printf(TEXT("Dig, %s: on the dug surface"), PathName(Path)), In.WorstError <= 2.0f);
			TestEqual(*FString::Printf(TEXT("Dig, %s: elsewhere one point per column"), PathName(Path)), Out.ColumnsWithOne, Out.Columns);
			TestEqual(*FString::Printf(TEXT("Dig, %s: without the edits the dug columns have no surface (edit-blind)"), PathName(Path)), Blind.ColumnsWithOne, 0);
		}
	}

	// Build: the ground half a voxel under the layer's top (the lower chunk's top cell holds the surface); the player built
	// over the patch into the upper chunk's bottom layer.
	{
		const FPlaneWorldMode Mode(0.0f, LayerTop - 0.5f * VoxelSize);
		const FChunkEditLayer Build = MakeBottomPatch(PatchMin, PatchMax, FVoxelData::Solid(EVoxelMaterial::Stone));
		const TArray<FVoxelData> LowerVoxels = MakeChunk(Lower, Mode);
		const TArray<FVoxelData> UpperVoxels = ApplyLayer(MakeChunk(Upper, Mode), Build);
		for (const EPath Path : { EPath::Column, EPath::Cubic })
		{
			const FTwoChunkResult In = RunTwoChunks(Path, Mode, LowerVoxels, UpperVoxels, MakeContext(&Mode, &Build), MakeContext(&Mode), &Inside);
			const FTwoChunkResult Out = RunTwoChunks(Path, Mode, LowerVoxels, UpperVoxels, MakeContext(&Mode, &Build), MakeContext(&Mode), &Outside);
			const FTwoChunkResult Blind = RunTwoChunks(Path, Mode, LowerVoxels, UpperVoxels, MakeContext(&Mode), MakeContext(&Mode), &Inside);
			AddInfo(FString::Printf(TEXT("Build, %s: built columns %d of %d with one open point (worst %.2f uu, most %d), the rest %d of %d; without the edits most %d"),
				PathName(Path), In.ColumnsWithOne, In.Columns, In.WorstError, In.MaxOpenInColumn, Out.ColumnsWithOne, Out.Columns, Blind.MaxOpenInColumn));
			TestEqual(*FString::Printf(TEXT("Build, %s: the built-over columns have one open point (the build's top)"), PathName(Path)), In.ColumnsWithOne, In.Columns);
			TestTrue(*FString::Printf(TEXT("Build, %s: on the build's top"), PathName(Path)), In.WorstError <= 2.0f);
			TestEqual(*FString::Printf(TEXT("Build, %s: elsewhere one point per column"), PathName(Path)), Out.ColumnsWithOne, Out.Columns);
			TestEqual(*FString::Printf(TEXT("Build, %s: without the edits the covered top face stayed open surface under the build"), PathName(Path)),
				Blind.MaxOpenInColumn, 2);
		}
	}

	// Wiring: the scatter manager's own pipeline asks its edit layer source for the chunk above (the dig case).
	const FPlaneWorldMode Mode(0.0f, LayerTop + 0.5f * VoxelSize);
	const FChunkEditLayer Dig = MakeBottomPatch(PatchMin, PatchMax, FVoxelData::Air());
	const TArray<FVoxelData> LowerVoxels = MakeChunk(Lower, Mode);
	for (const bool bSource : { true, false })
	{
		FHarness Harness;
		Harness.Manager->SetWorldMode(&Mode);
		if (bSource)
		{
			Harness.Manager->SetEditLayerSource([&Dig](const FIntVector& ChunkCoord) -> const FChunkEditLayer*
			{
				return ChunkCoord == Upper ? &Dig : nullptr;
			});
		}
		const FChunkSurfaceData* Surface = Harness.Run(Lower, LowerVoxels);
		const TArray<FVoxelSurfacePoint> NoPoints; // a chunk whose extraction finds no point caches no surface data
		const TArray<FVoxelSurfacePoint>& Points = Surface ? Surface->SurfacePoints : NoPoints;
		int32 InsideOpen = 0, OutsideTop = 0;
		for (const FVoxelSurfacePoint& Point : Points)
		{
			const FIntPoint C = ColumnOf(EPath::Column, Point.Position);
			const bool bIn = C.X >= PatchMin && C.X <= PatchMax && C.Y >= PatchMin && C.Y <= PatchMax;
			if (OnTopLayer(EPath::Column, Point.Position, LayerTop))
			{
				InsideOpen += (bIn && !Point.bIsUnderground) ? 1 : 0;
				OutsideTop += bIn ? 0 : 1;
			}
		}
		AddInfo(FString::Printf(TEXT("%s: %d open top-face points over the dig, %d elsewhere on the top layer"), bSource ? TEXT("With the edit layer source") : TEXT("Without"),
			InsideOpen, OutsideTop));
		TestEqual(TEXT("No top-face point outside the dig"), OutsideTop, 0);
		TestEqual(bSource ? TEXT("With the edit layer source the dug top face is open surface") : TEXT("Without it the dug top face has no point"),
			InsideOpen, bSource ? Inside.Num() : 0);
		Harness.Manager->SetWorldMode(nullptr);
	}
	return true;
}

// ---------------------------------------------------------------------------
// World modes review G28 (WORLD_MODES_REVIEW_PLAN.md §14.1, §20.21): under ground deeper than the band, the voxel over a
// buried chunk's top layer was taken for air, so the chunk's whole top face carried a copy of the rock over it (underground
// points, on which underground-only scatter spawned inside the rock). It is now the voxel generation makes there at any
// depth: the generated height's density less the cave carve (the cave configuration's layers, its biome overrides, the
// sea's minimum depth), with the chunk above's edits applied.
//
// - BuriedCaves: real InfinitePlane terrain with biomes, a sea and the default cave layers; chunk pairs generated on the
//   CPU with their boundary 12 voxels or more under the ground. Over every column of the lower chunk's top layer, both
//   heightmap paths: a point there exactly where the upper chunk's bottom voxel is air (a cave floor at the boundary), and
//   underground; none where it is solid. Again with biome overrides (caves in only the most common biome, from a shallower
//   minimum depth) and with the upper chunk's bottom layer edited (dug over rock, filled over a cave).
// ---------------------------------------------------------------------------

namespace VoxelScatterBuriedTopFaceTestUtils
{
	using namespace VoxelScatterTopFaceTestUtils;

	/** A heightmap world as the streaming builds it: biomes, the default cave layers, a sea at 0. */
	struct FCaveWorld
	{
		UVoxelWorldConfiguration* Config = nullptr;
		UVoxelBiomeConfiguration* Biomes = nullptr;
		UVoxelCaveConfiguration* Caves = nullptr;
		TSharedPtr<IVoxelWorldMode> Mode;
		FVoxelCPUNoiseGenerator Generator;

		FCaveWorld()
		{
			Config = NewObject<UVoxelWorldConfiguration>();
			Config->AddToRoot();
			Config->WorldMode = EWorldMode::InfinitePlane;
			Config->ChunkSize = ChunkSize;
			Config->VoxelSize = VoxelSize;
			Config->NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
			Config->NoiseParams.Seed = 9173;
			Config->NoiseParams.Frequency = 0.0004f;
			Config->NoiseParams.Octaves = 4;
			Config->bEnableBiomes = true;
			Biomes = NewObject<UVoxelBiomeConfiguration>();
			Biomes->AddToRoot();
			Biomes->InitializeDefaults();
			Config->BiomeConfiguration = Biomes;
			Config->bEnableCaves = true;
			Caves = NewObject<UVoxelCaveConfiguration>(); // the default Cheese, Spaghetti and Noodle layers
			Caves->AddToRoot();
			Caves->bEnableCaves = true;
			Config->CaveConfiguration = Caves;
			Config->bEnableWaterLevel = true;
			Config->WaterLevel = 0.0f;
			Mode = FVoxelWorldModeFactory::Create(*Config);
			Generator.Initialize();
		}

		~FCaveWorld()
		{
			Generator.Shutdown();
			Caves->RemoveFromRoot();
			Biomes->RemoveFromRoot();
			Config->RemoveFromRoot();
		}

		/** A chunk as the chunk manager requests it (BuildChunkGenerationRequest), generated on the CPU. */
		TArray<FVoxelData> Generate(const FIntVector& Coord)
		{
			FVoxelNoiseGenerationRequest Request;
			Request.ChunkCoord = Coord;
			Request.LODLevel = 0;
			Request.ChunkSize = ChunkSize;
			Request.VoxelSize = VoxelSize;
			Request.NoiseParams = Config->NoiseParams;
			FVoxelWorldModeFactory::FillGenerationRequest(*Config, Request);
			Request.bEnableBiomes = Config->bEnableBiomes;
			Request.BiomeConfiguration = Config->BiomeConfiguration;
			Request.bEnableCaves = Config->bEnableCaves;
			Request.CaveConfiguration = Config->CaveConfiguration;
			Request.bEnableWaterLevel = Config->bEnableWaterLevel;
			Request.WaterLevel = Config->WaterLevel;
			Request.WaterRadius = Config->WaterRadius;
			TArray<FVoxelData> Voxels;
			Generator.GenerateChunkCPU(Request, Voxels);
			return Voxels;
		}

		/** The context MakeExtractionContext builds for a chunk (no conditioning zones here). */
		FScatterExtractionContext MakeContext(const TSharedPtr<const TMap<FIntVector, FVoxelEdit>>& AboveEdits = nullptr) const
		{
			FScatterExtractionContext Context;
			Context.WorldMode = Mode.Get();
			Context.NoiseParams = Config->NoiseParams;
			Context.Sea = FVoxelSeaSurface::FromConfig(*Config);
			Context.CaveConfiguration = Caves;
			Context.OutsideEdits = AboveEdits;
			return Context;
		}

		/** The lowest and highest generated height over a chunk column's voxel columns. */
		FVector2D HeightRange(int32 CX, int32 CY) const
		{
			FVector2D Range(TNumericLimits<double>::Max(), TNumericLimits<double>::Lowest());
			for (int32 Y = 0; Y < ChunkSize; ++Y)
			{
				for (int32 X = 0; X < ChunkSize; ++X)
				{
					const float H = static_cast<float>(Mode->GetSurfaceOnUpLine(FVector((CX * ChunkSize + X) * VoxelSize, (CY * ChunkSize + Y) * VoxelSize, 0.0), Config->NoiseParams).Position.Z);
					Range.X = FMath::Min(Range.X, static_cast<double>(H));
					Range.Y = FMath::Max(Range.Y, static_cast<double>(H));
				}
			}
			return Range;
		}
	};

	/** The lower chunk's top-layer row against the upper chunk's (edit-merged) bottom voxels, column by column. */
	struct FRowResult
	{
		int32 SolidTops = 0;    // columns whose top voxel is solid
		int32 Real = 0;         // of them, the voxel above air: a crossing at the boundary
		int32 Points = 0;       // top-layer points
		int32 Copies = 0;       // top-layer points whose voxel above is solid
		int32 Missed = 0;       // real crossings with no top-layer point
		int32 NotUnderground = 0; // top-layer points not classified underground (the chunk is buried)
		void Add(const FRowResult& O)
		{
			SolidTops += O.SolidTops; Real += O.Real; Points += O.Points; Copies += O.Copies; Missed += O.Missed; NotUnderground += O.NotUnderground;
		}
	};

	FRowResult CheckRow(EPath Path, const TArray<FVoxelData>& LowerVoxels, const TArray<FVoxelData>& UpperBottomMerged, const FIntVector& LowerCoord,
		const FScatterExtractionContext& Context)
	{
		const float ChunkTop = (LowerCoord.Z + 1) * ChunkWorldSize;
		const FVector Origin = ChunkOrigin(LowerCoord);
		TMap<int32, int32> RowPoints;
		FRowResult Result;
		for (const FVoxelSurfacePoint& Point : Extract(Path, LowerVoxels, LowerCoord, Context))
		{
			// On the top layer: the top face (cubic), or the top voxel's cell from the voxel itself up (smooth: a top voxel at
			// the surface density puts its crossing at its own height).
			const bool bTopLayer = Path == EPath::Cubic ? FMath::Abs(Point.Position.Z - ChunkTop) < 1.0 : Point.Position.Z >= ChunkTop - VoxelSize - 0.01;
			if (!bTopLayer)
			{
				continue;
			}
			const FIntPoint C = ColumnOf(Path, Point.Position - FVector(Origin.X, Origin.Y, 0.0));
			RowPoints.FindOrAdd(C.X + C.Y * ChunkSize) += 1;
			++Result.Points;
			Result.NotUnderground += Point.bIsUnderground ? 0 : 1;
		}
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				const int32 Key = X + Y * ChunkSize;
				const bool bHasPoint = RowPoints.Contains(Key);
				const bool bAboveSolid = UpperBottomMerged[Key].IsSolid();
				if (!LowerVoxels[Key + (ChunkSize - 1) * ChunkSize * ChunkSize].IsSolid())
				{
					continue;
				}
				++Result.SolidTops;
				Result.Real += bAboveSolid ? 0 : 1;
				Result.Copies += (bHasPoint && bAboveSolid) ? 1 : 0;
				Result.Missed += (!bHasPoint && !bAboveSolid) ? 1 : 0;
			}
		}
		return Result;
	}

	/** The upper chunk's bottom layer with an edit map (keyed X + Y * ChunkSize) applied. */
	TArray<FVoxelData> BottomMerged(const TArray<FVoxelData>& UpperVoxels, const TMap<int32, FVoxelEdit>* Edits)
	{
		TArray<FVoxelData> Bottom;
		Bottom.SetNumUninitialized(ChunkSize * ChunkSize);
		for (int32 I = 0; I < ChunkSize * ChunkSize; ++I)
		{
			const FVoxelEdit* Edit = Edits ? Edits->Find(I) : nullptr;
			Bottom[I] = Edit ? Edit->ApplyToProceduralData(UpperVoxels[I]) : UpperVoxels[I];
		}
		return Bottom;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterTopFaceBuriedCavesTest,
	"VoxelWorlds.Scatter.TopFace.BuriedCaves",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterTopFaceBuriedCavesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterBuriedTopFaceTestUtils;
	FCaveWorld World;

	// Chunk pairs over a 3 x 3 patch of chunk columns, the boundary at least 12 voxels under the lowest ground of each.
	struct FPair
	{
		FIntVector Lower;
		TArray<FVoxelData> LowerVoxels;
		TArray<FVoxelData> UpperVoxels;
	};
	auto MakePairs = [&World]()
	{
		TArray<FPair> Pairs;
		for (int32 CY = 0; CY < 3; ++CY)
		{
			for (int32 CX = 0; CX < 3; ++CX)
			{
				const FVector2D Range = World.HeightRange(CX, CY);
				const int32 LowerZ = FMath::FloorToInt32((Range.X - 12.0 * VoxelSize) / ChunkWorldSize) - 1;
				FPair& Pair = Pairs.AddDefaulted_GetRef();
				Pair.Lower = FIntVector(CX, CY, LowerZ);
				Pair.LowerVoxels = World.Generate(Pair.Lower);
				Pair.UpperVoxels = World.Generate(Pair.Lower + FIntVector(0, 0, 1));
			}
		}
		return Pairs;
	};

	auto RunPairs = [this, &World](const TArray<FPair>& Pairs, const TCHAR* Label, EPath Path)
	{
		FRowResult Total;
		for (const FPair& Pair : Pairs)
		{
			Total.Add(CheckRow(Path, Pair.LowerVoxels, BottomMerged(Pair.UpperVoxels, nullptr), Pair.Lower, World.MakeContext()));
		}
		AddInfo(FString::Printf(TEXT("%s, %s: %d columns with a solid top voxel, %d of them under air (a crossing at the boundary); %d top-layer points: %d copies under rock, %d crossings missed, %d not underground"),
			Label, PathName(Path), Total.SolidTops, Total.Real, Total.Points, Total.Copies, Total.Missed, Total.NotUnderground));
		return Total;
	};

	const TArray<FPair> Pairs = MakePairs();
	FRowResult AllReal;
	for (const EPath Path : { EPath::Column, EPath::Cubic })
	{
		const FRowResult New = RunPairs(Pairs, TEXT("Default caves"), Path);
		AllReal.Add(New);
		TestEqual(*FString::Printf(TEXT("Default caves, %s: no copy of the rock on the top layer"), PathName(Path)), New.Copies, 0);
		TestEqual(*FString::Printf(TEXT("Default caves, %s: every cave floor at the boundary has its point"), PathName(Path)), New.Missed, 0);
		TestEqual(*FString::Printf(TEXT("Default caves, %s: a buried chunk's top-layer points are underground"), PathName(Path)), New.NotUnderground, 0);
	}
	TestTrue(*FString::Printf(TEXT("Caves cross the boundaries (%d crossings): not vacuous"), AllReal.Real), AllReal.Real >= 20);

	// The chunk above's bottom layer edited: dug over rock (a floor exposed) and filled over a cave (a floor covered).
	{
		FRowResult Total;
		int32 Dug = 0, Filled = 0;
		for (const FPair& Pair : Pairs)
		{
			TMap<int32, FVoxelEdit> Edits;
			TMap<FIntVector, FVoxelEdit> Outside;
			for (int32 I = 0; I < ChunkSize * ChunkSize; I += 7)
			{
				const FIntVector Local(I % ChunkSize, I / ChunkSize, 0);
				const bool bSolid = Pair.UpperVoxels[I].IsSolid();
				const FVoxelEdit Edit(Local, bSolid ? FVoxelData::Air() : FVoxelData::Solid(EVoxelMaterial::Stone), EEditMode::Set);
				Edits.Add(I, Edit);
				Outside.Add(FIntVector(Local.X, Local.Y, ChunkSize), Edit);
				(bSolid ? Dug : Filled) += 1;
			}
			const TSharedPtr<const TMap<FIntVector, FVoxelEdit>> Shared = MakeShared<const TMap<FIntVector, FVoxelEdit>>(MoveTemp(Outside));
			for (const EPath Path : { EPath::Column, EPath::Cubic })
			{
				Total.Add(CheckRow(Path, Pair.LowerVoxels, BottomMerged(Pair.UpperVoxels, &Edits), Pair.Lower, World.MakeContext(Shared)));
			}
		}
		AddInfo(FString::Printf(TEXT("Edited above (%d dug, %d filled), both paths: %d solid tops, %d under air; %d points: %d copies, %d missed, %d not underground"),
			Dug, Filled, Total.SolidTops, Total.Real, Total.Points, Total.Copies, Total.Missed, Total.NotUnderground));
		TestTrue(TEXT("Edited above: both kinds of edit"), Dug > 0 && Filled > 0);
		TestEqual(TEXT("Edited above: no copy under rock or a fill"), Total.Copies, 0);
		TestEqual(TEXT("Edited above: every dug or open floor has its point"), Total.Missed, 0);
	}

	// Biome overrides: caves only in the most common biome of the lower chunks' top layers, from a shallower minimum depth.
	{
		TMap<uint8, int32> BiomeCounts;
		for (const FPair& Pair : Pairs)
		{
			for (int32 I = 0; I < ChunkSize * ChunkSize; ++I)
			{
				BiomeCounts.FindOrAdd(Pair.LowerVoxels[I + (ChunkSize - 1) * ChunkSize * ChunkSize].BiomeID) += 1;
			}
		}
		uint8 Common = 0;
		int32 CommonCount = -1;
		for (const TPair<uint8, int32>& Entry : BiomeCounts)
		{
			if (Entry.Value > CommonCount)
			{
				Common = Entry.Key;
				CommonCount = Entry.Value;
			}
		}
		World.Caves->BiomeOverrides.Empty();
		for (const TPair<uint8, int32>& Entry : BiomeCounts)
		{
			FBiomeCaveOverride Override;
			Override.BiomeID = Entry.Key;
			Override.CaveScale = Entry.Key == Common ? 1.5f : 0.0f;
			Override.MinDepthOverride = Entry.Key == Common ? 6.0f : -1.0f;
			World.Caves->BiomeOverrides.Add(Override);
		}
		const TArray<FPair> OverridePairs = MakePairs();
		FRowResult Total;
		for (const EPath Path : { EPath::Column, EPath::Cubic })
		{
			Total.Add(RunPairs(OverridePairs, *FString::Printf(TEXT("Biome overrides (%d biomes, caves in %d)"), BiomeCounts.Num(), Common), Path));
		}
		TestEqual(TEXT("Biome overrides: no copy"), Total.Copies, 0);
		TestEqual(TEXT("Biome overrides: no crossing missed"), Total.Missed, 0);
		TestTrue(*FString::Printf(TEXT("Biome overrides: crossings in the cave biome (%d)"), Total.Real), Total.Real > 0);
		World.Caves->BiomeOverrides.Empty();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
