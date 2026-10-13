// Copyright DamenTools internal. Precise Transform plugin.

#include "FloatingCoordinatePanel.h"
#include "PreciseTransform.h"
#include "PreciseTransformLibrary.h"
#include "Components/EditableTextBox.h"
#include "Components/CanvasPanelSlot.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"

void UFloatingCoordinatePanel::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	// All three fields bind to ONE handler on purpose: committing any single
	// field re-applies all three current values together, which is what the
	// existing WB_ObjectDescription row already does (RepositionPlacedObject
	// takes a whole FVector, so a per-axis commit has to send the other two
	// axes anyway - reading them straight back out of the boxes is both
	// simpler and impossible to get out of sync with what the user sees).
	//
	// Each bind is guarded because BindWidget members are legitimately null
	// until the paired Widget Blueprint declares widgets with matching names
	// (XInput/YInput/ZInput). Binding blind would crash the editor on the
	// first compile of a half-built WBP instead of just doing nothing.
	if (XInput)
	{
		XInput->OnTextCommitted.AddDynamic(this, &UFloatingCoordinatePanel::OnCoordinateCommitted);
	}
	if (YInput)
	{
		YInput->OnTextCommitted.AddDynamic(this, &UFloatingCoordinatePanel::OnCoordinateCommitted);
	}
	if (ZInput)
	{
		ZInput->OnTextCommitted.AddDynamic(this, &UFloatingCoordinatePanel::OnCoordinateCommitted);
	}
}

void UFloatingCoordinatePanel::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// Hover test against this widget's OWN geometry, done natively every tick.
	// Deliberately not Slate's OnMouseEnter/OnMouseLeave and not the Blueprint
	// USlateBlueprintLibrary wrapper: the crossing events are what failed
	// earlier today (they fire relative to widget boundaries the mouse never
	// cleanly crosses), and a state test that is recomputed from scratch each
	// frame cannot get stuck "half hovered" the way paired enter/leave events can.
	if (FSlateApplication::IsInitialized())
	{
		bIsPanelHovered = MyGeometry.IsUnderLocation(FSlateApplication::Get().GetCursorPos());
	}

	const bool bHasTarget = LockedTarget.IsValid();

	// Collapsed (not Hidden) with no target: zero layout footprint, so an
	// unused panel can never sit invisibly on top of the toolbar row and
	// swallow clicks. Visible (not SelfHitTestInvisible) with a target,
	// because the child EditableTextBoxes must be clickable to be typed into.
	// Only written on change - setting visibility every tick invalidates
	// layout every tick for no reason.
	const ESlateVisibility DesiredVisibility = bHasTarget ? ESlateVisibility::Visible : ESlateVisibility::Collapsed;
	if (GetVisibility() != DesiredVisibility)
	{
		SetVisibility(DesiredVisibility);
	}

	if (!bHasTarget)
	{
		return;
	}

	// Reposition CONTINUOUSLY while unhovered, not just when the target
	// changes: this project's camera pans and zooms freely, so a position
	// computed once goes stale the instant the camera moves - the panel would
	// drift away from the object while the user's mouse is still travelling
	// toward it, which is the exact structural bug this class exists to fix.
	//
	// Freeze completely once the mouse is inside the panel, so it cannot jump
	// out from under the cursor (or out from under a half-typed coordinate)
	// while the user is interacting with it.
	if (!bIsPanelHovered)
	{
		UpdateScreenPosition();
		RefreshFieldsFromTarget();
	}
}

void UFloatingCoordinatePanel::RefreshFieldsFromTarget()
{
	if (!LockedTarget.IsValid() || !XInput || !YInput || !ZInput)
	{
		return;
	}

	// Never overwrite a value the user is part-way through typing.
	if (XInput->HasKeyboardFocus() || YInput->HasKeyboardFocus() || ZInput->HasKeyboardFocus())
	{
		return;
	}

	FString XStr, YStr, ZStr;
	UPreciseTransformLibrary::FormatLocationAsStrings(LockedTarget->GetActorLocation(), XStr, YStr, ZStr);

	// Write only on change: SetText every frame would invalidate layout every frame.
	auto SetIfChanged = [](UEditableTextBox* Box, const FString& Value)
	{
		if (!Box->GetText().ToString().Equals(Value))
		{
			Box->SetText(FText::FromString(Value));
		}
	};
	SetIfChanged(XInput, XStr);
	SetIfChanged(YInput, YStr);
	SetIfChanged(ZInput, ZStr);
}

void UFloatingCoordinatePanel::SetLockedTarget(AActor* NewTarget)
{
	// Intentionally just the assignment. Visibility and position are derived
	// state, recomputed from LockedTarget.IsValid() in NativeTick - doing them
	// here too would mean two places that can disagree about whether the panel
	// should be showing, which is the class of bug this design removes.
	LockedTarget = NewTarget;
}

void UFloatingCoordinatePanel::UpdateScreenPosition()
{
	if (!LockedTarget.IsValid())
	{
		return;
	}

	APlayerController* PC = GetOwningPlayer();
	if (!PC)
	{
		return;
	}

	FVector2D ProjectedPos = FVector2D::ZeroVector;
	if (!PC->ProjectWorldLocationToScreen(LockedTarget->GetActorLocation(), ProjectedPos, /*bPlayerViewportRelative=*/false))
	{
		// Target is behind the camera / outside the frustum. Leave the last
		// good position alone rather than snapping the panel to a garbage
		// coordinate - the panel is about to be hidden anyway once hover ends.
		return;
	}

	// ProjectWorldLocationToScreen and GetViewportSize both work in real
	// viewport PIXELS, but a UCanvasPanelSlot position is in DPI-unscaled
	// layout units. Converting both into layout units keeps the panel pinned
	// to the object at any DPI/resolution scale; skipping this would look
	// correct only at scale 1.0 and drift proportionally everywhere else.
	const float ViewportScale = UWidgetLayoutLibrary::GetViewportScale(this);
	if (ViewportScale <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	const FVector2D ViewportSize = UWidgetLayoutLibrary::GetViewportSize(this) / ViewportScale;

	// Panel extents come from the widget itself, never a hardcoded size, so
	// the clamp stays correct if the paired WBP's layout changes. Desired size
	// is 0 on the very first tick (nothing laid out yet); the cached geometry
	// is the fallback, and a zero size just clamps against the full viewport.
	FVector2D PanelSize = GetDesiredSize();
	if (PanelSize.X <= 0.0f || PanelSize.Y <= 0.0f)
	{
		PanelSize = FVector2D(GetCachedGeometry().GetLocalSize());
	}

	FVector2D TargetPos = (ProjectedPos / ViewportScale) + ScreenOffset;

	// Clamp so the panel is never pushed partly off-screen by the offset or by
	// an object near a viewport edge - an unreachable half-visible panel is
	// the same user-facing failure as a mispositioned one.
	TargetPos.X = FMath::Clamp(TargetPos.X, 0.0f, FMath::Max(0.0f, ViewportSize.X - PanelSize.X));
	TargetPos.Y = FMath::Clamp(TargetPos.Y, 0.0f, FMath::Max(0.0f, ViewportSize.Y - PanelSize.Y));

	UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot);
	if (!CanvasSlot)
	{
		// Real setup error (this widget was not added as a direct child of a
		// CanvasPanel), so it must be reported - but reporting it from Tick
		// would put a line in the log every frame and bury everything else.
		// The guard is a function-local static rather than a member so the
		// reviewed/approved header stays byte-for-byte as signed off; one
		// warning per editor session is enough to diagnose a parenting mistake.
		static bool bLoggedMissingCanvasSlot = false;
		if (!bLoggedMissingCanvasSlot)
		{
			bLoggedMissingCanvasSlot = true;
			UE_LOG(LogPreciseTransform, Warning,
				TEXT("UFloatingCoordinatePanel::UpdateScreenPosition: widget '%s' is not a direct child of a CanvasPanel (Slot is %s), so it cannot reposition itself. Add it to WB_Demo_UI's root CanvasPanel_0 - see FloatingCoordinatePanel.h. Logged once per session."),
				*GetName(),
				Slot ? *Slot->GetClass()->GetName() : TEXT("null"));
		}
		return;
	}

	CanvasSlot->SetPosition(TargetPos);
}

void UFloatingCoordinatePanel::OnCoordinateCommitted(const FText& Text, ETextCommit::Type CommitMethod)
{
	ApplyCommittedCoordinates();
}

void UFloatingCoordinatePanel::ApplyCommittedCoordinates()
{
	if (!LockedTarget.IsValid() || !BuildManagerRef.IsValid())
	{
		return;
	}

	if (!XInput || !YInput || !ZInput)
	{
		return;
	}

	// Direct C++ call, not reflection: RepositionPlacedObjectFromStrings lives
	// in this same module, so there is nothing to bridge. It already owns all
	// of the parse/validate/snap/occupancy-re-register behavior (and fails
	// loudly on its own), and it is reused here completely unmodified -
	// nothing about the move is reimplemented in this class.
	FVector AppliedLocation = FVector::ZeroVector;
	UPreciseTransformLibrary::RepositionPlacedObjectFromStrings(
		BuildManagerRef.Get(),
		LockedTarget.Get(),
		XInput->GetText().ToString(),
		YInput->GetText().ToString(),
		ZInput->GetText().ToString(),
		AppliedLocation);
}
