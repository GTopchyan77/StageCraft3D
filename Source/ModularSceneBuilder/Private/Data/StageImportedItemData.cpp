// Copyright Epic Games, Inc. All Rights Reserved.

#include "Data/StageImportedItemData.h"

#include "Actors/ModularBaseActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Import/StageMeshImportTypes.h"
#include "Misc/Paths.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageImportedItemData)

void UStageImportedItemData::InitializeFromImport(UStaticMesh& InMesh, const FString& InSourceFile)
{
	SourceFile = InSourceFile;
	BoundsSize = InMesh.GetBoundingBox().GetSize();

	DisplayName = StageMeshImport::MakeDisplayName(InSourceFile);
	Description = FText::FromString(InSourceFile);
	CategoryTag = StageCraftTags::Category_Imported;
	ItemType = EStageItemType::Prop;
	ActorClass = AModularBaseActor::StaticClass();
	Mesh = &InMesh;
}

FText UStageImportedItemData::GetCatalogSubtitle() const
{
	// "GLB · 1.20 × 0.80 × 2.00 m": what it is and how big, the two things to know before placing an unknown model.
	FNumberFormattingOptions Metres;
	Metres.MinimumFractionalDigits = 2;
	Metres.MaximumFractionalDigits = 2;
	return FText::Format(NSLOCTEXT("StageImportedItem", "Subtitle", "{0} · {1} × {2} × {3} m"),
		FText::FromString(GetSourceFormat()),
		FText::AsNumber(BoundsSize.X / 100.0, &Metres), FText::AsNumber(BoundsSize.Y / 100.0, &Metres), FText::AsNumber(BoundsSize.Z / 100.0, &Metres));
}

UObject* UStageImportedItemData::GetCatalogIcon() const
{
	return Thumbnail;
}

void UStageImportedItemData::SetThumbnail(UTextureRenderTarget2D* InThumbnail)
{
	Thumbnail = InThumbnail;
}

FString UStageImportedItemData::GetSourceFormat() const
{
	return FPaths::GetExtension(SourceFile).ToUpper();
}
