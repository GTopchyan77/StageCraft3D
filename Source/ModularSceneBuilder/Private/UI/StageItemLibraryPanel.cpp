// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageItemLibraryPanel.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBox.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Data/BaseItemData.h"
#include "Data/StageItemTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Placement/StagePlacementToolComponent.h"
#include "Player/ModularPlayerController.h"
#include "Subsystems/StageItemSubsystem.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageToolButton.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageItemLibraryPanel)

#define LOCTEXT_NAMESPACE "StageItemLibrary"

namespace StageItemLibrary
{
	constexpr float TileSize = 30.f;
	constexpr float ChevronSize = 12.f;

	FText ItemName(const UBaseItemData& Item)
	{
		return Item.DisplayName.IsEmpty() ? FText::FromName(Item.GetFName()) : Item.DisplayName;
	}

	bool MatchesSearch(const FString& ItemName, const FString& CategoryLabel, const FString& Query)
	{
		TArray<FString> Words;
		Query.ParseIntoArrayWS(Words);
		for (const FString& Word : Words)
		{
			if (!ItemName.Contains(Word, ESearchCase::IgnoreCase) && !CategoryLabel.Contains(Word, ESearchCase::IgnoreCase))
			{
				return false;
			}
		}
		return true;
	}

	FText MakeCategoryLabel(const FGameplayTag& Category)
	{
		if (!Category.IsValid())
		{
			return LOCTEXT("Uncategorised", "Other");
		}
		FString Name = Category.ToString();
		Name.RemoveFromStart(TEXT("StageCraft.Category."));
		return FText::FromString(Name.Replace(TEXT("."), TEXT(" / ")));
	}

	/** First letter of the name, for the thumbnail tile of items without an icon. */
	FText MakeInitial(const FText& Name)
	{
		const FString String = Name.ToString().TrimStart();
		return String.IsEmpty() ? FText::GetEmpty() : FText::FromString(String.Left(1).ToUpper());
	}

	UTextBlock* MakeText(UWidgetTree& Tree, const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color, FName Name = NAME_None)
	{
		UTextBlock* Block = Tree.ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), Name);
		Block->SetText(Text);
		Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
		Block->SetColorAndOpacity(FSlateColor(Color));
		return Block;
	}
}

// --- Entry ---

void UStageItemLibraryEntry::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	// A WBP subclass already has its designer tree here; the native class builds the default one, early
	// enough that SetItem (called right after CreateWidget) finds the widgets.
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildDefaultTree();
		bCodeStyled = true;
	}
	if (EntryButton)
	{
		EntryButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleButtonClicked);
	}
}

void UStageItemLibraryEntry::BuildDefaultTree()
{
	// A tool button never takes keyboard focus, so Esc / P keep reaching the viewport right after a click.
	EntryButton = WidgetTree->ConstructWidget<UButton>(UStageToolButton::StaticClass(), TEXT("EntryButton"));
	WidgetTree->RootWidget = EntryButton;

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Row"));
	if (UButtonSlot* RowSlot = Cast<UButtonSlot>(EntryButton->AddChild(Row)))
	{
		RowSlot->SetPadding(FMargin(8.f, 4.f));
		RowSlot->SetHorizontalAlignment(HAlign_Fill);
	}

	// Thumbnail tile: the icon, or the item's initial on a neutral tile when it has none.
	USizeBox* TileSize = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("TileSize"));
	TileSize->SetWidthOverride(StageItemLibrary::TileSize);
	TileSize->SetHeightOverride(StageItemLibrary::TileSize);
	if (UHorizontalBoxSlot* TileSlot = Row->AddChildToHorizontalBox(TileSize))
	{
		TileSlot->SetVerticalAlignment(VAlign_Center);
	}

	TileBorder = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("TileBorder"));
	TileBorder->SetPadding(FMargin(0.f));
	TileSize->AddChild(TileBorder);

	UOverlay* TileOverlay = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("TileOverlay"));
	TileBorder->SetContent(TileOverlay);

	IconImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("IconImage"));
	if (UOverlaySlot* IconSlot = TileOverlay->AddChildToOverlay(IconImage))
	{
		IconSlot->SetHorizontalAlignment(HAlign_Fill);
		IconSlot->SetVerticalAlignment(VAlign_Fill);
	}

	InitialText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("InitialText"));
	if (UOverlaySlot* InitialSlot = TileOverlay->AddChildToOverlay(InitialText))
	{
		InitialSlot->SetHorizontalAlignment(HAlign_Center);
		InitialSlot->SetVerticalAlignment(VAlign_Center);
	}

	UVerticalBox* Labels = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Labels"));
	if (UHorizontalBoxSlot* LabelsSlot = Row->AddChildToHorizontalBox(Labels))
	{
		LabelsSlot->SetVerticalAlignment(VAlign_Center);
		LabelsSlot->SetPadding(FMargin(10.f, 0.f, 0.f, 0.f));
		LabelsSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}

	NameText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("NameText"));
	NameText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	Labels->AddChildToVerticalBox(NameText);

	DetailText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("DetailText"));
	DetailText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	Labels->AddChildToVerticalBox(DetailText);
}

void UStageItemLibraryEntry::SetItem(UBaseItemData* InItem, const UStageCraftUITheme& InTheme)
{
	Item = InItem;
	Theme = &InTheme;
	if (!Item)
	{
		return;
	}

	const FText Name = StageItemLibrary::ItemName(*Item);
	if (NameText)
	{
		NameText->SetText(Name);
	}
	if (DetailText)
	{
		DetailText->SetText(StaticEnum<EStageItemType>()->GetDisplayValueAsText(Item->ItemType));
	}

	// The catalog streams the UI bundle (icons) before OnCatalogLoaded; no synchronous load here.
	UTexture2D* Icon = Item->Icon.Get();
	if (IconImage)
	{
		IconImage->SetBrushFromTexture(Icon);
		IconImage->SetVisibility(Icon ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (InitialText)
	{
		InitialText->SetText(StageItemLibrary::MakeInitial(Name));
		InitialText->SetVisibility(Icon ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}

	if (EntryButton)
	{
		const FText Usage = LOCTEXT("EntryUsage", "Click to place. Each click in the viewport places a copy; Esc or P finishes.");
		EntryButton->SetToolTipText(Item->Description.IsEmpty() ? Usage
			: FText::Format(LOCTEXT("EntryTooltip", "{0}\n\n{1}"), Item->Description, Usage));
	}

	ApplyCodeStyle();
}

void UStageItemLibraryEntry::SetArmed(bool bInArmed)
{
	if (bArmed == bInArmed)
	{
		return;
	}
	bArmed = bInArmed;
	ApplyCodeStyle();
}

void UStageItemLibraryEntry::ApplyCodeStyle()
{
	if (!bCodeStyled || !Theme)
	{
		return;
	}

	using namespace StageCraftWidgetStyle;
	if (EntryButton)
	{
		// Armed: a tinted row with a selection outline, like the selected row of an editor outliner.
		FButtonStyle Style = MakeFlatButton(FLinearColor::Transparent, Theme->RowHover, Theme->Divider);
		if (bArmed)
		{
			const FSlateBrush ArmedBrush = MakeBox(WithAlpha(Theme->SelectionColor, 0.22f), CornerRadius, Theme->SelectionColor, 1.f);
			Style.SetNormal(ArmedBrush).SetHovered(ArmedBrush);
		}
		EntryButton->SetStyle(Style);
		EntryButton->SetBackgroundColor(FLinearColor::White);
	}
	if (TileBorder)
	{
		TileBorder->SetBrush(MakeBox(Theme->RowBackground, CornerRadius, Theme->Divider, 1.f));
	}
	if (NameText)
	{
		NameText->SetFont(MakeFont(9, bArmed ? TEXT("Bold") : TEXT("Regular")));
		NameText->SetColorAndOpacity(FSlateColor(Theme->TextPrimary));
	}
	if (DetailText)
	{
		DetailText->SetFont(MakeFont(8));
		DetailText->SetColorAndOpacity(FSlateColor(Theme->TextSecondary));
	}
	if (InitialText)
	{
		InitialText->SetFont(MakeFont(11, TEXT("Bold")));
		InitialText->SetColorAndOpacity(FSlateColor(bArmed ? Theme->SelectionColor : Theme->TextSecondary));
	}
}

void UStageItemLibraryEntry::HandleButtonClicked()
{
	OnClicked.ExecuteIfBound(Item);
}

// --- Category ---

void UStageItemLibraryCategory::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
	if (HeaderButton)
	{
		HeaderButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleHeaderClicked);
	}
}

void UStageItemLibraryCategory::BuildTree()
{
	UVerticalBox* Root = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Root"));
	WidgetTree->RootWidget = Root;

	HeaderButton = WidgetTree->ConstructWidget<UButton>(UStageToolButton::StaticClass(), TEXT("HeaderButton"));
	if (UVerticalBoxSlot* HeaderSlot = Root->AddChildToVerticalBox(HeaderButton))
	{
		HeaderSlot->SetPadding(FMargin(0.f, 4.f, 0.f, 1.f));
	}

	UHorizontalBox* HeaderRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("HeaderRow"));
	if (UButtonSlot* RowSlot = Cast<UButtonSlot>(HeaderButton->AddChild(HeaderRow)))
	{
		RowSlot->SetPadding(FMargin(6.f, 4.f));
		RowSlot->SetHorizontalAlignment(HAlign_Fill);
	}

	ChevronImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("ChevronImage"));
	ChevronImage->SetDesiredSizeOverride(FVector2D(StageItemLibrary::ChevronSize));
	if (UHorizontalBoxSlot* ChevronSlot = HeaderRow->AddChildToHorizontalBox(ChevronImage))
	{
		ChevronSlot->SetVerticalAlignment(VAlign_Center);
		ChevronSlot->SetPadding(FMargin(0.f, 0.f, 6.f, 0.f));
	}

	LabelText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("LabelText"));
	if (UHorizontalBoxSlot* LabelSlot = HeaderRow->AddChildToHorizontalBox(LabelText))
	{
		LabelSlot->SetVerticalAlignment(VAlign_Center);
		LabelSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}

	CountText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("CountText"));
	if (UHorizontalBoxSlot* CountSlot = HeaderRow->AddChildToHorizontalBox(CountText))
	{
		CountSlot->SetVerticalAlignment(VAlign_Center);
	}

	Body = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Body"));
	if (UVerticalBoxSlot* BodySlot = Root->AddChildToVerticalBox(Body))
	{
		BodySlot->SetPadding(FMargin(0.f, 1.f, 0.f, 4.f));
	}
}

void UStageItemLibraryCategory::InitializeCategory(const FGameplayTag& InCategory, const UStageCraftUITheme& InTheme, bool bInExpanded)
{
	using namespace StageCraftWidgetStyle;

	Category = InCategory;
	bExpanded = bInExpanded;

	if (HeaderButton)
	{
		HeaderButton->SetStyle(MakeFlatButton(FLinearColor::Transparent, InTheme.RowHover, InTheme.Divider));
		HeaderButton->SetBackgroundColor(FLinearColor::White);
	}
	if (LabelText)
	{
		LabelText->SetText(StageItemLibrary::MakeCategoryLabel(Category).ToUpper());
		LabelText->SetFont(MakeFont(8, TEXT("Bold")));
		LabelText->SetColorAndOpacity(FSlateColor(InTheme.TextSecondary));
	}
	if (CountText)
	{
		CountText->SetFont(MakeFont(8));
		CountText->SetColorAndOpacity(FSlateColor(InTheme.TextDisabled));
	}
	if (ChevronImage)
	{
		ChevronImage->SetColorAndOpacity(InTheme.TextSecondary);
	}
	RefreshBodyVisibility();
}

void UStageItemLibraryCategory::AddEntry(UStageItemLibraryEntry& Entry)
{
	if (Body)
	{
		Body->AddChildToVerticalBox(&Entry);
	}
}

void UStageItemLibraryCategory::SetSearchActive(bool bInSearchActive)
{
	bSearchActive = bInSearchActive;
	RefreshBodyVisibility();
}

void UStageItemLibraryCategory::RefreshVisibleCount()
{
	int32 Visible = 0;
	if (Body)
	{
		for (const UWidget* Child : Body->GetAllChildren())
		{
			Visible += Child && Child->GetVisibility() != ESlateVisibility::Collapsed ? 1 : 0;
		}
	}
	if (CountText)
	{
		CountText->SetText(FText::AsNumber(Visible));
	}
	SetVisibility(Visible > 0 ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
}

void UStageItemLibraryCategory::RefreshBodyVisibility()
{
	const bool bShowBody = bExpanded || bSearchActive;
	if (Body)
	{
		Body->SetVisibility(bShowBody ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (ChevronImage)
	{
		ChevronImage->SetBrush(*StageCraftWidgetStyle::AppBrush(bShowBody ? TEXT("Icons.ChevronDown") : TEXT("Icons.ChevronRight")));
		ChevronImage->SetDesiredSizeOverride(FVector2D(StageItemLibrary::ChevronSize));
	}
}

void UStageItemLibraryCategory::HandleHeaderClicked()
{
	bExpanded = !bExpanded;
	RefreshBodyVisibility();
	OnToggled.ExecuteIfBound(Category, bExpanded);
}

// --- Panel ---

void UStageItemLibraryPanel::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildDefaultTree();
		bCodeStyled = true;
	}

	if (SearchBox)
	{
		SearchBox->OnTextChanged.AddUniqueDynamic(this, &ThisClass::HandleSearchChanged);
	}
	if (PlaceModeButton)
	{
		PlaceModeButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandlePlaceModeClicked);
	}
}

const UStageCraftUITheme& UStageItemLibraryPanel::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

void UStageItemLibraryPanel::BuildDefaultTree()
{
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Root"));
	Root->SetBrush(StageCraftWidgetStyle::MakeBox(Palette.PanelBackground));
	Root->SetPadding(FMargin(0.f));
	WidgetTree->RootWidget = Root;

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Column"));
	Root->SetContent(Column);

	Column->AddChildToVerticalBox(BuildToolbar());

	UImage* Divider = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("ToolbarDivider"));
	Divider->SetBrush(StageCraftWidgetStyle::MakeBox(Palette.Divider));
	Divider->SetDesiredSizeOverride(FVector2D(1.f, 1.f));
	Column->AddChildToVerticalBox(Divider);

	Column->AddChildToVerticalBox(BuildSearchRow());

	if (UVerticalBoxSlot* ListSlot = Column->AddChildToVerticalBox(BuildListArea()))
	{
		ListSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
}

UWidget* UStageItemLibraryPanel::BuildToolbar()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Toolbar = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Toolbar"));
	Toolbar->SetBrush(MakeBox(Palette.PanelHeader));
	Toolbar->SetPadding(FMargin(10.f, 6.f, 6.f, 6.f));

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("ToolbarRow"));
	Toolbar->SetContent(Row);

	UTextBlock* Title = StageItemLibrary::MakeText(*WidgetTree, LOCTEXT("Title", "LIBRARY"), 9, TEXT("Bold"), Palette.TextPrimary, TEXT("TitleText"));
	Title->SetToolTipText(LOCTEXT("TitleTip", "Every item you can place on the stage."));
	if (UHorizontalBoxSlot* TitleSlot = Row->AddChildToHorizontalBox(Title))
	{
		TitleSlot->SetVerticalAlignment(VAlign_Center);
	}

	CountText = StageItemLibrary::MakeText(*WidgetTree, FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary, TEXT("CountText"));
	if (UHorizontalBoxSlot* CountSlot = Row->AddChildToHorizontalBox(CountText))
	{
		CountSlot->SetVerticalAlignment(VAlign_Center);
		CountSlot->SetPadding(FMargin(8.f, 1.f, 0.f, 0.f));
		CountSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}

	PlaceModeButton = WidgetTree->ConstructWidget<UButton>(UStageToolButton::StaticClass(), TEXT("PlaceModeButton"));
	PlaceModeButton->SetToolTipText(LOCTEXT("PlaceTip",
		"Place mode (P): the armed item follows the cursor and every click places a copy. Click again or press Esc to return to Select."));
	PlaceModeText = StageItemLibrary::MakeText(*WidgetTree, LOCTEXT("PlaceLabel", "PLACE   P"), 8, TEXT("Bold"), Palette.TextSecondary, TEXT("PlaceModeText"));
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(PlaceModeButton->AddChild(PlaceModeText)))
	{
		LabelSlot->SetPadding(FMargin(10.f, 4.f));
	}
	if (UHorizontalBoxSlot* ButtonSlot = Row->AddChildToHorizontalBox(PlaceModeButton))
	{
		ButtonSlot->SetVerticalAlignment(VAlign_Center);
	}
	return Toolbar;
}

UWidget* UStageItemLibraryPanel::BuildSearchRow()
{
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* SearchRow = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("SearchRow"));
	SearchRow->SetBrush(StageCraftWidgetStyle::MakeBox(Palette.PanelBackground));
	SearchRow->SetPadding(FMargin(8.f, 8.f, 8.f, 4.f));

	SearchBox = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(), TEXT("SearchBox"));
	SearchBox->SetWidgetStyle(StageCraftWidgetStyle::MakeInputField(Palette));
	SearchBox->SetHintText(LOCTEXT("SearchHint", "Search items or categories"));
	SearchBox->SetRevertTextOnEscape(true);
	SearchBox->SetClearKeyboardFocusOnCommit(true);
	SearchRow->SetContent(SearchBox);
	return SearchRow;
}

UWidget* UStageItemLibraryPanel::BuildListArea()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UVerticalBox* Area = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ListArea"));

	EmptyStateText = StageItemLibrary::MakeText(*WidgetTree, FText::GetEmpty(), 9, TEXT("Italic"), Palette.TextSecondary, TEXT("EmptyStateText"));
	EmptyStateText->SetAutoWrapText(true);
	EmptyStateText->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* EmptySlot = Area->AddChildToVerticalBox(EmptyStateText))
	{
		EmptySlot->SetPadding(FMargin(12.f, 16.f));
	}

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("Scroll"));
	Scroll->SetWidgetBarStyle(MakeScrollBar(Palette));
	Scroll->SetScrollbarThickness(FVector2D(6.f, 6.f));
	Scroll->SetScrollbarPadding(FMargin(2.f));
	if (UVerticalBoxSlot* ScrollSlot = Area->AddChildToVerticalBox(Scroll))
	{
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}

	UVerticalBox* List = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ItemList"));
	if (UScrollBoxSlot* ListSlot = Cast<UScrollBoxSlot>(Scroll->AddChild(List)))
	{
		ListSlot->SetPadding(FMargin(6.f, 0.f, 6.f, 8.f));
	}
	ItemList = List;
	return Area;
}

void UStageItemLibraryPanel::NativeConstruct()
{
	Super::NativeConstruct();

	if (!EntryClass)
	{
		EntryClass = UStageItemLibraryEntry::StaticClass();
	}

	const UGameInstance* GameInstance = GetGameInstance();
	UStageItemSubsystem* Items = GameInstance ? GameInstance->GetSubsystem<UStageItemSubsystem>() : nullptr;
	ItemSubsystem = Items;
	if (Items)
	{
		Items->OnCatalogLoaded.AddUniqueDynamic(this, &ThisClass::HandleCatalogLoaded);
	}

	const AModularPlayerController* Controller = GetStageController();
	if (UStagePlacementToolComponent* Tool = Controller ? Controller->GetPlacementTool() : nullptr)
	{
		PlacementTool = Tool;
		Tool->OnArmedItemChanged.AddUniqueDynamic(this, &ThisClass::HandleArmedItemChanged);
		Tool->OnEditModeChanged.AddUniqueDynamic(this, &ThisClass::HandleEditModeChanged);
	}

	RebuildEntries();
	RefreshPlaceModeButton();
}

void UStageItemLibraryPanel::NativeDestruct()
{
	if (UStageItemSubsystem* Items = ItemSubsystem.Get())
	{
		Items->OnCatalogLoaded.RemoveDynamic(this, &ThisClass::HandleCatalogLoaded);
	}
	if (UStagePlacementToolComponent* Tool = PlacementTool.Get())
	{
		Tool->OnArmedItemChanged.RemoveDynamic(this, &ThisClass::HandleArmedItemChanged);
		Tool->OnEditModeChanged.RemoveDynamic(this, &ThisClass::HandleEditModeChanged);
	}
	ClearEntries();

	Super::NativeDestruct();
}

void UStageItemLibraryPanel::ClearEntries()
{
	for (UStageItemLibraryEntry* Entry : Entries)
	{
		Entry->OnClicked.Unbind();
	}
	for (UStageItemLibraryCategory* Group : Categories)
	{
		Group->OnToggled.Unbind();
	}
	Entries.Reset();
	Categories.Reset();
	if (ItemList)
	{
		ItemList->ClearChildren();
	}
}

void UStageItemLibraryPanel::RebuildEntries()
{
	ClearEntries();

	const UStageItemSubsystem* Items = ItemSubsystem.Get();
	if (!Items || !Items->IsCatalogLoaded() || !ItemList || !EntryClass)
	{
		SetEmptyState(LOCTEXT("Loading", "Loading library…"));
		return;
	}

	// Grouped by category; the catalog is sorted by name, so a stable sort keeps items alphabetical inside a group.
	TArray<UBaseItemData*> Sorted = Items->GetCatalog();
	Sorted.RemoveAll([](const UBaseItemData* Item) { return Item == nullptr; });
	Sorted.StableSort([](const UBaseItemData& A, const UBaseItemData& B)
	{
		return A.CategoryTag.ToString() < B.CategoryTag.ToString();
	});

	const UStageCraftUITheme& Palette = GetTheme();
	UStageItemLibraryCategory* Group = nullptr;
	for (UBaseItemData* Item : Sorted)
	{
		if (!Group || Item->CategoryTag != Group->GetCategory())
		{
			Group = CreateWidget<UStageItemLibraryCategory>(this, UStageItemLibraryCategory::StaticClass());
			Group->InitializeCategory(Item->CategoryTag, Palette, !CollapsedCategories.Contains(Item->CategoryTag));
			Group->OnToggled.BindUObject(this, &ThisClass::HandleCategoryToggled);
			ItemList->AddChild(Group);
			Categories.Add(Group);
		}

		UStageItemLibraryEntry* Entry = CreateWidget<UStageItemLibraryEntry>(this, EntryClass);
		if (!Entry)
		{
			continue;
		}
		Entry->SetItem(Item, Palette);
		Entry->OnClicked.BindUObject(this, &ThisClass::HandleEntryClicked);
		Group->AddEntry(*Entry);
		Entries.Add(Entry);
	}

	if (CountText)
	{
		CountText->SetText(FText::Format(LOCTEXT("ItemCount", "{0} {0}|plural(one=item,other=items)"), Entries.Num()));
	}
	ApplySearch();
	RefreshArmedHighlight();
}

void UStageItemLibraryPanel::ApplySearch()
{
	const bool bSearching = !SearchQuery.TrimStartAndEnd().IsEmpty();
	int32 Matches = 0;
	for (UStageItemLibraryEntry* Entry : Entries)
	{
		const UBaseItemData* Item = Entry->GetItem();
		const bool bMatch = Item && StageItemLibrary::MatchesSearch(StageItemLibrary::ItemName(*Item).ToString(),
			StageItemLibrary::MakeCategoryLabel(Item->CategoryTag).ToString(), SearchQuery);
		Entry->SetVisibility(bMatch ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		Matches += bMatch ? 1 : 0;
	}
	for (UStageItemLibraryCategory* Group : Categories)
	{
		Group->SetSearchActive(bSearching);
		Group->RefreshVisibleCount();
	}

	if (Entries.IsEmpty())
	{
		SetEmptyState(LOCTEXT("NoItems", "The catalog has no items."));
	}
	else if (Matches == 0)
	{
		SetEmptyState(FText::Format(LOCTEXT("NoMatch", "No items match \"{0}\"."), FText::FromString(SearchQuery.TrimStartAndEnd())));
	}
	else
	{
		SetEmptyState(FText::GetEmpty());
	}
}

void UStageItemLibraryPanel::SetEmptyState(const FText& Message)
{
	if (EmptyStateText)
	{
		EmptyStateText->SetText(Message);
		EmptyStateText->SetVisibility(Message.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}
}

void UStageItemLibraryPanel::RefreshArmedHighlight()
{
	const UStagePlacementToolComponent* Tool = PlacementTool.Get();
	const UBaseItemData* Armed = Tool ? Tool->GetArmedItem() : nullptr;
	for (UStageItemLibraryEntry* Entry : Entries)
	{
		Entry->SetArmed(Armed && Entry->GetItem() == Armed);
	}
}

void UStageItemLibraryPanel::RefreshPlaceModeButton()
{
	if (!bCodeStyled || !PlaceModeButton)
	{
		return;
	}

	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	const UStagePlacementToolComponent* Tool = PlacementTool.Get();
	const bool bPlacing = Tool && Tool->IsPlaceMode();

	// On: solid accent, like a lit key on a console. Off: outlined, so it still reads as a button.
	FButtonStyle Style = bPlacing
		? MakeFlatButton(Palette.AccentColor, Palette.AccentColor * 1.1f, Palette.AccentColor * 0.85f)
		: MakeFlatButton(FLinearColor::Transparent, Palette.RowHover, Palette.Divider);
	if (!bPlacing)
	{
		Style.SetNormal(MakeBox(FLinearColor::Transparent, CornerRadius, Palette.Divider, 1.f));
	}
	PlaceModeButton->SetStyle(Style);
	PlaceModeButton->SetBackgroundColor(FLinearColor::White);
	PlaceModeButton->SetIsEnabled(Tool != nullptr);
	if (PlaceModeText)
	{
		PlaceModeText->SetColorAndOpacity(FSlateColor(bPlacing ? Palette.WindowBackground : Palette.TextSecondary));
	}
}

void UStageItemLibraryPanel::HandleEntryClicked(UBaseItemData* Item)
{
	// The controller asks the rules and reports a refusal (status bar + error cue); nothing to do on failure here.
	if (AModularPlayerController* Controller = GetStageController())
	{
		Controller->RequestPlaceItem(Item);
	}
}

void UStageItemLibraryPanel::HandleCategoryToggled(FGameplayTag Category, bool bExpanded)
{
	if (bExpanded)
	{
		CollapsedCategories.Remove(Category);
	}
	else
	{
		CollapsedCategories.Add(Category);
	}
}

void UStageItemLibraryPanel::HandlePlaceModeClicked()
{
	if (UStagePlacementToolComponent* Tool = PlacementTool.Get())
	{
		Tool->TogglePlaceMode();
	}
}

void UStageItemLibraryPanel::HandleCatalogLoaded()
{
	RebuildEntries();
}

void UStageItemLibraryPanel::HandleArmedItemChanged(UBaseItemData* ArmedItem)
{
	RefreshArmedHighlight();
}

void UStageItemLibraryPanel::HandleEditModeChanged(EStageEditMode NewMode, EStageEditMode PreviousMode)
{
	RefreshPlaceModeButton();
}

void UStageItemLibraryPanel::HandleSearchChanged(const FText& Text)
{
	SearchQuery = Text.ToString();
	ApplySearch();
}

AModularPlayerController* UStageItemLibraryPanel::GetStageController() const
{
	return Cast<AModularPlayerController>(GetOwningPlayer());
}

#undef LOCTEXT_NAMESPACE
