//  Copyright WASD Studio 2025 All Rights Reserved.

#include "UMGInSequenceEditorModule.h"

#include "PropertyEditorDelegates.h"
#include "PropertyEditorModule.h"
#include "UISSequencerSectionBPCustomization.h"

#define LOCTEXT_NAMESPACE "FUMGInSequenceEditorModule"

void FUMGInSequenceEditorModule::StartupModule()
{
	FPropertyEditorModule& PropertyModule =
	FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	
	PropertyModule.RegisterCustomClassLayout(
		TEXT("UISSequencerSectionBP"),
		FOnGetDetailCustomizationInstance::CreateStatic(&FUISSequencerSectionBPCustomization::MakeInstance)
	);

	PropertyModule.NotifyCustomizationModuleChanged();
}

void FUMGInSequenceEditorModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
	if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
	{
		FPropertyEditorModule& PropertyModule =
			FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");

		PropertyModule.UnregisterCustomClassLayout(TEXT("UISSequencerSectionBP"));
		PropertyModule.NotifyCustomizationModuleChanged();
	}
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FUMGInSequenceEditorModule, UMGInSequenceEditor)