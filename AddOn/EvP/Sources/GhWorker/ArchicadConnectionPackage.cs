// ⚠️ global::Grasshopper THROUGHOUT: this file is compiled into
// Tapioca.Grasshopper.gha as well as into the worker, and inside namespace
// Tapioca.GhWorker the compiler finds the nearer Tapioca.Grasshopper namespace
// first. See DefinitionHost.cs for the full reason.

using System;
using System.Globalization;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace Tapioca.GhWorker
{
    /// <summary>
    /// Points GRAPHISOFT's own Grasshopper-Archicad Live Connection at THIS
    /// Archicad, when the user has it installed.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ THE SAME PROBLEM AS TAPIR, AND THE SAME ONE-PROPERTY ANSWER. The Live
    /// Connection posts to <c>http://127.0.0.1:19723</c> — Archicad's command
    /// connection, the identical transport Tapir uses — and 19723 is a hard
    /// default in its own connection manager with no instance discovery. So with
    /// two Archicads open its components drive whichever one holds that port,
    /// which may not be the model on screen. Only code inside the Grasshopper
    /// process can answer "which Archicad am I", and the add-on has already
    /// asked <c>ACAPI_Command_GetHttpConnectionPort</c>.
    /// </para>
    /// <para>
    /// ⚠️ SETTING IT FROM OUTSIDE IS WHAT THE PLUG-IN ITSELF DOES. The port is a
    /// user setting there: its own connection dialog writes
    /// <c>AC_ConnectionManager.Instance.Port</c> from a menu text box. This
    /// writes the same public property on the same public singleton, so nothing
    /// private is being reached into and nothing is patched — which is the rule
    /// <see cref="TapirPackage"/> states for Tapir and it holds here for the same
    /// reason.
    /// </para>
    /// <para>
    /// ⚠️ ITS ABSENCE IS NOT A FAILURE. Most Rhinos do not have the Live
    /// Connection installed; Tapioca neither needs nor ships it. Every outcome
    /// here is one line of report, never an exception and never a failed start.
    /// </para>
    /// <para>
    /// Reflection rather than a reference for the obvious reason: a compile-time
    /// reference would make Tapioca fail to load without a GRAPHISOFT plug-in it
    /// has nothing to do with.
    /// </para>
    /// </remarks>
    internal static class ArchicadConnectionPackage
    {
        /// <summary>
        /// What Grasshopper's library list calls it, and what its assembly is
        /// named. Matched loosely (either) because the display name is
        /// localizable and the assembly name is not.
        /// </summary>
        private const string AssemblyName = "ArchicadConnection";

        private const string ManagerTypeName = "ArchiCAD.AC_ConnectionManager";
        private const string InstancePropertyName = "Instance";
        private const string PortPropertyName = "Port";
        private const string StartMethodName = "StartConnection";
        private const string IsConnectedPropertyName = "IsConnected";
        private const string BreakMethodName = "BreakConnection";

        /// <summary>
        /// Breaks the Live Connection this peer told to start. Returns one line
        /// for the log; never throws.
        /// </summary>
        /// <remarks>
        /// <para>
        /// ⚠️ SYMMETRY, AND IT IS NOT MERELY TIDINESS. Attaching points this
        /// plug-in at an Archicad and calls its own StartConnection; when the
        /// bridge drops, that Archicad is usually GONE -- Archicad quitting is
        /// the ordinary way a peer is disconnected. What is left is another
        /// project's connection manager holding a live connection to a dead
        /// loopback port inside the user's Rhino, and whatever it does about that
        /// is not ours to predict. So the peer undoes exactly what it did.
        /// </para>
        /// <para>
        /// BreakConnection is its OWN public method -- the one its Connection
        /// dialog's disconnect calls -- so this asks for the same teardown a user
        /// would, rather than reaching into its state.
        /// </para>
        /// </remarks>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static string ReleaseConnection()
        {
            try
            {
                Assembly connection = FindLoaded();
                if (connection == null)
                {
                    return "Archicad Live Connection: not loaded, so nothing to release.";
                }

                Type manager = connection.GetType(ManagerTypeName, false, false);
                PropertyInfo instanceProperty = manager == null
                    ? null
                    : manager.GetProperty(InstancePropertyName, BindingFlags.Public | BindingFlags.Static);
                object instance = instanceProperty == null ? null : instanceProperty.GetValue(null, null);
                if (instance == null)
                {
                    return "Archicad Live Connection: loaded, but its connection manager was not reachable, so its "
                           + "connection was left as it is.";
                }

                MethodInfo breakConnection = manager.GetMethod(
                    BreakMethodName,
                    BindingFlags.Public | BindingFlags.Instance,
                    null,
                    Type.EmptyTypes,
                    null);
                if (breakConnection == null)
                {
                    return "Archicad Live Connection: this version has no public " + BreakMethodName + "(), so its "
                           + "connection was left pointing at an Archicad that may be gone. Disconnect it in its "
                           + "Connection dialog.";
                }

                breakConnection.Invoke(instance, null);
                return "Archicad Live Connection: disconnected, because Tapioca is what connected it.";
            }
            catch (Exception exception)
            {
                return "Archicad Live Connection could not be disconnected: " + WorkerLog.Describe(exception);
            }
        }

        /// <summary>
        /// Points the Live Connection at <paramref name="port"/>. Returns one
        /// line for the log; never throws.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static string BindPort(uint port)
        {
            try
            {
                Assembly connection = FindLoaded();
                if (connection == null)
                {
                    // Said plainly rather than warned about: not having
                    // GRAPHISOFT's plug-in is the ordinary case.
                    return "Archicad Live Connection: not loaded in this Grasshopper.";
                }

                if (port == 0)
                {
                    return "Archicad Live Connection: loaded, but this Archicad did not report a JSON port, so its "
                           + "own default (19723) is left alone. Set the port in its Connection dialog if more than "
                           + "one Archicad is running.";
                }

                Type manager = connection.GetType(ManagerTypeName, false, false);
                if (manager == null)
                {
                    return "Archicad Live Connection: loaded, but " + ManagerTypeName + " is not in this version, so "
                           + "the port could not be set; set it in its Connection dialog ("
                           + port.ToString(CultureInfo.InvariantCulture) + ").";
                }

                PropertyInfo instanceProperty = manager.GetProperty(
                    InstancePropertyName,
                    BindingFlags.Public | BindingFlags.Static);
                object instance = instanceProperty == null ? null : instanceProperty.GetValue(null, null);
                if (instance == null)
                {
                    return "Archicad Live Connection: loaded, but its connection manager singleton was not reachable, "
                           + "so the port could not be set; set it in its Connection dialog ("
                           + port.ToString(CultureInfo.InvariantCulture) + ").";
                }

                PropertyInfo portProperty = manager.GetProperty(
                    PortPropertyName,
                    BindingFlags.Public | BindingFlags.Instance);
                if (portProperty == null || !portProperty.CanWrite || portProperty.PropertyType != typeof(int))
                {
                    return "Archicad Live Connection: its " + PortPropertyName + " is not a writable int in this "
                           + "version, so the port could not be set; set it in its Connection dialog ("
                           + port.ToString(CultureInfo.InvariantCulture) + ").";
                }

                portProperty.SetValue(instance, (int)port, null);

                // Read back rather than assume, exactly as TapirPackage does:
                // this is another project's internals, and "we set it" is worth
                // nothing next to "it holds that value".
                object readBack = portProperty.GetValue(instance, null);
                int held = readBack is int ? (int)readBack : 0;
                if (held != (int)port)
                {
                    return "Archicad Live Connection: the port was written but reads back as "
                           + held.ToString(CultureInfo.InvariantCulture) + " rather than "
                           + port.ToString(CultureInfo.InvariantCulture) + ".";
                }

                return "Archicad Live Connection: pointed at port " + port.ToString(CultureInfo.InvariantCulture)
                       + ". " + Connect(manager, instance);
            }
            catch (Exception exception)
            {
                return "Archicad Live Connection: the port could not be set: " + WorkerLog.Describe(exception);
            }
        }

        /// <summary>
        /// Asks the Live Connection to connect, and reports what it said.
        /// </summary>
        /// <remarks>
        /// <para>
        /// ⚠️ IT DOES NOT CONNECT ITSELF UNLESS THE USER TURNED THAT ON. The
        /// plug-in auto-connects only when AC_Utility.EnableAutoConnect is set,
        /// and that flag is a REGISTRY value under
        /// HKCU\Software\GRAPHISOFT\GrasshopperConnection — a preference
        /// somebody ticked once in its own palette, not something a Grasshopper
        /// starting up decides. Otherwise a person presses Connect in its
        /// Connection dialog. Neither is available during a headless run, which
        /// is the whole reason this exists.
        /// </para>
        /// <para>
        /// ⚠️ WE CALL ITS OWN PUBLIC StartConnection, WE DO NOT WRITE ITS
        /// REGISTRY FLAG. Setting somebody's preference behind their back would
        /// change what their Rhino does when Tapioca is not involved; calling the
        /// method affects this session only, and it is the same method their own
        /// Connect button calls. StartConnection version-checks, initialises its
        /// project model and sends StartEventProcessing, so there is no cheaper
        /// half of it to invoke.
        /// </para>
        /// <para>
        /// ⚠️ ASYNC, AND NOT WAITED ON. It returns Task&lt;bool&gt; and does HTTP
        /// on the way; blocking a Grasshopper start on another plug-in's
        /// handshake would be holding up Rhino for a plug-in that may not answer.
        /// It is started and the outcome is read from IsConnected afterwards, by
        /// whoever cares.
        /// </para>
        /// </remarks>
        [MethodImpl(MethodImplOptions.NoInlining)]
        private static string Connect(Type manager, object instance)
        {
            try
            {
                PropertyInfo connected = manager.GetProperty(
                    IsConnectedPropertyName,
                    BindingFlags.Public | BindingFlags.Instance);
                object already = connected == null ? null : connected.GetValue(instance, null);
                if (already is bool && (bool)already)
                {
                    return "It is already connected.";
                }

                MethodInfo start = manager.GetMethod(
                    StartMethodName,
                    BindingFlags.Public | BindingFlags.Instance,
                    null,
                    Type.EmptyTypes,
                    null);
                if (start == null)
                {
                    return "It exposes no " + StartMethodName + " in this version, so connect it in its own "
                           + "Connection dialog.";
                }

                start.Invoke(instance, null);
                return "Connect requested; it answers when Archicad does.";
            }
            catch (Exception exception)
            {
                return "Connecting it failed: " + WorkerLog.Describe(exception);
            }
        }

        /// <summary>
        /// The loaded Live Connection assembly, or null.
        /// </summary>
        /// <remarks>
        /// Grasshopper's own library list is asked first because that is the
        /// authority on what this Grasshopper actually loaded; the app domain is
        /// the fallback for a build that reached the process another way.
        /// </remarks>
        [MethodImpl(MethodImplOptions.NoInlining)]
        private static Assembly FindLoaded()
        {
            if (global::Grasshopper.Instances.IsComponentServer && global::Grasshopper.Instances.ComponentServer != null)
            {
                foreach (global::Grasshopper.Kernel.GH_AssemblyInfo library
                         in global::Grasshopper.Instances.ComponentServer.Libraries)
                {
                    if (library == null || library.Assembly == null)
                    {
                        continue;
                    }

                    if (string.Equals(
                            library.Assembly.GetName().Name, AssemblyName, StringComparison.OrdinalIgnoreCase))
                    {
                        return library.Assembly;
                    }
                }
            }

            Assembly[] loaded = AppDomain.CurrentDomain.GetAssemblies();
            for (int index = 0; index < loaded.Length; index++)
            {
                if (string.Equals(loaded[index].GetName().Name, AssemblyName, StringComparison.OrdinalIgnoreCase))
                {
                    return loaded[index];
                }
            }

            return null;
        }
    }
}
