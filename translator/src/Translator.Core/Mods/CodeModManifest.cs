using System.Text.Json;

namespace Translator.Core.Mods;

/// <summary>
/// What link-code-mods folded into a module set (<c>modules.json</c> beside <c>modules.bin</c>):
/// translate-mod builds the list into the game, which logs it, shows it on F10's Mods page and
/// compares it with the packs installed at launch.
/// </summary>
public sealed record CodeModManifest(
    string Format,
    int FormatVersion,
    string GameId,
    string ModuleSetSha256,
    IReadOnlyList<CodeModManifestModule> Modules)
{
    public const string CurrentFormat = "strikers-code-mods";
    public const int CurrentFormatVersion = 1;

    /// <summary>The manifest beside a module set, or null when there is none (a hand-made Code.pul).</summary>
    public static CodeModManifest? TryReadBeside(string moduleSetPath)
    {
        var path = Path.ChangeExtension(moduleSetPath, ".json");
        if (!File.Exists(path))
        {
            return null;
        }

        var manifest = JsonSerializer.Deserialize<CodeModManifest>(File.ReadAllText(path));
        return manifest is { Format: CurrentFormat, FormatVersion: CurrentFormatVersion } ? manifest : null;
    }
}

/// <param name="Name">The module's disc-root file name, e.g. <c>sml_50_superteam.bin</c>.</param>
/// <param name="Offset">Where the module starts in the module set.</param>
public sealed record CodeModManifestModule(
    string Name,
    string Sha256,
    string Source,
    string Pack,
    uint Offset,
    uint CodeSize,
    uint BssSize,
    int CtorCount);
