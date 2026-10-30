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
				"VoxelCore",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// The public headers only forward-declare UWorld; Engine headers are included from Private/ alone
				"Engine",
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
