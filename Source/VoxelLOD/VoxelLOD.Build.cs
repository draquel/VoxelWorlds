// Copyright Daniel Raquel. All Rights Reserved.

using UnrealBuildTool;

public class VoxelLOD : ModuleRules
{
	public VoxelLOD(ReadOnlyTargetRules Target) : base(Target)
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
				// Vertical terrain-culling bounds come from the generation height math
				// (FInfinitePlaneWorldMode::GetTerrainHeightBounds) — the single source of truth.
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
