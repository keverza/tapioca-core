"""Generate the compiled-in plant catalogue seed from its JSON files (#278).

The seed the Planting tab writes into %LOCALAPPDATA%\\Tapioca\\Planting on first
use is two tracked files beside the catalogue code:

- Sources/AddOn/ArchViz/PlantCatalogueSeed.plants.json
- Sources/AddOn/ArchViz/PlantCatalogueSeed.profiles.json

The plants are the Vilnius 2024 planting standard's tables (pp. 6-34); the
profiles are its sunlight thresholds. They are edited by hand and reviewed in
git. The add-on carries them as text in `ArchViz/PlantCatalogueSeed.inc`,
generated here and never edited by hand.

    python tools/generate_plant_catalogue_seed.py          # write
    python tools/generate_plant_catalogue_seed.py --check  # fail if the .inc is stale

This checks only what a generator can (valid JSON, schema version, unique ids,
known light classes and habits, sources named by every reference). The C++
reader (`ParseCatalogue`) is the full validator, and an offline test runs it on
the compiled seed.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

EVP = Path(__file__).resolve().parents[1]
ARCHVIZ = EVP / "Sources" / "AddOn" / "ArchViz"
PLANTS = ARCHVIZ / "PlantCatalogueSeed.plants.json"
PROFILES = ARCHVIZ / "PlantCatalogueSeed.profiles.json"
TARGET = ARCHVIZ / "PlantCatalogueSeed.inc"

LIGHT = {"shade", "partial", "full"}
HABITS = {"groundcover", "perennial", "grass", "fern", "bulb", "annual", "climber", "shrub", "tree"}
SECTIONS = {"trees", "shrubs", "perennials"}
MOISTURE = {"dry", "moist", "wet"}
LAYERS = {"D", "S", "Str"}
SITES = {
    "greenStrip",
    "roadside",
    "streets",
    "alleys",
    "squaresParks",
    "roundabouts",
    "urbanForest",
    "riverside",
    "onSlabs",
    "containers",
}
# PlantTraits.cpp's blocks and keys; values are checked by the C++ reader.
TRAIT_KEYS = {
    "ecology": {
        "ellenbergLight",
        "ellenbergMoisture",
        "ellenbergReaction",
        "ellenbergNutrients",
        "ellenbergSalinity",
        "pHMin",
        "pHMax",
        "nutrientPreference",
        "drainage",
        "hardiness",
    },
    "competition": {"establishmentSpeed", "competitiveVigour", "spreadMode", "lateralSpread", "selfSeeding"},
    "roots": {"depthClass", "architecture", "competitiveIntensity"},
    "canopy": {"density", "transmittance"},
    "phenology": {"emergenceMonths", "senescenceMonths", "leafOnMonths"},
    "lifecycle": {"lifespan", "maintenanceSensitivity"},
    "regulatory": {"status", "scope", "note"},
}
# MSVC refuses a string literal piece over 16 380 bytes (C2026); stay well below.
PIECE_BYTES = 8000
DELIMITER = "seed"


def _check_plants(document: dict) -> None:
    if document.get("schemaVersion") != 1:
        raise SystemExit(f"{PLANTS.name}: schemaVersion must be 1")
    sources = {s["id"] for s in document.get("sources", [])}
    seen = set()
    for plant in document["plants"]:
        ident = plant["id"]
        if ident in seen:
            raise SystemExit(f"{PLANTS.name}: {ident}: duplicate id")
        seen.add(ident)
        if not plant.get("scientificName"):
            raise SystemExit(f"{PLANTS.name}: {ident}: scientificName is required")
        habit = plant.get("habit")
        if habit is not None and habit not in HABITS:
            raise SystemExit(f"{PLANTS.name}: {ident}: unknown habit {habit}")
        section = plant.get("section")
        if section is not None and section not in SECTIONS:
            raise SystemExit(f"{PLANTS.name}: {ident}: unknown section {section}")
        for moisture in plant.get("moistureClasses", []):
            if moisture not in MOISTURE:
                raise SystemExit(f"{PLANTS.name}: {ident}: unknown moisture class {moisture}")
        for layer in plant.get("plantingLayers", []):
            if layer not in LAYERS:
                raise SystemExit(f"{PLANTS.name}: {ident}: unknown planting layer {layer}")
        for site in plant.get("sites", []):
            if site not in SITES:
                raise SystemExit(f"{PLANTS.name}: {ident}: unknown site {site}")
        for site in plant.get("siteSoils", {}):
            if site not in SITES:
                raise SystemExit(f"{PLANTS.name}: {ident}: unknown site {site}")
        for light in plant.get("light", {}).get("acceptedClasses", []):
            if light not in LIGHT:
                raise SystemExit(f"{PLANTS.name}: {ident}: unknown light class {light}")
        for ref in plant.get("sourceRefs", []):
            if ref["sourceId"] not in sources:
                raise SystemExit(f"{PLANTS.name}: {ident}: unknown source {ref['sourceId']}")
        for group, keys in TRAIT_KEYS.items():
            for key in plant.get(group) or {}:
                if key not in keys:
                    raise SystemExit(f"{PLANTS.name}: {ident}: unknown trait {group}.{key}")
        for evidence in plant.get("evidence") or []:
            if evidence.get("sourceId") not in sources:
                raise SystemExit(f"{PLANTS.name}: {ident}: evidence names unknown source {evidence.get('sourceId')}")


def _check_profiles(document: dict) -> None:
    if document.get("schemaVersion") != 1:
        raise SystemExit(f"{PROFILES.name}: schemaVersion must be 1")
    ids = [p["id"] for p in document["profiles"]]
    if not ids or len(ids) != len(set(ids)):
        raise SystemExit(f"{PROFILES.name}: at least one profile, ids unique")


def _pieces(text: str) -> list[str]:
    if f'){DELIMITER}"' in text:
        raise SystemExit(f'the seed may not contain ){DELIMITER}"')
    pieces, current = [], ""
    for line in text.splitlines(keepends=True):
        if current and len((current + line).encode("utf-8")) > PIECE_BYTES:
            pieces.append(current)
            current = ""
        current += line
    pieces.append(current)
    return pieces


def _array(name: str, text: str) -> list[str]:
    lines = [f"constexpr const char* {name}[] = {{"]
    for piece in _pieces(text):
        lines.append(f'    R"{DELIMITER}({piece}){DELIMITER}",')
    lines.append("};")
    return lines


def render() -> str:
    plants = PLANTS.read_text(encoding="utf-8")
    profiles = PROFILES.read_text(encoding="utf-8")
    _check_plants(json.loads(plants))
    _check_profiles(json.loads(profiles))
    lines = [
        "// Generated by AddOn/EvP/tools/generate_plant_catalogue_seed.py from",
        "// PlantCatalogueSeed.plants.json and PlantCatalogueSeed.profiles.json. Do not edit.",
        "// clang-format off",
        *_array("kSeedPlantsPieces", plants),
        *_array("kSeedProfilesPieces", profiles),
        "// clang-format on",
        "",
    ]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail if the generated file is stale")
    args = parser.parse_args(argv)
    text = render()
    current = TARGET.read_text(encoding="utf-8").replace("\r\n", "\n") if TARGET.exists() else ""
    if args.check:
        if current != text:
            print(f"{TARGET.relative_to(EVP)} is stale: run tools/generate_plant_catalogue_seed.py")
            return 1
        return 0
    if current != text:
        TARGET.write_bytes(text.encode("utf-8"))
        print(f"wrote {TARGET.relative_to(EVP)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
