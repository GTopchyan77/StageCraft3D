// Copyright DamenTools internal. Damen File Manager plugin.

#include "DamenFileManagerItemWidget.h"
#include "DamenFileManager.h"
#include "DlFm_Item_Data.h"
#include "DlFmMain_Logic.h"
#include "UI/Fragment/DlFmFilteredToggleButton.h"

namespace
{
	// Names inside the vendor row (BindWidget member / private UFUNCTION of
	// UDlFm_Item_Logic). Checked at runtime: if a vendor update renames either,
	// NativeConstruct logs an error and leaves the vendor behaviour in place.
	const FName BackgroundButtonName(TEXT("Button_Background"));
	const FName VendorRowClickHandlerName(TEXT("OnInternalClicked_Background"));
}

void UDamenFileManagerItemWidget::NativeConstruct()
{
	// Super binds the vendor handlers (it runs on every re-construct of a pooled entry).
	Super::NativeConstruct();

	UDlFmFilteredToggleButton* const Background = Cast<UDlFmFilteredToggleButton>(GetWidgetFromName(BackgroundButtonName));
	if (!Background)
	{
		UE_LOG(LogDamenFileManager, Error, TEXT("DamenFileManagerItem '%s': no '%s' toggle button - single-click folder navigation is disabled (folders need a double-click)."),
			*GetName(), *BackgroundButtonName.ToString());
		return;
	}

	const FName OwnHandlerName = GET_FUNCTION_NAME_CHECKED(UDamenFileManagerItemWidget, HandleRowClicked);
	const bool bVendorBound = Background->OnCheckStateChanged.Contains(this, VendorRowClickHandlerName);
	const bool bOwnBound = Background->OnCheckStateChanged.Contains(this, OwnHandlerName);
	if (!bVendorBound && !bOwnBound)
	{
		UE_LOG(LogDamenFileManager, Error, TEXT("DamenFileManagerItem '%s': vendor row-click handler '%s' not found on '%s' (DlFileManager changed?) - keeping vendor behaviour, folders need a double-click."),
			*GetName(), *VendorRowClickHandlerName.ToString(), *BackgroundButtonName.ToString());
		return;
	}

	// Replace, not add: both handlers on one click would run the vendor's
	// select/confirm AND ours, i.e. double navigation on the second click.
	if (bVendorBound)
	{
		Background->OnCheckStateChanged.Remove(this, VendorRowClickHandlerName);
	}
	if (!bOwnBound)
	{
		Background->OnCheckStateChanged.AddUniqueDynamic(this, &UDamenFileManagerItemWidget::HandleRowClicked);
	}
}

void UDamenFileManagerItemWidget::HandleRowClicked(bool bIsChecked)
{
	UDlFm_Item_Data* const Data = GetListItem<UDlFm_Item_Data>();
	if (!Data || !Data->Env || !Data->Item)
	{
		return;
	}

	const FDlUI_FileExplorer_ItemInfo& Info = *Data->Item;
	if (Info.bIsDirectory && Data->Env->GetCore().SelectMode == FDlUI_FileExplorer_Lib::ESelectMode::Single)
	{
		// Copy first: browsing rebuilds the list this row's Info points into.
		const FString FullPath = Info.Name;
		UE_LOG(LogDamenFileManager, Log, TEXT("Folder '%s' clicked - browsing to full path '%s'."), *Info.CleanName, *FullPath);
		Data->Env->Func_BrowseToPath(FullPath);
		return;
	}

	// Files and multi-select: exactly the vendor's own row-click path.
	Data->Env->OnInternal_ItemClicked(Data->FixedIndex, Info.ListSegment);
}
