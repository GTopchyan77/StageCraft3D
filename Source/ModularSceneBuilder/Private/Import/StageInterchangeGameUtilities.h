// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "InterchangeEditorUtilitiesBase.h"
#include "StageInterchangeGameUtilities.generated.h"

/**
 * Tells Interchange it is importing inside a running game.
 *
 * Interchange picks its runtime code (static meshes built from mesh descriptions, dynamic material instances) by asking its
 * editor-utilities object IsRuntimeOrPIE. In a standalone game launched from editor binaries (-game, "Standalone Game"), the
 * editor plugin that answers that question is not loaded and the default answer is "editor", while parts of the generic
 * pipeline check IsRunningGame() instead. The mix creates editor-only material instances and trips
 * check(GIsEditor) in UMaterialInstanceConstant (OBJ materials). UStageMeshImportSubsystem installs this class only in that
 * situation, so standalone sessions import exactly as a packaged game does. Packaged builds and the editor are untouched.
 */
UCLASS(Transient)
class UStageInterchangeGameUtilities : public UInterchangeEditorUtilitiesBase
{
	GENERATED_BODY()

public:
	virtual bool IsRuntimeOrPIE() const override { return true; }
};
