// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageSketchMenus.h"

#define LOCTEXT_NAMESPACE "StageSketchMenus"

namespace StageSketchMenus
{
	namespace
	{
		constexpr float GridPresets[] = { 25.f, 50.f, 100.f };
		constexpr float SmoothingPresets[] = { 0.f, 0.5f, 0.8f };

		bool IsPreset(float Preset, float Value)
		{
			return FMath::IsNearlyEqual(Preset, Value, 0.01f);
		}

		FText DescribeGrid(float Cm)
		{
			return Cm >= 100.f && FMath::IsNearlyEqual(FMath::Fmod(Cm, 100.f), 0.f)
				? FText::Format(LOCTEXT("GridMetres", "{0} m"), FText::AsNumber(FMath::RoundToInt(Cm / 100.f)))
				: FText::Format(LOCTEXT("GridCm", "{0} cm"), FText::AsNumber(FMath::RoundToInt(Cm)));
		}

		/** Off, Low, High for the presets; a sanitized value between them reads as the nearest. */
		FText DescribeSmoothing(float Smoothing)
		{
			const FText Names[] = { LOCTEXT("SmoothOff", "Off"), LOCTEXT("SmoothLow", "Low"), LOCTEXT("SmoothHigh", "High") };
			static_assert(UE_ARRAY_COUNT(Names) == UE_ARRAY_COUNT(SmoothingPresets), "One name per smoothing preset.");
			int32 Nearest = 0;
			for (int32 Index = 1; Index < UE_ARRAY_COUNT(SmoothingPresets); ++Index)
			{
				if (FMath::Abs(SmoothingPresets[Index] - Smoothing) < FMath::Abs(SmoothingPresets[Nearest] - Smoothing))
				{
					Nearest = Index;
				}
			}
			return Names[Nearest];
		}

		FText DescribeElement(EStageSketchElement Element)
		{
			switch (Element)
			{
			case EStageSketchElement::Truss:	return LOCTEXT("ElementTruss", "Truss");
			case EStageSketchElement::Fixture:	return LOCTEXT("ElementFixture", "Fixture");
			default:							return LOCTEXT("ElementDeck", "Deck");
			}
		}
	}

	TConstArrayView<float> GetGridPresets()
	{
		return GridPresets;
	}

	TConstArrayView<float> GetSmoothingPresets()
	{
		return SmoothingPresets;
	}

	FText DescribeSnap(const FStageSketchSettings& Settings)
	{
		return FText::Format(LOCTEXT("SnapLabel", "Snap {0} · Smooth {1}"),
			Settings.bSnapToGrid ? DescribeGrid(Settings.GridStep) : LOCTEXT("SnapOff", "Off"), DescribeSmoothing(Settings.Smoothing));
	}

	FStageMenu MakeSnapMenu(const FStageSketchSettings& Settings, bool bAvailable, TFunction<void(const FSettingsEdit&)> Apply)
	{
		// Each entry hands over an edit of one field, not a copy of the settings as they were when the menu opened: the window
		// applies it to the current settings, and the sketch's setter sanitizes and broadcasts the change.
		const auto Changed = [&Apply](FSettingsEdit Edit) -> TFunction<void()>
		{
			return [Apply, Edit = MoveTemp(Edit)]()
			{
				if (Apply)
				{
					Apply(Edit);
				}
			};
		};

		FStageMenu Menu;
		FStageMenuSection& Grid = Menu.AddSection(TEXT("Grid"), LOCTEXT("GridSection", "Grid Step"));
		for (const float Preset : GridPresets)
		{
			Grid.Entries.Add(StageMenu::MakeRadio(DescribeGrid(Preset), LOCTEXT("GridTip", "Grid step: deck corners and fixtures snap to it."),
				IsPreset(Preset, Settings.GridStep), bAvailable, Changed([Preset](FStageSketchSettings& Next) { Next.GridStep = Preset; })));
		}
		Grid.Entries.Add(StageMenu::MakeToggle(LOCTEXT("SnapToGrid", "Snap to Grid"),
			LOCTEXT("SnapTip", "Snap clicked points to the grid. Truss lines always snap to whole sections of their truss."),
			Settings.bSnapToGrid, bAvailable, Changed([](FStageSketchSettings& Next) { Next.bSnapToGrid = !Next.bSnapToGrid; })));

		FStageMenuSection& Smooth = Menu.AddSection(TEXT("Smoothing"), LOCTEXT("SmoothingSection", "Freehand Smoothing"));
		for (const float Preset : SmoothingPresets)
		{
			Smooth.Entries.Add(StageMenu::MakeRadio(DescribeSmoothing(Preset), LOCTEXT("SmoothTip", "Freehand stabiliser: higher values trail the cursor for smoother strokes."),
				IsPreset(Preset, Settings.Smoothing), bAvailable, Changed([Preset](FStageSketchSettings& Next) { Next.Smoothing = Preset; })));
		}
		return Menu;
	}

	FStageMenu MakeEditMenu(bool bAvailable, bool bHasStrokes, TFunction<void(EEditCommand)> Run)
	{
		const auto Command = [&Run](EEditCommand Which) -> TFunction<void()>
		{
			return [Run, Which]()
			{
				if (Run)
				{
					Run(Which);
				}
			};
		};
		FStageMenu Menu;
		FStageMenuSection& Sketch = Menu.AddSection(TEXT("Sketch"), LOCTEXT("SketchSection", "Sketch"));
		Sketch.Entries.Add(StageMenu::MakeAction(LOCTEXT("UndoStroke", "Undo Stroke"),
			LOCTEXT("UndoStrokeTip", "Remove the last stroke from the sketch (the stage is not changed)."), bAvailable && bHasStrokes, Command(EEditCommand::UndoStroke)));
		Sketch.Entries.Add(StageMenu::MakeAction(LOCTEXT("Clear", "Clear Sketch"),
			LOCTEXT("ClearTip", "Remove every stroke from the sketch (the stage is not changed)."), bAvailable && bHasStrokes, Command(EEditCommand::Clear)));
		Menu.AddSection(TEXT("View"), LOCTEXT("ViewSection", "View")).Entries.Add(StageMenu::MakeAction(LOCTEXT("Frame", "Frame Sketch"),
			LOCTEXT("FrameTip", "Fit the view to the sketch. The wheel zooms, the right or middle button pans."), bAvailable, Command(EEditCommand::Frame)));
		return Menu;
	}

	FStageMenu MakeItemsMenu(TConstArrayView<FItemChoice> Choices, TFunction<void(EStageSketchElement, int32)> Pick)
	{
		FStageMenu Menu;
		FStageMenuSection& Section = Menu.AddSection(TEXT("Items"), LOCTEXT("ItemsSection", "Built From"));
		for (const FItemChoice& Choice : Choices)
		{
			TArray<FStageMenuEntry> Candidates;
			Candidates.Reserve(Choice.Names.Num());
			for (int32 Index = 0; Index < Choice.Names.Num(); ++Index)
			{
				Candidates.Add(StageMenu::MakeRadio(Choice.Names[Index], LOCTEXT("ItemTip", "The catalog item this element is built from."),
					Index == Choice.Chosen, true, [Pick, Element = Choice.Element, Index]()
					{
						if (Pick)
						{
							Pick(Element, Index);
						}
					}));
			}
			const FText Current = Choice.Names.IsValidIndex(Choice.Chosen) ? Choice.Names[Choice.Chosen] : LOCTEXT("NoItem", "none in catalog");
			Section.Entries.Add(StageMenu::MakeSubMenu(FText::Format(LOCTEXT("ItemLabel", "{0}: {1}"), DescribeElement(Choice.Element), Current),
				Choice.Names.IsEmpty() ? LOCTEXT("NoItemTip", "No catalog item can build this.") : LOCTEXT("ItemSubTip", "The catalog item new strokes of this kind are built from."),
				MoveTemp(Candidates)));
		}
		return Menu;
	}

	FText DescribePhoto(const FPhotoState& State)
	{
		return State.bImporting ? LOCTEXT("Reading", "Reading...") : LOCTEXT("Photo", "Photo");
	}

	FStageMenu MakePhotoMenu(const FPhotoState& State, TFunction<void(EPhotoCommand)> Run)
	{
		const auto Command = [&Run](EPhotoCommand Which) -> TFunction<void()>
		{
			return [Run, Which]()
			{
				if (Run)
				{
					Run(Which);
				}
			};
		};
		const bool bIdle = State.bAvailable && !State.bImporting;

		FStageMenu Menu;
		FStageMenuSection& Photo = Menu.AddSection(TEXT("Photo"), LOCTEXT("PhotoSection", "Photo or Scan"));
		Photo.Entries.Add(StageMenu::MakeAction(LOCTEXT("ImportPhoto", "Import Photo..."),
			LOCTEXT("ImportPhotoTip", "Read a PNG or JPEG of a stage drawn from above (paper, whiteboard, printed plan): closed outlines become decks, lines become truss, dots and small circles become fixtures. The strokes are added to the sketch to check before GENERATE 3D. PHOTO WIDTH sets its real width."),
			bIdle && State.bCanBrowse, Command(EPhotoCommand::Import)));
		Photo.Entries.Add(StageMenu::MakeAction(LOCTEXT("RemovePhoto", "Remove Photo"),
			LOCTEXT("RemovePhotoTip", "Take the photo away from under the canvas (the strokes stay)."), bIdle && State.bHasPhoto, Command(EPhotoCommand::Remove)));
		Photo.Entries.Add(StageMenu::MakeToggle(LOCTEXT("ShowPhoto", "Show Under the Sketch"),
			LOCTEXT("ShowPhotoTip", "Show the imported photo faintly under the strokes, to check and correct them."),
			State.bHasPhoto && State.bShown, State.bHasPhoto, Command(EPhotoCommand::ToggleShown)));

		FStageMenuSection& Reader = Menu.AddSection(TEXT("Reader"), LOCTEXT("ReaderSection", "Read By"));
		Reader.Entries.Add(StageMenu::MakeRadio(LOCTEXT("ReaderLocal", "This PC"),
			LOCTEXT("ReaderLocalTip", "Trace the lines on this computer: offline, best with dark lines on plain paper."),
			State.Reader == EStageSketchReader::Local, bIdle, Command(EPhotoCommand::ReadHere)));
		Reader.Entries.Add(StageMenu::MakeRadio(LOCTEXT("ReaderClaude", "Claude"),
			State.bOnline ? LOCTEXT("ReaderClaudeTip", "Let Claude read the drawing (sends the image to Anthropic); better with photos, labels and messy sketches. Falls back to this computer.")
				: State.OnlineUnavailable,
			State.Reader == EStageSketchReader::Claude, bIdle && State.bOnline, Command(EPhotoCommand::ReadByClaude)));
		return Menu;
	}
}

#undef LOCTEXT_NAMESPACE
