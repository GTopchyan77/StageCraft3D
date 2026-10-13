// Copyright(c) 2022 Dragon Li Software Limited. All Rights Reserved.

#include "DlFileManager.h"

#define LOCTEXT_NAMESPACE "FDlFileManagerModule"

void FDlFileManagerModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
}

void FDlFileManagerModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FDlFileManagerModule, DlFileManager)