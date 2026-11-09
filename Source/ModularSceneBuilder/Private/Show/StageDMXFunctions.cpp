// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageDMXFunctions.h"

#include "Data/StageParameterTypes.h"

namespace StageDMXFunctions
{
	namespace
	{
		struct FSpan
		{
			int32 From = 0;
			int32 To = 0;
		};

		FSpan ClampedSpan(const FStageDMXFunction& Function, int32 MaxValue)
		{
			const int32 From = FMath::Clamp(Function.DMXFrom, 0, MaxValue);
			return { From, FMath::Clamp(Function.DMXTo, From, MaxValue) };
		}

		bool IsFixed(const FStageDMXFunction& Function)
		{
			return Function.Slot != INDEX_NONE || FMath::IsNearlyEqual(Function.PhysicalFrom, Function.PhysicalTo);
		}

		float Distance(const FStageDMXFunction& Function, float Value)
		{
			const float Low = Function.Slot != INDEX_NONE ? float(Function.Slot) : FMath::Min(Function.PhysicalFrom, Function.PhysicalTo);
			const float High = Function.Slot != INDEX_NONE ? float(Function.Slot) : FMath::Max(Function.PhysicalFrom, Function.PhysicalTo);
			return Value < Low ? Low - Value : (Value > High ? Value - High : 0.f);
		}

		int32 EncodeIn(const FStageDMXFunction& Function, float Value, int32 MaxValue)
		{
			const FSpan Span = ClampedSpan(Function, MaxValue);
			if (IsFixed(Function))
			{
				return Span.From + (Span.To - Span.From) / 2;
			}
			const double Alpha = FMath::Clamp((double(Value) - Function.PhysicalFrom) / (double(Function.PhysicalTo) - Function.PhysicalFrom), 0.0, 1.0);
			return FMath::Clamp(int32(FMath::RoundToDouble(Span.From + Alpha * (Span.To - Span.From))), Span.From, Span.To);
		}

		bool IsShutter(const FStageFixtureDMXChannel& Channel)
		{
			return Channel.Attribute == StageCraftTags::Attribute_Shutter;
		}
	}

	int32 Encode(const FStageFixtureDMXChannel& Channel, float Value)
	{
		const int32 MaxValue = Channel.GetMaxValue();
		const int32 Default = FMath::Clamp(Channel.DefaultDMX, 0, MaxValue);
		if (!FMath::IsFinite(Value))
		{
			return Default;
		}
		const FStageDMXFunction* Best = nullptr;
		float BestDistance = TNumericLimits<float>::Max();
		for (const FStageDMXFunction& Function : Channel.Functions)
		{
			if (Function.Attribute != Channel.Attribute || !Channel.Attribute.IsValid())
			{
				continue;
			}
			const float Gap = Distance(Function, Value);
			if (Gap < BestDistance)
			{
				Best = &Function;
				BestDistance = Gap;
				if (Gap <= 0.f)
				{
					break;
				}
			}
		}
		if (!Best || (BestDistance > 0.f && Value <= 0.f && IsShutter(Channel)))
		{
			return Default;
		}
		return EncodeIn(*Best, Value, MaxValue);
	}

	TOptional<float> Decode(const FStageFixtureDMXChannel& Channel, int32 DMXValue)
	{
		const int32 MaxValue = Channel.GetMaxValue();
		const int32 Received = FMath::Clamp(DMXValue, 0, MaxValue);
		for (const FStageDMXFunction& Function : Channel.Functions)
		{
			const FSpan Span = ClampedSpan(Function, MaxValue);
			if (Received < Span.From || Received > Span.To)
			{
				continue;
			}
			if (Function.Attribute != Channel.Attribute || !Channel.Attribute.IsValid())
			{
				break;
			}
			if (Function.Slot != INDEX_NONE)
			{
				return float(Function.Slot);
			}
			if (Span.To == Span.From)
			{
				return Function.PhysicalFrom;
			}
			const double Alpha = double(Received - Span.From) / double(Span.To - Span.From);
			return float(Function.PhysicalFrom + Alpha * (double(Function.PhysicalTo) - Function.PhysicalFrom));
		}
		return IsShutter(Channel) ? TOptional<float>(0.f) : TOptional<float>();
	}

	bool AreValid(const FStageFixtureDMXChannel& Channel)
	{
		if (Channel.Functions.Num() > MaxFunctions)
		{
			return false;
		}
		const int32 MaxValue = Channel.GetMaxValue();
		int32 Previous = -1;
		for (const FStageDMXFunction& Function : Channel.Functions)
		{
			if (Function.DMXFrom <= Previous || Function.DMXFrom > Function.DMXTo || Function.DMXTo > MaxValue
				|| !FMath::IsFinite(Function.PhysicalFrom) || !FMath::IsFinite(Function.PhysicalTo) || Function.Slot < INDEX_NONE)
			{
				return false;
			}
			Previous = Function.DMXTo;
		}
		return true;
	}
}
