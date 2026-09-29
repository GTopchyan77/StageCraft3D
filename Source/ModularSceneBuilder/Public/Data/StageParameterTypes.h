// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "NativeGameplayTags.h"
#include "StageParameterTypes.generated.h"

/**
 * Generic, self-describing parameter model shared by every placed item, the inspector UI, the
 * show control system and (later) save files.
 *
 * Items describe their editable state as sections of descriptors (IStageParameterInterface), so
 * the inspector never needs per-equipment widgets: a truss, a moving head and a line array are
 * all "a list of typed rows". Parameters are identified by GameplayTags, never by strings.
 * Tags under StageCraft.Attribute are fixture control channels: they are what cues record and
 * what DMX drives. Everything else (transform, patch, audio settings) is edit-only.
 */
UENUM(BlueprintType)
enum class EStageParameterType : uint8
{
	Float,
	Integer,
	Bool,
	Color,
	Vector,
	Rotator,
	Text,
};

/** Tagged union of every value a parameter can hold. Only the member matching Type is meaningful. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageParameterValue
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parameter")
	EStageParameterType Type = EStageParameterType::Float;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parameter")
	double Float = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parameter")
	int32 Integer = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parameter")
	bool bBool = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parameter")
	FLinearColor Color = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parameter")
	FVector Vector = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parameter")
	FRotator Rotator = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parameter")
	FText Text;

	static FStageParameterValue MakeFloat(double InValue);
	static FStageParameterValue MakeInteger(int32 InValue);
	static FStageParameterValue MakeBool(bool bInValue);
	static FStageParameterValue MakeColor(const FLinearColor& InValue);
	static FStageParameterValue MakeVector(const FVector& InValue);
	static FStageParameterValue MakeRotator(const FRotator& InValue);
	static FStageParameterValue MakeText(const FText& InValue);
};

/** One inspector row: identity, type, current value and how to present/limit it. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageParameterDescriptor
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Parameter", meta = (Categories = "StageCraft"))
	FGameplayTag Id;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Parameter")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Parameter")
	FStageParameterValue Value;

	/** Numeric limits in stored units. Min >= Max means unbounded. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Parameter")
	double Min = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Parameter")
	double Max = 0.0;

	/** Multiplier from stored units to displayed units (e.g. dimmer is stored 0..1, shown as 0..100 %). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Parameter")
	double DisplayScale = 1.0;

	/** Suggested drag step in displayed units. Zero lets the widget decide. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Parameter")
	double Step = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Parameter")
	FText Units;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Parameter")
	bool bReadOnly = false;

	EStageParameterType GetType() const { return Value.Type; }
	bool HasRange() const { return Min < Max; }

	FStageParameterDescriptor& Range(double InMin, double InMax) { Min = InMin; Max = InMax; return *this; }
	FStageParameterDescriptor& Display(double InScale, const FText& InUnits, double InStep = 0.0) { DisplayScale = InScale; Units = InUnits; Step = InStep; return *this; }
	FStageParameterDescriptor& ReadOnly() { bReadOnly = true; return *this; }
};

/** A titled group of rows. FeatureGroup drives the section color strip (GrandMA-style feature groups). */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageParameterSection
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Parameter", meta = (Categories = "StageCraft.FeatureGroup"))
	FGameplayTag FeatureGroup;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Parameter")
	FText Title;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Parameter")
	TArray<FStageParameterDescriptor> Parameters;

	FStageParameterSection() = default;
	FStageParameterSection(const FGameplayTag& InGroup, const FText& InTitle) : FeatureGroup(InGroup), Title(InTitle) {}

	/** Appends a row and returns it for chained Range/Display/ReadOnly calls. */
	FStageParameterDescriptor& Add(const FGameplayTag& InId, const FText& InName, const FStageParameterValue& InValue);
};

namespace StageCraftTags
{
	// Feature groups: inspector sections and their colors.
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(FeatureGroup);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(FeatureGroup_Info);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(FeatureGroup_Transform);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(FeatureGroup_Patch);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(FeatureGroup_Dimmer);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(FeatureGroup_Position);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(FeatureGroup_Color);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(FeatureGroup_Beam);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(FeatureGroup_Audio);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(FeatureGroup_Rigging);

	// Edit-only parameters.
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Info_Label);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Info_Model);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Info_Weight);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Info_Power);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Transform_Location);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Transform_Rotation);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Patch_FixtureId);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Patch_Universe);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Patch_Address);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Patch_Footprint);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Audio_Gain);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Audio_Mute);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Audio_Delay);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Audio_Polarity);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Audio_Splay);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Audio_Coverage);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Audio_Channels);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Rigging_Length);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Rigging_Profile);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Rigging_MaxPointLoad);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Param_Rigging_Points);

	// Fixture attributes: recordable in cues and addressable over DMX. Stored as floats in physical units.
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attribute);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attribute_Dimmer);		// 0..1
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attribute_Pan);			// degrees, within fixture pan limits
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attribute_Tilt);			// degrees, within fixture tilt limits
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attribute_Zoom);			// full beam angle in degrees
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attribute_Shutter);		// 0 = open, 0..1 = strobe rate
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attribute_ColorR);		// 0..1
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attribute_ColorG);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attribute_ColorB);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attribute_ColorW);
	/** Composite inspector row over ColorR/G/B. Never stored or recorded itself. */
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attribute_ColorRGB);
	/** Subtractive DMX channels, derived from RGB (C = 1 - R). Used only in DMX channel layouts of CMY fixtures. */
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attribute_ColorC);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attribute_ColorM);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attribute_ColorY);
}
