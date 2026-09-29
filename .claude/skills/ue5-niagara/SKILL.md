---
name: ue5-niagara
description: >
  Use this skill for any task involving Unreal Engine 5 Niagara VFX — creating Niagara Systems,
  Emitters, Modules, and Parameters from scratch or in C++/Blueprint; spawning effects at runtime;
  setting User parameters from C++; pooling, performance optimization, debugging; Niagara Data
  Channels; Niagara Fluids; custom HLSL modules; events & event handlers; and migrating from
  Cascade. Trigger whenever the user mentions "Niagara", "particle system", "VFX", "spawn effect",
  "emitter", "NiagaraComponent", "NiagaraFunctionLibrary", or anything related to real-time
  visual effects in UE5. Also trigger when the user asks how to attach, activate, deactivate,
  or pass data into a particle effect in Unreal Engine.
---

# UE5 Niagara Effects Skill

Source: UE 5.7 official documentation + community best practices.
Docs hub: https://dev.epicgames.com/documentation/en-us/unreal-engine/creating-visual-effects-in-niagara-for-unreal-engine

---

## 1. Core Concepts

Niagara has **four building blocks**:

| Block | Role |
|---|---|
| **System** | Top-level container. Holds one or more Emitters. Placed in the world as a `UNiagaraComponent`. |
| **Emitter** | Spawns and manages a population of particles. Stack-based: groups run top-to-bottom. |
| **Module** | Smallest unit of logic. Written in HLSL (or visually via the Graph). Added to Emitter groups. |
| **Parameter** | Abstracted data passed between modules. Types: Primitive, Enum, Struct, Data Interface. |

### Emitter Stack Groups (top-to-bottom execution order)

1. **Emitter Spawn** — runs once when the emitter is first created (CPU). Initial defaults.
2. **Emitter Update** — runs every frame (CPU). Spawn rate, looping, lifecycle.
3. **Particle Spawn** — runs once per particle at birth. Initial position, color, size.
4. **Particle Update** — runs every frame per particle. Forces, color over life, size over life.
5. **Event Handler** — inter-emitter communication (Generate + Listen events).
6. **Render** — how particles look: Sprite, Mesh, Ribbon, Light, Decal renderers.

Modules are processed **sequentially top-to-bottom** within each group. Order matters.

---

## 2. Editor Workflow

### Creating a Niagara System

1. **Content Browser → Add → Niagara System**
2. Choose: *New system from selected emitter(s)* or *Empty System*.
3. Double-click to open the **System Editor**.
4. The **Timeline** panel shows all contained emitters.

### Creating / Adding an Emitter

- Inside System Editor → **Add Emitter** button (top of Timeline).
- Choose a template (Fountain, Smoke, GPU Sprite, etc.) or start empty.
- Each emitter lives as an **Emitter Instance** inside the system (inherits from a shared Emitter asset).

### Adding Modules to an Emitter Group

1. Click **+** inside a group (e.g., Particle Update).
2. Browse or search the module library.
3. Drag to reorder. Modules resolve top-to-bottom.

### User-Exposed Parameters (for external control)

- In the **Parameters** panel, switch namespace to `User`.
- User parameters are the only ones settable from Blueprints/C++ at runtime.
- Naming convention: `User.MyParamName` (the prefix is required for external access).

---

## 3. C++ Integration

### Build.cs — Required Modules

```csharp
PublicDependencyModuleNames.AddRange(new string[]
{
    "Core", "CoreUObject", "Engine",
    "Niagara",      // UNiagaraComponent, UNiagaraFunctionLibrary, UNiagaraSystem
    "NiagaraCore",  // UNiagaraDataInterface base
});
```

### Required Headers

```cpp
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
```

### Declaring the Asset Reference (Header)

```cpp
UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="VFX")
TObjectPtr<UNiagaraSystem> ImpactVFXSystem;

// Optional: keep a live reference for systems that persist
UPROPERTY()
TObjectPtr<UNiagaraComponent> NiagaraComp;
```

### Spawn at World Location (one-shot)

```cpp
UNiagaraComponent* Comp = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
    this,                        // WorldContextObject
    ImpactVFXSystem,             // UNiagaraSystem* asset
    HitLocation,                 // FVector Location
    FRotator::ZeroRotator,       // FRotator Rotation
    FVector(1.f),                // FVector Scale
    /*bAutoDestroy=*/ true,      // destroy component when system finishes
    /*bAutoActivate=*/ true,
    ENCPoolMethod::AutoRelease,  // use the component pool when available
    /*bPreCullCheck=*/ true      // skip spawn if off-screen (perf)
);

// ALWAYS null-check — returns nullptr on dedicated servers and when culled
if (Comp)
{
    Comp->SetVariableVec3(FName("User.HitNormal"), HitNormal);
    Comp->SetVariableLinearColor(FName("User.HitColor"), DamageColor);
}
```

### Spawn Attached to a Component

```cpp
NiagaraComp = UNiagaraFunctionLibrary::SpawnSystemAttached(
    TrailVFXSystem,
    GetMesh(),               // USceneComponent* parent
    NAME_None,               // socket name (or NAME_None)
    FVector::ZeroVector,
    FRotator::ZeroRotator,
    FVector::OneVector,
    EAttachLocation::KeepRelativeOffset,
    /*bAutoDestroy=*/ false, // keep alive; we'll stop it manually
    ENCPoolMethod::None
);
```

### Setting User Parameters from C++

All setter functions live on `UNiagaraComponent`. Set values **before the first tick** (or at any time for continuously updated params):

```cpp
// Scalar / float
NiagaraComp->SetVariableFloat(FName("User.Speed"), 350.f);

// Integer
NiagaraComp->SetVariableInt(FName("User.BurstCount"), 8);

// Bool (useful for triggering burst events)
NiagaraComp->SetVariableBool(FName("User.bJustDied"), true);

// Vector
NiagaraComp->SetVariableVec3(FName("User.Direction"), Direction);

// Linear color (RGBA)
NiagaraComp->SetVariableLinearColor(FName("User.EmberColor"), FLinearColor::Red);

// Material (for Renderer material overrides)
NiagaraComp->SetVariableMaterial(FName("User.MyMaterial"), MatInstance);

// Actor reference
NiagaraComp->SetVariableActor(FName("User.Target"), TargetActor);
```

> **Note:** There are no public `GetVariable*` functions in UE5.5+. Read-back of Niagara state from C++ is not supported. Design your logic so Niagara consumes parameters, not returns them.

### Activate / Deactivate / Reset

```cpp
NiagaraComp->Activate(/*bReset=*/ true);   // restart from scratch
NiagaraComp->Deactivate();                 // let particles finish, then stop
NiagaraComp->DeactivateImmediate();        // kill all particles instantly

// Check if running
bool bIsActive = NiagaraComp->IsActive();
```

### Lifecycle Callback

```cpp
// Bind in BeginPlay:
NiagaraComp->OnSystemFinished.AddDynamic(this, &AMyActor::OnVFXFinished);

void AMyActor::OnVFXFinished(UNiagaraComponent* FinishedComp)
{
    // do something when system naturally completes
}

// IMPORTANT: always unbind to prevent crashes from dangling pointers
void AMyActor::EndPlay(const EEndPlayReason::Type Reason)
{
    if (NiagaraComp)
    {
        NiagaraComp->OnSystemFinished.RemoveDynamic(this, &AMyActor::OnVFXFinished);
    }
    Super::EndPlay(Reason);
}
```

---

## 4. Blueprint Integration

- **Spawn Niagara System at Location** node → one-shot world-space effect.
- **Spawn Niagara System Attached** node → attaches to a component/socket.
- Both return a `Niagara Component` reference.
- Use **Set Niagara Variable (Float/Bool/Vector/Color/Actor)** nodes to set User params.
- **Activate** / **Deactivate** / **Reset** nodes match the C++ API above.

---

## 5. Pooling & Performance

| Method | Use Case |
|---|---|
| `ENCPoolMethod::None` | Short effects on slower code paths; no pooling |
| `ENCPoolMethod::AutoRelease` | One-shot effects (explosions, impacts). Auto-returned to pool on finish. |
| `ENCPoolMethod::ManualRelease` | Sustained effects (beams, trails). Call `ReleaseToPool()` yourself. |

```cpp
// Manual release when done:
NiagaraComp->ReleaseToPool();

// Prime pool before a critical gameplay moment:
FNiagaraWorldManager* NM = FNiagaraWorldManager::Get(GetWorld());
if (NM) { NM->GetComponentPool()->PrimePool(ExplosionSystem, GetWorld()); }
```

**Pool CVars:**
- `FX.NiagaraComponentPool.Enable 1` — global on/off
- `FX.NiagaraComponentPool.KillUnusedTime` — seconds before idle components are culled

---

## 6. Simulation Spaces

| Space | Best For |
|---|---|
| **World Space** | Effects that should stay in place after the actor moves (explosions, sparks) |
| **Local Space** | Effects attached to moving actors (trails, auras). Particles inherit parent transform. |

Set per-emitter in **Emitter Properties → Local Space** checkbox.

---

## 7. Events & Event Handlers

Niagara's internal event system lets emitters communicate:

1. **Generate Location Event** in Emitter A — produces per-particle data (position, velocity, etc.) on a named event channel.
2. **Event Handler** group in Emitter B → set **Source** to Emitter A's event, **Execution Mode** (every particle, spawn particle, etc.).
3. Each received event can spawn new particles or modify existing ones.

To trigger a burst from C++, use a `User.bool` parameter that the spawn script polls:
```cpp
NiagaraComp->SetVariableBool(FName("User.bTriggerBurst"), true);
// There is no C++ API to inject raw Niagara events directly.
```

---

## 8. Custom Modules (HLSL / Scratch Pad)

- Open any module by **double-clicking** it in the emitter stack.
- The **Scratch Pad** (Window → Scratch Pad) lets you prototype modules inline before promoting them to assets.
- Modules use the **Parameter Map**: read with `Map.Get`, write with `Map.Set`.
- Use the **CustomHLSL** node to write raw HLSL inside the graph.
- Common pattern:
  1. `InputMap.Get` → retrieve particle attribute (e.g., `Particles.Position`)
  2. Math / HLSL
  3. `OutputMap.Set` → write back modified value

---

## 9. Data Channels (UE 5.3+)

Niagara Data Channels allow Niagara systems to share data with each other and with the game world outside of the normal event system.

- Define a **NiagaraDataChannelAsset** with a layout of named parameters.
- **Writer** (C++ or BP): call `UNiagaraDataChannelLibrary::WriteToNDC()` to push data.
- **Reader** (inside Niagara): use the **Read Data Channel** module to consume it.
- Useful for: gameplay-driven particle spawning, inter-system communication without tight coupling.

Docs: https://dev.epicgames.com/documentation/en-us/unreal-engine/niagara-data-channels-overview

---

## 10. Niagara Fluids (Plugin — Experimental)

- Enable via **Plugins → Niagara Fluids**.
- GPU-only. High performance cost — profile before shipping.
- Provides Gas (volumetric smoke/fire) and Liquid simulations.
- Recommended only for **hero effects** where visual impact justifies the GPU budget.
- Parameters are set the same way as regular Niagara User params.

---

## 11. Debugging & Optimization

| Tool | How to Access |
|---|---|
| Niagara Debugger | Window → Niagara Debugger (in editor or PIE) |
| Stat Niagara | Console: `stat Niagara` |
| GPU particle budget | Console: `FX.MaxCPUParticlesPerEmitter`, `FX.MaxGPUParticlesSpawnPerFrame` |
| Niagara Perf Baseline | Editor → Tools → Niagara → Performance Baseline |

**Common pitfalls:**
- Forgetting `Niagara` and `NiagaraCore` in `Build.cs` → linker errors.
- Not null-checking `SpawnSystemAtLocation` return → crash on dedicated server.
- Using `ENCPoolMethod::AutoRelease` on a looping system → system never released.
- Binding `OnSystemFinished` without unbinding in `EndPlay` → use-after-free crash.
- Setting parameters **after** the first simulation tick when using GPU emitters — set them before `Activate()` or immediately after spawn.

---

## 12. Cascade → Niagara Migration

- Use the **Cascade to Niagara Converter Plugin** (Plugins → Cascade to Niagara Converter).
- Right-click a Particle System asset → **Convert to Niagara System**.
- Not all Cascade features convert 1:1; review the output and test in-editor.
- `UParticleSystemComponent` (Cascade) is a separate class from `UNiagaraComponent` — they are not interchangeable.

---

## Reference Links (UE 5.7)

- Overview: https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-niagara-effects-for-unreal-engine
- Creating VFX: https://dev.epicgames.com/documentation/en-us/unreal-engine/creating-visual-effects-in-niagara-for-unreal-engine
- Tutorials: https://dev.epicgames.com/documentation/en-us/unreal-engine/tutorials-for-niagara-effects-in-unreal-engine
- Editor UI Reference: https://dev.epicgames.com/documentation/en-us/unreal-engine/editor-ui-reference-for-niagara-effects-in-unreal-engine
- Controlling Systems: https://dev.epicgames.com/documentation/en-us/unreal-engine/controlling-your-niagara-systems
- Data Channels: https://dev.epicgames.com/documentation/en-us/unreal-engine/niagara-data-channels-overview
- Blueprint API: https://dev.epicgames.com/documentation/en-us/unreal-engine/BlueprintAPI/Niagara
- C++ API (UNiagaraFunctionLibrary): https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/Niagara/UNiagaraFunctionLibrary
