// Copyright 2025-2026 NGG. All Rights Reserved.

#include "NGGPluginPaths.h"

#include "UnrealNGGMCPModule.h" // LogNGGBridge
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

namespace
{
	/**
	 * The module name from UnrealNGGMCP.Build.cs. This is the one identifier a
	 * folder rename cannot change: it is compiled into the binary and listed in
	 * the .uplugin's Modules array, so asking the plugin manager which plugin
	 * owns it works even when both the folder and the .uplugin were renamed.
	 */
	const TCHAR* const kModuleName = TEXT("UnrealNGGMCP");

	FString ResolvePluginDir()
	{
		IPluginManager& PluginManager = IPluginManager::Get();

		TSharedPtr<IPlugin> Plugin = PluginManager.GetModuleOwnerPlugin(kModuleName);
		if (!Plugin.IsValid())
		{
			// Descriptor name (i.e. the .uplugin filename) — correct whenever the
			// plugin still ships under its own name, whatever the folder is called.
			Plugin = PluginManager.FindPlugin(kModuleName);
		}

		if (Plugin.IsValid())
		{
			FString BaseDir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
			FPaths::NormalizeDirectoryName(BaseDir); // strips any trailing slash
			return BaseDir;
		}

		FString Fallback = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectPluginsDir(), kModuleName));
		FPaths::NormalizeDirectoryName(Fallback);

		UE_LOG(LogNGGBridge, Warning,
			TEXT("UnrealNGGMCP: The plugin manager does not know which plugin owns module '%s' — ")
			TEXT("assuming %s. If the plugin folder was renamed, the chat sidecar and the generated ")
			TEXT(".mcp.json may point at a path that does not exist."),
			kModuleName, *Fallback);

		return Fallback;
	}
}

const FString& FNGGPluginPaths::GetPluginDir()
{
	// Resolved once: a plugin cannot relocate while the editor is running.
	static const FString PluginDir = ResolvePluginDir();
	return PluginDir;
}

FString FNGGPluginPaths::GetThirdPartyPackageDir(const TCHAR* PackageName)
{
	return FPaths::Combine(GetPluginDir(), TEXT("Source"), TEXT("ThirdParty"), PackageName);
}

FString FNGGPluginPaths::GetPluginDirRelativeToProject()
{
	FString Relative = GetPluginDir();

	// MakePathRelativeTo treats its second argument as a file path and drops the
	// last component — ProjectDir()'s trailing slash makes that component empty,
	// so the base stays the project directory itself.
	if (!FPaths::MakePathRelativeTo(Relative, *FPaths::ProjectDir()))
	{
		return FString(); // different drive: no relative path exists
	}

	FPaths::NormalizeDirectoryName(Relative);

	// A path that has to climb out of the project is an engine-level install.
	if (Relative.IsEmpty() || Relative.StartsWith(TEXT("..")))
	{
		return FString();
	}

	return Relative;
}

bool FNGGPluginPaths::IsInsideProject()
{
	return !GetPluginDirRelativeToProject().IsEmpty();
}
