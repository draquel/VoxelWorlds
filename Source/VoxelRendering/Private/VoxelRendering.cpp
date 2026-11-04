// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelRendering.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"
#include "Components/ComponentInterfaces.h"
#include "VoxelRegionComponent.h"
#include "VoxelChunkSceneProxy.h"
#include "RenderingThread.h"

#define LOCTEXT_NAMESPACE "FVoxelRenderingModule"

DEFINE_LOG_CATEGORY(LogVoxelRendering);

void FVoxelRenderingModule::StartupModule()
{
	// Register shader directory for custom vertex factory
	FString PluginShaderDir = FPaths::Combine(
		IPluginManager::Get().FindPlugin(TEXT("VoxelWorlds"))->GetBaseDir(),
		TEXT("Shaders")
	);

	AddShaderSourceDirectoryMapping(TEXT("/Plugin/VoxelWorlds"), PluginShaderDir);

	// The region components' chunk primitives (G116) are primitives without components: the engine finds them for its
	// material and shader updates (re-caching their draw commands, recreating their render state) through this
	IPrimitiveComponent::AddImplementer(TComponentInterfaceImplementation<IPrimitiveComponent>(UVoxelRegionComponent::StaticClass(),
		TFunction<TArray<IPrimitiveComponent*>(UObject*)>([](UObject* Object)
		{
			return CastChecked<UVoxelRegionComponent>(Object)->GetChunkPrimitiveInterfaces();
		})));

	UE_LOG(LogVoxelRendering, Log, TEXT("VoxelRendering module started - Shader directory: %s"), *PluginShaderDir);
}

void FVoxelRenderingModule::ShutdownModule()
{
	IPrimitiveComponent::RemoveImplementer(UVoxelRegionComponent::StaticClass());
	// The chunk meshes still queued for release (G116)
	ENQUEUE_RENDER_COMMAND(DrainVoxelChunkMeshReleasesAtShutdown)([](FRHICommandListImmediate&) { DrainChunkMeshReleases_RenderThread(); });
	FlushRenderingCommands();
	UE_LOG(LogVoxelRendering, Log, TEXT("VoxelRendering module shutdown"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVoxelRenderingModule, VoxelRendering)
