// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "VoxelLocalVertexFactory.h"
#include "VoxelRegionComponent.h"
#include "VoxelVertex.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Spherical Phase 4 (WORLD_MODES_REVIEW_PLAN.md §18, WM-SY4; SP-D3): the face type (top, side, bottom) the custom vertex
 * factory writes into TexCoord1.y is measured against a local up. With up +Z the up-aware conversion equals the old one
 * bit for bit for 2,000 normals; with up +X (a planet's equator facing +X) a normal along +X is a top face, one along
 * world Z a side face; the world component carries the planet centre the scene proxy classifies against.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlanetFaceTypeTest,
	"VoxelWorlds.Rendering.Planet.FaceTypeByLocalUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlanetFaceTypeTest::RunTest(const FString& Parameters)
{
	FRandomStream Random(31);
	int32 Identical = 0;
	constexpr int32 Count = 2000;
	for (int32 i = 0; i < Count; ++i)
	{
		FVoxelVertex Vertex;
		Vertex.Position = FVector3f(Random.FRandRange(0.0f, 3200.0f), Random.FRandRange(0.0f, 3200.0f), Random.FRandRange(0.0f, 3200.0f));
		Vertex.UV = FVector2f(Random.FRand(), Random.FRand());
		Vertex.SetNormal(FVector3f(Random.VRand()));
		const FVoxelLocalVertex Old = FVoxelLocalVertex::FromVoxelVertex(Vertex);
		const FVoxelLocalVertex New = FVoxelLocalVertex::FromVoxelVertex(Vertex, FVector3f::UpVector);
		const bool bSame = Old.Position == New.Position && Old.TexCoord == New.TexCoord && Old.TexCoord1 == New.TexCoord1
			&& Old.Color == New.Color && Old.TangentX.Vector.Packed == New.TangentX.Vector.Packed
			&& Old.TangentZ.Vector.Packed == New.TangentZ.Vector.Packed;
		Identical += bSame ? 1 : 0;
	}
	TestEqual(TEXT("With up +Z the up-aware conversion equals the old one"), Identical, Count);

	auto FaceType = [](const FVector3f& Normal, const FVector3f& Up)
	{
		FVoxelVertex Vertex;
		Vertex.Position = FVector3f::ZeroVector;
		Vertex.SetNormal(Normal);
		return FMath::RoundToInt(FVoxelLocalVertex::FromVoxelVertex(Vertex, Up).TexCoord1.Y);
	};
	const FVector3f EquatorUp(1.0f, 0.0f, 0.0f);
	TestEqual(TEXT("Equator: a normal along local up is a top face"), FaceType(FVector3f(1, 0, 0), EquatorUp), 0);
	TestEqual(TEXT("Equator: a normal along world Z is a side face"), FaceType(FVector3f(0, 0, 1), EquatorUp), 1);
	TestEqual(TEXT("Equator: a normal against local up is a bottom face"), FaceType(FVector3f(-1, 0, 0), EquatorUp), 2);
	TestEqual(TEXT("The old conversion called that top face a side face"),
		FMath::RoundToInt(FVoxelLocalVertex::FromVoxelVertex([] { FVoxelVertex V; V.Position = FVector3f::ZeroVector; V.SetNormal(FVector3f(1, 0, 0)); return V; }()).TexCoord1.Y), 1);

	UVoxelRegionComponent* Component = NewObject<UVoxelRegionComponent>(GetTransientPackage());
	TestFalse(TEXT("A world component starts with face types along +Z"), Component->IsRadialFaceUp());
	Component->SetRadialFaceUp(true, FVector(100.0, 200.0, 300.0));
	TestTrue(TEXT("SetRadialFaceUp turns the radial face up on"), Component->IsRadialFaceUp());
	TestEqual(TEXT("... around the planet centre"), Component->GetFaceUpCenter(), FVector(100.0, 200.0, 300.0));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
