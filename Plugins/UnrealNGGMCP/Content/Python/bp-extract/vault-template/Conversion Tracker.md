---
type: tracker
tags: [moc]
---

# Conversion Tracker

Status values: `todo` · `in-progress` · `done` · `skip`. Edit the `status` property on each
asset note in `Assets/`.

## By system (progress)

```dataview
TABLE WITHOUT ID
  system AS System,
  length(rows) AS Total,
  length(filter(rows, (r) => r.status = "done")) AS Done,
  length(filter(rows, (r) => r.status = "in-progress")) AS "In Progress",
  length(filter(rows, (r) => r.status = "todo")) AS Todo,
  length(filter(rows, (r) => r.status = "skip")) AS Skip
FROM "Assets"
WHERE type = "asset"
GROUP BY system
SORT system ASC
```

## All assets

```dataview
TABLE WITHOUT ID
  file.link AS Asset,
  system AS System,
  asset_kind AS Kind,
  status AS Status,
  cpp_target AS "C++ Target"
FROM "Assets"
WHERE type = "asset"
SORT system ASC, status ASC, file.name ASC
```

## Not yet started

```dataview
LIST
FROM "Assets"
WHERE type = "asset" AND status = "todo"
SORT system ASC, file.name ASC
```
