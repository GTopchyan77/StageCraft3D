// Copyright 2024 Shape-Shift Studio GmbH. All Rights Reserved.

#include "FileDrop.h"
#include "Slate/SlateBrushAsset.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture2DDynamic.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Images/SImage.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "SFileDrop.h"

#if WITH_EDITOR
#include "TextureCompiler.h"
#endif
#include "Components/SlateWrapperTypes.h"
#include "Components/Widget.h"
#include "Engine/Font.h"
#include "UObject/ConstructorHelpers.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(FileDrop)

#define LOCTEXT_NAMESPACE "FILEDROP"

/////////////////////////////////////////////////////
// UFileDrop

UFileDrop::UFileDrop(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
    , NormalBrush(FSlateBrush())
    , AllowedBrush(FSlateBrush())
    , DeniedBrush(FSlateBrush())
    , ColorAndOpacity(FLinearColor::White)
    , FontColorAndOpacity(FLinearColor::White)
    , NormalMessage(LOCTEXT("Drag file here", "Drag file here"))
    , AllowedMessage(LOCTEXT("Drop file here", "Drop file here"))
    , WrongFileTypeMessage(LOCTEXT("File type not allowed", "File type not allowed"))
    , InvalidFileMessage(LOCTEXT("Invalid file", "Invalid file"))
    , bCheckFileExtensions(false)
    , AllowedFileExtensions({ TEXT("png"), TEXT("jpg"), TEXT("jpeg"), TEXT("bmp") })
{
    // Set default font
    if (!IsRunningDedicatedServer())
    {
        static ConstructorHelpers::FObjectFinder<UFont> RobotoFontObj(*UWidget::GetDefaultFontName());
        Font = FSlateFontInfo(RobotoFontObj.Object, 24, FName("Regular"));

        //set normal brush material
        static ConstructorHelpers::FObjectFinder<UMaterialInterface> NormalMaterialObj(TEXT("/FileDropUMG/Materials/MI_UI_DropArea_Normal"));
        if (NormalMaterialObj.Succeeded())
        {
            NormalBrush.SetResourceObject(NormalMaterialObj.Object);
        }

        //set allowed brush material
        static ConstructorHelpers::FObjectFinder<UMaterialInterface> AllowedMaterialObj(TEXT("/FileDropUMG/Materials/MI_UI_DropArea_Allowed"));
        if (AllowedMaterialObj.Succeeded())
        {
            AllowedBrush.SetResourceObject(AllowedMaterialObj.Object);
        }

        //set denied brush material
        static ConstructorHelpers::FObjectFinder<UMaterialInterface> DeniedMaterialObj(TEXT("/FileDropUMG/Materials/MI_UI_DropArea_Denied"));
        if (DeniedMaterialObj.Succeeded())
        {
            DeniedBrush.SetResourceObject(DeniedMaterialObj.Object);
        }
    }
}


FILEDROPUMG_API const FSlateBrush* UFileDrop::ConvertNormalImage(TAttribute<FSlateBrush> InImageAsset) const
{
    UFileDrop* MutableThis = const_cast<UFileDrop*>(this);
    MutableThis->UFileDrop::SetNormalBrush(InImageAsset.Get());

    return &NormalBrush;
}

FILEDROPUMG_API const FSlateBrush* UFileDrop::ConvertAllowedImage(TAttribute<FSlateBrush> InImageAsset) const
{
    UFileDrop* MutableThis = const_cast<UFileDrop*>(this);
    MutableThis->UFileDrop::SetAllowedBrush(InImageAsset.Get());

    return &AllowedBrush;
}

FILEDROPUMG_API const FSlateBrush* UFileDrop::ConvertDeniedImage(TAttribute<FSlateBrush> InImageAsset) const
{
    UFileDrop* MutableThis = const_cast<UFileDrop*>(this);
    MutableThis->UFileDrop::SetDeniedBrush(InImageAsset.Get());

    return &DeniedBrush;
}

TSharedRef<SWidget> UFileDrop::RebuildWidget()
{
    PRAGMA_DISABLE_DEPRECATION_WARNINGS
        FileDropWidget = SNew(SFileDrop);


    PRAGMA_ENABLE_DEPRECATION_WARNINGS
        return FileDropWidget.ToSharedRef();
}

void UFileDrop::SynchronizeProperties()
{
    Super::SynchronizeProperties();

    TAttribute<FSlateColor> ColorAndOpacityBinding = PROPERTY_BINDING(FSlateColor, ColorAndOpacity);
    TAttribute<FSlateColor> FontColorAndOpacityBinding = PROPERTY_BINDING(FSlateColor, FontColorAndOpacity);
    TAttribute<const FSlateBrush*> NormalImageBinding = OPTIONAL_BINDING_CONVERT(FSlateBrush, NormalBrush, const FSlateBrush*, ConvertNormalImage);
    TAttribute<const FSlateBrush*> AllowedImageBinding = OPTIONAL_BINDING_CONVERT(FSlateBrush, AllowedBrush, const FSlateBrush*, ConvertAllowedImage);
    TAttribute<const FSlateBrush*> DeniedImageBinding = OPTIONAL_BINDING_CONVERT(FSlateBrush, DeniedBrush, const FSlateBrush*, ConvertDeniedImage);
    TAttribute<FText> NormalMessageBinding = PROPERTY_BINDING(FText, NormalMessage);
    TAttribute<FText> AllowedMessageBinding = PROPERTY_BINDING(FText, AllowedMessage);
    TAttribute<FText> WrongFileTypeMessageBinding = PROPERTY_BINDING(FText, WrongFileTypeMessage);
    TAttribute<FText> InvalidFileMessageBinding = PROPERTY_BINDING(FText, InvalidFileMessage);

    if (FileDropWidget.IsValid())
    {
        FileDropWidget->SetColorAndOpacity(ColorAndOpacityBinding);
        FileDropWidget->SetNormalImage(NormalImageBinding);
        FileDropWidget->SetAllowedImage(AllowedImageBinding);
        FileDropWidget->SetDeniedImage(DeniedImageBinding);
        FileDropWidget->SetFont(Font);
        FileDropWidget->SetFontColorAndOpacity(FontColorAndOpacityBinding);
        FileDropWidget->SetNormalMessage(NormalMessageBinding);
        FileDropWidget->SetAllowedMessage(AllowedMessageBinding);
        FileDropWidget->SetWrongFileTypeMessage(WrongFileTypeMessageBinding);
        FileDropWidget->SetInvalidFileMessage(InvalidFileMessageBinding);

        FileDropWidget->SetCheckFileExtension(bCheckFileExtensions);
        FileDropWidget->SetAllowedFileExtensions(AllowedFileExtensions);

        FileDropWidget->SetOnFilesDropped(BIND_UOBJECT_DELEGATE(FOnFilesDropEventHandler, HandleFilesDropped));
        FileDropWidget->SetOnFilesDragOver(BIND_UOBJECT_DELEGATE(FOnFilesDragOverEventHandler, HandleFilesDragOver));
        FileDropWidget->SetOnFilesDragEnter(BIND_UOBJECT_DELEGATE(FOnFilesDragEnterEventHandler, HandleFilesDragEnter));
        FileDropWidget->SetOnFilesDragLeave(BIND_UOBJECT_DELEGATE(FOnFilesDragLeaveEventHandler, HandleFilesDragLeave));
        FileDropWidget->SetOnFilesValidCheck(BIND_UOBJECT_DELEGATE(FOnFilesValidCheckHandler, HandleValidateFiles));
    }
}

void UFileDrop::ReleaseSlateResources(bool bReleaseChildren)
{
    Super::ReleaseSlateResources(bReleaseChildren);

    FileDropWidget.Reset();
}

void UFileDrop::HandleFilesDropped(const TArray<FString>& Filepaths)
{
    if (OnFilesDropped.IsBound())
    {
        OnFilesDropped.Broadcast(Filepaths);
    }
}

FILEDROPUMG_API void UFileDrop::HandleFilesDragOver(const TArray<FString>& Filepaths, bool AllowDrop)
{
    if (OnFilesDragOver.IsBound())
    {
        OnFilesDragOver.Broadcast(Filepaths, AllowDrop);
    }
}

FILEDROPUMG_API void UFileDrop::HandleFilesDragEnter(const TArray<FString>& Filepaths)
{
    if (OnFilesDragEnter.IsBound())
    {
        OnFilesDragEnter.Broadcast(Filepaths);
    }
}

FILEDROPUMG_API void UFileDrop::HandleFilesDragLeave(const TArray<FString>& Filepaths)
{
    if (OnFilesDragLeave.IsBound())
    {
        OnFilesDragLeave.Broadcast(Filepaths);
    }
}

FILEDROPUMG_API bool UFileDrop::HandleValidateFiles(const TArray<FString>& Filepaths)
{
    if (OnFilesValidCheck.IsBound())
    {
        FFileCheckRetVal FileCheckResult = OnFilesValidCheck.Execute(Filepaths);
        if (!FileCheckResult.ErrorMessage.IsEmpty())
        {            
            //update invalid file message
            InvalidFileMessage = FileCheckResult.ErrorMessage;
            if (FileDropWidget.IsValid())
            {
                FileDropWidget->SetInvalidFileMessage(InvalidFileMessage);
                    
                //invalidate file drop
                FileDropWidget->InvalidateFileDrop();
            }
        }
        return FileCheckResult.bIsValid;
    }

    return true;
}

FILEDROPUMG_API void UFileDrop::SetCheckFileExtensions(bool bInCheckFileExtensions)
{
    if (bCheckFileExtensions != bInCheckFileExtensions)
    {
        bCheckFileExtensions = bInCheckFileExtensions;
        if (FileDropWidget.IsValid())
        {
            FileDropWidget->SetCheckFileExtension(bCheckFileExtensions);
        }
    }
}

FILEDROPUMG_API void UFileDrop::SetAllowedFileExtensions(const TArray<FString>& InFileExtensions)
{
    if (AllowedFileExtensions != InFileExtensions)
    {
        AllowedFileExtensions = InFileExtensions;
        if (FileDropWidget.IsValid())
        {            
            FileDropWidget->SetAllowedFileExtensions(AllowedFileExtensions);
        }
    }
}

FILEDROPUMG_API void UFileDrop::SetNormalBrush(const FSlateBrush& InBrush)
{
    if (NormalBrush != InBrush)
    {
        NormalBrush = InBrush;
        BroadcastFieldValueChanged(FFieldNotificationClassDescriptor::NormalBrush);
        if (FileDropWidget.IsValid())
        {            
            FileDropWidget->InvalidateFileDrop();
        }
    }
}

FILEDROPUMG_API void UFileDrop::SetAllowedBrush(const FSlateBrush& InBrush)
{
    if (AllowedBrush != InBrush)
    {
        AllowedBrush = InBrush;
        BroadcastFieldValueChanged(FFieldNotificationClassDescriptor::AllowedBrush);        
        if (FileDropWidget.IsValid())
        {            
            FileDropWidget->InvalidateFileDrop();
        }
    }
}

FILEDROPUMG_API void UFileDrop::SetDeniedBrush(const FSlateBrush& InBrush)
{
    if (DeniedBrush != InBrush)
    {
        DeniedBrush = InBrush;
        BroadcastFieldValueChanged(FFieldNotificationClassDescriptor::DeniedBrush);
        if (FileDropWidget.IsValid())
        {            
            FileDropWidget->InvalidateFileDrop();
        }
    }
}

FILEDROPUMG_API void UFileDrop::SetColorAndOpacity(FLinearColor InColorAndOpacity)
{
    ColorAndOpacity = InColorAndOpacity;
    if (FileDropWidget.IsValid())
    {
        FileDropWidget->SetColorAndOpacity(ColorAndOpacity);
        FileDropWidget->InvalidateFileDrop();
    }
}

FILEDROPUMG_API void UFileDrop::SetFont(const FSlateFontInfo& InFontInfo)
{
    Font = InFontInfo;
    if (FileDropWidget.IsValid())
    {
        FileDropWidget->SetFont(Font);
        FileDropWidget->InvalidateFileDrop();
    }
}

FILEDROPUMG_API void UFileDrop::SetFontColorAndOpacity(FLinearColor InColorAndOpacity)
{
    FontColorAndOpacity = InColorAndOpacity;
    if (FileDropWidget.IsValid())
    {
        FileDropWidget->SetFontColorAndOpacity(FontColorAndOpacity);
        FileDropWidget->InvalidateFileDrop();
    }
    
}

FILEDROPUMG_API void UFileDrop::SetNormalMessage(FText InMessage)
{
    NormalMessage = InMessage;
    if (FileDropWidget.IsValid())
    {
        FileDropWidget->SetNormalMessage(NormalMessage);
        FileDropWidget->InvalidateFileDrop();
    }
}

FILEDROPUMG_API void UFileDrop::SetAllowedMessage(FText InMessage)
{
    AllowedMessage = InMessage;
    if (FileDropWidget.IsValid())
    {
        FileDropWidget->SetAllowedMessage(AllowedMessage);
        FileDropWidget->InvalidateFileDrop();
    }
}

FILEDROPUMG_API void UFileDrop::SetWrongFileTypeMessage(FText InMessage)
{
    WrongFileTypeMessage = InMessage;
    if (FileDropWidget.IsValid())
    {
        FileDropWidget->SetWrongFileTypeMessage(WrongFileTypeMessage);
        FileDropWidget->InvalidateFileDrop();
    }
}

FILEDROPUMG_API void UFileDrop::SetInvalidFileMessage(FText InMessage)
{
    InvalidFileMessage = InMessage;
    if (FileDropWidget.IsValid())
    {
        FileDropWidget->SetInvalidFileMessage(InvalidFileMessage);
        FileDropWidget->InvalidateFileDrop();
    }
}

FILEDROPUMG_API bool UFileDrop::IsDropAllowed() const
{
    return FileDropWidget.IsValid() ? FileDropWidget->IsDropAllowed() : false;
}

FILEDROPUMG_API bool UFileDrop::IsFileExtensionValid() const
{
    return FileDropWidget.IsValid() ? FileDropWidget->IsFileExtensionValid() : false;
}

FILEDROPUMG_API bool UFileDrop::IsFileValid() const
{
    return FileDropWidget.IsValid() ? FileDropWidget->IsFileValid() : false;
}

#if WITH_ACCESSIBILITY
TSharedPtr<SWidget> UFileDrop::GetAccessibleWidget() const
{
    return FileDropWidget;
}
#endif

#if WITH_EDITOR

const FText UFileDrop::GetPaletteCategory()
{
    return LOCTEXT("Misc", "Misc");
}

#endif

/////////////////////////////////////////////////////

#undef LOCTEXT_NAMESPACE