"""Reusable fields and input groups for schema-form commands.

Controls belong here when their declaration has behavior that commands should
not restate. Commands may compose the model groups through normal inheritance:

    from tapioca.inputs import FileIOInputs, read_file, save_file

    class Inputs(FileIOInputs):
        input_path: str = read_file("csv", "xlsx", title="Source")
        output_path: str = save_file("json", "csv", title="Report")

This module is intentionally lazy: importing ``tapioca`` does not import it,
because schema models require Pydantic while the AST-only scanner does not.
"""

from .schema import Field, NodeModel, port

__all__ = [
    "INPUTS_VERSION",
    "FileIOInputs",
    "ReadFileInputs",
    "SaveFileInputs",
    "file_path",
    "read_file",
    "save_file",
]

# Semantic version of the reusable input-control library. Increment MAJOR for
# incompatible field/model contracts, MINOR for new controls or groups, and
# PATCH for compatible fixes.
INPUTS_VERSION = "1.0.0"


def file_path(*extensions, mode="open", default="", title=None, description=None, show_when=None):
    """Return one native file-picker field for a ``NodeModel``.

    ``extensions`` are the formats offered by the native dialog. Read mode
    selects an existing file; save mode selects a destination. The model value
    is always a normal path string including the selected format suffix.
    """
    field_options = {
        "default": default,
        "json_schema_extra": port(
            control="filepath",
            mode=mode,
            extensions=extensions,
            show_when=show_when,
        ),
    }
    if title is not None:
        field_options["title"] = title
    if description is not None:
        field_options["description"] = description
    return Field(**field_options)


def read_file(*extensions, default="", title=None, description=None, show_when=None):
    """Return a field backed by the native existing-file dialog."""
    return file_path(
        *extensions,
        mode="open",
        default=default,
        title=title,
        description=description,
        show_when=show_when,
    )


def save_file(*extensions, default="", title=None, description=None, show_when=None):
    """Return a field backed by the native destination-file dialog."""
    return file_path(
        *extensions,
        mode="save",
        default=default,
        title=title,
        description=description,
        show_when=show_when,
    )


class ReadFileInputs(NodeModel):
    """Composable group for a command that reads one file."""

    input_path: str = read_file(title="Input file")


class SaveFileInputs(NodeModel):
    """Composable group for a command that writes one file."""

    output_path: str = save_file(title="Output file")


class FileIOInputs(ReadFileInputs, SaveFileInputs):
    """Composable input and output file rows for converter-style commands."""
