// Copyright 2024 Shape-Shift Studio GmbH. All Rights Reserved.

#include "SFileDrop.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"

SLATE_IMPLEMENT_WIDGET(SFileDrop)
void SFileDrop::PrivateRegisterAttributes(FSlateAttributeInitializer& AttributeInitializer)
{
    SLATE_ADD_MEMBER_ATTRIBUTE_DEFINITION_WITH_NAME(AttributeInitializer, "Image_Normal", ImageAttribute_Normal, EInvalidateWidgetReason::Layout);
    SLATE_ADD_MEMBER_ATTRIBUTE_DEFINITION_WITH_NAME(AttributeInitializer, "Image_Hovered_Allowed", ImageAttribute_Hovered_Allowed, EInvalidateWidgetReason::Layout);
    SLATE_ADD_MEMBER_ATTRIBUTE_DEFINITION_WITH_NAME(AttributeInitializer, "Image_Hovered_Denied", ImageAttribute_Hovered_Denied, EInvalidateWidgetReason::Layout);
    SLATE_ADD_MEMBER_ATTRIBUTE_DEFINITION_WITH_NAME(AttributeInitializer, "FontInfo", FontInfoAttribute, EInvalidateWidgetReason::Layout);
    SLATE_ADD_MEMBER_ATTRIBUTE_DEFINITION_WITH_NAME(AttributeInitializer, "NormalMessage", NormalMessageAttribute, EInvalidateWidgetReason::Layout);
    SLATE_ADD_MEMBER_ATTRIBUTE_DEFINITION_WITH_NAME(AttributeInitializer, "AllowedMessage", AllowedMessageAttribute, EInvalidateWidgetReason::Layout);
    SLATE_ADD_MEMBER_ATTRIBUTE_DEFINITION_WITH_NAME(AttributeInitializer, "WrongFileTypeMessage", WrongFileTypeMessageAttribute, EInvalidateWidgetReason::Layout);
    SLATE_ADD_MEMBER_ATTRIBUTE_DEFINITION_WITH_NAME(AttributeInitializer, "InvalidFileMessage", InvalidFileMessageAttribute, EInvalidateWidgetReason::Layout);
    SLATE_ADD_MEMBER_ATTRIBUTE_DEFINITION_WITH_NAME(AttributeInitializer, "ColorAndOpacity", ColorAndOpacityAttribute, EInvalidateWidgetReason::Paint);
    SLATE_ADD_MEMBER_ATTRIBUTE_DEFINITION_WITH_NAME(AttributeInitializer, "FontColorAndOpacity", FontColorAndOpacityAttribute, EInvalidateWidgetReason::Paint);
    SLATE_ADD_MEMBER_ATTRIBUTE_DEFINITION_WITH_NAME(AttributeInitializer, "DesiredSizeOverride", DesiredSizeOverrideAttribute, EInvalidateWidgetReason::Layout);

}

FILEDROPUMG_API SFileDrop::SFileDrop()
    : ImageAttribute_Normal(*this)
    , ImageAttribute_Hovered_Allowed(*this)
    , ImageAttribute_Hovered_Denied(*this)
    , FontInfoAttribute(*this)
    , FontColorAndOpacityAttribute(*this)
    , ColorAndOpacityAttribute(*this)
    , DesiredSizeOverrideAttribute(*this)
    , NormalMessageAttribute(*this)
    , AllowedMessageAttribute(*this)
    , WrongFileTypeMessageAttribute(*this)
    , InvalidFileMessageAttribute(*this)
    , bIsAttributeNormalMessageBound(false)
    , bIsAttributeAllowedMessageBound(false)
    , bIsAttributeWrongFileTypeMessageBound(false)
{
    SetCanTick(false);
    bCanSupportFocus = true;
    bCheckfileExtensions = false;
    bFileExtensionsValid = false;
    bIsFileValid = false;
    bSetDragOver = false;
}

void SFileDrop::Construct(const FArguments& InArgs)
{
    if (InArgs._OnFilesDropped.IsBound())
    {
        OnFilesDropped = InArgs._OnFilesDropped;
    }

    if (InArgs._OnFilesDragOver.IsBound())
    {
        OnFilesDragOver = InArgs._OnFilesDragOver;
    }

    if (InArgs._OnFilesDragEnter.IsBound())
    {
        OnFilesDragEnter = InArgs._OnFilesDragEnter;
    }

    if (InArgs._OnFilesDragLeave.IsBound())
    {
        OnFilesDragLeave = InArgs._OnFilesDragLeave;
    }

    if (InArgs._OnFilesValidCheck.IsBound())
    {
        OnFilesValidCheck = InArgs._OnFilesValidCheck;
    }

    bCheckfileExtensions = InArgs._CheckFileExtensions;
    AllowedFileExtensions = InArgs._AllowedFileExtensions;

    ImageAttribute_Normal.Assign(*this, InArgs._Image_Normal);
    ImageAttribute_Hovered_Allowed.Assign(*this, InArgs._Image_Hovered_Allowed);
    ImageAttribute_Hovered_Denied.Assign(*this, InArgs._Image_Hovered_Denied);
    FontInfoAttribute.Assign(*this, InArgs._FontInfo);
    FontColorAndOpacityAttribute.Assign(*this, InArgs._FontColorAndOpacity);
    NormalMessageAttribute.Assign(*this, InArgs._NormalMessage);
    AllowedMessageAttribute.Assign(*this, InArgs._AllowedMessage);
    WrongFileTypeMessageAttribute.Assign(*this, InArgs._WrongFileTypeMessage);
    InvalidFileMessageAttribute.Assign(*this, InArgs._InvalidFileMessage);
    ColorAndOpacityAttribute.Assign(*this, InArgs._ColorAndOpacity);
    DesiredSizeOverrideAttribute.Assign(*this, InArgs._DesiredSizeOverride);


}

FILEDROPUMG_API void SFileDrop::OnDragEnter(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
    TSharedPtr<FExternalDragOperation> DragDropOp = DragDropEvent.GetOperationAs<FExternalDragOperation>();
    TArray<FString> DroppedFiles;
    if (DragDropOp)
    {
        if (DragDropOp.IsValid())
        {
            if (DragDropOp->HasFiles())
            {
                DroppedFiles = DragDropEvent.GetOperationAs<FExternalDragOperation>()->GetFiles();
            }
        }

        if (OnFilesDragEnter.IsBound())
        {
            OnFilesDragEnter.Execute(DroppedFiles);
        }
        bSetDragOver = true;
    }

    Super::OnDragEnter(MyGeometry, DragDropEvent);
}

FILEDROPUMG_API void SFileDrop::OnDragLeave(const FDragDropEvent& DragDropEvent)
{
    TSharedPtr<FExternalDragOperation> DragDropOp = DragDropEvent.GetOperationAs<FExternalDragOperation>();
    TArray<FString> DroppedFiles;
    if (DragDropOp)
    {
        if (DragDropOp.IsValid())
        {
            if (DragDropOp->HasFiles())
            {
                DroppedFiles = DragDropEvent.GetOperationAs<FExternalDragOperation>()->GetFiles();
            }
        }

        if (OnFilesDragLeave.IsBound())
        {
            OnFilesDragLeave.Execute(DroppedFiles);
        }

        bSetDragOver = false;
        bIsFileValid = false;
        bFileExtensionsValid = false;
    }

    Super::OnDragLeave(DragDropEvent);
}

FILEDROPUMG_API void SFileDrop::OnMouseLeave(const FPointerEvent& MouseEvent)
{
    bSetDragOver = false;
    bIsFileValid = false;
    bFileExtensionsValid = false;
    Super::OnMouseLeave(MouseEvent);
}

FILEDROPUMG_API void SFileDrop::SetOnFilesDropped(const FOnFilesDropEventHandler& InOnFilesDropped)
{
    OnFilesDropped = InOnFilesDropped;
}

FILEDROPUMG_API void SFileDrop::SetOnFilesDragOver(const FOnFilesDragOverEventHandler& InOnFilesDragOver)
{
    OnFilesDragOver = InOnFilesDragOver;
}

FILEDROPUMG_API void SFileDrop::SetOnFilesDragEnter(const FOnFilesDragEnterEventHandler& InOnFilesDragEnter)
{
    OnFilesDragEnter = InOnFilesDragEnter;
}

FILEDROPUMG_API void SFileDrop::SetOnFilesDragLeave(const FOnFilesDragLeaveEventHandler& InOnFilesDragLeave)
{
    OnFilesDragLeave = InOnFilesDragLeave;
}

FILEDROPUMG_API void SFileDrop::SetOnFilesValidCheck(const FOnFilesValidCheckHandler& InOnFilesValid)
{
    OnFilesValidCheck = InOnFilesValid;
}

FILEDROPUMG_API void SFileDrop::SetColorAndOpacity(TAttribute<FSlateColor> InColorAndOpacity)
{
    ColorAndOpacityAttribute.Assign(*this, MoveTemp(InColorAndOpacity));
}

FILEDROPUMG_API void SFileDrop::SetColorAndOpacity(FLinearColor InColorAndOpacity)
{
    ColorAndOpacityAttribute.Assign(*this, InColorAndOpacity);
}

FILEDROPUMG_API void SFileDrop::SetNormalImage(TAttribute<const FSlateBrush*> InImage)
{
    ImageAttribute_Normal.Assign(*this, MoveTemp(InImage));
}

FILEDROPUMG_API void SFileDrop::SetAllowedImage(TAttribute<const FSlateBrush*> InImage)
{
    ImageAttribute_Hovered_Allowed.Assign(*this, MoveTemp(InImage));
}

FILEDROPUMG_API void SFileDrop::SetDeniedImage(TAttribute<const FSlateBrush*> InImage)
{
    ImageAttribute_Hovered_Denied.Assign(*this, MoveTemp(InImage));
}

FILEDROPUMG_API void SFileDrop::InvalidateFileDrop()
{
    Invalidate(EInvalidateWidgetReason::Layout);
}

FILEDROPUMG_API void SFileDrop::SetFont(TAttribute<FSlateFontInfo> InFontInfo)
{
    FontInfoAttribute.Assign(*this, MoveTemp(InFontInfo));
}

FILEDROPUMG_API void SFileDrop::SetFontColorAndOpacity(TAttribute<FSlateColor> InColorAndOpacity)
{
    FontColorAndOpacityAttribute.Assign(*this, MoveTemp(InColorAndOpacity));
}

FILEDROPUMG_API void SFileDrop::SetFontColorAndOpacity(FLinearColor InColorAndOpacity)
{
    FontColorAndOpacityAttribute.Set(*this, InColorAndOpacity);
}

FILEDROPUMG_API void SFileDrop::SetDesiredSizeOverride(TAttribute<TOptional<FVector2D>> InDesiredSizeOverride)
{
    DesiredSizeOverrideAttribute.Assign(*this, MoveTemp(InDesiredSizeOverride));
}

FILEDROPUMG_API void SFileDrop::SetNormalMessage(TAttribute<FText> InMessage)
{
    bIsAttributeNormalMessageBound = InMessage.IsBound();
    NormalMessageAttribute.Assign(*this, MoveTemp(InMessage));
}

FILEDROPUMG_API void SFileDrop::SetAllowedMessage(TAttribute<FText> InMessage)
{
    bIsAttributeAllowedMessageBound = InMessage.IsBound();
    AllowedMessageAttribute.Assign(*this, MoveTemp(InMessage));
}

FILEDROPUMG_API void SFileDrop::SetWrongFileTypeMessage(TAttribute<FText> InMessage)
{
    bIsAttributeWrongFileTypeMessageBound = InMessage.IsBound();
    WrongFileTypeMessageAttribute.Assign(*this, MoveTemp(InMessage));
}

FILEDROPUMG_API void SFileDrop::SetInvalidFileMessage(TAttribute<FText> InMessage)
{
    bIsAttributeInvalidFileMessageBound = InMessage.IsBound();
    InvalidFileMessageAttribute.Assign(*this, MoveTemp(InMessage));
}

FILEDROPUMG_API bool SFileDrop::IsDropAllowed() const
{
    return IsFileExtensionValid() && IsFileValid();
}

FILEDROPUMG_API bool SFileDrop::IsFileExtensionValid() const
{
    return bFileExtensionsValid;
}

FILEDROPUMG_API bool SFileDrop::IsFileValid() const
{
    return bIsFileValid;
}

FILEDROPUMG_API void SFileDrop::SetCheckFileExtension(bool bCheckFileExtension)
{
    bCheckfileExtensions = bCheckFileExtension;
}

FILEDROPUMG_API void SFileDrop::SetAllowedFileExtensions(TArray<FString> InAllowedFileExtensions)
{
    this->AllowedFileExtensions = InAllowedFileExtensions;
}

FReply SFileDrop::OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{

    if (DragDropEvent.GetOperationAs<FExternalDragOperation>())
    {
        if (IsDropAllowed())
        {
            bSetDragOver = false;
            bIsFileValid = false;
            bFileExtensionsValid = false;
            TArray<FString> DroppedFiles = DragDropEvent.GetOperationAs<FExternalDragOperation>()->GetFiles();
            HandleFileDrop(DroppedFiles);
        }
        else
        {
            bSetDragOver = false;
        }
        return FReply::Handled();
    }
    return Super::OnDrop(MyGeometry, DragDropEvent);
}

FReply SFileDrop::OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{

    TSharedPtr<FExternalDragOperation> DragDropOperation = DragDropEvent.GetOperationAs<FExternalDragOperation>();
    if (DragDropOperation.IsValid())
    {
        if (DragDropOperation->HasFiles())
        {
            TArray<FString> DroppedFiles = DragDropOperation->GetFiles();

            bFileExtensionsValid = true;
            //check file extensions
            if (bCheckfileExtensions)
            {
                for (const FString& File : DroppedFiles)
                {
                    FString Extension = FPaths::GetExtension(File);
                    if (!AllowedFileExtensions.Contains(Extension))
                    {
                        bFileExtensionsValid = false;
                        break;
                    }
                }
            }

            if (bFileExtensionsValid)
            {
                //check custom file validation.
                if (OnFilesValidCheck.IsBound())
                {
                    bIsFileValid = OnFilesValidCheck.Execute(DroppedFiles);
                }
                else
                {
                    bIsFileValid = true;
                }
            }
            else
            {
                bIsFileValid = false;
            }
        }
        return FReply::Handled();
    }
    return Super::OnDragOver(MyGeometry, DragDropEvent);
}

void SFileDrop::HandleFileDrop(const TArray<FString>& Files)
{
    OnFilesDropped.ExecuteIfBound(Files);
}

FVector2D SFileDrop::ComputeDesiredSize(float) const
{
    return FVector2D::ZeroVector;
}

int32 SFileDrop::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
    const FSlateBrush* ImageBrush = nullptr;

    //get mouse windows position and check if it is over the widget
    bool bMouseOver = IsDirectlyHovered();

    FText LocalText = const_cast<FText&>(GetNormalMessage());

    if (bSetDragOver && bMouseOver)
    {
        

        if (!IsFileValid())
        {
            LocalText = const_cast<FText&>(GetInvalidFileMessage());
            ImageBrush = ImageAttribute_Hovered_Denied.Get();
        }
        
        if (!IsFileExtensionValid())
        {
            LocalText = const_cast<FText&>(GetWrongFileTypeMessage());
            ImageBrush = ImageAttribute_Hovered_Denied.Get();
        }

        if(IsFileValid() && IsFileExtensionValid())
        {
            LocalText = const_cast<FText&>(GetAllowedMessage());
            ImageBrush = ImageAttribute_Hovered_Allowed.Get();          
        }
    }
    else
    {
        ImageBrush = ImageAttribute_Normal.Get();
    }

    const bool bIsEnabled = ShouldBeEnabled(bParentEnabled);
    const ESlateDrawEffect DrawEffects = bIsEnabled ? ESlateDrawEffect::None : ESlateDrawEffect::DisabledEffect;
    const FLinearColor WidgetColorAndOpacity = ColorAndOpacityAttribute.Get().GetColor(InWidgetStyle);
    if ((ImageBrush != nullptr) && (ImageBrush->DrawAs != ESlateBrushDrawType::NoDrawType))
    {
        const FLinearColor FinalColorAndOpacity(WidgetColorAndOpacity * ImageBrush->GetTint(InWidgetStyle));
        FSlateDrawElement::MakeBox(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), ImageBrush, DrawEffects, FinalColorAndOpacity);
    }

    const FSlateFontInfo FontInfo = FontInfoAttribute.Get();

    const TSharedRef<FSlateFontMeasure> FontMeasureService = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();

    const FVector2D TextSize = FontMeasureService->Measure(LocalText, FontInfo);
    const FLinearColor FinalColorAndOpacity(WidgetColorAndOpacity * FontColorAndOpacityAttribute.Get().GetColor(InWidgetStyle));
    FVector2D TextPosition = AllottedGeometry.GetLocalSize() / 2.0f - TextSize / 2.0f;
    FSlateDrawElement::MakeText(
        OutDrawElements,
        LayerId,
        AllottedGeometry.ToPaintGeometry(ImageBrush->ImageSize, FSlateLayoutTransform(TextPosition)),
        LocalText,
        FontInfo,
        DrawEffects,
        FinalColorAndOpacity
    );
    return LayerId;
}