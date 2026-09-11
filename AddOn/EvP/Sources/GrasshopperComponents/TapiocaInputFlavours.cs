using System;
using System.Collections.Generic;
using System.Windows.Forms;

using Grasshopper.Kernel;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// The Archicad flavours a Tapioca input can take, and the right-click menu
    /// that chooses one.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ ONE COMPONENT PER DATA TYPE, A FLAVOUR PER ARCHICAD MEANING. A layer,
    /// a fill, a building material, a wall composite and a beam profile are all
    /// ONE TEXT VALUE on the wire; a storey and a pen are both ONE INTEGER. What
    /// differs between them is the control the panel puts in front of the user —
    /// which is a property of the input, not of its type. Fourteen separate
    /// parameter classes said otherwise, and made an author hunt the ribbon for
    /// "the layer one" when what they wanted was to say "this text is a layer".
    /// </para>
    /// <para>
    /// ⚠️ AND THE NATIVE PICKERS ARE KEPT EXACTLY AS THEY WERE. The flavour is
    /// simply what the schema declares as the input's type, and the panel's
    /// contract has always been that type name: "attribute:Layer" gets
    /// Archicad's own layer chooser through the identical path a dedicated layer
    /// parameter used. Nothing in the panel had to learn a new concept.
    /// </para>
    /// <para>
    /// ⚠️ A FLAVOUR THE PANEL CANNOT DRAW YET IS STILL OFFERED, AND DEGRADES
    /// HONESTLY. FilePath, View, Database, LibraryPart, Favourite and
    /// ProjectField are not attribute pickers — they take the Python panel's
    /// other branches (file dialog, Navigator browser, catalogue picker) and the
    /// workflow band has not learned those yet. An input that declares one gets a
    /// text field with its type written beside the label, which is what the band
    /// already does with any type it does not know: the value still reaches the
    /// definition, and the row says what it is meant to be.
    /// </para>
    /// </remarks>
    internal static class TapiocaInputFlavours
    {
        /// <summary>
        /// One offerable flavour: what the menu says, and what the schema
        /// declares.
        /// </summary>
        internal sealed class Flavour
        {
            internal Flavour(string label, string typeName)
            {
                Label = label;
                TypeName = typeName;
            }

            internal string Label { get; private set; }

            /// <summary>Empty for the plain type.</summary>
            internal string TypeName { get; private set; }
        }

        /// <summary>
        /// The text flavours, in the order the menu shows them: plain text, then
        /// Archicad's attribute pickers, then the ones the band still draws as
        /// text.
        /// </summary>
        /// <remarks>
        /// The attribute names are exactly the ones
        /// <c>Palette/AttributePickerTypes.cpp</c> maps to an Archicad control —
        /// that table is the authority, and a name this list spells differently
        /// would fall through to a text box for no visible reason.
        /// </remarks>
        internal static IList<Flavour> ForText()
        {
            return new List<Flavour>
            {
                new Flavour("Text", string.Empty),
                new Flavour("Layer", "attribute:Layer"),
                new Flavour("Fill", "attribute:Fill"),
                new Flavour("Line type", "attribute:LineType"),
                new Flavour("Surface", "attribute:Surface"),
                new Flavour("Building material", "attribute:BuildingMaterial"),
                new Flavour("Composite - wall", "attribute:WallComposite"),
                new Flavour("Composite - slab", "attribute:SlabComposite"),
                new Flavour("Composite - roof", "attribute:RoofComposite"),
                new Flavour("Composite - shell", "attribute:ShellComposite"),
                new Flavour("Profile - wall", "attribute:WallProfile"),
                new Flavour("Profile - beam", "attribute:BeamProfile"),
                new Flavour("Profile - column", "attribute:ColumnProfile"),
                new Flavour("Profile - handrail", "attribute:HandrailProfile"),
                new Flavour("Profile - any", "attribute:AllProfile"),
                new Flavour("File path", "filepath"),
                new Flavour("View", "view"),
                new Flavour("Database", "database"),
                new Flavour("Library part", "library-part"),
                new Flavour("Favourite", "favourite"),
                new Flavour("Project field", "project-field"),
            };
        }

        /// <summary>
        /// The integer flavours: a plain number, a storey, or a pen.
        /// </summary>
        /// <remarks>
        /// Both Archicad flavours are integers for a reason the Python panel
        /// already records: a STOREY is an index, and a PEN has no name at all —
        /// pens are numbers in Archicad's own UI, which is why
        /// AttributePickerTypes leaves them out of the picker table entirely.
        /// </remarks>
        internal static IList<Flavour> ForInteger()
        {
            return new List<Flavour>
            {
                new Flavour("Integer", string.Empty),
                new Flavour("Story", "story"),
                new Flavour("Pen", "pen"),
            };
        }

        /// <summary>
        /// Appends the flavour chooser to a parameter's context menu.
        /// </summary>
        /// <remarks>
        /// ⚠️ CHECKED, NOT JUST LISTED. The flavour changes what the panel shows
        /// and nothing else on the canvas, so a menu that did not mark the current
        /// one would leave an author unable to find out what their input is
        /// without opening Archicad.
        /// </remarks>
        internal static void Append(
            ToolStripDropDown menu, IGH_Param param, TapiocaInputCore core, IList<Flavour> flavours)
        {
            ToolStripMenuItem root = GH_DocumentObject.Menu_AppendItem(menu, "Archicad type");
            root.ToolTipText = "Which Archicad control the Tapioca panel shows for this input.";

            foreach (Flavour flavour in flavours)
            {
                Flavour chosen = flavour;
                bool current = string.Equals(core.Flavour, flavour.TypeName, StringComparison.Ordinal);
                GH_DocumentObject.Menu_AppendItem(
                    root.DropDown,
                    flavour.Label,
                    delegate(object sender, EventArgs args) { Apply(param, core, chosen); },
                    null,
                    true,
                    current);
            }
        }

        /// <summary>
        /// The word a parameter shows beside itself, so the flavour is legible on
        /// the canvas.
        /// </summary>
        internal static string Caption(TapiocaInputCore core, IList<Flavour> flavours)
        {
            foreach (Flavour flavour in flavours)
            {
                if (string.Equals(core.Flavour, flavour.TypeName, StringComparison.Ordinal))
                {
                    return flavour.Label;
                }
            }

            // A flavour from a newer build than this one, read out of a saved
            // definition: shown as whatever it says rather than silently as the
            // plain type, because the panel will still honour it.
            return core.Flavour.Length > 0 ? core.Flavour : string.Empty;
        }

        private static void Apply(IGH_Param param, TapiocaInputCore core, Flavour flavour)
        {
            if (string.Equals(core.Flavour, flavour.TypeName, StringComparison.Ordinal))
            {
                return;
            }

            param.RecordUndoEvent("Tapioca Archicad type");
            core.Flavour = flavour.TypeName;

            // ⚠️ EXPIRED, BECAUSE THE SCHEMA CHANGED. The panel rebuilds its rows
            // when the definition's schema differs from the last one it saw, and
            // the schema is read off the parameters — so a flavour that did not
            // expire anything would leave Archicad showing the previous control
            // until something unrelated re-solved.
            param.ExpireSolution(true);
        }
    }
}
