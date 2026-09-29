// Copyright 2025-2026 NGG. All Rights Reserved.

#include "SidecarLauncher.h"

#include "NGGPluginPaths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/FileHelper.h"
#include "Misc/App.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogNGGChat);

int32 FSidecarLauncher::GetSidecarPort()
{
	// Derive a project-unique port from the first 4 hex chars of the MD5 of the
	// .uproject path — the same hash used as the WebSocket session ID. This maps
	// every project stably into [20000, 29999] so multiple projects can each run
	// their own sidecar simultaneously without port conflicts.
	const FString Hash = FMD5::HashAnsiString(*FPaths::GetProjectFilePath());
	const uint32 HexVal = static_cast<uint32>(FCString::Strtoi(*Hash.Left(4), nullptr, 16));
	return 20000 + static_cast<int32>(HexVal % 10000);
}

int32 FSidecarLauncher::GetPermissionPort()
{
	return GetSidecarPort() + 1;
}

FString FSidecarLauncher::GetWebSocketURL()
{
	return FString::Printf(TEXT("ws://localhost:%d"), GetSidecarPort());
}

FString FSidecarLauncher::GetHealthURL()
{
	return FString::Printf(TEXT("http://localhost:%d/health"), GetSidecarPort());
}

FString FSidecarLauncher::GetShutdownURL()
{
	return FString::Printf(TEXT("http://localhost:%d/shutdown"), GetSidecarPort());
}

const TCHAR* FSidecarLauncher::GetExpectedSidecarVersion()
{
	// Must match VERSION in Plugins/UnrealNGGMCP/Source/ThirdParty/ngg-sidecar/index.js and
	// "version" in Plugins/UnrealNGGMCP/Source/ThirdParty/ngg-sidecar/package.json —
	// see comment in this header.
	return TEXT("1.5.0");
}

FString FSidecarLauncher::GetSidecarScriptPath()
{
	// Resolved from the plugin's own location: the folder is routinely renamed
	// on the way out of an engine/Fab install into a project.
	return FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FNGGPluginPaths::GetThirdPartyPackageDir(TEXT("ngg-sidecar")), TEXT("index.js")));
}

FString FSidecarLauncher::GetNodeExecutable()
{
	FString EnvOverride = FPlatformMisc::GetEnvironmentVariable(TEXT("NGG_NODE_PATH"));
	if (!EnvOverride.IsEmpty())
	{
		return EnvOverride;
	}
#if PLATFORM_WINDOWS
	return TEXT("node.exe");
#else
	return TEXT("node");
#endif
}

FString FSidecarLauncher::GetSidecarLogPath()
{
	// Per-project filename. The port is already project-unique (derived from the
	// .uproject path hash), so embedding it here gives each project's sidecar
	// its own log file. Without this, two projects' sidecars both target
	// `sidecar.log` — on Windows the shell `>"sidecar.log"` redirection
	// collides with the other sidecar's open file handle, causing the second
	// spawn to fail silently and surface as "Sidecar still not alive after 4s".
	const FString FileName = FString::Printf(TEXT("sidecar-%d.log"), GetSidecarPort());
#if PLATFORM_WINDOWS
	FString LocalAppData = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
	if (LocalAppData.IsEmpty())
	{
		LocalAppData = FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT(".."));
	}
	return FPaths::Combine(LocalAppData, TEXT("NGG"), FileName);
#elif PLATFORM_MAC
	const FString Home = FPlatformProcess::UserHomeDir();
	return FPaths::Combine(Home, TEXT("Library/Logs/NGG"), FileName);
#else
	const FString Home = FPlatformProcess::UserHomeDir();
	return FPaths::Combine(Home, TEXT(".local/state/NGG"), FileName);
#endif
}

bool FSidecarLauncher::IsSidecarAlive(float TimeoutSeconds)
{
	// TCP connect probe — deliberately avoids the HTTP module.
	//
	// The previous implementation spun calling GetHttpManager().Tick() while
	// already inside FTSTicker::Tick(). The re-entrant tick freed the HTTP
	// request's shared-pointer ref-controller before Req went out of scope,
	// causing EXCEPTION_ACCESS_VIOLATION in ReleaseSharedReferenceNoInline.
	//
	// A raw TCP connect to the sidecar's port is sufficient: if the port is
	// open the sidecar is alive. Loopback connects complete in <1 ms so the
	// Wait call returns almost immediately in both the alive and refused cases.
	ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SS) return false;

	bool bAddrValid = false;
	TSharedRef<FInternetAddr> Addr = SS->CreateInternetAddr();
	Addr->SetIp(TEXT("127.0.0.1"), bAddrValid);
	Addr->SetPort(GetSidecarPort());
	if (!bAddrValid) return false;

	FSocket* Sock = SS->CreateSocket(NAME_Stream, TEXT("SidecarProbe"), false);
	if (!Sock) return false;

	Sock->SetNonBlocking(true);
	Sock->Connect(*Addr); // WSAEWOULDBLOCK / EINPROGRESS expected on non-blocking socket

	// WaitForWrite becomes true when the connection is established (or on error on
	// some platforms). We check GetConnectionState() to distinguish the two.
	const bool bWritable = Sock->Wait(ESocketWaitConditions::WaitForWrite,
		FTimespan::FromSeconds(TimeoutSeconds));
	const bool bAlive = bWritable && (Sock->GetConnectionState() == SCS_Connected);

	SS->DestroySocket(Sock);
	UE_LOG(LogNGGChat, Verbose, TEXT("IsSidecarAlive -> %s"), bAlive ? TEXT("true") : TEXT("false"));
	return bAlive;
}

FString FSidecarLauncher::ProbeSidecarVersion(float TimeoutSeconds)
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetVerb(TEXT("GET"));
	Req->SetURL(GetHealthURL());
	Req->SetTimeout(TimeoutSeconds);

	bool bDone = false;
	FString Body;
	int32 ResponseCode = 0;
	Req->OnProcessRequestComplete().BindLambda(
		[&bDone, &Body, &ResponseCode](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
		{
			if (bSucceeded && Response.IsValid())
			{
				ResponseCode = Response->GetResponseCode();
				Body = Response->GetContentAsString();
			}
			bDone = true;
		});
	Req->ProcessRequest();

	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	while (!bDone && FPlatformTime::Seconds() < Deadline)
	{
		FHttpModule::Get().GetHttpManager().Tick(0.01f);
		FPlatformProcess::Sleep(0.01f);
	}
	if (!bDone) Req->CancelRequest();

	if (ResponseCode != 200 || Body.IsEmpty())
	{
		return FString();
	}

	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		UE_LOG(LogNGGChat, Warning, TEXT("ProbeSidecarVersion: malformed /health body: %s"), *Body);
		return FString();
	}
	FString Version;
	Json->TryGetStringField(TEXT("version"), Version);
	UE_LOG(LogNGGChat, Verbose, TEXT("ProbeSidecarVersion -> '%s'"), *Version);
	return Version;
}

uint32 FSidecarLauncher::ProbeSidecarPid(float TimeoutSeconds)
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetVerb(TEXT("GET"));
	Req->SetURL(GetHealthURL());
	Req->SetTimeout(TimeoutSeconds);

	// Heap-allocated shared state captured by value so a late completion (after
	// this function returns on timeout) writes owned memory, never freed stack.
	TSharedRef<bool, ESPMode::ThreadSafe> bDone = MakeShared<bool, ESPMode::ThreadSafe>(false);
	TSharedRef<FString, ESPMode::ThreadSafe> Body = MakeShared<FString, ESPMode::ThreadSafe>();
	TSharedRef<int32, ESPMode::ThreadSafe> ResponseCode = MakeShared<int32, ESPMode::ThreadSafe>(0);
	Req->OnProcessRequestComplete().BindLambda(
		[bDone, Body, ResponseCode](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
		{
			if (bSucceeded && Response.IsValid())
			{
				*ResponseCode = Response->GetResponseCode();
				*Body = Response->GetContentAsString();
			}
			*bDone = true;
		});
	Req->ProcessRequest();

	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	while (!*bDone && FPlatformTime::Seconds() < Deadline)
	{
		FHttpModule::Get().GetHttpManager().Tick(0.01f);
		FPlatformProcess::Sleep(0.01f);
	}
	if (!*bDone) Req->CancelRequest();

	if (*ResponseCode != 200 || Body->IsEmpty())
	{
		return 0;
	}

	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*Body);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		return 0;
	}
	double PidNum = 0.0;
	if (!Json->TryGetNumberField(TEXT("pid"), PidNum) || PidNum <= 0.0)
	{
		return 0;
	}
	UE_LOG(LogNGGChat, Verbose, TEXT("ProbeSidecarPid -> %u"), static_cast<uint32>(PidNum));
	return static_cast<uint32>(PidNum);
}

bool FSidecarLauncher::RequestSidecarShutdown(float ShutdownTimeoutSeconds)
{
	if (!IsSidecarAlive(0.3f))
	{
		return true; // already not running
	}

	// Capture the sidecar's OS pid up front (while it's still answering /health)
	// so we can forcibly reap it below if the graceful /shutdown never lands.
	const uint32 SidecarPid = ProbeSidecarPid(0.3f);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetVerb(TEXT("POST"));
	Req->SetURL(GetShutdownURL());
	Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Req->SetContentAsString(TEXT("{}"));
	Req->SetTimeout(ShutdownTimeoutSeconds);

	// Heap-allocated "done" flag captured by value (not &bDone on the stack):
	// on the send-timeout path below we deliberately do NOT cancel the request,
	// so its completion lambda can fire after this function has returned. A
	// by-reference stack capture would then write freed stack memory; a shared
	// bool is harmless when it lands late.
	TSharedRef<bool, ESPMode::ThreadSafe> bDone = MakeShared<bool, ESPMode::ThreadSafe>(false);
	Req->OnProcessRequestComplete().BindLambda(
		[bDone](FHttpRequestPtr, FHttpResponsePtr, bool) { *bDone = true; });
	Req->ProcessRequest();

	const double SendDeadline = FPlatformTime::Seconds() + 1.0;
	while (!*bDone && FPlatformTime::Seconds() < SendDeadline)
	{
		FHttpModule::Get().GetHttpManager().Tick(0.01f);
		FPlatformProcess::Sleep(0.01f);
	}
	// Don't CancelRequest here — even if the response was lost, the sidecar
	// may have already received the request and started shutting down. The
	// completion lambda only touches the shared bDone, so a late fire is safe.

	// Now poll until /health stops responding.
	const double DieDeadline = FPlatformTime::Seconds() + ShutdownTimeoutSeconds;
	while (FPlatformTime::Seconds() < DieDeadline)
	{
		FPlatformProcess::Sleep(0.1f);
		if (!IsSidecarAlive(0.2f))
		{
			UE_LOG(LogNGGChat, Log, TEXT("Sidecar shutdown confirmed"));
			return true;
		}
	}

	UE_LOG(LogNGGChat, Warning, TEXT("Sidecar still alive %.1fs after /shutdown"), ShutdownTimeoutSeconds);

	// Graceful shutdown failed. If we learned the pid from /health, try to reap
	// the orphan directly so the user isn't told to kill it by hand. We only
	// target the exact pid the sidecar reported for THIS project's port, so we
	// never risk terminating an unrelated node process.
	if (SidecarPid != 0)
	{
		UE_LOG(LogNGGChat, Warning,
			TEXT("Attempting to forcibly terminate orphaned sidecar pid=%u"), SidecarPid);
		FProcHandle Handle = FPlatformProcess::OpenProcess(SidecarPid);
		if (Handle.IsValid())
		{
			FPlatformProcess::TerminateProc(Handle, /*KillTree=*/true);
			FPlatformProcess::CloseProc(Handle);

			// Re-poll briefly to confirm the port actually freed up.
			const double KillDeadline = FPlatformTime::Seconds() + 2.0;
			while (FPlatformTime::Seconds() < KillDeadline)
			{
				FPlatformProcess::Sleep(0.1f);
				if (!IsSidecarAlive(0.2f))
				{
					UE_LOG(LogNGGChat, Log, TEXT("Orphaned sidecar pid=%u terminated"), SidecarPid);
					return true;
				}
			}
			UE_LOG(LogNGGChat, Warning,
				TEXT("Terminated pid=%u but sidecar port still responding"), SidecarPid);
		}
		else
		{
			UE_LOG(LogNGGChat, Warning,
				TEXT("Could not OpenProcess pid=%u to reap orphaned sidecar"), SidecarPid);
		}
	}
	// No reliable pid (or reap didn't take): the caller will surface the
	// manual-kill message. We intentionally do NOT FindProcessByName("node")
	// here — multiple unrelated node processes (other projects' sidecars, dev
	// tooling) commonly share that name, so killing by name could take down the
	// wrong process. Pid-targeted termination above is the only safe automatic
	// path; without a pid we leave it to the user.
	return false;
}

ESidecarEnsureResult FSidecarLauncher::EnsureCompatibleSidecar()
{
	const FString Expected = GetExpectedSidecarVersion();

	// TCP pre-check before attempting the HTTP version probe.  When the sidecar
	// is not running, ProbeSidecarVersion would spin calling GetHttpManager().Tick()
	// — which is re-entrant if we're already inside FTSTicker::Tick() (the restart
	// flow calls BeginConnect from a ticker callback).  The TCP probe is safe from
	// any call context and avoids that path entirely in the common "not running" case.
	if (!IsSidecarAlive(0.3f))
	{
		UE_LOG(LogNGGChat, Log, TEXT("EnsureCompatibleSidecar: no sidecar running, spawning v%s"), *Expected);
		return LaunchSidecarDetached()
			? ESidecarEnsureResult::Spawned
			: ESidecarEnsureResult::SpawnFailed;
	}

	const FString Current = ProbeSidecarVersion();

	if (Current.IsEmpty())
	{
		// TCP said alive but HTTP probe returned nothing — treat as not running.
		UE_LOG(LogNGGChat, Log, TEXT("EnsureCompatibleSidecar: TCP alive but no version; spawning v%s"), *Expected);
		return LaunchSidecarDetached()
			? ESidecarEnsureResult::Spawned
			: ESidecarEnsureResult::SpawnFailed;
	}

	if (Current == Expected)
	{
		UE_LOG(LogNGGChat, Log, TEXT("EnsureCompatibleSidecar: running sidecar v%s matches plugin"), *Current);
		return ESidecarEnsureResult::AliveCompatible;
	}

	UE_LOG(LogNGGChat, Log,
		TEXT("EnsureCompatibleSidecar: version mismatch (running=%s plugin=%s) — restarting"),
		*Current, *Expected);

	if (!RequestSidecarShutdown())
	{
		UE_LOG(LogNGGChat, Error,
			TEXT("EnsureCompatibleSidecar: stale sidecar v%s did not shut down"), *Current);
		return ESidecarEnsureResult::ShutdownFailed;
	}

	return LaunchSidecarDetached()
		? ESidecarEnsureResult::RestartedAfterMismatch
		: ESidecarEnsureResult::SpawnFailed;
}

static void EnsureParentDirectoryExists(const FString& FilePath)
{
	const FString Dir = FPaths::GetPath(FilePath);
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (!PF.DirectoryExists(*Dir))
	{
		PF.CreateDirectoryTree(*Dir);
	}
}

bool FSidecarLauncher::LaunchSidecarDetached()
{
	const FString ScriptPath = GetSidecarScriptPath();
	if (!FPaths::FileExists(ScriptPath))
	{
		UE_LOG(LogNGGChat, Error, TEXT("Sidecar script not found at %s"), *ScriptPath);
		return false;
	}

	const FString NodePath = GetNodeExecutable();
	const FString SidecarLog  = GetSidecarLogPath();
	EnsureParentDirectoryExists(SidecarLog);

	const int32 Port     = GetSidecarPort();
	const int32 PermPort = GetPermissionPort();
	UE_LOG(LogNGGChat, Log, TEXT("Spawning sidecar: node=%s script=%s log=%s port=%d permPort=%d"),
		*NodePath, *ScriptPath, *SidecarLog, Port, PermPort);

	FString Cmd;
	FString Args;

#if PLATFORM_WINDOWS
	// SET the project-specific ports into the child environment before calling
	// `start /B` so this project's sidecar listens on its own port pair and
	// never collides with a sidecar running for a different project.
	Cmd  = TEXT("cmd.exe");
	Args = FString::Printf(
		TEXT("/c SET NGG_SIDECAR_PORT=%d&&SET NGG_PERMISSION_PORT=%d&&start \"\" /B \"%s\" \"%s\" >\"%s\" 2>&1"),
		Port, PermPort, *NodePath, *ScriptPath, *SidecarLog);
#elif PLATFORM_MAC || PLATFORM_LINUX
	Cmd = TEXT("/bin/sh");
	const FString NodeInvoke = (NodePath == TEXT("node"))
		? FString(TEXT("/usr/bin/env node"))
		: FString::Printf(TEXT("'%s'"), *NodePath);
	Args = FString::Printf(
		TEXT("-c \"NGG_SIDECAR_PORT=%d NGG_PERMISSION_PORT=%d setsid nohup %s '%s' >'%s' 2>&1 </dev/null &\""),
		Port, PermPort, *NodeInvoke, *ScriptPath, *SidecarLog);
#else
	UE_LOG(LogNGGChat, Error, TEXT("Unsupported platform for sidecar spawn"));
	return false;
#endif

	uint32 Pid = 0;
	FProcHandle Handle = FPlatformProcess::CreateProc(
		*Cmd, *Args,
		/*bLaunchDetached=*/ true,
		/*bLaunchHidden=*/   true,
		/*bLaunchReallyHidden=*/ true,
		&Pid, 0, nullptr, nullptr, nullptr);

	if (!Handle.IsValid())
	{
		UE_LOG(LogNGGChat, Error, TEXT("Failed to spawn sidecar process"));
		return false;
	}

	// Critical: releasing the handle is what allows the child to outlive us.
	FPlatformProcess::CloseProc(Handle);

	UE_LOG(LogNGGChat, Log, TEXT("Sidecar spawn issued (pid=%u). Poll /health to confirm readiness."), Pid);
	return true;
}
