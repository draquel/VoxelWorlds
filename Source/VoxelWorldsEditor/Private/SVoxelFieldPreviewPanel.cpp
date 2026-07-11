// Copyright Daniel Raquel. All Rights Reserved.

#include "SVoxelFieldPreviewPanel.h"

#include "VoxelFieldTypes.h"
#include "VoxelWorldConfiguration.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SScrollBox.h"

#include "PropertyCustomizationHelpers.h"   // SObjectPropertyEntryBox
#include "AssetRegistry/AssetData.h"
#include "Engine/Texture2D.h"
#include "Styling/AppStyle.h"
#include "Brushes/SlateColorBrush.h"

#define LOCTEXT_NAMESPACE "SVoxelFieldPreviewPanel"

void SVoxelFieldPreviewPanel::Construct(const FArguments& InArgs)
{
	BakeParams.Center = FVector2D::ZeroVector;
	BakeParams.RegionSize = 51200.0;
	BakeParams.Resolution = 256;
	BakeParams.SampleZ = 0.0;
	StatusText = LOCTEXT("PickConfig", "Pick a world configuration to begin.");

	RebuildFieldOptions();

	PreviewBrush.SetResourceObject(nullptr);
	PreviewBrush.ImageSize = FVector2D(BakeParams.Resolution, BakeParams.Resolution);
	PreviewBrush.DrawAs = ESlateBrushDrawType::Image;

	// Local factory for a labeled double entry (keeps the layout below terse).
	auto MakeDoubleEntry = [](TFunction<double()> Get, TFunction<void(double)> Set) -> TSharedRef<SWidget>
	{
		return SNew(SNumericEntryBox<double>)
			.AllowSpin(false)
			.MinDesiredValueWidth(70.f)
			.Value_Lambda([Get]() { return TOptional<double>(Get()); })
			.OnValueCommitted_Lambda([Set](double NewValue, ETextCommit::Type) { Set(NewValue); });
	};

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.f)
		[
			SNew(SVerticalBox)

			// Configuration
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[ SNew(STextBlock).MinDesiredWidth(90.f).Text(LOCTEXT("ConfigLabel", "Configuration")) ]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(SObjectPropertyEntryBox)
					.AllowedClass(UVoxelWorldConfiguration::StaticClass())
					.ObjectPath(this, &SVoxelFieldPreviewPanel::GetConfigPath)
					.OnObjectChanged(this, &SVoxelFieldPreviewPanel::OnConfigChanged)
					.AllowClear(true)
					.DisplayThumbnail(false)
				]
			]

			// Field
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[ SNew(STextBlock).MinDesiredWidth(90.f).Text(LOCTEXT("FieldLabel", "Field")) ]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SAssignNew(FieldCombo, SComboBox<TSharedPtr<FName>>)
					.OptionsSource(&FieldOptions)
					.OnGenerateWidget(this, &SVoxelFieldPreviewPanel::MakeFieldComboItem)
					.OnSelectionChanged(this, &SVoxelFieldPreviewPanel::OnFieldSelectionChanged)
					.InitiallySelectedItem(SelectedField)
					[ SNew(STextBlock).Text(this, &SVoxelFieldPreviewPanel::GetSelectedFieldText) ]
				]
			]

			// Center X / Y
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[ SNew(STextBlock).MinDesiredWidth(90.f).Text(LOCTEXT("CenterLabel", "Center X / Y")) ]
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 4, 0)
				[ MakeDoubleEntry([this] { return BakeParams.Center.X; }, [this](double V) { BakeParams.Center.X = V; }) ]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[ MakeDoubleEntry([this] { return BakeParams.Center.Y; }, [this](double V) { BakeParams.Center.Y = V; }) ]
			]

			// Region size / Resolution
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[ SNew(STextBlock).MinDesiredWidth(90.f).Text(LOCTEXT("RegionLabel", "Region / Res")) ]
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 4, 0)
				[ MakeDoubleEntry([this] { return BakeParams.RegionSize; }, [this](double V) { BakeParams.RegionSize = FMath::Max(1.0, V); }) ]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(SNumericEntryBox<int32>)
					.AllowSpin(false)
					.MinValue(1).MaxValue(4096)
					.MinDesiredValueWidth(70.f)
					.Value_Lambda([this] { return TOptional<int32>(BakeParams.Resolution); })
					.OnValueCommitted_Lambda([this](int32 V, ETextCommit::Type) { BakeParams.Resolution = FMath::Clamp(V, 1, 4096); })
				]
			]

			// Sample Z (for 3D fields such as cave presence)
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[ SNew(STextBlock).MinDesiredWidth(90.f).Text(LOCTEXT("SampleZLabel", "Sample Z (3D)")) ]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[ MakeDoubleEntry([this] { return BakeParams.SampleZ; }, [this](double V) { BakeParams.SampleZ = V; }) ]
			]

			// Bake
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f, 6.f)
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.Text(LOCTEXT("BakeButton", "Bake Field"))
				.OnClicked(this, &SVoxelFieldPreviewPanel::OnBakeClicked)
			]

			// Status
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f)
			[ SNew(STextBlock).AutoWrapText(true).Text(this, &SVoxelFieldPreviewPanel::GetStatusText) ]

			// Field texture + legend, split horizontally: the texture fills the remaining width and
			// scales to fit (square, larger when maximized); the legend takes only the width it needs.
			+ SVerticalBox::Slot().FillHeight(1.f).Padding(2.f, 6.f)
			[
				SNew(SHorizontalBox)

				// Field texture (fills remaining width).
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0.f, 0.f, 4.f, 0.f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
					.Padding(2.f)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						SNew(SScaleBox)
						.Stretch(EStretch::ScaleToFit)
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						[
							SNew(SImage).Image(this, &SVoxelFieldPreviewPanel::GetPreviewBrush)
						]
					]
				]

				// Legend — only as wide as its content (capped), scrolls vertically.
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
					.Padding(6.f)
					[
						SNew(SBox).MinDesiredWidth(120.f).MaxDesiredWidth(240.f)
						[
							SNew(SScrollBox)
							+ SScrollBox::Slot()
							[ SAssignNew(LegendBox, SVerticalBox) ]
						]
					]
				]
			]
		]
	];
}

FString SVoxelFieldPreviewPanel::GetConfigPath() const
{
	return Config.IsValid() ? Config->GetPathName() : FString();
}

void SVoxelFieldPreviewPanel::OnConfigChanged(const FAssetData& AssetData)
{
	UVoxelWorldConfiguration* NewConfig = Cast<UVoxelWorldConfiguration>(AssetData.GetAsset());
	Config = TStrongObjectPtr<UVoxelWorldConfiguration>(NewConfig);
	Rebake();
}

void SVoxelFieldPreviewPanel::RebuildFieldOptions()
{
	FVoxelFieldRegistry::EnsureBuiltinsRegistered();

	FieldOptions.Reset();
	for (const FVoxelEditorField& Field : FVoxelFieldRegistry::GetFields())
	{
		FieldOptions.Add(MakeShared<FName>(Field.Id));
	}
	if (!SelectedField.IsValid() && FieldOptions.Num() > 0)
	{
		SelectedField = FieldOptions[0];
	}
}

TSharedRef<SWidget> SVoxelFieldPreviewPanel::MakeFieldComboItem(TSharedPtr<FName> InItem)
{
	FText Label = InItem.IsValid() ? FText::FromName(*InItem) : FText::GetEmpty();
	if (InItem.IsValid())
	{
		if (const FVoxelEditorField* Field = FVoxelFieldRegistry::FindField(*InItem))
		{
			Label = Field->DisplayName;
		}
	}
	return SNew(STextBlock).Text(Label);
}

void SVoxelFieldPreviewPanel::OnFieldSelectionChanged(TSharedPtr<FName> NewSelection, ESelectInfo::Type)
{
	if (NewSelection.IsValid())
	{
		SelectedField = NewSelection;
		Rebake();
	}
}

FText SVoxelFieldPreviewPanel::GetSelectedFieldText() const
{
	if (SelectedField.IsValid())
	{
		if (const FVoxelEditorField* Field = FVoxelFieldRegistry::FindField(*SelectedField))
		{
			return Field->DisplayName;
		}
		return FText::FromName(*SelectedField);
	}
	return LOCTEXT("SelectFieldPrompt", "Select a field");
}

FReply SVoxelFieldPreviewPanel::OnBakeClicked()
{
	Rebake();
	return FReply::Handled();
}

void SVoxelFieldPreviewPanel::Rebake()
{
	if (!Config.IsValid())
	{
		StatusText = LOCTEXT("NoConfigStatus", "No configuration selected.");
		PreviewBrush.SetResourceObject(nullptr);
		if (LegendBox.IsValid())
		{
			LegendBox->ClearChildren();
		}
		return;
	}
	if (!SelectedField.IsValid())
	{
		StatusText = LOCTEXT("NoFieldStatus", "No field selected.");
		return;
	}

	const FVoxelEditorField* Field = FVoxelFieldRegistry::FindField(*SelectedField);
	if (!Field)
	{
		StatusText = LOCTEXT("BadFieldStatus", "Selected field is not registered.");
		return;
	}

	FVoxelFieldSampleContext Ctx = FVoxelFieldSampleContext::FromConfiguration(Config.Get());
	if (!Ctx.IsValid())
	{
		StatusText = LOCTEXT("BadContextStatus", "Failed to build a sample context from the configuration.");
		return;
	}

	float RangeMin = 0.f, RangeMax = 0.f;
	TArray<int32> PresentCategoryIds;
	UTexture2D* Tex = FVoxelFieldImageBaker::BakeToTexture(*Field, Ctx, BakeParams, PreviewTexture.Get(), RangeMin, RangeMax, &PresentCategoryIds);
	PreviewTexture = TStrongObjectPtr<UTexture2D>(Tex);
	PreviewBrush.SetResourceObject(Tex);
	PreviewBrush.ImageSize = FVector2D(BakeParams.Resolution, BakeParams.Resolution);

	StatusText = FText::Format(
		LOCTEXT("BakedStatus", "{0}  -  {1}px  -  region {2} uu  -  Z {3}"),
		Field->DisplayName,
		FText::AsNumber(BakeParams.Resolution),
		FText::AsNumber(static_cast<int64>(BakeParams.RegionSize)),
		FText::AsNumber(static_cast<int64>(BakeParams.SampleZ)));

	RebuildLegend(Field, Ctx, PresentCategoryIds, RangeMin, RangeMax);
}

void SVoxelFieldPreviewPanel::RebuildLegend(const FVoxelEditorField* Field, const FVoxelFieldSampleContext& Ctx,
	const TArray<int32>& PresentCategoryIds, float RangeMin, float RangeMax)
{
	if (!LegendBox.IsValid())
	{
		return;
	}
	LegendBox->ClearChildren();
	if (!Field)
	{
		return;
	}

	// Dependency-free solid white brush, tinted per-swatch via SImage.ColorAndOpacity. Static so its
	// address stays valid for the lifetime of the SImages that reference it (no style-name lookup).
	static const FSlateColorBrush SolidWhiteBrush(FLinearColor::White);
	const FSlateBrush* WhiteBrush = &SolidWhiteBrush;

	// A solid color swatch = white brush tinted by a LINEAR color (Slate re-encodes for display,
	// so passing the linear form makes the swatch match the sRGB texture exactly).
	auto MakeSwatch = [WhiteBrush](const FLinearColor& LinearColor, float SizeXY) -> TSharedRef<SWidget>
	{
		return SNew(SBox).WidthOverride(SizeXY).HeightOverride(SizeXY)
			[ SNew(SImage).Image(WhiteBrush).ColorAndOpacity(LinearColor) ];
	};

	if (Field->Kind == EVoxelFieldKind::Categorical)
	{
		LegendBox->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
		[
			SNew(STextBlock).Text(FText::Format(
				LOCTEXT("LegendHeaderCat", "{0} — {1} in region"),
				Field->DisplayName, FText::AsNumber(PresentCategoryIds.Num())))
		];

		if (PresentCategoryIds.Num() == 0)
		{
			LegendBox->AddSlot().AutoHeight()[ SNew(STextBlock).Text(LOCTEXT("LegendEmpty", "(none in region)")) ];
		}

		for (int32 Id : PresentCategoryIds)
		{
			// MapCategoricalToColor returns an sRGB FColor (the texture's bytes); FromSRGBColor gives the
			// linear form the swatch needs so it renders identically to the heatmap.
			const FLinearColor SwatchColor = FLinearColor::FromSRGBColor(Field->MapCategoricalToColor(static_cast<float>(Id)));
			const FText Label = Field->GetCategoryLabel(Ctx, Id);
			LegendBox->AddSlot().AutoHeight().Padding(0.f, 1.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 6.f, 0.f)[ MakeSwatch(SwatchColor, 16.f) ]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)[ SNew(STextBlock).Text(Label) ]
			];
		}
	}
	else
	{
		LegendBox->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
		[ SNew(STextBlock).Text(Field->DisplayName) ];

		// Horizontal gradient bar from RampLow -> RampHigh (ramp colors are already linear).
		TSharedRef<SHorizontalBox> Gradient = SNew(SHorizontalBox);
		const int32 Steps = 48;
		for (int32 i = 0; i < Steps; ++i)
		{
			const float T = (Steps > 1) ? static_cast<float>(i) / static_cast<float>(Steps - 1) : 0.f;
			const FLinearColor StepColor = Field->RampLow + (Field->RampHigh - Field->RampLow) * T;
			Gradient->AddSlot().FillWidth(1.f)[ SNew(SImage).Image(WhiteBrush).ColorAndOpacity(StepColor) ];
		}
		LegendBox->AddSlot().AutoHeight()[ SNew(SBox).HeightOverride(16.f)[ Gradient ] ];

		LegendBox->AddSlot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()[ SNew(STextBlock).Text(FText::AsNumber(RangeMin)) ]
			+ SHorizontalBox::Slot().FillWidth(1.f).HAlign(HAlign_Center)[ SNew(STextBlock).Text(Field->Units) ]
			+ SHorizontalBox::Slot().AutoWidth()[ SNew(STextBlock).Text(FText::AsNumber(RangeMax)) ]
		];
	}
}

#undef LOCTEXT_NAMESPACE
