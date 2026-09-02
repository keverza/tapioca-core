// @name        Hello (JavaScript)
// @description The same node in the other language, on purpose.
//
// The two runtimes are meant to be indistinguishable apart from the syntax of the
// file: same header, same ports, same values, same failures. Point a JavaScript
// node at this and a Python node at 01-hello.py, wire the same number into both,
// and they should agree exactly.
//
// @in  value : number = 2   "Value"
// @out doubled : number     "Doubled"

doubled = value * 2;
