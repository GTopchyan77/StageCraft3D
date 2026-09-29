---
type: doc
tags: [commands, conversion, ru]
---

# Команды (RU) — шпаргалка

Только команды и фразы запуска. Метод — [[BP_TO_CPP_PLAYBOOK]], меню возможностей — [[Capabilities]].

## Тул bp-extract (PowerShell, из корня репозитория)

```powershell
# Извлечь систему / всё / один ассет в JSON (_extracted/) — нужен ОТКРЫТЫЙ редактор
py Tools\bp-extract --from-vault --system Data
py Tools\bp-extract --from-vault
py Tools\bp-extract --asset Content/Blueprints/<...>.uasset
py Tools\bp-extract --from-vault --dry-run        # показать цели, без обращения к мосту

# Порядок конвертации (офлайн) -> Conversion Order.md + _extracted/_order.json
py Tools\bp-extract order

# Бриф по BP (офлайн) -> _briefs/
py Tools\bp-extract brief SandboxCharacter_CMC
py Tools\bp-extract brief                          # все ассеты

# Кодген C++ типов/пустых родителей -> Source/GameAnimationSample/<System>/
py Tools\bp-extract codegen --system Data
py Tools\bp-extract codegen --kinds Enum,Struct    # фильтр по виду
py Tools\bp-extract codegen --system Data --dry-run
```

## Сборка / редактор

```powershell
# Собрать editor-таргет (редактор должен быть ЗАКРЫТ; в MCP — ue5_kill_editor)
& "C:\UE_5.7\Engine\Build\BatchFiles\Build.bat" GameAnimationSampleEditor Win64 Development `
  -project="D:\UnrealEngineProjects\GameAnimationSample\GameAnimationSample.uproject" -waitmutex

# Регенерация .sln (после добавления/удаления файлов или модулей)
& "C:\UE_5.7\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" -projectfiles `
  -project="D:\UnrealEngineProjects\GameAnimationSample\GameAnimationSample.uproject" -game -rocket -progress
```

## Node-тулинг моста

```bash
npm --prefix Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp test
```

## Фразы для запуска конвертации (писать мне)

- `сконвертируй E_Gait` — один ассет, полный цикл
- `сконвертируй тир 0` / `сконвертируй все енумы Data` — пачкой, по зависимостям
- `начни конвертацию по Conversion Order` — следующий незавершённый
- модификаторы: `только скаффолд` · `весь type-pipeline`
