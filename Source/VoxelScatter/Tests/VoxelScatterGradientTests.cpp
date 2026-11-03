// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "VoxelScatterTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "VoxelScatterExtraction.h"
#include "VoxelScatterManager.h"
#include "VoxelMaterialRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G109: the column scan's surface normal is the density gradient, a central difference over two voxels. At a chunk's face
// columns and in its top and bottom layers one of the two lies in the neighbour; clamped into the chunk, the difference
// was one-sided over one voxel read as two, and the slope there read about half (a 40-degree flank 18 at an edge column).
// Planes at 40 degrees along X, along Y and along the diagonal, cut into chunks at many heights so the crossings fall in
// every layer: the slope of the open points at the face columns and in the top and bottom layers reads as inside the
// chunk, mean and range. Without a heightmap world mode (no voxels past the faces) the one-sided difference is doubled to
// the same span: right on average, within 5 degrees. Pure: the extractor on generated chunks.
// ---------------------------------------------------------------------------

namespace VoxelScatterGradientTestUtils
{
	using namespace VoxelScatterTesting;

	constexpr int32 ChunkSize = 16;
	constexpr float VoxelSize = 100.0f;

	/** The slopes of a group of points. */
	struct FSlopes
	{
		int32 Count = 0;
		double Sum = 0.0;
		float Min = TNumericLimits<float>::Max();
		float Max = TNumericLimits<float>::Lowest();
		void Add(float Degrees)
		{
			++Count;
			Sum += Degrees;
			Min = FMath::Min(Min, Degrees);
			Max = FMath::Max(Max, Degrees);
		}
		double Mean() const { return Count > 0 ? Sum / Count : 0.0; }
		FString Describe() const { return FString::Printf(TEXT("%d points, %.1f to %.1f, mean %.2f"), Count, Min, Max, Mean()); }
	};

	/** The open points' slopes inside the chunks (Interior), at their face columns (Face) and in their top and bottom layers (Layer). */
	struct FGroups
	{
		FSlopes Interior;
		FSlopes Face;
		FSlopes Layer;
	};

	/** The open points of the plane H = 20.37 + SlopeX * X + SlopeY * Y over the chunk column at (0, 0), cut at chunk heights every 37 uu. */
	FGroups Extract(float SlopeX, float SlopeY, bool bWithWorldMode)
	{
		const FTiltedPlaneWorldMode Mode(20.37f, SlopeX, SlopeY);
		FScatterExtractionContext Context;
		Context.WorldMode = bWithWorldMode ? &Mode : nullptr;
		FGroups Groups;
		const float Top = Mode.HeightAt((ChunkSize - 1) * VoxelSize, (ChunkSize - 1) * VoxelSize);
		for (float OriginZ = -ChunkSize * VoxelSize; OriginZ < Top + VoxelSize; OriginZ += 37.0f)
		{
			const FVector Origin(0.0, 0.0, OriginZ);
			const TArray<FVoxelData> Voxels = MakeHeightmapChunk(Origin, ChunkSize, VoxelSize,
				[&Mode](double X, double Y) { return Mode.HeightAt(X, Y); },
				[](int32, int32) { return static_cast<uint8>(EVoxelMaterial::Grass); });
			FChunkSurfaceData Surface;
			UVoxelScatterManager::ExtractChunkSurfacePoints(false, Voxels, FIntVector::ZeroValue, Origin, ChunkSize, VoxelSize, VoxelSize, {},
				Context, Surface);
			for (const FVoxelSurfacePoint& Point : Surface.SurfacePoints)
			{
				if (Point.bIsUnderground)
				{
					continue;
				}
				const int32 X = FMath::RoundToInt32((Point.Position.X - Origin.X) / VoxelSize);
				const int32 Y = FMath::RoundToInt32((Point.Position.Y - Origin.Y) / VoxelSize);
				const int32 Z = FMath::FloorToInt32((Point.Position.Z - Origin.Z) / VoxelSize);
				const bool bFace = X == 0 || Y == 0 || X == ChunkSize - 1 || Y == ChunkSize - 1;
				const bool bLayer = Z == 0 || Z == ChunkSize - 1;
				(bLayer ? Groups.Layer : bFace ? Groups.Face : Groups.Interior).Add(Point.GetSlopeAngle());
			}
		}
		return Groups;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterGradientAtFacesTest,
	"VoxelWorlds.Scatter.Extraction.GradientAtChunkFaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterGradientAtFacesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterGradientTestUtils;
	const float Slope = FMath::Tan(FMath::DegreesToRadians(40.0f));
	struct FPlane
	{
		const TCHAR* Name;
		float SlopeX;
		float SlopeY;
	};
	const FPlane Planes[] = {
		{ TEXT("40 degrees along X"), Slope, 0.0f },
		{ TEXT("40 degrees along Y"), 0.0f, Slope },
		{ TEXT("40 degrees along the diagonal"), Slope * UE_INV_SQRT_2, Slope * UE_INV_SQRT_2 },
	};
	for (const FPlane& Plane : Planes)
	{
		const FGroups G = Extract(Plane.SlopeX, Plane.SlopeY, true);
		AddInfo(FString::Printf(TEXT("%s: inside %s; face columns %s; top and bottom layers %s"), Plane.Name, *G.Interior.Describe(),
			*G.Face.Describe(), *G.Layer.Describe()));
		if (!TestTrue(FString::Printf(TEXT("%s: every group has points"), Plane.Name), G.Interior.Count > 100 && G.Face.Count > 50 && G.Layer.Count > 20))
		{
			continue;
		}
		// The saturated densities read a plane a little steep inside the chunk too (about 41.4 degrees along an axis, 43.6
		// along the diagonal): the comparison below is with the interior, not with 40.
		TestTrue(FString::Printf(TEXT("%s: the interior reads about the plane's slope (non-vacuous)"), Plane.Name), FMath::Abs(G.Interior.Mean() - 40.0) < 4.5);
		for (const TPair<const TCHAR*, const FSlopes*>& Group : { TPair<const TCHAR*, const FSlopes*>(TEXT("face columns"), &G.Face),
			TPair<const TCHAR*, const FSlopes*>(TEXT("top and bottom layers"), &G.Layer) })
		{
			TestTrue(FString::Printf(TEXT("%s: the %s' mean slope is the interior's"), Plane.Name, Group.Key),
				FMath::Abs(Group.Value->Mean() - G.Interior.Mean()) < 0.5);
			TestTrue(FString::Printf(TEXT("%s: the %s' slopes lie in the interior's range"), Plane.Name, Group.Key),
				Group.Value->Min >= G.Interior.Min - 0.5f && Group.Value->Max <= G.Interior.Max + 0.5f);
		}
	}

	// No world mode: no voxels past the faces, the one-sided difference doubled.
	const FGroups Fallback = Extract(Slope, 0.0f, false);
	AddInfo(FString::Printf(TEXT("No world mode, 40 degrees along X: inside %s; face columns %s; top and bottom layers %s"),
		*Fallback.Interior.Describe(), *Fallback.Face.Describe(), *Fallback.Layer.Describe()));
	TestTrue(TEXT("No world mode: the face columns' mean slope is within 5 degrees of the interior's (it read about half)"),
		Fallback.Face.Count > 50 && FMath::Abs(Fallback.Face.Mean() - Fallback.Interior.Mean()) < 5.0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
