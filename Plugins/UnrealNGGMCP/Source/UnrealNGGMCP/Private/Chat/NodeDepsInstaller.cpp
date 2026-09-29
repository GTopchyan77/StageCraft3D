// Copyright 2025-2026 NGG. All Rights Reserved.

#include "NodeDepsInstaller.h"

#include "SidecarLauncher.h" // LogNGGChat
#include "NGGPluginPaths.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"

std::atomic<ENGGDepsState> FNodeDepsInstaller::State{ ENGGDepsState::Idle };
FCriticalSection FNodeDepsInstaller::DetailCS;
FString FNodeDepsInstaller::StatusDetail;

ENGGDepsState FNodeDepsInstaller::GetState()
{
	return State.load();
}

FString FNodeDepsInstaller::GetStatusDetail()
{
	FScopeLock Lock(&DetailCS);
	return StatusDetail;
}

void FNodeDepsInstaller::SetDetail(const FString& Detail)
{
	FScopeLock Lock(&DetailCS);
	StatusDetail = Detail;
}

TArray<FNodeDepsInstaller::FPackage> FNodeDepsInstaller::GetPackages()
{
	static const TCHAR* PackageNames[] = { TEXT("ngg-sidecar"), TEXT("unrealngg-mcp") };

	TArray<FPackage> Out;
	for (const TCHAR* Name : PackageNames)
	{
		// Located via the plugin manager rather than a fixed "Plugins/UnrealNGGMCP",
		// so the packages are still found when the plugin folder was renamed.
		const FString Dir = FPaths::ConvertRelativePathToFull(
			FNGGPluginPaths::GetThirdPartyPackageDir(Name));
		// Skip packages missing from a trimmed install rather than failing on them.
		if (FPaths::FileExists(FPaths::Combine(Dir, TEXT("package.json"))))
		{
			Out.Add({ Name, Dir });
		}
	}
	return Out;
}

FString FNodeDepsInstaller::ComputeLockHash(const FString& Dir)
{
	const FString Lock = FPaths::Combine(Dir, TEXT("package-lock.json"));
	const FString Src = FPaths::FileExists(Lock) ? Lock : FPaths::Combine(Dir, TEXT("package.json"));
	const FMD5Hash Hash = FMD5Hash::HashFile(*Src);
	return Hash.IsValid() ? LexToString(Hash) : FString();
}

FString FNodeDepsInstaller::GetStampPath(const FString& Dir)
{
	return FPaths::Combine(Dir, TEXT("node_modules"), TEXT(".ngg-deps-stamp"));
}

FString FNodeDepsInstaller::GetInstallLogPath(const FString& PkgName)
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealNGGMCP"),
		FString::Printf(TEXT("npm-install-%s.log"), *PkgName));
}

bool FNodeDepsInstaller::PackageNeedsInstall(const FPackage& Pkg)
{
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (!PF.DirectoryExists(*FPaths::Combine(Pkg.Dir, TEXT("node_modules"))))
	{
		return true;
	}
	// A node_modules from a manual `npm install` has no stamp — reinstall once
	// (a no-op for npm) to stamp it, then future checks are hash-compare only.
	FString Stamp;
	if (!FFileHelper::LoadFileToString(Stamp, *GetStampPath(Pkg.Dir)))
	{
		return true;
	}
	return Stamp.TrimStartAndEnd() != ComputeLockHash(Pkg.Dir);
}

bool FNodeDepsInstaller::AreDepsReady()
{
	if (State.load() == ENGGDepsState::Installing)
	{
		return false;
	}
	for (const FPackage& Pkg : GetPackages())
	{
		if (PackageNeedsInstall(Pkg))
		{
			return false;
		}
	}
	return true;
}

FString FNodeDepsInstaller::ResolveNpmCommand()
{
#if PLATFORM_WINDOWS
	const TCHAR* NpmName = TEXT("npm.cmd");
#else
	const TCHAR* NpmName = TEXT("npm");
#endif
	// NGG_NODE_PATH points at the node binary; npm sits next to it in every
	// standard layout (nodejs.org, nvm, brew). Fall back to a PATH scan.
	const FString NodeOverride = FPlatformMisc::GetEnvironmentVariable(TEXT("NGG_NODE_PATH"));
	if (!NodeOverride.IsEmpty())
	{
		const FString Sibling = FPaths::Combine(FPaths::GetPath(NodeOverride), NpmName);
		if (FPaths::FileExists(Sibling))
		{
			return Sibling;
		}
	}

	// Resolve npm to an ABSOLUTE path instead of letting cmd.exe find the bare
	// name. When npm.cmd is invoked by bare name with our working directory set
	// to the package dir, its internal %~dp0 expands against that cwd, so the
	// wrapper looks for node_modules/npm/bin/npm-cli.js inside the package and
	// dies with MODULE_NOT_FOUND. An absolute invocation keeps %~dp0 correct.
	const FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
	TArray<FString> PathDirs;
#if PLATFORM_WINDOWS
	PathEnv.ParseIntoArray(PathDirs, TEXT(";"), /*CullEmpty=*/true);
#else
	PathEnv.ParseIntoArray(PathDirs, TEXT(":"), /*CullEmpty=*/true);
#endif
	for (const FString& PathDir : PathDirs)
	{
		const FString Candidate = FPaths::Combine(PathDir, NpmName);
		if (FPaths::FileExists(Candidate))
		{
			return Candidate;
		}
	}

	return NpmName;
}

bool FNodeDepsInstaller::RunNpmInstall(const FPackage& Pkg, FString& OutError)
{
	const FString NpmCmd  = ResolveNpmCommand();
	const FString NpmLogPath = GetInstallLogPath(Pkg.Name);
	FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(NpmLogPath));

	UE_LOG(LogNGGChat, Log, TEXT("npm install for %s: npm=%s dir=%s log=%s"),
		*Pkg.Name, *NpmCmd, *Pkg.Dir, *NpmLogPath);

	FString Cmd;
	FString Args;
#if PLATFORM_WINDOWS
	// The doubled outer quotes make `cmd /c` strip exactly one pair, leaving
	// a quoted npm path (which may contain spaces) plus the redirection intact.
	Cmd  = TEXT("cmd.exe");
	Args = FString::Printf(
		TEXT("/c \"\"%s\" install --no-audit --no-fund --loglevel=error >\"%s\" 2>&1\""),
		*NpmCmd, *NpmLogPath);
#elif PLATFORM_MAC || PLATFORM_LINUX
	Cmd  = TEXT("/bin/sh");
	Args = FString::Printf(
		TEXT("-c \"cd '%s' && '%s' install --no-audit --no-fund --loglevel=error >'%s' 2>&1\""),
		*Pkg.Dir, *NpmCmd, *NpmLogPath);
#else
	OutError = TEXT("Unsupported platform for npm install");
	return false;
#endif

	uint32 Pid = 0;
	FProcHandle Handle = FPlatformProcess::CreateProc(
		*Cmd, *Args,
		/*bLaunchDetached=*/ false,
		/*bLaunchHidden=*/   true,
		/*bLaunchReallyHidden=*/ true,
		&Pid, 0, *Pkg.Dir, nullptr, nullptr);

	if (!Handle.IsValid())
	{
		OutError = FString::Printf(
			TEXT("Could not launch npm for %s. Install Node.js 18+ from https://nodejs.org ")
			TEXT("(or set NGG_NODE_PATH to your node binary), then click Reconnect."),
			*Pkg.Name);
		return false;
	}

	// Bounded wait — npm can hang indefinitely on a broken network. 10 minutes
	// covers a cold cache on a slow connection with plenty of margin.
	const double Deadline = FPlatformTime::Seconds() + 600.0;
	while (FPlatformProcess::IsProcRunning(Handle))
	{
		if (FPlatformTime::Seconds() > Deadline)
		{
			FPlatformProcess::TerminateProc(Handle, /*KillTree=*/true);
			FPlatformProcess::CloseProc(Handle);
			OutError = FString::Printf(
				TEXT("npm install for %s timed out after 10 minutes (network problem?). ")
				TEXT("See %s, then click Reconnect to retry."),
				*Pkg.Name, *NpmLogPath);
			return false;
		}
		FPlatformProcess::Sleep(0.25f);
	}

	int32 ReturnCode = -1;
	FPlatformProcess::GetProcReturnCode(Handle, &ReturnCode);
	FPlatformProcess::CloseProc(Handle);

	if (ReturnCode != 0)
	{
		FString LogContent;
		FFileHelper::LoadFileToString(LogContent, *NpmLogPath);
		LogContent.TrimEndInline();
		const FString Tail = LogContent.Right(600);
		OutError = FString::Printf(
			TEXT("npm install failed for %s (exit code %d). Log: %s%s%s\n")
			TEXT("If npm was not found, install Node.js 18+ from https://nodejs.org ")
			TEXT("(or set NGG_NODE_PATH), then click Reconnect."),
			*Pkg.Name, ReturnCode, *NpmLogPath,
			Tail.IsEmpty() ? TEXT("") : TEXT("\n"), *Tail);
		return false;
	}

	if (!FPlatformFileManager::Get().GetPlatformFile().DirectoryExists(
			*FPaths::Combine(Pkg.Dir, TEXT("node_modules"))))
	{
		OutError = FString::Printf(
			TEXT("npm install for %s reported success but node_modules was not created. See %s."),
			*Pkg.Name, *NpmLogPath);
		return false;
	}

	// Stamp with the current lock hash so future sessions skip the install
	// until the plugin ships a changed package-lock.json.
	FFileHelper::SaveStringToFile(ComputeLockHash(Pkg.Dir), *GetStampPath(Pkg.Dir),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	UE_LOG(LogNGGChat, Log, TEXT("npm install for %s succeeded"), *Pkg.Name);
	return true;
}

void FNodeDepsInstaller::StartInstallIfNeeded()
{
	ENGGDepsState Cur = State.load();
	if (Cur == ENGGDepsState::Installing || Cur == ENGGDepsState::Ready)
	{
		return;
	}
	if (AreDepsReady())
	{
		State.store(ENGGDepsState::Ready);
		return;
	}
	// Claim the install (also the retry path out of Failed). Both known
	// callers are game-thread, so a lost CAS just means someone else claimed.
	if (!State.compare_exchange_strong(Cur, ENGGDepsState::Installing))
	{
		return;
	}
	SetDetail(TEXT("Preparing to install Node.js packages..."));

	// Dedicated thread, not the pool — this blocks for however long npm takes.
	Async(EAsyncExecution::Thread, []()
	{
		TArray<FPackage> Todo;
		for (const FPackage& Pkg : GetPackages())
		{
			if (PackageNeedsInstall(Pkg))
			{
				Todo.Add(Pkg);
			}
		}

		for (int32 Index = 0; Index < Todo.Num(); ++Index)
		{
			SetDetail(FString::Printf(TEXT("Installing Node.js packages: %s (%d/%d)..."),
				*Todo[Index].Name, Index + 1, Todo.Num()));

			FString Error;
			if (!RunNpmInstall(Todo[Index], Error))
			{
				UE_LOG(LogNGGChat, Error, TEXT("%s"), *Error);
				SetDetail(Error);
				State.store(ENGGDepsState::Failed);
				return;
			}
		}

		SetDetail(FString());
		State.store(ENGGDepsState::Ready);
		UE_LOG(LogNGGChat, Log, TEXT("All bundled Node.js packages are installed"));
	});
}
