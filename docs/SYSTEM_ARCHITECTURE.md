# System Architecture — both processors and the boards under them

> **Status:** stub · **Last reviewed:** 2026-08-16
> **Keep this file current.** It is a placeholder with a defined scope, not a
> document. Fill it in when there is something to describe that is true of the
> *system* rather than of either firmware; delete it if that never happens
> rather than leaving an empty file that looks authoritative.

## Why this file exists

The isolated-link contract used to live in `KilnFW/docs/SAFETY_LINK.md` — inside
one of the two firmwares that implement it. That is how it came to describe the
optocoupler data directions backwards without anyone noticing for months: it
read as an ESP implementation note rather than as a contract with a second
party.

`firmware/CommonFW/` fixed half of that by giving the wire contract code and
documentation owned by neither firmware. This file is the other half: the place
for statements about the whole machine, which no single firmware's `docs/` can
own without quietly becoming that firmware's opinion.

## What belongs here

- The end-to-end path from a fire profile to a heating element, across both
  processors and both relay types — who commands, who can veto, and where the
  galvanic isolation boundaries fall.
- The board-to-board interface: what physically crosses between `mainBoard`,
  the thermocouple daughterboards, and the external contactors.
- The power domains (`GND_Main` / `GND_Safty`) and every crossing between them.
- Boot and shutdown ordering across both processors.
- The failure modes that need both firmwares to be read together to understand.

## What does not belong here

- Anything true of only one firmware. That goes in its `docs/`.
- The wire format. That is
  [`../firmware/CommonFW/docs/LINK_PROTOCOL.md`](../firmware/CommonFW/docs/LINK_PROTOCOL.md).
- Which faults trip and why. That is
  [`../firmware/SaftyFW/docs/SAFETY_MODEL.md`](../firmware/SaftyFW/docs/SAFETY_MODEL.md).
- Task and driver structure. Those are the per-firmware `ARCHITECTURE.md` files.

## Completion checklist

- [ ] Written, or deliberately deleted
- [ ] Referenced from [`../ROADMAP.md`](../ROADMAP.md) if it survives
