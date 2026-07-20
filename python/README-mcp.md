# KiCad MCP for Cline

This workspace now includes a local MCP server that lets Cline inspect the KiCad project directly.

## Files
- kicad_pcb_tool.py - lightweight KiCad PCB/netlist parser
- kicad_mcp_server.py - stdio MCP server exposing tools
- cline-mcp-config.json - Cline MCP config example

## Tools exposed
- inspect_kicad_project
- list_kicad_components
- get_kicad_component
- get_kicad_component_connections
- get_kicad_net
- list_kicad_nets
- find_kicad_components_by_net
- find_kicad_components_by_pin_connection
- search_kicad_component
- get_kicad_hierarchical_group
- list_kicad_sibling_instances
- list_kicad_hierarchical_templates
- diff_kicad_layout_template
- apply_kicad_layout_template
- apply_kicad_layout_changes
- move_kicad_group
- list_kicad_groups
- create_kicad_group
- delete_kicad_group

### Live tools (require KiCad running with the IPC API enabled)
- get_kicad_ipc_status
- get_kicad_live_bounding_box
- find_kicad_live_layout_collisions
- highlight_kicad_live_components
- clear_kicad_live_highlight
- get_kicad_live_selection

## Usage
1. Install the Python extension and Pylance in VS Code if they are not already present.
2. Install Python dependencies if needed:
   - pip install -r requirements-mcp.txt
3. In Cline, add the server from cline-mcp-config.json or paste the equivalent config.
4. Ask Cline to inspect the board, for example:
   - "Inspect the KiCad project in this workspace"
   - "List the components on the PCB"
   - "Show me component R1"
   - "Show me nets connected to component U1"
   - "Provide details for net /MainControler/CLK"

## Reorganizing a repeated sub-circuit's layout

This project has several hierarchical sheets that get stamped out multiple times
on the board (relay outputs, thermocouple inputs, current-sense channels,
regulators, etc). When one instance has a known-good layout (often marked
`locked` in the PCB) and the others need to match it, use the layout-template
tools instead of hand-computing offsets:

0. `list_kicad_hierarchical_templates()` - **start here.** One call, no
   reference needed, returns every repeated schematic sheet on the board with
   each instance's member references and whether it's the fully-locked
   reference layout. This replaces grepping/reading the file to figure out
   what belongs together and which instance to copy from - do this before
   anything else on a "make these repeated sub-circuits consistent" task.
1. `get_kicad_hierarchical_group(reference=<anchor of the good layout>)` - lists
   every footprint that belongs with that instance, matched via the schematic
   `path` rather than board proximity. This avoids accidentally grabbing an
   unrelated component that just happens to sit nearby on the board. Returns a
   trimmed view by default (position/uuid/locked/footprint only, no Datasheet
   URLs or Mouser part numbers); pass `verbose: true` only if you actually need
   full KiCad properties.
2. `list_kicad_sibling_instances(reference=<same anchor>)` - lists every other
   instance of the same schematic sheet (the other channels), with each one's
   own anchor reference and position.
3. `diff_kicad_layout_template(template_reference=<good anchor>, target_reference=<channel to fix>)`
   - dry-run preview of where every matching component in the target channel
   should move to reproduce the template's relative layout. If the target
   channel's anchor has a different rotation than the template's, the whole
   offset pattern is rotated to match (so components don't end up mirrored or
   on the wrong side).
4. `apply_kicad_layout_template(template_reference=..., target_references=[...], write=false)`
   to preview across every target channel at once, then call again with
   `write=true` to actually save. Always dry-run first.

Matching between template and target is done by the footprint's schematic
symbol identity, not by reference name or physical distance - two components on
different schematic sheets that merely sit close together on the board will
never be confused for one another.

## Relocating an already-correct cluster (no template needed)

If there's no separate known-good layout to copy from - you just need to move
a whole group somewhere else on the board, or nudge it a few mm to clear a
routing conflict - use `move_kicad_group(reference=<any member>, dx=, dy=)` (or
`to: {x, y}` for an absolute anchor position, `drotation` to rotate the whole
group in place). It moves every member of that component's hierarchical group
together, preserving their layout relative to each other. Defaults to
write=false; dry-run first.

## Creating/managing PCB groups (the Ctrl+G GUI construct)

Don't confuse this with the *hierarchical* group above - `get_kicad_hierarchical_group`
finds a sub-circuit's members via the schematic path, purely for computing layout
diffs; it's a query, not a board-file construct. A **PCB group** is the actual
`(group "name" (uuid ..) (members ..))` block KiCad writes to the board file, which
is what makes a cluster of footprints select/move together as one unit in the GUI.
The two are independent - a hierarchical group's members aren't grouped in the PCB
sense until you explicitly `create_kicad_group` them.

Typical flow for "make each instance of this repeated sub-circuit its own group":
1. `list_kicad_hierarchical_templates()` or `get_kicad_hierarchical_group(reference=...)`
   to find each instance's member references.
2. `list_kicad_groups()` to check nothing you're about to group is already in another
   group (KiCad groups don't nest/overlap).
3. `create_kicad_group(name=..., references=[...], write=false)` per instance to preview,
   then `write=true` to save. Raises if a reference isn't found on the board, or if it's
   already a member of an existing group.
4. `delete_kicad_group(name=... or group_uuid=..., write=false/true)` to undo - only the
   grouping is removed, member footprints are untouched. Pass `group_uuid` when several
   groups share a name (KiCad's own GUI-created groups are usually named `""`).

Like the layout tools, `create_kicad_group`/`delete_kicad_group` do targeted text
surgery keyed off footprint/group uuids rather than a full parse-mutate-reserialize -
safe on a multi-megabyte board file, and preserves the file's existing CRLF/LF line
endings so the diff stays limited to the lines that actually changed.

## Making one-off edits without a lookup round trip

`apply_kicad_layout_changes` and the `changes` it takes from
`diff_kicad_layout_template`/`move_kicad_group` accept either `uuid` or
`reference` per change - if you already know a component's designator, you can
write `{"reference": "R33", "new_position": {...}}` directly instead of first
calling `get_kicad_component` to look up its uuid.

## Notes
The parser reads the KiCad PCB and netlist files directly, so it does not require a full KiCad runtime.
The board file is parsed once and cached in-process (invalidated automatically
if the file changes on disk, by anyone or anything - not just this tool), so
repeated calls within one session don't re-parse a multi-megabyte board file
each time.

## Live tools (KiCad IPC API)

Most tools above (`kicad_pcb_tool.py`) parse `kiln.kicad_pcb` directly and never need
KiCad to be running. A second, smaller set of tools (`kicad_ipc_tool.py`) instead talks
to a *running* KiCad instance over its IPC API, via the `kicad-python` package - for the
handful of things only a live KiCad session can answer:

- **get_kicad_live_bounding_box** - KiCad's own computed bounding box for a footprint
  (real pad/silkscreen/courtyard geometry and exact rotation), instead of
  `estimate_kicad_footprint_radius`'s circle-from-footprint-name heuristic.
- **find_kicad_live_layout_collisions** - the same collision check as
  `find_kicad_layout_collisions`, but built on real bounding boxes instead of radii - more
  accurate for oblong parts (connectors, electrolytic cans, relays).
- **highlight_kicad_live_components** / **clear_kicad_live_highlight** - select components
  in the live PCB editor so a human can see what an agent is about to change before any
  write happens. Purely visual - writes still go through the `write=true` file-based tools.
- **get_kicad_live_selection** - read back whatever's currently selected in the GUI, so a
  person can point at a component by hand instead of typing its reference.
- **get_kicad_ipc_status** - checks connectivity; call this first if any live tool fails,
  to tell "KiCad isn't reachable" apart from "component not found".

Setup:
1. `pip install -r requirements-mcp.txt` (installs `kicad-python`; the server still starts
   fine without it, it just won't register the live tools).
2. In KiCad: **Preferences > Plugins > Enable IPC API**.
3. Open `kiln.kicad_pcb` in KiCad and keep it open while using the live tools.

There's no `run DRC and get violations` tool - `kicad-python` (as of 0.7.1) exposes DRC
*rule configuration* but not a way to trigger a check and read back the violation list, so
that capability doesn't exist yet on this stack.
