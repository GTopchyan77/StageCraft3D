// Copyright 2025-2026 NGG. All Rights Reserved.
//
// NGGAnimation.cpp
//
// Implementation of the AnimGraph authoring endpoints:
//   /anim/configure                     — state machines, states, transitions
//   /anim/add_{two_bone_ik,copy_bone,hand_ik_retargeting,layered_bone_blend,
//              sequence_player,modify_bone,look_at,aim_offset_blend_space}
//   /anim/delete_node
//   /skeleton/add_virtual_bone
//
// Creating the Anim Blueprint asset itself lives in NGGBlueprintAssets.cpp —
// this file only edits the graph inside one that already exists. Each splice
// helper is kept in its own anonymous namespace next to the handler that uses
// it, which is how the file read before it was split out.

#include "NGGHttpServer.h"
#include "UnrealNGGMCPModule.h"

// ---- UE5 HTTP Server -------------------------------------------------------
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
// ---- Engine ----------------------------------------------------------------
#include "Async/Async.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Editor.h"
// ---- Asset tools -----------------------------------------------------------
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "FileHelpers.h"          // UEditorLoadingAndSavingUtils
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
// ---- Animation Blueprint editor APIs ---------------------------------------
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateGraph.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimationTransitionGraph.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimationStateMachineSchema.h"
// ---- Two Bone IK authoring (HandleAnimAddTwoBoneIK) ------------------------
#include "AnimGraphNode_TwoBoneIK.h"
#include "AnimGraphNode_Root.h"
#include "BoneControllers/AnimNode_TwoBoneIK.h"
#include "Animation/AnimTypes.h"
// ---- Copy Bone + Hand IK Retargeting (HandleAnimAddCopyBone / HandleAnimAddHandIKRetargeting) ----
#include "AnimGraphNode_CopyBone.h"
#include "AnimGraphNode_HandIKRetargeting.h"
#include "BoneControllers/AnimNode_CopyBone.h"
#include "BoneControllers/AnimNode_HandIKRetargeting.h"
// ---- Layered Bone Blend + Sequence Player splice (HandleAnimAddLayeredBoneBlend / HandleAnimAddSequencePlayer) ----
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimNodes/AnimNode_LayeredBoneBlend.h"
#include "Animation/AnimData/BoneMaskFilter.h"
#include "Animation/AnimNode_SequencePlayer.h"
// ---- Modify Bone (HandleAnimAddModifyBone) ---------------------------------
#include "AnimGraphNode_ModifyBone.h"
#include "BoneControllers/AnimNode_ModifyBone.h"
// ---- Look At (HandleAnimAddLookAt) -----------------------------------------
#include "AnimGraphNode_LookAt.h"
#include "BoneControllers/AnimNode_LookAt.h"
// ---- Aim Offset / Rotation Offset Blend Space (HandleAnimAddAimOffsetBlendSpace) ----
#include "AnimGraphNode_RotationOffsetBlendSpace.h"
#include "AnimNodes/AnimNode_RotationOffsetBlendSpace.h"
#include "Animation/AimOffsetBlendSpace.h"
#include "Animation/AimOffsetBlendSpace1D.h"
// ---- Skeleton --------------------------------------------------------------
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"


// ============================================================================
// Handler: POST /editor/configure_anim_blueprint
//
// Wire an Animation Blueprint's state machine with animation sequences/blend
// spaces.  Creates states, assigns sequence/blend-space players, and adds
// transition rules based on variable conditions.
// ============================================================================

bool FNGGHttpServer::HandleConfigureAnimBlueprint(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString AssetPath; Body->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("asset_path is required")));
		return true;
	}

	// Parse states array
	struct FStateInput
	{
		FString Name;
		FString AnimationPath;
		bool bLoop = true;
	};
	TArray<FStateInput> States;
	{
		const TArray<TSharedPtr<FJsonValue>>* StatesArr = nullptr;
		if (Body->TryGetArrayField(TEXT("states"), StatesArr) && StatesArr)
		{
			for (const auto& Val : *StatesArr)
			{
				const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
				if (!Obj) continue;
				FStateInput S;
				Obj->TryGetStringField(TEXT("name"), S.Name);
				Obj->TryGetStringField(TEXT("animation"), S.AnimationPath);
				Obj->TryGetBoolField(TEXT("loop"), S.bLoop);
				States.Add(MoveTemp(S));
			}
		}
	}

	// Parse transitions array
	struct FTransInput
	{
		FString From;
		FString To;
		FString Condition;
	};
	TArray<FTransInput> Transitions;
	{
		const TArray<TSharedPtr<FJsonValue>>* TransArr = nullptr;
		if (Body->TryGetArrayField(TEXT("transitions"), TransArr) && TransArr)
		{
			for (const auto& Val : *TransArr)
			{
				const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
				if (!Obj) continue;
				FTransInput T;
				Obj->TryGetStringField(TEXT("from"), T.From);
				Obj->TryGetStringField(TEXT("to"), T.To);
				Obj->TryGetStringField(TEXT("condition"), T.Condition);
				Transitions.Add(MoveTemp(T));
			}
		}
	}

	if (States.Num() == 0)
	{
		Callback(JsonError(400, TEXT("At least one state is required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, States, Transitions]()
	{
		// 1. Load the AnimBlueprint asset
		UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AssetPath);
		if (!AnimBP)
		{
			// Try with object name appended
			FString AltPath = AssetPath + TEXT(".") + FPaths::GetBaseFilename(AssetPath);
			AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AltPath);
		}
		if (!AnimBP)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("AnimBlueprint not found: %s"), *AssetPath)));
			return;
		}

		UE_LOG(LogNGGBridge, Log, TEXT("ConfigureAnimBlueprint: loaded %s"), *AnimBP->GetPathName());

		// 2. Find or create the state machine node in the AnimGraph
		UAnimGraphNode_StateMachine* StateMachineNode = nullptr;

		// Find the anim graph — typically named "AnimGraph"
		UEdGraph* AnimGraph = nullptr;
		for (UEdGraph* Graph : AnimBP->FunctionGraphs)
		{
			if (Graph && Graph->GetFName() == TEXT("AnimGraph"))
			{
				AnimGraph = Graph;
				break;
			}
		}
		if (!AnimGraph)
		{
			for (UEdGraph* Graph : AnimBP->UbergraphPages)
			{
				if (Graph && Graph->GetFName() == TEXT("AnimGraph"))
				{
					AnimGraph = Graph;
					break;
				}
			}
		}

		if (!AnimGraph)
		{
			Callback(JsonError(500, TEXT("Could not find AnimGraph in the AnimBlueprint")));
			return;
		}

		// Find existing state machine node or create one
		for (UEdGraphNode* Node : AnimGraph->Nodes)
		{
			StateMachineNode = Cast<UAnimGraphNode_StateMachine>(Node);
			if (StateMachineNode) break;
		}

		if (!StateMachineNode)
		{
			StateMachineNode = NewObject<UAnimGraphNode_StateMachine>(AnimGraph);
			StateMachineNode->CreateNewGuid();
			StateMachineNode->NodePosX = 200;
			StateMachineNode->NodePosY = 0;
			AnimGraph->AddNode(StateMachineNode, false, false);
			StateMachineNode->AllocateDefaultPins();
			// PostPlacedNewNode creates the EditorStateMachineGraph
			StateMachineNode->PostPlacedNewNode();
			UE_LOG(LogNGGBridge, Log, TEXT("ConfigureAnimBlueprint: created new state machine node"));
		}

		// 3. Get the state machine graph — initialize if missing
		UAnimationStateMachineGraph* SMGraph = StateMachineNode->EditorStateMachineGraph;
		if (!SMGraph)
		{
			// The node exists but its graph was never created — create it manually
			SMGraph = CastChecked<UAnimationStateMachineGraph>(
				FBlueprintEditorUtils::CreateNewGraph(
					StateMachineNode, NAME_None,
					UAnimationStateMachineGraph::StaticClass(),
					UAnimationStateMachineSchema::StaticClass()));
			StateMachineNode->EditorStateMachineGraph = SMGraph;
			SMGraph->OwnerAnimGraphNode = StateMachineNode;
			const UEdGraphSchema* SMSchema = SMGraph->GetSchema();
			if (SMSchema)
			{
				SMSchema->CreateDefaultNodesForGraph(*SMGraph);
			}
			if (AnimGraph->SubGraphs.Find(SMGraph) == INDEX_NONE)
			{
				AnimGraph->SubGraphs.Add(SMGraph);
			}
			UE_LOG(LogNGGBridge, Log, TEXT("ConfigureAnimBlueprint: manually created state machine graph"));
		}
		if (!SMGraph)
		{
			Callback(JsonError(500, TEXT("State machine node has no EditorStateMachineGraph after initialization")));
			return;
		}

		// 4. Create state nodes
		TMap<FString, UAnimStateNode*> StateNodeMap;
		int32 PosX = 300;
		int32 PosY = 0;
		int32 StatesCreated = 0;

		for (const auto& StateInput : States)
		{
			// Check if state already exists
			UAnimStateNode* ExistingState = nullptr;
			for (UEdGraphNode* Node : SMGraph->Nodes)
			{
				UAnimStateNode* SN = Cast<UAnimStateNode>(Node);
				if (SN && SN->GetStateName() == StateInput.Name)
				{
					ExistingState = SN;
					break;
				}
			}

			if (ExistingState)
			{
				StateNodeMap.Add(StateInput.Name, ExistingState);
				UE_LOG(LogNGGBridge, Log, TEXT("  State '%s' already exists, reusing"), *StateInput.Name);
				continue;
			}

			// Create new state node via schema action
			UAnimStateNode* TemplateState = NewObject<UAnimStateNode>(SMGraph);
			UAnimStateNode* NewState = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(
				SMGraph, TemplateState, FVector2f(PosX, PosY), false);

			if (!NewState)
			{
				UE_LOG(LogNGGBridge, Warning, TEXT("  Failed to create state node '%s'"), *StateInput.Name);
				continue;
			}

			NewState->Modify();
			NewState->NodeComment = StateInput.Name;

			UAnimationStateGraph* StateGraph = Cast<UAnimationStateGraph>(NewState->BoundGraph);
			if (StateGraph)
			{
				StateGraph->Rename(*StateInput.Name, nullptr, REN_DontCreateRedirectors);
			}

			// Load the animation asset and create the appropriate player node
			UObject* AnimAsset = LoadObject<UObject>(nullptr, *StateInput.AnimationPath);
			if (!AnimAsset)
			{
				FString AltAnimPath = StateInput.AnimationPath + TEXT(".") + FPaths::GetBaseFilename(StateInput.AnimationPath);
				AnimAsset = LoadObject<UObject>(nullptr, *AltAnimPath);
			}

			if (AnimAsset && StateGraph)
			{
				// Find the result node in the state graph
				UAnimGraphNode_StateResult* ResultNode = nullptr;
				for (UEdGraphNode* GNode : StateGraph->Nodes)
				{
					ResultNode = Cast<UAnimGraphNode_StateResult>(GNode);
					if (ResultNode) break;
				}

				if (ResultNode)
				{
					UBlendSpace* BS = Cast<UBlendSpace>(AnimAsset);
					UAnimSequence* Seq = Cast<UAnimSequence>(AnimAsset);

					UAnimGraphNode_Base* PlayerNode = nullptr;

					if (BS)
					{
						UAnimGraphNode_BlendSpacePlayer* BSPlayer =
							NewObject<UAnimGraphNode_BlendSpacePlayer>(StateGraph);
						BSPlayer->CreateNewGuid();
						BSPlayer->PostPlacedNewNode(); // creates default UAnimGraphNodeBinding_Base sub-object — without this the node compiles ErrorType=4 and runs as a no-op
						BSPlayer->NodePosX = ResultNode->NodePosX - 300;
						BSPlayer->NodePosY = ResultNode->NodePosY;
						StateGraph->AddNode(BSPlayer, false, false);
						BSPlayer->AllocateDefaultPins();
						BSPlayer->SetAnimationAsset(BS);
						BSPlayer->Node.SetLoop(StateInput.bLoop);
						PlayerNode = BSPlayer;
						UE_LOG(LogNGGBridge, Log, TEXT("  State '%s': BlendSpacePlayer -> %s"),
							*StateInput.Name, *BS->GetName());
					}
					else if (Seq)
					{
						UAnimGraphNode_SequencePlayer* SeqPlayer =
							NewObject<UAnimGraphNode_SequencePlayer>(StateGraph);
						SeqPlayer->CreateNewGuid();
						SeqPlayer->PostPlacedNewNode(); // creates default UAnimGraphNodeBinding_Base sub-object — without this the node compiles ErrorType=4 and runs as a no-op
						SeqPlayer->NodePosX = ResultNode->NodePosX - 300;
						SeqPlayer->NodePosY = ResultNode->NodePosY;
						StateGraph->AddNode(SeqPlayer, false, false);
						SeqPlayer->AllocateDefaultPins();
						SeqPlayer->SetAnimationAsset(Seq);
						SeqPlayer->Node.SetLoopAnimation(StateInput.bLoop);
						PlayerNode = SeqPlayer;
						UE_LOG(LogNGGBridge, Log, TEXT("  State '%s': SequencePlayer -> %s"),
							*StateInput.Name, *Seq->GetName());
					}
					else
					{
						UE_LOG(LogNGGBridge, Warning,
							TEXT("  State '%s': asset '%s' is neither AnimSequence nor BlendSpace (class: %s)"),
							*StateInput.Name, *StateInput.AnimationPath,
							*AnimAsset->GetClass()->GetName());
					}

					// Connect player output pose pin to result node input
					if (PlayerNode)
					{
						// Find the first output pin on the player node
						UEdGraphPin* OutputPin = nullptr;
						for (UEdGraphPin* Pin : PlayerNode->Pins)
						{
							if (Pin->Direction == EGPD_Output)
							{
								OutputPin = Pin;
								break;
							}
						}
						// Find the first input pin on the result node
						UEdGraphPin* InputPin = nullptr;
						for (UEdGraphPin* Pin : ResultNode->Pins)
						{
							if (Pin->Direction == EGPD_Input)
							{
								InputPin = Pin;
								break;
							}
						}
						if (OutputPin && InputPin)
						{
							const UEdGraphSchema* Schema = StateGraph->GetSchema();
							if (Schema)
							{
								Schema->TryCreateConnection(OutputPin, InputPin);
								UE_LOG(LogNGGBridge, Log, TEXT("    Connected %s -> %s"),
									*OutputPin->GetName(), *InputPin->GetName());
							}
						}
						else
						{
							UE_LOG(LogNGGBridge, Warning,
								TEXT("    Could not find output/input pins to connect player to result"));
						}
					}
				}
			}
			else if (!AnimAsset)
			{
				UE_LOG(LogNGGBridge, Warning, TEXT("  State '%s': animation not found: %s"),
					*StateInput.Name, *StateInput.AnimationPath);
			}

			StateNodeMap.Add(StateInput.Name, NewState);
			StatesCreated++;

			PosX += 250;
			PosY += 100;
		}

		// 5. Create transitions
		int32 TransitionsCreated = 0;
		for (const auto& Trans : Transitions)
		{
			UAnimStateNode** FromNodePtr = StateNodeMap.Find(Trans.From);
			UAnimStateNode** ToNodePtr = StateNodeMap.Find(Trans.To);

			if (!FromNodePtr || !*FromNodePtr)
			{
				UE_LOG(LogNGGBridge, Warning, TEXT("  Transition '%s' -> '%s': source state not found"),
					*Trans.From, *Trans.To);
				continue;
			}
			if (!ToNodePtr || !*ToNodePtr)
			{
				UE_LOG(LogNGGBridge, Warning, TEXT("  Transition '%s' -> '%s': target state not found"),
					*Trans.From, *Trans.To);
				continue;
			}

			UAnimStateTransitionNode* TemplateTransNode = NewObject<UAnimStateTransitionNode>(SMGraph);
			UAnimStateTransitionNode* TransNode = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateTransitionNode>(
				SMGraph, TemplateTransNode,
				FVector2f(
					((*FromNodePtr)->NodePosX + (*ToNodePtr)->NodePosX) / 2.0f,
					((*FromNodePtr)->NodePosY + (*ToNodePtr)->NodePosY) / 2.0f),
				false);

			if (!TransNode)
			{
				UE_LOG(LogNGGBridge, Warning, TEXT("  Failed to create transition '%s' -> '%s'"),
					*Trans.From, *Trans.To);
				continue;
			}

			UEdGraphPin* FromOutput = (*FromNodePtr)->GetOutputPin();
			UEdGraphPin* ToInput = (*ToNodePtr)->GetInputPin();
			UEdGraphPin* TransInput = TransNode->GetInputPin();
			UEdGraphPin* TransOutput = TransNode->GetOutputPin();

			if (FromOutput && TransInput)
			{
				FromOutput->MakeLinkTo(TransInput);
			}
			if (TransOutput && ToInput)
			{
				TransOutput->MakeLinkTo(ToInput);
			}

			TransNode->NodeComment = Trans.Condition;

			UE_LOG(LogNGGBridge, Log, TEXT("  Transition: %s -> %s [%s]"),
				*Trans.From, *Trans.To, *Trans.Condition);
			TransitionsCreated++;
		}

		// 6. Connect the entry node to the first state
		if (States.Num() > 0)
		{
			UAnimStateNode** FirstStatePtr = StateNodeMap.Find(States[0].Name);
			if (FirstStatePtr && *FirstStatePtr)
			{
				UAnimStateEntryNode* EntryNode = SMGraph->EntryNode;
				if (EntryNode)
				{
					UEdGraphPin* EntryOutput = EntryNode->GetOutputPin();
					UEdGraphPin* FirstInput = (*FirstStatePtr)->GetInputPin();
					if (EntryOutput && FirstInput)
					{
						EntryOutput->BreakAllPinLinks();
						EntryOutput->MakeLinkTo(FirstInput);
						UE_LOG(LogNGGBridge, Log, TEXT("  Entry -> %s wired"), *States[0].Name);
					}
				}
			}
		}

		// 7. Compile the blueprint
		FKismetEditorUtilities::CompileBlueprint(AnimBP);
		AnimBP->MarkPackageDirty();

		UE_LOG(LogNGGBridge, Log, TEXT("ConfigureAnimBlueprint: done — %d states, %d transitions"),
			StatesCreated, TransitionsCreated);

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),              true);
		Resp->SetStringField(TEXT("asset_path"),           AssetPath);
		Resp->SetNumberField(TEXT("states_created"),       StatesCreated);
		Resp->SetNumberField(TEXT("transitions_created"),  TransitionsCreated);
		Resp->SetNumberField(TEXT("total_states"),         StateNodeMap.Num());

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/anim/add_two_bone_ik
//
// Insert a Two Bone IK skeletal control node into an Animation Blueprint's
// AnimGraph, splice it between the current pose source and the Output Pose
// (UAnimGraphNode_Root), configure its bone+space properties, then compile.
//
// EffectorLocation, JointTargetLocation, and Alpha pins are left exposed for
// the caller to bind via Property Access in the editor (right-click pin -> Bind),
// since binding to pawn-owned variables requires walking a chain of K2 helper
// nodes that's better authored interactively than synthesised here.
//
// Body:
//   {
//     "anim_bp_path": "/Game/Path/ABP_X",            // required
//     "ik_bone": "hand_l",                            // required - bone the IK drives
//     "effector_location_space": "WorldSpace",        // optional - WorldSpace|ComponentSpace|ParentBoneSpace|BoneSpace
//     "joint_target_location_space": "ComponentSpace",// optional - same enum
//     "take_rotation_from_effector": true,            // optional - bTakeRotationFromEffectorSpace
//     "node_offset_x": -250,                          // optional - X offset from Output Pose node
//     "node_offset_y": 0                              // optional - Y offset from Output Pose node
//   }
// ============================================================================

namespace
{
	/** Translate a friendly enum string into EBoneControlSpace. Returns BCS_WorldSpace on miss. */
	static EBoneControlSpace ParseBoneControlSpace(const FString& Name, EBoneControlSpace Default = BCS_WorldSpace)
	{
		if (Name.Equals(TEXT("WorldSpace"),      ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("World"),           ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("BCS_WorldSpace"),  ESearchCase::IgnoreCase))      return BCS_WorldSpace;
		if (Name.Equals(TEXT("ComponentSpace"),  ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("Component"),       ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("BCS_ComponentSpace"), ESearchCase::IgnoreCase))   return BCS_ComponentSpace;
		if (Name.Equals(TEXT("ParentBoneSpace"), ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("ParentBone"),      ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("BCS_ParentBoneSpace"), ESearchCase::IgnoreCase))  return BCS_ParentBoneSpace;
		if (Name.Equals(TEXT("BoneSpace"),       ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("Bone"),            ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("BCS_BoneSpace"),   ESearchCase::IgnoreCase))      return BCS_BoneSpace;
		return Default;
	}
}

bool FNGGHttpServer::HandleAnimAddTwoBoneIK(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString AssetPath, IKBoneName;
	Body->TryGetStringField(TEXT("anim_bp_path"), AssetPath);
	Body->TryGetStringField(TEXT("ik_bone"),       IKBoneName);
	if (AssetPath.IsEmpty() || IKBoneName.IsEmpty())
	{
		Callback(JsonError(400, TEXT("'anim_bp_path' and 'ik_bone' are required")));
		return true;
	}

	FString EffectorSpaceStr   = TEXT("WorldSpace");
	FString JointTargetSpaceStr = TEXT("ComponentSpace");
	bool    bTakeRotation       = true;
	double  OffsetX             = -250.0;
	double  OffsetY             = 0.0;
	FString EffectorTargetBone, JointTargetBone;
	double  JointTargetOffsetX = 0.0, JointTargetOffsetY = 0.0, JointTargetOffsetZ = 0.0;
	Body->TryGetStringField(TEXT("effector_location_space"),    EffectorSpaceStr);
	Body->TryGetStringField(TEXT("joint_target_location_space"), JointTargetSpaceStr);
	Body->TryGetBoolField  (TEXT("take_rotation_from_effector"), bTakeRotation);
	Body->TryGetNumberField(TEXT("node_offset_x"),               OffsetX);
	Body->TryGetNumberField(TEXT("node_offset_y"),               OffsetY);
	Body->TryGetStringField(TEXT("effector_target_bone"),        EffectorTargetBone);
	Body->TryGetStringField(TEXT("joint_target_bone"),           JointTargetBone);
	{
		const TArray<TSharedPtr<FJsonValue>>* OffArr = nullptr;
		if (Body->TryGetArrayField(TEXT("joint_target_offset"), OffArr) && OffArr && OffArr->Num() >= 3)
		{
			JointTargetOffsetX = (*OffArr)[0]->AsNumber();
			JointTargetOffsetY = (*OffArr)[1]->AsNumber();
			JointTargetOffsetZ = (*OffArr)[2]->AsNumber();
		}
	}

	UE_LOG(LogNGGBridge, Log,
		TEXT("/editor/anim/add_two_bone_ik bp='%s' bone='%s' effector_space='%s' joint_space='%s' take_rot=%d eff_target='%s' jt_target='%s' jt_off=[%.1f %.1f %.1f]"),
		*AssetPath, *IKBoneName, *EffectorSpaceStr, *JointTargetSpaceStr, bTakeRotation ? 1 : 0,
		*EffectorTargetBone, *JointTargetBone, JointTargetOffsetX, JointTargetOffsetY, JointTargetOffsetZ);

	const FString CapturedAssetPath = AssetPath;
	const FString CapturedBoneName  = IKBoneName;
	const EBoneControlSpace EffectorSpace   = ParseBoneControlSpace(EffectorSpaceStr,   BCS_WorldSpace);
	const EBoneControlSpace JointTargetSpace = ParseBoneControlSpace(JointTargetSpaceStr, BCS_ComponentSpace);
	const bool   CapturedTakeRotation = bTakeRotation;
	const int32  CapturedOffsetX = (int32)OffsetX;
	const int32  CapturedOffsetY = (int32)OffsetY;
	const FString CapturedEffectorTargetBone = EffectorTargetBone;
	const FString CapturedJointTargetBone    = JointTargetBone;
	const FVector CapturedJointTargetOffset(JointTargetOffsetX, JointTargetOffsetY, JointTargetOffsetZ);

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapturedAssetPath, CapturedBoneName, EffectorSpace, JointTargetSpace,
		 CapturedTakeRotation, CapturedOffsetX, CapturedOffsetY,
		 CapturedEffectorTargetBone, CapturedJointTargetBone, CapturedJointTargetOffset]()
	{
		// 1. Load the AnimBlueprint
		UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *CapturedAssetPath);
		if (!AnimBP)
		{
			const FString AltPath = CapturedAssetPath + TEXT(".") + FPaths::GetBaseFilename(CapturedAssetPath);
			AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AltPath);
		}
		if (!AnimBP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("AnimBlueprint not found: %s"), *CapturedAssetPath)));
			return;
		}

		// 2. Find the AnimGraph
		UEdGraph* AnimGraph = nullptr;
		for (UEdGraph* Graph : AnimBP->FunctionGraphs)
		{
			if (Graph && Graph->GetFName() == TEXT("AnimGraph")) { AnimGraph = Graph; break; }
		}
		if (!AnimGraph)
		{
			for (UEdGraph* Graph : AnimBP->UbergraphPages)
			{
				if (Graph && Graph->GetFName() == TEXT("AnimGraph")) { AnimGraph = Graph; break; }
			}
		}
		if (!AnimGraph)
		{
			Callback(JsonError(500, TEXT("Could not find AnimGraph in the AnimBlueprint")));
			return;
		}

		// 3. Locate the Output Pose node (UAnimGraphNode_Root) and what's currently feeding it
		UAnimGraphNode_Root* RootNode = nullptr;
		for (UEdGraphNode* Node : AnimGraph->Nodes)
		{
			RootNode = Cast<UAnimGraphNode_Root>(Node);
			if (RootNode) break;
		}
		if (!RootNode)
		{
			Callback(JsonError(500, TEXT("Could not find Output Pose (UAnimGraphNode_Root) in AnimGraph")));
			return;
		}

		// Output Pose has exactly one input pin (Result). Find it and capture its current upstream link.
		UEdGraphPin* RootInputPin = nullptr;
		for (UEdGraphPin* Pin : RootNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input) { RootInputPin = Pin; break; }
		}
		if (!RootInputPin)
		{
			Callback(JsonError(500, TEXT("Output Pose node has no input pin")));
			return;
		}

		UEdGraphPin* UpstreamPin = nullptr;
		FString      UpstreamNodeName;
		if (RootInputPin->LinkedTo.Num() > 0)
		{
			UpstreamPin = RootInputPin->LinkedTo[0];
			if (UpstreamPin && UpstreamPin->GetOwningNode())
			{
				UpstreamNodeName = UpstreamPin->GetOwningNode()->GetName();
			}
		}

		// 4. Spawn the Two Bone IK node
		UAnimGraphNode_TwoBoneIK* IKNode = NewObject<UAnimGraphNode_TwoBoneIK>(AnimGraph);
		IKNode->CreateNewGuid();
		IKNode->PostPlacedNewNode(); // creates default UAnimGraphNodeBinding_Base sub-object — without this the node compiles ErrorType=4 and runs as a no-op
		IKNode->NodePosX = RootNode->NodePosX + CapturedOffsetX;
		IKNode->NodePosY = RootNode->NodePosY + CapturedOffsetY;
		AnimGraph->AddNode(IKNode, /*bFromUI*/ false, /*bSelectNewNode*/ false);
		IKNode->AllocateDefaultPins();

		// 5. Configure FAnimNode_TwoBoneIK runtime properties
		IKNode->Node.IKBone.BoneName              = FName(*CapturedBoneName);
		IKNode->Node.EffectorLocationSpace        = EffectorSpace;
		IKNode->Node.JointTargetLocationSpace     = JointTargetSpace;
		IKNode->Node.bTakeRotationFromEffectorSpace = CapturedTakeRotation ? 1 : 0;

		// Optional: bone-target mode for the effector — when supplied, the IK reads from a bone's
		// transform (FBoneSocketTarget) instead of using the EffectorLocation vector pin. This is
		// the tutorial's "Bone Space + Target Bone = ik_hand_*" pattern.
		if (!CapturedEffectorTargetBone.IsEmpty())
		{
			IKNode->Node.EffectorTarget.bUseSocket = false;
			IKNode->Node.EffectorTarget.BoneReference.BoneName = FName(*CapturedEffectorTargetBone);
		}
		// Same for the joint target — bone-driven elbow/knee direction.
		if (!CapturedJointTargetBone.IsEmpty())
		{
			IKNode->Node.JointTarget.bUseSocket = false;
			IKNode->Node.JointTarget.BoneReference.BoneName = FName(*CapturedJointTargetBone);
		}
		// Per-axis offset applied on top of the joint target (UE field is the standalone vector
		// JointTargetLocation; the tutorial uses Y=50 to push the elbow outward).
		if (!CapturedJointTargetOffset.IsZero())
		{
			IKNode->Node.JointTargetLocation = CapturedJointTargetOffset;
		}

		// 6. Splice the new node into the chain.
		// SkeletalControlBase exposes:
		//   - input pin "ComponentPose"  (FComponentSpacePoseLink, EGPD_Input)
		//   - output pose pin           (FComponentSpacePoseLink, EGPD_Output)
		// Plus property pins: EffectorLocation (vector), JointTargetLocation (vector), Alpha (float).
		// Identify the pose pins by name / direction / pose link struct type.
		UEdGraphPin* IKInputPosePin  = nullptr;
		UEdGraphPin* IKOutputPosePin = nullptr;
		for (UEdGraphPin* Pin : IKNode->Pins)
		{
			if (!Pin) continue;
			const bool bIsPoseStruct =
				Pin->PinType.PinSubCategoryObject == FComponentSpacePoseLink::StaticStruct() ||
				Pin->PinType.PinCategory == TEXT("struct") && Pin->PinName == TEXT("ComponentPose");
			if (Pin->Direction == EGPD_Input && (Pin->PinName == TEXT("ComponentPose") || bIsPoseStruct))
			{
				IKInputPosePin = Pin;
			}
			else if (Pin->Direction == EGPD_Output && bIsPoseStruct)
			{
				IKOutputPosePin = Pin;
			}
		}
		if (!IKInputPosePin || !IKOutputPosePin)
		{
			Callback(JsonError(500, TEXT("Could not find ComponentPose input/output pins on the Two Bone IK node")));
			return;
		}

		const UEdGraphSchema* Schema = AnimGraph->GetSchema();
		if (!Schema)
		{
			Callback(JsonError(500, TEXT("AnimGraph has no schema")));
			return;
		}

		bool bSplicedUpstream = false;
		if (UpstreamPin)
		{
			RootInputPin->BreakAllPinLinks();
			Schema->TryCreateConnection(UpstreamPin, IKInputPosePin);
			bSplicedUpstream = true;
		}
		Schema->TryCreateConnection(IKOutputPosePin, RootInputPin);

		// 7. Compile + dirty
		FKismetEditorUtilities::CompileBlueprint(AnimBP);
		AnimBP->MarkPackageDirty();

		UE_LOG(LogNGGBridge, Log,
			TEXT("/editor/anim/add_two_bone_ik: added IK node for bone '%s' (spliced_upstream=%d, upstream_was='%s')"),
			*CapturedBoneName, bSplicedUpstream ? 1 : 0, *UpstreamNodeName);

		// 8. Build response
		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("ok"),                 true);
		Resp->SetStringField(TEXT("asset_path"),         AnimBP->GetPathName());
		Resp->SetStringField(TEXT("ik_bone"),            CapturedBoneName);
		Resp->SetStringField(TEXT("ik_node_id"),         IKNode->NodeGuid.ToString());
		Resp->SetBoolField  (TEXT("spliced_upstream"),   bSplicedUpstream);
		if (!UpstreamNodeName.IsEmpty())
		{
			Resp->SetStringField(TEXT("upstream_was"),   UpstreamNodeName);
		}

		// Hint to the caller which pins still need binding in the editor.
		TArray<TSharedPtr<FJsonValue>> UnboundPins;
		UnboundPins.Add(MakeShared<FJsonValueString>(TEXT("EffectorLocation")));
		UnboundPins.Add(MakeShared<FJsonValueString>(TEXT("JointTargetLocation")));
		UnboundPins.Add(MakeShared<FJsonValueString>(TEXT("Alpha")));
		Resp->SetArrayField(TEXT("unbound_pins"), UnboundPins);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp, Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Shared helper: load AnimBP and locate its AnimGraph + Output Pose node.
// Returns nullptr-out via the OutErr if anything goes wrong, with a 404-style message.
// ============================================================================

namespace
{
	struct FAnimGraphContext
	{
		UAnimBlueprint*       AnimBP   = nullptr;
		UEdGraph*             Graph    = nullptr;
		UAnimGraphNode_Root*  RootNode = nullptr;
		UEdGraphPin*          RootInputPin = nullptr;
	};

	static bool ResolveAnimGraphContext(const FString& AssetPath, FAnimGraphContext& Out, FString& OutErr)
	{
		Out.AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AssetPath);
		if (!Out.AnimBP)
		{
			const FString AltPath = AssetPath + TEXT(".") + FPaths::GetBaseFilename(AssetPath);
			Out.AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AltPath);
		}
		if (!Out.AnimBP)
		{
			OutErr = FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath);
			return false;
		}

		for (UEdGraph* G : Out.AnimBP->FunctionGraphs)
		{
			if (G && G->GetFName() == TEXT("AnimGraph")) { Out.Graph = G; break; }
		}
		if (!Out.Graph)
		{
			for (UEdGraph* G : Out.AnimBP->UbergraphPages)
			{
				if (G && G->GetFName() == TEXT("AnimGraph")) { Out.Graph = G; break; }
			}
		}
		if (!Out.Graph)
		{
			OutErr = TEXT("Could not find AnimGraph in the AnimBlueprint");
			return false;
		}

		for (UEdGraphNode* N : Out.Graph->Nodes)
		{
			Out.RootNode = Cast<UAnimGraphNode_Root>(N);
			if (Out.RootNode) break;
		}
		if (!Out.RootNode)
		{
			OutErr = TEXT("Could not find Output Pose (UAnimGraphNode_Root) in AnimGraph");
			return false;
		}

		for (UEdGraphPin* Pin : Out.RootNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input) { Out.RootInputPin = Pin; break; }
		}
		if (!Out.RootInputPin)
		{
			OutErr = TEXT("Output Pose node has no input pin");
			return false;
		}

		return true;
	}

	/** Locate the input ComponentPose pin and the output pose pin on a UAnimGraphNode_SkeletalControlBase node. */
	static void FindPosePins(UAnimGraphNode_Base* Node, UEdGraphPin*& OutIn, UEdGraphPin*& OutOut)
	{
		OutIn = nullptr; OutOut = nullptr;
		for (UEdGraphPin* P : Node->Pins)
		{
			if (!P) continue;
			const bool bIsPoseStruct =
				P->PinType.PinSubCategoryObject == FComponentSpacePoseLink::StaticStruct() ||
				(P->PinType.PinCategory == TEXT("struct") && P->PinName == TEXT("ComponentPose"));
			if (P->Direction == EGPD_Input && (P->PinName == TEXT("ComponentPose") || bIsPoseStruct))
			{
				OutIn = P;
			}
			else if (P->Direction == EGPD_Output && bIsPoseStruct)
			{
				OutOut = P;
			}
		}
	}

	/** Insert NewNode between (whatever feeds RootInputPin) and RootInputPin. Returns true if upstream was spliced. */
	static bool SpliceBeforeOutputPose(
		UEdGraph* Graph, UEdGraphPin* RootInputPin,
		UEdGraphPin* NewInPin, UEdGraphPin* NewOutPin,
		FString& OutUpstreamName)
	{
		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema) return false;

		UEdGraphPin* UpstreamPin = nullptr;
		if (RootInputPin->LinkedTo.Num() > 0)
		{
			UpstreamPin = RootInputPin->LinkedTo[0];
			if (UpstreamPin && UpstreamPin->GetOwningNode())
			{
				OutUpstreamName = UpstreamPin->GetOwningNode()->GetName();
			}
		}

		bool bSpliced = false;
		if (UpstreamPin)
		{
			RootInputPin->BreakAllPinLinks();
			Schema->TryCreateConnection(UpstreamPin, NewInPin);
			bSpliced = true;
		}
		Schema->TryCreateConnection(NewOutPin, RootInputPin);
		return bSpliced;
	}
}

// ============================================================================
// Handler: POST /editor/anim/add_copy_bone
//
// Insert a Copy Bone skeletal control node into an AnimBP's AnimGraph,
// splice it before the Output Pose, configure source/target bone + flags.
//
// Body:
//   {
//     "anim_bp_path": "/Game/Path/ABP_X",        // required
//     "source_bone":  "ik_hand_l",                // required
//     "target_bone":  "VB_ik_hand_left_weapon_space", // required
//     "copy_translation": true,                   // optional, default true
//     "copy_rotation":    true,                   // optional, default true
//     "copy_scale":       false,                  // optional, default false
//     "control_space":    "ComponentSpace",       // optional - WorldSpace|ComponentSpace|ParentBoneSpace|BoneSpace
//     "node_offset_x":    -250,                   // optional offset from Output Pose
//     "node_offset_y":    150
//   }
// ============================================================================

bool FNGGHttpServer::HandleAnimAddCopyBone(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString AssetPath, SourceBone, TargetBone;
	Body->TryGetStringField(TEXT("anim_bp_path"), AssetPath);
	Body->TryGetStringField(TEXT("source_bone"),  SourceBone);
	Body->TryGetStringField(TEXT("target_bone"),  TargetBone);
	if (AssetPath.IsEmpty() || SourceBone.IsEmpty() || TargetBone.IsEmpty())
	{
		Callback(JsonError(400, TEXT("'anim_bp_path', 'source_bone', and 'target_bone' are required")));
		return true;
	}

	bool bCopyTranslation = true, bCopyRotation = true, bCopyScale = false;
	FString ControlSpaceStr = TEXT("ComponentSpace");
	double  OffsetX = -250.0, OffsetY = 150.0;
	Body->TryGetBoolField  (TEXT("copy_translation"), bCopyTranslation);
	Body->TryGetBoolField  (TEXT("copy_rotation"),    bCopyRotation);
	Body->TryGetBoolField  (TEXT("copy_scale"),       bCopyScale);
	Body->TryGetStringField(TEXT("control_space"),    ControlSpaceStr);
	Body->TryGetNumberField(TEXT("node_offset_x"),    OffsetX);
	Body->TryGetNumberField(TEXT("node_offset_y"),    OffsetY);

	const EBoneControlSpace ControlSpace = ParseBoneControlSpace(ControlSpaceStr, BCS_ComponentSpace);

	UE_LOG(LogNGGBridge, Log,
		TEXT("/editor/anim/add_copy_bone bp='%s' src='%s' dst='%s' loc=%d rot=%d scale=%d space='%s'"),
		*AssetPath, *SourceBone, *TargetBone,
		bCopyTranslation ? 1 : 0, bCopyRotation ? 1 : 0, bCopyScale ? 1 : 0, *ControlSpaceStr);

	const FString CapturedAssetPath = AssetPath;
	const FString CapturedSrc       = SourceBone;
	const FString CapturedDst       = TargetBone;
	const bool    CapTranslation = bCopyTranslation, CapRotation = bCopyRotation, CapScale = bCopyScale;
	const int32   CapOffsetX = (int32)OffsetX, CapOffsetY = (int32)OffsetY;

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapturedAssetPath, CapturedSrc, CapturedDst, CapTranslation, CapRotation, CapScale,
		 ControlSpace, CapOffsetX, CapOffsetY]()
	{
		FAnimGraphContext Ctx; FString Err;
		if (!ResolveAnimGraphContext(CapturedAssetPath, Ctx, Err))
		{
			Callback(JsonError(404, Err));
			return;
		}

		UAnimGraphNode_CopyBone* CopyBoneNode = NewObject<UAnimGraphNode_CopyBone>(Ctx.Graph);
		CopyBoneNode->CreateNewGuid();
		CopyBoneNode->PostPlacedNewNode(); // creates default UAnimGraphNodeBinding_Base sub-object — without this the node compiles ErrorType=4 and runs as a no-op
		CopyBoneNode->NodePosX = Ctx.RootNode->NodePosX + CapOffsetX;
		CopyBoneNode->NodePosY = Ctx.RootNode->NodePosY + CapOffsetY;
		Ctx.Graph->AddNode(CopyBoneNode, /*bFromUI*/ false, /*bSelectNewNode*/ false);
		CopyBoneNode->AllocateDefaultPins();

		CopyBoneNode->Node.SourceBone.BoneName = FName(*CapturedSrc);
		CopyBoneNode->Node.TargetBone.BoneName = FName(*CapturedDst);
		CopyBoneNode->Node.bCopyTranslation    = CapTranslation;
		CopyBoneNode->Node.bCopyRotation       = CapRotation;
		CopyBoneNode->Node.bCopyScale          = CapScale;
		CopyBoneNode->Node.ControlSpace        = ControlSpace;

		UEdGraphPin* InPin = nullptr; UEdGraphPin* OutPin = nullptr;
		FindPosePins(CopyBoneNode, InPin, OutPin);
		if (!InPin || !OutPin)
		{
			Callback(JsonError(500, TEXT("Could not find ComponentPose pins on the Copy Bone node")));
			return;
		}

		FString UpstreamName;
		const bool bSpliced = SpliceBeforeOutputPose(Ctx.Graph, Ctx.RootInputPin, InPin, OutPin, UpstreamName);

		FKismetEditorUtilities::CompileBlueprint(Ctx.AnimBP);
		Ctx.AnimBP->MarkPackageDirty();

		UE_LOG(LogNGGBridge, Log,
			TEXT("/editor/anim/add_copy_bone: %s -> %s (spliced_upstream=%d, was='%s')"),
			*CapturedSrc, *CapturedDst, bSpliced ? 1 : 0, *UpstreamName);

		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("ok"),               true);
		Resp->SetStringField(TEXT("asset_path"),       Ctx.AnimBP->GetPathName());
		Resp->SetStringField(TEXT("source_bone"),      CapturedSrc);
		Resp->SetStringField(TEXT("target_bone"),      CapturedDst);
		Resp->SetStringField(TEXT("node_id"),          CopyBoneNode->NodeGuid.ToString());
		Resp->SetBoolField  (TEXT("spliced_upstream"), bSpliced);
		if (!UpstreamName.IsEmpty()) Resp->SetStringField(TEXT("upstream_was"), UpstreamName);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp, Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/anim/add_hand_ik_retargeting
//
// Insert a Hand IK Retargeting skeletal control node before Output Pose.
// Used to retarget IK hand bones to FK hand bone positions so characters of
// different proportions can grip the same prop. Per-axis alpha defaults to (1,1,1).
//
// Body:
//   {
//     "anim_bp_path": "/Game/Path/ABP_X",       // required
//     "right_hand_fk": "hand_r",                 // required
//     "left_hand_fk":  "hand_l",                 // required
//     "right_hand_ik": "ik_hand_r",              // required
//     "left_hand_ik":  "ik_hand_l",              // required
//     "ik_bones_to_move": ["ik_hand_gun"],       // optional - array of bones the node will move
//     "hand_fk_weight": 0.5,                     // optional, default 0.5 (equal weight)
//     "per_axis_alpha": [1, 1, 1],               // optional, default [1,1,1]
//     "alpha_curve_name": "disable_hand_ik_retargeting", // optional
//     "alpha_scale": -1.0,                       // optional, default 1.0
//     "alpha_bias":  1.0,                        // optional, default 0.0
//     "node_offset_x": -250,
//     "node_offset_y": -150
//   }
// ============================================================================

bool FNGGHttpServer::HandleAnimAddHandIKRetargeting(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString AssetPath, RightFK, LeftFK, RightIK, LeftIK;
	Body->TryGetStringField(TEXT("anim_bp_path"),   AssetPath);
	Body->TryGetStringField(TEXT("right_hand_fk"),  RightFK);
	Body->TryGetStringField(TEXT("left_hand_fk"),   LeftFK);
	Body->TryGetStringField(TEXT("right_hand_ik"),  RightIK);
	Body->TryGetStringField(TEXT("left_hand_ik"),   LeftIK);
	if (AssetPath.IsEmpty() || RightFK.IsEmpty() || LeftFK.IsEmpty() || RightIK.IsEmpty() || LeftIK.IsEmpty())
	{
		Callback(JsonError(400, TEXT("anim_bp_path, right_hand_fk, left_hand_fk, right_hand_ik, left_hand_ik are required")));
		return true;
	}

	TArray<FString> IKBonesToMove;
	const TArray<TSharedPtr<FJsonValue>>* IKBonesArr = nullptr;
	if (Body->TryGetArrayField(TEXT("ik_bones_to_move"), IKBonesArr) && IKBonesArr)
	{
		for (const auto& V : *IKBonesArr) { IKBonesToMove.Add(V->AsString()); }
	}

	double HandFKWeight = 0.5;
	Body->TryGetNumberField(TEXT("hand_fk_weight"), HandFKWeight);

	double AxX = 1.0, AxY = 1.0, AxZ = 1.0;
	{
		const TArray<TSharedPtr<FJsonValue>>* AxArr = nullptr;
		if (Body->TryGetArrayField(TEXT("per_axis_alpha"), AxArr) && AxArr && AxArr->Num() >= 3)
		{
			AxX = (*AxArr)[0]->AsNumber();
			AxY = (*AxArr)[1]->AsNumber();
			AxZ = (*AxArr)[2]->AsNumber();
		}
	}

	FString AlphaCurveName;
	double  AlphaScale = 1.0, AlphaBias = 0.0;
	Body->TryGetStringField(TEXT("alpha_curve_name"), AlphaCurveName);
	Body->TryGetNumberField(TEXT("alpha_scale"),       AlphaScale);
	Body->TryGetNumberField(TEXT("alpha_bias"),        AlphaBias);

	double OffsetX = -250.0, OffsetY = -150.0;
	Body->TryGetNumberField(TEXT("node_offset_x"), OffsetX);
	Body->TryGetNumberField(TEXT("node_offset_y"), OffsetY);

	UE_LOG(LogNGGBridge, Log,
		TEXT("/editor/anim/add_hand_ik_retargeting bp='%s' R(FK='%s' IK='%s') L(FK='%s' IK='%s') ikbones=%d"),
		*AssetPath, *RightFK, *RightIK, *LeftFK, *LeftIK, IKBonesToMove.Num());

	const FString CapturedAssetPath = AssetPath;
	const FString CapRightFK = RightFK, CapLeftFK = LeftFK, CapRightIK = RightIK, CapLeftIK = LeftIK;
	const TArray<FString> CapIKBones = IKBonesToMove;
	const float CapHandFKWeight = (float)HandFKWeight;
	const FVector CapPerAxis(AxX, AxY, AxZ);
	const FString CapAlphaCurveName = AlphaCurveName;
	const float CapAlphaScale = (float)AlphaScale, CapAlphaBias = (float)AlphaBias;
	const int32 CapOffsetX = (int32)OffsetX, CapOffsetY = (int32)OffsetY;

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapturedAssetPath, CapRightFK, CapLeftFK, CapRightIK, CapLeftIK, CapIKBones,
		 CapHandFKWeight, CapPerAxis, CapAlphaCurveName, CapAlphaScale, CapAlphaBias,
		 CapOffsetX, CapOffsetY]()
	{
		FAnimGraphContext Ctx; FString Err;
		if (!ResolveAnimGraphContext(CapturedAssetPath, Ctx, Err))
		{
			Callback(JsonError(404, Err));
			return;
		}

		UAnimGraphNode_HandIKRetargeting* IKRetNode = NewObject<UAnimGraphNode_HandIKRetargeting>(Ctx.Graph);
		IKRetNode->CreateNewGuid();
		IKRetNode->PostPlacedNewNode(); // creates default UAnimGraphNodeBinding_Base sub-object — without this the node compiles ErrorType=4 and runs as a no-op
		IKRetNode->NodePosX = Ctx.RootNode->NodePosX + CapOffsetX;
		IKRetNode->NodePosY = Ctx.RootNode->NodePosY + CapOffsetY;
		Ctx.Graph->AddNode(IKRetNode, false, false);
		IKRetNode->AllocateDefaultPins();

		IKRetNode->Node.RightHandFK.BoneName = FName(*CapRightFK);
		IKRetNode->Node.LeftHandFK.BoneName  = FName(*CapLeftFK);
		IKRetNode->Node.RightHandIK.BoneName = FName(*CapRightIK);
		IKRetNode->Node.LeftHandIK.BoneName  = FName(*CapLeftIK);
		IKRetNode->Node.HandFKWeight         = CapHandFKWeight;
		IKRetNode->Node.PerAxisAlpha         = CapPerAxis;

		IKRetNode->Node.IKBonesToMove.Empty();
		for (const FString& BoneName : CapIKBones)
		{
			FBoneReference Ref;
			Ref.BoneName = FName(*BoneName);
			IKRetNode->Node.IKBonesToMove.Add(Ref);
		}

		// Alpha config: switch to Curve input if a curve name was supplied; otherwise leave as Float.
		if (!CapAlphaCurveName.IsEmpty())
		{
			IKRetNode->Node.AlphaInputType = EAnimAlphaInputType::Curve;
			IKRetNode->Node.AlphaCurveName = FName(*CapAlphaCurveName);
			IKRetNode->Node.AlphaScaleBias.Scale = CapAlphaScale;
			IKRetNode->Node.AlphaScaleBias.Bias  = CapAlphaBias;
		}

		UEdGraphPin* InPin = nullptr; UEdGraphPin* OutPin = nullptr;
		FindPosePins(IKRetNode, InPin, OutPin);
		if (!InPin || !OutPin)
		{
			Callback(JsonError(500, TEXT("Could not find ComponentPose pins on the Hand IK Retargeting node")));
			return;
		}

		FString UpstreamName;
		const bool bSpliced = SpliceBeforeOutputPose(Ctx.Graph, Ctx.RootInputPin, InPin, OutPin, UpstreamName);

		FKismetEditorUtilities::CompileBlueprint(Ctx.AnimBP);
		Ctx.AnimBP->MarkPackageDirty();

		UE_LOG(LogNGGBridge, Log,
			TEXT("/editor/anim/add_hand_ik_retargeting: spliced_upstream=%d was='%s' alpha_curve='%s'"),
			bSpliced ? 1 : 0, *UpstreamName, *CapAlphaCurveName);

		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("ok"),               true);
		Resp->SetStringField(TEXT("asset_path"),       Ctx.AnimBP->GetPathName());
		Resp->SetStringField(TEXT("node_id"),          IKRetNode->NodeGuid.ToString());
		Resp->SetBoolField  (TEXT("spliced_upstream"), bSpliced);
		if (!UpstreamName.IsEmpty()) Resp->SetStringField(TEXT("upstream_was"), UpstreamName);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp, Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/anim/add_layered_bone_blend
//
// Insert a Layered Bone Blend node into an AnimBP's AnimGraph and (optionally)
// splice the existing upstream pose source into the new node's BasePose.
// Blend Pose 0 (and any additional blend poses) are LEFT UNCONNECTED — the
// caller is expected to feed them via /bp/connect_pins or by adding a
// SequencePlayer with /editor/anim/add_sequence_player and connecting that.
//
// Body:
//   {
//     "anim_bp_path":   "/Game/Path/ABP_X",            // required
//     "graph":          "AnimGraph",                    // optional, default "AnimGraph"
//     "branch_filters": [                               // one entry per blend pose (BranchFilter mode)
//       { "bone_name": "spine_03", "blend_depth": 1 }
//     ],
//     "blend_weights":  [1.0],                          // optional, one per blend pose; default all 1.0
//     "blend_mode":     "BranchFilter",                 // optional - BranchFilter|BoneMask. Default BranchFilter.
//     "alpha":          1.0,                            // reserved (FAnimNode_LayeredBoneBlend has no plain Alpha)
//     "splice_before_output": true,                     // optional, default true
//     "node_offset_x":  -250,                           // optional offset from Output Pose
//     "node_offset_y":  -250
//   }
// ============================================================================

namespace
{
	/** Translate friendly enum string to ELayeredBoneBlendMode. Defaults to BranchFilter. */
	static ELayeredBoneBlendMode ParseLayeredBoneBlendMode(const FString& Name)
	{
		if (Name.Equals(TEXT("BlendMask"), ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("BoneMask"),  ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("Mask"),      ESearchCase::IgnoreCase))
		{
			return ELayeredBoneBlendMode::BlendMask;
		}
		return ELayeredBoneBlendMode::BranchFilter;
	}

	/** Find the BasePose input pin and the output pose pin on a LayeredBoneBlend node (FPoseLink, not FComponentSpacePoseLink). */
	static void FindLayeredBoneBlendBasePins(UAnimGraphNode_Base* Node, UEdGraphPin*& OutBaseIn, UEdGraphPin*& OutPoseOut)
	{
		OutBaseIn = nullptr; OutPoseOut = nullptr;
		for (UEdGraphPin* P : Node->Pins)
		{
			if (!P) continue;
			const bool bIsPoseStruct =
				P->PinType.PinSubCategoryObject == FPoseLink::StaticStruct() ||
				P->PinType.PinSubCategoryObject == FComponentSpacePoseLink::StaticStruct();
			if (P->Direction == EGPD_Input && (P->PinName == TEXT("BasePose") || (bIsPoseStruct && !OutBaseIn)))
			{
				// Prefer an exact "BasePose" match, otherwise the first input pose pin.
				if (P->PinName == TEXT("BasePose")) { OutBaseIn = P; }
				else if (!OutBaseIn) { OutBaseIn = P; }
			}
			else if (P->Direction == EGPD_Output && bIsPoseStruct && !OutPoseOut)
			{
				OutPoseOut = P;
			}
		}
	}
}

bool FNGGHttpServer::HandleAnimAddLayeredBoneBlend(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString AssetPath;
	Body->TryGetStringField(TEXT("anim_bp_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("'anim_bp_path' is required")));
		return true;
	}

	FString GraphName = TEXT("AnimGraph");
	Body->TryGetStringField(TEXT("graph"), GraphName);

	FString BlendModeStr = TEXT("BranchFilter");
	Body->TryGetStringField(TEXT("blend_mode"), BlendModeStr);

	bool bSpliceBeforeOutput = true;
	Body->TryGetBoolField(TEXT("splice_before_output"), bSpliceBeforeOutput);

	double Alpha = 1.0;
	Body->TryGetNumberField(TEXT("alpha"), Alpha); // reserved; not currently mapped to a node field

	double OffsetX = -250.0, OffsetY = -250.0;
	Body->TryGetNumberField(TEXT("node_offset_x"), OffsetX);
	Body->TryGetNumberField(TEXT("node_offset_y"), OffsetY);

	// Parse branch_filters: array of { bone_name, blend_depth }.
	struct FBranchFilterIn { FString Bone; int32 Depth = 1; };
	TArray<FBranchFilterIn> BranchFilters;
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Body->TryGetArrayField(TEXT("branch_filters"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (!V.IsValid() || !V->TryGetObject(Obj) || !Obj || !Obj->IsValid()) continue;
				FBranchFilterIn BF;
				(*Obj)->TryGetStringField(TEXT("bone_name"), BF.Bone);
				double D = 1.0;
				if ((*Obj)->TryGetNumberField(TEXT("blend_depth"), D)) { BF.Depth = (int32)D; }
				BranchFilters.Add(BF);
			}
		}
	}

	// Parse blend_weights (optional). If shorter than branch_filters we'll pad with 1.0.
	TArray<double> Weights;
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Body->TryGetArrayField(TEXT("blend_weights"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr) { Weights.Add(V->AsNumber()); }
		}
	}

	UE_LOG(LogNGGBridge, Log,
		TEXT("/editor/anim/add_layered_bone_blend bp='%s' graph='%s' mode='%s' filters=%d weights=%d splice=%d off=[%.0f,%.0f]"),
		*AssetPath, *GraphName, *BlendModeStr, BranchFilters.Num(), Weights.Num(),
		bSpliceBeforeOutput ? 1 : 0, OffsetX, OffsetY);

	const FString CapAssetPath = AssetPath;
	const FString CapGraphName = GraphName;
	const ELayeredBoneBlendMode CapBlendMode = ParseLayeredBoneBlendMode(BlendModeStr);
	const bool   CapSplice = bSpliceBeforeOutput;
	const int32  CapOffsetX = (int32)OffsetX, CapOffsetY = (int32)OffsetY;
	const TArray<FBranchFilterIn> CapBranchFilters = BranchFilters;
	const TArray<double> CapWeights = Weights;

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapAssetPath, CapGraphName, CapBlendMode, CapSplice,
		 CapOffsetX, CapOffsetY, CapBranchFilters, CapWeights]()
	{
		// 1. Load the AnimBlueprint
		UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *CapAssetPath);
		if (!AnimBP)
		{
			const FString AltPath = CapAssetPath + TEXT(".") + FPaths::GetBaseFilename(CapAssetPath);
			AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AltPath);
		}
		if (!AnimBP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("AnimBlueprint not found: %s"), *CapAssetPath)));
			return;
		}

		// 2. Find the requested graph
		UEdGraph* AnimGraph = nullptr;
		const FName WantedName(*CapGraphName);
		for (UEdGraph* G : AnimBP->FunctionGraphs)
		{
			if (G && G->GetFName() == WantedName) { AnimGraph = G; break; }
		}
		if (!AnimGraph)
		{
			for (UEdGraph* G : AnimBP->UbergraphPages)
			{
				if (G && G->GetFName() == WantedName) { AnimGraph = G; break; }
			}
		}
		if (!AnimGraph)
		{
			Callback(JsonError(500, FString::Printf(TEXT("Could not find graph '%s' in the AnimBlueprint"), *CapGraphName)));
			return;
		}

		// 3. Locate the Output Pose root node and its input pin (used for splicing)
		UAnimGraphNode_Root* RootNode = nullptr;
		for (UEdGraphNode* N : AnimGraph->Nodes)
		{
			RootNode = Cast<UAnimGraphNode_Root>(N);
			if (RootNode) break;
		}
		if (!RootNode)
		{
			Callback(JsonError(500, TEXT("Could not find Output Pose (UAnimGraphNode_Root) in AnimGraph")));
			return;
		}

		UEdGraphPin* RootInputPin = nullptr;
		for (UEdGraphPin* Pin : RootNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input) { RootInputPin = Pin; break; }
		}
		if (!RootInputPin)
		{
			Callback(JsonError(500, TEXT("Output Pose node has no input pin")));
			return;
		}

		// 4. Spawn the LayeredBoneBlend node
		UAnimGraphNode_LayeredBoneBlend* LBBNode = NewObject<UAnimGraphNode_LayeredBoneBlend>(AnimGraph);
		LBBNode->CreateNewGuid();
		LBBNode->PostPlacedNewNode(); // creates default UAnimGraphNodeBinding_Base sub-object — without this the node compiles ErrorType=4 and runs as a no-op
		LBBNode->NodePosX = RootNode->NodePosX + CapOffsetX;
		LBBNode->NodePosY = RootNode->NodePosY + CapOffsetY;

		// 5. Configure the runtime FAnimNode_LayeredBoneBlend BEFORE AllocateDefaultPins
		// so the per-blend-pose pins are created in the right shape.
		// The default constructor (UAnimGraphNode_LayeredBoneBlend) calls Node.AddFirstPose(),
		// so the array sizes already start at 1 (BlendWeights, BlendPoses, LayerSetup).
		LBBNode->Node.BlendMode = CapBlendMode;

		const int32 RequestedPoses = FMath::Max(1, CapBranchFilters.Num());
		// Grow arrays to RequestedPoses (constructor seeded 1)
		while (LBBNode->Node.BlendPoses.Num() < RequestedPoses)
		{
			LBBNode->Node.AddPose();
		}
		// Defensive: if branch_filters had MORE entries than RequestedPoses (shouldn't happen)
		// or fewer, ensure the layer setup container is the right size.
		LBBNode->Node.BlendWeights.SetNum(RequestedPoses);
		if (CapBlendMode == ELayeredBoneBlendMode::BranchFilter)
		{
			LBBNode->Node.LayerSetup.SetNum(RequestedPoses);
		}

		// Apply branch filters (one BranchFilter per blend pose entry, matching the tutorial pattern).
		if (CapBlendMode == ELayeredBoneBlendMode::BranchFilter)
		{
			for (int32 i = 0; i < CapBranchFilters.Num() && i < LBBNode->Node.LayerSetup.Num(); ++i)
			{
				FInputBlendPose& IBP = LBBNode->Node.LayerSetup[i];
				IBP.BranchFilters.Reset();
				FBranchFilter BF;
				BF.BoneName   = FName(*CapBranchFilters[i].Bone);
				BF.BlendDepth = CapBranchFilters[i].Depth;
				IBP.BranchFilters.Add(BF);
			}
		}

		// Apply blend weights, padding with 1.0 if the array was shorter than RequestedPoses.
		for (int32 i = 0; i < LBBNode->Node.BlendWeights.Num(); ++i)
		{
			LBBNode->Node.BlendWeights[i] = (i < CapWeights.Num()) ? (float)CapWeights[i] : 1.0f;
		}

		AnimGraph->AddNode(LBBNode, /*bFromUI*/ false, /*bSelectNewNode*/ false);
		LBBNode->AllocateDefaultPins();

		// 6. Splice: locate BasePose input + output pose pin
		UEdGraphPin* BaseInPin = nullptr;
		UEdGraphPin* OutPosePin = nullptr;
		FindLayeredBoneBlendBasePins(LBBNode, BaseInPin, OutPosePin);
		if (!BaseInPin || !OutPosePin)
		{
			Callback(JsonError(500, TEXT("Could not find BasePose / output pose pins on the Layered Bone Blend node")));
			return;
		}

		const UEdGraphSchema* Schema = AnimGraph->GetSchema();
		if (!Schema)
		{
			Callback(JsonError(500, TEXT("AnimGraph has no schema")));
			return;
		}

		bool bSplicedUpstream = false;
		FString UpstreamNodeName;
		UEdGraphPin* UpstreamPin = nullptr;
		if (RootInputPin->LinkedTo.Num() > 0)
		{
			UpstreamPin = RootInputPin->LinkedTo[0];
			if (UpstreamPin && UpstreamPin->GetOwningNode())
			{
				UpstreamNodeName = UpstreamPin->GetOwningNode()->GetName();
			}
		}

		if (CapSplice)
		{
			if (UpstreamPin)
			{
				RootInputPin->BreakAllPinLinks();
				Schema->TryCreateConnection(UpstreamPin, BaseInPin);
				bSplicedUpstream = true;
			}
			Schema->TryCreateConnection(OutPosePin, RootInputPin);
		}

		// 7. Compile + dirty
		FKismetEditorUtilities::CompileBlueprint(AnimBP);
		AnimBP->MarkPackageDirty();

		UE_LOG(LogNGGBridge, Log,
			TEXT("/editor/anim/add_layered_bone_blend: added LBB (poses=%d) spliced=%d upstream_was='%s'"),
			LBBNode->Node.BlendPoses.Num(), bSplicedUpstream ? 1 : 0, *UpstreamNodeName);

		// 8. Build response
		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("ok"),               true);
		Resp->SetStringField(TEXT("asset_path"),       AnimBP->GetPathName());
		Resp->SetStringField(TEXT("node_id"),          LBBNode->NodeGuid.ToString());
		Resp->SetNumberField(TEXT("blend_pose_count"), LBBNode->Node.BlendPoses.Num());
		Resp->SetBoolField  (TEXT("spliced_upstream"), bSplicedUpstream);
		if (!UpstreamNodeName.IsEmpty()) Resp->SetStringField(TEXT("upstream_was"), UpstreamNodeName);

		// Tell the caller which pose pins still need wiring on this node.
		TArray<TSharedPtr<FJsonValue>> UnboundPins;
		for (int32 i = 0; i < LBBNode->Node.BlendPoses.Num(); ++i)
		{
			UnboundPins.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("BlendPose_%d"), i)));
		}
		Resp->SetArrayField(TEXT("unbound_pins"), UnboundPins);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp, Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/anim/add_sequence_player
//
// Spawn a UAnimGraphNode_SequencePlayer in an AnimBP's AnimGraph, configured
// to play a specific UAnimSequence. By default this does NOT splice into the
// pose chain — it's a leaf the caller wires up later via /bp/connect_pins
// (typically into a LayeredBoneBlend's BlendPose input).
//
// Body:
//   {
//     "anim_bp_path":   "/Game/Path/ABP_X",                    // required
//     "graph":          "AnimGraph",                            // optional, default "AnimGraph"
//     "sequence":       "/Game/Anims/MM_Idle.MM_Idle",          // required - hard ref to UAnimSequence
//     "loop":           true,                                   // optional, default true
//     "play_rate":      1.0,                                    // optional, default 1.0
//     "start_position": 0.0,                                    // optional, default 0.0
//     "node_offset_x":  -500,                                   // optional offset from Output Pose
//     "node_offset_y":  -250
//   }
// ============================================================================

bool FNGGHttpServer::HandleAnimAddSequencePlayer(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString AssetPath, SequencePath;
	Body->TryGetStringField(TEXT("anim_bp_path"), AssetPath);
	Body->TryGetStringField(TEXT("sequence"),     SequencePath);
	if (AssetPath.IsEmpty() || SequencePath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("'anim_bp_path' and 'sequence' are required")));
		return true;
	}

	FString GraphName = TEXT("AnimGraph");
	Body->TryGetStringField(TEXT("graph"), GraphName);

	bool   bLoop         = true;
	double PlayRate      = 1.0;
	double StartPosition = 0.0;
	double OffsetX       = -500.0;
	double OffsetY       = -250.0;
	Body->TryGetBoolField  (TEXT("loop"),           bLoop);
	Body->TryGetNumberField(TEXT("play_rate"),      PlayRate);
	Body->TryGetNumberField(TEXT("start_position"), StartPosition);
	Body->TryGetNumberField(TEXT("node_offset_x"),  OffsetX);
	Body->TryGetNumberField(TEXT("node_offset_y"),  OffsetY);

	UE_LOG(LogNGGBridge, Log,
		TEXT("/editor/anim/add_sequence_player bp='%s' graph='%s' seq='%s' loop=%d rate=%.2f start=%.2f off=[%.0f,%.0f]"),
		*AssetPath, *GraphName, *SequencePath, bLoop ? 1 : 0, PlayRate, StartPosition, OffsetX, OffsetY);

	const FString CapAssetPath  = AssetPath;
	const FString CapGraphName  = GraphName;
	const FString CapSequence   = SequencePath;
	const bool    CapLoop       = bLoop;
	const float   CapPlayRate   = (float)PlayRate;
	const float   CapStartPos   = (float)StartPosition;
	const int32   CapOffsetX    = (int32)OffsetX;
	const int32   CapOffsetY    = (int32)OffsetY;

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapAssetPath, CapGraphName, CapSequence, CapLoop, CapPlayRate, CapStartPos,
		 CapOffsetX, CapOffsetY]()
	{
		// 1. Load the AnimBlueprint
		UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *CapAssetPath);
		if (!AnimBP)
		{
			const FString AltPath = CapAssetPath + TEXT(".") + FPaths::GetBaseFilename(CapAssetPath);
			AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AltPath);
		}
		if (!AnimBP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("AnimBlueprint not found: %s"), *CapAssetPath)));
			return;
		}

		// 2. Resolve the AnimSequence asset
		UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *CapSequence);
		if (!Sequence)
		{
			const FString AltPath = CapSequence + TEXT(".") + FPaths::GetBaseFilename(CapSequence);
			Sequence = LoadObject<UAnimSequence>(nullptr, *AltPath);
		}
		if (!Sequence)
		{
			Callback(JsonError(404, FString::Printf(TEXT("AnimSequence not found: %s"), *CapSequence)));
			return;
		}

		// 3. Find the requested graph
		UEdGraph* AnimGraph = nullptr;
		const FName WantedName(*CapGraphName);
		for (UEdGraph* G : AnimBP->FunctionGraphs)
		{
			if (G && G->GetFName() == WantedName) { AnimGraph = G; break; }
		}
		if (!AnimGraph)
		{
			for (UEdGraph* G : AnimBP->UbergraphPages)
			{
				if (G && G->GetFName() == WantedName) { AnimGraph = G; break; }
			}
		}
		if (!AnimGraph)
		{
			Callback(JsonError(500, FString::Printf(TEXT("Could not find graph '%s' in the AnimBlueprint"), *CapGraphName)));
			return;
		}

		// 4. Locate the Output Pose node so we can position the new node relative to it
		UAnimGraphNode_Root* RootNode = nullptr;
		for (UEdGraphNode* N : AnimGraph->Nodes)
		{
			RootNode = Cast<UAnimGraphNode_Root>(N);
			if (RootNode) break;
		}
		const int32 BaseX = RootNode ? RootNode->NodePosX : 0;
		const int32 BaseY = RootNode ? RootNode->NodePosY : 0;

		// 5. Spawn the SequencePlayer node and configure its runtime FAnimNode_SequencePlayer.
		// Setters live on the base FAnimNode_SequencePlayerBase and forward to the right backing field.
		UAnimGraphNode_SequencePlayer* SPNode = NewObject<UAnimGraphNode_SequencePlayer>(AnimGraph);
		SPNode->CreateNewGuid();
		SPNode->PostPlacedNewNode(); // creates default UAnimGraphNodeBinding_Base sub-object — without this the node compiles ErrorType=4 and runs as a no-op
		SPNode->NodePosX = BaseX + CapOffsetX;
		SPNode->NodePosY = BaseY + CapOffsetY;
		AnimGraph->AddNode(SPNode, /*bFromUI*/ false, /*bSelectNewNode*/ false);
		SPNode->AllocateDefaultPins();

		SPNode->Node.SetSequence     (Sequence);
		SPNode->Node.SetLoopAnimation(CapLoop);
		SPNode->Node.SetPlayRate     (CapPlayRate);
		SPNode->Node.SetStartPosition(CapStartPos);

		// 6. Compile + dirty
		FKismetEditorUtilities::CompileBlueprint(AnimBP);
		AnimBP->MarkPackageDirty();

		UE_LOG(LogNGGBridge, Log,
			TEXT("/editor/anim/add_sequence_player: spawned SequencePlayer for '%s' (loop=%d rate=%.2f start=%.2f)"),
			*Sequence->GetPathName(), CapLoop ? 1 : 0, CapPlayRate, CapStartPos);

		// 7. Build response
		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("ok"),         true);
		Resp->SetStringField(TEXT("asset_path"), AnimBP->GetPathName());
		Resp->SetStringField(TEXT("node_id"),    SPNode->NodeGuid.ToString());
		Resp->SetStringField(TEXT("sequence"),   Sequence->GetPathName());
		Resp->SetBoolField  (TEXT("loop"),       CapLoop);
		Resp->SetNumberField(TEXT("play_rate"),  CapPlayRate);
		Resp->SetNumberField(TEXT("start_position"), CapStartPos);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp, Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/anim/add_modify_bone
//
// Insert a Transform (Modify) Bone (UAnimGraphNode_ModifyBone) into an AnimBP's
// AnimGraph. Optionally splice it before the Output Pose so it modifies whatever
// pose chain is currently feeding the root.
//
// Use case for ThrowBomb: rotate spine_03 in component space (RotationMode=Add)
// so the upper body follows the player's aim direction — the whole arm + the
// rifle pose rotate together, which makes the launcher naturally point at the
// crosshair target. The Rotation pin is left unwired so the AnimBP can drive it
// from a computed FRotator each frame (e.g. via BlueprintThreadSafeUpdateAnimation).
//
// Body:
//   {
//     "anim_bp_path":      "/Game/Path/ABP_X",        // required
//     "graph":             "AnimGraph",                // optional, default "AnimGraph"
//     "bone_name":         "spine_03",                 // required - bone to modify
//     "translation_mode":  "Ignore",                   // Ignore|Add|Replace, default Ignore
//     "rotation_mode":     "Add",                      // Ignore|Add|Replace, default Add
//     "scale_mode":        "Ignore",                   // default Ignore
//     "translation_space": "ComponentSpace",           // WorldSpace|ComponentSpace|ParentBoneSpace|BoneSpace, default ComponentSpace
//     "rotation_space":    "ComponentSpace",           // default ComponentSpace
//     "scale_space":       "ComponentSpace",           // default ComponentSpace
//     "translation":       [0, 0, 0],                  // default value baked into the node (overridden if pin is wired)
//     "rotation":          [0, 0, 0],                  // pitch, yaw, roll
//     "scale":             [1, 1, 1],
//     "alpha":             1.0,
//     "splice_before_output": false,                   // default false (caller wires)
//     "node_offset_x":     -250,
//     "node_offset_y":     0
//   }
//
// Response:
//   { "ok": true, "asset_path": "...", "node_id": "<guid>",
//     "bone": "spine_03", "spliced_upstream": bool, "upstream_was": "...",
//     "unbound_pins": ["Translation", "Rotation", "Scale", "Alpha"] }
// ============================================================================

namespace
{
	/** Translate "Ignore"/"Add"/"Replace" (or BMM_*) into EBoneModificationMode. */
	static EBoneModificationMode ParseBoneModificationMode(const FString& Name, EBoneModificationMode Default = BMM_Ignore)
	{
		if (Name.Equals(TEXT("Ignore"),     ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("BMM_Ignore"), ESearchCase::IgnoreCase))   return BMM_Ignore;
		if (Name.Equals(TEXT("Replace"),         ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("Replace Existing"), ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("BMM_Replace"),     ESearchCase::IgnoreCase)) return BMM_Replace;
		if (Name.Equals(TEXT("Add"),               ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("Additive"),          ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("Add to Existing"),   ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("BMM_Additive"),      ESearchCase::IgnoreCase)) return BMM_Additive;
		return Default;
	}

	/** Parse [x,y,z] FVector from a JSON array field; returns Default on miss. */
	static FVector ParseJsonVec3(const TSharedPtr<FJsonObject>& Body, const TCHAR* Field, const FVector& Default)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Body->TryGetArrayField(Field, Arr) || !Arr || Arr->Num() < 3) return Default;
		return FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
	}

	/** Parse [pitch,yaw,roll] FRotator from a JSON array field; returns Default on miss. */
	static FRotator ParseJsonRot3(const TSharedPtr<FJsonObject>& Body, const TCHAR* Field, const FRotator& Default)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Body->TryGetArrayField(Field, Arr) || !Arr || Arr->Num() < 3) return Default;
		return FRotator((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
	}
}

bool FNGGHttpServer::HandleAnimAddModifyBone(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString AssetPath, BoneName;
	Body->TryGetStringField(TEXT("anim_bp_path"), AssetPath);
	Body->TryGetStringField(TEXT("bone_name"),    BoneName);
	if (AssetPath.IsEmpty() || BoneName.IsEmpty())
	{
		Callback(JsonError(400, TEXT("'anim_bp_path' and 'bone_name' are required")));
		return true;
	}

	FString GraphName = TEXT("AnimGraph");
	Body->TryGetStringField(TEXT("graph"), GraphName);

	FString TranslationModeStr = TEXT("Ignore");
	FString RotationModeStr    = TEXT("Add");
	FString ScaleModeStr       = TEXT("Ignore");
	Body->TryGetStringField(TEXT("translation_mode"), TranslationModeStr);
	Body->TryGetStringField(TEXT("rotation_mode"),    RotationModeStr);
	Body->TryGetStringField(TEXT("scale_mode"),       ScaleModeStr);

	FString TranslationSpaceStr = TEXT("ComponentSpace");
	FString RotationSpaceStr    = TEXT("ComponentSpace");
	FString ScaleSpaceStr       = TEXT("ComponentSpace");
	Body->TryGetStringField(TEXT("translation_space"), TranslationSpaceStr);
	Body->TryGetStringField(TEXT("rotation_space"),    RotationSpaceStr);
	Body->TryGetStringField(TEXT("scale_space"),       ScaleSpaceStr);

	const FVector  Translation = ParseJsonVec3(Body, TEXT("translation"), FVector::ZeroVector);
	const FRotator Rotation    = ParseJsonRot3(Body, TEXT("rotation"),    FRotator::ZeroRotator);
	const FVector  Scale       = ParseJsonVec3(Body, TEXT("scale"),       FVector::OneVector);

	double AlphaIn = 1.0;
	Body->TryGetNumberField(TEXT("alpha"), AlphaIn);

	bool bSplice = false;
	Body->TryGetBoolField(TEXT("splice_before_output"), bSplice);

	double OffsetX = -250.0, OffsetY = 0.0;
	Body->TryGetNumberField(TEXT("node_offset_x"), OffsetX);
	Body->TryGetNumberField(TEXT("node_offset_y"), OffsetY);

	const EBoneModificationMode TranslationMode = ParseBoneModificationMode(TranslationModeStr, BMM_Ignore);
	const EBoneModificationMode RotationMode    = ParseBoneModificationMode(RotationModeStr,    BMM_Additive);
	const EBoneModificationMode ScaleMode       = ParseBoneModificationMode(ScaleModeStr,       BMM_Ignore);

	const EBoneControlSpace TranslationSpace = ParseBoneControlSpace(TranslationSpaceStr, BCS_ComponentSpace);
	const EBoneControlSpace RotationSpace    = ParseBoneControlSpace(RotationSpaceStr,    BCS_ComponentSpace);
	const EBoneControlSpace ScaleSpace       = ParseBoneControlSpace(ScaleSpaceStr,       BCS_ComponentSpace);

	UE_LOG(LogNGGBridge, Log,
		TEXT("/editor/anim/add_modify_bone bp='%s' graph='%s' bone='%s' tmode='%s' rmode='%s' smode='%s' "
			 "tspace='%s' rspace='%s' sspace='%s' splice=%d off=[%.0f,%.0f]"),
		*AssetPath, *GraphName, *BoneName,
		*TranslationModeStr, *RotationModeStr, *ScaleModeStr,
		*TranslationSpaceStr, *RotationSpaceStr, *ScaleSpaceStr,
		bSplice ? 1 : 0, OffsetX, OffsetY);

	const FString CapAssetPath  = AssetPath;
	const FString CapGraphName  = GraphName;
	const FString CapBoneName   = BoneName;
	const FVector  CapTranslation = Translation;
	const FRotator CapRotation    = Rotation;
	const FVector  CapScale       = Scale;
	const float   CapAlpha      = (float)AlphaIn;
	const bool    CapSplice     = bSplice;
	const int32   CapOffsetX    = (int32)OffsetX;
	const int32   CapOffsetY    = (int32)OffsetY;
	// Modes / spaces are POD enums — capture by value.

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapAssetPath, CapGraphName, CapBoneName,
		 CapTranslation, CapRotation, CapScale, CapAlpha, CapSplice,
		 CapOffsetX, CapOffsetY,
		 TranslationMode, RotationMode, ScaleMode,
		 TranslationSpace, RotationSpace, ScaleSpace]()
	{
		// 1. Load the AnimBlueprint
		UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *CapAssetPath);
		if (!AnimBP)
		{
			const FString AltPath = CapAssetPath + TEXT(".") + FPaths::GetBaseFilename(CapAssetPath);
			AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AltPath);
		}
		if (!AnimBP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("AnimBlueprint not found: %s"), *CapAssetPath)));
			return;
		}

		// 2. Find the requested graph
		UEdGraph* AnimGraph = nullptr;
		const FName WantedName(*CapGraphName);
		for (UEdGraph* G : AnimBP->FunctionGraphs)
		{
			if (G && G->GetFName() == WantedName) { AnimGraph = G; break; }
		}
		if (!AnimGraph)
		{
			for (UEdGraph* G : AnimBP->UbergraphPages)
			{
				if (G && G->GetFName() == WantedName) { AnimGraph = G; break; }
			}
		}
		if (!AnimGraph)
		{
			Callback(JsonError(500, FString::Printf(TEXT("Could not find graph '%s' in the AnimBlueprint"), *CapGraphName)));
			return;
		}

		// 3. Locate the Output Pose root node (used for positioning + optional splice)
		UAnimGraphNode_Root* RootNode = nullptr;
		for (UEdGraphNode* N : AnimGraph->Nodes)
		{
			RootNode = Cast<UAnimGraphNode_Root>(N);
			if (RootNode) break;
		}
		if (!RootNode)
		{
			Callback(JsonError(500, TEXT("Could not find Output Pose (UAnimGraphNode_Root) in AnimGraph")));
			return;
		}

		UEdGraphPin* RootInputPin = nullptr;
		for (UEdGraphPin* Pin : RootNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input) { RootInputPin = Pin; break; }
		}
		if (!RootInputPin)
		{
			Callback(JsonError(500, TEXT("Output Pose node has no input pin")));
			return;
		}

		// 4. Spawn the ModifyBone node, configure runtime fields, then allocate pins.
		// Setting Node fields BEFORE AllocateDefaultPins is the safe pattern (matches
		// how LayeredBoneBlend / TwoBoneIK seed their per-blend / per-bone arrays).
		UAnimGraphNode_ModifyBone* MBNode = NewObject<UAnimGraphNode_ModifyBone>(AnimGraph);
		MBNode->CreateNewGuid();
		MBNode->PostPlacedNewNode(); // creates default UAnimGraphNodeBinding_Base sub-object — without this the node compiles ErrorType=4 and runs as a no-op
		MBNode->NodePosX = RootNode->NodePosX + CapOffsetX;
		MBNode->NodePosY = RootNode->NodePosY + CapOffsetY;

		MBNode->Node.BoneToModify.BoneName = FName(*CapBoneName);
		MBNode->Node.TranslationMode = TranslationMode;
		MBNode->Node.RotationMode    = RotationMode;
		MBNode->Node.ScaleMode       = ScaleMode;
		MBNode->Node.TranslationSpace = TranslationSpace;
		MBNode->Node.RotationSpace    = RotationSpace;
		MBNode->Node.ScaleSpace       = ScaleSpace;
		MBNode->Node.Translation = CapTranslation;
		MBNode->Node.Rotation    = CapRotation;
		MBNode->Node.Scale       = CapScale;
		// Alpha lives on the SkeletalControlBase; default of 1.0 is fine for a wired-pin workflow.
		MBNode->Node.Alpha       = CapAlpha;

		AnimGraph->AddNode(MBNode, /*bFromUI*/ false, /*bSelectNewNode*/ false);
		MBNode->AllocateDefaultPins();

		// 5. Optional splice: insert MBNode between (whatever feeds RootInputPin) and RootInputPin.
		bool bSplicedUpstream = false;
		FString UpstreamNodeName;
		{
			// FindPosePins handles SkeletalControlBase-style ComponentPose pins.
			UEdGraphPin* InPin = nullptr; UEdGraphPin* OutPin = nullptr;
			FindPosePins(MBNode, InPin, OutPin);
			if (CapSplice)
			{
				if (!InPin || !OutPin)
				{
					Callback(JsonError(500, TEXT("Could not find ComponentPose pins on the Modify Bone node — cannot splice")));
					return;
				}
				bSplicedUpstream = SpliceBeforeOutputPose(AnimGraph, RootInputPin, InPin, OutPin, UpstreamNodeName);
			}
		}

		// 6. Compile + dirty
		FKismetEditorUtilities::CompileBlueprint(AnimBP);
		AnimBP->MarkPackageDirty();

		// 7. Walk the spawned node's pins so we report the *actual* runtime pin names
		// the caller will need for /bp/connect_pins. ModifyBone declares Translation,
		// Rotation, and Scale as PinShownByDefault, plus the SkeletalControlBase Alpha
		// system. (We hit a name mismatch with LayeredBoneBlend last time — better to
		// surface real names than guess.) We list every input pin that is NOT a pose
		// link and not already connected.
		TArray<TSharedPtr<FJsonValue>> UnboundPins;
		for (UEdGraphPin* P : MBNode->Pins)
		{
			if (!P || P->Direction != EGPD_Input) continue;
			const bool bIsPose =
				P->PinType.PinSubCategoryObject == FComponentSpacePoseLink::StaticStruct() ||
				P->PinType.PinSubCategoryObject == FPoseLink::StaticStruct() ||
				P->PinName == TEXT("ComponentPose") || P->PinName == TEXT("Pose");
			if (bIsPose) continue;
			if (P->LinkedTo.Num() > 0) continue; // skip already-connected pins
			UnboundPins.Add(MakeShared<FJsonValueString>(P->PinName.ToString()));
		}

		UE_LOG(LogNGGBridge, Log,
			TEXT("/editor/anim/add_modify_bone: added MB on bone='%s' (spliced=%d upstream='%s' unbound=%d)"),
			*CapBoneName, bSplicedUpstream ? 1 : 0, *UpstreamNodeName, UnboundPins.Num());

		// 8. Build response
		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("ok"),               true);
		Resp->SetStringField(TEXT("asset_path"),       AnimBP->GetPathName());
		Resp->SetStringField(TEXT("node_id"),          MBNode->NodeGuid.ToString());
		Resp->SetStringField(TEXT("bone"),             CapBoneName);
		Resp->SetBoolField  (TEXT("spliced_upstream"), bSplicedUpstream);
		if (!UpstreamNodeName.IsEmpty()) Resp->SetStringField(TEXT("upstream_was"), UpstreamNodeName);
		Resp->SetArrayField (TEXT("unbound_pins"),     UnboundPins);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp, Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/anim/add_look_at
//
// Insert a LookAt skeletal control node (UAnimGraphNode_LookAt /
// FAnimNode_LookAt) into an AnimBP's AnimGraph and (optionally) splice it
// before the Output Pose. Unlike a manual ModifyBone+Rotation approach, LookAt
// solves "given a bone, a local axis, and a world target, compute the rotation
// that aligns that axis through the target" — which correctly accounts for the
// spatial offset between the controlled bone (e.g. spine_03) and the gun.
//
// The LookAtLocation pin is left UNCONNECTED so the AnimBP can drive it from
// BlueprintThreadSafeUpdateAnimation each frame (read GetAimWorldTarget() into
// an AnimBP variable, then bp_connect_pins it to LookAtLocation).
//
// Body:
//   {
//     "anim_bp_path":           "/Game/Path/ABP_X",   // required
//     "graph":                  "AnimGraph",          // optional, default "AnimGraph"
//     "bone_to_modify":         "spine_03",           // required
//     "look_at_axis":           [1,0,0],              // local axis on bone_to_modify; default [1,0,0]
//     "look_at_axis_local":     true,                 // optional, default true
//     "look_at_target_bone":    "",                   // optional - if set, LookAt follows that bone
//     "look_at_socket":         "",                   // optional - socket on look_at_target_bone (mutually exclusive with bone)
//     "look_at_location":       [0,0,0],              // world-space target (when no target bone). Bind via bp_connect_pins.
//     "look_at_location_space": "WorldSpace",         // World|Component|ParentBone|Bone, default WorldSpace
//     "look_at_clamp":          0.0,                  // 0 = no clamp; >0 = max angle in degrees
//     "interpolation_type":     "Linear",             // None|Linear|Cubic|Sinusoidal|EaseInOutExponent2..5
//     "interpolation_time":     0.1,                  // smoothing time (seconds)
//     "alpha":                  1.0,
//     "splice_before_output":   true,                 // default true
//     "node_offset_x":          -250,
//     "node_offset_y":          0
//   }
//
// Response:
//   { "ok": true, "asset_path": "...", "node_id": "<guid>",
//     "bone": "spine_03", "spliced_upstream": bool, "upstream_was": "...",
//     "unbound_pins": [...] }   // actual runtime input pin names
// ============================================================================

namespace
{
	/** Translate "Linear"/"Cubic"/"Sinusoidal"/"EaseInOutExponent[2-5]"/"None" into EInterpolationBlend::Type. */
	static EInterpolationBlend::Type ParseInterpolationBlend(const FString& Name, EInterpolationBlend::Type Default = EInterpolationBlend::Linear)
	{
		// "None" maps to Linear with 0 interpolation_time at the call site; here we treat it as Linear.
		if (Name.Equals(TEXT("None"),     ESearchCase::IgnoreCase))           return EInterpolationBlend::Linear;
		if (Name.Equals(TEXT("Linear"),   ESearchCase::IgnoreCase))           return EInterpolationBlend::Linear;
		if (Name.Equals(TEXT("Cubic"),    ESearchCase::IgnoreCase))           return EInterpolationBlend::Cubic;
		if (Name.Equals(TEXT("Sinusoidal"), ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("Sine"),     ESearchCase::IgnoreCase))           return EInterpolationBlend::Sinusoidal;
		if (Name.Equals(TEXT("EaseInOut"),         ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("EaseInOutExponent2"), ESearchCase::IgnoreCase)) return EInterpolationBlend::EaseInOutExponent2;
		if (Name.Equals(TEXT("EaseInOutExponent3"), ESearchCase::IgnoreCase)) return EInterpolationBlend::EaseInOutExponent3;
		if (Name.Equals(TEXT("EaseInOutExponent4"), ESearchCase::IgnoreCase)) return EInterpolationBlend::EaseInOutExponent4;
		if (Name.Equals(TEXT("EaseInOutExponent5"), ESearchCase::IgnoreCase)) return EInterpolationBlend::EaseInOutExponent5;
		return Default;
	}
}

bool FNGGHttpServer::HandleAnimAddLookAt(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString AssetPath, BoneToModify;
	Body->TryGetStringField(TEXT("anim_bp_path"),    AssetPath);
	Body->TryGetStringField(TEXT("bone_to_modify"),  BoneToModify);
	if (AssetPath.IsEmpty() || BoneToModify.IsEmpty())
	{
		Callback(JsonError(400, TEXT("'anim_bp_path' and 'bone_to_modify' are required")));
		return true;
	}

	FString GraphName = TEXT("AnimGraph");
	Body->TryGetStringField(TEXT("graph"), GraphName);

	// Look-at axis on the controlled bone. Default to [1,0,0]; UE5 mannequin spine
	// forward axis is typically [0,1,0] (Y) — caller should pass that explicitly when known.
	const FVector LookAtAxisVec = ParseJsonVec3(Body, TEXT("look_at_axis"), FVector(1.0, 0.0, 0.0));
	bool bLookAtAxisLocal = true;
	Body->TryGetBoolField(TEXT("look_at_axis_local"), bLookAtAxisLocal);

	// Optional target — bone name OR socket name. Mutually exclusive (socket wins if both set).
	FString LookAtTargetBone, LookAtSocket;
	Body->TryGetStringField(TEXT("look_at_target_bone"), LookAtTargetBone);
	Body->TryGetStringField(TEXT("look_at_socket"),      LookAtSocket);

	// World-space (or other-space) target offset. Most callers will leave this at zero
	// and bind the LookAtLocation pin via /bp/connect_pins to a Get of an AnimBP variable.
	const FVector LookAtLocation = ParseJsonVec3(Body, TEXT("look_at_location"), FVector::ZeroVector);
	FString LookAtLocationSpaceStr = TEXT("WorldSpace");
	Body->TryGetStringField(TEXT("look_at_location_space"), LookAtLocationSpaceStr);

	double LookAtClampIn       = 0.0;
	double InterpolationTimeIn = 0.1;
	double AlphaIn             = 1.0;
	Body->TryGetNumberField(TEXT("look_at_clamp"),      LookAtClampIn);
	Body->TryGetNumberField(TEXT("interpolation_time"), InterpolationTimeIn);
	Body->TryGetNumberField(TEXT("alpha"),              AlphaIn);

	FString InterpolationTypeStr = TEXT("Linear");
	Body->TryGetStringField(TEXT("interpolation_type"), InterpolationTypeStr);

	bool bSplice = true;
	Body->TryGetBoolField(TEXT("splice_before_output"), bSplice);

	double OffsetX = -250.0, OffsetY = 0.0;
	Body->TryGetNumberField(TEXT("node_offset_x"), OffsetX);
	Body->TryGetNumberField(TEXT("node_offset_y"), OffsetY);

	const EInterpolationBlend::Type InterpolationType = ParseInterpolationBlend(InterpolationTypeStr, EInterpolationBlend::Linear);
	// LookAtLocation space — the AnimNode itself doesn't have a space enum (LookAtLocation
	// is plain world-space when no target bone is set, otherwise local to that bone). We
	// accept a string for caller-side clarity / future expansion but currently only log it.

	UE_LOG(LogNGGBridge, Log,
		TEXT("/editor/anim/add_look_at bp='%s' graph='%s' bone='%s' axis=[%.2f,%.2f,%.2f] axis_local=%d "
			 "target_bone='%s' socket='%s' loc_space='%s' clamp=%.1f interp='%s' time=%.3f alpha=%.2f splice=%d off=[%.0f,%.0f]"),
		*AssetPath, *GraphName, *BoneToModify,
		LookAtAxisVec.X, LookAtAxisVec.Y, LookAtAxisVec.Z, bLookAtAxisLocal ? 1 : 0,
		*LookAtTargetBone, *LookAtSocket, *LookAtLocationSpaceStr,
		LookAtClampIn, *InterpolationTypeStr, InterpolationTimeIn, AlphaIn,
		bSplice ? 1 : 0, OffsetX, OffsetY);

	const FString CapAssetPath          = AssetPath;
	const FString CapGraphName          = GraphName;
	const FString CapBoneToModify       = BoneToModify;
	const FVector CapLookAtAxisVec      = LookAtAxisVec;
	const bool    CapLookAtAxisLocal    = bLookAtAxisLocal;
	const FString CapLookAtTargetBone   = LookAtTargetBone;
	const FString CapLookAtSocket       = LookAtSocket;
	const FVector CapLookAtLocation     = LookAtLocation;
	const FString CapLookAtLocationSpace = LookAtLocationSpaceStr;
	const float   CapLookAtClamp        = (float)LookAtClampIn;
	const float   CapInterpolationTime  = (float)InterpolationTimeIn;
	const float   CapAlpha              = (float)AlphaIn;
	const bool    CapSplice             = bSplice;
	const int32   CapOffsetX            = (int32)OffsetX;
	const int32   CapOffsetY            = (int32)OffsetY;
	// InterpolationType is a POD enum — capture by value.

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapAssetPath, CapGraphName, CapBoneToModify,
		 CapLookAtAxisVec, CapLookAtAxisLocal,
		 CapLookAtTargetBone, CapLookAtSocket,
		 CapLookAtLocation, CapLookAtLocationSpace,
		 CapLookAtClamp, CapInterpolationTime, CapAlpha,
		 CapSplice, CapOffsetX, CapOffsetY,
		 InterpolationType]()
	{
		// 1. Load the AnimBlueprint (with .BaseFilename fallback)
		UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *CapAssetPath);
		if (!AnimBP)
		{
			const FString AltPath = CapAssetPath + TEXT(".") + FPaths::GetBaseFilename(CapAssetPath);
			AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AltPath);
		}
		if (!AnimBP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("AnimBlueprint not found: %s"), *CapAssetPath)));
			return;
		}

		// 2. Find the requested graph
		UEdGraph* AnimGraph = nullptr;
		const FName WantedName(*CapGraphName);
		for (UEdGraph* G : AnimBP->FunctionGraphs)
		{
			if (G && G->GetFName() == WantedName) { AnimGraph = G; break; }
		}
		if (!AnimGraph)
		{
			for (UEdGraph* G : AnimBP->UbergraphPages)
			{
				if (G && G->GetFName() == WantedName) { AnimGraph = G; break; }
			}
		}
		if (!AnimGraph)
		{
			Callback(JsonError(500, FString::Printf(TEXT("Could not find graph '%s' in the AnimBlueprint"), *CapGraphName)));
			return;
		}

		// 3. Locate the Output Pose root node
		UAnimGraphNode_Root* RootNode = nullptr;
		for (UEdGraphNode* N : AnimGraph->Nodes)
		{
			RootNode = Cast<UAnimGraphNode_Root>(N);
			if (RootNode) break;
		}
		if (!RootNode)
		{
			Callback(JsonError(500, TEXT("Could not find Output Pose (UAnimGraphNode_Root) in AnimGraph")));
			return;
		}

		UEdGraphPin* RootInputPin = nullptr;
		for (UEdGraphPin* Pin : RootNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input) { RootInputPin = Pin; break; }
		}
		if (!RootInputPin)
		{
			Callback(JsonError(500, TEXT("Output Pose node has no input pin")));
			return;
		}

		// 4. Spawn the LookAt node, configure runtime fields, then allocate pins.
		// Setting Node fields BEFORE AllocateDefaultPins is the safe pattern (matches
		// LayeredBoneBlend / TwoBoneIK / ModifyBone seeding).
		UAnimGraphNode_LookAt* LANode = NewObject<UAnimGraphNode_LookAt>(AnimGraph);
		LANode->CreateNewGuid();
		LANode->PostPlacedNewNode(); // creates default UAnimGraphNodeBinding_Base sub-object — without this the node compiles ErrorType=4 and runs as a no-op
		LANode->NodePosX = RootNode->NodePosX + CapOffsetX;
		LANode->NodePosY = RootNode->NodePosY + CapOffsetY;

		LANode->Node.BoneToModify.BoneName = FName(*CapBoneToModify);

		// FAxis: the runtime expects a normalized axis. The constructor normalizes; we
		// also assign bInLocalSpace explicitly.
		FAxis Axis;
		Axis.Axis = CapLookAtAxisVec.GetSafeNormal();
		Axis.bInLocalSpace = CapLookAtAxisLocal;
		LANode->Node.LookAt_Axis = Axis;

		// Target — socket wins over bone if both supplied, matching the FBoneSocketTarget
		// "bUseSocket" semantics.
		FBoneSocketTarget Target;
		if (!CapLookAtSocket.IsEmpty())
		{
			Target.bUseSocket = true;
			Target.SocketReference.SocketName = FName(*CapLookAtSocket);
			// If a target bone was also given it's stored on the BoneReference for completeness.
			if (!CapLookAtTargetBone.IsEmpty())
			{
				Target.BoneReference.BoneName = FName(*CapLookAtTargetBone);
			}
		}
		else if (!CapLookAtTargetBone.IsEmpty())
		{
			Target.bUseSocket = false;
			Target.BoneReference.BoneName = FName(*CapLookAtTargetBone);
		}
		else
		{
			// No target bone/socket → LookAtLocation is interpreted as world-space.
			Target.bUseSocket = false;
		}
		LANode->Node.LookAtTarget = Target;

		LANode->Node.LookAtLocation     = CapLookAtLocation;
		LANode->Node.LookAtClamp        = CapLookAtClamp;
		LANode->Node.InterpolationType  = InterpolationType;
		LANode->Node.InterpolationTime  = CapInterpolationTime;
		LANode->Node.Alpha              = CapAlpha;
		LANode->Node.bUseLookUpAxis     = false;

		AnimGraph->AddNode(LANode, /*bFromUI*/ false, /*bSelectNewNode*/ false);
		LANode->AllocateDefaultPins();

		// LookAtLocation, LookAtClamp, InterpolationType, InterpolationTime are declared as
		// PinHiddenByDefault on FAnimNode_LookAt — without flipping ShowPinForProperties they
		// can only be set via Details panel, not driven by graph wires. Force-show LookAtLocation
		// (the most common runtime-driven pin — the AnimBP feeds AimWorldTarget into it) and
		// reconstruct so the pin actually materializes.
		bool bReconstructed = false;
		for (FOptionalPinFromProperty& OptPin : LANode->ShowPinForProperties)
		{
			if (OptPin.PropertyName == TEXT("LookAtLocation") && !OptPin.bShowPin)
			{
				OptPin.bShowPin = true;
				bReconstructed = true;
			}
		}
		if (bReconstructed)
		{
			LANode->ReconstructNode();
		}

		// 5. Optional splice: insert LANode between (whatever feeds RootInputPin) and RootInputPin.
		bool bSplicedUpstream = false;
		FString UpstreamNodeName;
		{
			UEdGraphPin* InPin = nullptr; UEdGraphPin* OutPin = nullptr;
			FindPosePins(LANode, InPin, OutPin);
			if (CapSplice)
			{
				if (!InPin || !OutPin)
				{
					Callback(JsonError(500, TEXT("Could not find ComponentPose pins on the LookAt node — cannot splice")));
					return;
				}
				bSplicedUpstream = SpliceBeforeOutputPose(AnimGraph, RootInputPin, InPin, OutPin, UpstreamNodeName);
			}
		}

		// 6. Compile + dirty
		FKismetEditorUtilities::CompileBlueprint(AnimBP);
		AnimBP->MarkPackageDirty();

		// 7. Walk the spawned node's pins so we report the *actual* runtime pin names.
		// LookAt declares LookAtLocation, InterpolationType, LookAtClamp, InterpolationTime,
		// InterpolationTriggerThreashold (sic) as PinHiddenByDefault — they only exist as pins
		// when the user un-hides them. But Alpha (from SkeletalControlBase) is always present.
		// We list every input pin that is NOT a pose link and not already connected.
		TArray<TSharedPtr<FJsonValue>> UnboundPins;
		for (UEdGraphPin* P : LANode->Pins)
		{
			if (!P || P->Direction != EGPD_Input) continue;
			const bool bIsPose =
				P->PinType.PinSubCategoryObject == FComponentSpacePoseLink::StaticStruct() ||
				P->PinType.PinSubCategoryObject == FPoseLink::StaticStruct() ||
				P->PinName == TEXT("ComponentPose") || P->PinName == TEXT("Pose");
			if (bIsPose) continue;
			if (P->LinkedTo.Num() > 0) continue;
			UnboundPins.Add(MakeShared<FJsonValueString>(P->PinName.ToString()));
		}

		UE_LOG(LogNGGBridge, Log,
			TEXT("/editor/anim/add_look_at: added LookAt on bone='%s' (spliced=%d upstream='%s' unbound=%d)"),
			*CapBoneToModify, bSplicedUpstream ? 1 : 0, *UpstreamNodeName, UnboundPins.Num());

		// 8. Build response
		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("ok"),               true);
		Resp->SetStringField(TEXT("asset_path"),       AnimBP->GetPathName());
		Resp->SetStringField(TEXT("node_id"),          LANode->NodeGuid.ToString());
		Resp->SetStringField(TEXT("bone"),             CapBoneToModify);
		Resp->SetBoolField  (TEXT("spliced_upstream"), bSplicedUpstream);
		if (!UpstreamNodeName.IsEmpty()) Resp->SetStringField(TEXT("upstream_was"), UpstreamNodeName);
		Resp->SetArrayField (TEXT("unbound_pins"),     UnboundPins);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp, Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/anim/add_aim_offset_blend_space
//
// Spawn a UAnimGraphNode_RotationOffsetBlendSpace ("AimOffset Player") in an
// AnimBP's AnimGraph and (by default) splice it before the Output Pose so it
// applies an authored aim-offset blend space (UAimOffsetBlendSpace /
// UAimOffsetBlendSpace1D) on top of whatever pose currently feeds the root.
//
// This is the proper UE5 shooter-pattern aim system: the blend space supplies
// pre-authored "aim up / center / down" (and optionally left/right) per-bone
// rotation deltas. Driving it via a normalized pitch/yaw avoids the per-frame
// FRotator hacks that hand-rolled ModifyBone(spine_03) approaches need.
//
// The X / Y / Alpha pins are LEFT UNCONNECTED so the AnimBP can drive them
// from BlueprintThreadSafeUpdateAnimation (e.g. a normalized aim-pitch float
// from the character's AimRelativeRotation). For a 1D Y-only blend space
// (like AO_Rifle), only Y matters; X (and Z, if exposed) can stay at default.
//
// Body:
//   {
//     "anim_bp_path":         "/Game/Path/ABP_X",                         // required
//     "graph":                "AnimGraph",                                 // optional, default "AnimGraph"
//     "blend_space":          "/Game/.../AO_Rifle.AO_Rifle",               // required - hard ref to UAimOffsetBlendSpace[1D]
//     "alpha":                1.0,                                          // baked default; pin can override
//     "splice_before_output": true,                                         // default true
//     "node_offset_x":        -250,                                         // default
//     "node_offset_y":        0
//   }
//
// Response:
//   { "ok": true, "asset_path": "...", "node_id": "<guid>",
//     "blend_space": "...", "spliced_upstream": bool, "upstream_was": "...",
//     "unbound_pins": ["X","Y","Alpha", ...] }
// ============================================================================

bool FNGGHttpServer::HandleAnimAddAimOffsetBlendSpace(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString AssetPath, BlendSpacePath;
	Body->TryGetStringField(TEXT("anim_bp_path"), AssetPath);
	Body->TryGetStringField(TEXT("blend_space"),  BlendSpacePath);
	if (AssetPath.IsEmpty() || BlendSpacePath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("'anim_bp_path' and 'blend_space' are required")));
		return true;
	}

	FString GraphName = TEXT("AnimGraph");
	Body->TryGetStringField(TEXT("graph"), GraphName);

	double AlphaIn = 1.0;
	Body->TryGetNumberField(TEXT("alpha"), AlphaIn);

	bool bSplice = true;
	Body->TryGetBoolField(TEXT("splice_before_output"), bSplice);

	double OffsetX = -250.0, OffsetY = 0.0;
	Body->TryGetNumberField(TEXT("node_offset_x"), OffsetX);
	Body->TryGetNumberField(TEXT("node_offset_y"), OffsetY);

	UE_LOG(LogNGGBridge, Log,
		TEXT("/editor/anim/add_aim_offset_blend_space bp='%s' graph='%s' bs='%s' alpha=%.2f splice=%d off=[%.0f,%.0f]"),
		*AssetPath, *GraphName, *BlendSpacePath, AlphaIn, bSplice ? 1 : 0, OffsetX, OffsetY);

	const FString CapAssetPath     = AssetPath;
	const FString CapGraphName     = GraphName;
	const FString CapBlendSpacePath = BlendSpacePath;
	const float   CapAlpha         = (float)AlphaIn;
	const bool    CapSplice        = bSplice;
	const int32   CapOffsetX       = (int32)OffsetX;
	const int32   CapOffsetY       = (int32)OffsetY;

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapAssetPath, CapGraphName, CapBlendSpacePath,
		 CapAlpha, CapSplice, CapOffsetX, CapOffsetY]()
	{
		// 1. Load the AnimBlueprint (with .BaseFilename fallback)
		UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *CapAssetPath);
		if (!AnimBP)
		{
			const FString AltPath = CapAssetPath + TEXT(".") + FPaths::GetBaseFilename(CapAssetPath);
			AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AltPath);
		}
		if (!AnimBP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("AnimBlueprint not found: %s"), *CapAssetPath)));
			return;
		}

		// 2. Resolve the BlendSpace asset (must be a UBlendSpace; AimOffset[1D] are subclasses).
		UBlendSpace* BlendSpace = LoadObject<UBlendSpace>(nullptr, *CapBlendSpacePath);
		if (!BlendSpace)
		{
			const FString AltPath = CapBlendSpacePath + TEXT(".") + FPaths::GetBaseFilename(CapBlendSpacePath);
			BlendSpace = LoadObject<UBlendSpace>(nullptr, *AltPath);
		}
		if (!BlendSpace)
		{
			Callback(JsonError(404, FString::Printf(TEXT("BlendSpace not found: %s"), *CapBlendSpacePath)));
			return;
		}
		// Warn (don't reject) if it's not actually an AimOffset — the runtime node will compile
		// the same way, but the validator on UAnimGraphNode_RotationOffsetBlendSpace will error.
		const bool bIsAimOffset =
			BlendSpace->IsA(UAimOffsetBlendSpace::StaticClass()) ||
			BlendSpace->IsA(UAimOffsetBlendSpace1D::StaticClass());
		if (!bIsAimOffset)
		{
			UE_LOG(LogNGGBridge, Warning,
				TEXT("/editor/anim/add_aim_offset_blend_space: '%s' is a UBlendSpace but NOT a UAimOffsetBlendSpace[1D] — the AnimBP compiler will reject this."),
				*BlendSpace->GetPathName());
		}

		// 3. Find the requested graph
		UEdGraph* AnimGraph = nullptr;
		const FName WantedName(*CapGraphName);
		for (UEdGraph* G : AnimBP->FunctionGraphs)
		{
			if (G && G->GetFName() == WantedName) { AnimGraph = G; break; }
		}
		if (!AnimGraph)
		{
			for (UEdGraph* G : AnimBP->UbergraphPages)
			{
				if (G && G->GetFName() == WantedName) { AnimGraph = G; break; }
			}
		}
		if (!AnimGraph)
		{
			Callback(JsonError(500, FString::Printf(TEXT("Could not find graph '%s' in the AnimBlueprint"), *CapGraphName)));
			return;
		}

		// 4. Locate the Output Pose root node and its input pin
		UAnimGraphNode_Root* RootNode = nullptr;
		for (UEdGraphNode* N : AnimGraph->Nodes)
		{
			RootNode = Cast<UAnimGraphNode_Root>(N);
			if (RootNode) break;
		}
		if (!RootNode)
		{
			Callback(JsonError(500, TEXT("Could not find Output Pose (UAnimGraphNode_Root) in AnimGraph")));
			return;
		}

		UEdGraphPin* RootInputPin = nullptr;
		for (UEdGraphPin* Pin : RootNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input) { RootInputPin = Pin; break; }
		}
		if (!RootInputPin)
		{
			Callback(JsonError(500, TEXT("Output Pose node has no input pin")));
			return;
		}

		// 5. Spawn the RotationOffsetBlendSpace node and seed runtime fields BEFORE pin allocation,
		// so the BlendSpace property and Alpha default are present when AllocateDefaultPins reads them.
		// Node.SetBlendSpace() is the public virtual setter on FAnimNode_BlendSpacePlayerBase
		// (X/Y/PlayRate/StartPosition are private FoldProperty fields driven via the AnimGraph
		// FoldedProperties system — they materialise as PinShownByDefault input pins on the node
		// without needing direct FAnimNode access).
		UAnimGraphNode_RotationOffsetBlendSpace* AONode = NewObject<UAnimGraphNode_RotationOffsetBlendSpace>(AnimGraph);
		AONode->CreateNewGuid();
		AONode->PostPlacedNewNode(); // creates default UAnimGraphNodeBinding_Base sub-object — without this the node compiles ErrorType=4 and runs as a no-op
		AONode->NodePosX = RootNode->NodePosX + CapOffsetX;
		AONode->NodePosY = RootNode->NodePosY + CapOffsetY;

		AONode->Node.SetBlendSpace(BlendSpace);
		AONode->Node.Alpha = CapAlpha;

		AnimGraph->AddNode(AONode, /*bFromUI*/ false, /*bSelectNewNode*/ false);
		AONode->AllocateDefaultPins();

		// 6. Splice: locate BasePose input + output pose pin.
		// The RotationOffsetBlendSpace BasePose lives on FAnimNode_RotationOffsetBlendSpace itself
		// (not via SkeletalControlBase), so the input pin is named "BasePose" — distinct from
		// FindPosePins' "ComponentPose"/PoseLink heuristic. Walk pins manually.
		UEdGraphPin* BaseInPin = nullptr;
		UEdGraphPin* OutPosePin = nullptr;
		for (UEdGraphPin* P : AONode->Pins)
		{
			if (!P) continue;
			const bool bIsLocalPose =
				P->PinType.PinSubCategoryObject == FPoseLink::StaticStruct();
			const bool bIsCSPose =
				P->PinType.PinSubCategoryObject == FComponentSpacePoseLink::StaticStruct();
			if (P->Direction == EGPD_Input && (bIsLocalPose || bIsCSPose ||
				P->PinName == TEXT("BasePose") || P->PinName == TEXT("ComponentPose") || P->PinName == TEXT("Pose")))
			{
				BaseInPin = P;
			}
			else if (P->Direction == EGPD_Output && (bIsLocalPose || bIsCSPose))
			{
				OutPosePin = P;
			}
		}
		if (CapSplice && (!BaseInPin || !OutPosePin))
		{
			Callback(JsonError(500, TEXT("Could not find BasePose / output pose pins on the AimOffset node — cannot splice")));
			return;
		}

		bool bSplicedUpstream = false;
		FString UpstreamNodeName;
		if (CapSplice)
		{
			bSplicedUpstream = SpliceBeforeOutputPose(AnimGraph, RootInputPin, BaseInPin, OutPosePin, UpstreamNodeName);
		}

		// 7. Compile + dirty
		FKismetEditorUtilities::CompileBlueprint(AnimBP);
		AnimBP->MarkPackageDirty();

		// 8. Walk pins post-allocation to report the *actual* runtime pin names so callers can
		// pass them to /bp/connect_pins without guessing. Skip pose links and already-connected
		// pins (the spliced BasePose). Includes X, Y, Alpha (PinShownByDefault) and any others
		// that materialised — exact names depend on the editor's GET_MEMBER_NAME_STRING_CHECKED
		// expansion against FAnimNode_BlendSpacePlayer / FAnimNode_RotationOffsetBlendSpace.
		TArray<TSharedPtr<FJsonValue>> UnboundPins;
		for (UEdGraphPin* P : AONode->Pins)
		{
			if (!P || P->Direction != EGPD_Input) continue;
			const bool bIsPose =
				P->PinType.PinSubCategoryObject == FComponentSpacePoseLink::StaticStruct() ||
				P->PinType.PinSubCategoryObject == FPoseLink::StaticStruct() ||
				P->PinName == TEXT("BasePose") || P->PinName == TEXT("ComponentPose") || P->PinName == TEXT("Pose");
			if (bIsPose) continue;
			if (P->LinkedTo.Num() > 0) continue;
			UnboundPins.Add(MakeShared<FJsonValueString>(P->PinName.ToString()));
		}

		UE_LOG(LogNGGBridge, Log,
			TEXT("/editor/anim/add_aim_offset_blend_space: added AimOffset for '%s' (spliced=%d upstream='%s' unbound=%d)"),
			*BlendSpace->GetPathName(), bSplicedUpstream ? 1 : 0, *UpstreamNodeName, UnboundPins.Num());

		// 9. Build response
		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("ok"),               true);
		Resp->SetStringField(TEXT("asset_path"),       AnimBP->GetPathName());
		Resp->SetStringField(TEXT("node_id"),          AONode->NodeGuid.ToString());
		Resp->SetStringField(TEXT("blend_space"),      BlendSpace->GetPathName());
		Resp->SetBoolField  (TEXT("spliced_upstream"), bSplicedUpstream);
		if (!UpstreamNodeName.IsEmpty()) Resp->SetStringField(TEXT("upstream_was"), UpstreamNodeName);
		Resp->SetArrayField (TEXT("unbound_pins"),     UnboundPins);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp, Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/skeleton/add_virtual_bone
//
// Add a virtual bone to a USkeleton. Virtual bones live in the parent bone's
// reference frame and are positioned at the target bone's transform — useful
// for "follow the weapon" rigs (parent=weapon_r, target=ik_hand_l).
//
// Body:
//   {
//     "skeleton_path": "/Game/.../SK_Mannequin",   // required
//     "source_bone":   "weapon_r",                 // required - parent (where the VB lives)
//     "target_bone":   "ik_hand_l",                // required - VB tracks this bone's position
//     "vb_name":       "VB_ik_hand_left_weapon_space" // optional - explicit name. If omitted, UE auto-names
//   }
// ============================================================================

bool FNGGHttpServer::HandleSkeletonAddVirtualBone(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString SkeletonPath, SourceBone, TargetBone, ExplicitName;
	Body->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	Body->TryGetStringField(TEXT("source_bone"),   SourceBone);
	Body->TryGetStringField(TEXT("target_bone"),   TargetBone);
	Body->TryGetStringField(TEXT("vb_name"),       ExplicitName);

	if (SkeletonPath.IsEmpty() || SourceBone.IsEmpty() || TargetBone.IsEmpty())
	{
		Callback(JsonError(400, TEXT("'skeleton_path', 'source_bone', and 'target_bone' are required")));
		return true;
	}

	UE_LOG(LogNGGBridge, Log,
		TEXT("/editor/skeleton/add_virtual_bone path='%s' src='%s' dst='%s' name='%s'"),
		*SkeletonPath, *SourceBone, *TargetBone, *ExplicitName);

	const FString CapturedPath = SkeletonPath;
	const FName   CapturedSrc(*SourceBone);
	const FName   CapturedDst(*TargetBone);
	const FString CapturedName = ExplicitName;

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapturedPath, CapturedSrc, CapturedDst, CapturedName]()
	{
		USkeleton* Skel = LoadObject<USkeleton>(nullptr, *CapturedPath);
		if (!Skel)
		{
			const FString Alt = CapturedPath + TEXT(".") + FPaths::GetBaseFilename(CapturedPath);
			Skel = LoadObject<USkeleton>(nullptr, *Alt);
		}
		if (!Skel)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Skeleton not found at '%s'"), *CapturedPath)));
			return;
		}

		// Validate the source/target bones exist before attempting the add.
		if (Skel->GetReferenceSkeleton().FindBoneIndex(CapturedSrc) == INDEX_NONE)
		{
			Callback(JsonError(400, FString::Printf(TEXT("Source bone '%s' not found in skeleton"), *CapturedSrc.ToString())));
			return;
		}
		if (Skel->GetReferenceSkeleton().FindBoneIndex(CapturedDst) == INDEX_NONE)
		{
			Callback(JsonError(400, FString::Printf(TEXT("Target bone '%s' not found in skeleton"), *CapturedDst.ToString())));
			return;
		}

		FName CreatedName;
		bool  bOk = false;
		if (!CapturedName.IsEmpty())
		{
			CreatedName = FName(*CapturedName);
			bOk = Skel->AddNewNamedVirtualBone(CapturedSrc, CapturedDst, CreatedName);
		}
		else
		{
			bOk = Skel->AddNewVirtualBone(CapturedSrc, CapturedDst, CreatedName);
		}

		if (!bOk)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("AddNewVirtualBone failed for src='%s' dst='%s' (already exists?)"),
				*CapturedSrc.ToString(), *CapturedDst.ToString())));
			return;
		}

		// Note: AddNewVirtualBone(...) already broadcasts the change internally.
		// HandleVirtualBoneChanges() is not exported from Engine, so we don't call it here.
		Skel->MarkPackageDirty();
		Skel->PostEditChange();

		UE_LOG(LogNGGBridge, Log,
			TEXT("/editor/skeleton/add_virtual_bone: created '%s' on '%s'"),
			*CreatedName.ToString(), *Skel->GetPathName());

		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("ok"),            true);
		Resp->SetStringField(TEXT("skeleton_path"), Skel->GetPathName());
		Resp->SetStringField(TEXT("source_bone"),   CapturedSrc.ToString());
		Resp->SetStringField(TEXT("target_bone"),   CapturedDst.ToString());
		Resp->SetStringField(TEXT("vb_name"),       CreatedName.ToString());

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp, Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/anim/delete_node
//
// Delete a node from an AnimBP's AnimGraph by GUID. If reconnect_pose=true
// (the default), the upstream pose-link feeding the node's input is rewired
// to the consumer of the node's output, keeping the chain intact. This is
// what lets you remove a stale skeletal control without breaking the graph.
//
// Body:
//   {
//     "anim_bp_path":   "/Game/Path/ABP_X",  // required
//     "node_id":        "1D75784643A7BBE329E596BA3875B8F8",  // required - GUID
//     "reconnect_pose": true                  // optional, default true
//   }
// ============================================================================

bool FNGGHttpServer::HandleAnimDeleteNode(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString AssetPath, NodeIdStr;
	bool    bReconnectPose = true;
	Body->TryGetStringField(TEXT("anim_bp_path"),   AssetPath);
	Body->TryGetStringField(TEXT("node_id"),        NodeIdStr);
	Body->TryGetBoolField  (TEXT("reconnect_pose"), bReconnectPose);
	if (AssetPath.IsEmpty() || NodeIdStr.IsEmpty())
	{
		Callback(JsonError(400, TEXT("'anim_bp_path' and 'node_id' are required")));
		return true;
	}

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeIdStr, NodeGuid))
	{
		// FGuid::Parse expects the canonical 32-hex-with-hyphens form. The MCP responses
		// emit the hex without hyphens. Try the fixed-length parser too.
		if (!FGuid::ParseExact(NodeIdStr, EGuidFormats::Digits, NodeGuid))
		{
			Callback(JsonError(400, FString::Printf(TEXT("Invalid GUID '%s'"), *NodeIdStr)));
			return true;
		}
	}

	UE_LOG(LogNGGBridge, Log, TEXT("/editor/anim/delete_node bp='%s' node='%s' reconnect=%d"),
		*AssetPath, *NodeIdStr, bReconnectPose ? 1 : 0);

	const FString CapturedAssetPath = AssetPath;
	const FGuid   CapturedGuid      = NodeGuid;
	const bool    CapturedReconnect = bReconnectPose;

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapturedAssetPath, CapturedGuid, CapturedReconnect]()
	{
		FAnimGraphContext Ctx; FString Err;
		if (!ResolveAnimGraphContext(CapturedAssetPath, Ctx, Err))
		{
			Callback(JsonError(404, Err));
			return;
		}

		// Find the node by GUID
		UEdGraphNode* Target = nullptr;
		for (UEdGraphNode* N : Ctx.Graph->Nodes)
		{
			if (N && N->NodeGuid == CapturedGuid) { Target = N; break; }
		}
		if (!Target)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Node with id '%s' not found in AnimGraph"), *CapturedGuid.ToString())));
			return;
		}

		const FString DeletedClassName = Target->GetClass()->GetName();
		const FString DeletedNodeName  = Target->GetName();

		// Optionally reconnect the pose chain: find this node's input/output pose pins,
		// link the upstream of the input to the downstream of the output.
		bool bReconnected = false;
		if (CapturedReconnect)
		{
			UEdGraphPin* InPin = nullptr; UEdGraphPin* OutPin = nullptr;
			// Try the SkeletalControlBase / standard pose pin pattern first.
			if (UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Target))
			{
				FindPosePins(AnimNode, InPin, OutPin);
			}
			// Fallback: any input/output pose-typed pin.
			if (!InPin || !OutPin)
			{
				for (UEdGraphPin* P : Target->Pins)
				{
					if (!P) continue;
					const bool bPose =
						P->PinType.PinSubCategoryObject == FComponentSpacePoseLink::StaticStruct() ||
						P->PinType.PinSubCategoryObject == FPoseLink::StaticStruct();
					if (bPose && P->Direction == EGPD_Input  && !InPin)  InPin  = P;
					if (bPose && P->Direction == EGPD_Output && !OutPin) OutPin = P;
				}
			}

			UEdGraphPin* UpstreamOut  = (InPin  && InPin->LinkedTo.Num()  > 0) ? InPin->LinkedTo[0]  : nullptr;
			TArray<UEdGraphPin*> Downstreams;
			if (OutPin) { Downstreams = OutPin->LinkedTo; }

			if (UpstreamOut && Downstreams.Num() > 0)
			{
				const UEdGraphSchema* Schema = Ctx.Graph->GetSchema();
				if (Schema)
				{
					for (UEdGraphPin* DS : Downstreams)
					{
						if (DS) Schema->TryCreateConnection(UpstreamOut, DS);
					}
					bReconnected = true;
				}
			}
		}

		// Break all links on the target's pins, then remove the node.
		Target->BreakAllNodeLinks();
		Ctx.Graph->RemoveNode(Target);

		FKismetEditorUtilities::CompileBlueprint(Ctx.AnimBP);
		Ctx.AnimBP->MarkPackageDirty();

		UE_LOG(LogNGGBridge, Log,
			TEXT("/editor/anim/delete_node: removed '%s' (class=%s) reconnected=%d"),
			*DeletedNodeName, *DeletedClassName, bReconnected ? 1 : 0);

		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("ok"),                true);
		Resp->SetStringField(TEXT("asset_path"),        Ctx.AnimBP->GetPathName());
		Resp->SetStringField(TEXT("deleted_node"),      DeletedNodeName);
		Resp->SetStringField(TEXT("deleted_class"),     DeletedClassName);
		Resp->SetBoolField  (TEXT("reconnected_pose"),  bReconnected);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp, Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}
