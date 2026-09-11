using System.Drawing;

using Grasshopper.GUI.Canvas;
using Grasshopper.Kernel;
using Grasshopper.Kernel.Attributes;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// A Tapioca input that has a word for what it currently is.
    /// </summary>
    /// <remarks>
    /// ⚠️ SEPARATE FROM <see cref="ITapiocaInput"/> ON PURPOSE. That interface is
    /// the schema contract — everything on it is read to build the panel. This one
    /// is a CANVAS concern and nothing on the wire reads it, so a component that
    /// gains a schema field should not have to think about drawing, and the
    /// reverse.
    /// </remarks>
    internal interface ITapiocaKindCaption
    {
        /// <summary>
        /// The word drawn under the capsule: the chosen Archicad type when the
        /// input has a flavour chooser, its plain data type otherwise. Empty draws
        /// nothing.
        /// </summary>
        string TapiocaKindCaption { get; }
    }

    /// <summary>
    /// The canvas appearance of a Tapioca input parameter: the message tag a
    /// component gets for free, on a parameter that does not.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ A GRASSHOPPER PARAMETER HAS NO <c>Message</c> AT ALL, AND THAT IS WHY
    /// THIS CLASS EXISTS. <c>Message</c> is declared on <c>GH_Component</c>, not
    /// on the shared <c>GH_ActiveObject</c> base, and
    /// <c>GH_FloatingParamAttributes.Render</c> draws only a capsule — an icon or
    /// the nickname, nothing below it. So the description was the only place a
    /// flavour could previously be written, which meant hovering was the only way
    /// to find out whether an input was a layer or a fill.
    /// </para>
    /// <para>
    /// ⚠️ RENDERED WITH GRASSHOPPER'S OWN TAG, NOT A HAND-DRAWN IMITATION.
    /// <c>GH_CapsuleRenderEngine.RenderMessage</c> is what
    /// <c>GH_ComponentAttributes</c> calls, and it positions itself from the
    /// capsule's own box, shrinks its font when the text is wider than the
    /// capsule, and fades with <c>GH_Canvas.ZoomFadeMedium</c> exactly like every
    /// other message on the canvas. A throwaway capsule over the same bounds is
    /// enough to reach it, and it is drawn BEFORE the base capsule for the same
    /// reason the component does that: the tag tucks in behind.
    /// </para>
    /// </remarks>
    internal sealed class TapiocaInputAttributes : GH_FloatingParamAttributes
    {
        internal TapiocaInputAttributes (IGH_Param param)
            : base (param)
        {
        }

        protected override void Render (GH_Canvas canvas, Graphics graphics, GH_CanvasChannel channel)
        {
            if (channel == GH_CanvasChannel.Objects)
            {
                RenderKindCaption (graphics);
            }

            base.Render (canvas, graphics, channel);
        }

        private void RenderKindCaption (Graphics graphics)
        {
            ITapiocaKindCaption owner = Owner as ITapiocaKindCaption;
            if (owner == null)
            {
                return;
            }

            string caption = owner.TapiocaKindCaption;
            if (string.IsNullOrEmpty (caption))
            {
                return;
            }

            // The same palette and style the base capsule will use a moment from
            // now, so the tag belongs to the object rather than sitting beside it:
            // it greys out when the input is disabled and highlights with the
            // selection.
            GH_Palette palette = GH_CapsuleRenderEngine.GetImpliedPalette (Owner);
            bool hidden = Owner is IGH_PreviewObject && ((IGH_PreviewObject) Owner).Hidden;
            GH_PaletteStyle style = GH_CapsuleRenderEngine.GetImpliedStyle (palette, Selected, Owner.Locked, hidden);

            GH_Capsule tag = GH_Capsule.CreateCapsule (Bounds, palette);
            tag.RenderEngine.RenderMessage (graphics, caption, style);
            tag.Dispose ();
        }
    }
}
