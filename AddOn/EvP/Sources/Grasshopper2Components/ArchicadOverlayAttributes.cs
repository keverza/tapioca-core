using Eto.Drawing;
using Eto.Forms;
using Grasshopper2.Doc.Attributes;
using Grasshopper2.Eto.Drawing;
using Grasshopper2.Extensions;
using Grasshopper2.UI.Flex;
using Grasshopper2.UI.Primitives;
using Grasshopper2.UI.Skinning;

namespace TapiocaGH2;

public sealed class ArchicadOverlayAttributes : ComponentAttributes
{
    private readonly Action show, clear;
    private readonly ArchicadOverlayComponent overlay;
    private int pressed = -1;
    public ArchicadOverlayAttributes(ArchicadOverlayComponent component, Action show, Action clear) : base(component)
    {
        this.show = show;
        overlay = component;
        this.clear = clear;
        Responder.MouseDownHook += MouseDown;
        Responder.MouseUpHook += MouseUp;
    }
    protected override void LayoutCentralBox(Font font)
    {
        base.LayoutCentralBox(font);
        CentralBox = RectangleF.FromCenter(Pivot.ToPoint(),
            new SizeF(Math.Max(240, CentralBox.Width), Math.Max(144, CentralBox.Height)));
        ContentBox = CentralBox.AdjustSides(-8f);
    }
    private RectangleF Button(int index) => new(CentralBox.Left + 12,
        CentralBox.Top + CentralBox.Height / 2 - 60 + index * 40, CentralBox.Width - 24, 36);
    protected override void DrawContent(Context context, Skin skin, Capsule capsule, Shade shade)
    {
        for (int index = 0; index < 3; index++)
        {
            context.Graphics.FillRectangle(pressed == index ? shade.Apex : shade.Apex.Lighten(0.2f), Button(index));
            context.Graphics.DrawRectangle(new Pen(shade.Edge, 1), Button(index));
            context.Graphics.DrawTextInFrame(skin.Shape.FontWord, shade.Text,
                index == 0 ? "Show" : index == 1 ? (overlay.Automatic ? "Manual | [Auto]" : "[Manual] | Auto") : "Clear", Button(index), TextAlignment.Center);
        }
    }
    private Response MouseDown(ResponseMouseArgs args)
    {
        if (args.Buttons != MouseButtons.Primary) return Response.Ignored;
        for (int index = 0; index < 3; index++)
            if (Button(index).Contains(args.ContentLocation))
            {
                pressed = index;
                return Response.Capture;
            }
        return Response.Ignored;
    }
    private Response MouseUp(ResponseMouseArgs args)
    {
        if (pressed < 0) return Response.Ignored;
        int index = pressed;
        pressed = -1;
        if (Button(index).Contains(args.ContentLocation))
        {
            if (index == 0) show();
            else if (index == 1) overlay.ToggleMode();
            else clear();
        }
        return Response.Release;
    }
}
