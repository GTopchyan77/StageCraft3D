// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#include "ModifyU/UI/DlFmFloat_ShowSimple.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/Font.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SInvalidationPanel.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Images/SImage.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(DlFmFloat_ShowSimple)

#define LOCTEXT_NAMESPACE "UMG"

/////////////////////////////////////////////////////
// UDlFmFloat_ShowSimple

UDlFmFloat_ShowSimple::UDlFmFloat_ShowSimple(const FObjectInitializer& ObjectInitializer)
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
	UDlFmFloat_ShowSimple::SetJustification(ETextJustify::Type::Right);//Dl+
}

void UDlFmFloat_ShowSimple::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);

	MyTextBlock.Reset();
}

PRAGMA_DISABLE_DEPRECATION_WARNINGS
FSlateColor UDlFmFloat_ShowSimple::GetColorAndOpacity() const
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

void UDlFmFloat_ShowSimple::SetColorAndOpacity(FSlateColor InColorAndOpacity)
{
	ColorAndOpacity = InColorAndOpacity;
	if( MyTextBlock.IsValid() )
	{
		MyTextBlock->SetColorAndOpacity( InColorAndOpacity );
	}
}

void UDlFmFloat_ShowSimple::SetOpacity(float InOpacity)
{
	FLinearColor CurrentColor = ColorAndOpacity.GetSpecifiedColor();
	CurrentColor.A = InOpacity;
	
	SetColorAndOpacity(FSlateColor(CurrentColor));
}

FLinearColor UDlFmFloat_ShowSimple::GetShadowColorAndOpacity() const
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

void UDlFmFloat_ShowSimple::SetShadowColorAndOpacity(FLinearColor InShadowColorAndOpacity)
{
	ShadowColorAndOpacity = InShadowColorAndOpacity;
	if( MyTextBlock.IsValid() )
	{
		MyTextBlock->SetShadowColorAndOpacity(InShadowColorAndOpacity);
	}
}

FVector2D UDlFmFloat_ShowSimple::GetShadowOffset() const
{
	return ShadowOffset;
}

void UDlFmFloat_ShowSimple::SetShadowOffset(FVector2D InShadowOffset)
{
	ShadowOffset = InShadowOffset;
	if( MyTextBlock.IsValid() )
	{
		MyTextBlock->SetShadowOffset(ShadowOffset);
	}
}

const FSlateFontInfo& UDlFmFloat_ShowSimple::GetFont() const
{
	return Font;
}

void UDlFmFloat_ShowSimple::SetFont(FSlateFontInfo InFontInfo)
{
	Font = InFontInfo;
	if (MyTextBlock.IsValid())
	{
		MyTextBlock->SetFont(Font);
	}
	OnFontChanged();
}

const FSlateBrush& UDlFmFloat_ShowSimple::GetStrikeBrush() const
{
	return StrikeBrush;
}

void UDlFmFloat_ShowSimple::SetStrikeBrush(FSlateBrush InStrikeBrush)
{
	StrikeBrush = InStrikeBrush;
	if (MyTextBlock.IsValid())
	{
		MyTextBlock->SetStrikeBrush(&StrikeBrush);
	}
}

void UDlFmFloat_ShowSimple::OnShapedTextOptionsChanged(FShapedTextOptions InShapedTextOptions)
{
	Super::OnShapedTextOptionsChanged(InShapedTextOptions);
	if (MyTextBlock.IsValid())
	{
		InShapedTextOptions.SynchronizeShapedTextProperties(*MyTextBlock);
	}
}

void UDlFmFloat_ShowSimple::OnJustificationChanged(ETextJustify::Type InJustification)
{
	Super::OnJustificationChanged(InJustification);
	if (MyTextBlock.IsValid())
	{
		MyTextBlock->SetJustification(InJustification);
	}
}

void UDlFmFloat_ShowSimple::OnWrappingPolicyChanged(ETextWrappingPolicy InWrappingPolicy)
{
	Super::OnWrappingPolicyChanged(InWrappingPolicy);
	if (MyTextBlock.IsValid())
	{
		MyTextBlock->SetWrappingPolicy(InWrappingPolicy);
	}
}

void UDlFmFloat_ShowSimple::OnAutoWrapTextChanged(bool InAutoWrapText)
{
	Super::OnAutoWrapTextChanged(InAutoWrapText);
	if (MyTextBlock.IsValid())
	{
		MyTextBlock->SetAutoWrapText(InAutoWrapText);
	}
}

void UDlFmFloat_ShowSimple::OnWrapTextAtChanged(float InWrapTextAt)
{
	Super::OnWrapTextAtChanged(InWrapTextAt);
	if (MyTextBlock.IsValid())
	{
		MyTextBlock->SetWrapTextAt(InWrapTextAt);
	}
}

void UDlFmFloat_ShowSimple::OnLineHeightPercentageChanged(float InLineHeightPercentage)
{
	Super::OnLineHeightPercentageChanged(InLineHeightPercentage);
	if (MyTextBlock.IsValid())
	{
		MyTextBlock->SetLineHeightPercentage(InLineHeightPercentage);
	}
}

void UDlFmFloat_ShowSimple::OnApplyLineHeightToBottomLineChanged(bool InApplyLineHeightToBottomLine)
{
	Super::OnApplyLineHeightToBottomLineChanged(InApplyLineHeightToBottomLine);
	if (MyTextBlock.IsValid())
	{
		MyTextBlock->SetApplyLineHeightToBottomLine(InApplyLineHeightToBottomLine);
	}
}

void UDlFmFloat_ShowSimple::OnMarginChanged(const FMargin& InMargin)
{
	Super::OnMarginChanged(InMargin);
	if (MyTextBlock.IsValid())
	{
		MyTextBlock->SetMargin(InMargin);
	}
}

float UDlFmFloat_ShowSimple::GetMinDesiredWidth() const
{
	return MinDesiredWidth;
}

void UDlFmFloat_ShowSimple::SetMinDesiredWidth(float InMinDesiredWidth)
{
	MinDesiredWidth = InMinDesiredWidth;
	if (MyTextBlock.IsValid())
	{
		MyTextBlock->SetMinDesiredWidth(MinDesiredWidth);
	}
}

void UDlFmFloat_ShowSimple::SetAutoWrapText(bool InAutoWrapText)
{
	AutoWrapText = InAutoWrapText;
	if(MyTextBlock.IsValid())
	{
		MyTextBlock->SetAutoWrapText(InAutoWrapText);
	}
}

ETextTransformPolicy UDlFmFloat_ShowSimple::GetTextTransformPolicy() const
{
	return TextTransformPolicy;
}

void UDlFmFloat_ShowSimple::SetTextTransformPolicy(ETextTransformPolicy InTransformPolicy)
{
	TextTransformPolicy = InTransformPolicy;
	if(MyTextBlock.IsValid())
	{
		MyTextBlock->SetTransformPolicy(TextTransformPolicy);
	}
}

ETextOverflowPolicy UDlFmFloat_ShowSimple::GetTextOverflowPolicy() const
{
	return TextOverflowPolicy;
}

void UDlFmFloat_ShowSimple::SetTextOverflowPolicy(ETextOverflowPolicy InOverflowPolicy)
{
	TextOverflowPolicy = InOverflowPolicy;
	SynchronizeProperties();
}

void UDlFmFloat_ShowSimple::SetFontMaterial(UMaterialInterface* InMaterial)
{
	Font.FontMaterial = InMaterial;
	SetFont(Font);
}

void UDlFmFloat_ShowSimple::SetFontOutlineMaterial(UMaterialInterface* InMaterial)
{
	Font.OutlineSettings.OutlineMaterial = InMaterial;
	SetFont(Font);
}

UMaterialInstanceDynamic* UDlFmFloat_ShowSimple::GetDynamicFontMaterial()
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

UMaterialInstanceDynamic* UDlFmFloat_ShowSimple::GetDynamicOutlineMaterial()
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


TSharedRef<SWidget> UDlFmFloat_ShowSimple::RebuildWidget()
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
EVisibility UDlFmFloat_ShowSimple::GetTextWarningImageVisibility() const
{
	return Text.IsCultureInvariant() ? EVisibility::Visible : EVisibility::Collapsed;
}

#if WITH_ACCESSIBILITY
TSharedPtr<SWidget> UDlFmFloat_ShowSimple::GetAccessibleWidget() const
{
	return MyTextBlock;
}
#endif

void UDlFmFloat_ShowSimple::OnBindingChanged(const FName& Property)
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

void UDlFmFloat_ShowSimple::SynchronizeProperties()
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

float UDlFmFloat_ShowSimple::GetValue() const
{
	if (MyTextBlock.IsValid())
	{
		return FDlFmNumberFormat::Text2Float(MyTextBlock->GetText());
	}

	return CurValue;
}

/// @endcond

void UDlFmFloat_ShowSimple::SetValue(float InValue)
{
	if((CurValue==InValue)&&(FMath::IsNaN(CurValue)==FMath::IsNaN(InValue))){return;}//Dl+
	CurValue = InValue;//Dl+
	Text=FDlFmNumberFormat::Float2Text(CurValue,ValueFormat.GetOption());//Dl+
	if ( MyTextBlock.IsValid() )
	{
		MyTextBlock->SetText(Text);
	}
	OnTextChanged();
}

TAttribute<FText> UDlFmFloat_ShowSimple::GetDisplayText()
{
	return PROPERTY_BINDING(FText, Text);
}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

#if WITH_EDITOR

FString UDlFmFloat_ShowSimple::GetLabelMetadata() const
{
	const int32 MaxSampleLength = 15;

	FString TextStr = Text.ToString().Replace(TEXT("\n"), TEXT(" "));
	TextStr = TextStr.Len() <= MaxSampleLength ? TextStr : TextStr.Left(MaxSampleLength - 2) + TEXT("..");
	return TEXT(" \"") + TextStr + TEXT("\"");
}

void UDlFmFloat_ShowSimple::HandleTextCommitted(const FText& InText, ETextCommit::Type CommitteType)
{
	//TODO UMG How will this migrate to the template?  Seems to me we need the previews to have access to their templates!
	//TODO UMG How will the user click the editable area?  There is an overlay blocking input so that other widgets don't get them.
	//     Need a way to recognize one particular widget and forward things to them!
}

const FText UDlFmFloat_ShowSimple::GetPaletteCategory()
{
	return LOCTEXT("Common", "Common");
}

void UDlFmFloat_ShowSimple::OnCreationFromPalette()
{
	SetValue(0);
}

bool UDlFmFloat_ShowSimple::CanEditChange(const FProperty* InProperty) const
{
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	if (bSimpleTextMode && InProperty)
	{
		static TArray<FName> InvalidPropertiesInSimpleMode =
		{
			GET_MEMBER_NAME_CHECKED(UDlFmFloat_ShowSimple, ShapedTextOptions),
			GET_MEMBER_NAME_CHECKED(UDlFmFloat_ShowSimple, Justification),
			GET_MEMBER_NAME_CHECKED(UDlFmFloat_ShowSimple, WrappingPolicy),
			GET_MEMBER_NAME_CHECKED(UDlFmFloat_ShowSimple, AutoWrapText),
			GET_MEMBER_NAME_CHECKED(UDlFmFloat_ShowSimple, WrapTextAt),
			GET_MEMBER_NAME_CHECKED(UDlFmFloat_ShowSimple, Margin),
			GET_MEMBER_NAME_CHECKED(UDlFmFloat_ShowSimple, LineHeightPercentage),
			GET_MEMBER_NAME_CHECKED(UDlFmFloat_ShowSimple, AutoWrapText),
		};

		return !InvalidPropertiesInSimpleMode.Contains(InProperty->GetFName());
	}

	return Super::CanEditChange(InProperty);
PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

#endif //if WITH_EDITOR

/////////////////////////////////////////////////////

#undef LOCTEXT_NAMESPACE

