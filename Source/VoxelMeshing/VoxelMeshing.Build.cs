// Copyright Daniel Raquel. All Rights Reserved.

using UnrealBuildTool;

public class VoxelMeshing : ModuleRules
{
	public VoxelMeshing(ReadOnlyTargetRules Target) : base(Target)
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
				"RHI", // IVoxelMesher.h (RHI.h), the GPU meshers' FRHIGPUBufferReadback (RHIGPUReadback.h)
				"VoxelCore",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// RDG / FGlobalShader in the GPU meshers' .cpps and the tests; the public headers only
				// forward-declare FRDGPooledBuffer
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
