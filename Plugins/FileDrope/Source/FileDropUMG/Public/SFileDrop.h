// Copyright 2024 Shape-Shift Studio GmbH. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Misc/Attribute.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SLeafWidget.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Styling/SlateTypes.h"



DECLARE_DELEGATE_OneParam(FOnFilesDropEventHandler, const TArray<FString>& FilePaths);

DECLARE_DELEGATE_TwoParams(FOnFilesDragOverEventHandler, const TArray<FString>& FilePaths, bool bAllowDrop);

DECLARE_DELEGATE_OneParam(FOnFilesDragEnterEventHandler, const TArray<FString>& FilePaths);
DECLARE_DELEGATE_OneParam(FOnFilesDragLeaveEventHandler, const TArray<FString>& FilePaths);

DECLARE_DELEGATE_RetVal_OneParam(bool, FOnFilesValidCheckHandler, const TArray<FString>& FilePaths);



class SFileDrop : public SLeafWidget
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
    SLATE_DECLARE_WIDGET_API(SFileDrop, SLeafWidget, FILEDROPUMG_API)
#else
    SLATE_DECLARE_WIDGET(SFileDrop, SLeafWidget)
#endif

public:
    SLATE_BEGIN_ARGS(SFileDrop) {}
        SLATE_ATTRIBUTE(const FSlateBrush*, Image_Normal)
        SLATE_ATTRIBUTE(const FSlateBrush*, Image_Hovered_Allowed)
        SLATE_ATTRIBUTE(const FSlateBrush*, Image_Hovered_Denied)
        SLATE_ATTRIBUTE(FSlateFontInfo, FontInfo)
        SLATE_ATTRIBUTE(FSlateColor, FontColorAndOpacity)
        SLATE_ATTRIBUTE(FText, NormalMessage)
        SLATE_ATTRIBUTE(FText, AllowedMessage)
        SLATE_ATTRIBUTE(FText, WrongFileTypeMessage)
        SLATE_ATTRIBUTE(FText, InvalidFileMessage)
        SLATE_ATTRIBUTE(FSlateColor, ColorAndOpacity)
        SLATE_ATTRIBUTE(TOptional<FVector2D>, DesiredSizeOverride)
        SLATE_ARGUMENT(bool, CheckFileExtensions)
        SLATE_ARGUMENT(TArray<FString>, AllowedFileExtensions)
        SLATE_EVENT(FOnFilesDropEventHandler, OnFilesDropped)
        SLATE_EVENT(FOnFilesDragOverEventHandler, OnFilesDragOver)
        SLATE_EVENT(FOnFilesDragEnterEventHandler, OnFilesDragEnter)
        SLATE_EVENT(FOnFilesDragLeaveEventHandler, OnFilesDragLeave)
        SLATE_EVENT(FOnFilesValidCheckHandler, OnFilesValidCheck)
    SLATE_END_ARGS()


    /** Constructor */
    FILEDROPUMG_API SFileDrop();

    /** Constructs this widget with InArgs */
    void Construct(const FArguments& InArgs);


    FILEDROPUMG_API void OnDragEnter(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;


    FILEDROPUMG_API void OnDragLeave(const FDragDropEvent& DragDropEvent) override;


    FILEDROPUMG_API void OnMouseLeave(const FPointerEvent& MouseEvent) override;

public:
    FILEDROPUMG_API void SetOnFilesDropped(const FOnFilesDropEventHandler& InOnFilesDropped);

    FILEDROPUMG_API void SetOnFilesDragOver(const FOnFilesDragOverEventHandler& InOnFilesDragOver);

    FILEDROPUMG_API void SetOnFilesDragEnter(const FOnFilesDragEnterEventHandler& InOnFilesDragEnter);

    FILEDROPUMG_API void SetOnFilesDragLeave(const FOnFilesDragLeaveEventHandler& InOnFilesDragLeave);

    FILEDROPUMG_API void SetOnFilesValidCheck(const FOnFilesValidCheckHandler& InOnFilesValidCheck);

    /** Set the ColorAndOpacity attribute */
    FILEDROPUMG_API void SetColorAndOpacity(TAttribute<FSlateColor> InColorAndOpacity);

    /** See the ColorAndOpacity attribute */
    FILEDROPUMG_API void SetColorAndOpacity(FLinearColor InColorAndOpacity);

    /** Set the normal Image attribute */
    FILEDROPUMG_API void SetNormalImage(TAttribute<const FSlateBrush*> InImage);

    /** Set the allowed Image attribute */
    FILEDROPUMG_API void SetAllowedImage(TAttribute<const FSlateBrush*> InImage);

    /** Set the denied Image attribute */
    FILEDROPUMG_API void SetDeniedImage(TAttribute<const FSlateBrush*> InImage);

    /** Invalidate the Image */
    FILEDROPUMG_API void InvalidateFileDrop();


    FILEDROPUMG_API void SetFont(TAttribute<FSlateFontInfo> InFontInfo);

    /** Set the ColorAndOpacity attribute */
    FILEDROPUMG_API void SetFontColorAndOpacity(TAttribute<FSlateColor> InColorAndOpacity);

    /** See the ColorAndOpacity attribute */
    FILEDROPUMG_API void SetFontColorAndOpacity(FLinearColor InColorAndOpacity);

    /** Set SizeOverride attribute */
    FILEDROPUMG_API void SetDesiredSizeOverride(TAttribute<TOptional<FVector2D>> InDesiredSizeOverride);

    FILEDROPUMG_API void SetNormalMessage(TAttribute<FText> InMessage);

    FILEDROPUMG_API void SetAllowedMessage(TAttribute<FText> InMessage);

    FILEDROPUMG_API void SetWrongFileTypeMessage(TAttribute<FText> InMessage);

    FILEDROPUMG_API void SetInvalidFileMessage(TAttribute<FText> InMessage);

    FILEDROPUMG_API bool IsDropAllowed() const;

    FILEDROPUMG_API bool IsFileExtensionValid() const;

    FILEDROPUMG_API bool IsFileValid() const;

    const FText& GetNormalMessage() const
    {
        if (bIsAttributeNormalMessageBound)
        {
            SFileDrop& MutableSelf = const_cast<SFileDrop&>(*this);
            MutableSelf.NormalMessageAttribute.UpdateNow(MutableSelf);
        }
        return NormalMessageAttribute.Get();
    }

    const FText& GetAllowedMessage() const
    {
        if (bIsAttributeAllowedMessageBound)
        {
            SFileDrop& MutableSelf = const_cast<SFileDrop&>(*this);
            MutableSelf.AllowedMessageAttribute.UpdateNow(MutableSelf);
        }
        return AllowedMessageAttribute.Get();
    }

    const FText& GetWrongFileTypeMessage() const
    {
        if (bIsAttributeWrongFileTypeMessageBound)
        {
            SFileDrop& MutableSelf = const_cast<SFileDrop&>(*this);
            MutableSelf.WrongFileTypeMessageAttribute.UpdateNow(MutableSelf);
        }
        return WrongFileTypeMessageAttribute.Get();
    }

    const FText& GetInvalidFileMessage() const
    {
        if (bIsAttributeInvalidFileMessageBound)
        {
            SFileDrop& MutableSelf = const_cast<SFileDrop&>(*this);
            MutableSelf.InvalidFileMessageAttribute.UpdateNow(MutableSelf);
        }
        return InvalidFileMessageAttribute.Get();
    }

    FILEDROPUMG_API void SetCheckFileExtension(bool bCheckFileExtension);

    FILEDROPUMG_API void SetAllowedFileExtensions(TArray<FString> InAllowedFileExtensions);


    /** Override the OnDrop method to handle the drop event */
    FILEDROPUMG_API virtual FReply OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;


    FILEDROPUMG_API virtual FReply OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;

private:
    /** Handle the file drop */
    FILEDROPUMG_API void HandleFileDrop(const TArray<FString>& Files);


    FILEDROPUMG_API FVector2D ComputeDesiredSize(float) const override;


    FILEDROPUMG_API  int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;


protected:




    FOnFilesDropEventHandler OnFilesDropped;

    FOnFilesDragOverEventHandler OnFilesDragOver;

    FOnFilesDragEnterEventHandler OnFilesDragEnter;

    FOnFilesDragLeaveEventHandler OnFilesDragLeave;

    FOnFilesValidCheckHandler OnFilesValidCheck;

    bool bCheckfileExtensions = false;

    bool bFileExtensionsValid = false;
    bool bIsFileValid = false;

    bool bSetDragOver = false;

    TArray<FString> AllowedFileExtensions;

    /** @return an attribute reference of Image */
    TSlateAttributeRef<const FSlateBrush*> GetImageAttributeNormal() const { return TSlateAttributeRef<FSlateBrush const*>(SharedThis(this), ImageAttribute_Normal); }
    TSlateAttributeRef<const FSlateBrush*> GetImageAttributeAllowed() const { return TSlateAttributeRef<FSlateBrush const*>(SharedThis(this), ImageAttribute_Hovered_Allowed); }
    TSlateAttributeRef<const FSlateBrush*> GetImageAttributeDenied() const { return TSlateAttributeRef<FSlateBrush const*>(SharedThis(this), ImageAttribute_Hovered_Denied); }


    TSlateAttributeRef<FSlateFontInfo> GetFontInfoAttribute() const { return TSlateAttributeRef<FSlateFontInfo>(SharedThis(this), FontInfoAttribute); }
    TSlateAttributeRef<FSlateColor> GetFontColorAndOpacityAttribute() const { return TSlateAttributeRef<FSlateColor>(SharedThis(this), ColorAndOpacityAttribute); }

    /** @return an attribute reference of ColorAndOpacity */
    TSlateAttributeRef<FSlateColor> GetColorAndOpacityAttribute() const { return TSlateAttributeRef<FSlateColor>(SharedThis(this), ColorAndOpacityAttribute); }

    /** @return an attribute reference of DesiredSizeOverride */
    TSlateAttributeRef<TOptional<FVector2D>> GetDesiredSizeOverrideAttribute() const { return TSlateAttributeRef<TOptional<FVector2D>>(SharedThis(this), DesiredSizeOverrideAttribute); }

    TSlateAttributeRef< FText> GetNormalMessageAttribute() const { return TSlateAttributeRef<FText>(SharedThis(this), NormalMessageAttribute); }

    TSlateAttributeRef< FText> GetAllowedMessageAttribute() const { return TSlateAttributeRef<FText>(SharedThis(this), AllowedMessageAttribute); }

    TSlateAttributeRef< FText> GetWrongFileTypeMessageAttribute() const { return TSlateAttributeRef<FText>(SharedThis(this), WrongFileTypeMessageAttribute); }

    TSlateAttributeRef< FText> GetInvalidFileMessageAttribute() const { return TSlateAttributeRef<FText>(SharedThis(this), InvalidFileMessageAttribute); }
private:
    /** The slate brush to draw for the ImageAttribute that we can invalidate. */
    TSlateAttribute<const FSlateBrush*> ImageAttribute_Normal;
    TSlateAttribute<const FSlateBrush*> ImageAttribute_Hovered_Allowed;
    TSlateAttribute<const FSlateBrush*> ImageAttribute_Hovered_Denied;

    TSlateAttribute<FSlateFontInfo> FontInfoAttribute;
    TSlateAttribute<FSlateColor> FontColorAndOpacityAttribute;

    /** Color and opacity scale for this ImageAttribute */
    TSlateAttribute<FSlateColor> ColorAndOpacityAttribute;

    /** When specified, ignore the content's desired size and report the.HeightOverride as the Box's desired height. */
    TSlateAttribute<TOptional<FVector2D>> DesiredSizeOverrideAttribute;

    TSlateAttribute<FText> NormalMessageAttribute;

    TSlateAttribute<FText> AllowedMessageAttribute;

    TSlateAttribute<FText> WrongFileTypeMessageAttribute;
    TSlateAttribute<FText> InvalidFileMessageAttribute;

    /** Flags used to check if the SlateAttribute is set. */
    union
    {
        struct
        {
            uint16 bIsAttributeNormalMessageBound : 1;
            uint16 bIsAttributeAllowedMessageBound : 1;
            uint16 bIsAttributeWrongFileTypeMessageBound : 1;
            uint16 bIsAttributeInvalidFileMessageBound : 1;
        };
        uint16 Union_Flags;
    };
};