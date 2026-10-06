// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for placement, transform limits and audio volume rules (Docs/ADR/0002-placement-and-audio.md).
// No world: pure functions and world-independent state transitions only.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Placement+StageCraft.Gizmo+StageCraft.Audio; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Audio/StageAudioTypes.h"
#include "Data/StageItemTypes.h"
#include "Engine/HitResult.h"
#include "Interaction/StageTransformRules.h"
#include "Misc/AutomationTest.h"
#include <limits>
#include "Placement/StagePlacementMath.h"
#include "Placement/StagePlacementToolComponent.h"
#include "Settings/StageCraftUserSettings.h"
#include "UObject/Package.h"

namespace StagePlacementTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;
	constexpr double Tolerance = 1.e-4;

	FString Str(const FVector& Value) { return Value.ToString(); }
	FString Str(EStageEditMode Value) { return UEnum::GetValueAsString(Value); }
	FString Str(EStagePlacementPreviewState Value) { return UEnum::GetValueAsString(Value); }

	bool NearlyEqual(const FVector& A, const FVector& B) { return A.Equals(B, Tolerance); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStagePlacementMathTest, "StageCraft.Placement.Math", StagePlacementTests::Flags)

bool FStagePlacementMathTest::RunTest(const FString& Parameters)
{
	using namespace StagePlacementTests;

	FStageItemPlacementRules Rules;
	Rules.GridSize = FVector(100.0, 50.0, 0.0);

	// Per-axis snapping; a zero cell leaves the axis untouched.
	FTransform Result = StagePlacementMath::ComputePlacementTransform(Rules, FVector(149.0, 76.0, 12.3), FVector::UpVector);
	TestTrue(TEXT("Snaps X to 100 and Y to 50, keeps Z"), NearlyEqual(Result.GetLocation(), FVector(100.0, 100.0, 12.3)));
	TestTrue(TEXT("No surface alignment by default"), Result.GetRotation().Equals(FQuat::Identity, Tolerance));
	TestTrue(TEXT("Grid rules use snapping"), StagePlacementMath::UsesGridSnapping(Rules));

	// Negative coordinates round to the nearest cell, not towards zero.
	Result = StagePlacementMath::ComputePlacementTransform(Rules, FVector(-151.0, -24.0, 0.0), FVector::UpVector);
	TestTrue(TEXT("Negative values snap to the nearest cell"), NearlyEqual(Result.GetLocation(), FVector(-200.0, 0.0, 0.0)));

	// No grid at all: the exact hit point.
	FStageItemPlacementRules Free;
	Free.GridSize = FVector::ZeroVector;
	Result = StagePlacementMath::ComputePlacementTransform(Free, FVector(13.7, -2.2, 5.0), FVector::UpVector);
	TestTrue(TEXT("Zero grid keeps the hit point"), NearlyEqual(Result.GetLocation(), FVector(13.7, -2.2, 5.0)));
	TestFalse(TEXT("Zero grid does not use snapping"), StagePlacementMath::UsesGridSnapping(Free));

	// Offset is applied after snapping, in item space.
	Free.PlacementOffset = FVector(0.0, 0.0, 25.0);
	Result = StagePlacementMath::ComputePlacementTransform(Free, FVector(0.0, 0.0, 100.0), FVector::UpVector);
	TestEqual(TEXT("Offset lifts the pivot"), Str(Result.GetLocation()), Str(FVector(0.0, 0.0, 125.0)));

	// Aligned to a wall facing +X: the item's up axis follows the normal and the offset rotates with it.
	Free.bAlignToSurfaceNormal = true;
	Result = StagePlacementMath::ComputePlacementTransform(Free, FVector(500.0, 0.0, 200.0), FVector::ForwardVector);
	TestTrue(TEXT("Up axis follows the surface normal"), NearlyEqual(Result.GetRotation().GetUpVector(), FVector::ForwardVector));
	TestTrue(TEXT("Offset is rotated onto the normal"), NearlyEqual(Result.GetLocation(), FVector(525.0, 0.0, 200.0)));

	// A degenerate hit normal must not produce NaNs.
	Result = StagePlacementMath::ComputePlacementTransform(Free, FVector(1.0, 2.0, 3.0), FVector::ZeroVector);
	TestFalse(TEXT("Zero normal gives a finite transform"), Result.ContainsNaN());
	TestTrue(TEXT("Zero normal falls back to world up"), NearlyEqual(Result.GetRotation().GetUpVector(), FVector::UpVector));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageGizmoScaleRulesTest, "StageCraft.Gizmo.ScaleRules", StagePlacementTests::Flags)

bool FStageGizmoScaleRulesTest::RunTest(const FString& Parameters)
{
	using namespace StagePlacementTests;
	using namespace StageTransformRules;

	TestEqual(TEXT("No drag keeps the scale"), ComputeDragScaleFactor(0.0, 80.0), 1.0, Tolerance);
	TestEqual(TEXT("One handle length outwards doubles"), ComputeDragScaleFactor(80.0, 80.0), 2.0, Tolerance);
	TestEqual(TEXT("Half a handle inwards halves"), ComputeDragScaleFactor(-40.0, 80.0), 0.5, Tolerance);
	TestEqual(TEXT("Dragging past the pivot is floored, never negative"), ComputeDragScaleFactor(-500.0, 80.0), MinScale, Tolerance);
	TestEqual(TEXT("No reference length leaves the scale alone"), ComputeDragScaleFactor(50.0, 0.0), 1.0, Tolerance);
	TestEqual(TEXT("NaN drag leaves the scale alone"), ComputeDragScaleFactor(std::numeric_limits<double>::quiet_NaN(), 80.0), 1.0, Tolerance);

	TestEqual(TEXT("ClampScale keeps valid values"), Str(ClampScale(FVector(0.5, 1.0, 3.0))), Str(FVector(0.5, 1.0, 3.0)));
	TestEqual(TEXT("ClampScale clamps both ends and repairs NaN"),
		Str(ClampScale(FVector(0.0, 1000.0, std::numeric_limits<double>::quiet_NaN()))), Str(FVector(MinScale, MaxScale, 1.0)));
	TestEqual(TEXT("ClampScale makes negative (mirrored) scale the minimum"), Str(ClampScale(FVector(-2.0, 1.0, 1.0))), Str(FVector(MinScale, 1.0, 1.0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAudioMathTest, "StageCraft.Audio.Math", StagePlacementTests::Flags)

bool FStageAudioMathTest::RunTest(const FString& Parameters)
{
	using namespace StageAudioMath;

	TestEqual(TEXT("Volumes clamp below 0"), SanitizeVolume(-0.5f), 0.f);
	TestEqual(TEXT("Volumes clamp above 1"), SanitizeVolume(3.f), 1.f);
	TestEqual(TEXT("Non-finite volume becomes the default"), SanitizeVolume(std::numeric_limits<float>::quiet_NaN()), 1.f);

	TestEqual(TEXT("Master gain is the master volume"), ComputeMasterGain(0.6f, false), 0.6f);
	TestEqual(TEXT("Mute wins over any master volume"), ComputeMasterGain(1.f, true), 0.f);

	TestEqual(TEXT("Cue gain is effects x cue level"), ComputeCueGain(0.5f, 0.8f), 0.4f, 1.e-5f);
	TestEqual(TEXT("Effects at 0 silences cues"), ComputeCueGain(0.f, 1.f), 0.f);
	TestEqual(TEXT("A negative cue level is silent, not inverted"), ComputeCueGain(1.f, -1.f), 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAudioUserSettingsTest, "StageCraft.Audio.UserSettings", StagePlacementTests::Flags)

bool FStageAudioUserSettingsTest::RunTest(const FString& Parameters)
{
	// A transient instance: the engine's settings object and GameUserSettings.ini are never touched.
	UStageCraftUserSettings* Settings = NewObject<UStageCraftUserSettings>(GetTransientPackage());

	TestEqual(TEXT("Default master"), Settings->GetMasterVolume(), 1.f);
	TestEqual(TEXT("Default effects"), Settings->GetEffectsVolume(), 1.f);
	TestFalse(TEXT("Default unmuted"), Settings->IsAudioMuted());

	TestTrue(TEXT("Changing master reports a change"), Settings->SetMasterVolume(0.25f));
	TestFalse(TEXT("Setting the same master again reports no change"), Settings->SetMasterVolume(0.25f));
	Settings->SetMasterVolume(7.f);
	TestEqual(TEXT("Master is clamped to 1"), Settings->GetMasterVolume(), 1.f);
	Settings->SetEffectsVolume(-1.f);
	TestEqual(TEXT("Effects is clamped to 0"), Settings->GetEffectsVolume(), 0.f);
	TestTrue(TEXT("Muting reports a change"), Settings->SetAudioMuted(true));
	TestTrue(TEXT("Muted"), Settings->IsAudioMuted());

	Settings->SetToDefaults();
	TestEqual(TEXT("SetToDefaults resets master"), Settings->GetMasterVolume(), 1.f);
	TestEqual(TEXT("SetToDefaults resets effects"), Settings->GetEffectsVolume(), 1.f);
	TestFalse(TEXT("SetToDefaults unmutes"), Settings->IsAudioMuted());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStagePlacementToolModesTest, "StageCraft.Placement.ToolModes", StagePlacementTests::Flags)

bool FStagePlacementToolModesTest::RunTest(const FString& Parameters)
{
	using namespace StagePlacementTests;

	// No owner and no world: only the state machine is exercised; nothing can be spawned.
	UStagePlacementToolComponent* Tool = NewObject<UStagePlacementToolComponent>(GetTransientPackage());

	TestEqual(TEXT("Starts in Select mode"), Str(Tool->GetEditMode()), Str(EStageEditMode::Select));
	TestNull(TEXT("Select mode never places"), Tool->TryPlace(FHitResult()));

	Tool->EnterPlaceMode();
	TestEqual(TEXT("Enters Place mode"), Str(Tool->GetEditMode()), Str(EStageEditMode::Place));
	Tool->EnterPlaceMode();
	TestEqual(TEXT("Entering twice is idempotent"), Str(Tool->GetEditMode()), Str(EStageEditMode::Place));

	// Nothing armed: the preview stays hidden whatever the cursor hits.
	FHitResult FloorHit;
	FloorHit.bBlockingHit = true;
	FloorHit.ImpactPoint = FVector(100.0, 0.0, 0.0);
	FloorHit.ImpactNormal = FVector::UpVector;
	Tool->UpdateTarget(FloorHit);
	TestEqual(TEXT("No armed item: preview hidden"), Str(Tool->GetPreviewState()), Str(EStagePlacementPreviewState::Hidden));
	TestNull(TEXT("No armed item: nothing placed"), Tool->TryPlace(FloorHit));
	TestEqual(TEXT("A failed place keeps Place mode"), Str(Tool->GetEditMode()), Str(EStageEditMode::Place));

	Tool->TogglePlaceMode();
	TestEqual(TEXT("Toggle leaves Place mode"), Str(Tool->GetEditMode()), Str(EStageEditMode::Select));
	Tool->TogglePlaceMode();
	TestEqual(TEXT("Toggle enters Place mode"), Str(Tool->GetEditMode()), Str(EStageEditMode::Place));

	Tool->EnterSelectMode();
	Tool->EnterSelectMode();
	TestEqual(TEXT("Leaving twice is idempotent"), Str(Tool->GetEditMode()), Str(EStageEditMode::Select));
	TestEqual(TEXT("Select mode hides the preview"), Str(Tool->GetPreviewState()), Str(EStagePlacementPreviewState::Hidden));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
