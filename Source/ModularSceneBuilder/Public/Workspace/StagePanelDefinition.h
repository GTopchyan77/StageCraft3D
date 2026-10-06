// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "StagePanelDefinition.generated.h"

/**
 * One dockable workspace panel, described as data (Docs/ADR/0001-dockable-workspace.md §3.1).
 *
 * Adding a panel is content-only:
 * 1. Add a StageCraft.Panel.* tag.
 * 2. Create a data asset of this class under /Game/StageCraft/UI/Panels. The Asset Manager registers it through the StagePanel entry in DefaultGame.ini.
 * The workspace then offers the panel in the Window menu and can save it in layouts.
 *
 * The primary asset ID's name is the panel tag. The workspace can therefore register dock tab spawners from the
 * Asset Manager scan alone, before any definition is loaded, so a saved layout never drops a panel whose
 * definition is still streaming in. A second asset with the same tag is a duplicate primary asset ID, and the Asset Manager reports it.
 *
 * The 3D viewport is not a definition. There is exactly one game viewport, and the shell hosts it as a built-in panel (StageCraft.Panel.Viewport).
 */
UCLASS(BlueprintType, Const)
class MODULARSCENEBUILDER_API UStagePanelDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	static const FPrimaryAssetType PanelAssetType;

	/** Bundle that holds WidgetClass. The workspace loads it with the definition. */
	static const FName UIBundle;

	//~ Begin UObject Interface
	virtual FPrimaryAssetId GetPrimaryAssetId() const override;
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
	//~ End UObject Interface

	/** Identity of the panel. It is also the dock tab ID saved in layout files, so renaming it drops the panel from saved layouts. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Panel", meta = (Categories = "StageCraft.Panel"))
	FGameplayTag PanelTag;

	/** Tab label and Window menu entry. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Panel")
	FText DisplayName;

	/**
	 * The panel's UMG content. It is created with the local AModularPlayerController as its owning player. Panels reach the
	 * selection and the request bridge through GetOwningPlayer, as UStageParameterViewWidget does.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Panel", meta = (AssetBundles = "UI"))
	TSoftClassPtr<class UUserWidget> WidgetClass;
};
