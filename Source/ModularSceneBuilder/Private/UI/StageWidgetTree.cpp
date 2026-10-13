// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageWidgetTree.h"

#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"

namespace StageWidgetTree
{
	bool WrapInPlace(UWidget& Widget, UPanelWidget& Wrapper)
	{
		UPanelWidget* Parent = Widget.GetParent();
		if (!Parent || !Wrapper.CanAddMoreChildren() || &Wrapper == Parent)
		{
			return false;
		}

		// The old slot is the template of the new one (UPanelWidget::AddChild copies a template of the same class), so size
		// rule, padding and alignment carry over. It stays alive for this call: nothing here can run garbage collection.
		UPanelSlot* OldSlot = Widget.Slot;
		const int32 Index = Parent->GetChildIndex(&Widget);

		// AddChild removes Widget from Parent first.
		Wrapper.AddChild(&Widget);
		return Parent->InsertChildAt(Index, &Wrapper, OldSlot) != nullptr;
	}
}
