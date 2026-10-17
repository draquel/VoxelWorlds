// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelEditManager.h"
#include "VoxelEditTypes.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Spherical Phase 4, WM-SY17 (SP-F8): the cylinder brush's axis. On a plane the default axis is +Z and the brush edits the
// old voxels (the same set as an explicit +Z axis, every voxel within the old rule); on a planet the default is the radial
// direction at the brush centre: every edited voxel lies within the radius of the radial line and the half-height along
// it, the volume is the cylinder's, and voxels the Z rule would have dropped are edited.
// ---------------------------------------------------------------------------

namespace VoxelBrushAxisTestUtils
{
	constexpr int32 ChunkSize = 16;
	constexpr float VoxelSize = 100.0f;

	UVoxelEditManager* MakeManager(EWorldMode Mode, const FVector& Origin)
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->WorldMode = Mode;
		Config->ChunkSize = ChunkSize;
		Config->VoxelSize = VoxelSize;
		Config->WorldOrigin = Origin;
		UVoxelEditManager* Manager = NewObject<UVoxelEditManager>();
		Manager->Initialize(Config);
		Manager->SetEditSource(EEditSource::Player);
		return Manager;
	}

	/** The world position of every edited voxel (its sample corner). */
	TArray<FVector> EditedVoxels(UVoxelEditManager* Manager, const FVector& Origin)
	{
		TArray<FVector> Out;
		TArray<FIntVector> Chunks;
		Manager->GetEditedChunkCoords(Chunks);
		Chunks.Sort([](const FIntVector& A, const FIntVector& B) { return A.X != B.X ? A.X < B.X : (A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z); });
		for (const FIntVector& Chunk : Chunks)
		{
			const FChunkEditLayer* Layer = Manager->GetEditLayer(Chunk);
			if (!Layer)
			{
				continue;
			}
			TArray<int32> Indices;
			Layer->Edits.GetKeys(Indices);
			Indices.Sort();
			for (const int32 Index : Indices)
			{
				const FIntVector Local(Index % ChunkSize, (Index / ChunkSize) % ChunkSize, Index / (ChunkSize * ChunkSize));
				Out.Add(Origin + FVector(Chunk * ChunkSize + Local) * VoxelSize);
			}
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBrushCylinderAxisTest,
	"VoxelWorlds.Edit.Brush.CylinderAxis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelBrushCylinderAxisTest::RunTest(const FString& Parameters)
{
	using namespace VoxelBrushAxisTestUtils;
	FVoxelBrushParams Brush;
	Brush.Shape = EVoxelBrushShape::Cylinder;
	Brush.Radius = 450.0f;
	Brush.Strength = 1.0f;
	Brush.FalloffType = EVoxelBrushFalloff::Linear;
	Brush.DensityDelta = 255;

	// A plane: the default axis edits exactly what an explicit +Z axis edits, all within the old rule.
	{
		const FVector Origin(1000.0, -2000.0, 500.0);
		const FVector Centre = Origin + FVector(1234.0, 567.0, 890.0);
		UVoxelEditManager* Default = MakeManager(EWorldMode::InfinitePlane, Origin);
		UVoxelEditManager* Explicit = MakeManager(EWorldMode::InfinitePlane, Origin);
		Default->ApplyBrushEdit(Centre, Brush, EEditMode::Subtract);
		FVoxelBrushParams ZBrush = Brush;
		ZBrush.Axis = FVector(0.0, 0.0, 5.0);
		Explicit->ApplyBrushEdit(Centre, ZBrush, EEditMode::Subtract);
		const TArray<FVector> A = EditedVoxels(Default, Origin);
		const TArray<FVector> B = EditedVoxels(Explicit, Origin);
		int32 Outside = 0;
		for (const FVector& Voxel : A)
		{
			const FVector Delta = Voxel - Centre;
			Outside += (FMath::Sqrt(Delta.X * Delta.X + Delta.Y * Delta.Y) > Brush.Radius + VoxelSize * 1.5 || FMath::Abs(Delta.Z) > Brush.Radius + VoxelSize) ? 1 : 0;
		}
		AddInfo(FString::Printf(TEXT("Plane: %d voxels edited"), A.Num()));
		TestTrue(TEXT("Plane: voxels edited"), A.Num() > 100);
		TestTrue(TEXT("Plane: the default axis is +Z (the same voxels)"), A == B);
		TestEqual(TEXT("Plane: every voxel within the Z cylinder"), Outside, 0);
	}

	// A planet at 45 degrees: the radial axis.
	{
		const FVector Origin(1000.0, -2000.0, 500.0);
		const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
		const FVector Radial(C45, 0.0, C45);
		const FVector Centre = Origin + Radial * 250000.0;
		UVoxelEditManager* Manager = MakeManager(EWorldMode::SphericalPlanet, Origin);
		Manager->ApplyBrushEdit(Centre, Brush, EEditMode::Subtract);
		const TArray<FVector> Voxels = EditedVoxels(Manager, Origin);
		int32 Outside = 0, ZRuleWouldDrop = 0;
		for (const FVector& Voxel : Voxels)
		{
			const FVector Delta = Voxel - Centre;
			const double Along = FVector::DotProduct(Delta, Radial);
			const double Across = (Delta - Radial * Along).Size();
			Outside += (Across > Brush.Radius + VoxelSize * 1.8 || FMath::Abs(Along) > Brush.Radius + VoxelSize * 1.8) ? 1 : 0;
			ZRuleWouldDrop += (FMath::Sqrt(Delta.X * Delta.X + Delta.Y * Delta.Y) > Brush.Radius + VoxelSize * 1.5) ? 1 : 0;
		}
		// The cylinder's volume in voxels, with the linear falloff's 1% cut at the rim (radius 0.99 R).
		const double Expected = UE_DOUBLE_PI * FMath::Square(0.99 * Brush.Radius) * 2.0 * Brush.Radius / FMath::Pow(static_cast<double>(VoxelSize), 3.0);
		AddInfo(FString::Printf(TEXT("Planet at 45 degrees: %d voxels edited (%.0f expected), %d outside the radial cylinder, %d past the Z rule's reach"),
			Voxels.Num(), Expected, Outside, ZRuleWouldDrop));
		TestEqual(TEXT("Planet: every voxel within the radial cylinder"), Outside, 0);
		TestTrue(TEXT("Planet: the cylinder's volume (within 15%)"), FMath::Abs(Voxels.Num() / Expected - 1.0) < 0.15);
		TestTrue(TEXT("Planet: voxels the Z rule would have dropped are edited (the axis is radial)"), ZRuleWouldDrop > 10);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
