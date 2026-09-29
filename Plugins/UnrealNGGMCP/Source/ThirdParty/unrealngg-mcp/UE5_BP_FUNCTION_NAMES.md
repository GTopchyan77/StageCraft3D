# UE5 Blueprint Function Name Reference

Blueprint display names (what you see in the node palette, and what you pass as `"function"` in `ue5_bp_add_logic`) often differ from the internal UFunction name the engine stores in the `.uasset` graph node.  
The bridge resolves common aliases automatically — but if a call fails with "function not found", this file is the authoritative lookup.

**Column meanings**
- **Display name** — what you pass as `"function"` in `ue5_bp_add_logic`
- **Internal UFunction name** — what the engine actually stores; use this if the display name is rejected
- **Pure** — `BlueprintPure` (white diamond, **no exec pin**). Pure nodes cannot be part of an exec chain; wire exec directly between surrounding impure nodes.
- **Impure** — `BlueprintCallable` (has exec pin)

---

## General rules

1. **K2_ = the real name the engine stores.** `Set Actor Location` is cosmetic; `K2_SetActorLocation` is what lives in the `.uasset`. When creating nodes programmatically, use the internal name if the display name is rejected.
2. **Getters are almost always Pure (no exec pin); setters are always Impure.** Do not try to route exec flow through a getter — it has no `execute` or `then` pin.
3. **`Receive*` events are BlueprintImplementableEvents with a DisplayName.** `ReceiveBeginPlay` shows as "Event BeginPlay", `ReceiveTick` as "Event Tick". The bridge auto-aliases `BeginPlay` → `ReceiveBeginPlay` etc., so the short form is fine.
4. **Old redirect names still work at load time** (via `BaseEngine.ini K2FieldRedirects`) but must not be used when *creating* new nodes — use the current internal name.
5. **UE5.0+ uses `_DoubleDouble` math operators.** The `_FloatFloat` variants still exist as deprecated aliases but will produce compile warnings. See the KismetMathLibrary section.

---

## AActor

| Display Name | Internal UFunction Name | Pure? | Notes |
|---|---|---|---|
| Get Actor Location | `K2_GetActorLocation` | Pure | Returns FVector world location |
| Set Actor Location | `K2_SetActorLocation` | Impure | Has Sweep, SweepHitResult, bTeleport params |
| Get Actor Rotation | `K2_GetActorRotation` | Pure | Returns FRotator |
| Set Actor Rotation | `K2_SetActorRotation` | Impure | |
| Get Actor Scale 3D | `K2_GetActorScale3D` | Pure | Via RootComponent |
| Set Actor Scale 3D | `K2_SetActorScale3D` | Impure | |
| Get Actor Transform | `K2_GetActorTransform` | Pure | Returns FTransform |
| Set Actor Transform | `K2_SetActorTransform` | Impure | |
| Set Actor Location and Rotation | `K2_SetActorLocationAndRotation` | Impure | |
| Set Actor Relative Location | `K2_SetActorRelativeLocation` | Impure | |
| Set Actor Relative Rotation | `K2_SetActorRelativeRotation` | Impure | |
| Set Actor Relative Transform | `K2_SetActorRelativeTransform` | Impure | |
| Add Actor Local Offset | `K2_AddActorLocalOffset` | Impure | |
| Add Actor Local Rotation | `K2_AddActorLocalRotation` | Impure | |
| Add Actor Local Transform | `K2_AddActorLocalTransform` | Impure | |
| Add Actor World Offset | `K2_AddActorWorldOffset` | Impure | |
| Add Actor World Rotation | `K2_AddActorWorldRotation` | Impure | |
| Add Actor World Transform | `K2_AddActorWorldTransform` | Impure | |
| Add Actor World Transform (Keep Scale) | `K2_AddActorWorldTransformKeepScale` | Impure | UE5+ |
| Teleport To | `K2_TeleportTo` | Impure | Nudges out of geometry; returns bool |
| Destroy Actor | `K2_DestroyActor` | Impure | |
| Attach To Actor | `K2_AttachToActor` | Impure | |
| Attach To Component | `K2_AttachToComponent` | Impure | |
| Attach Root Component To | `K2_AttachRootComponentTo` | Impure | **Deprecated** — use AttachToComponent |
| Attach Root Component To Actor | `K2_AttachRootComponentToActor` | Impure | **Deprecated** — use AttachToActor |
| Get World | `K2_GetWorld` | Pure | |
| Get Components By Class | `K2_GetComponentsByClass` | Impure | Returns TArray |
| Get Actor Forward Vector | `GetActorForwardVector` | Pure | No K2_ prefix |
| Get Actor Right Vector | `GetActorRightVector` | Pure | No K2_ prefix |
| Get Actor Up Vector | `GetActorUpVector` | Pure | No K2_ prefix |

**AActor events (BlueprintImplementableEvent display-name aliases)**

| Blueprint display name | Internal UFunction name |
|---|---|
| Event BeginPlay | `ReceiveBeginPlay` |
| Event EndPlay | `ReceiveEndPlay` |
| Event Tick | `ReceiveTick` |
| Event Hit | `ReceiveHit` |
| Event Actor Begin Overlap | `ReceiveActorBeginOverlap` |
| Event Actor End Overlap | `ReceiveActorEndOverlap` |
| On Become View Target | `K2_OnBecomeViewTarget` |
| On End View Target | `K2_OnEndViewTarget` |

---

## USceneComponent

| Display Name | Internal UFunction Name | Pure? | Notes |
|---|---|---|---|
| Get World Location | `K2_GetComponentLocation` | Pure | C++ name is `GetComponentLocation()` — different |
| Get World Rotation | `K2_GetComponentRotation` | Pure | C++ name is `GetComponentRotation()` |
| Get World Scale | `K2_GetComponentScale` | Pure | C++ name is `GetComponentScale()` |
| Get Component To World | `K2_GetComponentToWorld` | Pure | Returns FTransform |
| Get Component Velocity | `GetComponentVelocity` | Pure | Exact match |
| Set World Location | `K2_SetWorldLocation` | Impure | Old name `SetWorldTranslation` redirected |
| Set World Rotation | `K2_SetWorldRotation` | Impure | |
| Set World Scale 3D | `K2_SetWorldScale3D` | Impure | |
| Set World Transform | `K2_SetWorldTransform` | Impure | |
| Add World Offset | `K2_AddWorldOffset` | Impure | |
| Add World Rotation | `K2_AddWorldRotation` | Impure | |
| Add World Transform | `K2_AddWorldTransform` | Impure | |
| Set Relative Location | `K2_SetRelativeLocation` | Impure | Old param `NewTranslation` → `NewLocation` |
| Set Relative Rotation | `K2_SetRelativeRotation` | Impure | |
| Set Relative Transform | `K2_SetRelativeTransform` | Impure | |
| Set Relative Location and Rotation | `K2_SetRelativeLocationAndRotation` | Impure | |
| Add Relative Location | `K2_AddRelativeLocation` | Impure | Old name `AddRelativeTranslation` |
| Add Relative Rotation | `K2_AddRelativeRotation` | Impure | |
| Add Local Offset | `K2_AddLocalOffset` | Impure | Old name `AddLocalTranslation` |
| Add Local Rotation | `K2_AddLocalRotation` | Impure | |
| Add Local Transform | `K2_AddLocalTransform` | Impure | |
| Attach To Component | `K2_AttachToComponent` | Impure | Modern version with Rules enum |
| Attach To (deprecated) | `K2_AttachTo` | Impure | **Deprecated** |
| Detach From Component | `K2_DetachFromComponent` | Impure | |

---

## UPrimitiveComponent

| Display Name | Internal UFunction Name | Pure? | Notes |
|---|---|---|---|
| Set Simulate Physics | `SetSimulatePhysics` | Impure | Exact match |
| Set Collision Enabled | `SetCollisionEnabled` | Impure | Exact match |
| Set Collision Profile Name | `SetCollisionProfileName` | Impure | Exact match |
| Set Collision Object Type | `SetCollisionObjectType` | Impure | Old name `SetMovementChannel` redirected |
| Create Dynamic Material Instance | `CreateDynamicMaterialInstance` | Impure | Old name `CreateAndSetMaterialInstanceDynamic` |
| Get Overlapping Actors | `GetOverlappingActors` | Pure | Old name `GetTouchingActors` |
| Get Overlapping Components | `GetOverlappingComponents` | Pure | Old name `GetTouchingComponents` |

---

## ACharacter

| Display Name | Internal UFunction Name | Pure? | Notes |
|---|---|---|---|
| Event On Start Crouch | `K2_OnStartCrouch` | Event | BlueprintImplementableEvent |
| Event On End Crouch | `K2_OnEndCrouch` | Event | BlueprintImplementableEvent |
| Event On Movement Mode Changed | `K2_OnMovementModeChanged` | Event | BlueprintImplementableEvent |

---

## AController / AAIController

| Display Name | Internal UFunction Name | Pure? | Notes |
|---|---|---|---|
| Get Controlled Pawn | `K2_GetPawn` | Pure | On AController; old name `GetControlledPawn` redirected |
| Set Focal Point | `K2_SetFocalPoint` | Impure | On AAIController |
| Set Focus | `K2_SetFocus` | Impure | On AAIController |
| Clear Focus | `K2_ClearFocus` | Impure | On AAIController |

---

## UMovementComponent / UCharacterMovementComponent

| Display Name | Internal UFunction Name | Pure? | Notes |
|---|---|---|---|
| Move Updated Component | `K2_MoveUpdatedComponent` | Impure | On UMovementComponent |
| Get Modified Max Speed | `K2_GetModifiedMaxSpeed` | Pure | On UMovementComponent |
| Get Max Speed Modifier | `K2_GetMaxSpeedModifier` | Pure | On UMovementComponent; **Deprecated** UE5 |
| Get Modified Max Acceleration | `K2_GetModifiedMaxAcceleration` | Pure | On UCharacterMovementComponent; **Deprecated** UE5 |
| Get Input Vector | `K2_GetInputVector` | Pure | On UPawnMovementComponent; **Deprecated** — use `GetPendingInputVector()` |
| Get Movement Input Vector | `K2_GetMovementInputVector` | Pure | On APawn |

**CharacterMovementComponent property renames (property names used in set/get variable nodes)**

| Old Name | New Name |
|---|---|
| `AccelRate` | `MaxAcceleration` |
| `GroundSpeed` | `MaxWalkSpeed` |
| `AirSpeed` | `MaxFlySpeed` |
| `WaterSpeed` | `MaxSwimSpeed` |
| `JumpZ` | `JumpZVelocity` |
| `CrouchedPct` | `CrouchedSpeedMultiplier` |
| `AddMomentum` (function) | `AddImpulse` |
| `bOrientToMovement` | `bOrientRotationToMovement` |

---

## UKismetSystemLibrary — Timers

All timer functions go through `K2_*` internal names. The delegate-based variants end in `Delegate`; handle-based variants end in `Handle`.

| Display Name | Internal UFunction Name | Pure? |
|---|---|---|
| Set Timer by Function Name | `K2_SetTimer` | Impure |
| Clear Timer by Function Name | `K2_ClearTimer` | Impure |
| Pause Timer by Function Name | `K2_PauseTimer` | Impure |
| Unpause Timer by Function Name | `K2_UnPauseTimer` | Impure |
| Is Timer Active by Function Name | `K2_IsTimerActive` | Pure |
| Is Timer Paused by Function Name | `K2_IsTimerPaused` | Pure |
| Does Timer Exist by Function Name | `K2_TimerExists` | Pure |
| Get Timer Elapsed Time by Function Name | `K2_GetTimerElapsedTime` | Pure |
| Get Timer Remaining Time by Function Name | `K2_GetTimerRemainingTime` | Pure |
| Set Timer by Event | `K2_SetTimerDelegate` | Impure |
| Clear Timer by Event | `K2_ClearTimerDelegate` | Impure |
| Pause Timer by Event | `K2_PauseTimerDelegate` | Impure |
| Unpause Timer by Event | `K2_UnPauseTimerDelegate` | Impure |
| Is Timer Active by Event | `K2_IsTimerActiveDelegate` | Pure |
| Is Timer Paused by Event | `K2_IsTimerPausedDelegate` | Pure |
| Does Timer Exist by Event | `K2_TimerExistsDelegate` | Pure |
| Get Timer Elapsed Time by Event | `K2_GetTimerElapsedTimeDelegate` | Pure |
| Get Timer Remaining Time by Event | `K2_GetTimerRemainingTimeDelegate` | Pure |
| Clear Timer by Handle | `K2_ClearTimerHandle` | Impure |
| Clear and Invalidate Timer by Handle | `K2_ClearAndInvalidateTimerHandle` | Impure |
| Pause Timer by Handle | `K2_PauseTimerHandle` | Impure |
| Unpause Timer by Handle | `K2_UnPauseTimerHandle` | Impure |
| Is Timer Active by Handle | `K2_IsTimerActiveHandle` | Pure |
| Is Timer Paused by Handle | `K2_IsTimerPausedHandle` | Pure |
| Does Timer Exist by Handle | `K2_TimerExistsHandle` | Pure |
| Get Timer Elapsed Time by Handle | `K2_GetTimerElapsedTimeHandle` | Pure |
| Get Timer Remaining Time by Handle | `K2_GetTimerRemainingTimeHandle` | Pure |
| Is Valid (FTimerHandle) | `K2_IsValidTimerHandle` | Pure |
| Invalidate (FTimerHandle) | `K2_InvalidateTimerHandle` | Impure |
| Set Timer for Next Tick | `K2_SetTimerForNextTick` | Impure | UE5+ |
| Set Timer for Next Tick (Delegate) | `K2_SetTimerForNextTickDelegate` | Impure | UE5+ |

---

## UNavigationSystemV1

| Display Name | Internal UFunction Name | Pure? | Notes |
|---|---|---|---|
| Get Random Point in Navigable Radius | `K2_GetRandomPointInNavigableRadius` | Pure | **Deprecated** UE5 — use `GetRandomLocationInNavigableRadius` |
| Project Point to Navigation | `K2_ProjectPointToNavigation` | Pure | |

---

## UGameplayStatics — rename redirects

GameplayStatics functions mostly match their display names. Common renames from UE4:

| Old Display Name | Current Internal Name |
|---|---|
| Get Player Camera | `GetPlayerCameraManager` |
| Get Game Info | `GetGameMode` |
| Get Game Replication Info | `GetGameState` |
| Load Stream Level | `LoadStreamLevel` (moved from LevelScriptActor) |
| Unload Stream Level | `UnloadStreamLevel` (moved from LevelScriptActor) |
| Open Level | `OpenLevel` (moved from LevelScriptActor) |

---

## UKismetMathLibrary — Float → Double renames (UE5.0+)

UE5.0 promoted scalar types from `float` (32-bit) to `double` (64-bit). All math operator UFunction names changed from `_FloatFloat` to `_DoubleDouble`. The old names still exist as deprecated aliases but produce compile warnings.

**Always use the `_DoubleDouble` form in UE5 projects.**

| Blueprint Display Name | UE4 Internal Name | UE5 Internal Name |
|---|---|---|
| float + float | `Add_FloatFloat` | `Add_DoubleDouble` |
| float − float | `Subtract_FloatFloat` | `Subtract_DoubleDouble` |
| float × float | `Multiply_FloatFloat` | `Multiply_DoubleDouble` |
| float ÷ float | `Divide_FloatFloat` | `Divide_DoubleDouble` |
| float == float | `EqualEqual_FloatFloat` | `EqualEqual_DoubleDouble` |
| float != float | `NotEqual_FloatFloat` | `NotEqual_DoubleDouble` |
| float < float | `Less_FloatFloat` | `Less_DoubleDouble` |
| float > float | `Greater_FloatFloat` | `Greater_DoubleDouble` |
| float <= float | `LessEqual_FloatFloat` | `LessEqual_DoubleDouble` |
| float >= float | `GreaterEqual_FloatFloat` | `GreaterEqual_DoubleDouble` |
| float % float (FMod) | `Percent_FloatFloat` | `Percent_DoubleDouble` |
| Power (float ** float) | `MultiplyMultiply_FloatFloat` | `MultiplyMultiply_DoubleDouble` |

Functions that did **not** change names (already overloaded or type-generic):

| Display Name | Internal Name | Pure? |
|---|---|---|
| Abs | `Abs` | Pure |
| FMin | `FMin` | Pure |
| FMax | `FMax` | Pure |
| FClamp | `FClamp` | Pure |
| Lerp (float) | `Lerp` | Pure |
| Lerp (vector) | `VLerp` | Pure |
| Ease | `Ease` | Pure |
| Sin | `Sin` | Pure | Takes radians |
| Cos | `Cos` | Pure | Takes radians |
| DegSin | `DegSin` | Pure | Takes degrees |
| DegCos | `DegCos` | Pure | Takes degrees |
| Make Rotator | `MakeRotator` | Pure |
| Break Rotator | `BreakRotator` | Pure |
| Make Transform | `MakeTransform` | Pure |
| Break Transform | `BreakTransform` | Pure |
| Find Look at Rotation | `FindLookAtRotation` | Pure |
| Vector Length | `VSize` | Pure |
| Vector Length 2D | `VSize2D` | Pure |
| Vector Length Squared | `VSizeSquared` | Pure |
| Normalize (vector) | `Normal` | Pure |
| Dot Product | `Dot_VectorVector` | Pure |
| Cross Product | `Cross_VectorVector` | Pure |
| Get Forward Vector (from Rotator) | `GetForwardVector` | Pure |
| Get Right Vector (from Rotator) | `GetRightVector` | Pure |
| Get Up Vector (from Rotator) | `GetUpVector` | Pure |

---

## Quick-diagnosis: "function not found" checklist

1. Is the class correct? `AddActorLocalOffset` is on `Actor`, not `SceneComponent`.
2. Is the K2_ prefix needed? Check this file for the internal name.
3. Is it a Pure function you're trying to exec-chain through? Pure nodes have no exec pins — wire exec directly between the surrounding impure nodes.
4. Is it a UE5.0 math rename? Use `_DoubleDouble` not `_FloatFloat`.
5. Is it a deprecated UE4 name? Check the redirects tables above for the current name.
6. Is the display name actually a `BlueprintImplementableEvent`? Those must be declared as `type: "event"` or `type: "custom_event"` nodes, not `call_function`.
