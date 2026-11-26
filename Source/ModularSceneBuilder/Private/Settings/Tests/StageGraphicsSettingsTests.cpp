// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the graphics preferences (STATE.md #111): the pure rules, and the translation to and from the engine's
// UGameUserSettings on a transient settings object. Nothing is applied or saved, so the real GameUserSettings.ini and the
// running scalability levels are untouched.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Settings.Graphics; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Settings/StageCraftUserSettings.h"
#include "Settings/StageGraphicsSettings.h"
#include "Settings/StageGraphicsSubsystem.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageGraphicsSettingsTest, "StageCraft.Settings.Graphics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FStageGraphicsSettingsTest::RunTest(const FString& Parameters)
{
	const auto Make = [](EStageGraphicsQuality Quality, int32 Resolution, int32 Cap, bool bVSync)
	{
		FStageGraphicsChoices Choices;
		Choices.Quality = Quality;
		Choices.ResolutionPercent = Resolution;
		Choices.FrameCap = Cap;
		Choices.bVSync = bVSync;
		return Choices;
	};

	// --- Pure rules ---

	TestEqual(TEXT("Resolution row starts with Auto"), StageGraphics::GetResolutionPresets()[0], 0);
	TestEqual(TEXT("Frame cap row starts with Off"), StageGraphics::GetFrameCapPresets()[0], 0);
	for (const int32 Preset : StageGraphics::GetResolutionPresets())
	{
		TestEqual(FString::Printf(TEXT("resolution preset %d is kept by Sanitize"), Preset),
			StageGraphics::Sanitize(Make(EStageGraphicsQuality::Epic, Preset, 0, false)).ResolutionPercent, Preset);
	}
	for (const int32 Preset : StageGraphics::GetFrameCapPresets())
	{
		TestEqual(FString::Printf(TEXT("frame cap preset %d is kept by Sanitize"), Preset),
			StageGraphics::Sanitize(Make(EStageGraphicsQuality::Epic, 0, Preset, false)).FrameCap, Preset);
	}

	TestEqual(TEXT("negative resolution is Auto"), StageGraphics::Sanitize(Make(EStageGraphicsQuality::Epic, -5, 0, false)).ResolutionPercent, 0);
	TestEqual(TEXT("10 % is raised to the minimum"), StageGraphics::Sanitize(Make(EStageGraphicsQuality::Epic, 10, 0, false)).ResolutionPercent, StageGraphics::MinResolutionPercent);
	TestEqual(TEXT("150 % is lowered to 100"), StageGraphics::Sanitize(Make(EStageGraphicsQuality::Epic, 150, 0, false)).ResolutionPercent, 100);
	TestEqual(TEXT("negative cap is Off"), StageGraphics::Sanitize(Make(EStageGraphicsQuality::Epic, 0, -1, false)).FrameCap, 0);
	TestEqual(TEXT("5 fps is raised to the minimum"), StageGraphics::Sanitize(Make(EStageGraphicsQuality::Epic, 0, 5, false)).FrameCap, StageGraphics::MinFrameCap);
	TestEqual(TEXT("1000 fps is lowered to the maximum"), StageGraphics::Sanitize(Make(EStageGraphicsQuality::Epic, 0, 1000, false)).FrameCap, StageGraphics::MaxFrameCap);
	TestTrue(TEXT("an unknown quality is Custom"),
		StageGraphics::Sanitize(Make(static_cast<EStageGraphicsQuality>(9), 0, 0, false)).Quality == EStageGraphicsQuality::Custom);

	TestEqual(TEXT("Low is level 0"), StageGraphics::ToScalabilityLevel(EStageGraphicsQuality::Low), 0);
	TestEqual(TEXT("Epic is level 3"), StageGraphics::ToScalabilityLevel(EStageGraphicsQuality::Epic), 3);
	TestEqual(TEXT("Custom has no level"), StageGraphics::ToScalabilityLevel(EStageGraphicsQuality::Custom), int32(INDEX_NONE));

	TestTrue(TEXT("all groups at 2 are High"), StageGraphics::FromGroupLevels({ 2, 2, 2 }) == EStageGraphicsQuality::High);
	TestTrue(TEXT("mixed groups are Custom"), StageGraphics::FromGroupLevels({ 3, 3, 2 }) == EStageGraphicsQuality::Custom);
	TestTrue(TEXT("Cinematic groups are Custom"), StageGraphics::FromGroupLevels({ 4, 4 }) == EStageGraphicsQuality::Custom);
	TestTrue(TEXT("a negative level is Custom"), StageGraphics::FromGroupLevels({ -1 }) == EStageGraphicsQuality::Custom);
	TestTrue(TEXT("no groups is Custom"), StageGraphics::FromGroupLevels(TConstArrayView<int32>()) == EStageGraphicsQuality::Custom);

	// --- The engine's settings object (transient: never applied, never saved) ---

	UStageCraftUserSettings* Settings = NewObject<UStageCraftUserSettings>(GetTransientPackage());
	Settings->SetOverallScalabilityLevel(3);
	Settings->SetResolutionScaleValueEx(0.f);
	Settings->SetFrameRateLimit(0.f);
	Settings->SetVSyncEnabled(false);
	TestTrue(TEXT("Epic, Auto, no cap, no VSync reads back"),
		UStageGraphicsSubsystem::ReadChoices(*Settings) == Make(EStageGraphicsQuality::Epic, 0, 0, false));

	// Low at full resolution: the engine's own single level would call this Custom (Low's default is 50 %).
	const FStageGraphicsChoices LowFull = Make(EStageGraphicsQuality::Low, 100, 60, true);
	TestTrue(TEXT("writing Low, 100 %, 60 fps, VSync changes something"), UStageGraphicsSubsystem::WriteChoices(*Settings, LowFull));
	TestTrue(TEXT("... and reads back exactly"), UStageGraphicsSubsystem::ReadChoices(*Settings) == LowFull);
	TestEqual(TEXT("... with every group at Low"), Settings->GetShadowQuality(), 0);
	TestFalse(TEXT("writing the same choices again changes nothing"), UStageGraphicsSubsystem::WriteChoices(*Settings, LowFull));

	// A new quality must not leave the engine's per-level resolution behind (Medium's default would be 71 %), also when the
	// Resolution row itself did not change.
	const FStageGraphicsChoices MediumFull = Make(EStageGraphicsQuality::Medium, 100, 60, true);
	UStageGraphicsSubsystem::WriteChoices(*Settings, MediumFull);
	TestTrue(TEXT("Low -> Medium at an unchanged 100 % stays 100 %"), UStageGraphicsSubsystem::ReadChoices(*Settings) == MediumFull);
	const FStageGraphicsChoices MediumAuto = Make(EStageGraphicsQuality::Medium, 0, 0, false);
	UStageGraphicsSubsystem::WriteChoices(*Settings, MediumAuto);
	TestTrue(TEXT("Medium with Auto resolution keeps Auto"), UStageGraphicsSubsystem::ReadChoices(*Settings) == MediumAuto);

	// Out-of-range values are stored sanitized.
	UStageGraphicsSubsystem::WriteChoices(*Settings, Make(EStageGraphicsQuality::Epic, 10, 5, false));
	const FStageGraphicsChoices Clamped = UStageGraphicsSubsystem::ReadChoices(*Settings);
	TestEqual(TEXT("resolution stored at the minimum"), Clamped.ResolutionPercent, StageGraphics::MinResolutionPercent);
	TestEqual(TEXT("frame cap stored at the minimum"), Clamped.FrameCap, StageGraphics::MinFrameCap);

	// Hand-tuned groups read as Custom, and writing Custom with other rows keeps them.
	Settings->SetOverallScalabilityLevel(3);
	Settings->SetShadowQuality(1);
	TestTrue(TEXT("one group off the others reads Custom"), UStageGraphicsSubsystem::ReadChoices(*Settings).Quality == EStageGraphicsQuality::Custom);
	const FStageGraphicsChoices CustomRows = Make(EStageGraphicsQuality::Custom, 75, 30, false);
	TestTrue(TEXT("Custom with a new resolution and cap changes something"), UStageGraphicsSubsystem::WriteChoices(*Settings, CustomRows));
	TestEqual(TEXT("... the shadow group is kept"), Settings->GetShadowQuality(), 1);
	TestEqual(TEXT("... the other groups are kept"), Settings->GetTextureQuality(), 3);
	TestTrue(TEXT("... and the rows read back"), UStageGraphicsSubsystem::ReadChoices(*Settings) == CustomRows);

	Settings->MarkAsGarbage();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
