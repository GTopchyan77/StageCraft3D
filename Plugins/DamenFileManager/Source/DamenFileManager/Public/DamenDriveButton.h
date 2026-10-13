// Copyright DamenTools internal. Damen File Manager plugin.

#pragma once

#include "CoreMinimal.h"
#include "Components/Button.h"
#include "DamenDriveButton.generated.h"

class UDamenFileManagerWidget;

/**
 * One button in UDamenFileManagerWidget's drive bar ("C:", "D:", ...).
 * Built natively by UDamenFileManagerWidget::RebuildDriveBar - not placed in
 * the Designer. UButton::OnClicked carries no payload, so each button keeps
 * its own drive root and owner and forwards the click itself.
 */
UCLASS()
class DAMENFILEMANAGER_API UDamenDriveButton : public UButton
{
	GENERATED_BODY()

public:
	void Init(UDamenFileManagerWidget* InOwner, const FString& InDriveRoot);

	const FString& GetDriveRoot() const { return DriveRoot; }

private:
	UFUNCTION()
	void HandleClicked();

	UPROPERTY()
	TWeakObjectPtr<UDamenFileManagerWidget> Owner;

	/** Absolute drive root, e.g. "D:/". */
	UPROPERTY()
	FString DriveRoot;
};
