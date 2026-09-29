// Copyright 2025-2026 NGG. All Rights Reserved.

#include "NGGMcpConfig.h"

#include "NGGPluginPaths.h"
#include "UnrealNGGMCPModule.h" // LogNGGBridge

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// ============================================================================
// First-run .mcp.json provisioning
// ============================================================================
//
// What gets written, and what deliberately does not:
//
//   command  "node"       — resolved from PATH, so any Node install works.
//   args     one relative path to this plugin's bootstrap.js. Relative to the
//                           project (not absolute) so the file stays valid for
//                           everyone who clones the project.
//   cwd      "."          — the project root, i.e. where .mcp.json itself lives.
//
//   env      NGG_BRIDGE_TIMEOUT_MS   the default, written out so the knob is
//                                    discoverable.
//            MESHY_API_KEY           empty placeholder for ue5_meshy_generate.
//            NGG_PROJECT_ROOT        only for layouts where the server cannot
//                                    work the project root out for itself.
//
// Neither NGG_BRIDGE_URL nor NGG_BRIDGE_TOKEN is written, on purpose:
//
//   * The bridge picks a free port at startup (6776..6800, so several editors
//     can run at once) and publishes it in Saved/UnrealNGGMCP/bridge.json, which
//     the MCP server reads. A pinned URL overrides that discovery and points at
//     whichever editor happens to hold 6776.
//   * The token is read from Config/DefaultEngine.ini by both ends. A copy in
//     .mcp.json goes stale the moment the token is regenerated, and every
//     request then fails with 401 — the exact case ue5client's 401 hint warns
//     about. Leaving it out also keeps a shared secret out of a file that is
//     normally committed.
// ============================================================================

namespace
{
	/**
	 * The key under "mcpServers". Stays "ue5-ngg" no matter what the plugin
	 * folder is called: the tool names the model sees (mcp__ue5-ngg__*), the
	 * README, and the bundled skills all derive from it, so it is part of the
	 * plugin's contract rather than a detail of the install.
	 */
	const TCHAR* const kServerKey = TEXT("ue5-ngg");

	/** MCP server entry point, relative to the plugin folder. */
	const TCHAR* const kBootstrapRelative = TEXT("Source/ThirdParty/unrealngg-mcp/bootstrap.js");

	/** Tail shared by every copy of that entry point, whatever the folder above it is named. */
	const TCHAR* const kBootstrapTail = TEXT("unrealngg-mcp/bootstrap.js");

	FString ProjectDirFull()
	{
		FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FPaths::NormalizeDirectoryName(Dir);
		return Dir;
	}

	FString ProjectMcpConfigPath()
	{
		return FPaths::Combine(ProjectDirFull(), TEXT(".mcp.json"));
	}

	/** JSON carries forward slashes on every platform, including Windows. */
	FString ToJsonPath(const FString& Path)
	{
		FString Out = Path;
		Out.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
		return Out;
	}

	bool LooksLikeNGGBootstrap(const FString& Arg)
	{
		return ToJsonPath(Arg).EndsWith(kBootstrapTail, ESearchCase::IgnoreCase);
	}

	/**
	 * Resolves an args entry the way the MCP client will: relative to the entry's
	 * "cwd", which is itself relative to the project root when not absolute.
	 */
	FString ResolveArgPath(const FString& Arg, const FString& CwdField)
	{
		if (Arg.IsEmpty())
		{
			return FString();
		}
		if (!FPaths::IsRelative(Arg))
		{
			return FPaths::ConvertRelativePathToFull(Arg);
		}

		FString Base = CwdField.TrimStartAndEnd();
		if (Base.IsEmpty() || Base == TEXT(".") || Base == TEXT("./"))
		{
			Base = ProjectDirFull();
		}
		else if (FPaths::IsRelative(Base))
		{
			Base = FPaths::Combine(ProjectDirFull(), Base);
		}
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(Base, Arg));
	}

	/**
	 * Whether the MCP server can find <Project>/Saved/UnrealNGGMCP/bridge.json
	 * and <Project>/Config/DefaultEngine.ini on its own.
	 *
	 * ue5client.js gets there by walking a fixed five levels up from its own
	 * directory — unrealngg-mcp, ThirdParty, Source, <plugin folder>, Plugins —
	 * which lands on the project root only for the standard
	 * <Project>/Plugins/<PluginFolder> layout. A plugin grouped one level deeper
	 * (Plugins/Tools/<PluginFolder>), or left in the engine, needs to be told
	 * where the project is; NGG_PROJECT_ROOT does that.
	 */
	bool ServerFindsProjectRootItself(const FString& PluginRelativeDir)
	{
		if (PluginRelativeDir.IsEmpty())
		{
			return false; // outside the project entirely
		}
		TArray<FString> Parts;
		PluginRelativeDir.ParseIntoArray(Parts, TEXT("/"), /*bCullEmpty=*/true);
		return Parts.Num() == 2 && Parts[0].Equals(TEXT("Plugins"), ESearchCase::IgnoreCase);
	}

	/** Everything the generated (or repaired) server entry needs. */
	struct FServerSpec
	{
		/** What goes into args[0]: project-relative where possible, absolute otherwise. */
		FString BootstrapArg;
		/** Absolute project dir, set only when the server cannot derive it itself. */
		FString ProjectRootOverride;
	};

	FServerSpec BuildServerSpec()
	{
		FServerSpec Spec;

		const FString PluginRelative = FNGGPluginPaths::GetPluginDirRelativeToProject();

		Spec.BootstrapArg = PluginRelative.IsEmpty()
			? ToJsonPath(FPaths::Combine(FNGGPluginPaths::GetPluginDir(), kBootstrapRelative))
			: ToJsonPath(FPaths::Combine(PluginRelative, kBootstrapRelative));

		if (!ServerFindsProjectRootItself(PluginRelative))
		{
			Spec.ProjectRootOverride = ToJsonPath(ProjectDirFull());
		}

		return Spec;
	}

	TSharedRef<FJsonObject> MakeServerEntry(const FServerSpec& Spec)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("command"), TEXT("node"));

		TArray<TSharedPtr<FJsonValue>> Args;
		Args.Add(MakeShared<FJsonValueString>(Spec.BootstrapArg));
		Entry->SetArrayField(TEXT("args"), Args);

		Entry->SetStringField(TEXT("cwd"), TEXT("."));

		const TSharedRef<FJsonObject> Env = MakeShared<FJsonObject>();
		Env->SetStringField(TEXT("NGG_BRIDGE_TIMEOUT_MS"), TEXT("15000"));
		if (!Spec.ProjectRootOverride.IsEmpty())
		{
			Env->SetStringField(TEXT("NGG_PROJECT_ROOT"), Spec.ProjectRootOverride);
		}
		// Placeholder rather than an omission: ue5_meshy_generate needs a key, and
		// an empty string is falsy in Node, so an unfilled placeholder behaves
		// exactly like no variable at all.
		Env->SetStringField(TEXT("MESHY_API_KEY"), TEXT(""));
		Entry->SetObjectField(TEXT("env"), Env);

		return Entry;
	}

	/** Serialised form of the entry, for log messages that ask the user to add it by hand. */
	FString DescribeEntry(const FServerSpec& Spec)
	{
		const TSharedRef<FJsonObject> Servers = MakeShared<FJsonObject>();
		Servers->SetObjectField(kServerKey, MakeServerEntry(Spec));
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("mcpServers"), Servers);

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);
		return Out;
	}

	bool WriteConfigFile(const TSharedRef<FJsonObject>& Root, FString& OutError)
	{
		const FString Path = ProjectMcpConfigPath();

		if (FPaths::FileExists(Path) && IFileManager::Get().IsReadOnly(*Path))
		{
			OutError = FString::Printf(
				TEXT("%s is read-only — check it out from source control first"), *Path);
			return false;
		}

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		if (!FJsonSerializer::Serialize(Root, Writer))
		{
			OutError = TEXT("could not serialise the MCP config");
			return false;
		}

		if (!FFileHelper::SaveStringToFile(Out, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("could not write %s"), *Path);
			return false;
		}
		return true;
	}

	/**
	 * Finds the entry that launches this plugin's MCP server: the "ue5-ngg" key
	 * if it is there, otherwise any server whose args name a bootstrap.js of
	 * ours. The second pass matters because a developer who renamed the key
	 * should get their entry repaired rather than a duplicate added next to it.
	 */
	TSharedPtr<FJsonObject> FindOurServer(const TSharedPtr<FJsonObject>& Servers, FString& OutKey)
	{
		if (Servers->HasTypedField<EJson::Object>(kServerKey))
		{
			OutKey = kServerKey;
			return Servers->GetObjectField(kServerKey);
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Servers->Values)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetObject(Entry) || !Entry)
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Args = nullptr;
			if (!(*Entry)->TryGetArrayField(TEXT("args"), Args) || !Args)
			{
				continue;
			}

			for (const TSharedPtr<FJsonValue>& Arg : *Args)
			{
				FString ArgStr;
				if (Arg.IsValid() && Arg->TryGetString(ArgStr) && LooksLikeNGGBootstrap(ArgStr))
				{
					OutKey = Pair.Key;
					return *Entry;
				}
			}
		}

		OutKey.Empty();
		return nullptr;
	}
}

// ============================================================================
// FNGGMcpConfig
// ============================================================================

FNGGMcpConfig::EResult FNGGMcpConfig::EnsureProjectConfig()
{
	// Commandlets (cook, package, resave) load this module too and must never
	// mutate the project as a side effect of a build.
	if (IsRunningCommandlet() || !FApp::HasProjectName())
	{
		return EResult::Skipped;
	}

	bool bAutoProvision = true;
	GConfig->GetBool(TEXT("UnrealNGGMCP"), TEXT("AutoProvisionMcpConfig"), bAutoProvision, GEngineIni);
	if (!bAutoProvision)
	{
		UE_LOG(LogNGGBridge, Log,
			TEXT("UnrealNGGMCP: AutoProvisionMcpConfig=false — leaving .mcp.json alone."));
		return EResult::Disabled;
	}

	// A trimmed install without the bundled MCP server has nothing to point at,
	// and an entry naming a missing file is worse than no entry at all.
	const FString BootstrapAbsolute = FPaths::Combine(FNGGPluginPaths::GetPluginDir(), kBootstrapRelative);
	if (!FPaths::FileExists(BootstrapAbsolute))
	{
		UE_LOG(LogNGGBridge, Warning,
			TEXT("UnrealNGGMCP: No MCP server at %s — skipping .mcp.json provisioning. ")
			TEXT("The editor bridge still works; Claude Code has nothing to launch."),
			*BootstrapAbsolute);
		return EResult::Skipped;
	}

	const FServerSpec Spec = BuildServerSpec();
	const FString ConfigPath = ProjectMcpConfigPath();

	if (!FNGGPluginPaths::IsInsideProject())
	{
		// README Step 1: the plugin belongs in the project. We still write a
		// working entry (absolute path + NGG_PROJECT_ROOT) so Claude Code is
		// usable, but the chat sidecar's own launcher expects the in-project
		// layout, so say what is still missing.
		UE_LOG(LogNGGBridge, Warning,
			TEXT("UnrealNGGMCP: The plugin is installed outside this project (%s), so .mcp.json gets an ")
			TEXT("absolute path to it. Move the plugin to %s/Plugins/ (README Step 1) — an engine-level ")
			TEXT("install is not a supported layout."),
			*FNGGPluginPaths::GetPluginDir(), *ProjectDirFull());
	}

	// ---- Load any existing config -----------------------------------------
	const bool bFileExists = FPaths::FileExists(ConfigPath);

	FString ExistingText;
	if (bFileExists && !FFileHelper::LoadFileToString(ExistingText, *ConfigPath))
	{
		UE_LOG(LogNGGBridge, Warning,
			TEXT("UnrealNGGMCP: Could not read %s — leaving it alone."), *ConfigPath);
		return EResult::Failed;
	}

	TSharedPtr<FJsonObject> Root;
	if (bFileExists && !ExistingText.TrimStartAndEnd().IsEmpty())
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ExistingText);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			// Never overwrite a file we cannot understand — it is the developer's,
			// and a half-typed edit is not a reason to throw their config away.
			UE_LOG(LogNGGBridge, Warning,
				TEXT("UnrealNGGMCP: %s is not valid JSON, so it was left untouched. ")
				TEXT("Fix or delete it and restart the editor, or merge this in by hand:\n%s"),
				*ConfigPath, *DescribeEntry(Spec));
			return EResult::Failed;
		}
	}
	else
	{
		Root = MakeShared<FJsonObject>();
	}

	TSharedPtr<FJsonObject> Servers;
	if (Root->HasTypedField<EJson::Object>(TEXT("mcpServers")))
	{
		Servers = Root->GetObjectField(TEXT("mcpServers"));
	}
	else
	{
		if (Root->Values.Contains(TEXT("mcpServers")))
		{
			UE_LOG(LogNGGBridge, Warning,
				TEXT("UnrealNGGMCP: \"mcpServers\" in %s is not an object — replacing it, since no MCP ")
				TEXT("client can read it as it stands."), *ConfigPath);
		}
		Servers = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("mcpServers"), Servers);
	}

	// ---- Already provisioned? ----------------------------------------------
	FString ExistingKey;
	const TSharedPtr<FJsonObject> Existing = FindOurServer(Servers, ExistingKey);

	if (Existing.IsValid())
	{
		FString Cwd;
		Existing->TryGetStringField(TEXT("cwd"), Cwd);

		TArray<TSharedPtr<FJsonValue>> Args;
		const TArray<TSharedPtr<FJsonValue>>* FoundArgs = nullptr;
		if (Existing->TryGetArrayField(TEXT("args"), FoundArgs) && FoundArgs)
		{
			Args = *FoundArgs;
		}

		// The argument to check is the one naming a bootstrap.js; failing that,
		// the first one — an entry pointing somewhere else entirely is still the
		// developer's call, so it only gets touched when it does not resolve.
		int32 ArgIndex = INDEX_NONE;
		for (int32 Index = 0; Index < Args.Num(); ++Index)
		{
			FString ArgStr;
			if (Args[Index].IsValid() && Args[Index]->TryGetString(ArgStr) && LooksLikeNGGBootstrap(ArgStr))
			{
				ArgIndex = Index;
				break;
			}
		}
		if (ArgIndex == INDEX_NONE && Args.Num() > 0)
		{
			ArgIndex = 0;
		}

		FString CurrentArg;
		if (Args.IsValidIndex(ArgIndex) && Args[ArgIndex].IsValid())
		{
			Args[ArgIndex]->TryGetString(CurrentArg);
		}

		if (!CurrentArg.IsEmpty() && FPaths::FileExists(ResolveArgPath(CurrentArg, Cwd)))
		{
			// Points at a file that is there. Even if that is a different copy of
			// the plugin, the developer put it there deliberately.
			return EResult::AlreadyCorrect;
		}

		// Stale: the usual cause is the plugin folder having been renamed or
		// moved since the entry was written.
		if (Args.IsValidIndex(ArgIndex))
		{
			Args[ArgIndex] = MakeShared<FJsonValueString>(Spec.BootstrapArg);
		}
		else
		{
			Args.Insert(MakeShared<FJsonValueString>(Spec.BootstrapArg), 0);
		}
		Existing->SetArrayField(TEXT("args"), Args);

		// An absolute cwd from another machine (or an old project location) makes
		// the relative arg unresolvable however correct it is.
		if (!Cwd.IsEmpty() && !FPaths::IsRelative(Cwd) && !FPaths::DirectoryExists(Cwd))
		{
			Existing->SetStringField(TEXT("cwd"), TEXT("."));
		}

		// Keep the project-root hint in step with where the plugin sits now.
		TSharedPtr<FJsonObject> Env;
		if (Existing->HasTypedField<EJson::Object>(TEXT("env")))
		{
			Env = Existing->GetObjectField(TEXT("env"));
		}

		if (!Spec.ProjectRootOverride.IsEmpty() || (Env.IsValid() && Env->HasField(TEXT("NGG_PROJECT_ROOT"))))
		{
			if (!Env.IsValid())
			{
				Env = MakeShared<FJsonObject>();
				Existing->SetObjectField(TEXT("env"), Env);
			}
			if (Spec.ProjectRootOverride.IsEmpty())
			{
				Env->RemoveField(TEXT("NGG_PROJECT_ROOT"));
			}
			else
			{
				Env->SetStringField(TEXT("NGG_PROJECT_ROOT"), Spec.ProjectRootOverride);
			}
		}

		FString Error;
		if (!WriteConfigFile(Root.ToSharedRef(), Error))
		{
			UE_LOG(LogNGGBridge, Warning,
				TEXT("UnrealNGGMCP: The \"%s\" entry in %s points at a file that is not there (%s), ")
				TEXT("and it could not be fixed (%s). Update args to: %s"),
				*ExistingKey, *ConfigPath, *CurrentArg, *Error, *Spec.BootstrapArg);
			return EResult::Failed;
		}

		UE_LOG(LogNGGBridge, Log,
			TEXT("UnrealNGGMCP: Repaired the \"%s\" entry in %s — it pointed at '%s', which is not there; ")
			TEXT("now '%s'. Restart Claude Code (or run /mcp) to pick this up."),
			*ExistingKey, *ConfigPath, *CurrentArg, *Spec.BootstrapArg);
		return EResult::Repaired;
	}

	// ---- Nothing of ours in the file: add it -------------------------------
	Servers->SetObjectField(kServerKey, MakeServerEntry(Spec));

	FString Error;
	if (!WriteConfigFile(Root.ToSharedRef(), Error))
	{
		UE_LOG(LogNGGBridge, Warning,
			TEXT("UnrealNGGMCP: Could not provision %s (%s). Claude Code has no way to launch this ")
			TEXT("project's MCP server until the file exists. Create it by hand with:\n%s"),
			*ConfigPath, *Error, *DescribeEntry(Spec));
		return EResult::Failed;
	}

	if (bFileExists)
	{
		UE_LOG(LogNGGBridge, Log,
			TEXT("UnrealNGGMCP: Added the \"%s\" MCP server to %s (args: %s); the servers already there ")
			TEXT("were kept. Restart Claude Code (or run /mcp) to pick it up."),
			kServerKey, *ConfigPath, *Spec.BootstrapArg);
		return EResult::Added;
	}

	UE_LOG(LogNGGBridge, Log,
		TEXT("UnrealNGGMCP: This project had no .mcp.json — wrote %s with the \"%s\" server ")
		TEXT("(args: %s), so Claude Code can drive this editor with no further setup. ")
		TEXT("Restart Claude Code (or run /mcp) to pick it up."),
		*ConfigPath, kServerKey, *Spec.BootstrapArg);
	return EResult::Created;
}
