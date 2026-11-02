// Copyright Epic Games, Inc. All Rights Reserved.

#include "ModularSceneBuilder.h"

#include "Modules/ModuleManager.h"
#include "MoviePlayer.h"
#include "Startup/SStageBrandedLoader.h"

#define LOCTEXT_NAMESPACE "ModularSceneBuilder"

DEFINE_LOG_CATEGORY(LogStageCraft);

/**
 * The game module. Besides the default behaviour, it gives the engine's loading screen StageCraft's loader, so starting the
 * app shows the wordmark while the map loads instead of a blank window (Docs/ADR/0024-startup-atmospheres-starter-stages.md §1).
 */
class FModularSceneBuilderModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		// The editor (and PIE) never shows the engine's loading screen; neither do commandlets and servers.
		if (GIsEditor || IsRunningCommandlet() || IsRunningDedicatedServer() || !IsMoviePlayerEnabled())
		{
			return;
		}
		if (IGameMoviePlayer* MoviePlayer = GetMoviePlayer())
		{
			// The movie player asks just before it starts a loading screen (PreLoadMap), when the UObject system is up.
			PrepareHandle = MoviePlayer->OnPrepareLoadingScreen().AddRaw(this, &FModularSceneBuilderModule::PrepareLoadingScreen);
		}
	}

	virtual void ShutdownModule() override
	{
		if (PrepareHandle.IsValid() && IsMoviePlayerEnabled())
		{
			if (IGameMoviePlayer* MoviePlayer = GetMoviePlayer())
			{
				MoviePlayer->OnPrepareLoadingScreen().Remove(PrepareHandle);
			}
		}
		PrepareHandle.Reset();
	}

private:
	void PrepareLoadingScreen()
	{
		FLoadingScreenAttributes Attributes;
		// The map load reports no fraction, so the bar sweeps; the in-app overlay takes over with real progress (§2).
		Attributes.WidgetLoadingScreen = SNew(SStageBrandedLoader)
			.Status(LOCTEXT("OpeningStage", "Opening the stage…"));
		Attributes.bAutoCompleteWhenLoadingCompletes = true;
		Attributes.bMoviesAreSkippable = false;
		GetMoviePlayer()->SetupLoadingScreen(Attributes);
	}

	FDelegateHandle PrepareHandle;
};

IMPLEMENT_PRIMARY_GAME_MODULE(FModularSceneBuilderModule, ModularSceneBuilder, "ModularSceneBuilder");

#undef LOCTEXT_NAMESPACE
