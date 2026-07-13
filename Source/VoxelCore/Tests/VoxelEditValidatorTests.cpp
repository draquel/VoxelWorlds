// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelEditManager.h"
#include "IVoxelEditValidator.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Edit-protection seam: a validator installed on the edit manager vetoes
// Player-source voxels inside a protected box, passes System edits, and a
// brush straddling the boundary edits only the unprotected part.
// ---------------------------------------------------------------------------

namespace VoxelEditValidatorTestUtils
{
	/** Rejects PLAYER edits inside an AABB; everything else passes. */
	struct FDenyBoxValidator : public IVoxelEditValidator
	{
		FBox ProtectedBox;

		explicit FDenyBoxValidator(const FBox& InBox) : ProtectedBox(InBox) {}

		virtual bool CanApplyEdit(const FVector& WorldPos, EEditSource Source) const override
		{
			if (Source != EEditSource::Player)
			{
				return true;
			}
			return !ProtectedBox.IsInsideOrOn(WorldPos);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEditValidatorTest,
	"VoxelWorlds.Edit.Validator.PlayerEditsVetoedInsideProtection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelEditValidatorTest::RunTest(const FString& Parameters)
{
	using namespace VoxelEditValidatorTestUtils;

	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
	Config->ChunkSize = 16;
	Config->VoxelSize = 100.0f;

	UVoxelEditManager* EditManager = NewObject<UVoxelEditManager>();
	EditManager->AddToRoot();
	EditManager->Initialize(Config);

	// Protect a 10m box around the origin.
	FDenyBoxValidator Validator(FBox(FVector(-1000.0f), FVector(1000.0f)));
	EditManager->SetEditValidator(&Validator);

	FVoxelBrushParams Brush;
	Brush.Shape = EVoxelBrushShape::Sphere;
	Brush.Radius = 300.0f;
	Brush.Strength = 1.0f;
	Brush.DensityDelta = 255;

	// Player brush fully inside protection: nothing modified, rejections counted.
	EditManager->SetEditSource(EEditSource::Player);
	const int32 InsideModified = EditManager->ApplyBrushEdit(FVector::ZeroVector, Brush, EEditMode::Subtract);
	TestEqual(TEXT("Player brush inside protection modifies nothing"), InsideModified, 0);
	TestTrue(TEXT("Rejections were counted"), EditManager->GetLastRejectedEditCount() > 0);

	// System edits pass everywhere (the dungeon stamper / POI flatten path).
	EditManager->SetEditSource(EEditSource::System);
	const int32 SystemModified = EditManager->ApplyBrushEdit(FVector::ZeroVector, Brush, EEditMode::Subtract);
	TestTrue(TEXT("System brush inside protection edits normally"), SystemModified > 0);
	TestEqual(TEXT("No rejections for System"), EditManager->GetLastRejectedEditCount(), 0);

	// Player brush far outside protection: unaffected.
	EditManager->SetEditSource(EEditSource::Player);
	const int32 OutsideModified = EditManager->ApplyBrushEdit(FVector(5000.0f, 0.0f, 0.0f), Brush, EEditMode::Subtract);
	TestTrue(TEXT("Player brush outside protection edits normally"), OutsideModified > 0);
	TestEqual(TEXT("No rejections outside"), EditManager->GetLastRejectedEditCount(), 0);

	// Straddling brush: edits the unprotected half only, counts the protected half.
	const int32 StraddleModified = EditManager->ApplyBrushEdit(FVector(1000.0f, 0.0f, 0.0f), Brush, EEditMode::Subtract);
	TestTrue(TEXT("Straddling brush edits the unprotected part"), StraddleModified > 0);
	TestTrue(TEXT("Straddling brush counts the protected part"), EditManager->GetLastRejectedEditCount() > 0);

	// Single-edit path honors the veto too.
	const bool bSingleInside = EditManager->ApplyEdit(FVector::ZeroVector, FVoxelData::Air(), EEditMode::Set);
	TestFalse(TEXT("Single player edit inside protection rejected"), bSingleInside);

	// Removing the validator restores normal editing.
	EditManager->SetEditValidator(nullptr);
	const int32 UnprotectedModified = EditManager->ApplyBrushEdit(FVector::ZeroVector, Brush, EEditMode::Subtract);
	TestTrue(TEXT("No validator -> edits apply"), UnprotectedModified > 0);

	EditManager->Shutdown();
	EditManager->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
