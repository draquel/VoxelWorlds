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
				"VoxelCore",
				"VoxelGeneration",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"VoxelStreaming",
				"VoxelMap",
			}
		);

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
			}
		);
	}
}
