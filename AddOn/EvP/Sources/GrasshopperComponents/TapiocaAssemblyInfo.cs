using System;
using System.Drawing;

using Grasshopper.Kernel;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// How the package identifies itself to Grasshopper's library list.
    /// </summary>
    /// <remarks>
    /// Worth having rather than letting Grasshopper infer a name: the add-on
    /// checks this package's presence and version at startup the same way it
    /// checks Tapir's, and "which Tapioca is loaded" has to be answerable from
    /// the same list.
    /// </remarks>
    public class TapiocaAssemblyInfo : GH_AssemblyInfo
    {
        public override string Name
        {
            get { return "Tapioca"; }
        }

        public override Bitmap Icon
        {
            get { return null; }
        }

        public override string Description
        {
            get
            {
                return "Archicad from Grasshopper, in process. These components call the Tapioca add-on "
                     + "directly on Archicad's main thread — the thread a Grasshopper solve already runs "
                     + "on — so there is no socket, no scheduling and no timeout.";
            }
        }

        public override Guid Id
        {
            get { return new Guid("b6e21d78-4f05-42a3-9c8e-3a7f01d9b5c2"); }
        }

        public override string AuthorName
        {
            get { return "Tapioca"; }
        }

        public override string AuthorContact
        {
            get { return string.Empty; }
        }
    }
}
