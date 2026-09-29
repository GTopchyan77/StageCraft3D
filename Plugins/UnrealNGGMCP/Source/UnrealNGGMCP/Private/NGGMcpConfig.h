// Copyright 2025-2026 NGG. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * FNGGMcpConfig
 *
 * First-run provisioning of <Project>/.mcp.json — the file Claude Code (and the
 * in-editor chat, which merges the same file) reads to learn how to spawn this
 * plugin's MCP server.
 *
 * Dropping the plugin into a project used to leave the developer to hand-write
 * that file, with a path they had to get exactly right; the plugin knows its own
 * location, so it writes the entry itself at editor startup.
 *
 * The generated path is derived from the plugin's real folder name rather than a
 * hard-coded "Plugins/UnrealNGGMCP", because a Fab install lands in the engine
 * and the folder the developer moves it to is theirs to name.
 *
 * Existing files are respected: a working entry is left untouched, other MCP
 * servers are preserved, and a .mcp.json that is not valid JSON is never
 * overwritten.
 *
 * Opt out with, in DefaultEngine.ini:
 *   [UnrealNGGMCP]
 *   AutoProvisionMcpConfig=false
 */
class FNGGMcpConfig
{
public:
	/** Outcome of one provisioning pass. Returned for logging and tests. */
	enum class EResult : uint8
	{
		/** AutoProvisionMcpConfig=false. */
		Disabled,
		/** Nothing to provision: commandlet, no project, or no bundled MCP server on disk. */
		Skipped,
		/** A .mcp.json entry already pointed at a bootstrap.js that exists. */
		AlreadyCorrect,
		/** No .mcp.json existed — wrote one. */
		Created,
		/** A .mcp.json existed without our server — added it alongside the others. */
		Added,
		/** An entry existed but pointed at a path that is not there any more — fixed it. */
		Repaired,
		/** Could not read, parse or write the file; nothing was changed. */
		Failed
	};

	/**
	 * Creates or repairs <Project>/.mcp.json so it can launch the copy of the
	 * MCP server bundled with this plugin. Safe to call on every editor start:
	 * when the entry is already right, nothing is written.
	 */
	static EResult EnsureProjectConfig();
};
