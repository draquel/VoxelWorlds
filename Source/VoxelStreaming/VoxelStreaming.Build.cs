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
				"VoxelCore",
				"VoxelLOD",
				"VoxelRendering",
				"VoxelGeneration",
				"VoxelMeshing",
				"PhysicsCore",
				"Chaos",
				"ChaosCore",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
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
