// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved
#include "VatAssetArrayWidget.h"

#include "Widgets/SBoxPanel.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SScrollBar.h"
#include "IImageWrapper.h" 
#include "IImageWrapperModule.h" 
#include "ImageUtils.h"
#include "ObjectTools.h"
#include "VatAssetWidget.h"
#include "Components/AssetThumbnailWidget.h"
#include "Components/Image.h"
#include "Styling/SlateIconFinder.h"
#include "Widgets/Input/SSpinBox.h"


void SVatAssetArrayWidget::Construct(const FArguments& InArgs)
{
	TArray<UVatMeshPairing*> InAssets = InArgs._InVatAssetArray;
	TArray<UObject*> InObjects = InArgs._InObjectArray;
	ThumbnailScale = InArgs._InScale;
	RowName = InArgs._InRowName;
	OnSelected = InArgs._OnSelected;
	NameArray = InArgs._InNameArray;
	WardrobeID = InArgs._InWardrobeID;
	
	ContentBox = SNew(SHorizontalBox);
	
	int NumTotal = InAssets.Num() + InObjects.Num(); 
	for(auto Asset: InAssets)
	{
		AddVatAsset(Asset, NumTotal);
	}
	int Index = 0;
	for(auto Asset: InObjects)
	{
		FString Name;
		if(Index < InObjects.Num())
			Name = NameArray[Index++];
		
		AddNormalAsset(Asset, NumTotal, Name);
	}
	int AdditionalSpace = (NumTotal == 1) ? 100 : 0; 
	ContentBox->AddSlot()
	[
		SNew(SSpacer).Size(FVector2D(100 + AdditionalSpace,0))
	];
	
	this->ChildSlot
	[
		
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		[
			SNew(STextBlock).Text(FText::FromString(RowName))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(20 * ThumbnailScale,0,20 * ThumbnailScale,15)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().HAlign(HAlign_Left)
			[
				SNew(SScrollBox).Orientation(Orient_Horizontal)
				+ SScrollBox::Slot()
				[
					ContentBox.ToSharedRef()
				]
			]
		]
		
	];
}

void SVatAssetArrayWidget::AddVatAsset(UVatMeshPairing* VatAsset,int NumTotal)
{
	ETextJustify::Type Justify;
	if(VatAssetWidgets.Num()==0)
		Justify = ETextJustify::Left;
	//else if(VatAssetWidgets.Num() == (NumTotal - 1))
	//	Justify = ETextJustify::Right;
	else
		Justify = ETextJustify::Center;
	
	TSharedPtr<SVatAssetWidget> NewWidget = SNew(SVatAssetWidget)
	.InAsset(VatAsset)
	.InScale(ThumbnailScale)
	.InJustify(Justify)
	.InWardrobeID(WardrobeID)
	.OnSelected_Lambda([this](UVatMeshPairing* VatAsset, bool bIsSelected)
	{
		if(OnSelected.IsBound())
			OnSelected.Execute(VatAsset,bIsSelected);
		
	});
		
	ContentBox->AddSlot()
	.AutoWidth()
	.Padding(0,0,20,0)
	.HAlign(HAlign_Left)
	.VAlign(VAlign_Center)
	[
		NewWidget.ToSharedRef()
	];

	VatAssetWidgets.Add(NewWidget);
}

void SVatAssetArrayWidget::AddNormalAsset(UObject* Asset, int NumTotal, FString Name)
{
	ETextJustify::Type Justify;
	if(AssetThumbnailWidgets.Num()==0)
		Justify = ETextJustify::Left;
	else
		Justify = ETextJustify::Center;
	
	TSharedPtr<SOverCrowdThumbnailWidget> Thumbnail = SNew(SOverCrowdThumbnailWidget)
		.InAsset(Asset)
		.InScale(ThumbnailScale)
		.InJustify(Justify)
		.InTextYOffset(8)
		.InName(Name)
		.OnClicked_Lambda([this]()
		{
			return FReply::Handled();
		});
	
		
	ContentBox->AddSlot()
	
	//.Padding(0,20,0,0)
	.HAlign(HAlign_Left)
	.VAlign(VAlign_Center)
	[
		Thumbnail.ToSharedRef()
	];

	AssetThumbnailWidgets.Add(Thumbnail);

}

void SVatAssetArrayWidget::GetSelected(TArray<UVatMeshPairing*>& OutSelected)
{
	for(auto Widget: VatAssetWidgets)
	{
		if(!Widget)
			continue;
		
		if(Widget->bSelected)
		{
			OutSelected.Add(Widget->VatObject);
		}
	}
}

void SVatAssetArrayWidget::SetSelected(const TArray<UVatMeshPairing*>& Selected)
{
	for(auto Widget: VatAssetWidgets)
	{
		if(!Widget)
			continue;

		if(Selected.Contains(Widget->VatObject))
		{
			Widget->bSelected = true;
		}
		else
		{
			Widget->bSelected = false;
		}
	}
}

void SVatAssetArrayWidget::SetSelectedWithSkeleton(USkeleton* Skeleton)
{
	for(auto Widget: VatAssetWidgets)
	{
		if(!Widget || !Widget->VatObject)
			continue;
		
		if(Widget->VatObject->GetSkelMeshSkeleton() == Skeleton)
		{
			Widget->bSelected = true;
		}
		else
		{
			Widget->bSelected = false;
		}
	}
}

