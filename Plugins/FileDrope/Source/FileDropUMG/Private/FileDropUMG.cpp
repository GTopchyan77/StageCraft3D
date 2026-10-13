// Copyright 2024 Shape-Shift Studio GmbH. All Rights Reserved.

#include "FileDropUMG.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"

#define LOCTEXT_NAMESPACE "FFileDropUMGModule"

#define IMAGE_BRUSH( RelativePath, ... ) FSlateImageBrush( StyleInstance->RootToContentDir( RelativePath, TEXT(".png") ), __VA_ARGS__ )

TSharedPtr<FSlateStyleSet> StyleInstance = nullptr;

void FFileDropUMGModule::StartupModule()
{
    // This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
    // Create a new Slate style set
    StyleInstance = MakeShareable(new FSlateStyleSet("FileDropUMGStyle"));

    // Set the content root where the resources (icons) are stored
    FString PluginContentDir = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("FileDropUMG/Content/Slate"));
    StyleInstance->SetContentRoot(PluginContentDir);

    // Register your custom icon from the content folder
    StyleInstance->Set("ClassIcon.FileDrop", new IMAGE_BRUSH("icon", FVector2D(16.0f, 16.0f)));    

    // Register the Slate style set
    FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
}

void FFileDropUMGModule::ShutdownModule()
{
    // This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
    // we call this function before unloading the module.
    FSlateStyleRegistry::UnRegisterSlateStyle("FileDropUMGStyle");
    if (StyleInstance.IsValid())
    {
        FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
        StyleInstance.Reset();
    }
 }



IMPLEMENT_MODULE(FFileDropUMGModule, FileDropUMG)

#undef IMAGE_BRUSH
#undef LOCTEXT_NAMESPACE