// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateBrush.h"
#include "Rendering/DrawElements.h"
#include "Layout/Geometry.h"
#include "DlFmJoystick_Lib.generated.h"

class UDlFmJoystick;
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDlFmJoystick_HandleChanged, const FVector2D&, Value);

USTRUCT(BlueprintType)
struct FDlFmJoystick_DrawBox
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere,Category="Appearance")
	bool bVisible=true;
	UPROPERTY(EditAnywhere, Category = "Appearance")
	FSlateBrush Icon;
	UPROPERTY(EditAnywhere, Category = "Appearance")
	FLinearColor IconColor=FColor::White;
	UPROPERTY(EditAnywhere, Category = "Appearance")
	FVector2D Paint_OverflowPercent=FVector2D(1,1);
	UPROPERTY(EditAnywhere, Category = "Appearance")
	FVector2D Paint_OffsetPercent=FVector2D(0,0);
	void OnPaint(FSlateWindowElementList& ElementList,
		uint32 InLayer,
		const FGeometry& AllottedGeometry) const;
	void OnPaint(FSlateWindowElementList& ElementList,
		uint32 InLayer,
		const FGeometry& AllottedGeometry,
		const FVector2D& LocalOffset,
		const FVector2D&LocalSize) const;
};
USTRUCT(BlueprintType)
struct FDlFmJoystick_CommonConfig
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category="Behavior")
	bool bReverseX=false;
	UPROPERTY(EditAnywhere, Category="Behavior")
	bool bReverseY=false;
	
	UPROPERTY(EditAnywhere,Category="Appearance")
	FDlFmJoystick_DrawBox BackgroundDrawBox;
};
UENUM()
enum class EDlFmJoystick_ReachableAreaType:uint8
{
	Rectangle,
	Circle,
	Vertical,
	Horizontal,
	None,
};


USTRUCT(BlueprintType)
struct FDlFmJoystick_ReachableAreaConfig
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category="Behavior")
	EDlFmJoystick_ReachableAreaType Type=EDlFmJoystick_ReachableAreaType::Rectangle;
	
	UPROPERTY(EditAnywhere, Category="Behavior")
	bool bMarginByPercent=true;
	UPROPERTY(EditAnywhere,meta=(EditCondition="bIconSizeByPercent"), Category="Behavior")
	FMargin MarginByPercent=FMargin(0.05,0.05,0.05,0.05);
	UPROPERTY(EditAnywhere,meta=(EditCondition="!bIconSizeByPercent"), Category="Behavior")
	FMargin MarginByValue=FMargin(20,20,20,20);

	UPROPERTY(EditAnywhere,Category="Appearance")
	FDlFmJoystick_DrawBox DrawBox;
	
	//In widget geometry space
	void CheckSet(const FVector2D& WidgetSize,FVector2D &LocalCenter,FVector2D& ReachableHalfSize);
	FVector2D ClampPercent(const FVector2D& Src) const;

};

UENUM()
enum class EDlFmJoystick_ControlSource:uint8
{
	InputEvent_Waiting,
	InputEvent_Mouse,
	InputEvent_Touch,
	
	None_AcceptNothing,
	ProgramSet,
	Handle0FollowHandle1,
};

USTRUCT(BlueprintType)
struct FDlFmJoystick_HandleConfig
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere,Category="Appearance")
	FDlFmJoystick_DrawBox DrawBox;
	UPROPERTY(EditAnywhere,Category="Appearance")
	bool bIconSizeByPercent=true;
	UPROPERTY(EditAnywhere,Category="Appearance",meta=(EditCondition="bIconSizeByPercent"))
	float SizePercent_IconToReachableArea=0.2;
	UPROPERTY(EditAnywhere,Category="Appearance",meta=(EditCondition="!bIconSizeByPercent"))
	FVector2D SizeValue_Inch=FVector2D(20,20);
	
	UPROPERTY(EditAnywhere,Category="Behavior")
	bool bAutoBack_WhenNoControl=true;
	UPROPERTY(EditAnywhere,Category="Behavior", meta=(EditCondition="bAutoBack"))
	FVector2D AutoBackPosition=FVector2D::ZeroVector;
	UPROPERTY(EditAnywhere,Category="Behavior", meta=(EditCondition="bAutoBack"))
	float AutoBackSpeed=5;
	UPROPERTY(EditAnywhere,Category="Behavior")
	bool bLimitMoveSpeed=true;
	UPROPERTY(EditAnywhere,Category="Behavior")
	float MaxMoveSpeed=10;
	UPROPERTY(EditAnywhere,Category="Program")
	bool bBroadcastOnEvent=false;
	UPROPERTY(EditAnywhere,Category="Program")
	bool bBroadcastOnTick=true;
	
	void SetSize(const FVector2D& ReachableAreaHalfSize,FVector2D& ActualSize) const;
};

USTRUCT(BlueprintType)
struct FDlFmJoystick_PlayerConfig
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere,Category="Behavior")
	bool bAcceptInput_Mouse=false;
	UPROPERTY(EditAnywhere,Category="Behavior")
	bool bAcceptInput_Touch=true;
	UPROPERTY(EditAnywhere,Category="Behavior")
	float Speed_WhenFollowOtherHandle=1;
};
USTRUCT(Blueprintable)
struct FDlFmJoystick_Config
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category="Behavior")
	FDlFmJoystick_CommonConfig Common;
	UPROPERTY(EditAnywhere, Category="Behavior")
	FDlFmJoystick_ReachableAreaConfig ReachableArea;
	UPROPERTY(EditAnywhere, Category="Behavior")
	FDlFmJoystick_PlayerConfig Player;
	UPROPERTY(EditAnywhere, Category="Behavior")
	FDlFmJoystick_HandleConfig PlayerHandle;
	UPROPERTY(EditAnywhere, Category="Behavior")
	FDlFmJoystick_HandleConfig AIHandle;
};



