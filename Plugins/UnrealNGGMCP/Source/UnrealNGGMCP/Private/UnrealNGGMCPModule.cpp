// Copyright 2025-2026 NGG. All Rights Reserved.

#include "UnrealNGGMCPModule.h"
#include "NGGHttpServer.h"
#include "NGGMcpConfig.h"
#include "Modules/ModuleManager.h"
#include "Chat/NGGChatTabSpawner.h"
#include "Chat/NodeDepsInstaller.h"
#include "Containers/Ticker.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/App.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "IPAddress.h"

DEFINE_LOG_CATEGORY(LogNGGBridge);

namespace
{
	// Range scanned when the configured port is already in use. Picked so two-
	// dozen concurrent editors can coexist without manual config.
	constexpr uint32 kPortScanFirst = 6776;
	constexpr uint32 kPortScanLast  = 6800;

	/**
	 * Tests whether a TCP port on loopback is free by attempting to bind a
	 * temporary socket to it. Returns true if the bind succeeded (port was
	 * unused at probe time).
	 *
	 * There is a small TOCTOU window between this probe and FHttpServerModule
	 * binding for real — accepted because the alternative (asking the
	 * HTTPServer module after-the-fact whether bind succeeded) is not exposed
	 * in the public API.
	 */
	bool IsLoopbackPortFree(uint32 Port)
	{
		ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (!SocketSubsystem) return false;

		const TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
		bool bIsValid = false;
		Addr->SetIp(TEXT("127.0.0.1"), bIsValid);
		Addr->SetPort(Port);
		if (!bIsValid) return false;

		FSocket* TestSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("NGGPortProbe"), false);
		if (!TestSocket) return false;

		const bool bBound = TestSocket->Bind(*Addr);
		TestSocket->Close();
		SocketSubsystem->DestroySocket(TestSocket);
		return bBound;
	}

	FString GetBridgeDiscoveryPath()
	{
		// <Project>/Saved/UnrealNGGMCP/bridge.json — sidecar reads from the same
		// relative location after walking up from its install dir under Plugins/.
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealNGGMCP"), TEXT("bridge.json"));
	}

	void WriteBridgeDiscoveryFile(uint32 Port)
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("port"), static_cast<double>(Port));
		Root->SetNumberField(TEXT("pid"),  static_cast<double>(FPlatformProcess::GetCurrentProcessId()));
		Root->SetStringField(TEXT("uproject_path"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
		Root->SetStringField(TEXT("project_name"),  FApp::GetProjectName());
		Root->SetStringField(TEXT("started_at"),    FDateTime::UtcNow().ToIso8601());

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);

		const FString Path = GetBridgeDiscoveryPath();
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		PlatformFile.CreateDirectoryTree(*FPaths::GetPath(Path));

		if (!FFileHelper::SaveStringToFile(Out, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogNGGBridge, Warning, TEXT("UnrealNGGMCP: Could not write bridge discovery file at %s"), *Path);
		}
		else
		{
			UE_LOG(LogNGGBridge, Log, TEXT("UnrealNGGMCP: Wrote bridge discovery file %s"), *Path);
		}
	}

	void DeleteBridgeDiscoveryFile()
	{
		const FString Path = GetBridgeDiscoveryPath();
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (PlatformFile.FileExists(*Path))
		{
			PlatformFile.DeleteFile(*Path);
		}
	}
}

// ---------------------------------------------------------------------------
// IModuleInterface
// ---------------------------------------------------------------------------

void FUnrealNGGMCPModule::StartupModule()
{
	// Skip HTTP server when running as a commandlet (cook, package, etc.)
	if (IsRunningCommandlet())
	{
		UE_LOG(LogNGGBridge, Log, TEXT("UnrealNGGMCP: Running as commandlet — skipping HTTP bridge"));
		return;
	}

	UE_LOG(LogNGGBridge, Log, TEXT("UnrealNGGMCP: StartupModule — initialising HTTP bridge"));

	// Default port can be overridden per-project via DefaultEngine.ini:
	//   [UnrealNGGMCP]
	//   Port=6776
	int32 ConfiguredPort = static_cast<int32>(kPortScanFirst);
	GConfig->GetInt(TEXT("UnrealNGGMCP"), TEXT("Port"), ConfiguredPort, GEngineIni);

	// Pick the first free port: try the configured one first, then scan upward.
	// Skipping the scan entirely would mean two editors on the same machine
	// collide on the default port — sidecar would silently talk to whichever
	// editor grabbed it first.
	uint32 ChosenPort = 0;
	{
		const uint32 ConfiguredU = ConfiguredPort > 0 ? static_cast<uint32>(ConfiguredPort) : kPortScanFirst;
		if (IsLoopbackPortFree(ConfiguredU))
		{
			ChosenPort = ConfiguredU;
		}
		else
		{
			UE_LOG(LogNGGBridge, Log,
				TEXT("UnrealNGGMCP: Port %u busy (another editor?) — scanning %u..%u"),
				ConfiguredU, kPortScanFirst, kPortScanLast);

			for (uint32 P = kPortScanFirst; P <= kPortScanLast; ++P)
			{
				if (P == ConfiguredU) continue;
				if (IsLoopbackPortFree(P))
				{
					ChosenPort = P;
					break;
				}
			}
		}
	}

	if (ChosenPort == 0)
	{
		UE_LOG(LogNGGBridge, Error,
			TEXT("UnrealNGGMCP: No free port in range %u..%u. HTTP bridge will not start. "
				 "Close other editors or set a free [UnrealNGGMCP] Port=N in DefaultEngine.ini."),
			kPortScanFirst, kPortScanLast);
		return;
	}

	HttpServer = MakeShared<FNGGHttpServer>(ChosenPort);

	if (!HttpServer->Start())
	{
		UE_LOG(LogNGGBridge, Error,
			TEXT("UnrealNGGMCP: Failed to start HTTP server on port %u. "
				 "Check that no other process is using that port."),
			HttpServer->GetPort());
		HttpServer.Reset();
	}
	else
	{
		// NOTE: UE binds this listener to localhost unless the project overrides
		// [HTTPServer.Listeners] BindAddress, so the port is normally loopback-only.
		// Should it be bound wider, FNGGHttpServer::IsLoopbackPeer still refuses
		// non-local peers with 403 unless AllowRemoteClients is set.
		UE_LOG(LogNGGBridge, Log,
			TEXT("UnrealNGGMCP: HTTP bridge reachable on port %u (local sidecar uses http://localhost:%u)"),
			HttpServer->GetPort(), HttpServer->GetPort());
		WriteBridgeDiscoveryFile(HttpServer->GetPort());
	}

	// Make sure this project can actually launch the MCP server that ships with
	// the plugin: without a .mcp.json naming it, Claude Code has no entry point
	// and the plugin looks dead from the outside. The path written there is
	// derived from where this plugin really sits, so a renamed plugin folder
	// (what a Fab install moved into a project usually ends up as) still works.
	FNGGMcpConfig::EnsureProjectConfig();

	// Claude Chat editor window — tab + toolbar button.
	FNGGChatTabSpawner::Register();

	// First-run bootstrap: the bundled Node.js packages (chat sidecar + MCP
	// server) ship without node_modules, so on a fresh install the chat can't
	// start until `npm install` has run in each. Kick that in the background
	// right at editor startup — by the time the user opens the chat it is
	// usually done — and show a progress toast so they know to wait. The chat
	// window independently polls the same installer state if opened mid-install.
	//
	// The first ticker fire is delayed a second so the main editor window
	// exists before the toast is queued; subsequent fires poll until the
	// install resolves, then complete the toast and unregister.
	if (!FNodeDepsInstaller::AreDepsReady())
	{
		DepsToastTicker = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda(
				[Toast = TSharedPtr<SNotificationItem>(), bKicked = false](float) mutable -> bool
				{
					if (IsEngineExitRequested()) return false;

					// Kick exactly once; toast creation may need extra fires if
					// Slate has no main window yet (AddNotification returns null),
					// and a re-kick after an early failure would loop the install.
					if (!bKicked)
					{
						bKicked = true;
						FNodeDepsInstaller::StartInstallIfNeeded();
					}

					const ENGGDepsState DepsState = FNodeDepsInstaller::GetState();

					if (!Toast.IsValid())
					{
						// If the install resolved before a toast could ever be
						// shown (headless-ish session), just stop — the chat
						// window surfaces the same state on open.
						if (DepsState == ENGGDepsState::Ready || DepsState == ENGGDepsState::Failed)
						{
							return false;
						}
						FNotificationInfo Info(NSLOCTEXT("UnrealNGGMCP", "DepsInstallToast",
							"NGG Chat: first-run setup — installing Node.js packages (npm install).\n"
							"Please wait; the chat becomes available when this finishes."));
						Info.bFireAndForget = false;
						Info.bUseThrobber   = true;
						Info.FadeOutDuration = 2.0f;
						Toast = FSlateNotificationManager::Get().AddNotification(Info);
						if (Toast.IsValid())
						{
							Toast->SetCompletionState(SNotificationItem::CS_Pending);
						}
						return true;
					}

					if (DepsState == ENGGDepsState::Installing || DepsState == ENGGDepsState::Idle)
					{
						const FString Detail = FNodeDepsInstaller::GetStatusDetail();
						if (!Detail.IsEmpty())
						{
							Toast->SetText(FText::FromString(FString::Printf(
								TEXT("NGG Chat: first-run setup\n%s"), *Detail)));
						}
						return true;
					}

					if (DepsState == ENGGDepsState::Ready)
					{
						Toast->SetText(NSLOCTEXT("UnrealNGGMCP", "DepsInstallDone",
							"NGG Chat: Node.js packages installed — the chat is ready to use."));
						Toast->SetCompletionState(SNotificationItem::CS_Success);
					}
					else // Failed
					{
						Toast->SetText(NSLOCTEXT("UnrealNGGMCP", "DepsInstallFail",
							"NGG Chat: npm install failed. Open the NGG Chat window for details "
							"(log in Saved/UnrealNGGMCP/)."));
						Toast->SetCompletionState(SNotificationItem::CS_Fail);
						Toast->SetExpireDuration(8.0f);
					}
					Toast->ExpireAndFadeout();
					return false; // unregister
				}),
			1.0f);
	}
}

void FUnrealNGGMCPModule::ShutdownModule()
{
	UE_LOG(LogNGGBridge, Log, TEXT("UnrealNGGMCP: ShutdownModule — stopping HTTP bridge"));

	if (DepsToastTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DepsToastTicker);
		DepsToastTicker.Reset();
	}

	FNGGChatTabSpawner::Unregister();

	if (HttpServer.IsValid())
	{
		HttpServer->Stop();
		HttpServer.Reset();
	}

	DeleteBridgeDiscoveryFile();
}

// ---------------------------------------------------------------------------
// Module registration
// ---------------------------------------------------------------------------

IMPLEMENT_MODULE(FUnrealNGGMCPModule, UnrealNGGMCP)
