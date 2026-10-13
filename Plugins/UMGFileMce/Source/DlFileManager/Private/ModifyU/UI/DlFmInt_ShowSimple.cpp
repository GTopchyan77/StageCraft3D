// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#include "ModifyU/UI/DlFmInt_ShowSimple.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/Font.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SInvalidationPanel.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Images/SImage.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(DlFmInt_ShowSimple)

#define LOCTEXT_NAMESPACE "UMG"

/////////////////////////////////////////////////////
// UDlFmInt_ShowSimple

UDlFmInt_ShowSimple::UDlFmInt_ShowSimple(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bIsVariable = false;
	bWrapWithInvalidationPanel = false;
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	ShadowOffset = FVector2D(1.0f, 1.0f);
	ColorAndOpacity = FLinearColor::White;
	ShadowColorAndOpacity = FLinearColor::Transparent;
	TextTransformPolicy = ETextTransformPolicy::None;
	TextOverflowPolicy = ETextOverflowPolicy::Clip;

	if (!IsRunningDedicatedServer())
	{
		static ConstructorHelpers::FObjectFinder<UFont> RobotoFontObj(*UWidget::GetDefaultFontName());
		Font = FSlateFontInfo(RobotoFontObj.Object, 12, FName("Regular"));//Dl+
	}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

#if WITH_EDITORONLY_DATA
	AccessibleBehavior = ESlateAccessibleBehavior::Auto;
	bCanChildrenBeAccessible = false;
#endif
	UDlFmInt_ShowSimple::SetJustification(ETextJustify::Type::Right);//Dl+
}

void UDlFmInt_ShowSimple::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);

	MyTextBlock.Reset();
}

PRAGMA_DISABLE_DEPRECATION_WARNINGS
FSlateColor UDlFmInt_ShowSimple::GetColorAndOpacity() const
{
	if (ColorAndOpacityDelegate.IsBound() && !IsDesignTime())
	{
		return ColorAndOpacityDelegate.Execute();
	}
	else
	{
		return ColorAndOpacity;
	}
}

void UDlFmInt_ShowSimple::SetColorAndOpacity(FSlateColor InColorAndOpacity)
{
	ColorAndOpacity = InColorAndOpacity;
	if( MyTextBlock.IsValid() )
	{
		MyTextBlock->SetColorAndOpacity( InColorAndOpacity );
	}
}

void UDlFmInt_ShowSimple::SetOpacity(float InOpacity)
{
	FLinearColor CurrentColor = ColorAndOpacity.GetSpecifiedColor();
	CurrentColor.A = InOpacity;
	
	SetColorAndOpacity(FSlateColor(CurrentColor));
}

FLinearColor UDlFmInt_ShowSimple::GetShadowColorAndOpacity() const
{
	if (ShadowColorAndOpacityDelegate.IsBound() && !IsDesignTime())
	{
		return ShadowColorAndOpacityDelegate.Execute();
	}
	else
	{
		return ShadowColorAndOpacity;
	}
}

void UDlFmInt_ShowSimple::SetShadowColorAndOpacity(FLinearColor InShadowColorAndOpacity)
{
	ShadowColorAndOpacity = InShadowColorAndOpacity;
	if( MyTextBlock.IsValid() )
	{
		MyTextBlock->SetShadowColorAndOpacity(InShadowColorAndOpacity);
	}
}

FVector2D UDlFmInt_ShowSimple::GetShadowOffset() const
{
	return ShadowOffset;
}

void UDlFmInt_ShowSimple::SetShadowOffset(FVector2D InShadowOffset)
{
	ShadowOffset = InShadowOffset;
	if( MyTextBlock.IsValid() )
	{
		MyTextBlock->SetShadowOffset(ShadowOffset);
	}
}

const FSlateFontInfo& UDlFmInt_ShowSimple::GetFont() const
{
	return Font;
}

void UDlFmInt_ShowSimple::SetFont(FSlateFontInfo InFontInfo)
{
	Font = InFontInfo;
	if (MyTextBlock.IsValid())
	{
		MyTextBlock->SetFont(Font);
	}
	OnFontChanged();
}

const FSlateBrush& UDlFmInt_ShowSimple::GetStrikeBrush() const
{
	return StrikeBrush;
}

void UDlFmInt_ShowSimple::SetStrikeBrush(FSlateBrush InStrikeBrush)
{
	StrikeBrush = InStrikeBrush;
	if (MyTextBlock.IsValid())
	{
		MyTextBlock->SetStrikeBrush(&StrikeBrush);
	}
}

void UDlFmInt_ShowSimple::OnShapedTextOptionsChanged(FShapedTextOptions InShapedTextOptions)
{
	Super::OnShapedTextOptionsChanged(InShapedTextOptions);
	if (MyTextBlock.IsValid())
	{
		InShapedTextOptions.SynchronizeShapedTextProperties(*MyTextBlock);
	}
}

void UDlFmInt_ShowSimple::OnJustificationChanged(ETextJustify::Type InJustification)
{
	Super::OnJustificationChanged(InJustification);
	if (MyTextBlock.IsValid())
	{
		MyTextBlock->SetJustification(InJustification);
	}
}

void UDlFmInt_ShowSimple::OnWrappingPolicyChanged(ETextWrappingPolicy InWrappingPolicy)
{
	Super::OnWrappingPolicyChanged(InWrappingPolicy);
	if (MyTextBlock.IsValid())
	{
		MyTextBlock->SetWrappingPolicy(InWrappingPolicy);
	}
}

void UDlFmInt_ShowSimple::OnAutoWrapTextChanged(bool InAutoWrapText)
{
	Super::OnAutoWrapTextChanged(InAutoWrapText);
	if (MyTextBlock.IsValid())
	{
		MyTextBlock->SetAutoWrapText(InAutoWrapText);
	}
}

void UDlFmInt_ShowSimple::OnWrapTextAtChanged(float InWrapTextAt)
{
	Super::OnWrapTextAtChanged(InWrapTextAt);
	if (MyTextBlock.IsValid())
	{
		MyTextBlock->SetWrapTextAt(InWrapTextAt);
	}
}

void UDlFmInt_ShowSimple::OnLineHeightPercentageChanged(float InLineHeightPercentage)
{
	Super::OnLineHeightPercentageChanged(InLineHeightPercentage);
	if (MyTextBlock.IsValid())
	{
		MyTextBlock->SetLineHeightPercentage(InLineHeightPercentage);
	}
}

void UDlFmInt_ShowSimple::OnApplyLineHeightToBottomLineChanged(bool InApplyLineHeightToBottomLine)
{
	Super::OnApplyLineHeightToBottomLineChanged(InApplyLineHeightToBottomLine);
	if (MyTextBlock.IsValid())
	{
		MyTextBlock->SetApplyLineHeightToBottomLine(InApplyLineHeightToBottomLine);
	}
}

void UDlFmInt_ShowSimple::OnMarginChanged(const FMargin& InMargin)
{
	Super::OnMarginChanged(InMargin);
	if (MyTextBlock.IsValid())
	{
		MyTextBlock->SetMargin(InMargin);
	}
}

float UDlFmInt_ShowSimple::GetMinDesiredWidth() const
{
	return MinDesiredWidth;
}

void UDlFmInt_ShowSimple::SetMinDesiredWidth(float InMinDesiredWidth)
{
	MinDesiredWidth = InMinDesiredWidth;
	if (MyTextBlock.IsValid())
	{
		MyTextBlock->SetMinDesiredWidth(MinDesiredWidth);
	}
}

void UDlFmInt_ShowSimple::SetAutoWrapText(bool InAutoWrapText)
{
	AutoWrapText = InAutoWrapText;
	if(MyTextBlock.IsValid())
	{
		MyTextBlock->SetAutoWrapText(InAutoWrapText);
	}
}

ETextTransformPolicy UDlFmInt_ShowSimple::GetTextTransformPolicy() const
{
	return TextTransformPolicy;
}

void UDlFmInt_ShowSimple::SetTextTransformPolicy(ETextTransformPolicy InTransformPolicy)
{
	TextTransformPolicy = InTransformPolicy;
	if(MyTextBlock.IsValid())
	{
		MyTextBlock->SetTransformPolicy(TextTransformPolicy);
	}
}

ETextOverflowPolicy UDlFmInt_ShowSimple::GetTextOverflowPolicy() const
{
	return TextOverflowPolicy;
}

void UDlFmInt_ShowSimple::SetTextOverflowPolicy(ETextOverflowPolicy InOverflowPolicy)
{
	TextOverflowPolicy = InOverflowPolicy;
	SynchronizeProperties();
}

void UDlFmInt_ShowSimple::SetFontMaterial(UMaterialInterface* InMaterial)
{
	Font.FontMaterial = InMaterial;
	SetFont(Font);
}

void UDlFmInt_ShowSimple::SetFontOutlineMaterial(UMaterialInterface* InMaterial)
{
	Font.OutlineSettings.OutlineMaterial = InMaterial;
	SetFont(Font);
}

UMaterialInstanceDynamic* UDlFmInt_ShowSimple::GetDynamicFontMaterial()
{
	if (Font.FontMaterial)
	{
		UMaterialInterface* Material = CastChecked<UMaterialInterface>(Font.FontMaterial);

		UMaterialInstanceDynamic* DynamicMaterial = Cast<UMaterialInstanceDynamic>(Material);

		if (!DynamicMaterial)
		{
			DynamicMaterial = UMaterialInstanceDynamic::Create(Material, this);
			Font.FontMaterial = DynamicMaterial;

			SetFont(Font);
		}

		return DynamicMaterial;
	}

	return nullptr;
}

UMaterialInstanceDynamic* UDlFmInt_ShowSimple::GetDynamicOutlineMaterial()
{
	if (Font.OutlineSettings.OutlineMaterial)
	{
		UMaterialInterface* Material = CastChecked<UMaterialInterface>(Font.OutlineSettings.OutlineMaterial);

		UMaterialInstanceDynamic* DynamicMaterial = Cast<UMaterialInstanceDynamic>(Material);

		if (!DynamicMaterial)
		{
			DynamicMaterial = UMaterialInstanceDynamic::Create(Material, this);
			Font.OutlineSettings.OutlineMaterial = DynamicMaterial;

			SetFont(Font);
		}

		return DynamicMaterial;
	}

	return nullptr;
}
PRAGMA_ENABLE_DEPRECATION_WARNINGS


TSharedRef<SWidget> UDlFmInt_ShowSimple::RebuildWidget()
{
 	if (bWrapWithInvalidationPanel && !IsDesignTime())
 	{
 		TSharedPtr<SWidget> RetWidget = SNew(SInvalidationPanel)
 		[
 			SAssignNew(MyTextBlock, STextBlock)
			.SimpleTextMode(bSimpleTextMode)
 		];
 		return RetWidget.ToSharedRef();
 	}
 	else
	{
		MyTextBlock =
			SNew(STextBlock)
			.SimpleTextMode(bSimpleTextMode);

		//if (IsDesignTime())
		//{
		//	return SNew(SOverlay)

		//	+ SOverlay::Slot()
		//	[
		//		MyTextBlock.ToSharedRef()
		//	]

		//	+ SOverlay::Slot()
		//	.VAlign(VAlign_Top)
		//	.HAlign(HAlign_Right)
		//	[
		//		SNew(SImage)
		//		.Image(FCoreStyle::Get().GetBrush("Icons.Warning"))
		//		.Visibility_UObject(this, &ThisClass::GetTextWarningImageVisibility)
		//		.ToolTipText(LOCTEXT("TextNotLocalizedWarningToolTip", "This text is marked as 'culture invariant' and won't be gathered for localization.\nYou can change this by editing the advanced text settings."))
		//	];
		//}
		
		return MyTextBlock.ToSharedRef();
	}
}

PRAGMA_DISABLE_DEPRECATION_WARNINGS
EVisibility UDlFmInt_ShowSimple::GetTextWarningImageVisibility() const
{
	return Text.IsCultureInvariant() ? EVisibility::Visible : EVisibility::Collapsed;
}

#if WITH_ACCESSIBILITY
TSharedPtr<SWidget> UDlFmInt_ShowSimple::GetAccessibleWidget() const
{
	return MyTextBlock;
}
#endif

void UDlFmInt_ShowSimple::OnBindingChanged(const FName& Property)
{
	Super::OnBindingChanged(Property);

	if ( MyTextBlock.IsValid() )
	{
		static const FName TextProperty(TEXT("TextDelegate"));
		static const FName ColorAndOpacityProperty(TEXT("ColorAndOpacityDelegate"));
		static const FName ShadowColorAndOpacityProperty(TEXT("ShadowColorAndOpacityDelegate"));

		if ( Property == TextProperty )
		{
			TAttribute<FText> TextBinding = GetDisplayText();
			MyTextBlock->SetText(TextBinding);
		}
		else if ( Property == ColorAndOpacityProperty )
		{
			TAttribute<FSlateColor> ColorAndOpacityBinding = PROPERTY_BINDING(FSlateColor, ColorAndOpacity);
			MyTextBlock->SetColorAndOpacity(ColorAndOpacityBinding);
		}
		else if ( Property == ShadowColorAndOpacityProperty )
		{
			TAttribute<FLinearColor> ShadowColorAndOpacityBinding = PROPERTY_BINDING(FLinearColor, ShadowColorAndOpacity);
			MyTextBlock->SetShadowColorAndOpacity(ShadowColorAndOpacityBinding);
		}
	}
}

void UDlFmInt_ShowSimple::SynchronizeProperties()
{
	Super::SynchronizeProperties();

	TAttribute<FText> TextBinding = GetDisplayText();
	TAttribute<FSlateColor> ColorAndOpacityBinding = PROPERTY_BINDING(FSlateColor, ColorAndOpacity);
	TAttribute<FLinearColor> ShadowColorAndOpacityBinding = PROPERTY_BINDING(FLinearColor, ShadowColorAndOpacity);

	if ( MyTextBlock.IsValid() )
	{
		MyTextBlock->SetText( TextBinding );
		MyTextBlock->SetFont( Font );
		MyTextBlock->SetStrikeBrush( &StrikeBrush );
		MyTextBlock->SetColorAndOpacity( ColorAndOpacityBinding );
		MyTextBlock->SetShadowOffset( ShadowOffset );
		MyTextBlock->SetShadowColorAndOpacity( ShadowColorAndOpacityBinding );
		MyTextBlock->SetMinDesiredWidth( MinDesiredWidth );
		MyTextBlock->SetTransformPolicy( TextTransformPolicy );
		MyTextBlock->SetOverflowPolicy(TextOverflowPolicy);

		Super::SynchronizeTextLayoutProperties( *MyTextBlock );
	}
}

/// @cond DOXYGEN_WARNINGS

int32 UDlFmInt_ShowSimple::GetValue() const
{
	if (MyTextBlock.IsValid())
	{
		return FDlFmNumberFormat::Text2Int(MyTextBlock->GetText());
	}

	return CurValue;
}

/// @endcond

void UDlFmInt_ShowSimple::SetValue(int32 InValue)
{
	if(CurValue==InValue){return;}//Dl+
	CurValue = InValue;//Dl+
	Text=FDlFmNumberFormat::Int2Text(CurValue,ValueFormat.GetOption());//Dl+
	if ( MyTextBlock.IsValid() )
	{
		MyTextBlock->SetText(Text);
	}
	OnTextChanged();
}

TAttribute<FText> UDlFmInt_ShowSimple::GetDisplayText()
{
	return PROPERTY_BINDING(FText, Text);
}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

#if WITH_EDITOR

FString UDlFmInt_ShowSimple::GetLabelMetadata() const
{
	const int32 MaxSampleLength = 15;

	FString TextStr = Text.ToString().Replace(TEXT("\n"), TEXT(" "));
	TextStr = TextStr.Len() <= MaxSampleLength ? TextStr : TextStr.Left(MaxSampleLength - 2) + TEXT("..");
	return TEXT(" \"") + TextStr + TEXT("\"");
}

void UDlFmInt_ShowSimple::HandleTextCommitted(const FText& InText, ETextCommit::Type CommitteType)
{
	//TODO UMG How will this migrate to the template?  Seems to me we need the previews to have access to their templates!
	//TODO UMG How will the user click the editable area?  There is an overlay blocking input so that other widgets don't get them.
	//     Need a way to recognize one particular widget and forward things to them!
}

const FText UDlFmInt_ShowSimple::GetPaletteCategory()
{
	return LOCTEXT("Common", "Common");
}

void UDlFmInt_ShowSimple::OnCreationFromPalette()
{
	SetValue(0);
}

bool UDlFmInt_ShowSimple::CanEditChange(const FProperty* InProperty) const
{
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	if (bSimpleTextMode && InProperty)
	{
		static TArray<FName> InvalidPropertiesInSimpleMode =
		{
			GET_MEMBER_NAME_CHECKED(UDlFmInt_ShowSimple, ShapedTextOptions),
			GET_MEMBER_NAME_CHECKED(UDlFmInt_ShowSimple, Justification),
			GET_MEMBER_NAME_CHECKED(UDlFmInt_ShowSimple, WrappingPolicy),
			GET_MEMBER_NAME_CHECKED(UDlFmInt_ShowSimple, AutoWrapText),
			GET_MEMBER_NAME_CHECKED(UDlFmInt_ShowSimple, WrapTextAt),
			GET_MEMBER_NAME_CHECKED(UDlFmInt_ShowSimple, Margin),
			GET_MEMBER_NAME_CHECKED(UDlFmInt_ShowSimple, LineHeightPercentage),
			GET_MEMBER_NAME_CHECKED(UDlFmInt_ShowSimple, AutoWrapText),
		};

		return !InvalidPropertiesInSimpleMode.Contains(InProperty->GetFName());
	}

	return Super::CanEditChange(InProperty);
PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

#endif //if WITH_EDITOR

/////////////////////////////////////////////////////

#undef LOCTEXT_NAMESPACE

