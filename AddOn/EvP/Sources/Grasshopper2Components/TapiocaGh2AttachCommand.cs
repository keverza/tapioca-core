using Rhino;
using Rhino.Commands;
using Tapioca.Gh2Worker;

namespace TapiocaGH2;

// Explicit command: loading the .rhp or solving a component does not attach.
public sealed class TapiocaGh2AttachCommand : Command
{
    private static int attaching;

    public override string EnglishName => "TapiocaGh2Attach";

    protected override Result RunCommand(RhinoDoc doc, RunMode mode)
    {
        if (Interlocked.CompareExchange(ref attaching, 1, 0) != 0)
        {
            RhinoApp.WriteLine("A GH2 attach is already in progress.");
            return Result.Nothing;
        }

        _ = Task.Run(() =>
        {
            try
            {
                Gh2Peer.AttachOnce(text => RhinoApp.InvokeOnUiThread((Action)(() => RhinoApp.WriteLine(text))));
            }
            catch (Exception error)
            {
                RhinoApp.InvokeOnUiThread((Action)(() => RhinoApp.WriteLine("GH2 attach failed: " + error.Message)));
            }
            finally { Interlocked.Exchange(ref attaching, 0); }
        });
        RhinoApp.WriteLine($"Searching for a waiting Archicad GH2 bridge in the background " +
            $"(Rhino command OS thread {Gh2Ping.NativeThreadId}).");
        return Result.Success;
    }
}
