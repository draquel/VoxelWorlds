// Copyright Daniel Raquel. All Rights Reserved.

using UnrealBuildTool;

public class VoxelStreaming : ModuleRules
{
	public VoxelStreaming(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.AddRange(
			new string[] {
			}
		);

		PrivateIncludePaths.AddRange(
			new string[] {
			}
		);

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore", // linked, not included: EKeys in the test actor (AVoxelWorldTestActor; leaves with it, D-CR11)
				"VoxelCore",
				"VoxelLOD",
				"VoxelRendering",
				"VoxelGeneration",
				"VoxelMeshing",
				"Chaos", // VoxelCollisionManager.h: Chaos/TriangleMeshImplicitObject.h (FAsyncCollisionResult::TriMesh)
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"ProceduralMeshComponent", // the far ocean's meshes (WM-E1)
				"RenderCore", // the bench's game and render thread times (stat unit's)
				"RHI", // the bench's GPU frame time
				"PhysicsCore", // VoxelCollisionManager.cpp: UPhysicalMaterial; the public headers reach FCollisionShape through Engine
				"VoxelScatter", // the chunk manager, bench and test actor; VoxelChunkManager.h only forward-declares UVoxelScatterManager
			}
		);

		// Editor-only dependencies for viewport camera tracking
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
			}
		);
	}
}
