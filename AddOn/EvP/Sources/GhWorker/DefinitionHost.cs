// ⚠️ global::Grasshopper THROUGHOUT, AND IT IS LOAD-BEARING RATHER THAN
// FUSSY. This file is compiled into Tapioca.Grasshopper.gha as well as into the
// worker (see the .gha's .csproj: one set of files, so the two ends of the
// bridge cannot skew). That assembly declares the namespace Tapioca.Grasshopper,
// and from inside namespace Tapioca.GhWorker the compiler walks outward and
// finds Tapioca.Grasshopper before it ever considers the global one -- so a bare
// "Grasshopper.Kernel" resolves to Tapioca.Grasshopper.Kernel, which does not
// exist. The qualification says which Grasshopper is meant.

using System;
using System.Globalization;
using System.IO;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Text;

namespace Tapioca.GhWorker
{
    /// <summary>
    /// Owns one Grasshopper document loaded from a file, for one session.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ SESSION-OWNED, NOT CANVAS-OWNED, AND THAT IS THE WHOLE POINT OF THIS
    /// CLASS. <see cref="DefinitionRunner"/> solves whatever is on the active
    /// canvas, which is right for the Authoring gesture and is exactly wrong for
    /// a Player: the canvas is a UI surface the user can change under a solve,
    /// it is a single global, and a headless worker has no canvas at all.
    /// HANDOFF-GHHost.md §2 names replacing that assumption as the disposition
    /// for the run path. This class is the replacement: the host says which file,
    /// this loads it, holds it, and disposes it on request.
    /// </para>
    /// <para>
    /// ⚠️ ENGINE THREAD ONLY. Every method here touches a GH_Document; the
    /// document belongs to the one STA engine thread and nothing may reach it
    /// from the reader thread. <see cref="GhEngineThread"/> is how a request
    /// gets here.
    /// </para>
    /// <para>
    /// ⚠️ DISPOSAL IS EXPLICIT AND IS THE CLASS'S OTHER JOB. A document that is
    /// merely dropped stays in Grasshopper's DocumentServer, keeps its
    /// components alive, keeps whatever they hold open, and re-solves when the
    /// server tells it to. §5 records that complete removal behaviour for
    /// arbitrary third-party packages is NOT verified — so this removes and
    /// disposes deliberately, reports what it managed, and never claims the
    /// packages let go of everything.
    /// </para>
    /// <para>
    /// No Grasshopper type is named outside a non-inlinable method, for the
    /// reason <see cref="RhinoBoot"/> gives.
    /// </para>
    /// </remarks>
    internal sealed class DefinitionHost
    {
        /// <summary>
        /// The object key Grasshopper's own archive uses for a definition. The
        /// same one compute reads, and not a name to guess at.
        /// </summary>
        private const string DefinitionKey = "Definition";

        private global::Grasshopper.Kernel.GH_Document _document;
        private string _path = string.Empty;
        private string _identity = string.Empty;
        private string _contentHash = string.Empty;

        /// <summary>The loaded document, or null when there is none.</summary>
        internal global::Grasshopper.Kernel.GH_Document Document
        {
            get { return _document; }
        }

        internal bool IsLoaded
        {
            get { return _document != null; }
        }

        /// <summary>The path this document was loaded from; empty when none.</summary>
        internal string Path
        {
            get { return _path; }
        }

        /// <summary>
        /// What a stored revision records about where it came from: path, content
        /// hash and package fingerprints.
        /// </summary>
        internal string Identity
        {
            get { return _identity; }
        }

        /// <summary>
        /// The definition file's content hash. Separate from
        /// <see cref="Identity"/> because it is what "the definition bytes
        /// changed" is decided by (§7), and a rendered identity line is not
        /// comparable.
        /// </summary>
        internal string ContentHash
        {
            get { return _contentHash; }
        }

        /// <summary>
        /// Loads a .gh or .ghx into this session, replacing whatever it held.
        /// </summary>
        /// <remarks>
        /// <para>
        /// The old document is disposed BEFORE the new one is read, not after:
        /// two full definitions resident at once is the memory spike a repeated
        /// load-and-reload would grow without bound, and there is nothing to roll
        /// back to on failure anyway — a session whose load failed is Invalid,
        /// which is a state the host is told about rather than a silent revert to
        /// a definition the user has stopped asking for.
        /// </para>
        /// <para>
        /// Returns a failure code and a message; <see cref="SessionProtocol.FailureCode.None"/>
        /// on success. Never throws: this runs on the engine thread, and a fault
        /// here would take every session in the process with it.
        /// </para>
        /// </remarks>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal SessionProtocol.FailureCode Load(string path, out string message)
        {
            message = string.Empty;
            Dispose();

            if (string.IsNullOrWhiteSpace(path))
            {
                message = "No definition path was given.";
                return SessionProtocol.FailureCode.DefinitionInvalid;
            }

            string full;
            try
            {
                full = System.IO.Path.GetFullPath(path);
            }
            catch (Exception exception)
            {
                message = "That is not a usable definition path: " + WorkerLog.Describe(exception);
                return SessionProtocol.FailureCode.DefinitionInvalid;
            }

            if (!File.Exists(full))
            {
                message = "There is no definition at " + full + ".";
                return SessionProtocol.FailureCode.DefinitionInvalid;
            }

            try
            {
                // GH_Archive.ReadFromFile reads BOTH .gh (binary) and .ghx (xml)
                // and picks by content, so the extension is not switched on here.
                GH_IO.Serialization.GH_Archive archive = new GH_IO.Serialization.GH_Archive();
                if (!archive.ReadFromFile(full))
                {
                    message = "Grasshopper could not read " + full + ". It may be from a newer Grasshopper, or "
                              + "the file may be damaged.";
                    return SessionProtocol.FailureCode.DefinitionInvalid;
                }

                global::Grasshopper.Kernel.GH_Document document = new global::Grasshopper.Kernel.GH_Document();
                if (!archive.ExtractObject(document, DefinitionKey))
                {
                    message = full + " does not contain a Grasshopper definition.";
                    return SessionProtocol.FailureCode.DefinitionInvalid;
                }

                // ⚠️ ADDED TO THE DOCUMENT SERVER, AND NOT AS A FORMALITY.
                // Packages are told about documents through the server; a
                // document that was never added is one whose third-party
                // components never received their document-added notifications,
                // and several packages do their per-document setup there. It is
                // also what makes the removal in Dispose meaningful.
                global::Grasshopper.Instances.DocumentServer.AddDocument(document);

                _document = document;
                _path = full;
                _contentHash = HashFile(full);
                _identity = BuildIdentity(document, full, _contentHash);
                return SessionProtocol.FailureCode.None;
            }
            catch (Exception exception)
            {
                Dispose();
                message = "The definition could not be loaded: " + WorkerLog.Describe(exception);
                return SessionProtocol.FailureCode.DefinitionInvalid;
            }
        }

        /// <summary>
        /// Re-reads the same path. A convenience over <see cref="Load"/> that
        /// refuses when there is nothing to reload, so the caller does not have
        /// to remember the path itself.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal SessionProtocol.FailureCode Reload(out string message)
        {
            string path = _path;
            if (string.IsNullOrEmpty(path))
            {
                message = "This session has no definition to reload.";
                return SessionProtocol.FailureCode.DefinitionInvalid;
            }

            return Load(path, out message);
        }

        /// <summary>
        /// Removes the document from Grasshopper and disposes it. Idempotent.
        /// </summary>
        /// <remarks>
        /// ⚠️ REMOVE FIRST, DISPOSE SECOND, AND NEITHER STEP MAY SKIP THE OTHER.
        /// A disposed document still in the server is one the server may hand to
        /// a package or ask to solve; a removed document that was never disposed
        /// leaks everything its components hold. Each is guarded separately so
        /// that a package which throws out of its removal notification cannot
        /// leave the document undisposed.
        /// </remarks>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal void Dispose()
        {
            global::Grasshopper.Kernel.GH_Document document = _document;
            _document = null;
            _path = string.Empty;
            _identity = string.Empty;
            _contentHash = string.Empty;
            if (document == null)
            {
                return;
            }

            try
            {
                if (global::Grasshopper.Instances.DocumentServer != null)
                {
                    global::Grasshopper.Instances.DocumentServer.RemoveDocument(document);
                }
            }
            catch (Exception exception)
            {
                WorkerLog.Write(
                    "a definition would not leave Grasshopper's document server: " + WorkerLog.Describe(exception));
            }

            try
            {
                document.Dispose();
            }
            catch (Exception exception)
            {
                // Recorded, never fatal. §5: complete disposal behaviour for
                // every loaded third-party package is not verified, and this is
                // the line that says so when one of them proves it.
                WorkerLog.Write("a definition did not dispose cleanly: " + WorkerLog.Describe(exception));
            }
        }

        /// <summary>
        /// The definition's byte hash, as a short hex string.
        /// </summary>
        /// <remarks>
        /// SHA-256 over the file rather than over the extracted document: the
        /// file is what the user picked and what the trust check ran against
        /// (§12), and it hashes the same on both halves without either of them
        /// having to agree on a serialisation.
        /// </remarks>
        private static string HashFile(string path)
        {
            try
            {
                using (FileStream stream = File.OpenRead(path))
                using (SHA256 sha = SHA256.Create())
                {
                    byte[] digest = sha.ComputeHash(stream);
                    StringBuilder text = new StringBuilder(digest.Length * 2);
                    foreach (byte value in digest)
                    {
                        text.Append(value.ToString("x2", CultureInfo.InvariantCulture));
                    }

                    return text.ToString();
                }
            }
            catch (Exception exception)
            {
                WorkerLog.Write("a definition could not be hashed: " + WorkerLog.Describe(exception));
                return string.Empty;
            }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static string BuildIdentity(
            global::Grasshopper.Kernel.GH_Document document, string path, string contentHash)
        {
            StringBuilder text = new StringBuilder();
            text.Append(path);
            text.Append(" sha256:");
            text.Append(string.IsNullOrEmpty(contentHash) ? "(unhashed)" : contentHash);
            text.Append(" objects:");
            text.Append(document.ObjectCount.ToString(CultureInfo.InvariantCulture));

            string[] dependencies = WorkflowFacade.DescribeDependencies(document);
            if (dependencies.Length == 0)
            {
                return text.ToString();
            }

            text.Append(" packages:[");
            for (int index = 0; index < dependencies.Length; index++)
            {
                if (index > 0)
                {
                    text.Append("; ");
                }

                text.Append(dependencies[index]);
            }

            text.Append(']');
            return text.ToString();
        }
    }
}
