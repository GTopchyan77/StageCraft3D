// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#include "ModifyU/UI/DlFmButtonSlot.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Input/SButton.h"
#include "Components/Widget.h"
#include "ModifyU/UI/SDlFmButton.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(DlFmButtonSlot)

/////////////////////////////////////////////////////
// UDlFmButtonSlot

UDlFmButtonSlot::UDlFmButtonSlot(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	Padding = FMargin(4.f, 2.f);

	HorizontalAlignment = HAlign_Center;
	VerticalAlignment = VAlign_Center;
PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

void UDlFmButtonSlot::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);

	Button.Reset();
}

void UDlFmButtonSlot::BuildSlot(TSharedRef<SDlFmButton> InButton)
{
	Button = InButton;

PRAGMA_DISABLE_DEPRECATION_WARNINGS
	InButton->SetContentPadding(Padding);
	InButton->SetHAlign(HorizontalAlignment);
	InButton->SetVAlign(VerticalAlignment);
PRAGMA_ENABLE_DEPRECATION_WARNINGS

	InButton->SetContent(Content ? Content->TakeWidget() : SNullWidget::NullWidget);
}

PRAGMA_DISABLE_DEPRECATION_WARNINGS
FMargin UDlFmButtonSlot::GetPadding() const
{
	return Padding;
}

void UDlFmButtonSlot::SetPadding(FMargin InPadding)
{
	Padding = InPadding;
	if (Button.IsValid())
	{
		Button.Pin()->SetContentPadding(InPadding);
	}
}

EHorizontalAlignment UDlFmButtonSlot::GetHorizontalAlignment() const
{
	return HorizontalAlignment;
}

void UDlFmButtonSlot::SetHorizontalAlignment(EHorizontalAlignment InHorizontalAlignment)
{
	HorizontalAlignment = InHorizontalAlignment;
	if (Button.IsValid())
	{
		Button.Pin()->SetHAlign(InHorizontalAlignment);
	}
}

EVerticalAlignment UDlFmButtonSlot::GetVerticalAlignment() const
{
	return VerticalAlignment;
}

void UDlFmButtonSlot::SetVerticalAlignment(EVerticalAlignment InVerticalAlignment)
{
	VerticalAlignment = InVerticalAlignment;
	if (Button.IsValid())
	{
		Button.Pin()->SetVAlign(InVerticalAlignment);
	}
}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

void UDlFmButtonSlot::SynchronizeProperties()
{
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	SetPadding(Padding);
	SetHorizontalAlignment(HorizontalAlignment);
	SetVerticalAlignment(VerticalAlignment);
PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

