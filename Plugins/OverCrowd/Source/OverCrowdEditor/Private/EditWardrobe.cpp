// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#include "EditWardrobe.h"

#include "Widgets/SBoxPanel.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SScrollBar.h"
#include "GameplayTagContainer.h"
#include "OverCrowdUtilityWidget.h"
#include "SGameplayTagPicker.h"



void SEditWardrobe::Construct(const FArguments& InArgs)
{
	OnWardrobeEdited = InArgs._OnWardrobeEdited;
	OverCrowdUtilityWidget = InArgs._InOverCrowdUtilityWidget;

	//auto CurrentWardrobeContainer = InArgs._WardrobeContainer;
	//ContainersForPicker.Empty();
	//ContainersForPicker.Add(CurrentWardrobeContainer);

	TArray<FName> PropertiesToShow = {"WardrobeIDs"};
	WardrobeIDsView = NewObject<UCustomDetailsView>(UCustomDetailsView::StaticClass());
	WardrobeIDsView->SetObject(OverCrowdUtilityWidget);
	WardrobeIDsView->GetPropertyChangedEvent().AddDynamic(OverCrowdUtilityWidget,&UOverCrowdUtilityWidget::OnPropertyEdited);
	//WardrobeIDsView->Set("WardrobeIDs");
	WardrobeIDsView->SetCategoryName("Wardrobe");
	WardrobeIDsView->PropertiesToShow = PropertiesToShow;
	this->ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().HAlign(HAlign_Fill)
		[
			SNew(SScrollBox)
			
			+ SScrollBox::Slot().HAlign(HAlign_Fill)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().VAlign(VAlign_Center).HAlign(HAlign_Fill).FillWidth(0.3)
				[
					WardrobeIDsView->TakeWidget()
					
					// SNew(SGameplayTagPicker)
					// .TagContainers(ContainersForPicker)
					// .MaxHeight(0)	
					// .MultiSelect(true)
					// //.GameplayTagPickerMode(EGameplayTagPickerMode::HybridMode)
					// .OnTagChanged_Lambda([&](const TArray<FGameplayTagContainer>& NewSelection)
					// {
					// 	CurrentWardrobeContainer = NewSelection[0];
					// })
				]
				
			]
		]
		+ SVerticalBox::Slot().HAlign(HAlign_Fill).AutoHeight().VAlign(VAlign_Top)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().VAlign(VAlign_Center).HAlign(HAlign_Fill)
			[
				SNew(SButton)
				.Text(FText::FromString("Apply"))
				.ButtonColorAndOpacity(FColor::Green)
				.OnClicked_Lambda([&]()
				{
					if(OnWardrobeEdited.IsBound())
					{
						OnWardrobeEdited.Execute(true);
					}
					return FReply::Handled();
				})
				//.OnClicked(this, &SKytCSNWidget::OnSpawnVehicles)
			]
			// + SHorizontalBox::Slot().VAlign(VAlign_Center).HAlign(HAlign_Fill)
			// [
			// 	SNew(SButton)
			// 	.Text(FText::FromString("Cancel"))
			// 	.OnClicked_Lambda([&]()	
			// 	{
			// 		if(OnWardrobeEdited.IsBound())
			// 		{
			// 			OnWardrobeEdited.Execute(false);
			// 		}
			// 		return FReply::Handled();
			// 	})
			//
			// ]
		]
	];


}
	
	//this->ChildSlot
	//[
	//	SNew(SHorizontalBox)
	//	+ SHorizontalBox::Slot().FillWidth(1.0).HAlign(HAlign_Fill)
	//	[
	//		SNew(SVerticalBox)
	//		
	//		+ SVerticalBox::Slot().HAlign(HAlign_Fill).AutoHeight()
	//		[
	//			SNew(SHorizontalBox)
	//			+ SHorizontalBox::Slot().VAlign(VAlign_Center).HAlign(HAlign_Fill).FillWidth(0.3)
	//			[
	//				SNew(STextBlock)
	//				.Text(FText::FromString("Draw CSN"))
	//			]
	//			+ SHorizontalBox::Slot().HAlign(HAlign_Fill).FillWidth(0.7).VAlign(VAlign_Center)
	//			[
	//				SAssignNew(DrawCSNCombo,SComboBox<TSharedPtr<FStringIntPair>>)
	//				.ContentPadding(FMargin(6.0f, 2.0f))
	//				.Content()
	//				[
	//					SNew(STextBlock).Text_Lambda([this]
	//					{
	//						auto StringItem = DrawCSNCombo->GetSelectedItem();
	//						if (StringItem)
	//						{
	//							return FText::FromString(StringItem->String);
	//						}
	//						return FText::FromString("None");
	//					})
	//				]
	//				.OptionsSource(&DrawCSNLabels)
	//				.InitiallySelectedItem(DrawCSNLabels[0])
	//				.OnGenerateWidget_Lambda([bSameValue](TSharedPtr<FStringIntPair> Item)
	//				{
	//					return SNew(STextBlock).Text(FText::FromString(Item->String));
	//				})
	//				.OnSelectionChanged(this, &SKytCSNWidget::OnDrawCSNSelectionChanged)
	//			]
	//		]

	//		+ SVerticalBox::Slot().HAlign(HAlign_Fill).AutoHeight()
	//		[
	//			SNew(SHorizontalBox)
	//			+ SHorizontalBox::Slot().VAlign(VAlign_Center).HAlign(HAlign_Fill).FillWidth(0.3)
	//			[
	//				SNew(STextBlock)
	//				.Text(FText::FromString("Roads"))
	//			]
	//			+ SHorizontalBox::Slot().HAlign(HAlign_Fill).FillWidth(0.7).VAlign(VAlign_Center)
	//			[
	//				SAssignNew(RoadsCombo,SComboBox<TSharedPtr<FStringIntPair>>)
	//				.ContentPadding(FMargin(6.0f, 2.0f))
	//				.Content()
	//				[
	//					SNew(STextBlock).Text_Lambda([this]
	//					{
	//						auto StringItem = RoadsCombo->GetSelectedItem();
	//						if (StringItem)
	//						{
	//							return FText::FromString(StringItem->String);
	//						}
	//						return FText::FromString("None");
	//					})
	//				]
	//				.OptionsSource(&RoadsLabels)
	//				.InitiallySelectedItem(RoadsLabels[0])
	//				.OnGenerateWidget_Lambda([bSameValue](TSharedPtr<FStringIntPair> Item)
	//				{
	//					return SNew(STextBlock).Text(FText::FromString(Item->String));
	//				})
	//				.OnSelectionChanged(this, &SKytCSNWidget::OnRoadsSelectionChanged)
	//			]
	//		]

	//		+ SVerticalBox::Slot().HAlign(HAlign_Fill).AutoHeight()
	//		[
	//			SNew(SHorizontalBox)
	//			+ SHorizontalBox::Slot().VAlign(VAlign_Center).HAlign(HAlign_Fill).FillWidth(0.3)
	//			[
	//				SNew(STextBlock)
	//				.Text(FText::FromString("Sidewalks"))
	//			]
	//			+ SHorizontalBox::Slot().HAlign(HAlign_Fill).FillWidth(0.7).VAlign(VAlign_Center)
	//			[
	//				SAssignNew(SidewalksCombo,SComboBox<TSharedPtr<FStringIntPair>>)
	//				.ContentPadding(FMargin(6.0f, 2.0f))
	//				.Content()
	//				[
	//					SNew(STextBlock).Text_Lambda([this]
	//					{
	//						auto StringItem = SidewalksCombo->GetSelectedItem();
	//						if (StringItem)
	//						{
	//							return FText::FromString(StringItem->String);
	//						}
	//						return FText::FromString("None");
	//					})
	//				]
	//				.OptionsSource(&SidewalksLabels)
	//				.InitiallySelectedItem(SidewalksLabels[0])
	//				.OnGenerateWidget_Lambda([bSameValue](TSharedPtr<FStringIntPair> Item)
	//				{
	//					return SNew(STextBlock).Text(FText::FromString(Item->String));
	//				})
	//				.OnSelectionChanged(this, &SKytCSNWidget::OnSidewalksSelectionChanged)
	//			]
	//		]




	//		+ SVerticalBox::Slot().HAlign(HAlign_Fill).AutoHeight()
	//		[
	//			SNew(SHorizontalBox)
	//			+ SHorizontalBox::Slot().VAlign(VAlign_Center).HAlign(HAlign_Fill).FillWidth(0.3)
	//			[
	//				SNew(STextBlock)
	//				.Text(FText::FromString("Indices"))
	//			]
	//			+ SHorizontalBox::Slot().HAlign(HAlign_Fill).FillWidth(0.7).VAlign(VAlign_Center)
	//			[
	//				SAssignNew(IndicesCombo,SComboBox<TSharedPtr<FStringIntPair>>)
	//				.ContentPadding(FMargin(6.0f, 2.0f))
	//				.Content()
	//				[
	//					SNew(STextBlock).Text_Lambda([this]
	//					{
	//						auto StringItem = IndicesCombo->GetSelectedItem();
	//						if (StringItem)
	//						{
	//							return FText::FromString(StringItem->String);
	//						}
	//						return FText::FromString("None");
	//					})
	//				]
	//				.OptionsSource(&IndicesLabels)
	//				.InitiallySelectedItem(IndicesLabels[0])
	//				.OnGenerateWidget_Lambda([bSameValue](TSharedPtr<FStringIntPair> Item)
	//				{
	//					return SNew(STextBlock).Text(FText::FromString(Item->String));
	//				})
	//				.OnSelectionChanged(this, &SKytCSNWidget::OnIndicesSelectionChanged)
	//			]
	//		]




	//		+ SVerticalBox::Slot().HAlign(HAlign_Fill).AutoHeight()
	//		[
	//			SNew(SHorizontalBox)
	//			+ SHorizontalBox::Slot().VAlign(VAlign_Center).HAlign(HAlign_Fill).FillWidth(0.3)
	//			[
	//				SNew(STextBlock)
	//				.Text(FText::FromString("Vehicles"))
	//			]
	//			+ SHorizontalBox::Slot().HAlign(HAlign_Fill).FillWidth(0.7).VAlign(VAlign_Center)
	//			[
	//				SAssignNew(VehiclesCombo, SComboBox<TSharedPtr<FStringIntPair>> )
	//				.ContentPadding(FMargin(6.0f, 2.0f))
	//				.Content()
	//				[
	//					SNew(STextBlock).Text_Lambda([this]
	//					{
	//						auto Selectedtem = VehiclesCombo->GetSelectedItem();
	//						if (Selectedtem)
	//						{
	//							return FText::FromString(Selectedtem->String);
	//						}
	//						return FText::FromString("None");
	//					})
	//				]
	//				.OptionsSource(&VehiclesLabels)
	//				.InitiallySelectedItem(VehiclesLabels[0])
	//				.OnGenerateWidget_Lambda([bSameValue](TSharedPtr<FStringIntPair> Item)
	//				{
	//					return SNew(STextBlock).Text(FText::FromString(Item->String)); ;
	//				})
	//				.OnSelectionChanged(this, &SKytCSNWidget::OnVehicleSelectionChanged)
	//			]
	//		]




	//		+ SVerticalBox::Slot().HAlign(HAlign_Fill).AutoHeight()
	//		[
	//			SNew(SHorizontalBox)
	//			+ SHorizontalBox::Slot().VAlign(VAlign_Center).HAlign(HAlign_Fill).FillWidth(0.3)
	//			[
	//				SNew(STextBlock)
	//				.Text(FText::FromString("Timing"))
	//			]
	//			+ SHorizontalBox::Slot().HAlign(HAlign_Fill).FillWidth(0.7).VAlign(VAlign_Center)
	//			[
	//				SAssignNew(TimingsCombo,SComboBox<TSharedPtr<FStringIntPair>>)
	//				.ContentPadding(FMargin(6.0f, 2.0f))
	//				.Content()
	//				[
	//					SNew(STextBlock).Text_Lambda([this]
	//					{
	//						auto StringItem = TimingsCombo->GetSelectedItem();
	//						if (StringItem)
	//						{
	//							return FText::FromString(StringItem->String);
	//						}
	//						return FText::FromString("None");
	//					})
	//				]
	//				.OptionsSource(&TimingsLabels)
	//				.InitiallySelectedItem(TimingsLabels[0])
	//				.OnGenerateWidget_Lambda([bSameValue](TSharedPtr<FStringIntPair> Item)
	//				{
	//					return SNew(STextBlock).Text(FText::FromString(Item->String));
	//				})
	//				.OnSelectionChanged(this, &SKytCSNWidget::OnTimingSelectionChanged)
	//			]
	//		]


	//		+ SVerticalBox::Slot().HAlign(HAlign_Fill).AutoHeight()
	//		[
	//			SNew(SHorizontalBox)
	//			+ SHorizontalBox::Slot().VAlign(VAlign_Center).HAlign(HAlign_Fill).FillWidth(0.3)
	//			[
	//				SNew(STextBlock)
	//				.Text(FText::FromString("Timing Multiplier"))
	//			]
	//			+ SHorizontalBox::Slot().HAlign(HAlign_Fill).FillWidth(0.7).VAlign(VAlign_Center)
	//			[
	//				SAssignNew(TimingMultiplierCombo, SComboBox<TSharedPtr<FStringFloatPair>> )
	//				.ContentPadding(FMargin(6.0f, 2.0f))
	//				.Content()
	//				[
	//					SNew(STextBlock).Text_Lambda([this]
	//					{
	//						auto Selectedtem = TimingMultiplierCombo->GetSelectedItem();
	//						if (Selectedtem)
	//						{
	//							return FText::FromString(Selectedtem->String);
	//						}
	//						return FText::FromString("None");
	//					})
	//				]
	//				.OptionsSource(&TimingMultiplierSource)
	//				.InitiallySelectedItem(TimingMultiplierSource[2])
	//				.OnGenerateWidget_Lambda([bSameValue](TSharedPtr<FStringFloatPair> Item)
	//				{
	//					return SNew(STextBlock).Text(FText::FromString(Item->String)); ;
	//				})
	//				.OnSelectionChanged(this, &SKytCSNWidget::OnTimingMultiplierSelectionChanged)
	//			]
	//		]


	//		+ SVerticalBox::Slot().HAlign(HAlign_Fill).AutoHeight()
	//		[
	//			SNew(SHorizontalBox)
	//			+ SHorizontalBox::Slot().VAlign(VAlign_Center).HAlign(HAlign_Fill).FillWidth(0.3)
	//			[
	//				SNew(STextBlock)
	//				.Text(FText::FromString("LOD"))
	//			]
	//			+ SHorizontalBox::Slot().HAlign(HAlign_Fill).FillWidth(0.7).VAlign(VAlign_Center)
	//			[
	//				SAssignNew(LODsCombo, SComboBox<TSharedPtr<FStringIntPair>> )
	//				.ContentPadding(FMargin(6.0f, 2.0f))
	//				.Content()
	//				[
	//					SNew(STextBlock).Text_Lambda([this]
	//					{
	//						auto Selectedtem = LODsCombo->GetSelectedItem();
	//						if (Selectedtem)
	//						{
	//							return FText::FromString(Selectedtem->String);
	//						}
	//						return FText::FromString("None");
	//					})
	//				]
	//				.OptionsSource(&LODsLabels)
	//				.InitiallySelectedItem(LODsLabels[0])
	//				.OnGenerateWidget_Lambda([bSameValue](TSharedPtr<FStringIntPair> Item)
	//				{
	//					return SNew(STextBlock).Text(FText::FromString(Item->String)); ;
	//				})
	//				.OnSelectionChanged(this, &SKytCSNWidget::OnLODsSelectionChanged)
	//			]
	//		]

	//		+ SVerticalBox::Slot().AutoHeight().Padding(0, 20).HAlign(HAlign_Fill).AutoHeight()
	//		[
	//			SNew(SHorizontalBox)
	//			+ SHorizontalBox::Slot().VAlign(VAlign_Center).HAlign(HAlign_Fill)
	//			[
	//				SNew(SButton)
	//				.Text(FText::FromString("Spawn Vehicles"))
	//				.OnClicked(this, &SKytCSNWidget::OnSpawnVehicles)
	//			]
	//			+ SHorizontalBox::Slot().VAlign(VAlign_Center).HAlign(HAlign_Fill)
	//			[
	//				SNew(SButton)
	//				.Text(FText::FromString("Spawn Pedestrians"))
	//				.OnClicked(this, &SKytCSNWidget::OnSpawnPedestrians)
	//			]
	//			+ SHorizontalBox::Slot().VAlign(VAlign_Center).HAlign(HAlign_Fill)
	//			[
	//				SNew(SButton)
	//				.Text(FText::FromString("Play/Stop"))
	//				.OnClicked(this, &SKytCSNWidget::OnPlayCSN)
	//			]
	//			+ SHorizontalBox::Slot().VAlign(VAlign_Center).HAlign(HAlign_Fill)
	//			[
	//				SNew(SButton)
	//				.Text(FText::FromString("Reset"))
	//				.OnClicked(this, &SKytCSNWidget::OnResetCSN)
	//			]
	//			/*+ SHorizontalBox::Slot().HAlign(HAlign_Fill).FillWidth(0.7).VAlign(VAlign_Center)
	//			[
	//				SNew(SButton)
	//				.Text(FText::FromString("Import"))
	//				.OnClicked(this, &SKytCSNWidget::OnImportCSN)
	//			]*/
	//		]



	//	]
	//];

//
//SVatThumbnail::~SVatThumbnail()
//{
//
//}

