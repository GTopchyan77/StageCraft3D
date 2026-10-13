// Copyright NGG. All Rights Reserved.

#include "NGGVideoRecorderSettings.h"

UNGGVideoRecorderSettings::UNGGVideoRecorderSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("NGG Video Recorder");

	ToggleRecordingKey = EKeys::F9;
	ToggleUIKey = EKeys::F10;
}

const UNGGVideoRecorderSettings& UNGGVideoRecorderSettings::Get()
{
	const UNGGVideoRecorderSettings* Settings = GetDefault<UNGGVideoRecorderSettings>();
	check(Settings);
	return *Settings;
}
