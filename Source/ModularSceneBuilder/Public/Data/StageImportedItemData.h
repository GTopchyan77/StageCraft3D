// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/BaseItemData.h"
#include "StageImportedItemData.generated.h"

/**
 * Catalog entry for a model the user imported from a file while the app runs (UStageMeshImportSubsystem).
 *
 * Lives only in memory (a never-saved package under StageMeshImport::PackageRoot) and is rebuilt from the source file on
 * the next start.
 * Its object path is derived from the source file path, so scene files and undo history that name it resolve again
 * once the file is re-imported. It spawns as a plain prop (AModularBaseActor with the imported mesh), is free, and has no
 * equipment specs: weight and power are unknown for an arbitrary model.
 *
 * Created and configured only by UStageMeshImportSubsystem; everything is read-only afterwards.
 */
UCLASS(Transient)
class MODULARSCENEBUILDER_API UStageImportedItemData : public UBaseItemData
{
	GENERATED_BODY()

public:
	/** Fills the catalog fields from an imported mesh. Call once, right after NewObject. */
	void InitializeFromImport(class UStaticMesh& InMesh, const FString& InSourceFile);

	//~ Begin UBaseItemData Interface
	virtual bool RequiresAssetStreaming() const override { return false; }
	virtual FText GetCatalogSubtitle() const override;
	virtual UObject* GetCatalogIcon() const override;
	//~ End UBaseItemData Interface

	const FString& GetSourceFile() const { return SourceFile; }

	/** Upper-case source extension, e.g. "GLB". */
	FString GetSourceFormat() const;

	/** The Library thumbnail, rendered once after import (null without a renderer). */
	void SetThumbnail(class UTextureRenderTarget2D* InThumbnail);

	/** Size of the model's bounding box in centimetres. */
	const FVector& GetBoundsSize() const { return BoundsSize; }

private:
	UPROPERTY(VisibleInstanceOnly, Category = "Import")
	FString SourceFile;

	UPROPERTY(VisibleInstanceOnly, Category = "Import")
	FVector BoundsSize = FVector::ZeroVector;

	UPROPERTY(Transient)
	TObjectPtr<class UTextureRenderTarget2D> Thumbnail = nullptr;
};
