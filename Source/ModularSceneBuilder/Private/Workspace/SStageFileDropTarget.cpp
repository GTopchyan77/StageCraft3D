// Copyright Epic Games, Inc. All Rights Reserved.

#include "Workspace/SStageFileDropTarget.h"

#include "Input/DragAndDrop.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "StageFileDropTarget"

void SStageFileDropTarget::Construct(const FArguments& InArgs)
{
	CanDropFiles = InArgs._CanDropFiles;
	OnFilesDropped = InArgs._OnFilesDropped;

	// The shell has no theme asset of its own; it uses the palette defaults like the menu bar.
	const UStageCraftUITheme& Theme = StageCraftWidgetStyle::ResolveTheme(nullptr);
	AcceptColor = Theme.AccentColor;
	RefuseColor = Theme.WarningColor;
	AcceptBrush = StageCraftWidgetStyle::MakeBox(StageCraftWidgetStyle::WithAlpha(Theme.WindowBackground, 0.72f), 4.f, AcceptColor, 2.f);
	RefuseBrush = StageCraftWidgetStyle::MakeBox(StageCraftWidgetStyle::WithAlpha(Theme.WindowBackground, 0.72f), 4.f, RefuseColor, 2.f);

	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			InArgs._Content.Widget
		]
		+ SOverlay::Slot()
		.Padding(6.f)
		[
			SNew(SBorder)
			.Visibility(this, &SStageFileDropTarget::GetHintVisibility)
			.BorderImage(this, &SStageFileDropTarget::GetHintBrush)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				.Padding(0.f, 0.f, 0.f, 8.f)
				[
					SNew(SImage)
					.Image(StageCraftWidgetStyle::AppBrush(TEXT("Icons.Import")))
					.ColorAndOpacity(this, &SStageFileDropTarget::GetHintColor)
					.DesiredSizeOverride(FVector2D(32.0, 32.0))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SStageFileDropTarget::GetHintText)
					.Font(StageCraftWidgetStyle::MakeFont(11, TEXT("Bold")))
					.ColorAndOpacity(this, &SStageFileDropTarget::GetHintColor)
					.Justification(ETextJustify::Center)
				]
			]
		]
	];
}

const TArray<FString>* SStageFileDropTarget::GetDraggedFiles(const FDragDropEvent& DragDropEvent)
{
	const TSharedPtr<FExternalDragOperation> Operation = DragDropEvent.GetOperationAs<FExternalDragOperation>();
	return Operation.IsValid() && Operation->HasFiles() ? &Operation->GetFiles() : nullptr;
}

void SStageFileDropTarget::OnDragEnter(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	if (const TArray<FString>* Files = GetDraggedFiles(DragDropEvent))
	{
		HoverState = CanDropFiles.IsBound() && CanDropFiles.Execute(*Files) ? EHoverState::Accepted : EHoverState::Refused;
	}
}

void SStageFileDropTarget::OnDragLeave(const FDragDropEvent& DragDropEvent)
{
	HoverState = EHoverState::None;
}

FReply SStageFileDropTarget::OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	// Handling the hover is what tells the OS this window takes the drop (copy cursor).
	return GetDraggedFiles(DragDropEvent) ? FReply::Handled() : FReply::Unhandled();
}

FReply SStageFileDropTarget::OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	HoverState = EHoverState::None;
	const TArray<FString>* Files = GetDraggedFiles(DragDropEvent);
	if (!Files)
	{
		return FReply::Unhandled();
	}
	OnFilesDropped.ExecuteIfBound(*Files);
	return FReply::Handled();
}

EVisibility SStageFileDropTarget::GetHintVisibility() const
{
	return HoverState == EHoverState::None ? EVisibility::Collapsed : EVisibility::HitTestInvisible;
}

FText SStageFileDropTarget::GetHintText() const
{
	return HoverState == EHoverState::Accepted
		? LOCTEXT("DropToImport", "Drop to import into the Library")
		: LOCTEXT("CannotImport", "Not a 3D model format StageCraft can import");
}

FSlateColor SStageFileDropTarget::GetHintColor() const
{
	return HoverState == EHoverState::Accepted ? AcceptColor : RefuseColor;
}

const FSlateBrush* SStageFileDropTarget::GetHintBrush() const
{
	return HoverState == EHoverState::Accepted ? &AcceptBrush : &RefuseBrush;
}

#undef LOCTEXT_NAMESPACE
