// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelRendering.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"
#include "Components/ComponentInterfaces.h"
#include "VoxelRegionComponent.h"
#include "VoxelChunkSceneProxy.h"
#include "RenderingThread.h"
#include "Containers/Ticker.h"

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

	// The chunk meshes the scene's deletion task queued for release go to the render thread each frame, also when no region
	// flushes any more (the last region component unregistered at the end of play)
	ChunkMeshReleaseTicker = FTSTicker::GetCoreTicker().AddTicker(TEXT("VoxelChunkMeshReleases"), 0.0f, [](float)
	{
		EnqueueDrainChunkMeshReleases();
		return true;
	});

	UE_LOG(LogVoxelRendering, Log, TEXT("VoxelRendering module started - Shader directory: %s"), *PluginShaderDir);
}

void FVoxelRenderingModule::ShutdownModule()
{
	IPrimitiveComponent::RemoveImplementer(UVoxelRegionComponent::StaticClass());
	FTSTicker::RemoveTicker(ChunkMeshReleaseTicker);
	// The chunk meshes still queued for release (G116)
	ENQUEUE_RENDER_COMMAND(DrainVoxelChunkMeshReleasesAtShutdown)([](FRHICommandListImmediate&) { DrainChunkMeshReleases_RenderThread(); });
	FlushRenderingCommands();
	UE_LOG(LogVoxelRendering, Log, TEXT("VoxelRendering module shutdown"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVoxelRenderingModule, VoxelRendering)
