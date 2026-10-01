// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageFaderBankWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/PanelWidget.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Interaction/StageParameterInterface.h"
#include "UI/StageEncoderWidget.h"
#include "UI/StageFaderWidget.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageFaderBankWidget)

#define LOCTEXT_NAMESPACE "StageCraftFaderBank"

namespace StageFaderBank
{
	const FLinearColor BarBackground(0.008f, 0.008f, 0.01f, 0.92f);
	const FLinearColor TitleColor(0.95f, 0.62f, 0.12f, 1.f);
	const FLinearColor HintColor(0.45f, 0.45f, 0.5f, 1.f);

	void StyleText(UTextBlock& Text, int32 Size, const FLinearColor& Color)
	{
		FSlateFontInfo Font = Text.GetFont();
		Font.Size = Size;
		Text.SetFont(Font);
		Text.SetColorAndOpacity(FSlateColor(Color));
	}
}

UStageFaderBankWidget::UStageFaderBankWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	FeatureGroups = {
		StageCraftTags::FeatureGroup_Dimmer,
		StageCraftTags::FeatureGroup_Color,
		StageCraftTags::FeatureGroup_Position,
		StageCraftTags::FeatureGroup_Beam,
		StageCraftTags::FeatureGroup_Audio,
	};
	EncoderParameters.AddTag(StageCraftTags::Attribute_Pan);
	EncoderParameters.AddTag(StageCraftTags::Attribute_Tilt);
	FaderWidgetClass = UStageFaderWidget::StaticClass();
	EncoderWidgetClass = UStageEncoderWidget::StaticClass();
}

void UStageFaderBankWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildDefaultTree();
	}
}

void UStageFaderBankWidget::BuildDefaultTree()
{
	using namespace StageFaderBank;

	UBorder* Background = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("BarBackground"));
	Background->SetBrushColor(BarBackground);
	Background->SetPadding(FMargin(8.f, 6.f));
	WidgetTree->RootWidget = Background;

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("BarColumn"));
	Background->SetContent(Column);

	TitleText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("TitleText"));
	StyleText(*TitleText, 9, TitleColor);
	Column->AddChildToVerticalBox(TitleText)->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));

	EmptyState = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("EmptyState"));
	StyleText(*EmptyState, 8, HintColor);
	Column->AddChildToVerticalBox(EmptyState);

	// Many parameters (a full moving head) scroll sideways instead of growing off screen.
	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("FaderScroll"));
	Scroll->SetOrientation(Orient_Horizontal);
	Column->AddChildToVerticalBox(Scroll);

	FaderContainer = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("FaderContainer"));
	Scroll->AddChild(FaderContainer);
}

void UStageFaderBankWidget::ClearView()
{
	if (FaderContainer)
	{
		FaderContainer->ClearChildren();
	}
}

void UStageFaderBankWidget::AddStrip(UStageParameterControlWidget& Control, float LeftPadding)
{
	UPanelSlot* StripSlot = FaderContainer->AddChild(&Control);
	if (UHorizontalBoxSlot* BoxSlot = Cast<UHorizontalBoxSlot>(StripSlot))
	{
		BoxSlot->SetPadding(FMargin(LeftPadding, 0.f, 1.f, 0.f));
		BoxSlot->SetVerticalAlignment(VAlign_Fill);
	}
}

void UStageFaderBankWidget::BuildView(UObject& Target, const TArray<FStageParameterSection>& Sections)
{
	if (!FaderContainer || !FaderWidgetClass)
	{
		return;
	}

	bool bFirstGroup = true;
	for (const FStageParameterSection& Section : Sections)
	{
		if (!FeatureGroups.IsEmpty() && !FeatureGroups.Contains(Section.FeatureGroup))
		{
			continue;
		}

		const FLinearColor SectionColor = GetGroupColor(Section.FeatureGroup);
		float LeftPadding = bFirstGroup ? 0.f : GroupSpacing;
		bool bAddedToGroup = false;

		auto Place = [&](UStageParameterControlWidget& Control)
		{
			AddStrip(Control, LeftPadding);
			RegisterControl(Control);
			LeftPadding = 0.f;
			bAddedToGroup = true;
		};

		for (const FStageParameterDescriptor& Descriptor : Section.Parameters)
		{
			if (Descriptor.bReadOnly)
			{
				continue;
			}

			switch (Descriptor.GetType())
			{
			case EStageParameterType::Float:
			case EStageParameterType::Integer:
				if (!Descriptor.HasRange())
				{
					break; // A fader needs end stops; unbounded values stay in the inspector.
				}
				if (EncoderWidgetClass && EncoderParameters.HasTagExact(Descriptor.Id))
				{
					UStageEncoderWidget* Encoder = CreateWidget<UStageEncoderWidget>(this, EncoderWidgetClass);
					Encoder->InitializeControl(Descriptor, SectionColor);
					Place(*Encoder);
				}
				else
				{
					UStageFaderWidget* Fader = CreateWidget<UStageFaderWidget>(this, FaderWidgetClass);
					Fader->InitializeControl(Descriptor, SectionColor);
					Place(*Fader);
				}
				break;

			case EStageParameterType::Bool:
			{
				UStageFaderWidget* Toggle = CreateWidget<UStageFaderWidget>(this, FaderWidgetClass);
				Toggle->InitializeControl(Descriptor, SectionColor);
				Place(*Toggle);
				break;
			}

			case EStageParameterType::Color:
				for (int32 Channel = 0; Channel < 3; ++Channel)
				{
					UStageFaderWidget* ChannelFader = CreateWidget<UStageFaderWidget>(this, FaderWidgetClass);
					ChannelFader->SetColorComponent(Channel);
					ChannelFader->InitializeControl(Descriptor, SectionColor);
					Place(*ChannelFader);
				}
				break;

			default:
				break;
			}
		}

		if (bAddedToGroup)
		{
			bFirstGroup = false;
		}
	}
}

void UStageFaderBankWidget::OnViewUpdated(UObject* Target)
{
	if (EmptyState)
	{
		const bool bEmpty = GetNumControls() == 0;
		EmptyState->SetVisibility(bEmpty ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		EmptyState->SetText(Target
			? LOCTEXT("NothingToControl", "No live parameters on this item.")
			: LOCTEXT("SelectSomething", "Select a fixture or speaker to get faders."));
	}
	RefreshTitle(Target);
}

void UStageFaderBankWidget::OnParameterRefreshed(const FGameplayTag& ParameterId, const FStageParameterValue& Value)
{
	if (ParameterId == StageCraftTags::Param_Info_Label)
	{
		RefreshTitle(GetInspectedObject());
	}
}

void UStageFaderBankWidget::RefreshTitle(UObject* Target)
{
	if (!TitleText)
	{
		return;
	}

	FStageParameterValue Label;
	if (Target && IStageParameterInterface::Execute_GetParameterValue(Target, StageCraftTags::Param_Info_Label, Label))
	{
		TitleText->SetText(FText::Format(LOCTEXT("TitleFormat", "FADERS  |  {0}"), Label.Text));
	}
	else
	{
		TitleText->SetText(LOCTEXT("TitleEmpty", "FADERS"));
	}
}

#undef LOCTEXT_NAMESPACE
