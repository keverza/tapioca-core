using Grasshopper2.UI;
using Grasshopper2.UI.Icon;

namespace TapiocaGH2
{
    // The installed GH2 Plugin reads its identity from the assembly's Rhino GUID.
    public sealed class TapiocaGH2PluginInfo : Grasshopper2.Framework.Plugin
    {
        public override IIcon Icon => AbstractIcon.FromResource("TapiocaGH2Plugin", typeof(TapiocaGH2PluginInfo));
    }
}
