using System;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace Tapioca.GrasshopperHost
{
    /// <summary>
    /// Gives the Grasshopper canvas its keyboard back inside Archicad.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ THE PROBLEM THIS SOLVES IS NOT A GRASSHOPPER BUG. Archicad owns the
    /// one message loop on this thread, and that loop translates Archicad's own
    /// accelerators BEFORE it dispatches anything. <c>TranslateAccelerator</c>
    /// ignores <c>MSG.hwnd</c> entirely — it turns a matching keystroke into a
    /// <c>WM_COMMAND</c> for Archicad's main window no matter which window in
    /// the thread has focus. So Delete, Escape and every other Archicad
    /// shortcut is eaten before the focused Grasshopper canvas ever sees it.
    /// </para>
    /// <para>
    /// The second, quieter half of the same problem: WinForms' dialog-key
    /// pre-processing (Tab, arrows, Enter, mnemonics, Escape) lives in
    /// <c>Application.ThreadContext.PreTranslateMessage</c>, which only runs
    /// inside a WinForms message loop. There is no WinForms loop in this
    /// process, so none of it runs either.
    /// </para>
    /// <para>
    /// The fix is a thread-local <c>WH_GETMESSAGE</c> hook. That hook runs
    /// INSIDE <c>GetMessage</c>/<c>PeekMessage</c>, which is strictly earlier
    /// than anything Archicad's loop does with the message. For a keystroke
    /// aimed at a Grasshopper-owned window we do what a WinForms loop would
    /// have done — pre-process, translate, dispatch — and then blank the
    /// message to <c>WM_NULL</c> so Archicad's loop finds nothing to steal.
    /// Every other message is left completely alone, so Archicad's own
    /// shortcuts keep working exactly as before whenever its own windows have
    /// focus.
    /// </para>
    /// <para>
    /// Deliberately free of Rhino and Grasshopper types: the set of windows to
    /// gate is handed in from <see cref="RhinoBoot"/>, which is the one file
    /// allowed to name them.
    /// </para>
    /// </remarks>
    internal static class EditorInput
    {
        private const int WhGetMessage = 3;
        private const int HcAction = 0;
        private const uint PmRemove = 0x0001;
        private const uint WmNull = 0x0000;
        private const uint WmKeyFirst = 0x0100;
        private const uint WmKeyLast = 0x0109;
        private const uint GaRoot = 2;
        private const uint GwOwner = 4;

        // Deep enough for canvas -> menu -> submenu -> tooltip, shallow enough
        // that a cycle in a malformed owner chain cannot spin here.
        private const int MaxOwnerHops = 8;

        private static readonly object Sync = new object();

        // Held in a static field on purpose: the hook is called from unmanaged
        // code, so the delegate must outlive the P/Invoke that installs it. A
        // local would be collected and the next keystroke would tear the
        // process down.
        private static HookProc _hookProc;
        private static IntPtr _hook = IntPtr.Zero;
        private static IntPtr[] _roots = new IntPtr[0];

        [ThreadStatic]
        private static bool _inHook;

        private delegate IntPtr HookProc(int code, IntPtr wParam, IntPtr lParam);

        internal static bool IsInstalled
        {
            get { lock (Sync) { return _hook != IntPtr.Zero; } }
        }

        /// <summary>
        /// Records which top-level windows count as Grasshopper's. Called on
        /// every show, because the editor form can be recreated.
        /// </summary>
        internal static void SetGatedRoots(IntPtr[] roots)
        {
            lock (Sync)
            {
                _roots = roots ?? new IntPtr[0];
            }
        }

        /// <summary>
        /// Installs the hook on the CALLING thread, which must be the thread
        /// that owns both Archicad's message loop and RhinoCore. Idempotent.
        /// Returns a line worth logging and never throws.
        /// </summary>
        internal static string Install()
        {
            try
            {
                lock (Sync)
                {
                    if (_hook != IntPtr.Zero)
                    {
                        return "Keyboard gate already installed.";
                    }

                    _hookProc = HookCallback;
                    _hook = SetWindowsHookEx(WhGetMessage, _hookProc, IntPtr.Zero, GetCurrentThreadId());
                    if (_hook == IntPtr.Zero)
                    {
                        int error = Marshal.GetLastWin32Error();
                        _hookProc = null;
                        return "Keyboard gate could not be installed (SetWindowsHookEx failed, error "
                               + error.ToString(CultureInfo.InvariantCulture)
                               + "). Archicad will keep intercepting Delete, Escape and other shortcuts "
                               + "while the Grasshopper canvas has focus.";
                    }

                    return "Keyboard gate installed; Grasshopper windows now receive their own keystrokes.";
                }
            }
            catch (Exception exception)
            {
                return "Keyboard gate could not be installed: " + exception.GetType().Name + ": " + exception.Message;
            }
        }

        /// <summary>
        /// Removes the hook. MUST be called before the host stops: a hook left
        /// pointing at a delegate in a runtime that is going away is exactly
        /// the kind of dangling callback the unload rules exist to prevent.
        /// </summary>
        internal static string Uninstall()
        {
            try
            {
                lock (Sync)
                {
                    if (_hook == IntPtr.Zero)
                    {
                        return "Keyboard gate was not installed.";
                    }

                    bool removed = UnhookWindowsHookEx(_hook);
                    _hook = IntPtr.Zero;
                    _hookProc = null;
                    _roots = new IntPtr[0];
                    return removed ? "Keyboard gate removed." : "Keyboard gate removal reported failure.";
                }
            }
            catch (Exception exception)
            {
                return "Keyboard gate removal failed: " + exception.GetType().Name + ": " + exception.Message;
            }
        }

        // ⚠️ NOTHING IN THIS METHOD MAY THROW. It runs on an unmanaged stack
        // inside GetMessage; an escaping exception does not unwind into a
        // handler, it takes Archicad down.
        private static IntPtr HookCallback(int code, IntPtr wParam, IntPtr lParam)
        {
            try
            {
                if (code == HcAction && (uint)wParam.ToInt64() == PmRemove && !_inHook)
                {
                    MSG message = (MSG)Marshal.PtrToStructure(lParam, typeof(MSG));
                    if (message.Message >= WmKeyFirst
                        && message.Message <= WmKeyLast
                        && IsGated(message.Hwnd))
                    {
                        _inHook = true;
                        try
                        {
                            Deliver(ref message);
                        }
                        finally
                        {
                            _inHook = false;
                        }

                        // Blank it in place. Archicad's loop still gets a
                        // message and still behaves normally; it just gets one
                        // with nothing in it to translate into a shortcut.
                        message.Message = WmNull;
                        message.WParam = IntPtr.Zero;
                        message.LParam = IntPtr.Zero;
                        Marshal.StructureToPtr(message, lParam, false);
                    }
                }
            }
            catch (Exception)
            {
                // A gate that misbehaves must degrade to "Archicad eats the
                // key", never to a dead Archicad.
            }

            return CallNextHookEx(IntPtr.Zero, code, wParam, lParam);
        }

        /// <summary>
        /// Does what a WinForms message loop would have done with this message.
        /// </summary>
        private static void Deliver(ref MSG message)
        {
            Control target = Control.FromChildHandle(message.Hwnd);
            if (target != null)
            {
                Message winforms = Message.Create(
                    message.Hwnd,
                    (int)message.Message,
                    message.WParam,
                    message.LParam);
                if (target.PreProcessControlMessage(ref winforms) == PreProcessControlState.MessageProcessed)
                {
                    // A dialog key (Tab, an arrow, Escape, a mnemonic) that the
                    // control tree consumed. Dispatching it as well would run
                    // it twice.
                    return;
                }
            }

            // TranslateMessage POSTS the resulting WM_CHAR rather than
            // delivering it, so the character comes back through this same hook
            // and is gated the same way. That is why translating here is safe.
            TranslateMessage(ref message);
            DispatchMessage(ref message);
        }

        private static bool IsGated(IntPtr hwnd)
        {
            if (hwnd == IntPtr.Zero)
            {
                return false;
            }

            IntPtr[] roots;
            lock (Sync)
            {
                roots = _roots;
            }

            IntPtr root = GetAncestor(hwnd, GaRoot);
            if (root == IntPtr.Zero)
            {
                root = hwnd;
            }

            IntPtr candidate = root;
            for (int hop = 0; hop <= MaxOwnerHops && candidate != IntPtr.Zero; hop++)
            {
                for (int index = 0; index < roots.Length; index++)
                {
                    if (roots[index] != IntPtr.Zero && roots[index] == candidate)
                    {
                        return true;
                    }
                }

                IntPtr owner = GetWindow(candidate, GwOwner);
                if (owner == candidate)
                {
                    break;
                }

                candidate = owner;
            }

            // Last resort, and a reliable one in THIS process: Archicad's own
            // UI is native, so a window that WinForms recognises as one of its
            // own controls can only have come from Rhino or Grasshopper. This
            // catches the canvas popups and menus that hang off no owner we
            // were told about.
            return Control.FromChildHandle(hwnd) != null;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct MSG
        {
            public IntPtr Hwnd;
            public uint Message;
            public IntPtr WParam;
            public IntPtr LParam;
            public uint Time;
            public int X;
            public int Y;
        }

        [DllImport("user32.dll", SetLastError = true)]
        private static extern IntPtr SetWindowsHookEx(int idHook, HookProc lpfn, IntPtr hMod, uint dwThreadId);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool UnhookWindowsHookEx(IntPtr hhk);

        [DllImport("user32.dll")]
        private static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);

        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool TranslateMessage(ref MSG message);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr DispatchMessage(ref MSG message);

        [DllImport("user32.dll")]
        private static extern IntPtr GetAncestor(IntPtr hwnd, uint flags);

        [DllImport("user32.dll")]
        private static extern IntPtr GetWindow(IntPtr hwnd, uint command);

        [DllImport("kernel32.dll")]
        private static extern uint GetCurrentThreadId();
    }
}
