using Eto.Drawing;
using Eto.Forms;
using Grasshopper2.Doc.Attributes;
using Grasshopper2.Extensions;
using Grasshopper2.UI.Flex;
using Grasshopper2.UI.Primitives;
using Grasshopper2.UI.Skinning;

namespace TapiocaGH2;

public sealed class ArchicadOverlayAttributes : ComponentAttributes
{
    private readonly Action show, clear;
    private int pressed = -1;
    public ArchicadOverlayAttributes(ArchicadOverlayComponent component, Action show, Action clear) : base(component)
    {
        this.show = show;
        this.clear = clear;
        Responder.MouseDownHook += MouseDown;
        Responder.MouseUpHook += MouseUp;
    }
    protected override void LayoutCentralBox(FontDescription font)
    {
        CentralBox = RectangleF.FromCenter(Pivot.ToPoint(), new SizeF(200, 44));
        ContentBox = CentralBox.AdjustSides(-8f);
    }
    private RectangleF Button(int index) => new(CentralBox.Left + 8 + index * 96,
        CentralBox.Top + 6, 88, CentralBox.Height - 12);
    protected override void DrawContent(Context context, Skin skin, Capsule capsule, Shade shade)
    {
        for (int index = 0; index < 2; index++)
        {
            context.Graphics.FillRectangle(pressed == index ? shade.Apex : shade.Apex.Lighten(0.2f), Button(index));
            context.Graphics.DrawRectangle(new Pen(shade.Edge, 1), Button(index));
            context.Graphics.DrawTextInFrame(skin.Shape.FontWord.ToEtoFont(), shade.Text,
                index == 0 ? "Show" : "Clear", Button(index), TextAlignment.Center);
        }
    }
    private Response MouseDown(ResponseMouseArgs args)
    {
        if (args.Buttons != MouseButtons.Primary) return Response.Ignored;
        for (int index = 0; index < 2; index++)
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
        if (Button(index).Contains(args.ContentLocation)) (index == 0 ? show : clear)();
        return Response.Release;
    }
}
