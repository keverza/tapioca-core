# Virtual editor surfaces

The virtual editor can use the locked Archicad overlay or the separate **Tapioca
3D viewer**. Both hosts use the same native document, selection, working plane,
tools, tracker, profile panel and undo/redo history. Edits remain virtual; switching
surfaces does not write to the Archicad model.

In the Virtual Editor command, choose **Editor view → Tapioca 3D viewer** when
opening, or choose **Switch view** to move an existing session. The dock's lower
circle switches from the overlay to the viewer; its upper circle switches back.
Only one surface runs at a time. Unfinished drags, drawing points and typed input
are cancelled on a switch; committed edits and selection are retained. Closing
the viewer ends its editor, as do Done, command replacement and project teardown.
Report/save before closing if the definition must be kept.

The viewer has its own navigable camera. Middle-button pan, Shift+middle orbit
and wheel zoom remain viewer navigation. Left/right canvas input and editor keys
are handled by the same scoped native input owner as the overlay. Other windows
and dialogs do not become editor input targets.

Native API: `Tapioca.VirtualEditor` accepts
`{"action":"surface","surface":"viewer"}` or `"surface":"overlay"` for an
active editor. This queues a main-thread switch, not a synchronous rendering
guarantee. `Tapioca.EditorState` reports `surface`, document revision and undo
depth for inspection after the switch. The ordinary start/state/stop contracts
are unchanged.

The editor/HUD stay on the main thread. Only owned prepared draw data goes to the
viewer render thread; its camera matrices are published back by value for picking.
No Archicad camera discovery, injection lifecycle or model-writing path is reused
by the standalone renderer. This requires a matching rebuilt add-on and command
package, followed by restart/Rescan. Offline tests are not live rendering/input
acceptance; verify switching, editing, navigation, resize/DPI and close/reopen in
Archicad before relying on the new host.
