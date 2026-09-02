# @name        Every type
# @description One port of every type a script node can carry, so a glance at the
# @description node tells you the whole vocabulary - and so a change to the value
# @description marshalling shows up here first.
#
# The type words are the ones you type in the header. They are deliberately not
# the runtime's internal names: "number", not "double"; "text", not "string".
#
# @in  flag : bool = true          "A bool"
# @in  count : integer = 3         "An integer"
# @in  size : number = 1.5         "A number"
# @in  label : text = "wall"       "Some text"
# @in  origin : point              "A point"
# @in  numbers : list              "A list of numbers"
#
# @out flag_out : bool
# @out count_out : integer
# @out size_out : number
# @out label_out : text
# @out origin_out : point
# @out numbers_out : list
# @out total : number              "Sum of the list"

flag_out = not flag
count_out = count * 2
size_out = size * 2
label_out = label.upper()

# A point arrives as a plain object with x, y and z. Writing one back is the same
# shape; omitting z means zero, which is what a script working in plan wants.
origin_out = {"x": origin["x"] + 1, "y": origin["y"], "z": origin["z"]}

numbers_out = [n * 2 for n in numbers]
total = sum(numbers)
