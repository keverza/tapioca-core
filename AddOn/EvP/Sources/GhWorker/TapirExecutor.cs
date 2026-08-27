using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace Tapioca.GhWorker
{
    /// <summary>
    /// Presses Tapir's execute buttons, so that one Run is one executor pass.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ THIS EXISTS BECAUSE TAPIR'S WRITES DO NOT HAPPEN DURING A SOLVE. Its
    /// write components carry a capsule BUTTON and fire from that, not from
    /// SolveInstance — so solving a definition that creates elements creates
    /// nothing, and the run reports a clean solve over a model it never touched.
    /// Measured twice on 2026-08-27: a definition built around CreateLineElements
    /// solved in 9 ms and 16 ms and wrote nothing either time.
    /// </para>
    /// <para>
    /// ⚠️ REFLECTION, NOT A REFERENCE, FOR THE REASON THE WHOLE PACKAGE FOLLOWS.
    /// Tapir is a pinned third-party <c>.gha</c> loaded by Grasshopper; taking a
    /// compile-time reference would bind Tapioca's build to it. Everything here
    /// is bound by interface NAME at run time, and every failure to find
    /// something is reported rather than thrown.
    /// </para>
    /// <para>
    /// It works at all because Tapir routed its button through a PUBLIC
    /// interface rather than a private click handler:
    /// <c>IButtonComponent.OnCapsuleButtonPressed(int)</c>, whose implementations
    /// call public <c>ManualExecute()</c> / <c>ManualRefresh()</c> — the same
    /// methods behind its right-click "Execute in Archicad". That is a property of
    /// how Tapir is built, not a promise it makes, which is one more reason the
    /// pinned version matters and a good item for the upstream issue.
    /// </para>
    /// <para>
    /// ⚠️ ONE PASS, NOT ONE PER COMPONENT. When a definition carries an
    /// "Execute All" button — ConnectArchicad's — that ONE button is pressed and
    /// nothing else, because it is Tapir's own way of running every executor once.
    /// Pressing it AND each executor's own button would run every write twice, and
    /// a duplicated write is worse than an absent one: the first is a wrong model,
    /// the second is a missing feature.
    /// </para>
    /// <para>
    /// ⚠️ THIS MAKES A RUN WRITE TO THE PROJECT, AHEAD OF SLICE 4'S GATING. That
    /// is deliberate and requested, and the run report says so plainly rather than
    /// letting a user discover it. What slice 4 still owes is the gating, not the
    /// mechanism.
    /// </para>
    /// <para>
    /// ⚠️ UI THREAD ONLY, AFTER THE SOLVE. The inputs a button reads are whatever
    /// the last solution produced, so pressing before one is pressing against
    /// stale data.
    /// </para>
    /// </remarks>
    internal static class TapirExecutor
    {
        private const string ButtonInterface = "IButtonComponent";
        private const string PressMethod = "OnCapsuleButtonPressed";
        private const string ButtonTextsProperty = "CapsuleButtonTexts";

        /// <summary>
        /// Tapir's own run-everything button. Preferred over per-component
        /// buttons when a definition has one.
        /// </summary>
        private const string ExecuteAll = "Execute All";

        /// <summary>
        /// Presses what needs pressing and returns a line for the run report.
        /// Never throws: a definition that cannot be executed must still report a
        /// solve.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static string PressExecuteButtons(Grasshopper.Kernel.GH_Document document)
        {
            if (document == null)
            {
                return string.Empty;
            }

            try
            {
                List<Candidate> candidates = FindButtons(document);
                if (candidates.Count == 0)
                {
                    return string.Empty;
                }

                // "Execute All" first, and alone. See the class remarks.
                foreach (Candidate candidate in candidates)
                {
                    if (candidate.IsExecuteAll)
                    {
                        return Press(candidate, "Execute All");
                    }
                }

                List<string> pressed = new List<string>();
                foreach (Candidate candidate in candidates)
                {
                    if (!candidate.IsExecute)
                    {
                        continue;
                    }

                    string report = Press(candidate, candidate.Text);
                    if (!string.IsNullOrEmpty(report))
                    {
                        pressed.Add(candidate.Name);
                    }
                }

                if (pressed.Count == 0)
                {
                    return string.Empty;
                }

                return "Pressed " + pressed.Count.ToString(CultureInfo.InvariantCulture)
                       + " Tapir execute button(s): " + string.Join(", ", pressed)
                       + ". Those writes are committed and are not part of the solve above.";
            }
            catch (Exception exception)
            {
                return "Tapir executors could not be pressed: " + WorkerLog.Describe(exception);
            }
        }

        private sealed class Candidate
        {
            internal object Component;

            internal MethodInfo Press;

            internal int Index;

            internal string Text;

            internal string Name;

            internal bool IsExecuteAll
            {
                get { return Text != null && Text.IndexOf(ExecuteAll, StringComparison.OrdinalIgnoreCase) >= 0; }
            }

            internal bool IsExecute
            {
                get { return Text != null && Text.IndexOf("Execute", StringComparison.OrdinalIgnoreCase) >= 0; }
            }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static List<Candidate> FindButtons(Grasshopper.Kernel.GH_Document document)
        {
            List<Candidate> found = new List<Candidate>();
            foreach (Grasshopper.Kernel.IGH_DocumentObject item in document.Objects)
            {
                if (item == null)
                {
                    continue;
                }

                Type type = item.GetType();
                if (!ImplementsButtonInterface(type))
                {
                    continue;
                }

                MethodInfo press = type.GetMethod(
                    PressMethod,
                    BindingFlags.Public | BindingFlags.Instance,
                    null,
                    new[] { typeof(int) },
                    null);
                if (press == null)
                {
                    continue;
                }

                foreach (KeyValuePair<int, string> button in ButtonTexts(item, type))
                {
                    found.Add(new Candidate
                    {
                        Component = item,
                        Press = press,
                        Index = button.Key,
                        Text = button.Value,
                        Name = item.NickName ?? type.Name,
                    });
                }
            }

            return found;
        }

        private static bool ImplementsButtonInterface(Type type)
        {
            foreach (Type contract in type.GetInterfaces())
            {
                if (string.Equals(contract.Name, ButtonInterface, StringComparison.Ordinal))
                {
                    return true;
                }
            }

            return false;
        }

        /// <summary>
        /// The button labels, by index. A component with no readable labels is
        /// skipped rather than pressed blind: pressing an unnamed button on a
        /// third-party component is exactly the kind of thing that writes
        /// something nobody asked for.
        /// </summary>
        private static IEnumerable<KeyValuePair<int, string>> ButtonTexts(object component, Type type)
        {
            List<KeyValuePair<int, string>> texts = new List<KeyValuePair<int, string>>();
            PropertyInfo property = type.GetProperty(
                ButtonTextsProperty, BindingFlags.Public | BindingFlags.Instance);
            if (property == null)
            {
                return texts;
            }

            IEnumerable values = property.GetValue(component) as IEnumerable;
            if (values == null)
            {
                return texts;
            }

            int index = 0;
            foreach (object value in values)
            {
                texts.Add(new KeyValuePair<int, string>(index, value as string));
                index++;
            }

            return texts;
        }

        private static string Press(Candidate candidate, string label)
        {
            try
            {
                candidate.Press.Invoke(candidate.Component, new object[] { candidate.Index });
                WorkerLog.Write("pressed Tapir button '" + label + "' on " + candidate.Name);
                return "Pressed Tapir's \"" + label + "\" on " + candidate.Name
                       + ". Those writes are committed and are not part of the solve above.";
            }
            catch (TargetInvocationException exception)
            {
                Exception inner = exception.InnerException ?? exception;
                return "Tapir's \"" + label + "\" on " + candidate.Name + " threw: " + WorkerLog.Describe(inner);
            }
            catch (Exception exception)
            {
                return "Tapir's \"" + label + "\" on " + candidate.Name + " could not be pressed: "
                       + WorkerLog.Describe(exception);
            }
        }
    }
}
