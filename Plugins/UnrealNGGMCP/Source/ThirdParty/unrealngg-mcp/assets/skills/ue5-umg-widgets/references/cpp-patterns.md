# UE5 UMG — C++ Patterns Reference

## Table of Contents
1. Build.cs Setup
2. C++ Base Widget Class
3. BindWidget & BindWidgetOptional
4. Widget Lifecycle
5. Creating & Spawning Widgets
6. Delegates for UI Updates
7. ListView / TileView Pattern
8. Animations from C++

---

## 1. Build.cs Setup

```csharp
PublicDependencyModuleNames.AddRange(new string[]
{
    "Core", "CoreUObject", "Engine", "InputCore",
    "UMG",       // Required for UUserWidget
    "Slate",     // Required for Slate types
    "SlateCore"  // Required for Slate core types
});
```

---

## 2. C++ Base Widget Class

Pattern: C++ defines logic and widget bindings; Blueprint subclass provides layout.

```cpp
// MyHealthWidget.h
#pragma once
#include "Blueprint/UserWidget.h"
#include "MyHealthWidget.generated.h"

class UProgressBar;
class UTextBlock;

UCLASS(Abstract)
class MYGAME_API UMyHealthWidget : public UUserWidget
{
    GENERATED_BODY()

protected:
    // These MUST match the exact widget name in the Blueprint
    UPROPERTY(BlueprintReadOnly, meta=(BindWidget))
    TObjectPtr<UProgressBar> HealthBar;

    UPROPERTY(BlueprintReadOnly, meta=(BindWidget))
    TObjectPtr<UTextBlock> HealthLabel;

    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional))
    TObjectPtr<UTextBlock> ShieldLabel;   // No compile error if absent in BP

    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

public:
    UFUNCTION(BlueprintCallable, Category="Health")
    void UpdateHealth(float CurrentHP, float MaxHP);
};
```

```cpp
// MyHealthWidget.cpp
#include "MyHealthWidget.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"

void UMyHealthWidget::NativeConstruct()
{
    Super::NativeConstruct();
    // BindWidget pointers are valid here (NOT in constructor)
    UpdateHealth(100.f, 100.f);  // Initialize display
}

void UMyHealthWidget::NativeDestruct()
{
    Super::NativeDestruct();
    // Unbind any delegates here
}

void UMyHealthWidget::UpdateHealth(float CurrentHP, float MaxHP)
{
    if (HealthBar)
        HealthBar->SetPercent(MaxHP > 0.f ? CurrentHP / MaxHP : 0.f);

    if (HealthLabel)
        HealthLabel->SetText(FText::Format(
            NSLOCTEXT("UI", "HealthFormat", "{0} / {1}"),
            FText::AsNumber(FMath::RoundToInt(CurrentHP)),
            FText::AsNumber(FMath::RoundToInt(MaxHP))
        ));
}
```

---

## 3. BindWidget & BindWidgetOptional

| Macro | Behavior |
|---|---|
| `meta=(BindWidget)` | Widget MUST exist in BP with exact name + type. Compile error if missing. |
| `meta=(BindWidgetOptional)` | Widget MAY exist in BP. No error if absent — check for nullptr before use. |
| `meta=(BindWidgetAnim)` | Binds to a UWidgetAnimation in the BP by name. |

**Important:** BindWidget pointers are **null in the C++ constructor**. Always use `NativeConstruct()` for initialization.

**Access from Blueprint:** Add `BlueprintReadOnly` or `BlueprintReadWrite` to make the widget accessible in the Blueprint graph:
```cpp
UPROPERTY(BlueprintReadOnly, meta=(BindWidget))
TObjectPtr<UTextBlock> ScoreLabel;
```

---

## 4. Widget Lifecycle

```
Constructor          — DO NOT access BindWidget pointers here (they are null)
PreConstruct(bool)   — Runs in editor preview; use for default state setup
NativeConstruct      — Widget added to viewport; bind delegates, initialize state
NativeTick(float)    — Every frame; avoid heavy logic; prefer event-driven updates
NativeDestruct       — Widget removed; unbind delegates, cleanup
```

**NativeOnMouseEnter / NativeOnMouseLeave** — Override for hover behavior in C++.

---

## 5. Creating & Spawning Widgets

### From C++ (PlayerController recommended)
```cpp
// Header
UPROPERTY(EditDefaultsOnly, Category="UI")
TSubclassOf<UMyHealthWidget> HealthWidgetClass;

UPROPERTY()
TObjectPtr<UMyHealthWidget> HealthWidget;

// BeginPlay
void AMyPlayerController::BeginPlay()
{
    Super::BeginPlay();

    if (HealthWidgetClass)
    {
        HealthWidget = CreateWidget<UMyHealthWidget>(this, HealthWidgetClass);
        if (HealthWidget)
            HealthWidget->AddToViewport(0); // ZOrder: higher = on top
    }
}

// Cleanup
void AMyPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (HealthWidget && HealthWidget->IsInViewport())
        HealthWidget->RemoveFromParent();

    Super::EndPlay(EndPlayReason);
}
```

### AddToViewport vs AddToPlayerScreen
- `AddToViewport()` — Added to global viewport; all players see it (splitscreen: careful)
- `AddToPlayerScreen()` — Added to the specific player's screen (splitscreen-safe)

---

## 6. Delegates for UI Updates

### Declare in Character/GameState
```cpp
// Character.h
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnHealthChanged, float, Current, float, Max);

UPROPERTY(BlueprintAssignable, Category="Events")
FOnHealthChanged OnHealthChanged;

// Call when health changes:
OnHealthChanged.Broadcast(CurrentHP, MaxHP);
```

### Bind in Widget
```cpp
void UMyHealthWidget::NativeConstruct()
{
    Super::NativeConstruct();

    if (APawn* Pawn = GetOwningPlayerPawn())
    {
        if (AMyCharacter* Char = Cast<AMyCharacter>(Pawn))
        {
            Char->OnHealthChanged.AddDynamic(this, &UMyHealthWidget::UpdateHealth);
        }
    }
}

void UMyHealthWidget::NativeDestruct()
{
    if (APawn* Pawn = GetOwningPlayerPawn())
    {
        if (AMyCharacter* Char = Cast<AMyCharacter>(Pawn))
        {
            Char->OnHealthChanged.RemoveDynamic(this, &UMyHealthWidget::UpdateHealth);
        }
    }
    Super::NativeDestruct();
}
```

---

## 7. ListView / TileView Pattern

Use ListView/TileView instead of dynamically adding child widgets to a ScrollBox.
This recycles entry widgets instead of creating/destroying them.

### Entry Widget (C++)
```cpp
// ListEntryWidget.h
#pragma once
#include "Blueprint/UserWidget.h"
#include "Blueprint/IUserObjectListEntry.h"
#include "ListEntryWidget.generated.h"

UCLASS()
class UListEntryWidget : public UUserWidget, public IUserObjectListEntry
{
    GENERATED_BODY()

protected:
    UPROPERTY(BlueprintReadOnly, meta=(BindWidget))
    TObjectPtr<UTextBlock> ItemName;

    UPROPERTY(BlueprintReadOnly, meta=(BindWidget))
    TObjectPtr<UImage> ItemIcon;

    // Called by ListView when this entry is assigned data
    virtual void NativeOnListItemObjectSet(UObject* ListItemObject) override;
};
```

```cpp
// ListEntryWidget.cpp
void UListEntryWidget::NativeOnListItemObjectSet(UObject* ListItemObject)
{
    if (UMyItemData* ItemData = Cast<UMyItemData>(ListItemObject))
    {
        if (ItemName) ItemName->SetText(ItemData->GetDisplayName());
        if (ItemIcon) ItemIcon->SetBrushFromTexture(ItemData->GetIcon());
    }
}
```

### Populate ListView
```cpp
// In parent widget NativeConstruct:
for (UMyItemData* Item : AllItems)
{
    MyListView->AddItem(Item);  // MyListView is a UListView* bound with BindWidget
}
```

---

## 8. Widget Animations from C++

```cpp
// Header — bind animation by name
UPROPERTY(meta=(BindWidgetAnim), Transient)
TObjectPtr<UWidgetAnimation> FadeInAnim;

// Play animation
PlayAnimation(FadeInAnim);
PlayAnimation(FadeInAnim, 0.f, 1, EUMGSequencePlayMode::Forward, 1.0f);

// Play in reverse
PlayAnimation(FadeInAnim, 0.f, 1, EUMGSequencePlayMode::Reverse);

// Check if playing
IsAnimationPlaying(FadeInAnim);

// Stop
StopAnimation(FadeInAnim);
```

Note: `Transient` is required alongside `BindWidgetAnim` to prevent serialization issues.
