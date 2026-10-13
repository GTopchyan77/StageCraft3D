// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved
#include "VatAssetWidget.h"
#include "AssetThumbnail.h"
#include "OverCrowdEditorLibrary.h"
#include "OverCrowdRuntimeLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"

#include "Styling/SlateIconFinder.h"
#include "Widgets/SCanvas.h"

#define LOCTEXT_NAMESPACE "OverCrowdEditor"
	
void SVatAssetWidget::Construct(const FArguments& InArgs)
{
	UVatMeshPairing* InAsset = InArgs._InAsset;
	ThumbnailScale = InArgs._InScale;
	Justify = InArgs._InJustify;
	OnSelected = InArgs._OnSelected;
	WardrobeID = InArgs._InWardrobeID;
	if(!InAsset)
		return;

	VatObject = InAsset;

	FString AssetName = VatObject->GetName();
	// FSlateFontInfo TitleFont = FAppStyle::Get().GetFontStyle("NormalFont");
	// TitleFont.Size = 7.5;

	// FSlateFontInfo HoverFont = FAppStyle::Get().GetFontStyle("NormalFont");
	// HoverFont.Size = 15;
	// HoverFont.OutlineSettings.OutlineSize = 1;
	// HoverFont.OutlineSettings.OutlineColor = FLinearColor::Black;
	//
	auto FindAssetBrush = *FCoreStyle::Get().GetBrush("Kismet.Tabs.FindResults");
	auto SkelMeshBrush = *FCoreStyle::Get().GetBrush("ShowFlagsMenu.SkeletalMeshes");
	auto VatMeshBrush = *FCoreStyle::Get().GetBrush("ShowFlagsMenu.StaticMeshes");
	auto VatAnimBrush = *FCoreStyle::Get().GetBrush("Graph.AnimationResultNode.Body");

	BackgroundBrush = *FCoreStyle::Get().GetBrush("NoBorder");
	BackgroundBrush.DrawAs = ESlateBrushDrawType::Type::Box;
	BackgroundBrush.TintColor = FColor::Black;
	
	FindAssetBrush.TintColor = FColor(255,255,255);
	GotoVatAssetButtonStyle.SetNormal(FindAssetBrush);
	GotoVatAssetButtonStyle.SetPressed(FindAssetBrush);
	FindAssetBrush.TintColor = FColor(128,128,128);
	GotoVatAssetButtonStyle.SetHovered(FindAssetBrush);

	SkelMeshBrush.TintColor = FColor(255,255,255);
	GotoSkelMeshButtonStyle.SetNormal(SkelMeshBrush);
	GotoSkelMeshButtonStyle.SetPressed(SkelMeshBrush);
	SkelMeshBrush.TintColor = FColor(128,128,128);
	GotoSkelMeshButtonStyle.SetHovered(SkelMeshBrush);

	VatMeshBrush.TintColor = FColor(255,255,255);
	GotoVatMeshButtonStyle.SetNormal(VatMeshBrush);
	GotoVatMeshButtonStyle.SetPressed(VatMeshBrush);
	VatMeshBrush.TintColor = FColor(128,128,128);
	GotoVatMeshButtonStyle.SetHovered(VatMeshBrush);
	
	VatAnimBrush.TintColor = FColor(255,255,255);
	GotoVatAnimButtonStyle.SetNormal(VatAnimBrush);
	GotoVatAnimButtonStyle.SetPressed(VatAnimBrush);
	VatAnimBrush.TintColor = FColor(128,128,128);
	GotoVatAnimButtonStyle.SetHovered(VatAnimBrush);
	
	auto BorderBrush = new FSlateRoundedBoxBrush(FStyleColors::Transparent, FStyleColors::AccentYellow, 5.0f);
	BorderBrush->OutlineSettings.CornerRadii = FVector4(10,10,10,10);
	BorderBrush->OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
	
	auto Thumbnail = SNew(SOverCrowdThumbnailWidget)
		.InAsset(VatObject)
		.InScale(ThumbnailScale)
		.InJustify(Justify)
		.InTextYOffset(0)
		.InToolTipText(LOCTEXT("SelectMultipleTip", "(Ctrl + LeftMouse) to select multiple"))
		.OnClicked_Lambda([this]()
		{
			bSelected = !bSelected;
			if(VatObject)
			{
				if(OnSelected.IsBound())
					OnSelected.Execute(VatObject,bSelected);
			}

			return FReply::Handled();
		});
	
	this->ChildSlot
	[
		SNew(SBox)
		.WidthOverride(256 * ThumbnailScale)  
		.HeightOverride(256 * ThumbnailScale) 
		[
			SNew(SOverlay)
			+SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill)
			[
				SNew(SBorder).BorderImage(&BackgroundBrush)
				.ColorAndOpacity(FLinearColor(0.2, 0.2, 0.2,1))
			]
			+SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().FillHeight(0.9)
				[
					SNew(SOverlay)
					+SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill).ZOrder(-1)
					[
						SNew(SBox)
						[
							Thumbnail
						]	
					]
				]
				+ SVerticalBox::Slot().FillHeight(0.2).Padding(FMargin(5,0,5,0))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill).Padding(5,3,5,0)
					[
						SNew(SButton).ButtonStyle(&GotoVatAnimButtonStyle)
						.ToolTipText(LOCTEXT("NavToVatAnimTooltip", "Find VatAnimationDataAsset in the ContentBrowser"))
						.OnClicked_Lambda([this]()
						{
							if(VatObject)
							{
								UOverCrowdEditorLibrary::NavToAssetInContentBrowser(UOverCrowdRuntimeLibrary::FindAnimationDataAsset(VatObject,WardrobeID));
							}
						
							return FReply::Handled();
						})
					]	
					+ SHorizontalBox::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill).Padding(5,3,5,0)
					[
						SNew(SButton).ButtonStyle(&GotoVatAssetButtonStyle)
						.ToolTipText(LOCTEXT("NavToVatAssetTooltip", "Find VatMeshPairing Asset in the Content Browser"))
						.OnClicked_Lambda([this]()
						{
							UOverCrowdEditorLibrary::NavToAssetInContentBrowser(VatObject);
						
							return FReply::Handled();
						})
						
					]
					+ SHorizontalBox::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill).Padding(5,3,5,0)
					[
						SNew(SButton).ButtonStyle(&GotoSkelMeshButtonStyle)
						.ToolTipText(LOCTEXT("NavToSkelMeshTooltip", "Find source SkeletalMesh in the Content Browser"))
						.OnClicked_Lambda([this]()
						{
							if(VatObject && VatObject->SkelMesh.LoadSynchronous())
							{
								UOverCrowdEditorLibrary::NavToAssetInContentBrowser(VatObject->SkelMesh.LoadSynchronous());
							}
						
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill).Padding(5,3,5,0)
					[
						SNew(SButton).ButtonStyle(&GotoVatMeshButtonStyle)
						.ToolTipText(LOCTEXT("NavToVatMeshTooltip", "Find Vat StaticMesh in the Content Browser"))
						.OnClicked_Lambda([this]()
						{
							if(VatObject && VatObject->VatStaticMesh.LoadSynchronous())
							{
								UOverCrowdEditorLibrary::NavToAssetInContentBrowser(VatObject->VatStaticMesh.LoadSynchronous());
							}
						
							return FReply::Handled();
						})
					]
					
				]
			]
			+SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill)
			[
				SNew(SBorder).BorderImage(BorderBrush).Visibility_Lambda([this]()
				{
				   return (bSelected) ? EVisibility::HitTestInvisible : EVisibility::Hidden;
				})
			]
		]
	];
}

