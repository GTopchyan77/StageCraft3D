// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#include "OverCrowdEditorStyle.h"
#include "OverCrowdEditor.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/SlateStyleRegistry.h"
#include "Slate/SlateGameResources.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyleMacros.h"

#define RootToContentDir Style->RootToContentDir

TSharedPtr<FSlateStyleSet> FOverCrowdEditorStyle::StyleInstance = nullptr;

void FOverCrowdEditorStyle::Initialize()
{
	if (!StyleInstance.IsValid())
	{
		StyleInstance = Create();
		FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
	}
}

void FOverCrowdEditorStyle::Shutdown()
{
	FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
	ensure(StyleInstance.IsUnique());
	StyleInstance.Reset();
}

FName FOverCrowdEditorStyle::GetStyleSetName()
{
	static FName StyleSetName(TEXT("OverCrowdEditorStyle"));
	return StyleSetName;
}


const FVector2D Icon16x16(16.0f, 16.0f);
const FVector2D Icon20x20(128.0f, 128.0f);

TSharedRef< FSlateStyleSet > FOverCrowdEditorStyle::Create()
{
	TSharedRef< FSlateStyleSet > Style = MakeShareable(new FSlateStyleSet("OverCrowdEditorStyle"));
	Style->SetContentRoot(IPluginManager::Get().FindPlugin("OverCrowd")->GetBaseDir() / TEXT("Resources"));

	Style->Set("OverCrowd.PluginAction", new IMAGE_BRUSH(TEXT("OCLogo"), Icon20x20));
	return Style;
}

void FOverCrowdEditorStyle::ReloadTextures()
{
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
	}
}

const ISlateStyle& FOverCrowdEditorStyle::Get()
{
	return *StyleInstance;
}
