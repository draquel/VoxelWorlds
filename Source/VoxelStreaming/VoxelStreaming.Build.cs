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
				"InputCore",
				"VoxelCore",
				"VoxelLOD",
				"VoxelRendering",
				"VoxelGeneration",
				"VoxelMeshing",
				"VoxelScatter",
				"PhysicsCore",
				"Chaos",
				"ChaosCore",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"ProceduralMeshComponent", // the far ocean's meshes (WM-E1)
				"RenderCore", // the bench's game and render thread times (stat unit's)
				"RHI", // the bench's GPU frame time
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
