"""Z-up seasonal report fixture. The first material is the 40 x 40 m receiver."""
from pathlib import Path
import sys


def write(folder):
    folder.mkdir(parents=True, exist_ok=True)
    rows = ["mtllib courtyard.mtl", "usemtl Ground",
            "v -20 -20 0", "v 20 -20 0", "v 20 20 0", "v -20 20 0", "f 1 2 3 4", "usemtl Masonry"]
    count = 4
    for x, y, w, d, h in [(-17, -11, 5, 18, 6), (-17, 8, 14, 5, 7), (-1, 8, 14, 5, 5),
                           (10, -9, 5, 16, 8), (-7, -14, 14, 4, 4), (-4, -3, 6, 4, 3)]:
        for z in (0, h):
            for a, b in ((0, 0), (w, 0), (w, d), (0, d)):
                rows.append(f"v {x+a} {y+b} {z}")
        for face in ((4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)):
            rows.append("f " + " ".join(str(count + v + 1) for v in face))
        count += 8
    (folder / "courtyard.obj").write_text("\n".join(rows) + "\n")
    (folder / "courtyard.mtl").write_text("newmtl Ground\nKd .85 .85 .85\nnewmtl Masonry\nKd .88 .88 .88\n")


if __name__ == "__main__":
    write(Path(sys.argv[1]))
