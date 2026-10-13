// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#include "ModifyU/UI/DlFmFloat_EditSimple.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/Font.h"
#include "Engine/LocalPlayer.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SEditableText.h"
#include "Slate/SlateBrushAsset.h"
#include "Styling/DefaultStyleCache.h"
#include "Styling/UMGCoreStyle.h"
#include "Materials/MaterialInterface.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(DlFmFloat_EditSimple)

#define LOCTEXT_NAMESPACE "UMG"

/////////////////////////////////////////////////////
// UDlFmFloat_EditSimple

UDlFmFloat_EditSimple::UDlFmFloat_EditSimple(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{

PRAGMA_DISABLE_DEPRECATION_WARNINGS
	WidgetStyle = UE::Slate::Private::FDefaultStyleCache::GetRuntime().GetEditableTextStyle();

	if (!IsRunningDedicatedServer())
	{
		static ConstructorHelpers::FObjectFinder<UFont> DefaultFontObj(*UWidget::GetDefaultFontName());
		FSlateFontInfo Font(DefaultFontObj.Object, 12, FName("Regular"));//Dl+
		//The FSlateFontInfo just created doesn't contain a composite font (while the default from the WidgetStyle does),
		//so in the case the Font object is replaced by a null one, we have to keep the composite one as a fallback.
		Font.CompositeFont = WidgetStyle.Font.CompositeFont;
		WidgetStyle.SetFont(Font);
	}
	
#if WITH_EDITOR 
	if (IsEditorWidget())
	{
		WidgetStyle = UE::Slate::Private::FDefaultStyleCache::GetEditor().GetEditableTextStyle();
	}
#endif // WITH_EDITOR

	IsReadOnly = false;
	IsPassword = false;
	MinimumDesiredWidth = 0.0f;
	IsCaretMovedWhenGainFocus = true;
	SelectAllTextWhenFocused = false;
	RevertTextOnEscape = false;
	ClearKeyboardFocusOnCommit = true;
	SelectAllTextOnCommit = false;
	AllowContextMenu = true;
	EnableIntegratedKeyboard = false;
	VirtualKeyboardTrigger = EVirtualKeyboardTrigger::OnFocusByPointer;
	VirtualKeyboardDismissAction = EVirtualKeyboardDismissAction::TextChangeOnDismiss;
	SetClipping(EWidgetClipping::ClipToBounds);
	OverflowPolicy = ETextOverflowPolicy::Clip;
PRAGMA_ENABLE_DEPRECATION_WARNINGS

#if WITH_EDITORONLY_DATA
	AccessibleBehavior = ESlateAccessibleBehavior::Auto;
	bCanChildrenBeAccessible = false;
#endif
}

void UDlFmFloat_EditSimple::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);

	MyEditableText.Reset();
}

TSharedRef<SWidget> UDlFmFloat_EditSimple::RebuildWidget()
{
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	MyEditableText = SNew(SEditableText)
		.Style(&WidgetStyle)
		.IsReadOnly(IsReadOnly)
		.IsPassword(IsPassword)
		.MinDesiredWidth(MinimumDesiredWidth)
		.IsCaretMovedWhenGainFocus(IsCaretMovedWhenGainFocus)
		.SelectAllTextWhenFocused(SelectAllTextWhenFocused)
		.RevertTextOnEscape(RevertTextOnEscape)
		.ClearKeyboardFocusOnCommit(ClearKeyboardFocusOnCommit)
		.SelectAllTextOnCommit(SelectAllTextOnCommit)
		.OnTextChanged(BIND_UOBJECT_DELEGATE(FOnTextChanged, HandleOnTextChanged))
		.OnTextCommitted(BIND_UOBJECT_DELEGATE(FOnTextCommitted, HandleOnTextCommitted))
		.VirtualKeyboardType(EVirtualKeyboardType::AsKeyboardType(KeyboardType.GetValue()))
		.VirtualKeyboardOptions(VirtualKeyboardOptions)
		.VirtualKeyboardTrigger(VirtualKeyboardTrigger)
		.VirtualKeyboardDismissAction(VirtualKeyboardDismissAction)
		.Justification(Justification)
		.OverflowPolicy(OverflowPolicy)
		.EnableIntegratedKeyboard(EnableIntegratedKeyboard);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	
	return MyEditableText.ToSharedRef();
}

void UDlFmFloat_EditSimple::SynchronizeProperties()
{
	Super::SynchronizeProperties();

	if (!MyEditableText.IsValid())
	{
		return;
	}

	PRAGMA_DISABLE_DEPRECATION_WARNINGS

	TAttribute<FText> TextBinding = PROPERTY_BINDING(FText, Text);
	TAttribute<FText> HintTextBinding = PROPERTY_BINDING(FText, HintText);

	MyEditableText->SetText(TextBinding);
	MyEditableText->SetTextStyle(WidgetStyle);
	MyEditableText->SetHintText(HintTextBinding);
	MyEditableText->SetIsReadOnly(IsReadOnly);
	MyEditableText->SetIsPassword(IsPassword);
	MyEditableText->SetAllowContextMenu(AllowContextMenu);
	MyEditableText->SetVirtualKeyboardDismissAction(VirtualKeyboardDismissAction);
	MyEditableText->SetJustification(Justification);
	MyEditableText->SetOverflowPolicy(OverflowPolicy);
	MyEditableText->SetEnableIntegratedKeyboard(EnableIntegratedKeyboard);

	PRAGMA_ENABLE_DEPRECATION_WARNINGS

	// TODO UMG Complete making all properties settable on SEditableText

	ShapedTextOptions.SynchronizeShapedTextProperties(*MyEditableText);
}


PRAGMA_DISABLE_DEPRECATION_WARNINGS

float UDlFmFloat_EditSimple::GetValue() const//Dl+
{//Dl+
	//if ( MyEditableText.IsValid() )//Dl+
	//{//Dl+
	//	return FDlFmNumberFormat::Text2Float(MyEditableText->GetText());//Dl+
	//}//Dl+
	return CurValue;//Dl+
}//Dl+

void UDlFmFloat_EditSimple::SetValue(float InValue)//Dl+
{//Dl+
	if(CurValue==InValue){return;}//Dl+
	CurValue = InValue;//Dl+
	Text=FDlFmNumberFormat::Float2Text(CurValue,ValueFormat.GetOption());//Dl+
	if ( MyEditableText.IsValid() )//Dl+
	{
		MyEditableText->SetText(Text);//Dl+
	}//Dl+
}//Dl+

void UDlFmFloat_EditSimple::SetValue_NoBroadcast(float InValue)//Dl+
{//Dl+
	if(CurValue==InValue){return;}//Dl+
	CurValue = InValue;//Dl+
	Text = FDlFmNumberFormat::Float2Text(CurValue, ValueFormat.GetOption());//Dl+
	bBlockingNextBoardcast = true;//Dl+
	BlockingBoardcastContent = Text;//Dl+
	if (MyEditableText.IsValid())//Dl+
	{//Dl+
		MyEditableText->SetText(Text);//Dl+
	}//Dl+
}//Dl+

void UDlFmFloat_EditSimple::SetIsPassword(bool InbIsPassword)
{
	IsPassword = InbIsPassword;
	if ( MyEditableText.IsValid() )
	{
		MyEditableText->SetIsPassword(IsPassword);
	}
}

FText UDlFmFloat_EditSimple::GetHintText() const
{
	if (MyEditableText.IsValid())
	{
		return MyEditableText->GetHintText();
	}

	return HintText;
}

void UDlFmFloat_EditSimple::SetHintText(FText InHintText)
{
	HintText = InHintText;
	if ( MyEditableText.IsValid() )
	{
		MyEditableText->SetHintText(HintText);
	}
}

float UDlFmFloat_EditSimple::GetMinimumDesiredWidth() const
{
	return MinimumDesiredWidth;
}

void UDlFmFloat_EditSimple::SetMinimumDesiredWidth(float InMinDesiredWidth)
{
	MinimumDesiredWidth = InMinDesiredWidth;
	if (MyEditableText.IsValid())
	{
		MyEditableText->SetMinDesiredWidth(MinimumDesiredWidth);
	}
}

void UDlFmFloat_EditSimple::SetIsCaretMovedWhenGainFocus(bool bIsCaretMovedWhenGainFocus)
{
	IsCaretMovedWhenGainFocus = bIsCaretMovedWhenGainFocus;
	if (MyEditableText.IsValid())
	{
		MyEditableText->SetIsCaretMovedWhenGainFocus(bIsCaretMovedWhenGainFocus);
	}
}

bool UDlFmFloat_EditSimple::GetIsCaretMovedWhenGainFocus() const
{
	return IsCaretMovedWhenGainFocus;
}

void UDlFmFloat_EditSimple::SetSelectAllTextWhenFocused(bool bSelectAllTextWhenFocused)
{
	SelectAllTextWhenFocused = bSelectAllTextWhenFocused;
	if (MyEditableText.IsValid())
	{
		MyEditableText->SetSelectAllTextWhenFocused(bSelectAllTextWhenFocused);
	}
}

bool UDlFmFloat_EditSimple::GetSelectAllTextWhenFocused() const
{
	return SelectAllTextWhenFocused;
}

void UDlFmFloat_EditSimple::SetRevertTextOnEscape(bool bRevertTextOnEscape)
{
	RevertTextOnEscape = bRevertTextOnEscape;
	if (MyEditableText.IsValid())
	{
		MyEditableText->SetRevertTextOnEscape(bRevertTextOnEscape);
	}
}

bool UDlFmFloat_EditSimple::GetRevertTextOnEscape() const
{
	return RevertTextOnEscape;
}

bool UDlFmFloat_EditSimple::GetClearKeyboardFocusOnCommit() const
{
	return ClearKeyboardFocusOnCommit;
}

void UDlFmFloat_EditSimple::SetSelectAllTextOnCommit(bool bSelectAllTextOnCommit)
{
	SelectAllTextOnCommit = bSelectAllTextOnCommit;
	if (MyEditableText.IsValid())
	{
		MyEditableText->SetSelectAllTextOnCommit(bSelectAllTextOnCommit);
	}
}

bool UDlFmFloat_EditSimple::GetSelectAllTextOnCommit() const
{
	return SelectAllTextOnCommit;
}

void UDlFmFloat_EditSimple::SetWidgetStyle(const FEditableTextStyle& InEditableTextStyle)
{
	WidgetStyle = InEditableTextStyle;
	if (MyEditableText.IsValid())
	{
		MyEditableText->SetTextStyle(WidgetStyle);
	}
}

bool UDlFmFloat_EditSimple::GetIsReadOnly() const
{
	return IsReadOnly;
}

void UDlFmFloat_EditSimple::SetIsReadOnly(bool InbIsReadyOnly)
{
	IsReadOnly = InbIsReadyOnly;
	if ( MyEditableText.IsValid() )
	{
		MyEditableText->SetIsReadOnly(IsReadOnly);
	}
}


bool UDlFmFloat_EditSimple::GetIsPassword() const
{
	return IsPassword;
}


ETextJustify::Type UDlFmFloat_EditSimple::GetJustification() const
{
	return Justification;
}

void UDlFmFloat_EditSimple::SetJustification(ETextJustify::Type InJustification)
{
	Justification = InJustification;
	if (MyEditableText.IsValid())
	{
		MyEditableText->SetJustification(InJustification);
	}
}

ETextOverflowPolicy UDlFmFloat_EditSimple::GetTextOverflowPolicy() const
{
	return OverflowPolicy;
}

void UDlFmFloat_EditSimple::SetTextOverflowPolicy(ETextOverflowPolicy InOverflowPolicy)
{
	OverflowPolicy = InOverflowPolicy;
	if (MyEditableText.IsValid())
	{
		MyEditableText->SetOverflowPolicy(InOverflowPolicy);
	}
}

void UDlFmFloat_EditSimple::SetClearKeyboardFocusOnCommit(bool bClearKeyboardFocusOnCommit)
{
	ClearKeyboardFocusOnCommit = bClearKeyboardFocusOnCommit;
	if (MyEditableText.IsValid())
	{
		MyEditableText->SetClearKeyboardFocusOnCommit(ClearKeyboardFocusOnCommit);
	}
}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

void UDlFmFloat_EditSimple::SetKeyboardType(EVirtualKeyboardType::Type Type)
{
	KeyboardType = Type;
}

const FSlateFontInfo& UDlFmFloat_EditSimple::GetFont() const
{
	return WidgetStyle.Font;
}

void UDlFmFloat_EditSimple::SetFont(FSlateFontInfo InFontInfo)
{
	WidgetStyle.SetFont(InFontInfo);

	if (MyEditableText.IsValid())
	{
		MyEditableText->SetTextStyle(WidgetStyle);
	}
}

void UDlFmFloat_EditSimple::SetFontMaterial(UMaterialInterface* InMaterial)
{
	WidgetStyle.SetFontMaterial(InMaterial);

	if (MyEditableText.IsValid())
	{
		MyEditableText->SetTextStyle(WidgetStyle);
	}
}

void UDlFmFloat_EditSimple::SetFontOutlineMaterial(UMaterialInterface* InMaterial)
{
	WidgetStyle.SetFontOutlineMaterial(InMaterial);

	if (MyEditableText.IsValid())
	{
		MyEditableText->SetTextStyle(WidgetStyle);
	}
}

bool UDlFmFloat_EditSimple::GetEnableIntegratedKeyboard() const
{
	return EnableIntegratedKeyboard;
}

void UDlFmFloat_EditSimple::SetEnableIntegratedKeyboard(bool InEnableIntegratedKeyboard)
{
	EnableIntegratedKeyboard = InEnableIntegratedKeyboard;
	
	if (MyEditableText.IsValid())
	{
		MyEditableText->SetEnableIntegratedKeyboard(EnableIntegratedKeyboard);
	}
}

void UDlFmFloat_EditSimple::HandleOnTextChanged(const FText& InText)
{//Dl+
}//Dl+

void UDlFmFloat_EditSimple::HandleOnTextCommitted(const FText& InText, ETextCommit::Type CommitMethod)
{
	const float TmpV= FDlFmNumberFormat::Text2Float(InText);//Dl+
	if (bBlockingNextBoardcast) //Dl+
	{//Dl+
		if ( BlockingBoardcastContent.EqualTo(InText)) //Dl+
		{//Dl+
			bBlockingNextBoardcast = false;//Dl+
			return;//Dl+
		}//Dl+
	}//Dl+
	if(FMath::IsNearlyEqual(TmpV,CurValue,//Dl+
		FMath::Pow(0.1,static_cast<float>(ValueFormat.MaximumFractionalDigits)))){return;}//Dl+
	CurValue=TmpV;//Dl+
	Text=FDlFmNumberFormat::Float2Text(CurValue, ValueFormat.GetOption());//Dl+
	if ( MyEditableText.IsValid() )//Dl+
	{//Dl+
		MyEditableText->SetText(Text);//Dl+
	}
	OnValueCommitted.Broadcast(CurValue, CommitMethod);//Dl+
}

#if WITH_ACCESSIBILITY
TSharedPtr<SWidget> UDlFmFloat_EditSimple::GetAccessibleWidget() const
{
	return MyEditableText;
}
#endif

#if WITH_EDITOR

const FText UDlFmFloat_EditSimple::GetPaletteCategory()
{
	return LOCTEXT("Input", "Input");
}

#endif

void UDlFmFloat_EditSimple::ToggleVirtualKeyboard(bool bShow)
{
	if (MyEditableText.IsValid())
	{
		if (ULocalPlayer* LocalPlayer = GetOwningLocalPlayer())
		{
			const int32 UserIndex = FSlateApplication::Get().GetUserIndexForController(LocalPlayer->GetControllerId());
			MyEditableText->ToggleVirtualKeyboard(bShow, UserIndex);
		}
	}
}

/////////////////////////////////////////////////////

#undef LOCTEXT_NAMESPACE

