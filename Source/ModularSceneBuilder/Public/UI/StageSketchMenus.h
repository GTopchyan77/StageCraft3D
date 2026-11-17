// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sketch/StageSketchTypes.h"
#include "UI/StageMenuModel.h"

/**
 * The Sketch to Stage window's toolbar dropdowns (UStageSketchCanvasWidget, STATE.md #100), as pure functions of the sketch's
 * state: Edit (undo stroke, clear, frame), Snap (grid step, snap to grid, freehand smoothing), Items (the catalog item per element)
 * and Photo (import, remove, show, who reads it). The window passes its state and callbacks into its own validated handlers; tests
 * call them without a widget or a world.
 */
namespace StageSketchMenus
{
	/** Grid steps (cm) and freehand smoothing amounts the Snap menu offers, in menu order. */
	TConstArrayView<float> GetGridPresets();
	TConstArrayView<float> GetSmoothingPresets();

	/** The Snap button's label: "Snap 50 cm · Smooth Low", "Snap Off · Smooth Off". */
	FText DescribeSnap(const FStageSketchSettings& Settings);

	/** One change to the settings: applied by the window to the settings as they are when the entry is picked. */
	using FSettingsEdit = TFunction<void(FStageSketchSettings&)>;

	/**
	 * Grid step and smoothing presets (radio) and Snap to Grid (toggle), checked from Settings. A pick calls Apply with an edit of that
	 * one field, so a setting changed elsewhere while the menu was open is not reverted.
	 */
	FStageMenu MakeSnapMenu(const FStageSketchSettings& Settings, bool bAvailable, TFunction<void(const FSettingsEdit&)> Apply);

	enum class EEditCommand : uint8
	{
		UndoStroke,
		Clear,
		Frame,
	};

	/** Undo Stroke and Clear Sketch (only with strokes), Frame Sketch. */
	FStageMenu MakeEditMenu(bool bAvailable, bool bHasStrokes, TFunction<void(EEditCommand)> Run);

	/** The catalog items that can build one element, and which of them the sketch uses. */
	struct FItemChoice
	{
		EStageSketchElement Element = EStageSketchElement::Deck;
		TArray<FText> Names;
		int32 Chosen = INDEX_NONE;
	};

	/** One submenu per element ("Deck: Stage Deck 2x1") listing its candidates (radio). A pick calls Pick with the candidate's index. */
	FStageMenu MakeItemsMenu(TConstArrayView<FItemChoice> Choices, TFunction<void(EStageSketchElement, int32)> Pick);

	struct FPhotoState
	{
		/** The sketch exists in this level. */
		bool bAvailable = false;
		/** The OS file dialog can open (StageFileDialog::IsAvailable). */
		bool bCanBrowse = false;
		bool bImporting = false;
		bool bHasPhoto = false;
		bool bShown = false;
		/** Claude can read the photo; otherwise OnlineUnavailable says why. */
		bool bOnline = false;
		FText OnlineUnavailable;
		EStageSketchReader Reader = EStageSketchReader::Local;
	};

	enum class EPhotoCommand : uint8
	{
		Import,
		Remove,
		ToggleShown,
		ReadHere,
		ReadByClaude,
	};

	/** The Photo button's label: "Photo", or "Reading..." while an import runs. */
	FText DescribePhoto(const FPhotoState& State);

	/** Import Photo..., Remove Photo, Show Under the Sketch (toggle); Read By: This PC / Claude (radio). */
	FStageMenu MakePhotoMenu(const FPhotoState& State, TFunction<void(EPhotoCommand)> Run);
}
