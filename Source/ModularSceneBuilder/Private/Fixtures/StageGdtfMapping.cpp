// Copyright Epic Games, Inc. All Rights Reserved.

#include "Fixtures/StageGdtfMapping.h"

#include "Data/StageItemTypes.h"
#include "Data/StageParameterTypes.h"
#include "Fixtures/StageGdtfParser.h"
#include "Fixtures/StageGdtfUnits.h"

int32 FStageGdtfModeLayout::CountStatus(EStageGdtfChannelStatus Status) const
{
	return Channels.FilterByPredicate([Status](const FStageGdtfLayoutChannel& Channel) { return Channel.Status == Status; }).Num();
}

FString FStageGdtfImportReport::ToText() const
{
	TArray<FString> Out;
	Out.Add(Title);
	Out.Append(Lines);
	for (const FString& Warning : Warnings)
	{
		Out.Add(TEXT("Warning: ") + Warning);
	}
	for (const FString& Error : Errors)
	{
		Out.Add(TEXT("Error: ") + Error);
	}
	return FString::Join(Out, TEXT("\n"));
}

namespace StageGdtfMapping
{
	namespace
	{
		bool IsOpenSlot(const FStageGdtfWheelSlot& Slot)
		{
			return Slot.PrismFacets == 0 && Slot.MediaFileName.IsEmpty()
				&& (Slot.Name.IsEmpty() || Slot.Name.Contains(TEXT("Open")) || Slot.Name.Equals(TEXT("Out"), ESearchCase::IgnoreCase));
		}

		int32 FindOpenSlot(const FStageGdtfWheel& Wheel)
		{
			return Wheel.Slots.IndexOfByPredicate(&IsOpenSlot);
		}

		/** One function of a channel with its DMX span in the channel's resolution. */
		struct FSpan
		{
			const FStageGdtfChannelFunction* Function = nullptr;
			int64 From = 0;
			int64 To = 0;
		};

		struct FChannelInstance
		{
			const FStageGdtfDMXChannel* Source = nullptr;
			TArray<int32> Offsets;
			int32 Cell = INDEX_NONE;
		};

		/** The attribute a function drives on this instance: the cell's control on a repeated geometry, else the fixture's. */
		FGameplayTag ResolveAttribute(const FStageGdtfAttributeMapping& Mapping, int32 Cell)
		{
			if (Cell == INDEX_NONE)
			{
				return Mapping.Attribute;
			}
			return Mapping.CellChannel == EStageCellChannel::Count ? FGameplayTag() : StageFixtureCells::GetCellAttribute(Cell, Mapping.CellChannel);
		}

		/** The slot ranges of a wheel function from its channel sets. False when it has none to use. */
		bool AddSlotRanges(const FStageGdtfFixtureType& Type, const FSpan& Span, int32 Bytes, const FGameplayTag& Owner, TArray<FStageGdtfRange>& Ranges, FString& Note)
		{
			const FStageGdtfWheel* Wheel = Type.FindWheel(Span.Function->Wheel);
			const TArray<FStageGdtfChannelSet>& Sets = Span.Function->Sets;
			if (!Wheel || Sets.IsEmpty())
			{
				return false;
			}
			int64 Previous = Span.From - 1;
			TArray<FStageGdtfRange> Added;
			for (int32 Index = 0; Index < Sets.Num(); ++Index)
			{
				const int64 From = Sets[Index].DMXFrom.bSet ? StageGdtf::ToChannelValue(Sets[Index].DMXFrom, Bytes) : Span.From;
				if (From <= Previous || From > Span.To)
				{
					Note = FString::Printf(TEXT("%s: channel sets are out of order; the wheel is not driven"), *Span.Function->Name);
					return false;
				}
				Previous = From;
				FStageGdtfRange& Range = Added.AddDefaulted_GetRef();
				Range.DMXFrom = From;
				Range.Label = Sets[Index].Name;
				const int32 Slot = Sets[Index].WheelSlotIndex > 0 ? MapWheelSlot(*Wheel, Sets[Index].WheelSlotIndex) : INDEX_NONE;
				if (Slot != INDEX_NONE)
				{
					Range.Attribute = Owner;
					Range.Slot = Slot;
					Range.ValueFrom = Range.ValueTo = float(Slot);
				}
			}
			if (Added[0].DMXFrom > Span.From)
			{
				FStageGdtfRange Lead;
				Lead.DMXFrom = Span.From;
				Lead.Label = Span.Function->Name;
				Added.Insert(Lead, 0);
			}
			for (int32 Index = 0; Index < Added.Num(); ++Index)
			{
				Added[Index].DMXTo = Index + 1 < Added.Num() ? Added[Index + 1].DMXFrom - 1 : Span.To;
			}
			Ranges.Append(Added);
			return true;
		}

		/** Shutter1: its sets say which part is open (value 0); the rest (closed) is never selected. */
		void AddShutterRanges(const FSpan& Span, int32 Bytes, const FGameplayTag& Owner, TArray<FStageGdtfRange>& Ranges, FString& Note)
		{
			const TArray<FStageGdtfChannelSet>& Sets = Span.Function->Sets;
			auto IsOpen = [](const FString& Name) { return Name.Contains(TEXT("Open")) && !Name.Contains(TEXT("Close")); };
			if (Sets.IsEmpty())
			{
				FStageGdtfRange& Range = Ranges.AddDefaulted_GetRef();
				Range.DMXFrom = Span.From;
				Range.DMXTo = Span.To;
				Range.Attribute = Owner;
				Range.Label = Span.Function->Name;
				Note = TEXT("Shutter1 has no Open/Closed sets; the whole range is taken as open");
				return;
			}
			TArray<FStageGdtfRange> Added;
			int64 Previous = Span.From - 1;
			for (const FStageGdtfChannelSet& Set : Sets)
			{
				const int64 From = Set.DMXFrom.bSet ? StageGdtf::ToChannelValue(Set.DMXFrom, Bytes) : Span.From;
				if (From <= Previous || From > Span.To)
				{
					Note = TEXT("Shutter1 sets are out of order; taken as open");
					Added.Reset();
					break;
				}
				Previous = From;
				FStageGdtfRange& Range = Added.AddDefaulted_GetRef();
				Range.DMXFrom = From;
				Range.Label = Set.Name;
				if (IsOpen(Set.Name))
				{
					Range.Attribute = Owner;
				}
			}
			if (Added.IsEmpty())
			{
				FStageGdtfRange& Range = Added.AddDefaulted_GetRef();
				Range.DMXFrom = Span.From;
				Range.Attribute = Owner;
				Range.Label = Span.Function->Name;
			}
			else if (Added[0].DMXFrom > Span.From)
			{
				FStageGdtfRange Lead;
				Lead.DMXFrom = Span.From;
				Lead.Label = Span.Function->Name;
				Added.Insert(Lead, 0);
			}
			for (int32 Index = 0; Index < Added.Num(); ++Index)
			{
				Added[Index].DMXTo = Index + 1 < Added.Num() ? Added[Index + 1].DMXFrom - 1 : Span.To;
			}
			Ranges.Append(Added);
		}

		/** Builds one channel instance's ranges, owner, default and status. Errors that refuse the mode go to Layout.Errors. */
		FStageGdtfLayoutChannel BuildChannel(const FStageGdtfFixtureType& Type, const FChannelInstance& Instance, FStageGdtfModeLayout& Layout)
		{
			const FStageGdtfDMXChannel& Source = *Instance.Source;
			FStageGdtfLayoutChannel Channel;
			Channel.Offsets = Instance.Offsets;
			Channel.Cell = Instance.Cell;
			Channel.Geometry = Source.Geometry;
			const int32 Bytes = Channel.Offsets.Num();
			const int64 MaxValue = Bytes > 0 ? (int64(1) << (8 * Bytes)) - 1 : 0;

			TArray<FSpan> Spans;
			for (const FStageGdtfLogicalChannel& Logical : Source.LogicalChannels)
			{
				for (const FStageGdtfChannelFunction& Function : Logical.Functions)
				{
					Spans.Add({ &Function, Function.DMXFrom.bSet ? StageGdtf::ToChannelValue(Function.DMXFrom, FMath::Max(Bytes, 1)) : 0, 0 });
				}
			}
			if (Spans.IsEmpty())
			{
				Channel.Status = EStageGdtfChannelStatus::Default;
				return Channel;
			}
			// Named after its logical channel (a control channel's first function is often "NoFeature"); the owner renames it below.
			Channel.GdtfAttribute = Source.LogicalChannels[0].Attribute.IsEmpty() ? Spans[0].Function->Attribute : Source.LogicalChannels[0].Attribute;
			if (Bytes == 0)
			{
				Channel.Status = EStageGdtfChannelStatus::Virtual;
				Channel.Note = TEXT("virtual channel (no DMX slot)");
				return Channel;
			}

			// Spans run from their DMXFrom to the next one's, in the order written; a later span may not start at or before an earlier one.
			for (int32 Index = 0; Index < Spans.Num(); ++Index)
			{
				if (Index > 0 && Spans[Index].From <= Spans[Index - 1].From)
				{
					Layout.Errors.Add(FString::Printf(TEXT("line %d: channel functions of %s do not rise in DMX value"), Source.Line, *Channel.GdtfAttribute));
					Channel.Status = EStageGdtfChannelStatus::Default;
					return Channel;
				}
				Spans[Index].To = Index + 1 < Spans.Num() ? Spans[Index + 1].From - 1 : MaxValue;
			}

			// The initial function's default (its node path ends with the function's name), else the first function's start.
			const FSpan* Initial = &Spans[0];
			FString InitialName = Source.InitialFunction;
			int32 Dot = INDEX_NONE;
			if (InitialName.FindLastChar(TEXT('.'), Dot))
			{
				InitialName.RightChopInline(Dot + 1);
			}
			if (const FSpan* Named = Spans.FindByPredicate([&InitialName](const FSpan& Span) { return Span.Function->Name.Equals(InitialName, ESearchCase::CaseSensitive); }))
			{
				Initial = Named;
			}
			Channel.DefaultDMX = FMath::Clamp(Initial->Function->Default.bSet ? StageGdtf::ToChannelValue(Initial->Function->Default, Bytes) : Initial->From, int64(0), MaxValue);

			const bool bResolutionSupported = Bytes == 1 || (Bytes == 2 && Channel.Offsets[1] == Channel.Offsets[0] + 1);
			if (!bResolutionSupported)
			{
				Channel.Status = EStageGdtfChannelStatus::Default;
				Channel.Note = Bytes > 2 ? FString::Printf(TEXT("%d-bit channels are not supported; kept at default"), Bytes * 8)
					: FString(TEXT("a 16-bit channel whose bytes are not next to each other; kept at default"));
				return Channel;
			}

			// The owner: the first function whose attribute StageCraft can drive here.
			const FSpan* OwnerSpan = nullptr;
			FStageGdtfAttributeMapping OwnerMapping;
			for (const FSpan& Span : Spans)
			{
				const FStageGdtfAttributeMapping Mapping = MapAttribute(Span.Function->Attribute);
				if (ResolveAttribute(Mapping, Instance.Cell).IsValid())
				{
					OwnerSpan = &Span;
					OwnerMapping = Mapping;
					break;
				}
			}
			if (!OwnerSpan)
			{
				Channel.Status = EStageGdtfChannelStatus::Default;
				Channel.Note = FString::Printf(TEXT("%s has no StageCraft control; kept at its default"), *Channel.GdtfAttribute);
				return Channel;
			}
			Channel.Attribute = ResolveAttribute(OwnerMapping, Instance.Cell);
			Channel.GdtfAttribute = OwnerSpan->Function->Attribute;
			Channel.Wheel = OwnerSpan->Function->Wheel;
			Channel.Emitter = OwnerSpan->Function->Emitter;
			TArray<FString> Notes;
			if (OwnerMapping.bApproximated)
			{
				Notes.Add(FString::Printf(TEXT("%s is approximated"), *Channel.GdtfAttribute));
			}

			FVector2D StrobeHz(TNumericLimits<double>::Max(), TNumericLimits<double>::Lowest());
			TArray<int32, TInlineAllocator<8>> StrobeRanges;
			for (const FSpan& Span : Spans)
			{
				const FStageGdtfAttributeMapping Mapping = MapAttribute(Span.Function->Attribute);
				const bool bOwner = ResolveAttribute(Mapping, Instance.Cell) == Channel.Attribute;
				FString Note;
				if (bOwner && Mapping.Kind == EStageGdtfValueKind::Slot)
				{
					if (!AddSlotRanges(Type, Span, Bytes, Channel.Attribute, Channel.Ranges, Note))
					{
						FStageGdtfRange& Range = Channel.Ranges.AddDefaulted_GetRef();
						Range.DMXFrom = Span.From;
						Range.DMXTo = Span.To;
						Range.Label = Span.Function->Name;
						Notes.Add(Note.IsEmpty() ? FString::Printf(TEXT("%s has no wheel slots to select"), *Span.Function->Name) : Note);
					}
					else if (!Note.IsEmpty())
					{
						Notes.Add(Note);
					}
					continue;
				}
				if (bOwner && Mapping.ShutterRole == EStageGdtfShutterRole::OpenClosed)
				{
					AddShutterRanges(Span, Bytes, Channel.Attribute, Channel.Ranges, Note);
					if (!Note.IsEmpty())
					{
						Notes.Add(Note);
					}
					continue;
				}

				FStageGdtfRange& Range = Channel.Ranges.AddDefaulted_GetRef();
				Range.DMXFrom = Span.From;
				Range.DMXTo = Span.To;
				Range.Label = Span.Function->Name;
				if (!bOwner)
				{
					if (ResolveAttribute(Mapping, Instance.Cell).IsValid())
					{
						Notes.Add(FString::Printf(TEXT("%s shares this channel and is not driven"), *Span.Function->Attribute));
					}
					continue;
				}
				Range.Attribute = Channel.Attribute;
				const float From = Span.Function->PhysicalFrom;
				const float To = Span.Function->PhysicalTo;
				switch (Mapping.Kind)
				{
				case EStageGdtfValueKind::Rpm:
					Range.ValueFrom = StageGdtfUnits::DegreesPerSecondToRpm(From);
					Range.ValueTo = StageGdtfUnits::DegreesPerSecondToRpm(To);
					break;
				case EStageGdtfValueKind::Shutter: // The strobe role: Hz now, scaled below.
					StrobeRanges.Add(Channel.Ranges.Num() - 1);
					StrobeHz.X = FMath::Min(StrobeHz.X, double(FMath::Min(From, To)));
					StrobeHz.Y = FMath::Max(StrobeHz.Y, double(FMath::Max(From, To)));
					Range.ValueFrom = From;
					Range.ValueTo = To;
					Range.bStrobe = true;
					Range.StrobeHzFrom = From;
					Range.StrobeHzTo = To;
					break;
				default:
					Range.ValueFrom = From;
					Range.ValueTo = To;
					break;
				}
			}

			// Scale the kinds StageCraft holds as 0..1 (and the strobe) over the channel's physical span of the owner.
			if (OwnerMapping.Kind == EStageGdtfValueKind::Normalized)
			{
				float Low = TNumericLimits<float>::Max();
				float High = TNumericLimits<float>::Lowest();
				for (const FStageGdtfRange& Range : Channel.Ranges)
				{
					if (Range.Attribute == Channel.Attribute)
					{
						Low = FMath::Min3(Low, Range.ValueFrom, Range.ValueTo);
						High = FMath::Max3(High, Range.ValueFrom, Range.ValueTo);
					}
				}
				for (FStageGdtfRange& Range : Channel.Ranges)
				{
					if (Range.Attribute == Channel.Attribute)
					{
						const bool bSpan = High - Low > UE_KINDA_SMALL_NUMBER;
						Range.ValueFrom = bSpan ? (Range.ValueFrom - Low) / (High - Low) : 0.f;
						Range.ValueTo = bSpan ? (Range.ValueTo - Low) / (High - Low) : 1.f;
					}
				}
			}
			if (StrobeHz.X <= StrobeHz.Y)
			{
				for (const int32 Index : StrobeRanges)
				{
					FStageGdtfRange& Range = Channel.Ranges[Index];
					const double Span = FMath::Max(StrobeHz.Y - StrobeHz.X, double(UE_KINDA_SMALL_NUMBER));
					Range.ValueFrom = float(StrobeFloor + (1.0 - StrobeFloor) * (Range.ValueFrom - StrobeHz.X) / Span);
					Range.ValueTo = float(StrobeFloor + (1.0 - StrobeFloor) * (Range.ValueTo - StrobeHz.X) / Span);
				}
				Layout.StrobeRateHz.X = Layout.StrobeRateHz.IsZero() ? StrobeHz.X : FMath::Min(Layout.StrobeRateHz.X, StrobeHz.X);
				Layout.StrobeRateHz.Y = FMath::Max(Layout.StrobeRateHz.Y, StrobeHz.Y);
			}

			Channel.Status = Notes.IsEmpty() ? EStageGdtfChannelStatus::Mapped : EStageGdtfChannelStatus::Approximated;
			Channel.Note = FString::Join(Notes, TEXT("; "));
			return Channel;
		}

		/**
		 * The beam geometry a mode lights: the first Beam under its top geometry, else the first Beam of a geometry it repeats
		 * (a pixel bar's beams sit on the referenced pixel, not under the body).
		 */
		int32 FindBeam(const FStageGdtfFixtureType& Type, int32 Top)
		{
			auto FirstBeamUnder = [&Type](int32 Root)
			{
				for (int32 Index = 0; Index < Type.Geometries.Num(); ++Index)
				{
					if (Type.Geometries[Index].Type == EStageGdtfGeometryType::Beam && Type.IsInSubtree(Index, Root))
					{
						return Index;
					}
				}
				return int32(INDEX_NONE);
			};
			const int32 Direct = FirstBeamUnder(Top);
			if (Direct != INDEX_NONE)
			{
				return Direct;
			}
			for (int32 Index = 0; Index < Type.Geometries.Num(); ++Index)
			{
				const FStageGdtfGeometry& Geometry = Type.Geometries[Index];
				if (Geometry.Type == EStageGdtfGeometryType::GeometryReference && Type.IsInSubtree(Index, Top))
				{
					const int32 Referenced = FirstBeamUnder(Type.FindGeometry(Geometry.ReferencedGeometry));
					if (Referenced != INDEX_NONE)
					{
						return Referenced;
					}
				}
			}
			return INDEX_NONE;
		}
	}

	FStageGdtfAttributeMapping MapAttribute(FStringView Name)
	{
		FStageGdtfAttributeMapping Mapping;
		auto Is = [Name](const TCHAR* Candidate) { return Name.Equals(Candidate, ESearchCase::CaseSensitive); };
		auto Set = [&Mapping](const FGameplayTag& Tag, EStageGdtfValueKind Kind, EStageCellChannel Cell = EStageCellChannel::Count, bool bApproximated = false)
		{
			Mapping.Attribute = Tag;
			Mapping.Kind = Kind;
			Mapping.CellChannel = Cell;
			Mapping.bApproximated = bApproximated;
		};
		using namespace StageCraftTags;
		using EKind = EStageGdtfValueKind;

		if (Is(TEXT("Dimmer"))) { Set(Attribute_Dimmer, EKind::Normalized, EStageCellChannel::Dimmer); }
		else if (Is(TEXT("Pan"))) { Set(Attribute_Pan, EKind::Degrees); }
		else if (Is(TEXT("Tilt"))) { Set(Attribute_Tilt, EKind::Degrees); }
		else if (Is(TEXT("Shutter1"))) { Set(Attribute_Shutter, EKind::Shutter); Mapping.ShutterRole = EStageGdtfShutterRole::OpenClosed; }
		else if (Is(TEXT("Shutter1Strobe")) || Is(TEXT("StrobeFrequency"))) { Set(Attribute_Shutter, EKind::Shutter); Mapping.ShutterRole = EStageGdtfShutterRole::Strobe; }
		// ColorRGB_* are GDTF 1.0's names for the same emitters.
		else if (Is(TEXT("ColorAdd_R")) || Is(TEXT("ColorRGB_Red"))) { Set(Attribute_ColorR, EKind::Normalized, EStageCellChannel::ColorR); }
		else if (Is(TEXT("ColorAdd_G")) || Is(TEXT("ColorRGB_Green"))) { Set(Attribute_ColorG, EKind::Normalized, EStageCellChannel::ColorG); }
		else if (Is(TEXT("ColorAdd_B")) || Is(TEXT("ColorRGB_Blue"))) { Set(Attribute_ColorB, EKind::Normalized, EStageCellChannel::ColorB); }
		else if (Is(TEXT("ColorAdd_W"))) { Set(Attribute_ColorW, EKind::Normalized, EStageCellChannel::ColorW); }
		else if (Is(TEXT("ColorAdd_RY"))) { Set(Attribute_ColorA, EKind::Normalized, EStageCellChannel::ColorA); } // Amber: GDTF has no ColorAdd_A.
		else if (Is(TEXT("ColorSub_C"))) { Set(Attribute_ColorC, EKind::Normalized); }
		else if (Is(TEXT("ColorSub_M"))) { Set(Attribute_ColorM, EKind::Normalized); }
		else if (Is(TEXT("ColorSub_Y"))) { Set(Attribute_ColorY, EKind::Normalized); }
		else if (Is(TEXT("CTO"))) { Set(Attribute_CTO, EKind::Normalized); }
		else if (Is(TEXT("CTC"))) { Set(Attribute_ColorTemp, EKind::Kelvin); }
		else if (Is(TEXT("Color1"))) { Set(Attribute_ColorWheel, EKind::Slot); }
		else if (Is(TEXT("Gobo1"))) { Set(Attribute_Gobo, EKind::Slot); }
		else if (Is(TEXT("Gobo1Pos"))) { Set(Attribute_GoboRotation, EKind::Degrees); }
		else if (Is(TEXT("Gobo1PosRotate"))) { Set(Attribute_GoboSpin, EKind::Rpm); }
		else if (Is(TEXT("Gobo2"))) { Set(Attribute_Gobo2, EKind::Slot); }
		else if (Is(TEXT("Prism1"))) { Set(Attribute_Prism, EKind::Slot); }
		else if (Is(TEXT("Prism1PosRotate"))) { Set(Attribute_PrismRotation, EKind::Rpm); }
		else if (Is(TEXT("Zoom"))) { Set(Attribute_Zoom, EKind::Degrees); }
		else if (Is(TEXT("Iris"))) { Set(Attribute_Iris, EKind::Normalized); }
		else if (Is(TEXT("Frost1"))) { Set(Attribute_Frost, EKind::Normalized); }
		// StageCraft's blades have one insertion each; GDTF's A edge drives it, the B edge and per-blade rotation are not driven.
		else if (Is(TEXT("Blade1A"))) { Set(Attribute_BladeA, EKind::Normalized, EStageCellChannel::Count, true); }
		else if (Is(TEXT("Blade2A"))) { Set(Attribute_BladeB, EKind::Normalized, EStageCellChannel::Count, true); }
		else if (Is(TEXT("Blade3A"))) { Set(Attribute_BladeC, EKind::Normalized, EStageCellChannel::Count, true); }
		else if (Is(TEXT("Blade4A"))) { Set(Attribute_BladeD, EKind::Normalized, EStageCellChannel::Count, true); }
		else if (Is(TEXT("ShaperRot"))) { Set(Attribute_BladeRotation, EKind::Degrees); }
		return Mapping;
	}

	int32 MapWheelSlot(const FStageGdtfWheel& Wheel, int32 WheelSlotIndex)
	{
		const int32 Index = WheelSlotIndex - 1;
		if (!Wheel.Slots.IsValidIndex(Index))
		{
			return INDEX_NONE;
		}
		const int32 Open = FindOpenSlot(Wheel);
		if (Index == Open)
		{
			return 0;
		}
		// StageCraft lists the other slots in wheel order from 1, the open slot left out.
		return (Open != INDEX_NONE && Open < Index) ? Index : Index + 1;
	}

	TArray<FStageGdtfSlotOut> ListSlots(const FStageGdtfWheel& Wheel)
	{
		TArray<FStageGdtfSlotOut> Slots;
		const int32 Open = FindOpenSlot(Wheel);
		for (int32 Index = 0; Index < Wheel.Slots.Num(); ++Index)
		{
			if (Index == Open)
			{
				continue;
			}
			const FStageGdtfWheelSlot& Source = Wheel.Slots[Index];
			FStageGdtfSlotOut& Slot = Slots.AddDefaulted_GetRef();
			Slot.Name = Source.Name.IsEmpty() ? FString::Printf(TEXT("Slot %d"), Index + 1) : Source.Name;
			Slot.Filter = Source.Color.bSet ? StageGdtfUnits::ChromaticityToLinear(Source.Color) : FLinearColor::White;
			Slot.MediaFileName = Source.MediaFileName;
			Slot.Facets = Source.PrismFacets >= 2 ? FMath::Min(Source.PrismFacets, 16) : 3;
		}
		return Slots;
	}

	FStageGdtfModeLayout BuildModeLayout(const FStageGdtfFixtureType& Type, int32 ModeIndex)
	{
		FStageGdtfModeLayout Layout;
		if (!Type.Modes.IsValidIndex(ModeIndex))
		{
			Layout.Errors.Add(TEXT("no such mode"));
			return Layout;
		}
		const FStageGdtfMode& Mode = Type.Modes[ModeIndex];
		Layout.ModeIndex = ModeIndex;
		Layout.Name = Mode.Name;
		const int32 Top = Type.FindGeometry(Mode.Geometry);

		// Repeated geometries (cells): every GeometryReference under the mode's geometry, in document order.
		TArray<int32> References;
		for (int32 Index = 0; Index < Type.Geometries.Num(); ++Index)
		{
			if (Type.Geometries[Index].Type == EStageGdtfGeometryType::GeometryReference && Type.IsInSubtree(Index, Top))
			{
				References.Add(Index);
			}
		}

		TArray<FChannelInstance> Instances;
		for (const FStageGdtfDMXChannel& Source : Mode.Channels)
		{
			if (!Source.bBreakOverwrite)
			{
				if (Source.DMXBreak != 1)
				{
					Layout.Errors.Add(FString::Printf(TEXT("line %d: the mode uses DMX break %d; StageCraft patches one address per fixture"), Source.Line, Source.DMXBreak));
					continue;
				}
				Instances.Add({ &Source, Source.Offsets, INDEX_NONE });
				continue;
			}
			// A template channel: one copy per reference to the geometry it sits on, at that reference's offset.
			const int32 ChannelGeometry = Type.FindGeometry(Source.Geometry);
			TArray<int32> Using;
			for (const int32 Reference : References)
			{
				if (Type.IsInSubtree(ChannelGeometry, Type.FindGeometry(Type.Geometries[Reference].ReferencedGeometry)))
				{
					Using.Add(Reference);
				}
			}
			if (Using.IsEmpty())
			{
				Layout.Warnings.Add(FString::Printf(TEXT("line %d: a repeated channel on \"%s\" has no instance in this mode"), Source.Line, *Source.Geometry));
				continue;
			}
			Layout.CellCount = FMath::Max(Layout.CellCount, Using.Num());
			for (int32 Cell = 0; Cell < Using.Num(); ++Cell)
			{
				const FStageGdtfGeometry& Reference = Type.Geometries[Using[Cell]];
				const FStageGdtfBreak Break = Reference.Breaks.IsEmpty() ? FStageGdtfBreak() : Reference.Breaks[0];
				if (Break.DMXBreak != 1)
				{
					Layout.Errors.Add(FString::Printf(TEXT("\"%s\" uses DMX break %d; StageCraft patches one address per fixture"), *Reference.Name, Break.DMXBreak));
					continue;
				}
				FChannelInstance& Instance = Instances.AddDefaulted_GetRef();
				Instance.Source = &Source;
				Instance.Cell = Using.Num() > 1 ? Cell : INDEX_NONE;
				for (const int32 Offset : Source.Offsets)
				{
					Instance.Offsets.Add(Offset + Break.DMXOffset - 1);
				}
			}
		}
		if (Layout.CellCount > StageFixtureCells::MaxCells)
		{
			Layout.Errors.Add(FString::Printf(TEXT("%d cells; StageCraft supports up to %d"), Layout.CellCount, StageFixtureCells::MaxCells));
			return Layout;
		}

		TBitArray<> Used(false, 513);
		for (const FChannelInstance& Instance : Instances)
		{
			FStageGdtfLayoutChannel Channel = BuildChannel(Type, Instance, Layout);
			for (const int32 Offset : Channel.Offsets)
			{
				if (Offset < 1 || Offset > 512)
				{
					Layout.Errors.Add(FString::Printf(TEXT("%s reaches channel %d, past 512"), *Channel.GdtfAttribute, Offset));
					continue;
				}
				if (Used[Offset])
				{
					Layout.Errors.Add(FString::Printf(TEXT("channel %d is used twice (%s)"), Offset, *Channel.GdtfAttribute));
				}
				Used[Offset] = true;
				Layout.Footprint = FMath::Max(Layout.Footprint, Offset);
			}
			Layout.Channels.Add(MoveTemp(Channel));
		}
		if (Layout.Footprint == 0 && Layout.Errors.IsEmpty())
		{
			Layout.Errors.Add(TEXT("the mode has no DMX channels"));
		}
		Layout.Channels.Sort([](const FStageGdtfLayoutChannel& A, const FStageGdtfLayoutChannel& B)
		{
			const int32 FirstA = A.Offsets.IsEmpty() ? MAX_int32 : A.Offsets[0];
			const int32 FirstB = B.Offsets.IsEmpty() ? MAX_int32 : B.Offsets[0];
			return FirstA < FirstB;
		});
		return Layout;
	}

	FStageGdtfCapabilities DeriveCapabilities(const FStageGdtfFixtureType& Type, TConstArrayView<FStageGdtfModeLayout> Layouts)
	{
		using namespace StageCraftTags;
		FStageGdtfCapabilities Caps;
		Caps.WeightKg = Type.Properties.WeightKg;

		TSet<FGameplayTag> Attributes;
		TMap<FGameplayTag, FFloatInterval> Spans;
		TMap<FGameplayTag, const FStageGdtfLayoutChannel*> FirstChannel;
		bool bCellDimmer = false;
		bool bCellColor = false;
		const FStageGdtfModeLayout* FirstUsable = nullptr;
		bool bStrobeSeen = false;
		for (const FStageGdtfModeLayout& Layout : Layouts)
		{
			if (!Layout.IsUsable())
			{
				continue;
			}
			FirstUsable = FirstUsable ? FirstUsable : &Layout;
			Caps.CellCount = FMath::Max(Caps.CellCount, Layout.CellCount);
			if (Layout.StrobeRateHz.Y > 0.0)
			{
				Caps.StrobeRateHz = bStrobeSeen ? FVector2D(FMath::Min(Caps.StrobeRateHz.X, Layout.StrobeRateHz.X), FMath::Max(Caps.StrobeRateHz.Y, Layout.StrobeRateHz.Y))
					: Layout.StrobeRateHz;
				bStrobeSeen = true;
			}
			for (const FStageGdtfLayoutChannel& Channel : Layout.Channels)
			{
				if (!Channel.Attribute.IsValid())
				{
					continue;
				}
				int32 Cell = INDEX_NONE;
				EStageCellChannel CellChannel = EStageCellChannel::Count;
				if (StageFixtureCells::ParseCellAttribute(Channel.Attribute, Cell, CellChannel))
				{
					bCellDimmer |= CellChannel == EStageCellChannel::Dimmer;
					bCellColor |= CellChannel != EStageCellChannel::Dimmer;
					// A cell's colour also tells the colour system (and its emitter colour, from the first cell).
					const FGameplayTag Whole = CellChannel == EStageCellChannel::ColorR ? FGameplayTag(Attribute_ColorR)
						: CellChannel == EStageCellChannel::ColorG ? FGameplayTag(Attribute_ColorG)
						: CellChannel == EStageCellChannel::ColorB ? FGameplayTag(Attribute_ColorB)
						: CellChannel == EStageCellChannel::ColorW ? FGameplayTag(Attribute_ColorW)
						: CellChannel == EStageCellChannel::ColorA ? FGameplayTag(Attribute_ColorA) : FGameplayTag();
					if (Whole.IsValid())
					{
						Attributes.Add(Whole);
						FirstChannel.FindOrAdd(Whole, &Channel);
					}
					continue;
				}
				Attributes.Add(Channel.Attribute);
				FirstChannel.FindOrAdd(Channel.Attribute, &Channel);
				for (const FStageGdtfRange& Range : Channel.Ranges)
				{
					if (Range.Attribute == Channel.Attribute && Range.Slot == INDEX_NONE)
					{
						FFloatInterval& Interval = Spans.FindOrAdd(Channel.Attribute, FFloatInterval(TNumericLimits<float>::Max(), TNumericLimits<float>::Lowest()));
						Interval.Min = FMath::Min3(Interval.Min, Range.ValueFrom, Range.ValueTo);
						Interval.Max = FMath::Max3(Interval.Max, Range.ValueFrom, Range.ValueTo);
					}
				}
			}
		}
		auto Has = [&Attributes](const FGameplayTag& Tag) { return Attributes.Contains(Tag); };
		auto SpanOf = [&Spans](const FGameplayTag& Tag) { const FFloatInterval* Interval = Spans.Find(Tag); return Interval && Interval->Min <= Interval->Max ? *Interval : FFloatInterval(0.f, 0.f); };

		// Colour.
		const bool bRGB = Has(Attribute_ColorR) && Has(Attribute_ColorG) && Has(Attribute_ColorB);
		if (bRGB)
		{
			const bool bW = Has(Attribute_ColorW);
			const bool bA = Has(Attribute_ColorA);
			Caps.ColorSystem = bW && bA ? EStageFixtureColorSystem::RGBWA : bW ? EStageFixtureColorSystem::RGBW : bA ? EStageFixtureColorSystem::RGBA : EStageFixtureColorSystem::RGB;
			TArray<FGameplayTag> Order = { Attribute_ColorR, Attribute_ColorG, Attribute_ColorB };
			if (bW) { Order.Add(Attribute_ColorW); }
			if (bA) { Order.Add(Attribute_ColorA); }
			for (const FGameplayTag& Tag : Order)
			{
				const FStageGdtfLayoutChannel* const* Channel = FirstChannel.Find(Tag);
				const FStageGdtfEmitter* Emitter = Channel ? Type.FindEmitter((*Channel)->Emitter) : nullptr;
				if (!Emitter || !Emitter->Color.bSet)
				{
					Caps.Emitters.Reset(); // Any emitter unknown: the colour system's defaults for all.
					break;
				}
				Caps.Emitters.Add(StageGdtfUnits::ChromaticityToLinear(Emitter->Color));
			}
		}
		else if (Has(Attribute_ColorC) && Has(Attribute_ColorM) && Has(Attribute_ColorY))
		{
			Caps.ColorSystem = EStageFixtureColorSystem::CMY;
		}
		Caps.bHasCTO = Has(Attribute_CTO);
		Caps.bHasColorTemperatureControl = Has(Attribute_ColorTemp) && Caps.ColorSystem != EStageFixtureColorSystem::None;
		if (Caps.bHasColorTemperatureControl)
		{
			const FFloatInterval Kelvin = SpanOf(Attribute_ColorTemp);
			Caps.ColorTemperatureRangeK = FVector2D(FMath::Clamp(Kelvin.Min, 1700.f, 12000.f), FMath::Clamp(Kelvin.Max, 1700.f, 12000.f));
		}

		// Wheels, from the wheel each slot attribute's channel names.
		auto WheelSlots = [&](const FGameplayTag& Tag)
		{
			const FStageGdtfLayoutChannel* const* Channel = FirstChannel.Find(Tag);
			const FStageGdtfWheel* Wheel = Channel ? Type.FindWheel((*Channel)->Wheel) : nullptr;
			return Wheel ? ListSlots(*Wheel) : TArray<FStageGdtfSlotOut>();
		};
		Caps.ColorWheel = WheelSlots(Attribute_ColorWheel);
		Caps.GoboWheel = WheelSlots(Attribute_Gobo);
		Caps.StaticGoboWheel = WheelSlots(Attribute_Gobo2);
		Caps.Prisms = WheelSlots(Attribute_Prism);
		Caps.bGoboSpin = Has(Attribute_GoboSpin);
		const float MaxRpm = FMath::Max(FMath::Max(FMath::Abs(SpanOf(Attribute_GoboSpin).Min), FMath::Abs(SpanOf(Attribute_GoboSpin).Max)),
			FMath::Max(FMath::Abs(SpanOf(Attribute_PrismRotation).Min), FMath::Abs(SpanOf(Attribute_PrismRotation).Max)));
		Caps.MaxRotationRpm = MaxRpm > 0.f ? FMath::Clamp(MaxRpm, 1.f, 300.f) : 60.f;

		// Beam shaping and shutter.
		Caps.bHasFrost = Has(Attribute_Frost);
		Caps.bHasIris = Has(Attribute_Iris);
		Caps.BeamShaper = Has(Attribute_BladeA) || Has(Attribute_BladeB) || Has(Attribute_BladeC) || Has(Attribute_BladeD) ? EStageBeamShaper::FramingShutters : EStageBeamShaper::None;
		Caps.bHasShutter = Has(Attribute_Shutter);
		Caps.StrobeRateHz = FVector2D(FMath::Clamp(Caps.StrobeRateHz.X, 0.1, 50.0), FMath::Clamp(Caps.StrobeRateHz.Y, 0.1, 50.0));
		Caps.CellControl = bCellDimmer && bCellColor ? EStageCellControl::DimmerAndColor : bCellDimmer ? EStageCellControl::Dimmer : EStageCellControl::Color;

		// Motion.
		Caps.bHasPanTilt = Has(Attribute_Pan) || Has(Attribute_Tilt);
		if (Has(Attribute_Pan))
		{
			Caps.PanLimits = FVector2D(SpanOf(Attribute_Pan).Min, SpanOf(Attribute_Pan).Max);
		}
		if (Has(Attribute_Tilt))
		{
			Caps.TiltLimits = FVector2D(SpanOf(Attribute_Tilt).Min, SpanOf(Attribute_Tilt).Max);
		}

		// Beam and body, from the first usable mode's geometry.
		const int32 Top = FirstUsable && Type.Modes.IsValidIndex(FirstUsable->ModeIndex) ? Type.FindGeometry(Type.Modes[FirstUsable->ModeIndex].Geometry) : INDEX_NONE;
		const int32 Beam = Top != INDEX_NONE ? FindBeam(Type, Top) : INDEX_NONE;
		EStageGdtfBeamType BeamType = EStageGdtfBeamType::Wash;
		if (Beam != INDEX_NONE)
		{
			const FStageGdtfBeam& Light = Type.Geometries[Beam].Beam;
			BeamType = Light.BeamType;
			Caps.LuminousFlux = Light.LuminousFlux;
			Caps.ColorTemperatureK = FMath::Clamp(Light.ColorTemperatureK, 1700.f, 12000.f);
			Caps.BeamAngleMin = Caps.BeamAngleMax = FMath::Clamp(Light.BeamAngleDeg, 0.5f, 170.f);
			const bool bHardEdge = BeamType == EStageGdtfBeamType::Spot || BeamType == EStageGdtfBeamType::Rectangle;
			Caps.FieldAngleRatio = Light.FieldAngleDeg > Light.BeamAngleDeg && Light.BeamAngleDeg > 0.f
				? FMath::Clamp(Light.FieldAngleDeg / Light.BeamAngleDeg, 1.f, 3.f) : (bHardEdge ? 1.1f : 1.6f);
			Caps.LensDiameterCm = Light.BeamRadiusM * 200.f;
			Caps.PowerW = Type.Properties.PowerW > 0.f ? Type.Properties.PowerW : Light.PowerConsumptionW;
		}
		else
		{
			Caps.PowerW = Type.Properties.PowerW;
		}
		if (Has(Attribute_Zoom))
		{
			Caps.BeamAngleMin = FMath::Clamp(SpanOf(Attribute_Zoom).Min, 0.5f, 170.f);
			Caps.BeamAngleMax = FMath::Clamp(SpanOf(Attribute_Zoom).Max, 0.5f, 170.f);
		}

		auto ModelSize = [&Type](int32 Geometry)
		{
			const FStageGdtfModel* Model = Type.Geometries.IsValidIndex(Geometry) ? Type.FindModel(Type.Geometries[Geometry].Model) : nullptr;
			return Model ? StageGdtfUnits::SizeToUnrealCm(Model->SizeM) : FVector::ZeroVector;
		};
		auto GeometryOf = [&](const FGameplayTag& Tag)
		{
			const FStageGdtfLayoutChannel* const* Channel = FirstChannel.Find(Tag);
			return Channel ? Type.FindGeometry((*Channel)->Geometry) : INDEX_NONE;
		};
		if (Caps.bHasPanTilt)
		{
			const int32 Yoke = GeometryOf(Attribute_Pan);
			const int32 Head = GeometryOf(Attribute_Tilt);
			Caps.BaseSizeCm = ModelSize(Top);
			Caps.YokeSizeCm = ModelSize(Yoke);
			Caps.HeadSizeCm = ModelSize(Head);
			if (Type.Geometries.IsValidIndex(Yoke)) { Caps.PanPivotOffsetCm = StageGdtfUnits::PositionToUnrealCm(Type.Geometries[Yoke].Position); }
			if (Type.Geometries.IsValidIndex(Head)) { Caps.TiltPivotOffsetCm = StageGdtfUnits::PositionToUnrealCm(Type.Geometries[Head].Position); }
		}
		else
		{
			Caps.HeadSizeCm = ModelSize(Top); // A static fixture is all head: the beam leaves its body.
		}
		if (Beam != INDEX_NONE)
		{
			Caps.BeamOriginOffsetCm = StageGdtfUnits::PositionToUnrealCm(Type.Geometries[Beam].Position);
		}
		for (int32 Index = 0; Index < Type.Geometries.Num(); ++Index)
		{
			if (Top != INDEX_NONE && Type.IsInSubtree(Index, Top))
			{
				Caps.DimensionsCm = Caps.DimensionsCm.ComponentMax(ModelSize(Index));
			}
		}

		// Kind (ADR 0033 §5.6) and category.
		const bool bOnlyDimmerAndShutter = !Attributes.IsEmpty() && Attributes.Array().FilterByPredicate([](const FGameplayTag& Tag)
			{ return Tag != Attribute_Dimmer && Tag != Attribute_Shutter; }).IsEmpty();
		if (Caps.CellCount > 1)
		{
			Caps.Kind = Caps.CellControl == EStageCellControl::Dimmer ? EStageFixtureKind::Blinder : EStageFixtureKind::PixelBar;
		}
		else if (Caps.bHasPanTilt)
		{
			Caps.Kind = !Caps.GoboWheel.IsEmpty() ? (Caps.BeamAngleMin < 5.f ? EStageFixtureKind::MovingHeadHybrid : EStageFixtureKind::MovingHeadSpot)
				: Caps.BeamAngleMax <= 5.f ? EStageFixtureKind::MovingHeadBeam : EStageFixtureKind::MovingHeadWash;
		}
		else if (Caps.BeamShaper != EStageBeamShaper::None)
		{
			Caps.Kind = EStageFixtureKind::Profile;
		}
		else if (bOnlyDimmerAndShutter && Caps.bHasShutter && Caps.StrobeRateHz.Y >= 10.0)
		{
			Caps.Kind = EStageFixtureKind::Strobe;
		}
		else if (BeamType == EStageGdtfBeamType::Fresnel || BeamType == EStageGdtfBeamType::PC)
		{
			Caps.Kind = EStageFixtureKind::Fresnel;
		}
		else
		{
			Caps.Kind = EStageFixtureKind::LEDPar;
		}
		Caps.Category = CategoryForKind(Caps.Kind);
		const bool bConventionalBeam = BeamType == EStageGdtfBeamType::Fresnel || BeamType == EStageGdtfBeamType::PC || BeamType == EStageGdtfBeamType::Spot;
		Caps.bAcceptsGel = Caps.ColorSystem == EStageFixtureColorSystem::None && !Caps.bHasPanTilt && Caps.CellCount <= 1 && bConventionalBeam;
		return Caps;
	}

	FGameplayTag CategoryForKind(EStageFixtureKind Kind)
	{
		using namespace StageCraftTags;
		switch (Kind)
		{
		case EStageFixtureKind::MovingHeadSpot:
		case EStageFixtureKind::MovingHeadBeam:
		case EStageFixtureKind::MovingHeadWash:
		case EStageFixtureKind::MovingHeadHybrid: return Category_Lighting_MovingHead;
		case EStageFixtureKind::Strobe: return Category_Lighting_Strobe;
		case EStageFixtureKind::PixelBar: return Category_Lighting_Pixel;
		case EStageFixtureKind::LEDPar: return Category_Lighting_Par;
		default: return Category_Lighting_Conventional;
		}
	}

	FStageGdtfImportReport BuildReport(const FStageGdtfParseResult& Parsed, TConstArrayView<FStageGdtfModeLayout> Layouts, const FStageGdtfCapabilities& Capabilities)
	{
		FStageGdtfImportReport Report;
		const FStageGdtfFixtureType& Type = Parsed.Type;
		Report.Title = FString::Printf(TEXT("%s %s"), *Type.Manufacturer, *Type.Name).TrimStartAndEnd();
		Report.Errors = Parsed.Errors;
		Report.Warnings = Parsed.Warnings;
		if (!Parsed.IsOk())
		{
			return Report;
		}

		const UEnum* KindEnum = StaticEnum<EStageFixtureKind>();
		const int32 Usable = Layouts.FilterByPredicate([](const FStageGdtfModeLayout& Layout) { return Layout.IsUsable(); }).Num();
		Report.Lines.Add(FString::Printf(TEXT("GDTF %s, %s, %d of %d modes can be used."), *Type.DataVersion,
			KindEnum ? *KindEnum->GetDisplayNameTextByValue(int64(Capabilities.Kind)).ToString() : TEXT("fixture"), Usable, Layouts.Num()));
		TSet<FString> Notes;
		for (const FStageGdtfModeLayout& Layout : Layouts)
		{
			if (!Layout.IsUsable())
			{
				Report.Warnings.Add(FString::Printf(TEXT("Mode \"%s\" is left out: %s"), *Layout.Name, *FString::Join(Layout.Errors, TEXT("; "))));
				continue;
			}
			TArray<FString> AtDefault;
			for (const FStageGdtfLayoutChannel& Channel : Layout.Channels)
			{
				if (Channel.Status == EStageGdtfChannelStatus::Default)
				{
					AtDefault.AddUnique(Channel.GdtfAttribute);
				}
				if (Channel.Status == EStageGdtfChannelStatus::Approximated && !Channel.Note.IsEmpty())
				{
					Notes.Add(Channel.Note);
				}
			}
			FString Line = FString::Printf(TEXT("%s: %d channels; %d mapped, %d approximated, %d kept at default"), *Layout.Name, Layout.Footprint,
				Layout.CountStatus(EStageGdtfChannelStatus::Mapped), Layout.CountStatus(EStageGdtfChannelStatus::Approximated), Layout.CountStatus(EStageGdtfChannelStatus::Default));
			if (Layout.CellCount > 1)
			{
				Line += FString::Printf(TEXT("; %d cells"), Layout.CellCount);
			}
			if (!AtDefault.IsEmpty())
			{
				const int32 Shown = FMath::Min(AtDefault.Num(), 8);
				Line += FString::Printf(TEXT(" (%s%s)"), *FString::Join(TArrayView<const FString>(AtDefault.GetData(), Shown), TEXT(", ")), AtDefault.Num() > Shown ? TEXT(", ...") : TEXT(""));
			}
			Report.Lines.Add(Line + TEXT("."));
			Report.Warnings.Append(Layout.Warnings);
		}
		Report.Warnings.Append(Notes.Array());
		Report.bCanImport = Usable > 0;
		if (!Report.bCanImport)
		{
			Report.Errors.Add(TEXT("No mode of this fixture can be used in StageCraft."));
		}
		return Report;
	}
}
