// Copyright Daniel Raquel. All Rights Reserved.

using UnrealBuildTool;

/**
 * Editor-only module for VoxelWorlds authoring tools (procedural field visualization,
 * config preview, and — in later milestones — non-streaming generation, edit authoring,
 * and claim/POI visualization).
 *
 * Type "Editor" in VoxelWorlds.uplugin, so this module (and every tool in it) is compiled
 * ONLY for editor targets and is stripped from cooked/shipping game builds by construction.
 * Depends only on the runtime voxel modules (downward) — never the reverse.
 */
public class VoxelWorldsEditor : ModuleRules
{
	public VoxelWorldsEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"InputCore", // linked, not included: EKeys through the Slate widgets
			// Editor framework (the public module header only forward-declares SDockTab / FSpawnTabArgs)
			"Slate",
			"SlateCore",
			"PropertyEditor",
			"ToolMenus",
			"WorkspaceMenuStructure",

			// Voxel runtime modules the tooling samples through
			"VoxelCore",
			"VoxelGeneration",
		});
	}
}
