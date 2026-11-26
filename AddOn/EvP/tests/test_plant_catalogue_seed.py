"""The plant catalogue seed is two tracked JSON files (#278).

PlantCatalogueSeed.plants.json and PlantCatalogueSeed.profiles.json are the
source; ArchViz/PlantCatalogueSeed.inc, which the add-on compiles in, is
generated from them. These tests fail when the .inc drifts from the JSON and
when the seed breaks a rule the generator checks.
"""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

EVP = Path(__file__).resolve().parents[1]
GENERATOR = EVP / "tools" / "generate_plant_catalogue_seed.py"
ARCHVIZ = EVP / "Sources" / "AddOn" / "ArchViz"


def _generator():
    spec = importlib.util.spec_from_file_location("generate_plant_catalogue_seed", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_the_compiled_seed_is_current():
    assert _generator().main(["--check"]) == 0, "run tools/generate_plant_catalogue_seed.py"


def test_the_seed_is_the_vilnius_transcription():
    plants = json.loads((ARCHVIZ / "PlantCatalogueSeed.plants.json").read_text(encoding="utf-8"))
    assert plants["catalogueId"] == "vilnius-2024"
    assert plants["sources"][0]["year"] == 2024
    assert len(plants["plants"]) == 269
    for plant in plants["plants"]:
        assert plant["section"] in {"trees", "shrubs", "perennials"}, plant["id"]
        assert plant["sourceRefs"][0]["page"] in range(7, 35), plant["id"]


def test_pieces_stay_below_the_compiler_limit():
    generator = _generator()
    long = "".join(f'  "line-{i}": "{"x" * 90}",\n' for i in range(400))
    pieces = generator._pieces(long)
    assert len(pieces) > 1
    assert "".join(pieces) == long
    assert all(len(p.encode("utf-8")) <= generator.PIECE_BYTES for p in pieces)
