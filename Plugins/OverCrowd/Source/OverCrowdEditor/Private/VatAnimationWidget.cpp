// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved
#include "VatAnimationWidget.h"

#include "OverCrowdRuntimeLibrary.h"
#include "OverCrowdUtilityWidget.h"
#include "VatAssetArrayWidget.h"
#include "VatAssetPicker.h"


#define LOCTEXT_NAMESPACE "OverCrowdEditor"

void SVatAnimationWidget::SetAssetPicker(UVatAssetPicker* Picker)
{
	VatAssetPicker = Picker;
	
	Refresh();
}


void SVatAnimationWidget::Refresh()
{
	UVatAnimationDataAsset* VatAnimation = nullptr;
	if (VatAssetPicker)
		VatAnimation = VatAssetPicker->VatAnimDataAsset;
	SetVatAnimationDataAsset(VatAnimation);
}

void SVatAnimationWidget::SetVatAnimationDataAsset(UVatAnimationDataAsset* VatAnimationDataAsset)
{
	if (VatAssetPicker)
	{
		VatAssetPicker->AdditiveVatAnimDataAsset = nullptr;
		VatAssetPicker->VatAnimDataAsset = VatAnimationDataAsset;
		if(VatAnimationDataAsset)
			VatAssetPicker->AdditiveVatAnimDataAsset = VatAnimationDataAsset->AdditiveVatAnimationDataAsset;
	}
	
	ContentBox->ClearChildren();
	
	VatAnimPropertyView = NewObject<UCustomSinglePropertyView>(UCustomSinglePropertyView::StaticClass());
	AnimToBakePropertyView = NewObject<UCustomSinglePropertyView>(UCustomSinglePropertyView::StaticClass());
	AdditiveRootBoneNameView = NewObject<UCustomSinglePropertyView>(UCustomSinglePropertyView::StaticClass());

	auto AddIconBrush = *FCoreStyle::Get().GetBrush("WorldBrowser.AddLayer");

	auto SkelMeshBrush = *FCoreStyle::Get().GetBrush("ShowFlagsMenu.SkeletalMeshes");

	SkelMeshBrush.TintColor = FColor(255,0,128);
	MakeDefaultSkeletonButtonStyle.SetNormal(SkelMeshBrush);
	MakeDefaultSkeletonButtonStyle.SetPressed(SkelMeshBrush);
	SkelMeshBrush.TintColor = FColor(128,128,128);
	MakeDefaultSkeletonButtonStyle.SetHovered(SkelMeshBrush);
	SkelMeshBrush.TintColor = FColor(128,128,128,64);
	MakeDefaultSkeletonButtonStyle.SetDisabled(SkelMeshBrush);

	SelectSkeletonButtonStyle = MakeDefaultSkeletonButtonStyle;
	RevertToDefaultSkeletonButtonStyle = MakeDefaultSkeletonButtonStyle;
	
	SkelMeshBrush.TintColor = FColor(255,255,0);
	SelectSkeletonButtonStyle.SetNormal(SkelMeshBrush);
	SelectSkeletonButtonStyle.SetPressed(SkelMeshBrush);

	SkelMeshBrush.TintColor = FColor(128,255,255);
	RevertToDefaultSkeletonButtonStyle.SetNormal(SkelMeshBrush);
	RevertToDefaultSkeletonButtonStyle.SetPressed(SkelMeshBrush);
	
	AddIconBrush.TintColor = FColor(255,255,255);
	AddButtonStyle.SetNormal(AddIconBrush);
	AddButtonStyle.SetPressed(AddIconBrush);
	AddIconBrush.TintColor = FColor(128,128,128);
	AddButtonStyle.SetHovered(AddIconBrush);
	
	TArray<UObject*> AnimationList;
	TArray<FString> AnimationTagList;
	
	if(VatAssetPicker)
	{
		VatAnimPropertyView->SetObject(VatAssetPicker);
		if(VatAssetPicker->VatAnimDataAsset && VatAssetPicker->VatAnimDataAsset->AdditiveVatAnimationDataAsset)
		{
			AnimToBakePropertyView->SetVisibility(ESlateVisibility::Visible);
			AnimToBakePropertyView->SetObject(VatAssetPicker->VatAnimDataAsset->AdditiveVatAnimationDataAsset);
			AdditiveRootBoneNameView->SetVisibility(ESlateVisibility::Visible);
			AdditiveRootBoneNameView->SetObject(VatAssetPicker->VatAnimDataAsset);
		}
		else
		{
			AnimToBakePropertyView->SetObject(nullptr);
			AnimToBakePropertyView->SetVisibility(ESlateVisibility::Hidden);
			AdditiveRootBoneNameView->SetVisibility(ESlateVisibility::Hidden);
		}
		
		VatAnimPropertyView->GetPropertyChangedEvent().AddDynamic(VatAssetPicker,&UVatAssetPicker::OnPropertyEdited);
		AnimToBakePropertyView->GetPropertyChangedEvent().AddDynamic(VatAssetPicker,&UVatAssetPicker::OnPropertyEdited);

		if(VatAssetPicker->VatAnimDataAsset && VatAssetPicker->VatAnimDataAsset->AdditiveVatAnimationDataAsset &&
			VatAssetPicker->VatAnimDataAsset->AdditiveVatAnimationDataAsset->AnimationsToBake)
		{
			int Index = 0;
			for(FOverCrowdAnimSequenceInfo AnimInfo : VatAssetPicker->VatAnimDataAsset->AdditiveVatAnimationDataAsset->AnimationsToBake->AnimSequences)
			{
				if(AnimInfo.AnimSequence)
				{
					auto Tmp = FString::Format(TEXT("Index:{0} \n"), {Index++});
					
					AnimationList.Add(AnimInfo.AnimSequence);
					AnimationTagList.Add(Tmp + AnimInfo.AnimationTag.ToString());
				}
			}	
		}
	}

	VatAnimPropertyView->SetPropertyName(   "AdditiveVatAnimDataAsset");
	AnimToBakePropertyView->SetPropertyName("AnimationsToBake");
	AdditiveRootBoneNameView->SetPropertyName("AdditveRootBoneName");
	
	ContentBox->AddSlot()
	[
		SNew(SBox)
		.HeightOverride(256 * ThumbnailScale)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().HAlign(HAlign_Left).AutoWidth()
			[
				SNew(SBox)
				.WidthOverride(350)  
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).VAlign(VAlign_Top)
					[
					 	SNew(SBox).HeightOverride(32)
					 	[
					 		SNew(SHorizontalBox)
					 		+ SHorizontalBox::Slot().AutoWidth().HAlign(HAlign_Left).VAlign(VAlign_Top).Padding(0,0,0,0)
					 		[
					 			AdditiveRootBoneNameView->TakeWidget()
					 		]
						]
					]
					// 			SNew(SButton)
					// 			.ButtonStyle(&SelectSkeletonButtonStyle)
					// 			.ToolTipText(LOCTEXT("SelectSameSkel", "Select all body parts with the same skeleton"))
					// 			.VAlign(VAlign_Top).HAlign(HAlign_Right)
					// 			.OnClicked_Lambda([this]()
					// 			{
					// 				//VatAssetPicker->SelectAllWithSameSkeleton();
					// 			
					// 				return FReply::Handled();
					// 			})
					// 		]
					// 		+ SHorizontalBox::Slot().AutoWidth().HAlign(HAlign_Right).VAlign(VAlign_Top).Padding(0,0,8,0)
					// 		[
					// 			SNew(SButton)
					// 			.ButtonStyle(&RevertToDefaultSkeletonButtonStyle)
					// 			.ToolTipText(LOCTEXT("RevertAnimAsset", "Revert selected body parts to default VatAnimationDataAsset"))
					// 			.VAlign(VAlign_Top).HAlign(HAlign_Right)
					// 			.OnClicked_Lambda([this]()
					// 			{
					// 				//VatAssetPicker->RevertToDefaultSkeleton();
					// 			
					// 				return FReply::Handled();
					// 			})
					// 			.IsEnabled_Lambda([this]()
					// 			{
					// 				if(VatAssetPicker)
					// 				{
					// 					//return !VatAssetPicker->IsDefaultVatAnimation();
					// 				}
					// 				//VatAssetPicker->
					// 				return false;
					// 			})
					// 		]
					// 		+ SHorizontalBox::Slot().AutoWidth().HAlign(HAlign_Right).VAlign(VAlign_Top)
					// 		[
					// 			SNew(SButton)
					// 			.ButtonStyle(&MakeDefaultSkeletonButtonStyle)
					// 			.ToolTipText(LOCTEXT("MakeDefaultSkel", "Make this VatAnimationDataAsset the default for all body parts that use the same skeleton"))
					// 			.VAlign(VAlign_Top).HAlign(HAlign_Right)
					// 			.OnClicked_Lambda([this]()
					// 			{
					// 				//VatAssetPicker->SetAsDefaultSkeleton();
					// 			
					// 				return FReply::Handled();
					// 			})
					// 			.IsEnabled_Lambda([this]()
					// 			{
					// 				if(VatAssetPicker)
					// 				{
					// 					//return !VatAssetPicker->IsDefaultVatAnimation();
					// 				}
					// 				//VatAssetPicker->
					// 				return false;
					// 			})
					// 		]	
					// 	]
					// 	
					// ]
					+ SVerticalBox::Slot()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(0.1).VAlign(VAlign_Center).HAlign(HAlign_Left)
						[
							SNew(SButton)
							.ButtonStyle(&AddButtonStyle)
							.Visibility_Lambda([this]()
							{
								if(!VatAssetPicker)
									return EVisibility::Collapsed;
								if(VatAssetPicker->VatAnimDataAsset && !VatAssetPicker->VatAnimDataAsset->AdditiveVatAnimationDataAsset)
									return EVisibility::Visible;
								return EVisibility::Collapsed;
							})
							.OnClicked_Lambda([this]()
							{
								if(!VatAssetPicker || !VatAssetPicker->VatAnimDataAsset)
									return FReply::Handled();
								
								TArray<UVatMeshPairing*> SelectedAssets;
								VatAssetPicker->GetSelected(SelectedAssets);

								if(!SelectedAssets.Num())
									return FReply::Handled();
								
								VatAssetPicker->VatAnimDataAsset->AdditiveVatAnimationDataAsset = UOverCrowdUtilityWidget::CreateAdditveVatAnimationDataAsset(SelectedAssets[0]);

								if(!VatAssetPicker->VatAnimDataAsset || !VatAssetPicker->VatAnimDataAsset->AdditiveVatAnimationDataAsset)
									return FReply::Handled();
								
								Refresh();
								
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot()
						[
							VatAnimPropertyView->TakeWidget()	
						]
							
					]
					+ SVerticalBox::Slot()
					[
						SNew(SHorizontalBox).Visibility_Lambda([this]()
						{
							return (VatAssetPicker && VatAssetPicker->VatAnimDataAsset)	? EVisibility::Visible : EVisibility::Hidden;
						})
						+ SHorizontalBox::Slot().FillWidth(0.1).VAlign(VAlign_Center).HAlign(HAlign_Left)
						[
							SNew(SButton)
							.ButtonStyle(&AddButtonStyle)
							.Visibility_Lambda([this]()
							{
								if(!VatAssetPicker)
									return EVisibility::Collapsed;
								if(VatAssetPicker->VatAnimDataAsset &&
									VatAssetPicker->VatAnimDataAsset->AdditiveVatAnimationDataAsset &&
									!VatAssetPicker->VatAnimDataAsset->AdditiveVatAnimationDataAsset->AnimationsToBake)
									return EVisibility::Visible;
								return EVisibility::Collapsed;
								
								// return (VatAssetPicker
								// 	&& VatAssetPicker->VatAnimDataAsset
								// 	&& VatAssetPicker->VatAnimDataAsset->AnimationsToBake)
								// ? EVisibility::Collapsed : EVisibility::Visible;
							})
							.OnClicked_Lambda([this]()
							{
								if(!VatAssetPicker || !VatAssetPicker->VatAnimDataAsset || !VatAssetPicker->VatAnimDataAsset->AdditiveVatAnimationDataAsset)
									return FReply::Handled();
								
								UOverCrowdUtilityWidget::CreateAdditveAnimToBakeDataAsset(VatAssetPicker->VatAnimDataAsset->AdditiveVatAnimationDataAsset);	

								//VatAssetPicker->VatAnimDataAsset->
								Refresh();
								
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot()
						[
							AnimToBakePropertyView->TakeWidget()
						]
					]	
				]
			]
			+ SHorizontalBox::Slot().Padding(0,0,0,20)
			.HAlign(HAlign_Left)
			.VAlign(VAlign_Center)
			[
				SNew(SVatAssetArrayWidget)
				.InScale(ThumbnailScale)
				.InObjectArray(AnimationList)
				.InWardrobeID(WardrobeID)
				.InNameArray(AnimationTagList)
			]
		]
		
	];
}

void SVatAnimationWidget::Construct(const FArguments& InArgs)
{
	//auto VatAnimDataAsset = InArgs._InVatAnimation;

	ThumbnailScale = InArgs._ThumbnailScale;
	WardrobeID = InArgs._InWardrobeID;
	ContentBox = SNew(SHorizontalBox);
	
	this->ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		[
			SNew(SScrollBox).Orientation(Orient_Vertical)
			+ SScrollBox::Slot()
			[
				ContentBox.ToSharedRef()
				
			]
		]
	];
	
	SetVatAnimationDataAsset(nullptr);
}
