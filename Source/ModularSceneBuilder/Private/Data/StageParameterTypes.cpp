// Copyright Epic Games, Inc. All Rights Reserved.

#include "Data/StageParameterTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageParameterTypes)

FStageParameterValue FStageParameterValue::MakeFloat(double InValue)
{
	FStageParameterValue Result;
	Result.Type = EStageParameterType::Float;
	Result.Float = InValue;
	return Result;
}

FStageParameterValue FStageParameterValue::MakeInteger(int32 InValue)
{
	FStageParameterValue Result;
	Result.Type = EStageParameterType::Integer;
	Result.Integer = InValue;
	return Result;
}

FStageParameterValue FStageParameterValue::MakeBool(bool bInValue)
{
	FStageParameterValue Result;
	Result.Type = EStageParameterType::Bool;
	Result.bBool = bInValue;
	return Result;
}

FStageParameterValue FStageParameterValue::MakeColor(const FLinearColor& InValue)
{
	FStageParameterValue Result;
	Result.Type = EStageParameterType::Color;
	Result.Color = InValue;
	return Result;
}

FStageParameterValue FStageParameterValue::MakeVector(const FVector& InValue)
{
	FStageParameterValue Result;
	Result.Type = EStageParameterType::Vector;
	Result.Vector = InValue;
	return Result;
}

FStageParameterValue FStageParameterValue::MakeRotator(const FRotator& InValue)
{
	FStageParameterValue Result;
	Result.Type = EStageParameterType::Rotator;
	Result.Rotator = InValue;
	return Result;
}

FStageParameterValue FStageParameterValue::MakeText(const FText& InValue)
{
	FStageParameterValue Result;
	Result.Type = EStageParameterType::Text;
	Result.Text = InValue;
	return Result;
}

FStageParameterValue FStageParameterValue::MakeEnum(int32 InOptionIndex)
{
	FStageParameterValue Result;
	Result.Type = EStageParameterType::Enum;
	Result.Integer = InOptionIndex;
	return Result;
}

FStageParameterDescriptor& FStageParameterSection::Add(const FGameplayTag& InId, const FText& InName, const FStageParameterValue& InValue)
{
	FStageParameterDescriptor& Descriptor = Parameters.AddDefaulted_GetRef();
	Descriptor.Id = InId;
	Descriptor.DisplayName = InName;
	Descriptor.Value = InValue;
	return Descriptor;
}

namespace StageCraftTags
{
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(FeatureGroup,				"StageCraft.FeatureGroup",				"Root of inspector feature groups.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(FeatureGroup_Info,			"StageCraft.FeatureGroup.Info",			"Name, model and physical specs.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(FeatureGroup_Transform,		"StageCraft.FeatureGroup.Transform",	"World position and rotation.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(FeatureGroup_Patch,			"StageCraft.FeatureGroup.Patch",		"Fixture ID and DMX patch.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(FeatureGroup_Dimmer,			"StageCraft.FeatureGroup.Dimmer",		"Intensity and shutter.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(FeatureGroup_Position,		"StageCraft.FeatureGroup.Position",		"Pan and tilt.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(FeatureGroup_Color,			"StageCraft.FeatureGroup.Color",		"Color mixing.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(FeatureGroup_Beam,			"StageCraft.FeatureGroup.Beam",			"Zoom, focus and beam shaping.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(FeatureGroup_Audio,			"StageCraft.FeatureGroup.Audio",		"Speaker and system settings.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(FeatureGroup_Rigging,		"StageCraft.FeatureGroup.Rigging",		"Truss and hoist specs.");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Info_Label,			"StageCraft.Param.Info.Label",			"User label of the placed instance.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Info_Model,			"StageCraft.Param.Info.Model",			"Catalog model name (read-only).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Info_Type,			"StageCraft.Param.Info.Type",			"Catalog item of the instance; changing it swaps the model in place.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Info_Weight,			"StageCraft.Param.Info.Weight",			"Weight in kg (read-only).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Info_Power,			"StageCraft.Param.Info.Power",			"Power draw in W (read-only).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Transform_Location,	"StageCraft.Param.Transform.Location",	"World location in cm.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Transform_Rotation,	"StageCraft.Param.Transform.Rotation",	"World rotation in degrees.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Transform_Scale,		"StageCraft.Param.Transform.Scale",		"Per-axis scale, clamped to StageTransformRules limits.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Patch_FixtureId,		"StageCraft.Param.Patch.FixtureId",		"Console fixture ID.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Patch_Universe,		"StageCraft.Param.Patch.Universe",		"DMX universe (0 = unpatched).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Patch_Address,			"StageCraft.Param.Patch.Address",		"DMX start address 1..512.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Patch_Footprint,		"StageCraft.Param.Patch.Footprint",		"Channel count of the DMX mode (read-only).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Audio_Gain,			"StageCraft.Param.Audio.Gain",			"Output gain in dB.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Audio_Mute,			"StageCraft.Param.Audio.Mute",			"Muted.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Audio_Delay,			"StageCraft.Param.Audio.Delay",			"Alignment delay in ms.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Audio_Polarity,		"StageCraft.Param.Audio.Polarity",		"Polarity inverted.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Audio_Splay,			"StageCraft.Param.Audio.Splay",			"Line array inter-cabinet splay angle.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Audio_Coverage,		"StageCraft.Param.Audio.Coverage",		"Nominal H x V coverage (read-only).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Audio_Channels,		"StageCraft.Param.Audio.Channels",		"Console inputs or amplifier outputs (read-only).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Rigging_Length,		"StageCraft.Param.Rigging.Length",		"Truss length in cm (read-only).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Rigging_Profile,		"StageCraft.Param.Rigging.Profile",		"Truss profile (read-only).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Rigging_MaxPointLoad,	"StageCraft.Param.Rigging.MaxPointLoad","Max centre point load in kg (read-only).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Param_Rigging_Points,		"StageCraft.Param.Rigging.Points",		"Number of rigging points (read-only).");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attribute,					"StageCraft.Attribute",					"Root of fixture attributes (cue-recordable, DMX-addressable).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attribute_Dimmer,			"StageCraft.Attribute.Dimmer",			"Intensity 0..1.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attribute_Pan,				"StageCraft.Attribute.Pan",				"Pan in degrees.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attribute_Tilt,				"StageCraft.Attribute.Tilt",			"Tilt in degrees.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attribute_Zoom,				"StageCraft.Attribute.Zoom",			"Full beam angle in degrees.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attribute_Shutter,			"StageCraft.Attribute.Shutter",			"0 = open, above 0 = strobe rate.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attribute_ColorR,			"StageCraft.Attribute.ColorR",			"Red 0..1.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attribute_ColorG,			"StageCraft.Attribute.ColorG",			"Green 0..1.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attribute_ColorB,			"StageCraft.Attribute.ColorB",			"Blue 0..1.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attribute_ColorW,			"StageCraft.Attribute.ColorW",			"White 0..1.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attribute_ColorRGB,			"StageCraft.Attribute.ColorRGB",		"Composite inspector row over R/G/B.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attribute_ColorC,			"StageCraft.Attribute.ColorC",			"Cyan DMX channel (1 - R).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attribute_ColorM,			"StageCraft.Attribute.ColorM",			"Magenta DMX channel (1 - G).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attribute_ColorY,			"StageCraft.Attribute.ColorY",			"Yellow DMX channel (1 - B).");
}
