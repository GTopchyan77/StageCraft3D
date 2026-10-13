// Copyright 2024 Shape-Shift Studio GmbH. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "Misc/Attribute.h"
#include "Input/Reply.h"
#include "Widgets/SWidget.h"
#include "Components/Widget.h"
#include "UObject/ScriptInterface.h"
#include "Slate/SlateTextureAtlasInterface.h"
#include "Engine/StreamableManager.h"
#include "Styling/SlateBrush.h"
#include "UObject/ScriptInterface.h"
#include "Components/Widget.h"

#include "FileDrop.generated.h"

class SFileDrop;

USTRUCT(BlueprintType)
struct FFileCheckRetVal
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintReadWrite, Category = "FileDrop")
    bool bIsValid = false;

    UPROPERTY(BlueprintReadWrite, Category = "FileDrop")
    FText ErrorMessage = FText();
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnFilesDropEvent, const TArray<FString>&, FilePaths);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnFilesDragOver, const TArray<FString>&, FilePaths, bool, bDropAllowed);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnFilesDragEnter, const TArray<FString>&, FilePaths);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnFilesDragLeave, const TArray<FString>&, FilePaths);
DECLARE_DYNAMIC_DELEGATE_RetVal_OneParam(FFileCheckRetVal, FOnFilesValidCheck, const TArray<FString>&, FilePaths);



/**
 * The FileDrop widget is a simple widget that allows you to drop files onto it.
 * It will trigger events when files are dropped, dragged over, dragged into and dragged out of the widget.
 *
 * * No Children
 */

UCLASS(MinimalAPI)
class UFileDrop : public UWidget
{
    GENERATED_UCLASS_BODY()

public:

    UPROPERTY(BlueprintAssignable, EditAnywhere, Category = Events)
    FOnFilesDropEvent OnFilesDropped;

    UPROPERTY(BlueprintAssignable, EditAnywhere, Category = Events)
    FOnFilesDragOver OnFilesDragOver;

    UPROPERTY(BlueprintAssignable, EditAnywhere, Category = Events)
    FOnFilesDragEnter OnFilesDragEnter;

    UPROPERTY(BlueprintAssignable, EditAnywhere, Category = Events)
    FOnFilesDragLeave OnFilesDragLeave;

    UPROPERTY(EditAnywhere, Category = Events, meta = (IsBindableEvent = "True"))
    FOnFilesValidCheck OnFilesValidCheck;

    //~ Begin UWidget Interface
    FILEDROPUMG_API virtual void SynchronizeProperties() override;
    //~ End UWidget Interface

    //~ Begin UVisual Interface
    FILEDROPUMG_API virtual void ReleaseSlateResources(bool bReleaseChildren) override;
    //~ End UVisual Interface

    FILEDROPUMG_API void HandleFilesDropped(const TArray<FString>& Filepaths);

    FILEDROPUMG_API void HandleFilesDragOver(const TArray<FString>& Filepaths, bool AllowDrop);

    FILEDROPUMG_API void HandleFilesDragEnter(const TArray<FString>& Filepaths);

    FILEDROPUMG_API void HandleFilesDragLeave(const TArray<FString>& Filepaths);

    FILEDROPUMG_API bool HandleValidateFiles(const TArray<FString>& Filepaths);

    UPROPERTY(EditAnywhere, FieldNotify, Category = "FileDrop|Appearance")
    FSlateBrush NormalBrush;

    /** A bindable delegate for the Image. */
    UPROPERTY()
    FGetSlateBrush NormalBrushDelegate;

    UPROPERTY(EditAnywhere, FieldNotify, Category = "FileDrop|Appearance")
    FSlateBrush AllowedBrush;

    UPROPERTY()
    FGetSlateBrush AllowedBrushDelegate;

    UPROPERTY(EditAnywhere, FieldNotify, Category = "FileDrop|Appearance")
    FSlateBrush DeniedBrush;

    UPROPERTY()
    FGetSlateBrush DeniedBrushDelegate;

    UPROPERTY(EditAnywhere, Category = "FileDrop|Appearance", meta = (sRGB = "true"))
    FLinearColor ColorAndOpacity;

    UPROPERTY()
    FGetLinearColor ColorAndOpacityDelegate;

    UPROPERTY(EditAnywhere, Category = "FileDrop|Text", meta = (sRGB = "true"))
    FLinearColor FontColorAndOpacity;

    UPROPERTY()
    FGetLinearColor FontColorAndOpacityDelegate;

    UPROPERTY(EditAnywhere, Category = "FileDrop|Text")
    FSlateFontInfo Font;



    UPROPERTY(EditAnywhere, Category = "FileDrop|Text")
    FText NormalMessage;

    UPROPERTY()
    FGetText NormalMessageDelegate;

    UPROPERTY(EditAnywhere, Category = "FileDrop|Text")
    FText AllowedMessage;

    UPROPERTY()
    FGetText AllowedMessageDelegate;

    UPROPERTY(EditAnywhere, Category = "FileDrop|Text")
    FText WrongFileTypeMessage;

    UPROPERTY()
    FGetText WrongFileTypeMessageDelegate;

        UPROPERTY(EditAnywhere, Category = "FileDrop|Text")
    FText InvalidFileMessage;

    UPROPERTY()
    FGetText InvalidFileMessageDelegate;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "FileDrop|Filter")
    bool bCheckFileExtensions = false;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "FileDrop|Filter")
    TArray<FString> AllowedFileExtensions;


    UFUNCTION(BlueprintCallable, Category = "FileDrop|Filter")
    FILEDROPUMG_API virtual void SetCheckFileExtensions(bool bInCheckFileExtensions);

    UFUNCTION(BlueprintCallable, Category = "FileDrop|Filter")
    FILEDROPUMG_API virtual void SetAllowedFileExtensions(const TArray<FString>& InFileExtensions);



    UFUNCTION(BlueprintCallable, Category = "FileDrop|Appearance")
    FILEDROPUMG_API virtual void SetNormalBrush(const FSlateBrush& InBrush);

    UFUNCTION(BlueprintCallable, Category = "FileDrop|Appearance")
    FILEDROPUMG_API virtual void SetAllowedBrush(const FSlateBrush& InBrush);

    UFUNCTION(BlueprintCallable, Category = "FileDrop|Appearance")
    FILEDROPUMG_API virtual void SetDeniedBrush(const FSlateBrush& InBrush);

    UFUNCTION(BlueprintCallable, Category = "FileDrop|Appearance")
    FILEDROPUMG_API const FSlateBrush& GetNormalBrush() const { return NormalBrush; }

    UFUNCTION(BlueprintCallable, Category = "FileDrop|Appearance")
    FILEDROPUMG_API const FSlateBrush& GetAllowedBrush() const { return AllowedBrush; }

    UFUNCTION(BlueprintCallable, Category = "FileDrop|Appearance")
    FILEDROPUMG_API const FSlateBrush& GetDeniedBrush() const { return DeniedBrush; }

    UFUNCTION(BlueprintCallable, Category = "FileDrop|Appearance")
    FILEDROPUMG_API void SetColorAndOpacity(FLinearColor InColorAndOpacity);

    UFUNCTION(BlueprintCallable, Category = "FileDrop|Text")
    FILEDROPUMG_API void SetFont(const FSlateFontInfo& InFontInfo);

    UFUNCTION(BlueprintCallable, Category = "FileDrop|Text")
    FILEDROPUMG_API void SetFontColorAndOpacity(FLinearColor InColorAndOpacity);


    UFUNCTION(BlueprintCallable, Category = "FileDrop|Text")
    FILEDROPUMG_API void SetNormalMessage(FText InMessage);

    UFUNCTION(BlueprintCallable, Category = "FileDrop|Text")
    FILEDROPUMG_API void SetAllowedMessage(FText InMessage);

    UFUNCTION(BlueprintCallable, Category = "FileDrop|Text")
    FILEDROPUMG_API void SetWrongFileTypeMessage(FText InMessage);

    UFUNCTION(BlueprintCallable, Category = "FileDrop|Text")
    FILEDROPUMG_API void SetInvalidFileMessage(FText InMessage);

    UFUNCTION(BlueprintPure, Category = "FileDrop")
    FILEDROPUMG_API bool IsDropAllowed() const;

    UFUNCTION(BlueprintPure, Category = "FileDrop")
    FILEDROPUMG_API bool IsFileExtensionValid() const;

    UFUNCTION(BlueprintPure, Category = "FileDrop")
    FILEDROPUMG_API bool IsFileValid() const;



#if WITH_EDITOR
    //~ Begin UWidget Interface
    FILEDROPUMG_API virtual const FText GetPaletteCategory() override;
    //~ End UWidget Interface
#endif

protected:
    /** Translates the bound brush data and assigns it to the cached brush used by this widget. */
    FILEDROPUMG_API const FSlateBrush* ConvertNormalImage(TAttribute<FSlateBrush> InImageAsset) const;

    FILEDROPUMG_API const FSlateBrush* ConvertAllowedImage(TAttribute<FSlateBrush> InImageAsset) const;

    FILEDROPUMG_API const FSlateBrush* ConvertDeniedImage(TAttribute<FSlateBrush> InImageAsset) const;

    //~ Begin UWidget Interface
    FILEDROPUMG_API virtual TSharedRef<SWidget> RebuildWidget() override;
    //~ End UWidget Interface
#if WITH_ACCESSIBILITY
    FILEDROPUMG_API virtual TSharedPtr<SWidget> GetAccessibleWidget() const override;
#endif

protected:
    TSharedPtr<SFileDrop> FileDropWidget;

    PROPERTY_BINDING_IMPLEMENTATION(FSlateColor, ColorAndOpacity);
    PROPERTY_BINDING_IMPLEMENTATION(FSlateColor, FontColorAndOpacity);
    PROPERTY_BINDING_IMPLEMENTATION(FText, NormalMessage);
    PROPERTY_BINDING_IMPLEMENTATION(FText, AllowedMessage);
    PROPERTY_BINDING_IMPLEMENTATION(FText, WrongFileTypeMessage);
    PROPERTY_BINDING_IMPLEMENTATION(FText, InvalidFileMessage);
};
