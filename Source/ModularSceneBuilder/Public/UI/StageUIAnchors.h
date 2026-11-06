// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/Widget.h"
#include "GameplayTagContainer.h"
#include "Show/StageShowDiagnostics.h"
#include "UObject/Interface.h"
#include "StageUIAnchors.generated.h"

/**
 * One panel control the assistant can name and point at (Docs/ADR/0030-assistant-knowledge-live-context-and-tools.md §3). A
 * panel declares its anchors in one static table next to its build code and takes the control's label and tooltip from it, so
 * the help the assistant gives and the tooltip the user sees cannot drift apart. Slate widget paths are never used: an anchor is
 * a stable id, and the panel resolves it to its live widget.
 */
struct FStageUIAnchor
{
	FName ControlId;
	FText Label;
	FText Tooltip;
};

UINTERFACE(MinimalAPI, meta = (CannotImplementInterfaceInBlueprint))
class UStageAnchoredPanel : public UInterface
{
	GENERATED_BODY()
};

/** A panel with anchors: it resolves an anchor id to the widget that shows it now. */
class MODULARSCENEBUILDER_API IStageAnchoredPanel
{
	GENERATED_BODY()

public:
	/** The live widget of ControlId; null when the panel does not show that control now (a collapsed section). */
	virtual class UWidget* FindAnchorWidget(FName ControlId) const = 0;
};

/** The live widgets of one panel's anchors. The panel resets it when it rebuilds its tree and adds each anchored control again. */
struct MODULARSCENEBUILDER_API FStageAnchorWidgets
{
	void Reset() { Widgets.Reset(); }
	void Add(FName ControlId, class UWidget* Widget);
	class UWidget* Find(FName ControlId) const;

private:
	TMap<FName, TWeakObjectPtr<class UWidget>> Widgets;
};

namespace StageUIAnchors
{
	/** One panel's table. */
	struct FTable
	{
		FGameplayTag Panel;
		TConstArrayView<FStageUIAnchor> Anchors;
	};

	/** Every anchored panel's table, in a fixed order (the knowledge index and the tests read it). */
	MODULARSCENEBUILDER_API TArray<FTable> GetTables();

	/** The anchor ControlId of Panel; null when there is none. */
	MODULARSCENEBUILDER_API const FStageUIAnchor* Find(const FGameplayTag& Panel, FName ControlId);

	/**
	 * The anchor ControlId in a panel's own table, for its build code. A missing id is a programmer error: it ensures and returns
	 * an empty anchor, so the panel still builds.
	 */
	MODULARSCENEBUILDER_API const FStageUIAnchor& Get(TConstArrayView<FStageUIAnchor> Anchors, FName ControlId);

	/** Whether Widget and everything that contains it are visible: a control in a collapsed section is not shown. */
	MODULARSCENEBUILDER_API bool IsShown(const UWidget& Widget);

	/** Scrolls every scroll box around Widget so it is in view. */
	MODULARSCENEBUILDER_API void BringIntoView(UWidget& Widget);

	/**
	 * A soft 1.5 s pulse on Widget (its render opacity dips three times), then its opacity is restored and nothing more runs. A
	 * second pulse on the same widget restarts it. Game thread only; a widget destroyed meanwhile simply ends its pulse.
	 */
	MODULARSCENEBUILDER_API void Pulse(UWidget& Widget);

	/** How long a pulse lasts (seconds). */
	constexpr float PulseSeconds = 1.5f;

	/**
	 * The panel and anchored control where a diagnostic place is fixed (Docs/ADR/0030 §5): the Problems list goes there and the
	 * assistant's SHOW ME points at it. False for EStageDiagnosticPlace::None.
	 */
	MODULARSCENEBUILDER_API bool GetPlaceTarget(EStageDiagnosticPlace Place, FGameplayTag& OutPanel, FName& OutControl);
}
