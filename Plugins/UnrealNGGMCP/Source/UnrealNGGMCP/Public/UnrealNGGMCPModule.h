// Copyright 2025-2026 NGG. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "Containers/Ticker.h"

UNREALNGGMCP_API DECLARE_LOG_CATEGORY_EXTERN(LogNGGBridge, Log, All);

class FNGGHttpServer;

/**
 * FUnrealNGGMCPModule
 *
 * IModuleInterface for the UnrealNGGMCP editor plugin.
 * On startup it instantiates FNGGHttpServer and starts it on the configured
 * port (default 6776).  On shutdown it tears the server down cleanly so the
 * port is released before the editor exits.
 *
 * LoadingPhase is PostEngineInit so that the Asset Registry and all editor
 * subsystems are available when the server first accepts requests.
 */
class UNREALNGGMCP_API FUnrealNGGMCPModule : public IModuleInterface
{
public:
	// ~IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	// ~IModuleInterface

	/** Singleton accessor — safe to call from any game-thread code inside the editor. */
	static FUnrealNGGMCPModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FUnrealNGGMCPModule>("UnrealNGGMCP");
	}

	static bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("UnrealNGGMCP");
	}

	/** Returns the running HTTP server instance for use by extension modules. May be null if startup failed. */
	TSharedPtr<FNGGHttpServer> GetHttpServer() const { return HttpServer; }

private:
	TSharedPtr<FNGGHttpServer> HttpServer;

	/** Drives the first-run `npm install` progress toast (see StartupModule). */
	FTSTicker::FDelegateHandle DepsToastTicker;
};
