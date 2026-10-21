// Copyright Epic Games, Inc. All Rights Reserved.

#include "Sketch/StageSketchTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSketchTypes)

EStageSketchElement FStageSketchStroke::GetElement() const
{
	switch (Kind)
	{
	case EStageSketchStrokeKind::Freehand:	return bClosed ? EStageSketchElement::Deck : EStageSketchElement::Truss;
	case EStageSketchStrokeKind::Shape:		return EStageSketchElement::Deck;
	case EStageSketchStrokeKind::Truss:		return EStageSketchElement::Truss;
	case EStageSketchStrokeKind::Fixture:	return EStageSketchElement::Fixture;
	default:								return EStageSketchElement::Count;
	}
}

int32 FStageSketchBuildPlan::GetPanelCount() const
{
	int32 Panels = 0;
	for (const FStageSketchDeckPlan& Deck : Decks)
	{
		Panels += Deck.Panels;
	}
	return Panels;
}

int32 FStageSketchBuildPlan::GetSegmentCount() const
{
	int32 Segments = 0;
	for (const FStageSketchTrussPlan& Truss : Trusses)
	{
		Segments += Truss.SegmentCount;
	}
	return Segments;
}
