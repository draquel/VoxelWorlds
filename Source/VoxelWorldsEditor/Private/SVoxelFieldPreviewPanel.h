// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "UObject/StrongObjectPtr.h"
#include "Styling/SlateBrush.h"
#include "VoxelFieldImageBaker.h"   // FVoxelFieldBakeParams

class UVoxelWorldConfiguration;
class UTexture2D;
class SVerticalBox;
struct FAssetData;
struct FVoxelEditorField;
struct FVoxelFieldSampleContext;
template<typename OptionType> class SComboBox;

/**
 * Dockable panel that bakes and displays a procedural field (height, biome, cave presence, ...)
 * for a chosen UVoxelWorldConfiguration over a region. Location-agnostic iteration: pick a config
 * asset, a field, a region/resolution/Z, and Bake. Shares the exact core (FVoxelFieldSampleContext
 * + FVoxelFieldImageBaker) with the in-world preview actor.
 */
class SVoxelFieldPreviewPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelFieldPreviewPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	// Config asset picker
	FString GetConfigPath() const;
	void OnConfigChanged(const FAssetData& AssetData);

	// Field combo
	void RebuildFieldOptions();
	TSharedRef<SWidget> MakeFieldComboItem(TSharedPtr<FName> InItem);
	void OnFieldSelectionChanged(TSharedPtr<FName> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetSelectedFieldText() const;

	// Bake
	FReply OnBakeClicked();
	void Rebake();

	// Display bindings
	FText GetStatusText() const { return StatusText; }
	const FSlateBrush* GetPreviewBrush() const { return &PreviewBrush; }

	/** Rebuild the legend (categorical swatches, or a scalar gradient bar) for the just-baked field. */
	void RebuildLegend(const FVoxelEditorField* Field, const FVoxelFieldSampleContext& Ctx,
		const TArray<int32>& PresentCategoryIds, float RangeMin, float RangeMax);

	/** Strong refs keep the picked config + baked texture alive (this widget is not a UObject). */
	TStrongObjectPtr<UVoxelWorldConfiguration> Config;
	TStrongObjectPtr<UTexture2D> PreviewTexture;

	/** Brush wrapping PreviewTexture for the SImage. */
	FSlateBrush PreviewBrush;

	/** Field id options for the combo (built-ins first). */
	TArray<TSharedPtr<FName>> FieldOptions;
	TSharedPtr<FName> SelectedField;
	TSharedPtr<SComboBox<TSharedPtr<FName>>> FieldCombo;

	/** Legend rows (swatch + label, or gradient), rebuilt on each bake. */
	TSharedPtr<SVerticalBox> LegendBox;

	FVoxelFieldBakeParams BakeParams;
	FText StatusText;
};
