// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateBrush.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

DECLARE_DELEGATE_RetVal_OneParam(bool, FStageCanDropFiles, const TArray<FString>& /*Files*/);
DECLARE_DELEGATE_OneParam(FStageFilesDropped, const TArray<FString>& /*Files*/);

/**
 * Wraps a panel so files dragged in from the operating system (Explorer) can be dropped on it.
 *
 * While files hover the panel, an overlay says whether they will be imported (CanDropFiles) or not. A drop always hands
 * the files to OnFilesDropped, also unsupported ones, so the owner can say why they were refused. Drags from inside the app
 * (tabs, widgets) are ignored and pass through. Game thread only.
 */
class SStageFileDropTarget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SStageFileDropTarget) {}
		SLATE_DEFAULT_SLOT(FArguments, Content)
		SLATE_EVENT(FStageCanDropFiles, CanDropFiles)
		SLATE_EVENT(FStageFilesDropped, OnFilesDropped)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	//~ Begin SWidget Interface
	virtual void OnDragEnter(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	virtual void OnDragLeave(const FDragDropEvent& DragDropEvent) override;
	virtual FReply OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	virtual FReply OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	//~ End SWidget Interface

private:
	/** The dragged files, or null for any other drag. */
	static const TArray<FString>* GetDraggedFiles(const FDragDropEvent& DragDropEvent);

	EVisibility GetHintVisibility() const;
	FText GetHintText() const;
	FSlateColor GetHintColor() const;
	const FSlateBrush* GetHintBrush() const;

	FStageCanDropFiles CanDropFiles;
	FStageFilesDropped OnFilesDropped;

	enum class EHoverState : uint8
	{
		None,
		Accepted,
		Refused,
	};
	EHoverState HoverState = EHoverState::None;

	FSlateBrush AcceptBrush;
	FSlateBrush RefuseBrush;
	FLinearColor AcceptColor;
	FLinearColor RefuseColor;
};
