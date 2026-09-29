// Copyright 2025-2026 NGG. All Rights Reserved.
//
// NGGWidgets.cpp
//
// Implementation of the UMG / Widget Blueprint endpoints:
//   /widgets/{create,add,remove,reparent,rename,compile}
//   /widgets/{get_tree,style}
//
// The widget-type registry and the slot-property plumbing shared by all of
// these live in the NGGWidgetPriv namespace at the top of this file. Handlers
// call them qualified — adaptive non-unity builds fail with C2668 on
// unqualified static helpers shared across handler blocks.

#include "NGGHttpServer.h"
#include "UnrealNGGMCPModule.h"

// ---- UE5 HTTP Server -------------------------------------------------------
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
// ---- Engine ----------------------------------------------------------------
#include "Async/Async.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Editor.h"
// ---- Asset tools -----------------------------------------------------------
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "FileHelpers.h"          // UEditorLoadingAndSavingUtils
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
// ---- Widget Blueprint creation ----------------------------------------------
#include "WidgetBlueprintFactory.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/EditableTextBox.h"
#include "Components/ComboBoxString.h"
#include "Components/ProgressBar.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/Slider.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/GridPanel.h"
#include "Components/GridSlot.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/WrapBox.h"
#include "Components/WrapBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/SizeBox.h"
#include "Components/ScaleBox.h"
#include "Components/ScaleBoxSlot.h"
#include "Components/SafeZone.h"
#include "Components/SafeZoneSlot.h"
#include "Components/Spacer.h"
#include "Components/WidgetSwitcher.h"
#include "Components/NamedSlot.h"
#include "Components/CheckBox.h"
#include "Components/RichTextBlock.h"
#include "Components/CircularThrobber.h"
#include "Components/Throbber.h"
#include "Components/BackgroundBlur.h"
#include "Components/RetainerBox.h"
#include "Components/InvalidationBox.h"
#include "Components/MenuAnchor.h"
#include "Components/MultiLineEditableText.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/EditableText.h"
#include "Blueprint/UserWidget.h"
#include "Styling/SlateBrush.h"
// ---- UObject reflection ----------------------------------------------------
#include "UObject/PropertyIterator.h"
#include "Engine/Texture2D.h"

// ============================================================================
// Shared widget helpers (used by HandleCreateWidgetBlueprint, HandleStyleWidgets,
// HandleGetWidgetTree, HandleRemoveWidgetFromBlueprint, HandleReparentWidget,
// HandleRenameWidget, HandleCompileWidgetBlueprint).
// Call qualified as NGGWidgetPriv:: from handlers — adaptive non-unity builds
// fail with C2668 on unqualified static helpers shared across handler blocks.
// ============================================================================
namespace NGGWidgetPriv
{
	static UClass* ResolveWidgetClass(const FString& TypeName, const FString& UserWidgetClassPath)
	{
		if (TypeName == TEXT("UserWidget") || TypeName == TEXT("UserWidgetInstance"))
		{
			if (UserWidgetClassPath.IsEmpty()) return nullptr;
			if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *UserWidgetClassPath))
			{
				return BP->GeneratedClass ? BP->GeneratedClass : nullptr;
			}
			return nullptr;
		}
		if (TypeName == TEXT("Button"))                 return UButton::StaticClass();
		if (TypeName == TEXT("TextBlock"))              return UTextBlock::StaticClass();
		if (TypeName == TEXT("ComboBoxString"))         return UComboBoxString::StaticClass();
		if (TypeName == TEXT("EditableTextBox"))        return UEditableTextBox::StaticClass();
		if (TypeName == TEXT("EditableText"))           return UEditableText::StaticClass();
		if (TypeName == TEXT("ProgressBar"))            return UProgressBar::StaticClass();
		if (TypeName == TEXT("Image"))                  return UImage::StaticClass();
		if (TypeName == TEXT("Slider"))                 return USlider::StaticClass();
		if (TypeName == TEXT("CheckBox"))               return UCheckBox::StaticClass();
		if (TypeName == TEXT("VerticalBox"))            return UVerticalBox::StaticClass();
		if (TypeName == TEXT("HorizontalBox"))          return UHorizontalBox::StaticClass();
		if (TypeName == TEXT("CanvasPanel"))            return UCanvasPanel::StaticClass();
		if (TypeName == TEXT("Overlay"))                return UOverlay::StaticClass();
		if (TypeName == TEXT("Border"))                 return UBorder::StaticClass();
		if (TypeName == TEXT("ScrollBox"))              return UScrollBox::StaticClass();
		if (TypeName == TEXT("SizeBox"))                return USizeBox::StaticClass();
		if (TypeName == TEXT("GridPanel"))              return UGridPanel::StaticClass();
		if (TypeName == TEXT("UniformGridPanel"))       return UUniformGridPanel::StaticClass();
		if (TypeName == TEXT("WrapBox"))                return UWrapBox::StaticClass();
		if (TypeName == TEXT("Spacer"))                 return USpacer::StaticClass();
		if (TypeName == TEXT("ScaleBox"))               return UScaleBox::StaticClass();
		if (TypeName == TEXT("WidgetSwitcher"))         return UWidgetSwitcher::StaticClass();
		if (TypeName == TEXT("NamedSlot"))              return UNamedSlot::StaticClass();
		if (TypeName == TEXT("RichTextBlock"))          return URichTextBlock::StaticClass();
		if (TypeName == TEXT("SafeZone"))               return USafeZone::StaticClass();
		if (TypeName == TEXT("Throbber"))               return UThrobber::StaticClass();
		if (TypeName == TEXT("CircularThrobber"))       return UCircularThrobber::StaticClass();
		if (TypeName == TEXT("BackgroundBlur"))         return UBackgroundBlur::StaticClass();
		if (TypeName == TEXT("RetainerBox"))            return URetainerBox::StaticClass();
		if (TypeName == TEXT("InvalidationBox"))        return UInvalidationBox::StaticClass();
		if (TypeName == TEXT("MenuAnchor"))             return UMenuAnchor::StaticClass();
		if (TypeName == TEXT("MultiLineEditableText"))  return UMultiLineEditableText::StaticClass();
		if (TypeName == TEXT("MultiLineEditableTextBox"))return UMultiLineEditableTextBox::StaticClass();

		if (UserWidgetClassPath.IsEmpty()) return UTextBlock::StaticClass();
		return nullptr;
	}

	static EHorizontalAlignment ParseHAlign(const FString& V)
	{
		if (V == TEXT("Left"))   return HAlign_Left;
		if (V == TEXT("Right"))  return HAlign_Right;
		if (V == TEXT("Center")) return HAlign_Center;
		if (V == TEXT("Fill"))   return HAlign_Fill;
		return HAlign_Fill;
	}
	static FString HAlignToString(EHorizontalAlignment V)
	{
		switch (V)
		{
		case HAlign_Left:   return TEXT("Left");
		case HAlign_Center: return TEXT("Center");
		case HAlign_Right:  return TEXT("Right");
		case HAlign_Fill:   return TEXT("Fill");
		default:            return TEXT("Fill");
		}
	}

	static EVerticalAlignment ParseVAlign(const FString& V)
	{
		if (V == TEXT("Top"))    return VAlign_Top;
		if (V == TEXT("Bottom")) return VAlign_Bottom;
		if (V == TEXT("Center")) return VAlign_Center;
		if (V == TEXT("Fill"))   return VAlign_Fill;
		return VAlign_Fill;
	}
	static FString VAlignToString(EVerticalAlignment V)
	{
		switch (V)
		{
		case VAlign_Top:    return TEXT("Top");
		case VAlign_Center: return TEXT("Center");
		case VAlign_Bottom: return TEXT("Bottom");
		case VAlign_Fill:   return TEXT("Fill");
		default:            return TEXT("Fill");
		}
	}

	static ESlateVisibility ParseVisibility(const FString& V)
	{
		if (V == TEXT("Collapsed"))            return ESlateVisibility::Collapsed;
		if (V == TEXT("Hidden"))               return ESlateVisibility::Hidden;
		if (V == TEXT("HitTestInvisible"))     return ESlateVisibility::HitTestInvisible;
		if (V == TEXT("SelfHitTestInvisible")) return ESlateVisibility::SelfHitTestInvisible;
		return ESlateVisibility::Visible;
	}

	static FLinearColor ParseHexColor(const FString& Hex)
	{
		FString Clean = Hex.StartsWith(TEXT("#")) ? Hex.Mid(1) : Hex;
		return FLinearColor(FColor::FromHex(Clean));
	}
	static FString ColorToHex(const FLinearColor& C)
	{
		const FColor SRGB = C.ToFColor(true);
		return FString::Printf(TEXT("#%02X%02X%02X%02X"), SRGB.R, SRGB.G, SRGB.B, SRGB.A);
	}

	static EOrientation ParseOrientation(const FString& V)
	{
		if (V == TEXT("Vertical")) return Orient_Vertical;
		return Orient_Horizontal;
	}

	static ESlateBrushDrawType::Type ParseBrushDrawAs(const FString& V)
	{
		if (V == TEXT("Box"))         return ESlateBrushDrawType::Box;
		if (V == TEXT("Border"))      return ESlateBrushDrawType::Border;
		if (V == TEXT("RoundedBox"))  return ESlateBrushDrawType::RoundedBox;
		if (V == TEXT("NoDrawType"))  return ESlateBrushDrawType::NoDrawType;
		return ESlateBrushDrawType::Image;
	}
	static FString BrushDrawAsToString(ESlateBrushDrawType::Type V)
	{
		switch (V)
		{
		case ESlateBrushDrawType::Box:        return TEXT("Box");
		case ESlateBrushDrawType::Border:     return TEXT("Border");
		case ESlateBrushDrawType::RoundedBox: return TEXT("RoundedBox");
		case ESlateBrushDrawType::NoDrawType: return TEXT("NoDrawType");
		default:                              return TEXT("Image");
		}
	}

	static EProgressBarFillType::Type ParseBarFillType(const FString& V)
	{
		if (V == TEXT("RightToLeft"))            return EProgressBarFillType::RightToLeft;
		if (V == TEXT("FillFromCenter"))         return EProgressBarFillType::FillFromCenter;
		if (V == TEXT("FillFromCenterHorizontal")) return EProgressBarFillType::FillFromCenterHorizontal;
		if (V == TEXT("FillFromCenterVertical")) return EProgressBarFillType::FillFromCenterVertical;
		if (V == TEXT("TopToBottom"))            return EProgressBarFillType::TopToBottom;
		if (V == TEXT("BottomToTop"))            return EProgressBarFillType::BottomToTop;
		return EProgressBarFillType::LeftToRight;
	}

	static EButtonClickMethod::Type ParseClickMethod(const FString& V)
	{
		if (V == TEXT("MouseDown"))    return EButtonClickMethod::MouseDown;
		if (V == TEXT("MouseUp"))      return EButtonClickMethod::MouseUp;
		if (V == TEXT("PreciseClick")) return EButtonClickMethod::PreciseClick;
		return EButtonClickMethod::DownAndUp;
	}

	static EWidgetClipping ParseClipping(const FString& V)
	{
		if (V == TEXT("ClipToBounds"))                  return EWidgetClipping::ClipToBounds;
		if (V == TEXT("ClipToBoundsAlways"))            return EWidgetClipping::ClipToBoundsAlways;
		if (V == TEXT("OnDemand"))                      return EWidgetClipping::OnDemand;
		if (V == TEXT("ClipToBoundsWithoutIntersecting")) return EWidgetClipping::ClipToBoundsWithoutIntersecting;
		return EWidgetClipping::Inherit;
	}

	static void EnsureGuid(UWidgetBlueprint* WBP, UWidget* W)
	{
		if (!WBP || !W) return;
		if (!WBP->WidgetVariableNameToGuidMap.Contains(W->GetFName()))
		{
			WBP->WidgetVariableNameToGuidMap.Add(W->GetFName(), FGuid::NewGuid());
		}
	}

	static UTexture2D* LoadTextureAsset(const FString& Path)
	{
		if (Path.IsEmpty()) return nullptr;
		return LoadObject<UTexture2D>(nullptr, *Path);
	}

	static void MarkWBPDirtyAndModified(UWidgetBlueprint* WBP)
	{
		if (!WBP) return;
		FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
		WBP->MarkPackageDirty();
	}

	static FMargin ReadMargin(const TSharedPtr<FJsonObject>& Obj)
	{
		double L = 0, T = 0, R = 0, B = 0;
		Obj->TryGetNumberField(TEXT("left"),   L);
		Obj->TryGetNumberField(TEXT("top"),    T);
		Obj->TryGetNumberField(TEXT("right"),  R);
		Obj->TryGetNumberField(TEXT("bottom"), B);
		return FMargin((float)L, (float)T, (float)R, (float)B);
	}

	static TSharedPtr<FJsonObject> MarginToJson(const FMargin& M)
	{
		TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetNumberField(TEXT("left"),   M.Left);
		J->SetNumberField(TEXT("top"),    M.Top);
		J->SetNumberField(TEXT("right"),  M.Right);
		J->SetNumberField(TEXT("bottom"), M.Bottom);
		return J;
	}
}

// ============================================================================
// Handler: POST /editor/create_widget_blueprint
// ============================================================================

bool FNGGHttpServer::HandleCreateWidgetBlueprint(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString ParentClassName; Body->TryGetStringField(TEXT("parent_class"), ParentClassName);
	FString AssetPath;       Body->TryGetStringField(TEXT("asset_path"),   AssetPath);

	// Capture widgets array (may be absent -> empty). Guard the type so a
	// non-array "widgets" value yields a 400 instead of a check() crash.
	TArray<TSharedPtr<FJsonValue>> WidgetsJson;
	if (Body->HasField(TEXT("widgets")))
	{
		const TArray<TSharedPtr<FJsonValue>>* WidgetsPtr = nullptr;
		if (!Body->TryGetArrayField(TEXT("widgets"), WidgetsPtr) || !WidgetsPtr)
		{
			Callback(JsonError(400, TEXT("'widgets' must be an array")));
			return true;
		}
		WidgetsJson = *WidgetsPtr;
	}

	FString RootType;
	Body->TryGetStringField(TEXT("root_type"), RootType);
	if (RootType.IsEmpty()) RootType = TEXT("CanvasPanel");

	if (ParentClassName.IsEmpty() || AssetPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("parent_class and asset_path are required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, ParentClassName, AssetPath, WidgetsJson, RootType]()
	{
		// ---------- 1. Resolve parent UClass (strip U prefix for GetName()) ---
		UClass* ParentClass = nullptr;
		FString WStrippedU = ParentClassName;
		if (WStrippedU.StartsWith(TEXT("U"))) WStrippedU = WStrippedU.Mid(1);

		TArray<FString> CandidateNames = { WStrippedU, ParentClassName };
		for (const FString& Candidate : CandidateNames)
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->GetName() == Candidate)
				{
					ParentClass = *It;
					break;
				}
			}
			if (ParentClass) break;
		}

		if (!ParentClass)
		{
			// Fall back to UUserWidget so the file is still useful
			ParentClass = UUserWidget::StaticClass();
			UE_LOG(LogNGGBridge, Warning,
				TEXT("HandleCreateWidgetBlueprint: could not find class '%s', falling back to UUserWidget"),
				*ParentClassName);
		}

		// ---------- 2. Split asset_path ---------------------------------------
		int32 LastSlash;
		if (!AssetPath.FindLastChar(TEXT('/'), LastSlash))
		{
			Callback(JsonError(400, TEXT("asset_path must contain '/'")));
			return;
		}
		const FString PackagePath = AssetPath.Left(LastSlash);
		const FString AssetName   = AssetPath.Mid(LastSlash + 1);

		// ---------- 3. Check for pre-existing asset ---------------------------
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		FAssetData Existing = AR.GetAssetByObjectPath(FSoftObjectPath(AssetPath + TEXT(".") + AssetName));
		if (Existing.IsValid())
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField  (TEXT("success"),        true);
			Result->SetBoolField  (TEXT("already_existed"),true);
			Result->SetStringField(TEXT("asset_path"),     AssetPath);

			FString BodyStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
			FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
			Callback(JsonOk(BodyStr));
			return;
		}

		// ---------- 4. Create Widget Blueprint --------------------------------
		IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

		UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>(GetTransientPackage());
		Factory->ParentClass = ParentClass;

		UObject* NewAsset = AT.CreateAsset(AssetName, PackagePath, UWidgetBlueprint::StaticClass(), Factory);
		UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(NewAsset);

		int32 WidgetCount = 0;

		// ---------- 5. Populate widget tree if requested ----------------------
		if (IsValid(WBP) && IsValid(WBP->WidgetTree))
		{
			// Construct the requested root panel (or fall back to CanvasPanel).
			UClass* RootClass = NGGWidgetPriv::ResolveWidgetClass(RootType, FString());
			if (!RootClass || !RootClass->IsChildOf(UPanelWidget::StaticClass()))
			{
				RootClass = UCanvasPanel::StaticClass();
			}
			const FString RootName = RootType + TEXT("_Root");
			UPanelWidget* Root = Cast<UPanelWidget>(
				WBP->WidgetTree->ConstructWidget<UWidget>(RootClass, FName(*RootName)));
			WBP->WidgetTree->RootWidget = Root;
			NGGWidgetPriv::EnsureGuid(WBP, Root);

			TMap<FString, UWidget*> NameToWidget;

			for (const TSharedPtr<FJsonValue>& Entry : WidgetsJson)
			{
				const TSharedPtr<FJsonObject>* EntryObj;
				if (!Entry->TryGetObject(EntryObj)) continue;

				FString TypeName, WidgetName, ParentName, UserWidgetClassPath;
				(*EntryObj)->TryGetStringField(TEXT("type"),              TypeName);
				(*EntryObj)->TryGetStringField(TEXT("name"),              WidgetName);
				(*EntryObj)->TryGetStringField(TEXT("parent"),            ParentName);
				(*EntryObj)->TryGetStringField(TEXT("user_widget_class"), UserWidgetClassPath);
				if (WidgetName.IsEmpty()) continue;

				UClass* WidgetClass = NGGWidgetPriv::ResolveWidgetClass(TypeName, UserWidgetClassPath);
				if (!WidgetClass) continue;

				UWidget* W = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*WidgetName));
				if (!W) continue;
				NGGWidgetPriv::EnsureGuid(WBP, W);

				NameToWidget.Add(WidgetName, W);

				UWidget** ParentPtr = ParentName.IsEmpty() ? nullptr : NameToWidget.Find(ParentName);
				UPanelWidget* ParentPanel = ParentPtr ? Cast<UPanelWidget>(*ParentPtr) : nullptr;

				if (ParentPanel)
				{
					ParentPanel->AddChild(W);
				}
				else if (UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(Root))
				{
					UCanvasPanelSlot* Slot = RootCanvas->AddChildToCanvas(W);
					if (Slot) Slot->SetAutoSize(false);
				}
				else if (Root)
				{
					Root->AddChild(W);
				}
				++WidgetCount;
			}

			FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
		}

		if (IsValid(WBP))
		{
			WBP->MarkPackageDirty();
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),        IsValid(WBP));
		Result->SetBoolField  (TEXT("already_existed"),false);
		Result->SetStringField(TEXT("asset_path"),     AssetPath);
		Result->SetNumberField(TEXT("widget_count"),   static_cast<double>(WidgetCount));

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(IsValid(WBP) ? JsonCreated(BodyStr) : JsonError(500, TEXT("WidgetBlueprint creation failed")));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/style_widgets
//
// Body:
// {
//   "widget_blueprint": "/Game/UI/WBP_MainMenu",
//   "styles": [
//     {
//       "widget_name":          "StartButton",      // required — WidgetTree FName
//       "font_size":            36,                 // UTextBlock / UEditableTextBox
//       "font_bold":            true,               // UTextBlock
//       "text":                 "AVVIA",            // UTextBlock / UEditableTextBox
//       "auto_wrap":            false,              // UTextBlock
//       "color":                "#58A6FF",          // UTextBlock foreground; UProgressBar fill
//       "fill_color":           "#58A6FF",          // UProgressBar (alias for color)
//       "background_color":     "#1C1C1C",          // UButton Normal/Hovered/Pressed tint
//       "width_override":       320,               // UCanvasPanelSlot size X
//       "height_override":      72,                // UCanvasPanelSlot size Y
//       "anchor":               "bottom-right",    // UCanvasPanelSlot FAnchors preset
//       "position_x":           -220,              // UCanvasPanelSlot position offset X
//       "position_y":           -90,               // UCanvasPanelSlot position offset Y
//       "alignment_x":          1.0,               // UCanvasPanelSlot pivot X (0-1)
//       "alignment_y":          1.0,               // UCanvasPanelSlot pivot Y (0-1)
//       "visibility":           "Visible",          // any UWidget
//       "horizontal_alignment": "Center",           // UTextBlock justification
//       "padding": { "left":16,"top":8,"right":16,"bottom":8 }, // UCanvasPanelSlot
//       "percent":              0.75               // UProgressBar
//     }
//   ]
// }
//
// Unknown widget names and unrecognised property keys are skipped silently.
// Returns per-widget applied/skipped lists plus aggregate totals.
// ============================================================================

// ============================================================================
// Handler: POST /editor/get_widget_tree
// Returns the full widget hierarchy of a Widget Blueprint as nested JSON.
// ============================================================================

bool FNGGHttpServer::HandleGetWidgetTree(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString WBPPath;
	Body->TryGetStringField(TEXT("widget_blueprint"), WBPPath);
	if (WBPPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("widget_blueprint is required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [WBPPath, Callback]()
	{
		TFunction<TSharedPtr<FJsonObject>(UWidget*)> BuildNode;
		BuildNode = [&BuildNode](UWidget* W) -> TSharedPtr<FJsonObject>
		{
			TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
			Node->SetStringField(TEXT("name"),  W->GetFName().ToString());
			Node->SetStringField(TEXT("class"), W->GetClass()->GetName());

			if (UUserWidget* UW = Cast<UUserWidget>(W))
			{
				if (W->GetClass() != UUserWidget::StaticClass())
				{
					Node->SetBoolField(TEXT("is_user_widget_instance"), true);
					Node->SetStringField(TEXT("user_widget_class"), W->GetClass()->GetPathName());
				}
			}

			Node->SetBoolField(TEXT("is_enabled"),  W->GetIsEnabled());
			Node->SetBoolField(TEXT("is_variable"), W->bIsVariable);
			const FString Tip = W->GetToolTipText().ToString();
			if (!Tip.IsEmpty()) Node->SetStringField(TEXT("tooltip"), Tip);

			if (W->Slot)
			{
				Node->SetStringField(TEXT("slot_class"), W->Slot->GetClass()->GetName());
				TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();

				if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(W->Slot))
				{
					FVector2D Pos = CSlot->GetPosition();
					FVector2D Sz  = CSlot->GetSize();
					FAnchors  An  = CSlot->GetAnchors();
					FVector2D Al  = CSlot->GetAlignment();
					int32 Z = 0;
					if (FIntProperty* P = FindFProperty<FIntProperty>(CSlot->GetClass(), TEXT("ZOrder")))
					{
						Z = P->GetPropertyValue_InContainer(CSlot);
					}
					SlotObj->SetNumberField(TEXT("pos_x"),         Pos.X);
					SlotObj->SetNumberField(TEXT("pos_y"),         Pos.Y);
					SlotObj->SetNumberField(TEXT("width"),         Sz.X);
					SlotObj->SetNumberField(TEXT("height"),        Sz.Y);
					SlotObj->SetNumberField(TEXT("anchor_min_x"),  An.Minimum.X);
					SlotObj->SetNumberField(TEXT("anchor_min_y"),  An.Minimum.Y);
					SlotObj->SetNumberField(TEXT("anchor_max_x"),  An.Maximum.X);
					SlotObj->SetNumberField(TEXT("anchor_max_y"),  An.Maximum.Y);
					SlotObj->SetNumberField(TEXT("alignment_x"),   Al.X);
					SlotObj->SetNumberField(TEXT("alignment_y"),   Al.Y);
					SlotObj->SetNumberField(TEXT("z_order"),       Z);
					SlotObj->SetBoolField  (TEXT("auto_size"),     CSlot->GetAutoSize());
					Node->SetObjectField(TEXT("canvas_slot"), SlotObj);
				}
				else if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(W->Slot))
				{
					SlotObj->SetObjectField(TEXT("padding"), NGGWidgetPriv::MarginToJson(VS->GetPadding()));
					SlotObj->SetStringField(TEXT("h_align"), NGGWidgetPriv::HAlignToString(VS->GetHorizontalAlignment()));
					SlotObj->SetStringField(TEXT("v_align"), NGGWidgetPriv::VAlignToString(VS->GetVerticalAlignment()));
					const FSlateChildSize Sz = VS->GetSize();
					SlotObj->SetStringField(TEXT("size_rule"), Sz.SizeRule == ESlateSizeRule::Fill ? TEXT("Fill") : TEXT("Auto"));
					SlotObj->SetNumberField(TEXT("size_value"), Sz.Value);
				}
				else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(W->Slot))
				{
					SlotObj->SetObjectField(TEXT("padding"), NGGWidgetPriv::MarginToJson(HS->GetPadding()));
					SlotObj->SetStringField(TEXT("h_align"), NGGWidgetPriv::HAlignToString(HS->GetHorizontalAlignment()));
					SlotObj->SetStringField(TEXT("v_align"), NGGWidgetPriv::VAlignToString(HS->GetVerticalAlignment()));
					const FSlateChildSize Sz = HS->GetSize();
					SlotObj->SetStringField(TEXT("size_rule"), Sz.SizeRule == ESlateSizeRule::Fill ? TEXT("Fill") : TEXT("Auto"));
					SlotObj->SetNumberField(TEXT("size_value"), Sz.Value);
				}
				else if (UOverlaySlot* OS = Cast<UOverlaySlot>(W->Slot))
				{
					SlotObj->SetObjectField(TEXT("padding"), NGGWidgetPriv::MarginToJson(OS->GetPadding()));
					SlotObj->SetStringField(TEXT("h_align"), NGGWidgetPriv::HAlignToString(OS->GetHorizontalAlignment()));
					SlotObj->SetStringField(TEXT("v_align"), NGGWidgetPriv::VAlignToString(OS->GetVerticalAlignment()));
				}
				else if (UGridSlot* GS = Cast<UGridSlot>(W->Slot))
				{
					SlotObj->SetNumberField(TEXT("row"),         GS->GetRow());
					SlotObj->SetNumberField(TEXT("column"),      GS->GetColumn());
					SlotObj->SetNumberField(TEXT("row_span"),    GS->GetRowSpan());
					SlotObj->SetNumberField(TEXT("column_span"), GS->GetColumnSpan());
					SlotObj->SetObjectField(TEXT("padding"),     NGGWidgetPriv::MarginToJson(GS->GetPadding()));
					SlotObj->SetStringField(TEXT("h_align"),     NGGWidgetPriv::HAlignToString(GS->GetHorizontalAlignment()));
					SlotObj->SetStringField(TEXT("v_align"),     NGGWidgetPriv::VAlignToString(GS->GetVerticalAlignment()));
					SlotObj->SetNumberField(TEXT("layer"),       GS->GetLayer());
				}
				else if (UUniformGridSlot* UGS = Cast<UUniformGridSlot>(W->Slot))
				{
					SlotObj->SetNumberField(TEXT("row"),         UGS->GetRow());
					SlotObj->SetNumberField(TEXT("column"),      UGS->GetColumn());
					SlotObj->SetStringField(TEXT("h_align"),     NGGWidgetPriv::HAlignToString(UGS->GetHorizontalAlignment()));
					SlotObj->SetStringField(TEXT("v_align"),     NGGWidgetPriv::VAlignToString(UGS->GetVerticalAlignment()));
				}
				else if (UScrollBoxSlot* SS = Cast<UScrollBoxSlot>(W->Slot))
				{
					SlotObj->SetObjectField(TEXT("padding"), NGGWidgetPriv::MarginToJson(SS->GetPadding()));
					SlotObj->SetStringField(TEXT("h_align"), NGGWidgetPriv::HAlignToString(SS->GetHorizontalAlignment()));
				}
				else if (UBorderSlot* BS = Cast<UBorderSlot>(W->Slot))
				{
					SlotObj->SetObjectField(TEXT("padding"), NGGWidgetPriv::MarginToJson(BS->GetPadding()));
					SlotObj->SetStringField(TEXT("h_align"), NGGWidgetPriv::HAlignToString(BS->GetHorizontalAlignment()));
					SlotObj->SetStringField(TEXT("v_align"), NGGWidgetPriv::VAlignToString(BS->GetVerticalAlignment()));
				}
				else if (UWrapBoxSlot* WS = Cast<UWrapBoxSlot>(W->Slot))
				{
					SlotObj->SetObjectField(TEXT("padding"), NGGWidgetPriv::MarginToJson(WS->GetPadding()));
					SlotObj->SetStringField(TEXT("h_align"), NGGWidgetPriv::HAlignToString(WS->GetHorizontalAlignment()));
					SlotObj->SetStringField(TEXT("v_align"), NGGWidgetPriv::VAlignToString(WS->GetVerticalAlignment()));
					if (FFloatProperty* P = FindFProperty<FFloatProperty>(WS->GetClass(), TEXT("FillSpanWhenLessThan")))
					{
						SlotObj->SetNumberField(TEXT("fill_span"), P->GetPropertyValue_InContainer(WS));
					}
					if (FBoolProperty* P2 = FindFProperty<FBoolProperty>(WS->GetClass(), TEXT("bFillEmptySpace")))
					{
						SlotObj->SetBoolField(TEXT("fill_empty_space"), P2->GetPropertyValue_InContainer(WS));
					}
				}
				else if (UScaleBoxSlot* SBS = Cast<UScaleBoxSlot>(W->Slot))
				{
					SlotObj->SetStringField(TEXT("h_align"), NGGWidgetPriv::HAlignToString(SBS->GetHorizontalAlignment()));
					SlotObj->SetStringField(TEXT("v_align"), NGGWidgetPriv::VAlignToString(SBS->GetVerticalAlignment()));
				}

				if (SlotObj->Values.Num() > 0 && !Node->HasField(TEXT("canvas_slot")))
				{
					Node->SetObjectField(TEXT("slot"), SlotObj);
				}
			}

			if (UPanelWidget* Panel = Cast<UPanelWidget>(W))
			{
				TArray<TSharedPtr<FJsonValue>> ChildArr;
				for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
				{
					if (UWidget* Child = Panel->GetChildAt(i))
					{
						ChildArr.Add(MakeShared<FJsonValueObject>(BuildNode(Child)));
					}
				}
				Node->SetArrayField(TEXT("children"), ChildArr);
			}

			if (UTextBlock* TB = Cast<UTextBlock>(W))
			{
				Node->SetStringField(TEXT("text"),  TB->GetText().ToString());
				const FSlateFontInfo Font = TB->GetFont();
				Node->SetNumberField(TEXT("font_size"), Font.Size);
				Node->SetStringField(TEXT("font_typeface"), Font.TypefaceFontName.ToString());
				Node->SetStringField(TEXT("color"), NGGWidgetPriv::ColorToHex(TB->GetColorAndOpacity().GetSpecifiedColor()));
			}
			else if (UButton* Btn = Cast<UButton>(W))
			{
				const FButtonStyle& Style = Btn->GetStyle();
				Node->SetStringField(TEXT("background_color"),
					NGGWidgetPriv::ColorToHex(Style.Normal.TintColor.GetSpecifiedColor()));
			}
			else if (UImage* Img = Cast<UImage>(W))
			{
				const FSlateBrush& B = Img->GetBrush();
				if (B.GetResourceObject())
				{
					Node->SetStringField(TEXT("brush_texture"), B.GetResourceObject()->GetPathName());
				}
				Node->SetStringField(TEXT("brush_tint"),  NGGWidgetPriv::ColorToHex(Img->GetColorAndOpacity()));
				Node->SetNumberField(TEXT("brush_size_x"),B.ImageSize.X);
				Node->SetNumberField(TEXT("brush_size_y"),B.ImageSize.Y);
				Node->SetStringField(TEXT("brush_draw_as"), NGGWidgetPriv::BrushDrawAsToString((ESlateBrushDrawType::Type)B.DrawAs));
			}
			else if (UBorder* Brd = Cast<UBorder>(W))
			{
				if (FStructProperty* P = FindFProperty<FStructProperty>(Brd->GetClass(), TEXT("Background")))
				{
					if (FSlateBrush* B = P->ContainerPtrToValuePtr<FSlateBrush>(Brd))
					{
						if (B->GetResourceObject())
						{
							Node->SetStringField(TEXT("brush_texture"), B->GetResourceObject()->GetPathName());
						}
					}
				}
				Node->SetStringField(TEXT("brush_tint"), NGGWidgetPriv::ColorToHex(Brd->GetBrushColor()));
			}
			else if (UProgressBar* PB = Cast<UProgressBar>(W))
			{
				Node->SetNumberField(TEXT("percent"), PB->GetPercent());
				Node->SetStringField(TEXT("fill_color"),
					NGGWidgetPriv::ColorToHex(PB->GetFillColorAndOpacity()));
			}
			else if (USlider* Sl = Cast<USlider>(W))
			{
				Node->SetNumberField(TEXT("value"),     Sl->GetValue());
				Node->SetNumberField(TEXT("min_value"), Sl->GetMinValue());
				Node->SetNumberField(TEXT("max_value"), Sl->GetMaxValue());
			}
			else if (UCheckBox* CB = Cast<UCheckBox>(W))
			{
				Node->SetBoolField(TEXT("is_checked"), CB->IsChecked());
			}

			return Node;
		};

		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
		if (!IsValid(WBP) || !IsValid(WBP->WidgetTree))
		{
			Callback(JsonError(404, FString::Printf(TEXT("Widget Blueprint not found: %s"), *WBPPath)));
			return;
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("widget_blueprint"), WBPPath);
		if (UWidget* Root = WBP->WidgetTree->RootWidget)
		{
			Result->SetObjectField(TEXT("root"), BuildNode(Root));
		}
		else
		{
			Result->SetBoolField(TEXT("root"), false);
		}

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(FHttpServerResponse::Create(BodyStr, TEXT("application/json")));
	});

	return true;
}

bool FNGGHttpServer::HandleStyleWidgets(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString WBPPath; Body->TryGetStringField(TEXT("widget_blueprint"), WBPPath);
	if (WBPPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("widget_blueprint is required")));
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> StylesJson;
	if (Body->HasField(TEXT("styles")))
	{
		const TArray<TSharedPtr<FJsonValue>>* StylesPtr = nullptr;
		if (!Body->TryGetArrayField(TEXT("styles"), StylesPtr) || !StylesPtr)
		{
			Callback(JsonError(400, TEXT("'styles' must be an array")));
			return true;
		}
		StylesJson = *StylesPtr;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, WBPPath, StylesJson]()
	{
		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
		if (!IsValid(WBP))
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("Widget Blueprint not found at '%s'"), *WBPPath)));
			return;
		}
		if (!IsValid(WBP->WidgetTree))
		{
			Callback(JsonError(500, TEXT("Widget Blueprint has no WidgetTree")));
			return;
		}

		auto ParseJustify = [](const FString& H) -> ETextJustify::Type
		{
			if (H == TEXT("Right"))  return ETextJustify::Right;
			if (H == TEXT("Center")) return ETextJustify::Center;
			return ETextJustify::Left;
		};

		static const TArray<FString> AllKnownKeys = {
			// universal
			TEXT("visibility"),  TEXT("tooltip_text"), TEXT("is_enabled"), TEXT("is_variable"),
			TEXT("render_translation_x"), TEXT("render_translation_y"),
			TEXT("render_angle"), TEXT("render_scale_x"), TEXT("render_scale_y"),
			TEXT("render_shear_x"), TEXT("render_shear_y"),
			TEXT("render_pivot_x"), TEXT("render_pivot_y"),
			TEXT("clip_to_bounds"), TEXT("clipping"),
			// canvas-slot
			TEXT("width_override"), TEXT("height_override"), TEXT("padding"),
			TEXT("anchor"), TEXT("position_x"), TEXT("position_y"),
			TEXT("alignment_x"), TEXT("alignment_y"), TEXT("z_order"), TEXT("auto_size"),
			// generic slot
			TEXT("slot_padding"), TEXT("slot_h_align"), TEXT("slot_v_align"),
			TEXT("slot_size_rule"), TEXT("slot_size_value"),
			TEXT("slot_row"), TEXT("slot_column"), TEXT("slot_row_span"), TEXT("slot_column_span"),
			TEXT("slot_z_order"),
			// text
			TEXT("font_size"), TEXT("font_bold"), TEXT("font_typeface"),
			TEXT("text"), TEXT("color"), TEXT("auto_wrap"), TEXT("horizontal_alignment"),
			TEXT("shadow_offset_x"), TEXT("shadow_offset_y"), TEXT("shadow_color"),
			TEXT("outline_size"), TEXT("outline_color"), TEXT("min_desired_width"),
			// button
			TEXT("background_color"), TEXT("hovered_color"), TEXT("pressed_color"),
			TEXT("disabled_color"), TEXT("content_padding"), TEXT("click_method"), TEXT("is_focusable"),
			// image / border
			TEXT("brush_texture"), TEXT("brush_tint"), TEXT("brush_size_x"), TEXT("brush_size_y"),
			TEXT("brush_draw_as"), TEXT("brush_margin"), TEXT("brush_tiling"),
			TEXT("vertical_alignment"), TEXT("content_color"),
			// progress bar
			TEXT("fill_color"), TEXT("percent"), TEXT("bar_fill_type"), TEXT("marquee"),
			// slider
			TEXT("value"), TEXT("min_value"), TEXT("max_value"), TEXT("step_size"),
			TEXT("orientation"), TEXT("slider_bar_color"), TEXT("slider_handle_color"),
			// scroll box
			TEXT("scroll_bar_visibility"), TEXT("allow_overscroll"),
			TEXT("scroll_bar_thickness"), TEXT("always_show_scrollbar"),
			// size box
			TEXT("min_width"), TEXT("min_height"), TEXT("max_width"), TEXT("max_height"),
			TEXT("min_aspect_ratio"), TEXT("max_aspect_ratio"),
			TEXT("clear_width_override"), TEXT("clear_height_override"),
			// spacer / switcher / rtb / throbber / blur / safezone / scalebox / combobox / checkbox
			TEXT("size_x"), TEXT("size_y"),
			TEXT("active_widget_index"),
			TEXT("text_style_set"),
			TEXT("number_of_pieces"), TEXT("period"), TEXT("radius"),
			TEXT("animate_horizontally"), TEXT("animate_vertically"), TEXT("animate_opacity"),
			TEXT("blur_strength"), TEXT("blur_radius"),
			TEXT("pad_left"), TEXT("pad_top"), TEXT("pad_right"), TEXT("pad_bottom"),
			TEXT("pad_sides"), TEXT("is_titlesafe"),
			TEXT("stretch"), TEXT("stretch_direction"), TEXT("user_specified_scale"),
			TEXT("add_option"), TEXT("clear_options"), TEXT("selected_option"),
			TEXT("is_checked"), TEXT("check_type")
		};

		// ---------- 4. Process each style entry ------------------------------
		TArray<TSharedPtr<FJsonValue>> ResultsJson;
		int32 TotalApplied = 0;
		int32 TotalSkipped = 0;

		for (const TSharedPtr<FJsonValue>& Entry : StylesJson)
		{
			const TSharedPtr<FJsonObject>* EntryObjPtr;
			if (!Entry->TryGetObject(EntryObjPtr)) continue;
			const TSharedPtr<FJsonObject>& S = *EntryObjPtr;

			FString WidgetName;
			S->TryGetStringField(TEXT("widget_name"), WidgetName);
			if (WidgetName.IsEmpty()) continue;

			// Build per-widget result object
			TSharedPtr<FJsonObject> WidgetResult = MakeShared<FJsonObject>();
			WidgetResult->SetStringField(TEXT("widget_name"), WidgetName);

			UWidget* W = WBP->WidgetTree->FindWidget(FName(*WidgetName));
			if (!IsValid(W))
			{
				WidgetResult->SetStringField(TEXT("status"), TEXT("not_found"));
				ResultsJson.Add(MakeShared<FJsonValueObject>(WidgetResult));
				++TotalSkipped;
				continue;
			}

			TArray<FString> Applied;
			TArray<FString> Skipped;

			auto AddUnsupportedSlot = [&](const FString& Key)
			{
				const FString ClassName = W->Slot ? W->Slot->GetClass()->GetName() : TEXT("None");
				Skipped.Add(FString::Printf(TEXT("%s (unsupported slot type: %s)"), *Key, *ClassName));
			};
			auto AddUnsupportedWidget = [&](const FString& Key)
			{
				Skipped.Add(FString::Printf(TEXT("%s (unsupported widget type: %s)"),
					*Key, *W->GetClass()->GetName()));
			};

			// ---------- Universal UWidget properties --------------------------
			FString VisStr;
			if (S->TryGetStringField(TEXT("visibility"), VisStr))
			{
				W->SetVisibility(NGGWidgetPriv::ParseVisibility(VisStr));
				Applied.Add(TEXT("visibility"));
			}
			FString TipStr;
			if (S->TryGetStringField(TEXT("tooltip_text"), TipStr))
			{
				W->SetToolTipText(FText::FromString(TipStr));
				Applied.Add(TEXT("tooltip_text"));
			}
			bool bEnabled = false;
			if (S->TryGetBoolField(TEXT("is_enabled"), bEnabled))
			{
				W->SetIsEnabled(bEnabled);
				Applied.Add(TEXT("is_enabled"));
			}
			bool bIsVar = false;
			if (S->TryGetBoolField(TEXT("is_variable"), bIsVar))
			{
				W->bIsVariable = bIsVar;
				Applied.Add(TEXT("is_variable"));
			}
			{
				double TX=0, TY=0, Ang=0, ScX=1, ScY=1, ShX=0, ShY=0;
				const bool bHasTX = S->TryGetNumberField(TEXT("render_translation_x"), TX);
				const bool bHasTY = S->TryGetNumberField(TEXT("render_translation_y"), TY);
				const bool bHasA  = S->TryGetNumberField(TEXT("render_angle"),         Ang);
				const bool bHasScX= S->TryGetNumberField(TEXT("render_scale_x"),       ScX);
				const bool bHasScY= S->TryGetNumberField(TEXT("render_scale_y"),       ScY);
				const bool bHasShX= S->TryGetNumberField(TEXT("render_shear_x"),       ShX);
				const bool bHasShY= S->TryGetNumberField(TEXT("render_shear_y"),       ShY);
				if (bHasTX||bHasTY||bHasA||bHasScX||bHasScY||bHasShX||bHasShY)
				{
					FWidgetTransform T = W->GetRenderTransform();
					if (bHasTX) T.Translation.X = (float)TX;
					if (bHasTY) T.Translation.Y = (float)TY;
					if (bHasA)  T.Angle         = (float)Ang;
					if (bHasScX) T.Scale.X      = (float)ScX;
					if (bHasScY) T.Scale.Y      = (float)ScY;
					if (bHasShX) T.Shear.X      = (float)ShX;
					if (bHasShY) T.Shear.Y      = (float)ShY;
					W->SetRenderTransform(T);
					if (bHasTX) Applied.Add(TEXT("render_translation_x"));
					if (bHasTY) Applied.Add(TEXT("render_translation_y"));
					if (bHasA)  Applied.Add(TEXT("render_angle"));
					if (bHasScX) Applied.Add(TEXT("render_scale_x"));
					if (bHasScY) Applied.Add(TEXT("render_scale_y"));
					if (bHasShX) Applied.Add(TEXT("render_shear_x"));
					if (bHasShY) Applied.Add(TEXT("render_shear_y"));
				}
			}
			{
				double PX = 0.5, PY = 0.5;
				const bool bHasPX = S->TryGetNumberField(TEXT("render_pivot_x"), PX);
				const bool bHasPY = S->TryGetNumberField(TEXT("render_pivot_y"), PY);
				if (bHasPX || bHasPY)
				{
					FVector2D Cur = W->RenderTransformPivot;
					if (bHasPX) Cur.X = (float)PX;
					if (bHasPY) Cur.Y = (float)PY;
					W->SetRenderTransformPivot(Cur);
					if (bHasPX) Applied.Add(TEXT("render_pivot_x"));
					if (bHasPY) Applied.Add(TEXT("render_pivot_y"));
				}
			}
			FString ClipStr;
			if (S->TryGetStringField(TEXT("clipping"), ClipStr))
			{
				W->SetClipping(NGGWidgetPriv::ParseClipping(ClipStr));
				Applied.Add(TEXT("clipping"));
			}
			bool bClip = false;
			if (S->TryGetBoolField(TEXT("clip_to_bounds"), bClip))
			{
				W->SetClipping(bClip ? EWidgetClipping::ClipToBounds : EWidgetClipping::Inherit);
				Applied.Add(TEXT("clip_to_bounds"));
			}

			// ---------- Slot dispatch ----------------------------------------
			if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(W->Slot))
			{
				double WidthOvr = 0, HeightOvr = 0;
				const bool bHasW = S->TryGetNumberField(TEXT("width_override"),  WidthOvr);
				const bool bHasH = S->TryGetNumberField(TEXT("height_override"), HeightOvr);
				if (bHasW || bHasH)
				{
					CSlot->SetAutoSize(false);
					FVector2D Sz = CSlot->GetSize();
					if (bHasW) Sz.X = (float)WidthOvr;
					if (bHasH) Sz.Y = (float)HeightOvr;
					CSlot->SetSize(Sz);
					if (bHasW) Applied.Add(TEXT("width_override"));
					if (bHasH) Applied.Add(TEXT("height_override"));
				}
				const TSharedPtr<FJsonObject>* PaddingObj;
				if (S->TryGetObjectField(TEXT("padding"), PaddingObj))
				{
					CSlot->SetOffsets(NGGWidgetPriv::ReadMargin(*PaddingObj));
					Applied.Add(TEXT("padding"));
				}
				FString AnchorStr;
				if (S->TryGetStringField(TEXT("anchor"), AnchorStr))
				{
					float AMinX=0, AMinY=0, AMaxX=0, AMaxY=0;
					if      (AnchorStr == TEXT("top-left"))      { AMinX=0;   AMinY=0;   AMaxX=0;   AMaxY=0; }
					else if (AnchorStr == TEXT("top-center"))    { AMinX=0.5f;AMinY=0;   AMaxX=0.5f;AMaxY=0; }
					else if (AnchorStr == TEXT("top-right"))     { AMinX=1;   AMinY=0;   AMaxX=1;   AMaxY=0; }
					else if (AnchorStr == TEXT("center-left"))   { AMinX=0;   AMinY=0.5f;AMaxX=0;   AMaxY=0.5f; }
					else if (AnchorStr == TEXT("center"))        { AMinX=0.5f;AMinY=0.5f;AMaxX=0.5f;AMaxY=0.5f; }
					else if (AnchorStr == TEXT("center-right"))  { AMinX=1;   AMinY=0.5f;AMaxX=1;   AMaxY=0.5f; }
					else if (AnchorStr == TEXT("bottom-left"))   { AMinX=0;   AMinY=1;   AMaxX=0;   AMaxY=1; }
					else if (AnchorStr == TEXT("bottom-center")) { AMinX=0.5f;AMinY=1;   AMaxX=0.5f;AMaxY=1; }
					else if (AnchorStr == TEXT("bottom-right"))  { AMinX=1;   AMinY=1;   AMaxX=1;   AMaxY=1; }
					else if (AnchorStr == TEXT("fill"))          { AMinX=0;   AMinY=0;   AMaxX=1;   AMaxY=1; }
					CSlot->SetAnchors(FAnchors(AMinX, AMinY, AMaxX, AMaxY));
					Applied.Add(TEXT("anchor"));
				}
				double PosX = 0, PosY = 0;
				const bool bHasPosX = S->TryGetNumberField(TEXT("position_x"), PosX);
				const bool bHasPosY = S->TryGetNumberField(TEXT("position_y"), PosY);
				if (bHasPosX || bHasPosY)
				{
					FVector2D Cur = CSlot->GetPosition();
					if (bHasPosX) Cur.X = (float)PosX;
					if (bHasPosY) Cur.Y = (float)PosY;
					CSlot->SetPosition(Cur);
					if (bHasPosX) Applied.Add(TEXT("position_x"));
					if (bHasPosY) Applied.Add(TEXT("position_y"));
				}
				double AlignX = 0, AlignY = 0;
				const bool bHasAX = S->TryGetNumberField(TEXT("alignment_x"), AlignX);
				const bool bHasAY = S->TryGetNumberField(TEXT("alignment_y"), AlignY);
				if (bHasAX || bHasAY)
				{
					FVector2D Cur = CSlot->GetAlignment();
					if (bHasAX) Cur.X = (float)AlignX;
					if (bHasAY) Cur.Y = (float)AlignY;
					CSlot->SetAlignment(Cur);
					if (bHasAX) Applied.Add(TEXT("alignment_x"));
					if (bHasAY) Applied.Add(TEXT("alignment_y"));
				}
				int32 Z = 0;
				if (S->TryGetNumberField(TEXT("z_order"), Z))
				{
					CSlot->SetZOrder(Z);
					Applied.Add(TEXT("z_order"));
				}
				bool bAutoSize = false;
				if (S->TryGetBoolField(TEXT("auto_size"), bAutoSize))
				{
					CSlot->SetAutoSize(bAutoSize);
					Applied.Add(TEXT("auto_size"));
				}
			}

			// Generic slot keys
			auto ReadSlotPadding = [&](auto* Slot) -> bool
			{
				const TSharedPtr<FJsonObject>* P;
				if (S->TryGetObjectField(TEXT("slot_padding"), P))
				{
					Slot->SetPadding(NGGWidgetPriv::ReadMargin(*P));
					Applied.Add(TEXT("slot_padding"));
					return true;
				}
				return false;
			};
			auto ReadSlotHAlign = [&](auto* Slot) -> bool
			{
				FString V;
				if (S->TryGetStringField(TEXT("slot_h_align"), V))
				{
					Slot->SetHorizontalAlignment(NGGWidgetPriv::ParseHAlign(V));
					Applied.Add(TEXT("slot_h_align"));
					return true;
				}
				return false;
			};
			auto ReadSlotVAlign = [&](auto* Slot) -> bool
			{
				FString V;
				if (S->TryGetStringField(TEXT("slot_v_align"), V))
				{
					Slot->SetVerticalAlignment(NGGWidgetPriv::ParseVAlign(V));
					Applied.Add(TEXT("slot_v_align"));
					return true;
				}
				return false;
			};
			auto ReadSlotSize = [&](auto* Slot) -> bool
			{
				FString Rule;
				if (S->TryGetStringField(TEXT("slot_size_rule"), Rule))
				{
					FSlateChildSize Sz;
					if (Rule == TEXT("Fill"))
					{
						Sz.SizeRule = ESlateSizeRule::Fill;
						double Val = 1.0;
						S->TryGetNumberField(TEXT("slot_size_value"), Val);
						Sz.Value = (float)Val;
					}
					else
					{
						Sz.SizeRule = ESlateSizeRule::Automatic;
					}
					Slot->SetSize(Sz);
					Applied.Add(TEXT("slot_size_rule"));
					if (S->HasField(TEXT("slot_size_value"))) Applied.Add(TEXT("slot_size_value"));
					return true;
				}
				return false;
			};

			if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(W->Slot))
			{
				ReadSlotPadding(VS); ReadSlotHAlign(VS); ReadSlotVAlign(VS); ReadSlotSize(VS);
			}
			else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(W->Slot))
			{
				ReadSlotPadding(HS); ReadSlotHAlign(HS); ReadSlotVAlign(HS); ReadSlotSize(HS);
			}
			else if (UOverlaySlot* OS = Cast<UOverlaySlot>(W->Slot))
			{
				ReadSlotPadding(OS); ReadSlotHAlign(OS); ReadSlotVAlign(OS);
			}
			else if (UGridSlot* GS = Cast<UGridSlot>(W->Slot))
			{
				ReadSlotPadding(GS); ReadSlotHAlign(GS); ReadSlotVAlign(GS);
				int32 Row=0, Col=0, RS=0, CS=0, Layer=0;
				if (S->TryGetNumberField(TEXT("slot_row"),         Row)) { GS->SetRow(Row);            Applied.Add(TEXT("slot_row")); }
				if (S->TryGetNumberField(TEXT("slot_column"),      Col)) { GS->SetColumn(Col);         Applied.Add(TEXT("slot_column")); }
				if (S->TryGetNumberField(TEXT("slot_row_span"),    RS))  { GS->SetRowSpan(RS);         Applied.Add(TEXT("slot_row_span")); }
				if (S->TryGetNumberField(TEXT("slot_column_span"), CS))  { GS->SetColumnSpan(CS);      Applied.Add(TEXT("slot_column_span")); }
				if (S->TryGetNumberField(TEXT("slot_z_order"),     Layer)){ GS->SetLayer(Layer);       Applied.Add(TEXT("slot_z_order")); }
			}
			else if (UUniformGridSlot* UGS = Cast<UUniformGridSlot>(W->Slot))
			{
				ReadSlotHAlign(UGS); ReadSlotVAlign(UGS);
				int32 Row=0, Col=0;
				if (S->TryGetNumberField(TEXT("slot_row"),    Row)) { UGS->SetRow(Row);    Applied.Add(TEXT("slot_row")); }
				if (S->TryGetNumberField(TEXT("slot_column"), Col)) { UGS->SetColumn(Col); Applied.Add(TEXT("slot_column")); }
			}
			else if (UWrapBoxSlot* WS = Cast<UWrapBoxSlot>(W->Slot))
			{
				ReadSlotPadding(WS); ReadSlotHAlign(WS); ReadSlotVAlign(WS);
			}
			else if (UScrollBoxSlot* SBSlot = Cast<UScrollBoxSlot>(W->Slot))
			{
				ReadSlotPadding(SBSlot); ReadSlotHAlign(SBSlot);
			}
			else if (UBorderSlot* BdSlot = Cast<UBorderSlot>(W->Slot))
			{
				ReadSlotPadding(BdSlot); ReadSlotHAlign(BdSlot); ReadSlotVAlign(BdSlot);
			}
			else if (UScaleBoxSlot* SLBSlot = Cast<UScaleBoxSlot>(W->Slot))
			{
				ReadSlotHAlign(SLBSlot); ReadSlotVAlign(SLBSlot);
			}
			else if (USafeZoneSlot* SZSlot = Cast<USafeZoneSlot>(W->Slot))
			{
				const TSharedPtr<FJsonObject>* P;
				if (S->TryGetObjectField(TEXT("slot_padding"), P))
				{
					SZSlot->SetPadding(NGGWidgetPriv::ReadMargin(*P));
					Applied.Add(TEXT("slot_padding"));
				}
				FString HA;
				if (S->TryGetStringField(TEXT("slot_h_align"), HA))
				{
					SZSlot->SetHorizontalAlignment(NGGWidgetPriv::ParseHAlign(HA));
					Applied.Add(TEXT("slot_h_align"));
				}
				FString VA;
				if (S->TryGetStringField(TEXT("slot_v_align"), VA))
				{
					SZSlot->SetVerticalAlignment(NGGWidgetPriv::ParseVAlign(VA));
					Applied.Add(TEXT("slot_v_align"));
				}
			}
			else
			{
				static const TArray<FString> SlotKeys = {
					TEXT("slot_padding"), TEXT("slot_h_align"), TEXT("slot_v_align"),
					TEXT("slot_size_rule"), TEXT("slot_size_value"),
					TEXT("slot_row"), TEXT("slot_column"), TEXT("slot_row_span"),
					TEXT("slot_column_span"), TEXT("slot_z_order")
				};
				for (const FString& K : SlotKeys)
				{
					if (S->HasField(K)) AddUnsupportedSlot(K);
				}
			}

			// ---------- Widget-class-specific properties ---------------------
			if (UTextBlock* TB = Cast<UTextBlock>(W))
			{
				FString TextStr;
				if (S->TryGetStringField(TEXT("text"), TextStr))
				{
					TB->SetText(FText::FromString(TextStr));
					Applied.Add(TEXT("text"));
				}

				FSlateFontInfo FontInfo = TB->GetFont();
				bool bFontDirty = false;
				double FontSizeVal = 0;
				if (S->TryGetNumberField(TEXT("font_size"), FontSizeVal))
				{
					FontInfo.Size = (int32)FontSizeVal;
					bFontDirty = true;
					Applied.Add(TEXT("font_size"));
				}
				bool bFontBold = false;
				if (S->TryGetBoolField(TEXT("font_bold"), bFontBold))
				{
					FontInfo.TypefaceFontName = bFontBold ? FName(TEXT("Bold")) : FName(TEXT("Regular"));
					bFontDirty = true;
					Applied.Add(TEXT("font_bold"));
				}
				FString Typeface;
				if (S->TryGetStringField(TEXT("font_typeface"), Typeface))
				{
					FontInfo.TypefaceFontName = FName(*Typeface);
					bFontDirty = true;
					Applied.Add(TEXT("font_typeface"));
				}
				double OutlineSize = 0;
				if (S->TryGetNumberField(TEXT("outline_size"), OutlineSize))
				{
					FontInfo.OutlineSettings.OutlineSize = (int32)OutlineSize;
					bFontDirty = true;
					Applied.Add(TEXT("outline_size"));
				}
				FString OutlineHex;
				if (S->TryGetStringField(TEXT("outline_color"), OutlineHex))
				{
					FontInfo.OutlineSettings.OutlineColor = NGGWidgetPriv::ParseHexColor(OutlineHex);
					bFontDirty = true;
					Applied.Add(TEXT("outline_color"));
				}
				if (bFontDirty) TB->SetFont(FontInfo);

				FString ColorHex;
				if (S->TryGetStringField(TEXT("color"), ColorHex))
				{
					TB->SetColorAndOpacity(FSlateColor(NGGWidgetPriv::ParseHexColor(ColorHex)));
					Applied.Add(TEXT("color"));
				}
				bool bAutoWrap = false;
				if (S->TryGetBoolField(TEXT("auto_wrap"), bAutoWrap))
				{
					TB->SetAutoWrapText(bAutoWrap);
					Applied.Add(TEXT("auto_wrap"));
				}
				FString HA;
				if (S->TryGetStringField(TEXT("horizontal_alignment"), HA))
				{
					TB->SetJustification(ParseJustify(HA));
					Applied.Add(TEXT("horizontal_alignment"));
				}

				double SOX=0, SOY=0;
				const bool bSOX = S->TryGetNumberField(TEXT("shadow_offset_x"), SOX);
				const bool bSOY = S->TryGetNumberField(TEXT("shadow_offset_y"), SOY);
				if (bSOX || bSOY)
				{
					FVector2D Cur(0, 0);
					if (FStructProperty* P = FindFProperty<FStructProperty>(TB->GetClass(), TEXT("ShadowOffset")))
					{
						if (FVector2D* V = P->ContainerPtrToValuePtr<FVector2D>(TB))
						{
							Cur = *V;
						}
					}
					if (bSOX) Cur.X = (float)SOX;
					if (bSOY) Cur.Y = (float)SOY;
					TB->SetShadowOffset(Cur);
					if (bSOX) Applied.Add(TEXT("shadow_offset_x"));
					if (bSOY) Applied.Add(TEXT("shadow_offset_y"));
				}
				FString ShadowHex;
				if (S->TryGetStringField(TEXT("shadow_color"), ShadowHex))
				{
					TB->SetShadowColorAndOpacity(NGGWidgetPriv::ParseHexColor(ShadowHex));
					Applied.Add(TEXT("shadow_color"));
				}
				double MinDW = 0;
				if (S->TryGetNumberField(TEXT("min_desired_width"), MinDW))
				{
					TB->SetMinDesiredWidth((float)MinDW);
					Applied.Add(TEXT("min_desired_width"));
				}
			}
			else if (URichTextBlock* RTB = Cast<URichTextBlock>(W))
			{
				FString TextStr;
				if (S->TryGetStringField(TEXT("text"), TextStr))
				{
					RTB->SetText(FText::FromString(TextStr));
					Applied.Add(TEXT("text"));
				}
				bool bAutoWrap = false;
				if (S->TryGetBoolField(TEXT("auto_wrap"), bAutoWrap))
				{
					RTB->SetAutoWrapText(bAutoWrap);
					Applied.Add(TEXT("auto_wrap"));
				}
				double MinDW = 0;
				if (S->TryGetNumberField(TEXT("min_desired_width"), MinDW))
				{
					if (FFloatProperty* P = FindFProperty<FFloatProperty>(RTB->GetClass(), TEXT("MinDesiredWidth")))
					{
						P->SetPropertyValue_InContainer(RTB, (float)MinDW);
						Applied.Add(TEXT("min_desired_width"));
					}
					else
					{
						Skipped.Add(TEXT("min_desired_width (property not found)"));
					}
				}
				FString StylePath;
				if (S->TryGetStringField(TEXT("text_style_set"), StylePath))
				{
					if (UDataTable* DT = LoadObject<UDataTable>(nullptr, *StylePath))
					{
						RTB->SetTextStyleSet(DT);
						Applied.Add(TEXT("text_style_set"));
					}
					else
					{
						Skipped.Add(TEXT("text_style_set (asset load failed)"));
					}
				}
			}
			else if (UButton* Btn = Cast<UButton>(W))
			{
				FButtonStyle Style = Btn->GetStyle();
				bool bStyleDirty = false;

				FString BgHex;
				if (S->TryGetStringField(TEXT("background_color"), BgHex))
				{
					Style.Normal.TintColor = FSlateColor(NGGWidgetPriv::ParseHexColor(BgHex));
					bStyleDirty = true;
					Applied.Add(TEXT("background_color"));
				}
				FString HoverHex;
				if (S->TryGetStringField(TEXT("hovered_color"), HoverHex))
				{
					Style.Hovered.TintColor = FSlateColor(NGGWidgetPriv::ParseHexColor(HoverHex));
					bStyleDirty = true;
					Applied.Add(TEXT("hovered_color"));
				}
				FString PressHex;
				if (S->TryGetStringField(TEXT("pressed_color"), PressHex))
				{
					Style.Pressed.TintColor = FSlateColor(NGGWidgetPriv::ParseHexColor(PressHex));
					bStyleDirty = true;
					Applied.Add(TEXT("pressed_color"));
				}
				FString DisHex;
				if (S->TryGetStringField(TEXT("disabled_color"), DisHex))
				{
					Style.Disabled.TintColor = FSlateColor(NGGWidgetPriv::ParseHexColor(DisHex));
					bStyleDirty = true;
					Applied.Add(TEXT("disabled_color"));
				}
				const TSharedPtr<FJsonObject>* CPObj;
				if (S->TryGetObjectField(TEXT("content_padding"), CPObj))
				{
					const FMargin M = NGGWidgetPriv::ReadMargin(*CPObj);
					Style.NormalPadding  = M;
					Style.PressedPadding = M;
					bStyleDirty = true;
					Applied.Add(TEXT("content_padding"));
				}
				if (bStyleDirty) Btn->SetStyle(Style);

				FString Click;
				if (S->TryGetStringField(TEXT("click_method"), Click))
				{
					Btn->SetClickMethod(NGGWidgetPriv::ParseClickMethod(Click));
					Applied.Add(TEXT("click_method"));
				}
				bool bFocus = false;
				if (S->TryGetBoolField(TEXT("is_focusable"), bFocus))
				{
					if (FBoolProperty* P = FindFProperty<FBoolProperty>(Btn->GetClass(), TEXT("bIsFocusable")))
					{
						P->SetPropertyValue_InContainer(Btn, bFocus);
						Applied.Add(TEXT("is_focusable"));
					}
					else if (FBoolProperty* P2 = FindFProperty<FBoolProperty>(Btn->GetClass(), TEXT("IsFocusable")))
					{
						P2->SetPropertyValue_InContainer(Btn, bFocus);
						Applied.Add(TEXT("is_focusable"));
					}
					else
					{
						Skipped.Add(TEXT("is_focusable (property not found)"));
					}
				}
			}
			else if (UImage* Img = Cast<UImage>(W))
			{
				FSlateBrush NewBrush = Img->GetBrush();
				bool bBrushDirty = false;

				FString TexPath;
				if (S->TryGetStringField(TEXT("brush_texture"), TexPath))
				{
					if (UTexture2D* Tex = NGGWidgetPriv::LoadTextureAsset(TexPath))
					{
						NewBrush.SetResourceObject(Tex);
						NewBrush.ImageSize = FVector2D(Tex->GetSizeX(), Tex->GetSizeY());
						bBrushDirty = true;
						Applied.Add(TEXT("brush_texture"));
					}
					else
					{
						Skipped.Add(TEXT("brush_texture (texture load failed)"));
					}
				}
				FString DrawAs;
				if (S->TryGetStringField(TEXT("brush_draw_as"), DrawAs))
				{
					NewBrush.DrawAs = NGGWidgetPriv::ParseBrushDrawAs(DrawAs);
					bBrushDirty = true;
					Applied.Add(TEXT("brush_draw_as"));
				}
				const TSharedPtr<FJsonObject>* MarginObj;
				if (S->TryGetObjectField(TEXT("brush_margin"), MarginObj))
				{
					NewBrush.Margin = NGGWidgetPriv::ReadMargin(*MarginObj);
					bBrushDirty = true;
					Applied.Add(TEXT("brush_margin"));
				}
				FString Tiling;
				if (S->TryGetStringField(TEXT("brush_tiling"), Tiling))
				{
					if      (Tiling == TEXT("Horizontal")) NewBrush.Tiling = ESlateBrushTileType::Horizontal;
					else if (Tiling == TEXT("Vertical"))   NewBrush.Tiling = ESlateBrushTileType::Vertical;
					else if (Tiling == TEXT("Both"))       NewBrush.Tiling = ESlateBrushTileType::Both;
					else                                   NewBrush.Tiling = ESlateBrushTileType::NoTile;
					bBrushDirty = true;
					Applied.Add(TEXT("brush_tiling"));
				}
				double BSX = 0, BSY = 0;
				const bool bBSX = S->TryGetNumberField(TEXT("brush_size_x"), BSX);
				const bool bBSY = S->TryGetNumberField(TEXT("brush_size_y"), BSY);
				if (bBSX || bBSY)
				{
					if (bBSX) NewBrush.ImageSize.X = (float)BSX;
					if (bBSY) NewBrush.ImageSize.Y = (float)BSY;
					bBrushDirty = true;
					if (bBSX) Applied.Add(TEXT("brush_size_x"));
					if (bBSY) Applied.Add(TEXT("brush_size_y"));
				}
				if (bBrushDirty) Img->SetBrush(NewBrush);

				FString TintHex;
				if (S->TryGetStringField(TEXT("brush_tint"), TintHex))
				{
					Img->SetColorAndOpacity(NGGWidgetPriv::ParseHexColor(TintHex));
					Applied.Add(TEXT("brush_tint"));
				}
			}
			else if (UBorder* Brd = Cast<UBorder>(W))
			{
				FString TexPath;
				if (S->TryGetStringField(TEXT("brush_texture"), TexPath))
				{
					if (UTexture2D* Tex = NGGWidgetPriv::LoadTextureAsset(TexPath))
					{
						Brd->SetBrushFromTexture(Tex);
						Applied.Add(TEXT("brush_texture"));
					}
					else
					{
						Skipped.Add(TEXT("brush_texture (texture load failed)"));
					}
				}
				FString TintHex;
				if (S->TryGetStringField(TEXT("brush_tint"), TintHex))
				{
					Brd->SetBrushColor(NGGWidgetPriv::ParseHexColor(TintHex));
					Applied.Add(TEXT("brush_tint"));
				}
				FString ContentHex;
				if (S->TryGetStringField(TEXT("content_color"), ContentHex))
				{
					Brd->SetContentColorAndOpacity(NGGWidgetPriv::ParseHexColor(ContentHex));
					Applied.Add(TEXT("content_color"));
				}
				const TSharedPtr<FJsonObject>* CPObj;
				if (S->TryGetObjectField(TEXT("content_padding"), CPObj))
				{
					Brd->SetPadding(NGGWidgetPriv::ReadMargin(*CPObj));
					Applied.Add(TEXT("content_padding"));
				}
				FString HA;
				if (S->TryGetStringField(TEXT("horizontal_alignment"), HA))
				{
					Brd->SetHorizontalAlignment(NGGWidgetPriv::ParseHAlign(HA));
					Applied.Add(TEXT("horizontal_alignment"));
				}
				FString VA;
				if (S->TryGetStringField(TEXT("vertical_alignment"), VA))
				{
					Brd->SetVerticalAlignment(NGGWidgetPriv::ParseVAlign(VA));
					Applied.Add(TEXT("vertical_alignment"));
				}
				FString DrawAs;
				if (S->TryGetStringField(TEXT("brush_draw_as"), DrawAs))
				{
					if (FStructProperty* P = FindFProperty<FStructProperty>(Brd->GetClass(), TEXT("Background")))
					{
						if (FSlateBrush* BPtr = P->ContainerPtrToValuePtr<FSlateBrush>(Brd))
						{
							BPtr->DrawAs = NGGWidgetPriv::ParseBrushDrawAs(DrawAs);
							Applied.Add(TEXT("brush_draw_as"));
						}
					}
				}
			}
			else if (UCheckBox* CB = Cast<UCheckBox>(W))
			{
				bool bChecked = false;
				if (S->TryGetBoolField(TEXT("is_checked"), bChecked))
				{
					CB->SetIsChecked(bChecked);
					Applied.Add(TEXT("is_checked"));
				}
				if (S->HasField(TEXT("check_type")))
				{
					Applied.Add(TEXT("check_type"));
				}
			}
			else if (UProgressBar* PB = Cast<UProgressBar>(W))
			{
				FString FillHex;
				if (!S->TryGetStringField(TEXT("fill_color"), FillHex))
				{
					S->TryGetStringField(TEXT("color"), FillHex);
				}
				if (!FillHex.IsEmpty())
				{
					PB->SetFillColorAndOpacity(NGGWidgetPriv::ParseHexColor(FillHex));
					Applied.Add(TEXT("fill_color"));
					if (S->HasField(TEXT("color"))) Applied.Add(TEXT("color"));
				}
				double Pct = 0;
				if (S->TryGetNumberField(TEXT("percent"), Pct))
				{
					PB->SetPercent((float)Pct);
					Applied.Add(TEXT("percent"));
				}
				FString FillType;
				if (S->TryGetStringField(TEXT("bar_fill_type"), FillType))
				{
					if (FByteProperty* P = FindFProperty<FByteProperty>(PB->GetClass(), TEXT("BarFillType")))
					{
						P->SetPropertyValue_InContainer(PB, (uint8)NGGWidgetPriv::ParseBarFillType(FillType));
						Applied.Add(TEXT("bar_fill_type"));
					}
					else if (FEnumProperty* EP = FindFProperty<FEnumProperty>(PB->GetClass(), TEXT("BarFillType")))
					{
						uint8 V = (uint8)NGGWidgetPriv::ParseBarFillType(FillType);
						EP->GetUnderlyingProperty()->SetIntPropertyValue(
							EP->ContainerPtrToValuePtr<void>(PB), (int64)V);
						Applied.Add(TEXT("bar_fill_type"));
					}
					else
					{
						Skipped.Add(TEXT("bar_fill_type (property not found)"));
					}
				}
				bool bMarquee = false;
				if (S->TryGetBoolField(TEXT("marquee"), bMarquee))
				{
					if (FBoolProperty* P = FindFProperty<FBoolProperty>(PB->GetClass(), TEXT("bIsMarquee")))
					{
						P->SetPropertyValue_InContainer(PB, bMarquee);
						Applied.Add(TEXT("marquee"));
					}
					else
					{
						Skipped.Add(TEXT("marquee (property not found)"));
					}
				}
			}
			else if (USlider* Sl = Cast<USlider>(W))
			{
				double Val = 0;
				if (S->TryGetNumberField(TEXT("value"), Val))
				{
					Sl->SetValue((float)Val);
					Applied.Add(TEXT("value"));
				}
				double MinV = 0;
				if (S->TryGetNumberField(TEXT("min_value"), MinV))
				{
					Sl->SetMinValue((float)MinV);
					Applied.Add(TEXT("min_value"));
				}
				double MaxV = 0;
				if (S->TryGetNumberField(TEXT("max_value"), MaxV))
				{
					Sl->SetMaxValue((float)MaxV);
					Applied.Add(TEXT("max_value"));
				}
				double Step = 0;
				if (S->TryGetNumberField(TEXT("step_size"), Step))
				{
					Sl->SetStepSize((float)Step);
					Applied.Add(TEXT("step_size"));
				}
				FString Ori;
				if (S->TryGetStringField(TEXT("orientation"), Ori))
				{
					Sl->SetOrientation(NGGWidgetPriv::ParseOrientation(Ori));
					Applied.Add(TEXT("orientation"));
				}
				FString BarHex;
				if (S->TryGetStringField(TEXT("slider_bar_color"), BarHex))
				{
					Sl->SetSliderBarColor(NGGWidgetPriv::ParseHexColor(BarHex));
					Applied.Add(TEXT("slider_bar_color"));
				}
				FString HandleHex;
				if (S->TryGetStringField(TEXT("slider_handle_color"), HandleHex))
				{
					Sl->SetSliderHandleColor(NGGWidgetPriv::ParseHexColor(HandleHex));
					Applied.Add(TEXT("slider_handle_color"));
				}
			}
			else if (UScrollBox* SB = Cast<UScrollBox>(W))
			{
				FString Ori;
				if (S->TryGetStringField(TEXT("orientation"), Ori))
				{
					SB->SetOrientation(NGGWidgetPriv::ParseOrientation(Ori));
					Applied.Add(TEXT("orientation"));
				}
				FString VisStr2;
				if (S->TryGetStringField(TEXT("scroll_bar_visibility"), VisStr2))
				{
					SB->SetScrollBarVisibility(NGGWidgetPriv::ParseVisibility(VisStr2));
					Applied.Add(TEXT("scroll_bar_visibility"));
				}
				bool bOver = false;
				if (S->TryGetBoolField(TEXT("allow_overscroll"), bOver))
				{
					SB->SetAllowOverscroll(bOver);
					Applied.Add(TEXT("allow_overscroll"));
				}
				double Thick = 0;
				if (S->TryGetNumberField(TEXT("scroll_bar_thickness"), Thick))
				{
					SB->SetScrollbarThickness(FVector2D((float)Thick, (float)Thick));
					Applied.Add(TEXT("scroll_bar_thickness"));
				}
				bool bAlways = false;
				if (S->TryGetBoolField(TEXT("always_show_scrollbar"), bAlways))
				{
					SB->SetAlwaysShowScrollbar(bAlways);
					Applied.Add(TEXT("always_show_scrollbar"));
				}
			}
			else if (USizeBox* SZB = Cast<USizeBox>(W))
			{
				double V = 0;
				if (S->TryGetNumberField(TEXT("width_override"),  V)) { SZB->SetWidthOverride((float)V);     Applied.Add(TEXT("width_override")); }
				if (S->TryGetNumberField(TEXT("height_override"), V)) { SZB->SetHeightOverride((float)V);    Applied.Add(TEXT("height_override")); }
				if (S->TryGetNumberField(TEXT("min_width"),       V)) { SZB->SetMinDesiredWidth((float)V);   Applied.Add(TEXT("min_width")); }
				if (S->TryGetNumberField(TEXT("min_height"),      V)) { SZB->SetMinDesiredHeight((float)V);  Applied.Add(TEXT("min_height")); }
				if (S->TryGetNumberField(TEXT("max_width"),       V)) { SZB->SetMaxDesiredWidth((float)V);   Applied.Add(TEXT("max_width")); }
				if (S->TryGetNumberField(TEXT("max_height"),      V)) { SZB->SetMaxDesiredHeight((float)V);  Applied.Add(TEXT("max_height")); }
				if (S->TryGetNumberField(TEXT("min_aspect_ratio"),V)) { SZB->SetMinAspectRatio((float)V);    Applied.Add(TEXT("min_aspect_ratio")); }
				if (S->TryGetNumberField(TEXT("max_aspect_ratio"),V)) { SZB->SetMaxAspectRatio((float)V);    Applied.Add(TEXT("max_aspect_ratio")); }
				bool bClr = false;
				if (S->TryGetBoolField(TEXT("clear_width_override"),  bClr) && bClr) { SZB->ClearWidthOverride();  Applied.Add(TEXT("clear_width_override")); }
				if (S->TryGetBoolField(TEXT("clear_height_override"), bClr) && bClr) { SZB->ClearHeightOverride(); Applied.Add(TEXT("clear_height_override")); }
			}
			else if (USpacer* Sp = Cast<USpacer>(W))
			{
				double SX = 0, SY = 0;
				const bool bSX = S->TryGetNumberField(TEXT("size_x"), SX);
				const bool bSY = S->TryGetNumberField(TEXT("size_y"), SY);
				if (bSX || bSY)
				{
					FVector2D Cur(0, 0);
					if (FStructProperty* P = FindFProperty<FStructProperty>(Sp->GetClass(), TEXT("Size")))
					{
						if (FVector2D* V = P->ContainerPtrToValuePtr<FVector2D>(Sp))
						{
							Cur = *V;
						}
					}
					if (bSX) Cur.X = (float)SX;
					if (bSY) Cur.Y = (float)SY;
					Sp->SetSize(Cur);
					if (bSX) Applied.Add(TEXT("size_x"));
					if (bSY) Applied.Add(TEXT("size_y"));
				}
			}
			else if (UWidgetSwitcher* WS2 = Cast<UWidgetSwitcher>(W))
			{
				int32 Idx = 0;
				if (S->TryGetNumberField(TEXT("active_widget_index"), Idx))
				{
					WS2->SetActiveWidgetIndex(Idx);
					Applied.Add(TEXT("active_widget_index"));
				}
			}
			else if (UCircularThrobber* CT = Cast<UCircularThrobber>(W))
			{
				int32 N = 0;
				if (S->TryGetNumberField(TEXT("number_of_pieces"), N))
				{
					CT->SetNumberOfPieces(N);
					Applied.Add(TEXT("number_of_pieces"));
				}
				double Period = 0, Radius = 0;
				if (S->TryGetNumberField(TEXT("period"), Period)) { CT->SetPeriod((float)Period); Applied.Add(TEXT("period")); }
				if (S->TryGetNumberField(TEXT("radius"), Radius)) { CT->SetRadius((float)Radius); Applied.Add(TEXT("radius")); }
			}
			else if (UThrobber* Th = Cast<UThrobber>(W))
			{
				int32 N = 0;
				if (S->TryGetNumberField(TEXT("number_of_pieces"), N))
				{
					Th->SetNumberOfPieces(N);
					Applied.Add(TEXT("number_of_pieces"));
				}
				// Throbber animate flags are not exposed as setters in all UE5 versions;
				// set them via reflection so the build doesn't depend on which setters exist.
				auto SetThrobberBool = [&](const FString& Key, const TCHAR* PropName)
				{
					bool bVal = false;
					if (!S->TryGetBoolField(Key, bVal)) return;
					if (FBoolProperty* P = FindFProperty<FBoolProperty>(Th->GetClass(), PropName))
					{
						P->SetPropertyValue_InContainer(Th, bVal);
						Applied.Add(Key);
					}
				};
				SetThrobberBool(TEXT("animate_horizontally"), TEXT("bAnimateHorizontally"));
				SetThrobberBool(TEXT("animate_vertically"),   TEXT("bAnimateVertically"));
				SetThrobberBool(TEXT("animate_opacity"),      TEXT("bAnimateOpacity"));
			}
			else if (UBackgroundBlur* BB = Cast<UBackgroundBlur>(W))
			{
				double BS = 0;
				if (S->TryGetNumberField(TEXT("blur_strength"), BS))
				{
					BB->SetBlurStrength((float)BS);
					Applied.Add(TEXT("blur_strength"));
				}
				int32 BR = 0;
				if (S->TryGetNumberField(TEXT("blur_radius"), BR))
				{
					BB->SetBlurRadius(BR);
					Applied.Add(TEXT("blur_radius"));
				}
				const TSharedPtr<FJsonObject>* PObj;
				if (S->TryGetObjectField(TEXT("padding"), PObj))
				{
					BB->SetPadding(NGGWidgetPriv::ReadMargin(*PObj));
					Applied.Add(TEXT("padding"));
				}
				else if (S->TryGetObjectField(TEXT("content_padding"), PObj))
				{
					BB->SetPadding(NGGWidgetPriv::ReadMargin(*PObj));
					Applied.Add(TEXT("content_padding"));
				}
			}
			else if (USafeZone* SZ = Cast<USafeZone>(W))
			{
				PRAGMA_DISABLE_DEPRECATION_WARNINGS
				bool bL = SZ->PadLeft, bT = SZ->PadTop, bR = SZ->PadRight, bB = SZ->PadBottom;
				PRAGMA_ENABLE_DEPRECATION_WARNINGS
				bool bChanged = false;
				const TSharedPtr<FJsonObject>* SidesObj;
				if (S->TryGetObjectField(TEXT("pad_sides"), SidesObj))
				{
					(*SidesObj)->TryGetBoolField(TEXT("left"),   bL);
					(*SidesObj)->TryGetBoolField(TEXT("top"),    bT);
					(*SidesObj)->TryGetBoolField(TEXT("right"),  bR);
					(*SidesObj)->TryGetBoolField(TEXT("bottom"), bB);
					bChanged = true;
					Applied.Add(TEXT("pad_sides"));
				}
				bool bTmp = false;
				if (S->TryGetBoolField(TEXT("pad_left"),   bTmp)) { bL = bTmp; bChanged = true; Applied.Add(TEXT("pad_left")); }
				if (S->TryGetBoolField(TEXT("pad_top"),    bTmp)) { bT = bTmp; bChanged = true; Applied.Add(TEXT("pad_top")); }
				if (S->TryGetBoolField(TEXT("pad_right"),  bTmp)) { bR = bTmp; bChanged = true; Applied.Add(TEXT("pad_right")); }
				if (S->TryGetBoolField(TEXT("pad_bottom"), bTmp)) { bB = bTmp; bChanged = true; Applied.Add(TEXT("pad_bottom")); }
				if (bChanged) SZ->SetSidesToPad(bL, bR, bT, bB);
				if (S->HasField(TEXT("is_titlesafe")))
				{
					Skipped.Add(TEXT("is_titlesafe (USafeZone::IsTitleSafe removed in UE5.7 — use r.DebugSafeZone.TitleRatio cvar)"));
				}
			}
			else if (UScaleBox* SCB = Cast<UScaleBox>(W))
			{
				FString St;
				if (S->TryGetStringField(TEXT("stretch"), St))
				{
					EStretch::Type ST = EStretch::None;
					if      (St == TEXT("Fill"))           ST = EStretch::Fill;
					else if (St == TEXT("ScaleToFit"))     ST = EStretch::ScaleToFit;
					else if (St == TEXT("ScaleToFitX"))    ST = EStretch::ScaleToFitX;
					else if (St == TEXT("ScaleToFitY"))    ST = EStretch::ScaleToFitY;
					else if (St == TEXT("ScaleToFill"))    ST = EStretch::ScaleToFill;
					else if (St == TEXT("ScaleBySafeZone"))ST = EStretch::ScaleBySafeZone;
					else if (St == TEXT("UserSpecified"))  ST = EStretch::UserSpecified;
					SCB->SetStretch(ST);
					Applied.Add(TEXT("stretch"));
				}
				FString SD;
				if (S->TryGetStringField(TEXT("stretch_direction"), SD))
				{
					EStretchDirection::Type D = EStretchDirection::Both;
					if      (SD == TEXT("DownOnly")) D = EStretchDirection::DownOnly;
					else if (SD == TEXT("UpOnly"))   D = EStretchDirection::UpOnly;
					SCB->SetStretchDirection(D);
					Applied.Add(TEXT("stretch_direction"));
				}
				double USS = 0;
				if (S->TryGetNumberField(TEXT("user_specified_scale"), USS))
				{
					SCB->SetUserSpecifiedScale((float)USS);
					Applied.Add(TEXT("user_specified_scale"));
				}
			}
			else if (UComboBoxString* CBox = Cast<UComboBoxString>(W))
			{
				bool bClear = false;
				if (S->TryGetBoolField(TEXT("clear_options"), bClear) && bClear)
				{
					CBox->ClearOptions();
					Applied.Add(TEXT("clear_options"));
				}
				FString OneOption;
				if (S->TryGetStringField(TEXT("add_option"), OneOption))
				{
					CBox->AddOption(OneOption);
					Applied.Add(TEXT("add_option"));
				}
				else
				{
					const TArray<TSharedPtr<FJsonValue>>* OptArr;
					if (S->TryGetArrayField(TEXT("add_option"), OptArr))
					{
						for (const TSharedPtr<FJsonValue>& Opt : *OptArr)
						{
							FString OptStr;
							if (Opt->TryGetString(OptStr))
							{
								CBox->AddOption(OptStr);
							}
						}
						Applied.Add(TEXT("add_option"));
					}
				}
				FString Sel;
				if (S->TryGetStringField(TEXT("selected_option"), Sel))
				{
					CBox->SetSelectedOption(Sel);
					Applied.Add(TEXT("selected_option"));
				}
			}
			else if (UEditableTextBox* ETB = Cast<UEditableTextBox>(W))
			{
				double FontSizeVal = 0;
				if (S->TryGetNumberField(TEXT("font_size"), FontSizeVal))
				{
					FEditableTextBoxStyle Style = ETB->WidgetStyle;
					Style.TextStyle.Font.Size = (int32)FontSizeVal;
					ETB->WidgetStyle = Style;
					Applied.Add(TEXT("font_size"));
				}
				FString TextStr;
				if (S->TryGetStringField(TEXT("text"), TextStr))
				{
					ETB->SetText(FText::FromString(TextStr));
					Applied.Add(TEXT("text"));
				}
			}
			else
			{
				static const TArray<FString> TypeSpecificKeys = {
					TEXT("text"), TEXT("font_size"), TEXT("font_bold"), TEXT("font_typeface"),
					TEXT("color"), TEXT("fill_color"), TEXT("background_color"),
					TEXT("hovered_color"), TEXT("pressed_color"), TEXT("disabled_color"),
					TEXT("auto_wrap"), TEXT("horizontal_alignment"), TEXT("percent"),
					TEXT("brush_texture"), TEXT("brush_tint"), TEXT("brush_size_x"),
					TEXT("brush_size_y"), TEXT("brush_draw_as"), TEXT("brush_margin"),
					TEXT("brush_tiling"), TEXT("value"), TEXT("min_value"), TEXT("max_value"),
					TEXT("step_size"), TEXT("orientation"), TEXT("is_checked"),
					TEXT("active_widget_index"), TEXT("number_of_pieces"), TEXT("blur_strength"),
					TEXT("stretch"), TEXT("add_option")
				};
				for (const FString& K : TypeSpecificKeys)
				{
					if (S->HasField(K)) AddUnsupportedWidget(K);
				}
			}

			for (const FString& K : AllKnownKeys)
			{
				if (S->HasField(K) && !Applied.Contains(K))
				{
					const bool bAlreadySkipped = Skipped.ContainsByPredicate(
						[&K](const FString& Elem){ return Elem.StartsWith(K); });
					if (!bAlreadySkipped)
					{
						Skipped.Add(K);
					}
				}
			}

			TotalApplied += Applied.Num();
			TotalSkipped += Skipped.Num();

			TArray<TSharedPtr<FJsonValue>> AppliedJson, SkippedJson;
			for (const FString& A  : Applied)  AppliedJson.Add(MakeShared<FJsonValueString>(A));
			for (const FString& Sk : Skipped)  SkippedJson.Add(MakeShared<FJsonValueString>(Sk));

			WidgetResult->SetStringField(TEXT("status"),       TEXT("ok"));
			WidgetResult->SetStringField(TEXT("widget_class"), W->GetClass()->GetName());
			WidgetResult->SetArrayField (TEXT("applied"),      AppliedJson);
			WidgetResult->SetArrayField (TEXT("skipped"),      SkippedJson);
			ResultsJson.Add(MakeShared<FJsonValueObject>(WidgetResult));
		}

		// ---------- 5. Mark blueprint modified + dirty -----------------------
		FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
		WBP->MarkPackageDirty();

		// ---------- 6. Build response ----------------------------------------
		TSharedPtr<FJsonObject> Resp2 = MakeShared<FJsonObject>();
		Resp2->SetBoolField  (TEXT("success"),         true);
		Resp2->SetStringField(TEXT("widget_blueprint"), WBPPath);
		Resp2->SetNumberField(TEXT("total_applied"),    static_cast<double>(TotalApplied));
		Resp2->SetNumberField(TEXT("total_skipped"),    static_cast<double>(TotalSkipped));
		Resp2->SetArrayField (TEXT("results"),          ResultsJson);

		FString RespBody2;
		TSharedRef<TJsonWriter<>> Writer2 = TJsonWriterFactory<>::Create(&RespBody2);
		FJsonSerializer::Serialize(Resp2.ToSharedRef(), Writer2);
		Callback(JsonOk(RespBody2));
	});

	return true;
}


// ============================================================================
// Handler: POST /editor/remove_widget_from_blueprint
// Body: { "widget_blueprint": "/Game/...", "widget_name": "MyButton", "cascade": true }
// ============================================================================
bool FNGGHttpServer::HandleRemoveWidgetFromBlueprint(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString WBPPath, WidgetName;
	Body->TryGetStringField(TEXT("widget_blueprint"), WBPPath);
	Body->TryGetStringField(TEXT("widget_name"),      WidgetName);
	bool bCascade = true;
	Body->TryGetBoolField(TEXT("cascade"), bCascade);

	if (WBPPath.IsEmpty() || WidgetName.IsEmpty())
	{
		Callback(JsonError(400, TEXT("widget_blueprint and widget_name are required")));
		return true;
	}

	UE_LOG(LogNGGBridge, Log, TEXT("HandleRemoveWidgetFromBlueprint: '%s' name='%s' cascade=%s"),
		*WBPPath, *WidgetName, bCascade ? TEXT("true") : TEXT("false"));

	AsyncTask(ENamedThreads::GameThread, [WBPPath, WidgetName, bCascade, Callback]()
	{
		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
		if (!IsValid(WBP) || !IsValid(WBP->WidgetTree))
		{
			Callback(JsonError(404, FString::Printf(TEXT("Widget Blueprint not found: %s"), *WBPPath)));
			return;
		}

		UWidget* Target = WBP->WidgetTree->FindWidget(FName(*WidgetName));
		if (!IsValid(Target))
		{
			Callback(JsonError(404, FString::Printf(TEXT("Widget '%s' not found"), *WidgetName)));
			return;
		}

		if (WBP->WidgetTree->RootWidget == Target)
		{
			Callback(JsonError(400, TEXT("cannot remove root widget")));
			return;
		}

		UPanelWidget* Panel = Cast<UPanelWidget>(Target);
		if (!bCascade && Panel && Panel->GetChildrenCount() > 0)
		{
			Callback(JsonError(400, TEXT("widget has children — pass cascade:true")));
			return;
		}

		TArray<FString> Removed;
		TArray<UWidget*> ToRemove;
		TFunction<void(UWidget*)> Walk;
		Walk = [&Walk, &Removed, &ToRemove](UWidget* W)
		{
			if (!W) return;
			Removed.Add(W->GetFName().ToString());
			ToRemove.Add(W);
			if (UPanelWidget* P = Cast<UPanelWidget>(W))
			{
				for (int32 i = 0; i < P->GetChildrenCount(); ++i)
				{
					Walk(P->GetChildAt(i));
				}
			}
		};
		Walk(Target);

		// Detach the target from its parent panel first so removal is clean.
		if (UPanelWidget* CurParent = Target->GetParent())
		{
			CurParent->RemoveChild(Target);
		}
		// Remove all descendants (and the target) from the WidgetTree, then
		// move them out from under the WBP outer chain so the widget BP
		// compiler's ForEachSourceWidget (which uses ForEachObjectWithOuter)
		// no longer enumerates them.  Without this step a later
		// compile_widget_blueprint call can crash inside
		// ValidateAndFixUpVariableGuids while iterating still-parented dead
		// widgets whose GUID entry we just removed.
		UPackage* TransientPkg = GetTransientPackage();
		for (UWidget* W2 : ToRemove)
		{
			if (!IsValid(W2)) continue;
			WBP->WidgetTree->RemoveWidget(W2);
			W2->Rename(
				nullptr,
				TransientPkg,
				// UE5.8: REN_ForceNoResetLoaders is deprecated — Rename no longer calls
				// ResetLoaders, so the flag is a no-op and was dropped.
				REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional);
			W2->MarkAsGarbage();
		}

		for (const FString& Name : Removed)
		{
			WBP->WidgetVariableNameToGuidMap.Remove(FName(*Name));
		}

		NGGWidgetPriv::MarkWBPDirtyAndModified(WBP);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),          true);
		Result->SetStringField(TEXT("widget_blueprint"), WBPPath);
		TArray<TSharedPtr<FJsonValue>> RemovedJson;
		for (const FString& N : Removed)
		{
			RemovedJson.Add(MakeShared<FJsonValueString>(N));
		}
		Result->SetArrayField(TEXT("removed"), RemovedJson);

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/reparent_widget
// Body: { "widget_blueprint", "widget_name", "new_parent", "child_index"? }
// ============================================================================
bool FNGGHttpServer::HandleReparentWidget(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString WBPPath, WidgetName, NewParent;
	Body->TryGetStringField(TEXT("widget_blueprint"), WBPPath);
	Body->TryGetStringField(TEXT("widget_name"),      WidgetName);
	Body->TryGetStringField(TEXT("new_parent"),       NewParent);
	int32 ChildIndex = INDEX_NONE;
	const bool bHasIndex = Body->TryGetNumberField(TEXT("child_index"), ChildIndex);

	if (WBPPath.IsEmpty() || WidgetName.IsEmpty() || NewParent.IsEmpty())
	{
		Callback(JsonError(400, TEXT("widget_blueprint, widget_name, new_parent are required")));
		return true;
	}

	UE_LOG(LogNGGBridge, Log, TEXT("HandleReparentWidget: '%s' '%s' -> '%s' idx=%d"),
		*WBPPath, *WidgetName, *NewParent, ChildIndex);

	AsyncTask(ENamedThreads::GameThread, [WBPPath, WidgetName, NewParent, ChildIndex, bHasIndex, Callback]()
	{
		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
		if (!IsValid(WBP) || !IsValid(WBP->WidgetTree))
		{
			Callback(JsonError(404, FString::Printf(TEXT("Widget Blueprint not found: %s"), *WBPPath)));
			return;
		}

		UWidget* W = WBP->WidgetTree->FindWidget(FName(*WidgetName));
		UWidget* P = WBP->WidgetTree->FindWidget(FName(*NewParent));
		if (!IsValid(W))
		{
			Callback(JsonError(404, FString::Printf(TEXT("Widget '%s' not found"), *WidgetName)));
			return;
		}
		if (!IsValid(P))
		{
			Callback(JsonError(404, FString::Printf(TEXT("Parent widget '%s' not found"), *NewParent)));
			return;
		}
		if (WBP->WidgetTree->RootWidget == W)
		{
			Callback(JsonError(400, TEXT("cannot reparent root widget")));
			return;
		}
		UPanelWidget* NewParentPanel = Cast<UPanelWidget>(P);
		if (!NewParentPanel)
		{
			Callback(JsonError(400, FString::Printf(TEXT("new_parent '%s' is not a panel"), *NewParent)));
			return;
		}
		if (W == P)
		{
			Callback(JsonError(400, TEXT("cannot reparent widget under itself")));
			return;
		}

		// Reject if P is a descendant of W (would create a cycle).
		UWidget* Cursor = P;
		while (Cursor)
		{
			if (Cursor == W)
			{
				Callback(JsonError(400, TEXT("cannot reparent under a descendant")));
				return;
			}
			Cursor = Cursor->GetParent();
		}

		if (!NewParentPanel->CanAddMoreChildren())
		{
			Callback(JsonError(400, FString::Printf(
				TEXT("new_parent '%s' (%s) cannot accept more children — single-child container already full"),
				*NewParent, *NewParentPanel->GetClass()->GetName())));
			return;
		}

		UPanelWidget* OldParent  = W->GetParent();
		const int32   OldIndex   = OldParent ? OldParent->GetChildIndex(W) : INDEX_NONE;

		if (OldParent)
		{
			OldParent->RemoveChild(W);
		}

		UPanelSlot* NewSlot = NewParentPanel->AddChild(W);
		if (!NewSlot)
		{
			if (OldParent)
			{
				if (UPanelSlot* RestoredSlot = OldParent->AddChild(W))
				{
					if (OldIndex >= 0 && OldIndex < OldParent->GetChildrenCount())
					{
						OldParent->ShiftChild(OldIndex, W);
					}
				}
			}
			Callback(JsonError(400, FString::Printf(
				TEXT("new_parent '%s' (%s) refused the widget — likely a single-child container that is already full"),
				*NewParent, *NewParentPanel->GetClass()->GetName())));
			return;
		}

		if (bHasIndex && ChildIndex >= 0 && ChildIndex < NewParentPanel->GetChildrenCount())
		{
			NewParentPanel->ShiftChild(ChildIndex, W);
		}

		NGGWidgetPriv::EnsureGuid(WBP, W);
		NGGWidgetPriv::MarkWBPDirtyAndModified(WBP);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),          true);
		Result->SetStringField(TEXT("widget_blueprint"), WBPPath);
		Result->SetStringField(TEXT("widget_name"),      WidgetName);
		Result->SetStringField(TEXT("new_parent"),       NewParent);
		Result->SetStringField(TEXT("new_slot_class"),   W->Slot ? W->Slot->GetClass()->GetName() : TEXT(""));

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/rename_widget
// Body: { "widget_blueprint", "old_name", "new_name" }
// ============================================================================
bool FNGGHttpServer::HandleRenameWidget(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString WBPPath, OldName, NewName;
	Body->TryGetStringField(TEXT("widget_blueprint"), WBPPath);
	Body->TryGetStringField(TEXT("old_name"),         OldName);
	Body->TryGetStringField(TEXT("new_name"),         NewName);

	if (WBPPath.IsEmpty() || OldName.IsEmpty() || NewName.IsEmpty())
	{
		Callback(JsonError(400, TEXT("widget_blueprint, old_name, new_name are required")));
		return true;
	}

	UE_LOG(LogNGGBridge, Log, TEXT("HandleRenameWidget: '%s' '%s' -> '%s'"), *WBPPath, *OldName, *NewName);

	AsyncTask(ENamedThreads::GameThread, [WBPPath, OldName, NewName, Callback]()
	{
		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
		if (!IsValid(WBP) || !IsValid(WBP->WidgetTree))
		{
			Callback(JsonError(404, FString::Printf(TEXT("Widget Blueprint not found: %s"), *WBPPath)));
			return;
		}

		UWidget* W = WBP->WidgetTree->FindWidget(FName(*OldName));
		if (!IsValid(W))
		{
			Callback(JsonError(404, FString::Printf(TEXT("Widget '%s' not found"), *OldName)));
			return;
		}

		if (UWidget* Existing = WBP->WidgetTree->FindWidget(FName(*NewName)))
		{
			if (Existing != W)
			{
				Callback(JsonError(400, FString::Printf(TEXT("Widget '%s' already exists"), *NewName)));
				return;
			}
		}

		const FName OldFName = W->GetFName();
		W->Rename(*NewName, W->GetOuter(), REN_DontCreateRedirectors);

		FGuid OldGuid;
		if (FGuid* Found = WBP->WidgetVariableNameToGuidMap.Find(OldFName))
		{
			OldGuid = *Found;
		}
		else
		{
			OldGuid = FGuid::NewGuid();
		}
		WBP->WidgetVariableNameToGuidMap.Remove(OldFName);
		WBP->WidgetVariableNameToGuidMap.Add(W->GetFName(), OldGuid);

		NGGWidgetPriv::MarkWBPDirtyAndModified(WBP);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),          true);
		Result->SetStringField(TEXT("widget_blueprint"), WBPPath);
		Result->SetStringField(TEXT("old_name"),         OldName);
		Result->SetStringField(TEXT("new_name"),         W->GetFName().ToString());

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/compile_widget_blueprint
// Body: { "widget_blueprint" }
// ============================================================================
bool FNGGHttpServer::HandleCompileWidgetBlueprint(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString WBPPath;
	Body->TryGetStringField(TEXT("widget_blueprint"), WBPPath);
	if (WBPPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("widget_blueprint is required")));
		return true;
	}

	UE_LOG(LogNGGBridge, Log, TEXT("HandleCompileWidgetBlueprint: '%s'"), *WBPPath);

	AsyncTask(ENamedThreads::GameThread, [WBPPath, Callback]()
	{
		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
		if (!IsValid(WBP))
		{
			Callback(JsonError(404, FString::Printf(TEXT("Widget Blueprint not found: %s"), *WBPPath)));
			return;
		}

		// Defensive pre-compile: walk the live widget tree and reconcile
		// WidgetVariableNameToGuidMap so it has exactly one entry per live
		// widget.  Without this a previous rename/remove path that left the
		// map drifted (orphan entries or missing entries) can crash the
		// widget compiler in ValidateAndFixUpVariableGuids.
		if (IsValid(WBP->WidgetTree))
		{
			TSet<FName> LiveNames;
			WBP->WidgetTree->ForEachWidget([&LiveNames](UWidget* W)
			{
				if (IsValid(W))
				{
					LiveNames.Add(W->GetFName());
				}
			});

			// Prune orphan entries (FNames not present in the tree any more).
			TArray<FName> Orphans;
			for (const TPair<FName, FGuid>& Pair : WBP->WidgetVariableNameToGuidMap)
			{
				if (!LiveNames.Contains(Pair.Key))
				{
					Orphans.Add(Pair.Key);
				}
			}
			for (const FName& N : Orphans)
			{
				WBP->WidgetVariableNameToGuidMap.Remove(N);
			}

			// Ensure every live widget has a GUID entry.
			WBP->WidgetTree->ForEachWidget([WBP](UWidget* W)
			{
				if (IsValid(W) && !WBP->WidgetVariableNameToGuidMap.Contains(W->GetFName()))
				{
					WBP->WidgetVariableNameToGuidMap.Add(W->GetFName(), FGuid::NewGuid());
				}
			});
		}

		FKismetEditorUtilities::CompileBlueprint(WBP);

		FString StatusStr;
		switch (WBP->Status)
		{
		case BS_UpToDate:                  StatusStr = TEXT("UpToDate");                break;
		case BS_UpToDateWithWarnings:      StatusStr = TEXT("UpToDateWithWarnings");    break;
		case BS_Error:                     StatusStr = TEXT("Error");                   break;
		case BS_Dirty:                     StatusStr = TEXT("Dirty");                   break;
		case BS_Unknown:                   StatusStr = TEXT("Unknown");                 break;
		default:                           StatusStr = TEXT("Unknown");                 break;
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),          WBP->Status != BS_Error);
		Result->SetStringField(TEXT("widget_blueprint"), WBPPath);
		Result->SetStringField(TEXT("status"),           StatusStr);
		Result->SetArrayField (TEXT("warnings"),         TArray<TSharedPtr<FJsonValue>>());
		Result->SetArrayField (TEXT("errors"),           TArray<TSharedPtr<FJsonValue>>());

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/add_widget_to_blueprint
//
// Adds one or more UMG widgets to an existing Widget Blueprint's widget tree.
// Widgets are appended to the existing hierarchy — does NOT clear existing widgets.
//
// Request body:
// {
//   "widget_blueprint": "/Game/MyGame/UI/WBP_Settings",
//   "widgets": [
//     {
//       "type":   "Slider",           // Button|TextBlock|Image|ProgressBar|EditableTextBox|
//                                     // ComboBoxString|Slider|VerticalBox|HorizontalBox|CanvasPanel
//       "name":   "MasterVolumeSlider",
//       "parent": "VerticalBox_Root"  // optional — name of an already-existing parent widget
//                                     // (can be a widget added earlier in this same request)
//     }
//   ]
// }
//
// Returns: { success, widget_blueprint, widgets_added, widgets_skipped }
// ============================================================================

bool FNGGHttpServer::HandleAddWidgetToBlueprint(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString WBPPath;
	Body->TryGetStringField(TEXT("widget_blueprint"), WBPPath);
	if (WBPPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("widget_blueprint is required")));
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> WidgetsJson;
	if (Body->HasField(TEXT("widgets")))
	{
		const TArray<TSharedPtr<FJsonValue>>* WidgetsPtr = nullptr;
		if (!Body->TryGetArrayField(TEXT("widgets"), WidgetsPtr) || !WidgetsPtr)
		{
			Callback(JsonError(400, TEXT("'widgets' must be an array")));
			return true;
		}
		WidgetsJson = *WidgetsPtr;
	}

	if (WidgetsJson.Num() == 0)
	{
		Callback(JsonError(400, TEXT("widgets array is required and must not be empty")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [WBPPath, WidgetsJson, Callback]()
	{
		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
		if (!IsValid(WBP))
		{
			Callback(JsonError(404, FString::Printf(TEXT("Widget Blueprint not found: %s"), *WBPPath)));
			return;
		}
		if (!IsValid(WBP->WidgetTree))
		{
			Callback(JsonError(500, TEXT("Widget Blueprint has no WidgetTree")));
			return;
		}

		// Map widget type string -> UClass*
		auto ResolveWidgetClass = [](const FString& TypeName) -> UClass*
		{
			if (TypeName == TEXT("Button"))          return UButton::StaticClass();
			if (TypeName == TEXT("TextBlock"))        return UTextBlock::StaticClass();
			if (TypeName == TEXT("ComboBoxString"))   return UComboBoxString::StaticClass();
			if (TypeName == TEXT("EditableTextBox"))  return UEditableTextBox::StaticClass();
			if (TypeName == TEXT("ProgressBar"))      return UProgressBar::StaticClass();
			if (TypeName == TEXT("Image"))            return UImage::StaticClass();
			if (TypeName == TEXT("Slider"))           return USlider::StaticClass();
			if (TypeName == TEXT("VerticalBox"))      return UVerticalBox::StaticClass();
			if (TypeName == TEXT("HorizontalBox"))    return UHorizontalBox::StaticClass();
			if (TypeName == TEXT("CanvasPanel"))      return UCanvasPanel::StaticClass();
			return UTextBlock::StaticClass(); // safe fallback
		};

		// Build a map of all EXISTING widgets so new widgets can parent to them
		TMap<FString, UWidget*> NameToWidget;
		WBP->WidgetTree->ForEachWidget([&NameToWidget](UWidget* W)
		{
			if (W) { NameToWidget.Add(W->GetFName().ToString(), W); }
		});

		// Ensure there's a root canvas panel — create one if missing
		UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget);
		if (!RootCanvas)
		{
			// If root exists but is not a canvas, we still use it as fallback
			// Create a new canvas and set it as root if there's nothing
			if (!IsValid(WBP->WidgetTree->RootWidget))
			{
				RootCanvas = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(
					UCanvasPanel::StaticClass(), TEXT("CanvasPanel_Root"));
				WBP->WidgetTree->RootWidget = RootCanvas;
				NameToWidget.Add(TEXT("CanvasPanel_Root"), RootCanvas);
				// UE5.4+ compiler ensures every named widget has a GUID entry.
				if (RootCanvas) { WBP->WidgetVariableNameToGuidMap.Add(RootCanvas->GetFName(), FGuid::NewGuid()); }
			}
		}

		// Self-heal: existing widgets in the tree without GUIDs would trip
		// the WidgetBlueprintCompiler ensure on next compile. Common when the
		// WBP was created by older plugin builds.
		WBP->WidgetTree->ForEachWidget([WBP](UWidget* W)
		{
			if (W && !WBP->WidgetVariableNameToGuidMap.Contains(W->GetFName()))
			{
				WBP->WidgetVariableNameToGuidMap.Add(W->GetFName(), FGuid::NewGuid());
			}
		});

		int32 Added = 0, Skipped = 0;

		for (const TSharedPtr<FJsonValue>& Entry : WidgetsJson)
		{
			const TSharedPtr<FJsonObject>* EntryObj;
			if (!Entry->TryGetObject(EntryObj)) { ++Skipped; continue; }

			FString TypeName, WidgetName, ParentName;
			(*EntryObj)->TryGetStringField(TEXT("type"),   TypeName);
			(*EntryObj)->TryGetStringField(TEXT("name"),   WidgetName);
			(*EntryObj)->TryGetStringField(TEXT("parent"), ParentName);

			if (WidgetName.IsEmpty()) { ++Skipped; continue; }

			// Skip if widget with this name already exists
			if (NameToWidget.Contains(WidgetName)) { ++Skipped; continue; }

			UClass* WidgetClass = ResolveWidgetClass(TypeName);
			UWidget* W = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, *WidgetName);
			if (!W) { ++Skipped; continue; }
			WBP->WidgetVariableNameToGuidMap.Add(W->GetFName(), FGuid::NewGuid());

			NameToWidget.Add(WidgetName, W);

			// Find parent panel
			UWidget** ParentPtr = ParentName.IsEmpty() ? nullptr : NameToWidget.Find(ParentName);
			UPanelWidget* ParentPanel = ParentPtr ? Cast<UPanelWidget>(*ParentPtr) : nullptr;

			if (ParentPanel)
			{
				ParentPanel->AddChild(W);
			}
			else if (RootCanvas)
			{
				UCanvasPanelSlot* Slot = RootCanvas->AddChildToCanvas(W);
				if (Slot) Slot->SetAutoSize(true);
			}
			else if (UPanelWidget* RootPanel = Cast<UPanelWidget>(WBP->WidgetTree->RootWidget))
			{
				RootPanel->AddChild(W);
			}

			++Added;
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
		WBP->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),          true);
		Result->SetStringField(TEXT("widget_blueprint"), WBPPath);
		Result->SetNumberField(TEXT("widgets_added"),    static_cast<double>(Added));
		Result->SetNumberField(TEXT("widgets_skipped"),  static_cast<double>(Skipped));

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(JsonOk(BodyStr));
	});

	return true;
}
