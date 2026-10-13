// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#pragma once

#include "Framework/Commands/Commands.h"
#include "OverCrowdEditorStyle.h"

class FOverCrowdEditorCommands : public TCommands<FOverCrowdEditorCommands>
{
public:

	FOverCrowdEditorCommands()
		: TCommands<FOverCrowdEditorCommands>(TEXT("OverCrowd"), NSLOCTEXT("Contexts", "OverCrowd", "OverCrowd Plugin"), NAME_None, FOverCrowdEditorStyle::GetStyleSetName())
	{
	}

	// TCommands<> interface
	virtual void RegisterCommands() override;

public:
	TSharedPtr< FUICommandInfo > PluginAction;
};
