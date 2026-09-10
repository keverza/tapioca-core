using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;

namespace Tapioca.GhWorker
{
    /// <summary>
    /// Finds <c>Tapioca.Grasshopper.gha</c>'s workflow facade in this process and
    /// calls it.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ REFLECTION, NOT A REFERENCE, AND THE MIRROR OF
    /// <c>TapiocaBridge.cs</c> IN THE PACKAGE. That file explains why the .gha
    /// may not reference this assembly; the same loader argument runs the other
    /// way. Grasshopper loads the package from its Libraries folder at a path
    /// this worker does not know when it is built, and a compile-time reference
    /// would resolve a SECOND Tapioca.Grasshopper into the process whose
    /// components are not the ones on the canvas — so discovery would report an
    /// empty schema for a definition full of inputs.
    /// </para>
    /// <para>
    /// <see cref="AppDomain.GetAssemblies"/> spans every load context, so
    /// looking the package up by simple name finds the instance Grasshopper
    /// actually loaded.
    /// </para>
    /// <para>
    /// ⚠️ RESOLVED LAZILY, AND A FAILURE IS NEVER CACHED. The package loads
    /// while Grasshopper comes up, which can be after the first session message
    /// arrives; a null cached at that moment would never recover for the life of
    /// the process.
    /// </para>
    /// <para>
    /// Every method here names a Grasshopper type in its signature, so all of
    /// them are non-inlinable for the reason <see cref="RhinoBoot"/> gives.
    /// </para>
    /// </remarks>
    internal static class WorkflowFacade
    {
        private const string PackageAssembly = "Tapioca.Grasshopper";
        private const string FacadeType = "Tapioca.Grasshopper.TapiocaWorkflowFacade";

        /// <summary>
        /// The output and diagnostic ceilings the facade clips at. They are the
        /// worker's, not the package's: the package cannot see the wire, and
        /// SessionProtocol's own limits are what a payload must fit inside.
        /// </summary>
        private const int MaxOutputItems = (int)SessionProtocol.MaxSessionOutputs;

        private const int MaxDiagnosticItems = (int)SessionProtocol.MaxSessionDiagnostics;

        private static MethodInfo _describeSchema;
        private static MethodInfo _applyInputs;
        private static MethodInfo _describeInputs;
        private static MethodInfo _collectOutputs;
        private static MethodInfo _collectDiagnostics;
        private static MethodInfo _describeDependencies;
        private static Type _facade;

        internal static bool IsAvailable
        {
            get { return Resolve() != null; }
        }

        /// <summary>
        /// The definition's WorkflowSchema JSON, or a schema whose errors array
        /// says why there is none. Never throws.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static string DescribeSchema(
            Grasshopper.Kernel.GH_Document document, string workflowId, string workflowName)
        {
            MethodInfo method = Bind(ref _describeSchema, "DescribeSchema");
            if (method == null)
            {
                return NoPackageSchema();
            }

            object result = Invoke(method, new object[] { document, workflowId, workflowName });
            return result as string ?? NoPackageSchema();
        }

        /// <summary>
        /// Writes one input snapshot into the document. Returns an empty string
        /// when every value landed and one line per rejection otherwise.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static string ApplyInputs(Grasshopper.Kernel.GH_Document document, string[] ids, string[] values)
        {
            MethodInfo method = Bind(ref _applyInputs, "ApplyInputs");
            if (method == null)
            {
                return NoPackageMessage();
            }

            object result = Invoke(method, new object[] { document, ids, values });
            return result as string ?? NoPackageMessage();
        }

        /// <summary>
        /// One line per input saying what it is and what it holds, for the
        /// panel's transcript. Empty rather than null.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static string[] DescribeInputs(Grasshopper.Kernel.GH_Document document)
        {
            MethodInfo method = Bind(ref _describeInputs, "DescribeInputs");
            if (method == null)
            {
                return new string[0];
            }

            return Invoke(method, new object[] { document }) as string[] ?? new string[0];
        }

        /// <summary>
        /// Every published output after a solution, as {id, type, path, value}
        /// quadruples. Empty rather than null.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static string[] CollectOutputs(Grasshopper.Kernel.GH_Document document)
        {
            MethodInfo method = Bind(ref _collectOutputs, "CollectOutputs");
            if (method == null)
            {
                return new string[0];
            }

            return Invoke(method, new object[] { document, MaxOutputItems }) as string[] ?? new string[0];
        }

        /// <summary>
        /// Every runtime message on the document, as {level, component, text}
        /// triples.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static string[] CollectDiagnostics(Grasshopper.Kernel.GH_Document document)
        {
            MethodInfo method = Bind(ref _collectDiagnostics, "CollectDiagnostics");
            if (method == null)
            {
                return new string[0];
            }

            return Invoke(method, new object[] { document, MaxDiagnosticItems }) as string[] ?? new string[0];
        }

        /// <summary>
        /// The non-core packages this document's components came from, as
        /// "name version" lines.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static string[] DescribeDependencies(Grasshopper.Kernel.GH_Document document)
        {
            MethodInfo method = Bind(ref _describeDependencies, "DescribeDependencies");
            if (method == null)
            {
                return new string[0];
            }

            return Invoke(method, new object[] { document }) as string[] ?? new string[0];
        }

        private static MethodInfo Bind(ref MethodInfo cached, string name)
        {
            MethodInfo bound = Volatile.Read(ref cached);
            if (bound != null)
            {
                return bound;
            }

            Type facade = Resolve();
            if (facade == null)
            {
                return null;
            }

            try
            {
                bound = facade.GetMethod(name, BindingFlags.Public | BindingFlags.Static);
            }
            catch (Exception exception)
            {
                WorkerLog.Write(
                    "the Tapioca package's " + name + " could not be bound: " + WorkerLog.Describe(exception));
                return null;
            }

            if (bound == null)
            {
                // A package that loaded but has no such method is a version skew
                // between the .gha and this worker -- two halves that ship
                // together, so the only legitimate cause is a stale copy left in
                // Grasshopper's Libraries folder.
                WorkerLog.Write(
                    "the Tapioca package in this process has no " + name
                    + "; it is older than this worker. Restart Archicad so the shipped package is reinstalled.");
                return null;
            }

            Volatile.Write(ref cached, bound);
            return bound;
        }

        private static Type Resolve()
        {
            Type facade = Volatile.Read(ref _facade);
            if (facade != null)
            {
                return facade;
            }

            try
            {
                foreach (Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
                {
                    AssemblyName name = assembly.GetName();
                    if (name == null
                        || !string.Equals(name.Name, PackageAssembly, StringComparison.OrdinalIgnoreCase))
                    {
                        continue;
                    }

                    facade = assembly.GetType(FacadeType, false);
                    if (facade != null)
                    {
                        Volatile.Write(ref _facade, facade);
                        return facade;
                    }
                }
            }
            catch (Exception exception)
            {
                WorkerLog.Write("the Tapioca package could not be located: " + WorkerLog.Describe(exception));
            }

            return null;
        }

        private static object Invoke(MethodInfo method, object[] arguments)
        {
            try
            {
                return method.Invoke(null, arguments);
            }
            catch (TargetInvocationException exception)
            {
                // The facade is written not to throw, so reaching here means a
                // Grasshopper type faulted underneath it. Named rather than
                // rethrown: this runs on the engine thread, which owns every
                // session in the process.
                WorkerLog.Write(
                    "the Tapioca package's " + method.Name + " faulted: "
                    + WorkerLog.Describe(exception.InnerException ?? exception));
                return null;
            }
            catch (Exception exception)
            {
                WorkerLog.Write(
                    "the Tapioca package's " + method.Name + " could not be called: "
                    + WorkerLog.Describe(exception));
                return null;
            }
        }

        private static string NoPackageMessage()
        {
            return "Tapioca's Grasshopper package is not loaded in this process, so this definition's inputs and "
                   + "outputs cannot be reached. Restart Archicad to reinstall it.";
        }

        private static string NoPackageSchema()
        {
            return "{\"workflowId\":\"\",\"name\":\"\",\"version\":1,\"inputs\":[],\"errors\":[\""
                   + NoPackageMessage() + "\"]}";
        }
    }
}
