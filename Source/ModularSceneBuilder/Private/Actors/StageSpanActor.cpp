// Copyright Epic Games, Inc. All Rights Reserved.

#include "Actors/StageSpanActor.h"

#include "Components/StageSpanGeometryComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Data/StageSpanData.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSpanActor)

#define LOCTEXT_NAMESPACE "StageCraftStageSpanActor"

namespace StageSpanActor
{
	constexpr double CmToMetres = 0.01;

	/** Inspector drag step for stretched runs, in metres. Whole-segment runs step by one segment. */
	constexpr double StretchStepMetres = 0.1;
}

AStageSpanActor::AStageSpanActor()
{
	Geometry = CreateDefaultSubobject<UStageSpanGeometryComponent>(TEXT("Geometry"));
	Geometry->SetupAttachment(MeshComponent);
	Geometry->SetUsage(EStageSpanGeometryUsage::PlacedItem);
}

UStageSpanData* AStageSpanActor::GetSpanData() const
{
	return Cast<UStageSpanData>(ItemData);
}

FStageSpanLayout AStageSpanActor::ComputeLayoutFor(double InLength) const
{
	const UStageSpanData* Span = GetSpanData();
	return Span ? StageSpanMath::ComputeLayout(StageSpanMath::MakeRules(*Span), InLength) : FStageSpanLayout();
}

FStageSpanLayout AStageSpanActor::GetLayout() const
{
	return ComputeLayoutFor(SpanLength);
}

int32 AStageSpanActor::GetQuantity() const
{
	const FStageSpanLayout Layout = GetLayout();
	return Layout.IsValid() ? Layout.SegmentCount : Super::GetQuantity();
}

int32 AStageSpanActor::PredictQuantity(const FGameplayTag& ParameterId, const FStageParameterValue& Value) const
{
	if (ParameterId == StageCraftTags::Param_Span_Length && Value.Type == EStageParameterType::Float)
	{
		const FStageSpanLayout Layout = ComputeLayoutFor(Value.Float);
		return Layout.IsValid() ? Layout.SegmentCount : GetQuantity();
	}
	return Super::PredictQuantity(ParameterId, Value);
}

int32 AStageSpanActor::GetGeometryInstanceCount() const
{
	return Geometry->GetInstanceCount();
}

void AStageSpanActor::InitializeSpanLength(double InLength)
{
	if (!ensureMsgf(!HasActorBegunPlay(), TEXT("%s: InitializeSpanLength is for runs being spawned; edit placed runs through the Length parameter."), *GetName()))
	{
		return;
	}
	SetSpanLength(InLength);
}

void AStageSpanActor::SetSpanLength(double InLength)
{
	// Stored normalised, so the saved value is always the run that was built.
	const FStageSpanLayout Layout = ComputeLayoutFor(InLength);
	SpanLength = Layout.IsValid() ? Layout.Length : FMath::Max(0.0, FMath::IsFinite(InLength) ? InLength : 0.0);
	RebuildGeometry();
}

void AStageSpanActor::ApplyItemData(const UBaseItemData& Data)
{
	Super::ApplyItemData(Data);

	// A swapped item has its own segment length and limits; a loaded length may be out of range. Both normalise here.
	SetSpanLength(SpanLength);
}

void AStageSpanActor::RebuildGeometry()
{
	Geometry->Build(GetSpanData(), GetLayout());
	// Parts created by this build start without an overlay.
	Geometry->SetOverlayMaterial(GetHighlightOverlay());
}

void AStageSpanActor::UpdateHighlight()
{
	Super::UpdateHighlight();
	Geometry->SetOverlayMaterial(GetHighlightOverlay());
}

void AStageSpanActor::GatherParameterSections(TArray<FStageParameterSection>& OutSections) const
{
	Super::GatherParameterSections(OutSections);

	const UStageSpanData* Span = GetSpanData();
	if (!Span)
	{
		return;
	}

	using namespace StageSpanActor;
	const StageSpanMath::FRules Rules = StageSpanMath::MakeRules(*Span);
	const FStageSpanLayout Layout = GetLayout();
	const bool bWholeSegments = Span->Fit == EStageSpanFit::WholeSegments;

	FStageParameterSection& Section = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Span, LOCTEXT("SpanSection", "Span"));
	Section.Add(StageCraftTags::Param_Span_Length, LOCTEXT("Length", "Length"), FStageParameterValue::MakeFloat(Layout.Length))
		.Range(StageSpanMath::GetMinLength(Rules), StageSpanMath::GetMaxLength(Rules))
		.Display(CmToMetres, LOCTEXT("Metres", "m"), bWholeSegments ? Span->SegmentLength * CmToMetres : StretchStepMetres);
	Section.Add(StageCraftTags::Param_Span_Segments, LOCTEXT("Segments", "Segments"), FStageParameterValue::MakeInteger(Layout.SegmentCount)).ReadOnly();
	Section.Add(StageCraftTags::Param_Span_SegmentLength, LOCTEXT("SegmentLength", "Segment"), FStageParameterValue::MakeFloat(Layout.SegmentPitch))
		.Display(CmToMetres, LOCTEXT("Metres", "m")).ReadOnly();
	Section.Add(StageCraftTags::Param_Span_Fit, LOCTEXT("Fit", "Fit"),
		FStageParameterValue::MakeText(StaticEnum<EStageSpanFit>()->GetDisplayNameTextByValue(int64(Span->Fit)))).ReadOnly();
}

bool AStageSpanActor::ReadParameter(const FGameplayTag& ParameterId, FStageParameterValue& OutValue) const
{
	const FStageSpanLayout Layout = GetLayout();
	if (ParameterId == StageCraftTags::Param_Span_Length)
	{
		OutValue = FStageParameterValue::MakeFloat(Layout.Length);
		return Layout.IsValid();
	}
	if (ParameterId == StageCraftTags::Param_Span_Segments)
	{
		OutValue = FStageParameterValue::MakeInteger(Layout.SegmentCount);
		return Layout.IsValid();
	}
	if (ParameterId == StageCraftTags::Param_Span_SegmentLength)
	{
		OutValue = FStageParameterValue::MakeFloat(Layout.SegmentPitch);
		return Layout.IsValid();
	}
	return Super::ReadParameter(ParameterId, OutValue);
}

bool AStageSpanActor::WriteParameter(const FGameplayTag& ParameterId, const FStageParameterValue& Value)
{
	if (ParameterId == StageCraftTags::Param_Span_Length)
	{
		if (Value.Type != EStageParameterType::Float || !FMath::IsFinite(Value.Float) || !GetSpanData())
		{
			return false;
		}
		SetSpanLength(Value.Float);
		// The count and pitch follow the length; their rows refresh with it.
		NotifyParameterChanged(StageCraftTags::Param_Span_Length);
		NotifyParameterChanged(StageCraftTags::Param_Span_Segments);
		NotifyParameterChanged(StageCraftTags::Param_Span_SegmentLength);
		return true;
	}
	return Super::WriteParameter(ParameterId, Value);
}

#undef LOCTEXT_NAMESPACE
