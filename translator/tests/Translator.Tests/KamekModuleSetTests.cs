using System.Buffers.Binary;
using Translator.Core.Parsing.Kamek;
using Xunit;

namespace Translator.Tests;

public class KamekModuleSetTests
{
    [Fact]
    public void RawChunkIsSelectedForAnyRegion()
    {
        var pul = KamekPulFile.Parse(BuildModule(code: new byte[8], bss: 0, ctorStart: 0, ctorEnd: 0, commands: []));

        Assert.Same(pul.Chunks[0], pul.SelectRegion("E"));
        Assert.Same(pul.Chunks[0], pul.SelectRegion("J"));
    }

    [Fact]
    public void FoldsModulesInOrderAndMovesRelativeAddresses()
    {
        // Module A: 0x24 bytes of code, 0x10 of BSS, one ctor at +0x4 (slot +0x20), a call into the game
        // from +0x8, a pointer written into the game that points at +0x4.
        var a = BuildModule(
            code: new byte[0x24], bss: 0x10, ctorStart: 0x20, ctorEnd: 0x24,
            commands:
            [
                Command(KamekCommandId.Addr32, absolute: false, 0x20, 0x4),
                Command(KamekCommandId.Rel24, absolute: false, 0x8, 0x803B5BE4),
                Command(KamekCommandId.Addr32, absolute: true, 0x80520000, 0x4),
                Command(KamekCommandId.Write32, absolute: true, 0x806E0F98, 1),
            ]);
        // Module B: a branch from the game into its +0x0, and a write of the same game word as A.
        var b = BuildModule(
            code: new byte[0x8], bss: 0, ctorStart: 0, ctorEnd: 0,
            commands:
            [
                Command(KamekCommandId.Branch, absolute: true, 0x80223D88, 0x0),
                Command(KamekCommandId.Write32, absolute: true, 0x806E0F98, 2),
            ]);

        var set = KamekModuleSet.Fold([("sml_10_a.bin", a), ("sml_20_b.bin", b)]);

        // A at 0, its BSS as zeros to 0x34; B at the next 0x20 boundary.
        Assert.Equal(0u, set.Modules[0].Offset);
        Assert.Equal(0x40u, set.Modules[1].Offset);
        Assert.Equal(1, set.Modules[0].CtorCount);
        Assert.Equal(0, set.Modules[1].CtorCount);
        Assert.Equal(0u, set.Chunk.BssSize);
        Assert.Equal(0x48u, set.Chunk.CtorStart);
        Assert.Equal(0x4Cu, set.Chunk.CtorEnd);
        Assert.Equal(0x4Cu, set.Chunk.CodeSize);

        var commands = set.Chunk.Commands;
        // A's relative commands stay at A's offsets; its game targets stay absolute.
        Assert.Equal((0x20u, 0x4u), (commands[0].Address, commands[0].Arguments[0]));
        Assert.Equal((0x8u, 0x803B5BE4u), (commands[1].Address, commands[1].Arguments[0]));
        Assert.Equal((0x80520000u, 0x4u), (commands[2].Address, commands[2].Arguments[0]));
        // B's module-relative target moves by B's offset; the command into the game stays absolute.
        Assert.True(commands[4].AddressIsAbsolute);
        Assert.Equal((0x80223D88u, 0x40u), (commands[4].Address, commands[4].Arguments[0]));
        // Same game word, B after A: B's value wins when applied in order.
        Assert.Equal(2u, commands[5].Arguments[0]);
        // The new ctor table relocates to A's ctor.
        Assert.Equal(KamekCommandId.Addr32, commands[^1].Id);
        Assert.Equal((0x48u, 0x4u), (commands[^1].Address, commands[^1].Arguments[0]));
    }

    [Fact]
    public void SerializedSetParsesBack()
    {
        var a = BuildModule(new byte[0x10], 0x8, 0xC, 0x10,
            [Command(KamekCommandId.Addr32, false, 0xC, 0x0), Command(KamekCommandId.BranchLink, true, 0x802B44EC, 0x0)]);
        var b = BuildModule(new byte[0x4], 0, 0, 0, [Command(KamekCommandId.Write8, true, 0x806E0F98, 0x7F)]);
        var set = KamekModuleSet.Fold([("sml_1_a.bin", a), ("sml_2_b.bin", b)]);

        var parsed = KamekPulFile.Parse(set.Serialize()).SelectRegion("E");

        Assert.Equal(set.Chunk.CodeSize, parsed.CodeSize);
        Assert.Equal(set.Chunk.CtorStart, parsed.CtorStart);
        Assert.Equal(set.Chunk.CtorEnd, parsed.CtorEnd);
        Assert.Equal(set.Chunk.CodeBlob, parsed.CodeBlob);
        Assert.Equal(
            set.Chunk.Commands.Select(c => (c.Id, c.AddressIsAbsolute, c.Address, string.Join(",", c.Arguments))),
            parsed.Commands.Select(c => (c.Id, c.AddressIsAbsolute, c.Address, string.Join(",", c.Arguments))));
    }

    [Fact]
    public void RefusesAMultiRegionCodePul()
    {
        var chunk = BuildModule(new byte[4], 0, 0, 0, []);
        var combined = new byte[0x10 + chunk.Length];
        BinaryPrimitives.WriteUInt32BigEndian(combined, (uint)chunk.Length);
        chunk.CopyTo(combined, 0x10);

        Assert.Throws<InvalidDataException>(() => KamekModuleSet.Fold([("sml_x.bin", combined)]));
    }

    private static byte[] BuildModule(byte[] code, uint bss, uint ctorStart, uint ctorEnd, byte[][] commands)
    {
        var size = KamekChunk.HeaderSize + code.Length + commands.Sum(c => c.Length);
        var data = new byte[size];
        WriteU32(data, 0x00, KamekChunk.Magic0);
        WriteU32(data, 0x04, KamekChunk.Magic1V2);
        WriteU32(data, 0x08, bss);
        WriteU32(data, 0x0C, (uint)code.Length);
        WriteU32(data, 0x10, ctorStart);
        WriteU32(data, 0x14, ctorEnd);
        code.CopyTo(data, KamekChunk.HeaderSize);
        var offset = KamekChunk.HeaderSize + code.Length;
        foreach (var command in commands)
        {
            command.CopyTo(data, offset);
            offset += command.Length;
        }
        return data;
    }

    private static byte[] Command(KamekCommandId id, bool absolute, uint address, params uint[] args)
    {
        var data = new byte[4 + (absolute ? 4 : 0) + args.Length * 4];
        WriteU32(data, 0, ((uint)(byte)id << 24) | (absolute ? 0x00FFFFFEu : address));
        var offset = 4;
        if (absolute)
        {
            WriteU32(data, offset, address);
            offset += 4;
        }
        foreach (var arg in args)
        {
            WriteU32(data, offset, arg);
            offset += 4;
        }
        return data;
    }

    private static void WriteU32(byte[] data, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32BigEndian(data.AsSpan(offset, 4), value);
}
