// Copyright DamenTools internal. Damen File Manager plugin.

#include "DamenDriveButton.h"
#include "DamenFileManagerWidget.h"

void UDamenDriveButton::Init(UDamenFileManagerWidget* InOwner, const FString& InDriveRoot)
{
	Owner = InOwner;
	DriveRoot = InDriveRoot;
	OnClicked.AddUniqueDynamic(this, &UDamenDriveButton::HandleClicked);
}

void UDamenDriveButton::HandleClicked()
{
	if (UDamenFileManagerWidget* const OwnerWidget = Owner.Get())
	{
		OwnerWidget->BrowseToDrive(DriveRoot);
	}
}
