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
#include "VoxelMaterialRegistry.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// World modes review G27 (WORLD_MODES_REVIEW_PLAN.md §14.1, §19.5): scatter's surface points must cover the cell layers
// along all six faces of a chunk. The GPU extractor read the chunk's interior mesh, which since seam ownership has no
// vertex in the cells against the chunk's max faces (they belong to its seams), so every chunk it extracted had no point
// there: strips without scatter along chunk borders and along the contour where the terrain crosses a chunk layer's top.
// It was retired; every chunk extracts from its voxels.
//
// - EveryCellLayerCovered: a plane tilted along X and Y through a 3 x 3 x 2 block of chunks, crossing the two layers'
//   shared plane. Every voxel column has exactly one open-surface point, on the plane, and each of the six boundary cell
//   layers (local 0 and ChunkSize - 1 on each axis) holds the points of every column whose surface lies in it.
// ---------------------------------------------------------------------------

namespace VoxelScatterBoundaryTestUtils
{
	constexpr int32 ChunkSize = 16;
	constexpr float VoxelSize = 100.0f;
	constexpr float ChunkWorldSize = ChunkSize * VoxelSize;

	/** A heightmap world mode whose natural terrain is the plane H = Offset + SlopeX * X + SlopeY * Y. */
	struct FTiltedPlaneWorldMode : public IVoxelWorldMode
	{
		float Offset = 0.0f;
		float SlopeX = 0.0f;
		float SlopeY = 0.0f;
		FTiltedPlaneWorldMode(float InOffset, float InSlopeX, float InSlopeY) : Offset(InOffset), SlopeX(InSlopeX), SlopeY(InSlopeY) {}
		virtual float GetTerrainHeightAt(float X, float Y, const FVoxelNoiseParams&) const override { return Offset + SlopeX * X + SlopeY * Y; }
		virtual bool IsHeightmapBased() const override { return true; }
		virtual float GetDensityAt(const FVector&, int32, float) const override { return 0.0f; }
		virtual FIntVector WorldToChunkCoord(const FVector&, int32, float) const override { return FIntVector::ZeroValue; }
		virtual FVector ChunkCoordToWorld(const FIntVector&, int32, float, int32) const override { return FVector::ZeroVector; }
		virtual int32 GetMinZ() const override { return -100000; }
		virtual int32 GetMaxZ() const override { return 100000; }
		virtual EWorldMode GetWorldModeType() const override { return EWorldMode::InfinitePlane; }
		virtual uint8 GetMaterialAtDepth(const FVector&, float, float) const override { return 0; }

		float HeightAt(double X, double Y) const { return GetTerrainHeightAt(static_cast<float>(X), static_cast<float>(Y), FVoxelNoiseParams()); }
	};

	FVector ChunkOrigin(const FIntVector& Coord) { return FVector(Coord) * ChunkWorldSize; }

	/** A chunk generated from the world mode's height with the heightmap generator's SDF (grass, no caves). */
	TArray<FVoxelData> MakeChunk(const FIntVector& Coord, const FTiltedPlaneWorldMode& Mode)
	{
		TArray<FVoxelData> Voxels;
		Voxels.SetNumUninitialized(ChunkSize * ChunkSize * ChunkSize);
		const FVector Origin = ChunkOrigin(Coord);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				const float Height = Mode.HeightAt(Origin.X + X * VoxelSize, Origin.Y + Y * VoxelSize);
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterBoundaryEveryLayerTest,
	"VoxelWorlds.Scatter.Boundary.EveryCellLayerCovered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterBoundaryEveryLayerTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterBoundaryTestUtils;

	// H = 700.37 + 0.13 X + 0.11 Y over X, Y in [0, 4,800): from 700 to 1,852, so the plane crosses the layers' shared
	// plane (Z 1,600) along a diagonal and every chunk boundary in X and Y. At every column H lies at least 0.37 uu from a
	// voxel plane, past the SDF's rounding, so which cell holds the crossing is the plane's own.
	const FTiltedPlaneWorldMode Mode(700.37f, 0.13f, 0.11f);
	constexpr int32 Blocks = 3;
	constexpr int32 Columns = Blocks * ChunkSize;
	FScatterExtractionContext Context;
	Context.WorldMode = &Mode;

	TMap<FIntPoint, TArray<double>> OpenZ;
	// Per face (-X, +X, -Y, +Y, -Z, +Z): open points in a chunk's boundary cell layer, and the columns whose surface lies there.
	int32 BoundaryPoints[6] = { 0, 0, 0, 0, 0, 0 };
	int32 Expected[6] = { 0, 0, 0, 0, 0, 0 };
	static const TCHAR* FaceNames[6] = { TEXT("-X"), TEXT("+X"), TEXT("-Y"), TEXT("+Y"), TEXT("-Z"), TEXT("+Z") };

	for (int32 CZ = 0; CZ < 2; ++CZ)
	{
		for (int32 CY = 0; CY < Blocks; ++CY)
		{
			for (int32 CX = 0; CX < Blocks; ++CX)
			{
				const FIntVector Coord(CX, CY, CZ);
				const FVector Origin = ChunkOrigin(Coord);
				FChunkSurfaceData Surface;
				UVoxelScatterManager::ExtractHeightmapSurfacePoints(MakeChunk(Coord, Mode), Coord, Origin, ChunkSize, VoxelSize, VoxelSize, false,
					Context, Surface);
				for (const FVoxelSurfacePoint& Point : Surface.SurfacePoints)
				{
					if (Point.bIsUnderground)
					{
						continue;
					}
					const FVector Local = (Point.Position - Origin) / VoxelSize;
					OpenZ.FindOrAdd(FIntPoint(CX * ChunkSize + FMath::RoundToInt32(Local.X), CY * ChunkSize + FMath::RoundToInt32(Local.Y))).Add(Point.Position.Z);
					const FIntVector Cell(FMath::Clamp(FMath::FloorToInt32(Local.X + 0.001), 0, ChunkSize - 1),
						FMath::Clamp(FMath::FloorToInt32(Local.Y + 0.001), 0, ChunkSize - 1), FMath::Clamp(FMath::FloorToInt32(Local.Z), 0, ChunkSize - 1));
					for (int32 Face = 0; Face < 6; ++Face)
					{
						BoundaryPoints[Face] += Cell[Face >> 1] == ((Face & 1) != 0 ? ChunkSize - 1 : 0) ? 1 : 0;
					}
				}
				// The columns whose surface lies in each boundary layer of this chunk.
				for (int32 Y = 0; Y < ChunkSize; ++Y)
				{
					for (int32 X = 0; X < ChunkSize; ++X)
					{
						const double H = Mode.HeightAt(Origin.X + X * VoxelSize, Origin.Y + Y * VoxelSize);
						if (H < Origin.Z || H >= Origin.Z + ChunkWorldSize)
						{
							continue;
						}
						const int32 CellZ = FMath::FloorToInt32((H - Origin.Z) / VoxelSize);
						Expected[0] += X == 0 ? 1 : 0;
						Expected[1] += X == ChunkSize - 1 ? 1 : 0;
						Expected[2] += Y == 0 ? 1 : 0;
						Expected[3] += Y == ChunkSize - 1 ? 1 : 0;
						Expected[4] += CellZ == 0 ? 1 : 0;
						Expected[5] += CellZ == ChunkSize - 1 ? 1 : 0;
					}
				}
			}
		}
	}

	int32 ColumnsWithOne = 0;
	double WorstError = 0.0;
	for (int32 Y = 0; Y < Columns; ++Y)
	{
		for (int32 X = 0; X < Columns; ++X)
		{
			const TArray<double>* Zs = OpenZ.Find(FIntPoint(X, Y));
			ColumnsWithOne += (Zs && Zs->Num() == 1) ? 1 : 0;
			if (Zs)
			{
				for (const double Z : *Zs)
				{
					WorstError = FMath::Max(WorstError, FMath::Abs(Z - Mode.HeightAt(X * VoxelSize, Y * VoxelSize)));
				}
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d of %d columns with exactly one open point, worst %.2f uu off the plane"), ColumnsWithOne, Columns * Columns, WorstError));
	TestEqual(TEXT("Every column has exactly one open-surface point"), ColumnsWithOne, Columns * Columns);
	TestTrue(TEXT("Every open point lies on the plane (2 uu)"), WorstError <= 2.0);

	for (int32 Face = 0; Face < 6; ++Face)
	{
		AddInfo(FString::Printf(TEXT("face %s: %d points in the boundary layers, %d columns' surface there"), FaceNames[Face], BoundaryPoints[Face], Expected[Face]));
		TestTrue(FString::Printf(TEXT("Face %s: the scenario reaches its boundary layer"), FaceNames[Face]), Expected[Face] > 0);
		TestEqual(FString::Printf(TEXT("Face %s: every column whose surface lies in the boundary layer has its point there"), FaceNames[Face]),
			BoundaryPoints[Face], Expected[Face]);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
