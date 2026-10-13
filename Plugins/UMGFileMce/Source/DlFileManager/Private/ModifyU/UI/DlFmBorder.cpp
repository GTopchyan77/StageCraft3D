// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#include "ModifyU/UI/DlFmBorder.h"
#include "Slate/SlateBrushAsset.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Types/ReflectionMetadata.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "ModifyU/UI/SDlFmBorder.h"
#include "ModifyU/UI/DlFmBorderSlot.h"
#include "ObjectEditorUtils.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(DlFmBorder)

#define LOCTEXT_NAMESPACE "UMG"

/////////////////////////////////////////////////////
// UDlFmBorder

UDlFmBorder::UDlFmBorder(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bIsVariable = false;

PRAGMA_DISABLE_DEPRECATION_WARNINGS
	ContentColorAndOpacity = FLinearColor::White;
	BrushColor = FLinearColor::White;

	Padding = FMargin(4, 2);

	HorizontalAlignment = HAlign_Fill;
	VerticalAlignment = VAlign_Fill;

	DesiredSizeScale = FVector2D(1, 1);

	bShowEffectWhenDisabled = true;
PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

void UDlFmBorder::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);

	MyBorder.Reset();
}

TSharedRef<SWidget> UDlFmBorder::RebuildWidget()
{
	MyBorder = SNew(SDlFmBorder)
		.FlipForRightToLeftFlowDirection(bFlipForRightToLeftFlowDirection)
		.Paint_OffsetPercent(Paint_OffsetPercent)//Dl+
		.Paint_OverflowPercent(Paint_OverflowPercent);//Dl+;
	
	if ( GetChildrenCount() > 0 )
	{
		Cast<UDlFmBorderSlot>(GetContentSlot())->BuildSlot(MyBorder.ToSharedRef());
	}

	return MyBorder.ToSharedRef();
}

void UDlFmBorder::SynchronizeProperties()
{
	Super::SynchronizeProperties();

	if (!MyBorder.IsValid())
	{
		return;
	}
	
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	TAttribute<FLinearColor> ContentColorAndOpacityBinding = PROPERTY_BINDING(FLinearColor, ContentColorAndOpacity);
	TAttribute<FSlateColor> BrushColorBinding = OPTIONAL_BINDING_CONVERT(FLinearColor, BrushColor, FSlateColor, ConvertLinearColorToSlateColor);
	TAttribute<const FSlateBrush*> ImageBinding = OPTIONAL_BINDING_CONVERT(FSlateBrush, Background, const FSlateBrush*, ConvertImage);
	
	MyBorder->SetPadding(Padding);
	MyBorder->SetBorderBackgroundColor(BrushColorBinding);
	MyBorder->SetColorAndOpacity(ContentColorAndOpacityBinding);

	MyBorder->SetBorderImage(ImageBinding);
	
	MyBorder->SetDesiredSizeScale(DesiredSizeScale);
	MyBorder->SetShowEffectWhenDisabled(bShowEffectWhenDisabled != 0);
PRAGMA_ENABLE_DEPRECATION_WARNINGS

	MyBorder->SetOnMouseButtonDown(BIND_UOBJECT_DELEGATE(FPointerEventHandler, HandleMouseButtonDown));
	MyBorder->SetOnMouseButtonUp(BIND_UOBJECT_DELEGATE(FPointerEventHandler, HandleMouseButtonUp));
	MyBorder->SetOnMouseMove(BIND_UOBJECT_DELEGATE(FPointerEventHandler, HandleMouseMove));
	MyBorder->SetOnMouseDoubleClick(BIND_UOBJECT_DELEGATE(FPointerEventHandler, HandleMouseDoubleClick));
}

UClass* UDlFmBorder::GetSlotClass() const
{
	return UDlFmBorderSlot::StaticClass();
}

void UDlFmBorder::OnSlotAdded(UPanelSlot* InSlot)
{
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	// Copy the content properties into the new slot so that it matches what has been setup
	// so far by the user.
	UDlFmBorderSlot* BorderSlot = CastChecked<UDlFmBorderSlot>(InSlot);
	BorderSlot->Padding = Padding;
	BorderSlot->HorizontalAlignment = HorizontalAlignment;
	BorderSlot->VerticalAlignment = VerticalAlignment;
PRAGMA_ENABLE_DEPRECATION_WARNINGS

	// Add the child to the live slot if it already exists
	if ( MyBorder.IsValid() )
	{
		// Construct the underlying slot.
		BorderSlot->BuildSlot(MyBorder.ToSharedRef());
	}
}

void UDlFmBorder::OnSlotRemoved(UPanelSlot* InSlot)
{
	// Remove the widget from the live slot if it exists.
	if ( MyBorder.IsValid() )
	{
		MyBorder->SetContent(SNullWidget::NullWidget);
	}
}

PRAGMA_DISABLE_DEPRECATION_WARNINGS
FLinearColor UDlFmBorder::GetContentColorAndOpacity() const
{
	if (MyBorder.IsValid())
	{
		return MyBorder->GetColorAndOpacity();
	}
	return ContentColorAndOpacity;
}

void UDlFmBorder::SetContentColorAndOpacity(FLinearColor Color)
{
	ContentColorAndOpacity = Color;
	if ( MyBorder.IsValid() )
	{
		MyBorder->SetColorAndOpacity(Color);
	}
}

FMargin UDlFmBorder::GetPadding() const
{
	return Padding;
}

void UDlFmBorder::SetPadding(FMargin InPadding)
{
	Padding = InPadding;
	if ( MyBorder.IsValid() )
	{
		MyBorder->SetPadding(InPadding);
	}
}

EHorizontalAlignment UDlFmBorder::GetHorizontalAlignment() const
{
	return HorizontalAlignment;
}

void UDlFmBorder::SetHorizontalAlignment(EHorizontalAlignment InHorizontalAlignment)
{
	HorizontalAlignment = InHorizontalAlignment;
	if (MyBorder.IsValid())
	{
		MyBorder->SetHAlign(InHorizontalAlignment);
	}
}

EVerticalAlignment UDlFmBorder::GetVerticalAlignment() const
{
	return VerticalAlignment;
}

void UDlFmBorder::SetVerticalAlignment(EVerticalAlignment InVerticalAlignment)
{
	VerticalAlignment = InVerticalAlignment;
	if ( MyBorder.IsValid() )
	{
		MyBorder->SetVAlign(InVerticalAlignment);
	}
}

FLinearColor UDlFmBorder::GetBrushColor() const
{
	return BrushColor;
}

void UDlFmBorder::SetBrushColor(FLinearColor Color)
{
	BrushColor = Color;
	if ( MyBorder.IsValid() )
	{
		MyBorder->SetBorderBackgroundColor(Color);
	}
}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

FReply UDlFmBorder::HandleMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& MouseEvent)
{
	if ( OnMouseButtonDownEvent.IsBound() )
	{
		return OnMouseButtonDownEvent.Execute(Geometry, MouseEvent).NativeReply;
	}

	return FReply::Unhandled();
}

FReply UDlFmBorder::HandleMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& MouseEvent)
{
	if ( OnMouseButtonUpEvent.IsBound() )
	{
		return OnMouseButtonUpEvent.Execute(Geometry, MouseEvent).NativeReply;
	}

	return FReply::Unhandled();
}

FReply UDlFmBorder::HandleMouseMove(const FGeometry& Geometry, const FPointerEvent& MouseEvent)
{
	if ( OnMouseMoveEvent.IsBound() )
	{
		return OnMouseMoveEvent.Execute(Geometry, MouseEvent).NativeReply;
	}

	return FReply::Unhandled();
}

FReply UDlFmBorder::HandleMouseDoubleClick(const FGeometry& Geometry, const FPointerEvent& MouseEvent)
{
	if ( OnMouseDoubleClickEvent.IsBound() )
	{
		return OnMouseDoubleClickEvent.Execute(Geometry, MouseEvent).NativeReply;
	}

	return FReply::Unhandled();
}

void UDlFmBorder::SetBrush(const FSlateBrush& Brush)
{
	Background = Brush;

	if ( MyBorder.IsValid() )
	{
		MyBorder->SetBorderImage(&Background);
	}
}

void UDlFmBorder::SetBrushFromAsset(USlateBrushAsset* Asset)
{
	Background = Asset ? Asset->Brush : FSlateBrush();

	if ( MyBorder.IsValid() )
	{
		MyBorder->SetBorderImage(&Background);
	}
}

void UDlFmBorder::SetBrushFromTexture(UTexture2D* Texture)
{
	Background.SetResourceObject(Texture);

	if ( MyBorder.IsValid() )
	{
		MyBorder->SetBorderImage(&Background);
	}
}

void UDlFmBorder::SetBrushFromMaterial(UMaterialInterface* Material)
{
	if (!Material)
	{
		UE_LOG(LogSlate, Log, TEXT("UDlFmBorder::SetBrushFromMaterial. Incoming material is null. %s"), *GetPathName());
	}

	Background.SetResourceObject(Material);

	//TODO UMG Check if the material can be used with the UI

	if ( MyBorder.IsValid() )
	{
		MyBorder->SetBorderImage(&Background);
	}
}

PRAGMA_DISABLE_DEPRECATION_WARNINGS
bool UDlFmBorder::GetShowEffectWhenDisabled() const
{
	return bShowEffectWhenDisabled;
}

void UDlFmBorder::SetShowEffectWhenDisabled(bool bInShowEffectWhenDisabled)
{
	bShowEffectWhenDisabled = bInShowEffectWhenDisabled;
	if(MyBorder)
	{
		MyBorder->SetShowEffectWhenDisabled(bShowEffectWhenDisabled != 0);
	}
}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

UMaterialInstanceDynamic* UDlFmBorder::GetDynamicMaterial()
{
	UMaterialInterface* Material = nullptr;

	UObject* Resource = Background.GetResourceObject();
	Material = Cast<UMaterialInterface>(Resource);

	if ( Material )
	{
		UMaterialInstanceDynamic* DynamicMaterial = Cast<UMaterialInstanceDynamic>(Material);

		if ( !DynamicMaterial )
		{
			DynamicMaterial = UMaterialInstanceDynamic::Create(Material, this);
			Background.SetResourceObject(DynamicMaterial);

			if ( MyBorder.IsValid() )
			{
				MyBorder->SetBorderImage(&Background);
			}
		}

		return DynamicMaterial;
	}

	//TODO UMG can we do something for textures?  General purpose dynamic material for them?
	return nullptr;
}

PRAGMA_DISABLE_DEPRECATION_WARNINGS
FVector2D UDlFmBorder::GetDesiredSizeScale() const
{
	return DesiredSizeScale;
}

void UDlFmBorder::SetDesiredSizeScale(FVector2D InScale)
{
	DesiredSizeScale = InScale;
	if (MyBorder.IsValid())
	{
		MyBorder->SetDesiredSizeScale(InScale);
	}
}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

const FSlateBrush* UDlFmBorder::ConvertImage(TAttribute<FSlateBrush> InImageAsset) const
{
	UDlFmBorder* MutableThis = const_cast<UDlFmBorder*>( this );
	MutableThis->Background = InImageAsset.Get();

	return &Background;
}

void UDlFmBorder::PostLoad()
{
	Super::PostLoad();

	if ( GetChildrenCount() > 0 )
	{
		//TODO UMG Pre-Release Upgrade, now have slots of their own.  Convert existing slot to new slot.
		if ( UPanelSlot* PanelSlot = GetContentSlot() )
		{
			UDlFmBorderSlot* BorderSlot = Cast<UDlFmBorderSlot>(PanelSlot);
			if ( BorderSlot == NULL )
			{
				BorderSlot = NewObject<UDlFmBorderSlot>(this);
				BorderSlot->Content = GetContentSlot()->Content;
				BorderSlot->Content->Slot = BorderSlot;
				Slots[0] = BorderSlot;
			}
		}
	}
}

#if WITH_EDITOR

void UDlFmBorder::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	static bool IsReentrant = false;

	if ( !IsReentrant )
	{
		IsReentrant = true;

		if ( PropertyChangedEvent.Property )
		{
			static const FName PaddingName("Padding");
			static const FName HorizontalAlignmentName("HorizontalAlignment");
			static const FName VerticalAlignmentName("VerticalAlignment");

			FName PropertyName = PropertyChangedEvent.Property->GetFName();

			if ( UDlFmBorderSlot* BorderSlot = Cast<UDlFmBorderSlot>(GetContentSlot()) )
			{
				if (PropertyName == PaddingName)
				{
					FObjectEditorUtils::MigratePropertyValue(this, PaddingName, BorderSlot, PaddingName);
				}
				else if (PropertyName == HorizontalAlignmentName)
				{
					FObjectEditorUtils::MigratePropertyValue(this, HorizontalAlignmentName, BorderSlot, HorizontalAlignmentName);
				}
				else if (PropertyName == VerticalAlignmentName)
				{
					FObjectEditorUtils::MigratePropertyValue(this, VerticalAlignmentName, BorderSlot, VerticalAlignmentName);
				}
			}
		}

		IsReentrant = false;
	}
}

const FText UDlFmBorder::GetPaletteCategory()
{
	return LOCTEXT("Common", "Common");
}

#endif

/////////////////////////////////////////////////////

#undef LOCTEXT_NAMESPACE

