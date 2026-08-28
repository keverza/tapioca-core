using System.Globalization;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;
using System.Text.Json;
using Dynamo.Applications;
using Dynamo.Graph.Nodes;
using Dynamo.Models;

internal static class Program
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
    };

    [ModuleInitializer]
    internal static void ConfigureAssemblyResolution()
    {
        AssemblyLoadContext.Default.Resolving += (_, name) =>
        {
            var fileName = $"{name.Name}.dll";
            foreach (var directory in new[] { Environment.CurrentDirectory, Path.Combine(Environment.CurrentDirectory, "nodes") })
            {
                var path = Path.Combine(directory, fileName);
                if (File.Exists(path))
                {
                    return AssemblyLoadContext.Default.LoadFromAssemblyPath(path);
                }
            }

            return null;
        };
    }

    [STAThread]
    private static async Task<int> Main()
    {
        using var protocol = new StreamWriter(Console.OpenStandardOutput()) { AutoFlush = true };
        Console.SetOut(Console.Error);
        CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
        CultureInfo.DefaultThreadCurrentUICulture = CultureInfo.InvariantCulture;

        DynamoModel? model = null;
        try
        {
            var startupArguments = StartupUtils.CommandLineArguments.Parse(
                ["--NoNetworkMode", "--DisableAnalytics"]);
            var activeModel = StartupUtils.MakeCLIModel(startupArguments);
            model = activeModel;
            Send(protocol, new { type = "ready", version = DynamoModel.Version });

            string? loadedPath = null;
            while (await Console.In.ReadLineAsync() is { } line)
            {
                try
                {
                    using var document = JsonDocument.Parse(line);
                    var request = document.RootElement;
                    var operation = ReadOperation(request);

                    if (operation == "shutdown")
                    {
                        RequireProperties(request, "operation");
                        activeModel.ShutDown(false);
                        model = null;
                        return 0;
                    }

                    var generation = ReadGeneration(request);
                    if (operation == "load")
                    {
                        RequireProperties(request, "operation", "generation", "path");
                        try
                        {
                            loadedPath = Load(activeModel, ReadGraphPath(request));
                            Send(protocol, new { type = "status", generation, status = "loaded", message = "Graph loaded." });
                        }
                        catch (Exception exception)
                        {
                            loadedPath = null;
                            Send(protocol, new { type = "status", generation, status = "error", message = exception.Message });
                        }
                    }
                    else if (operation == "run")
                    {
                        RequireProperties(request, "operation", "generation", "path", "inputs");
                        try
                        {
                            var path = ReadGraphPath(request);
                            var inputs = ReadInputs(request);
                            if (!string.Equals(loadedPath, path, StringComparison.OrdinalIgnoreCase))
                            {
                                loadedPath = Load(activeModel, path);
                            }

                            ApplyInputs(activeModel, inputs);
                            var evaluation = await Run(activeModel);
                            var issues = GetNodeIssues(activeModel);
                            var success = evaluation.EvaluationSucceeded && issues.Count == 0;
                            var message = success
                                ? "Graph evaluated successfully."
                                : BuildFailureMessage(evaluation, issues);
                            Send(protocol, new { type = "result", generation, success, message });
                        }
                        catch (Exception exception)
                        {
                            Send(protocol, new { type = "result", generation, success = false, message = exception.Message });
                        }
                    }
                    else
                    {
                        throw new InvalidDataException($"Unsupported operation '{operation}'.");
                    }
                }
                catch (Exception exception)
                {
                    Send(protocol, new { type = "error", message = exception.Message });
                }
            }

            return 0;
        }
        catch (Exception exception)
        {
            Send(protocol, new { type = "error", message = exception.Message });
            return 1;
        }
        finally
        {
            if (model is not null && !model.ShutdownRequested)
            {
                model.ShutDown(false);
            }
        }
    }

    private static string ReadOperation(JsonElement request)
    {
        if (request.ValueKind != JsonValueKind.Object ||
            !request.TryGetProperty("operation", out var operation) ||
            operation.ValueKind != JsonValueKind.String ||
            string.IsNullOrWhiteSpace(operation.GetString()))
        {
            throw new InvalidDataException("Request must contain a string operation.");
        }

        return operation.GetString()!;
    }

    private static long ReadGeneration(JsonElement request)
    {
        if (!request.TryGetProperty("generation", out var generation) ||
            !generation.TryGetInt64(out var value))
        {
            throw new InvalidDataException("Request must contain an integer generation.");
        }

        return value;
    }

    private static string ReadGraphPath(JsonElement request)
    {
        if (!request.TryGetProperty("path", out var pathElement) || pathElement.ValueKind != JsonValueKind.String)
        {
            throw new InvalidDataException("Request must contain a string path.");
        }

        var path = Path.GetFullPath(pathElement.GetString()!);
        if (!string.Equals(Path.GetExtension(path), ".dyn", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("Graph path must have a .dyn extension.");
        }

        if (!File.Exists(path))
        {
            throw new FileNotFoundException("Graph file does not exist.", path);
        }

        return path;
    }

    private static JsonElement ReadInputs(JsonElement request)
    {
        if (!request.TryGetProperty("inputs", out var inputs) || inputs.ValueKind != JsonValueKind.Object)
        {
            throw new InvalidDataException("Run request must contain an inputs object.");
        }

        return inputs;
    }

    private static void RequireProperties(JsonElement request, params string[] allowed)
    {
        var names = new HashSet<string>(allowed, StringComparer.Ordinal);
        foreach (var property in request.EnumerateObject())
        {
            if (!names.Contains(property.Name))
            {
                throw new InvalidDataException($"Unknown request property '{property.Name}'.");
            }
        }
    }

    private static string Load(DynamoModel model, string path)
    {
        model.OpenFileFromPath(path, forceManualExecutionMode: true);
        return path;
    }

    private static void ApplyInputs(DynamoModel model, JsonElement inputs)
    {
        var exposedInputs = model.CurrentWorkspace.Nodes
            .Where(node => node.IsSetAsInput && node.InputData is not null)
            .ToDictionary(node => node.GUID);

        foreach (var input in inputs.EnumerateObject())
        {
            if (input.NameEquals("graph"))
            {
                continue;
            }
            if (!Guid.TryParse(input.Name, out var nodeGuid) || !exposedInputs.ContainsKey(nodeGuid))
            {
                throw new InvalidDataException($"Unknown exposed input '{input.Name}'.");
            }

            model.ExecuteCommand(new DynamoModel.UpdateModelValueCommand(
                nodeGuid,
                "Value",
                SerializeInput(input.Value)));
        }
    }

    private static string SerializeInput(JsonElement value)
    {
        return value.ValueKind switch
        {
            JsonValueKind.String => value.GetString()!,
            JsonValueKind.Number or JsonValueKind.True or JsonValueKind.False or
                JsonValueKind.Array or JsonValueKind.Object => value.GetRawText(),
            _ => throw new InvalidDataException("Input values cannot be null or undefined.")
        };
    }

    private static async Task<EvaluationCompletedEventArgs> Run(DynamoModel model)
    {
        var completion = new TaskCompletionSource<EvaluationCompletedEventArgs>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        EventHandler<EvaluationCompletedEventArgs>? handler = null;
        handler = (_, arguments) => completion.TrySetResult(arguments);
        model.EvaluationCompleted += handler;
        try
        {
            model.ForceRun();
            return await completion.Task.WaitAsync(TimeSpan.FromMinutes(5));
        }
        finally
        {
            model.EvaluationCompleted -= handler;
        }
    }

    private static List<string> GetNodeIssues(DynamoModel model)
    {
        var issueStates = new HashSet<ElementState>
        {
            ElementState.Warning,
            ElementState.PersistentWarning,
            ElementState.Error,
            ElementState.AstBuildBroken
        };

        return model.CurrentWorkspace.Nodes
            .Where(node => issueStates.Contains(node.State))
            .Select(node =>
            {
                var messages = node.NodeInfos
                    .Where(info => issueStates.Contains(info.State))
                    .Select(info => info.Message)
                    .Where(message => !string.IsNullOrWhiteSpace(message));
                var detail = string.Join("; ", messages);
                return $"{node.Name} ({node.GUID}): {(detail.Length == 0 ? node.State : detail)}";
            })
            .ToList();
    }

    private static string BuildFailureMessage(EvaluationCompletedEventArgs evaluation, List<string> issues)
    {
        var messages = new List<string>();
        if (!evaluation.EvaluationSucceeded)
        {
            messages.Add($"Evaluation failed: {evaluation.Error.Message}");
        }

        messages.AddRange(issues);
        return messages.Count == 0 ? "Graph evaluation did not complete successfully." : string.Join(" | ", messages);
    }

    private static void Send(StreamWriter protocol, object response)
    {
        protocol.WriteLine(JsonSerializer.Serialize(response, JsonOptions));
    }
}
