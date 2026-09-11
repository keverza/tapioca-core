using System;
using System.Collections.Generic;
using System.Drawing;
using System.Reflection;

using Grasshopper.Kernel;
using Grasshopper.Kernel.Types;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// Reads the Archicad element GUID out of another Archicad plug-in's element,
    /// so Tapioca's components can work on it.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ THIS DIRECTION WORKS AND THE OTHER ONE CANNOT, WHICH IS WHY THERE IS
    /// ONLY ONE COMPONENT HERE. GRAPHISOFT's Live Connection element goo
    /// (<c>ArchiCAD.Types.AC_Elem&lt;T&gt;</c>) exposes its element's real
    /// Archicad GUID — its own Select command sends exactly that
    /// (<c>elem.Guid</c>) — so pulling a GUID out is reading a public value off a
    /// public interface, and Tapioca's property, info and selection components
    /// take GUID text.
    /// </para>
    /// <para>
    /// ⚠️ GOING THE OTHER WAY IS NOT A CONVERSION, IT IS A DIFFERENT MODEL. Their
    /// element parameter is filled in exactly two ways: by one of their create
    /// components, or by their "Get …" context-menu item, which asks Archicad to
    /// run an interactive pick and tags the result to THE PARAMETER'S OWN
    /// InstanceGuid — <c>AC_GetElements(ParamType, InstanceGuid, isSingle)</c>.
    /// There is no get-element-by-element-GUID command in their vocabulary, and
    /// their goo's <c>CastFrom</c> accepts only their own element type, not a
    /// GUID or a string. So a GUID cannot become one of their elements without
    /// reconstructing their element model from the outside and inventing a
    /// parameter ref for it — which would break on their next release and be
    /// wrong in between.
    /// </para>
    /// <para>
    /// WHAT TO DO INSTEAD, when a definition needs one of their deconstructors:
    /// pick the element with THEIR parameter's "Get …" item, then feed this
    /// component from the same parameter to get the GUID for Tapioca's side. One
    /// pick, both worlds. For a headless run, use Tapioca Selection instead — the
    /// panel pushes the elements in, and Tapioca's own components read them
    /// without any of their types being involved.
    /// </para>
    /// <para>
    /// ⚠️ REFLECTION, AND NO REFERENCE. Tapioca must load in a Rhino that has
    /// never heard of their plug-in, so their types are touched by name or not at
    /// all. A goo that is not theirs is reported, not thrown over.
    /// </para>
    /// </remarks>
    public class TapiocaElementGuidComponent : GH_Component
    {
        /// <summary>
        /// The property every Live Connection element data object carries, and
        /// the interface it is declared on.
        /// </summary>
        private const string GuidPropertyName = "Guid";

        public TapiocaElementGuidComponent()
            : base(
                "Tapioca Element GUID",
                "Tapioca Element GUID",
                "The Archicad GUID of an element from another Archicad plug-in, so Tapioca's components can read it.",
                "Tapioca",
                "Archicad")
        {
        }

        protected override void RegisterInputParams(GH_InputParamManager pManager)
        {
            // Generic, because the type it accepts belongs to a plug-in that may
            // not be installed: a typed parameter would be a compile-time
            // reference to it. Whatever arrives is asked for a Guid.
            pManager.AddGenericParameter(
                "Element",
                "E",
                "An element from another Archicad plug-in (GRAPHISOFT's Live Connection), or a GUID as text.",
                GH_ParamAccess.list);
        }

        protected override void RegisterOutputParams(GH_OutputParamManager pManager)
        {
            pManager.AddTextParameter("Elements", "E", "Archicad element GUIDs.", GH_ParamAccess.list);
            pManager.AddBooleanParameter(
                "Found", "F", "Whether a GUID could be read from each input.", GH_ParamAccess.list);
        }

        protected override void SolveInstance(IGH_DataAccess DA)
        {
            List<IGH_Goo> items = new List<IGH_Goo>();
            if (!DA.GetDataList(0, items) || items.Count == 0)
            {
                return;
            }

            List<string> guids = new List<string>();
            List<bool> found = new List<bool>();
            int misses = 0;

            for (int index = 0; index < items.Count; index++)
            {
                string guid = GuidOf(items[index]);
                guids.Add(guid);
                found.Add(guid.Length > 0);
                if (guid.Length == 0)
                {
                    misses++;
                }
            }

            DA.SetDataList(0, guids);
            DA.SetDataList(1, found);

            if (misses > 0)
            {
                AddRuntimeMessage(
                    GH_RuntimeMessageLevel.Warning,
                    misses.ToString(System.Globalization.CultureInfo.InvariantCulture)
                    + " input(s) carried no Archicad GUID. Their element parameters only hold elements their own "
                    + "\"Get ...\" item or their create components put there.");
            }
        }

        /// <summary>
        /// The Archicad GUID of one goo: their element, a GUID, or text.
        /// </summary>
        /// <remarks>
        /// ⚠️ THE GOO'S <c>ScriptVariable</c> IS ASKED FIRST, because that is how
        /// their wrapper hands out the element data object it holds — their own
        /// <c>AC_Elem&lt;T&gt;.ScriptVariable</c> returns <c>this.Value</c>. The
        /// goo itself is tried as well, so a future shape that puts the property
        /// on the wrapper still resolves.
        /// </remarks>
        private static string GuidOf(IGH_Goo goo)
        {
            if (goo == null)
            {
                return string.Empty;
            }

            // Text and GUIDs first: cheap, and it makes the component a no-op
            // passthrough for guids that are already guids, which is what anyone
            // will try when wiring it up.
            GH_String text = goo as GH_String;
            if (text != null)
            {
                return text.Value == null ? string.Empty : text.Value.Trim();
            }

            GH_Guid gooGuid = goo as GH_Guid;
            if (gooGuid != null)
            {
                return Format(gooGuid.Value);
            }

            object value = null;
            try
            {
                value = goo.ScriptVariable();
            }
            catch (Exception)
            {
                // A goo that will not unwrap is simply not one of theirs.
            }

            string fromValue = ReadGuidProperty(value);
            if (fromValue.Length > 0)
            {
                return fromValue;
            }

            return ReadGuidProperty(goo);
        }

        private static string ReadGuidProperty(object holder)
        {
            if (holder == null)
            {
                return string.Empty;
            }

            try
            {
                PropertyInfo property = holder.GetType().GetProperty(
                    GuidPropertyName,
                    BindingFlags.Public | BindingFlags.Instance);
                if (property == null || property.PropertyType != typeof(Guid))
                {
                    return string.Empty;
                }

                object read = property.GetValue(holder, null);
                return read is Guid ? Format((Guid)read) : string.Empty;
            }
            catch (Exception)
            {
                return string.Empty;
            }
        }

        /// <summary>
        /// A GUID spelled the way Archicad's own API spells it.
        /// </summary>
        /// <remarks>
        /// ⚠️ UPPER CASE WITH BRACES, because that is what APIGuidToString
        /// produces and what every Tapioca element command answers with. The
        /// add-on canonicalises a guid it is GIVEN, so a different spelling would
        /// still resolve — but a list of guids that does not match the list the
        /// next component returns is a needless thing to hand somebody.
        /// </remarks>
        private static string Format(Guid guid)
        {
            return guid == Guid.Empty ? string.Empty : guid.ToString("B").ToUpperInvariant();
        }

        protected override Bitmap Icon
        {
            get { return null; }
        }

        public override Guid ComponentGuid
        {
            get { return new Guid("a17b4e60-2d93-4f58-8c14-6e05b39a7d21"); }
        }
    }
}
