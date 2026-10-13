// Copyright DamenTools internal. Damen File Manager plugin.

#pragma once

#include "CoreMinimal.h"
#include "DlFm_Item_Logic.h"
#include "DamenFileManagerItemWidget.generated.h"

/**
 * Row widget for WBP_DamenFileManager's folder/file lists - parent class of
 * WBP_DamenFileManagerItem (a Damen copy of the vendor row BP_DlFm_Item_D, so
 * the vendor asset and source stay untouched).
 *
 * Only behaviour change vs the vendor row: a SINGLE click on a folder opens it.
 * The vendor row forwards every click to UDlFmMain_Logic::OnInternal_ItemClicked,
 * where the first click only selects and the second one (double-click)
 * navigates. NativeConstruct swaps the vendor's private row-click binding for
 * HandleRowClicked, which navigates folders immediately (passing the row's FULL
 * path, FDlUI_FileExplorer_ItemInfo::Name) and hands every other click (files,
 * multi-select mode) to the vendor exactly as before - so files still need a
 * double-click / "Choose" to import and a stray click never imports anything.
 */
UCLASS()
class DAMENFILEMANAGER_API UDamenFileManagerItemWidget : public UDlFm_Item_Logic
{
	GENERATED_BODY()

protected:
	virtual void NativeConstruct() override;

private:
	UFUNCTION()
	void HandleRowClicked(bool bIsChecked);
};
