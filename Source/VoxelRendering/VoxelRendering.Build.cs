// Copyright Daniel Raquel. All Rights Reserved.

using UnrealBuildTool;

public class VoxelRendering : ModuleRules
{
	public VoxelRendering(ReadOnlyTargetRules Target) : base(Target)
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
				"RenderCore",
				"RHI",
				"VoxelCore",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"ProceduralMeshComponent",
			}
		);

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
			}
		);
	}
}
