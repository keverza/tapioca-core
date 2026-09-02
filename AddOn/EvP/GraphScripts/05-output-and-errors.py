# @name        Output and errors
# @description How a script talks back: printing, and failing.
#
# `print` has nowhere to go - the node runs on a worker thread inside Archicad
# with no console attached - so it is captured and shown behind the node's
# "Output" button.
#
# Set `mode` to see each failure the node can report. Every one of them fails
# THIS node with a message and leaves the rest of the graph running; none of them
# takes Archicad down.
#
#   ok       runs normally
#   raise    an exception, reported with its line number
#   forget   never sets `result`, so the node names the output that is missing
#   wrong    puts text in a number port, so the node names the port
#   slow     an infinite loop, stopped by the node's time budget
#
# @in  mode : text = "ok"   "Mode"
# @out result : number      "Result"

print("mode is", mode)

if mode == "raise":
    raise ValueError("this is what a failing script looks like")
elif mode == "forget":
    print("about to finish without setting result")
elif mode == "wrong":
    result = "not a number"
elif mode == "slow":
    print("looping forever; the time budget should stop this")
    while True:
        pass
else:
    print("computing normally")
    result = 42
