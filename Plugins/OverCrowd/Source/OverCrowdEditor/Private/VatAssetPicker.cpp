// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved
#include "VatAssetPicker.h"

#include "VatAssetArrayWidget.h"

#include "Widgets/SBoxPanel.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SScrollBar.h"
#include "IImageWrapper.h" 
#include "IImageWrapperModule.h" 
#include "ImageUtils.h"
#include "ObjectTools.h"
#include "OverCrowdDeveloperSettings.h"
#include "OverCrowdRuntimeLibrary.h"
#include "VatAssetWidget.h"
#include "Components/AssetThumbnailWidget.h"
#include "Components/Image.h"
#include "Styling/SlateIconFinder.h"
#include "Widgets/Input/SSpinBox.h"


void UVatAssetPicker::ReleaseSlateResources(bool bReleaseChildren)
{
	VatAssetPicker.Reset();
}

void UVatAssetPicker::Refresh(const TArray<UVatMeshPairing*>& VatAssets, FGameplayTag Wardrobe_ID)
{
	WardrobeID = Wardrobe_ID;
	VatObjects = VatAssets;
	if(VatAssetPicker)
	{
		VatAssetPicker->WardrobeID = WardrobeID;
		VatAssetPicker->VatAssets = VatAssets;
		VatAssetPicker->Refresh();
		//UE_LOG(LogOverCrowdEditor, Display, TEXT("Refreshing VatAssets"));
	}
}

void UVatAssetPicker::GetSelected(TArray<UVatMeshPairing*>& VatAssets)
{
	if(VatAssetPicker)
		VatAssetPicker->GetSelected(VatAssets);
}
//
// bool UVatAssetPicker::IsDefaultVatAnimation()
// {
// 	if(VatAssetPicker)
// 	{
// 		if(VatAssetPicker->LastUserSelected)
// 		{
// 			return UOverCrowdRuntimeLibrary::IsUsingDefaultAnimationDataAsset(VatAssetPicker->LastUserSelected);
// 		}
// 	}
// 		
// 	return false;
// }
//
// void UVatAssetPicker::SelectAllWithSameSkeleton()
// {
// 	//TArray<UVatMeshPairing*> SelectedAssets;
// 	//GetSelected(SelectedAssets);
// 	
// 	if(VatAssetPicker->LastUserSelected)
// 	{
// 		if(auto Skel = VatAssetPicker->LastUserSelected->GetSkelMeshSkeleton())
// 		{
// 			VatAssetPicker->SetSelectedWithSkeleton(Skel);
// 		}
// 	}
// }
//
// void UVatAssetPicker::SetAsDefaultSkeleton()
// {
// 	TArray<UVatMeshPairing*> SelectedAssets;
// 	GetSelected(SelectedAssets);
// 	for(auto SelectedAsset : SelectedAssets)
// 	{
// 		if(SelectedAsset)
// 		{
// 			UOverCrowdRuntimeLibrary::AddAnimationDataAsset(SelectedAsset,VatAssetPicker->LastVatAnimation,true);
// 			UOverCrowdRuntimeLibrary::UseDefaultAnimationDataAsset(SelectedAsset);
// 			
// 		}
// 	}
// }
//
// void UVatAssetPicker::RevertToDefaultSkeleton()
// {
// 	TArray<UVatMeshPairing*> SelectedAssets;
// 	GetSelected(SelectedAssets);
// 	for(auto SelectedAsset : SelectedAssets)
// 	{
// 		if(SelectedAsset)
// 		{
// 			UOverCrowdRuntimeLibrary::UseDefaultAnimationDataAsset(SelectedAsset);
// 		}
// 	}
// }

void UVatAssetPicker::OnPropertyEdited(FName Name)
{
	if(!VatAssetPicker || !VatAssetPicker->VatAnimationWidget)// || !AdditiveVatAnimDataAsset)
		return;
	
	if(Name == "AdditiveVatAnimDataAsset")
	{
		//
		if(VatAssetPicker->CurrentVatAnimation)
		{
			VatAssetPicker->CurrentVatAnimation->AdditiveVatAnimationDataAsset = AdditiveVatAnimDataAsset;
			VatAssetPicker->CurrentVatAnimation->MarkPackageDirty();
		}
		VatAssetPicker->VatAnimationWidget->Refresh();

		//
		// TArray<UVatMeshPairing*> SelectedAssets;
		// GetSelected(SelectedAssets);
		//
		// for(auto Asset: SelectedAssets)
		// {
		// 	//UOverCrowdRuntimeLibrary::AddAnimationDataAsset(Asset, VatAssetPicker->LastVatAnimation, false);
		// }
	}
	else if(Name == "AnimationsToBake")
	{
		VatAssetPicker->VatAnimationWidget->Refresh();
	}
}

TSharedRef<SWidget> UVatAssetPicker::RebuildWidget()
{
	VatAssetPicker = SNew(SVatAssetPicker)
		.InVatAssetArray(VatObjects)
		.InScale(ThumbnailScale)
		.InWardrobeID(WardrobeID)
		.InHideVatAnim(HideVatAnim)
		
		.OnSelected_Lambda([this](UVatMeshPairing* VatAsset, bool bIsSelected)
		{
			const FModifierKeysState ModifierKeys = FSlateApplication::Get().GetModifierKeys();
			TArray<UVatMeshPairing*> Selected;

			if(bIsSelected)
				VatAssetPicker->UserSelectedVatPairing = VatAsset;
			else
				VatAssetPicker->UserSelectedVatPairing = nullptr;
			
			if (!(ModifierKeys.IsLeftControlDown() || ModifierKeys.IsRightControlDown()))
			{
				if(VatAsset && bIsSelected)
				{
					Selected.Add(VatAsset);
				
				}
				
				VatAssetPicker->SetSelected(Selected);
			}
			else
			{
				VatAssetPicker->RefreshShowVatAnimation();
			}
				
			//if(OnSelected.IsBound())
			//	OnSelected.Broadcast(VatAsset,bIsSelected);
		});

	
	VatAssetPicker->VatAnimationWidget->SetAssetPicker(this);
	
	return VatAssetPicker.ToSharedRef();
}



void SVatAssetPicker::GetSelected(TArray<UVatMeshPairing*>& Out_Selected)
{
	for(auto Array:VatAssetWidgetArray)
	{
		Array->GetSelected(Out_Selected);
	}
	CachedSelected = Out_Selected;
	//bSelectedHaveSameSkel = UOverCrowdRuntimeLibrary::IsSameSkeleton(Out_Selected);
}

void SVatAssetPicker::RefreshShowVatAnimation()
{
	TArray<UVatMeshPairing*> Selected;
	GetSelected(Selected);
	bSelectedHaveSameSkel = UOverCrowdRuntimeLibrary::IsSameSkeleton(Selected);

	//UE_LOG(LogOverCrowdEditor, Display, TEXT("bSelectedHaveSameSkel %d"), bSelectedHaveSameSkel);
	
	if(Selected.Num() && !UserSelectedVatPairing)
		UserSelectedVatPairing = Selected[0];
	else if(!Selected.Num())
	{
		UserSelectedVatPairing = nullptr;
		bSelectedHaveSameSkel = false;
	}
	CurrentAdditiveVatAnimation = nullptr;
	CurrentVatAnimation = nullptr;
	
	if(UserSelectedVatPairing && WardrobeID.IsValid())
	{
		CurrentVatAnimation = UOverCrowdRuntimeLibrary::FindAnimationDataAsset(UserSelectedVatPairing, WardrobeID);
		//if(CurrentVatAnimation)
		//	CurrentAdditiveVatAnimation = CurrentVatAnimation->AdditiveVatAnimationDataAsset;
	}
	 		
	if(VatAnimationWidget)
		VatAnimationWidget->SetVatAnimationDataAsset(CurrentVatAnimation);
}

void SVatAssetPicker::SetSelectedWithSkeleton(USkeleton* Skeleton)
{
	for(auto Array:VatAssetWidgetArray)
	{
		Array->SetSelectedWithSkeleton(Skeleton);
	}
}

void SVatAssetPicker::SetSelected(const TArray<UVatMeshPairing*>& In_Selected)
{
	for(auto Array:VatAssetWidgetArray)
	{
		Array->SetSelected(In_Selected);
	}
	RefreshShowVatAnimation();
}

void SVatAssetPicker::Construct(const FArguments& InArgs)
{
	VatAssets = InArgs._InVatAssetArray;
	ThumbnailScale = UOverCrowdDeveloperSettings::Get()->IconScale;//InArgs._InScale;
	OnSelected = InArgs._OnSelected;
	//WardrobeID = InArgs._InWardrobeID;
	
	bool HideVatAnim = InArgs._InHideVatAnim;

	EVisibility Visibility = EVisibility::Visible;
	if(HideVatAnim)
		Visibility = EVisibility::Collapsed;
	
	ContentBox = SNew(SVerticalBox);
	VatAnimationWidget = SNew(SVatAnimationWidget).ThumbnailScale(ThumbnailScale).Visibility(Visibility).InWardrobeID(WardrobeID);

	//bSelectedHaveSameSkel = false;
	FSlateFontInfo MainFont = FAppStyle::Get().GetFontStyle("NormalFont");
	MainFont.Size = 8.25;

	FSlateBrush EmptyBrush = *FCoreStyle::Get().GetBrush("ShowFlagsMenu.StaticMeshes");
	EmptyBrush.DrawAs = ESlateBrushDrawType::NoDrawType;
	
	TransparentStyle.SetNormal(EmptyBrush);
	TransparentStyle.SetPressed(EmptyBrush);
	TransparentStyle.SetHovered(EmptyBrush);
	
	this->ChildSlot
	[
		SNew(SVerticalBox)
		// + SVerticalBox::Slot().AutoHeight().VAlign(VAlign_Top).Padding(10,5,5,5)
		// [
		// 	SNew(SHorizontalBox)
		// 	.Visibility_Lambda([this](){ return !VatAssets.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
		// 	+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		// 	[
		// 		SNew(STextBlock)
		// 		.Text(FText::FromString("Icon Scale"))
		// 		.Font(MainFont)
		// 	]
		// 	+ SHorizontalBox::Slot().AutoWidth()
		// 	[
		// 		SNew(SSpinBox<float>).MinValue(0.25).MaxValue(1.0).WheelStep(0.05).Delta(0.05)
		// 		.Value_Lambda([this]() -> float
		// 		{
		// 			return ThumbnailScale;
		// 		})					
		// 		.OnValueChanged_Lambda([this](float NewValue)
		// 		{
		// 			ThumbnailScale = NewValue;
		// 			Refresh();
		// 		})
		// 	]
		// ]
		+ SVerticalBox::Slot()
		[
			SNew(SScrollBox).Orientation(Orient_Vertical)
			+ SScrollBox::Slot()
			[
				SNew(SOverlay)
				+ SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill).ZOrder(-100)
				[
					SNew(SButton)
					.ButtonStyle(&TransparentStyle)
					.ButtonColorAndOpacity(FSlateColor(FColor(0,0,0,0)))
					.ForegroundColor(FSlateColor(FColor(0,0,0,0)))
					.OnClicked_Lambda([this]()
					{
						if(OnSelected.IsBound())
							OnSelected.Execute(nullptr,false);
					
						return FReply::Handled();
					})
				]
				+ SOverlay::Slot()
				[
					ContentBox.ToSharedRef()
				]
			]
		]
		+ SVerticalBox::Slot().VAlign(VAlign_Bottom).AutoHeight().Padding(20,0,0,0)
		[
			SNew(SBox)
			.Visibility_Lambda([this]()
			{
				return (bSelectedHaveSameSkel) ? EVisibility::Visible : EVisibility::Collapsed;
			})
			[
				VatAnimationWidget.ToSharedRef()	
			]
		]	
	];

	Refresh();
}


void SVatAssetPicker::Refresh()
{
	if(!ContentBox)
		return;

	if(VatAnimationWidget)
	{
		VatAnimationWidget->ThumbnailScale = ThumbnailScale;
		VatAnimationWidget->SetVatAnimationDataAsset(CurrentVatAnimation);
	}
	
	ContentBox->ClearChildren();
	
	TMap<FString,FVatAssetArray> SortedVatAssets;
	
	VatAssetWidgetArray.Empty();
	
	for(auto Asset: VatAssets)
	{
		if(!Asset)
			continue;
		
		SortedVatAssets.FindOrAdd(Asset->BodyPart.ToString()).TaggedVatAssets.Add(Asset);
	}
	
	for(auto Asset: SortedVatAssets)
	{
		TSharedPtr<SVatAssetArrayWidget> ArrayWidget = SNew(SVatAssetArrayWidget)
		.InScale(ThumbnailScale)
		.InRowName(Asset.Key)
		.InWardrobeID(WardrobeID)
		.InVatAssetArray(Asset.Value.TaggedVatAssets)
		.OnSelected_Lambda([this](UVatMeshPairing* VatAsset, bool bIsSelected)
		{
			if (OnSelected.IsBound())
				OnSelected.Execute(VatAsset, bIsSelected);
			
			RefreshShowVatAnimation();
		});

		VatAssetWidgetArray.Add(ArrayWidget);

		ContentBox->AddSlot()
		          .AutoHeight()
		          .Padding(10,0,0,0)
		          .HAlign(HAlign_Left)
		          .VAlign(VAlign_Center)
		[
			ArrayWidget.ToSharedRef()
		];
		RefreshShowVatAnimation();
	}
}
