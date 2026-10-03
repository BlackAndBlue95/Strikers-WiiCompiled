using System.Text.RegularExpressions;
using System.Xml;
using System.Xml.Linq;

namespace Translator.Core.Mods;

/// <summary>A code mod an installed Riivolution pack puts on the disc.</summary>
/// <param name="DiscName">The module's file name at the disc root, e.g. <c>sml_50_superteam.bin</c>.</param>
/// <param name="HostPath">The pack file it comes from.</param>
/// <param name="XmlPath">The pack XML that maps it.</param>
public sealed record RiivolutionCodeModule(string DiscName, string HostPath, string XmlPath);

/// <summary>
/// Finds the code mods of the Riivolution packs installed in the port's overlay roots. A code mod is
/// a Kamek module that an active <c>&lt;file&gt;</c> patch places at the disc root as
/// <c>sml_&lt;NN&gt;_&lt;pack&gt;.bin</c>, where the Strikers Mod Loader looks for them on a console.
/// The port translates them ahead of time, so the build has to pick the same files the game will see:
/// this mirrors the runtime's pack evaluation (runtime/include/hle/riivolution_contract.h and
/// runtime/src/hle/storage/riivolution.cpp, both after Dolphin), the part that decides which file
/// patches are active and which host file each one reads.
/// </summary>
public static partial class RiivolutionCodeModules
{
    /// <param name="overlayRoots">In precedence order, highest first, as the runtime discovers them.</param>
    /// <param name="gameId">Six characters, e.g. <c>R4QE01</c>.</param>
    /// <param name="log">Receives one line per pack and per module found.</param>
    public static IReadOnlyList<RiivolutionCodeModule> Find(
        IEnumerable<string> overlayRoots, string gameId, Action<string>? log = null)
    {
        var chosen = new Dictionary<string, RiivolutionCodeModule>(StringComparer.OrdinalIgnoreCase);
        foreach (var root in overlayRoots)
        {
            // Within a root a later mapping replaces an earlier one; across roots the first root wins.
            var fromRoot = new Dictionary<string, RiivolutionCodeModule>(StringComparer.OrdinalIgnoreCase);
            foreach (var module in FindInRoot(Path.GetFullPath(root), gameId, log))
            {
                fromRoot[module.DiscName] = module;
            }
            foreach (var (name, module) in fromRoot)
            {
                chosen.TryAdd(name, module);
            }
        }

        // Modules load in name order (sml_10_... before sml_50_...), the same order on a console.
        return chosen.Values.OrderBy(static module => module.DiscName, StringComparer.OrdinalIgnoreCase).ToArray();
    }

    /// <summary>Whether a disc path names a code module: a file at the disc root called sml_*.bin.</summary>
    public static bool IsCodeModuleDiscPath(string discPath, out string discName)
    {
        discName = discPath.TrimStart('/');
        return CodeModuleName().IsMatch(discName);
    }

    private static IEnumerable<RiivolutionCodeModule> FindInRoot(string root, string gameId, Action<string>? log)
    {
        var xmlDirectory = Path.Combine(root, "riivolution");
        if (!Directory.Exists(xmlDirectory))
        {
            yield break;
        }

        var sdRoot = Generic(root);
        var config = ReadConfig(Path.Combine(xmlDirectory, "config", gameId[..4] + ".xml"));
        foreach (var xmlPath in Directory.EnumerateFiles(xmlDirectory, "*.xml")
                     .Order(StringComparer.Ordinal))
        {
            var disc = ReadDisc(xmlPath);
            if (disc is null)
            {
                log?.Invoke($"{xmlPath}: not a Riivolution XML (version 1 wiidisc), skipped");
                continue;
            }
            if (!disc.IsValidForGame(gameId))
            {
                continue;
            }

            if (config is not null)
            {
                disc.ApplyConfig(config);
            }

            var xmlDirectoryGeneric = Generic(Path.GetDirectoryName(xmlPath)!);
            foreach (var patch in disc.ActivePatches(gameId))
            {
                var patchRoot = ResolvePatchRoot(sdRoot, xmlDirectoryGeneric, patch.Root);
                foreach (var file in patch.Files)
                {
                    if (string.IsNullOrEmpty(file.Disc) || !IsCodeModuleDiscPath(file.Disc, out var discName))
                    {
                        continue;
                    }

                    var resolved = MakeAbsoluteFromRelative(sdRoot, patchRoot, file.External);
                    if (resolved is null || !File.Exists(resolved))
                    {
                        log?.Invoke($"{xmlPath}: {file.Disc} -> {file.External} doesn't exist, skipped");
                        continue;
                    }

                    var hostPath = Path.GetFullPath(resolved);
                    log?.Invoke($"{xmlPath}: code mod {discName} <- {hostPath}");
                    yield return new RiivolutionCodeModule(discName, hostPath, xmlPath);
                }
            }
        }
    }

    // ---- the pack model: the subset of riivolution_contract.h that decides active file patches ----

    private sealed record FilePatch(string Disc, string External);

    private sealed record Patch(string Id, string Root, IReadOnlyList<FilePatch> Files);

    private sealed record PatchReference(string Id, IReadOnlyDictionary<string, string> Parameters);

    private sealed record Choice(IReadOnlyList<PatchReference> Patches);

    private sealed class Option
    {
        public required string Id { get; init; }
        public required string Name { get; init; }
        public required IReadOnlyList<Choice> Choices { get; init; }
        /// <summary>1-based; 0 is off.</summary>
        public uint Selected { get; set; }
    }

    private sealed record Section(string Name, IReadOnlyList<Option> Options);

    private sealed class Disc
    {
        public string? Game { get; init; }
        public string? Developer { get; init; }
        public bool HasDiscFilter { get; init; }
        public bool HasVersionFilter { get; init; }
        public IReadOnlyList<string>? Regions { get; init; }
        public required IReadOnlyList<Section> Sections { get; init; }
        public required IReadOnlyList<Patch> Patches { get; init; }

        // Dolphin's Disc::IsValidForGame with no disc number or revision known, as the port calls it.
        public bool IsValidForGame(string gameId)
        {
            if (gameId.Length != 6) return false;
            if (Game is not null && !gameId.StartsWith(Game, StringComparison.Ordinal)) return false;
            if (Developer is not null && gameId.Substring(4, 2) != Developer) return false;
            if (HasDiscFilter || HasVersionFilter) return false;
            if (Regions is { Count: > 0 } regions && !regions.Contains(gameId.Substring(3, 1))) return false;
            return true;
        }

        // An option is addressed by its id, or by section name + option name when it has none.
        public void ApplyConfig(IReadOnlyList<(string Id, uint Choice)> config)
        {
            foreach (var (id, choice) in config)
            {
                foreach (var section in Sections)
                {
                    foreach (var option in section.Options)
                    {
                        var matches = option.Id.Length == 0 ? section.Name + option.Name == id : option.Id == id;
                        if (matches) option.Selected = choice;
                    }
                }
            }
        }

        public IEnumerable<Patch> ActivePatches(string gameId)
        {
            foreach (var option in Sections.SelectMany(static section => section.Options))
            {
                if (option.Selected == 0 || option.Selected > option.Choices.Count) continue;
                foreach (var reference in option.Choices[(int)option.Selected - 1].Patches)
                {
                    var patch = Patches.FirstOrDefault(candidate => candidate.Id == reference.Id);
                    if (patch is null) continue;

                    var replacements = new List<(string Key, string Value)>
                    {
                        ("{$__gameid}", gameId[..3]),
                        ("{$__region}", gameId.Substring(3, 1)),
                        ("{$__maker}", gameId.Substring(4, 2)),
                    };
                    replacements.AddRange(reference.Parameters
                        .OrderBy(static parameter => parameter.Key, StringComparer.Ordinal)
                        .Select(static parameter => ("{$" + parameter.Key + "}", parameter.Value)));
                    yield return new Patch(
                        patch.Id,
                        Replace(patch.Root, replacements),
                        patch.Files.Select(file => new FilePatch(
                            Replace(file.Disc, replacements), Replace(file.External, replacements))).ToArray());
                }
            }
        }

        private static string Replace(string text, IReadOnlyList<(string Key, string Value)> replacements)
        {
            var result = new System.Text.StringBuilder(text.Length);
            var index = 0;
            while (index < text.Length)
            {
                var replaced = false;
                foreach (var (key, value) in replacements)
                {
                    if (string.CompareOrdinal(text, index, key, 0, key.Length) == 0)
                    {
                        result.Append(value);
                        index += key.Length;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced)
                {
                    result.Append(text[index++]);
                }
            }
            return result.ToString();
        }
    }

    private static XElement? LoadRoot(string path)
    {
        try
        {
            return XDocument.Load(path).Root;
        }
        catch (Exception ex) when (ex is XmlException or IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    private static Disc? ReadDisc(string path)
    {
        var root = LoadRoot(path);
        if (root is null || root.Name.LocalName != "wiidisc" || ParseInt(Attribute(root, "version"), -1) != 1)
        {
            return null;
        }

        var defaultRoot = Attribute(root, "root");
        string? game = null, developer = null;
        bool discFilter = false, versionFilter = false;
        List<string>? regions = null;
        var sections = new List<Section>();
        var patches = new List<Patch>();
        foreach (var node in root.Elements())
        {
            switch (node.Name.LocalName)
            {
                case "id":
                    game = (string?)node.Attribute("game") ?? game;
                    developer = (string?)node.Attribute("developer") ?? developer;
                    // A disc or version filter rejects the pack, since neither is known; -1 (what an
                    // unreadable value parses to) is the one that compares equal to "unknown".
                    discFilter |= node.Attribute("disc") is { } discNumber && ParseInt(discNumber.Value, -1) != -1;
                    versionFilter |= node.Attribute("version") is { } version && ParseInt(version.Value, -1) != -1;
                    var regionTypes = node.Elements("region").Select(region => Attribute(region, "type")).ToList();
                    if (regionTypes.Count > 0) regions = regionTypes;
                    break;
                case "options":
                    foreach (var sectionNode in node.Elements("section"))
                    {
                        var options = new List<Option>();
                        foreach (var optionNode in sectionNode.Elements("option"))
                        {
                            var optionParameters = ReadParameters(optionNode, new Dictionary<string, string>());
                            var choices = optionNode.Elements("choice").Select(choiceNode =>
                            {
                                var choiceParameters = ReadParameters(choiceNode, optionParameters);
                                return new Choice(choiceNode.Elements("patch")
                                    .Select(reference => new PatchReference(
                                        Attribute(reference, "id"), ReadParameters(reference, choiceParameters)))
                                    .ToArray());
                            }).ToArray();
                            options.Add(new Option
                            {
                                Id = Attribute(optionNode, "id"),
                                Name = Attribute(optionNode, "name"),
                                Choices = choices,
                                Selected = ParseUInt(Attribute(optionNode, "default"), 0),
                            });
                        }
                        sections.Add(new Section(Attribute(sectionNode, "name"), options));
                    }
                    break;
                case "patch":
                    var patchRoot = Attribute(node, "root");
                    patches.Add(new Patch(
                        Attribute(node, "id"),
                        patchRoot.Length == 0 ? defaultRoot : patchRoot,
                        node.Elements("file")
                            .Select(file => new FilePatch(Attribute(file, "disc"), Attribute(file, "external")))
                            .ToArray()));
                    break;
            }
        }

        return new Disc
        {
            Game = game,
            Developer = developer,
            HasDiscFilter = discFilter,
            HasVersionFilter = versionFilter,
            Regions = regions,
            Sections = sections,
            Patches = patches,
        };
    }

    // riivolution/config/<GameID4>.xml: the remembered choices (Riivolution, Dolphin and the port's F10 write it).
    private static IReadOnlyList<(string Id, uint Choice)>? ReadConfig(string path)
    {
        if (!File.Exists(path)) return null;
        var root = LoadRoot(path);
        if (root is null || root.Name.LocalName != "riivolution" || ParseInt(Attribute(root, "version"), -1) != 2)
        {
            return null;
        }
        return root.Elements("option")
            .Select(option => (Attribute(option, "id"), ParseUInt(Attribute(option, "default"), 0)))
            .ToArray();
    }

    private static Dictionary<string, string> ReadParameters(XElement node, IReadOnlyDictionary<string, string> inherited)
    {
        var parameters = new Dictionary<string, string>(inherited, StringComparer.Ordinal);
        foreach (var parameter in node.Elements("param"))
        {
            parameters[Attribute(parameter, "name")] = Attribute(parameter, "value");
        }
        return parameters;
    }

    private static string Attribute(XElement node, string name) => (string?)node.Attribute(name) ?? string.Empty;

    // Decimal or 0x-prefixed hex, as riivolution_contract.h's AttributeUint reads them.
    private static uint ParseUInt(string text, uint fallback)
    {
        if (text.Length == 0) return fallback;
        var hex = text.StartsWith("0x", StringComparison.OrdinalIgnoreCase);
        var digits = hex ? text[2..] : text;
        return digits.Length > 0 && uint.TryParse(
            digits,
            hex ? System.Globalization.NumberStyles.AllowHexSpecifier : System.Globalization.NumberStyles.None,
            System.Globalization.CultureInfo.InvariantCulture,
            out var value)
            ? value
            : fallback;
    }

    private static int ParseInt(string text, int fallback)
    {
        if (text.StartsWith('-'))
        {
            var magnitude = ParseUInt(text[1..], 0x80000000u);
            return magnitude <= 0x80000000u ? -(int)magnitude : fallback;
        }
        var value = ParseUInt(text, 0x80000000u);
        return value < 0x80000000u ? (int)value : fallback;
    }

    // ---- external path resolution (Dolphin FileDataLoaderHostFS semantics, '/'-separated) ----

    private static string Generic(string path) => path.Replace('\\', '/');

    /// <summary>A leading '/' is relative to the SD root, anything else to the patch root. ".." can't
    /// climb above where it started, and a backslash is refused (Riivolution treats it as part of a name).</summary>
    internal static string? MakeAbsoluteFromRelative(string sdRoot, string patchRoot, string external)
    {
        if (external.Contains('\\')) return null;

        var absolute = external.StartsWith('/');
        var result = (absolute ? sdRoot : patchRoot).TrimEnd('/');
        var depth = 0;
        foreach (var element in external.Trim('/').Split('/'))
        {
            if (element.Length == 0 || element == ".") continue;
            if (element == "..")
            {
                if (depth == 0) return null;
                depth--;
                var lastSlash = result.LastIndexOf('/');
                if (lastSlash < 0) return null;
                result = result[..lastSlash];
                continue;
            }
            depth++;
            result += "/" + element;
        }
        return result;
    }

    internal static string ResolvePatchRoot(string sdRoot, string xmlDirectory, string rootAttribute) =>
        rootAttribute.Length != 0 && MakeAbsoluteFromRelative(sdRoot, xmlDirectory, rootAttribute) is { } resolved
            ? resolved
            : xmlDirectory;

    [GeneratedRegex(@"^sml_[^/\\]+\.bin$", RegexOptions.IgnoreCase)]
    private static partial Regex CodeModuleName();
}
