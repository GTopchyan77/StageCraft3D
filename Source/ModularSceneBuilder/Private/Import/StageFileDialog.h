// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The operating system's "open file" dialog, through the engine's IDesktopPlatform.
 *
 * IDesktopPlatform is a Developer module, so it is only linked where developer tools are built (editor, Development and
 * DebugGame game builds); see ModularSceneBuilder.Build.cs. Elsewhere IsAvailable is false and nothing opens.
 * Game thread only. The dialog is modal: the call returns when the user closes it.
 */
namespace StageFileDialog
{
	bool IsAvailable();

	/**
	 * Lets the user pick one or more files. Filter uses the "Label|*.a;*.b|Label|*.c" form. Returns false when the dialog is
	 * unavailable or the user cancelled; on true OutFiles holds absolute paths.
	 */
	bool OpenFiles(const FText& Title, const FString& DefaultDirectory, const FString& Filter, TArray<FString>& OutFiles);
}
