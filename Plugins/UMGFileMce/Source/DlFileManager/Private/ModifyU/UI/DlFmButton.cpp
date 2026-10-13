// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#include "ModifyU/UI/DlFmButton.h"

#include "Binding/States/WidgetStateBitfield.h"
#include "Binding/States/WidgetStateRegistration.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "ModifyU/UI/SDlFmButton.h"
#include "ModifyU/UI/DlFmButtonSlot.h"
#include "Styling/DefaultStyleCache.h"
#include "Styling/UMGCoreStyle.h"
#include "Blueprint/WidgetTree.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(DlFmButton)

#define LOCTEXT_NAMESPACE "UMG"

/////////////////////////////////////////////////////
// UDlFmButton

UDlFmButton::UDlFmButton(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	WidgetStyle = UE::Slate::Private::FDefaultStyleCache::GetRuntime().GetButtonStyle();
PRAGMA_ENABLE_DEPRECATION_WARNINGS

#if WITH_EDITOR 
	if (IsEditorWidget())
	{
PRAGMA_DISABLE_DEPRECATION_WARNINGS
		WidgetStyle = UE::Slate::Private::FDefaultStyleCache::GetEditor().GetButtonStyle();
PRAGMA_ENABLE_DEPRECATION_WARNINGS
	}
#endif // WITH_EDITOR

PRAGMA_DISABLE_DEPRECATION_WARNINGS
	ColorAndOpacity = FLinearColor::White;
	BackgroundColor = FLinearColor::White;

	ClickMethod = EButtonClickMethod::DownAndUp;
	TouchMethod = EButtonTouchMethod::DownAndUp;

	// Buttons default to not being draggable
	bAllowDragDrop = false;

	IsFocusable = true;
PRAGMA_ENABLE_DEPRECATION_WARNINGS

#if WITH_EDITORONLY_DATA
	AccessibleBehavior = ESlateAccessibleBehavior::Summary;
	bCanChildrenBeAccessible = false;
#endif
}

void UDlFmButton::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);

	MyButton.Reset();
}

TSharedRef<SWidget> UDlFmButton::RebuildWidget()
{
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	MyButton = SNew(SDlFmButton)
		.OnClicked(BIND_UOBJECT_DELEGATE(FOnClicked, SlateHandleClicked))
		.OnPressed(BIND_UOBJECT_DELEGATE(FSimpleDelegate, SlateHandlePressed))
		.OnReleased(BIND_UOBJECT_DELEGATE(FSimpleDelegate, SlateHandleReleased))
		.OnHovered_UObject( this, &ThisClass::SlateHandleHovered )
		.OnUnhovered_UObject( this, &ThisClass::SlateHandleUnhovered )
		.OnReceivedFocus_UObject(this, &ThisClass::SlateHandleOnReceivedFocus)
		.OnLostFocus_UObject(this, &ThisClass::SlateHandleOnLostFocus)
		.OnSlateButtonDragDetected(BIND_UOBJECT_DELEGATE(FOnDragDetected, SlateHandleDragDetected))
		.OnSlateButtonDragEnter(BIND_UOBJECT_DELEGATE(FOnDragEnter, SlateHandleDragEnter))
		.OnSlateButtonDragLeave(BIND_UOBJECT_DELEGATE(FOnDragLeave, SlateHandleDragLeave))
		.OnSlateButtonDragOver(BIND_UOBJECT_DELEGATE(FOnDragOver, SlateHandleDragOver))
		.OnSlateButtonDrop(BIND_UOBJECT_DELEGATE(FOnDrop, SlateHandleDrop))
		.ButtonStyle(&WidgetStyle)
		.ClickMethod(ClickMethod)
		.TouchMethod(TouchMethod)
		.PressMethod(PressMethod)
		.Paint_OffsetPercent(Paint_OffsetPercent)//Dl+
		.Paint_OverflowPercent(Paint_OverflowPercent)//Dl+
		.IsFocusable(IsFocusable)
		.AllowDragDrop(bAllowDragDrop);
		;
PRAGMA_ENABLE_DEPRECATION_WARNINGS
	if ( GetChildrenCount() > 0 )
	{
		Cast<UDlFmButtonSlot>(GetContentSlot())->BuildSlot(MyButton.ToSharedRef());
	}
	
	return MyButton.ToSharedRef();
}

void UDlFmButton::SynchronizeProperties()
{
	Super::SynchronizeProperties();

	if (!MyButton.IsValid())
	{
		return;
	}

PRAGMA_DISABLE_DEPRECATION_WARNINGS
	MyButton->SetButtonStyle(&WidgetStyle);
	MyButton->SetColorAndOpacity( ColorAndOpacity );
	MyButton->SetBorderBackgroundColor( BackgroundColor );
	MyButton->SetClickMethod(ClickMethod);
	MyButton->SetTouchMethod(TouchMethod);
	MyButton->SetPressMethod(PressMethod);
	MyButton->SetAllowDragDrop(bAllowDragDrop);
PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

UClass* UDlFmButton::GetSlotClass() const
{
	return UDlFmButtonSlot::StaticClass();
}

void UDlFmButton::OnSlotAdded(UPanelSlot* InSlot)
{
	// Add the child to the live slot if it already exists
	if ( MyButton.IsValid() )
	{
		CastChecked<UDlFmButtonSlot>(InSlot)->BuildSlot(MyButton.ToSharedRef());
	}
}

void UDlFmButton::OnSlotRemoved(UPanelSlot* InSlot)
{
	// Remove the widget from the live slot if it exists.
	if ( MyButton.IsValid() )
	{
		MyButton->SetContent(SNullWidget::NullWidget);
	}
}

PRAGMA_DISABLE_DEPRECATION_WARNINGS
void UDlFmButton::SetStyle(const FButtonStyle& InStyle)
{
	WidgetStyle = InStyle;
	if ( MyButton.IsValid() )
	{
		MyButton->SetButtonStyle(&WidgetStyle);
	}
}

const FButtonStyle& UDlFmButton::GetStyle() const
{
	return WidgetStyle;
}

void UDlFmButton::SetColorAndOpacity(FLinearColor InColorAndOpacity)
{
	ColorAndOpacity = InColorAndOpacity;
	if ( MyButton.IsValid() )
	{
		MyButton->SetColorAndOpacity(InColorAndOpacity);
	}
}

FLinearColor UDlFmButton::GetColorAndOpacity() const
{
	return ColorAndOpacity;
}

void UDlFmButton::SetBackgroundColor(FLinearColor InBackgroundColor)
{
	BackgroundColor = InBackgroundColor;
	if ( MyButton.IsValid() )
	{
		MyButton->SetBorderBackgroundColor(InBackgroundColor);
	}
}

FLinearColor UDlFmButton::GetBackgroundColor() const
{
	return BackgroundColor;
}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

bool UDlFmButton::IsPressed() const
{
	if ( MyButton.IsValid() )
	{
		return MyButton->IsPressed();
	}

	return false;
}

PRAGMA_DISABLE_DEPRECATION_WARNINGS
void UDlFmButton::SetClickMethod(EButtonClickMethod::Type InClickMethod)
{
	ClickMethod = InClickMethod;
	if ( MyButton.IsValid() )
	{
		MyButton->SetClickMethod(ClickMethod);
	}
}

EButtonClickMethod::Type UDlFmButton::GetClickMethod() const
{
	return ClickMethod;
}

void UDlFmButton::SetTouchMethod(EButtonTouchMethod::Type InTouchMethod)
{
	TouchMethod = InTouchMethod;
	if ( MyButton.IsValid() )
	{
		MyButton->SetTouchMethod(TouchMethod);
	}
}

EButtonTouchMethod::Type UDlFmButton::GetTouchMethod() const
{
	return TouchMethod;
}

void UDlFmButton::SetPressMethod(EButtonPressMethod::Type InPressMethod)
{
	PressMethod = InPressMethod;
	if ( MyButton.IsValid() )
	{
		MyButton->SetPressMethod(PressMethod);
	}
}

EButtonPressMethod::Type UDlFmButton::GetPressMethod() const
{
	return PressMethod;
}

bool UDlFmButton::GetIsFocusable() const
{
	return IsFocusable;
}

DLFILEMANAGER_API void UDlFmButton::SetAllowDragDrop(bool bInAllowDragDrop)
{
	bAllowDragDrop = bInAllowDragDrop;
	if (MyButton.IsValid() && MyButton->IsParentValid())
	{
		MyButton->SetAllowDragDrop(bAllowDragDrop);
	}
}

void UDlFmButton::InitIsFocusable(bool InIsFocusable)
{
	IsFocusable = InIsFocusable;
}

PRAGMA_ENABLE_DEPRECATION_WARNINGS

void UDlFmButton::PostLoad()
{
	Super::PostLoad();

	if ( GetChildrenCount() > 0 )
	{
		//TODO UMG Pre-Release Upgrade, now buttons have slots of their own.  Convert existing slot to new slot.
		if ( UPanelSlot* PanelSlot = GetContentSlot() )
		{
			UDlFmButtonSlot* ButtonSlot = Cast<UDlFmButtonSlot>(PanelSlot);
			if ( ButtonSlot == NULL )
			{
				ButtonSlot = NewObject<UDlFmButtonSlot>(this);
				ButtonSlot->Content = GetContentSlot()->Content;
				ButtonSlot->Content->Slot = ButtonSlot;
				Slots[0] = ButtonSlot;
			}
		}
	}
}

FReply UDlFmButton::SlateHandleClicked()
{
	OnClicked.Broadcast();

	const float TmpTime=GetWorld()->RealTimeSeconds;//Dl+
	if (TmpTime >= LastClickedTime+ GapTime)//Dl+
	{//Dl+
		LastClickedTime = TmpTime;//Dl+
		OnFilteredClicked.Broadcast();//Dl+
	}//Dl+

	return FReply::Handled();
}

void UDlFmButton::SlateHandlePressed()
{
	OnPressed.Broadcast();
	BroadcastBinaryPostStateChange(UWidgetPressedStateRegistration::Bit, true);
	
	const float TmpTime=GetWorld()->RealTimeSeconds;//Dl+
	if (TmpTime >= LastPressedTime+ GapTime)//Dl+
	{//Dl+
		LastPressedTime = TmpTime;//Dl+
		OnFilteredPressed.Broadcast();//Dl+
	}//Dl+
}

void UDlFmButton::SlateHandleReleased()
{
	OnReleased.Broadcast();
	BroadcastBinaryPostStateChange(UWidgetPressedStateRegistration::Bit, false);
	
	const float TmpTime=GetWorld()->RealTimeSeconds;//Dl+
	if (TmpTime >= LastReleasedTime+ GapTime)//Dl+
	{//Dl+
		LastReleasedTime = TmpTime;//Dl+
		OnFilteredReleased.Broadcast();//Dl+
	}//Dl+
}

void UDlFmButton::SlateHandleHovered()
{
	OnHovered.Broadcast();
	BroadcastBinaryPostStateChange(UWidgetHoveredStateRegistration::Bit, true);
}

void UDlFmButton::SlateHandleUnhovered()
{
	OnUnhovered.Broadcast();
	BroadcastBinaryPostStateChange(UWidgetHoveredStateRegistration::Bit, false);
}

void UDlFmButton::SlateHandleOnReceivedFocus()
{
	OnReceivedFocus.ExecuteIfBound();
}

void UDlFmButton::SlateHandleOnLostFocus()
{
	OnLostFocus.ExecuteIfBound();
}

FReply UDlFmButton::SlateHandleDragDetected(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (OnButtonDragDetected.IsBound())
	{
		return OnButtonDragDetected.Execute(MyGeometry, MouseEvent);
	}
	return FReply::Unhandled();
}

void UDlFmButton::SlateHandleDragEnter(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	if (OnButtonDragEnter.IsBound())
	{
		OnButtonDragEnter.Execute(MyGeometry, DragDropEvent);
	}
}

void UDlFmButton::SlateHandleDragLeave(const FDragDropEvent& DragDropEvent)
{
	if (OnButtonDragLeave.IsBound())
	{
		OnButtonDragLeave.Execute(DragDropEvent);
	}
}

FReply UDlFmButton::SlateHandleDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	if (OnButtonDragOver.IsBound())
	{
		return OnButtonDragOver.Execute(MyGeometry, DragDropEvent);
	}
	return FReply::Unhandled();
}

FReply UDlFmButton::SlateHandleDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	if (OnButtonDrop.IsBound())
	{
		return OnButtonDrop.Execute(MyGeometry, DragDropEvent);
	}
	return FReply::Unhandled();
}

#if WITH_ACCESSIBILITY
TSharedPtr<SWidget> UDlFmButton::GetAccessibleWidget() const
{
	return MyButton;
}
#endif

#if WITH_EDITOR

const FText UDlFmButton::GetPaletteCategory()
{
	return LOCTEXT("Common", "Common");
}

#endif

/////////////////////////////////////////////////////

#undef LOCTEXT_NAMESPACE

