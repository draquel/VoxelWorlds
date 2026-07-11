// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class SDockTab;
class FSpawnTabArgs;

/** Log category for the VoxelWorlds editor tooling. */
DECLARE_LOG_CATEGORY_EXTERN(LogVoxelEditor, Log, All);

/**
 * Editor-only module for VoxelWorlds authoring tools.
 *
 * Registered as Type "Editor" in VoxelWorlds.uplugin — it and everything it contains are
 * stripped from cooked/shipping builds, satisfying the "editor tooling must not ship in the
 * delivered game" requirement without per-symbol WITH_EDITOR gating.
 *
 * StartupModule registers the built-in visualization fields, the dockable "Voxel Field Preview"
 * nomad tab, and its Tools-menu entry.
 */
class FVoxelWorldsEditorModule : public IModuleInterface
{
public:
	// IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	/** Add the "Tools > Voxel Worlds > Voxel Field Preview" menu entry. */
	void RegisterMenus();

	/** Spawn the dockable field-preview panel tab. */
	TSharedRef<SDockTab> SpawnPreviewTab(const FSpawnTabArgs& Args);
};
