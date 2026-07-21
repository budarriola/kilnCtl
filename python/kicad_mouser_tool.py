"""Look up manufacturer specs for a component from Mouser's official Search API.

Requires a free Mouser API key (MOUSER_API_KEY in the repo-root .env - see
.env.example) - Mouser's product-page HTML is actively bot-blocked for
scripted clients, so this deliberately does not fall back to scraping it;
a missing key is a hard stop with a message asking for one, not a silent
degrade. No bs4/requests dependency - just the stdlib.
"""

from __future__ import annotations

import json
import os
import re
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

from kicad_pcb_tool import get_schematic_part, list_schematic_parts

# Repo-root .env/.mouser_cache.json (both gitignored) - see .env.example for
# the expected key name.
_REPO_ROOT = Path(__file__).resolve().parent.parent
_ENV_FILE = _REPO_ROOT / ".env"
_CACHE_FILE = _REPO_ROOT / ".mouser_cache.json"
_CACHE_TTL_SECONDS = 3600  # one hour, per the "same part within the same hour" requirement

_MOUSER_API_BASE = "https://api.mouser.com/api/v1"

# Mouser's free-tier Search API caps calls per minute ("TooManyRequests" /
# "MaxCallPerMinute") - a bulk lookup across a whole schematic's worth of
# parts routinely bursts past that, so requests are both spaced out and
# retried with a backoff on that specific error instead of failing outright.
_RATE_LIMIT_BACKOFF_SECONDS = 20.0
_RATE_LIMIT_MAX_RETRIES = 4
_BULK_REQUEST_DELAY_SECONDS = 1.5


def _load_env_file(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return values
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        values[key.strip()] = value.strip().strip('"').strip("'")
    return values


def _get_mouser_api_key() -> str | None:
    key = os.environ.get("MOUSER_API_KEY")
    if key:
        return key.strip()
    return _load_env_file(_ENV_FILE).get("MOUSER_API_KEY") or None


def _require_mouser_api_key() -> str:
    key = _get_mouser_api_key()
    if not key:
        raise RuntimeError(
            "No Mouser API key configured. Please provide your Mouser API key - either set "
            "MOUSER_API_KEY in the repo-root .env file (copy .env.example and fill it in) or "
            "export it as an environment variable. This tool does not fall back to scraping "
            "Mouser's website, since that gets blocked by their bot protection."
        )
    return key


# ---------------------------------------------------------------------------
# Per-part result cache (JSON file, gitignored) - a bulk lookup across the
# whole schematic is expensive (rate-limited, ~1.5s/part minimum) and this
# project's parts don't change every minute, so a fresh-within-the-hour
# lookup for the same part is served from disk instead of hitting the API
# again. Keyed by host+path (ignoring the `?qs=...` tracking query string, so
# two properties pointing at "the same" product with different tracking
# params still share one cache entry) rather than the full URL.
# ---------------------------------------------------------------------------


def _cache_key_for_url(url: str) -> str:
    parsed = urllib.parse.urlparse(url)
    return f"{(parsed.hostname or '').lower()}{parsed.path}".rstrip("/")


def _load_mouser_cache() -> dict[str, Any]:
    try:
        return json.loads(_CACHE_FILE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def _save_mouser_cache(cache: dict[str, Any]) -> None:
    try:
        _CACHE_FILE.write_text(json.dumps(cache, indent=2, sort_keys=True), encoding="utf-8")
    except OSError:
        pass


def _get_cached_mouser_result(url: str) -> dict[str, Any] | None:
    entry = _load_mouser_cache().get(_cache_key_for_url(url))
    if not entry:
        return None
    if time.time() - entry.get("cached_at", 0) > _CACHE_TTL_SECONDS:
        return None
    result = dict(entry.get("result") or {})
    result["url"] = url
    result["cache_hit"] = True
    return result


def _store_mouser_cache_result(url: str, result: dict[str, Any]) -> None:
    cache = _load_mouser_cache()
    cache[_cache_key_for_url(url)] = {"cached_at": time.time(), "url": url, "result": result}
    _save_mouser_cache(cache)


def _validate_mouser_url(url: str) -> str:
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme not in ("http", "https"):
        raise ValueError(f"Unsupported URL scheme: {parsed.scheme!r}")
    host = (parsed.hostname or "").lower()
    if "mouser" not in host.split("."):
        raise ValueError(f"Refusing to look up a non-Mouser host: {parsed.hostname!r}")
    return url


def _extract_mpn_hint_from_url(url: str) -> str | None:
    """Mouser product URLs look like .../ProductDetail/<Manufacturer>/<PartNumber>
    - the last path segment is normally the manufacturer part number, which
    makes a good Search API keyword even before any lookup has happened.
    """
    path = urllib.parse.urlparse(url).path
    segments = [urllib.parse.unquote(s) for s in path.split("/") if s]
    return segments[-1] if segments else None


def find_all_mouser_urls(properties: dict[str, str]) -> dict[str, str]:
    """Find all Mouser product links in a schematic part's properties,
    indexed by field name. Returns a dict of {field_name: url} with primary
    and alternate Mouser part numbers. Supports multiple Mouser fields
    (Mouser, Mouser Part Number, Mouser Part Number Alt, etc) and prefers
    /ProductDetail/ links over datasheet PDFs since they identify a specific part.
    """
    urls: dict[str, str] = {}
    product_detail_urls: dict[str, str] = {}

    # Check all properties and extract Mouser URLs
    for key, value in properties.items():
        if not value or "mouser" not in value.lower():
            continue
        host = urllib.parse.urlparse(value).hostname or ""
        if "mouser" not in host.lower():
            continue

        # Prefer /ProductDetail/ URLs as they identify a specific part
        if "/productdetail/" in value.lower():
            product_detail_urls[key] = value
        else:
            urls[key] = value

    # Combine with ProductDetail URLs taking precedence
    result = {**urls, **product_detail_urls}
    return result


def list_component_mouser_urls(project_path: str | Path, reference: str) -> dict[str, Any]:
    """List all Mouser URLs (primary and alternates) for a component by
    reference designator. Returns both product detail links and raw properties,
    along with metadata about which are primary vs. alternates.
    """
    component = get_schematic_part(project_path, reference)
    properties = component.get("properties", {})

    all_urls = find_all_mouser_urls(properties)
    primary_url = find_mouser_url(properties)

    urls_with_metadata = []
    for field_name in sorted(all_urls.keys()):
        url = all_urls[field_name]
        is_primary = url == primary_url
        mpn_hint = _extract_mpn_hint_from_url(url)

        urls_with_metadata.append(
            {
                "field_name": field_name,
                "url": url,
                "is_primary": is_primary,
                "mpn_hint": mpn_hint,
            }
        )

    return {
        "reference": reference,
        "value": component.get("value", ""),
        "footprint": component.get("footprint", ""),
        "primary_url": primary_url,
        "alternate_count": len(urls_with_metadata) - 1 if primary_url else len(urls_with_metadata),
        "urls": urls_with_metadata,
    }


def find_mouser_url(properties: dict[str, str]) -> str | None:
    """Pick the best Mouser link out of a schematic part's properties (as
    returned by get_kicad_schematic_part). Prefers /ProductDetail/ links
    (product page URLs whose last path segment is a usable search hint) over
    plain datasheet PDFs, and prioritizes fields in this order:
    1. Mouser Part Number (primary product link)
    2. Mouser (alternate field name)
    3. Mouser Part Number Alt (alternate product link)
    4. Any other Mouser URL found
    """
    all_urls = find_all_mouser_urls(properties)
    if not all_urls:
        return None

    # Priority order for field names
    priority_fields = [
        "Mouser Part Number",
        "Mouser",
        "Mouser Part Number Alt",
    ]

    # Check priority fields first
    for field in priority_fields:
        if field in all_urls:
            return all_urls[field]

    # If no priority field matched, prefer ProductDetail links over datasheets
    for key, url in sorted(all_urls.items()):
        if "/productdetail/" in url.lower():
            return url

    # Fall back to any Mouser URL found
    return next(iter(all_urls.values())) if all_urls else None


# ---------------------------------------------------------------------------
# Mouser Search API
# ---------------------------------------------------------------------------


def _mouser_api_request(path: str, api_key: str, body: dict[str, Any], timeout: float = 15.0) -> dict[str, Any]:
    url = f"{_MOUSER_API_BASE}{path}?apiKey={urllib.parse.quote(api_key)}"
    request = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json", "Accept": "application/json"},
        method="POST",
    )
    for attempt in range(_RATE_LIMIT_MAX_RETRIES + 1):
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            error_body = exc.read().decode("utf-8", errors="replace")
            if exc.code == 403 and "toomanyrequests" in error_body.lower() and attempt < _RATE_LIMIT_MAX_RETRIES:
                time.sleep(_RATE_LIMIT_BACKOFF_SECONDS)
                continue
            raise RuntimeError(f"Mouser API returned HTTP {exc.code} for {path}: {error_body[:300]}") from exc
        except urllib.error.URLError as exc:
            raise RuntimeError(f"Could not reach Mouser API: {exc.reason}") from exc
    raise RuntimeError(f"Mouser API rate limit ('calls per minute') not cleared after {_RATE_LIMIT_MAX_RETRIES} retries for {path}")


def _mouser_api_search_keyword(keyword: str, api_key: str, records: int = 5) -> list[dict[str, Any]]:
    body = {
        "SearchByKeywordRequest": {
            "keyword": keyword,
            "records": records,
            "startingRecord": 0,
            "searchOptions": "",
            "searchWithYourSignUpLanguage": "",
        }
    }
    data = _mouser_api_request("/search/keyword", api_key, body)
    errors = (data or {}).get("Errors") or []
    if errors:
        messages = "; ".join(str(e.get("Message", e)) for e in errors)
        raise RuntimeError(f"Mouser API error: {messages}")
    return ((data or {}).get("SearchResults") or {}).get("Parts") or []


def _pick_best_part_match(parts: list[dict[str, Any]], mpn_hint: str | None) -> dict[str, Any] | None:
    """Keyword search can return several parts (different packaging/tape-and-reel
    variants, similar part numbers, etc) - prefer the one whose own manufacturer
    part number matches the hint exactly (ignoring case/punctuation), else just
    take the top-ranked result.
    """
    if not parts:
        return None
    if mpn_hint:
        target = re.sub(r"[^a-z0-9]", "", mpn_hint.lower())
        for part in parts:
            candidate = re.sub(r"[^a-z0-9]", "", str(part.get("ManufacturerPartNumber", "")).lower())
            if candidate and candidate == target:
                return part
    return parts[0]


_DESC_CAPACITANCE_RE = re.compile(r"\b\d+(?:\.\d+)?\s*(?:pF|nF|uF|µF|mF)\b", re.IGNORECASE)
_DESC_VOLTAGE_RE = re.compile(r"\b(\d+(?:\.\d+)?)\s*V(?:DC)?\b(?![a-zA-Z])")
_DESC_RESISTANCE_RE = re.compile(r"\b\d+(?:\.\d+)?\s*[kKmM]?\s*OHM\b", re.IGNORECASE)


def _parse_description_specs(description: str | None) -> dict[str, str]:
    """The Mouser Search API's `ProductAttributes` list is often sparse (just
    packaging/pack-qty, no electrical parametrics) for a given search result,
    but its free-text `Description` field usually still carries them, e.g.
    "Multilayer Ceramic Capacitors MLCC - SMD/SMT 50V 1800pF X7R 0402 5%" or
    "Thick Film Resistors - SMD 10K OHM 1%". Parsed as a lower-priority
    fallback source under the same field names used everywhere else.
    """
    if not description:
        return {}
    normalized = re.sub(r"\s+", " ", description).strip()
    specs: dict[str, str] = {}
    cap_match = _DESC_CAPACITANCE_RE.search(normalized)
    if cap_match:
        specs["Capacitance"] = cap_match.group(0)
    voltage_match = _DESC_VOLTAGE_RE.search(normalized)
    if voltage_match:
        specs["Voltage Rating"] = voltage_match.group(0)
    resistance_match = _DESC_RESISTANCE_RE.search(normalized)
    if resistance_match:
        specs["Resistance"] = resistance_match.group(0)
    return specs


# Standard EIA/IEC imperial case codes.
_EIA_IMPERIAL_CODES = frozenset(
    {
        "008004", "01005", "0201", "0402", "0603", "0805", "1008", "1111",
        "1206", "1210", "1218", "1806", "1812", "1825", "2010", "2512", "2515", "2920",
    }
)


def _extract_package_code(value: str | None) -> str | None:
    """Pull the imperial size code out of a Case/Package spec value, e.g.
    "0402 (1005 Metric)" -> "0402". Mouser lists the imperial code first with
    the metric equivalent trailing in parentheses, so the first token that
    matches a known EIA code is the imperial one.
    """
    if not value:
        return None
    for token in re.findall(r"\d{4,6}", value):
        if token in _EIA_IMPERIAL_CODES:
            return token
    return None


def _extract_package_code_from_mpn(mpn: str | None) -> str | None:
    """Series-prefix manufacturer part numbers often encode the imperial case
    size right after the prefix with no separator (Vishay "RCA0402...", KEMET
    "C0402...", Yageo "RC0402...", Murata "GRM0402...") - scan every digit run
    in the MPN for an embedded known EIA code as a last-resort package guess
    when nothing more explicit (a Case/Package spec or the description text)
    mentions it.
    """
    if not mpn:
        return None
    for run in re.findall(r"\d+", mpn):
        for length in (6, 5, 4):
            if len(run) < length:
                continue
            for start in range(len(run) - length + 1):
                token = run[start : start + length]
                if token in _EIA_IMPERIAL_CODES:
                    return token
    return None


def _specs_from_mouser_api_part(part: dict[str, Any]) -> dict[str, str]:
    specs: dict[str, str] = {}
    for attr in part.get("ProductAttributes") or []:
        name = attr.get("AttributeName")
        value = attr.get("AttributeValue")
        if name and value:
            specs[str(name).strip()] = str(value).strip()
    for key, value in _parse_description_specs(part.get("Description")).items():
        specs.setdefault(key, value)
    return specs


def _normalize_key(key: str) -> str:
    return re.sub(r"[^a-z0-9]+", " ", key.lower()).strip()


def _lookup_spec(specs: dict[str, str], *aliases: str) -> str | None:
    normalized = {_normalize_key(k): v for k, v in specs.items()}
    for alias in aliases:
        value = normalized.get(_normalize_key(alias))
        if value:
            return value
    return None


def _looks_like(value: str | None, *needles: str) -> bool:
    if not value:
        return False
    lowered = value.lower()
    return any(needle in lowered for needle in needles)


def _parse_price(price_text: Any) -> float | None:
    match = re.search(r"[\d.]+", str(price_text or ""))
    return float(match.group(0)) if match else None


def _price_breaks_from_part(part: dict[str, Any]) -> list[dict[str, Any]]:
    breaks: list[dict[str, Any]] = []
    for price_break in part.get("PriceBreaks") or []:
        quantity = price_break.get("Quantity")
        unit_price = _parse_price(price_break.get("Price"))
        if quantity is None or unit_price is None:
            continue
        breaks.append({"quantity": int(quantity), "unit_price": unit_price, "currency": price_break.get("Currency") or "USD"})
    breaks.sort(key=lambda entry: entry["quantity"])
    return breaks


def _unit_price_for_quantity(price_breaks: list[dict[str, Any]], quantity: int) -> dict[str, Any] | None:
    """Pick the price-break tier that applies when buying `quantity` units -
    Mouser's pricing model is that each listed tier's unit price holds from
    that quantity up to (but not including) the next tier, so this is the
    highest tier at or below `quantity`. Buying fewer than the lowest listed
    tier still costs at least that tier's unit price (there's no cheaper
    option), so the lowest tier is used as the floor.
    """
    if not price_breaks:
        return None
    applicable = price_breaks[0]
    for tier in price_breaks:
        if tier["quantity"] <= quantity:
            applicable = tier
        else:
            break
    return applicable


def _interpret_part(part: dict[str, Any]) -> dict[str, Any]:
    """Turn one Mouser API part record into the tool's field-level result.
    `Category` (Mouser's own part category, e.g. "Ceramic Capacitors" /
    "Chip Resistor - Surface Mount") is used as extra signal for detected_type
    and for telling MLCC/SMT construction apart from other cap/resistor types
    when the more specific attribute isn't present.
    """
    mpn = part.get("ManufacturerPartNumber")
    manufacturer = part.get("Manufacturer")
    category = part.get("Category")
    specs = _specs_from_mouser_api_part(part)

    capacitance = _lookup_spec(specs, "Capacitance")
    voltage_rating = _lookup_spec(
        specs, "Voltage Rating", "Voltage - Rated", "Rated Voltage", "Voltage Rating - DC", "Voltage Rating DC"
    )
    resistance = _lookup_spec(specs, "Resistance", "Resistance (Ohms)")
    package = _lookup_spec(specs, "Case/Package", "Package/Case", "Case / Package", "Package / Case", "Size/Dimension")
    capacitor_construction = _lookup_spec(specs, "Capacitor Type", "Dielectric", "Construction")
    mounting_style = _lookup_spec(specs, "Mounting Style", "Termination Style", "Product Type")

    if capacitance is not None or _looks_like(category, "capacitor"):
        detected_type = "capacitor"
    elif resistance is not None or _looks_like(category, "resistor"):
        detected_type = "resistor"
    elif specs or category:
        detected_type = "other"
    else:
        detected_type = "unknown"

    result: dict[str, Any] = {
        "manufacturer_part_number": mpn or "unknown",
        "manufacturer": manufacturer or "unknown",
        "mouser_part_number": part.get("MouserPartNumber") or "unknown",
        "category": category or "unknown",
        "availability": part.get("Availability") or "unknown",
        "availability_in_stock": part.get("AvailabilityInStock"),
        "lifecycle_status": part.get("LifecycleStatus") or None,
        "product_detail_url": part.get("ProductDetailUrl") or "",
        "price_breaks": _price_breaks_from_part(part),
        "detected_type": detected_type,
        "capacitance": "unsupported",
        "voltage_rating": "unsupported",
        "resistance": "unsupported",
        "package_size_inch": "unsupported",
        "raw_specifications": specs,
    }

    if detected_type == "capacitor":
        result["capacitance"] = capacitance or "unknown"
        result["voltage_rating"] = voltage_rating or "unknown"
        construction_signal = " ".join(s for s in (capacitor_construction, category) if s)
        if construction_signal and not _looks_like(construction_signal, "ceramic", "mlcc"):
            result["package_size_inch"] = "unsupported"
        else:
            code = _extract_package_code(package) or _extract_package_code_from_mpn(mpn)
            result["package_size_inch"] = code or "unknown"
    elif detected_type == "resistor":
        result["resistance"] = resistance or "unknown"
        mounting_signal = " ".join(s for s in (mounting_style, category) if s)
        if mounting_signal and not _looks_like(mounting_signal, "surface mount", "smd", "smt", "chip"):
            result["package_size_inch"] = "unsupported"
        else:
            code = _extract_package_code(package) or _extract_package_code_from_mpn(mpn)
            result["package_size_inch"] = code or "unknown"
    elif detected_type == "unknown":
        result["capacitance"] = "unknown"
        result["voltage_rating"] = "unknown"
        result["resistance"] = "unknown"
        result["package_size_inch"] = "unknown"
    # detected_type == "other": every field stays "unsupported" (set above)

    return result


def lookup_mouser_part(url: str) -> dict[str, Any]:
    """Look up a Mouser product's manufacturer part number, stock/lifecycle
    status, and type-specific electrical specs via Mouser's official Search
    API. Pass a Mouser product link, e.g. one found in a schematic part's
    `Datasheet`/`Mouser Part Number`/`Mouser Price/Stock` property via
    get_kicad_schematic_part (find_mouser_url picks the best one out of a
    part's properties). Only ever talks to mouser.* hosts.

    Requires MOUSER_API_KEY (repo-root .env - see .env.example); raises with
    a clear message asking for one if it's not configured, rather than
    falling back to scraping the product page (which Mouser's bot protection
    blocks for a meaningful fraction of scripted requests).

    `detected_type` is "capacitor", "resistor", "other" (confidently neither -
    an inductor, diode, connector, etc), or "unknown" (couldn't tell at all).
    Each of capacitance/voltage_rating/resistance/package_size_inch is then
    either a scraped value, "unsupported" (doesn't apply to this part's
    detected type - e.g. resistance on a capacitor, or a package code on a
    through-hole/electrolytic part that doesn't use 0402-style imperial
    sizing), or "unknown" (should apply but couldn't be found/parsed).
    `raw_specifications` carries every spec this lookup found under Mouser's
    own field names, in case the mapping above misses one. `availability`/
    `lifecycle_status` support the out-of-stock/NRND report.

    Results are cached to disk (.mouser_cache.json, gitignored) per part for
    one hour - a repeat lookup for the same part within that window is served
    from the cache (`cache_hit: true`) instead of calling the API again.
    """
    _validate_mouser_url(url)
    cached = _get_cached_mouser_result(url)
    if cached is not None:
        return cached

    api_key = _require_mouser_api_key()
    mpn_hint = _extract_mpn_hint_from_url(url)

    parts = _mouser_api_search_keyword(mpn_hint or url, api_key)
    part = _pick_best_part_match(parts, mpn_hint)
    if part is None:
        raise RuntimeError(f"Mouser API returned no matching parts for {url!r} (keyword {mpn_hint!r})")

    result = _interpret_part(part)
    _store_mouser_cache_result(url, result)
    result = dict(result)
    result["url"] = url
    result["cache_hit"] = False
    return result


def bulk_list_component_mouser_urls(project_path: str | Path, references: list[str] | None = None) -> dict[str, Any]:
    """List all Mouser URLs for many schematic parts in one call - useful for
    auditing which parts have alternates, or to spot parts missing Mouser links
    entirely. Defaults to one representative reference per unique part from
    list_schematic_parts; pass `references` for a specific subset.
    """
    representative_to_group: dict[str, dict[str, Any]] = {}
    if references is None:
        parts = list_schematic_parts(project_path)["parts"]
        references = []
        for part in parts:
            if not part["references"]:
                continue
            representative = part["references"][0]
            references.append(representative)
            representative_to_group[representative] = part

    results: list[dict[str, Any]] = []
    no_mouser_link: list[dict[str, Any]] = []

    for reference in references:
        try:
            component_mouser_info = list_component_mouser_urls(project_path, reference)
        except KeyError as exc:
            no_mouser_link.append(
                {
                    "reference": reference,
                    "error": str(exc),
                }
            )
            continue

        if not component_mouser_info["urls"]:
            group = representative_to_group.get(reference)
            all_refs = group["references"] if group else [reference]
            quantity = group["quantity"] if group else 1

            no_mouser_link.append(
                {
                    "reference": reference,
                    "all_references": all_refs,
                    "quantity": quantity,
                    "value": component_mouser_info["value"],
                    "reason": "no Mouser link found",
                }
            )
            continue

        group = representative_to_group.get(reference)
        all_refs = group["references"] if group else [reference]
        quantity = group["quantity"] if group else 1

        results.append(
            {
                "reference": reference,
                "all_references": all_refs,
                "quantity": quantity,
                "value": component_mouser_info["value"],
                "footprint": component_mouser_info["footprint"],
                "primary_url": component_mouser_info["primary_url"],
                "alternate_count": component_mouser_info["alternate_count"],
                "urls": component_mouser_info["urls"],
            }
        )

    return {
        "checked_count": len(references),
        "with_mouser_link_count": len(results),
        "with_alternates_count": sum(1 for r in results if r["alternate_count"] > 0),
        "no_mouser_link_count": len(no_mouser_link),
        "results": results,
        "no_mouser_link": no_mouser_link,
    }


def bulk_lookup_mouser_parts(project_path: str | Path, references: list[str] | None = None) -> dict[str, Any]:
    """Look up Mouser data for many schematic parts in one call instead of one
    tool round-trip per part - the batch-speed entry point for auditing an
    entire schematic's worth of parts (MPN verification, stock/NRND report).

    Defaults to one representative reference per unique part from
    list_schematic_parts (i.e. every distinct Value+Footprint group once, not
    every individual placed instance); pass `references` to look up a
    specific subset instead. Each part missing a discoverable Mouser link, or
    whose lookup fails for any reason (no API match, transient API error), is
    reported under `skipped`/`errors` rather than aborting the whole batch.
    """
    representative_to_group: dict[str, dict[str, Any]] = {}
    if references is None:
        parts = list_schematic_parts(project_path)["parts"]
        references = []
        for part in parts:
            if not part["references"]:
                continue
            representative = part["references"][0]
            references.append(representative)
            representative_to_group[representative] = part

    results: list[dict[str, Any]] = []
    skipped: list[dict[str, Any]] = []
    errors: list[dict[str, Any]] = []

    made_api_call = False
    for reference in references:
        try:
            component = get_schematic_part(project_path, reference)
        except KeyError as exc:
            errors.append({"reference": reference, "error": str(exc)})
            continue

        group = representative_to_group.get(reference)
        all_references = group["references"] if group else [reference]
        quantity = group["quantity"] if group else 1

        mouser_url = find_mouser_url(component.get("properties", {}))
        if not mouser_url:
            skipped.append(
                {
                    "reference": reference,
                    "all_references": all_references,
                    "quantity": quantity,
                    "value": component.get("value", ""),
                    "reason": "no Mouser link in properties",
                }
            )
            continue

        if _get_cached_mouser_result(mouser_url) is None:
            if made_api_call:
                time.sleep(_BULK_REQUEST_DELAY_SECONDS)  # stay under Mouser's per-minute call cap
            made_api_call = True

        try:
            mouser = lookup_mouser_part(mouser_url)
        except Exception as exc:
            errors.append(
                {
                    "reference": reference,
                    "all_references": all_references,
                    "quantity": quantity,
                    "value": component.get("value", ""),
                    "mouser_url": mouser_url,
                    "error": str(exc),
                }
            )
            continue

        results.append(
            {
                "reference": reference,
                "all_references": all_references,
                "quantity": quantity,
                "value": component.get("value", ""),
                "footprint": component.get("footprint", ""),
                "schematic_properties": component.get("properties", {}),
                "mouser_url": mouser_url,
                "mouser": mouser,
            }
        )

    return {
        "requested_count": len(references),
        "found_count": len(results),
        "skipped_count": len(skipped),
        "error_count": len(errors),
        "results": results,
        "skipped": skipped,
        "errors": errors,
    }


def _canonical_mpn_from_properties(properties: dict[str, str]) -> str | None:
    for key, value in properties.items():
        if re.sub(r"[^a-z0-9]", "", key.lower()) == "manufacturerpartnumber" and value:
            return value
    return None


def _normalize_mpn_for_compare(value: str | None) -> str:
    return re.sub(r"[^a-z0-9]", "", (value or "").lower())


def audit_manufacturer_part_numbers(project_path: str | Path, references: list[str] | None = None) -> dict[str, Any]:
    """Cross-check each schematic part's `Manufacturer_Part_Number` property
    against the manufacturer part number Mouser's Search API actually returns
    for that part's own Mouser link - catches typos, copy-paste errors, or a
    stale value left over from swapping which exact part a symbol points to.
    Run normalize_manufacturer_part_number_properties first so parts that
    only carry the MPN under a differently-named property (PROD_ID, MPN,
    etc.) get picked up here too, instead of showing up as "missing".

    Parts with no Mouser link at all, or whose Mouser lookup failed, aren't
    compared (nothing to compare against) - see `skipped`/`errors`, carried
    through unchanged from bulk_lookup_mouser_parts.
    """
    bulk = bulk_lookup_mouser_parts(project_path, references=references)

    matched: list[dict[str, Any]] = []
    mismatched: list[dict[str, Any]] = []
    missing_schematic_mpn: list[dict[str, Any]] = []

    for entry in bulk["results"]:
        schematic_mpn = _canonical_mpn_from_properties(entry["schematic_properties"])
        mouser_mpn = entry["mouser"].get("manufacturer_part_number")
        mouser_mpn = None if mouser_mpn in (None, "unknown") else mouser_mpn
        if mouser_mpn is None:
            continue  # Mouser lookup didn't identify an MPN either - nothing to compare

        row = {
            "reference": entry["reference"],
            "all_references": entry["all_references"],
            "value": entry["value"],
            "schematic_manufacturer_part_number": schematic_mpn,
            "mouser_manufacturer_part_number": mouser_mpn,
            "mouser_url": entry["mouser_url"],
        }
        if schematic_mpn is None:
            missing_schematic_mpn.append(row)
        elif _normalize_mpn_for_compare(schematic_mpn) == _normalize_mpn_for_compare(mouser_mpn):
            matched.append(row)
        else:
            mismatched.append(row)

    return {
        "checked_count": len(bulk["results"]),
        "matched_count": len(matched),
        "mismatched_count": len(mismatched),
        "missing_schematic_mpn_count": len(missing_schematic_mpn),
        "skipped_count": bulk["skipped_count"],
        "error_count": bulk["error_count"],
        "matched": matched,
        "mismatched": mismatched,
        "missing_schematic_mpn": missing_schematic_mpn,
        "skipped": bulk["skipped"],
        "errors": bulk["errors"],
    }


_LIFECYCLE_FLAG_MARKERS = (
    "nrnd",
    "not recommended",
    "obsolete",
    "eol",
    "end of life",
    "discontinued",
    "last time buy",
)


def _availability_in_stock_count(mouser_result: dict[str, Any]) -> int | None:
    raw = mouser_result.get("availability_in_stock")
    if raw in (None, ""):
        return None
    try:
        return int(re.sub(r"[^\d]", "", str(raw)) or "0")
    except ValueError:
        return None


def generate_mouser_stock_report(
    project_path: str | Path,
    report_path: str | Path | None = None,
    references: list[str] | None = None,
) -> dict[str, Any]:
    """Run bulk_lookup_mouser_parts across the schematic's unique parts (or a
    given subset) and write a Markdown report listing a BOM cost estimate
    (one board's worth, using Mouser's own quantity-break pricing) plus every
    part Mouser currently shows as out of stock or lifecycle-flagged (Not
    Recommended for New Designs, obsolete, discontinued, etc), so those can
    be addressed before fabrication/ordering. Defaults to
    `mouser_stock_report.md` at the project root.

    Mouser's API only sometimes populates `LifecycleStatus` - a part with no
    status isn't necessarily healthy, it just means Mouser didn't report
    anything either way, and is left out of `not_recommended` accordingly.
    Parts with no Mouser link, a failed lookup, or no price-break data at all
    are excluded from the cost total and listed separately so the total's
    coverage is clear rather than silently understated.
    """
    schematic_dir = Path(list_schematic_parts(project_path)["schematic_dir"])
    output_path = Path(report_path) if report_path is not None else schematic_dir / "mouser_stock_report.md"

    bulk = bulk_lookup_mouser_parts(project_path, references=references)

    out_of_stock: list[dict[str, Any]] = []
    not_recommended: list[dict[str, Any]] = []
    bom_lines: list[dict[str, Any]] = []
    bom_unpriced: list[dict[str, Any]] = []
    bom_total_by_currency: dict[str, float] = {}

    for entry in bulk["results"]:
        mouser = entry["mouser"]
        row = {
            "reference": entry["reference"],
            "all_references": entry["all_references"],
            "quantity": entry["quantity"],
            "value": entry["value"],
            "manufacturer_part_number": mouser.get("manufacturer_part_number"),
            "manufacturer": mouser.get("manufacturer"),
            "availability": mouser.get("availability"),
            "lifecycle_status": mouser.get("lifecycle_status"),
            "mouser_url": entry["mouser_url"],
        }
        in_stock_count = _availability_in_stock_count(mouser)
        if in_stock_count is not None and in_stock_count <= 0:
            out_of_stock.append(row)
        if mouser.get("lifecycle_status") and _looks_like(mouser.get("lifecycle_status"), *_LIFECYCLE_FLAG_MARKERS):
            not_recommended.append(row)

        tier = _unit_price_for_quantity(mouser.get("price_breaks") or [], entry["quantity"])
        if tier is None:
            bom_unpriced.append(row)
            continue
        line_cost = round(tier["unit_price"] * entry["quantity"], 4)
        bom_total_by_currency[tier["currency"]] = round(bom_total_by_currency.get(tier["currency"], 0.0) + line_cost, 4)
        bom_lines.append(
            {
                **row,
                "unit_price": tier["unit_price"],
                "price_break_quantity": tier["quantity"],
                "currency": tier["currency"],
                "line_cost": line_cost,
            }
        )

    lines = [
        "# Mouser Stock & Lifecycle Report",
        "",
        f"Checked {bulk['found_count']} of {bulk['requested_count']} unique parts "
        f"({bulk['skipped_count']} had no Mouser link, {bulk['error_count']} failed to look up).",
        "",
        "## Bill of Materials Cost Estimate",
        "",
    ]
    if bom_lines:
        totals_text = ", ".join(f"{currency} {total:,.2f}" for currency, total in sorted(bom_total_by_currency.items()))
        lines.append(f"**Estimated total (one board): {totals_text}**, from {len(bom_lines)} priced line items.")
        if bom_unpriced:
            lines.append(f"({len(bom_unpriced)} part(s) excluded - no Mouser price-break data; see below.)")
        lines.append("")
        lines.append("| Reference(s) | Value | Qty | Unit Price | Line Cost | MPN | Mouser Link |")
        lines.append("|---|---|---|---|---|---|---|")
        for row in sorted(bom_lines, key=lambda r: r["line_cost"], reverse=True):
            refs = ", ".join(row["all_references"])
            lines.append(
                f"| {refs} | {row['value']} | {row['quantity']} | {row['currency']} {row['unit_price']:.4f} | "
                f"{row['currency']} {row['line_cost']:.2f} | {row['manufacturer_part_number']} | {row['mouser_url']} |"
            )
    else:
        lines.append("No priced line items.")
    if bom_unpriced:
        lines += ["", "### Excluded from cost total (no price-break data)", ""]
        lines.append("| Reference(s) | Value | Qty | Mouser Link |")
        lines.append("|---|---|---|---|")
        for row in bom_unpriced:
            refs = ", ".join(row["all_references"])
            lines.append(f"| {refs} | {row['value']} | {row['quantity']} | {row['mouser_url']} |")
    lines += ["", "## Out of Stock", ""]
    if out_of_stock:
        lines.append("| Reference(s) | Value | MPN | Manufacturer | Availability | Mouser Link |")
        lines.append("|---|---|---|---|---|---|")
        for row in out_of_stock:
            refs = ", ".join(row["all_references"])
            lines.append(
                f"| {refs} | {row['value']} | {row['manufacturer_part_number']} | {row['manufacturer']} | "
                f"{row['availability']} | {row['mouser_url']} |"
            )
    else:
        lines.append("None found.")
    lines += ["", "## Not Recommended for New Designs / Obsolete / Discontinued", ""]
    if not_recommended:
        lines.append("| Reference(s) | Value | MPN | Manufacturer | Lifecycle Status | Mouser Link |")
        lines.append("|---|---|---|---|---|---|")
        for row in not_recommended:
            refs = ", ".join(row["all_references"])
            lines.append(
                f"| {refs} | {row['value']} | {row['manufacturer_part_number']} | {row['manufacturer']} | "
                f"{row['lifecycle_status']} | {row['mouser_url']} |"
            )
    else:
        lines.append("None found.")
    lines += [
        "",
        "---",
        "Note: Mouser's Search API only sometimes populates lifecycle status; a part not listed here "
        "isn't guaranteed current, it just wasn't flagged by Mouser at the time of this report.",
        "",
    ]

    output_path.write_text("\n".join(lines), encoding="utf-8")

    return {
        "report_path": str(output_path),
        "checked_count": bulk["found_count"],
        "bom_total_by_currency": bom_total_by_currency,
        "bom_priced_line_count": len(bom_lines),
        "bom_unpriced_count": len(bom_unpriced),
        "bom_lines": bom_lines,
        "bom_unpriced": bom_unpriced,
        "out_of_stock_count": len(out_of_stock),
        "not_recommended_count": len(not_recommended),
        "out_of_stock": out_of_stock,
        "not_recommended": not_recommended,
        "skipped": bulk["skipped"],
        "errors": bulk["errors"],
    }
