// Copyright 2024 Shape-Shift Studio GmbH. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class FFileDropUMGModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
