"""Convert a point-cloud file to binary PLY with CloudCompare."""

import tapioca


@tapioca.command(
    title="CloudCompare Convert",
    category="Examples",
    description="Calls CloudCompare out of process and saves a binary PLY.",
    runtime="embedded",
    timeout_s=600,
)
def run(
    executable_path: tapioca.FilePath(extensions=("exe",)) = (
        r"C:\Program Files\CloudCompare\CloudCompare.exe"
    ),
    input_path: tapioca.FilePath(extensions=("e57", "las", "laz", "ply")) = "",
    output_path: tapioca.FilePath(mode="save", extensions=("ply",)) = "",
):
    if not input_path.strip():
        tapioca.ui.text("Choose an input point-cloud file.")
        return

    output = output_path.strip() or tapioca.paths.output_path("cloudcompare_output.ply")
    result = tapioca.call(
        "Tapioca.RunCloudCompare",
        {
            "executablePath": executable_path,
            "inputPath": input_path,
            "outputPath": output,
        },
        raise_on_error=False,
    )
    data = result.data or {}
    if result.ok and data.get("succeeded"):
        tapioca.ui.text("CloudCompare wrote:\n%s" % data.get("outputPath", output))
        return

    failure = data.get("failureReason") if result.ok else result.error
    tapioca.ui.text("CloudCompare failed:\n%s" % failure)
