// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/StageCameraNavigation.h"

namespace StageCameraNavigation
{
	EStageCameraDrag ClassifyDrag(const FStageCameraButtons& Buttons)
	{
		if (!Buttons.AnyButton())
		{
			return EStageCameraDrag::None;
		}
		if (Buttons.bAlt)
		{
			// IsOrbitRotationMode / IsOrbitPanMode / IsOrbitZoomMode.
			if (Buttons.bLeft && !Buttons.bMiddle && !Buttons.bRight)
			{
				return EStageCameraDrag::Orbit;
			}
			if (Buttons.bMiddle)
			{
				return EStageCameraDrag::OrbitPan;
			}
			return EStageCameraDrag::OrbitDolly;
		}
		if (Buttons.bLeft && Buttons.bRight)
		{
			return EStageCameraDrag::PanVertical;
		}
		if (Buttons.bMiddle)
		{
			return EStageCameraDrag::Pan;
		}
		return Buttons.bRight ? EStageCameraDrag::Look : EStageCameraDrag::FlyAndTurn;
	}

	float SanitizeCameraSpeed(float Speed)
	{
		return FMath::IsFinite(Speed) ? FMath::Clamp(Speed, MinCameraSpeed, MaxCameraSpeed) : DefaultCameraSpeed;
	}

	float StepCameraSpeed(float Speed, float Notches)
	{
		float Result = SanitizeCameraSpeed(Speed);
		if (!FMath::IsFinite(Notches))
		{
			return Result;
		}
		// The wheel axis reports whole notches; a fractional remainder (a smooth-scrolling wheel) scales one last step.
		const int32 WholeSteps = FMath::Min(FMath::Abs(FMath::TruncToInt(Notches)), 1000);
		const float Remainder = FMath::Abs(Notches) - FMath::Abs(FMath::TruncToFloat(Notches));
		const float Direction = Notches > 0.f ? 1.f : -1.f;
		for (int32 Step = 0; Step < WholeSteps; ++Step)
		{
			Result += Direction * Result * SpeedStepFraction;
		}
		Result += Direction * Result * SpeedStepFraction * Remainder;
		return SanitizeCameraSpeed(Result);
	}

	float GetFlySpeed(float CameraSpeed)
	{
		return SanitizeCameraSpeed(CameraSpeed) * FlyCmPerSecondPerSpeed;
	}

	FVector StepFlightVelocity(const FVector& Velocity, const FVector& WorldImpulse, float CameraSpeed, float DeltaSeconds)
	{
		if (Velocity.ContainsNaN() || WorldImpulse.ContainsNaN() || !FMath::IsFinite(DeltaSeconds))
		{
			return FVector::ZeroVector;
		}
		const double Step = FMath::Clamp(DeltaSeconds, 0.f, MaxFlightStepSeconds);
		const FVector Accelerated = Velocity + WorldImpulse * (FlyAccelerationPerSpeed * SanitizeCameraSpeed(CameraSpeed) * Step);
		return Accelerated - Accelerated * FMath::Min(FlyDampingPerSecond * Step, static_cast<double>(MaxFlyDampingPerStep));
	}

	FVector GetFlyAndTurnTranslation(const FVector2D& Delta, double YawDegrees, float CameraSpeed)
	{
		const double Yaw = FMath::DegreesToRadians(YawDegrees);
		const double Amount = Delta.Y * DragCmPerCount * SanitizeCameraSpeed(CameraSpeed);
		return FVector(FMath::Cos(Yaw) * Amount, FMath::Sin(Yaw) * Amount, 0.0);
	}

	double GetFlyAndTurnYaw(const FVector2D& Delta)
	{
		return Delta.X * DegreesPerCount;
	}

	FVector GetPanTranslation(const FVector2D& Delta, const FRotator& View, float CameraSpeed)
	{
		// Camera-relative: the camera follows the mouse (bInvertMiddleMousePan off).
		return FQuat(View).RotateVector(FVector(0.0, Delta.X, Delta.Y)) * DragCmPerCount * SanitizeCameraSpeed(CameraSpeed);
	}

	FVector GetVerticalPanTranslation(const FVector2D& Delta, double YawDegrees, float CameraSpeed)
	{
		const double Yaw = FMath::DegreesToRadians(YawDegrees);
		const double Scale = DragCmPerCount * SanitizeCameraSpeed(CameraSpeed);
		return FVector(-Delta.X * FMath::Sin(Yaw), Delta.X * FMath::Cos(Yaw), Delta.Y) * Scale;
	}

	FRotator GetOrbitRotation(const FRotator& View, const FVector2D& Delta)
	{
		// SetViewRotation(GetViewRotation() + FRotator(Rot.Pitch, -Rot.Yaw, Rot.Roll)).
		return FRotator(View.Pitch + Delta.Y * DegreesPerCount, FRotator::NormalizeAxis(View.Yaw - Delta.X * DegreesPerCount), 0.0);
	}

	FVector GetOrbitLocation(const FVector& Pivot, const FRotator& View, double Distance)
	{
		return Pivot - View.Vector() * FMath::Max(Distance, MinOrbitDistance);
	}

	double GetDollyDistance(double Distance, const FVector2D& Delta, float CameraSpeed)
	{
		const double Closer = (Delta.X - Delta.Y) * DragCmPerCount * SanitizeCameraSpeed(CameraSpeed);
		return FMath::Max(Distance - Closer, MinOrbitDistance);
	}

	FVector GetWheelDolly(const FRotator& View, float Notches)
	{
		return FMath::IsFinite(Notches) ? View.Vector() * (WheelDollyCm * Notches) : FVector::ZeroVector;
	}

	FVector GetFocusLocation(const FBox& Bounds, const FRotator& View, float FOVDegrees, float Aspect, const FVector& Current)
	{
		if (!Bounds.IsValid || Bounds.ContainsNaN())
		{
			return Current;
		}
		double Radius = FMath::Max(Bounds.GetExtent().Size(), MinimumFocusRadius);
		// The editor fits the sphere into the shorter screen side.
		if (FMath::IsFinite(Aspect) && Aspect > 1.f)
		{
			Radius *= Aspect;
		}
		const double HalfFOV = FMath::DegreesToRadians(FMath::Clamp(FMath::IsFinite(FOVDegrees) ? FOVDegrees : 90.f, 5.f, 170.f) * 0.5);
		const double Distance = Radius / FMath::Tan(HalfFOV);
		return Bounds.GetCenter() - View.Vector() * Distance;
	}

	float EaseFocus(float Alpha)
	{
		const float T = FMath::Clamp(Alpha, 0.f, 1.f);
		return 1.f - FMath::Cube(1.f - T);
	}
}
