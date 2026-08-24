"""Measure horizontal section loops through selected 3D elements."""

import tapioca


@tapioca.command(
    title="Horizontal Slice Report",
    category="Examples",
    description="Measures section-loop areas and perimeters at a world elevation.",
    needs_selection=True,
)
def run(elevation: tapioca.Float(unit="m") = 1.0):
    guids = tapioca.selection.get()
    if not guids:
        tapioca.ui.text("Nothing is selected.")
        return

    sections = tapioca.geometry.slice_z(elevation, guids=guids)
    if not sections:
        tapioca.ui.text("No selected geometry crosses elevation %.3f m." % elevation)
        return

    lines = ["Horizontal slice at %.3f m" % elevation]
    total_area = 0.0
    for section in sections:
        area = sum(tapioca.elements.polygon_area(loop) for loop in section["loops"])
        perimeter = sum(
            tapioca.elements.polygon_perimeter(loop) for loop in section["loops"]
        )
        total_area += area
        lines.append(
            "%s | %d loop(s) | area %.2f m2 | perimeter %.2f m"
            % (section["guid"], len(section["loops"]), area, perimeter)
        )

    lines.extend(["", "Combined loop area: %.2f m2" % total_area])
    tapioca.ui.text("\n".join(lines))
