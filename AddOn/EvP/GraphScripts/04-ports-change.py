# @name        Ports change
# @description For watching a node reshape itself. Edit this file, save it, and
# @description the node should follow within a moment.
#
# Things worth trying, in order:
#
#   1. Rename `width` to `breadth`. The port renames; any wire into it is dropped,
#      and the node says how many connections went.
#   2. Change `: number` to `: text` on an output. Same again - a port that
#      changed type is as broken as one that was deleted.
#   3. Add `# @out perimeter : number` and set it in the body. A new port appears.
#   4. Delete the `:` from a type. The header stops parsing: the node keeps the
#      ports it has and shows the diagnostic with its line number, rather than
#      shedding every wire while you are mid-edit.
#
# @in  width : number = 3    "Width"
# @in  height : number = 4   "Height"
# @out area : number         "Area"

area = width * height
