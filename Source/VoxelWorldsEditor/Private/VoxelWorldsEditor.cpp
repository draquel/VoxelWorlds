// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWorldsEditor.h"
#include "VoxelFieldTypes.h"
#include "SVoxelFieldPreviewPanel.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Textures/SlateIcon.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "FVoxelWorldsEditorModule"

DEFINE_LOG_CATEGORY(LogVoxelEditor);

static const FName VoxelFieldPreviewTabName(TEXT("VoxelFieldPreview"));

void FVoxelWorldsEditorModule::StartupModule()
{
	UE_LOG(LogVoxelEditor, Log, TEXT("VoxelWorldsEditor module started."));

	// Register the built-in visualization fields (Height, Biome, CavePresence, ...) so the
	// preview surfaces can enumerate them. Adding a field later = one RegisterField call.
	FVoxelFieldRegistry::EnsureBuiltinsRegistered();

	// Dockable "Voxel Field Preview" nomad tab.
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
			VoxelFieldPreviewTabName,
			FOnSpawnTab::CreateRaw(this, &FVoxelWorldsEditorModule::SpawnPreviewTab))
		.SetDisplayName(LOCTEXT("VoxelFieldPreviewTitle", "Voxel Field Preview"))
		.SetTooltipText(LOCTEXT("VoxelFieldPreviewTooltip", "Visualize procedural fields (height, biome, caves, ...) for a world configuration."))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Layout"));

	// Tools-menu entry to open it.
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FVoxelWorldsEditorModule::RegisterMenus));
}

void FVoxelWorldsEditorModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);

	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(VoxelFieldPreviewTabName);
	}

	UE_LOG(LogVoxelEditor, Log, TEXT("VoxelWorldsEditor module shut down."));
}

void FVoxelWorldsEditorModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
	if (!Menu)
	{
		return;
	}

	FToolMenuSection& Section = Menu->FindOrAddSection("VoxelWorlds", LOCTEXT("VoxelWorldsMenuSection", "Voxel Worlds"));
	Section.AddMenuEntry(
		"OpenVoxelFieldPreview",
		LOCTEXT("OpenVoxelFieldPreviewLabel", "Voxel Field Preview"),
		LOCTEXT("OpenVoxelFieldPreviewTip", "Open the voxel procedural field visualizer."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Layout"),
		FUIAction(FExecuteAction::CreateLambda([]()
		{
			FGlobalTabmanager::Get()->TryInvokeTab(VoxelFieldPreviewTabName);
		})));
}

TSharedRef<SDockTab> FVoxelWorldsEditorModule::SpawnPreviewTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SVoxelFieldPreviewPanel)
		];
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVoxelWorldsEditorModule, VoxelWorldsEditor)
