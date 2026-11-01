// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageMarketplaceWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/ButtonSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/ScaleBox.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/SpinBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "Data/BaseItemData.h"
#include "Engine/GameInstance.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Marketplace/StageMarketplacePreview.h"
#include "Marketplace/StagePackage.h"
#include "Player/ModularPlayerController.h"
#include "Scene/StageSceneComponent.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Subsystems/StageProfileSubsystem.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageMarketplacePreviewWidget.h"
#include "UI/StageThemedComboBox.h"
#include "UI/StageToolButton.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageMarketplaceWidget)

#define LOCTEXT_NAMESPACE "StageMarketplaceWidget"

namespace StageMarketplacePanel
{
	/** Cards per page: short enough that a page needs no list virtualization; the assistant counts results on the same page. */
	constexpr int32 PageSize = FStageMarketplaceQuery::BrowsePageSize;
	constexpr float GroupGap = 14.f;
	constexpr float FilterColumnWidth = 420.f;
	/** Degrees per dragged pixel. */
	constexpr float OrbitYawPerPixel = 0.4f;
	constexpr float OrbitPitchPerPixel = 0.3f;

	/** Kind chips: ALL, then each kind. */
	constexpr EStagePackageKind Kinds[] = { EStagePackageKind::StageDesign, EStagePackageKind::LightingRig, EStagePackageKind::CueList };

	const TArray<FGameplayTag>& Categories()
	{
		static const TArray<FGameplayTag> Tags = {
			StageCraftTags::Marketplace_Category_Concert, StageCraftTags::Marketplace_Category_Festival, StageCraftTags::Marketplace_Category_Theatre,
			StageCraftTags::Marketplace_Category_Corporate, StageCraftTags::Marketplace_Category_Club, StageCraftTags::Marketplace_Category_Broadcast,
			StageCraftTags::Marketplace_Category_Worship, StageCraftTags::Marketplace_Category_Other };
		return Tags;
	}

	FText CategoryName(const FGameplayTag& Category)
	{
		const FString Name = Category.GetTagName().ToString();
		int32 Dot = INDEX_NONE;
		return FText::FromString(Name.FindLastChar(TEXT('.'), Dot) ? Name.Mid(Dot + 1) : Name);
	}

	TArray<FText> CategoryNames()
	{
		TArray<FText> Names;
		for (const FGameplayTag& Category : Categories())
		{
			Names.Add(CategoryName(Category));
		}
		return Names;
	}

	FText KindName(EStagePackageKind Kind)
	{
		switch (Kind)
		{
		case EStagePackageKind::LightingRig:	return LOCTEXT("KindRig", "Lighting rig");
		case EStagePackageKind::CueList:		return LOCTEXT("KindCues", "Cue list");
		default:								return LOCTEXT("KindStage", "Stage design");
		}
	}

	constexpr EStageMarketplaceSort Sorts[] = { EStageMarketplaceSort::Popular, EStageMarketplaceSort::Newest, EStageMarketplaceSort::PriceLowToHigh,
		EStageMarketplaceSort::PriceHighToLow, EStageMarketplaceSort::Title };

	void SetLabel(UStageChoiceButton* Button, const FText& Label)
	{
		if (UTextBlock* Text = Button ? Cast<UTextBlock>(Button->GetContent()) : nullptr)
		{
			Text->SetText(Label);
		}
	}
}

// --- Construction ---

const UStageCraftUITheme& UStageMarketplaceWidget::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

AModularPlayerController* UStageMarketplaceWidget::GetStageController() const
{
	return Cast<AModularPlayerController>(GetOwningPlayer());
}

void UStageMarketplaceWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	Query.Limit = StageMarketplacePanel::PageSize;
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

UTextBlock* UStageMarketplaceWidget::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

UStageChoiceButton* UStageMarketplaceWidget::MakeButton(const FText& Label, const FText& Tip, int32 Index, void (UStageMarketplaceWidget::*Handler)(int32))
{
	UStageChoiceButton* Button = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
	Button->SetChoiceIndex(Index);
	Button->SetToolTipText(Tip);
	Button->OnPicked.BindUObject(this, Handler);
	UTextBlock* Text = MakeText(Label, 8, TEXT("Bold"), GetTheme().TextSecondary);
	Text->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(Text)))
	{
		LabelSlot->SetPadding(FMargin(8.f, 3.f));
	}
	StyleChoice(*Button, false);
	return Button;
}

UEditableTextBox* UStageMarketplaceWidget::MakeTextField(const FText& Hint)
{
	UEditableTextBox* Field = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass());
	Field->SetWidgetStyle(StageCraftWidgetStyle::MakeInputField(GetTheme()));
	Field->SetHintText(Hint);
	Field->SetRevertTextOnEscape(true);
	return Field;
}

UStageThemedComboBox* UStageMarketplaceWidget::MakeCombo(TConstArrayView<FText> Options)
{
	UStageThemedComboBox* Combo = WidgetTree->ConstructWidget<UStageThemedComboBox>(UStageThemedComboBox::StaticClass());
	Combo->ApplyTheme(GetTheme());
	for (const FText& Option : Options)
	{
		Combo->AddOption(Option.ToString());
	}
	Combo->SetSelectedIndex(0);
	return Combo;
}

UWidget* UStageMarketplaceWidget::BuildRow(TConstArrayView<UWidget*> Children, float Gap)
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	for (int32 Index = 0; Index < Children.Num(); ++Index)
	{
		if (UHorizontalBoxSlot* ChildSlot = Row->AddChildToHorizontalBox(Children[Index]))
		{
			ChildSlot->SetVerticalAlignment(VAlign_Center);
			ChildSlot->SetPadding(FMargin(Index == 0 ? 0.f : Gap, 0.f, 0.f, 0.f));
		}
	}
	return Row;
}

UWidget* UStageMarketplaceWidget::BuildLabeled(const FText& Label, UWidget* Field)
{
	UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Box->AddChildToVerticalBox(MakeText(Label, 7, TEXT("Bold"), GetTheme().TextSecondary))->SetPadding(FMargin(0.f, 0.f, 0.f, 2.f));
	Box->AddChildToVerticalBox(Field);
	return Box;
}

UWidget* UStageMarketplaceWidget::BuildSegment(TConstArrayView<FText> Labels, TArray<TObjectPtr<UStageChoiceButton>>& OutButtons, void (UStageMarketplaceWidget::*Handler)(int32))
{
	OutButtons.Reset();
	TArray<UWidget*> Buttons;
	for (int32 Index = 0; Index < Labels.Num(); ++Index)
	{
		UStageChoiceButton* Button = MakeButton(Labels[Index], FText::GetEmpty(), Index, Handler);
		OutButtons.Add(Button);
		Buttons.Add(Button);
	}
	return BuildRow(Buttons, 3.f);
}

void UStageMarketplaceWidget::StyleChoice(UStageChoiceButton& Button, bool bSelected) const
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	FButtonStyle Style = bSelected
		? MakeFlatButton(Palette.AccentColor, Palette.AccentColor * 1.1f, Palette.AccentColor * 0.85f)
		: MakeFlatButton(FLinearColor::Transparent, Palette.RowHover, Palette.Divider);
	if (!bSelected)
	{
		Style.SetNormal(MakeBox(FLinearColor::Transparent, CornerRadius, Palette.Divider, 1.f));
	}
	Button.SetStyle(Style);
	if (UTextBlock* Label = Cast<UTextBlock>(Button.GetContent()))
	{
		Label->SetColorAndOpacity(FSlateColor(bSelected ? Palette.WindowBackground : Palette.TextSecondary));
	}
}

void UStageMarketplaceWidget::StyleCard(UStageChoiceButton& Card, bool bSelected) const
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	FButtonStyle Style = MakeFlatButton(Palette.RowBackground, Palette.RowHover, Palette.RowHover);
	Style.SetNormal(MakeBox(bSelected ? Palette.RowHover : Palette.RowBackground, CornerRadius, bSelected ? Palette.AccentColor : Palette.Divider, 1.f));
	Style.SetHovered(MakeBox(Palette.RowHover, CornerRadius, bSelected ? Palette.AccentColor : Palette.Divider, 1.f));
	Card.SetStyle(Style);
}

void UStageMarketplaceWidget::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Root"));
	Root->SetBrush(MakeBox(Palette.PanelBackground));
	Root->SetPadding(FMargin(0.f));
	WidgetTree->RootWidget = Root;

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Column"));
	Root->SetContent(Column);
	Column->AddChildToVerticalBox(BuildHeader());

	Pages = WidgetTree->ConstructWidget<UWidgetSwitcher>(UWidgetSwitcher::StaticClass(), TEXT("Pages"));
	Pages->AddChild(BuildBrowsePage());
	Pages->AddChild(BuildPublishPage());
	if (UVerticalBoxSlot* PagesSlot = Column->AddChildToVerticalBox(Pages))
	{
		PagesSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		PagesSlot->SetPadding(FMargin(10.f, 8.f));
	}
	HandlePagesPicked(0);
}

UWidget* UStageMarketplaceWidget::BuildHeader()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Header = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Header"));
	Header->SetBrush(MakeBox(Palette.PanelHeader));
	Header->SetPadding(FMargin(10.f, 6.f));

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Header->SetContent(Row);
	Row->AddChildToHorizontalBox(MakeText(LOCTEXT("Title", "COMMUNITY MARKETPLACE"), 10, TEXT("Bold"), Palette.AccentColor))->SetVerticalAlignment(VAlign_Center);
	UWidget* PageSegment = BuildSegment({ LOCTEXT("PageBrowse", "BROWSE"), LOCTEXT("PagePublish", "PUBLISH") }, PageButtons, &ThisClass::HandlePagesPicked);
	if (UHorizontalBoxSlot* PageSlot = Row->AddChildToHorizontalBox(PageSegment))
	{
		PageSlot->SetPadding(FMargin(StageMarketplacePanel::GroupGap, 0.f, 0.f, 0.f));
		PageSlot->SetVerticalAlignment(VAlign_Center);
	}
	CreditsText = MakeText(FText::GetEmpty(), 9, TEXT("Bold"), Palette.TextPrimary);
	if (UHorizontalBoxSlot* CreditsSlot = Row->AddChildToHorizontalBox(CreditsText))
	{
		CreditsSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		CreditsSlot->SetHorizontalAlignment(HAlign_Right);
		CreditsSlot->SetVerticalAlignment(VAlign_Center);
	}
	return Header;
}

UWidget* UStageMarketplaceWidget::BuildBrowsePage()
{
	UHorizontalBox* Page = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("BrowsePage"));
	USizeBox* FilterBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	FilterBox->SetWidthOverride(StageMarketplacePanel::FilterColumnWidth);
	FilterBox->SetContent(BuildFilterColumn());
	Page->AddChildToHorizontalBox(FilterBox)->SetVerticalAlignment(VAlign_Fill);
	if (UHorizontalBoxSlot* DetailSlot = Page->AddChildToHorizontalBox(BuildDetailColumn()))
	{
		DetailSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		DetailSlot->SetPadding(FMargin(StageMarketplacePanel::GroupGap, 0.f, 0.f, 0.f));
	}
	return Page;
}

UWidget* UStageMarketplaceWidget::BuildFilterColumn()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("FilterColumn"));
	const auto Add = [Column](UWidget* Widget) { Column->AddChildToVerticalBox(Widget)->SetPadding(FMargin(0.f, 3.f)); };

	SearchField = MakeTextField(LOCTEXT("SearchHint", "Search titles, publishers, keywords (Enter)"));
	SearchField->SetClearKeyboardFocusOnCommit(false);
	Add(SearchField);

	Add(BuildRow({ MakeText(LOCTEXT("Kind", "KIND"), 7, TEXT("Bold"), Palette.TextSecondary),
		BuildSegment({ LOCTEXT("KindAll", "ALL"), LOCTEXT("KindStageChip", "STAGE"), LOCTEXT("KindRigChip", "RIG"), LOCTEXT("KindCuesChip", "CUES") }, KindButtons, &ThisClass::HandleKindPicked) }));
	Add(BuildRow({ MakeText(LOCTEXT("Price", "PRICE"), 7, TEXT("Bold"), Palette.TextSecondary),
		BuildSegment({ LOCTEXT("PriceAny", "ANY"), LOCTEXT("PriceFree", "FREE"), LOCTEXT("PricePaid", "PAID") }, PriceButtons, &ThisClass::HandlePricePicked) }));

	TArray<FText> CategoryOptions = { LOCTEXT("AllCategories", "All categories") };
	CategoryOptions.Append(StageMarketplacePanel::CategoryNames());
	CategoryCombo = MakeCombo(CategoryOptions);
	SortCombo = MakeCombo({ LOCTEXT("SortPopular", "Most downloaded"), LOCTEXT("SortNewest", "Newest"), LOCTEXT("SortPriceUp", "Price: low to high"),
		LOCTEXT("SortPriceDown", "Price: high to low"), LOCTEXT("SortTitle", "Title") });
	UHorizontalBox* Combos = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Combos->AddChildToHorizontalBox(BuildLabeled(LOCTEXT("Category", "CATEGORY"), CategoryCombo))->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	if (UHorizontalBoxSlot* SortSlot = Combos->AddChildToHorizontalBox(BuildLabeled(LOCTEXT("Sort", "SORT"), SortCombo)))
	{
		SortSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		SortSlot->SetPadding(FMargin(6.f, 0.f, 0.f, 0.f));
	}
	Add(Combos);

	ResultsText = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), Palette.TextSecondary);
	UHorizontalBox* Paging = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Paging->AddChildToHorizontalBox(ResultsText)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	Paging->AddChildToHorizontalBox(BuildSegment({ LOCTEXT("Prev", "PREV"), LOCTEXT("Next", "NEXT") }, PagingButtons, &ThisClass::HandlePagePicked));
	Add(Paging);

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("CardScroll"));
	Scroll->SetWidgetBarStyle(MakeScrollBar(Palette));
	Scroll->SetScrollbarThickness(FVector2D(6.f, 6.f));
	CardList = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("CardList"));
	Scroll->AddChild(CardList);
	if (UVerticalBoxSlot* ScrollSlot = Column->AddChildToVerticalBox(Scroll))
	{
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ScrollSlot->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
	}
	return Column;
}

UWidget* UStageMarketplaceWidget::BuildDetailColumn()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("DetailColumn"));
	const auto Add = [Column](UWidget* Widget) { Column->AddChildToVerticalBox(Widget)->SetPadding(FMargin(0.f, 3.f)); };

	// The preview: a 16:9 image that scales to the column, framed like the stage viewport.
	PreviewSurface = WidgetTree->ConstructWidget<UStageMarketplacePreviewWidget>(UStageMarketplacePreviewWidget::StaticClass(), TEXT("PreviewSurface"));
	PreviewSurface->OnOrbit.BindUObject(this, &ThisClass::HandlePreviewOrbit);
	PreviewSurface->OnZoom.BindUObject(this, &ThisClass::HandlePreviewZoom);
	USizeBox* Aspect = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Aspect->SetWidthOverride(FStageMarketplacePreview::Width / 2.f);
	Aspect->SetHeightOverride(FStageMarketplacePreview::Height / 2.f);
	Aspect->SetContent(PreviewSurface);
	UScaleBox* Scale = WidgetTree->ConstructWidget<UScaleBox>(UScaleBox::StaticClass());
	Scale->SetStretch(EStretch::ScaleToFit);
	Scale->SetContent(Aspect);
	UBorder* Frame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("PreviewFrame"));
	Frame->SetBrush(MakeBox(Palette.WindowBackground, 0.f, Palette.Divider, 1.f));
	Frame->SetPadding(FMargin(1.f));
	Frame->SetContent(Scale);
	if (UVerticalBoxSlot* FrameSlot = Column->AddChildToVerticalBox(Frame))
	{
		FrameSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}

	PreviewText = MakeText(LOCTEXT("PreviewIdle", "Select a listing and press PREVIEW 3D to inspect it here, read-only, before you get it."), 7, TEXT("Italic"), Palette.TextSecondary);
	PreviewText->SetAutoWrapText(true);
	ActionButtons.SetNum(4);
	ActionButtons[int32(EAction::Frame)] = MakeButton(LOCTEXT("Frame", "FRAME"), LOCTEXT("FrameTip", "Back to the view of the whole stage."), int32(EAction::Frame), &ThisClass::HandleActionPicked);
	UHorizontalBox* PreviewRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	PreviewRow->AddChildToHorizontalBox(PreviewText)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	PreviewRow->AddChildToHorizontalBox(ActionButtons[int32(EAction::Frame)])->SetVerticalAlignment(VAlign_Center);
	Add(PreviewRow);

	DetailTitle = MakeText(FText::GetEmpty(), 12, TEXT("Bold"), Palette.TextPrimary);
	DetailTitle->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	Add(DetailTitle);
	DetailByline = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary);
	Add(DetailByline);
	DetailStats = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary);
	DetailStats->SetAutoWrapText(true);
	Add(DetailStats);
	DetailDescription = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextPrimary);
	DetailDescription->SetAutoWrapText(true);
	USizeBox* DescriptionBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	DescriptionBox->SetMaxDesiredHeight(90.f);
	DescriptionBox->SetClipping(EWidgetClipping::ClipToBounds);
	DescriptionBox->SetContent(DetailDescription);
	Add(DescriptionBox);
	DetailPrice = MakeText(FText::GetEmpty(), 9, TEXT("Bold"), Palette.AccentColor);
	Add(DetailPrice);

	ActionButtons[int32(EAction::Preview)] = MakeButton(LOCTEXT("Preview", "PREVIEW 3D"),
		LOCTEXT("PreviewTip", "Inspect this stage in 3D, read-only. Nothing is downloaded into your scenes and nothing is bought."), int32(EAction::Preview), &ThisClass::HandleActionPicked);
	ActionButtons[int32(EAction::Acquire)] = MakeButton(LOCTEXT("Get", "GET"),
		LOCTEXT("GetTip", "Add this listing to your saved scenes. Paid listings are bought with credits first."), int32(EAction::Acquire), &ThisClass::HandleActionPicked);
	ActionButtons[int32(EAction::Open)] = MakeButton(LOCTEXT("Open", "OPEN IN STAGE"),
		LOCTEXT("OpenTip", "Open the scene you got from this listing, replacing the stage (like File > Open)."), int32(EAction::Open), &ThisClass::HandleActionPicked);
	Add(BuildRow({ ActionButtons[int32(EAction::Preview)], ActionButtons[int32(EAction::Acquire)], ActionButtons[int32(EAction::Open)] }));

	StatusText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary);
	StatusText->SetAutoWrapText(true);
	Add(StatusText);
	return Column;
}

UWidget* UStageMarketplaceWidget::BuildPublishPage()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("PublishPage"));
	const auto Add = [Page](UWidget* Widget) { Page->AddChildToVerticalBox(Widget)->SetPadding(FMargin(0.f, 4.f)); };

	UTextBlock* Intro = MakeText(LOCTEXT("PublishIntro", "Publish one of your saved scenes. What is saved on disk is what others get: save the stage first (File > Save). "
		"Stages with imported models cannot be published yet, and real-money prices need an online store."), 8, TEXT("Regular"), Palette.TextSecondary);
	Intro->SetAutoWrapText(true);
	Add(Intro);

	SceneCombo = MakeCombo({});
	Add(BuildLabeled(LOCTEXT("Scene", "SAVED SCENE"), SceneCombo));
	TitleField = MakeTextField(LOCTEXT("TitleHint", "Listing title (1-80 characters; empty uses the scene name)"));
	Add(BuildLabeled(LOCTEXT("ListingTitle", "TITLE"), TitleField));

	DescriptionField = WidgetTree->ConstructWidget<UMultiLineEditableTextBox>(UMultiLineEditableTextBox::StaticClass());
	// Set before the Slate widget exists: UMultiLineEditableTextBox has no style setter.
	DescriptionField->WidgetStyle = MakeInputField(Palette);
	DescriptionField->SetHintText(LOCTEXT("DescriptionHint", "What the stage is, what it is for, what it needs (up to 2000 characters)."));
	USizeBox* DescriptionBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	DescriptionBox->SetHeightOverride(96.f);
	DescriptionBox->SetContent(DescriptionField);
	Add(BuildLabeled(LOCTEXT("Description", "DESCRIPTION"), DescriptionBox));

	KeywordsField = MakeTextField(LOCTEXT("KeywordsHint", "festival, LED wall, moving heads (comma separated, up to 16)"));
	Add(BuildLabeled(LOCTEXT("Keywords", "TAGS"), KeywordsField));

	UWidget* KindSegment = BuildSegment({ LOCTEXT("PublishStage", "STAGE DESIGN"), LOCTEXT("PublishRig", "LIGHTING RIG"), LOCTEXT("PublishCues", "CUE LIST") },
		PublishKindButtons, &ThisClass::HandlePublishKindPicked);
	PublishCategoryCombo = MakeCombo(StageMarketplacePanel::CategoryNames());
	PriceField = WidgetTree->ConstructWidget<USpinBox>(USpinBox::StaticClass());
	PriceField->SetWidgetStyle(MakeNumericField(Palette));
	PriceField->SetFont(MakeFont(9));
	PriceField->SetForegroundColor(Palette.TextPrimary);
	PriceField->SetMinFractionalDigits(0);
	PriceField->SetMaxFractionalDigits(0);
	PriceField->SetDelta(10.f);
	PriceField->SetMinValue(0.f);
	PriceField->SetMaxValue(float(StagePackage::MaxCreditsPrice));
	PriceField->SetMinSliderValue(0.f);
	PriceField->SetMaxSliderValue(5000.f);
	PriceField->SetValue(0.f);
	PriceField->SetToolTipText(LOCTEXT("PriceTip", "Price in credits; 0 makes the listing free."));
	USizeBox* PriceBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	PriceBox->SetWidthOverride(140.f);
	PriceBox->SetContent(PriceField);
	USizeBox* CategoryBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	CategoryBox->SetWidthOverride(180.f);
	CategoryBox->SetContent(PublishCategoryCombo);
	Add(BuildRow({ BuildLabeled(LOCTEXT("PublishKind", "KIND"), KindSegment), BuildLabeled(LOCTEXT("PublishCategory", "CATEGORY"), CategoryBox),
		BuildLabeled(LOCTEXT("PublishPrice", "PRICE (CREDITS)"), PriceBox) }, StageMarketplacePanel::GroupGap));

	PublishButton = MakeButton(LOCTEXT("Publish", "PUBLISH"), LOCTEXT("PublishTip", "Package the saved scene and publish it to the community folder."), 0, &ThisClass::HandlePublishPicked);
	StyleChoice(*PublishButton, true);
	PublishStatusText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary);
	PublishStatusText->SetAutoWrapText(true);
	UHorizontalBox* Footer = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Footer->AddChildToHorizontalBox(PublishButton)->SetVerticalAlignment(VAlign_Center);
	if (UHorizontalBoxSlot* StatusSlot = Footer->AddChildToHorizontalBox(PublishStatusText))
	{
		StatusSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		StatusSlot->SetPadding(FMargin(StageMarketplacePanel::GroupGap, 0.f, 0.f, 0.f));
		StatusSlot->SetVerticalAlignment(VAlign_Center);
	}
	Add(Footer);
	HandlePublishKindPicked(0);
	return Page;
}

// --- Lifetime ---

void UStageMarketplaceWidget::NativeConstruct()
{
	Super::NativeConstruct();
	UGameInstance* GameInstance = GetGameInstance();
	if (UStageMarketplaceSubsystem* Marketplace = GameInstance ? GameInstance->GetSubsystem<UStageMarketplaceSubsystem>() : nullptr)
	{
		BoundMarketplace = Marketplace;
		Marketplace->OnListingsUpdated.AddUniqueDynamic(this, &ThisClass::HandleListingsUpdated);
		Marketplace->OnAcquireCompleted.AddUniqueDynamic(this, &ThisClass::HandleAcquireCompleted);
		Marketplace->OnPublishCompleted.AddUniqueDynamic(this, &ThisClass::HandlePublishCompleted);
		PreviewReadyHandle = Marketplace->OnPreviewReady.AddUObject(this, &ThisClass::HandlePreviewReady);
	}
	if (UStageProfileSubsystem* Profile = GameInstance ? GameInstance->GetSubsystem<UStageProfileSubsystem>() : nullptr)
	{
		BoundProfile = Profile;
		Profile->OnBalanceChanged.AddUniqueDynamic(this, &ThisClass::HandleBalanceChanged);
		Profile->OnEntitlementsChanged.AddUniqueDynamic(this, &ThisClass::HandleOwnershipChanged);
		Profile->OnProfileLoaded.AddUniqueDynamic(this, &ThisClass::HandleOwnershipChanged);
	}
	SearchField->OnTextCommitted.AddUniqueDynamic(this, &ThisClass::HandleSearchCommitted);
	CategoryCombo->OnSelectionChanged.AddUniqueDynamic(this, &ThisClass::HandleCategoryChanged);
	SortCombo->OnSelectionChanged.AddUniqueDynamic(this, &ThisClass::HandleSortChanged);

	// Reopened after an assistant search (or an earlier visit): show the same results, refreshed.
	AdoptAnsweredQuery();
	RefreshHeader();
	RefreshFilters();
	RefreshSceneChoices();
	RebuildCards();
	RunQuery();
}

void UStageMarketplaceWidget::NativeDestruct()
{
	if (UStageMarketplaceSubsystem* Marketplace = BoundMarketplace.Get())
	{
		Marketplace->OnListingsUpdated.RemoveDynamic(this, &ThisClass::HandleListingsUpdated);
		Marketplace->OnAcquireCompleted.RemoveDynamic(this, &ThisClass::HandleAcquireCompleted);
		Marketplace->OnPublishCompleted.RemoveDynamic(this, &ThisClass::HandlePublishCompleted);
		Marketplace->OnPreviewReady.Remove(PreviewReadyHandle);
	}
	if (UStageProfileSubsystem* Profile = BoundProfile.Get())
	{
		Profile->OnBalanceChanged.RemoveDynamic(this, &ThisClass::HandleBalanceChanged);
		Profile->OnEntitlementsChanged.RemoveDynamic(this, &ThisClass::HandleOwnershipChanged);
		Profile->OnProfileLoaded.RemoveDynamic(this, &ThisClass::HandleOwnershipChanged);
	}
	if (SearchField)
	{
		SearchField->OnTextCommitted.RemoveDynamic(this, &ThisClass::HandleSearchCommitted);
	}
	if (CategoryCombo)
	{
		CategoryCombo->OnSelectionChanged.RemoveDynamic(this, &ThisClass::HandleCategoryChanged);
	}
	if (SortCombo)
	{
		SortCombo->OnSelectionChanged.RemoveDynamic(this, &ThisClass::HandleSortChanged);
	}
	BoundMarketplace.Reset();
	BoundProfile.Reset();
	PreviewReadyHandle.Reset();

	// The preview world goes with the panel; the image must stop pointing at its render target first.
	if (PreviewSurface)
	{
		PreviewSurface->SetImage(nullptr);
	}
	Preview.Reset();
	PreviewListing.Invalidate();
	Super::NativeDestruct();
}

// --- Browsing ---

void UStageMarketplaceWidget::RunQuery()
{
	if (UStageMarketplaceSubsystem* Marketplace = GetMarketplace())
	{
		Marketplace->RequestQuery(Query);
		ResultsText->SetText(LOCTEXT("Searching", "Searching..."));
	}
	else
	{
		ResultsText->SetText(LOCTEXT("NoMarketplace", "The marketplace is not available."));
	}
}

void UStageMarketplaceWidget::AdoptAnsweredQuery()
{
	const UStageMarketplaceSubsystem* Marketplace = GetMarketplace();
	if (!Marketplace)
	{
		return;
	}
	Query = Marketplace->GetAnsweredQuery();
	Query.Limit = StageMarketplacePanel::PageSize;
	SearchField->SetText(FText::FromString(Query.SearchText));
	// Set from code (ESelectInfo::Direct): the handlers ignore it, so this starts no query.
	CategoryCombo->SetSelectedIndex(StageMarketplacePanel::Categories().IndexOfByKey(Query.Category) + 1);
	const int32 SortIndex = MakeArrayView(StageMarketplacePanel::Sorts).IndexOfByKey(Query.Sort);
	SortCombo->SetSelectedIndex(FMath::Max(SortIndex, 0));
	RefreshFilters();
}

void UStageMarketplaceWidget::RebuildCards()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	const UStageMarketplaceSubsystem* Marketplace = GetMarketplace();

	ShownListings = Marketplace ? Marketplace->GetListings() : TArray<FStageMarketplaceListing>();
	CardList->ClearChildren();
	Cards.Reset();
	for (int32 Index = 0; Index < ShownListings.Num(); ++Index)
	{
		const FStageMarketplaceListing& Listing = ShownListings[Index];
		const EStageOwnershipState Ownership = Marketplace->GetListingOwnership(Listing.ListingId);

		UStageChoiceButton* Card = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
		Card->SetChoiceIndex(Index);
		Card->OnPicked.BindUObject(this, &ThisClass::HandleCardPicked);
		UVerticalBox* Content = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		UTextBlock* Title = MakeText(FText::FromString(Listing.Title), 9, TEXT("Bold"), Palette.TextPrimary);
		Title->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		Content->AddChildToVerticalBox(Title);
		Content->AddChildToVerticalBox(MakeText(FText::Format(LOCTEXT("CardByline", "by {0}  ·  {1}  ·  {2}"), FText::FromString(Listing.AuthorName),
			StageMarketplacePanel::KindName(Listing.Kind), StageMarketplacePanel::CategoryName(Listing.Category)), 7, TEXT("Regular"), Palette.TextSecondary));
		Content->AddChildToVerticalBox(MakeText(FText::Format(LOCTEXT("CardStats", "{0} items  ·  {1} fixtures  ·  {2} cues  ·  {3} downloads"),
			Listing.ItemCount, Listing.FixtureCount, Listing.CueCount, FText::AsNumber(Listing.Downloads)), 7, TEXT("Regular"), Palette.TextSecondary));
		const bool bLocked = Ownership == EStageOwnershipState::Locked;
		Content->AddChildToVerticalBox(MakeText(DescribePrice(Listing, Ownership), 8, TEXT("Bold"), bLocked ? Palette.WarningColor : Palette.AccentColor));
		if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(Card->AddChild(Content)))
		{
			ContentSlot->SetPadding(FMargin(8.f, 5.f));
			ContentSlot->SetHorizontalAlignment(HAlign_Fill);
		}
		CardList->AddChildToVerticalBox(Card)->SetPadding(FMargin(0.f, 0.f, 4.f, 4.f));
		Cards.Add(Card);
	}
	if (!ShownListings.ContainsByPredicate([this](const FStageMarketplaceListing& Listing) { return Listing.ListingId == SelectedListing; }))
	{
		SelectedListing.Invalidate();
	}

	const int32 Total = Marketplace ? Marketplace->GetTotalMatches() : 0;
	const int32 First = ShownListings.IsEmpty() ? 0 : Query.Offset + 1;
	const FText Results = Total == 0
		? LOCTEXT("NoResults", "No listings match. Publish one, or loosen the filters.")
		: FText::Format(LOCTEXT("Results", "{0}-{1} of {2} listings"), First, Query.Offset + ShownListings.Num(), Total);
	ResultsText->SetText(Query.MaxCredits > 0
		? FText::Format(LOCTEXT("ResultsMaxCredits", "{0}  ·  up to {1} credits"), Results, FText::AsNumber(Query.MaxCredits))
		: Results);
	PagingButtons[0]->SetIsEnabled(Query.Offset > 0);
	PagingButtons[1]->SetIsEnabled(Query.Offset + ShownListings.Num() < Total);
	RefreshCardStyles();
	RefreshDetails();
}

void UStageMarketplaceWidget::RefreshCardStyles()
{
	for (int32 Index = 0; Index < Cards.Num(); ++Index)
	{
		StyleCard(*Cards[Index], ShownListings.IsValidIndex(Index) && ShownListings[Index].ListingId == SelectedListing);
	}
}

void UStageMarketplaceWidget::RefreshFilters()
{
	for (int32 Index = 0; Index < KindButtons.Num(); ++Index)
	{
		const bool bSelected = Index == 0 ? Query.Kinds.IsEmpty() : Query.Kinds.Num() == 1 && Query.Kinds[0] == StageMarketplacePanel::Kinds[Index - 1];
		StyleChoice(*KindButtons[Index], bSelected);
	}
	for (int32 Index = 0; Index < PriceButtons.Num(); ++Index)
	{
		StyleChoice(*PriceButtons[Index], Index == int32(Query.PriceFilter));
	}
}

void UStageMarketplaceWidget::RefreshHeader()
{
	const UStageProfileSubsystem* Profile = BoundProfile.Get();
	const UStageMarketplaceSubsystem* Marketplace = GetMarketplace();
	CreditsText->SetText(FText::Format(LOCTEXT("Credits", "{0}   ·   {1} CREDITS"),
		Marketplace ? Marketplace->GetBackendName() : FText::GetEmpty(),
		FText::AsNumber(Profile ? Profile->GetBalance(StageCraftTags::Currency_Credits) : 0)));
}

const FStageMarketplaceListing* UStageMarketplaceWidget::GetSelectedListing() const
{
	return ShownListings.FindByPredicate([this](const FStageMarketplaceListing& Listing) { return Listing.ListingId == SelectedListing; });
}

FText UStageMarketplaceWidget::DescribePrice(const FStageMarketplaceListing& Listing, EStageOwnershipState Ownership) const
{
	switch (Ownership)
	{
	case EStageOwnershipState::Free:
		return LOCTEXT("PriceFreeBadge", "FREE");
	case EStageOwnershipState::Owned:
		return LOCTEXT("PriceOwnedBadge", "OWNED");
	case EStageOwnershipState::Purchasable:
		return FText::Format(LOCTEXT("PriceCreditsBadge", "{0} CREDITS"), FText::AsNumber(Listing.Price.Credits));
	default:
		return Listing.Price.HasRealMoneyPrice()
			? LOCTEXT("PriceMoneyBadge", "REAL-MONEY LISTING · NEEDS AN ONLINE STORE")
			: LOCTEXT("PriceLockedBadge", "LOCKED · PROFILE COULD NOT BE VERIFIED");
	}
}

void UStageMarketplaceWidget::RefreshDetails()
{
	const FStageMarketplaceListing* Listing = GetSelectedListing();
	const UStageMarketplaceSubsystem* Marketplace = GetMarketplace();
	for (UStageChoiceButton* Button : { ActionButtons[int32(EAction::Preview)].Get(), ActionButtons[int32(EAction::Acquire)].Get(), ActionButtons[int32(EAction::Open)].Get() })
	{
		Button->SetIsEnabled(Listing != nullptr && Marketplace != nullptr);
	}
	ActionButtons[int32(EAction::Frame)]->SetIsEnabled(Preview.IsValid() && PreviewListing.IsValid());
	if (!Listing || !Marketplace)
	{
		DetailTitle->SetText(LOCTEXT("NoSelection", "Select a listing"));
		DetailByline->SetText(FText::GetEmpty());
		DetailStats->SetText(FText::GetEmpty());
		DetailDescription->SetText(FText::GetEmpty());
		DetailPrice->SetText(FText::GetEmpty());
		StageMarketplacePanel::SetLabel(ActionButtons[int32(EAction::Acquire)], LOCTEXT("Get", "GET"));
		return;
	}

	const EStageOwnershipState Ownership = Marketplace->GetListingOwnership(Listing->ListingId);
	DetailTitle->SetText(FText::FromString(Listing->Title));
	DetailByline->SetText(FText::Format(LOCTEXT("DetailByline", "by {0}  ·  {1}  ·  {2}  ·  published {3}"), FText::FromString(Listing->AuthorName),
		StageMarketplacePanel::KindName(Listing->Kind), StageMarketplacePanel::CategoryName(Listing->Category), FText::AsDate(Listing->PublishedAtUtc)));
	DetailStats->SetText(FText::Format(LOCTEXT("DetailStats", "{0} items  ·  {1} fixtures  ·  {2} cues  ·  {3} downloads  ·  {4}{5}"),
		Listing->ItemCount, Listing->FixtureCount, Listing->CueCount, FText::AsNumber(Listing->Downloads), FText::AsMemory(Listing->PackageBytes),
		Listing->Keywords.IsEmpty() ? FText::GetEmpty() : FText::FromString(TEXT("  ·  #") + FString::Join(Listing->Keywords, TEXT("  #")))));
	DetailDescription->SetText(Listing->Description.IsEmpty() ? LOCTEXT("NoDescription", "No description.") : FText::FromString(Listing->Description));
	DetailPrice->SetText(DescribePrice(*Listing, Ownership));
	DetailPrice->SetColorAndOpacity(FSlateColor(Ownership == EStageOwnershipState::Locked ? GetTheme().WarningColor : GetTheme().AccentColor));

	FText AcquireLabel;
	if (Marketplace->IsAcquiring(Listing->ListingId))
	{
		AcquireLabel = LOCTEXT("Getting", "GETTING...");
	}
	else
	{
		switch (Ownership)
		{
		case EStageOwnershipState::Free:		AcquireLabel = LOCTEXT("GetFree", "GET (FREE)"); break;
		case EStageOwnershipState::Owned:		AcquireLabel = LOCTEXT("GetOwned", "GET AGAIN (OWNED)"); break;
		case EStageOwnershipState::Purchasable:	AcquireLabel = FText::Format(LOCTEXT("Buy", "BUY · {0} CREDITS"), FText::AsNumber(Listing->Price.Credits)); break;
		default:								AcquireLabel = LOCTEXT("Unavailable", "NOT AVAILABLE"); break;
		}
	}
	StageMarketplacePanel::SetLabel(ActionButtons[int32(EAction::Acquire)], AcquireLabel);
	ActionButtons[int32(EAction::Acquire)]->SetIsEnabled(!Marketplace->IsAcquiring(Listing->ListingId) && Ownership != EStageOwnershipState::Locked);
	StyleChoice(*ActionButtons[int32(EAction::Acquire)], Ownership == EStageOwnershipState::Purchasable);

	const bool bImported = ImportedScenes.Contains(Listing->ListingId);
	ActionButtons[int32(EAction::Open)]->SetIsEnabled(bImported);
	StageMarketplacePanel::SetLabel(ActionButtons[int32(EAction::Open)], ConfirmOpenListing == Listing->ListingId
		? LOCTEXT("OpenConfirm", "DISCARD CHANGES AND OPEN") : LOCTEXT("Open", "OPEN IN STAGE"));
	StyleChoice(*ActionButtons[int32(EAction::Open)], ConfirmOpenListing == Listing->ListingId);
	ActionButtons[int32(EAction::Preview)]->SetIsEnabled(!(Marketplace->IsPreviewLoading() && PreviewListing == Listing->ListingId));
}

void UStageMarketplaceWidget::ShowStatus(const FText& Message, bool bWarning)
{
	StatusText->SetText(Message);
	StatusText->SetColorAndOpacity(FSlateColor(bWarning ? GetTheme().WarningColor : GetTheme().TextSecondary));
}

void UStageMarketplaceWidget::HandlePagePicked(int32 Index)
{
	Query.Offset = FMath::Max(0, Query.Offset + (Index == 0 ? -StageMarketplacePanel::PageSize : StageMarketplacePanel::PageSize));
	RunQuery();
}

void UStageMarketplaceWidget::HandleKindPicked(int32 Index)
{
	Query.Kinds.Reset();
	if (Index > 0)
	{
		Query.Kinds.Add(StageMarketplacePanel::Kinds[Index - 1]);
	}
	Query.Offset = 0;
	RefreshFilters();
	RunQuery();
}

void UStageMarketplaceWidget::HandlePricePicked(int32 Index)
{
	Query.PriceFilter = static_cast<EStageMarketplacePriceFilter>(FMath::Clamp(Index, 0, 2));
	// A price limit set by the assistant has no chip; picking a price filter replaces it.
	Query.MaxCredits = 0;
	Query.Offset = 0;
	RefreshFilters();
	RunQuery();
}

void UStageMarketplaceWidget::HandleSearchCommitted(const FText& Text, ETextCommit::Type CommitMethod)
{
	if (CommitMethod != ETextCommit::OnEnter)
	{
		return;
	}
	Query.SearchText = Text.ToString();
	Query.Offset = 0;
	RunQuery();
}

void UStageMarketplaceWidget::HandleCategoryChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	if (SelectionType == ESelectInfo::Direct)
	{
		return;
	}
	const int32 Index = CategoryCombo->GetSelectedIndex();
	const TArray<FGameplayTag>& Categories = StageMarketplacePanel::Categories();
	Query.Category = Categories.IsValidIndex(Index - 1) ? Categories[Index - 1] : FGameplayTag();
	Query.Offset = 0;
	RunQuery();
}

void UStageMarketplaceWidget::HandleSortChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	if (SelectionType == ESelectInfo::Direct)
	{
		return;
	}
	const int32 Index = SortCombo->GetSelectedIndex();
	Query.Sort = StageMarketplacePanel::Sorts[FMath::Clamp(Index, 0, int32(UE_ARRAY_COUNT(StageMarketplacePanel::Sorts)) - 1)];
	Query.Offset = 0;
	RunQuery();
}

void UStageMarketplaceWidget::HandleCardPicked(int32 Index)
{
	if (!ShownListings.IsValidIndex(Index))
	{
		return;
	}
	SelectedListing = ShownListings[Index].ListingId;
	ConfirmOpenListing.Invalidate();
	// Only restyled here: the cards are rebuilt by query answers, never inside a card's own click.
	RefreshCardStyles();
	RefreshDetails();
}

void UStageMarketplaceWidget::HandleActionPicked(int32 Index)
{
	const FStageMarketplaceListing* Listing = GetSelectedListing();
	AModularPlayerController* Controller = GetStageController();
	switch (static_cast<EAction>(Index))
	{
	case EAction::Preview:
		StartPreview();
		break;

	case EAction::Acquire:
		if (Listing && Controller && Controller->RequestMarketplaceAcquire(Listing->ListingId))
		{
			ShowStatus(FText::Format(LOCTEXT("Acquiring", "Getting \"{0}\"..."), FText::FromString(Listing->Title)), false);
		}
		RefreshDetails();
		break;

	case EAction::Open:
	{
		const FString* SceneName = Listing ? ImportedScenes.Find(Listing->ListingId) : nullptr;
		if (!SceneName || !Controller)
		{
			break;
		}
		// The same duty as File > Open: a dirty stage is replaced only after an explicit second click.
		const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(GetWorld());
		if (Session && Session->IsDirty() && ConfirmOpenListing != Listing->ListingId)
		{
			ConfirmOpenListing = Listing->ListingId;
			ShowStatus(LOCTEXT("ConfirmOpen", "The stage has unsaved changes. Click DISCARD CHANGES AND OPEN to replace it, or save it first (File > Save)."), true);
			RefreshDetails();
			break;
		}
		ConfirmOpenListing.Invalidate();
		if (Controller->RequestLoadScene(*SceneName) == EStageSceneResult::Success)
		{
			ShowStatus(FText::Format(LOCTEXT("Opening", "Opening \"{0}\" in the stage..."), FText::FromString(*SceneName)), false);
		}
		RefreshDetails();
		break;
	}

	case EAction::Frame:
		if (Preview.IsValid())
		{
			Preview->Frame();
		}
		break;
	}
}

void UStageMarketplaceWidget::HandleListingsUpdated(FStageMarketplaceResultInfo Result)
{
	if (!Result.IsSuccess())
	{
		ShowStatus(Result.Message, true);
	}
	else
	{
		// The answer may be to the assistant's query (ADR 0021 §13.5): the filters show what the cards are.
		AdoptAnsweredQuery();
	}
	RebuildCards();
}

void UStageMarketplaceWidget::HandleAcquireCompleted(FGuid ListingId, FStageMarketplaceResultInfo Result, const FString& SceneName)
{
	if (Result.IsSuccess())
	{
		ImportedScenes.Add(ListingId, SceneName);
		RefreshSceneChoices();
	}
	ShowStatus(Result.Message, !Result.IsSuccess());
	// Ownership and downloads changed: ask again rather than patching the cards.
	RunQuery();
}

void UStageMarketplaceWidget::HandleBalanceChanged(FGameplayTag Currency, int64 NewBalance)
{
	RefreshHeader();
}

void UStageMarketplaceWidget::HandleOwnershipChanged()
{
	RefreshHeader();
	RefreshDetails();
}

// --- Preview ---

void UStageMarketplaceWidget::StartPreview()
{
	const FStageMarketplaceListing* Listing = GetSelectedListing();
	UStageMarketplaceSubsystem* Marketplace = GetMarketplace();
	if (!Listing || !Marketplace)
	{
		return;
	}
	const FStageMarketplaceResultInfo Result = Marketplace->RequestPreview(Listing->ListingId);
	if (!Result.IsSuccess())
	{
		ShowStatus(Result.Message, true);
		return;
	}
	PreviewListing = Listing->ListingId;
	PreviewText->SetText(FText::Format(LOCTEXT("PreviewLoading", "Loading \"{0}\" into the preview..."), FText::FromString(Listing->Title)));
	RefreshDetails();
}

void UStageMarketplaceWidget::HandlePreviewReady(const FGuid& ListingId, const FStageMarketplaceResultInfo& Result, const FStageMarketplacePreviewScene& Scene)
{
	// The subsystem already drops superseded previews, so this is the newest one, whoever asked for it (this panel, the console,
	// later the assistant): show it and select its card.
	PreviewListing = ListingId;
	if (ShownListings.ContainsByPredicate([&ListingId](const FStageMarketplaceListing& Shown) { return Shown.ListingId == ListingId; }))
	{
		SelectedListing = ListingId;
		RefreshCardStyles();
	}
	const FStageMarketplaceListing* Listing = ShownListings.FindByPredicate([&ListingId](const FStageMarketplaceListing& Shown) { return Shown.ListingId == ListingId; });
	const FText Title = Listing ? FText::FromString(Listing->Title) : FText::GetEmpty();
	if (!Result.IsSuccess() || !Scene.IsValid())
	{
		PreviewText->SetText(FText::Format(LOCTEXT("PreviewFailed", "\"{0}\" cannot be previewed: {1}"), Title, Result.Message));
		RefreshDetails();
		return;
	}

	if (!Preview.IsValid())
	{
		Preview = MakeShared<FStageMarketplacePreview>();
	}
	if (!Preview->IsAvailable())
	{
		PreviewText->SetText(LOCTEXT("PreviewNoRenderer", "The 3D preview needs a renderer (it is not available in this mode)."));
		RefreshDetails();
		return;
	}

	// Only items of the catalog are shown, whether or not they are owned: viewing grants nothing.
	TMap<FSoftObjectPath, UBaseItemData*> Catalog;
	if (const UStageItemSubsystem* Items = GetGameInstance() ? GetGameInstance()->GetSubsystem<UStageItemSubsystem>() : nullptr)
	{
		for (UBaseItemData* Item : Items->GetCatalog())
		{
			if (Item)
			{
				Catalog.Add(FSoftObjectPath(Item), Item);
			}
		}
	}
	const FStageMarketplacePreviewStats Stats = Preview->Show(*Scene, Catalog);
	PreviewSurface->SetImage(Preview->GetRenderTarget());
	PreviewText->SetText(Stats.NotInLibrary > 0
		? FText::Format(LOCTEXT("PreviewShownMissing", "Read-only preview of \"{0}\": {1} items shown, {2} not in your library. Drag to orbit, wheel to zoom."), Title, Stats.Placed, Stats.NotInLibrary)
		: FText::Format(LOCTEXT("PreviewShown", "Read-only preview of \"{0}\": {1} items. Drag to orbit, wheel to zoom."), Title, Stats.Placed));
	RefreshDetails();
}

void UStageMarketplaceWidget::HandlePreviewOrbit(const FVector2D& DragPixels)
{
	if (Preview.IsValid() && PreviewListing.IsValid())
	{
		Preview->Orbit(DragPixels.X * StageMarketplacePanel::OrbitYawPerPixel, -DragPixels.Y * StageMarketplacePanel::OrbitPitchPerPixel);
	}
}

void UStageMarketplaceWidget::HandlePreviewZoom(float WheelSteps)
{
	if (Preview.IsValid() && PreviewListing.IsValid())
	{
		Preview->Zoom(WheelSteps);
	}
}

// --- Publishing ---

void UStageMarketplaceWidget::HandlePagesPicked(int32 Index)
{
	Pages->SetActiveWidgetIndex(Index);
	for (int32 Button = 0; Button < PageButtons.Num(); ++Button)
	{
		StyleChoice(*PageButtons[Button], Button == Index);
	}
	if (Index == 1)
	{
		// Scenes saved since the last visit.
		RefreshSceneChoices();
	}
}

void UStageMarketplaceWidget::RefreshSceneChoices()
{
	const AModularPlayerController* Controller = GetStageController();
	const UStageSceneComponent* Scenes = Controller ? Controller->GetSceneFiles() : nullptr;
	const FString Previous = SceneCombo->GetSelectedOption();
	SceneCombo->ClearOptions();
	if (Scenes)
	{
		for (const FString& Name : Scenes->GetSceneNames())
		{
			SceneCombo->AddOption(Name);
		}
	}
	if (SceneCombo->FindOptionIndex(Previous) != INDEX_NONE)
	{
		SceneCombo->SetSelectedOption(Previous);
	}
	else if (SceneCombo->GetOptionCount() > 0)
	{
		SceneCombo->SetSelectedIndex(0);
	}
	PublishButton->SetIsEnabled(SceneCombo->GetOptionCount() > 0);
	if (SceneCombo->GetOptionCount() == 0)
	{
		PublishStatusText->SetText(LOCTEXT("NoScenes", "There are no saved scenes yet. Save the stage first (File > Save)."));
	}
}

void UStageMarketplaceWidget::HandlePublishKindPicked(int32 Index)
{
	PublishKind = StageMarketplacePanel::Kinds[FMath::Clamp(Index, 0, int32(UE_ARRAY_COUNT(StageMarketplacePanel::Kinds)) - 1)];
	for (int32 Button = 0; Button < PublishKindButtons.Num(); ++Button)
	{
		StyleChoice(*PublishKindButtons[Button], Button == Index);
	}
}

void UStageMarketplaceWidget::HandlePublishPicked(int32 Unused)
{
	AModularPlayerController* Controller = GetStageController();
	if (!Controller)
	{
		return;
	}

	FStageMarketplacePublishRequest Request;
	Request.SceneName = SceneCombo->GetSelectedOption();
	Request.Title = TitleField->GetText().ToString().TrimStartAndEnd();
	if (Request.Title.IsEmpty())
	{
		Request.Title = Request.SceneName;
	}
	Request.Description = DescriptionField->GetText().ToString();
	KeywordsField->GetText().ToString().ParseIntoArray(Request.Keywords, TEXT(","), /*CullEmpty*/ true);
	for (FString& Keyword : Request.Keywords)
	{
		Keyword.TrimStartAndEndInline();
	}
	Request.Kind = PublishKind;
	const TArray<FGameplayTag>& Categories = StageMarketplacePanel::Categories();
	const int32 CategoryIndex = PublishCategoryCombo->GetSelectedIndex();
	Request.Category = Categories.IsValidIndex(CategoryIndex) ? Categories[CategoryIndex] : StageCraftTags::Marketplace_Category_Other.GetTag();
	Request.Price.Credits = FMath::Max<int64>(0, FMath::RoundToInt64(PriceField->GetValue()));

	if (Controller->RequestMarketplacePublish(Request))
	{
		PublishStatusText->SetText(FText::Format(LOCTEXT("Publishing", "Publishing \"{0}\"..."), FText::FromString(Request.Title)));
		PublishStatusText->SetColorAndOpacity(FSlateColor(GetTheme().TextSecondary));
		PublishButton->SetIsEnabled(false);
	}
}

void UStageMarketplaceWidget::HandlePublishCompleted(FStageMarketplaceResultInfo Result, const FStageMarketplaceListing& Listing)
{
	PublishStatusText->SetText(Result.Message);
	PublishStatusText->SetColorAndOpacity(FSlateColor(Result.IsSuccess() ? GetTheme().AccentColor : GetTheme().WarningColor));
	PublishButton->SetIsEnabled(SceneCombo->GetOptionCount() > 0);
	if (Result.IsSuccess())
	{
		TitleField->SetText(FText::GetEmpty());
		DescriptionField->SetText(FText::GetEmpty());
		KeywordsField->SetText(FText::GetEmpty());
		RunQuery();
	}
}

#undef LOCTEXT_NAMESPACE
