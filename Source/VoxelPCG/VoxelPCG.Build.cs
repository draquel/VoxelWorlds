// Copyright Daniel Raquel. All Rights Reserved.

using UnrealBuildTool;

public class VoxelPCG : ModuleRules
{
	public VoxelPCG(ReadOnlyTargetRules Target) : base(Target)
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
				// PCG engine plugin — isolated to this module so no other VoxelWorlds
				// module pulls in a hard PCG dependency.
				"PCG",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// The public headers (PCG settings / elements, the biome mapping asset) include no voxel header
				"VoxelCore",
				"VoxelGeneration",
				"VoxelStreaming",
			}
		);

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
			}
		);
	}
}
