// Copyright Daniel Raquel. All Rights Reserved.

using UnrealBuildTool;

public class VoxelNavigation : ModuleRules
{
	public VoxelNavigation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"VoxelCore",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"VoxelStreaming", // chunk manager: edit-merged / analytic surface queries, load + edit events
				"VoxelGeneration", // FVoxelSurfaceSample (the planet surface along local up)
				"VoxelLOD",       // tests: a chunk manager with the distance-band strategy
				"VoxelRendering", // tests: a renderer stub (IVoxelMeshRenderer)
			}
		);
	}
}
