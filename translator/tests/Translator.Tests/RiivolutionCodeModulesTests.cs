using Translator.Core.Mods;
using Xunit;

namespace Translator.Tests;

public sealed class RiivolutionCodeModulesTests : IDisposable
{
    private readonly string _root = Path.Combine(Path.GetTempPath(), "riivo-code-mods-" + Guid.NewGuid().ToString("N"));

    public void Dispose()
    {
        if (Directory.Exists(_root)) Directory.Delete(_root, recursive: true);
    }

    [Fact]
    public void FindsTheCodeModulesOfActiveChoices()
    {
        var sd = Path.Combine(_root, "sd");
        WriteFile(sd, "superteam/code/superteam.bin", "module");
        WriteFile(sd, "superteam/files/menu.res", "art");
        WriteFile(sd, "riivolution/superteam.xml", """
            <wiidisc version="1">
              <id game="R4Q"/>
              <options>
                <section name="Super Team">
                  <option name="Super Team" id="superteam" default="1">
                    <choice name="Enabled"><patch id="superteam"/></choice>
                  </option>
                </section>
              </options>
              <patch id="superteam" root="/superteam">
                <file disc="/sml_50_superteam.bin" external="code/superteam.bin" create="true"/>
                <file disc="/Art/fe/menu.res" external="files/menu.res"/>
              </patch>
            </wiidisc>
            """);

        var modules = RiivolutionCodeModules.Find([sd], "R4QE01");

        var module = Assert.Single(modules);
        Assert.Equal("sml_50_superteam.bin", module.DiscName);
        Assert.Equal(Path.GetFullPath(Path.Combine(sd, "superteam/code/superteam.bin")), module.HostPath);
    }

    [Fact]
    public void ConfigChoicesOverrideTheXmlDefault()
    {
        var sd = Path.Combine(_root, "sd");
        WriteFile(sd, "pack/code.bin", "module");
        WriteFile(sd, "riivolution/pack.xml", """
            <wiidisc version="1">
              <options>
                <section name="Pack">
                  <option name="Code"><choice name="On"><patch id="code"/></choice></option>
                </section>
              </options>
              <patch id="code"><file disc="sml_10_pack.bin" external="/pack/code.bin"/></patch>
            </wiidisc>
            """);

        Assert.Empty(RiivolutionCodeModules.Find([sd], "R4QE01"));

        // An option without an id is addressed as section name + option name.
        WriteFile(sd, "riivolution/config/R4QE.xml", """
            <riivolution version="2"><option id="PackCode" default="1"/></riivolution>
            """);

        Assert.Equal("sml_10_pack.bin", Assert.Single(RiivolutionCodeModules.Find([sd], "R4QE01")).DiscName);
    }

    [Fact]
    public void SkipsOtherGamesAndNonModuleFiles()
    {
        var sd = Path.Combine(_root, "sd");
        WriteFile(sd, "pack/code.bin", "module");
        WriteFile(sd, "riivolution/mkw.xml", """
            <wiidisc version="1">
              <id game="RMC"/>
              <options><section name="S"><option name="O" default="1"><choice name="C"><patch id="p"/></choice></option></section></options>
              <patch id="p"><file disc="/sml_10_mkw.bin" external="/pack/code.bin"/></patch>
            </wiidisc>
            """);
        WriteFile(sd, "riivolution/other.xml", """
            <wiidisc version="1">
              <options><section name="S"><option name="O" default="1"><choice name="C"><patch id="p"/></choice></option></section></options>
              <patch id="p">
                <file disc="/mods/sml_10_nested.bin" external="/pack/code.bin"/>
                <file disc="/code.bin" external="/pack/code.bin"/>
              </patch>
            </wiidisc>
            """);

        Assert.Empty(RiivolutionCodeModules.Find([sd], "R4QE01"));
    }

    [Fact]
    public void TheFirstRootWinsAndModulesAreInNameOrder()
    {
        var high = Path.Combine(_root, "high");
        var low = Path.Combine(_root, "low");
        foreach (var (sd, name) in new[] { (high, "high"), (low, "low") })
        {
            WriteFile(sd, $"{name}.bin", name);
            WriteFile(sd, "riivolution/pack.xml", $$"""
                <wiidisc version="1">
                  <options><section name="S"><option name="O" default="1"><choice name="C"><patch id="p"/></choice></option></section></options>
                  <patch id="p">
                    <file disc="/sml_50_shared.bin" external="/{{name}}.bin"/>
                    <file disc="/sml_{{(name == "high" ? "90" : "10")}}_{{name}}.bin" external="/{{name}}.bin"/>
                  </patch>
                </wiidisc>
                """);
        }

        var modules = RiivolutionCodeModules.Find([high, low], "R4QE01");

        Assert.Equal(["sml_10_low.bin", "sml_50_shared.bin", "sml_90_high.bin"], modules.Select(m => m.DiscName));
        Assert.EndsWith("high.bin", modules[1].HostPath);
    }

    [Theory]
    [InlineData("/sd", "/sd/pack", "code.bin", "/sd/pack/code.bin")]
    [InlineData("/sd", "/sd/pack", "/other/code.bin", "/sd/other/code.bin")]
    [InlineData("/sd", "/sd/pack", "../code.bin", null)]
    [InlineData("/sd", "/sd/pack", "dir/../code.bin", "/sd/pack/code.bin")]
    [InlineData("/sd", "/sd/pack", "dir\\code.bin", null)]
    public void ResolvesExternalPathsLikeDolphin(string sdRoot, string patchRoot, string external, string? expected)
    {
        Assert.Equal(expected, RiivolutionCodeModules.MakeAbsoluteFromRelative(sdRoot, patchRoot, external));
    }

    private static void WriteFile(string root, string relative, string text)
    {
        var path = Path.Combine(root, relative);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, text);
    }
}
