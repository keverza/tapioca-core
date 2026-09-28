using Eto.Drawing;
using Eto.Forms;
using Grasshopper2.Doc.Attributes;
using Grasshopper2.Extensions;
using Grasshopper2.UI.Flex;
using Grasshopper2.UI.Primitives;
using Grasshopper2.UI.Skinning;

namespace TapiocaGH2;

public sealed class CreateArchicadTerrainMeshAttributes : ComponentAttributes
{
    private readonly Action create;
    private bool pressed;
    public CreateArchicadTerrainMeshAttributes(Grasshopper2.Components.Component component, Action create) : base(component)
    {
        this.create = create;
        Responder.MouseDownHook += MouseDown;
        Responder.MouseUpHook += MouseUp;
    }
    protected override void LayoutCentralBox(FontDescription font)
    {
        CentralBox = RectangleF.FromCenter(Pivot.ToPoint(), new SizeF(220, 44));
        ContentBox = CentralBox.AdjustSides(-8f);
    }
    private RectangleF Button => new(CentralBox.Left + 8f, CentralBox.Top + 6f,
        CentralBox.Width - 16f, CentralBox.Height - 12f);
    protected override void DrawContent(Context context, Skin skin, Capsule capsule, Shade shade)
    {
        context.Graphics.FillRectangle(pressed ? shade.Apex : shade.Apex.Lighten(0.2f), Button);
        context.Graphics.DrawRectangle(new Pen(shade.Edge, 1f), Button);
        context.Graphics.DrawTextInFrame(skin.Shape.FontWord.ToEtoFont(), shade.Text,
            "Create Mesh", Button, TextAlignment.Center);
    }
    private Response MouseDown(ResponseMouseArgs args)
    {
        if (args.Buttons != MouseButtons.Primary || !Button.Contains(args.ContentLocation)) return Response.Ignored;
        pressed = true;
        return Response.Capture;
    }
    private Response MouseUp(ResponseMouseArgs args)
    {
        if (!pressed) return Response.Ignored;
        pressed = false;
        if (Button.Contains(args.ContentLocation)) create();
        return Response.Release;
    }
}
