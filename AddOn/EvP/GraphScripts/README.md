# Script node examples

Folders a **JavaScript** or **Python** script node can be pointed at directly, in
the order worth reading them.

A node is a FOLDER: `main.py` (or `main.js`) is its entry point, and any other
`.py` beside it is a helper the entry can import by name. None of the examples
below needs a helper yet - they are one file each - but they are folders because
that is what a node is, and pointing a node at a loose `.py` no longer works.

A node's ports come from the header at the top of `main.py` and from nowhere
else, so opening one of these in VSCode, Sublime or the palette's own editor and
saving it is the whole loop: the node reshapes itself to match.

| Folder | What it shows |
|---|---|
| `01-hello/` | The smallest script that is still a node |
| `02-hello/` | The same node in JavaScript, to show the two agree |
| `03-every-type/` | One port of every type a script can carry |
| `04-ports-change/` | Watching a node reshape itself as you edit |
| `05-output-and-errors/` | `print`, and every way a script can fail |
| `06-geometry/` | Points and polylines, and what a script may not make |

A node pointed at a loose file from before the folder model is CONVERTED on its
next reload: `offset.py` becomes `offset/main.py` and the node is repointed at
`offset/`. It happens once, it is reported, and the file is moved rather than
copied - so there is never a second copy to diverge.

## The header

```python
# @name        Offset polygon
# @description What this node does.
#
# @in  polygon : polygon
# @in  distance : number = 0.5   "Offset distance"
# @out result : polygon
```

`//` instead of `#` in JavaScript; everything else is identical.

- **Only the leading comment block is read.** The block ends at the first line
  that is neither a comment nor blank. A `@out` further down the file - in a
  docstring, in a commented-out experiment - cannot reshape your node behind your
  back.
- **Types**: `bool`, `integer`, `number`, `text`, `point`, `polyline`, `polygon`,
  `mesh`, `element`, `list`, and `any`.
- **An input may omit its type** and take anything. **An output may not**: the
  runtime checks what a script produced against the port's type, so an untyped
  output could only accept anything, and every wiring mistake would surface
  somewhere downstream instead of here.
- **A default** (`= 0.5`) makes the input optional: the node works the moment it
  is placed, and the port is there so something upstream can take over. Your typed
  value is never overwritten by a later reload.
- **A quoted string** at the end of the line is the port's label on the node.

## Writing the body

Inputs arrive as ordinary top-level variables named by their ports. Outputs are
read back from variables of the same names. There is no envelope to unwrap and
nothing to import.

A script sees its inputs and **nothing else** - no filesystem, no network, no
Archicad. A script that needs the model wires an Archicad node into itself, which
is something the graph can see, cache and re-run. It also runs under a time
budget, so a runaway loop fails its own node rather than hanging Archicad.

`print` (or `console.log`) is captured and shown behind the node's **Output**
button.

## The buttons

**Reload** re-reads the file now. **Create** scaffolds a new file at the path you
typed and refuses to touch an existing one. **Open** hands the file to whatever
your machine opens `.py` and `.js` with. The **folder** icon shows the file in
Explorer.

Saving in your editor normally reloads the node on its own. If the node says it is
not watching the folder, press Reload after each save.
