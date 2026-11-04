// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "RHI.h"             // GUsingNullRHI: no scene proxy under -nullrhi, the test skips there
#include "RenderUtils.h"     // IsRayTracingEnabled
#include "RenderingThread.h" // FlushRenderingCommands
#include "Materials/MaterialInterface.h"
#include "Testing/VoxelTestScopedCVar.h"
#include "VoxelFarField.h"
#include "VoxelFarFieldPatchComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G121: a far-field patch component's proxy builds ray tracing geometry only for its terrain section, and only while
// something can trace it. A real world, component and scene proxy with a terrain section (marked for ray tracing) and a
// sea section (not): in the far-field layer (voxel.World.FarFieldRayTracing 1) the proxy builds none with Lumen's far field
// off and the terrain's alone with it on; in the near-field layer (2) the terrain's; out of the scene (0) none. The sea's
// never. Needs a render scene and ray tracing: runs in the editor on a ray tracing GPU, skips under -nullrhi or without it.
// ---------------------------------------------------------------------------

namespace VoxelFarFieldRayTracingProxyTest
{
	/** A grid of Cells x Cells quads at height Z, facing up */
	FVoxelFarFieldSection MakeGrid(int32 Cells, float Z)
	{
		FVoxelFarFieldSection Mesh;
		for (int32 Y = 0; Y <= Cells; ++Y)
		{
			for (int32 X = 0; X <= Cells; ++X)
			{
				Mesh.Positions.Add(FVector3f(X * 100.0f, Y * 100.0f, Z));
				Mesh.Normals.Add(FVector3f::UpVector);
				Mesh.Tangents.Add(FVector3f::ForwardVector);
				Mesh.UV0.Add(FVector2f(X, Y));
				Mesh.UV1.Add(FVector2f::ZeroVector);
			}
		}
		for (int32 Y = 0; Y < Cells; ++Y)
		{
			for (int32 X = 0; X < Cells; ++X)
			{
				const int32 I = Y * (Cells + 1) + X;
				Mesh.Triangles.Append({ I, I + Cells + 1, I + 1, I + 1, I + Cells + 1, I + Cells + 2 });
			}
		}
		return Mesh;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFarFieldRayTracingGeometryTest,
	"VoxelWorlds.Rendering.RayTracing.FarFieldGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelFarFieldRayTracingGeometryTest::RunTest(const FString& Parameters)
{
	using namespace VoxelFarFieldRayTracingProxyTest;
	using VoxelTesting::FScopedCVar;
	if (GUsingNullRHI) { AddInfo(TEXT("Skipped: needs a render scene (run without -nullrhi, where components create no scene proxy)")); return true; }
	if (!IsRayTracingEnabled()) { AddInfo(TEXT("Skipped: ray tracing is not enabled here (the project's support, the RHI or the GPU)")); return true; }

	UMaterialInterface* Terrain = LoadObject<UMaterialInterface>(nullptr, TEXT("/VoxelWorlds/Materials/M_VoxelMaster.M_VoxelMaster"));
	if (!TestNotNull(TEXT("The terrain master loads"), Terrain))
	{
		return false;
	}
	const FScopedCVar LumenFarField(TEXT("r.LumenScene.FarField"), 0);

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelFarFieldRayTracingProxyTestWorld"));
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(World);
	{
		AActor* Owner = World->SpawnActor<AActor>();
		UVoxelFarFieldPatchComponent* Component = NewObject<UVoxelFarFieldPatchComponent>(Owner, NAME_None, RF_Transient);
		Owner->SetRootComponent(Component);
		Component->bRayTracingFarField = true;
		Component->bVisibleInRayTracing = true;
		Component->SetSection(0, MakeGrid(4, 0.0f), false, /*bRayTracing=*/true);
		Component->SetSection(1, MakeGrid(2, 50.0f), false, /*bRayTracing=*/false);
		Component->SetMaterial(0, Terrain);
		Component->SetMaterial(1, Terrain);
		Component->RegisterComponent();

		// The geometries the proxy holds once its commands ran, after a recreation the frame's end would make
		auto Geometries = [&]()
		{
			World->SendAllEndOfFrameUpdates();
			FlushRenderingCommands();
			return Component->GetProxyRayTracingGeometryCount();
		};
		auto SetLayer = [&](bool bFarField, bool bVisible)
		{
			Component->bRayTracingFarField = bFarField;
			Component->bVisibleInRayTracing = bVisible;
			Component->MarkRenderStateDirty();
		};

		TestEqual(TEXT("Both sections hold triangles (32 and 8)"),
			Component->GetSection(0)->Indices.Num() / 3 + Component->GetSection(1)->Indices.Num() / 3, 40);
		int32 N = Geometries();
		TestEqual(TEXT("Far-field layer, Lumen's far field off: no geometry"), N, 0);
		TestEqual(TEXT("Far-field layer, Lumen's far field off: no section to trace"), Component->GetRayTracingSectionCount(), 0);
		{
			// The cvar's change recreates every proxy.
			const FScopedCVar On(TEXT("r.LumenScene.FarField"), 1);
			N = Geometries();
			TestEqual(TEXT("Far-field layer, Lumen's far field on: the terrain's geometry alone"), N, 1);
			TestEqual(TEXT("Far-field layer, Lumen's far field on: one section to trace"), Component->GetRayTracingSectionCount(), 1);

			// The terrain section rebuilt as an underside: still the one traced section
			Component->SetSection(0, MakeGrid(3, 0.0f), true, /*bRayTracing=*/true);
			N = Geometries();
			TestEqual(TEXT("Far-field layer, rebuilt terrain: its geometry alone"), N, 1);
		}
		N = Geometries();
		TestEqual(TEXT("Lumen's far field off again: no geometry"), N, 0);

		SetLayer(false, true);
		N = Geometries();
		TestEqual(TEXT("Near-field layer: the terrain's geometry alone"), N, 1);

		SetLayer(false, false);
		N = Geometries();
		TestEqual(TEXT("Out of the ray tracing scene: no geometry"), N, 0);

		// A hidden section keeps drawing state on the proxy without a recreation
		SetLayer(false, true);
		Geometries();
		Component->SetSectionVisible(0, false);
		FlushRenderingCommands();
		TestFalse(TEXT("Section visibility on the game thread"), Component->IsSectionVisible(0));
		TestEqual(TEXT("A hidden section keeps its geometry (no recreation)"), Component->GetProxyRayTracingGeometryCount(), 1);

		Component->DestroyComponent();
		Owner->Destroy();
		World->SendAllEndOfFrameUpdates();
	}
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
