# Alternate Mouser Part Numbers Support

## Overview

Added support for tracking and auditing **alternate Mouser part numbers** in the KiCad MCP server. This allows you to:

- Store multiple Mouser sources per component (primary + alternates)
- Audit which components have multiple Mouser links
- Detect stale or conflicting part number references
- Fall back to alternate sources when the primary is out of stock

## New Functions

### In `kicad_mouser_tool.py`

#### `find_all_mouser_urls(properties: dict[str, str]) -> dict[str, str]`
Finds all Mouser product links in a component's schematic properties, returning them indexed by field name. Prefers `/ProductDetail/` links (which identify a specific part) over datasheet PDFs.

```python
from kicad_mouser_tool import find_all_mouser_urls

urls = find_all_mouser_urls(component_properties)
# Returns: {"Mouser": "https://...", "Mouser_Alt": "https://..."}
```

#### `list_component_mouser_urls(project_path, reference: str) -> dict[str, Any]`
Lists all available Mouser URLs for a single component, with metadata indicating which are primary vs. alternates.

```python
from kicad_mouser_tool import list_component_mouser_urls

result = list_component_mouser_urls(".", "U1")
# Returns:
# {
#   "reference": "U1",
#   "value": "LM7805_TO220",
#   "footprint": "Package_TO_SOT_THT:TO-220-3_Vertical",
#   "primary_url": "https://www.mouser.com/...",
#   "alternate_count": 0,
#   "urls": [
#     {
#       "field_name": "Datasheet",
#       "url": "https://www.mouser.com/...",
#       "is_primary": True,
#       "mpn_hint": "MC7805ACTG"
#     }
#   ]
# }
```

#### `bulk_list_component_mouser_urls(project_path, references: list[str] | None) -> dict[str, Any]`
Bulk audit of Mouser URLs across many components. Defaults to one representative per unique part; pass specific `references` to check a subset.

Returns summary statistics plus details on which parts have alternates and which lack Mouser links entirely.

```python
from kicad_mouser_tool import bulk_list_component_mouser_urls

result = bulk_list_component_mouser_urls(".")
# Returns:
# {
#   "checked_count": 44,
#   "with_mouser_link_count": 44,
#   "with_alternates_count": 2,
#   "no_mouser_link_count": 0,
#   "results": [...],
#   "no_mouser_link": [...]
# }
```

## New MCP Tools

### `list_kicad_component_mouser_urls`
Get all available Mouser URLs (primary and alternates) for a single component by reference designator. Useful for spotting components with multiple Mouser sources or stale/incorrect links.

**Inputs:**
- `project_path` (required): KiCad project directory or .kicad_pcb/.kicad_sch path
- `reference` (required): Component reference designator (e.g., "U1", "R47")

**Output:** Same as `list_component_mouser_urls()` above

### `bulk_list_kicad_component_mouser_urls`
List all Mouser URLs for many schematic parts in one call — an audit of which parts have alternates, which are missing Mouser links, and which fields each link came from.

**Inputs:**
- `project_path` (required): KiCad project directory or .kicad_pcb/.kicad_sch path
- `references` (optional): Specific reference designators to check instead of every unique part

**Output:** Same as `bulk_list_component_mouser_urls()` above

## Use Cases

### 1. Detect Stale/Duplicate Links
Some components might accidentally have the same Mouser link in multiple fields (copy-paste errors). Use the bulk audit to spot these:

```python
result = bulk_list_component_mouser_urls(".")
for r in result['results']:
    if r['alternate_count'] > 0:
        print(f"{r['reference']}: has {r['alternate_count']} alternate(s)")
        for url_info in r['urls']:
            print(f"  - {url_info['field_name']}: {url_info['mpn_hint']}")
```

### 2. Find Components Needing Mouser Links
Audit parts with missing links before running a stock/lifecycle report:

```python
result = bulk_list_component_mouser_urls(".")
for missing in result['no_mouser_link']:
    print(f"{missing['reference']}: {missing['value']} — no Mouser link")
```

### 3. Support Fallback Sources
When the primary Mouser link is out of stock or obsolete, the `urls` list shows alternates by field name, so you can decide which to promote to primary.

## Integration with Existing Tools

The existing `find_mouser_url()` function now internally uses `find_all_mouser_urls()`, so it continues to work for single-URL lookups. All existing tools (`lookup_mouser_part`, `bulk_lookup_mouser_parts`, `generate_mouser_stock_report`, etc.) automatically benefit from the improved URL discovery.

## Schema Definition

### Component Mouser URL Info
```json
{
  "field_name": "string",          // Property name (e.g., "Mouser", "Mouser_Alt")
  "url": "string",                 // Full Mouser URL
  "is_primary": "boolean",         // True if this is the primary URL
  "mpn_hint": "string or null"    // MPN extracted from URL (e.g., "MC7805ACTG")
}
```

### Bulk Result Summary
```json
{
  "checked_count": "integer",           // Total references checked
  "with_mouser_link_count": "integer",  // Components with at least one Mouser link
  "with_alternates_count": "integer",   // Components with multiple Mouser links
  "no_mouser_link_count": "integer",    // Components with no Mouser link found
  "results": "[{...}]",                 // Full details per component
  "no_mouser_link": "[{...}]"          // Components missing links
}
```

## Property Field Name Conventions

The tools recognize these common field names for Mouser links:
- `Mouser` — primary Mouser product link
- `Mouser_Alt` — alternate Mouser product link
- `Mouser Part Number` — Mouser product link (alt naming)
- `Mouser Part Number Alt` — alternate Mouser product link (alt naming)
- `Datasheet` — if it points to mouser.com (fallback)

You can add any custom field name to a schematic symbol; if it contains a mouser.com URL, it will be discovered and returned in the audit.

## Example: Auditing and Fixing

```python
from kicad_mouser_tool import bulk_list_component_mouser_urls, set_schematic_property
from kicad_pcb_tool import set_schematic_property

# Step 1: Audit for problems
audit = bulk_list_component_mouser_urls(".")
print(f"Components with alternates: {audit['with_alternates_count']}")
print(f"Missing links: {audit['no_mouser_link_count']}")

# Step 2: Review and decide which alternates to promote
for r in audit['results']:
    if r['alternate_count'] > 0:
        print(f"\n{r['reference']}: {r['value']}")
        for url in r['urls']:
            marker = "PRIMARY" if url['is_primary'] else "alt"
            print(f"  [{marker}] {url['field_name']}: {url['mpn_hint']}")

# Step 3: If needed, update primary links (see kicad_pcb_tool for set_schematic_property)
# Example: set_schematic_property(".", "R47", "Mouser", new_url, write=True)
```
