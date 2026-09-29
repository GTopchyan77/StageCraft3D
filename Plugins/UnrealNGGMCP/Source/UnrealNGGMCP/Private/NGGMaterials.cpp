// Copyright 2025-2026 NGG. All Rights Reserved.
//
// NGGMaterials.cpp
//
// Implementation of the material and curve endpoints:
//   /materials/{create,create_post_process,create_instance,read}
//   /curves/{create_float,create_color,read}
//
// Material graphs are built through UMaterialEditingLibrary so the expressions
// land in the asset the same way the editor would place them, and the material
// is recompiled before the response goes out.

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
#include "EngineUtils.h"          // TActorIterator
#include "Kismet2/BlueprintEditorUtils.h"
// ---- Material creation -----------------------------------------------------
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionSceneTexture.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionFrac.h"
#include "Materials/MaterialExpressionRound.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionIf.h"
#include "MaterialEditingLibrary.h"
#include "Factories/MaterialFactoryNew.h"
#include "Materials/MaterialInstance.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/Scene.h"
// ---- Curves ----------------------------------------------------------------
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveFloat.h"
// ---- Assigning a new material onto a Blueprint's components ----------------
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
// ---- UObject reflection ----------------------------------------------------
#include "UObject/PropertyIterator.h"



// ============================================================================
// Handler: POST /editor/create_material
// Body: { "asset_path": "/Game/Materials/M_Green", "base_color": {"r":0,"g":1,"b":0}, "actor_label": "Cube15" }
// ============================================================================

bool FNGGHttpServer::HandleCreateMaterial(
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

	FString AssetPath; Body->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("asset_path is required")));
		return true;
	}

	// Base color (linear space). Defaults: warm wooden-crate brown.
	float R = 0.35f, G = 0.12f, B = 0.03f;
	{
		const TSharedPtr<FJsonObject>* ColorObj;
		if (Body->TryGetObjectField(TEXT("base_color"), ColorObj))
		{
			double Rd, Gd, Bd;
			if ((*ColorObj)->TryGetNumberField(TEXT("r"), Rd)) R = (float)Rd;
			if ((*ColorObj)->TryGetNumberField(TEXT("g"), Gd)) G = (float)Gd;
			if ((*ColorObj)->TryGetNumberField(TEXT("b"), Bd)) B = (float)Bd;
		}
	}

	// pixel_art:true  → adds UV-border HLSL so each face shows a dark outline
	bool bPixelArt = false;
	Body->TryGetBoolField(TEXT("pixel_art"), bPixelArt);

	// use_parameters:true → creates VectorParameter("BaseColor") and
	// ScalarParameter("BrightnessMultiplier") with graph: (BaseColor * Brightness) → BaseColor pin
	bool bUseParameters = false;
	Body->TryGetBoolField(TEXT("use_parameters"), bUseParameters);

	// texture_path: optional content path to a texture (e.g. "/Game/Textures/T_PixelCrate").
	// When set, creates a TextureSample node multiplied by BaseColor VectorParameter
	// so hover tinting still works. Implies use_parameters=true.
	FString TexturePath;
	Body->TryGetStringField(TEXT("texture_path"), TexturePath);
	if (!TexturePath.IsEmpty()) bUseParameters = true;

	// unlit:true → route the color to Emissive Color and set Shading Model = Unlit
	// so the material renders the exact authored color, independent of scene
	// lighting (ideal for cables, holograms, UI-like glows, debug viz).
	bool bUnlit = false;
	Body->TryGetBoolField(TEXT("unlit"), bUnlit);

	// Optional scalar defaults
	float BrightnessDefault = 1.0f;
	{
		double Bd;
		if (Body->TryGetNumberField(TEXT("brightness_default"), Bd)) BrightnessDefault = (float)Bd;
	}

	// Optional: also apply the new material to a Blueprint CDO component
	FString BlueprintPath, ComponentName;
	Body->TryGetStringField(TEXT("blueprint_path"),  BlueprintPath);
	Body->TryGetStringField(TEXT("component_name"),  ComponentName);

	FString ActorLabel;
	Body->TryGetStringField(TEXT("actor_label"), ActorLabel);

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, R, G, B, bPixelArt, bUseParameters, bUnlit, BrightnessDefault, BlueprintPath, ComponentName, ActorLabel, TexturePath]()
	{
		FString PackagePath, AssetName;
		AssetPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		PackagePath = AssetPath.Left(AssetPath.Len() - AssetName.Len() - 1);

		UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
		bool bAlreadyExisted = (Mat != nullptr);

		if (!Mat)
		{
			IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
			UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
			UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UMaterial::StaticClass(), Factory);
			Mat = Cast<UMaterial>(NewAsset);
		}

		if (!Mat)
		{
			Callback(JsonError(500, FString::Printf(TEXT("Failed to create material at '%s'"), *AssetPath)));
			return;
		}

		// Clear all existing expressions so we start fresh each time
		Mat->GetExpressionCollection().Empty();

		if (bUseParameters)
		{
			// ── VectorParameter "BaseColor" ─────────────────────────────
			UMaterialExpressionVectorParameter* ColorParam =
				NewObject<UMaterialExpressionVectorParameter>(Mat);
			ColorParam->ParameterName = TEXT("BaseColor");
			ColorParam->DefaultValue  = FLinearColor(R, G, B, 1.f);
			Mat->GetExpressionCollection().AddExpression(ColorParam);

			// ── ScalarParameter "BrightnessMultiplier" ──────────────────
			UMaterialExpressionScalarParameter* BrightParam =
				NewObject<UMaterialExpressionScalarParameter>(Mat);
			BrightParam->ParameterName = TEXT("BrightnessMultiplier");
			BrightParam->DefaultValue  = BrightnessDefault;
			Mat->GetExpressionCollection().AddExpression(BrightParam);

			// ── Optional TextureSample node ─────────────────────────────
			UMaterialExpression* ColorSource = ColorParam; // default: just the parameter
			if (!TexturePath.IsEmpty())
			{
				UTexture* Tex = LoadObject<UTexture>(nullptr, *TexturePath);
				if (Tex)
				{
					UMaterialExpressionTextureSample* TexSample =
						NewObject<UMaterialExpressionTextureSample>(Mat);
					TexSample->Texture = Tex;
					TexSample->SamplerType = SAMPLERTYPE_Color;
					Mat->GetExpressionCollection().AddExpression(TexSample);

					// Multiply texture by BaseColor so hover tinting works
					UMaterialExpressionMultiply* TexColorMul =
						NewObject<UMaterialExpressionMultiply>(Mat);
					TexColorMul->A.Expression = TexSample;
					TexColorMul->B.Expression = ColorParam;
					Mat->GetExpressionCollection().AddExpression(TexColorMul);

					ColorSource = TexColorMul;
				}
			}

			// ── Multiply node: ColorSource * BrightnessMultiplier ───────
			UMaterialExpressionMultiply* MulExpr =
				NewObject<UMaterialExpressionMultiply>(Mat);
			MulExpr->A.Expression = ColorSource;
			MulExpr->B.Expression = BrightParam;
			Mat->GetExpressionCollection().AddExpression(MulExpr);

			// Wire Multiply output → material Base Color pin
			Mat->GetEditorOnlyData()->BaseColor.Expression  = MulExpr;
			Mat->GetEditorOnlyData()->BaseColor.OutputIndex = 0;

			// Roughness = 1 (matte — no specular glare on pixel art boxes)
			UMaterialExpressionConstant* RoughExp =
				NewObject<UMaterialExpressionConstant>(Mat);
			RoughExp->R = 1.0f;
			Mat->GetExpressionCollection().AddExpression(RoughExp);
			Mat->GetEditorOnlyData()->Roughness.Expression = RoughExp;
		}
		else if (bPixelArt)
		{
			// ── TextureCoordinate ───────────────────────────────────────
			UMaterialExpressionTextureCoordinate* UVExp =
				NewObject<UMaterialExpressionTextureCoordinate>(Mat);
			UVExp->UTiling = 1.0f;
			UVExp->VTiling = 1.0f;
			Mat->GetExpressionCollection().AddExpression(UVExp);

			// ── Custom HLSL: pixel-art border on every face ─────────────
			// frac(UV) gives 0→1 per face.  step(border, frac) creates a
			// mask that is 1 inside the face and 0 at the outer edge strip.
			UMaterialExpressionCustom* CustomExp =
				NewObject<UMaterialExpressionCustom>(Mat);
			CustomExp->Code = FString::Printf(TEXT(
				"float2 f = frac(TexCoord);\n"
				"float2 m = step(0.07, f) * step(0.07, 1.0 - f);\n"
				"float mask = m.x * m.y;\n"
				"float3 body = float3(%.5f, %.5f, %.5f);\n"
				"float3 edge = body * 0.12;\n"
				"return lerp(edge, body, mask);\n"
			), R, G, B);
			CustomExp->OutputType = CMOT_Float3;
			CustomExp->Description = TEXT("PixelArtBorder");

			FCustomInput UVInput;
			UVInput.InputName = TEXT("TexCoord");
			UVInput.Input.Expression  = UVExp;
			UVInput.Input.OutputIndex = 0;
			CustomExp->Inputs.Add(UVInput);

			Mat->GetExpressionCollection().AddExpression(CustomExp);
			Mat->GetEditorOnlyData()->BaseColor.Expression  = CustomExp;
			Mat->GetEditorOnlyData()->BaseColor.OutputIndex = 0;

			// Roughness = 1 (matte — no specular glare on pixel art)
			UMaterialExpressionConstant* RoughExp =
				NewObject<UMaterialExpressionConstant>(Mat);
			RoughExp->R = 1.0f;
			Mat->GetExpressionCollection().AddExpression(RoughExp);
			Mat->GetEditorOnlyData()->Roughness.Expression = RoughExp;
		}
		else
		{
			UMaterialExpressionConstant3Vector* ColorExpr =
				NewObject<UMaterialExpressionConstant3Vector>(Mat);
			ColorExpr->Constant = FLinearColor(R, G, B, 1.f);
			Mat->GetExpressionCollection().AddExpression(ColorExpr);
			Mat->GetEditorOnlyData()->BaseColor.Expression = ColorExpr;
		}

		// Unlit: drive Emissive Color with whatever feeds Base Color, then switch
		// the shading model so the color is shown verbatim (no lighting applied).
		if (bUnlit)
		{
			UMaterialEditorOnlyData* EOD = Mat->GetEditorOnlyData();
			EOD->EmissiveColor.Expression  = EOD->BaseColor.Expression;
			EOD->EmissiveColor.OutputIndex = EOD->BaseColor.OutputIndex;
			Mat->SetShadingModel(MSM_Unlit);
		}

		UMaterialEditingLibrary::RecompileMaterial(Mat);
		Mat->MarkPackageDirty();

		// ── Optional: apply to a Blueprint component ────────────────────
		// SCS-first lookup (components added via add_component_to_blueprint live
		// in BP->SimpleConstructionScript and are NOT direct FObjectProperty
		// fields on the CDO). Fall through to the FObjectProperty loop only for
		// inherited / native C++ components.
		bool bAppliedToBlueprint = false;
		if (!BlueprintPath.IsEmpty())
		{
			UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
			if (BP && BP->GeneratedClass)
			{
				const FString CompName = ComponentName.IsEmpty() ? TEXT("StaticMeshComponent0") : ComponentName;

				// --- Path 1: SCS-owned component ---
				if (BP->SimpleConstructionScript)
				{
					for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
					{
						if (Node && Node->ComponentTemplate &&
							Node->GetVariableName().ToString() == CompName)
						{
							if (UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Node->ComponentTemplate))
							{
								SMC->SetMaterial(0, Mat);
								bAppliedToBlueprint = true;
							}
							break;
						}
					}
				}

				// --- Path 2: Inherited / native C++ component on the CDO ---
				if (!bAppliedToBlueprint)
				{
					UObject* CDO = BP->GeneratedClass->GetDefaultObject();
					if (CDO)
					{
						for (TFieldIterator<FObjectProperty> PropIt(CDO->GetClass()); PropIt; ++PropIt)
						{
							if (PropIt->GetName() == CompName || PropIt->GetFName() == *CompName)
							{
								UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(PropIt->GetObjectPropertyValue_InContainer(CDO));
								if (SMC)
								{
									SMC->SetMaterial(0, Mat);
									bAppliedToBlueprint = true;
								}
								break;
							}
						}
					}
				}

				if (bAppliedToBlueprint)
				{
					FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
					if (UPackage* BpPkg = BP->GetOutermost())
					{
						BpPkg->MarkPackageDirty();
					}
				}
			}
		}

		// ── Optional: apply to a level actor ────────────────────────────
		bool bAppliedToActor = false;
		if (!ActorLabel.IsEmpty())
		{
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (World)
			{
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (IsValid(*It) && It->GetActorLabel() == ActorLabel)
					{
						UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(
							It->FindComponentByClass(UStaticMeshComponent::StaticClass()));
						if (SMC)
						{
							SMC->SetMaterial(0, Mat);
							It->ReregisterAllComponents();
							It->MarkPackageDirty();
							bAppliedToActor = true;
						}
						break;
					}
				}
			}
		}

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),               true);
		Resp->SetStringField(TEXT("asset_path"),            AssetPath);
		Resp->SetBoolField  (TEXT("already_existed"),       bAlreadyExisted);
		Resp->SetBoolField  (TEXT("pixel_art"),             bPixelArt);
		Resp->SetBoolField  (TEXT("applied_to_blueprint"),  bAppliedToBlueprint);
		Resp->SetBoolField  (TEXT("applied_to_actor"),      bAppliedToActor);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonCreated(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/create_color_curve
// Body: { "asset_path": "/Game/.../Curve_X",
//         "keys": [ { "time": 0.0, "r": 0, "g": 1, "b": 0 }, ... ],
//         "linear": true }   // linear (default) or constant (stepped) interp
// Creates / overwrites a UCurveLinearColor with the given RGB keys (alpha = 1).
// ============================================================================

bool FNGGHttpServer::HandleCreateColorCurve(
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

	FString AssetPath; Body->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("asset_path is required")));
		return true;
	}

	bool bLinear = true;
	Body->TryGetBoolField(TEXT("linear"), bLinear);

	struct FColorKeyIn { float Time = 0.f, R = 0.f, G = 0.f, B = 0.f; };
	TArray<FColorKeyIn> Keys;
	const TArray<TSharedPtr<FJsonValue>>* KeysArr = nullptr;
	if (Body->TryGetArrayField(TEXT("keys"), KeysArr))
	{
		for (const TSharedPtr<FJsonValue>& V : *KeysArr)
		{
			const TSharedPtr<FJsonObject>* K = nullptr;
			if (V->TryGetObject(K) && K)
			{
				FColorKeyIn Key;
				double D;
				if ((*K)->TryGetNumberField(TEXT("time"), D)) Key.Time = (float)D;
				if ((*K)->TryGetNumberField(TEXT("r"),    D)) Key.R    = (float)D;
				if ((*K)->TryGetNumberField(TEXT("g"),    D)) Key.G    = (float)D;
				if ((*K)->TryGetNumberField(TEXT("b"),    D)) Key.B    = (float)D;
				Keys.Add(Key);
			}
		}
	}
	if (Keys.Num() == 0)
	{
		Callback(JsonError(400, TEXT("keys[] is required — each { time, r, g, b }")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, Keys, bLinear]()
	{
		FString PackagePath, AssetName;
		AssetPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		PackagePath = AssetPath.Left(AssetPath.Len() - AssetName.Len() - 1);

		UCurveLinearColor* Curve = LoadObject<UCurveLinearColor>(nullptr, *AssetPath);
		const bool bAlreadyExisted = (Curve != nullptr);
		if (!Curve)
		{
			UPackage* Package = CreatePackage(*AssetPath);
			if (!Package)
			{
				Callback(JsonError(500, FString::Printf(TEXT("Failed to create package for '%s'"), *AssetPath)));
				return;
			}
			Curve = NewObject<UCurveLinearColor>(Package, FName(*AssetName),
				RF_Public | RF_Standalone | RF_Transactional);
			FAssetRegistryModule::AssetCreated(Curve);
		}
		if (!Curve)
		{
			Callback(JsonError(500, FString::Printf(TEXT("Failed to create curve at '%s'"), *AssetPath)));
			return;
		}

		// Rebuild all four channel curves (R,G,B,A) from the supplied keys.
		for (int32 c = 0; c < 4; ++c)
		{
			Curve->FloatCurves[c].Reset();
		}
		const ERichCurveInterpMode Interp = bLinear ? RCIM_Linear : RCIM_Constant;
		for (const FColorKeyIn& K : Keys)
		{
			const FKeyHandle HR = Curve->FloatCurves[0].AddKey(K.Time, K.R);
			const FKeyHandle HG = Curve->FloatCurves[1].AddKey(K.Time, K.G);
			const FKeyHandle HB = Curve->FloatCurves[2].AddKey(K.Time, K.B);
			const FKeyHandle HA = Curve->FloatCurves[3].AddKey(K.Time, 1.0f);
			Curve->FloatCurves[0].SetKeyInterpMode(HR, Interp);
			Curve->FloatCurves[1].SetKeyInterpMode(HG, Interp);
			Curve->FloatCurves[2].SetKeyInterpMode(HB, Interp);
			Curve->FloatCurves[3].SetKeyInterpMode(HA, Interp);
		}

		Curve->MarkPackageDirty();

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),         true);
		Resp->SetStringField(TEXT("asset_path"),      AssetPath);
		Resp->SetBoolField  (TEXT("already_existed"), bAlreadyExisted);
		Resp->SetNumberField(TEXT("key_count"),       Keys.Num());

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonCreated(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/create_post_process_material
//
// Builds a UMaterial with MaterialDomain = MD_PostProcess that highlights any
// mesh whose RenderCustomDepthPass is enabled, but only when the mesh is
// occluded by another opaque pixel (i.e. behind a wall). Used by the
// "enemy through walls" outline effect.
//
// Body:
//   {
//     "asset_path": "/Game/Materials/M_EnemyHighlight",
//     "effect": "occlusion_outline",       // optional, only one supported today
//     "highlight_color": {"r":1,"g":0.05,"b":0.05}
//   }
//
// Graph (when effect == "occlusion_outline"):
//   SceneTexture(SceneDepth)        ──┐
//                                     ├─→ Mask.R ─→ NamedRefSceneDepth ─┐
//   SceneTexture(CustomDepth)       ──┘                                  │
//                ↳ Mask.R ─→ NamedRefCustomDepth ─→ Frac ─→ Round ──┐    │
//   SceneTexture(PostProcessInput0)                                 │    │
//                ↳ Mask.RGB ─→ NamedRefOriginalColor                │    │
//                                                                   │    │
//   Lerp(OriginalColor, HighlightColor, Round)                  ←───┘    │
//                                                                        │
//   If(CustomDepth > SceneDepth ? Lerp : OriginalColor) ─→ EmissiveColor─┘
// ============================================================================

bool FNGGHttpServer::HandleCreatePostProcessMaterial(
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

	FString AssetPath; Body->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("asset_path is required")));
		return true;
	}

	FString Effect = TEXT("occlusion_outline");
	Body->TryGetStringField(TEXT("effect"), Effect);
	if (Effect != TEXT("occlusion_outline"))
	{
		Callback(JsonError(400, FString::Printf(
			TEXT("Unknown effect '%s'. Supported: 'occlusion_outline'"), *Effect)));
		return true;
	}

	// Highlight color (linear RGB). Default: bright red.
	float R = 1.0f, G = 0.05f, B = 0.05f;
	{
		const TSharedPtr<FJsonObject>* ColorObj;
		if (Body->TryGetObjectField(TEXT("highlight_color"), ColorObj))
		{
			double Rd, Gd, Bd;
			if ((*ColorObj)->TryGetNumberField(TEXT("r"), Rd)) R = (float)Rd;
			if ((*ColorObj)->TryGetNumberField(TEXT("g"), Gd)) G = (float)Gd;
			if ((*ColorObj)->TryGetNumberField(TEXT("b"), Bd)) B = (float)Bd;
		}
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, Effect, R, G, B]()
	{
		FString PackagePath, AssetName;
		AssetPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		PackagePath = AssetPath.Left(AssetPath.Len() - AssetName.Len() - 1);

		UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
		const bool bAlreadyExisted = (Mat != nullptr);

		if (!Mat)
		{
			IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
			UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
			UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UMaterial::StaticClass(), Factory);
			Mat = Cast<UMaterial>(NewAsset);
		}

		if (!Mat)
		{
			Callback(JsonError(500, FString::Printf(TEXT("Failed to create material at '%s'"), *AssetPath)));
			return;
		}

		// ---- Configure as a PostProcess material ----------------------------
		// Must be set BEFORE wiring expressions so the editor exposes the
		// EmissiveColor pin (the only output for PostProcess materials).
		Mat->MaterialDomain     = MD_PostProcess;
		Mat->BlendableLocation  = BL_SceneColorAfterTonemapping;
		Mat->BlendablePriority  = 0;

		// Clear any existing graph so we get a clean rebuild on every call.
		Mat->GetExpressionCollection().Empty();
		// Disconnect surface-domain pins that may still be wired from a previous build.
		Mat->GetEditorOnlyData()->BaseColor.Expression  = nullptr;
		Mat->GetEditorOnlyData()->Roughness.Expression  = nullptr;
		Mat->GetEditorOnlyData()->Metallic.Expression   = nullptr;
		Mat->GetEditorOnlyData()->Specular.Expression   = nullptr;
		Mat->GetEditorOnlyData()->EmissiveColor.Expression = nullptr;

		// ---- SceneTexture nodes ---------------------------------------------
		UMaterialExpressionSceneTexture* StSceneDepth =
			NewObject<UMaterialExpressionSceneTexture>(Mat);
		StSceneDepth->SceneTextureId = PPI_SceneDepth;
		StSceneDepth->MaterialExpressionEditorX = -1200;
		StSceneDepth->MaterialExpressionEditorY = -200;
		Mat->GetExpressionCollection().AddExpression(StSceneDepth);

		UMaterialExpressionSceneTexture* StCustomDepth =
			NewObject<UMaterialExpressionSceneTexture>(Mat);
		StCustomDepth->SceneTextureId = PPI_CustomDepth;
		StCustomDepth->MaterialExpressionEditorX = -1200;
		StCustomDepth->MaterialExpressionEditorY = 100;
		Mat->GetExpressionCollection().AddExpression(StCustomDepth);

		UMaterialExpressionSceneTexture* StColor =
			NewObject<UMaterialExpressionSceneTexture>(Mat);
		StColor->SceneTextureId = PPI_PostProcessInput0;
		StColor->MaterialExpressionEditorX = -1200;
		StColor->MaterialExpressionEditorY = 400;
		Mat->GetExpressionCollection().AddExpression(StColor);

		// ---- Component masks ------------------------------------------------
		UMaterialExpressionComponentMask* MaskSceneDepthR =
			NewObject<UMaterialExpressionComponentMask>(Mat);
		MaskSceneDepthR->R = 1; MaskSceneDepthR->G = 0; MaskSceneDepthR->B = 0; MaskSceneDepthR->A = 0;
		MaskSceneDepthR->Input.Expression  = StSceneDepth;
		MaskSceneDepthR->Input.OutputIndex = 0;
		MaskSceneDepthR->MaterialExpressionEditorX = -900;
		MaskSceneDepthR->MaterialExpressionEditorY = -200;
		Mat->GetExpressionCollection().AddExpression(MaskSceneDepthR);

		UMaterialExpressionComponentMask* MaskCustomDepthR =
			NewObject<UMaterialExpressionComponentMask>(Mat);
		MaskCustomDepthR->R = 1; MaskCustomDepthR->G = 0; MaskCustomDepthR->B = 0; MaskCustomDepthR->A = 0;
		MaskCustomDepthR->Input.Expression  = StCustomDepth;
		MaskCustomDepthR->Input.OutputIndex = 0;
		MaskCustomDepthR->MaterialExpressionEditorX = -900;
		MaskCustomDepthR->MaterialExpressionEditorY = 100;
		Mat->GetExpressionCollection().AddExpression(MaskCustomDepthR);

		UMaterialExpressionComponentMask* MaskColorRGB =
			NewObject<UMaterialExpressionComponentMask>(Mat);
		MaskColorRGB->R = 1; MaskColorRGB->G = 1; MaskColorRGB->B = 1; MaskColorRGB->A = 0;
		MaskColorRGB->Input.Expression  = StColor;
		MaskColorRGB->Input.OutputIndex = 0;
		MaskColorRGB->MaterialExpressionEditorX = -900;
		MaskColorRGB->MaterialExpressionEditorY = 400;
		Mat->GetExpressionCollection().AddExpression(MaskColorRGB);

		// ---- Frac → Round on custom depth -----------------------------------
		// Pixels with NO custom depth report a huge integer (~1e10) — frac() is
		// 0 there, round() stays 0. Pixels with custom depth almost always have
		// some fractional component → frac() > 0, round() = 1. This becomes the
		// lerp alpha (0 = original color, 1 = highlight color).
		UMaterialExpressionFrac* FracExpr = NewObject<UMaterialExpressionFrac>(Mat);
		FracExpr->Input.Expression  = MaskCustomDepthR;
		FracExpr->Input.OutputIndex = 0;
		FracExpr->MaterialExpressionEditorX = -600;
		FracExpr->MaterialExpressionEditorY = 100;
		Mat->GetExpressionCollection().AddExpression(FracExpr);

		UMaterialExpressionRound* RoundExpr = NewObject<UMaterialExpressionRound>(Mat);
		RoundExpr->Input.Expression  = FracExpr;
		RoundExpr->Input.OutputIndex = 0;
		RoundExpr->MaterialExpressionEditorX = -400;
		RoundExpr->MaterialExpressionEditorY = 100;
		Mat->GetExpressionCollection().AddExpression(RoundExpr);

		// ---- Highlight color constant ---------------------------------------
		UMaterialExpressionConstant3Vector* HighlightExpr =
			NewObject<UMaterialExpressionConstant3Vector>(Mat);
		HighlightExpr->Constant = FLinearColor(R, G, B, 1.f);
		HighlightExpr->MaterialExpressionEditorX = -400;
		HighlightExpr->MaterialExpressionEditorY = 300;
		Mat->GetExpressionCollection().AddExpression(HighlightExpr);

		// ---- Lerp(OriginalColor, HighlightColor, RoundAlpha) ----------------
		UMaterialExpressionLinearInterpolate* LerpExpr =
			NewObject<UMaterialExpressionLinearInterpolate>(Mat);
		LerpExpr->A.Expression       = MaskColorRGB;
		LerpExpr->A.OutputIndex      = 0;
		LerpExpr->B.Expression       = HighlightExpr;
		LerpExpr->B.OutputIndex      = 0;
		LerpExpr->Alpha.Expression   = RoundExpr;
		LerpExpr->Alpha.OutputIndex  = 0;
		LerpExpr->MaterialExpressionEditorX = -150;
		LerpExpr->MaterialExpressionEditorY = 200;
		Mat->GetExpressionCollection().AddExpression(LerpExpr);

		// ---- If (CustomDepth.R > SceneDepth.R) ? Lerp : OriginalColor -------
		// Equal/less-than branches return the original color so pixels in front
		// of (or co-located with) opaque geometry render normally — the high­
		// light only kicks in when the custom-depth object is *behind* something.
		UMaterialExpressionIf* IfExpr = NewObject<UMaterialExpressionIf>(Mat);
		IfExpr->A.Expression               = MaskCustomDepthR;
		IfExpr->A.OutputIndex              = 0;
		IfExpr->B.Expression               = MaskSceneDepthR;
		IfExpr->B.OutputIndex              = 0;
		IfExpr->AGreaterThanB.Expression   = LerpExpr;
		IfExpr->AGreaterThanB.OutputIndex  = 0;
		IfExpr->AEqualsB.Expression        = MaskColorRGB;
		IfExpr->AEqualsB.OutputIndex       = 0;
		IfExpr->ALessThanB.Expression      = MaskColorRGB;
		IfExpr->ALessThanB.OutputIndex     = 0;
		IfExpr->MaterialExpressionEditorX  = 100;
		IfExpr->MaterialExpressionEditorY  = 100;
		Mat->GetExpressionCollection().AddExpression(IfExpr);

		// ---- Wire If output → Material EmissiveColor ------------------------
		Mat->GetEditorOnlyData()->EmissiveColor.Expression  = IfExpr;
		Mat->GetEditorOnlyData()->EmissiveColor.OutputIndex = 0;

		// ---- Recompile + persist --------------------------------------------
		UMaterialEditingLibrary::RecompileMaterial(Mat);
		Mat->MarkPackageDirty();

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),         true);
		Resp->SetStringField(TEXT("asset_path"),      AssetPath);
		Resp->SetBoolField  (TEXT("already_existed"), bAlreadyExisted);
		Resp->SetStringField(TEXT("effect"),          Effect);
		Resp->SetStringField(TEXT("material_domain"), TEXT("PostProcess"));

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonCreated(RespBody));
	});

	return true;
}

// ---------------------------------------------------------------------------
// POST /editor/create_material_instance
// ---------------------------------------------------------------------------
bool FNGGHttpServer::HandleCreateMaterialInstance(
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

	FString AssetPath;     Body->TryGetStringField(TEXT("asset_path"),      AssetPath);
	FString ParentMatPath; Body->TryGetStringField(TEXT("parent_material"), ParentMatPath);

	if (AssetPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("asset_path is required")));
		return true;
	}
	if (ParentMatPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("parent_material is required")));
		return true;
	}

	// Parse scalar parameter overrides
	struct FScalarParam { FString Name; float Value; };
	TArray<FScalarParam> ScalarParams;
	{
		const TArray<TSharedPtr<FJsonValue>>* ScalarArr;
		if (Body->TryGetArrayField(TEXT("scalar_params"), ScalarArr))
		{
			for (const auto& Elem : *ScalarArr)
			{
				auto Obj = Elem->AsObject();
				if (Obj.IsValid())
				{
					FScalarParam P;
					// TryGet* tolerates missing/wrong-typed sub-fields instead
					// of crashing in GetStringField/GetNumberField.
					Obj->TryGetStringField(TEXT("name"), P.Name);
					double Value = 0.0;
					Obj->TryGetNumberField(TEXT("value"), Value);
					P.Value = static_cast<float>(Value);
					ScalarParams.Add(P);
				}
			}
		}
	}

	// Parse vector parameter overrides
	struct FVectorParam { FString Name; FLinearColor Value; };
	TArray<FVectorParam> VectorParams;
	{
		const TArray<TSharedPtr<FJsonValue>>* VectorArr;
		if (Body->TryGetArrayField(TEXT("vector_params"), VectorArr))
		{
			for (const auto& Elem : *VectorArr)
			{
				auto Obj = Elem->AsObject();
				if (Obj.IsValid())
				{
					FVectorParam P;
					Obj->TryGetStringField(TEXT("name"), P.Name);
					const TSharedPtr<FJsonObject>* ValObj;
					if (Obj->TryGetObjectField(TEXT("value"), ValObj))
					{
						double Rd = 0, Gd = 0, Bd = 0, Ad = 1;
						(*ValObj)->TryGetNumberField(TEXT("r"), Rd);
						(*ValObj)->TryGetNumberField(TEXT("g"), Gd);
						(*ValObj)->TryGetNumberField(TEXT("b"), Bd);
						(*ValObj)->TryGetNumberField(TEXT("a"), Ad);
						P.Value = FLinearColor((float)Rd, (float)Gd, (float)Bd, (float)Ad);
					}
					VectorParams.Add(P);
				}
			}
		}
	}

	// Parse texture parameter overrides ({ name, texture_path })
	struct FTextureParam { FString Name; FString TexturePath; };
	TArray<FTextureParam> TextureParams;
	{
		const TArray<TSharedPtr<FJsonValue>>* TexArr;
		if (Body->TryGetArrayField(TEXT("texture_params"), TexArr))
		{
			for (const auto& Elem : *TexArr)
			{
				auto Obj = Elem->AsObject();
				if (Obj.IsValid())
				{
					FTextureParam P;
					Obj->TryGetStringField(TEXT("name"), P.Name);
					Obj->TryGetStringField(TEXT("texture_path"), P.TexturePath);
					if (!P.Name.IsEmpty() && !P.TexturePath.IsEmpty())
					{
						TextureParams.Add(P);
					}
				}
			}
		}
	}

	FString BlueprintPath, ComponentName;
	Body->TryGetStringField(TEXT("blueprint_path"), BlueprintPath);
	Body->TryGetStringField(TEXT("component_name"), ComponentName);

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, ParentMatPath, ScalarParams, VectorParams, TextureParams, BlueprintPath, ComponentName]()
	{
		// Load parent material
		UMaterialInterface* ParentMat = LoadObject<UMaterialInterface>(nullptr, *ParentMatPath);
		if (!ParentMat)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Parent material not found: '%s'"), *ParentMatPath)));
			return;
		}

		FString PackagePath, AssetName;
		AssetPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		PackagePath = AssetPath.Left(AssetPath.Len() - AssetName.Len() - 1);

		// Load existing or create new MI
		UMaterialInstanceConstant* MI = LoadObject<UMaterialInstanceConstant>(nullptr, *AssetPath);
		bool bAlreadyExisted = (MI != nullptr);

		if (!MI)
		{
			IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
			UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
			Factory->InitialParent = ParentMat;
			UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UMaterialInstanceConstant::StaticClass(), Factory);
			MI = Cast<UMaterialInstanceConstant>(NewAsset);
		}

		if (!MI)
		{
			Callback(JsonError(500, FString::Printf(TEXT("Failed to create material instance at '%s'"), *AssetPath)));
			return;
		}

		// Update parent if it changed
		if (MI->Parent != ParentMat)
		{
			MI->SetParentEditorOnly(ParentMat);
		}

		// Apply scalar parameter overrides
		for (const auto& P : ScalarParams)
		{
			MI->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(*P.Name), P.Value);
		}

		// Apply vector parameter overrides
		for (const auto& P : VectorParams)
		{
			MI->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(*P.Name), P.Value);
		}

		// Apply texture parameter overrides
		int32 TextureParamsSet = 0;
		for (const auto& P : TextureParams)
		{
			UTexture* Tex = LoadObject<UTexture>(nullptr, *P.TexturePath);
			if (Tex)
			{
				MI->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(*P.Name), Tex);
				++TextureParamsSet;
			}
		}

		MI->PostEditChange();
		MI->MarkPackageDirty();

		// Optional: apply MI to a Blueprint CDO component
		bool bAppliedToBlueprint = false;
		if (!BlueprintPath.IsEmpty())
		{
			UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
			if (BP && BP->GeneratedClass)
			{
				UObject* CDO = BP->GeneratedClass->GetDefaultObject();
				if (CDO)
				{
					const FString CompName = ComponentName.IsEmpty() ? TEXT("StaticMeshComponent0") : ComponentName;
					for (TFieldIterator<FObjectProperty> PropIt(CDO->GetClass()); PropIt; ++PropIt)
					{
						if (PropIt->GetName() == CompName || PropIt->GetFName() == *CompName)
						{
							UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(PropIt->GetObjectPropertyValue_InContainer(CDO));
							if (SMC)
							{
								SMC->SetMaterial(0, MI);
								bAppliedToBlueprint = true;
							}
							break;
						}
					}
					if (bAppliedToBlueprint)
					{
						FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
					}
				}
			}
		}

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),              true);
		Resp->SetStringField(TEXT("asset_path"),           AssetPath);
		Resp->SetStringField(TEXT("parent_material"),      ParentMatPath);
		Resp->SetBoolField  (TEXT("already_existed"),      bAlreadyExisted);
		Resp->SetNumberField(TEXT("scalar_params_set"),    ScalarParams.Num());
		Resp->SetNumberField(TEXT("vector_params_set"),    VectorParams.Num());
		Resp->SetNumberField(TEXT("texture_params_set"),   TextureParamsSet);
		Resp->SetBoolField  (TEXT("applied_to_blueprint"), bAppliedToBlueprint);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonCreated(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/read_material
// Body: { "asset_path": "/Game/.../M_Foo" }
// Inspects a UMaterial or UMaterialInstance and reports its domain, blend mode,
// shading model, parent (instances only), and every scalar / vector / texture
// parameter with its current value. The editor is the source of truth — use
// this before SetVectorParameterValue calls to confirm the parameter exists.
// ============================================================================

bool FNGGHttpServer::HandleReadMaterial(
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

	FString AssetPath; Body->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("asset_path is required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath]()
	{
		UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *AssetPath);
		if (!Mat)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Material not found: '%s'"), *AssetPath)));
			return;
		}

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),    true);
		Resp->SetStringField(TEXT("asset_path"), AssetPath);

		// Is it an instance? Report parent + class.
		if (UMaterialInstance* MInst = Cast<UMaterialInstance>(Mat))
		{
			Resp->SetStringField(TEXT("class"), TEXT("MaterialInstance"));
			Resp->SetStringField(TEXT("parent"),
				MInst->Parent ? MInst->Parent->GetPathName() : TEXT(""));
		}
		else
		{
			Resp->SetStringField(TEXT("class"), TEXT("Material"));
		}

		// Domain / blend mode / shading model via enum reflection (stable names).
		if (const UMaterial* BaseMat = Mat->GetMaterial())
		{
			if (const UEnum* E = StaticEnum<EMaterialDomain>())
				Resp->SetStringField(TEXT("domain"), E->GetNameStringByValue((int64)BaseMat->MaterialDomain));
		}
		if (const UEnum* E = StaticEnum<EBlendMode>())
			Resp->SetStringField(TEXT("blend_mode"), E->GetNameStringByValue((int64)Mat->GetBlendMode()));
		if (const UEnum* E = StaticEnum<EMaterialShadingModel>())
			Resp->SetStringField(TEXT("shading_model"),
				E->GetNameStringByValue((int64)Mat->GetShadingModels().GetFirstShadingModel()));

		// Scalar parameters
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Ids;
			Mat->GetAllScalarParameterInfo(Infos, Ids);
			for (const FMaterialParameterInfo& Info : Infos)
			{
				float Value = 0.f;
				Mat->GetScalarParameterValue(Info, Value);
				TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("name"),  Info.Name.ToString());
				P->SetNumberField(TEXT("value"), Value);
				Arr.Add(MakeShared<FJsonValueObject>(P));
			}
			Resp->SetArrayField(TEXT("scalar_params"), Arr);
		}

		// Vector parameters
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Ids;
			Mat->GetAllVectorParameterInfo(Infos, Ids);
			for (const FMaterialParameterInfo& Info : Infos)
			{
				FLinearColor Value = FLinearColor::Black;
				Mat->GetVectorParameterValue(Info, Value);
				TSharedPtr<FJsonObject> Val = MakeShared<FJsonObject>();
				Val->SetNumberField(TEXT("r"), Value.R);
				Val->SetNumberField(TEXT("g"), Value.G);
				Val->SetNumberField(TEXT("b"), Value.B);
				Val->SetNumberField(TEXT("a"), Value.A);
				TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("name"),  Info.Name.ToString());
				P->SetObjectField(TEXT("value"), Val);
				Arr.Add(MakeShared<FJsonValueObject>(P));
			}
			Resp->SetArrayField(TEXT("vector_params"), Arr);
		}

		// Texture parameters
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Ids;
			Mat->GetAllTextureParameterInfo(Infos, Ids);
			for (const FMaterialParameterInfo& Info : Infos)
			{
				UTexture* Tex = nullptr;
				Mat->GetTextureParameterValue(Info, Tex);
				TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("name"),         Info.Name.ToString());
				P->SetStringField(TEXT("texture_path"), Tex ? Tex->GetPathName() : TEXT(""));
				Arr.Add(MakeShared<FJsonValueObject>(P));
			}
			Resp->SetArrayField(TEXT("texture_params"), Arr);
		}

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/create_float_curve
// Body: { "asset_path": "/Game/.../Curve_X",
//         "keys": [ { "time": 0.0, "value": 0.0 }, ... ],
//         "linear": true }   // linear (default) or constant (stepped) interp
// Creates / overwrites a UCurveFloat — the scalar counterpart of a color curve
// (intensity / speed / alpha over time, sampled with GetFloatValue).
// ============================================================================

bool FNGGHttpServer::HandleCreateFloatCurve(
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

	FString AssetPath; Body->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("asset_path is required")));
		return true;
	}

	bool bLinear = true;
	Body->TryGetBoolField(TEXT("linear"), bLinear);

	struct FFloatKeyIn { float Time = 0.f, Value = 0.f; };
	TArray<FFloatKeyIn> Keys;
	const TArray<TSharedPtr<FJsonValue>>* KeysArr = nullptr;
	if (Body->TryGetArrayField(TEXT("keys"), KeysArr))
	{
		for (const TSharedPtr<FJsonValue>& V : *KeysArr)
		{
			const TSharedPtr<FJsonObject>* K = nullptr;
			if (V->TryGetObject(K) && K)
			{
				FFloatKeyIn Key;
				double D;
				if ((*K)->TryGetNumberField(TEXT("time"),  D)) Key.Time  = (float)D;
				if ((*K)->TryGetNumberField(TEXT("value"), D)) Key.Value = (float)D;
				Keys.Add(Key);
			}
		}
	}
	if (Keys.Num() == 0)
	{
		Callback(JsonError(400, TEXT("keys[] is required — each { time, value }")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, Keys, bLinear]()
	{
		FString PackagePath, AssetName;
		AssetPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

		UCurveFloat* Curve = LoadObject<UCurveFloat>(nullptr, *AssetPath);
		const bool bAlreadyExisted = (Curve != nullptr);
		if (!Curve)
		{
			UPackage* Package = CreatePackage(*AssetPath);
			if (!Package)
			{
				Callback(JsonError(500, FString::Printf(TEXT("Failed to create package for '%s'"), *AssetPath)));
				return;
			}
			Curve = NewObject<UCurveFloat>(Package, FName(*AssetName),
				RF_Public | RF_Standalone | RF_Transactional);
			FAssetRegistryModule::AssetCreated(Curve);
		}
		if (!Curve)
		{
			Callback(JsonError(500, FString::Printf(TEXT("Failed to create curve at '%s'"), *AssetPath)));
			return;
		}

		Curve->FloatCurve.Reset();
		const ERichCurveInterpMode Interp = bLinear ? RCIM_Linear : RCIM_Constant;
		for (const FFloatKeyIn& K : Keys)
		{
			const FKeyHandle H = Curve->FloatCurve.AddKey(K.Time, K.Value);
			Curve->FloatCurve.SetKeyInterpMode(H, Interp);
		}

		Curve->MarkPackageDirty();

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),         true);
		Resp->SetStringField(TEXT("asset_path"),      AssetPath);
		Resp->SetBoolField  (TEXT("already_existed"), bAlreadyExisted);
		Resp->SetNumberField(TEXT("key_count"),       Keys.Num());

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonCreated(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/read_curve
// Body: { "asset_path": "/Game/.../Curve_X" }
// Reads a UCurveFloat or UCurveLinearColor and returns its keys. Float curves
// return [{ time, value }]; color curves return [{ time, r, g, b, a }].
// ============================================================================

bool FNGGHttpServer::HandleReadCurve(
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

	FString AssetPath; Body->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("asset_path is required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath]()
	{
		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),    true);
		Resp->SetStringField(TEXT("asset_path"), AssetPath);

		if (UCurveLinearColor* Color = LoadObject<UCurveLinearColor>(nullptr, *AssetPath))
		{
			Resp->SetStringField(TEXT("type"), TEXT("color"));
			TArray<TSharedPtr<FJsonValue>> Arr;
			// Channel 0 (R) drives the key times; sample all channels at each time.
			for (const FRichCurveKey& Key : Color->FloatCurves[0].Keys)
			{
				const FLinearColor C = Color->GetLinearColorValue(Key.Time);
				TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>();
				K->SetNumberField(TEXT("time"), Key.Time);
				K->SetNumberField(TEXT("r"),    C.R);
				K->SetNumberField(TEXT("g"),    C.G);
				K->SetNumberField(TEXT("b"),    C.B);
				K->SetNumberField(TEXT("a"),    C.A);
				Arr.Add(MakeShared<FJsonValueObject>(K));
			}
			Resp->SetArrayField(TEXT("keys"), Arr);
		}
		else if (UCurveFloat* Float = LoadObject<UCurveFloat>(nullptr, *AssetPath))
		{
			Resp->SetStringField(TEXT("type"), TEXT("float"));
			TArray<TSharedPtr<FJsonValue>> Arr;
			for (const FRichCurveKey& Key : Float->FloatCurve.Keys)
			{
				TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>();
				K->SetNumberField(TEXT("time"),  Key.Time);
				K->SetNumberField(TEXT("value"), Key.Value);
				Arr.Add(MakeShared<FJsonValueObject>(K));
			}
			Resp->SetArrayField(TEXT("keys"), Arr);
		}
		else
		{
			Callback(JsonError(404, FString::Printf(TEXT("Curve not found (float or color): '%s'"), *AssetPath)));
			return;
		}

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}
