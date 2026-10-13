//  Copyright WASD Studio 2025 All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class FUMGInSequenceEditorModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
