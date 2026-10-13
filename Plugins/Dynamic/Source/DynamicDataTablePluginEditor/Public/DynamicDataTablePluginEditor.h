// Copyright Vyrulent 2023 All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class FDynamicDataTablePluginEditorModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
