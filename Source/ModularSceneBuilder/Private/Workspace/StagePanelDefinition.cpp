// Copyright Epic Games, Inc. All Rights Reserved.

#include "Workspace/StagePanelDefinition.h"

#include "Blueprint/UserWidget.h"
#include "Workspace/StageWorkspaceTypes.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(StagePanelDefinition)

#define LOCTEXT_NAMESPACE "StagePanelDefinition"

const FPrimaryAssetType UStagePanelDefinition::PanelAssetType(TEXT("StagePanel"));
const FName UStagePanelDefinition::UIBundle(TEXT("UI"));

FPrimaryAssetId UStagePanelDefinition::GetPrimaryAssetId() const
{
	// Invalid until a tag is set, so the Asset Manager never registers an anonymous panel.
	return PanelTag.IsValid() ? FPrimaryAssetId(PanelAssetType, PanelTag.GetTagName()) : FPrimaryAssetId();
}

#if WITH_EDITOR
EDataValidationResult UStagePanelDefinition::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);

	if (!PanelTag.MatchesTag(StageCraftTags::Panel) || PanelTag == StageCraftTags::Panel)
	{
		Context.AddError(LOCTEXT("BadTag", "PanelTag must be a child of StageCraft.Panel."));
		Result = EDataValidationResult::Invalid;
	}
	else if (PanelTag == StageCraftTags::Panel_Viewport)
	{
		Context.AddError(LOCTEXT("ViewportTag", "StageCraft.Panel.Viewport is the built-in 3D viewport and cannot be defined by an asset."));
		Result = EDataValidationResult::Invalid;
	}

	if (WidgetClass.IsNull())
	{
		Context.AddError(LOCTEXT("NoWidget", "WidgetClass is required."));
		Result = EDataValidationResult::Invalid;
	}

	if (DisplayName.IsEmpty())
	{
		Context.AddError(LOCTEXT("NoName", "DisplayName is required; it is the tab label."));
		Result = EDataValidationResult::Invalid;
	}

	return Result;
}
#endif

#undef LOCTEXT_NAMESPACE
