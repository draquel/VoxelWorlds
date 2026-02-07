// Copyright Daniel Raquel. All Rights Reserved.

using UnrealBuildTool;

public class VoxelScatter : ModuleRules
{
	public VoxelScatter(ReadOnlyTargetRules Target) : base(Target)
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
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"VoxelGeneration",
			}
		);

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
			}
		);
	}
}
