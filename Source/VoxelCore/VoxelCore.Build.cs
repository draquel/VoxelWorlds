// Copyright Daniel Raquel. All Rights Reserved.

using UnrealBuildTool;

public class VoxelCore : ModuleRules
{
	public VoxelCore(ReadOnlyTargetRules Target) : base(Target)
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
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// FImage / ERawImageFormat — used by UVoxelMaterialAtlas::BakeMapColors to average
				// albedo source textures in linear space.
				"ImageCore",
			}
		);

		if (Target.bBuildEditor)
		{
			// UVoxelMaterialAtlas: the texture arrays' build key reads source textures' saved package hashes, and new
			// array assets are registered
			PrivateDependencyModuleNames.Add("AssetRegistry");
		}

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
			}
		);
	}
}
