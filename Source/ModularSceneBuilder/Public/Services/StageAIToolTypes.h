// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StageAIToolTypes.generated.h"

/** What an external AI 3D tool can do (Docs/ADR/0023-ai-tool-providers-and-credential-vault.md §3). */
UENUM(BlueprintType, meta = (Bitflags, UseEnumValuesAsMaskValuesInEditor = "true"))
enum class EStageAIToolCapability : uint8
{
	None = 0 UMETA(Hidden),
	/** Takes a finished model (a .glb from StageCraft): remeshing, retexturing, retopology, LODs. The only one used by D3. */
	AcceptModel = 1 << 0,
	/** Generates a model from a text prompt (D5). */
	TextTo3D = 1 << 1,
	/** Generates a model from an image (D5). */
	ImageTo3D = 1 << 2,
};
ENUM_CLASS_FLAGS(EStageAIToolCapability);

/** Outcome codes of AI tool operations. Appended only, never reordered. */
UENUM(BlueprintType)
enum class EStageAIToolResult : uint8
{
	Success,
	InvalidRequest,
	/** No tool with that id is configured. */
	NotFound,
	/** The tool needs an API key and none is saved. */
	NeedsKey,
	/** The tool's service refused the key. */
	KeyRejected,
	/** Another job is running. */
	Busy,
	/** Keys cannot be stored safely on this platform, or the tool is unavailable. */
	Unavailable,
	/** The tool or its service failed. */
	Failed,
	/** Cancelled before the tool answered. */
	Cancelled,
	/** The tool did not answer within UStageAIToolSubsystem::OperationTimeoutSeconds; the operation was cancelled. */
	TimedOut,
};

/** What is known about a tool's key. Never the key itself. */
UENUM(BlueprintType)
enum class EStageAIToolKeyState : uint8
{
	/** The tool needs no key. */
	NotNeeded,
	Missing,
	/** Saved, not checked in this run. */
	Saved,
	/** Accepted by the tool's service in this run. */
	Verified,
	/** Refused by the tool's service in this run. */
	Rejected,
};

USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageAIToolResultInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AI Tools")
	EStageAIToolResult Code = EStageAIToolResult::Success;

	/** User-facing. Never contains a key. */
	UPROPERTY(BlueprintReadOnly, Category = "AI Tools")
	FText Message;

	UPROPERTY(BlueprintReadOnly, Category = "AI Tools")
	FName ProviderId;

	/** A file the tool produced, when it produced one. */
	UPROPERTY(BlueprintReadOnly, Category = "AI Tools")
	FString OutputPath;

	bool IsSuccess() const { return Code == EStageAIToolResult::Success; }

	static FStageAIToolResultInfo Ok(FName InProviderId, const FText& InMessage = FText::GetEmpty())
	{
		FStageAIToolResultInfo Info;
		Info.ProviderId = InProviderId;
		Info.Message = InMessage;
		return Info;
	}

	static FStageAIToolResultInfo Fail(EStageAIToolResult InCode, FName InProviderId, const FText& InMessage)
	{
		FStageAIToolResultInfo Info;
		Info.Code = InCode;
		Info.ProviderId = InProviderId;
		Info.Message = InMessage;
		return Info;
	}
};

/** A configured tool as views see it. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageAIToolInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AI Tools")
	FName ProviderId;

	UPROPERTY(BlueprintReadOnly, Category = "AI Tools")
	FText DisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "AI Tools", meta = (Bitmask, BitmaskEnum = "/Script/ModularSceneBuilder.EStageAIToolCapability"))
	uint8 Capabilities = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AI Tools")
	EStageAIToolKeyState KeyState = EStageAIToolKeyState::Missing;

	bool Can(EStageAIToolCapability Capability) const { return EnumHasAnyFlags(static_cast<EStageAIToolCapability>(Capabilities), Capability); }

	/** True when the tool can be used now as far as keys go (no key needed, or one saved and not refused). */
	bool HasUsableKey() const
	{
		return KeyState == EStageAIToolKeyState::NotNeeded || KeyState == EStageAIToolKeyState::Saved || KeyState == EStageAIToolKeyState::Verified;
	}
};

/** One model handed to a tool. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageAIToolJob
{
	GENERATED_BODY()

	/** An absolute .glb path that StageCraft exported. */
	UPROPERTY(BlueprintReadOnly, Category = "AI Tools")
	FString ModelPath;

	/** The stage's name, for the tool's own naming. */
	UPROPERTY(BlueprintReadOnly, Category = "AI Tools")
	FString Name;
};
