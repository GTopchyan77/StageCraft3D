// Copyright Epic Games, Inc. All Rights Reserved.

#include "Actors/LightingFixtureActor.h"

#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Data/LightingFixtureData.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Interaction/StageCraftCollision.h"
#include "ModularSceneBuilder.h"
#include "Subsystems/ShowControlSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LightingFixtureActor)

#define LOCTEXT_NAMESPACE "StageCraftLightingFixtureActor"

namespace
{
	/** Moving parts must be selectable like the base, but never block placement or physics. */
	void ConfigureSelectableMesh(UStaticMeshComponent& Mesh)
	{
		Mesh.SetMobility(EComponentMobility::Movable);
		Mesh.SetCollisionProfileName(UCollisionProfile::BlockAllDynamic_ProfileName);
		Mesh.SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Mesh.SetCollisionResponseToChannel(StageCraftCollision::StageItemChannel, ECR_Block);
		Mesh.SetGenerateOverlapEvents(false);
	}

	bool IsColorAttribute(const FGameplayTag& Attribute)
	{
		return Attribute == StageCraftTags::Attribute_ColorR
			|| Attribute == StageCraftTags::Attribute_ColorG
			|| Attribute == StageCraftTags::Attribute_ColorB;
	}

	/** C/M/Y channels are the inverse of R/G/B, so they read and write the RGB attribute they mirror. */
	FGameplayTag ResolveDMXAttribute(const FGameplayTag& ChannelAttribute, bool& bOutInverted)
	{
		bOutInverted = true;
		if (ChannelAttribute == StageCraftTags::Attribute_ColorC) { return StageCraftTags::Attribute_ColorR; }
		if (ChannelAttribute == StageCraftTags::Attribute_ColorM) { return StageCraftTags::Attribute_ColorG; }
		if (ChannelAttribute == StageCraftTags::Attribute_ColorY) { return StageCraftTags::Attribute_ColorB; }
		bOutInverted = false;
		return ChannelAttribute;
	}
}

ALightingFixtureActor::ALightingFixtureActor()
{
	PanPivot = CreateDefaultSubobject<USceneComponent>(TEXT("PanPivot"));
	PanPivot->SetupAttachment(MeshComponent);

	YokeMeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("YokeMesh"));
	YokeMeshComponent->SetupAttachment(PanPivot);
	ConfigureSelectableMesh(*YokeMeshComponent);

	TiltPivot = CreateDefaultSubobject<USceneComponent>(TEXT("TiltPivot"));
	TiltPivot->SetupAttachment(PanPivot);

	HeadMeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HeadMesh"));
	HeadMeshComponent->SetupAttachment(TiltPivot);
	ConfigureSelectableMesh(*HeadMeshComponent);

	BeamLight = CreateDefaultSubobject<USpotLightComponent>(TEXT("BeamLight"));
	BeamLight->SetupAttachment(TiltPivot);
	// Spot lights shine along +X; pitch it so the beam leaves the head along +Z (straight up for a standing fixture, down when hung).
	BeamLight->SetRelativeRotation(FRotator(90.0, 0.0, 0.0));
	BeamLight->SetMobility(EComponentMobility::Movable);
	BeamLight->IntensityUnits = ELightUnits::Lumens;
	BeamLight->CastShadows = true;
}

ULightingFixtureData* ALightingFixtureActor::GetFixtureData() const
{
	return Cast<ULightingFixtureData>(ItemData);
}

UShowControlSubsystem* ALightingFixtureActor::GetShowControl() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetSubsystem<UShowControlSubsystem>() : nullptr;
}

void ALightingFixtureActor::BeginPlay()
{
	Super::BeginPlay();

	if (UShowControlSubsystem* ShowControl = GetShowControl())
	{
		ShowControl->RegisterFixture(this);
	}
}

void ALightingFixtureActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UShowControlSubsystem* ShowControl = GetShowControl())
	{
		ShowControl->UnregisterFixture(this);
	}

	Super::EndPlay(EndPlayReason);
}

void ALightingFixtureActor::ApplyItemData(const UBaseItemData& Data)
{
	Super::ApplyItemData(Data);

	const ULightingFixtureData* Fixture = Cast<ULightingFixtureData>(&Data);
	if (!Fixture)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: item '%s' is not a LightingFixtureData; the fixture will stay dark."), *GetName(), *Data.GetName());
		return;
	}

	PanPivot->SetRelativeLocation(Fixture->PanPivotOffset);
	TiltPivot->SetRelativeLocation(Fixture->TiltPivotOffset);
	BeamLight->SetRelativeLocation(Fixture->BeamOriginOffset);

	YokeMeshComponent->SetStaticMesh(Fixture->bHasPanTilt ? Fixture->YokeMesh.LoadSynchronous() : nullptr);
	HeadMeshComponent->SetStaticMesh(Fixture->HeadMesh.LoadSynchronous());

	BeamLight->SetAttenuationRadius(Fixture->BeamRange);
	BeamLight->SetUseTemperature(Fixture->ColorSystem == EStageFixtureColorSystem::None);
	BeamLight->SetTemperature(Fixture->ColorTemperatureK);

	// Keep values the instance already has (re-applied data, loaded shows); fill in new ones; drop ones the fixture cannot do.
	const TArray<FGameplayTag> Supported = Fixture->GetSupportedAttributes();
	for (auto It = Attributes.CreateIterator(); It; ++It)
	{
		if (!Supported.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}
	for (const FGameplayTag& Attribute : Supported)
	{
		if (!Attributes.Contains(Attribute))
		{
			Attributes.Add(Attribute, Fixture->GetAttributeDefault(Attribute));
		}
	}

	ApplyAttributesToComponents();
}

void ALightingFixtureActor::UpdateHighlight()
{
	Super::UpdateHighlight();

	UMaterialInterface* Overlay = MeshComponent->GetOverlayMaterial();
	YokeMeshComponent->SetOverlayMaterial(Overlay);
	HeadMeshComponent->SetOverlayMaterial(Overlay);
}

bool ALightingFixtureActor::StoreAttribute(const FGameplayTag& Attribute, float Value)
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	float Min = 0.f;
	float Max = 1.f;
	if (!Fixture || !Fixture->GetAttributeRange(Attribute, Min, Max))
	{
		return false;
	}

	float& Stored = Attributes.FindOrAdd(Attribute);
	const float Clamped = FMath::Clamp(Value, Min, Max);
	if (FMath::IsNearlyEqual(Stored, Clamped))
	{
		return false;
	}
	Stored = Clamped;
	return true;
}

void ALightingFixtureActor::NotifyAttributeChanged(const FGameplayTag& Attribute)
{
	NotifyParameterChanged(Attribute);
	if (IsColorAttribute(Attribute))
	{
		NotifyParameterChanged(StageCraftTags::Attribute_ColorRGB);
	}
}

bool ALightingFixtureActor::SetAttribute(FGameplayTag Attribute, float Value)
{
	if (!StoreAttribute(Attribute, Value))
	{
		return false;
	}
	ApplyAttributesToComponents();
	NotifyAttributeChanged(Attribute);
	return true;
}

bool ALightingFixtureActor::SetAttributes(const TMap<FGameplayTag, float>& Values)
{
	TArray<FGameplayTag, TInlineAllocator<16>> Changed;
	for (const TPair<FGameplayTag, float>& Pair : Values)
	{
		if (StoreAttribute(Pair.Key, Pair.Value))
		{
			Changed.Add(Pair.Key);
		}
	}

	if (Changed.IsEmpty())
	{
		return false;
	}

	ApplyAttributesToComponents();
	bool bColorNotified = false;
	for (const FGameplayTag& Attribute : Changed)
	{
		NotifyParameterChanged(Attribute);
		bColorNotified |= IsColorAttribute(Attribute);
	}
	if (bColorNotified)
	{
		NotifyParameterChanged(StageCraftTags::Attribute_ColorRGB);
	}
	return true;
}

float ALightingFixtureActor::GetAttribute(FGameplayTag Attribute) const
{
	const float* Value = Attributes.Find(Attribute);
	return Value ? *Value : 0.f;
}

FLinearColor ALightingFixtureActor::GetColorRGB() const
{
	return FLinearColor(
		GetAttribute(StageCraftTags::Attribute_ColorR),
		GetAttribute(StageCraftTags::Attribute_ColorG),
		GetAttribute(StageCraftTags::Attribute_ColorB));
}

void ALightingFixtureActor::ApplyAttributesToComponents()
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	if (!Fixture)
	{
		BeamLight->SetIntensity(0.f);
		return;
	}

	if (Fixture->bHasPanTilt)
	{
		PanPivot->SetRelativeRotation(FRotator(0.0, GetAttribute(StageCraftTags::Attribute_Pan), 0.0));
		TiltPivot->SetRelativeRotation(FRotator(GetAttribute(StageCraftTags::Attribute_Tilt), 0.0, 0.0));
	}

	// Shutter/strobe is recorded and sent over DMX but not animated yet: that needs a central strobe clock (see STATE.md).
	BeamLight->SetIntensity(Fixture->LuminousFlux * GetAttribute(StageCraftTags::Attribute_Dimmer));

	const float HalfAngle = 0.5f * (Fixture->HasZoom() ? GetAttribute(StageCraftTags::Attribute_Zoom) : Fixture->BeamAngleMax);
	BeamLight->SetOuterConeAngle(HalfAngle);
	BeamLight->SetInnerConeAngle(HalfAngle * 0.7f);

	if (Fixture->ColorSystem != EStageFixtureColorSystem::None)
	{
		// Additive white mix, renormalised so full RGB + full W stays a valid (brightest-channel = 1) color.
		const float White = GetAttribute(StageCraftTags::Attribute_ColorW);
		FLinearColor Mixed = GetColorRGB() + FLinearColor(White, White, White, 0.f);
		const float Peak = Mixed.GetMax();
		if (Peak > 1.f)
		{
			Mixed /= Peak;
		}
		Mixed.A = 1.f;
		BeamLight->SetLightColor(Mixed, /*bSRGB*/ false);
	}
}

bool ALightingFixtureActor::SetFixtureId(int32 NewFixtureId)
{
	if (NewFixtureId <= 0 || NewFixtureId == FixtureId)
	{
		return NewFixtureId == FixtureId;
	}

	const UShowControlSubsystem* ShowControl = GetShowControl();
	if (ShowControl && !ShowControl->IsFixtureIdFree(NewFixtureId, this))
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: fixture ID %d is already in use."), *GetName(), NewFixtureId);
		return false;
	}

	FixtureId = NewFixtureId;
	NotifyParameterChanged(StageCraftTags::Param_Patch_FixtureId);
	return true;
}

void ALightingFixtureActor::SetPatch(const FStageDMXPatch& NewPatch)
{
	FStageDMXPatch Clamped;
	Clamped.Universe = FMath::Clamp(NewPatch.Universe, 0, 63999);
	Clamped.Address = FMath::Clamp(NewPatch.Address, 0, 512);
	if (Clamped == Patch)
	{
		return;
	}

	const bool bUniverseChanged = Clamped.Universe != Patch.Universe;
	const bool bAddressChanged = Clamped.Address != Patch.Address;
	Patch = Clamped;
	if (bUniverseChanged)
	{
		NotifyParameterChanged(StageCraftTags::Param_Patch_Universe);
	}
	if (bAddressChanged)
	{
		NotifyParameterChanged(StageCraftTags::Param_Patch_Address);
	}
}

int32 ALightingFixtureActor::GetDMXFootprint() const
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	return Fixture ? Fixture->GetDMXFootprint() : 0;
}

void ALightingFixtureActor::WriteDMX(TArrayView<uint8> UniverseData) const
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	if (!Fixture || !Patch.IsPatched())
	{
		return;
	}

	for (const FStageFixtureDMXChannel& Channel : Fixture->DMXChannels)
	{
		bool bInverted = false;
		const FGameplayTag Attribute = ResolveDMXAttribute(Channel.Attribute, bInverted);
		float Min = 0.f;
		float Max = 1.f;
		if (!Fixture->SupportsAttribute(Channel.Attribute) || !Fixture->GetAttributeRange(Attribute, Min, Max))
		{
			continue;
		}

		const int32 Index = Patch.Address - 1 + Channel.Channel - 1;
		if (Index < 0 || Index + Channel.GetWidth() > UniverseData.Num())
		{
			continue;
		}

		float Normalized = Max > Min ? (GetAttribute(Attribute) - Min) / (Max - Min) : 0.f;
		Normalized = FMath::Clamp(bInverted ? 1.f - Normalized : Normalized, 0.f, 1.f);

		if (Channel.b16Bit)
		{
			const uint16 Value = static_cast<uint16>(FMath::RoundToInt(Normalized * 65535.f));
			UniverseData[Index] = static_cast<uint8>(Value >> 8);
			UniverseData[Index + 1] = static_cast<uint8>(Value & 0xFF);
		}
		else
		{
			UniverseData[Index] = static_cast<uint8>(FMath::RoundToInt(Normalized * 255.f));
		}
	}
}

bool ALightingFixtureActor::ReadDMX(TConstArrayView<uint8> UniverseData)
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	if (!Fixture || !Patch.IsPatched())
	{
		return false;
	}

	TMap<FGameplayTag, float> Values;
	for (const FStageFixtureDMXChannel& Channel : Fixture->DMXChannels)
	{
		bool bInverted = false;
		const FGameplayTag Attribute = ResolveDMXAttribute(Channel.Attribute, bInverted);
		float Min = 0.f;
		float Max = 1.f;
		if (!Fixture->SupportsAttribute(Channel.Attribute) || !Fixture->GetAttributeRange(Attribute, Min, Max))
		{
			continue;
		}

		const int32 Index = Patch.Address - 1 + Channel.Channel - 1;
		if (Index < 0 || Index + Channel.GetWidth() > UniverseData.Num())
		{
			continue;
		}

		float Normalized = Channel.b16Bit
			? static_cast<float>((UniverseData[Index] << 8) | UniverseData[Index + 1]) / 65535.f
			: static_cast<float>(UniverseData[Index]) / 255.f;
		if (bInverted)
		{
			Normalized = 1.f - Normalized;
		}

		Values.Add(Attribute, FMath::Lerp(Min, Max, Normalized));
	}

	return SetAttributes(Values);
}

FStageItemInteractionDetails ALightingFixtureActor::GetInteractionDetails_Implementation() const
{
	FStageItemInteractionDetails Details = Super::GetInteractionDetails_Implementation();

	const ULightingFixtureData* Fixture = GetFixtureData();
	Details.bSupportsColorEditing = Fixture && Fixture->ColorSystem != EStageFixtureColorSystem::None;
	Details.Color = GetColorRGB();
	return Details;
}

void ALightingFixtureActor::GatherParameterSections(TArray<FStageParameterSection>& OutSections) const
{
	Super::GatherParameterSections(OutSections);

	const ULightingFixtureData* Fixture = GetFixtureData();
	if (!Fixture)
	{
		return;
	}

	FStageParameterSection& PatchSection = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Patch, LOCTEXT("PatchSection", "Patch"));
	PatchSection.Add(StageCraftTags::Param_Patch_FixtureId, LOCTEXT("FixtureId", "Fixture ID"), FStageParameterValue::MakeInteger(FixtureId)).Range(1, 99999);
	PatchSection.Add(StageCraftTags::Param_Patch_Universe, LOCTEXT("Universe", "Universe"), FStageParameterValue::MakeInteger(Patch.Universe)).Range(0, 63999);
	PatchSection.Add(StageCraftTags::Param_Patch_Address, LOCTEXT("Address", "Address"), FStageParameterValue::MakeInteger(Patch.Address)).Range(0, 512);
	PatchSection.Add(StageCraftTags::Param_Patch_Footprint, FText::Format(LOCTEXT("FootprintFormat", "Mode: {0}"), FText::FromName(Fixture->DMXModeName)),
		FStageParameterValue::MakeInteger(Fixture->GetDMXFootprint())).Display(1.0, LOCTEXT("Channels", "ch")).ReadOnly();

	auto AddAttribute = [this, Fixture](FStageParameterSection& Section, const FGameplayTag& Attribute, const FText& Name, double DisplayScale, const FText& Units)
	{
		float Min = 0.f;
		float Max = 1.f;
		if (Fixture->GetAttributeRange(Attribute, Min, Max))
		{
			Section.Add(Attribute, Name, FStageParameterValue::MakeFloat(GetAttribute(Attribute))).Range(Min, Max).Display(DisplayScale, Units);
		}
	};

	const FText Percent = LOCTEXT("Percent", "%");
	const FText Degrees = LOCTEXT("Degrees", "deg");

	FStageParameterSection& Dimmer = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Dimmer, LOCTEXT("DimmerSection", "Dimmer"));
	AddAttribute(Dimmer, StageCraftTags::Attribute_Dimmer, LOCTEXT("Dim", "Dimmer"), 100.0, Percent);
	AddAttribute(Dimmer, StageCraftTags::Attribute_Shutter, LOCTEXT("Shutter", "Strobe"), 100.0, Percent);

	if (Fixture->bHasPanTilt)
	{
		FStageParameterSection& Position = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Position, LOCTEXT("PositionSection", "Position"));
		AddAttribute(Position, StageCraftTags::Attribute_Pan, LOCTEXT("Pan", "Pan"), 1.0, Degrees);
		AddAttribute(Position, StageCraftTags::Attribute_Tilt, LOCTEXT("Tilt", "Tilt"), 1.0, Degrees);
	}

	if (Fixture->ColorSystem != EStageFixtureColorSystem::None)
	{
		FStageParameterSection& Color = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Color, LOCTEXT("ColorSection", "Color"));
		Color.Add(StageCraftTags::Attribute_ColorRGB, LOCTEXT("ColorRGB", "Color"), FStageParameterValue::MakeColor(GetColorRGB()));
		AddAttribute(Color, StageCraftTags::Attribute_ColorW, LOCTEXT("White", "White"), 100.0, Percent);
	}

	if (Fixture->HasZoom())
	{
		FStageParameterSection& Beam = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Beam, LOCTEXT("BeamSection", "Beam"));
		AddAttribute(Beam, StageCraftTags::Attribute_Zoom, LOCTEXT("Zoom", "Zoom"), 1.0, Degrees);
	}
}

bool ALightingFixtureActor::ReadParameter(const FGameplayTag& ParameterId, FStageParameterValue& OutValue) const
{
	if (ParameterId == StageCraftTags::Attribute_ColorRGB)
	{
		OutValue = FStageParameterValue::MakeColor(GetColorRGB());
		return true;
	}
	if (ParameterId.MatchesTag(StageCraftTags::Attribute) && Attributes.Contains(ParameterId))
	{
		OutValue = FStageParameterValue::MakeFloat(GetAttribute(ParameterId));
		return true;
	}
	if (ParameterId == StageCraftTags::Param_Patch_FixtureId)
	{
		OutValue = FStageParameterValue::MakeInteger(FixtureId);
		return true;
	}
	if (ParameterId == StageCraftTags::Param_Patch_Universe)
	{
		OutValue = FStageParameterValue::MakeInteger(Patch.Universe);
		return true;
	}
	if (ParameterId == StageCraftTags::Param_Patch_Address)
	{
		OutValue = FStageParameterValue::MakeInteger(Patch.Address);
		return true;
	}
	return Super::ReadParameter(ParameterId, OutValue);
}

bool ALightingFixtureActor::WriteParameter(const FGameplayTag& ParameterId, const FStageParameterValue& Value)
{
	if (ParameterId == StageCraftTags::Attribute_ColorRGB && Value.Type == EStageParameterType::Color)
	{
		TMap<FGameplayTag, float> Rgb;
		Rgb.Add(StageCraftTags::Attribute_ColorR, Value.Color.R);
		Rgb.Add(StageCraftTags::Attribute_ColorG, Value.Color.G);
		Rgb.Add(StageCraftTags::Attribute_ColorB, Value.Color.B);
		SetAttributes(Rgb);
		return true;
	}
	if (ParameterId.MatchesTag(StageCraftTags::Attribute) && Value.Type == EStageParameterType::Float && Attributes.Contains(ParameterId))
	{
		SetAttribute(ParameterId, static_cast<float>(Value.Float));
		return true;
	}
	if (Value.Type == EStageParameterType::Integer)
	{
		if (ParameterId == StageCraftTags::Param_Patch_FixtureId)
		{
			return SetFixtureId(Value.Integer);
		}
		if (ParameterId == StageCraftTags::Param_Patch_Universe)
		{
			FStageDMXPatch NewPatch = Patch;
			NewPatch.Universe = Value.Integer;
			SetPatch(NewPatch);
			return true;
		}
		if (ParameterId == StageCraftTags::Param_Patch_Address)
		{
			FStageDMXPatch NewPatch = Patch;
			NewPatch.Address = Value.Integer;
			SetPatch(NewPatch);
			return true;
		}
	}
	return Super::WriteParameter(ParameterId, Value);
}

#undef LOCTEXT_NAMESPACE
