// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UI/Fragment/SDlFmJoystick.h"
#include "UI/Fragment/DlFmJoystick_Lib.h"
#include "Blueprint/UserWidget.h"
#include "DlFmJoystick.generated.h"

class UCanvasPanel;
class UBorder;
class UDlFmBorder;
class UCanvasPanelSlot;
class UDlFmBorderSlot;
class SDlFmJoystick;
/**
 * 
 */
UCLASS()
class DLFILEMANAGER_API UDlFmJoystick : public UWidget
{
	GENERATED_BODY()
public:
	FOnDlFmJoystick_HandleChanged OnPlayerHandleChanged;
	
	virtual void SynchronizeProperties() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

	UFUNCTION(BlueprintCallable, Category="API")
	void SetCtrlSrc_PlayerHandle(EDlFmJoystick_ControlSource Val);
	UFUNCTION(BlueprintCallable, Category="API")
	void SetCtrlSrc_AIHandle(EDlFmJoystick_ControlSource Val);
	UFUNCTION(BlueprintCallable, Category="API")
	void SmoothedTo_AIHandle(const FVector2D& Val);
	UFUNCTION(BlueprintCallable, Category="API")
	void Set_PercentUnClamp_NoBroadcast_PlayerHandle(const FVector2D& Val);
	UFUNCTION(BlueprintCallable, Category="API")
	FVector2D Get_Percent();
	
	UFUNCTION(BlueprintCallable, Category="Configure")
	void SetConfig(FDlFmJoystick_Config& Val);
	UFUNCTION(BlueprintCallable, Category="Configure")
	FDlFmJoystick_Config& GetConfig(){return Config;}
	UFUNCTION(BlueprintCallable, Category="Configure")
	void RefreshConfig();
private:
	UPROPERTY(EditAnywhere, Category="Configure")
	FDlFmJoystick_Config Config;
#if WITH_EDITOR
	virtual const FText GetPaletteCategory() override;
#endif
protected:
	TSharedPtr<SDlFmJoystick> MyDlFmJoystick;

	UFUNCTION()
	void DelegateBridget_PlayerHandleChanged(const FVector2D& Val);
	virtual TSharedRef<SWidget> RebuildWidget() override;
};

