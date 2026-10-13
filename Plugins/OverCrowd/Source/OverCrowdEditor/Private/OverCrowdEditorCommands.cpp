// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#include "OverCrowdEditorCommands.h"

#define LOCTEXT_NAMESPACE "FOverCrowdEditorModule"

void FOverCrowdEditorCommands::RegisterCommands()
{
	UI_COMMAND(PluginAction, "OverCrowd", "Open Crowd Editor", EUserInterfaceActionType::Button, FInputChord());
}

#undef LOCTEXT_NAMESPACE
