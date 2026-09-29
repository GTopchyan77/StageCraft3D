// Copyright 2025-2026 NGG. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * FNGGChatTabSpawner
 *
 * Registers the "NGGChatTab" nomad tab and the "Claude" Level Editor
 * toolbar button. Designed to be called from the plugin module's
 * StartupModule / ShutdownModule.
 */
class FNGGChatTabSpawner
{
public:
	static void Register();
	static void Unregister();

	/** Tab ID used by FGlobalTabmanager. Exposed for InvokeTab callers. */
	static const FName TabId;

private:
	static TSharedRef<class SDockTab> SpawnChatTab(const class FSpawnTabArgs& Args);
	static void RegisterToolbarExtension();
	static void UnregisterToolbarExtension();

	static bool bRegistered;
};
