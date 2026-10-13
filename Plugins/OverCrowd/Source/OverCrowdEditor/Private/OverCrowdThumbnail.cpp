// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved
#include "OverCrowdThumbnail.h"
#include "AssetThumbnail.h"
#include "OverCrowdEditorLibrary.h"

#define LOCTEXT_NAMESPACE "OverCrowdEditor"
void SOverCrowdThumbnailWidget::Construct(const FArguments& InArgs)
{

	Object = InArgs._InAsset;
	ThumbnailScale = InArgs._InScale;
	Justify = InArgs._InJustify;
	OnClicked = InArgs._OnClicked;
	TextYOffset = InArgs._InTextYOffset;
	ToolTipText = InArgs._InToolTipText;
	FString AdditionalName = InArgs._InName;

	FString AssetName = Object->GetName();
	if(AdditionalName.IsEmpty())
		AdditionalName = AssetName;
	
	FSlateFontInfo TitleFont = FAppStyle::Get().GetFontStyle("NormalFont");
	TitleFont.Size = 7.5;

	FSlateFontInfo HoverFont = FAppStyle::Get().GetFontStyle("NormalFont");
	HoverFont.Size = 15;
	HoverFont.OutlineSettings.OutlineSize = 1;
	HoverFont.OutlineSettings.OutlineColor = FLinearColor::Black;
	
	ThumbnailWidget = NewObject<UAssetThumbnailWidget>(UAssetThumbnailWidget::StaticClass());
	ThumbnailWidget->SetAsset(Object);
	ThumbnailWidget->SetResolution(FIntPoint(200 * ThumbnailScale,256*ThumbnailScale));

	// auto BorderBrush = new FSlateRoundedBoxBrush(FStyleColors::Transparent, FStyleColors::AccentYellow, 5.0f);
	// BorderBrush->OutlineSettings.CornerRadii = FVector4(10,10,10,10);
	// BorderBrush->OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;

	FSlateBrush EmptyBrush = *FCoreStyle::Get().GetBrush("ShowFlagsMenu.StaticMeshes");
	EmptyBrush.DrawAs = ESlateBrushDrawType::NoDrawType;
	
	TransparentButtonStyle.SetNormal(EmptyBrush);
	TransparentButtonStyle.SetPressed(EmptyBrush);
	TransparentButtonStyle.SetHovered(EmptyBrush);

	TSharedRef<SWidget> Thumbnail = ThumbnailWidget->RebuildWidget();
	FSlateRenderTransform Trans;
	Trans.SetTranslation(FVector2D(0,5));
	Thumbnail->SetRenderTransform(Trans);
	
	this->ChildSlot
	[
		SNew(SBox)
		.WidthOverride(256 * ThumbnailScale)  
		[
			SNew(SOverlay)
			+SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill)
			[
				SNew(SOverlay)
				+SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill)
				[
					SNew(SBox).Clipping(EWidgetClipping::ClipToBounds)
					[
						Thumbnail
					]	
				]
				+SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill)
				[
					SNew(SButton)
					//.ToolTipText(LOCTEXT("SelectMultipleTip", "(Ctrl + LeftMouse) to select multiple"))
					.ToolTipText(ToolTipText)
					.ButtonStyle(&TransparentButtonStyle)
					.ButtonColorAndOpacity(FSlateColor(FColor(0,0,0,0)))
					.ForegroundColor(FSlateColor(FColor(0,0,0,0)))
					
					.OnHovered_Lambda([this] { bIsHovering = true;  })
					.OnUnhovered_Lambda([this] { bIsHovering = false; })
					.OnClicked_Lambda([this]()
					{
						if(OnClicked.IsBound())
							return OnClicked.Execute();
					
						return FReply::Handled();
					})
				]
			]
			
			+SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Clipping(EWidgetClipping::Inherit)
				.Font(HoverFont)
				.ColorAndOpacity(FSlateColor(FColor(255,255,255,255)))
				.Text(FText::FromString(AdditionalName))
				.Justification(Justify)
				.AutoWrapText(false)
				.Visibility_Lambda([this]()
				{
					return (bIsHovering) ? EVisibility::HitTestInvisible : EVisibility::Hidden;
				})
			]
			+SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Top).Padding(0,TextYOffset,0,0)//.ZOrder(91000)
			[
				SNew(STextBlock)
				.Clipping(EWidgetClipping::Inherit)
				.Font(TitleFont)
				.ColorAndOpacity(FSlateColor(FColor(255,255,255,255)))
				.Text(FText::FromString(AssetName))
				.Justification(ETextJustify::Left)
				.AutoWrapText(true).WrappingPolicy(ETextWrappingPolicy::AllowPerCharacterWrapping)
				.Visibility_Lambda([this]()
				{
					return (ThumbnailScale > 0.35f && !bIsHovering) ? EVisibility::HitTestInvisible : EVisibility::Hidden;
				})
			]
		]
	];
}

