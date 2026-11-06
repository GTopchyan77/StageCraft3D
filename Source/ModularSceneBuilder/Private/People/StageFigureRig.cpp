// Copyright Epic Games, Inc. All Rights Reserved.

#include "People/StageFigureRig.h"

#include "UObject/UnrealType.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageFigureRig)

namespace StageFigure
{
	namespace
	{
		FQuat AboutX(double Degrees) { return FQuat(FVector::XAxisVector, FMath::DegreesToRadians(Degrees)); }
		FQuat AboutY(double Degrees) { return FQuat(FVector::YAxisVector, FMath::DegreesToRadians(Degrees)); }
		FQuat AboutZ(double Degrees) { return FQuat(FVector::ZAxisVector, FMath::DegreesToRadians(Degrees)); }

		int32 Index(EBone Bone) { return static_cast<int32>(Bone); }

		/**
		 * Joint conventions in this file (rest pose, limbs hanging along -Z):
		 * - AboutY(-a) swings a hanging limb forward (arm raised to the front, hip flexed, elbow bent); AboutY(+a) bends a knee.
		 * - AboutY(+a) leans the chest or nods the head forward.
		 * - AboutX(-a) lifts the left arm sideways, AboutX(+a) the right arm; AboutX(-a) leans the chest to the right.
		 * - AboutZ(+a) turns towards the figure's right.
		 */
		double Wave(double Time, double Cycles = 1.0, double Offset = 0.0)
		{
			return FMath::Sin(UE_TWO_PI * (Time * Cycles + Offset));
		}

		/** 0..1, peaking at Offset, for a hit or a step. */
		double Pulse(double Time, double Cycles = 1.0, double Offset = 0.0)
		{
			return 0.5 + 0.5 * FMath::Cos(UE_TWO_PI * (Time * Cycles - Offset));
		}

		constexpr double ThighLength = 42.0;
		constexpr double ShinLength = 41.0;

		/**
		 * Points an arm: the upper arm along ToElbow (from the shoulder) and the forearm along ToWrist (from the elbow), both in
		 * figure space. Side is -1 for the left arm. Assumes the chest and pelvis are not turned in the pose.
		 */
		void AimArm(FLocalPose& Pose, double Side, const FVector& ToElbow, const FVector& ToWrist)
		{
			const EBone Upper = Side < 0.0 ? EBone::UpperArmL : EBone::UpperArmR;
			const EBone Lower = Side < 0.0 ? EBone::ForearmL : EBone::ForearmR;
			const FVector RestUpper = (GetRestPivot(Lower) - GetRestPivot(Upper)).GetSafeNormal();
			const FVector RestLower = FVector::DownVector;
			const FQuat UpperGlobal = FQuat::FindBetweenNormals(RestUpper, ToElbow.GetSafeNormal());
			const FQuat LowerGlobal = FQuat::FindBetweenNormals(RestLower, ToWrist.GetSafeNormal());
			Pose.Rotations[Index(Upper)] = UpperGlobal;
			Pose.Rotations[Index(Lower)] = UpperGlobal.Inverse() * LowerGlobal;
		}

		void SetArmsRelaxed(FLocalPose& Pose, double Spread = 6.0)
		{
			Pose.Rotations[Index(EBone::UpperArmL)] = AboutX(-Spread);
			Pose.Rotations[Index(EBone::UpperArmR)] = AboutX(Spread);
			Pose.Rotations[Index(EBone::ForearmL)] = AboutY(-12.0);
			Pose.Rotations[Index(EBone::ForearmR)] = AboutY(-12.0);
		}

		FLocalPose CrowdIdle(double T)
		{
			FLocalPose Pose;
			const double Shift = Wave(T);
			Pose.RootOffset = FVector(0.0, 2.0 * Shift, -0.6 * FMath::Abs(Shift));
			Pose.Rotations[Index(EBone::Pelvis)] = AboutX(2.0 * Shift);
			Pose.Rotations[Index(EBone::Chest)] = AboutX(-3.0 * Shift) * AboutY(2.0);
			Pose.Rotations[Index(EBone::Head)] = AboutZ(8.0 * Wave(T, 1.0, 0.25)) * AboutY(-3.0);
			SetArmsRelaxed(Pose);
			// The leg carrying the weight stays straight; the other relaxes at the knee.
			Pose.Rotations[Index(EBone::ShinL)] = AboutY(6.0 * FMath::Max(0.0, Shift));
			Pose.Rotations[Index(EBone::ShinR)] = AboutY(6.0 * FMath::Max(0.0, -Shift));
			Pose.Rotations[Index(EBone::ThighL)] = AboutY(-3.0 * FMath::Max(0.0, Shift));
			Pose.Rotations[Index(EBone::ThighR)] = AboutY(-3.0 * FMath::Max(0.0, -Shift));
			return Pose;
		}

		FLocalPose CrowdBounce(double T)
		{
			FLocalPose Pose;
			// Down on the beat (T = 0), up between beats. The hips drop by what the bent legs lose in height, so the feet stay planted,
			// plus a small hop between beats.
			const double Down = Pulse(T);
			const double Knee = 36.0 * Down;
			const double Hip = Knee * 0.5;
			const double Drop = ThighLength * (1.0 - FMath::Cos(FMath::DegreesToRadians(Hip))) + ShinLength * (1.0 - FMath::Cos(FMath::DegreesToRadians(Knee - Hip)));
			Pose.RootOffset = FVector(0.0, 0.0, -Drop + 2.5 * (1.0 - Down));
			Pose.Rotations[Index(EBone::ThighL)] = AboutY(-Hip);
			Pose.Rotations[Index(EBone::ThighR)] = AboutY(-Hip);
			Pose.Rotations[Index(EBone::ShinL)] = AboutY(Knee);
			Pose.Rotations[Index(EBone::ShinR)] = AboutY(Knee);
			Pose.Rotations[Index(EBone::Pelvis)] = AboutY(4.0 * Down);
			Pose.Rotations[Index(EBone::Chest)] = AboutY(4.0 * Down);
			Pose.Rotations[Index(EBone::Head)] = AboutY(10.0 * Down - 4.0);
			// One fist pumps on the beat, the other arm stays low.
			Pose.Rotations[Index(EBone::UpperArmR)] = AboutY(-150.0 + 25.0 * Down) * AboutX(10.0);
			Pose.Rotations[Index(EBone::ForearmR)] = AboutY(-20.0 - 30.0 * Down);
			Pose.Rotations[Index(EBone::UpperArmL)] = AboutX(-10.0) * AboutY(-15.0);
			Pose.Rotations[Index(EBone::ForearmL)] = AboutY(-55.0);
			return Pose;
		}

		FLocalPose CrowdCheer(double T)
		{
			FLocalPose Pose;
			const double Sway = Wave(T);
			Pose.RootOffset = FVector(0.0, 0.0, 2.0 * Pulse(T, 2.0));
			Pose.Rotations[Index(EBone::Chest)] = AboutX(-5.0 * Sway) * AboutY(-4.0);
			Pose.Rotations[Index(EBone::Head)] = AboutY(-14.0);
			// Both arms up in a V, waving side to side together.
			Pose.Rotations[Index(EBone::UpperArmL)] = AboutX(-150.0 + 18.0 * Sway);
			Pose.Rotations[Index(EBone::UpperArmR)] = AboutX(150.0 + 18.0 * Sway);
			Pose.Rotations[Index(EBone::ForearmL)] = AboutX(-10.0 * Sway);
			Pose.Rotations[Index(EBone::ForearmR)] = AboutX(-10.0 * Sway);
			return Pose;
		}

		FLocalPose CrowdClap(double T)
		{
			FLocalPose Pose;
			// Two claps per loop: the hands meet in front of the chest at T = 0 and 0.5 and open between.
			const double Open = 1.0 - Pulse(T, 2.0);
			Pose.RootOffset = FVector(0.0, 0.0, -1.5 * Pulse(T, 2.0));
			Pose.Rotations[Index(EBone::Head)] = AboutY(-6.0 + 4.0 * Pulse(T, 2.0));
			for (const double Side : { -1.0, 1.0 })
			{
				// Elbow out to the side and down, forearm in towards the middle (directions from the shoulder and the elbow).
				const FVector ToElbow(10.0, Side * (6.0 + 4.0 * Open), -22.0);
				const FVector ToWrist(18.0, -Side * (26.0 - 18.0 * Open), 4.0 + 2.0 * Open);
				AimArm(Pose, Side, ToElbow, ToWrist);
			}
			return Pose;
		}

		FLocalPose Drummer(double T)
		{
			FLocalPose Pose;
			// Seated on a throne about 56 cm high, knees apart; thighs at 70 degrees so the shins come down to the pedals. The right
			// foot works the bass drum on the beat.
			Pose.RootOffset = FVector(-4.0, 0.0, -28.0);
			Pose.Rotations[Index(EBone::ThighL)] = AboutZ(-12.0) * AboutY(-70.0);
			Pose.Rotations[Index(EBone::ThighR)] = AboutZ(12.0) * AboutY(-70.0);
			Pose.Rotations[Index(EBone::ShinL)] = AboutY(70.0);
			Pose.Rotations[Index(EBone::ShinR)] = AboutY(70.0 - 10.0 * Pulse(T, 1.0, 0.0));
			Pose.Rotations[Index(EBone::Chest)] = AboutY(8.0) * AboutZ(6.0 * Wave(T, 0.5));
			Pose.Rotations[Index(EBone::Head)] = AboutY(4.0 + 10.0 * Pulse(T, 1.0));
			// Sticks: the hands alternate, each hitting twice a loop.
			const double HitL = Pulse(T, 2.0, 0.0);
			const double HitR = Pulse(T, 2.0, 0.5);
			Pose.Rotations[Index(EBone::UpperArmL)] = AboutY(-38.0) * AboutX(-22.0);
			Pose.Rotations[Index(EBone::UpperArmR)] = AboutY(-38.0) * AboutX(22.0);
			Pose.Rotations[Index(EBone::ForearmL)] = AboutY(-20.0 + 25.0 * HitL);
			Pose.Rotations[Index(EBone::ForearmR)] = AboutY(-20.0 + 25.0 * HitR);
			return Pose;
		}

		FLocalPose Guitarist(double T)
		{
			FLocalPose Pose;
			const double Nod = Pulse(T, 2.0);
			Pose.RootOffset = FVector(0.0, 0.0, -2.0 * Nod);
			Pose.Rotations[Index(EBone::ThighL)] = AboutY(-8.0) * AboutX(6.0);
			Pose.Rotations[Index(EBone::ThighR)] = AboutX(-6.0);
			Pose.Rotations[Index(EBone::ShinL)] = AboutY(10.0);
			Pose.Rotations[Index(EBone::Chest)] = AboutY(6.0 + 4.0 * Nod) * AboutZ(-8.0);
			Pose.Rotations[Index(EBone::Head)] = AboutY(14.0 * Nod - 2.0);
			// Fretting hand out at the neck (to the left), strumming hand at the body, four strokes a loop.
			Pose.Rotations[Index(EBone::UpperArmL)] = AboutY(-35.0) * AboutX(-30.0);
			Pose.Rotations[Index(EBone::ForearmL)] = AboutY(-60.0) * AboutZ(-35.0 + 6.0 * Wave(T, 0.5));
			Pose.Rotations[Index(EBone::UpperArmR)] = AboutY(-25.0) * AboutX(18.0);
			Pose.Rotations[Index(EBone::ForearmR)] = AboutY(-80.0 + 18.0 * Wave(T, 4.0)) * AboutZ(30.0);
			return Pose;
		}

		FLocalPose Vocalist(double T)
		{
			FLocalPose Pose;
			const double Sway = Wave(T);
			Pose.RootOffset = FVector(0.0, 3.0 * Sway, -1.0 * Pulse(T, 2.0));
			Pose.Rotations[Index(EBone::Pelvis)] = AboutX(3.0 * Sway);
			Pose.Rotations[Index(EBone::Chest)] = AboutX(-5.0 * Sway) * AboutY(3.0);
			Pose.Rotations[Index(EBone::Head)] = AboutY(-6.0 + 5.0 * Pulse(T, 2.0)) * AboutZ(6.0 * Sway);
			// Right hand holds the microphone at the mouth; the left arm reaches out to the audience and back.
			Pose.Rotations[Index(EBone::UpperArmR)] = AboutY(-40.0) * AboutX(25.0);
			Pose.Rotations[Index(EBone::ForearmR)] = AboutY(-125.0) * AboutZ(-35.0);
			const double Reach = Pulse(T, 1.0, 0.25);
			Pose.Rotations[Index(EBone::UpperArmL)] = AboutY(-30.0 - 55.0 * Reach) * AboutX(-25.0 - 15.0 * Reach);
			Pose.Rotations[Index(EBone::ForearmL)] = AboutY(-50.0 + 35.0 * Reach);
			Pose.Rotations[Index(EBone::ShinL)] = AboutY(8.0 * FMath::Max(0.0, Sway));
			Pose.Rotations[Index(EBone::ShinR)] = AboutY(8.0 * FMath::Max(0.0, -Sway));
			return Pose;
		}

		FLocalPose Keyboardist(double T)
		{
			FLocalPose Pose;
			const double Nod = Pulse(T, 2.0);
			Pose.Rotations[Index(EBone::Chest)] = AboutY(10.0 + 3.0 * Nod) * AboutZ(5.0 * Wave(T));
			Pose.Rotations[Index(EBone::Head)] = AboutY(18.0 + 8.0 * Nod);
			// Both hands forward and down on the keys at hip height, alternating presses.
			const double PressL = Pulse(T, 4.0, 0.0);
			const double PressR = Pulse(T, 4.0, 0.5);
			Pose.Rotations[Index(EBone::UpperArmL)] = AboutY(-28.0) * AboutX(-12.0);
			Pose.Rotations[Index(EBone::UpperArmR)] = AboutY(-28.0) * AboutX(12.0);
			Pose.Rotations[Index(EBone::ForearmL)] = AboutY(-62.0 + 8.0 * PressL) * AboutZ(18.0 + 10.0 * Wave(T, 0.5));
			Pose.Rotations[Index(EBone::ForearmR)] = AboutY(-62.0 + 8.0 * PressR) * AboutZ(-18.0 + 10.0 * Wave(T, 0.5));
			return Pose;
		}

		FLocalPose DeskOperator(double T)
		{
			FLocalPose Pose;
			// Leaning on a desk about 90 cm high, eyes going between the desk and the stage.
			const double Look = Pulse(T, 1.0, 0.0);
			Pose.Rotations[Index(EBone::Chest)] = AboutY(14.0);
			Pose.Rotations[Index(EBone::Head)] = AboutY(28.0 - 38.0 * Look) * AboutZ(10.0 * Wave(T, 0.5));
			const double Fader = Wave(T, 2.0);
			Pose.Rotations[Index(EBone::UpperArmL)] = AboutY(-26.0) * AboutX(-10.0);
			Pose.Rotations[Index(EBone::UpperArmR)] = AboutY(-26.0 - 6.0 * Fader) * AboutX(10.0);
			Pose.Rotations[Index(EBone::ForearmL)] = AboutY(-58.0) * AboutZ(15.0);
			Pose.Rotations[Index(EBone::ForearmR)] = AboutY(-58.0 - 8.0 * Fader) * AboutZ(-15.0);
			return Pose;
		}

		FLocalPose Stagehand(double T)
		{
			FLocalPose Pose;
			// Arms folded, weight on one leg, looking around now and then.
			Pose.Rotations[Index(EBone::Pelvis)] = AboutX(3.0);
			Pose.RootOffset = FVector(0.0, 2.5, 0.0);
			Pose.Rotations[Index(EBone::Head)] = AboutZ(35.0 * Wave(T) * FMath::Abs(Wave(T))) * AboutY(-2.0);
			Pose.Rotations[Index(EBone::Chest)] = AboutZ(6.0 * Wave(T));
			Pose.Rotations[Index(EBone::UpperArmL)] = AboutY(-25.0) * AboutZ(-10.0);
			Pose.Rotations[Index(EBone::UpperArmR)] = AboutY(-25.0) * AboutZ(10.0);
			Pose.Rotations[Index(EBone::ForearmL)] = AboutY(-95.0) * AboutX(70.0);
			Pose.Rotations[Index(EBone::ForearmR)] = AboutY(-95.0) * AboutX(-70.0);
			Pose.Rotations[Index(EBone::ShinR)] = AboutY(10.0);
			Pose.Rotations[Index(EBone::ThighR)] = AboutY(-4.0) * AboutX(-4.0);
			return Pose;
		}

		// --- Mesh building ---

		struct FBuilder
		{
			FMeshData& Mesh;

			int32 AddVertex(const FVector& Position, const FVector& Normal, const FVector2D& UV, EBone Bone, float Region)
			{
				Mesh.Positions.Add(FVector3f(Position));
				Mesh.Normals.Add(FVector3f(Normal));
				Mesh.UVs.Add(FVector2f(UV));
				Mesh.Bones.Add(Index(Bone));
				Mesh.Regions.Add(Region);
				return Mesh.Positions.Num() - 1;
			}

			/** One triangle, wound so that it faces Facing (the convention of StageDeckMath::GetFaceNormal and static meshes). */
			void AddTriangle(int32 A, int32 B, int32 C, const FVector& Facing)
			{
				const FVector P0(Mesh.Positions[A]);
				const FVector P1(Mesh.Positions[B]);
				const FVector P2(Mesh.Positions[C]);
				const FVector Normal = (P1 - P2) ^ (P0 - P2);
				if ((Normal | Facing) >= 0.0)
				{
					Mesh.Indices.Append({ A, B, C });
				}
				else
				{
					Mesh.Indices.Append({ A, C, B });
				}
			}

			void AddQuad(const FVector& A, const FVector& B, const FVector& C, const FVector& D, const FVector& Normal, EBone Bone, float Region)
			{
				const int32 IA = AddVertex(A, Normal, FVector2D(0.0, 0.0), Bone, Region);
				const int32 IB = AddVertex(B, Normal, FVector2D(1.0, 0.0), Bone, Region);
				const int32 IC = AddVertex(C, Normal, FVector2D(1.0, 1.0), Bone, Region);
				const int32 ID = AddVertex(D, Normal, FVector2D(0.0, 1.0), Bone, Region);
				AddTriangle(IA, IB, IC, Normal);
				AddTriangle(IA, IC, ID, Normal);
			}

			/** A box from Bottom (centre of the lower face, size BottomSize in X and Y) to Top, so a box can taper. Flat faces. */
			void AddTaperedBox(const FVector& Bottom, const FVector2D& BottomSize, const FVector& Top, const FVector2D& TopSize, EBone Bone, float Region)
			{
				const auto Corner = [](const FVector& Centre, const FVector2D& Size, double SX, double SY)
				{
					return Centre + FVector(SX * Size.X * 0.5, SY * Size.Y * 0.5, 0.0);
				};
				const FVector B[4] = { Corner(Bottom, BottomSize, -1, -1), Corner(Bottom, BottomSize, 1, -1), Corner(Bottom, BottomSize, 1, 1), Corner(Bottom, BottomSize, -1, 1) };
				const FVector T[4] = { Corner(Top, TopSize, -1, -1), Corner(Top, TopSize, 1, -1), Corner(Top, TopSize, 1, 1), Corner(Top, TopSize, -1, 1) };
				const FVector Centre = (Bottom + Top) * 0.5;
				AddQuad(B[0], B[1], B[2], B[3], FVector::DownVector, Bone, Region);
				AddQuad(T[0], T[1], T[2], T[3], FVector::UpVector, Bone, Region);
				for (int32 Side = 0; Side < 4; ++Side)
				{
					const int32 Next = (Side + 1) % 4;
					const FVector FaceCentre = (B[Side] + B[Next] + T[Side] + T[Next]) * 0.25;
					FVector Normal = ((B[Next] - B[Side]) ^ (T[Side] - B[Side])).GetSafeNormal();
					if ((Normal | (FaceCentre - Centre)) < 0.0)
					{
						Normal = -Normal;
					}
					AddQuad(B[Side], B[Next], T[Next], T[Side], Normal, Bone, Region);
				}
			}

			void AddBox(const FVector& Centre, const FVector& Size, EBone Bone, float Region)
			{
				const FVector2D Footprint(Size.X, Size.Y);
				AddTaperedBox(Centre - FVector(0.0, 0.0, Size.Z * 0.5), Footprint, Centre + FVector(0.0, 0.0, Size.Z * 0.5), Footprint, Bone, Region);
			}

			/** A prism of Sides sides from A (radius RadiusA) to B (radius RadiusB): smooth sides, flat caps. */
			void AddLimb(const FVector& A, const FVector& B, double RadiusA, double RadiusB, int32 Sides, EBone Bone, float Region)
			{
				const FVector Axis = (B - A).GetSafeNormal();
				FVector U = (Axis ^ FVector::XAxisVector);
				if (U.SizeSquared() < 1e-4)
				{
					U = Axis ^ FVector::YAxisVector;
				}
				U.Normalize();
				const FVector W = (Axis ^ U).GetSafeNormal();

				TArray<int32> RingA;
				TArray<int32> RingB;
				for (int32 Side = 0; Side < Sides; ++Side)
				{
					// Half a step of offset, so a four-sided limb is a box aligned with the body, not a diamond.
					const double Angle = UE_TWO_PI * (Side + 0.5) / Sides;
					const FVector Radial = U * FMath::Cos(Angle) + W * FMath::Sin(Angle);
					RingA.Add(AddVertex(A + Radial * RadiusA, Radial, FVector2D(double(Side) / Sides, 0.0), Bone, Region));
					RingB.Add(AddVertex(B + Radial * RadiusB, Radial, FVector2D(double(Side) / Sides, 1.0), Bone, Region));
				}
				for (int32 Side = 0; Side < Sides; ++Side)
				{
					const int32 Next = (Side + 1) % Sides;
					const double Mid = UE_TWO_PI * (Side + 1.0) / Sides;
					const FVector Facing = U * FMath::Cos(Mid) + W * FMath::Sin(Mid);
					AddTriangle(RingA[Side], RingA[Next], RingB[Next], Facing);
					AddTriangle(RingA[Side], RingB[Next], RingB[Side], Facing);
				}
				// Caps as fans of their own vertices (flat normals).
				for (int32 End = 0; End < 2; ++End)
				{
					const FVector Centre = End == 0 ? A : B;
					const double Radius = End == 0 ? RadiusA : RadiusB;
					const FVector Facing = End == 0 ? -Axis : Axis;
					TArray<int32> Cap;
					for (int32 Side = 0; Side < Sides; ++Side)
					{
						const double Angle = UE_TWO_PI * (Side + 0.5) / Sides;
						const FVector Radial = U * FMath::Cos(Angle) + W * FMath::Sin(Angle);
						Cap.Add(AddVertex(Centre + Radial * Radius, Facing, FVector2D(0.5 + 0.5 * FMath::Cos(Angle), 0.5 + 0.5 * FMath::Sin(Angle)), Bone, Region));
					}
					for (int32 Side = 1; Side + 1 < Sides; ++Side)
					{
						AddTriangle(Cap[0], Cap[Side], Cap[Side + 1], Facing);
					}
				}
			}
		};

		/** Left or right limb pieces: Side is -1 for left (-Y) and +1 for right. */
		void AddArm(FBuilder& Builder, double Side, int32 Sides, bool bHand)
		{
			const EBone Upper = Side < 0 ? EBone::UpperArmL : EBone::UpperArmR;
			const EBone Lower = Side < 0 ? EBone::ForearmL : EBone::ForearmR;
			const FVector Shoulder = GetRestPivot(Upper);
			const FVector Elbow = GetRestPivot(Lower);
			const FVector Wrist(0.0, Elbow.Y, 93.0);
			Builder.AddLimb(Shoulder + FVector(0.0, 0.0, 1.0), Elbow, 5.2, 4.4, Sides, Upper, RegionTop);
			Builder.AddLimb(Elbow, Wrist, 4.2, 3.4, Sides, Lower, RegionSkin);
			if (bHand)
			{
				Builder.AddBox(Wrist - FVector(0.0, 0.0, 5.0), FVector(7.0, 3.5, 10.0), Lower, RegionSkin);
			}
		}

		void AddLeg(FBuilder& Builder, double Side, int32 Sides, bool bShoe)
		{
			const EBone Upper = Side < 0 ? EBone::ThighL : EBone::ThighR;
			const EBone Lower = Side < 0 ? EBone::ShinL : EBone::ShinR;
			const FVector Hip = GetRestPivot(Upper);
			const FVector Knee = GetRestPivot(Lower);
			const FVector Ankle(0.0, Knee.Y, 8.0);
			Builder.AddLimb(Hip, Knee, 8.0, 5.8, Sides, Upper, RegionBottom);
			Builder.AddLimb(Knee, Ankle, 5.6, 4.2, Sides, Lower, RegionBottom);
			if (bShoe)
			{
				Builder.AddBox(FVector(4.0, Knee.Y, 4.0), FVector(26.0, 10.5, 8.0), Lower, RegionDark);
			}
		}
	}

	FLocalPose::FLocalPose()
	{
		for (FQuat& Rotation : Rotations)
		{
			Rotation = FQuat::Identity;
		}
	}

	EBone GetParent(EBone Bone)
	{
		switch (Bone)
		{
		case EBone::Chest:		return EBone::Pelvis;
		case EBone::Head:		return EBone::Chest;
		case EBone::UpperArmL:	return EBone::Chest;
		case EBone::ForearmL:	return EBone::UpperArmL;
		case EBone::UpperArmR:	return EBone::Chest;
		case EBone::ForearmR:	return EBone::UpperArmR;
		case EBone::ThighL:		return EBone::Pelvis;
		case EBone::ShinL:		return EBone::ThighL;
		case EBone::ThighR:		return EBone::Pelvis;
		case EBone::ShinR:		return EBone::ThighR;
		default:				return EBone::Count;
		}
	}

	FVector GetRestPivot(EBone Bone)
	{
		switch (Bone)
		{
		case EBone::Pelvis:		return FVector(0.0, 0.0, 95.0);
		case EBone::Chest:		return FVector(0.0, 0.0, 100.0);
		case EBone::Head:		return FVector(0.0, 0.0, 149.0);
		case EBone::UpperArmL:	return FVector(0.0, -20.0, 144.0);
		case EBone::ForearmL:	return FVector(0.0, -20.5, 117.0);
		case EBone::UpperArmR:	return FVector(0.0, 20.0, 144.0);
		case EBone::ForearmR:	return FVector(0.0, 20.5, 117.0);
		case EBone::ThighL:		return FVector(0.0, -9.5, 91.0);
		case EBone::ShinL:		return FVector(0.0, -9.5, 49.0);
		case EBone::ThighR:		return FVector(0.0, 9.5, 91.0);
		case EBone::ShinR:		return FVector(0.0, 9.5, 49.0);
		default:				return FVector::ZeroVector;
		}
	}

	FPose Solve(const FLocalPose& Local)
	{
		FPose Pose;
		TStaticArray<FQuat, BoneCount> Global;
		TStaticArray<FVector, BoneCount> Pivot;
		for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
		{
			const EBone Bone = static_cast<EBone>(BoneIndex);
			const EBone Parent = GetParent(Bone);
			const FVector Rest = GetRestPivot(Bone);
			if (Parent == EBone::Count)
			{
				Global[BoneIndex] = Local.Rotations[BoneIndex].GetNormalized();
				Pivot[BoneIndex] = Rest + Local.RootOffset;
			}
			else
			{
				// Parents come first in EBone, so theirs are already solved.
				const int32 ParentIndex = Index(Parent);
				Global[BoneIndex] = (Global[ParentIndex] * Local.Rotations[BoneIndex]).GetNormalized();
				Pivot[BoneIndex] = Pivot[ParentIndex] + Global[ParentIndex].RotateVector(Rest - GetRestPivot(Parent));
			}
			// A rest vertex v of the bone ends at Pivot + R (v - Rest) = R v + (Pivot - R Rest).
			Pose[BoneIndex].Rotation = Global[BoneIndex];
			Pose[BoneIndex].Translation = Pivot[BoneIndex] - Global[BoneIndex].RotateVector(Rest);
		}
		return Pose;
	}

	FPose EvaluateClip(EStageFigureClip Clip, double Time)
	{
		const double T = FMath::IsFinite(Time) ? Time - FMath::FloorToDouble(Time) : 0.0;
		switch (Clip)
		{
		case EStageFigureClip::CrowdIdle:		return Solve(CrowdIdle(T));
		case EStageFigureClip::CrowdBounce:		return Solve(CrowdBounce(T));
		case EStageFigureClip::CrowdCheer:		return Solve(CrowdCheer(T));
		case EStageFigureClip::CrowdClap:		return Solve(CrowdClap(T));
		case EStageFigureClip::Drummer:			return Solve(Drummer(T));
		case EStageFigureClip::Guitarist:		return Solve(Guitarist(T));
		case EStageFigureClip::Vocalist:		return Solve(Vocalist(T));
		case EStageFigureClip::Keyboardist:		return Solve(Keyboardist(T));
		case EStageFigureClip::DeskOperator:	return Solve(DeskOperator(T));
		case EStageFigureClip::Stagehand:		return Solve(Stagehand(T));
		default:								return Solve(FLocalPose());
		}
	}

	float GetClipCyclesPerSecond(EStageFigureClip Clip)
	{
		// A beat at 128 BPM is 2.133 Hz; clips with two hits a loop run at half of it.
		constexpr float Beat = 128.f / 60.f;
		switch (Clip)
		{
		case EStageFigureClip::CrowdIdle:		return 0.25f;
		case EStageFigureClip::CrowdBounce:		return Beat;
		case EStageFigureClip::CrowdCheer:		return 0.5f;
		case EStageFigureClip::CrowdClap:		return Beat * 0.5f;
		case EStageFigureClip::Drummer:			return Beat * 0.5f;
		case EStageFigureClip::Guitarist:		return Beat * 0.5f;
		case EStageFigureClip::Vocalist:		return Beat * 0.25f;
		case EStageFigureClip::Keyboardist:		return Beat * 0.5f;
		case EStageFigureClip::DeskOperator:	return 0.125f;
		case EStageFigureClip::Stagehand:		return 0.1f;
		default:								return 1.f;
		}
	}

	FString GetClipName(EStageFigureClip Clip)
	{
		const UEnum* Enum = StaticEnum<EStageFigureClip>();
		return Enum ? Enum->GetDisplayNameTextByValue(static_cast<int64>(Clip)).ToString() : FString::FromInt(static_cast<int32>(Clip));
	}

	bool FMeshData::IsValid() const
	{
		const int32 Count = Positions.Num();
		if (Count == 0 || Normals.Num() != Count || UVs.Num() != Count || Bones.Num() != Count || Regions.Num() != Count
			|| Indices.IsEmpty() || Indices.Num() % 3 != 0)
		{
			return false;
		}
		for (const int32 VertexIndex : Indices)
		{
			if (!Positions.IsValidIndex(VertexIndex))
			{
				return false;
			}
		}
		for (const int32 Bone : Bones)
		{
			if (Bone < 0 || Bone >= BoneCount)
			{
				return false;
			}
		}
		return true;
	}

	FMeshData BuildMesh(int32 Lod)
	{
		FMeshData Mesh;
		if (Lod < 0 || Lod >= LodCount)
		{
			return Mesh;
		}
		FBuilder Builder{ Mesh };

		if (Lod == LodCount - 1)
		{
			// Far away: five boxes, no hands or feet. The arms still swing with their upper arm, so a cheering crowd reads.
			Builder.AddBox(FVector(0.0, 0.0, 70.0), FVector(18.0, 32.0, 50.0), EBone::Pelvis, RegionBottom);
			Builder.AddTaperedBox(FVector(0.0, 0.0, 96.0), FVector2D(20.0, 30.0), FVector(0.0, 0.0, 146.0), FVector2D(22.0, 40.0), EBone::Chest, RegionTop);
			Builder.AddBox(FVector(1.0, 0.0, 162.0), FVector(18.0, 16.0, 24.0), EBone::Head, RegionSkin);
			Builder.AddBox(FVector(0.0, -21.0, 119.0), FVector(8.0, 7.0, 50.0), EBone::UpperArmL, RegionTop);
			Builder.AddBox(FVector(0.0, 21.0, 119.0), FVector(8.0, 7.0, 50.0), EBone::UpperArmR, RegionTop);
			Builder.AddBox(FVector(0.0, 0.0, 24.0), FVector(18.0, 30.0, 48.0), EBone::Pelvis, RegionDark);
			return Mesh;
		}

		const bool bDetailed = Lod == 0;
		const int32 Sides = bDetailed ? 8 : 4;

		// Hips, torso widening to the shoulders, neck, head with hair.
		Builder.AddTaperedBox(FVector(0.0, 0.0, 84.0), FVector2D(21.0, 33.0), FVector(0.0, 0.0, 101.0), FVector2D(20.0, 31.0), EBone::Pelvis, RegionBottom);
		Builder.AddTaperedBox(FVector(0.0, 0.0, 100.0), FVector2D(19.0, 30.0), FVector(0.0, 0.0, 146.0), FVector2D(22.0, 40.0), EBone::Chest, RegionTop);
		Builder.AddLimb(FVector(0.0, 0.0, 144.0), FVector(0.0, 0.0, 154.0), 5.0, 4.6, Sides, EBone::Head, RegionSkin);
		if (bDetailed)
		{
			Builder.AddTaperedBox(FVector(1.0, 0.0, 152.0), FVector2D(17.0, 14.5), FVector(1.0, 0.0, 168.0), FVector2D(19.5, 16.5), EBone::Head, RegionSkin);
			Builder.AddTaperedBox(FVector(-0.5, 0.0, 168.0), FVector2D(20.5, 17.5), FVector(-1.0, 0.0, 175.0), FVector2D(17.0, 14.0), EBone::Head, RegionDark);
		}
		else
		{
			Builder.AddBox(FVector(1.0, 0.0, 162.5), FVector(19.0, 16.0, 25.0), EBone::Head, RegionSkin);
		}
		AddArm(Builder, -1.0, Sides, bDetailed);
		AddArm(Builder, 1.0, Sides, bDetailed);
		AddLeg(Builder, -1.0, Sides, true);
		AddLeg(Builder, 1.0, Sides, true);
		return Mesh;
	}

	TArray<FVector4f> BakeAnimationTexels()
	{
		TArray<FVector4f> Texels;
		Texels.SetNumZeroed(TextureWidth * TextureHeight);
		for (int32 ClipIndex = 0; ClipIndex < ClipCount; ++ClipIndex)
		{
			TStaticArray<FQuat, BoneCount> Previous;
			for (int32 Frame = 0; Frame < FramesPerClip; ++Frame)
			{
				const FPose Pose = EvaluateClip(static_cast<EStageFigureClip>(ClipIndex), double(Frame) / FramesPerClip);
				const int32 Row = ClipIndex * FramesPerClip + Frame;
				for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
				{
					FQuat Rotation = Pose[BoneIndex].Rotation;
					// q and -q are the same rotation; the material blends neighbouring frames, so keep each bone in one hemisphere.
					if (Frame > 0 && (Rotation | Previous[BoneIndex]) < 0.0)
					{
						Rotation = -Rotation;
					}
					Previous[BoneIndex] = Rotation;
					const FVector& Translation = Pose[BoneIndex].Translation;
					Texels[Row * TextureWidth + BoneIndex * 2] = FVector4f(Rotation.X, Rotation.Y, Rotation.Z, Rotation.W);
					Texels[Row * TextureWidth + BoneIndex * 2 + 1] = FVector4f(Translation.X, Translation.Y, Translation.Z, 0.f);
				}
			}
		}
		return Texels;
	}
}
