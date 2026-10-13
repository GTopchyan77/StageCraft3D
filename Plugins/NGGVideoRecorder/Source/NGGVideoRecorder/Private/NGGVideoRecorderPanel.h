// Copyright NGG. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateTypes.h"
#include "Widgets/SCompoundWidget.h"

class UNGGVideoRecorderSubsystem;

/**
 * The recorder control panel: record/stop, live status with elapsed time, and an export button that
 * reveals the finished MP4 in Explorer.
 *
 * Pure Slate so the plugin stays content-free and the panel works identically in PIE and packaged
 * builds. The widget spans the whole viewport (hit-test invisible outside its content) and parks the
 * actual panel bottom-right. Shown/hidden by UNGGVideoRecorderSubsystem (see ShowRecorderUI /
 * ToggleUIKey).
 *
 * Note the panel is part of the presented image, so while it is on screen it is burned into the
 * recording like any other UI. bHidePanelWhileRecording in the settings collapses it during capture.
 */
class SNGGRecorderPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SNGGRecorderPanel)
		{
		}

	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, UNGGVideoRecorderSubsystem* InSubsystem);

private:
	FReply OnRecordClicked();
	FReply OnExportClicked();

	FText GetStatusText() const;
	FText GetRecordLabel() const;
	FText GetLastFileText() const;
	FText GetLastFileTooltip() const;
	FSlateColor GetDotColor() const;
	bool IsExportEnabled() const;
	EVisibility GetPanelVisibility() const;

	bool IsRecording() const;
	const FString& GetLastPath() const;

	/** Rebuilds the file-name texts, but only when the recording path actually changed. */
	void RefreshFileTexts() const;

	TWeakObjectPtr<UNGGVideoRecorderSubsystem> Subsystem;

	/** Brushes and styles must outlive the widgets referencing them, so they live on the panel. */
	FSlateBrush PanelBrush;
	FSlateBrush DotBrush;
	FButtonStyle RecordButtonStyle;
	FButtonStyle ExportButtonStyle;

	/**
	 * Slate polls the text attributes every frame; these caches keep that from allocating a fresh
	 * FText (and re-wrapping the file-name text block) for values that change at most once a second.
	 */
	mutable FString CachedPath;
	mutable bool bFileTextsCached = false;
	mutable FText CachedFileText;
	mutable FText CachedFileTooltip;
	mutable int32 CachedStatusSeconds = -1;
	mutable FText CachedStatusText;
};
