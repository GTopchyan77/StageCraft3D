// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageSseParser.h"

#include "Containers/StringConv.h"

void FStageSseParser::Feed(const uint8* Bytes, int32 Count, TArray<FStageSseEvent>& OutEvents)
{
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const uint8 Byte = Bytes[Index];
		if (Byte == '\n' && bAfterCR)
		{
			bAfterCR = false;
			continue;
		}
		bAfterCR = Byte == '\r';
		if (Byte == '\n' || Byte == '\r')
		{
			HandleLine(OutEvents);
			continue;
		}
		if (Line.Num() < MaxLineBytes)
		{
			Line.Add(Byte);
		}
		else
		{
			bMalformed = true;
		}
	}
}

void FStageSseParser::Finish(TArray<FStageSseEvent>& OutEvents)
{
	if (Line.Num() > 0)
	{
		HandleLine(OutEvents);
	}
	Dispatch(OutEvents);
	bAfterCR = false;
}

void FStageSseParser::HandleLine(TArray<FStageSseEvent>& OutEvents)
{
	if (Line.IsEmpty())
	{
		Dispatch(OutEvents);
		return;
	}
	const FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Line.GetData()), Line.Num());
	const FString Text(Converter.Length(), Converter.Get());
	Line.Reset();

	if (Text.StartsWith(TEXT(":")))
	{
		return;
	}
	int32 Colon = INDEX_NONE;
	FString Field = Text;
	FString Value;
	if (Text.FindChar(TEXT(':'), Colon))
	{
		Field = Text.Left(Colon);
		Value = Text.Mid(Colon + 1);
		if (Value.StartsWith(TEXT(" ")))
		{
			Value.RightChopInline(1);
		}
	}
	if (Field == TEXT("event"))
	{
		EventName = Value;
	}
	else if (Field == TEXT("data"))
	{
		if (bHasData)
		{
			Data.AppendChar(TEXT('\n'));
		}
		Data.Append(Value);
		bHasData = true;
	}
}

void FStageSseParser::Dispatch(TArray<FStageSseEvent>& OutEvents)
{
	if (bHasData)
	{
		FStageSseEvent& Event = OutEvents.AddDefaulted_GetRef();
		Event.Event = MoveTemp(EventName);
		Event.Data = MoveTemp(Data);
	}
	EventName.Reset();
	Data.Reset();
	bHasData = false;
}
