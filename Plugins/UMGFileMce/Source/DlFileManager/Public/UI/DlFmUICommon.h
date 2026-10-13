// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DlFmUICommon.generated.h"


USTRUCT()
struct DLFILEMANAGER_API FDlFmNumberFormat
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category="Content")
	bool bAlwaysSign = false;
	UPROPERTY(EditAnywhere, Category="Content")
	bool bUseGrouping = false;
	UPROPERTY(EditAnywhere, Category="Content")
	TEnumAsByte<ERoundingMode> RoundingMode = ERoundingMode::HalfFromZero;
	UPROPERTY(EditAnywhere, Category="Content")
	int32 MinimumIntegralDigits = 1;
	UPROPERTY(EditAnywhere, Category="Content")
	int32 MaximumIntegralDigits = 32;
	UPROPERTY(EditAnywhere, Category="Content")
	int32 MinimumFractionalDigits = 3;
	UPROPERTY(EditAnywhere, Category="Content")
	int32 MaximumFractionalDigits = 3;
	UPROPERTY(EditAnywhere, Category="Content")
	float ErrorTolerance_NearZero = 0.0001;

	FDlFmNumberFormat() { }

	FDlFmNumberFormat(
		bool bAlwaysSignVal,
		bool bUseGroupingVal,
		TEnumAsByte<ERoundingMode> RoundingModeVal,
		int32 MinimumIntegralDigitsVal,
		int32 MaximumIntegralDigitsVal,
		int32 MinimumFractionalDigitsVal,
		int32 MaximumFractionalDigitsVal,
		float ErrorTolerance_NearZeroVal)
	{
		bAlwaysSign = bAlwaysSignVal;
		bUseGrouping = bUseGroupingVal;
		RoundingMode = RoundingModeVal;
		MinimumIntegralDigits = MinimumIntegralDigitsVal;
		MaximumIntegralDigits = MaximumIntegralDigitsVal;
		MinimumFractionalDigits = MinimumFractionalDigitsVal;
		MaximumFractionalDigits = MaximumFractionalDigitsVal;
		ErrorTolerance_NearZero = ErrorTolerance_NearZeroVal;
	}
 
	void InitFormat(FNumberFormattingOptions& Target) const;

	static FText Float2Text(const float& Val, const FNumberFormattingOptions& NumFormat);
	static FText Int2Text(const int32& Val, const FNumberFormattingOptions& NumFormat);
	static float Text2Float(const FText& Val);
	static int32 Text2Int(const FText& Val);

	FNumberFormattingOptions GetOption() const
	{
		FNumberFormattingOptions Tmp;
		Tmp.AlwaysSign = bAlwaysSign;
		Tmp.UseGrouping = bUseGrouping;
		Tmp.RoundingMode = RoundingMode;
		Tmp.MinimumIntegralDigits = MinimumIntegralDigits;
		Tmp.MaximumIntegralDigits = MaximumIntegralDigits;
		Tmp.MinimumFractionalDigits = MinimumFractionalDigits;
		Tmp.MaximumFractionalDigits = MaximumFractionalDigits;
		return Tmp;
	}
};

namespace FDlFmNumberFormat_Default
{
	inline FDlFmNumberFormat Integer = {false, false, HalfFromZero, 1, 32, 0, 0, 0.001};
}
