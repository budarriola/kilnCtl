#!/usr/bin/env python3
"""Minimal stdio MCP server for inspecting KiCad projects from Cline/VS Code."""

from __future__ import annotations

import argparse
import json
import sys
import time
import traceback
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, cast

LOG_PATH = Path(__file__).resolve().with_name("kicad_mcp_server.log")
SUPPORTED_PROTOCOL_VERSIONS = {"2024-11-05", "2025-03-26"}


def log_message(message: str) -> None:
    LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    line = f"[{timestamp}] {message}"
    try:
        with LOG_PATH.open("a", encoding="utf-8") as handle:
            handle.write(line + "\n")
    except Exception:
        pass


try:
    from kicad_pcb_tool import (
    align_component_pin,
    align_components_to_anchor,
    apply_flip_template,
    apply_layout_changes,
    apply_layout_template,
    apply_property_position_changes,
    apply_property_position_template,
    classify_group_by_anchor_pin,
    create_group,
    delete_group,
    diff_flip_template,
    diff_layout_by_role,
    diff_layout_template,
    diff_property_position_template,
    estimate_footprint_radius,
    find_components_by_net,
    find_components_by_pin_connection,
    find_layout_collisions,
    get_component,
    get_component_connections,
    get_footprint_pads,
    get_hierarchical_group,
    get_net,
    get_pin_position,
    get_property_position,
    inspect_project,
    list_components,
    list_groups,
    list_hierarchical_templates,
    list_nets,
    list_sibling_instances,
    match_group_members_by_role,
    move_group,
    nudge_to_clear,
    pin_distance,
    search_component_by_reference,
    suggest_component_placement,
    )
except Exception as exc:  # pragma: no cover - import safety
    log_message(f"Failed to import KiCad parser module: {exc}")
    traceback.print_exc(file=sys.stderr)
    raise


try:
    from kicad_ipc_tool import (
        clear_live_highlight,
        find_live_layout_collisions,
        get_ipc_status,
        get_live_bounding_box,
        get_live_selection,
        highlight_live_components,
    )
    _IPC_AVAILABLE = True
except Exception as exc:  # pragma: no cover - optional dependency
    log_message(f"KiCad IPC tools unavailable (is kicad-python installed? {exc})")
    _IPC_AVAILABLE = False


log_message("KiCad MCP server module imported successfully")


class KiCadMcpServer:
    def __init__(self) -> None:
        self.tools: dict[str, dict[str, Any]] = {
            "inspect_kicad_project": {
                "description": "Inspect a KiCad project directory or board file and return a summary.",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {
                            "type": "string",
                            "description": "Path to a KiCad project directory, .kicad_pcb file, or .kicad_pro file.",
                        }
                    },
                    "required": ["project_path"],
                },
                "handler": self._tool_inspect_project,
            },
            "list_kicad_components": {
                "description": "List components from a KiCad PCB file.",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "limit": {"type": "integer", "default": 50},
                    },
                    "required": ["project_path"],
                },
                "handler": self._tool_list_components,
            },
            "get_kicad_component": {
                "description": "Get a specific component by its reference/designator.",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "reference": {"type": "string"},
                    },
                    "required": ["project_path", "reference"],
                },
                "handler": self._tool_get_component,
            },
            "get_kicad_component_connections": {
                "description": "Get the net connections for a specific component reference.",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "reference": {"type": "string"},
                    },
                    "required": ["project_path", "reference"],
                },
                "handler": self._tool_get_component_connections,
            },
            "get_kicad_net": {
                "description": "Get details for a specific net name from the KiCad netlist.",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "net_name": {"type": "string"},
                    },
                    "required": ["project_path", "net_name"],
                },
                "handler": self._tool_get_net,
            },
            "find_kicad_components_by_net": {
                "description": "Find all components connected to a specific net.",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "net_name": {"type": "string"},
                    },
                    "required": ["project_path", "net_name"],
                },
                "handler": self._tool_find_components_by_net,
            },
            "find_kicad_components_by_pin_connection": {
                "description": "Find components that connect to a specific pin on a given component reference.",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "reference": {"type": "string"},
                        "pin": {"type": "string"},
                    },
                    "required": ["project_path", "reference", "pin"],
                },
                "handler": self._tool_find_components_by_pin_connection,
            },
            "suggest_kicad_component_placement": {
                "description": "Suggest component placement positions based on connection grouping and rotation hints.",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "reference": {"type": "string"},
                        "group_size": {"type": "integer", "default": 4},
                        "spacing": {"type": "number", "default": 10.0},
                        "rotation": {"type": "number", "default": 0.0},
                    },
                    "required": ["project_path", "reference"],
                },
                "handler": self._tool_suggest_component_placement,
            },
            "list_kicad_nets": {
                "description": "List nets from the KiCad netlist.",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                    },
                    "required": ["project_path"],
                },
                "handler": self._tool_list_nets,
            },
            "search_kicad_component": {
                "description": "Search for a component by reference designator and return its line numbers in the PCB file. Use this to efficiently locate component sections without reading the entire file.",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "reference": {"type": "string"},
                    },
                    "required": ["project_path", "reference"],
                },
                "handler": self._tool_search_component,
            },
            "get_kicad_hierarchical_group": {
                "description": (
                    "Given a component reference, return every other footprint that belongs to the same "
                    "hierarchical-sheet instance (e.g. all the parts of one relay channel, or one thermocouple "
                    "channel), matched via the schematic path rather than board position. Use this before trying "
                    "to reorganize a repeated sub-circuit's layout, to find its true member list without "
                    "guessing from proximity."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "reference": {"type": "string"},
                        "verbose": {"type": "boolean", "default": False, "description": "Include full KiCad properties (Datasheet, Mouser part numbers, Sim.* fields) per component. Leave false unless you actually need them - it's the largest cost in the response."},
                    },
                    "required": ["project_path", "reference"],
                },
                "handler": self._tool_get_hierarchical_group,
            },
            "list_kicad_sibling_instances": {
                "description": (
                    "Given a component reference, find every other instance of the same hierarchical schematic "
                    "sheet (e.g. given one relay channel or one thermocouple channel, list the other channels "
                    "stamped from the same template page), with each sibling's own anchor reference/position."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "reference": {"type": "string"},
                    },
                    "required": ["project_path", "reference"],
                },
                "handler": self._tool_list_sibling_instances,
            },
            "diff_kicad_layout_template": {
                "description": (
                    "Dry-run: compute where every sibling of target_reference's hierarchical group should move "
                    "to match the relative layout (offsets AND rotations) of template_reference's group. "
                    "Rotates the whole offset pattern to account for a difference in the two anchors' own "
                    "rotation. Returns a list of changes; nothing is written. Use this to preview before calling "
                    "apply_kicad_layout_template."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "template_reference": {"type": "string", "description": "Reference of the anchor component in the known-good template group (e.g. a locked relay or chip)."},
                        "target_reference": {"type": "string", "description": "Reference of the anchor component in the group to be repositioned."},
                    },
                    "required": ["project_path", "template_reference", "target_reference"],
                },
                "handler": self._tool_diff_layout_template,
            },
            "apply_kicad_layout_template": {
                "description": (
                    "Reposition every sibling group's components to match template_reference's layout, one call "
                    "per target anchor listed in target_references. Defaults to write=false (dry run) - always "
                    "call it that way first and review apply_result/diffs before calling again with write=true "
                    "to actually modify the board file."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "template_reference": {"type": "string"},
                        "target_references": {"type": "array", "items": {"type": "string"}},
                        "write": {"type": "boolean", "default": False},
                        "allow_while_open": {"type": "boolean", "default": False, "description": "Skip the check that refuses to write while KiCad has this board open for editing (see get_kicad_ipc_status)."},
                    },
                    "required": ["project_path", "template_reference", "target_references"],
                },
                "handler": self._tool_apply_layout_template,
            },
            "apply_kicad_layout_changes": {
                "description": (
                    "Low-level: apply an explicit list of {reference or uuid, new_position:{x,y,rotation}} changes "
                    "(as returned in diff_kicad_layout_template's `changes`, or written by hand) to the board file. "
                    "Defaults to write=false to preview; call again with write=true to commit."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "changes": {"type": "array", "items": {"type": "object"}},
                        "write": {"type": "boolean", "default": False},
                        "allow_while_open": {"type": "boolean", "default": False, "description": "Skip the check that refuses to write while KiCad has this board open for editing (see get_kicad_ipc_status)."},
                    },
                    "required": ["project_path", "changes"],
                },
                "handler": self._tool_apply_layout_changes,
            },
            "list_kicad_hierarchical_templates": {
                "description": (
                    "Board-wide overview, in one call, of every schematic sheet stamped out more than once "
                    "(one row per repeated sheet file, with every instance's member references and whether it's "
                    "the fully-locked reference layout). Run this FIRST on any 'make these repeated sub-circuits "
                    "consistent' task instead of exploring with search/grep - it replaces the manual discovery "
                    "work of figuring out which components belong together and which instance is the reference."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                    },
                    "required": ["project_path"],
                },
                "handler": self._tool_list_hierarchical_templates,
            },
            "move_kicad_group": {
                "description": (
                    "Rigid-body move: shift every member of a component's hierarchical group together, keeping "
                    "their layout relative to each other. Use this (instead of diff/apply_kicad_layout_template) "
                    "when there's no separate known-good template to copy from - e.g. relocating an "
                    "already-correct cluster elsewhere on the board, or nudging one channel to clear a routing "
                    "conflict. Give dx/dy as a plain offset, or `to: {x, y}` to move the anchor to an absolute "
                    "position. Defaults to write=false to preview."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "reference": {"type": "string", "description": "Any component reference in the group to move."},
                        "dx": {"type": "number", "default": 0.0},
                        "dy": {"type": "number", "default": 0.0},
                        "drotation": {"type": "number", "default": 0.0, "description": "Additional rotation (degrees) applied to every member and the anchor."},
                        "to": {"type": "object", "properties": {"x": {"type": "number"}, "y": {"type": "number"}}, "description": "Move the anchor to this absolute position instead of using dx/dy."},
                        "write": {"type": "boolean", "default": False},
                        "allow_while_open": {"type": "boolean", "default": False, "description": "Skip the check that refuses to write while KiCad has this board open for editing (see get_kicad_ipc_status)."},
                    },
                    "required": ["project_path", "reference"],
                },
                "handler": self._tool_move_group,
            },
            "list_kicad_groups": {
                "description": (
                    "List every top-level PCB group already on the board (KiCad's Ctrl+G grouping construct, "
                    "which lets a cluster of footprints be selected/moved as one unit). Each member uuid is "
                    "resolved back to its reference designator. Use this before create_kicad_group to check "
                    "whether components are already grouped, or to find a group's exact name/uuid before "
                    "deleting it."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                    },
                    "required": ["project_path"],
                },
                "handler": self._tool_list_groups,
            },
            "create_kicad_group": {
                "description": (
                    "Create a new named PCB group containing the given footprint references, so they "
                    "select/move together as one unit in the KiCad GUI - the same construct KiCad itself "
                    "writes for Ctrl+G. Typical flow: get_kicad_hierarchical_group to find a sub-circuit's "
                    "members, then create_kicad_group to group them (e.g. group every part of one thermocouple "
                    "or relay channel). Raises if a reference isn't found on the board, or already belongs to "
                    "another group. Defaults to write=false (dry run) - always preview first, then call again "
                    "with write=true to save."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "name": {"type": "string", "description": "Group name shown in the KiCad GUI (can be empty string, matching KiCad's default for GUI-created groups)."},
                        "references": {"type": "array", "items": {"type": "string"}, "description": "Footprint reference designators to include, e.g. [\"R1\", \"C4\", \"U2\"]."},
                        "write": {"type": "boolean", "default": False},
                        "allow_while_open": {"type": "boolean", "default": False, "description": "Skip the check that refuses to write while KiCad has this board open for editing (see get_kicad_ipc_status)."},
                    },
                    "required": ["project_path", "name", "references"],
                },
                "handler": self._tool_create_group,
            },
            "get_kicad_footprint_pads": {
                "description": (
                    "Get every pad of a footprint - number, net (read straight off the board file's own pad "
                    "entries, not the schematic pin numbering), and absolute board position. Use this whenever "
                    "a placement decision depends on exactly where a pin is (e.g. an IC's pins), not just where "
                    "the footprint's origin is."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "reference": {"type": "string"},
                    },
                    "required": ["project_path", "reference"],
                },
                "handler": self._tool_get_footprint_pads,
            },
            "get_kicad_pin_position": {
                "description": "Look up one pad's net and absolute board position by reference + pin number.",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "reference": {"type": "string"},
                        "pin": {"type": "string"},
                    },
                    "required": ["project_path", "reference", "pin"],
                },
                "handler": self._tool_get_pin_position,
            },
            "get_kicad_pin_distance": {
                "description": (
                    "Euclidean distance between two specific pads. Use to check a placement's quality "
                    "before/after - e.g. confirming a bypass cap's pad ended up closer to the IC pin it bypasses."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "reference_a": {"type": "string"},
                        "pin_a": {"type": "string"},
                        "reference_b": {"type": "string"},
                        "pin_b": {"type": "string"},
                    },
                    "required": ["project_path", "reference_a", "pin_a", "reference_b", "pin_b"],
                },
                "handler": self._tool_pin_distance,
            },
            "align_kicad_component_pin": {
                "description": (
                    "Rigid-move a component (translate, and optionally rotate first) so that one of its pads "
                    "ends up exactly at a given absolute board position. Core primitive for datasheet-guided "
                    "placement: point a passive's pad at the IC pin/pad it needs to reach instead of eyeballing "
                    "footprint-origin offsets. Defaults to write=false (dry run) - review `change`, then call "
                    "again with write=true to commit."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "reference": {"type": "string"},
                        "pin": {"type": "string"},
                        "target": {"type": "object", "properties": {"x": {"type": "number"}, "y": {"type": "number"}}, "required": ["x", "y"]},
                        "rotation": {"type": "number", "description": "Optional new footprint rotation (degrees), applied before the translate."},
                        "write": {"type": "boolean", "default": False},
                        "allow_while_open": {"type": "boolean", "default": False, "description": "Skip the check that refuses to write while KiCad has this board open for editing (see get_kicad_ipc_status)."},
                    },
                    "required": ["project_path", "reference", "pin", "target"],
                },
                "handler": self._tool_align_component_pin,
            },
            "align_kicad_components_to_anchor": {
                "description": (
                    "Batch-place support components relative to one anchor's pins - e.g. arrange every "
                    "capacitor/resistor/inductor around a regulator IC to mirror a datasheet layout guide. Each "
                    "alignments entry: {reference, pin, anchor_pin, offset:{dx,dy} (default 0,0), rotation "
                    "(optional degrees)}. Target = anchor_pin's absolute pad position + offset; `reference`'s "
                    "`pin` pad is placed there. Defaults to write=false (dry run) - review `results`, then call "
                    "again with write=true to commit all of them."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "anchor_reference": {"type": "string"},
                        "alignments": {"type": "array", "items": {"type": "object"}},
                        "write": {"type": "boolean", "default": False},
                        "allow_while_open": {"type": "boolean", "default": False, "description": "Skip the check that refuses to write while KiCad has this board open for editing (see get_kicad_ipc_status)."},
                    },
                    "required": ["project_path", "anchor_reference", "alignments"],
                },
                "handler": self._tool_align_components_to_anchor,
            },
            "classify_kicad_group_by_anchor_pin": {
                "description": (
                    "For every other member of a hierarchical group, find which of the anchor's own pads it "
                    "shares a net with - i.e. its electrical role (VIN cap, feedback divider resistor, etc.), "
                    "read straight off board nets. Automatic version of hand-building a 'which part goes with "
                    "which IC pin' table - usually you want match_kicad_group_members_by_role or "
                    "diff_kicad_layout_by_role instead of calling this directly."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "anchor_reference": {"type": "string"},
                    },
                    "required": ["project_path", "anchor_reference"],
                },
                "handler": self._tool_classify_group_by_anchor_pin,
            },
            "match_kicad_group_members_by_role": {
                "description": (
                    "Match components between two hierarchical groups by which anchor pin they connect to, "
                    "instead of KiCad's symbol_uuid (which only works between instances of the *same* schematic "
                    "sheet). Works even when the two groups are on entirely different sheet files, as long as "
                    "their anchors share a compatible pinout - e.g. two independently-drawn but functionally "
                    "analogous regulator circuits. Ties (more than one same-footprint candidate on either side) "
                    "are broken by matching component value; anything still tied comes back under `ambiguous` "
                    "instead of being guessed - pass `overrides` ({template_reference: target_reference}) to "
                    "force those once you've eyeballed which is which."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "template_reference": {"type": "string"},
                        "target_reference": {"type": "string"},
                        "overrides": {"type": "object", "description": "{template_reference: target_reference} forced pairings for ambiguous ties."},
                    },
                    "required": ["project_path", "template_reference", "target_reference"],
                },
                "handler": self._tool_match_group_members_by_role,
            },
            "diff_kicad_layout_by_role": {
                "description": (
                    "Like apply_kicad_layout_template's diff step, but for two hierarchical groups on *different* "
                    "schematic sheets - matches members by anchor-pin role (match_kicad_group_members_by_role) "
                    "instead of shared symbol_uuid, then carries the template group's relative layout (offsets + "
                    "rotations) over onto the target anchor's own position. Check `ambiguous`/`template_unmatched`/"
                    "`target_unmatched` before trusting `changes` is complete. `changes` is ready to hand straight "
                    "to apply_kicad_layout_changes."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "template_reference": {"type": "string"},
                        "target_reference": {"type": "string"},
                        "overrides": {"type": "object", "description": "{template_reference: target_reference} forced pairings for ambiguous ties."},
                    },
                    "required": ["project_path", "template_reference", "target_reference"],
                },
                "handler": self._tool_diff_layout_by_role,
            },
            "estimate_kicad_footprint_radius": {
                "description": (
                    "Best-effort collision-check radius (mm) for a footprint: a known-good manual override for "
                    "packages where pad span badly underestimates body size (electrolytic cans, connectors), else "
                    "a size parsed out of a standard KiCad SMD footprint name, else a pad-bounding-box estimate, "
                    "else a conservative 2.0mm default."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "reference": {"type": "string"},
                    },
                    "required": ["project_path", "reference"],
                },
                "handler": self._tool_estimate_footprint_radius,
            },
            "find_kicad_layout_collisions": {
                "description": (
                    "Collision-check a set of footprints (typically one hierarchical group's members) both "
                    "against each other and against any *other* board component nearby - catching e.g. a group's "
                    "inductor ending up on top of an unrelated connector from a different subsystem. Uses "
                    "estimate_kicad_footprint_radius for every part's envelope, so no radius table needs to be "
                    "built by the caller. Read-only."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "references": {"type": "array", "items": {"type": "string"}},
                        "extra_search_radius": {"type": "number", "default": 25.0, "description": "mm; how far to look for outside obstacles."},
                        "margin": {"type": "number", "default": 0.4, "description": "mm; required clearance between envelopes."},
                    },
                    "required": ["project_path", "references"],
                },
                "handler": self._tool_find_layout_collisions,
            },
            "nudge_kicad_to_clear": {
                "description": (
                    "Move a component the minimum distance needed to clear a collision, searching outward in a "
                    "ring from its *current* position so it stays as close as possible to wherever it already "
                    "was (usually intentional) rather than being fully re-placed. Obstacles default to every "
                    "other board component within search_radius mm; pass avoid_references for an explicit list "
                    "instead. Defaults to write=false (dry run) - review `new_position`, then call again with "
                    "write=true to commit."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "reference": {"type": "string"},
                        "avoid_references": {"type": "array", "items": {"type": "string"}},
                        "search_radius": {"type": "number", "default": 25.0},
                        "margin": {"type": "number", "default": 0.4},
                        "max_search_radius": {"type": "number", "default": 20.0},
                        "write": {"type": "boolean", "default": False},
                        "allow_while_open": {"type": "boolean", "default": False, "description": "Skip the check that refuses to write while KiCad has this board open for editing (see get_kicad_ipc_status)."},
                    },
                    "required": ["project_path", "reference"],
                },
                "handler": self._tool_nudge_to_clear,
            },
            "delete_kicad_group": {
                "description": (
                    "Delete a top-level PCB group by name or uuid - only the grouping is removed, member "
                    "footprints are untouched. Give group_uuid when multiple groups share a name (common for "
                    "unnamed \"\" groups); name alone must match exactly one group. Defaults to write=false "
                    "(dry run) - preview the matched group, then call again with write=true to save."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "name": {"type": "string"},
                        "group_uuid": {"type": "string"},
                        "write": {"type": "boolean", "default": False},
                        "allow_while_open": {"type": "boolean", "default": False, "description": "Skip the check that refuses to write while KiCad has this board open for editing (see get_kicad_ipc_status)."},
                    },
                    "required": ["project_path"],
                },
                "handler": self._tool_delete_group,
            },
            "get_kicad_property_position": {
                "description": (
                    "Get a footprint's child text property's own local (at x y rotation) and layer - e.g. exactly "
                    "where the 'Reference' designator text sits on the silkscreen, relative to the footprint's own "
                    "origin. Different from get_kicad_component's position, which is the footprint's own placement, "
                    "not its label's offset. Use this to read a known-good instance's label placement before "
                    "copying it with diff_kicad_property_position_template."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "reference": {"type": "string"},
                        "property_name": {"type": "string", "default": "Reference"},
                    },
                    "required": ["project_path", "reference"],
                },
                "handler": self._tool_get_property_position,
            },
            "diff_kicad_property_position_template": {
                "description": (
                    "Dry-run: the silkscreen-label analogue of diff_kicad_layout_template. Compute which text-"
                    "property offsets (default 'Reference', pass e.g. 'Value' for others) in target_reference's "
                    "hierarchical group need to change to match template_reference's group - use this after a "
                    "reference instance's labels have been hand-decluttered to avoid overlaps, and you want to "
                    "copy that exact treatment onto sibling instances (e.g. other repeated channels). Matches "
                    "members by symbol_uuid like diff_kicad_layout_template. Any matched pair whose own footprint "
                    "rotation differs is reported under `skipped` rather than guessed at, since a label offset's "
                    "rotation does not transform under a simple linear rule. Returns `changes`; nothing is written "
                    "- pass `changes` to apply_kicad_property_position_changes, or use "
                    "apply_kicad_property_position_template to do both in one call."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "template_reference": {"type": "string"},
                        "target_reference": {"type": "string"},
                        "property_name": {"type": "string", "default": "Reference"},
                    },
                    "required": ["project_path", "template_reference", "target_reference"],
                },
                "handler": self._tool_diff_property_position_template,
            },
            "apply_kicad_property_position_changes": {
                "description": (
                    "Low-level: apply an explicit list of {reference or uuid, property, new_at:{x,y,rotation}} "
                    "changes (as returned in diff_kicad_property_position_template's `changes`, or written by "
                    "hand) to the matching child property's (at ...) line inside each footprint's block. Defaults "
                    "to write=false to preview; call again with write=true to commit."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "changes": {"type": "array", "items": {"type": "object"}},
                        "write": {"type": "boolean", "default": False},
                        "allow_while_open": {"type": "boolean", "default": False, "description": "Skip the check that refuses to write while KiCad has this board open for editing (see get_kicad_ipc_status)."},
                    },
                    "required": ["project_path", "changes"],
                },
                "handler": self._tool_apply_property_position_changes,
            },
            "apply_kicad_property_position_template": {
                "description": (
                    "Copy template_reference's group's text-property label offsets (default 'Reference') onto "
                    "every group in target_references, one call per target - the silkscreen-label analogue of "
                    "apply_kicad_layout_template. Example: after hand-decluttering U7's 'Reference' silkscreen "
                    "labels to stop them overlapping, apply_kicad_property_position_template(project_path, 'U7', "
                    "['U8','U9','U6']) copies that exact same label placement onto the matching component in each "
                    "sibling channel. Defaults to write=false (dry run) - review diffs/apply_result, then call "
                    "again with write=true to commit."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "template_reference": {"type": "string"},
                        "target_references": {"type": "array", "items": {"type": "string"}},
                        "property_name": {"type": "string", "default": "Reference"},
                        "write": {"type": "boolean", "default": False},
                        "allow_while_open": {"type": "boolean", "default": False, "description": "Skip the check that refuses to write while KiCad has this board open for editing (see get_kicad_ipc_status)."},
                    },
                    "required": ["project_path", "template_reference", "target_references"],
                },
                "handler": self._tool_apply_property_position_template,
            },
            "diff_kicad_flip_template": {
                "description": (
                    "Dry-run: find which members of target_reference's hierarchical group sit on the wrong copper "
                    "side (front/back) compared to their matching member (by symbol_uuid) in template_reference's "
                    "group - e.g. the template channel has some support parts deliberately flipped to the back to "
                    "save front-side space, and this target channel doesn't yet. Rotation mismatches between a "
                    "matched pair are reported under `skipped` rather than attempted. Returns `changes`; nothing "
                    "is written - pass to apply_kicad_flip_template."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "template_reference": {"type": "string"},
                        "target_reference": {"type": "string"},
                    },
                    "required": ["project_path", "template_reference", "target_reference"],
                },
                "handler": self._tool_diff_flip_template,
            },
            "apply_kicad_flip_template": {
                "description": (
                    "Flip every part of target_references' hierarchical groups that needs it to match "
                    "template_reference's group's front/back layer split, by CLONING the template member's "
                    "already-correctly-flipped footprint block (mirrored silkscreen/fab graphics, swapped F./B. "
                    "layer names, 'justify mirror' text flags, adjusted pad angles - everything KiCad's own Flip "
                    "command produces) onto the target footprint, while keeping the target's own identity: its "
                    "uuid, schematic path/sheetname/sheetfile, board position, and (matched by pad number) its own "
                    "net names. Use this instead of hand-deriving a flip transform - a text property's stored "
                    "rotation does not transform under mirroring by one fixed rule, so the only trustworthy source "
                    "for 'what does a correctly-flipped instance of this footprint look like' is an instance KiCad "
                    "itself already flipped. template_reference's group must already contain one for every role "
                    "that needs flipping. Defaults to write=false (dry run) - inspect flipped/failed, then call "
                    "again with write=true to commit."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "project_path": {"type": "string"},
                        "template_reference": {"type": "string"},
                        "target_references": {"type": "array", "items": {"type": "string"}},
                        "write": {"type": "boolean", "default": False},
                        "allow_while_open": {"type": "boolean", "default": False, "description": "Skip the check that refuses to write while KiCad has this board open for editing (see get_kicad_ipc_status)."},
                    },
                    "required": ["project_path", "template_reference", "target_references"],
                },
                "handler": self._tool_apply_flip_template,
            },
        }
        if _IPC_AVAILABLE:
            self.tools.update(self._ipc_tools())

    def _ipc_tools(self) -> dict[str, dict[str, Any]]:
        """Tools that talk to a *running* KiCad instance over the IPC API
        (kicad-python), instead of parsing kiln.kicad_pcb on disk. Require
        KiCad to be open with this board loaded and Preferences > Plugins >
        'Enable IPC API' turned on - every one of these fails fast with a
        clear message if that's not the case. None of them take a
        project_path; they operate on whatever board KiCad currently has
        open.
        """
        return {
            "get_kicad_ipc_status": {
                "description": (
                    "Check whether KiCad's IPC API is reachable right now, and report the "
                    "connected KiCad version and which board (if any) is open. Call this first "
                    "when any other get_kicad_live_*/find_kicad_live_*/*_kicad_live_* tool fails, "
                    "to tell 'KiCad isn't running/API disabled' apart from 'component not found'."
                ),
                "inputSchema": {"type": "object", "properties": {}},
                "handler": self._tool_get_ipc_status,
            },
            "get_kicad_live_bounding_box": {
                "description": (
                    "Real KiCad-computed bounding box (mm) for a footprint, straight from KiCad's "
                    "own geometry engine - accounts for actual pad/silkscreen/courtyard shapes and "
                    "rotation exactly, unlike estimate_kicad_footprint_radius's circle-from-name "
                    "heuristic. Use for oddly-shaped parts (connectors, electrolytic cans, relays) "
                    "where the heuristic is least trustworthy."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "reference": {"type": "string"},
                        "include_text": {"type": "boolean", "default": False, "description": "Include the footprint's reference/value silkscreen text in the box."},
                    },
                    "required": ["reference"],
                },
                "handler": self._tool_get_live_bounding_box,
            },
            "find_kicad_live_layout_collisions": {
                "description": (
                    "Live-board analogue of find_kicad_layout_collisions: same internal (among "
                    "references) + external (nearby obstacles) collision check, but using KiCad's "
                    "own bounding boxes instead of the file tool's circular-radius estimate - more "
                    "accurate for oblong parts. Read-only."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "references": {"type": "array", "items": {"type": "string"}},
                        "extra_search_radius": {"type": "number", "default": 25.0, "description": "mm; how far to look for outside obstacles."},
                        "margin": {"type": "number", "default": 0.4, "description": "mm; required clearance between boxes."},
                    },
                    "required": ["references"],
                },
                "handler": self._tool_find_live_layout_collisions,
            },
            "highlight_kicad_live_components": {
                "description": (
                    "Select the given component references in the live KiCad PCB editor window, "
                    "replacing whatever's currently selected - so a human reviewing an agent's "
                    "proposed change can see exactly which footprints it's about to touch before "
                    "any write happens. Purely visual; writes still go through the write=true "
                    "file-based tools."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": {"references": {"type": "array", "items": {"type": "string"}}},
                    "required": ["references"],
                },
                "handler": self._tool_highlight_live_components,
            },
            "clear_kicad_live_highlight": {
                "description": "Clear the current selection in the live KiCad PCB editor window.",
                "inputSchema": {"type": "object", "properties": {}},
                "handler": self._tool_clear_live_highlight,
            },
            "get_kicad_live_selection": {
                "description": (
                    "Read back whatever is currently selected in the live KiCad PCB editor - e.g. "
                    "so a person can point at a component by hand in the GUI instead of typing its "
                    "reference designator for a follow-up tool call."
                ),
                "inputSchema": {"type": "object", "properties": {}},
                "handler": self._tool_get_live_selection,
            },
        }

    def _tool_inspect_project(self, args: dict[str, Any]) -> dict[str, Any]:
        return inspect_project(args["project_path"])

    def _tool_list_components(self, args: dict[str, Any]) -> list[dict[str, Any]]:
        return list_components(args["project_path"], limit=int(args.get("limit", 50)))

    def _tool_get_component(self, args: dict[str, Any]) -> dict[str, Any]:
        return get_component(args["project_path"], args["reference"])

    def _tool_get_component_connections(self, args: dict[str, Any]) -> dict[str, Any]:
        return get_component_connections(args["project_path"], args["reference"])

    def _tool_get_net(self, args: dict[str, Any]) -> dict[str, Any]:
        return get_net(args["project_path"], args["net_name"])

    def _tool_find_components_by_net(self, args: dict[str, Any]) -> dict[str, Any]:
        return find_components_by_net(args["project_path"], args["net_name"])

    def _tool_find_components_by_pin_connection(self, args: dict[str, Any]) -> dict[str, Any]:
        return find_components_by_pin_connection(
            args["project_path"],
            args["reference"],
            args["pin"],
        )

    def _tool_suggest_component_placement(self, args: dict[str, Any]) -> dict[str, Any]:
        return suggest_component_placement(
            args["project_path"],
            args["reference"],
            group_size=int(args.get("group_size", 4)),
            spacing=float(args.get("spacing", 10.0)),
            rotation=float(args.get("rotation", 0.0)),
        )

    def _tool_list_nets(self, args: dict[str, Any]) -> list[dict[str, Any]]:
        return list_nets(args["project_path"])

    def _tool_search_component(self, args: dict[str, Any]) -> dict[str, Any]:
        return search_component_by_reference(args["project_path"], args["reference"])

    def _tool_get_hierarchical_group(self, args: dict[str, Any]) -> dict[str, Any]:
        return get_hierarchical_group(args["project_path"], args["reference"], verbose=bool(args.get("verbose", False)))

    def _tool_list_sibling_instances(self, args: dict[str, Any]) -> dict[str, Any]:
        return list_sibling_instances(args["project_path"], args["reference"])

    def _tool_diff_layout_template(self, args: dict[str, Any]) -> dict[str, Any]:
        return diff_layout_template(args["project_path"], args["template_reference"], args["target_reference"])

    def _tool_apply_layout_template(self, args: dict[str, Any]) -> dict[str, Any]:
        return apply_layout_template(
            args["project_path"],
            args["template_reference"],
            list(args["target_references"]),
            write=bool(args.get("write", False)),
            allow_while_open=bool(args.get("allow_while_open", False)),
        )

    def _tool_apply_layout_changes(self, args: dict[str, Any]) -> dict[str, Any]:
        return apply_layout_changes(
            args["project_path"],
            list(args["changes"]),
            write=bool(args.get("write", False)),
            allow_while_open=bool(args.get("allow_while_open", False)),
        )

    def _tool_list_hierarchical_templates(self, args: dict[str, Any]) -> dict[str, Any]:
        return list_hierarchical_templates(args["project_path"])

    def _tool_move_group(self, args: dict[str, Any]) -> dict[str, Any]:
        return move_group(
            args["project_path"],
            args["reference"],
            dx=float(args.get("dx", 0.0)),
            dy=float(args.get("dy", 0.0)),
            drotation=float(args.get("drotation", 0.0)),
            to=args.get("to"),
            write=bool(args.get("write", False)),
            allow_while_open=bool(args.get("allow_while_open", False)),
        )

    def _tool_list_groups(self, args: dict[str, Any]) -> dict[str, Any]:
        return list_groups(args["project_path"])

    def _tool_create_group(self, args: dict[str, Any]) -> dict[str, Any]:
        return create_group(
            args["project_path"],
            args["name"],
            list(args["references"]),
            write=bool(args.get("write", False)),
            allow_while_open=bool(args.get("allow_while_open", False)),
        )

    def _tool_get_footprint_pads(self, args: dict[str, Any]) -> dict[str, Any]:
        return get_footprint_pads(args["project_path"], args["reference"])

    def _tool_get_pin_position(self, args: dict[str, Any]) -> dict[str, Any]:
        return get_pin_position(args["project_path"], args["reference"], args["pin"])

    def _tool_pin_distance(self, args: dict[str, Any]) -> dict[str, Any]:
        return pin_distance(
            args["project_path"],
            args["reference_a"],
            args["pin_a"],
            args["reference_b"],
            args["pin_b"],
        )

    def _tool_align_component_pin(self, args: dict[str, Any]) -> dict[str, Any]:
        return align_component_pin(
            args["project_path"],
            args["reference"],
            args["pin"],
            args["target"],
            rotation=args.get("rotation"),
            write=bool(args.get("write", False)),
            allow_while_open=bool(args.get("allow_while_open", False)),
        )

    def _tool_align_components_to_anchor(self, args: dict[str, Any]) -> dict[str, Any]:
        return align_components_to_anchor(
            args["project_path"],
            args["anchor_reference"],
            list(args["alignments"]),
            write=bool(args.get("write", False)),
            allow_while_open=bool(args.get("allow_while_open", False)),
        )

    def _tool_classify_group_by_anchor_pin(self, args: dict[str, Any]) -> dict[str, Any]:
        return classify_group_by_anchor_pin(args["project_path"], args["anchor_reference"])

    def _tool_match_group_members_by_role(self, args: dict[str, Any]) -> dict[str, Any]:
        return match_group_members_by_role(
            args["project_path"],
            args["template_reference"],
            args["target_reference"],
            overrides=args.get("overrides"),
        )

    def _tool_diff_layout_by_role(self, args: dict[str, Any]) -> dict[str, Any]:
        return diff_layout_by_role(
            args["project_path"],
            args["template_reference"],
            args["target_reference"],
            overrides=args.get("overrides"),
        )

    def _tool_estimate_footprint_radius(self, args: dict[str, Any]) -> dict[str, Any]:
        return {"reference": args["reference"], "radius": estimate_footprint_radius(args["project_path"], args["reference"])}

    def _tool_find_layout_collisions(self, args: dict[str, Any]) -> dict[str, Any]:
        return find_layout_collisions(
            args["project_path"],
            list(args["references"]),
            extra_search_radius=float(args.get("extra_search_radius", 25.0)),
            margin=float(args.get("margin", 0.4)),
        )

    def _tool_nudge_to_clear(self, args: dict[str, Any]) -> dict[str, Any]:
        return nudge_to_clear(
            args["project_path"],
            args["reference"],
            avoid_references=args.get("avoid_references"),
            search_radius=float(args.get("search_radius", 25.0)),
            margin=float(args.get("margin", 0.4)),
            max_search_radius=float(args.get("max_search_radius", 20.0)),
            write=bool(args.get("write", False)),
            allow_while_open=bool(args.get("allow_while_open", False)),
        )

    def _tool_delete_group(self, args: dict[str, Any]) -> dict[str, Any]:
        return delete_group(
            args["project_path"],
            name=args.get("name"),
            group_uuid=args.get("group_uuid"),
            write=bool(args.get("write", False)),
            allow_while_open=bool(args.get("allow_while_open", False)),
        )

    def _tool_get_property_position(self, args: dict[str, Any]) -> dict[str, Any]:
        return get_property_position(
            args["project_path"],
            args["reference"],
            property_name=args.get("property_name", "Reference"),
        )

    def _tool_diff_property_position_template(self, args: dict[str, Any]) -> dict[str, Any]:
        return diff_property_position_template(
            args["project_path"],
            args["template_reference"],
            args["target_reference"],
            property_name=args.get("property_name", "Reference"),
        )

    def _tool_apply_property_position_changes(self, args: dict[str, Any]) -> dict[str, Any]:
        return apply_property_position_changes(
            args["project_path"],
            list(args["changes"]),
            write=bool(args.get("write", False)),
            allow_while_open=bool(args.get("allow_while_open", False)),
        )

    def _tool_apply_property_position_template(self, args: dict[str, Any]) -> dict[str, Any]:
        return apply_property_position_template(
            args["project_path"],
            args["template_reference"],
            list(args["target_references"]),
            property_name=args.get("property_name", "Reference"),
            write=bool(args.get("write", False)),
            allow_while_open=bool(args.get("allow_while_open", False)),
        )

    def _tool_diff_flip_template(self, args: dict[str, Any]) -> dict[str, Any]:
        return diff_flip_template(args["project_path"], args["template_reference"], args["target_reference"])

    def _tool_apply_flip_template(self, args: dict[str, Any]) -> dict[str, Any]:
        return apply_flip_template(
            args["project_path"],
            args["template_reference"],
            list(args["target_references"]),
            write=bool(args.get("write", False)),
            allow_while_open=bool(args.get("allow_while_open", False)),
        )

    def _tool_get_ipc_status(self, args: dict[str, Any]) -> dict[str, Any]:
        return get_ipc_status()

    def _tool_get_live_bounding_box(self, args: dict[str, Any]) -> dict[str, Any]:
        return get_live_bounding_box(args["reference"], include_text=bool(args.get("include_text", False)))

    def _tool_find_live_layout_collisions(self, args: dict[str, Any]) -> dict[str, Any]:
        return find_live_layout_collisions(
            list(args["references"]),
            extra_search_radius=float(args.get("extra_search_radius", 25.0)),
            margin=float(args.get("margin", 0.4)),
        )

    def _tool_highlight_live_components(self, args: dict[str, Any]) -> dict[str, Any]:
        return highlight_live_components(list(args["references"]))

    def _tool_clear_live_highlight(self, args: dict[str, Any]) -> dict[str, Any]:
        return clear_live_highlight()

    def _tool_get_live_selection(self, args: dict[str, Any]) -> dict[str, Any]:
        return get_live_selection()

    def handle(self, message: dict[str, Any]) -> dict[str, Any] | None:
        method = message.get("method")
        if method is None:
            return None

        if method == "initialize":
            params = message.get("params") or {}
            requested_version = params.get("protocolVersion")
            protocol_version = requested_version if requested_version in SUPPORTED_PROTOCOL_VERSIONS else "2024-11-05"
            return {
                "jsonrpc": "2.0",
                "id": message.get("id"),
                "result": {
                    "protocolVersion": protocol_version,
                    "capabilities": {"tools": {"listChanged": False}},
                    "serverInfo": {"name": "kiln-kicad-mcp", "version": "1.0.0"},
                },
            }

        if method == "tools/list":
            return {
                "jsonrpc": "2.0",
                "id": message.get("id"),
                "result": {
                    "tools": [
                        {
                            "name": name,
                            "description": info["description"],
                            "inputSchema": info["inputSchema"],
                        }
                        for name, info in self.tools.items()
                    ]
                },
            }

        if method == "tools/call":
            params = message.get("params", {})
            tool_name = params.get("name")
            arguments = params.get("arguments", {}) or {}
            tool = self.tools.get(tool_name)
            if not tool:
                return {
                    "jsonrpc": "2.0",
                    "id": message.get("id"),
                    "error": {"code": -1, "message": f"Unknown tool: {tool_name}"},
                }
            try:
                result = tool["handler"](arguments)
            except Exception as exc:  # pragma: no cover - runtime safety
                return {
                    "jsonrpc": "2.0",
                    "id": message.get("id"),
                    "error": {"code": -2, "message": str(exc)},
                }
            return {
                "jsonrpc": "2.0",
                "id": message.get("id"),
                "result": {
                    "content": [
                        {
                            "type": "text",
                            "text": json.dumps(result, indent=2, ensure_ascii=False),
                        }
                    ],
                    "isError": False,
                },
            }

        if method == "ping":
            return {"jsonrpc": "2.0", "id": message.get("id"), "result": {"ok": True}}

        return {
            "jsonrpc": "2.0",
            "id": message.get("id"),
            "error": {"code": -32601, "message": f"Method not found: {method}"},
        }


def _read_message() -> dict[str, Any] | None:
    headers: dict[str, str] = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            log_message("[kicad-mcp] stdin closed before message")
            return None
        if line in (b"\r\n", b"\n"):
            break
        key, _, value = line.decode("utf-8").partition(":")
        if key:
            headers[key.strip().lower()] = value.strip()

    length = int(headers.get("content-length", "0"))
    body = sys.stdin.buffer.read(length)
    if not body:
        log_message("[kicad-mcp] empty body received")
        return None
    try:
        message = json.loads(body.decode("utf-8"))
    except Exception as exc:
        log_message(f"[kicad-mcp] invalid JSON: {exc}")
        raise
    return message


class MCPHTTPRequestHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format: str, *args: object) -> None:
        return

    def _send_json(self, response: dict[str, Any], status: int = 200) -> None:
        payload = json.dumps(response, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        self.wfile.write(payload)
        self.wfile.flush()

    def do_POST(self) -> None:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.send_error(411, "Content-Length required")
            return

        body = self.rfile.read(length)
        try:
            message = json.loads(body.decode("utf-8"))
        except Exception as exc:
            self.send_error(400, f"Invalid JSON: {exc}")
            return

        method = message.get("method")
        if method == "notifications/initialized":
            self.send_response(202)
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            return

        server = cast("KicadMcpHTTPServer", self.server)
        response = server.kicad_mcp.handle(message)
        if response is None:
            self.send_response(204)
            self.end_headers()
            return
        self._send_json(response)

    def do_GET(self) -> None:
        accept_header = self.headers.get("Accept", "")
        if "text/event-stream" in accept_header.lower():
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            try:
                while True:
                    self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
                    time.sleep(15)
            except BrokenPipeError:
                return
            except ConnectionResetError:
                return
        else:
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            self.wfile.write(b'{"status":"ok"}')
            self.wfile.flush()


class KicadMcpHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True

    def __init__(self, server_address: tuple[str, int], RequestHandlerClass: type[BaseHTTPRequestHandler], kicad_mcp: KiCadMcpServer) -> None:
        super().__init__(server_address, RequestHandlerClass)
        self.kicad_mcp = kicad_mcp


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="KiCad MCP server supporting stdio and HTTP transports")
    parser.add_argument("--transport", choices=["stdio", "http"], default="stdio", help="Transport to use for MCP communication")
    parser.add_argument("--host", default="127.0.0.1", help="HTTP host to bind when using HTTP transport")
    parser.add_argument("--port", type=int, default=8765, help="HTTP port to bind when using HTTP transport")
    return parser.parse_args()


def _write_message(message: dict[str, Any] | None) -> None:
    if message is None:
        return
    payload = json.dumps(message, ensure_ascii=False).encode("utf-8")
    header = f"Content-Length: {len(payload)}\r\n\r\n".encode("utf-8")
    sys.stdout.buffer.write(header)
    sys.stdout.buffer.write(payload)
    sys.stdout.buffer.flush()


def main() -> None:
    log_message("[kicad-mcp] starting")
    server = KiCadMcpServer()
    log_message("[kicad-mcp] server initialized")
    args = parse_args()

    if args.transport == "http":
        http_server = KicadMcpHTTPServer((args.host, args.port), MCPHTTPRequestHandler, server)
        log_message(f"[kicad-mcp] HTTP transport listening on http://{args.host}:{args.port}")
        print(f"HTTP server listening on http://{args.host}:{args.port}", flush=True)
        try:
            http_server.serve_forever()
        except KeyboardInterrupt:
            log_message("[kicad-mcp] HTTP server shutting down")
            http_server.server_close()
    else:
        while True:
            try:
                message = _read_message()
                if message is None:
                    log_message("[kicad-mcp] exiting because input ended")
                    break
                if message.get("method") == "notifications/initialized":
                    log_message("[kicad-mcp] ignoring notifications/initialized")
                    continue
                response = server.handle(message)
                _write_message(response)
            except Exception as exc:
                log_message(f"[kicad-mcp] unhandled error: {exc}")
                traceback.print_exc(file=sys.stderr)
                break


if __name__ == "__main__":
    main()
