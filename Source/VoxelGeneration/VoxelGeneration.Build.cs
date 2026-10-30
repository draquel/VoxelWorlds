// Copyright Daniel Raquel. All Rights Reserved.

using UnrealBuildTool;

public class VoxelGeneration : ModuleRules
{
	public VoxelGeneration(ReadOnlyTargetRules Target) : base(Target)
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
				"RHI", // VoxelGPUNoiseGenerator.h: FRHIGPUBufferReadback (RHIFwd.h)
				"VoxelCore",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// RDG / FGlobalShader in the GPU noise generator's .cpp and the tests; the public header only
				// forward-declares FRDGPooledBuffer
				"RenderCore",
			}
		);

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
			}
		);
	}
}
