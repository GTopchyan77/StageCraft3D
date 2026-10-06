// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageItemLibraryPanel.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Data/BaseItemData.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Placement/StagePlacementToolComponent.h"
#include "Player/ModularPlayerController.h"
#include "Subsystems/StageItemSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageItemLibraryPanel)

#define LOCTEXT_NAMESPACE "StageItemLibrary"

namespace StageItemLibrary
{
	constexpr float IconSize = 32.f;
	const FMargin EntryPadding(4.f, 2.f);
	const FLinearColor HeaderColor(0.6f, 0.6f, 0.65f);

	FText CategoryLabel(const FGameplayTag& Category)
	{
		if (!Category.IsValid())
		{
			return LOCTEXT("Uncategorised", "Other");
		}
		// StageCraft.Category.Lighting.MovingHead reads "Lighting / MovingHead".
		FString Name = Category.ToString();
		Name.RemoveFromStart(TEXT("StageCraft.Category."));
		return FText::FromString(Name.Replace(TEXT("."), TEXT(" / ")));
	}
}

// --- Entry ---

void UStageItemLibraryEntry::BuildDefaultTree()
{
	EntryButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("EntryButton"));
	WidgetTree->RootWidget = EntryButton;

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Row"));
	EntryButton->AddChild(Row);

	IconImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("IconImage"));
	IconImage->SetDesiredSizeOverride(FVector2D(StageItemLibrary::IconSize));
	if (UHorizontalBoxSlot* IconSlot = Row->AddChildToHorizontalBox(IconImage))
	{
		IconSlot->SetVerticalAlignment(VAlign_Center);
		IconSlot->SetPadding(FMargin(0.f, 0.f, 6.f, 0.f));
	}

	NameText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("NameText"));
	if (UHorizontalBoxSlot* NameSlot = Row->AddChildToHorizontalBox(NameText))
	{
		NameSlot->SetVerticalAlignment(VAlign_Center);
	}
}

void UStageItemLibraryEntry::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	// A WBP subclass already has its designer tree here; the native class builds the default one, early
	// enough that SetItem (called right after CreateWidget) finds the widgets.
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildDefaultTree();
	}
	if (EntryButton)
	{
		EntryButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleButtonClicked);
	}
}

void UStageItemLibraryEntry::SetItem(UBaseItemData* InItem)
{
	Item = InItem;
	if (!Item)
	{
		return;
	}

	if (NameText)
	{
		NameText->SetText(Item->DisplayName.IsEmpty() ? FText::FromName(Item->GetFName()) : Item->DisplayName);
	}
	if (EntryButton)
	{
		EntryButton->SetToolTipText(Item->Description);
	}
	if (IconImage)
	{
		// The catalog streams the UI bundle (icons) before OnCatalogLoaded; no synchronous load here.
		UTexture2D* Icon = Item->Icon.Get();
		IconImage->SetBrushFromTexture(Icon);
		IconImage->SetVisibility(Icon ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);
	}
}

void UStageItemLibraryEntry::SetArmed(bool bArmed)
{
	if (EntryButton)
	{
		EntryButton->SetBackgroundColor(bArmed ? ArmedTint : FLinearColor::White);
	}
}

void UStageItemLibraryEntry::HandleButtonClicked()
{
	OnClicked.ExecuteIfBound(Item);
}

// --- Panel ---

void UStageItemLibraryPanel::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildDefaultTree();
	}
}

void UStageItemLibraryPanel::BuildDefaultTree()
{
	UVerticalBox* Root = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Root"));
	WidgetTree->RootWidget = Root;

	StatusText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("StatusText"));
	StatusText->SetAutoWrapText(true);
	if (UVerticalBoxSlot* StatusSlot = Root->AddChildToVerticalBox(StatusText))
	{
		StatusSlot->SetPadding(FMargin(6.f, 4.f));
	}

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("Scroll"));
	if (UVerticalBoxSlot* ScrollSlot = Root->AddChildToVerticalBox(Scroll))
	{
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}

	UVerticalBox* List = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ItemList"));
	Scroll->AddChild(List);
	ItemList = List;
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

	if (AModularPlayerController* Controller = GetStageController())
	{
		PlacementTool = Controller->GetPlacementTool();
		PlacementTool->OnArmedItemChanged.AddUniqueDynamic(this, &ThisClass::HandleArmedItemChanged);
	}

	RebuildEntries();
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
	}
	for (UStageItemLibraryEntry* Entry : Entries)
	{
		Entry->OnClicked.Unbind();
	}
	Entries.Reset();

	Super::NativeDestruct();
}

void UStageItemLibraryPanel::RebuildEntries()
{
	for (UStageItemLibraryEntry* Entry : Entries)
	{
		Entry->OnClicked.Unbind();
	}
	Entries.Reset();
	if (ItemList)
	{
		ItemList->ClearChildren();
	}

	const UStageItemSubsystem* Items = ItemSubsystem.Get();
	const bool bLoaded = Items && Items->IsCatalogLoaded();
	if (StatusText)
	{
		StatusText->SetText(bLoaded
			? LOCTEXT("Hint", "Click an item, then click in the viewport to place it. P toggles placing, Esc cancels.")
			: LOCTEXT("Loading", "Loading library…"));
	}
	if (!bLoaded || !ItemList || !EntryClass)
	{
		return;
	}

	// Grouped by category (catalog order is by name, so items stay alphabetical inside a group).
	TArray<UBaseItemData*> Sorted = Items->GetCatalog();
	Sorted.StableSort([](const UBaseItemData& A, const UBaseItemData& B)
	{
		return A.CategoryTag.ToString() < B.CategoryTag.ToString();
	});

	FGameplayTag CurrentCategory;
	bool bFirst = true;
	for (UBaseItemData* Item : Sorted)
	{
		if (!Item)
		{
			continue;
		}

		if (bFirst || Item->CategoryTag != CurrentCategory)
		{
			bFirst = false;
			CurrentCategory = Item->CategoryTag;
			UTextBlock* Header = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
			Header->SetText(StageItemLibrary::CategoryLabel(CurrentCategory));
			Header->SetColorAndOpacity(FSlateColor(StageItemLibrary::HeaderColor));
			ItemList->AddChild(Header);
		}

		UStageItemLibraryEntry* Entry = CreateWidget<UStageItemLibraryEntry>(this, EntryClass);
		if (!Entry)
		{
			continue;
		}
		Entry->SetItem(Item);
		Entry->OnClicked.BindUObject(this, &ThisClass::HandleEntryClicked);
		Entry->SetPadding(StageItemLibrary::EntryPadding);
		ItemList->AddChild(Entry);
		Entries.Add(Entry);
	}

	RefreshArmedHighlight();
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

void UStageItemLibraryPanel::HandleEntryClicked(UBaseItemData* Item)
{
	// The controller asks the rules and reports a refusal (toast + error cue); nothing to do on failure here.
	if (AModularPlayerController* Controller = GetStageController())
	{
		Controller->RequestPlaceItem(Item);
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

AModularPlayerController* UStageItemLibraryPanel::GetStageController() const
{
	return Cast<AModularPlayerController>(GetOwningPlayer());
}

#undef LOCTEXT_NAMESPACE
