// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StageBridgeTypes.generated.h"

/** Outcome of a scene export or a send to a 3D app (Docs/ADR/0022-dcc-bridge.md). */
UENUM(BlueprintType)
enum class EStageBridgeResult : uint8
{
	Success,
	/** Bad scene name, file path or app. */
	InvalidRequest,
	/** There is no saved scene with that name. */
	NotFound,
	/** The scene file cannot be read (damaged, or written by an incompatible version). */
	Unreadable,
	/** The scene was acquired from the marketplace; only the user's own stages leave the app. */
	NotOriginal,
	/** None of the scene's items can be exported (none in this Library, or none owned). */
	NothingToExport,
	/** The glTF exporter failed; the log has its messages. */
	ExportFailed,
	/** The file could not be written. */
	WriteFailed,
	/** Another export or send is running. */
	Busy,
	/** The bridge is switched off, or no app with that id is connected. */
	NotConnected,
	/** The app did not import the file (its reason is in the message), or did not answer in time. */
	AppFailed,
	/** No game instance, item library or profile. */
	Unavailable,
};

/** The 3D apps the bridge add-ons exist for. Unknown: a client that named an app this build does not know. */
UENUM(BlueprintType)
enum class EStageBridgeApp : uint8
{
	Unknown,
	Blender,
	Maya,
	Max,
	Cinema4D,
};

/** A 3D app connected to the bridge (it presented this run's token). */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageBridgeClientInfo
{
	GENERATED_BODY()

	/** Unique for this run of the bridge; never reused. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Bridge")
	int32 ClientId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Bridge")
	EStageBridgeApp App = EStageBridgeApp::Unknown;

	/** As the add-on reported it, capped and stripped to printable characters. Display only. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Bridge")
	FString AppVersion;

	/** Whether the app can import glTF binary. Maya cannot until the USD writer exists (ADR 0022 §1). */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Bridge")
	bool bImportsGlb = false;
};

/** Result of an export to a file or a send to an app. Message is user-facing; Code is for logic. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageBridgeResultInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Bridge")
	EStageBridgeResult Code = EStageBridgeResult::Success;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Bridge")
	FText Message;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Bridge")
	FString SceneName;

	/** The written file (an export, or the outbox copy that was sent). */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Bridge")
	FString FilePath;

	/** The app the scene went to; 0 for an export to a file. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Bridge")
	int32 ClientId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Bridge")
	int32 ExportedItems = 0;

	/** Items left out: not in this Library, or not owned. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Bridge")
	int32 SkippedItems = 0;

	bool IsSuccess() const { return Code == EStageBridgeResult::Success; }

	static FStageBridgeResultInfo Fail(EStageBridgeResult InCode, const FText& InMessage)
	{
		FStageBridgeResultInfo Info;
		Info.Code = InCode;
		Info.Message = InMessage;
		return Info;
	}
};
