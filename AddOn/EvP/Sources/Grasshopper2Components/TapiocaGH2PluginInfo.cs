using Eto.Forms;
using Grasshopper2.UI;
using Grasshopper2.UI.Icon;
using Rhino;
using Rhino.PlugIns;

namespace TapiocaGH2
{
    // The installed GH2 Plugin reads its identity from the assembly's Rhino GUID.
    public sealed class TapiocaGH2PluginInfo : Grasshopper2.Framework.Plugin
    {
        public override IIcon Icon => AbstractIcon.FromResource("TapiocaGH2Plugin", typeof(TapiocaGH2PluginInfo));

        public override void OnLoaded()
        {
            base.OnLoaded();

            // GH2's plugin server harvests Component types but does not create
            // the Rhino PlugIn instance that registers Rhino.Commands.Command.
            // Register only in standalone Rhino, never in the owned headless
            // worker (which loads this library by path and owns no Rhino UI).
            if (!string.Equals(Path.GetFileNameWithoutExtension(Environment.ProcessPath),
                    "Rhino", StringComparison.OrdinalIgnoreCase))
                return;

            string location = typeof(TapiocaGH2PluginInfo).Assembly.Location;
            RhinoApp.WriteLine($"Tapioca GH2 {typeof(TapiocaGH2PluginInfo).Assembly.GetName().Version} loaded from {location}.");
            Application? app = Application.Instance;
            if (app is null)
            {
                RhinoApp.WriteLine("Tapioca GH2: Rhino UI is not ready to register TapiocaGh2Attach.");
                return;
            }

            // Schedule after GH2 finishes harvesting this assembly. Loading it
            // as a Rhino PlugIn while PluginServer holds its load lock risks a
            // second loader path re-entering the same assembly mid-harvest.
            try
            {
                app.AsyncInvoke((Action)(() =>
                {
                    try
                    {
                        if (Rhino.Commands.Command.IsCommand("TapiocaGh2Attach"))
                        {
                            RhinoApp.WriteLine("Tapioca GH2: TapiocaGh2Attach command ready.");
                            return;
                        }
                        LoadPlugInResult outcome = PlugIn.LoadPlugIn(location, out Guid _);
                        if ((outcome != LoadPlugInResult.Success && outcome != LoadPlugInResult.SuccessAlreadyLoaded) ||
                            !Rhino.Commands.Command.IsCommand("TapiocaGh2Attach"))
                            RhinoApp.WriteLine($"Tapioca GH2: attach command registration failed ({outcome}). " +
                                $"Load '{location}' in Rhino's PlugInManager and restart Rhino if it was already loaded.");
                        else
                            RhinoApp.WriteLine("Tapioca GH2: TapiocaGh2Attach command ready.");
                    }
                    catch (Exception error)
                    {
                        RhinoApp.WriteLine("Tapioca GH2: attach command registration failed: " + error.Message);
                    }
                }));
            }
            catch (Exception error)
            {
                RhinoApp.WriteLine("Tapioca GH2: could not queue attach command registration: " + error.Message);
            }
        }
    }
}
