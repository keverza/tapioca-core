using Eto.Drawing;
using Eto.Forms;
using Grasshopper2.Doc.Attributes;
using Grasshopper2.Extensions;
using Grasshopper2.UI.Flex;
using Grasshopper2.UI.Primitives;
using Grasshopper2.UI.Skinning;

namespace TapiocaGH2;

// Canvas body controls are deliberate actions, never solver-side API calls.
public sealed class ArchicadSelectionAttributes : ComponentAttributes
{
    private static readonly string[] Labels = ["Add", "Remove", "Update", "Clear", "Reselect"];
    private readonly ArchicadSelectionInput selection;
    private int pressed = -1;

    public ArchicadSelectionAttributes(ArchicadSelectionInput owner) : base(owner)
    {
        selection = owner;
        Responder.MouseDownHook += MouseDown;
        Responder.MouseUpHook += MouseUp;
    }

    protected override void LayoutCentralBox(FontDescription font)
    {
        CentralBox = RectangleF.FromCenter(Pivot.ToPoint(), new SizeF(320, 92));
        ContentBox = CentralBox.AdjustSides(-8f);
    }

    // The body is a two-row, three-column grid. The last cell is a count,
    // never an action target. These same bounds drive drawing and hit tests.
    private RectangleF Cell(int column, int row) => new(CentralBox.Left + 8f + column * 104f,
        CentralBox.Top + 8f + row * 40f, 96f, 32f);

    private RectangleF Button(int index) => index switch
    {
        0 => Cell(0, 0), // Add
        1 => Cell(0, 1), // Remove
        2 => Cell(1, 0), // Update
        3 => Cell(2, 0), // Clear
        4 => Cell(1, 1), // Reselect
        _ => RectangleF.Empty
    };

    protected override void DrawContent(Context context, Skin skin, Capsule capsule, Shade shade)
    {
        Font font = skin.Shape.FontWord.ToEtoFont();
        for (int i = 0; i < Labels.Length; i++)
        {
            RectangleF bounds = Button(i);
            context.Graphics.FillRectangle(i == pressed ? shade.Apex : shade.Apex.Lighten(0.2f), bounds);
            context.Graphics.DrawRectangle(new Pen(shade.Edge, 1f), bounds);
            context.Graphics.DrawTextInFrame(font, shade.Text, Labels[i], bounds, TextAlignment.Center);
        }
        RectangleF count = Cell(2, 1);
        context.Graphics.DrawTextInFrame(font, shade.Text, $"Count: {selection.Count}",
            count, TextAlignment.Center);
    }

    private Response MouseDown(ResponseMouseArgs args)
    {
        if (args.Buttons != MouseButtons.Primary)
            return Response.Ignored;
        for (int i = 0; i < Labels.Length; i++)
        {
            if (!Button(i).Contains(args.ContentLocation))
                continue;
            pressed = i;
            return Response.Capture;
        }
        return Response.Ignored;
    }

    private Response MouseUp(ResponseMouseArgs args)
    {
        if (pressed < 0)
            return Response.Ignored;
        int button = pressed;
        pressed = -1;
        if (Button(button).Contains(args.ContentLocation))
            selection.Activate(button);
        return Response.Release;
    }
}
