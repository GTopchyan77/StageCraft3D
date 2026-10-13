/// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#include "OverCrowd.h"
#include "GameplayTagsManager.h"

#define LOCTEXT_NAMESPACE "FOverCrowdModule"


DEFINE_LOG_CATEGORY(LogOverCrowdRuntime);

void FOverCrowdModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	//UGameplayTagsManager::Get().AddTagIniSearchPath(FPaths::ProjectPluginsDir() / TEXT("OverCrowd/Config"));
	//UGameplayTagsManager::Get().AddTagIniSearchPath(FPaths::EnginePluginsDir() / TEXT("OverCrowd/Config"));
}

void FOverCrowdModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FOverCrowdModule, OverCrowd)