// Copyright DamenTools internal. Damen File Manager plugin.

#include "DamenFileManagerWidget.h"
#include "DamenDriveButton.h"
#include "DamenFileManager.h"
#include "Components/TextBlock.h"
#include "HAL/FileManager.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h" // GetLogicalDrives
#endif
#include "FileDrop.h"
#include "Blueprint/GameViewportSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/BorderSlot.h"
#include "ModifyU/UI/DlFmButton.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/VerticalBoxSlot.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	/**
	 * The one Blueprint-only class this widget has to reach into. Kept as a
	 * single named constant so a moved/renamed asset is a one-line fix, and so
	 * the error logs below can quote the exact path they failed to load.
	 * See DamenFileManagerWidget.h's ImportFileViaDamenImporter comment for the
	 * full contract this path is half of.
	 */
	const TCHAR* const DamenImporterClassPath = TEXT("/Game/DamenTool/Blueprints/DamenImporterBP.DamenImporterBP_C");

	/**
	 * Stretches the drop zone across its whole anchor, whichever panel type the
	 * Designer used for FileDropAnchor.
	 *
	 * FileDropAnchor is typed UPanelWidget rather than UOverlay specifically so
	 * WBP_DamenFileManager could be built through the ue5-ngg MCP bridge: that
	 * bridge's ue5_add_widget_to_blueprint silently creates a TextBlock instead
	 * of an Overlay/Border/SizeBox when appending to an existing Widget
	 * Blueprint (known, reproduced bug - VerticalBox/HorizontalBox are among the
	 * types it does create correctly), and every Overlay already in the
	 * duplicated vendor tree is load-bearing (the sort-arrow indicators inside
	 * the Button_SortBy_* buttons). An Overlay anchor is still handled here and
	 * is the nicer choice if one is ever added by hand in the Designer.
	 *
	 * REQUIRED, not cosmetic: SFileDrop::ComputeDesiredSize() returns
	 * FVector2D::ZeroVector, and every UMG slot type defaults to a non-filling
	 * alignment (UOverlaySlot is HAlign_Left/VAlign_Top, box slots are
	 * ESlateSizeRule::Automatic), so a default-slotted UFileDrop is laid out at
	 * zero size - it draws nothing and can never be hit by a drag. Filling the
	 * anchor is what gives it droppable area. (The same zero-size trap is why a
	 * Center/Center-slotted UFileDrop, as in the older WBP_FBXdrop, has no area
	 * of its own either.)
	 *
	 * @return true if the slot type was recognised and stretched.
	 */
	bool StretchSlotToFillAnchor(UPanelSlot* Slot)
	{
		if (UOverlaySlot* const AsOverlay = Cast<UOverlaySlot>(Slot))
		{
			AsOverlay->SetHorizontalAlignment(HAlign_Fill);
			AsOverlay->SetVerticalAlignment(VAlign_Fill);
			return true;
		}
		if (UVerticalBoxSlot* const AsVerticalBox = Cast<UVerticalBoxSlot>(Slot))
		{
			AsVerticalBox->SetHorizontalAlignment(HAlign_Fill);
			AsVerticalBox->SetVerticalAlignment(VAlign_Fill);
			AsVerticalBox->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			return true;
		}
		if (UHorizontalBoxSlot* const AsHorizontalBox = Cast<UHorizontalBoxSlot>(Slot))
		{
			AsHorizontalBox->SetHorizontalAlignment(HAlign_Fill);
			AsHorizontalBox->SetVerticalAlignment(VAlign_Fill);
			AsHorizontalBox->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			return true;
		}
		if (UBorderSlot* const AsBorder = Cast<UBorderSlot>(Slot))
		{
			AsBorder->SetHorizontalAlignment(HAlign_Fill);
			AsBorder->SetVerticalAlignment(VAlign_Fill);
			return true;
		}
		return false;
	}

	/**
	 * Calls a Blueprint UFUNCTION via reflection. Same shape (and same reason)
	 * as FScopedBPCall in PreciseTransformLibrary.cpp - reimplemented locally
	 * rather than depending on that module, to keep this plugin self-contained:
	 * it allocates the parameter block from Func->ParmsSize, initialises and
	 * destroys it properly, and writes params by NAME rather than through a
	 * hand-rolled positional struct, so a parameter reorder/rename in the
	 * Blueprint can be detected instead of silently corrupting the call.
	 */
	struct FScopedBPCall
	{
		UFunction* Func = nullptr;
		void* Params = nullptr;

		explicit FScopedBPCall(UFunction* InFunc) : Func(InFunc)
		{
			if (Func && Func->ParmsSize > 0)
			{
				Params = FMemory::Malloc(Func->ParmsSize, Func->GetMinAlignment());
				FMemory::Memzero(Params, Func->ParmsSize);
				Func->InitializeStruct(Params);
			}
		}

		~FScopedBPCall()
		{
			if (Params)
			{
				Func->DestroyStruct(Params);
				FMemory::Free(Params);
			}
		}

		template <typename T>
		bool SetParam(const FProperty* Prop, const T& Value)
		{
			if (!Func || !Params || !Prop) return false;
			*Prop->ContainerPtrToValuePtr<T>(Params) = Value;
			return true;
		}

		void Call(UObject* Obj)
		{
			if (Func && Obj)
			{
				Obj->ProcessEvent(Func, Params);
			}
		}
	};
}

UDamenFileManagerWidget::UDamenFileManagerWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Matches the existing DamenTool FBX pipeline (DamenImporterBP/WBP_FBXdrop).
	// No leading dot - see the ImportFileExtensions comment in the header.
	ImportFileExtensions = { TEXT("fbx") };

	DriveBarLabel = NSLOCTEXT("DamenFileManager", "DriveBarLabel", "Drives:");
	DriveButtonTextColor = FSlateColor(FLinearColor::Black); // default UButton style is light grey
	DriveBarFontSize = 14;
	DriveBarItemPadding = FMargin(4.f, 2.f);
}

TArray<FString> UDamenFileManagerWidget::GetAvailableDrives()
{
	TArray<FString> Drives;
#if PLATFORM_WINDOWS
	constexpr int32 NumDriveLetters = 26;
	const uint32 DriveMask = ::GetLogicalDrives();
	for (int32 Letter = 0; Letter < NumDriveLetters; ++Letter)
	{
		if (DriveMask & (1u << Letter))
		{
			const FString Root = FString::Printf(TEXT("%c:/"), static_cast<TCHAR>(TEXT('A') + Letter));
			if (IFileManager::Get().DirectoryExists(*Root))
			{
				Drives.Add(Root);
			}
		}
	}
#else
	Drives.Add(TEXT("/"));
#endif
	return Drives;
}

void UDamenFileManagerWidget::BrowseToDrive(const FString& DriveRoot)
{
	UE_LOG(LogDamenFileManager, Log, TEXT("BrowseToDrive: browsing to '%s'."), *DriveRoot);
	Func_BrowseToPath(DriveRoot);
}

void UDamenFileManagerWidget::SetVisibility(ESlateVisibility InVisibility)
{
	Super::SetVisibility(InVisibility);
	if (InVisibility != ESlateVisibility::Collapsed && InVisibility != ESlateVisibility::Hidden)
	{
		RebuildDriveBar();
	}
}

void UDamenFileManagerWidget::ShowStandalone(int32 ZOrder, FVector2D Size, FVector2D Alignment)
{
	UGameViewportSubsystem* const Subsystem = UGameViewportSubsystem::Get(GetWorld());
	if (!Subsystem)
	{
		UE_LOG(LogDamenFileManager, Error, TEXT("ShowStandalone: no GameViewportSubsystem for widget '%s' - panel NOT shown."), *GetName());
		return;
	}

	// SetWidgetSlotDesiredSize zeroes Anchors, so anchors are set after it - see header.
	FGameViewportWidgetSlot ViewportSlot = UGameViewportSubsystem::SetWidgetSlotDesiredSize(FGameViewportWidgetSlot(), Size);
	ViewportSlot.Anchors = FAnchors(0.5f, 0.5f);
	ViewportSlot.Alignment = Alignment;
	ViewportSlot.ZOrder = ZOrder;

	if (Subsystem->IsWidgetAdded(this))
	{
		Subsystem->SetWidgetSlot(this, ViewportSlot);
	}
	else
	{
		Subsystem->AddWidget(this, ViewportSlot);
	}

	SetVisibility(ESlateVisibility::Visible);
}

void UDamenFileManagerWidget::RequestClose()
{
	SetVisibility(ESlateVisibility::Collapsed);
	OnCloseRequested.Broadcast();
}

void UDamenFileManagerWidget::BindCloseRequested(FDamenFileManagerCloseRequestedDelegate Event)
{
	if (!Event.IsBound())
	{
		UE_LOG(LogDamenFileManager, Warning, TEXT("BindCloseRequested: called on '%s' with an unbound event - ignored."), *GetName());
		return;
	}

	FDamenFileManagerCloseRequestedSignature::FDelegate AsMulticastEntry;
	AsMulticastEntry.BindUFunction(Event.GetUObject(), Event.GetFunctionName());
	OnCloseRequested.AddUnique(AsMulticastEntry);
}

void UDamenFileManagerWidget::RebuildDriveBar()
{
	if (!DriveBarAnchor || !WidgetTree)
	{
		return; // optional anchor - NativeConstruct already warned once
	}

	DriveBarAnchor->ClearChildren();

	auto AddToBar = [this](UWidget* Child)
	{
		UPanelSlot* const Slot = DriveBarAnchor->AddChild(Child);
		if (UHorizontalBoxSlot* const BoxSlot = Cast<UHorizontalBoxSlot>(Slot))
		{
			BoxSlot->SetPadding(DriveBarItemPadding);
			BoxSlot->SetVerticalAlignment(VAlign_Center);
		}
	};

	if (!DriveBarLabel.IsEmpty())
	{
		UTextBlock* const Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Label->SetText(DriveBarLabel);
		FSlateFontInfo LabelFont = Label->GetFont();
		LabelFont.Size = DriveBarFontSize;
		Label->SetFont(LabelFont);
		AddToBar(Label);
	}

	const TArray<FString> Drives = GetAvailableDrives();
	for (const FString& DriveRoot : Drives)
	{
		UDamenDriveButton* const Button = WidgetTree->ConstructWidget<UDamenDriveButton>(UDamenDriveButton::StaticClass());
		Button->Init(this, DriveRoot);
		Button->SetToolTipText(FText::FromString(DriveRoot));

		UTextBlock* const Caption = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		FString CaptionText = DriveRoot;
		CaptionText.RemoveFromEnd(TEXT("/"));
		Caption->SetText(FText::FromString(CaptionText));
		Caption->SetColorAndOpacity(DriveButtonTextColor);
		FSlateFontInfo CaptionFont = Caption->GetFont();
		CaptionFont.Size = DriveBarFontSize;
		Caption->SetFont(CaptionFont);
		Button->AddChild(Caption);

		AddToBar(Button);
	}

	UE_LOG(LogDamenFileManager, Log, TEXT("RebuildDriveBar: %d drive(s): %s"), Drives.Num(), *FString::Join(Drives, TEXT(" ")));
}

void UDamenFileManagerWidget::NativeConstruct()
{
	// Super FIRST, and unconditionally: UDlFmMain_Logic::NativeConstruct is not
	// an empty stub - it does the vendor file browser's real initialisation
	// (start path, directory/file list views, every Button_ and Toggle_ binding).
	// Anything below here that early-returns must still have let it run, or the
	// browser half of this widget comes up dead.
	Super::NativeConstruct();

	// Click-to-import: independent of the drop zone below, so it's bound
	// unconditionally, before the FileDropAnchor early-return. Fires on
	// double-click of a file row or on Button_SelectConfirm - both already
	// implemented by the vendor browser (see HandleFileListSelectConfirmed's
	// doc comment in the header).
	OnFileListSelectConfirmed.AddDynamic(this, &UDamenFileManagerWidget::HandleFileListSelectConfirmed);

	// AddUnique: NativeConstruct re-runs on every AddToViewport.
	if (Button_Close)
	{
		Button_Close->OnClicked.AddUniqueDynamic(this, &UDamenFileManagerWidget::RequestClose);
	}
	else
	{
		UE_LOG(LogDamenFileManager, Warning,
			TEXT("NativeConstruct: widget '%s' has no DlFmButton named 'Button_Close' - the panel cannot be closed from its own X button."), *GetName());
	}

	// Also before the FileDropAnchor early-return: drives are independent of drag-and-drop.
	if (DriveBarAnchor)
	{
		RebuildDriveBar();
	}
	else
	{
		UE_LOG(LogDamenFileManager, Warning,
			TEXT("NativeConstruct: widget '%s' has no panel named 'DriveBarAnchor' - no drive buttons (type a drive into the path box instead)."), *GetName());
	}

	if (!FileDropAnchor)
	{
		UE_LOG(LogDamenFileManager, Warning,
			TEXT("NativeConstruct: widget '%s' (class %s) has no panel widget named 'FileDropAnchor' - drag-and-drop FBX import is DISABLED for this instance (the file browser itself still works). Add a panel named EXACTLY 'FileDropAnchor' to the Widget Blueprint - see DamenFileManagerWidget.h."),
			*GetName(), *GetClass()->GetName());
		return;
	}

	// NativeConstruct runs every time the widget is added to the viewport, so a
	// second pass must not parent a second drop zone into the anchor.
	if (FileDropWidget)
	{
		if (FileDropWidget->GetParent() != FileDropAnchor.Get())
		{
			StretchSlotToFillAnchor(FileDropAnchor->AddChild(FileDropWidget));
		}
		return;
	}

	if (!WidgetTree)
	{
		UE_LOG(LogDamenFileManager, Error,
			TEXT("NativeConstruct: widget '%s' has no WidgetTree - cannot construct the drop zone, drag-and-drop FBX import is disabled."),
			*GetName());
		return;
	}

	FileDropWidget = WidgetTree->ConstructWidget<UFileDrop>(UFileDrop::StaticClass(), TEXT("DamenFileManager_FileDrop"));
	if (!FileDropWidget)
	{
		UE_LOG(LogDamenFileManager, Error,
			TEXT("NativeConstruct: failed to construct the UFileDrop drop zone for widget '%s' - drag-and-drop FBX import is disabled. Is the FileDropUMG plugin enabled?"),
			*GetName());
		return;
	}

	FileDropWidget->SetCheckFileExtensions(true);
	FileDropWidget->SetAllowedFileExtensions(ImportFileExtensions);
	FileDropWidget->OnFilesDropped.AddDynamic(this, &UDamenFileManagerWidget::HandleFilesDropped);

	UPanelSlot* const DropSlot = FileDropAnchor->AddChild(FileDropWidget);
	if (!DropSlot)
	{
		UE_LOG(LogDamenFileManager, Error,
			TEXT("NativeConstruct: FileDropAnchor ('%s', class %s) refused the drop zone as a child - nothing can be dropped on it. Is FileDropAnchor a panel that accepts children?"),
			*FileDropAnchor->GetName(), *FileDropAnchor->GetClass()->GetName());
		return;
	}

	// See StretchSlotToFillAnchor: without this the drop zone is laid out at
	// zero size and silently catches nothing.
	if (!StretchSlotToFillAnchor(DropSlot))
	{
		UE_LOG(LogDamenFileManager, Error,
			TEXT("NativeConstruct: FileDropAnchor ('%s') produced an unhandled slot type '%s' - the drop zone could not be stretched to fill it and will have zero size, so nothing can be dropped on it. Use an Overlay, VerticalBox, HorizontalBox or Border for FileDropAnchor, or extend StretchSlotToFillAnchor in DamenFileManagerWidget.cpp."),
			*FileDropAnchor->GetName(), *DropSlot->GetClass()->GetName());
		return;
	}

	UE_LOG(LogDamenFileManager, Log,
		TEXT("NativeConstruct: drag-and-drop import armed on '%s' - accepting %d extension(s): %s"),
		*GetName(), ImportFileExtensions.Num(), *FString::Join(ImportFileExtensions, TEXT(", ")));
}

void UDamenFileManagerWidget::HandleFilesDropped(const TArray<FString>& FilePaths)
{
	if (FilePaths.Num() == 0)
	{
		UE_LOG(LogDamenFileManager, Warning, TEXT("HandleFilesDropped: drop event fired with zero file paths - nothing imported."));
		return;
	}

	int32 ImportedCount = 0;

	for (const FString& FilePath : FilePaths)
	{
		// Defence in depth: UFileDrop already filters by extension, but its
		// filter is a runtime-settable property on a vendor widget, so the
		// import path re-checks rather than trusting it.
		if (!IsExtensionAllowed(FilePath))
		{
			UE_LOG(LogDamenFileManager, Warning,
				TEXT("HandleFilesDropped: REJECTED '%s' - extension '%s' is not in ImportFileExtensions (%s)."),
				*FilePath, *FPaths::GetExtension(FilePath), *FString::Join(ImportFileExtensions, TEXT(", ")));
			continue;
		}

		if (ImportFileViaDamenImporter(FilePath))
		{
			++ImportedCount;
			UE_LOG(LogDamenFileManager, Log, TEXT("HandleFilesDropped: sent '%s' to DamenImporterBP.ImportMesh."), *FilePath);
		}
		// Failure is already logged (with the specific cause) by ImportFileViaDamenImporter.
	}

	UE_LOG(LogDamenFileManager, Log,
		TEXT("HandleFilesDropped: %d of %d dropped file(s) were handed to DamenImporterBP.ImportMesh."),
		ImportedCount, FilePaths.Num());
}

bool UDamenFileManagerWidget::ImportFileViaDamenImporter(const FString& FilePath) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogDamenFileManager, Error,
			TEXT("ImportFileViaDamenImporter: no UWorld from widget '%s' - cannot find the DamenImporterBP actor, '%s' NOT imported."),
			*GetName(), *FilePath);
		return false;
	}

	// See header comment: this class path must match DamenImporterBP exactly.
	UClass* const ImporterClass = StaticLoadClass(AActor::StaticClass(), nullptr, DamenImporterClassPath);
	if (!ImporterClass)
	{
		UE_LOG(LogDamenFileManager, Error,
			TEXT("ImportFileViaDamenImporter: could not load the DamenImporterBP generated class at '%s' - the asset may have been renamed/moved/deleted. '%s' NOT imported. Update DamenImporterClassPath in DamenFileManagerWidget.cpp (and the header comment) if DamenImporterBP moved."),
			DamenImporterClassPath, *FilePath);
		return false;
	}

	// Same resolution WBP_FBXdrop's existing Blueprint drop flow uses: the
	// single DamenImporterBP actor placed in the currently loaded level.
	AActor* const Importer = UGameplayStatics::GetActorOfClass(World, ImporterClass);
	if (!Importer)
	{
		UE_LOG(LogDamenFileManager, Error,
			TEXT("ImportFileViaDamenImporter: no actor of class '%s' found in level '%s' - the level must contain a placed DamenImporterBP actor. '%s' NOT imported."),
			*ImporterClass->GetName(), *World->GetMapName(), *FilePath);
		return false;
	}

	UFunction* const ImportFunc = Importer->GetClass()->FindFunctionByName(TEXT("ImportMesh"));
	if (!ImportFunc)
	{
		UE_LOG(LogDamenFileManager, Error,
			TEXT("ImportFileViaDamenImporter: function 'ImportMesh' not found on class '%s' - DamenImporterBP may have been renamed/refactored, see DamenFileManagerWidget.h. '%s' NOT imported."),
			*Importer->GetClass()->GetName(), *FilePath);
		return false;
	}

	// Matched by name and type-checked, not assumed positionally - a renamed or
	// retyped parameter must be an error, never a silently wrong call.
	const FStrProperty* const FileLocationProp = CastField<FStrProperty>(ImportFunc->FindPropertyByName(TEXT("FileLocation")));
	if (!FileLocationProp)
	{
		UE_LOG(LogDamenFileManager, Error,
			TEXT("ImportFileViaDamenImporter: 'ImportMesh' on class '%s' has no FString parameter named 'FileLocation' (MISSING/WRONG TYPE) - DamenImporterBP's event signature changed, see DamenFileManagerWidget.h. '%s' NOT imported."),
			*Importer->GetClass()->GetName(), *FilePath);
		return false;
	}

	FScopedBPCall Call(ImportFunc);
	if (!Call.SetParam<FString>(FileLocationProp, FilePath))
	{
		UE_LOG(LogDamenFileManager, Error,
			TEXT("ImportFileViaDamenImporter: failed to write the 'FileLocation' parameter for 'ImportMesh' on class '%s'. '%s' NOT imported."),
			*Importer->GetClass()->GetName(), *FilePath);
		return false;
	}

	// ProcessEvent is synchronous: ImportMesh's whole Blueprint graph runs to
	// completion (nothing else, including Tick, interleaves) before this
	// returns, so the per-file logging in HandleFilesDropped stays in the same
	// order as the imports actually happened.
	Call.Call(Importer);
	return true;
}

bool UDamenFileManagerWidget::IsExtensionAllowed(const FString& FilePath) const
{
	const FString Extension = FPaths::GetExtension(FilePath);
	for (const FString& Allowed : ImportFileExtensions)
	{
		// Entries are expected dot-less ("fbx"), but tolerate ".fbx" so a
		// hand-edited default in the WBP can't silently reject everything.
		const FString Normalized = Allowed.StartsWith(TEXT(".")) ? Allowed.RightChop(1) : Allowed;
		if (Extension.Equals(Normalized, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

void UDamenFileManagerWidget::HandleFileListSelectConfirmed(const TArray<FDlUI_FileExplorer_ItemInfo>& FileNodeList)
{
	if (FileNodeList.Num() == 0)
	{
		UE_LOG(LogDamenFileManager, Warning, TEXT("HandleFileListSelectConfirmed: select-confirm fired with zero items - nothing imported."));
		return;
	}

	int32 ImportedCount = 0;

	for (const FDlUI_FileExplorer_ItemInfo& Item : FileNodeList)
	{
		if (Item.bIsDirectory)
		{
			// Defence in depth: the double-click path (Single_Confirm) never
			// reaches this delegate for a directory, but Button_SelectConfirm's
			// "select current path when nothing is selected" fallback can.
			UE_LOG(LogDamenFileManager, Warning,
				TEXT("HandleFileListSelectConfirmed: REJECTED '%s' - it's a directory, not a file."), *Item.Name);
			continue;
		}

		if (!IsExtensionAllowed(Item.Name))
		{
			UE_LOG(LogDamenFileManager, Warning,
				TEXT("HandleFileListSelectConfirmed: REJECTED '%s' - extension '%s' is not in ImportFileExtensions (%s)."),
				*Item.Name, *Item.Extension, *FString::Join(ImportFileExtensions, TEXT(", ")));
			continue;
		}

		if (ImportFileViaDamenImporter(Item.Name))
		{
			++ImportedCount;
			UE_LOG(LogDamenFileManager, Log, TEXT("HandleFileListSelectConfirmed: sent '%s' to DamenImporterBP.ImportMesh."), *Item.Name);
		}
		// Failure is already logged (with the specific cause) by ImportFileViaDamenImporter.
	}

	UE_LOG(LogDamenFileManager, Log,
		TEXT("HandleFileListSelectConfirmed: %d of %d confirmed item(s) were handed to DamenImporterBP.ImportMesh."),
		ImportedCount, FileNodeList.Num());
}
