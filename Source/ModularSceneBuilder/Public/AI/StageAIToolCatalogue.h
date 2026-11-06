// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageAITypes.h"

/** What a tool touches, for the proposal card and the rule of what may apply without asking (Docs/ADR/0030 §6). */
enum class EStageAIToolEffect : uint8
{
	/** Answers from the live stage, the marketplace results or the knowledge; changes nothing. */
	None,
	/** Fixture controls (dimmer, colour, beam, position) of placed fixtures. */
	Fixtures,
	/** Cue playback (GO, BACK, go to a cue). Not recorded in the history, so a rollback does not undo it. */
	Playback,
	/** The cue list (store, update, delete). */
	Cues,
	/** The DMX patch (universe and address of fixtures). */
	Patch,
	/** Placed items (add, move, replace, delete, build). */
	Stage,
	/** Weather, time of day, sky. */
	Environment,
	/** Report files on disk. Not undoable. */
	Files,
	/** Undo and redo themselves. */
	History,
	/** The community marketplace. A paid listing is bought only after a local yes (Docs/ADR/0021 §13.3). */
	Marketplace,
};

/** One tool of the assistant: an intent, its wire name and what it may do. */
struct FStageAITool
{
	EStageAIIntent Intent = EStageAIIntent::Help;

	/** The name the online model uses ("store_cue"), stable: it is part of the wire format. */
	const TCHAR* Name = TEXT("");

	EStageAIToolEffect Effect = EStageAIToolEffect::None;

	/** Recorded in the undo history, so a plan's rollback and Ctrl+Z revert it. */
	bool bUndoable = false;

	/** May be one step of a plan of several steps. Tools that are not (undo, export, the marketplace) only ever run alone. */
	bool bPlannable = false;

	bool IsChange() const { return Effect != EStageAIToolEffect::None; }
};

/**
 * The closed, bounded set of things the AI assistant can do (Docs/ADR/0030 §2, §6): one entry per EStageAIIntent, in enum order.
 * Pure and static. StageAICommandSchema takes the wire names from it, UStageAICommandSubsystem the plan rules, the chat panel the
 * proposal card's lines. Covered by StageCraft.Assistant.ToolCatalogue.
 *
 * Never a tool (ADR §2): purchases outside the confirmed marketplace flow, the wallet, profile or entitlements, credentials, file
 * deletion, scene open / save / overwrite, Clear Stage, development console commands, DMX OUT on or off. GetNeverExposed names
 * them; the tests check no tool matches one.
 */
namespace StageAIToolCatalogue
{
	/** Most steps one plan may hold (ADR §6). */
	constexpr int32 MaxPlanSteps = 32;

	/** Every tool, in EStageAIIntent order. */
	MODULARSCENEBUILDER_API TConstArrayView<FStageAITool> GetTools();

	/** The tool of Intent. Intent must be below EStageAIIntent::Count. */
	MODULARSCENEBUILDER_API const FStageAITool& GetTool(EStageAIIntent Intent);

	/** The tool named Name ("store_cue"), or null. */
	MODULARSCENEBUILDER_API const FStageAITool* FindTool(FStringView Name);

	/**
	 * Whether Command may apply without the user's APPLY (ADR §6): cue playback, and fixture controls of the selected fixtures.
	 * Everything else (cues, patch, placed items, environment, controls of fixtures picked by name) waits for APPLY when a plan
	 * comes from the online model or holds several steps.
	 */
	MODULARSCENEBUILDER_API bool CanApplyWithoutAsking(const FStageAICommand& Command);

	/**
	 * Whether Steps can run as one plan: 1 to MaxPlanSteps action steps, each one plannable when there are several. False with a
	 * user-facing reason in Language otherwise; nothing has run.
	 */
	MODULARSCENEBUILDER_API bool CheckPlan(TConstArrayView<FStageAICommand> Steps, EStageAILanguage Language, FText& OutError);

	/** Whether any step of Steps changes something the history does not record (a GO): a rollback leaves it as it is. */
	MODULARSCENEBUILDER_API bool HasStepsNotUndone(TConstArrayView<FStageAICommand> Steps);

	/** One line of a proposal card for a validated action ("Store cue 5 \"Intro\", 3 s fade"), in Language. */
	MODULARSCENEBUILDER_API FText DescribeStep(const FStageAICommand& Command, EStageAILanguage Language);

	/** The operations no tool may ever offer, as plain words, for the system prompt and the tests. */
	MODULARSCENEBUILDER_API TConstArrayView<const TCHAR*> GetNeverExposed();
}
