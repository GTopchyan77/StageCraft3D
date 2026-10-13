// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#include "DlFmBorderSlot.h"
#include "Widgets/SNullWidget.h"
#include "ModifyU/UI/SDlFmBorder.h"
#include "ModifyU/UI/DlFmBorder.h"
#include "ObjectEditorUtils.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(DlFmBorderSlot)

/////////////////////////////////////////////////////
// UDlFmBorderSlot

UDlFmBorderSlot::UDlFmBorderSlot(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	Padding = FMargin(4.f, 2.f);

	HorizontalAlignment = HAlign_Fill;
	VerticalAlignment = VAlign_Fill;
PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

void UDlFmBorderSlot::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);

	Border.Reset();
}

void UDlFmBorderSlot::BuildSlot(TSharedRef<SDlFmBorder> InBorder)
{
	Border = InBorder;

PRAGMA_DISABLE_DEPRECATION_WARNINGS
	InBorder->SetPadding(Padding);
	InBorder->SetHAlign(HorizontalAlignment);
	InBorder->SetVAlign(VerticalAlignment);
PRAGMA_ENABLE_DEPRECATION_WARNINGS

	InBorder->SetContent(Content ? Content->TakeWidget() : SNullWidget::NullWidget);
}

PRAGMA_DISABLE_DEPRECATION_WARNINGS
FMargin UDlFmBorderSlot::GetPadding() const
{
	return Padding;
}

void UDlFmBorderSlot::SetPadding(FMargin InPadding)
{
	Padding = InPadding;
	CastChecked<UDlFmBorder>(Parent)->SetPadding(InPadding);
}

EHorizontalAlignment UDlFmBorderSlot::GetHorizontalAlignment() const
{
	return HorizontalAlignment;
}

void UDlFmBorderSlot::SetHorizontalAlignment(EHorizontalAlignment InHorizontalAlignment)
{
	HorizontalAlignment = InHorizontalAlignment;
	CastChecked<UDlFmBorder>(Parent)->SetHorizontalAlignment(InHorizontalAlignment);
}

EVerticalAlignment UDlFmBorderSlot::GetVerticalAlignment() const
{
	return VerticalAlignment;
}

void UDlFmBorderSlot::SetVerticalAlignment(EVerticalAlignment InVerticalAlignment)
{
	VerticalAlignment = InVerticalAlignment;
	CastChecked<UDlFmBorder>(Parent)->SetVerticalAlignment(InVerticalAlignment);
}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

void UDlFmBorderSlot::SynchronizeProperties()
{
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	if ( Border.IsValid() )
	{
		SetPadding(Padding);
		SetHorizontalAlignment(HorizontalAlignment);
		SetVerticalAlignment(VerticalAlignment);
	}
PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

#if WITH_EDITOR

void UDlFmBorderSlot::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
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

			if ( UDlFmBorder* ParentBorder = CastChecked<UDlFmBorder>(Parent) )
			{
				if ( PropertyName == PaddingName)
				{
					FObjectEditorUtils::MigratePropertyValue(this, PaddingName, ParentBorder, PaddingName);
				}
				else if ( PropertyName == HorizontalAlignmentName)
				{
					FObjectEditorUtils::MigratePropertyValue(this, HorizontalAlignmentName, ParentBorder, HorizontalAlignmentName);
				}
				else if ( PropertyName == VerticalAlignmentName)
				{
					FObjectEditorUtils::MigratePropertyValue(this, VerticalAlignmentName, ParentBorder, VerticalAlignmentName);
				}
			}
		}

		IsReentrant = false;
	}
}

#endif

