// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/PanelWidget.h"

/**
 * Small structural edits to a widget tree that C++ bases apply to their Blueprint layouts, so a layout rule (a fixed column
 * width, a scrolling strip) holds whatever the Blueprint author did (Rules.md: correctness never depends on an undocumented
 * Blueprint setup step).
 */
namespace StageWidgetTree
{
	/**
	 * Puts Wrapper where Widget is in its parent panel and Widget inside Wrapper. The parent's slot settings (size rule, padding,
	 * alignment) carry over to Wrapper's new slot, so the layout around it is unchanged.
	 *
	 * Only valid before the parent builds its Slate widget (NativeOnInitialized of the owning user widget): UMG reorders the
	 * children of a live panel only on a full rebuild (UPanelWidget::InsertChildAt). Returns false, changing nothing, when Widget
	 * has no parent panel or Wrapper cannot take another child.
	 */
	bool WrapInPlace(UWidget& Widget, UPanelWidget& Wrapper);

	/** The nearest ancestor of Widget of class T, or null. */
	template <typename T>
	T* FindAncestor(const UWidget& Widget);
}

template <typename T>
T* StageWidgetTree::FindAncestor(const UWidget& Widget)
{
	for (UPanelWidget* Parent = Widget.GetParent(); Parent; Parent = Parent->GetParent())
	{
		if (T* Match = Cast<T>(Parent))
		{
			return Match;
		}
	}
	return nullptr;
}
