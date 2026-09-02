#ifndef EVP_PYTHON_GRAPHSCRIPTRUNTIME_HPP
#define EVP_PYTHON_GRAPHSCRIPTRUNTIME_HPP

// The Python half of the script node family.
//
// ⚠️ IT LIVES HERE, NOT IN NodeGraph/, AND THAT IS THE WHOLE REASON THE RUNTIME
// IS AN INTERFACE. Running Python means PythonHost, which means loading
// python312.dll and EvPPy.dll by full path - none of which the offline C++ suite
// can do or should try to. So the graph runtime names IScriptRuntime, this
// installs an implementation of it, and NodeGraph/ stays free of CPython exactly
// as it stays free of the DevKit. It is the ArchicadHost/ArchicadHostImpl split,
// applied to the second host a script node needs.
//
// ⚠️ AND ITS ABSENCE IS ORDINARY. In the offline suite, and in an add-on whose
// CPython did not resolve, no Python runtime is installed and a Python script
// node fails with "the python runtime is not available in this build". That is a
// reported outcome, not a crash and not a silent skip - the JavaScript nodes in
// the same graph go on working.

namespace evp {

// Installs the Python script runtime. Called once during add-on startup.
//
// Deliberately does NOT initialize CPython: that happens lazily on the first
// Python node that actually runs. A graph with no Python nodes in it must not
// pay a multi-second interpreter start, and an add-on on a machine with no
// runtime must still load.
void InstallPythonScriptRuntime ();

} // namespace evp

#endif
