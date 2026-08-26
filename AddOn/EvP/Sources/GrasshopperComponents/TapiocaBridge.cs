using System;
using System.Reflection;
using System.Threading;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// Finds the running Tapioca add-on's managed host and calls it.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ REFLECTION, NOT A REFERENCE, AND THE REASON IS LOAD CONTEXTS.
    /// <c>Tapioca.GrasshopperHost.dll</c> is loaded by the add-on through
    /// hostfxr, into its own <c>AssemblyLoadContext</c>. This <c>.gha</c> is
    /// loaded by Grasshopper into the default one. A compile-time reference
    /// would therefore let the loader resolve a SECOND copy of the host from
    /// disk — a fresh set of statics whose native pointer was never bound — and
    /// every component would report "the bridge is not available" on a machine
    /// where it plainly is. That failure would look like a bug in the add-on and
    /// would be nearly impossible to read from the symptom.
    /// </para>
    /// <para>
    /// <see cref="AppDomain.GetAssemblies"/> spans every load context, so
    /// looking the host up by simple name finds the instance the add-on
    /// actually bound. One <see cref="MethodInfo"/>, cached.
    /// </para>
    /// <para>
    /// It is resolved LAZILY rather than in a static constructor: Grasshopper
    /// loads this package while the editor is coming up, which can be before the
    /// host has finished binding, and a failure cached at that moment would
    /// never recover.
    /// </para>
    /// </remarks>
    internal static class TapiocaBridge
    {
        private const string HostAssembly = "Tapioca.GrasshopperHost";
        private const string HostType = "Tapioca.GrasshopperHost.TapiocaNative";
        private const string CallMethod = "Call";

        private static MethodInfo _call;

        /// <summary>
        /// Runs one Tapioca command. Returns the JSON envelope the add-on
        /// produced, or a locally-built one describing why it could not be
        /// reached. Never throws.
        /// </summary>
        internal static string Call(string commandName, string parametersJson)
        {
            MethodInfo call = Volatile.Read(ref _call);
            if (call == null)
            {
                call = Resolve();
                if (call == null)
                {
                    return Error(
                        "Tapioca's add-on bridge was not found in this process. Grasshopper must be opened "
                        + "from Archicad with Tapioca > Grasshopper Editor; a Grasshopper started any other "
                        + "way has no Archicad to talk to.");
                }

                Volatile.Write(ref _call, call);
            }

            try
            {
                object result = call.Invoke(null, new object[] { commandName, parametersJson ?? string.Empty });
                return result as string ?? Error("The bridge returned nothing.");
            }
            catch (TargetInvocationException exception)
            {
                Exception inner = exception.InnerException ?? exception;
                return Error(inner.GetType().Name + ": " + inner.Message);
            }
            catch (Exception exception)
            {
                return Error(exception.GetType().Name + ": " + exception.Message);
            }
        }

        private static MethodInfo Resolve()
        {
            try
            {
                Assembly[] loaded = AppDomain.CurrentDomain.GetAssemblies();
                for (int index = 0; index < loaded.Length; index++)
                {
                    AssemblyName name = loaded[index].GetName();
                    if (!string.Equals(name.Name, HostAssembly, StringComparison.OrdinalIgnoreCase))
                    {
                        continue;
                    }

                    Type type = loaded[index].GetType(HostType, false, false);
                    if (type == null)
                    {
                        continue;
                    }

                    MethodInfo method = type.GetMethod(
                        CallMethod,
                        BindingFlags.Public | BindingFlags.Static,
                        null,
                        new Type[] { typeof(string), typeof(string) },
                        null);
                    if (method != null)
                    {
                        return method;
                    }
                }
            }
            catch (Exception)
            {
                // Treated as "not found": a component must degrade to a message
                // on the canvas, never to an exception during a solve.
            }

            return null;
        }

        private static string Error(string message)
        {
            return "{\"ok\":false,\"error\":\""
                   + message.Replace("\\", "\\\\").Replace("\"", "\\\"")
                   + "\"}";
        }
    }
}
