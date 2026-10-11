using System.Reflection;
using Grasshopper2.Components;
using Grasshopper2.Doc;
using Grasshopper2.Framework;
using Rhino.Geometry;

namespace Tapioca.Gh2Worker;

internal static class Gh2OverlayChecks
{
    internal static Component[] AddComponents(Document document)
    {
        Guid[] ids = [new("7a120dfe-5cf1-4a02-9233-771aa947f5c0"), new("94af991e-bccb-44c4-8495-061d0673fbc9")];
        return ids.Select(id =>
        {
            var component = ObjectProxies.TryEmit<IDocumentObject>(id) as Component ??
                throw new InvalidOperationException("GH2 did not harvest both overlay components.");
            if (!document.Objects.Add(component) || component.Parameters.InputCount != 5 ||
                component.Parameters.OutputCount != 1 || ObjectProxies.FindById(id)?.Nomen.Section != "Display")
                throw new InvalidOperationException("Overlay component ports/category changed.");
            return component;
        }).ToArray();
    }

    internal static void Geometry(Assembly plugin)
    {
        MethodInfo prepare = plugin.GetType("TapiocaGH2.ArchicadOverlayGeometry")!
            .GetMethod("Prepare", BindingFlags.NonPublic | BindingFlags.Static)!;
        object Build(Curve[] curves, Mesh[] meshes, Point3d[] points, bool plan, double scale = 0.001, double width = 2) =>
            prepare.Invoke(null, [curves, meshes, points, plan, scale, "6495ED80", width])!;
        using var curve = new PolylineCurve([new(1000, 2000, 3000), new(2000, 2000, 4000)]);
        using var mesh = new Mesh();
        mesh.Vertices.Add(0, 0, 1000); mesh.Vertices.Add(1000, 0, 1000);
        mesh.Vertices.Add(1000, 1000, 1000); mesh.Vertices.Add(0, 1000, 1000);
        mesh.Faces.AddFace(0, 1, 2, 3);
        Array Items(object batch, string key) => (Array)batch.GetType().GetProperty(key)!.GetValue(batch)!;
        double[] Coords(Array items) => (double[])items.GetValue(0)!.GetType().GetProperty("points")!.GetValue(items.GetValue(0))!;
        object floor = Build([curve], [mesh], [new Point3d(1000, 1000, 5000)], true);
        object model = Build([curve], [mesh], [new Point3d(1000, 1000, 5000)], false);
        if (!Coords(Items(floor, "Lines")).SequenceEqual([1, 2, 0, 2, 2, 0]) ||
            !Coords(Items(model, "Lines")).SequenceEqual([1, 2, 3, 2, 2, 4]) ||
            !Coords(Items(floor, "Points")).SequenceEqual([1, 1, 0]) ||
            !Coords(Items(model, "Points")).SequenceEqual([1, 1, 5]))
            throw new InvalidOperationException("Overlay units or independent XY/XYZ conversion failed.");
        object surface = Items(model, "Meshes").GetValue(0)!;
        if (!((int[])surface.GetType().GetProperty("indices")!.GetValue(surface)!).SequenceEqual([0, 1, 2, 0, 2, 3]) ||
            !mesh.Faces[0].IsQuad || mesh.Vertices[0].Z != 1000)
            throw new InvalidOperationException("Overlay lost quad triangulation or mutated source geometry.");
        curve.Translate(8000, 7000, 6000);
        if (Coords(Items(model, "Lines"))[0] != 1)
            throw new InvalidOperationException("Prepared overlay retained mutable source vertices.");
        using var circle = new ArcCurve(new Circle(Plane.WorldXY, 1000));
        object sampled = Build([circle], [], [], false);
        if (Coords(Items(sampled, "Lines")).Length != 128 * 3)
            throw new InvalidOperationException("Closed curve sampling is not bounded or duplicated its endpoint.");
        foreach (Action invalid in new Action[]
        {
            () => Build([], [], [], false, scale: 0),
            () => Build([], [], [], false, width: double.NaN),
            () => Build([], [], [Point3d.Unset], false),
            () => Build([], new Mesh[65], [], false),
            () => Build([], [], new Point3d[20001], false)
        })
        {
            try { invalid(); throw new InvalidOperationException("Overlay accepted invalid/oversized geometry."); }
            catch (TargetInvocationException error) when (error.InnerException is ArgumentException) { }
        }
        Console.WriteLine("GH2 overlay harvest, units, XY/XYZ, curves, quads, immutability and budget checks passed.");
    }

    internal static void Reopened(Document restored, Component[] originals)
    {
        foreach (Component original in originals)
        {
            Component copy = restored.Objects.ActiveObjects.OfType<Component>().Single(item => item.GetType() == original.GetType());
            var producer = original.GetType().BaseType!.GetField("producer", BindingFlags.Instance | BindingFlags.NonPublic)!;
            var prepared = original.GetType().BaseType!.GetField("prepared", BindingFlags.Instance | BindingFlags.NonPublic)!;
            if (producer.GetValue(copy)!.Equals(producer.GetValue(original)) || prepared.GetValue(copy) is not null)
                throw new InvalidOperationException("Overlay layer/action state leaked through save/reopen.");
        }
    }
}
