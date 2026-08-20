"""Smallest possible Tapioca command: inputs in, text result out."""

import tapioca


@tapioca.command(
    title="Hello Tapioca",
    category="Examples",
    description="Demonstrates a text input, a bounded integer, and a copyable result.",
)
def run(
    name: tapioca.Text = "Archicad",
    repetitions: tapioca.Int(minimum=1, maximum=5) = 1,
):
    lines = ["Hello, %s!" % name for _ in range(repetitions)]
    tapioca.ui.text("\n".join(lines))
