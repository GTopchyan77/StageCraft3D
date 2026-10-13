// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/EditableText.h"
#include "UI/DlFmUICommon.h"
#include "DlFmInt_EditSimple.generated.h"

class SEditableText;
class UMaterialInterface;
class USlateBrushAsset;
class USlateWidgetStyleAsset;

/**
 * Editable text box widget
 */
UCLASS(MinimalAPI)
class UDlFmInt_EditSimple : public UWidget
{
	GENERATED_UCLASS_BODY()

public:

	//DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEditableTextChangedEvent, const FText&, Text);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnEditableTextCommittedEvent, int32, Value, ETextCommit::Type, CommitMethod);//Dl+

private://Dl+
	/** The text content for this editable text box widget */ //Dl+
	UPROPERTY()//Dl+
	FText Text = FText::FromString(TEXT("0"));//Dl+
public://Dl+
	UPROPERTY(EditAnywhere, Category=Content)//Dl+
	int32 CurValue;//Dl+
	UPROPERTY(EditAnywhere, Category=Content)//Dl+
	FDlFmNumberFormat ValueFormat;//Dl+

	/** A bindable delegate to allow logic to drive the text of the widget */
	UPROPERTY()
	FGetText TextDelegate;

public:

	UE_DEPRECATED(5.1, "Direct access to Hint Text is deprecated. Please use the getter or setter.")
	/** Hint text that appears when there is no text in the text box */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Getter, Setter, BlueprintGetter = "GetHintText", BlueprintSetter = "SetHintText", Category = Content, meta = (MultiLine = "true"))
	FText HintText;

	/** A bindable delegate to allow logic to drive the hint text of the widget */
	UPROPERTY()
	FGetText HintTextDelegate;

	/** The style */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Setter, Category=Appearance, meta=(DisplayName="Style", ShowOnlyInnerProperties))
	FEditableTextStyle WidgetStyle;

	UE_DEPRECATED(5.1, "Direct access to IsReadOnly is deprecated. Please use the getter or setter.")
	/** Sets whether this text box can actually be modified interactively by the user */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Getter, Setter, Category = Appearance)
	bool IsReadOnly;

	UE_DEPRECATED(5.1, "Direct access to IsPassword is deprecated. Please use the getter or setter.")
	/** Sets whether this text box is for storing a password */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Getter, Setter, BlueprintSetter = "SetIsPassword", Category = Appearance)
	bool IsPassword;

	UE_DEPRECATED(5.1, "Direct access to MinimumDesiredWidth is deprecated. Please use the getter or setter.")
	/** The minimum desired Width for the text */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Getter, Setter, BlueprintSetter = "SetMinimumDesiredWidth", Category = "Appearance")
	float MinimumDesiredWidth;

	UE_DEPRECATED(5.1, "Direct access to IsCaretMovedWhenGainFocus is deprecated. Please use the getter or setter.")
	/** When set to true the caret is moved when gaining focus */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Getter, Setter, Category = Behavior, AdvancedDisplay)
	bool IsCaretMovedWhenGainFocus;

	UE_DEPRECATED(5.1, "Direct access to SelectAllTextWhenFocused is deprecated. Please use the getter or setter.")
	/** Whether to select all text when the user clicks to give focus on the widget */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Getter, Setter, Category = Behavior, AdvancedDisplay)
	bool SelectAllTextWhenFocused;

	UE_DEPRECATED(5.1, "Direct access to RevertTextOnEscape is deprecated. Please use the getter or setter.")
	/** Whether to allow the user to back out of changes when they press the escape key */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Getter, Setter, Category = Behavior, AdvancedDisplay)
	bool RevertTextOnEscape;

	UE_DEPRECATED(5.1, "Direct access to ClearKeyboardFocusOnCommit is deprecated. Please use the getter or setter.")
	/** Whether to clear keyboard focus when pressing enter to commit changes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Getter, Setter, Category = Behavior, AdvancedDisplay)
	bool ClearKeyboardFocusOnCommit;

	UE_DEPRECATED(5.1, "Direct access to SelectAllTextOnCommit is deprecated. Please use the getter or setter.")
	/** Whether to select all text when pressing enter to commit changes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Getter, Setter, Category = Behavior, AdvancedDisplay)
	bool SelectAllTextOnCommit;

	/** Whether the context menu can be opened */
	UPROPERTY(EditAnywhere, Category=Behavior, AdvancedDisplay)
	bool AllowContextMenu;

	/** If we're on a platform that requires a virtual keyboard, what kind of keyboard should this widget use? */
	UPROPERTY(EditAnywhere, Category=Behavior, AdvancedDisplay)
	TEnumAsByte<EVirtualKeyboardType::Type> KeyboardType;

	/** Additional options for the virtual keyboard */
	UPROPERTY(EditAnywhere, Category=Behavior, AdvancedDisplay)
	FVirtualKeyboardOptions VirtualKeyboardOptions;

	UPROPERTY(EditAnywhere, Category = Behavior, AdvancedDisplay)
	EVirtualKeyboardTrigger VirtualKeyboardTrigger;

	/** What action should be taken when the virtual keyboard is dismissed? */
	UPROPERTY(EditAnywhere, Category=Behavior, AdvancedDisplay)
	EVirtualKeyboardDismissAction VirtualKeyboardDismissAction;
	
	UE_DEPRECATED(5.1, "Direct access to Justification is deprecated. Please use the getter or setter.")
	/** How the text should be aligned with the margin. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Getter, Setter, Blueprintgetter = GetJustification, BlueprintSetter = SetJustification, Category = Appearance)
	TEnumAsByte<ETextJustify::Type> Justification;

	UE_DEPRECATED(5.1, "Direct access to OverflowPolicy is deprecated. Please use the getter or setter.")
	/** Sets what happens to text that is clipped and doesn't fit within the clip rect for this widget */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Getter = "GetTextOverflowPolicy", Setter = "SetTextOverflowPolicy", BlueprintSetter = "SetTextOverflowPolicy", Category = "Clipping", AdvancedDisplay, meta = (DisplayName = "Overflow Policy"))
	ETextOverflowPolicy OverflowPolicy;

	/** Controls how the text within this widget should be shaped. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category=Localization, AdvancedDisplay, meta=(ShowOnlyInnerProperties))
	FShapedTextOptions ShapedTextOptions;

public:

	/** Called whenever the text is changed programmatically or interactively by the user */
	//UPROPERTY(BlueprintAssignable, Category="Widget Event", meta=(DisplayName="OnTextChanged (Editable Text)"))
	//FOnEditableTextChangedEvent OnTextChanged;

	/** Called whenever the text is committed.  This happens when the user presses enter or the text box loses focus. */
	UPROPERTY(BlueprintAssignable, Category="Widget Event", meta=(DisplayName="OnTextCommitted (Editable Int)"))//Dl+
	FOnEditableTextCommittedEvent OnValueCommitted;//Dl+

	/**
	 * Gets the widget text
	 * @return The widget text
	 */
	UFUNCTION(BlueprintCallable, Category = "Widget", meta=(DisplayName="GetText (Editable Text)"))
	DLFILEMANAGER_API int32 GetValue() const;//Dl+

	/**
	 * Directly sets the widget text.
	 * Warning: This will wipe any binding created for the Text property!
	 * @param InText The text to assign to the widget
	 */
	UFUNCTION(BlueprintCallable, Category = "Widget", meta=(DisplayName="SetValue (Editable Float)"))//Dl+
	DLFILEMANAGER_API void SetValue(int32 InValue);//Dl+
	UFUNCTION(BlueprintCallable, Category = "Widget", meta = (DisplayName = "SetValue (Editable Float)"))//Dl+
	DLFILEMANAGER_API void SetValue_NoBroadcast(int32 InValue);//Dl+

	DLFILEMANAGER_API bool GetIsPassword() const;

	UFUNCTION(BlueprintCallable, Category = "Widget")
	DLFILEMANAGER_API void SetIsPassword(UPARAM(DisplayName="IsPassword") bool InbIsPassword);

	/** Gets the Hint text that appears when there is no text in the text box */
	UFUNCTION(BlueprintCallable, Category = "Widget", meta = (DisplayName = "GetHintText (Editable Text)"))
	DLFILEMANAGER_API FText GetHintText() const;

	UFUNCTION(BlueprintCallable, Category = "Widget")
	DLFILEMANAGER_API void SetHintText(FText InHintText);

	/** @return the minimum desired width for this text box */
	DLFILEMANAGER_API float GetMinimumDesiredWidth() const;

	/**
	 *  Set the minimum desired width for this text box
	 *
	 *  @param InMinDesiredWidth new minimum desired width
	*/
	UFUNCTION(BlueprintCallable, Category = "Appearance")
	DLFILEMANAGER_API void SetMinimumDesiredWidth(float InMinDesiredWidth);

	/** When set to true the caret is moved when gaining focus */
	DLFILEMANAGER_API void SetIsCaretMovedWhenGainFocus(bool bIsCaretMovedWhenGainFocus);

	/** Return true when the caret is moved when gaining focus */
	DLFILEMANAGER_API bool GetIsCaretMovedWhenGainFocus() const;

	/** Set to true to select all text when the user clicks to give focus on the widget */
	DLFILEMANAGER_API void SetSelectAllTextWhenFocused(bool bSelectAllTextWhenFocused);

	/** Whether to select all text when the user clicks to give focus on the widget */
	DLFILEMANAGER_API bool GetSelectAllTextWhenFocused() const;

	/** Set to true to allow the user to back out of changes when they press the escape key */
	DLFILEMANAGER_API void SetRevertTextOnEscape(bool bRevertTextOnEscape);

	/** Whether to allow the user to back out of changes when they press the escape key  */
	DLFILEMANAGER_API bool GetRevertTextOnEscape() const;

	/** Whether to clear keyboard focus when pressing enter to commit changes */
	DLFILEMANAGER_API bool GetClearKeyboardFocusOnCommit() const;

	/** Set to true to select all text when pressing enter to commit changes */
	DLFILEMANAGER_API void SetSelectAllTextOnCommit(bool bSelectAllTextOnCommit);

	/** Whether to select all text when pressing enter to commit changes */
	DLFILEMANAGER_API bool GetSelectAllTextOnCommit() const;

	/** Set the style and ensure the visual will be updated accordingly */
	DLFILEMANAGER_API void SetWidgetStyle(const FEditableTextStyle& InEditableTextStyle);

	DLFILEMANAGER_API bool GetIsReadOnly() const;

	UFUNCTION(BlueprintCallable, Category = "Widget", meta=(DisplayName="SetIsReadOnly (Editable Text)"))
	DLFILEMANAGER_API void SetIsReadOnly(UPARAM(DisplayName="ReadyOnly") bool InbIsReadyOnly);

	UFUNCTION(BlueprintCallable, Category = "Widget")
	DLFILEMANAGER_API ETextJustify::Type GetJustification() const;

	UFUNCTION(BlueprintSetter)
	DLFILEMANAGER_API void SetJustification(ETextJustify::Type InJustification);

	/** @return the text overflow policy for this text block. */
	DLFILEMANAGER_API ETextOverflowPolicy GetTextOverflowPolicy() const;

	UFUNCTION(BlueprintSetter)
	DLFILEMANAGER_API void SetTextOverflowPolicy(ETextOverflowPolicy InOverflowPolicy);

	/** Set to true to clear keyboard focus when pressing enter to commit changes */
	DLFILEMANAGER_API void SetClearKeyboardFocusOnCommit(bool bInClearKeyboardFocusOnCommit);

	DLFILEMANAGER_API void SetKeyboardType(EVirtualKeyboardType::Type Type);

	UFUNCTION(BlueprintCallable, Category = "Appearance")
	DLFILEMANAGER_API const FSlateFontInfo& GetFont() const;

	UFUNCTION(BlueprintCallable, Category = "Appearance")
	DLFILEMANAGER_API void SetFont(FSlateFontInfo InFontInfo);

	UFUNCTION(BlueprintCallable, Category = "Appearance")
	DLFILEMANAGER_API void SetFontMaterial(UMaterialInterface* InMaterial);

	UFUNCTION(BlueprintCallable, Category = "Appearance")
	DLFILEMANAGER_API void SetFontOutlineMaterial(UMaterialInterface* InMaterial);

	DLFILEMANAGER_API bool GetEnableIntegratedKeyboard() const;
	DLFILEMANAGER_API void SetEnableIntegratedKeyboard(bool InEnableIntegratedKeyboard);

	/* Function to spawn virtual keyboard on demand */
	UFUNCTION(BlueprintCallable, Category = "VirtualKeyboard")
	DLFILEMANAGER_API void ToggleVirtualKeyboard(bool bShow); 

public:
	
	//~ Begin UWidget Interface
	DLFILEMANAGER_API virtual void SynchronizeProperties() override;
	//~ End UWidget Interface

	//~ Begin UVisual Interface
	DLFILEMANAGER_API virtual void ReleaseSlateResources(bool bReleaseChildren) override;
	//~ End UVisual Interface

#if WITH_EDITOR
	DLFILEMANAGER_API virtual const FText GetPaletteCategory() override;
#endif
	//~ End UWidget Interface

protected:
	//~ Begin UWidget Interface
	DLFILEMANAGER_API virtual TSharedRef<SWidget> RebuildWidget() override;
	// End of UWidget

	DLFILEMANAGER_API void HandleOnTextChanged(const FText& Text);
	DLFILEMANAGER_API void HandleOnTextCommitted(const FText& Text, ETextCommit::Type CommitMethod);

#if WITH_ACCESSIBILITY
	DLFILEMANAGER_API virtual TSharedPtr<SWidget> GetAccessibleWidget() const override;
#endif

protected:
	TSharedPtr<SEditableText> MyEditableText;

	PROPERTY_BINDING_IMPLEMENTATION(FText, Text);
	PROPERTY_BINDING_IMPLEMENTATION(FText, HintText);

private:
	/** @return true if the text was changed, or false if identical. */
	bool SetTextInternal(const FText& InText);
	
	/** Whether the integrated keyboard is enabled for this editable text. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Getter, Setter, Category = Mobile, AdvancedDisplay, meta = (AllowPrivateAccess = "true"))
	bool EnableIntegratedKeyboard;
	
	bool bBlockingNextBoardcast = false;//Dl+
	FText BlockingBoardcastContent;//Dl+
};
