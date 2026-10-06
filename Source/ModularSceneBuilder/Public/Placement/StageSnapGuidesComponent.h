// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "Placement/StageSnapTypes.h"
#include "StageSnapGuidesComponent.generated.h"

/**
 * Draws object-snap alignment guides as thin lines on top of the scene. Used by the placement preview
 * (while placing) and the transform gizmo (while dragging), so both show snaps the same way.
 *
 * Pure presentation: no tick, no collision. Lines are created on first use (at most MaxGuides) and
 * positioned in world space, so they ignore whatever the owner is attached to. The material is the
 * gizmo handle material (unlit, no depth test, "GizmoColor" parameter): guides are never hidden by meshes.
 */
UCLASS(ClassGroup = (StageCraft))
class MODULARSCENEBUILDER_API UStageSnapGuidesComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UStageSnapGuidesComponent();

	/** One line per guide (extra guides beyond MaxGuides are ignored); unused lines hide. */
	void ShowGuides(TConstArrayView<FStageSnapGuide> Guides);

	/** Number of lines currently shown (diagnostics and tests). */
	int32 GetVisibleGuideCount() const;

	/** Hides every line. Idempotent. */
	void HideGuides();

	static constexpr int32 MaxGuides = 3;

protected:
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Snapping")
	TObjectPtr<class UStaticMesh> LineMesh = nullptr;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Snapping")
	TObjectPtr<class UMaterialInterface> LineMaterial = nullptr;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Snapping")
	FLinearColor GuideColor = FLinearColor(1.0f, 0.2f, 0.85f);

private:
	class UStaticMeshComponent* GetOrCreateLine(int32 Index);

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStaticMeshComponent>> Lines;

	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> LineMaterialInstance = nullptr;
};
