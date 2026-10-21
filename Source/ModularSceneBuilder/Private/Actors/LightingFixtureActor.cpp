// Copyright Epic Games, Inc. All Rights Reserved.

#include "Actors/LightingFixtureActor.h"

#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Data/LightingFixtureData.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Interaction/StageCraftCollision.h"
#include "Materials/MaterialInstanceDynamic.h"
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
		// StageCraft has no navigation. A colliding mesh would otherwise update navigation data on every pan/tilt step,
		// about a fifth of a moving head's game-thread cost (Docs/ADR/0012-production-reports-and-fixture-benchmark.md §3).
		Mesh.SetCanEverAffectNavigation(false);
	}

	bool IsColorAttribute(const FGameplayTag& Attribute)
	{
		return Attribute == StageCraftTags::Attribute_ColorR
			|| Attribute == StageCraftTags::Attribute_ColorG
			|| Attribute == StageCraftTags::Attribute_ColorB;
	}

	/** Parameter names of the beam-shaping light-function material (Scripts/Content/create_lighting_content.py builds it). */
	namespace BeamShapingParams
	{
		const FName GoboTexture(TEXT("GoboTexture"));
		const FName GoboEnabled(TEXT("GoboEnabled"));
		const FName GoboRotation(TEXT("GoboRotation"));
		const FName IrisRadius(TEXT("IrisRadius"));
		const FName BladeTop(TEXT("BladeTop"));
		const FName BladeRight(TEXT("BladeRight"));
		const FName BladeBottom(TEXT("BladeBottom"));
		const FName BladeLeft(TEXT("BladeLeft"));
		const FName BladeRotation(TEXT("BladeRotation"));
	}

	/** Rotations, strobe and tint move or flash the beam but change no photometric value. */
	bool AffectsPhotometrics(const FGameplayTag& Attribute)
	{
		return Attribute != StageCraftTags::Attribute_GoboRotation
			&& Attribute != StageCraftTags::Attribute_BladeRotation
			&& Attribute != StageCraftTags::Attribute_Shutter
			&& Attribute != StageCraftTags::Attribute_ColorTemp;
	}

	bool MovesBeamAxis(const FGameplayTag& Attribute)
	{
		return Attribute == StageCraftTags::Attribute_Pan || Attribute == StageCraftTags::Attribute_Tilt;
	}

	/** Additive white mix, renormalised so full RGB + full W stays a valid (brightest channel = 1) colour. */
	FLinearColor MixBeamColor(const FLinearColor& Rgb, float White)
	{
		FLinearColor Mixed = Rgb + FLinearColor(White, White, White, 0.f);
		const float Peak = Mixed.GetMax();
		if (Peak > 1.f)
		{
			Mixed /= Peak;
		}
		Mixed.A = 1.f;
		return Mixed;
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

	/** Throw, illuminance and beam size need a surface. A beam that hits nothing in range says so instead of showing "0.0 m". */
	bool IsThrowDependentParameter(const FGameplayTag& ParameterId)
	{
		return ParameterId == StageCraftTags::Param_Photo_Throw || ParameterId == StageCraftTags::Param_Photo_Illuminance
			|| ParameterId == StageCraftTags::Param_Photo_BeamDiameter;
	}

	FText FormatThrowDependent(const StagePhotometry::FResult& Photo, const FGameplayTag& ParameterId)
	{
		if (!Photo.bHasThrow)
		{
			return LOCTEXT("PhotoNoSurface", "No surface");
		}

		FNumberFormattingOptions Options;
		if (ParameterId == StageCraftTags::Param_Photo_Illuminance)
		{
			Options.SetMaximumFractionalDigits(0);
			return FText::Format(LOCTEXT("PhotoLux", "{0} lx"), FText::AsNumber(Photo.IlluminanceLux, &Options));
		}
		// Centimetre precision, as a rigging plot needs.
		Options.SetMinimumFractionalDigits(2).SetMaximumFractionalDigits(2);
		const double Metres = ParameterId == StageCraftTags::Param_Photo_Throw ? Photo.ThrowMetres : Photo.BeamDiameterMetres;
		return FText::Format(LOCTEXT("PhotoMetres", "{0} m"), FText::AsNumber(Metres, &Options));
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
	// Item data (and a restored Shutter) was applied before play, when there was no strobe clock to join yet.
	UpdateStrobeRegistration();
}

void ALightingFixtureActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UShowControlSubsystem* ShowControl = GetShowControl())
	{
		// Unregistering also leaves the strobe clock.
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
	CachedThrowCm.Reset();

	// A swapped-in model without beam shaping must not keep projecting the previous model's gobo.
	if (!Fixture->HasBeamShaping())
	{
		BeamLight->SetLightFunctionMaterial(nullptr);
		BeamShapingInstance = nullptr;
	}
	AppliedGoboSlot = INDEX_NONE;

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
	SanitizeRestoredState(*Fixture);

	ApplyAttributesToComponents();
	UpdateStrobeRegistration();
}

void ALightingFixtureActor::UpdateHighlight()
{
	Super::UpdateHighlight();

	UMaterialInterface* Overlay = MeshComponent->GetOverlayMaterial();
	YokeMeshComponent->SetOverlayMaterial(Overlay);
	HeadMeshComponent->SetOverlayMaterial(Overlay);
}

void ALightingFixtureActor::SanitizeRestoredState(const ULightingFixtureData& Fixture)
{
	// Values arrive unchecked from scene files and undo snapshots; the same limits as every live write apply
	// (Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md §3.2).
	for (TPair<FGameplayTag, float>& Pair : Attributes)
	{
		float Min = 0.f;
		float Max = 1.f;
		if (!Fixture.GetAttributeRange(Pair.Key, Min, Max))
		{
			continue;
		}
		const float Sanitized = FMath::IsFinite(Pair.Value) ? SanitizeAttributeValue(Pair.Key, Pair.Value, Min, Max) : Fixture.GetAttributeDefault(Pair.Key);
		if (!FMath::IsNearlyEqual(Pair.Value, Sanitized) || !FMath::IsFinite(Pair.Value))
		{
			ReportStateRepair(FString::Printf(TEXT("%s %s -> %s"), *Pair.Key.ToString(), *FString::SanitizeFloat(Pair.Value), *FString::SanitizeFloat(Sanitized)));
			Pair.Value = Sanitized;
		}
	}

	const FStageDMXPatch ClampedPatch = ClampPatch(Patch);
	if (!(ClampedPatch == Patch))
	{
		ReportStateRepair(FString::Printf(TEXT("DMX patch %d.%d -> %d.%d"), Patch.Universe, Patch.Address, ClampedPatch.Universe, ClampedPatch.Address));
		Patch = ClampedPatch;
	}
	if (FixtureId < 0)
	{
		ReportStateRepair(FString::Printf(TEXT("fixture ID %d -> automatic"), FixtureId));
		FixtureId = 0;
	}
}

float ALightingFixtureActor::SanitizeAttributeValue(const FGameplayTag& Attribute, float Value, float Min, float Max)
{
	const float Clamped = FMath::Clamp(Value, Min, Max);
	return ULightingFixtureData::IsSteppedAttribute(Attribute) ? FMath::RoundHalfFromZero(Clamped) : Clamped;
}

FStageDMXPatch ALightingFixtureActor::ClampPatch(const FStageDMXPatch& InPatch)
{
	FStageDMXPatch Clamped;
	Clamped.Universe = FMath::Clamp(InPatch.Universe, 0, 63999);
	Clamped.Address = FMath::Clamp(InPatch.Address, 0, 512);
	return Clamped;
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

	if (!FMath::IsFinite(Value))
	{
		return false;
	}

	float& Stored = Attributes.FindOrAdd(Attribute);
	const float Clamped = SanitizeAttributeValue(Attribute, Value, Min, Max);
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
	if (MovesBeamAxis(Attribute))
	{
		CachedThrowCm.Reset();
	}
	if (AffectsPhotometrics(Attribute))
	{
		NotifyPhotometricsChanged();
	}
}

void ALightingFixtureActor::NotifyPhotometricsChanged()
{
	NotifyParameterChanged(StageCraftTags::Param_Photo_BeamAngle);
	NotifyParameterChanged(StageCraftTags::Param_Photo_Intensity);
	NotifyParameterChanged(StageCraftTags::Param_Photo_Throw);
	NotifyParameterChanged(StageCraftTags::Param_Photo_Illuminance);
	NotifyParameterChanged(StageCraftTags::Param_Photo_BeamDiameter);
}

void ALightingFixtureActor::OnStageTransformChanged()
{
	Super::OnStageTransformChanged();
	CachedThrowCm.Reset();
	NotifyPhotometricsChanged();
}

bool ALightingFixtureActor::SetAttribute(FGameplayTag Attribute, float Value)
{
	if (!StoreAttribute(Attribute, Value))
	{
		return false;
	}
	ApplyOutputs(GetDrivenOutputs(Attribute));
	if (Attribute == StageCraftTags::Attribute_Shutter)
	{
		UpdateStrobeRegistration();
	}
	NotifyAttributeChanged(Attribute);
	return true;
}

bool ALightingFixtureActor::SetAttributes(const TMap<FGameplayTag, float>& Values)
{
	TArray<FGameplayTag, TInlineAllocator<16>> Changed;
	EFixtureOutputs Outputs = EFixtureOutputs::None;
	for (const TPair<FGameplayTag, float>& Pair : Values)
	{
		if (StoreAttribute(Pair.Key, Pair.Value))
		{
			Changed.Add(Pair.Key);
			Outputs |= GetDrivenOutputs(Pair.Key);
		}
	}

	if (Changed.IsEmpty())
	{
		return false;
	}

	ApplyOutputs(Outputs);
	if (Changed.Contains(StageCraftTags::Attribute_Shutter))
	{
		UpdateStrobeRegistration();
	}

	// Composite and derived rows are announced once per frame of a fade, not once per changed attribute.
	bool bColorNotified = false;
	bool bPhotometricsNotified = false;
	for (const FGameplayTag& Attribute : Changed)
	{
		NotifyParameterChanged(Attribute);
		bColorNotified |= IsColorAttribute(Attribute);
		bPhotometricsNotified |= AffectsPhotometrics(Attribute);
		if (MovesBeamAxis(Attribute))
		{
			CachedThrowCm.Reset();
		}
	}
	if (bColorNotified)
	{
		NotifyParameterChanged(StageCraftTags::Attribute_ColorRGB);
	}
	if (bPhotometricsNotified)
	{
		NotifyPhotometricsChanged();
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
	ApplyOutputs(EFixtureOutputs::All);
}

ALightingFixtureActor::EFixtureOutputs ALightingFixtureActor::GetDrivenOutputs(const FGameplayTag& Attribute)
{
	if (MovesBeamAxis(Attribute))
	{
		return EFixtureOutputs::Pivots;
	}
	// The shutter drives the output through the strobe clock (bStrobeOpen), applied with the dimmer level.
	if (Attribute == StageCraftTags::Attribute_Dimmer || Attribute == StageCraftTags::Attribute_Shutter)
	{
		return EFixtureOutputs::Intensity;
	}
	if (Attribute == StageCraftTags::Attribute_Zoom)
	{
		return EFixtureOutputs::Cone;
	}
	if (IsColorAttribute(Attribute) || Attribute == StageCraftTags::Attribute_ColorW)
	{
		return EFixtureOutputs::Color;
	}
	if (Attribute == StageCraftTags::Attribute_ColorTemp)
	{
		return EFixtureOutputs::Temperature;
	}
	if (Attribute == StageCraftTags::Attribute_Gobo || Attribute == StageCraftTags::Attribute_GoboRotation || Attribute == StageCraftTags::Attribute_Iris
		|| Attribute == StageCraftTags::Attribute_BladeA || Attribute == StageCraftTags::Attribute_BladeB || Attribute == StageCraftTags::Attribute_BladeC
		|| Attribute == StageCraftTags::Attribute_BladeD || Attribute == StageCraftTags::Attribute_BladeRotation)
	{
		return EFixtureOutputs::BeamShaping;
	}
	// An attribute this list does not know yet still reaches the components.
	return EFixtureOutputs::All;
}

void ALightingFixtureActor::ApplyOutputs(EFixtureOutputs Outputs)
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	if (!Fixture)
	{
		BeamLight->SetIntensity(0.f);
		return;
	}

	if (EnumHasAnyFlags(Outputs, EFixtureOutputs::Pivots) && Fixture->bHasPanTilt)
	{
		// Both rotations first, then one transform update from the pan pivot down: setting them one after the other moved the
		// head, the lens and the beam twice per change.
		TiltPivot->SetRelativeRotation_Direct(FRotator(GetAttribute(StageCraftTags::Attribute_Tilt), 0.0, 0.0));
		PanPivot->SetRelativeRotation_Direct(FRotator(0.0, GetAttribute(StageCraftTags::Attribute_Pan), 0.0));
		PanPivot->UpdateComponentToWorld();

		// Engine cost workaround (UE 5.8, measured with 500 moving heads): a moved light goes through the render thread's
		// per-light UpdateLightTransform, about 87 us per light per frame, while a light whose render state is rebuilt goes
		// through the batched remove/add, about 11 us. A pending rebuild replaces the transform update, so moving heads take
		// the batched path: 500 fixtures panning went from 58 to 27 ms a frame. The light is rebuilt anyway on every zoom
		// change (USpotLightComponent::SetOuterConeAngle). Docs/ADR/0012-production-reports-and-fixture-benchmark.md §3.
		BeamLight->MarkRenderStateDirty();
	}

	if (EnumHasAnyFlags(Outputs, EFixtureOutputs::Intensity))
	{
		// The strobe clock closes the output between flashes; the dimmer level is kept for the next flash.
		BeamLight->SetIntensity(bStrobeOpen ? Fixture->LuminousFlux * GetAttribute(StageCraftTags::Attribute_Dimmer) : 0.f);
	}

	if (EnumHasAnyFlags(Outputs, EFixtureOutputs::Cone))
	{
		// The cone is the zoom; the iris and the blades crop inside it (the light function), so the projection scales with zoom.
		const float HalfAngle = 0.5f * (Fixture->HasZoom() ? GetAttribute(StageCraftTags::Attribute_Zoom) : Fixture->BeamAngleMax);
		BeamLight->SetOuterConeAngle(HalfAngle);
		BeamLight->SetInnerConeAngle(HalfAngle * 0.7f);
	}

	if (EnumHasAnyFlags(Outputs, EFixtureOutputs::Color) && Fixture->ColorSystem != EStageFixtureColorSystem::None)
	{
		BeamLight->SetLightColor(MixBeamColor(GetColorRGB(), GetAttribute(StageCraftTags::Attribute_ColorW)), /*bSRGB*/ false);
	}

	if (EnumHasAnyFlags(Outputs, EFixtureOutputs::Temperature))
	{
		// The light's temperature tints relative to 6500 K: CTC on colour mixers, the fixed source temperature on white-only fixtures.
		if (Fixture->HasColorTemperatureControl())
		{
			BeamLight->SetUseTemperature(true);
			BeamLight->SetTemperature(GetAttribute(StageCraftTags::Attribute_ColorTemp));
		}
		else
		{
			BeamLight->SetUseTemperature(Fixture->ColorSystem == EStageFixtureColorSystem::None);
			BeamLight->SetTemperature(Fixture->ColorTemperatureK);
		}
	}

	if (EnumHasAnyFlags(Outputs, EFixtureOutputs::BeamShaping) && Fixture->HasBeamShaping())
	{
		ApplyBeamShaping(*Fixture);
	}
}

void ALightingFixtureActor::ApplyBeamShaping(const ULightingFixtureData& Fixture)
{
	const int32 GoboSlot = FMath::RoundToInt32(GetAttribute(StageCraftTags::Attribute_Gobo));
	const float IrisRadius = Fixture.bHasIris ? FMath::Lerp(1.f, Fixture.IrisMinFraction, GetAttribute(StageCraftTags::Attribute_Iris)) : 1.f;
	const bool bShaping = GoboSlot != 0 || IrisRadius < 1.f
		|| GetAttribute(StageCraftTags::Attribute_BladeA) > 0.f || GetAttribute(StageCraftTags::Attribute_BladeB) > 0.f
		|| GetAttribute(StageCraftTags::Attribute_BladeC) > 0.f || GetAttribute(StageCraftTags::Attribute_BladeD) > 0.f;
	if (!bShaping)
	{
		// An open beam projects nothing, and a light function is a full extra lighting pass per light: with 500 fixtures it
		// was two thirds of the GPU frame (Docs/ADR/0012-production-reports-and-fixture-benchmark.md §3). The instance is kept.
		if (BeamLight->LightFunctionMaterial)
		{
			BeamLight->SetLightFunctionMaterial(nullptr);
		}
		return;
	}

	if (!BeamShapingInstance)
	{
		if (bBeamShapingUnavailable)
		{
			return;
		}
		UMaterialInterface* Parent = BeamShapingMaterial.LoadSynchronous();
		if (!Parent)
		{
			// Once per fixture: attribute changes would otherwise repeat it every frame of a fade.
			bBeamShapingUnavailable = true;
			UE_LOG(LogStageCraft, Warning, TEXT("%s: BeamShapingMaterial is not set ([/Script/ModularSceneBuilder.LightingFixtureActor] in DefaultGame.ini); gobo, iris and blades are not projected."),
				*GetName());
			return;
		}
		BeamShapingInstance = UMaterialInstanceDynamic::Create(Parent, this);
		AppliedGoboSlot = INDEX_NONE;
	}
	if (BeamLight->LightFunctionMaterial != BeamShapingInstance)
	{
		BeamLight->SetLightFunctionMaterial(BeamShapingInstance);
	}

	// The texture is swapped only when the slot changes; the scalars are cheap to set on every attribute change.
	if (GoboSlot != AppliedGoboSlot)
	{
		UTexture2D* GoboTexture = Fixture.LoadGoboTexture(GoboSlot);
		if (GoboTexture)
		{
			BeamShapingInstance->SetTextureParameterValue(BeamShapingParams::GoboTexture, GoboTexture);
		}
		BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::GoboEnabled, GoboTexture ? 1.f : 0.f);
		AppliedGoboSlot = GoboSlot;
	}
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::GoboRotation, GetAttribute(StageCraftTags::Attribute_GoboRotation));
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::IrisRadius, IrisRadius);
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::BladeTop, GetAttribute(StageCraftTags::Attribute_BladeA));
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::BladeRight, GetAttribute(StageCraftTags::Attribute_BladeB));
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::BladeBottom, GetAttribute(StageCraftTags::Attribute_BladeC));
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::BladeLeft, GetAttribute(StageCraftTags::Attribute_BladeD));
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::BladeRotation, GetAttribute(StageCraftTags::Attribute_BladeRotation));
}

// --- Strobe ---

float ALightingFixtureActor::GetStrobeRateHz() const
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	const float Shutter = GetAttribute(StageCraftTags::Attribute_Shutter);
	if (!Fixture || !Fixture->bHasShutter || Shutter <= 0.f)
	{
		return 0.f;
	}
	return FMath::Lerp(static_cast<float>(Fixture->StrobeRateHz.X), static_cast<float>(Fixture->StrobeRateHz.Y), FMath::Clamp(Shutter, 0.f, 1.f));
}

void ALightingFixtureActor::SetStrobeOpen(bool bOpen)
{
	if (bStrobeOpen == bOpen)
	{
		return;
	}
	bStrobeOpen = bOpen;
	// Only the output changes; nothing the inspector shows, so no parameter notification.
	const ULightingFixtureData* Fixture = GetFixtureData();
	BeamLight->SetIntensity(bStrobeOpen && Fixture ? Fixture->LuminousFlux * GetAttribute(StageCraftTags::Attribute_Dimmer) : 0.f);
}

void ALightingFixtureActor::UpdateStrobeRegistration()
{
	// Before BeginPlay (deferred spawn, editor construction) the subsystem may not exist; BeginPlay's data apply registers then.
	if (UShowControlSubsystem* ShowControl = HasActorBegunPlay() ? GetShowControl() : nullptr)
	{
		ShowControl->SetFixtureStrobing(this, GetStrobeRateHz() > 0.f);
	}
}

// --- Photometrics ---

double ALightingFixtureActor::TraceThrow() const
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	const UWorld* World = GetWorld();
	if (!Fixture || !World)
	{
		return 0.0;
	}

	const FVector Start = BeamLight->GetComponentLocation();
	const FVector End = Start + BeamLight->GetForwardVector() * Fixture->BeamRange;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(StageFixtureThrow), /*bTraceComplex*/ false, this);
	FHitResult Hit;
	return World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params) ? Hit.Distance : 0.0;
}

StagePhotometry::FResult ALightingFixtureActor::ComputePhotometrics() const
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	if (!Fixture)
	{
		return StagePhotometry::FResult();
	}

	const double Zoom = Fixture->HasZoom() ? GetAttribute(StageCraftTags::Attribute_Zoom) : Fixture->BeamAngleMax;
	const double Iris = Fixture->bHasIris ? GetAttribute(StageCraftTags::Attribute_Iris) : 0.0;

	double Transmission = GetAttribute(StageCraftTags::Attribute_Dimmer)
		* StagePhotometry::GetIrisTransmission(Iris, Fixture->IrisMinFraction)
		* Fixture->GetGoboTransmission(FMath::RoundToInt32(GetAttribute(StageCraftTags::Attribute_Gobo)));
	if (Fixture->ColorSystem != EStageFixtureColorSystem::None)
	{
		Transmission *= StagePhotometry::GetColorTransmission(MixBeamColor(GetColorRGB(), GetAttribute(StageCraftTags::Attribute_ColorW)));
	}
	if (Fixture->bHasFramingShutters)
	{
		Transmission *= StagePhotometry::GetShutterOpenFraction(GetAttribute(StageCraftTags::Attribute_BladeA), GetAttribute(StageCraftTags::Attribute_BladeB),
			GetAttribute(StageCraftTags::Attribute_BladeC), GetAttribute(StageCraftTags::Attribute_BladeD));
	}

	if (!CachedThrowCm.IsSet())
	{
		CachedThrowCm = TraceThrow();
	}

	StagePhotometry::FInputs Inputs;
	Inputs.LuminousFluxLm = Fixture->LuminousFlux;
	Inputs.BeamAngleDegrees = StagePhotometry::ApplyIris(Zoom, Iris, Fixture->IrisMinFraction);
	Inputs.Transmission = Transmission;
	Inputs.ThrowCm = CachedThrowCm.GetValue();
	return StagePhotometry::Compute(Inputs);
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
	const FStageDMXPatch Clamped = ClampPatch(NewPatch);
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
		if (!Fixture->SupportsAttribute(Channel.Attribute) || !Fixture->SupportsAttribute(Attribute))
		{
			continue;
		}

		const int32 Index = Patch.Address - 1 + Channel.Channel - 1;
		if (Index < 0 || Index + Channel.GetWidth() > UniverseData.Num())
		{
			continue;
		}

		float Normalized = Fixture->NormalizeForDMX(Attribute, GetAttribute(Attribute));
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
		if (!Fixture->SupportsAttribute(Channel.Attribute) || !Fixture->SupportsAttribute(Attribute))
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

		Values.Add(Attribute, Fixture->DenormalizeFromDMX(Attribute, Normalized));
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
	PatchSection.Add(StageCraftTags::Param_Patch_Mode, LOCTEXT("DMXMode", "DMX Mode"), FStageParameterValue::MakeText(FText::FromName(Fixture->DMXModeName))).ReadOnly();
	PatchSection.Add(StageCraftTags::Param_Patch_Footprint, LOCTEXT("Footprint", "Channels"),
		FStageParameterValue::MakeInteger(Fixture->GetDMXFootprint())).Display(1.0, LOCTEXT("ChannelUnits", "ch")).ReadOnly();

	auto AddAttribute = [this, Fixture](FStageParameterSection& Section, const FGameplayTag& Attribute, const FText& Name, double DisplayScale, const FText& Units, const FText& ShortName = FText::GetEmpty())
	{
		float Min = 0.f;
		float Max = 1.f;
		if (Fixture->GetAttributeRange(Attribute, Min, Max))
		{
			Section.Add(Attribute, Name, FStageParameterValue::MakeFloat(GetAttribute(Attribute))).Range(Min, Max).Display(DisplayScale, Units)
				.WithDefault(FStageParameterValue::MakeFloat(Fixture->GetAttributeDefault(Attribute))).Abbreviated(ShortName);
		}
	};

	const FText Percent = LOCTEXT("Percent", "%");
	const FText Degrees = LOCTEXT("Degrees", "°");

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
		AddAttribute(Color, StageCraftTags::Attribute_ColorTemp, LOCTEXT("ColorTemp", "Color Temp"), 1.0, LOCTEXT("Kelvin", "K"), LOCTEXT("ColorTempShort", "CTC"));
	}

	if (Fixture->HasZoom() || Fixture->bHasIris || !Fixture->GoboWheel.IsEmpty())
	{
		FStageParameterSection& Beam = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Beam, LOCTEXT("BeamSection", "Beam"));
		AddAttribute(Beam, StageCraftTags::Attribute_Zoom, LOCTEXT("Zoom", "Zoom"), 1.0, Degrees);
		AddAttribute(Beam, StageCraftTags::Attribute_Iris, LOCTEXT("Iris", "Iris"), 100.0, Percent);
		if (!Fixture->GoboWheel.IsEmpty())
		{
			// A wheel is picked by slot, so it is a dropdown here; cues and DMX see the slot index.
			TArray<FText> Slots;
			Slots.Reserve(Fixture->GetGoboSlotCount());
			for (int32 Slot = 0; Slot < Fixture->GetGoboSlotCount(); ++Slot)
			{
				Slots.Add(Fixture->GetGoboSlotName(Slot));
			}
			Beam.Add(StageCraftTags::Attribute_Gobo, LOCTEXT("Gobo", "Gobo"), FStageParameterValue::MakeEnum(FMath::RoundToInt32(GetAttribute(StageCraftTags::Attribute_Gobo))))
				.WithOptions(MoveTemp(Slots));
			AddAttribute(Beam, StageCraftTags::Attribute_GoboRotation, LOCTEXT("GoboRotation", "Gobo Rotate"), 1.0, Degrees, LOCTEXT("GoboRotationShort", "Gobo Rot"));
		}
	}

	if (Fixture->bHasFramingShutters)
	{
		FStageParameterSection& Shapers = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Shapers, LOCTEXT("ShapersSection", "Shapers"));
		AddAttribute(Shapers, StageCraftTags::Attribute_BladeA, LOCTEXT("BladeA", "Blade Top"), 100.0, Percent, LOCTEXT("BladeAShort", "Blade T"));
		AddAttribute(Shapers, StageCraftTags::Attribute_BladeB, LOCTEXT("BladeB", "Blade Right"), 100.0, Percent, LOCTEXT("BladeBShort", "Blade R"));
		AddAttribute(Shapers, StageCraftTags::Attribute_BladeC, LOCTEXT("BladeC", "Blade Bottom"), 100.0, Percent, LOCTEXT("BladeCShort", "Blade B"));
		AddAttribute(Shapers, StageCraftTags::Attribute_BladeD, LOCTEXT("BladeD", "Blade Left"), 100.0, Percent, LOCTEXT("BladeDShort", "Blade L"));
		AddAttribute(Shapers, StageCraftTags::Attribute_BladeRotation, LOCTEXT("BladeRotation", "Shaper Rotate"), 1.0, Degrees, LOCTEXT("BladeRotationShort", "Shp Rot"));
	}

	// Approximate photometrics: read-only, refreshed by the same events as the attributes and position they follow.
	const StagePhotometry::FResult Photo = ComputePhotometrics();
	FStageParameterSection& Photometrics = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Photometrics, LOCTEXT("PhotometricsSection", "Photometrics"));
	Photometrics.Add(StageCraftTags::Param_Photo_BeamAngle, LOCTEXT("PhotoBeamAngle", "Beam Angle"), FStageParameterValue::MakeFloat(Photo.BeamAngleDegrees))
		.Display(1.0, Degrees).ReadOnly();
	Photometrics.Add(StageCraftTags::Param_Photo_Intensity, LOCTEXT("PhotoIntensity", "Intensity"), FStageParameterValue::MakeFloat(Photo.IntensityCd))
		.Display(0.001, LOCTEXT("Kilocandela", "kcd")).ReadOnly();
	Photometrics.Add(StageCraftTags::Param_Photo_Throw, LOCTEXT("PhotoThrow", "Throw"),
		FStageParameterValue::MakeText(FormatThrowDependent(Photo, StageCraftTags::Param_Photo_Throw))).ReadOnly();
	Photometrics.Add(StageCraftTags::Param_Photo_Illuminance, LOCTEXT("PhotoIlluminance", "Illuminance"),
		FStageParameterValue::MakeText(FormatThrowDependent(Photo, StageCraftTags::Param_Photo_Illuminance))).ReadOnly();
	Photometrics.Add(StageCraftTags::Param_Photo_BeamDiameter, LOCTEXT("PhotoBeamDiameter", "Beam Size"),
		FStageParameterValue::MakeText(FormatThrowDependent(Photo, StageCraftTags::Param_Photo_BeamDiameter))).ReadOnly();
}

bool ALightingFixtureActor::ReadParameter(const FGameplayTag& ParameterId, FStageParameterValue& OutValue) const
{
	if (ParameterId == StageCraftTags::Attribute_ColorRGB)
	{
		OutValue = FStageParameterValue::MakeColor(GetColorRGB());
		return true;
	}
	if (ParameterId == StageCraftTags::Attribute_Gobo && Attributes.Contains(ParameterId))
	{
		// Matches the dropdown row; the stored attribute is the same slot index as a float.
		OutValue = FStageParameterValue::MakeEnum(FMath::RoundToInt32(GetAttribute(ParameterId)));
		return true;
	}
	if (StageCraftTags::IsPhotometricParameter(ParameterId))
	{
		const StagePhotometry::FResult Photo = ComputePhotometrics();
		OutValue = IsThrowDependentParameter(ParameterId)
			? FStageParameterValue::MakeText(FormatThrowDependent(Photo, ParameterId))
			: FStageParameterValue::MakeFloat(ParameterId == StageCraftTags::Param_Photo_BeamAngle ? Photo.BeamAngleDegrees : Photo.IntensityCd);
		return GetFixtureData() != nullptr;
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
	if (ParameterId == StageCraftTags::Attribute_Gobo && Value.Type == EStageParameterType::Enum && Attributes.Contains(ParameterId))
	{
		SetAttribute(ParameterId, static_cast<float>(Value.Integer));
		return true;
	}
	if (StageCraftTags::IsPhotometricParameter(ParameterId))
	{
		// Derived, never written.
		return false;
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
